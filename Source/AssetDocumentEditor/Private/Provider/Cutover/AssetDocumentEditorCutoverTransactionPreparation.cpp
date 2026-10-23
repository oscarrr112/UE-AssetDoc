// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorCutoverTransactionPreparation.h"
#include "AssetDocumentEditorTargetLifecycleCatalog.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Provider/Cutover/AssetDocumentEditorExistingPropertyRollbackSeed.h"
#include "Provider/Cutover/AssetDocumentEditorMutationFootprintBuilders.h"
#include "Provider/Cutover/AssetDocumentEditorRollbackSeed.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecycleAdmission.h"
#include "Provider/Lifecycle/AssetDocumentEditorOrphanPackageCleanup.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace EditorCutoverTransactionPreparationLocal
{
void AddPreparationDiagnostic(
	FAssetDocumentEditorCutoverTransactionPreparation& Result,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/CutoverPreparation");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
}

bool IsExactClassPath(FStringView Value)
{
	const FString ClassPath(Value);
	const FSoftClassPath SoftPath(ClassPath);
	return !ClassPath.IsEmpty()
		&& SoftPath.ToString() == ClassPath
			&& !SoftPath.IsSubobject();
}

bool IsAdmittedFactoryClassPath(FStringView Value)
{
	const FString ClassPath(Value);
	const FSoftClassPath SoftPath(ClassPath);
	const UClass* ResolvedClass = SoftPath.ResolveClass();
	return ResolvedClass != nullptr
		&& ResolvedClass->GetPathName() == ClassPath
		&& IsAssetDocumentEditorFactoryClassAdmitted(ResolvedClass);
}

bool IsIntentContextValid(
	const FAssetDocumentEditorCutoverMutationIntent& Intent)
{
	return IsInGameThread() && GIsEditor
		&& Intent.RequestId.IsValid() && Intent.RuntimeRevision != 0;
}

bool HasRequestedOperation(
	const FAssetDocumentEditorCutoverMutationIntent& Intent)
{
	return Intent.bCreateByFactory || Intent.bWriteAdmittedProperties
		|| Intent.FactoryId.IsEmpty();
}

bool IsFactoryIntentValid(
	const FAssetDocumentEditorCutoverMutationIntent& Intent)
{
	if (!Intent.bCreateByFactory)
	{
		return Intent.FactoryId.IsEmpty();
	}
	if (const FAssetDocumentEditorTargetLifecycleCatalogEntry* Lifecycle =
		FindAssetDocumentEditorTargetLifecycle(Intent.ExactAssetClassPath);
		Lifecycle != nullptr && Intent.FactoryId == Lifecycle->FactoryId)
	{
		// Class-specific lifecycles own creation only; the generic factory row
		// may create and populate admitted properties in one transaction.
		return CanAssetDocumentEditorTargetLifecycleCreate(*Lifecycle, Intent.ExactAssetClassPath)
			&& (Lifecycle->IsClassNeutral() || !Intent.bWriteAdmittedProperties);
	}
	return (Intent.FactoryId == TEXT("data_asset_factory.v1")
		|| Intent.FactoryId == TEXT("behavior_tree_factory.v1"))
		&& IsAdmittedFactoryClassPath(Intent.ExactAssetClassPath);
}

bool IsIntentShapeValid(
	const FAssetDocumentEditorCutoverMutationIntent& Intent)
{
	return IsIntentContextValid(Intent)
		&& HasRequestedOperation(Intent)
		&& IsExactClassPath(Intent.ExactAssetClassPath)
		&& IsFactoryIntentValid(Intent);
}

bool IsExactFactoryTarget(
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	const FAssetDocumentEditorMutationIdentity& Identity)
{
	const FSoftObjectPath TargetPath(Intent.CanonicalObjectPath);
	return TargetPath.GetAssetName()
		== FPackageName::GetLongPackageAssetName(Identity.PackageName);
}

bool IsCreateTargetAbsent(
	const FAssetDocumentEditorMutationIdentity& Identity)
{
	return FSoftObjectPath(Identity.CanonicalObjectPath).ResolveObject() == nullptr
		&& !IAssetRegistry::GetChecked().GetAssetByObjectPath(
			FSoftObjectPath(Identity.CanonicalObjectPath)).IsValid()
		&& FindPackage(nullptr, *Identity.PackageName) == nullptr
		&& !FPackageName::DoesPackageExist(Identity.PackageName)
		&& !IFileManager::Get().FileExists(*Identity.PackageFilename);
}

bool HasUniqueStableIds(
	TConstArrayView<FAssetDocumentMutationFootprint> Footprint)
{
	TSet<FString> StableIds;
	for (const FAssetDocumentMutationFootprint& Item : Footprint)
	{
		if (Item.StableId.IsEmpty()
			|| Item.RollbackComponentId.IsEmpty()
			|| StableIds.Contains(Item.StableId))
		{
			return false;
		}
		StableIds.Add(Item.StableId);
	}
	return !Footprint.IsEmpty();
}

bool TryBuildPreparationIdentity(
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	FAssetDocumentEditorCutoverTransactionPreparation& Result,
	FAssetDocumentEditorMutationIdentity& OutIdentity)
{
	if (TryBuildAssetDocumentEditorMutationIdentity(
		Intent.CanonicalObjectPath, OutIdentity))
	{
		return true;
	}
	AddPreparationDiagnostic(
		Result, TEXT("CutoverPreparation.InvalidTarget"),
		TEXT("The target identity is not canonical."));
	return false;
}

bool TryValidateCreateTarget(
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	const FAssetDocumentEditorMutationIdentity& Identity,
	FAssetDocumentEditorCutoverTransactionPreparation& Result)
{
	if (!IsExactFactoryTarget(Intent, Identity))
	{
		AddPreparationDiagnostic(
			Result, TEXT("CutoverPreparation.InvalidTarget"),
			TEXT("Factory preparation requires one exact target identity."));
		return false;
	}
	// An empty, never-saved, clean package under the target name (left by a failed load
	// of the missing asset) holds no asset: the editor's New Asset flow reuses it, so it
	// is dropped here and the target counts as absent.
	DiscardAssetDocumentEditorEmptyUnsavedPackage(Identity.PackageName);
	if (!IsCreateTargetAbsent(Identity))
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/CutoverPreparation");
		Diagnostic.Code = TEXT("CutoverPreparation.TargetExists");
		Diagnostic.Message = FString::Printf(
			TEXT("Create requires an absent target, but '%s' already exists (loaded, registered or on disk); use Action Update or CreateOrUpdate."),
			*Identity.CanonicalObjectPath);
		return false;
	}
	return true;
}

bool IsExactExistingObjectIdentity(
	const UObject* Target,
	const FAssetDocumentEditorCutoverMutationIntent& Intent)
{
	if (Target == nullptr)
	{
		return false;
	}
	const UClass* TargetClass = Target->GetClass();
	return Target->GetPathName() == Intent.CanonicalObjectPath
		&& TargetClass != nullptr
		&& TargetClass->GetPathName() == Intent.ExactAssetClassPath;
}

bool IsExactExistingPackageIdentity(
	const UObject& Target,
	const FAssetDocumentEditorMutationIdentity& Identity)
{
	const UPackage* Package = Target.GetOutermost();
	return Package != nullptr && Package->GetName() == Identity.PackageName;
}

bool IsExactExistingPropertyTarget(
	const UObject* Target,
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	const FAssetDocumentEditorMutationIdentity& Identity)
{
	return IsExactExistingObjectIdentity(Target, Intent)
		&& IsExactExistingPackageIdentity(*Target, Identity);
}

UObject* ResolveExactExistingPropertyTarget(
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	const FAssetDocumentEditorMutationIdentity& Identity)
{
	UObject* Target =
		FSoftObjectPath(Intent.CanonicalObjectPath).ResolveObject();
	return IsExactExistingPropertyTarget(Target, Intent, Identity)
		? Target
		: nullptr;
}

void PrepareCreateOnly(
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	const FAssetDocumentEditorMutationIdentity& Identity,
	FAssetDocumentEditorCutoverTransactionPreparation& Result)
{
	Result.CompleteFootprint.Append(
		BuildAssetDocumentEditorFactoryMutationFootprint(Identity));
	Result.RollbackSeeds.Add(
		MakeAssetDocumentEditorCutoverRollbackSeed(
			Identity, Intent.ExactAssetClassPath));
}

void PrepareCombined(
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	const FAssetDocumentEditorMutationIdentity& Identity,
	FAssetDocumentEditorCutoverTransactionPreparation& Result)
{
	Result.CompleteFootprint.Append(
		BuildAssetDocumentEditorFactoryMutationFootprint(Identity));
	Result.CompleteFootprint.Append(
		BuildAssetDocumentEditorPropertyMutationFootprint(Identity));
	Result.RollbackSeeds =
		MakeAssetDocumentEditorCombinedRollbackSeeds(
			MakeAssetDocumentEditorCutoverRollbackSeed(
				Identity, Intent.ExactAssetClassPath));
}

bool PreparePropertyOnly(
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	const FAssetDocumentEditorMutationIdentity& Identity,
	FAssetDocumentEditorCutoverTransactionPreparation& Result)
{
	UObject* ExactTarget =
		ResolveExactExistingPropertyTarget(Intent, Identity);
	if (ExactTarget == nullptr)
	{
		AddPreparationDiagnostic(
			Result, TEXT("CutoverPreparation.InvalidTarget"),
			TEXT("Property preparation requires one exact live target."));
		return false;
	}
	FAssetDocumentEditorPropertyPreimageCaptureResult Capture =
		CaptureAssetDocumentEditorWritablePropertyPreimage(*ExactTarget);
	if (!Capture.bSucceeded)
	{
		Result.Diagnostics = MoveTemp(Capture.Diagnostics);
		return false;
	}
	Result.CompleteFootprint.Append(
		BuildAssetDocumentEditorPropertyMutationFootprint(Identity));
	Result.RollbackSeeds =
		MakeAssetDocumentEditorExistingPropertyRollbackSeeds(
			MoveTemp(Capture.Preimage));
	return true;
}

bool PrepareAcquisitionOnly(
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	const FAssetDocumentEditorMutationIdentity& Identity,
	FAssetDocumentEditorCutoverTransactionPreparation& Result)
{
	if (ResolveExactExistingPropertyTarget(Intent, Identity) == nullptr)
	{
		AddPreparationDiagnostic(Result, TEXT("CutoverPreparation.InvalidTarget"),
			TEXT("Acquisition requires one exact live target."));
		return false;
	}
	return true;
}

bool TryPrepareRequestedOperation(
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	const FAssetDocumentEditorMutationIdentity& Identity,
	FAssetDocumentEditorCutoverTransactionPreparation& Result)
{
	if (!Intent.bCreateByFactory)
	{
		return Intent.bWriteAdmittedProperties
			? PreparePropertyOnly(Intent, Identity, Result)
			: PrepareAcquisitionOnly(Intent, Identity, Result);
	}
	if (!TryValidateCreateTarget(Intent, Identity, Result))
	{
		return false;
	}
	if (Intent.bWriteAdmittedProperties)
	{
		PrepareCombined(Intent, Identity, Result);
	}
	else
	{
		PrepareCreateOnly(Intent, Identity, Result);
	}
	return true;
}
}

FAssetDocumentEditorCutoverTransactionPreparation
PrepareAssetDocumentEditorCutoverTransaction(
	const FAssetDocumentEditorCutoverMutationIntent& Intent)
{
	using namespace EditorCutoverTransactionPreparationLocal;
	FAssetDocumentEditorCutoverTransactionPreparation Result;
	if (!IsIntentShapeValid(Intent))
	{
		AddPreparationDiagnostic(
			Result, TEXT("CutoverPreparation.InvalidIntent"),
			TEXT("Cutover preparation requires one exact typed intent."));
		return Result;
	}
	FAssetDocumentEditorMutationIdentity Identity;
	if (!TryBuildPreparationIdentity(Intent, Result, Identity)
		|| !TryPrepareRequestedOperation(
			Intent, Identity, Result))
	{
		return Result;
	}
	const bool bAcquisitionOnly = !Intent.bCreateByFactory
		&& !Intent.bWriteAdmittedProperties;
	if ((!bAcquisitionOnly && !HasUniqueStableIds(Result.CompleteFootprint))
		|| (bAcquisitionOnly && (!Result.CompleteFootprint.IsEmpty()
			|| !Result.RollbackSeeds.IsEmpty())))
	{
		Result.CompleteFootprint.Reset();
		Result.RollbackSeeds.Reset();
		AddPreparationDiagnostic(
			Result, TEXT("CutoverPreparation.InvalidFootprint"),
			TEXT("The complete footprint contains missing or duplicate identity."));
		return Result;
	}
	Result.bPrepared = true;
	return Result;
}
