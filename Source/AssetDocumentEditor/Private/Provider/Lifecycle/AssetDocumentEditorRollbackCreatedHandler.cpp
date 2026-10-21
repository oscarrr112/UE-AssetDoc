// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Lifecycle/AssetDocumentEditorLifecycleHandlers.h"

#include "Misc/PackageName.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Cutover/AssetDocumentEditorMutationFootprintBuilders.h"
#include "Provider/Cutover/AssetDocumentEditorRollbackSeed.h"
#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecyclePayloads.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace EditorRollbackCreatedHandlerLocal
{
using namespace UE::AssetDocument::EditorPrimitive;

bool IsRollbackCreatedFactoryId(FStringView FactoryId)
{
	return FactoryId == TEXT("data_asset_factory.v1")
		|| FactoryId == TEXT("behavior_tree_factory.v1")
		// The Blueprint row declares this primitive as its rollback and its creation
		// record carries this id (see AssetDocumentEditorBlueprintLifecycleHandlers.cpp).
		|| FactoryId == TEXT("blueprint_factory.v1")
		|| FactoryId == AssetDocumentEditorGenericFactoryLifecycleId;
}

struct FRollbackCreatedTarget
{
	UObject* Asset = nullptr;
	UPackage* Package = nullptr;
	FString CanonicalObjectPath;
	FString PackageName;
	FString PackageFilename;
};

FAssetDocumentPrimitiveOutcome MakeRollbackFailure(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	if (!Diagnostics.IsEmpty())
	{
		Outcome.Diagnostics.Append(Diagnostics.GetData(), Diagnostics.Num());
		return Outcome;
	}
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeRollbackFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	return MakeRollbackFailure(
		Code, Message, TConstArrayView<FAssetDocumentRuntimeDiagnostic>());
}

const FAssetDocumentPrimitiveHandleArgument* FindRollbackArgument(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const TCHAR* Name)
{
	if (Invocation.HandleArguments.Num() != 2)
	{
		return nullptr;
	}
	for (const FAssetDocumentPrimitiveHandleArgument& Argument :
		Invocation.HandleArguments)
	{
		if (Argument.Name == Name)
		{
			return &Argument;
		}
	}
	return nullptr;
}

FAssetDocumentHandleValidationContext MakeRollbackLeaseContext(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	EAssetDocumentHandleKind Kind)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("editor");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = Kind;
	Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Context;
}

bool TryAcquireRollbackLease(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	const TCHAR* Name,
	EAssetDocumentHandleKind Kind,
	FAssetDocumentVerifiedLeaseResult& OutLease,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	const FAssetDocumentPrimitiveHandleArgument* Argument =
		FindRollbackArgument(Invocation, Name);
	if (Argument == nullptr)
	{
		return false;
	}
	OutLease = Handles.AcquireVerifiedLease(
		Argument->Handle,
		MakeRollbackLeaseContext(Invocation, Kind));
	if (!OutLease.Lease.IsValid() || !OutLease.Diagnostics.IsEmpty())
	{
		OutDiagnostics.Append(OutLease.Diagnostics);
		return false;
	}
	return true;
}

bool AreSharedCreationRecords(
	const FAssetDocumentEditorCreationRecord* AssetRecord,
	const FAssetDocumentEditorCreationRecord* PackageRecord)
{
	return AssetRecord != nullptr
		&& PackageRecord != nullptr
		&& AssetRecord == PackageRecord;
}

bool IsCreationScopeExact(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentEditorCreationRecord& Record)
{
	return Record.RequestId == Invocation.RequestId
		&& Record.TransactionId == Invocation.TransactionId
		&& Record.RuntimeRevision == Invocation.RuntimeRevision
		&& IsRollbackCreatedFactoryId(Record.FactoryId);
}

bool WasCreationTargetAbsent(
	const FAssetDocumentEditorCreationRecord& Record)
{
	return !Record.bObjectExistedBefore
		&& !Record.bPackageExistedBefore
		&& !Record.bFileExistedBefore
		&& !Record.bPackageWasDirty;
}

bool HasCanonicalCreationFilename(
	const FAssetDocumentEditorCreationRecord& Record)
{
	return FPackageName::LongPackageNameToFilename(
		Record.PackageName,
		FPackageName::GetAssetPackageExtension())
		== Record.PackageFilename;
}

bool IsCreationRecordAdmitted(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentEditorCreationRecord& Record)
{
	return IsCreationScopeExact(Invocation, Record)
		&& WasCreationTargetAbsent(Record)
		&& HasCanonicalCreationFilename(Record);
}

bool IsAssetIdentityExact(
	const FAssetDocumentEditorCreationRecord& Record,
	const FObjectIdentityView& Identity)
{
	return Identity.Object != nullptr
		&& Identity.Object == Record.Asset.Get()
		&& Identity.CanonicalObjectPath == Record.CanonicalObjectPath
		&& Identity.Object->GetPathName() == Record.CanonicalObjectPath
		&& Identity.Object->GetOutermost() == Record.Package.Get();
}

bool IsPackageIdentityExact(
	const FAssetDocumentEditorCreationRecord& Record,
	const FPackageIdentityView& Identity)
{
	return Identity.Package != nullptr
		&& Identity.Package == Record.Package.Get()
		&& Identity.PackageName == Record.PackageName
		&& Identity.Filename == Record.PackageFilename
		&& Identity.Package->GetName() == Record.PackageName;
}

bool IsCreatedAssetPayloadExact(
	IAssetDocumentNativeHandlePayload& Payload,
	const FAssetDocumentEditorCreationRecord& Record)
{
	INativeIdentityPayload* IdentityPayload =
		TryGetNativeIdentityPayload(Payload, CreatedAssetPayloadTypeId);
	if (IdentityPayload == nullptr)
	{
		return false;
	}
	FObjectIdentityView Identity;
	if (!IdentityPayload->TryGetObjectIdentity(Identity))
	{
		return false;
	}
	return IsAssetIdentityExact(Record, Identity);
}

bool IsCreatedPackagePayloadExact(
	IAssetDocumentNativeHandlePayload& Payload,
	const FAssetDocumentEditorCreationRecord& Record)
{
	INativeIdentityPayload* IdentityPayload =
		TryGetNativeIdentityPayload(Payload, CreatedPackagePayloadTypeId);
	if (IdentityPayload == nullptr)
	{
		return false;
	}
	FPackageIdentityView Identity;
	if (!IdentityPayload->TryGetPackageIdentity(Identity))
	{
		return false;
	}
	return IsPackageIdentityExact(Record, Identity);
}

bool TrySnapshotRollbackTarget(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentNativeHandlePayload& AssetPayload,
	IAssetDocumentNativeHandlePayload& PackagePayload,
	FRollbackCreatedTarget& OutTarget)
{
	const FAssetDocumentEditorCreationRecord* AssetRecord =
		TryGetAssetDocumentEditorCreatedAssetRecord(AssetPayload);
	const FAssetDocumentEditorCreationRecord* PackageRecord =
		TryGetAssetDocumentEditorCreatedPackageRecord(PackagePayload);
	if (!AreSharedCreationRecords(AssetRecord, PackageRecord))
	{
		return false;
	}
	if (!IsCreationRecordAdmitted(Invocation, *AssetRecord))
	{
		return false;
	}
	if (!IsCreatedAssetPayloadExact(AssetPayload, *AssetRecord))
	{
		return false;
	}
	if (!IsCreatedPackagePayloadExact(PackagePayload, *PackageRecord))
	{
		return false;
	}
	OutTarget.Asset = AssetRecord->Asset.Get();
	OutTarget.Package = AssetRecord->Package.Get();
	OutTarget.CanonicalObjectPath = AssetRecord->CanonicalObjectPath;
	OutTarget.PackageName = AssetRecord->PackageName;
	OutTarget.PackageFilename = AssetRecord->PackageFilename;
	return true;
}

bool TryResolveRollbackTarget(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FRollbackCreatedTarget& OutTarget,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	FAssetDocumentVerifiedLeaseResult AssetLease;
	if (!TryAcquireRollbackLease(
		Invocation, Handles, TEXT("asset"),
		EAssetDocumentHandleKind::Asset,
		AssetLease, OutDiagnostics))
	{
		return false;
	}
	FAssetDocumentVerifiedLeaseResult PackageLease;
	if (!TryAcquireRollbackLease(
		Invocation, Handles, TEXT("package"),
		EAssetDocumentHandleKind::Package,
		PackageLease, OutDiagnostics))
	{
		return false;
	}
	return TrySnapshotRollbackTarget(
		Invocation,
		AssetLease.Lease->GetPayloadChecked(),
		PackageLease.Lease->GetPayloadChecked(),
		OutTarget);
}

FAssetDocumentRuntimeValueDto MakeRollbackBoolean(bool bValue)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = bValue;
	return Result;
}

FAssetDocumentPrimitiveOutcome MakeRollbackSuccess(
	const FAssetDocumentEditorCreatedTargetAbsence& Evidence)
{
	const FAssetDocumentEditorValueField Fields[] = {
		{TEXT("object_absent"),
			MakeRollbackBoolean(Evidence.bObjectAbsent)},
		{TEXT("package_absent"),
			MakeRollbackBoolean(Evidence.bPackageAbsent)},
		{TEXT("registry_absent"),
			MakeRollbackBoolean(Evidence.bRegistryAbsent)},
		{TEXT("file_absent"),
			MakeRollbackBoolean(Evidence.bFileAbsent)},
		{TEXT("dirty_residue"),
			MakeRollbackBoolean(Evidence.bDirtyResidue)},
	};
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	FString DiagnosticCode;
	if (!TryBuildAssetDocumentEditorValueObject(
		Fields, Outcome.Value, DiagnosticCode))
	{
		return MakeRollbackFailure(
			TEXT("RollbackCreated.InvalidOutcome"),
			TEXT("Rollback absence evidence could not be projected."));
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome DeleteRollbackTarget(
	const FRollbackCreatedTarget& Target,
	IAssetDocumentAtomicTransaction& Transaction)
{
	FAssetDocumentEditorMutationIdentity Identity;
	if (!TryBuildAssetDocumentEditorMutationIdentity(
		Target.CanonicalObjectPath, Identity)
		|| Identity.PackageName != Target.PackageName
		|| Identity.PackageFilename != Target.PackageFilename)
	{
		return MakeRollbackFailure(
			TEXT("RollbackCreated.IdentityMismatch"),
			TEXT("Rollback identity is not canonical."));
	}
	const FAssetDocumentContractValidationResult Enlistment =
		Transaction.EnlistFootprint(
			BuildAssetDocumentEditorFactoryMutationFootprint(Identity));
	if (!Enlistment.bValid)
	{
		return MakeRollbackFailure(
			TEXT("RollbackCreated.FootprintRejected"),
			TEXT("The atomic transaction rejected the rollback footprint."),
			Enlistment.Diagnostics);
	}
	FAssetDocumentEditorCreatedTarget SharedTarget;
	SharedTarget.Asset = Target.Asset;
	SharedTarget.Package = Target.Package;
	SharedTarget.Identity = MoveTemp(Identity);
	FAssetDocumentEditorCreatedTargetAbsence Absence;
	const FAssetDocumentContractValidationResult Deletion =
		DeleteAssetDocumentEditorCreatedTarget(SharedTarget, Absence);
	if (!Deletion.bValid)
	{
		return MakeRollbackFailure(
			TEXT("RollbackCreated.DeleteFailed"),
			TEXT("The authoritative deletion helper failed."),
			Deletion.Diagnostics);
	}
	return MakeRollbackSuccess(Absence);
}
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorRollbackCreatedAsset(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorRollbackCreatedHandlerLocal;
	FRollbackCreatedTarget Target;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	if (!TryResolveRollbackTarget(
		Invocation, Handles, Target, Diagnostics))
	{
		return MakeRollbackFailure(
			TEXT("RollbackCreated.CreationRecordMismatch"),
			TEXT("Rollback requires matching transaction-owned identities."),
			Diagnostics);
	}
	return DeleteRollbackTarget(
		Target, *ActiveTransaction);
}
