// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Cutover/AssetDocumentEditorRollbackSeed.h"

#include "AssetDocumentAssetCompilation.h"
#include "AssetDocumentAssetDelete.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "ObjectTools.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace
{
FAssetDocumentContractValidationResult MakeSeedResult(
	bool bValid,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!bValid)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/CutoverRollbackSeed");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

// Where a created target is still present: "object, package, ...".
FString ResidueSummary(const FAssetDocumentEditorCreatedTargetAbsence& Absence)
{
	TArray<FString> Left;
	if (!Absence.bObjectAbsent) { Left.Add(TEXT("object loaded")); }
	if (!Absence.bPackageAbsent) { Left.Add(TEXT("package loaded")); }
	if (!Absence.bRegistryAbsent) { Left.Add(TEXT("asset registry entry")); }
	if (!Absence.bFileAbsent) { Left.Add(TEXT("package file")); }
	if (Absence.bDirtyResidue) { Left.Add(TEXT("dirty package")); }
	return FString::Join(Left, TEXT(", "));
}

class FAssetDocumentEditorRollbackSeedState;

class FAssetDocumentEditorMutationCapture final
	: public IAssetDocumentEditorCutoverMutationCapture
{
public:
	explicit FAssetDocumentEditorMutationCapture(
		TSharedRef<FAssetDocumentEditorRollbackSeedState> InState);
	virtual ~FAssetDocumentEditorMutationCapture() override;

private:
	TSharedRef<FAssetDocumentEditorRollbackSeedState> State;
};

class FAssetDocumentEditorRollbackSeedState final
	: public TSharedFromThis<FAssetDocumentEditorRollbackSeedState>
{
public:
	FAssetDocumentEditorRollbackSeedState(
		FAssetDocumentEditorMutationIdentity InIdentity,
		FString InExactAssetClassPath)
		: Identity(MoveTemp(InIdentity))
		, ExactAssetClassPath(MoveTemp(InExactAssetClassPath))
	{
	}

	bool Open(const FGuid& TransactionId)
	{
		if (!IsInGameThread() || !GIsEditor || !TransactionId.IsValid()
			|| bCaptureWasOpened || bCaptureOpen || bConsumed)
		{
			return false;
		}
		bCaptureWasOpened = true;
		bCaptureOpen = true;
		TWeakPtr<FAssetDocumentEditorRollbackSeedState> WeakState =
			AsShared();
		DelegateHandle = IAssetRegistry::GetChecked().OnAssetAdded().AddLambda(
			[WeakState](const FAssetData& AssetData)
			{
				if (TSharedPtr<FAssetDocumentEditorRollbackSeedState> State =
					WeakState.Pin())
				{
					State->ObserveAddedAsset(AssetData);
				}
			});
		return DelegateHandle.IsValid();
	}

	void Close()
	{
		if (!bCaptureOpen)
		{
			return;
		}
		IAssetRegistry::GetChecked().OnAssetAdded().Remove(DelegateHandle);
		DelegateHandle.Reset();
		bCaptureOpen = false;
	}

	FAssetDocumentContractValidationResult Rollback()
	{
		if (!IsInGameThread() || !GIsEditor || bCaptureOpen || bConsumed)
		{
			return MakeSeedResult(
				false, TEXT("CutoverRollback.InvalidState"),
				TEXT("Rollback requires one closed, unconsumed capture."));
		}
		bConsumed = true;
		if (bPoisoned)
		{
			return MakeSeedResult(
				false, TEXT("CutoverRollback.PoisonedCapture"),
				TEXT("The creation capture did not preserve exact identity."));
		}
		if (!bCaptured)
		{
			const FAssetDocumentEditorCreatedTargetAbsence Absence =
				CaptureAssetDocumentEditorCreatedTargetAbsence(Identity);
			return IsAssetDocumentEditorCreatedTargetFullyAbsent(Absence)
				? MakeSeedResult(true, TEXT(""), TEXT(""))
				: MakeSeedResult(
					false, TEXT("CutoverRollback.MissingCapture"),
					TEXT("The target appeared without an exact capture."));
		}
		FAssetDocumentEditorCreatedTarget Target;
		Target.Asset = Asset.Get();
		Target.Package = Package.Get();
		Target.Identity = Identity;
		Asset.Reset();
		Package.Reset();
		FAssetDocumentEditorCreatedTargetAbsence Absence;
		return DeleteAssetDocumentEditorCreatedTarget(Target, Absence);
	}

private:
	void ObserveAddedAsset(const FAssetData& AssetData)
	{
		if (!bCaptureOpen
			|| AssetData.GetObjectPathString() != Identity.CanonicalObjectPath)
		{
			return;
		}
		if (bCaptured
			|| AssetData.AssetClassPath.ToString() != ExactAssetClassPath)
		{
			bPoisoned = true;
			return;
		}
		UObject* Added = AssetData.GetAsset();
		if (Added == nullptr
			|| Added->GetPathName() != Identity.CanonicalObjectPath
			|| Added->GetClass()->GetPathName() != ExactAssetClassPath
			|| Added->GetOutermost()->GetName() != Identity.PackageName)
		{
			bPoisoned = true;
			return;
		}
		Asset.Reset(Added);
		Package.Reset(Added->GetOutermost());
		bCaptured = true;
	}

	FAssetDocumentEditorMutationIdentity Identity;
	FString ExactAssetClassPath;
	TStrongObjectPtr<UObject> Asset;
	TStrongObjectPtr<UPackage> Package;
	FDelegateHandle DelegateHandle;
	bool bCaptureWasOpened = false;
	bool bCaptureOpen = false;
	bool bCaptured = false;
	bool bPoisoned = false;
	bool bConsumed = false;
};

FAssetDocumentEditorMutationCapture::FAssetDocumentEditorMutationCapture(
	TSharedRef<FAssetDocumentEditorRollbackSeedState> InState)
	: State(MoveTemp(InState))
{
}

FAssetDocumentEditorMutationCapture::~FAssetDocumentEditorMutationCapture()
{
	State->Close();
}

class FAssetDocumentEditorRollbackSeed final
	: public IAssetDocumentEditorCutoverRollbackSeed
{
public:
	FAssetDocumentEditorRollbackSeed(
		FAssetDocumentEditorMutationIdentity InIdentity,
		FString InExactAssetClassPath)
		: State(MakeShared<FAssetDocumentEditorRollbackSeedState>(
			MoveTemp(InIdentity), MoveTemp(InExactAssetClassPath)))
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return TEXT("editor.asset.rollback_created.v1");
	}

	virtual TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>
	OpenMutationCapture(const FGuid& TransactionId) override
	{
		if (!State->Open(TransactionId))
		{
			return nullptr;
		}
		return MakeUnique<FAssetDocumentEditorMutationCapture>(State);
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		(void)StableFailureCode;
		return State->Rollback();
	}

private:
	TSharedRef<FAssetDocumentEditorRollbackSeedState> State;
};
}

FAssetDocumentEditorCreatedTargetAbsence
CaptureAssetDocumentEditorCreatedTargetAbsence(
	const FAssetDocumentEditorMutationIdentity& Identity)
{
	FAssetDocumentEditorCreatedTargetAbsence Absence;
	Absence.bObjectAbsent =
		FSoftObjectPath(Identity.CanonicalObjectPath).ResolveObject() == nullptr;
	UPackage* ExistingPackage =
		FindPackage(nullptr, *Identity.PackageName);
	Absence.bPackageAbsent = ExistingPackage == nullptr;
	Absence.bRegistryAbsent = !IAssetRegistry::GetChecked()
		.GetAssetByObjectPath(FSoftObjectPath(
			Identity.CanonicalObjectPath)).IsValid();
	Absence.bFileAbsent = !IFileManager::Get().FileExists(
		*Identity.PackageFilename);
	Absence.bDirtyResidue =
		ExistingPackage != nullptr && ExistingPackage->IsDirty();
	return Absence;
}

bool IsAssetDocumentEditorCreatedTargetFullyAbsent(
	const FAssetDocumentEditorCreatedTargetAbsence& Absence)
{
	return Absence.bObjectAbsent
		&& Absence.bPackageAbsent
		&& Absence.bRegistryAbsent
		&& Absence.bFileAbsent
		&& !Absence.bDirtyResidue;
}

FAssetDocumentContractValidationResult
DeleteAssetDocumentEditorCreatedTarget(
	const FAssetDocumentEditorCreatedTarget& Target,
	FAssetDocumentEditorCreatedTargetAbsence& OutAbsence)
{
	if (Target.Asset == nullptr || Target.Package == nullptr
		|| Target.Asset->GetPathName() != Target.Identity.CanonicalObjectPath
		|| Target.Package->GetName() != Target.Identity.PackageName
		|| Target.Asset->GetOutermost() != Target.Package)
	{
		return MakeSeedResult(
			false, TEXT("RollbackCreated.IdentityMismatch"),
			TEXT("Authoritative deletion requires exact native identity."));
	}
 // An animation asset's cached data controller references its model subobject;
 // the editor's asset deletion would see that as an external reference. Unbind
 // it for the delete and restore it if the delete is refused.
 TWeakObjectPtr<UObject> OriginalModel;
 TWeakObjectPtr<UObject> OriginalAsset(Target.Asset);
 IAnimationDataController* Controller=nullptr;
 if (UAnimSequenceBase* Animation=Cast<UAnimSequenceBase>(Target.Asset))
 {
  auto Model=Animation->GetDataModelInterface();
  if (Model.GetObject())
  {
   Controller=&Animation->GetController();
   if (Controller->GetModelInterface().GetObject()!=Model.GetObject())
    return MakeSeedResult(false,TEXT("RollbackCreated.ControllerMismatch"),TEXT("The animation controller is bound to another model."));
   OriginalModel=Model.GetObject();
   Controller->SetModel(nullptr);
  }
 }
 // Editor-equivalent (ObjectTools::DeleteSingleObject, the Delete Assets
 // dialog): when only the undo buffer still references the created asset,
 // because a populate step transacted on it (e.g. its animation data model or
 // a Blueprint's members), the editor resets the transaction history before
 // deleting so garbage collection can purge it. DeleteObjects skips that reset.
 // As in the dialog, systems first drop the references they hold only for
 // their own bookkeeping (OnAssetsPreDelete: the Blueprint action database's
 // spawners of a compiled Blueprint), and the check follows a garbage
 // collection, as the delete (FAssetDeleteModel) does before its own reference
 // scan: editor caches released on GC (e.g. Niagara's digested compile graphs,
 // which duplicate a system's data interfaces) are not users of the asset that
 // would keep the undo history; transient caches that are never released (see
 // AssetDocumentAssetDelete::ReleaseTransientReferencers) drop their references into the package. Behaviour tests:
 // UEAssetDoc.GenericRuntime.Rollback.CreateFailureLeavesNothing (NiagaraScript),
 // UEAssetDoc.GenericRuntime.Rollback.CreateFailureCompile,
 // UEAssetDoc.MontageLifecycle.GenericFactoryContext,
 // UEAssetDoc.UBlueprint.RoundTrip.FailedCreateLeavesNothing.
 {
  FEditorDelegates::OnAssetsPreDelete.Broadcast(TArray<UObject*>{Target.Asset});
  CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
  const TArray<FString> Released=AssetDocumentAssetDelete::ReleaseTransientReferencers(*Target.Asset, *Target.Package);
  if (!Released.IsEmpty())
  {
   UE_LOG(LogTemp, Log, TEXT("AssetDocument: rollback released transient references to created %s: %s"),
    *Target.Identity.CanonicalObjectPath, *FString::Join(Released, TEXT("; ")));
  }
  bool bReferenced=false, bReferencedByUndo=false;
  ObjectTools::GatherObjectReferencersForDeletion(Target.Asset, bReferenced, bReferencedByUndo);
  if (!bReferenced && bReferencedByUndo && GEditor)
   GEditor->ResetTransaction(NSLOCTEXT("AssetDocument", "RollbackCreatedAsset", "Roll back created asset"));
 }
	AssetDocumentAssetCompilation::FinishPackageCompilation({Target.Package});
	const int32 Deleted = ObjectTools::DeleteObjects(
		{Target.Asset}, false,
		ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
 OutAbsence=CaptureAssetDocumentEditorCreatedTargetAbsence(Target.Identity);
 // The asset registry can hold a cached entry for an asset that was never saved:
 // an asset that reports finalized tags (Niagara does on every system compile,
 // AssetTagsFinalized) is queued for the registry's loaded-asset cache, which
 // snapshots it while its package is clean. The deletion only tells the registry
 // that a package was deleted when the package has a file (ObjectTools unloads a
 // file-less package instead), so that cached entry outlived the asset: the
 // rollback reported residue and the target kept "existing" for later requests.
 // A rescan of the absent package file drops the package's cached entries, as a
 // delete on disk does.
 if (Deleted == 1 && OutAbsence.bObjectAbsent && OutAbsence.bPackageAbsent
  && OutAbsence.bFileAbsent && !OutAbsence.bRegistryAbsent)
 {
  IAssetRegistry::GetChecked().ScanModifiedAssetFiles({Target.Identity.PackageFilename});
  OutAbsence=CaptureAssetDocumentEditorCreatedTargetAbsence(Target.Identity);
 }
 if (Deleted != 1 || !IsAssetDocumentEditorCreatedTargetFullyAbsent(OutAbsence))
 {
  // DeleteObjects may report a deleted row while a legitimate strong reference
  // keeps the object alive. Every incomplete deletion restores surviving state.
  if (Controller && OriginalAsset.IsValid())
  {
   if (!OriginalModel.IsValid())
    return MakeSeedResult(false,TEXT("RollbackCreated.ControllerRestoreFailed"),TEXT("Incomplete deletion invalidated the original model."));
   auto Model=CastChecked<UAnimSequenceBase>(OriginalAsset.Get())->GetDataModelInterface();
   if (Model.GetObject()!=OriginalModel.Get())
    return MakeSeedResult(false,TEXT("RollbackCreated.ControllerRestoreFailed"),TEXT("Incomplete deletion changed model identity."));
   Controller->SetModel(Model);
   if (Controller->GetModelInterface().GetObject()!=OriginalModel.Get())
    return MakeSeedResult(false,TEXT("RollbackCreated.ControllerRestoreFailed"),TEXT("Controller restoration failed."));
  }
  if (Deleted != 1)
  {
   const FString Blockers = OriginalAsset.IsValid() ? AssetDocumentAssetDelete::DescribeDeleteBlockers(AssetDocumentAssetDelete::FindDeleteBlockers(*OriginalAsset.Get())) : FString(TEXT("the target is gone"));
   UE_LOG(LogTemp, Warning, TEXT("AssetDocument: rollback could not delete created %s: %s"), *Target.Identity.CanonicalObjectPath, *Blockers);
   return MakeSeedResult(false,TEXT("RollbackCreated.DeleteRejected"),*FString::Printf(TEXT("The editor's asset deletion refused the created target: %s."), *Blockers));
  }
  return MakeSeedResult(false,TEXT("RollbackCreated.ResidueDetected"),*FString::Printf(TEXT("Authoritative deletion left rollback residue (%s)."),
     *ResidueSummary(OutAbsence)));
 }
	return MakeSeedResult(true, TEXT(""), TEXT(""));
}

TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>
MakeAssetDocumentEditorCutoverRollbackSeed(
	const FAssetDocumentEditorMutationIdentity& Identity,
	FString ExactAssetClassPath)
{
	return MakeShared<FAssetDocumentEditorRollbackSeed>(
		Identity, MoveTemp(ExactAssetClassPath));
}
