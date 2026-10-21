// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "TypedPreimage/AssetDocumentAIBlackboardTypedPreimage.h"
#include "TypedPreimage/AssetDocumentAISelectorTypedPreimage.h"
#include "TypedPreimage/AssetDocumentAITypedPreimageInternal.h"

#include "AssetDocumentAITypedPreimageTestTypes.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentRollbackAction.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "IDetailTreeNode.h"
#include "Internationalization/TextPackageNamespaceUtil.h"
#include "IPropertyRowGenerator.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#include <limits>

namespace AssetDocumentAITypedPreimageTests
{
struct FTestContext
{
	FGuid RequestId = FGuid::NewGuid();
	FGuid TransactionId = FGuid::NewGuid();
	uint64 RuntimeRevision = 1;
	FString ProviderId = TEXT("editor");
};

FAssetDocumentContractValidationResult MakeValidResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentContractValidationResult MakeTestFailure(const TCHAR* Code)
{
	FAssetDocumentContractValidationResult Result;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/AITypedPreimageTest");
	Diagnostic.Code = Code;
	Diagnostic.Message = TEXT("The test double rejected the operation.");
	return Result;
}

bool HasDiagnostic(
	const TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics,
	FStringView Code)
{
	return Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

bool HasExactDiagnostic(
	const TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics,
	FStringView Path,
	FStringView Code)
{
	return Diagnostics.Num() == 1
		&& Diagnostics[0].Path == Path
		&& Diagnostics[0].Code == Code
		&& !Diagnostics[0].Message.IsEmpty();
}

void AddFailureDiagnostics(
	FAutomationTestBase& Test,
	FStringView Prefix,
	const TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Diagnostics)
	{
		Test.AddError(FString::Printf(
			TEXT("%.*s: %s at %s: %s"),
			Prefix.Len(), Prefix.GetData(),
			*Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

class FTestTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	explicit FTestTransaction(
		const FTestContext& Context,
		bool bInRequireRollbackActionBeforeFootprint = true)
		: bRequireRollbackActionBeforeFootprint(
			bInRequireRollbackActionBeforeFootprint)
	{
		Token.RequestId = Context.RequestId;
		Token.TransactionId = Context.TransactionId;
		Token.RuntimeRevision = Context.RuntimeRevision;
	}

	virtual FGuid GetTransactionId() const override
	{
		return Token.TransactionId;
	}

	virtual EAssetDocumentAtomicState GetState() const override
	{
		return State;
	}

	virtual const FAssetDocumentMutationToken& GetMutationToken() const override
	{
		return Token;
	}

	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override
	{
		if (!bRequireRollbackActionBeforeFootprint)
		{
			EnlistedFootprint.Append(Footprint.GetData(), Footprint.Num());
			State = EAssetDocumentAtomicState::Mutating;
			return MakeValidResult();
		}
		for (const FAssetDocumentMutationFootprint& Item : Footprint)
		{
			const int32 Index = Actions.IndexOfByPredicate(
				[&Item](const TSharedRef<IAssetDocumentRollbackAction>& Action)
				{
					return Action->GetRollbackComponentId()
						== Item.RollbackComponentId;
				});
			if (Index == INDEX_NONE)
			{
				return MakeTestFailure(TEXT("Test.MissingRollbackAction"));
			}
			if (!RollbackTargets.Contains(Index))
			{
				RollbackTargets.Add(Index);
			}
		}
		EnlistedFootprint.Append(Footprint.GetData(), Footprint.Num());
		State = EAssetDocumentAtomicState::Mutating;
		return MakeValidResult();
	}

	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		State = EAssetDocumentAtomicState::ReadyToCommit;
		return MakeValidResult();
	}

	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt&) override
	{
		State = EAssetDocumentAtomicState::Committed;
		Actions.Reset();
		RollbackTargets.Reset();
		return MakeValidResult();
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		FAssetDocumentContractValidationResult Result = MakeValidResult();
		for (int32 Target = RollbackTargets.Num() - 1; Target >= 0; --Target)
		{
			const FAssetDocumentContractValidationResult ActionResult =
				Actions[RollbackTargets[Target]]->Rollback(StableFailureCode);
			if (!ActionResult.bValid)
			{
				Result = ActionResult;
			}
		}
		State = Result.bValid
			? EAssetDocumentAtomicState::RolledBack
			: EAssetDocumentAtomicState::Poisoned;
		Actions.Reset();
		RollbackTargets.Reset();
		return Result;
	}

	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		const int32 CallIndex = EnlistCallCount++;
		ObservedComponentIds.Add(FString(Action->GetRollbackComponentId()));
		if (CallIndex == RejectEnlistAtCall)
		{
			return MakeTestFailure(TEXT("Test.RejectRollbackAction"));
		}
		Actions.Add(MoveTemp(Action));
		return MakeValidResult();
	}

	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		return MakeValidResult();
	}

	void SetState(EAssetDocumentAtomicState InState)
	{
		State = InState;
	}

	int32 RejectEnlistAtCall = INDEX_NONE;
	int32 EnlistCallCount = 0;
	TArray<FString> ObservedComponentIds;
	TArray<FAssetDocumentMutationFootprint> EnlistedFootprint;
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;

private:
	bool bRequireRollbackActionBeforeFootprint = true;
	FAssetDocumentMutationToken Token;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Live;
	TArray<int32> RollbackTargets;
};

class FObservedRollbackAction final : public IAssetDocumentRollbackAction
{
public:
	FObservedRollbackAction(
		FString InComponentId,
		TFunction<void()> InObservation)
		: ComponentId(MoveTemp(InComponentId))
		, Observation(MoveTemp(InObservation))
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return ComponentId;
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		(void)StableFailureCode;
		++RollbackCount;
		Observation();
		return MakeValidResult();
	}

	int32 RollbackCount = 0;

private:
	FString ComponentId;
	TFunction<void()> Observation;
};

void RequireFailureClosedCapture(
	FAutomationTestBase& Test,
	FStringView Label,
	const FAssetDocumentEditorTypedPreimageResult& Capture,
	const FTestTransaction& Transaction,
	FStringView ExpectedPath,
	FStringView ExpectedCode,
	int32 LifecycleBefore,
	int32 LifecycleAfter,
	bool bDirtyBefore,
	bool bDirtyAfter)
{
	const FString Prefix(Label);
	Test.TestFalse(*(Prefix + TEXT(" fails")), Capture.bSucceeded);
	Test.TestTrue(*(Prefix + TEXT(" has one exact diagnostic")),
		HasExactDiagnostic(Capture.Diagnostics, ExpectedPath, ExpectedCode));
	Test.TestEqual(*(Prefix + TEXT(" enlists no action")),
		Transaction.EnlistCallCount, 0);
	Test.TestTrue(*(Prefix + TEXT(" enlists no footprint")),
		Transaction.EnlistedFootprint.IsEmpty());
	Test.TestEqual(*(Prefix + TEXT(" emits no lifecycle")),
		LifecycleAfter, LifecycleBefore);
	Test.TestEqual(*(Prefix + TEXT(" preserves dirty state")),
		bDirtyAfter, bDirtyBefore);
}

class FOverriddenLease final : public IAssetDocumentVerifiedHandleLease
{
public:
	FOverriddenLease(
		FAssetDocumentOpaqueHandle InHandle,
		IAssetDocumentNativeHandlePayload& InPayload)
		: Handle(MoveTemp(InHandle))
		, Payload(&InPayload)
	{
	}

	virtual const FAssetDocumentOpaqueHandle& GetHandle() const override
	{
		return Handle;
	}

	virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override
	{
		check(Payload != nullptr);
		return *Payload;
	}

private:
	FAssetDocumentOpaqueHandle Handle;
	IAssetDocumentNativeHandlePayload* Payload = nullptr;
};

class FTestHandleResolver final : public IAssetDocumentHandleResolver
{
	struct FEntry
	{
		EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Object;
		EAssetDocumentHandleAccess Access =
			EAssetDocumentHandleAccess::ReadOnly;
		uint32 Generation = 1;
		TUniquePtr<IAssetDocumentNativeHandlePayload> Payload;
	};

	class FLease final : public IAssetDocumentVerifiedHandleLease
	{
	public:
		FLease(
			FAssetDocumentOpaqueHandle InHandle,
			IAssetDocumentNativeHandlePayload& InPayload)
			: Handle(MoveTemp(InHandle))
			, Payload(&InPayload)
		{
		}

		virtual const FAssetDocumentOpaqueHandle& GetHandle() const override
		{
			return Handle;
		}

		virtual IAssetDocumentNativeHandlePayload&
		GetPayloadChecked() const override
		{
			check(Payload != nullptr);
			return *Payload;
		}

	private:
		FAssetDocumentOpaqueHandle Handle;
		IAssetDocumentNativeHandlePayload* Payload = nullptr;
	};

public:
	explicit FTestHandleResolver(FTestContext InContext)
		: Context(MoveTemp(InContext))
	{
	}

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		FAssetDocumentIssuedHandleResult Result;
		if (!Payload || !Payload->IsAlive())
		{
			Result.Diagnostics.Add(
				MakeTestDiagnostic(TEXT("Test.InvalidPayload")));
			return Result;
		}
		FEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Kind = Kind;
		Entry.Access = Access;
		Entry.Payload = MoveTemp(Payload);
		FAssetDocumentOpaqueHandle Handle;
		Handle.RequestId = Context.RequestId;
		Handle.TransactionId = Context.TransactionId;
		Handle.RuntimeRevision = Context.RuntimeRevision;
		Handle.ProviderId = Context.ProviderId;
		Handle.Slot = Entries.Num();
		Handle.Generation = Entry.Generation;
		Handle.Kind = Kind;
		Handle.Access = Access;
		Result.Handle = MoveTemp(Handle);
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Validation) override
	{
		FAssetDocumentVerifiedLeaseResult Result;
		const FAssetDocumentContractValidationResult Metadata =
			ValidateAssetDocumentHandleMetadata(Handle, Validation);
		if (!Metadata.bValid)
		{
			Result.Diagnostics = Metadata.Diagnostics;
			return Result;
		}
		const int32 Index = static_cast<int32>(Handle.Slot) - 1;
		if (!Entries.IsValidIndex(Index))
		{
			Result.Diagnostics.Add(
				MakeTestDiagnostic(TEXT("HandleMetadata.StaleHandle")));
			return Result;
		}
		FEntry& Entry = Entries[Index];
		if (Entry.Generation != Handle.Generation
			|| Entry.Kind != Handle.Kind
			|| Entry.Access != Handle.Access
			|| !Entry.Payload
			|| !Entry.Payload->IsAlive())
		{
			Result.Diagnostics.Add(
				MakeTestDiagnostic(TEXT("HandleMetadata.StaleHandle")));
			return Result;
		}
		Result.Lease = MakeUnique<FLease>(Handle, *Entry.Payload);
		return Result;
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Invalidation) override
	{
		if (Invalidation.RequestId == Context.RequestId
			&& Invalidation.TransactionId == Context.TransactionId
			&& Invalidation.RuntimeRevision == Context.RuntimeRevision
			&& Invalidation.ProviderId == Context.ProviderId)
		{
			const uint32 InvalidatedHandleCount = static_cast<uint32>(Entries.Num());
			Entries.Reset();
			return {InvalidatedHandleCount};
		}
		return {};
	}

private:
	static FAssetDocumentRuntimeDiagnostic MakeTestDiagnostic(
		const TCHAR* Code)
	{
		FAssetDocumentRuntimeDiagnostic Diagnostic;
		Diagnostic.Path = TEXT("/AITypedPreimageTest/Handle");
		Diagnostic.Code = Code;
		Diagnostic.Message = TEXT("The test resolver rejected the handle.");
		return Diagnostic;
	}

	FTestContext Context;
	TArray<FEntry> Entries;
};

FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeNullValue()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const IAssetDocumentPrimitiveProvider& Provider,
	FStringView PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		Provider.DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

const FAssetDocumentPrimitiveHandleResult* FindHandleResult(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Name)
{
	for (const FAssetDocumentPrimitiveHandleResult& Result :
		Outcome.HandleResults)
	{
		if (Result.Name == Name)
		{
			return &Result;
		}
	}
	return nullptr;
}

FAssetDocumentPrimitiveInvocation MakeInvocation(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FTestContext& Context,
	FAssetDocumentRuntimeValueDto Value)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = Descriptor.PrimitiveId;
	Invocation.Version = Descriptor.Version;
	Invocation.RequestId = Context.RequestId;
	Invocation.TransactionId = Context.TransactionId;
	Invocation.RuntimeRevision = Context.RuntimeRevision;
	Invocation.Phase = Descriptor.AllowedPhases[0];
	Invocation.ValueArguments = MoveTemp(Value);
	return Invocation;
}

FAssetDocumentHandleValidationContext MakeLeaseContext(
	const FTestContext& Context,
	EAssetDocumentHandleAccess Access)
{
	FAssetDocumentHandleValidationContext Validation;
	Validation.RequestId = Context.RequestId;
	Validation.TransactionId = Context.TransactionId;
	Validation.RuntimeRevision = Context.RuntimeRevision;
	Validation.ProviderId = Context.ProviderId;
	Validation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Validation.RequiredKind = EAssetDocumentHandleKind::Asset;
	Validation.RequiredAccess = Access;
	return Validation;
}

struct FAssetFixture
{
	UObject* Asset = nullptr;
	UPackage* Package = nullptr;
	FTestContext Context;
	TUniquePtr<FTestHandleResolver> Resolver;
	TUniquePtr<FTestTransaction> Transaction;
	TUniquePtr<IAssetDocumentVerifiedHandleLease> Lease;

	void Cleanup()
	{
		Lease.Reset();
		Resolver.Reset();
		Transaction.Reset();
		if (Package != nullptr)
		{
			Package->SetDirtyFlag(false);
		}
		if (Asset != nullptr && IsValid(Asset))
		{
			ObjectTools::DeleteObjects(
				{Asset},
				false,
				ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
		}
		Asset = nullptr;
		Package = nullptr;
	}
};

bool ResolveExistingAssetFixture(
	FAutomationTestBase& Test,
	UObject& Asset,
	FAssetFixture& OutFixture)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* ResolveDescriptor =
		FindDescriptor(*Provider, TEXTVIEW("editor.asset.resolve_existing.v1"));
	if (!Test.TestNotNull(TEXT("resolve descriptor exists"), ResolveDescriptor))
	{
		return false;
	}
	OutFixture.Asset = &Asset;
	OutFixture.Package = Asset.GetOutermost();
	OutFixture.Resolver = MakeUnique<FTestHandleResolver>(OutFixture.Context);
	OutFixture.Transaction = MakeUnique<FTestTransaction>(OutFixture.Context);
	FAssetDocumentPrimitiveInvocation ResolveInvocation = MakeInvocation(
		*ResolveDescriptor,
		OutFixture.Context,
		MakeObjectValue(
			{TEXT("path"), TEXT("expected_class")},
			{MakeStringValue(Asset.GetPathName()),
				MakeStringValue(Asset.GetClass()->GetPathName())}));
	ResolveInvocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	const FAssetDocumentPrimitiveOutcome ResolveOutcome = Provider->Invoke(
		ResolveInvocation, *OutFixture.Resolver, nullptr);
	const FAssetDocumentPrimitiveHandleResult* Resolved =
		FindHandleResult(ResolveOutcome, TEXTVIEW("asset_mutable"));
	if (!Test.TestTrue(TEXT("real resolve_existing succeeds"),
		ResolveOutcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		|| !Test.TestNotNull(TEXT("resolved mutable handle exists"), Resolved)
		|| !Test.TestEqual(TEXT("one resolved mutable handle"),
			Resolved ? Resolved->Handles.Num() : 0, 1))
	{
		return false;
	}
	FAssetDocumentVerifiedLeaseResult ActiveLease =
		OutFixture.Resolver->AcquireVerifiedLease(
			Resolved->Handles[0],
			MakeLeaseContext(
				OutFixture.Context, EAssetDocumentHandleAccess::Mutable));
	OutFixture.Lease = MoveTemp(ActiveLease.Lease);
	return Test.TestNotNull(
		TEXT("resolved exact Editor payload leases"), OutFixture.Lease.Get());
}

bool CreateFactoryAsset(
	FAutomationTestBase& Test,
	UClass* AssetClass,
	FAssetFixture& OutFixture)
{
	if (!Test.TestNotNull(TEXT("test asset class exists"), AssetClass))
	{
		return false;
	}
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* ClassDescriptor =
		FindDescriptor(*Provider, TEXTVIEW("editor.class.resolve_canonical.v1"));
	const FAssetDocumentPrimitiveDescriptor* CreateDescriptor =
		FindDescriptor(*Provider, TEXTVIEW("editor.asset.create_by_factory.v1"));
	const FAssetDocumentPrimitiveDescriptor* ResolveDescriptor =
		FindDescriptor(*Provider, TEXTVIEW("editor.asset.resolve_existing.v1"));
	if (!Test.TestNotNull(TEXT("class descriptor exists"), ClassDescriptor)
		|| !Test.TestNotNull(TEXT("factory descriptor exists"), CreateDescriptor)
		|| !Test.TestNotNull(TEXT("resolve descriptor exists"), ResolveDescriptor))
	{
		return false;
	}

	FTestContext CreationContext;
	FTestHandleResolver CreationResolver(CreationContext);
	FTestTransaction CreationTransaction(CreationContext, false);
	FAssetDocumentPrimitiveInvocation ClassInvocation = MakeInvocation(
		*ClassDescriptor,
		CreationContext,
		MakeObjectValue(
			{TEXT("class"), TEXT("expected_base")},
			{MakeStringValue(AssetClass->GetPathName()), MakeNullValue()}));
	const FAssetDocumentPrimitiveOutcome ClassOutcome = Provider->Invoke(
		ClassInvocation, CreationResolver, nullptr);
	const FAssetDocumentPrimitiveHandleResult* ClassResult =
		FindHandleResult(ClassOutcome, TEXTVIEW("class"));
	if (!Test.TestTrue(TEXT("test asset class resolves"),
		ClassOutcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		|| !Test.TestNotNull(TEXT("class handle exists"), ClassResult)
		|| !Test.TestEqual(TEXT("one class handle"),
			ClassResult ? ClassResult->Handles.Num() : 0, 1))
	{
		return false;
	}

	const FString AssetName = FString::Printf(
		TEXT("DA_AITypedPreimage_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackagePath =
		TEXT("/Game/AssetDocumentTests/AITypedPreimage");
	FAssetDocumentPrimitiveInvocation CreateInvocation = MakeInvocation(
		*CreateDescriptor,
		CreationContext,
		MakeObjectValue(
			{TEXT("factory_id"), TEXT("package_path"), TEXT("asset_name")},
			{MakeStringValue(TEXT("data_asset_factory.v1")),
				MakeStringValue(PackagePath),
				MakeStringValue(AssetName)}));
	CreateInvocation.HandleArguments.Add(
		{TEXT("asset_class"), ClassResult->Handles[0]});
	const FAssetDocumentPrimitiveOutcome CreateOutcome = Provider->Invoke(
		CreateInvocation, CreationResolver, &CreationTransaction);
	const FAssetDocumentPrimitiveHandleResult* Created =
		FindHandleResult(CreateOutcome, TEXTVIEW("asset_mutable"));
	if (!Test.TestTrue(TEXT("test asset factory creation succeeds"),
		CreateOutcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		|| !Test.TestNotNull(TEXT("created mutable handle exists"), Created)
		|| !Test.TestEqual(TEXT("one created mutable handle"),
			Created ? Created->Handles.Num() : 0, 1))
	{
		return false;
	}
	const FString CanonicalObjectPath = FString::Printf(
		TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName);
	UObject* CreatedAsset = FSoftObjectPath(CanonicalObjectPath).ResolveObject();
	if (!Test.TestNotNull(TEXT("factory created canonical native asset"),
		CreatedAsset)
		|| !Test.TestTrue(TEXT("factory created exact asset class"),
			CreatedAsset && CreatedAsset->GetClass() == AssetClass))
	{
		return false;
	}

	OutFixture.Asset = CreatedAsset;
	OutFixture.Package = CreatedAsset->GetOutermost();
	OutFixture.Resolver = MakeUnique<FTestHandleResolver>(OutFixture.Context);
	OutFixture.Transaction = MakeUnique<FTestTransaction>(OutFixture.Context);
	FAssetDocumentPrimitiveInvocation ResolveInvocation = MakeInvocation(
		*ResolveDescriptor,
		OutFixture.Context,
		MakeObjectValue(
			{TEXT("path"), TEXT("expected_class")},
			{MakeStringValue(CreatedAsset->GetPathName()),
				MakeStringValue(AssetClass->GetPathName())}));
	ResolveInvocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	const FAssetDocumentPrimitiveOutcome ResolveOutcome = Provider->Invoke(
		ResolveInvocation, *OutFixture.Resolver, nullptr);
	const FAssetDocumentPrimitiveHandleResult* Resolved =
		FindHandleResult(ResolveOutcome, TEXTVIEW("asset_mutable"));
	if (!Test.TestTrue(TEXT("real resolve_existing succeeds"),
		ResolveOutcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		|| !Test.TestNotNull(TEXT("resolved mutable handle exists"), Resolved)
		|| !Test.TestEqual(TEXT("one resolved mutable handle"),
			Resolved ? Resolved->Handles.Num() : 0, 1))
	{
		OutFixture.Cleanup();
		return false;
	}
	FAssetDocumentVerifiedLeaseResult ActiveLease =
		OutFixture.Resolver->AcquireVerifiedLease(
			Resolved->Handles[0],
			MakeLeaseContext(
				OutFixture.Context, EAssetDocumentHandleAccess::Mutable));
	OutFixture.Lease = MoveTemp(ActiveLease.Lease);
	if (!Test.TestNotNull(TEXT("resolved exact Editor payload leases"),
		OutFixture.Lease.Get()))
	{
		OutFixture.Cleanup();
		return false;
	}
	return true;
}

TSharedPtr<IPropertyRowGenerator> MakeRows(UObject& Object)
{
	FPropertyRowGeneratorArgs Args;
	FPropertyEditorModule& PropertyEditor =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(
			TEXT("PropertyEditor"));
	TSharedRef<IPropertyRowGenerator> Rows =
		PropertyEditor.CreatePropertyRowGenerator(Args);
	Rows->OnRowsRefreshed().AddLambda([]() {});
	Rows->SetObjects({&Object});
	return Rows;
}

TSharedPtr<IPropertyHandle> FindHandle(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	FName Name,
	bool bRequireTopLevel = false)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		FProperty* Property = Handle.IsValid() ? Handle->GetProperty() : nullptr;
		if (Property != nullptr
			&& Property->GetFName() == Name
			&& (!bRequireTopLevel
				|| Property->GetOwnerProperty() == Property))
		{
			return Handle;
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		if (const TSharedPtr<IPropertyHandle> Child =
			FindHandle(Children, Name, bRequireTopLevel))
		{
			return Child;
		}
	}
	return nullptr;
}

TSharedPtr<IPropertyHandle> RequireHandle(
	FAutomationTestBase& Test,
	const TSharedPtr<IPropertyRowGenerator>& Rows,
	FName Name,
	bool bTopLevel = true)
{
	TSharedPtr<IPropertyHandle> Handle = Rows.IsValid()
		? FindHandle(Rows->GetRootTreeNodes(), Name, bTopLevel)
		: nullptr;
	Test.TestTrue(
		*FString::Printf(TEXT("property row exists: %s"), *Name.ToString()),
		Handle.IsValid());
	return Handle;
}

template <typename T>
bool RequireExactTypedReadback(
	FAutomationTestBase& Test,
	const TSharedPtr<IPropertyRowGenerator>& Rows,
	FName Name,
	const T& Expected,
	const TCHAR* Label)
{
	const TSharedPtr<IPropertyHandle> Handle = RequireHandle(Test, Rows, Name);
	T Current{};
	const bool bRead = Handle.IsValid()
		&& Handle->GetValue(Current) == FPropertyAccess::Success;
	Test.TestTrue(*(FString(Label) + TEXT(" PropertyEditor GetValue succeeds")),
		bRead);
	Test.TestTrue(*(FString(Label) + TEXT(" PropertyEditor readback is exact")),
		bRead && Current == Expected);
	return bRead && Current == Expected;
}

bool EnlistFootprints(
	FTestTransaction& Transaction,
	TConstArrayView<FString> ComponentIds,
	FStringView Prefix)
{
	TArray<FAssetDocumentMutationFootprint> Footprints;
	for (int32 Index = 0; Index < ComponentIds.Num(); ++Index)
	{
		Footprints.Add({
			EAssetDocumentMutationFootprintKind::UObjectState,
			FString::Printf(TEXT("%.*s.%d"),
				Prefix.Len(), Prefix.GetData(), Index),
			ComponentIds[Index]});
	}
	return Transaction.EnlistFootprint(Footprints).bValid;
}

void ApplyKeys(
	UBlackboardData& Blackboard,
	TArray<FBlackboardEntry> Replacement)
{
	FArrayProperty* KeysProperty = CastFieldChecked<FArrayProperty>(
		UBlackboardData::StaticClass()->FindPropertyByName(
			GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys)));
	Blackboard.PreEditChange(KeysProperty);
	Blackboard.Keys = MoveTemp(Replacement);
	FPropertyChangedEvent Event(
		KeysProperty,
		EPropertyChangeType::ArrayClear | EPropertyChangeType::ValueSet);
	Event.MemberProperty = KeysProperty;
	Blackboard.PostEditChangeProperty(Event);
	Blackboard.UpdateParentKeys();
	Blackboard.UpdateKeyIDs();
	Blackboard.UpdateIfHasSynchronizedKeys();
	Blackboard.PropagateKeyChangesToDerivedBlackboardAssets();
}

FBlackboardEntry MakeEntry(
	FName Name,
	UBlackboardKeyType& KeyType,
	const TCHAR* Description,
	FName Category,
	bool bSynced)
{
	FBlackboardEntry Entry;
	Entry.EntryName = Name;
#if WITH_EDITORONLY_DATA
	Entry.EntryDescription = Description;
	Entry.EntryCategory = Category;
#endif
	Entry.KeyType = &KeyType;
	Entry.bInstanceSynced = bSynced;
	return Entry;
}

bool RunKeyTypePositive(FAutomationTestBase& Test)
{
	FAssetFixture Fixture;
	if (!CreateFactoryAsset(
		Test,
		UAssetDocumentAITypedPreimageTestSelectorOwner::StaticClass(),
		Fixture))
	{
		return false;
	}
	UAssetDocumentAITypedPreimageTestKeyType* KeyType =
		NewObject<UAssetDocumentAITypedPreimageTestKeyType>(Fixture.Asset);
	UObject* OriginalObject =
		NewObject<UAssetDocumentAITypedPreimageTestSelectorOwner>(Fixture.Asset);
	TWeakObjectPtr<UObject> OriginalObjectWeak = OriginalObject;
	UObject* ReplacementObject =
		NewObject<UAssetDocumentAITypedPreimageTestSelectorOwner>(Fixture.Asset);
	const FVector OriginalVector(
		1.2345678901234567,
		-2.345678901234567,
		3.456789012345678);
	const FVector2D OriginalVector2D(
		4.567890123456789,
		-5.67890123456789);
	const FVector4 OriginalVector4(
		6.789012345678901,
		-7.890123456789012,
		8.901234567890123,
		-9.012345678901234);
	const FQuat OriginalQuat(
		0.1234567890123456,
		-0.2345678901234567,
		0.3456789012345678,
		0.9012345678901234);
	const FRotator OriginalRotator(
		10.123456789012345,
		-20.234567890123456,
		30.345678901234567);
	const FString TextNamespace = TextNamespaceUtil::BuildFullNamespace(
		TEXT("AssetDocumentAITypedPreimageTests"),
		TextNamespaceUtil::EnsurePackageNamespace(Fixture.Package),
		true);
	const FText OriginalText = FText::ChangeKey(
		TextNamespace,
		TEXT("OriginalTextIdentity"),
		FText::FromString(TEXT("same-visible-text")));
	const FText ReplacementText = FText::ChangeKey(
		TextNamespace,
		TEXT("ReplacementTextIdentity"),
		FText::FromString(TEXT("same-visible-text")));
	KeyType->BoolValue = true;
	KeyType->Int32Value = 17;
	KeyType->Int8Value = -8;
	KeyType->Int16Value = -160;
	KeyType->Int64Value = -6400;
	KeyType->ByteValue = 3;
	KeyType->UInt16Value = 160;
	KeyType->UInt32Value = 3200;
	KeyType->UInt64Value = 6400;
	KeyType->FloatValue = 1.25f;
	KeyType->DoubleValue = 2.5;
	KeyType->StringValue = TEXT("original");
	KeyType->TextValue = OriginalText;
	KeyType->NameValue = TEXT("OriginalName");
	KeyType->VectorValue = OriginalVector;
	KeyType->Vector2DValue = OriginalVector2D;
	KeyType->Vector4Value = OriginalVector4;
	KeyType->QuatValue = OriginalQuat;
	KeyType->RotatorValue = OriginalRotator;
	KeyType->ObjectValue = OriginalObject;
	KeyType->ClassValue = UDataAsset::StaticClass();
	KeyType->EnumValue = StaticEnum<EAssetDocumentAITypedPreimageTestEnum>();
	KeyType->PostEditChangePropertyCount = 0;
	Fixture.Package->SetDirtyFlag(false);

	TSharedPtr<IPropertyRowGenerator> Rows = MakeRows(*KeyType);
	const FName SupportedNames[] = {
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, BoolValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, Int32Value),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, Int8Value),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, Int16Value),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, Int64Value),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, ByteValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, UInt16Value),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, UInt32Value),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, UInt64Value),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, FloatValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, DoubleValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, StringValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, TextValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, NameValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, VectorValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, Vector2DValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, Vector4Value),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, QuatValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, RotatorValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, ObjectValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, ClassValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, EnumValue),
	};
	TArray<TSharedRef<IPropertyHandle>> Handles;
	for (FName Name : SupportedNames)
	{
		const TSharedPtr<IPropertyHandle> Handle =
			RequireHandle(Test, Rows, Name);
		if (!Handle.IsValid())
		{
			Fixture.Cleanup();
			return false;
		}
		Handles.Add(Handle.ToSharedRef());
	}
	CastFieldChecked<FTextProperty>(Handles[12]->GetProperty());
	Test.TestFalse(TEXT("text fixture identities differ despite equal display"),
		FTextInspector::GetTextId(OriginalText)
			== FTextInspector::GetTextId(ReplacementText));
	const TArray<FString> Components = {
		TEXT("rollback.ai.keytype.values"),
		TEXT("rollback.ai.keytype.package")};
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			*Fixture.Lease,
			*KeyType,
			Rows.ToSharedRef(),
			Handles,
			Components,
			*Fixture.Transaction);
	if (!Test.TestTrue(TEXT("KeyType typed capture succeeds"),
		Capture.bSucceeded)
		|| !Test.TestTrue(TEXT("KeyType footprints enlist"),
			EnlistFootprints(
				*Fixture.Transaction,
				TConstArrayView<FString>(Components).Left(1),
				TEXTVIEW("keytype"))))
	{
		Fixture.Cleanup();
		return false;
	}

	Handles[0]->SetValue(false, EPropertyValueSetFlags::NotTransactable);
	Handles[1]->SetValue(99, EPropertyValueSetFlags::NotTransactable);
	Handles[2]->SetValue(int8(-80), EPropertyValueSetFlags::NotTransactable);
	Handles[3]->SetValue(int16(-1600), EPropertyValueSetFlags::NotTransactable);
	Handles[4]->SetValue(int64(-64000), EPropertyValueSetFlags::NotTransactable);
	Handles[5]->SetValue(uint8(9), EPropertyValueSetFlags::NotTransactable);
	Handles[6]->SetValue(uint16(1600), EPropertyValueSetFlags::NotTransactable);
	Handles[7]->SetValue(uint32(32000), EPropertyValueSetFlags::NotTransactable);
	Handles[8]->SetValue(uint64(64000), EPropertyValueSetFlags::NotTransactable);
	Handles[9]->SetValue(9.25f, EPropertyValueSetFlags::NotTransactable);
	Handles[10]->SetValue(9.5, EPropertyValueSetFlags::NotTransactable);
	Handles[11]->SetValue(
		FString(TEXT("replacement")),
		EPropertyValueSetFlags::NotTransactable);
	Handles[12]->SetValue(
		ReplacementText,
		EPropertyValueSetFlags::NotTransactable);
	Handles[13]->SetValue(
		FName(TEXT("ReplacementName")),
		EPropertyValueSetFlags::NotTransactable);
	Handles[14]->SetValue(
		FVector(9.0, 8.0, 7.0),
		EPropertyValueSetFlags::NotTransactable);
	Handles[15]->SetValue(
		FVector2D(9.0, 8.0),
		EPropertyValueSetFlags::NotTransactable);
	Handles[16]->SetValue(
		FVector4(9.0, 8.0, 7.0, 6.0),
		EPropertyValueSetFlags::NotTransactable);
	Handles[17]->SetValue(
		FQuat(FVector::ForwardVector, 1.0),
		EPropertyValueSetFlags::NotTransactable);
	Handles[18]->SetValue(
		FRotator(7.0, 8.0, 9.0),
		EPropertyValueSetFlags::NotTransactable);
	Handles[19]->SetValue(
		ReplacementObject,
		EPropertyValueSetFlags::NotTransactable);
	Handles[20]->SetValue(
		UObject::StaticClass(),
		EPropertyValueSetFlags::NotTransactable);
	const UObject* NullEnum = nullptr;
	Handles[21]->SetValue(NullEnum, EPropertyValueSetFlags::NotTransactable);
	OriginalObject = nullptr;
	CollectGarbage(RF_NoFlags);
	Test.TestTrue(TEXT("captured object identity survives forced GC"),
		OriginalObjectWeak.IsValid());
	Rows->SetObjects({KeyType});
	Handles.Reset();
	Rows.Reset();
	const FAssetDocumentContractValidationResult Rollback =
		Fixture.Transaction->Rollback(TEXTVIEW("Test.KeyType.Rollback"));
	if (!Rollback.bValid)
	{
		AddFailureDiagnostics(Test, TEXTVIEW("KeyType rollback"),
			Rollback.Diagnostics);
	}
	Test.TestTrue(TEXT("KeyType rollback succeeds"), Rollback.bValid);
	Test.TestTrue(TEXT("bool restores"), KeyType->BoolValue);
	Test.TestEqual(TEXT("int restores"), KeyType->Int32Value, 17);
	Test.TestEqual(TEXT("int8 restores"), KeyType->Int8Value, int8(-8));
	Test.TestEqual(TEXT("int16 restores"), KeyType->Int16Value, int16(-160));
	Test.TestEqual(TEXT("int64 restores"), KeyType->Int64Value, int64(-6400));
	Test.TestEqual(TEXT("byte restores"), KeyType->ByteValue, uint8(3));
	Test.TestEqual(TEXT("uint16 restores"), KeyType->UInt16Value, uint16(160));
	Test.TestEqual(TEXT("uint32 restores"), KeyType->UInt32Value, uint32(3200));
	Test.TestEqual(TEXT("uint64 restores"), KeyType->UInt64Value, uint64(6400));
	Test.TestEqual(TEXT("float restores"), KeyType->FloatValue, 1.25f);
	Test.TestEqual(TEXT("double restores"), KeyType->DoubleValue, 2.5);
	Test.TestEqual(TEXT("string restores"),
		KeyType->StringValue, FString(TEXT("original")));
	Test.TestEqual(TEXT("text namespace restores exactly"),
		FTextInspector::GetNamespace(KeyType->TextValue).Get(FString()),
		FTextInspector::GetNamespace(OriginalText).Get(FString()));
	Test.TestEqual(TEXT("text key restores exactly"),
		FTextInspector::GetKey(KeyType->TextValue).Get(FString()),
		FTextInspector::GetKey(OriginalText).Get(FString()));
	const FString* RestoredTextSource =
		FTextInspector::GetSourceString(KeyType->TextValue);
	const FString* OriginalTextSource =
		FTextInspector::GetSourceString(OriginalText);
	Test.TestTrue(TEXT("text source restores exactly"),
		RestoredTextSource != nullptr
			&& OriginalTextSource != nullptr
			&& *RestoredTextSource == *OriginalTextSource);
	Test.TestEqual(TEXT("text flags restore exactly"),
		FTextInspector::GetFlags(KeyType->TextValue),
		FTextInspector::GetFlags(OriginalText));
	Test.TestEqual(TEXT("name restores"),
		KeyType->NameValue, FName(TEXT("OriginalName")));
	Test.TestTrue(TEXT("vector restores bit-exactly"),
		KeyType->VectorValue == OriginalVector);
	Test.TestTrue(TEXT("vector2d restores bit-exactly"),
		KeyType->Vector2DValue == OriginalVector2D);
	Test.TestTrue(TEXT("vector4 restores bit-exactly"),
		KeyType->Vector4Value == OriginalVector4);
	Test.TestTrue(TEXT("quat restores bit-exactly"),
		KeyType->QuatValue == OriginalQuat);
	Test.TestTrue(TEXT("rotator restores bit-exactly"),
		KeyType->RotatorValue == OriginalRotator);
	const TSharedPtr<IPropertyRowGenerator> ReadbackRows = MakeRows(*KeyType);
	RequireExactTypedReadback(
		Test, ReadbackRows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, VectorValue),
		OriginalVector, TEXT("FVector"));
	RequireExactTypedReadback(
		Test, ReadbackRows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, Vector2DValue),
		OriginalVector2D, TEXT("FVector2D"));
	RequireExactTypedReadback(
		Test, ReadbackRows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, Vector4Value),
		OriginalVector4, TEXT("FVector4"));
	RequireExactTypedReadback(
		Test, ReadbackRows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, QuatValue),
		OriginalQuat, TEXT("FQuat"));
	RequireExactTypedReadback(
		Test, ReadbackRows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, RotatorValue),
		OriginalRotator, TEXT("FRotator"));
	Test.TestEqual(TEXT("object identity restores"),
		KeyType->ObjectValue.Get(), OriginalObjectWeak.Get());
	Test.TestEqual(TEXT("class identity restores"),
		KeyType->ClassValue.Get(), UDataAsset::StaticClass());
	Test.TestEqual(TEXT("enum identity restores"),
		KeyType->EnumValue.Get(),
		StaticEnum<EAssetDocumentAITypedPreimageTestEnum>());
	Test.TestFalse(TEXT("original clean dirty bit restores"),
		Fixture.Package->IsDirty());
	Test.TestTrue(TEXT("KeyType lifecycle runs before dirty-last"),
		KeyType->bPackageDirtyDuringPostEditChangeProperty);
	Fixture.Cleanup();
	return Rollback.bValid;
}

bool RunTextHistoryCollisionPositive(FAutomationTestBase& Test)
{
	FAssetFixture Fixture;
	if (!CreateFactoryAsset(
		Test,
		UAssetDocumentAITypedPreimageTestSelectorOwner::StaticClass(),
		Fixture))
	{
		return false;
	}
	UAssetDocumentAITypedPreimageTestKeyType* KeyType =
		NewObject<UAssetDocumentAITypedPreimageTestKeyType>(Fixture.Asset);
	FNumberFormattingOptions Formatting;
	Formatting.MinimumFractionalDigits = 3;
	Formatting.MaximumFractionalDigits = 3;
	const FText OriginalText = FText::AsNumber(1.23441, &Formatting);
	const FText CollisionText = FText::AsNumber(1.23449, &Formatting);
	Test.TestTrue(TEXT("generated-history fixture displays collide"),
		OriginalText.EqualTo(CollisionText));
	Test.TestFalse(TEXT("generated-history fixture identities differ"),
		OriginalText.IdenticalTo(
			CollisionText,
			ETextIdenticalModeFlags::DeepCompare
				| ETextIdenticalModeFlags::LexicalCompareInvariants));
	Test.TestFalse(TEXT("rollback comparator rejects display collision"),
		UE::AssetDocument::AI::TypedPreimage::Blackboard::
			AreAuthoredTextsIdenticalForRollback(
				OriginalText, CollisionText));
	KeyType->TextValue = OriginalText;
	Fixture.Package->SetDirtyFlag(false);
	TSharedPtr<IPropertyRowGenerator> Rows = MakeRows(*KeyType);
	const TSharedPtr<IPropertyHandle> TextHandle = RequireHandle(
		Test,
		Rows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, TextValue));
	if (!TextHandle.IsValid())
	{
		Fixture.Cleanup();
		return false;
	}
	const TArray<TSharedRef<IPropertyHandle>> Handles = {
		TextHandle.ToSharedRef()};
	const TArray<FString> Components = {
		TEXT("rollback.ai.keytype.text-history"),
		TEXT("rollback.ai.keytype.text-history.package")};
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			*Fixture.Lease,
			*KeyType,
			Rows.ToSharedRef(),
			Handles,
			Components,
			*Fixture.Transaction);
	if (!Test.TestTrue(TEXT("generated-history capture succeeds"),
		Capture.bSucceeded)
		|| !Test.TestTrue(TEXT("generated-history footprint enlists"),
			EnlistFootprints(
				*Fixture.Transaction,
				TConstArrayView<FString>(Components).Left(1),
				TEXTVIEW("keytype-text-history"))))
	{
		Fixture.Cleanup();
		return false;
	}
	TextHandle->SetValue(
		CollisionText, EPropertyValueSetFlags::NotTransactable);
	Rows->SetObjects({KeyType});
	Rows.Reset();
	const FAssetDocumentContractValidationResult Rollback =
		Fixture.Transaction->Rollback(
			TEXTVIEW("Test.KeyType.TextHistoryRollback"));
	if (!Rollback.bValid)
	{
		AddFailureDiagnostics(
			Test, TEXTVIEW("generated-history rollback"), Rollback.Diagnostics);
	}
	Test.TestTrue(TEXT("generated-history rollback succeeds"), Rollback.bValid);
	Test.TestTrue(TEXT("generated history restores exactly"),
		KeyType->TextValue.IdenticalTo(
			OriginalText,
			ETextIdenticalModeFlags::DeepCompare
				| ETextIdenticalModeFlags::LexicalCompareInvariants));
	Test.TestTrue(TEXT("rollback comparator accepts exact generated history"),
		UE::AssetDocument::AI::TypedPreimage::Blackboard::
			AreAuthoredTextsIdenticalForRollback(
				KeyType->TextValue, OriginalText));
	Test.TestFalse(TEXT("generated-history dirty preimage restores"),
		Fixture.Package->IsDirty());
	Fixture.Cleanup();
	return Rollback.bValid;
}

bool RunStructKeyTypePositive(FAutomationTestBase& Test)
{
	FAssetFixture Fixture;
	if (!CreateFactoryAsset(
		Test,
		UAssetDocumentAITypedPreimageTestSelectorOwner::StaticClass(),
		Fixture))
	{
		return false;
	}
	UBlackboardKeyType_Struct* KeyType =
		NewObject<UBlackboardKeyType_Struct>(Fixture.Asset);
	FProperty* DefaultValueProperty = FindFProperty<FProperty>(
		UBlackboardKeyType_Struct::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Struct, DefaultValue));
	KeyType->PreEditChange(DefaultValueProperty);
	KeyType->DefaultValue.InitializeAs(FBlackboardEntry::StaticStruct());
	FBlackboardEntry& OriginalPayload =
		KeyType->DefaultValue.GetMutable<FBlackboardEntry>();
	OriginalPayload.EntryName = TEXT("InstancedOriginal");
	OriginalPayload.bInstanceSynced = true;
#if WITH_EDITORONLY_DATA
	OriginalPayload.EntryDescription = TEXT("instanced-original-description");
	OriginalPayload.EntryCategory = TEXT("InstancedOriginalCategory");
#endif
	const FBlackboardEntry ExpectedPayload = OriginalPayload;
	FPropertyChangedEvent InitialEvent(
		DefaultValueProperty, EPropertyChangeType::ValueSet);
	KeyType->PostEditChangeProperty(InitialEvent);
	const bool bOriginalHasInstance = KeyType->HasInstance();
	Fixture.Package->SetDirtyFlag(false);
	TSharedPtr<IPropertyRowGenerator> Rows = MakeRows(*KeyType);
	TSharedPtr<IPropertyHandle> DefaultValue = RequireHandle(
		Test, Rows,
		GET_MEMBER_NAME_CHECKED(
			UBlackboardKeyType_Struct, DefaultValue));
	if (!DefaultValue.IsValid())
	{
		Fixture.Cleanup();
		return false;
	}
	const TArray<FString> Components = {
		TEXT("rollback.ai.keytype.struct"),
		TEXT("rollback.ai.keytype.struct.package")};
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			*Fixture.Lease,
			*KeyType,
			Rows.ToSharedRef(),
			TArray<TSharedRef<IPropertyHandle>>({DefaultValue.ToSharedRef()}),
			Components,
			*Fixture.Transaction);
	Test.TestTrue(TEXT("FInstancedStruct capture succeeds"), Capture.bSucceeded);
	EnlistFootprints(
		*Fixture.Transaction,
		TConstArrayView<FString>(Components).Left(1),
		TEXTVIEW("struct"));
	FProperty* Property = DefaultValue->GetProperty();
	KeyType->PreEditChange(Property);
	KeyType->DefaultValue.Reset();
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	KeyType->PostEditChangeProperty(Event);
	Rows->SetObjects({KeyType});
	DefaultValue.Reset();
	Rows.Reset();
	const FAssetDocumentContractValidationResult Rollback =
		Fixture.Transaction->Rollback(TEXTVIEW("Test.Struct.Rollback"));
	if (!Rollback.bValid)
	{
		AddFailureDiagnostics(Test, TEXTVIEW("Struct rollback"),
			Rollback.Diagnostics);
	}
	Test.TestTrue(TEXT("FInstancedStruct rollback succeeds"), Rollback.bValid);
	Test.TestTrue(TEXT("FInstancedStruct type restores"),
		KeyType->DefaultValue.GetScriptStruct()
			== FBlackboardEntry::StaticStruct());
	if (KeyType->DefaultValue.GetScriptStruct()
		== FBlackboardEntry::StaticStruct())
	{
		const FBlackboardEntry& RestoredPayload =
			KeyType->DefaultValue.Get<FBlackboardEntry>();
		Test.TestEqual(TEXT("FInstancedStruct authored entry name restores"),
			RestoredPayload.EntryName, ExpectedPayload.EntryName);
		Test.TestEqual(TEXT("FInstancedStruct authored sync bit restores"),
			RestoredPayload.bInstanceSynced != 0,
			ExpectedPayload.bInstanceSynced != 0);
#if WITH_EDITORONLY_DATA
		Test.TestEqual(TEXT("FInstancedStruct authored description restores"),
			RestoredPayload.EntryDescription,
			ExpectedPayload.EntryDescription);
		Test.TestEqual(TEXT("FInstancedStruct authored category restores"),
			RestoredPayload.EntryCategory,
			ExpectedPayload.EntryCategory);
#endif
	}
	Test.TestEqual(TEXT("Struct derived HasInstance restores"),
		KeyType->HasInstance(), bOriginalHasInstance);
	Test.TestFalse(TEXT("Struct package dirty restores"),
		Fixture.Package->IsDirty());
	Fixture.Cleanup();
	return Rollback.bValid;
}

bool RunEnumKeyTypePositive(FAutomationTestBase& Test)
{
	FAssetFixture Fixture;
	if (!CreateFactoryAsset(
		Test,
		UAssetDocumentAITypedPreimageTestSelectorOwner::StaticClass(),
		Fixture))
	{
		return false;
	}
	UBlackboardKeyType_Enum* KeyType =
		NewObject<UBlackboardKeyType_Enum>(Fixture.Asset);
	TSharedPtr<IPropertyRowGenerator> Rows = MakeRows(*KeyType);
	TSharedPtr<IPropertyHandle> EnumName = RequireHandle(
		Test, Rows, GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Enum, EnumName));
	if (!EnumName.IsValid())
	{
		Fixture.Cleanup();
		return false;
	}
	const FString OriginalEnumName =
		StaticEnum<EAssetDocumentAITypedPreimageTestEnum>()->GetPathName();
	EnumName->SetValue(
		OriginalEnumName, EPropertyValueSetFlags::NotTransactable);
	if (!Test.TestTrue(TEXT("enum name establishes valid derived state"),
		KeyType->bIsEnumNameValid != 0))
	{
		Fixture.Cleanup();
		return false;
	}
	Fixture.Package->SetDirtyFlag(false);
	const TArray<FString> Components = {
		TEXT("rollback.ai.keytype.enum"),
		TEXT("rollback.ai.keytype.enum.package")};
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			*Fixture.Lease,
			*KeyType,
			Rows.ToSharedRef(),
			TArray<TSharedRef<IPropertyHandle>>({EnumName.ToSharedRef()}),
			Components,
			*Fixture.Transaction);
	Test.TestTrue(TEXT("enum KeyType capture succeeds"), Capture.bSucceeded);
	EnlistFootprints(
		*Fixture.Transaction,
		TConstArrayView<FString>(Components).Left(1),
		TEXTVIEW("enum"));
	EnumName->SetValue(
		FString(),
		EPropertyValueSetFlags::NotTransactable);
	Test.TestFalse(TEXT("enum mutation invalidates derived state"),
		KeyType->bIsEnumNameValid != 0);
	Rows->SetObjects({KeyType});
	EnumName.Reset();
	Rows.Reset();
	const FAssetDocumentContractValidationResult Rollback =
		Fixture.Transaction->Rollback(TEXTVIEW("Test.Enum.Rollback"));
	if (!Rollback.bValid)
	{
		AddFailureDiagnostics(Test, TEXTVIEW("Enum rollback"),
			Rollback.Diagnostics);
	}
	Test.TestTrue(TEXT("enum KeyType rollback succeeds"), Rollback.bValid);
	Test.TestEqual(TEXT("enum name restores"),
		KeyType->EnumName, OriginalEnumName);
	Test.TestTrue(TEXT("enum validity restores"),
		KeyType->bIsEnumNameValid != 0);
	Test.TestFalse(TEXT("enum package dirty restores"),
		Fixture.Package->IsDirty());
	Fixture.Cleanup();
	return Rollback.bValid;
}

} // namespace AssetDocumentAITypedPreimageTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIKeyTypePropertiesRollbackTest,
	"UEAssetDoc.AI.TypedPreimage.KeyTypePropertiesRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIKeyTypePropertiesRollbackTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAITypedPreimageTests;
	return RunKeyTypePositive(*this)
		&& RunTextHistoryCollisionPositive(*this)
		&& RunStructKeyTypePositive(*this)
		&& RunEnumKeyTypePositive(*this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIKeyTypePropertiesRejectsUnsupportedTest,
	"UEAssetDoc.AI.TypedPreimage.KeyTypePropertiesRejectsUnsupported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIKeyTypePropertiesRejectsUnsupportedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAITypedPreimageTests;
	FAssetFixture Fixture;
	if (!CreateFactoryAsset(
		*this,
		UAssetDocumentAITypedPreimageTestSelectorOwner::StaticClass(),
		Fixture))
	{
		return false;
	}
	UAssetDocumentAITypedPreimageTestKeyType* KeyType =
		NewObject<UAssetDocumentAITypedPreimageTestKeyType>(Fixture.Asset);
	Fixture.Package->SetDirtyFlag(false);
	const int32 LifecycleBeforeRejections =
		KeyType->PostEditChangePropertyCount;
	const bool bDirtyBeforeRejections = Fixture.Package->IsDirty();
	TSharedPtr<IPropertyRowGenerator> Rows = MakeRows(*KeyType);
	TSharedPtr<IPropertyHandle> Valid = RequireHandle(
		*this, Rows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, Int32Value));
	if (!Valid.IsValid())
	{
		Fixture.Cleanup();
		return false;
	}
	const FName UnsupportedNames[] = {
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, UnsupportedArrayValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, UnsupportedOptionalValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, UnsupportedStructValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, EnumScalarValue),
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, LegacyEnumValue),
	};
	const TArray<FString> Components = {
		TEXT("rollback.ai.keytype.reject"),
		TEXT("rollback.ai.keytype.reject.package")};
	const double NotANumber = std::numeric_limits<double>::quiet_NaN();
	const double PositiveInfinity = std::numeric_limits<double>::infinity();
	auto TestNonFiniteRejected = [this, &Fixture, KeyType, &Rows, &Components,
		LifecycleBeforeRejections, bDirtyBeforeRejections](FName PropertyName)
	{
		TSharedPtr<IPropertyHandle> Handle = RequireHandle(
			*this, Rows, PropertyName);
		if (!Handle.IsValid())
		{
			return;
		}
		FTestTransaction Transaction(Fixture.Context);
		const FAssetDocumentEditorTypedPreimageResult Capture =
			CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
				*Fixture.Lease,
				*KeyType,
				Rows.ToSharedRef(),
				TArray<TSharedRef<IPropertyHandle>>({Handle.ToSharedRef()}),
				Components,
				Transaction);
		TestFalse(
			*FString::Printf(TEXT("non-finite property fails: %s"),
				*PropertyName.ToString()),
			Capture.bSucceeded);
		TestTrue(TEXT("non-finite diagnostic is stable"),
			HasDiagnostic(
				Capture.Diagnostics,
				TEXTVIEW("AITypedPreimage.KeyTypePropertyValueNotFinite")));
		TestEqual(TEXT("non-finite enlists no action"),
			Transaction.EnlistCallCount, 0);
		TestTrue(TEXT("non-finite enlists no footprint"),
			Transaction.EnlistedFootprint.IsEmpty());
		RequireFailureClosedCapture(
			*this,
			TEXTVIEW("non-finite KeyType value"),
			Capture,
			Transaction,
			TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
			TEXTVIEW("AITypedPreimage.KeyTypePropertyValueNotFinite"),
			LifecycleBeforeRejections,
			KeyType->PostEditChangePropertyCount,
			bDirtyBeforeRejections,
			Fixture.Package->IsDirty());
	};
	KeyType->Vector2DValue.X = NotANumber;
	TestNonFiniteRejected(GET_MEMBER_NAME_CHECKED(
		UAssetDocumentAITypedPreimageTestKeyType, Vector2DValue));
	KeyType->VectorValue.Y = PositiveInfinity;
	TestNonFiniteRejected(GET_MEMBER_NAME_CHECKED(
		UAssetDocumentAITypedPreimageTestKeyType, VectorValue));
	KeyType->Vector4Value.Z = -PositiveInfinity;
	TestNonFiniteRejected(GET_MEMBER_NAME_CHECKED(
		UAssetDocumentAITypedPreimageTestKeyType, Vector4Value));
	KeyType->QuatValue.W = NotANumber;
	TestNonFiniteRejected(GET_MEMBER_NAME_CHECKED(
		UAssetDocumentAITypedPreimageTestKeyType, QuatValue));
	KeyType->RotatorValue.Roll = PositiveInfinity;
	TestNonFiniteRejected(GET_MEMBER_NAME_CHECKED(
		UAssetDocumentAITypedPreimageTestKeyType, RotatorValue));
	for (FName Name : UnsupportedNames)
	{
		TSharedPtr<IPropertyHandle> Unsupported = RequireHandle(
			*this, Rows, Name);
		if (Unsupported.IsValid())
		{
			FTestTransaction Transaction(Fixture.Context);
			const FAssetDocumentEditorTypedPreimageResult Capture =
				CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
					*Fixture.Lease,
					*KeyType,
					Rows.ToSharedRef(),
					TArray<TSharedRef<IPropertyHandle>>(
						{Unsupported.ToSharedRef()}),
					Components,
					Transaction);
			TestFalse(
				*FString::Printf(TEXT("unsupported property fails: %s"),
					*Name.ToString()),
				Capture.bSucceeded);
			TestTrue(TEXT("unsupported family diagnostic is stable"),
				HasDiagnostic(
					Capture.Diagnostics,
					TEXTVIEW("AITypedPreimage.KeyTypePropertyNotAdmitted")));
			TestTrue(TEXT("unsupported family enlists no action"),
				Transaction.Actions.IsEmpty());
			RequireFailureClosedCapture(
				*this,
				TEXTVIEW("unsupported KeyType family"),
				Capture,
				Transaction,
				TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
				TEXTVIEW("AITypedPreimage.KeyTypePropertyNotAdmitted"),
				LifecycleBeforeRejections,
				KeyType->PostEditChangePropertyCount,
				bDirtyBeforeRejections,
				Fixture.Package->IsDirty());
		}
	}

	TSharedPtr<IPropertyHandle> UnsupportedStruct = RequireHandle(
		*this,
		Rows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType,
			UnsupportedStructValue));
	TSharedPtr<IPropertyHandle> NestedValue = UnsupportedStruct.IsValid()
		? UnsupportedStruct->GetChildHandle(TEXT("Value"))
		: nullptr;
	if (TestTrue(TEXT("nested child handle exists"), NestedValue.IsValid()))
	{
		FTestTransaction NestedTransaction(Fixture.Context);
		const FAssetDocumentEditorTypedPreimageResult NestedCapture =
			CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
				*Fixture.Lease,
				*KeyType,
				Rows.ToSharedRef(),
				TArray<TSharedRef<IPropertyHandle>>(
					{NestedValue.ToSharedRef()}),
				Components,
				NestedTransaction);
		TestFalse(TEXT("nested property child fails"),
			NestedCapture.bSucceeded);
		RequireFailureClosedCapture(
			*this, TEXTVIEW("nested KeyType child"), NestedCapture,
			NestedTransaction,
			TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
			TEXTVIEW("AITypedPreimage.KeyTypePropertyNotAdmitted"),
			LifecycleBeforeRejections, KeyType->PostEditChangePropertyCount,
			bDirtyBeforeRejections, Fixture.Package->IsDirty());
	}

	TSharedPtr<IPropertyHandle> VisibleOnly = RequireHandle(
		*this,
		Rows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, Int32ValueMirror));
	if (VisibleOnly.IsValid())
	{
		FTestTransaction VisibleTransaction(Fixture.Context);
		const FAssetDocumentEditorTypedPreimageResult VisibleCapture =
			CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
				*Fixture.Lease,
				*KeyType,
				Rows.ToSharedRef(),
				TArray<TSharedRef<IPropertyHandle>>(
					{VisibleOnly.ToSharedRef()}),
				Components,
				VisibleTransaction);
		TestFalse(TEXT("visible transient property fails"),
			VisibleCapture.bSucceeded);
		RequireFailureClosedCapture(
			*this, TEXTVIEW("visible transient KeyType property"),
			VisibleCapture, VisibleTransaction,
			TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
			TEXTVIEW("AITypedPreimage.KeyTypePropertyNotAdmitted"),
			LifecycleBeforeRejections, KeyType->PostEditChangePropertyCount,
			bDirtyBeforeRejections, Fixture.Package->IsDirty());
	}

	FTestTransaction DuplicateRows(Fixture.Context);
	const FAssetDocumentEditorTypedPreimageResult Duplicate =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			*Fixture.Lease,
			*KeyType,
			Rows.ToSharedRef(),
			TArray<TSharedRef<IPropertyHandle>>(
				{Valid.ToSharedRef(), Valid.ToSharedRef()}),
			Components,
			DuplicateRows);
	TestFalse(TEXT("duplicate property rows fail"), Duplicate.bSucceeded);
	RequireFailureClosedCapture(
		*this, TEXTVIEW("duplicate KeyType rows"), Duplicate, DuplicateRows,
		TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
		TEXTVIEW("AITypedPreimage.KeyTypePropertySetInvalid"),
		LifecycleBeforeRejections, KeyType->PostEditChangePropertyCount,
		bDirtyBeforeRejections, Fixture.Package->IsDirty());

	UAssetDocumentAITypedPreimageTestKeyType* DecoyKeyType =
		NewObject<UAssetDocumentAITypedPreimageTestKeyType>(Fixture.Asset);
	TSharedPtr<IPropertyRowGenerator> DecoyRows = MakeRows(*DecoyKeyType);
	TSharedPtr<IPropertyHandle> DecoyHandle = RequireHandle(
		*this,
		DecoyRows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, Int32Value));
	if (DecoyHandle.IsValid())
	{
		FTestTransaction DecoyRowsTransaction(Fixture.Context);
		const FAssetDocumentEditorTypedPreimageResult DecoyRowsCapture =
			CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
				*Fixture.Lease,
				*KeyType,
				DecoyRows.ToSharedRef(),
				TArray<TSharedRef<IPropertyHandle>>(
					{DecoyHandle.ToSharedRef()}),
				Components,
				DecoyRowsTransaction);
		TestFalse(TEXT("foreign property rows fail"),
			DecoyRowsCapture.bSucceeded);
		RequireFailureClosedCapture(
			*this, TEXTVIEW("foreign KeyType rows"), DecoyRowsCapture,
			DecoyRowsTransaction,
			TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
			TEXTVIEW("AITypedPreimage.KeyTypePropertyRowsMismatch"),
			LifecycleBeforeRejections, KeyType->PostEditChangePropertyCount,
			bDirtyBeforeRejections, Fixture.Package->IsDirty());
	}

	UAssetDocumentAITypedPreimageTestKeyType* ForeignOwner =
		NewObject<UAssetDocumentAITypedPreimageTestKeyType>(GetTransientPackage());
	TSharedPtr<IPropertyRowGenerator> ForeignOwnerRows = MakeRows(*ForeignOwner);
	TSharedPtr<IPropertyHandle> ForeignOwnerHandle = RequireHandle(
		*this,
		ForeignOwnerRows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestKeyType, Int32Value));
	if (ForeignOwnerHandle.IsValid())
	{
		FTestTransaction WrongOwnerTransaction(Fixture.Context);
		const int32 ForeignOwnerLifecycleBefore =
			ForeignOwner->PostEditChangePropertyCount;
		UPackage* ForeignOwnerPackage = ForeignOwner->GetOutermost();
		const bool bForeignOwnerDirtyBefore = ForeignOwnerPackage->IsDirty();
		const bool bLeaseAssetDirtyBefore = Fixture.Package->IsDirty();
		const FAssetDocumentEditorTypedPreimageResult WrongOwner =
			CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
				*Fixture.Lease,
				*ForeignOwner,
				ForeignOwnerRows.ToSharedRef(),
				TArray<TSharedRef<IPropertyHandle>>(
					{ForeignOwnerHandle.ToSharedRef()}),
				Components,
				WrongOwnerTransaction);
		RequireFailureClosedCapture(
			*this, TEXTVIEW("wrong KeyType owner"), WrongOwner,
			WrongOwnerTransaction,
			TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
			TEXTVIEW("AITypedPreimage.KeyTypeRequired"),
			ForeignOwnerLifecycleBefore,
			ForeignOwner->PostEditChangePropertyCount,
			bForeignOwnerDirtyBefore,
			ForeignOwnerPackage->IsDirty());
		TestEqual(TEXT("wrong KeyType owner preserves lease asset dirty"),
			Fixture.Package->IsDirty(), bLeaseAssetDirtyBefore);
	}

	FTestTransaction InvalidComponents(Fixture.Context);
	const FAssetDocumentEditorTypedPreimageResult InvalidComponentCapture =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			*Fixture.Lease,
			*KeyType,
			Rows.ToSharedRef(),
			TArray<TSharedRef<IPropertyHandle>>({Valid.ToSharedRef()}),
			TArray<FString>({TEXT("duplicate"), TEXT("duplicate")}),
			InvalidComponents);
	TestFalse(TEXT("duplicate rollback components fail"),
		InvalidComponentCapture.bSucceeded);
	RequireFailureClosedCapture(
		*this, TEXTVIEW("duplicate KeyType components"),
		InvalidComponentCapture, InvalidComponents,
		TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
		TEXTVIEW("AITypedPreimage.RollbackComponentInvalid"),
		LifecycleBeforeRejections, KeyType->PostEditChangePropertyCount,
		bDirtyBeforeRejections, Fixture.Package->IsDirty());

	FTestTransaction ClosedTransaction(Fixture.Context);
	ClosedTransaction.SetState(EAssetDocumentAtomicState::Committed);
	const FAssetDocumentEditorTypedPreimageResult Closed =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			*Fixture.Lease,
			*KeyType,
			Rows.ToSharedRef(),
			TArray<TSharedRef<IPropertyHandle>>({Valid.ToSharedRef()}),
			Components,
			ClosedTransaction);
	TestFalse(TEXT("closed transaction fails"), Closed.bSucceeded);
	RequireFailureClosedCapture(
		*this, TEXTVIEW("closed KeyType transaction"), Closed,
		ClosedTransaction,
		TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
		TEXTVIEW("AITypedPreimage.TransactionStateInvalid"),
		LifecycleBeforeRejections, KeyType->PostEditChangePropertyCount,
		bDirtyBeforeRejections, Fixture.Package->IsDirty());

	FAssetDocumentOpaqueHandle ReadOnlyHandle = Fixture.Lease->GetHandle();
	ReadOnlyHandle.Access = EAssetDocumentHandleAccess::ReadOnly;
	FOverriddenLease ReadOnlyLease(
		MoveTemp(ReadOnlyHandle), Fixture.Lease->GetPayloadChecked());
	FTestTransaction ReadOnlyTransaction(Fixture.Context);
	const FAssetDocumentEditorTypedPreimageResult ReadOnly =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			ReadOnlyLease,
			*KeyType,
			Rows.ToSharedRef(),
			TArray<TSharedRef<IPropertyHandle>>({Valid.ToSharedRef()}),
			Components,
			ReadOnlyTransaction);
	TestFalse(TEXT("read-only lease fails"), ReadOnly.bSucceeded);
	RequireFailureClosedCapture(
		*this, TEXTVIEW("read-only KeyType lease"), ReadOnly,
		ReadOnlyTransaction,
		TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
		TEXTVIEW("AITypedPreimage.TransactionBindingMismatch"),
		LifecycleBeforeRejections, KeyType->PostEditChangePropertyCount,
		bDirtyBeforeRejections, Fixture.Package->IsDirty());

	FTestContext ForeignContext;
	FTestTransaction ForeignTransaction(ForeignContext);
	const FAssetDocumentEditorTypedPreimageResult Foreign =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			*Fixture.Lease,
			*KeyType,
			Rows.ToSharedRef(),
			TArray<TSharedRef<IPropertyHandle>>({Valid.ToSharedRef()}),
			Components,
			ForeignTransaction);
	TestFalse(TEXT("foreign token fails"), Foreign.bSucceeded);
	TestTrue(TEXT("foreign token diagnostic is stable"),
		HasDiagnostic(
			Foreign.Diagnostics,
			TEXTVIEW("AITypedPreimage.TransactionBindingMismatch")));
	RequireFailureClosedCapture(
		*this, TEXTVIEW("foreign KeyType token"), Foreign,
		ForeignTransaction,
		TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
		TEXTVIEW("AITypedPreimage.TransactionBindingMismatch"),
		LifecycleBeforeRejections, KeyType->PostEditChangePropertyCount,
		bDirtyBeforeRejections, Fixture.Package->IsDirty());

	FAssetDocumentOpaqueHandle RevisionHandle = Fixture.Lease->GetHandle();
	++RevisionHandle.RuntimeRevision;
	FOverriddenLease RevisionLease(
		MoveTemp(RevisionHandle), Fixture.Lease->GetPayloadChecked());
	FTestTransaction RevisionTransaction(Fixture.Context);
	const FAssetDocumentEditorTypedPreimageResult Revision =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			RevisionLease,
			*KeyType,
			Rows.ToSharedRef(),
			TArray<TSharedRef<IPropertyHandle>>({Valid.ToSharedRef()}),
			Components,
			RevisionTransaction);
	TestFalse(TEXT("lease revision mismatch fails"), Revision.bSucceeded);
	RequireFailureClosedCapture(
		*this, TEXTVIEW("KeyType revision mismatch"), Revision,
		RevisionTransaction,
		TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
		TEXTVIEW("AITypedPreimage.TransactionBindingMismatch"),
		LifecycleBeforeRejections, KeyType->PostEditChangePropertyCount,
		bDirtyBeforeRejections, Fixture.Package->IsDirty());

	FTestTransaction EmptyTransaction(Fixture.Context);
	const FAssetDocumentEditorTypedPreimageResult Empty =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			*Fixture.Lease,
			*KeyType,
			Rows.ToSharedRef(),
			{},
			Components,
			EmptyTransaction);
	TestFalse(TEXT("empty property set fails"), Empty.bSucceeded);
	RequireFailureClosedCapture(
		*this, TEXTVIEW("empty KeyType property set"), Empty,
		EmptyTransaction,
		TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
		TEXTVIEW("AITypedPreimage.KeyTypePropertySetInvalid"),
		LifecycleBeforeRejections, KeyType->PostEditChangePropertyCount,
		bDirtyBeforeRejections, Fixture.Package->IsDirty());

	FTestTransaction Partial(Fixture.Context);
	Partial.RejectEnlistAtCall = 1;
	const FAssetDocumentEditorTypedPreimageResult PartialCapture =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			*Fixture.Lease,
			*KeyType,
			Rows.ToSharedRef(),
			TArray<TSharedRef<IPropertyHandle>>({Valid.ToSharedRef()}),
			Components,
			Partial);
	TestFalse(TEXT("partial alias rejection fails"),
		PartialCapture.bSucceeded);
	TestTrue(TEXT("partial alias diagnostic is exact"),
		HasExactDiagnostic(
			PartialCapture.Diagnostics,
			TEXTVIEW("/AITypedPreimage/BlackboardKeyType"),
			TEXTVIEW("AITypedPreimage.RollbackActionEnlistmentFailed")));
	TestEqual(TEXT("partial observes caller order"),
		Partial.ObservedComponentIds, Components);
	TestEqual(TEXT("partial accepts one action"), Partial.Actions.Num(), 1);
	TestTrue(TEXT("partial enlists no footprint"),
		Partial.EnlistedFootprint.IsEmpty());
	const int32 LifecycleBeforeCleanup =
		KeyType->PostEditChangePropertyCount;
	TestTrue(TEXT("partial cleanup succeeds"),
		Partial.Rollback(TEXTVIEW("Test.PartialCleanup")).bValid);
	TestEqual(TEXT("unbound partial action does not mutate"),
		KeyType->PostEditChangePropertyCount, LifecycleBeforeCleanup);
	TestFalse(TEXT("rejections preserve package dirty"),
		Fixture.Package->IsDirty());
	Fixture.Cleanup();
	return true;
}

namespace AssetDocumentAITypedPreimageTests
{
bool InitializeBlackboardFixture(
	FAutomationTestBase& Test,
	FAssetFixture& RootFixture,
	FAssetFixture& ChildFixture,
	UAssetDocumentAITypedPreimageTestBlackboardData*& OutRoot,
	UAssetDocumentAITypedPreimageTestBlackboardData*& OutChild,
	UAssetDocumentAITypedPreimageTestLocalKeyType*& OutFirst,
	UAssetDocumentAITypedPreimageTestLocalKeyType*& OutSecond)
{
	if (!CreateFactoryAsset(
		Test,
		UAssetDocumentAITypedPreimageTestBlackboardData::StaticClass(),
		RootFixture)
		|| !CreateFactoryAsset(
			Test,
			UAssetDocumentAITypedPreimageTestBlackboardData::StaticClass(),
			ChildFixture))
	{
		return false;
	}
	OutRoot = Cast<UAssetDocumentAITypedPreimageTestBlackboardData>(
		RootFixture.Asset);
	OutChild = Cast<UAssetDocumentAITypedPreimageTestBlackboardData>(
		ChildFixture.Asset);
	if (!Test.TestNotNull(TEXT("root Blackboard exists"), OutRoot)
		|| !Test.TestNotNull(TEXT("child Blackboard exists"), OutChild))
	{
		return false;
	}
	OutFirst = NewObject<UAssetDocumentAITypedPreimageTestLocalKeyType>(OutRoot);
	OutSecond = NewObject<UAssetDocumentAITypedPreimageTestLocalKeyType>(OutRoot);
	OutFirst->Int32Value = 11;
	OutFirst->StringValue = TEXT("first-original");
	OutSecond->Int32Value = 22;
	OutSecond->StringValue = TEXT("second-original");
	ApplyKeys(*OutRoot, {
		MakeEntry(TEXT("First"), *OutFirst,
			TEXT("first-description"), TEXT("Alpha"), false),
		MakeEntry(TEXT("Second"), *OutSecond,
			TEXT("second-description"), TEXT("Beta"), true)});
	OutChild->Parent = OutRoot;
	OutChild->UpdateParentKeys();
	OutChild->UpdateKeyIDs();
	OutChild->UpdateIfHasSynchronizedKeys();
	OutRoot->PostEditChangePropertyCount = 0;
	OutChild->PostEditChangePropertyCount = 0;
	RootFixture.Package->SetDirtyFlag(false);
	ChildFixture.Package->SetDirtyFlag(true);
	return true;
}

bool RunLocalKeysPositive(FAutomationTestBase& Test)
{
	FAssetFixture RootFixture;
	FAssetFixture ChildFixture;
	UAssetDocumentAITypedPreimageTestBlackboardData* Root = nullptr;
	UAssetDocumentAITypedPreimageTestBlackboardData* Child = nullptr;
	UAssetDocumentAITypedPreimageTestLocalKeyType* First = nullptr;
	UAssetDocumentAITypedPreimageTestLocalKeyType* Second = nullptr;
	if (!InitializeBlackboardFixture(
		Test, RootFixture, ChildFixture,
		Root, Child, First, Second))
	{
		ChildFixture.Cleanup();
		RootFixture.Cleanup();
		return false;
	}
	TWeakObjectPtr<UAssetDocumentAITypedPreimageTestLocalKeyType> FirstWeak =
		First;
	const TArray<FString> Components = {
		TEXT("rollback.ai.blackboard.keys"),
		TEXT("rollback.ai.blackboard.derived"),
		TEXT("rollback.ai.blackboard.packages")};
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*RootFixture.Lease,
			*Root,
			Components,
			*RootFixture.Transaction);
	if (!Capture.bSucceeded)
	{
		AddFailureDiagnostics(Test, TEXTVIEW("Local keys capture"),
			Capture.Diagnostics);
	}
	if (!Test.TestTrue(TEXT("local keys aggregate capture succeeds"),
		Capture.bSucceeded)
		|| !Test.TestTrue(TEXT("local keys footprint enlists"),
			EnlistFootprints(
				*RootFixture.Transaction,
				TConstArrayView<FString>(Components).Left(1),
				TEXTVIEW("local-keys"))))
	{
		ChildFixture.Cleanup();
		RootFixture.Cleanup();
		return false;
	}

	First->Int32Value = 110;
	First->StringValue = TEXT("first-mutated");
	Second->Int32Value = 220;
	Second->StringValue = TEXT("second-mutated");
	TArray<FBlackboardEntry> Replacement = {
		MakeEntry(TEXT("Second"), *Second,
			TEXT("second-mutated-description"), TEXT("Gamma"), false)};
	ApplyKeys(*Root, MoveTemp(Replacement));
	First = nullptr;
	CollectGarbage(RF_NoFlags);
	Test.TestTrue(TEXT("removed instanced KeyType survives forced GC"),
		FirstWeak.IsValid());
	Test.TestEqual(TEXT("mutation changes local key count"),
		Root->Keys.Num(), 2);
	Test.TestEqual(TEXT("descendant projection follows mutation"),
		Child->ParentKeys.Num(), 2);

	const FAssetDocumentContractValidationResult Rollback =
		RootFixture.Transaction->Rollback(TEXTVIEW("Test.LocalKeys.Rollback"));
	Test.TestTrue(TEXT("local keys rollback succeeds"), Rollback.bValid);
	Test.TestEqual(TEXT("local key order/count restores"), Root->Keys.Num(), 3);
	if (Root->Keys.Num() == 3)
	{
		Test.TestEqual(TEXT("first name restores"),
			Root->Keys[0].EntryName, FName(TEXT("First")));
		Test.TestEqual(TEXT("second name restores"),
			Root->Keys[1].EntryName, FName(TEXT("Second")));
		Test.TestTrue(TEXT("removed identity restores"),
			Root->Keys[0].KeyType.Get() == FirstWeak.Get());
		Test.TestTrue(TEXT("existing identity restores"),
			Root->Keys[1].KeyType.Get() == Second);
#if WITH_EDITORONLY_DATA
		Test.TestEqual(TEXT("entry description restores"),
			Root->Keys[0].EntryDescription,
			FString(TEXT("first-description")));
		Test.TestEqual(TEXT("entry category restores"),
			Root->Keys[1].EntryCategory, FName(TEXT("Beta")));
#endif
		Test.TestFalse(TEXT("first sync bit restores"),
			Root->Keys[0].bInstanceSynced != 0);
		Test.TestTrue(TEXT("second sync bit restores"),
			Root->Keys[1].bInstanceSynced != 0);
		Test.TestEqual(TEXT("persistent SelfActor order restores"),
			Root->Keys[2].EntryName, FBlackboard::KeySelf);
	}
	Test.TestEqual(TEXT("KeyType scalar property restores"),
		Second->Int32Value, 22);
	Test.TestEqual(TEXT("KeyType string property restores"),
		Second->StringValue, FString(TEXT("second-original")));
	Test.TestEqual(TEXT("removed KeyType scalar property restores"),
		FirstWeak->Int32Value, 11);
	Test.TestEqual(TEXT("removed KeyType string property restores"),
		FirstWeak->StringValue, FString(TEXT("first-original")));
	Test.TestEqual(TEXT("descendant ParentKeys restores"),
		Child->ParentKeys.Num(), 3);
	Test.TestEqual(TEXT("root key ID restores"),
		Root->GetKeyID(TEXT("First")), FBlackboard::FKey(0));
	Test.TestEqual(TEXT("descendant key ID restores"),
		Child->GetKeyID(TEXT("Second")), FBlackboard::FKey(1));
	Test.TestEqual(TEXT("root key type projection restores"),
		Root->GetKeyType(FBlackboard::FKey(0)).Get(),
		UAssetDocumentAITypedPreimageTestLocalKeyType::StaticClass());
	Test.TestEqual(TEXT("descendant key type projection restores"),
		Child->GetKeyType(FBlackboard::FKey(1)).Get(),
		UAssetDocumentAITypedPreimageTestLocalKeyType::StaticClass());
	Test.TestTrue(TEXT("root synchronized-key cache restores"),
		Root->HasSynchronizedKeys());
	Test.TestTrue(TEXT("descendant synchronized-key cache restores"),
		Child->HasSynchronizedKeys());
	Test.TestFalse(TEXT("root clean dirty preimage restores"),
		RootFixture.Package->IsDirty());
	Test.TestTrue(TEXT("child dirty preimage restores"),
		ChildFixture.Package->IsDirty());
	Test.TestTrue(TEXT("lifecycle ran while dirty before dirty-last"),
		Root->bPackageDirtyDuringPostEditChangeProperty);
	ChildFixture.Cleanup();
	RootFixture.Cleanup();
	return Rollback.bValid;
}

bool RunLocalKeysStockIntRollback(FAutomationTestBase& Test)
{
	IModuleInterface& BehaviorTreeEditor =
		FModuleManager::LoadModuleChecked<IModuleInterface>(
			TEXT("BehaviorTreeEditor"));
	(void)BehaviorTreeEditor;

	FAssetFixture Fixture;
	const FString PackageName = FString::Printf(
		TEXT("/Game/AssetDocumentTests/AITypedPreimage/StockInt_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*PackageName);
	UBlackboardData* Root = NewObject<UBlackboardData>(
		Package,
		TEXT("BB_StockInt"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Test.TestNotNull(TEXT("stock Int Blackboard exists"), Root)
		|| !ResolveExistingAssetFixture(Test, *Root, Fixture))
	{
		return false;
	}
	UBlackboardKeyType_Int* StockInt =
		NewObject<UBlackboardKeyType_Int>(Root);
	StockInt->DefaultValue = 41;
	ApplyKeys(*Root, {
		MakeEntry(TEXT("StockInt"), *StockInt,
			TEXT("stock-int-description"), TEXT("Stock"), false)});
	Fixture.Package->SetDirtyFlag(false);
	TWeakObjectPtr<UBlackboardKeyType_Int> StockIntWeak = StockInt;
	const TArray<FString> Components = {
		TEXT("rollback.ai.local.stock-int"),
		TEXT("rollback.ai.local.stock-int.package")};
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*Fixture.Lease, *Root, Components, *Fixture.Transaction);
	if (!Capture.bSucceeded)
	{
		AddFailureDiagnostics(
			Test, TEXTVIEW("Stock Int local capture"), Capture.Diagnostics);
	}
	if (!Test.TestTrue(TEXT("stock Int local aggregate capture succeeds"),
		Capture.bSucceeded)
		|| !Test.TestTrue(TEXT("stock Int local footprint enlists"),
			EnlistFootprints(
				*Fixture.Transaction,
				TConstArrayView<FString>(Components).Left(1),
				TEXTVIEW("local-stock-int"))))
	{
		Fixture.Cleanup();
		return false;
	}

	StockInt->DefaultValue = -17;
	ApplyKeys(*Root, {});
	StockInt = nullptr;
	CollectGarbage(RF_NoFlags);
	Test.TestTrue(TEXT("removed stock Int survives forced GC"),
		StockIntWeak.IsValid());
	const FAssetDocumentContractValidationResult Rollback =
		Fixture.Transaction->Rollback(TEXTVIEW("Test.LocalKeys.StockInt"));
	if (!Rollback.bValid)
	{
		AddFailureDiagnostics(
			Test, TEXTVIEW("Stock Int local rollback"), Rollback.Diagnostics);
	}
	Test.TestTrue(TEXT("stock Int local rollback succeeds"), Rollback.bValid);
	Test.TestEqual(TEXT("stock KeyType local order/count restores"),
		Root->Keys.Num(), 2);
	if (Root->Keys.Num() == 2)
	{
		Test.TestEqual(TEXT("stock Int local name restores"),
			Root->Keys[0].EntryName, FName(TEXT("StockInt")));
		Test.TestTrue(TEXT("stock Int exact pointer restores"),
			Root->Keys[0].KeyType.Get() == StockIntWeak.Get());
		Test.TestEqual(TEXT("stock Int authored default restores"),
			StockIntWeak->DefaultValue, 41);
		Test.TestEqual(TEXT("persistent SelfActor order restores"),
			Root->Keys[1].EntryName, FBlackboard::KeySelf);
	}
	Test.TestFalse(TEXT("stock Int dirty preimage restores"),
		Fixture.Package->IsDirty());
	Fixture.Cleanup();
	return Rollback.bValid;
}

bool RunLocalKeysStockConditionalPayloadRollback(FAutomationTestBase& Test)
{
	IModuleInterface& BehaviorTreeEditor =
		FModuleManager::LoadModuleChecked<IModuleInterface>(
			TEXT("BehaviorTreeEditor"));
	(void)BehaviorTreeEditor;

	FAssetFixture Fixture;
	const FString PackageName = FString::Printf(
		TEXT("/Game/AssetDocumentTests/AITypedPreimage/StockConditional_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*PackageName);
	UBlackboardData* Root = NewObject<UBlackboardData>(
		Package,
		TEXT("BB_StockConditional"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Test.TestNotNull(TEXT("stock conditional Blackboard exists"), Root)
		|| !ResolveExistingAssetFixture(Test, *Root, Fixture))
	{
		return false;
	}
	UBlackboardKeyType_Rotator* StockRotator =
		NewObject<UBlackboardKeyType_Rotator>(Root);
	StockRotator->bUseDefaultValue = false;
	StockRotator->DefaultValue = FRotator(11.0, 22.0, 33.0);
	UBlackboardKeyType_Vector* StockVector =
		NewObject<UBlackboardKeyType_Vector>(Root);
	StockVector->bUseDefaultValue = false;
	StockVector->DefaultValue = FVector(13.0, 21.0, 34.0);
	ApplyKeys(*Root, {
		MakeEntry(TEXT("StockRotator"), *StockRotator,
			TEXT("stock-rotator-description"), TEXT("Stock"), false),
		MakeEntry(TEXT("StockVector"), *StockVector,
			TEXT("stock-vector-description"), TEXT("Stock"), false)});
	Fixture.Package->SetDirtyFlag(false);
	TWeakObjectPtr<UBlackboardKeyType_Rotator> StockRotatorWeak = StockRotator;
	TWeakObjectPtr<UBlackboardKeyType_Vector> StockVectorWeak = StockVector;
	const TArray<FString> Components = {
		TEXT("rollback.ai.local.stock-conditional"),
		TEXT("rollback.ai.local.stock-conditional.package")};
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*Fixture.Lease, *Root, Components, *Fixture.Transaction);
	if (!Capture.bSucceeded)
	{
		AddFailureDiagnostics(
			Test, TEXTVIEW("Stock conditional local capture"), Capture.Diagnostics);
	}
	if (!Test.TestTrue(
			TEXT("stock conditional local aggregate capture succeeds"),
			Capture.bSucceeded)
		|| !Test.TestTrue(TEXT("stock conditional local footprint enlists"),
			EnlistFootprints(
				*Fixture.Transaction,
				TConstArrayView<FString>(Components).Left(1),
				TEXTVIEW("local-stock-conditional"))))
	{
		Fixture.Cleanup();
		return false;
	}

	ApplyKeys(*Root, {});
	StockRotator = nullptr;
	StockVector = nullptr;
	CollectGarbage(RF_NoFlags);
	Test.TestTrue(TEXT("removed stock Rotator survives forced GC"),
		StockRotatorWeak.IsValid());
	Test.TestTrue(TEXT("removed stock Vector survives forced GC"),
		StockVectorWeak.IsValid());
	const FAssetDocumentContractValidationResult Rollback =
		Fixture.Transaction->Rollback(
			TEXTVIEW("Test.LocalKeys.StockConditional"));
	if (!Rollback.bValid)
	{
		AddFailureDiagnostics(
			Test, TEXTVIEW("Stock conditional local rollback"),
			Rollback.Diagnostics);
	}
	Test.TestTrue(TEXT("stock conditional local rollback succeeds"),
		Rollback.bValid);
	Test.TestEqual(TEXT("stock conditional local order/count restores"),
		Root->Keys.Num(), 3);
	if (Root->Keys.Num() == 3)
	{
		Test.TestEqual(TEXT("stock Rotator local name restores"),
			Root->Keys[0].EntryName, FName(TEXT("StockRotator")));
		Test.TestTrue(TEXT("stock Rotator exact pointer restores"),
			Root->Keys[0].KeyType.Get() == StockRotatorWeak.Get());
		Test.TestEqual(TEXT("stock Rotator authored default remains exact"),
			StockRotatorWeak->DefaultValue, FRotator(11.0, 22.0, 33.0));
		Test.TestFalse(TEXT("stock Rotator edit condition remains exact"),
			StockRotatorWeak->bUseDefaultValue);
		Test.TestEqual(TEXT("stock Vector local name restores"),
			Root->Keys[1].EntryName, FName(TEXT("StockVector")));
		Test.TestTrue(TEXT("stock Vector exact pointer restores"),
			Root->Keys[1].KeyType.Get() == StockVectorWeak.Get());
		Test.TestEqual(TEXT("stock Vector authored default remains exact"),
			StockVectorWeak->DefaultValue, FVector(13.0, 21.0, 34.0));
		Test.TestFalse(TEXT("stock Vector edit condition remains exact"),
			StockVectorWeak->bUseDefaultValue);
		Test.TestEqual(TEXT("persistent SelfActor order restores"),
			Root->Keys[2].EntryName, FBlackboard::KeySelf);
	}
	Test.TestFalse(TEXT("stock conditional dirty preimage restores"),
		Fixture.Package->IsDirty());
	Fixture.Cleanup();
	return Rollback.bValid;
}

bool RunLocalKeysStockConditionalPayloadMutationFailsClosed(
	FAutomationTestBase& Test)
{
	IModuleInterface& BehaviorTreeEditor =
		FModuleManager::LoadModuleChecked<IModuleInterface>(
			TEXT("BehaviorTreeEditor"));
	(void)BehaviorTreeEditor;

	FAssetFixture Fixture;
	const FString PackageName = FString::Printf(
		TEXT("/Game/AssetDocumentTests/AITypedPreimage/StockConditionalFailure_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*PackageName);
	UBlackboardData* Root = NewObject<UBlackboardData>(
		Package,
		TEXT("BB_StockConditionalFailure"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Test.TestNotNull(
			TEXT("stock conditional failure Blackboard exists"), Root)
		|| !ResolveExistingAssetFixture(Test, *Root, Fixture))
	{
		return false;
	}
	UBlackboardKeyType_Rotator* StockRotator =
		NewObject<UBlackboardKeyType_Rotator>(Root);
	UBlackboardKeyType_Int* StockInt =
		NewObject<UBlackboardKeyType_Int>(Root);
	StockInt->DefaultValue = 41;
	StockRotator->bUseDefaultValue = false;
	StockRotator->DefaultValue = FRotator(1.0, 2.0, 3.0);
	ApplyKeys(*Root, {
		MakeEntry(TEXT("StockInt"), *StockInt,
			TEXT("stock-int-failure-description"), TEXT("Stock"), false),
		MakeEntry(TEXT("StockRotator"), *StockRotator,
			TEXT("stock-rotator-failure-description"), TEXT("Stock"), false)});
	Fixture.Package->SetDirtyFlag(false);
	const TArray<FString> Components = {
		TEXT("rollback.ai.local.stock-conditional-failure"),
		TEXT("rollback.ai.local.stock-conditional-failure.package")};
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*Fixture.Lease, *Root, Components, *Fixture.Transaction);
	if (!Test.TestTrue(TEXT("stock conditional failure preimage captures"),
		Capture.bSucceeded)
		|| !Test.TestTrue(TEXT("stock conditional failure footprint enlists"),
			EnlistFootprints(
				*Fixture.Transaction,
				TConstArrayView<FString>(Components).Left(1),
				TEXTVIEW("local-stock-conditional-failure"))))
	{
		Fixture.Cleanup();
		return false;
	}

	const FRotator Mutated(17.0, 19.0, 23.0);
	StockInt->DefaultValue = -17;
	StockRotator->DefaultValue = Mutated;
	ApplyKeys(*Root, {});
	Fixture.Package->SetDirtyFlag(true);
	const TArray<FBlackboardEntry> MutatedKeys = Root->Keys;
	const FAssetDocumentContractValidationResult Rollback =
		Fixture.Transaction->Rollback(
			TEXTVIEW("Test.LocalKeys.StockConditionalFailure"));
	Test.TestFalse(TEXT("non-editable stock payload mutation fails closed"),
		Rollback.bValid);
	Test.TestTrue(TEXT("non-editable stock payload uses closed diagnostic"),
		HasDiagnostic(
			Rollback.Diagnostics,
			TEXTVIEW("AITypedPreimage.KeyTypeRollbackWriteFailed")));
	Test.TestEqual(TEXT("failed rollback preserves mutated key count"),
		Root->Keys.Num(), MutatedKeys.Num());
	if (Root->Keys.Num() == MutatedKeys.Num())
	{
		for (int32 Index = 0; Index < MutatedKeys.Num(); ++Index)
		{
			Test.TestEqual(
				*FString::Printf(
					TEXT("failed rollback preserves mutated key name %d"), Index),
				Root->Keys[Index].EntryName,
				MutatedKeys[Index].EntryName);
			Test.TestTrue(
				*FString::Printf(
					TEXT("failed rollback preserves mutated key pointer %d"), Index),
				Root->Keys[Index].KeyType.Get()
					== MutatedKeys[Index].KeyType.Get());
		}
	}
	Test.TestEqual(TEXT("failed rollback does not restore earlier editable payload"),
		StockInt->DefaultValue, -17);
	Test.TestEqual(TEXT("non-editable stock payload is not strong-written"),
		StockRotator->DefaultValue, Mutated);
	Test.TestFalse(TEXT("stock edit condition is not changed for rollback"),
		StockRotator->bUseDefaultValue);
	Test.TestTrue(TEXT("failed rollback preserves mutated dirty state"),
		Fixture.Package->IsDirty());
	Fixture.Cleanup();
	return !Rollback.bValid;
}

bool RunLocalKeysZeroFieldGcPositive(FAutomationTestBase& Test)
{
	FAssetFixture Fixture;
	if (!CreateFactoryAsset(
		Test,
		UAssetDocumentAITypedPreimageTestBlackboardData::StaticClass(),
		Fixture))
	{
		return false;
	}
	auto* Root = CastChecked<UAssetDocumentAITypedPreimageTestBlackboardData>(
		Fixture.Asset);
	UAssetDocumentAITypedPreimageTestZeroFieldKeyType* ZeroField =
		NewObject<UAssetDocumentAITypedPreimageTestZeroFieldKeyType>(Root);
	ApplyKeys(*Root, {
		MakeEntry(TEXT("ZeroField"), *ZeroField,
			TEXT("zero-field-description"), TEXT("ZeroField"), false)});
	Fixture.Package->SetDirtyFlag(false);
	TWeakObjectPtr<UAssetDocumentAITypedPreimageTestZeroFieldKeyType> ZeroWeak =
		ZeroField;
	const TArray<FString> Components = {
		TEXT("rollback.ai.local.zero-field"),
		TEXT("rollback.ai.local.zero-field.package")};
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*Fixture.Lease, *Root, Components, *Fixture.Transaction);
	if (!Test.TestTrue(TEXT("zero-field local KeyType captures"),
		Capture.bSucceeded)
		|| !Test.TestTrue(TEXT("zero-field local footprint enlists"),
			EnlistFootprints(
				*Fixture.Transaction,
				TConstArrayView<FString>(Components).Left(1),
				TEXTVIEW("local-zero-field"))))
	{
		Fixture.Cleanup();
		return false;
	}
	ApplyKeys(*Root, {});
	ZeroField = nullptr;
	CollectGarbage(RF_NoFlags);
	Test.TestTrue(TEXT("zero-field removed KeyType survives forced GC"),
		ZeroWeak.IsValid());
	const FAssetDocumentContractValidationResult Rollback =
		Fixture.Transaction->Rollback(TEXTVIEW("Test.LocalKeys.ZeroFieldGc"));
	if (!Rollback.bValid)
	{
		AddFailureDiagnostics(
			Test, TEXTVIEW("Zero-field local rollback"), Rollback.Diagnostics);
	}
	Test.TestTrue(TEXT("zero-field local rollback succeeds"), Rollback.bValid);
	Test.TestTrue(TEXT("zero-field local identity restores"),
		Root->Keys[0].KeyType.Get() == ZeroWeak.Get());
	Test.TestEqual(TEXT("zero-field local name restores"),
		Root->Keys[0].EntryName, FName(TEXT("ZeroField")));
	Test.TestFalse(TEXT("zero-field local dirty preimage restores"),
		Fixture.Package->IsDirty());
	Fixture.Cleanup();
	return Rollback.bValid;
}

bool RunLocalKeysStaleDescendantRejection(FAutomationTestBase& Test)
{
	FAssetFixture RootFixture;
	FAssetFixture ChildFixture;
	UAssetDocumentAITypedPreimageTestBlackboardData* Root = nullptr;
	UAssetDocumentAITypedPreimageTestBlackboardData* Child = nullptr;
	UAssetDocumentAITypedPreimageTestLocalKeyType* First = nullptr;
	UAssetDocumentAITypedPreimageTestLocalKeyType* Second = nullptr;
	if (!InitializeBlackboardFixture(
		Test, RootFixture, ChildFixture,
		Root, Child, First, Second))
	{
		ChildFixture.Cleanup();
		RootFixture.Cleanup();
		return false;
	}
	const TArray<FString> Components = {
		TEXT("rollback.ai.local.stale"),
		TEXT("rollback.ai.local.stale.package")};
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*RootFixture.Lease, *Root, Components, *RootFixture.Transaction);
	if (!Test.TestTrue(TEXT("stale-descendant preimage captures"),
		Capture.bSucceeded)
		|| !Test.TestTrue(TEXT("stale-descendant footprint enlists"),
			EnlistFootprints(
				*RootFixture.Transaction,
				TConstArrayView<FString>(Components).Left(1),
				TEXTVIEW("local-stale"))))
	{
		ChildFixture.Cleanup();
		RootFixture.Cleanup();
		return false;
	}

	ApplyKeys(*Root, {
		MakeEntry(TEXT("MutationSurvivesFailure"), *Second,
			TEXT("mutation-description"), TEXT("Mutation"), false)});
	RootFixture.Package->SetDirtyFlag(true);
	Child->Parent = nullptr;
	const int32 RootLifecycleBeforeRollback =
		Root->PostEditChangePropertyCount;
	const int32 RootKeyCountBeforeRollback = Root->Keys.Num();
	const FName RootFirstNameBeforeRollback = Root->Keys[0].EntryName;
	const FAssetDocumentContractValidationResult Rollback =
		RootFixture.Transaction->Rollback(
			TEXTVIEW("Test.LocalKeys.StaleDescendant"));
	Test.TestFalse(TEXT("stale descendant fails rollback"), Rollback.bValid);
	Test.TestTrue(TEXT("stale descendant diagnostic is exact"),
		HasExactDiagnostic(
			Rollback.Diagnostics,
			TEXTVIEW("/AITypedPreimage/BlackboardLocalKeys"),
			TEXTVIEW(
				"AITypedPreimage.BlackboardLocalKeysRollbackIdentityMismatch")));
	Test.TestEqual(TEXT("stale descendant fails before root lifecycle"),
		Root->PostEditChangePropertyCount, RootLifecycleBeforeRollback);
	Test.TestEqual(TEXT("stale descendant fails before key count restore"),
		Root->Keys.Num(), RootKeyCountBeforeRollback);
	Test.TestEqual(TEXT("stale descendant fails before key value restore"),
		Root->Keys[0].EntryName, RootFirstNameBeforeRollback);
	Test.TestTrue(TEXT("stale descendant failure preserves dirty mutation"),
		RootFixture.Package->IsDirty());
	Child->Parent = Root;
	ChildFixture.Cleanup();
	RootFixture.Cleanup();
	return !Rollback.bValid;
}

} // namespace AssetDocumentAITypedPreimageTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBlackboardLocalKeysRollbackTest,
	"UEAssetDoc.AI.TypedPreimage.BlackboardLocalKeysRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardLocalKeysRollbackTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	return AssetDocumentAITypedPreimageTests::RunLocalKeysPositive(*this)
		&& AssetDocumentAITypedPreimageTests::RunLocalKeysZeroFieldGcPositive(*this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBlackboardLocalKeysStockIntRollbackTest,
	"UEAssetDoc.AI.TypedPreimage.BlackboardLocalKeysStockIntRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardLocalKeysStockIntRollbackTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	// Test Design Scan:
	// - Behavior: aggregate local-key typed preimage captures and restores an
	//   exact stock UBlackboardKeyType_Int while BehaviorTreeEditor details
	//   customizations are loaded.
	// - Technique: public supported-entry state transition plus the one-authored-
	//   row boundary and forced-GC identity roundtrip.
	// - Risk or gap: keeps the initially reported stock Int family as a positive
	//   control while conditional stock payloads exercise the actual RED below.
	return AssetDocumentAITypedPreimageTests::RunLocalKeysStockIntRollback(*this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBlackboardLocalKeysStockConditionalPayloadRollbackTest,
	"UEAssetDoc.AI.TypedPreimage.BlackboardLocalKeysStockConditionalPayloadRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardLocalKeysStockConditionalPayloadRollbackTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	// Test Design Scan:
	// - Behavior: aggregate local-key typed preimage captures and restores exact
	//   stock Rotator and Vector payloads while their public EditCondition is
	//   false and BehaviorTreeEditor details customizations are loaded.
	// - Technique: public supported-entry state transition plus the stock
	//   conditional-row boundary, forced-GC identity, full order, and readback.
	// - Risk or gap: custom and stock Int KeyTypes pass while a readable authored
	//   stock payload row can be rejected solely because editing is disabled.
	// - Next RED: capture the two stock DefaultValue rows, remove both entries,
	//   force GC, and require exact pointer/order/payload rollback.
	// - Expected failure reason: the first conditional stock payload reports
	//   rows=0, expected=1 before action or footprint enlistment.
	return AssetDocumentAITypedPreimageTests::
		RunLocalKeysStockConditionalPayloadRollback(*this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBlackboardLocalKeysStockConditionalPayloadFailureTest,
	"UEAssetDoc.AI.TypedPreimage.BlackboardLocalKeysStockConditionalPayloadFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardLocalKeysStockConditionalPayloadFailureTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	// Test Design Scan:
	// - Behavior: a conditional stock payload changed after capture must fail
	//   rollback closed while its public property handle remains non-editable.
	// - Technique: post-capture state mutation plus exact existing diagnostic and
	//   negative readback proving no private or raw strong-write escape occurred.
	// - Risk or gap: accepting readable authored rows must not silently weaken the
	//   original write-authority boundary.
	return AssetDocumentAITypedPreimageTests::
		RunLocalKeysStockConditionalPayloadMutationFailsClosed(*this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBlackboardLocalKeysRejectsUnsupportedTest,
	"UEAssetDoc.AI.TypedPreimage.BlackboardLocalKeysRejectsUnsupported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardLocalKeysRejectsUnsupportedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAITypedPreimageTests;
	if (!RunLocalKeysStaleDescendantRejection(*this))
	{
		return false;
	}
	FAssetFixture Fixture;
	if (!CreateFactoryAsset(
		*this,
		UAssetDocumentAITypedPreimageTestBlackboardData::StaticClass(),
		Fixture))
	{
		return false;
	}
	auto* Root = CastChecked<UAssetDocumentAITypedPreimageTestBlackboardData>(
		Fixture.Asset);
	Fixture.Package->SetDirtyFlag(false);
	UAssetDocumentAITypedPreimageTestBlackboardData* Decoy =
		NewObject<UAssetDocumentAITypedPreimageTestBlackboardData>(Fixture.Asset);
	UAssetDocumentAITypedPreimageTestSelectorOwner* NonBlackboard =
		NewObject<UAssetDocumentAITypedPreimageTestSelectorOwner>(Fixture.Asset);
	const TArray<FString> Components = {
		TEXT("rollback.ai.local.reject"),
		TEXT("rollback.ai.local.reject.package")};
	FTestTransaction DecoyTransaction(Fixture.Context);
	const int32 DecoyLifecycleBefore = Decoy->PostEditChangePropertyCount;
	const bool bDecoyDirtyBefore = Fixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult DecoyCapture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*Fixture.Lease, *Decoy, Components, DecoyTransaction);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("non-identity Blackboard"),
		DecoyCapture,
		DecoyTransaction,
		TEXTVIEW("/AITypedPreimage/BlackboardLocalKeys"),
		TEXTVIEW("AITypedPreimage.BlackboardDataRequired"),
		DecoyLifecycleBefore,
		Decoy->PostEditChangePropertyCount,
		bDecoyDirtyBefore,
		Fixture.Package->IsDirty());

	FTestTransaction NonBlackboardTransaction(Fixture.Context);
	const int32 NonBlackboardLifecycleBefore =
		NonBlackboard->PostEditChangePropertyCount;
	const bool bNonBlackboardDirtyBefore = Fixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult NonBlackboardCapture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*Fixture.Lease,
			*NonBlackboard,
			Components,
			NonBlackboardTransaction);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("non-Blackboard object"),
		NonBlackboardCapture,
		NonBlackboardTransaction,
		TEXTVIEW("/AITypedPreimage/BlackboardLocalKeys"),
		TEXTVIEW("AITypedPreimage.BlackboardDataRequired"),
		NonBlackboardLifecycleBefore,
		NonBlackboard->PostEditChangePropertyCount,
		bNonBlackboardDirtyBefore,
		Fixture.Package->IsDirty());

	FTestContext ForeignContext;
	FTestTransaction ForeignTransaction(ForeignContext);
	const int32 ForeignLifecycleBefore = Root->PostEditChangePropertyCount;
	const bool bForeignDirtyBefore = Fixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult Foreign =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*Fixture.Lease, *Root, Components, ForeignTransaction);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("foreign local-keys transaction"),
		Foreign,
		ForeignTransaction,
		TEXTVIEW("/AITypedPreimage/BlackboardLocalKeys"),
		TEXTVIEW("AITypedPreimage.TransactionBindingMismatch"),
		ForeignLifecycleBefore,
		Root->PostEditChangePropertyCount,
		bForeignDirtyBefore,
		Fixture.Package->IsDirty());

	FTestTransaction EmptyComponents(Fixture.Context);
	const int32 EmptyLifecycleBefore = Root->PostEditChangePropertyCount;
	const bool bEmptyDirtyBefore = Fixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult Empty =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*Fixture.Lease, *Root, {}, EmptyComponents);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("empty local-keys components"),
		Empty,
		EmptyComponents,
		TEXTVIEW("/AITypedPreimage/BlackboardLocalKeys"),
		TEXTVIEW("AITypedPreimage.RollbackComponentInvalid"),
		EmptyLifecycleBefore,
		Root->PostEditChangePropertyCount,
		bEmptyDirtyBefore,
		Fixture.Package->IsDirty());

	FTestTransaction InvalidComponents(Fixture.Context);
	const int32 InvalidLifecycleBefore = Root->PostEditChangePropertyCount;
	const bool bInvalidDirtyBefore = Fixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult Invalid =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*Fixture.Lease,
			*Root,
			TArray<FString>({TEXT("duplicate"), TEXT("duplicate")}),
			InvalidComponents);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("duplicate local-keys components"),
		Invalid,
		InvalidComponents,
		TEXTVIEW("/AITypedPreimage/BlackboardLocalKeys"),
		TEXTVIEW("AITypedPreimage.RollbackComponentInvalid"),
		InvalidLifecycleBefore,
		Root->PostEditChangePropertyCount,
		bInvalidDirtyBefore,
		Fixture.Package->IsDirty());

	FTestTransaction ClosedTransaction(Fixture.Context);
	ClosedTransaction.SetState(EAssetDocumentAtomicState::Committed);
	const int32 ClosedLifecycleBefore = Root->PostEditChangePropertyCount;
	const bool bClosedDirtyBefore = Fixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult Closed =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*Fixture.Lease, *Root, Components, ClosedTransaction);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("closed local-keys transaction"),
		Closed,
		ClosedTransaction,
		TEXTVIEW("/AITypedPreimage/BlackboardLocalKeys"),
		TEXTVIEW("AITypedPreimage.TransactionStateInvalid"),
		ClosedLifecycleBefore,
		Root->PostEditChangePropertyCount,
		bClosedDirtyBefore,
		Fixture.Package->IsDirty());

	FAssetDocumentOpaqueHandle ReadOnlyHandle = Fixture.Lease->GetHandle();
	ReadOnlyHandle.Access = EAssetDocumentHandleAccess::ReadOnly;
	FOverriddenLease ReadOnlyLease(
		MoveTemp(ReadOnlyHandle), Fixture.Lease->GetPayloadChecked());
	FTestTransaction ReadOnlyTransaction(Fixture.Context);
	const int32 ReadOnlyLifecycleBefore = Root->PostEditChangePropertyCount;
	const bool bReadOnlyDirtyBefore = Fixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult ReadOnly =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			ReadOnlyLease, *Root, Components, ReadOnlyTransaction);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("read-only local-keys lease"),
		ReadOnly,
		ReadOnlyTransaction,
		TEXTVIEW("/AITypedPreimage/BlackboardLocalKeys"),
		TEXTVIEW("AITypedPreimage.TransactionBindingMismatch"),
		ReadOnlyLifecycleBefore,
		Root->PostEditChangePropertyCount,
		bReadOnlyDirtyBefore,
		Fixture.Package->IsDirty());

	UAssetDocumentAITypedPreimageTestKeyType* UnsupportedKeyType =
		NewObject<UAssetDocumentAITypedPreimageTestKeyType>(Root);
	ApplyKeys(*Root, {
		MakeEntry(TEXT("Unsupported"), *UnsupportedKeyType,
			TEXT("unsupported"), TEXT("Reject"), false)});
	Fixture.Package->SetDirtyFlag(false);
	FTestTransaction UnsupportedTransaction(Fixture.Context);
	const int32 UnsupportedLifecycleBefore = Root->PostEditChangePropertyCount;
	const bool bUnsupportedDirtyBefore = Fixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult Unsupported =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*Fixture.Lease, *Root, Components, UnsupportedTransaction);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("local KeyType arbitrary property closure"),
		Unsupported,
		UnsupportedTransaction,
		TEXTVIEW("/AITypedPreimage/BlackboardLocalKeys"),
		TEXTVIEW("AITypedPreimage.BlackboardLocalKeysInvalid"),
		UnsupportedLifecycleBefore,
		Root->PostEditChangePropertyCount,
		bUnsupportedDirtyBefore,
		Fixture.Package->IsDirty());
	ApplyKeys(*Root, {});
	Root->PostEditChangePropertyCount = 0;
	Fixture.Package->SetDirtyFlag(false);

	FTestTransaction Partial(Fixture.Context);
	Partial.RejectEnlistAtCall = 1;
	const FAssetDocumentEditorTypedPreimageResult PartialCapture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			*Fixture.Lease, *Root, Components, Partial);
	TestFalse(TEXT("local-keys partial enlistment fails"),
		PartialCapture.bSucceeded);
	TestTrue(TEXT("local-keys partial diagnostic is exact"),
		HasExactDiagnostic(
			PartialCapture.Diagnostics,
			TEXTVIEW("/AITypedPreimage/BlackboardLocalKeys"),
			TEXTVIEW("AITypedPreimage.RollbackActionEnlistmentFailed")));
	TestEqual(TEXT("one local-keys alias accepted"), Partial.Actions.Num(), 1);
	TestEqual(TEXT("local-keys aliases preserve caller order"),
		Partial.ObservedComponentIds, Components);
	TestTrue(TEXT("local-keys partial enlists no footprint"),
		Partial.EnlistedFootprint.IsEmpty());
	TestTrue(TEXT("local-keys partial cleanup succeeds"),
		Partial.Rollback(TEXTVIEW("Test.LocalPartialCleanup")).bValid);
	TestEqual(TEXT("partial cleanup emits no lifecycle"),
		Root->PostEditChangePropertyCount, 0);
	TestFalse(TEXT("local-key rejections preserve dirty"),
		Fixture.Package->IsDirty());
	Fixture.Cleanup();
	return true;
}

namespace AssetDocumentAITypedPreimageTests
{
struct FSelectorFixture
{
	FAssetFixture AssetFixture;
	UAssetDocumentAITypedPreimageTestSelectorOwner* Owner = nullptr;
	UBlackboardData* Blackboard = nullptr;
	UBlackboardKeyType_Int* IntType = nullptr;
	UBlackboardKeyType_Bool* BoolType = nullptr;
	TSharedPtr<IPropertyRowGenerator> Rows;
	TSharedPtr<IPropertyHandle> Selector;
	TSharedPtr<IPropertyHandle> Name;
	TSharedPtr<IPropertyHandle> Type;
	TSharedPtr<IPropertyHandle> Id;

	void Cleanup()
	{
		Id.Reset();
		Type.Reset();
		Name.Reset();
		Selector.Reset();
		Rows.Reset();
		AssetFixture.Cleanup();
	}
};

bool InitializeSelectorFixture(
	FAutomationTestBase& Test,
	FSelectorFixture& Fixture)
{
	if (!CreateFactoryAsset(
		Test,
		UAssetDocumentAITypedPreimageTestSelectorOwner::StaticClass(),
		Fixture.AssetFixture))
	{
		return false;
	}
	Fixture.Owner =
		Cast<UAssetDocumentAITypedPreimageTestSelectorOwner>(
			Fixture.AssetFixture.Asset);
	if (!Test.TestNotNull(TEXT("selector owner exists"), Fixture.Owner))
	{
		return false;
	}
	Fixture.Blackboard = NewObject<UBlackboardData>(Fixture.Owner);
	Fixture.IntType = NewObject<UBlackboardKeyType_Int>(Fixture.Blackboard);
	Fixture.BoolType = NewObject<UBlackboardKeyType_Bool>(Fixture.Blackboard);
	Fixture.Blackboard->Keys = {
		MakeEntry(TEXT("Target"), *Fixture.IntType,
			TEXT("selector target"), TEXT("Selector"), false)};
	Fixture.Blackboard->UpdateParentKeys();
	Fixture.Blackboard->UpdateKeyIDs();
	Fixture.Owner->Selector.AllowedTypes.Add(Fixture.IntType);
	Fixture.Owner->Selector.AllowNoneAsValue(false);
	Fixture.Owner->Selector.SelectedKeyName = TEXT("Target");
	Fixture.Owner->Selector.ResolveSelectedKey(*Fixture.Blackboard);
	Fixture.Rows = MakeRows(*Fixture.Owner);
	Fixture.Selector = RequireHandle(
		Test,
		Fixture.Rows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestSelectorOwner, Selector));
	if (!Fixture.Selector.IsValid())
	{
		return false;
	}
	Fixture.Name = Fixture.Selector->GetChildHandle(TEXT("SelectedKeyName"));
	Fixture.Type = Fixture.Selector->GetChildHandle(TEXT("SelectedKeyType"));
	Fixture.Id = Fixture.Selector->GetChildHandle(TEXT("SelectedKeyID"));
	if (!Test.TestTrue(TEXT("selector name child exists"), Fixture.Name.IsValid())
		|| !Test.TestTrue(TEXT("selector type child exists"), Fixture.Type.IsValid())
		|| !Test.TestTrue(TEXT("selector ID child exists"), Fixture.Id.IsValid()))
	{
		return false;
	}
	Fixture.Name->SetValue(
		FName(TEXT("Target")), EPropertyValueSetFlags::NotTransactable);
	Fixture.Type->SetValue(
		Fixture.BoolType->GetClass(), EPropertyValueSetFlags::NotTransactable);
	Fixture.Id->SetValue(77, EPropertyValueSetFlags::NotTransactable);
	Fixture.Owner->PostEditChangePropertyCount = 0;
	Fixture.AssetFixture.Package->SetDirtyFlag(false);
	return true;
}

bool RunSelectorPositive(
	FAutomationTestBase& Test,
	bool bEnlistBothAliases,
	int32& OutLifecycleDelta,
	TArray<FString>* OutOrdering = nullptr)
{
	FSelectorFixture Fixture;
	if (!InitializeSelectorFixture(Test, Fixture))
	{
		Fixture.Cleanup();
		return false;
	}
	const TArray<FString> Components = {
		TEXT("rollback.ai.selector.fields"),
		TEXT("rollback.ai.selector.package")};
	const TArray<FString> EarlierComponents = {
		TEXT("rollback.ai.selector.order.earlier")};
	const TArray<FString> LaterComponents = {
		TEXT("rollback.ai.selector.order.later")};
	TSharedPtr<FObservedRollbackAction> EarlierAction;
	TSharedPtr<FObservedRollbackAction> LaterAction;
	if (OutOrdering != nullptr)
	{
		EarlierAction = MakeShared<FObservedRollbackAction>(
			EarlierComponents[0],
			[&Fixture, OutOrdering]()
			{
				const bool bSeesRestored =
					Fixture.Owner->Selector.SelectedKeyName == TEXT("Target")
					&& Fixture.Owner->Selector.SelectedKeyType.Get()
						== Fixture.BoolType->GetClass()
					&& Fixture.Owner->Selector.GetSelectedKeyID()
						== FBlackboard::FKey(77);
				OutOrdering->Add(bSeesRestored
					? TEXT("earlier.sees-restored")
					: TEXT("earlier.sees-unrestored"));
			});
		if (!Test.TestTrue(TEXT("earlier ordering action enlists"),
			Fixture.AssetFixture.Transaction->EnlistRollbackAction(
				EarlierAction.ToSharedRef()).bValid))
		{
			Fixture.Cleanup();
			return false;
		}
	}
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
			*Fixture.AssetFixture.Lease,
			*Fixture.Owner,
			Fixture.Rows.ToSharedRef(),
			Fixture.Selector.ToSharedRef(),
			Components,
			*Fixture.AssetFixture.Transaction);
	if (!Test.TestTrue(TEXT("selector capture succeeds"), Capture.bSucceeded))
	{
		Fixture.Cleanup();
		return false;
	}
	if (OutOrdering != nullptr)
	{
		LaterAction = MakeShared<FObservedRollbackAction>(
			LaterComponents[0],
			[&Fixture, OutOrdering]()
			{
				const bool bSeesNormalized =
					Fixture.Owner->Selector.SelectedKeyName == TEXT("Target")
					&& Fixture.Owner->Selector.SelectedKeyType.Get()
						== Fixture.IntType->GetClass()
					&& Fixture.Owner->Selector.GetSelectedKeyID()
						== FBlackboard::FKey(0);
				OutOrdering->Add(bSeesNormalized
					? TEXT("later.sees-normalized")
					: TEXT("later.sees-restored"));
			});
		if (!Test.TestTrue(TEXT("later ordering action enlists"),
			Fixture.AssetFixture.Transaction->EnlistRollbackAction(
				LaterAction.ToSharedRef()).bValid))
		{
			Fixture.Cleanup();
			return false;
		}
		Test.TestTrue(TEXT("earlier first footprint enlists"),
			EnlistFootprints(
				*Fixture.AssetFixture.Transaction,
				EarlierComponents,
				TEXTVIEW("selector-order-earlier")));
	}
	const TConstArrayView<FString> FootprintComponents = bEnlistBothAliases
		? TConstArrayView<FString>(Components)
		: TConstArrayView<FString>(Components).Left(1);
	Test.TestTrue(TEXT("selector footprint enlists"),
		EnlistFootprints(
			*Fixture.AssetFixture.Transaction,
			FootprintComponents,
			TEXTVIEW("selector")));
	if (OutOrdering != nullptr)
	{
		Test.TestTrue(TEXT("later last footprint enlists"),
			EnlistFootprints(
				*Fixture.AssetFixture.Transaction,
				LaterComponents,
				TEXTVIEW("selector-order-later")));
		const TArray<FString> ExpectedActionOrder = {
			EarlierComponents[0], Components[0], Components[1], LaterComponents[0]};
		Test.TestEqual(TEXT("actions enlist independently in caller order"),
			Fixture.AssetFixture.Transaction->ObservedComponentIds,
			ExpectedActionOrder);
	}
	else
	{
		Test.TestEqual(TEXT("selector aliases enlist in caller order"),
			Fixture.AssetFixture.Transaction->ObservedComponentIds,
			Components);
	}
	Test.TestEqual(TEXT("selector enlists one action per alias"),
		Fixture.AssetFixture.Transaction->EnlistCallCount,
		Components.Num() + (OutOrdering != nullptr ? 2 : 0));
	Fixture.Owner->Selector.ResolveSelectedKey(*Fixture.Blackboard);
	Fixture.AssetFixture.Package->SetDirtyFlag(true);
	Test.TestEqual(TEXT("stock resolve normalizes ID"),
		Fixture.Owner->Selector.GetSelectedKeyID(), FBlackboard::FKey(0));
	Test.TestEqual(TEXT("stock resolve normalizes type"),
		Fixture.Owner->Selector.SelectedKeyType.Get(),
		Fixture.IntType->GetClass());
	Fixture.Rows->SetObjects({Fixture.Owner});
	Fixture.Id.Reset();
	Fixture.Type.Reset();
	Fixture.Name.Reset();
	Fixture.Selector.Reset();
	const int32 LifecycleBeforeRollback =
		Fixture.Owner->PostEditChangePropertyCount;
	const FAssetDocumentContractValidationResult Rollback =
		Fixture.AssetFixture.Transaction->Rollback(
			TEXTVIEW("Test.Selector.Rollback"));
	OutLifecycleDelta =
		Fixture.Owner->PostEditChangePropertyCount - LifecycleBeforeRollback;
	Test.TestTrue(TEXT("selector rollback succeeds"), Rollback.bValid);
	Test.TestEqual(TEXT("stale name restores"),
		Fixture.Owner->Selector.SelectedKeyName, FName(TEXT("Target")));
	Test.TestEqual(TEXT("stale type restores"),
		Fixture.Owner->Selector.SelectedKeyType.Get(),
		Fixture.BoolType->GetClass());
	Test.TestEqual(TEXT("stale ID restores"),
		Fixture.Owner->Selector.GetSelectedKeyID(), FBlackboard::FKey(77));
	Test.TestEqual(TEXT("AllowedTypes identity remains"),
		Fixture.Owner->Selector.AllowedTypes.Num(), 1);
	if (Fixture.Owner->Selector.AllowedTypes.Num() == 1)
	{
		Test.TestTrue(TEXT("AllowedTypes element identity remains"),
			Fixture.Owner->Selector.AllowedTypes[0].Get() == Fixture.IntType);
	}
	Test.TestFalse(TEXT("none-allowed invariant remains false"),
		Fixture.Owner->Selector.IsNone());
	Test.TestFalse(TEXT("selector dirty preimage restores"),
		Fixture.AssetFixture.Package->IsDirty());
	Test.TestTrue(TEXT("selector lifecycle observes dirty before dirty-last"),
		Fixture.Owner->bPackageDirtyDuringPostEditChangeProperty);
	if (OutOrdering != nullptr)
	{
		Test.TestEqual(TEXT("earlier action executes once"),
			EarlierAction->RollbackCount, 1);
		Test.TestEqual(TEXT("later action executes once"),
			LaterAction->RollbackCount, 1);
	}
	Fixture.Cleanup();
	return Rollback.bValid;
}

} // namespace AssetDocumentAITypedPreimageTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBehaviorTreeSelectorRollbackTest,
	"UEAssetDoc.AI.TypedPreimage.BehaviorTreeSelectorRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBehaviorTreeSelectorRollbackTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	int32 LifecycleDelta = 0;
	return AssetDocumentAITypedPreimageTests::RunSelectorPositive(
		*this, false, LifecycleDelta);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBehaviorTreeSelectorRejectsUnsupportedTest,
	"UEAssetDoc.AI.TypedPreimage.BehaviorTreeSelectorRejectsUnsupported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBehaviorTreeSelectorRejectsUnsupportedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAITypedPreimageTests;
	FSelectorFixture Fixture;
	if (!InitializeSelectorFixture(*this, Fixture))
	{
		Fixture.Cleanup();
		return false;
	}
	TSharedPtr<IPropertyHandle> Wrong = RequireHandle(
		*this,
		Fixture.Rows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestSelectorOwner, WrongVector));
	TSharedPtr<IPropertyHandle> WrongStruct = RequireHandle(
		*this,
		Fixture.Rows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestSelectorOwner, WrongStruct));
	TSharedPtr<IPropertyHandle> NestedOwner = RequireHandle(
		*this,
		Fixture.Rows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestSelectorOwner, Nested));
	TSharedPtr<IPropertyHandle> Nested = NestedOwner.IsValid()
		? NestedOwner->GetChildHandle(TEXT("Selector"))
		: nullptr;
	const TArray<FString> Components = {
		TEXT("rollback.ai.selector.reject"),
		TEXT("rollback.ai.selector.reject.package")};
	FTestTransaction WrongTransaction(Fixture.AssetFixture.Context);
	const int32 WrongLifecycleBefore =
		Fixture.Owner->PostEditChangePropertyCount;
	const bool bWrongDirtyBefore = Fixture.AssetFixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult WrongCapture =
		CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
			*Fixture.AssetFixture.Lease,
			*Fixture.Owner,
			Fixture.Rows.ToSharedRef(),
			Wrong.ToSharedRef(),
			Components,
			WrongTransaction);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("wrong selector struct family"),
		WrongCapture,
		WrongTransaction,
		TEXTVIEW("/AITypedPreimage/BehaviorTreeSelector"),
		TEXTVIEW("AITypedPreimage.BlackboardSelectorRequired"),
		WrongLifecycleBefore,
		Fixture.Owner->PostEditChangePropertyCount,
		bWrongDirtyBefore,
		Fixture.AssetFixture.Package->IsDirty());
	if (WrongStruct.IsValid())
	{
		FTestTransaction WrongStructTransaction(Fixture.AssetFixture.Context);
		const int32 WrongStructLifecycleBefore =
			Fixture.Owner->PostEditChangePropertyCount;
		const bool bWrongStructDirtyBefore =
			Fixture.AssetFixture.Package->IsDirty();
		const FAssetDocumentEditorTypedPreimageResult WrongStructCapture =
			CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
				*Fixture.AssetFixture.Lease,
				*Fixture.Owner,
				Fixture.Rows.ToSharedRef(),
				WrongStruct.ToSharedRef(),
				Components,
				WrongStructTransaction);
		RequireFailureClosedCapture(
			*this,
			TEXTVIEW("unrelated selector struct"),
			WrongStructCapture,
			WrongStructTransaction,
			TEXTVIEW("/AITypedPreimage/BehaviorTreeSelector"),
			TEXTVIEW("AITypedPreimage.BlackboardSelectorRequired"),
			WrongStructLifecycleBefore,
			Fixture.Owner->PostEditChangePropertyCount,
			bWrongStructDirtyBefore,
			Fixture.AssetFixture.Package->IsDirty());
	}

	if (TestTrue(TEXT("nested selector child exists"), Nested.IsValid()))
	{
		FTestTransaction NestedTransaction(Fixture.AssetFixture.Context);
		const int32 NestedLifecycleBefore =
			Fixture.Owner->PostEditChangePropertyCount;
		const bool bNestedDirtyBefore = Fixture.AssetFixture.Package->IsDirty();
		const FAssetDocumentEditorTypedPreimageResult NestedCapture =
			CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
				*Fixture.AssetFixture.Lease,
				*Fixture.Owner,
				Fixture.Rows.ToSharedRef(),
				Nested.ToSharedRef(),
				Components,
				NestedTransaction);
		RequireFailureClosedCapture(
			*this,
			TEXTVIEW("nested selector"),
			NestedCapture,
			NestedTransaction,
			TEXTVIEW("/AITypedPreimage/BehaviorTreeSelector"),
			TEXTVIEW("AITypedPreimage.BlackboardSelectorRequired"),
			NestedLifecycleBefore,
			Fixture.Owner->PostEditChangePropertyCount,
			bNestedDirtyBefore,
			Fixture.AssetFixture.Package->IsDirty());
	}

	UAssetDocumentAITypedPreimageTestSelectorOwner* ForeignOwner =
		NewObject<UAssetDocumentAITypedPreimageTestSelectorOwner>(
			GetTransientPackage());
	TSharedPtr<IPropertyRowGenerator> ForeignRows = MakeRows(*ForeignOwner);
	TSharedPtr<IPropertyHandle> ForeignSelector = RequireHandle(
		*this,
		ForeignRows,
		GET_MEMBER_NAME_CHECKED(
			UAssetDocumentAITypedPreimageTestSelectorOwner, Selector));
	if (ForeignSelector.IsValid())
	{
		FTestTransaction ForeignOwnerTransaction(Fixture.AssetFixture.Context);
		const int32 ForeignOwnerLifecycleBefore =
			ForeignOwner->PostEditChangePropertyCount;
		UPackage* ForeignOwnerPackage = ForeignOwner->GetOutermost();
		const bool bForeignOwnerDirtyBefore =
			ForeignOwnerPackage->IsDirty();
		const bool bLeaseAssetDirtyBefore =
			Fixture.AssetFixture.Package->IsDirty();
		const FAssetDocumentEditorTypedPreimageResult ForeignOwnerCapture =
			CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
				*Fixture.AssetFixture.Lease,
				*ForeignOwner,
				ForeignRows.ToSharedRef(),
				ForeignSelector.ToSharedRef(),
				Components,
				ForeignOwnerTransaction);
		RequireFailureClosedCapture(
			*this,
			TEXTVIEW("foreign-package selector owner"),
			ForeignOwnerCapture,
			ForeignOwnerTransaction,
			TEXTVIEW("/AITypedPreimage/BehaviorTreeSelector"),
			TEXTVIEW("AITypedPreimage.BlackboardSelectorRequired"),
			ForeignOwnerLifecycleBefore,
			ForeignOwner->PostEditChangePropertyCount,
			bForeignOwnerDirtyBefore,
			ForeignOwnerPackage->IsDirty());
		TestEqual(TEXT("foreign selector owner preserves lease asset dirty"),
			Fixture.AssetFixture.Package->IsDirty(), bLeaseAssetDirtyBefore);
	}

	FTestContext ForeignContext;
	FTestTransaction ForeignTransaction(ForeignContext);
	const int32 ForeignLifecycleBefore =
		Fixture.Owner->PostEditChangePropertyCount;
	const bool bForeignDirtyBefore = Fixture.AssetFixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult Foreign =
		CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
			*Fixture.AssetFixture.Lease,
			*Fixture.Owner,
			Fixture.Rows.ToSharedRef(),
			Fixture.Selector.ToSharedRef(),
			Components,
			ForeignTransaction);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("foreign selector token"),
		Foreign,
		ForeignTransaction,
		TEXTVIEW("/AITypedPreimage/BehaviorTreeSelector"),
		TEXTVIEW("AITypedPreimage.TransactionBindingMismatch"),
		ForeignLifecycleBefore,
		Fixture.Owner->PostEditChangePropertyCount,
		bForeignDirtyBefore,
		Fixture.AssetFixture.Package->IsDirty());

	FTestTransaction EmptyComponents(Fixture.AssetFixture.Context);
	const int32 EmptyLifecycleBefore =
		Fixture.Owner->PostEditChangePropertyCount;
	const bool bEmptyDirtyBefore = Fixture.AssetFixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult Empty =
		CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
			*Fixture.AssetFixture.Lease,
			*Fixture.Owner,
			Fixture.Rows.ToSharedRef(),
			Fixture.Selector.ToSharedRef(),
			{},
			EmptyComponents);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("empty selector components"),
		Empty,
		EmptyComponents,
		TEXTVIEW("/AITypedPreimage/BehaviorTreeSelector"),
		TEXTVIEW("AITypedPreimage.RollbackComponentInvalid"),
		EmptyLifecycleBefore,
		Fixture.Owner->PostEditChangePropertyCount,
		bEmptyDirtyBefore,
		Fixture.AssetFixture.Package->IsDirty());

	FTestTransaction InvalidComponents(Fixture.AssetFixture.Context);
	const int32 InvalidLifecycleBefore =
		Fixture.Owner->PostEditChangePropertyCount;
	const bool bInvalidDirtyBefore = Fixture.AssetFixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult Invalid =
		CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
			*Fixture.AssetFixture.Lease,
			*Fixture.Owner,
			Fixture.Rows.ToSharedRef(),
			Fixture.Selector.ToSharedRef(),
			TArray<FString>({TEXT("duplicate"), TEXT("duplicate")}),
			InvalidComponents);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("duplicate selector components"),
		Invalid,
		InvalidComponents,
		TEXTVIEW("/AITypedPreimage/BehaviorTreeSelector"),
		TEXTVIEW("AITypedPreimage.RollbackComponentInvalid"),
		InvalidLifecycleBefore,
		Fixture.Owner->PostEditChangePropertyCount,
		bInvalidDirtyBefore,
		Fixture.AssetFixture.Package->IsDirty());

	FTestTransaction ClosedTransaction(Fixture.AssetFixture.Context);
	ClosedTransaction.SetState(EAssetDocumentAtomicState::Committed);
	const int32 ClosedLifecycleBefore =
		Fixture.Owner->PostEditChangePropertyCount;
	const bool bClosedDirtyBefore = Fixture.AssetFixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult Closed =
		CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
			*Fixture.AssetFixture.Lease,
			*Fixture.Owner,
			Fixture.Rows.ToSharedRef(),
			Fixture.Selector.ToSharedRef(),
			Components,
			ClosedTransaction);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("closed selector transaction"),
		Closed,
		ClosedTransaction,
		TEXTVIEW("/AITypedPreimage/BehaviorTreeSelector"),
		TEXTVIEW("AITypedPreimage.TransactionStateInvalid"),
		ClosedLifecycleBefore,
		Fixture.Owner->PostEditChangePropertyCount,
		bClosedDirtyBefore,
		Fixture.AssetFixture.Package->IsDirty());

	FAssetDocumentOpaqueHandle ReadOnlyHandle =
		Fixture.AssetFixture.Lease->GetHandle();
	ReadOnlyHandle.Access = EAssetDocumentHandleAccess::ReadOnly;
	FOverriddenLease ReadOnlyLease(
		MoveTemp(ReadOnlyHandle),
		Fixture.AssetFixture.Lease->GetPayloadChecked());
	FTestTransaction ReadOnlyTransaction(Fixture.AssetFixture.Context);
	const int32 ReadOnlyLifecycleBefore =
		Fixture.Owner->PostEditChangePropertyCount;
	const bool bReadOnlyDirtyBefore = Fixture.AssetFixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult ReadOnly =
		CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
			ReadOnlyLease,
			*Fixture.Owner,
			Fixture.Rows.ToSharedRef(),
			Fixture.Selector.ToSharedRef(),
			Components,
			ReadOnlyTransaction);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("read-only selector lease"),
		ReadOnly,
		ReadOnlyTransaction,
		TEXTVIEW("/AITypedPreimage/BehaviorTreeSelector"),
		TEXTVIEW("AITypedPreimage.TransactionBindingMismatch"),
		ReadOnlyLifecycleBefore,
		Fixture.Owner->PostEditChangePropertyCount,
		bReadOnlyDirtyBefore,
		Fixture.AssetFixture.Package->IsDirty());

	FTestTransaction Partial(Fixture.AssetFixture.Context);
	Partial.RejectEnlistAtCall = 1;
	const FAssetDocumentEditorTypedPreimageResult PartialCapture =
		CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
			*Fixture.AssetFixture.Lease,
			*Fixture.Owner,
			Fixture.Rows.ToSharedRef(),
			Fixture.Selector.ToSharedRef(),
			Components,
			Partial);
	TestFalse(TEXT("selector partial enlistment fails"),
		PartialCapture.bSucceeded);
	TestTrue(TEXT("selector partial diagnostic is exact"),
		HasExactDiagnostic(
			PartialCapture.Diagnostics,
			TEXTVIEW("/AITypedPreimage/BehaviorTreeSelector"),
			TEXTVIEW("AITypedPreimage.RollbackActionEnlistmentFailed")));
	TestEqual(TEXT("one selector alias accepted"), Partial.Actions.Num(), 1);
	TestEqual(TEXT("selector aliases preserve caller order"),
		Partial.ObservedComponentIds, Components);
	TestTrue(TEXT("selector partial enlists no footprint"),
		Partial.EnlistedFootprint.IsEmpty());
	const int32 BeforeCleanup = Fixture.Owner->PostEditChangePropertyCount;
	TestTrue(TEXT("selector partial cleanup succeeds"),
		Partial.Rollback(TEXTVIEW("Test.SelectorPartialCleanup")).bValid);
	TestEqual(TEXT("selector partial cleanup does not mutate"),
		Fixture.Owner->PostEditChangePropertyCount, BeforeCleanup);

	FTestTransaction InvariantTransaction(Fixture.AssetFixture.Context);
	const FAssetDocumentEditorTypedPreimageResult InvariantCapture =
		CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
			*Fixture.AssetFixture.Lease,
			*Fixture.Owner,
			Fixture.Rows.ToSharedRef(),
			Fixture.Selector.ToSharedRef(),
			Components,
			InvariantTransaction);
	TestTrue(TEXT("selector invariant preimage captures"),
		InvariantCapture.bSucceeded);
	TestTrue(TEXT("selector invariant footprint enlists"),
		EnlistFootprints(
			InvariantTransaction,
			TConstArrayView<FString>(Components).Left(1),
			TEXTVIEW("selector-invariant")));
	Fixture.Owner->Selector.AllowedTypes.Add(Fixture.BoolType);
	const int32 BeforeInvariantRollback =
		Fixture.Owner->PostEditChangePropertyCount;
	const FAssetDocumentContractValidationResult InvariantRollback =
		InvariantTransaction.Rollback(
			TEXTVIEW("Test.SelectorInvariantDrift"));
	TestFalse(TEXT("selector invariant drift poisons rollback"),
		InvariantRollback.bValid);
	TestTrue(TEXT("selector invariant diagnostic is exact"),
		HasExactDiagnostic(
			InvariantRollback.Diagnostics,
			TEXTVIEW("/AITypedPreimage/BehaviorTreeSelector"),
			TEXTVIEW(
				"AITypedPreimage.BlackboardSelectorRollbackInvariantMismatch")));
	TestEqual(TEXT("invariant drift fails before selector writes"),
		Fixture.Owner->PostEditChangePropertyCount,
		BeforeInvariantRollback);

	Fixture.Rows->SetObjects({});
	FTestTransaction MissingChildTransaction(Fixture.AssetFixture.Context);
	const int32 MissingChildLifecycleBefore =
		Fixture.Owner->PostEditChangePropertyCount;
	const bool bMissingChildDirtyBefore = Fixture.AssetFixture.Package->IsDirty();
	const FAssetDocumentEditorTypedPreimageResult MissingChild =
		CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
			*Fixture.AssetFixture.Lease,
			*Fixture.Owner,
			Fixture.Rows.ToSharedRef(),
			Fixture.Selector.ToSharedRef(),
			Components,
			MissingChildTransaction);
	RequireFailureClosedCapture(
		*this,
		TEXTVIEW("stale selector row"),
		MissingChild,
		MissingChildTransaction,
		TEXTVIEW("/AITypedPreimage/BehaviorTreeSelector"),
		TEXTVIEW("AITypedPreimage.BlackboardSelectorRequired"),
		MissingChildLifecycleBefore,
		Fixture.Owner->PostEditChangePropertyCount,
		bMissingChildDirtyBefore,
		Fixture.AssetFixture.Package->IsDirty());
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAITypedPreimageRollbackOrderingTest,
	"UEAssetDoc.AI.TypedPreimage.RollbackOrdering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAITypedPreimageRollbackOrderingTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	int32 LifecycleDelta = 0;
	TArray<FString> Ordering;
	const bool bSucceeded =
		AssetDocumentAITypedPreimageTests::RunSelectorPositive(
			*this, true, LifecycleDelta, &Ordering);
	TestEqual(TEXT("alias state restores two changed selector fields once"),
		LifecycleDelta, 2);
	const TArray<FString> ExpectedOrdering = {
		TEXT("later.sees-normalized"), TEXT("earlier.sees-restored")};
	TestEqual(TEXT("rollback follows reverse first-footprint order around aliases"),
		Ordering, ExpectedOrdering);
	return bSucceeded;
}

#endif // WITH_DEV_AUTOMATION_TESTS
