// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Lifecycle/AssetDocumentEditorLifecycleHandlers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Factories/DataAssetFactory.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Lifecycle/AssetDocumentEditorEditorSafety.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecycleAdmission.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecyclePayloads.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace EditorScratchCreateFromClassHandlerLocal
{
using namespace UE::AssetDocument::EditorPrimitive;

struct FCreateScratchArguments
{
	FString CategoryId;
	FString FactoryId;
};

struct FTransientScratchResidue
{
	bool bPackageDirty = false;
	TArray<FString> RegistryEntries;
};

FAssetDocumentPrimitiveOutcome MakeCreateScratchFailure(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics = {})
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Outcome.Diagnostics.Append(Diagnostics.GetData(), Diagnostics.Num());
	return Outcome;
}

bool TryReadCreateScratchArguments(
	const FAssetDocumentRuntimeValueDto& Value,
	FCreateScratchArguments& OutArguments)
{
	const FAssetDocumentEditorValueFieldSpec Fields[] = {
		{TEXT("category_id"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("factory_id"), EAssetDocumentRuntimeValueKind::String},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	if (!TryReadAssetDocumentEditorExactObject(
		Value, Fields, Values, DiagnosticCode))
	{
		return false;
	}
	OutArguments.CategoryId = Values[0]->StringValue;
	OutArguments.FactoryId = Values[1]->StringValue;
	return OutArguments.CategoryId == TEXT("data_asset_exact.v1")
		&& OutArguments.FactoryId == TEXT("data_asset_factory.v1");
}

const FAssetDocumentPrimitiveHandleArgument* FindClassArgument(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	if (Invocation.HandleArguments.Num() == 1
		&& Invocation.HandleArguments[0].Name == TEXT("asset_class"))
	{
		return &Invocation.HandleArguments[0];
	}
	return nullptr;
}

FAssetDocumentHandleValidationContext MakeClassLeaseContext(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("editor");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = EAssetDocumentHandleKind::Class;
	Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	return Context;
}

bool TryAcquireClassLease(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FAssetDocumentVerifiedLeaseResult& OutLease)
{
	const FAssetDocumentPrimitiveHandleArgument* Argument =
		FindClassArgument(Invocation);
	if (Argument == nullptr)
	{
		return false;
	}
	OutLease = Handles.AcquireVerifiedLease(
		Argument->Handle, MakeClassLeaseContext(Invocation));
	return OutLease.Lease.IsValid() && OutLease.Diagnostics.IsEmpty();
}

UClass* TryGetCanonicalClass(
	IAssetDocumentVerifiedHandleLease& Lease)
{
	INativeIdentityPayload* Payload = TryGetNativeIdentityPayload(
		Lease.GetPayloadChecked(), ClassPayloadTypeId);
	FClassIdentityView Identity;
	if (Payload == nullptr
		|| !Payload->TryGetClassIdentity(Identity)
		|| Identity.Class == nullptr
		|| Identity.CanonicalClassPath != Identity.Class->GetPathName())
	{
		return nullptr;
	}
	return Identity.Class;
}

TArray<FString> SnapshotTransientRegistryEntries()
{
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssetsByPackageName(
		GetTransientPackage()->GetFName(), Assets);
	TArray<FString> Result;
	for (const FAssetData& Asset : Assets)
	{
		Result.Add(Asset.GetObjectPathString());
	}
	Result.Sort();
	return Result;
}

FTransientScratchResidue CaptureTransientScratchResidue()
{
	FTransientScratchResidue Result;
	Result.bPackageDirty = GetTransientPackage()->IsDirty();
	Result.RegistryEntries = SnapshotTransientRegistryEntries();
	return Result;
}

bool IsTransientScratchResidueUnchanged(
	const FTransientScratchResidue& Before,
	const FTransientScratchResidue& After)
{
	return Before.bPackageDirty == After.bPackageDirty
		&& Before.RegistryEntries == After.RegistryEntries;
}

bool IsCreatedScratchIdentityValid(
	const UObject* Scratch,
	const UClass& AssetClass)
{
	return Scratch != nullptr
		&& Scratch->GetClass() == &AssetClass
		&& Scratch->GetOuter() == GetTransientPackage()
		&& Scratch->HasAnyFlags(RF_Transient)
		&& !Scratch->HasAnyFlags(RF_Public | RF_Standalone);
}

TStrongObjectPtr<UObject> CreateTransientScratch(
	UDataAssetFactory& Factory,
	UClass& AssetClass)
{
	UObject* TransientOuter = GetTransientPackage();
	const FName ScratchName = MakeUniqueObjectName(
		TransientOuter, &AssetClass, TEXT("DA_CreateScratch"));
	// UDataAssetFactory never prompts in FactoryCreateNew; the scope keeps it so.
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	return TStrongObjectPtr<UObject>(Factory.FactoryCreateNew(
		&AssetClass,
		TransientOuter,
		ScratchName,
		RF_Transient,
		nullptr,
		GWarn));
}

FAssetDocumentRuntimeValueDto MakeCreateScratchString(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeCreateScratchOutcomeValue(
	const FCreateScratchArguments& Arguments,
	const UClass& AssetClass,
	const UObject& Scratch)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = {
		TEXT("category_id"),
		TEXT("factory_id"),
		TEXT("canonical_class"),
		TEXT("scratch_path"),
	};
	Result.Elements = {
		MakeCreateScratchString(Arguments.CategoryId),
		MakeCreateScratchString(Arguments.FactoryId),
		MakeCreateScratchString(AssetClass.GetPathName()),
		MakeCreateScratchString(Scratch.GetPathName()),
	};
	return Result;
}

bool IsIssuedScratchValid(
	const FAssetDocumentIssuedHandleResult& Issued)
{
	return Issued.Handle.IsSet() && Issued.Diagnostics.IsEmpty();
}

FAssetDocumentPrimitiveOutcome ProjectAndIssueScratch(
	const FCreateScratchArguments& Arguments,
	UClass& AssetClass,
	TStrongObjectPtr<UObject> Scratch,
	IAssetDocumentHandleResolver& Handles)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MakeCreateScratchOutcomeValue(
		Arguments, AssetClass, *Scratch);
	if (!ValidateAssetDocumentRuntimeValue(Outcome.Value).bValid)
	{
		return MakeCreateScratchFailure(
			TEXT("ScratchCreate.InvalidOutcome"),
			TEXT("Scratch identity evidence could not be projected."));
	}
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentEditorScratchPayload>(*Scratch));
	Scratch.Reset();
	if (!IsIssuedScratchValid(Issued))
	{
		return MakeCreateScratchFailure(
			TEXT("ScratchCreate.HandleIssueFailed"),
			TEXT("The request context rejected the scratch handle."),
			Issued.Diagnostics);
	}
	FAssetDocumentPrimitiveHandleResult& Result =
		Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = TEXT("scratch");
	Result.Handles.Add(MoveTemp(Issued.Handle.GetValue()));
	return Outcome;
}

FAssetDocumentPrimitiveOutcome CreateAndIssueScratch(
	const FCreateScratchArguments& Arguments,
	UClass& AssetClass,
	UDataAssetFactory& Factory,
	IAssetDocumentHandleResolver& Handles)
{
	const FTransientScratchResidue Before =
		CaptureTransientScratchResidue();
	TStrongObjectPtr<UObject> Scratch =
		CreateTransientScratch(Factory, AssetClass);
	const FTransientScratchResidue After =
		CaptureTransientScratchResidue();
	if (!Scratch.IsValid())
	{
		return MakeCreateScratchFailure(
			TEXT("ScratchCreate.AuthorityFailed"),
			TEXT("The exported factory helper did not create a scratch."));
	}
	if (!IsTransientScratchResidueUnchanged(Before, After))
	{
		return MakeCreateScratchFailure(
			TEXT("ScratchCreate.ResidueDetected"),
			TEXT("The exported factory helper left persistent residue."));
	}
	if (!IsCreatedScratchIdentityValid(Scratch.Get(), AssetClass))
	{
		return MakeCreateScratchFailure(
			TEXT("ScratchCreate.InvalidIdentity"),
			TEXT("The exported factory helper returned a wrong identity."));
	}
	return ProjectAndIssueScratch(
		Arguments, AssetClass, MoveTemp(Scratch), Handles);
}

FAssetDocumentPrimitiveOutcome CreateScratchForClass(
	const FCreateScratchArguments& Arguments,
	UClass& AssetClass,
	IAssetDocumentHandleResolver& Handles)
{
	if (!IsAssetDocumentEditorFactoryClassAdmitted(&AssetClass))
	{
		return MakeCreateScratchFailure(
			TEXT("ScratchCreate.UnsupportedClass"),
			TEXT("The Class handle is not an admitted data-asset class."));
	}
	UDataAssetFactory* Factory =
		GetAssetDocumentEditorTransientScratchFactory(&AssetClass);
	if (Factory == nullptr)
	{
		return MakeCreateScratchFailure(
			TEXT("ScratchCreate.FactoryUnavailable"),
			TEXT("The exact data-asset factory CDO is unavailable."));
	}
	return CreateAndIssueScratch(
		Arguments, AssetClass, *Factory, Handles);
}

FAssetDocumentPrimitiveOutcome CreateScratchFromLease(
	const FCreateScratchArguments& Arguments,
	IAssetDocumentVerifiedHandleLease& Lease,
	IAssetDocumentHandleResolver& Handles)
{
	UClass* AssetClass = TryGetCanonicalClass(Lease);
	if (AssetClass == nullptr)
	{
		return MakeCreateScratchFailure(
			TEXT("ScratchCreate.InvalidIdentity"),
			TEXT("The Class handle identity is not canonical."));
	}
	return CreateScratchForClass(Arguments, *AssetClass, Handles);
}

FAssetDocumentPrimitiveOutcome CreateScratchFromInvocation(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FCreateScratchArguments& Arguments,
	IAssetDocumentHandleResolver& Handles)
{
	FAssetDocumentVerifiedLeaseResult ClassLease;
	if (!TryAcquireClassLease(Invocation, Handles, ClassLease))
	{
		return MakeCreateScratchFailure(
			TEXT("ScratchCreate.ClassLeaseFailed"),
			TEXT("The Class handle could not be leased."),
			ClassLease.Diagnostics);
	}
	return CreateScratchFromLease(
		Arguments, *ClassLease.Lease, Handles);
}
}

FAssetDocumentPrimitiveOutcome
HandleAssetDocumentEditorScratchCreateFromClass(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorScratchCreateFromClassHandlerLocal;
	if (ActiveTransaction != nullptr)
	{
		return MakeCreateScratchFailure(
			TEXT("ScratchCreate.InvalidArguments"),
			TEXT("Scratch creation forbids an active transaction."));
	}
	FCreateScratchArguments Arguments;
	if (!TryReadCreateScratchArguments(
		Invocation.ValueArguments, Arguments))
	{
		return MakeCreateScratchFailure(
			TEXT("ScratchCreate.InvalidArguments"),
			TEXT("Scratch creation requires the exact value arguments."));
	}
	return CreateScratchFromInvocation(
		Invocation, Arguments, Handles);
}
