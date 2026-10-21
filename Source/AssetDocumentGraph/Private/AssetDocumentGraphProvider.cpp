// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentGraphProvider.h"

#include "AssetDocumentEditorNativeState.h"
#include "AssetDocumentGraphPrimitives.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Package.h"

namespace UE::AssetDocument::Graph
{
namespace
{
FAssetDocumentRuntimeDiagnostic MakeDiagnostic(FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/Primitive"))
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = FString(Path);
	Diagnostic.Code = FString(Code);
	Diagnostic.Message = FString(Message);
	return Diagnostic;
}

FAssetDocumentPrimitiveOutcome MakeFailure(TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Outcome.Diagnostics = MoveTemp(Diagnostics);
	if (Outcome.Diagnostics.IsEmpty())
	{
		Outcome.Diagnostics.Add(MakeDiagnostic(TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), TEXT("The primitive did not succeed.")));
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeFailure(const FAssetDocumentGraphError& Error)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic = MakeDiagnostic(
		Error.Code.IsEmpty() ? FStringView(TEXT("AD.RUNTIME.PRIMITIVE_FAILED")) : FStringView(Error.Code),
		Error.Message.IsEmpty() ? FStringView(TEXT("The graph primitive failed.")) : FStringView(Error.Message),
		Error.Path.IsEmpty() ? FStringView(TEXT("/")) : FStringView(Error.Path));
	Diagnostic.Context = Error.Context;
	return MakeFailure(TArray<FAssetDocumentRuntimeDiagnostic>{MoveTemp(Diagnostic)});
}

// One stateless provider over a descriptor/binding table pair: "graph" and
// "blueprint_member" share the invocation, lease and footprint plumbing.
struct FProviderTables
{
	const TCHAR* Id = nullptr;
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> (*Descriptors)() = nullptr;
	TConstArrayView<FBinding> (*Bindings)() = nullptr;
	// False for providers over assets without graphs (FCall::Graphs stays empty).
	bool bCollectGraphs = true;
	// The Blueprint providers ("graph", "blueprint"): a host asset's handle addresses its hosted Blueprint.
	bool bHostedBlueprints = false;
};

// Host class path, never the UClass: unregistering at module shutdown must not touch a purged class.
struct FHostedEntry
{
	FTopLevelAssetPath HostClassPath;
	FAssetDocumentHostedBlueprintResolver Resolver = nullptr;
};

TArray<FHostedEntry>& HostedRegistry()
{
	static TArray<FHostedEntry> Entries;
	return Entries;
}

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(const FProviderTables& Tables, const FAssetDocumentPrimitiveInvocation& Invocation)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Tables.Descriptors())
	{
		if (Descriptor.PrimitiveId == Invocation.PrimitiveId && Descriptor.Version == Invocation.Version)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

FHandler FindHandler(const FProviderTables& Tables, const FAssetDocumentPrimitiveInvocation& Invocation)
{
	for (const FBinding& Binding : Tables.Bindings())
	{
		if (Invocation.PrimitiveId == Binding.PrimitiveId)
		{
			return Binding.Handler;
		}
	}
	return nullptr;
}

bool IsTransactionValid(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const IAssetDocumentAtomicTransaction* Transaction)
{
	if (!Descriptor.bRequiresActiveTransaction)
	{
		return true;
	}
	if (Transaction == nullptr || Transaction->GetTransactionId() != Invocation.TransactionId)
	{
		return false;
	}
	const EAssetDocumentAtomicState State = Transaction->GetState();
	if (State != EAssetDocumentAtomicState::Live && State != EAssetDocumentAtomicState::Mutating)
	{
		return false;
	}
	const FAssetDocumentMutationToken& Token = Transaction->GetMutationToken();
	return Token.RequestId == Invocation.RequestId && Token.TransactionId == Invocation.TransactionId
		&& Token.RuntimeRevision == Invocation.RuntimeRevision;
}

bool IsCatalogValid(const FProviderTables& Tables)
{
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors = Tables.Descriptors();
	if (Descriptors.Num() != Tables.Bindings().Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Descriptors.Num(); ++Index)
	{
		FAssetDocumentPrimitiveInvocation Probe;
		Probe.PrimitiveId = Descriptors[Index].PrimitiveId;
		if (!ValidateAssetDocumentPrimitiveDescriptor(Descriptors[Index], Descriptors.Left(Index)).bValid
			|| FindHandler(Tables, Probe) == nullptr)
		{
			return false;
		}
	}
	return true;
}

// The whole package of the asset is the footprint of every graph write; Core
// snapshots it before the first write and restores it on rollback.
bool EnlistPackageFootprint(UObject& Asset, IAssetDocumentAtomicTransaction& Transaction, FAssetDocumentGraphError& OutError)
{
	const UPackage* Package = Asset.GetPackage();
	FAssetDocumentMutationFootprint Footprint;
	Footprint.Kind = EAssetDocumentMutationFootprintKind::Package;
	Footprint.StableId = TEXT("Package:") + (Package != nullptr ? Package->GetName() : Asset.GetPathName());
	Footprint.RollbackComponentId = TEXT("rollback.package");
	const FAssetDocumentContractValidationResult Result = Transaction.EnlistFootprint({Footprint});
	if (Result.bValid)
	{
		return true;
	}
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Result.Diagnostics)
	{
		if (Diagnostic.Code == TEXT("Atomic.FootprintAlreadyEnlisted"))
		{
			return true;
		}
	}
	OutError.Set(Result.Diagnostics.IsEmpty() ? FStringView(TEXT("AD.RUNTIME.PRIMITIVE_FAILED")) : FStringView(Result.Diagnostics[0].Code),
		Result.Diagnostics.IsEmpty() ? FStringView(TEXT("The package footprint was rejected.")) : FStringView(Result.Diagnostics[0].Message),
		TEXT("/HandleArguments/asset"));
	return false;
}

FValue WithSchemaVersion(FValue Body)
{
	FValue Out = FObjectBuilder().AddInt(TEXT("schema_version"), 1).Build();
	for (int32 Index = 0; Index < Body.ObjectKeys.Num(); ++Index)
	{
		if (Body.ObjectKeys[Index] != TEXT("schema_version"))
		{
			Out.ObjectKeys.Add(MoveTemp(Body.ObjectKeys[Index]));
			Out.Elements.Add(MoveTemp(Body.Elements[Index]));
		}
	}
	return Out;
}

FAssetDocumentPrimitiveOutcome RunHandler(
	const TCHAR* OwnerProviderId,
	bool bCollectGraphs,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FHandler Handler,
	UObject& Asset,
	IAssetDocumentAtomicTransaction* Transaction)
{
	FAssetDocumentGraphError Error;
	const bool bMutating = Descriptor.Access == EAssetDocumentPrimitiveAccess::Mutating;
	if (bMutating && GEditor != nullptr && GEditor->PlayWorld != nullptr)
	{
		Error.Set(TEXT("Graph.PieActive"), TEXT("Graph edits are rejected while Play-In-Editor is running."));
		return MakeFailure(Error);
	}
	FCall Call(Invocation, Asset, Error);
	Call.bMutating = bMutating;
	if (bCollectGraphs && !Call.Refresh())
	{
		return MakeFailure(Error);
	}
	if (bMutating && !EnlistPackageFootprint(Asset, *Transaction, Error))
	{
		return MakeFailure(Error);
	}
	FObjectBuilder Result;
	if (!Handler(Call, Result))
	{
		if (Error.Code.IsEmpty())
		{
			Error.Set(TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), TEXT("The graph primitive failed."));
		}
		return MakeFailure(Error);
	}
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = WithSchemaVersion(Result.Build());
	if (CountValueNodes(Outcome.Value) > UE::AssetDocument::RuntimeTypes::Private::RuntimeValueMaximumNodes)
	{
		// Name only the paging the primitive offers (a generic hint sent callers to arguments
		// most primitives do not take).
		const FString& Primitive = Invocation.PrimitiveId;
		const TCHAR* Hint = TEXT("Read a smaller part per call.");
		if (Primitive == TEXT("graph.query.v1"))
		{
			Hint = TEXT("Pass fewer graphs per call (\"graphs\", addresses from graph.list.v1) and/or page nodes with node_offset/node_limit.");
		}
		else if (Primitive == TEXT("graph.list.v1"))
		{
			Hint = TEXT("Page the list with offset (0 first, then next_offset until it is null).");
		}
		Error.Set(TEXT("Graph.ResultTooLarge"), FString::Printf(
			TEXT("The %s result exceeds the runtime value budget. %s"), *Primitive, Hint));
		return MakeFailure(Error);
	}
	if (bMutating)
	{
		FAssetDocumentChangedRecord& Changed = Outcome.ChangedSet.AddDefaulted_GetRef();
		Changed.AuthorityId = OwnerProviderId;
		Changed.StableObjectId = Asset.GetPathName();
		Changed.Path = TEXT("/") + Invocation.PrimitiveId;
		Changed.ChangeKind = bCollectGraphs ? TEXT("graph.edit") : TEXT("asset.edit");
	}
	return Outcome;
}

class FGraphProvider final : public IAssetDocumentPrimitiveProvider
{
public:
	explicit FGraphProvider(const FProviderTables& InTables) : Tables(InTables) {}

	virtual FString GetProviderId() const override
	{
		return Tables.Id;
	}

	virtual TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override
	{
		return Tables.Descriptors();
	}

	virtual FAssetDocumentProviderAvailability InspectAvailability() const override
	{
		FAssetDocumentProviderAvailability Availability;
		Availability.ProviderId = GetProviderId();
		if (IsCatalogValid(Tables))
		{
			Availability.State = EAssetDocumentProviderAvailabilityState::Available;
			return Availability;
		}
		Availability.State = EAssetDocumentProviderAvailabilityState::Unavailable;
		Availability.Diagnostic = MakeDiagnostic(TEXT("GenericProvider.InvalidAssembly"),
			TEXT("The graph descriptor and binding tables are invalid or not bijective."));
		return Availability;
	}

	virtual FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(Tables, Invocation);
		const FHandler Handler = FindHandler(Tables, Invocation);
		if (Descriptor == nullptr || Handler == nullptr)
		{
			return MakeFailure({MakeDiagnostic(TEXT("GenericProvider.UnknownPrimitive"), TEXT("No exact primitive id and version binding exists."))});
		}
		const FAssetDocumentContractValidationResult InvocationValidation =
			ValidateAssetDocumentPrimitiveInvocation(Invocation, *Descriptor);
		if (!InvocationValidation.bValid)
		{
			return MakeFailure(InvocationValidation.Diagnostics);
		}
		if (!IsTransactionValid(Invocation, *Descriptor, ActiveTransaction))
		{
			return MakeFailure({MakeDiagnostic(TEXT("GenericProvider.InvalidTransaction"),
				TEXT("The active transaction identity, token, revision, or state is invalid."))});
		}
		const FAssetDocumentPrimitiveHandleArgument* AssetArgument = Invocation.HandleArguments.FindByPredicate(
			[](const FAssetDocumentPrimitiveHandleArgument& Argument) { return Argument.Name == TEXT("asset"); });
		if (AssetArgument == nullptr)
		{
			return MakeFailure({MakeDiagnostic(TEXT("PrimitiveInvocation.InvalidHandleArguments"), TEXT("The asset handle is required."), TEXT("/HandleArguments/asset"))});
		}
		FAssetDocumentHandleValidationContext Context;
		Context.RequestId = Invocation.RequestId;
		Context.TransactionId = Invocation.TransactionId;
		Context.RuntimeRevision = Invocation.RuntimeRevision;
		Context.ProviderId = Tables.Id;
		Context.Phase = Invocation.Phase;
		Context.RequiredKind = EAssetDocumentHandleKind::Asset;
		Context.RequiredAccess = Descriptor->HandleArgumentSchemas[0].RequiredAccess;
		FAssetDocumentVerifiedLeaseResult Lease = Handles.AcquireVerifiedLease(AssetArgument->Handle, Context);
		if (!Lease.Lease || !Lease.Diagnostics.IsEmpty())
		{
			return MakeFailure(MoveTemp(Lease.Diagnostics));
		}
		FAssetDocumentPrimitiveOutcome Outcome;
		const FAssetDocumentContractValidationResult Identity = WithAssetDocumentEditorAssetIdentity(*Lease.Lease,
			[&](const FAssetDocumentEditorAssetIdentityView& View)
			{
				UObject* Target = &View.GetObject();
				if (Tables.bHostedBlueprints && !Target->IsA<UBlueprint>())
				{
					bool bHostClass = false;
					UBlueprint* Hosted = ResolveAssetDocumentHostedBlueprint(*Target, &bHostClass);
					if (bHostClass && Hosted == nullptr)
					{
						Outcome = MakeFailure({MakeDiagnostic(TEXT("Graph.NoHostedBlueprint"), FString::Printf(TEXT("%s has no Blueprint "
							"(create it first)."), *Target->GetPathName()), TEXT("/HandleArguments/asset"))});
						FAssetDocumentContractValidationResult Valid;
						Valid.bValid = true;
						return Valid;
					}
					Target = Hosted != nullptr ? Hosted : Target;
				}
				Outcome = RunHandler(Tables.Id, Tables.bCollectGraphs, Invocation, *Descriptor, Handler, *Target, ActiveTransaction);
				FAssetDocumentContractValidationResult Valid;
				Valid.bValid = true;
				return Valid;
			});
		if (!Identity.bValid)
		{
			return MakeFailure(Identity.Diagnostics);
		}
		if (Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		{
			const FAssetDocumentContractValidationResult OutcomeValidation =
				ValidateAssetDocumentPrimitiveOutcome(Invocation, *Descriptor, Outcome);
			if (!OutcomeValidation.bValid)
			{
				return MakeFailure(OutcomeValidation.Diagnostics);
			}
		}
		return Outcome;
	}

private:
	FProviderTables Tables;
};
} // namespace
} // namespace UE::AssetDocument::Graph

void RegisterAssetDocumentHostedBlueprint(const FTopLevelAssetPath& HostClassPath, FAssetDocumentHostedBlueprintResolver Resolver)
{
	check(IsInGameThread());
	using namespace UE::AssetDocument::Graph;
	if (HostClassPath.IsNull() || Resolver == nullptr)
	{
		return;
	}
	UnregisterAssetDocumentHostedBlueprint(HostClassPath);
	HostedRegistry().Add({HostClassPath, Resolver});
}

void UnregisterAssetDocumentHostedBlueprint(const FTopLevelAssetPath& HostClassPath)
{
	check(IsInGameThread());
	using namespace UE::AssetDocument::Graph;
	HostedRegistry().RemoveAll([&HostClassPath](const FHostedEntry& Entry) { return Entry.HostClassPath == HostClassPath; });
}

UBlueprint* ResolveAssetDocumentHostedBlueprint(UObject& Host, bool* bOutHostClass)
{
	using namespace UE::AssetDocument::Graph;
	if (!HostedRegistry().IsEmpty())
	{
		// Host.IsA(registered class), by class path: Host's class and its super classes.
		for (const UClass* Class = Host.GetClass(); Class != nullptr; Class = Class->GetSuperClass())
		{
			const FTopLevelAssetPath ClassPath = Class->GetClassPathName();
			for (const FHostedEntry& Entry : HostedRegistry())
			{
				if (Entry.HostClassPath == ClassPath)
				{
					if (bOutHostClass != nullptr)
					{
						*bOutHostClass = true;
					}
					return Entry.Resolver(Host);
				}
			}
		}
	}
	if (bOutHostClass != nullptr)
	{
		*bOutHostClass = false;
	}
	return nullptr;
}

TSharedRef<IAssetDocumentPrimitiveProvider> CreateAssetDocumentGraphPrimitiveProvider()
{
	using namespace UE::AssetDocument::Graph;
	return MakeShared<FGraphProvider>(FProviderTables{ProviderId, &GetGraphPrimitiveDescriptors, &GetGraphPrimitiveBindings, true, true});
}

TSharedRef<IAssetDocumentPrimitiveProvider> CreateAssetDocumentBlueprintMemberPrimitiveProvider()
{
	using namespace UE::AssetDocument::Graph;
	return MakeShared<FGraphProvider>(FProviderTables{BlueprintMemberProviderId,
		&GetBlueprintMemberPrimitiveDescriptors, &GetBlueprintMemberPrimitiveBindings, true, true});
}

TSharedRef<IAssetDocumentPrimitiveProvider> CreateAssetDocumentGraphTablePrimitiveProvider(
	const TCHAR* ProviderId,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> (*Descriptors)(),
	TConstArrayView<UE::AssetDocument::Graph::FBinding> (*Bindings)())
{
	using namespace UE::AssetDocument::Graph;
	return MakeShared<FGraphProvider>(FProviderTables{ProviderId, Descriptors, Bindings});
}

TSharedRef<IAssetDocumentPrimitiveProvider> CreateAssetDocumentAssetTablePrimitiveProvider(
	const TCHAR* ProviderId,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> (*Descriptors)(),
	TConstArrayView<UE::AssetDocument::Graph::FBinding> (*Bindings)())
{
	using namespace UE::AssetDocument::Graph;
	return MakeShared<FGraphProvider>(FProviderTables{ProviderId, Descriptors, Bindings, false});
}
