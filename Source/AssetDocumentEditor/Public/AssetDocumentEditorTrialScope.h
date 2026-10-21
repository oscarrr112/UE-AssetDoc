// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/TopLevelAssetPath.h"

class FAssetDocumentEditorPreflightIsolation;
class UPackage;
class UTransactor;

// Editor state around Validate's trial apply (the Profile's Apply run on a transient copy of the
// target, then discarded; AssetDocumentGenericValidateTrial.h in Integration). Validate is a read:
//   - the editor's undo history is untouched: the stock editor operations the Apply uses record
//     their own transactions (FScopedTransaction); while the scope is open they go to a private,
//     empty transaction buffer that is dropped afterwards (GEditor->Trans is swapped back), so the
//     user's undo and redo entries stay as they were; while swapped out, the editor's own buffer is
//     held strongly (GEditor no longer references it, and the trial may collect garbage);
//   - no other package changes: the same isolation the property preflight uses
//     (FAssetDocumentEditorPreflightIsolation) snapshots any package other
//     than the transient one the edit reaches and restores it in Finish().
class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorTrialScope final
{
public:
	FAssetDocumentEditorTrialScope();
	~FAssetDocumentEditorTrialScope();

	FAssetDocumentEditorTrialScope(const FAssetDocumentEditorTrialScope&) = delete;
	FAssetDocumentEditorTrialScope& operator=(const FAssetDocumentEditorTrialScope&) = delete;

	// Restores touched packages and the undo buffer. False (with diagnostics) when a package
	// could not be restored. Single use; the destructor calls it when the caller did not.
	bool Finish(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

	// The trial target (a transient copy of the target, or a new transient asset; a staged asset for a
	// staged trial class, below) stands in for the target: primitives whose checks depend on the asset's
	// identity treat it as the target. IsTrialTarget: the object is a trial target of the open scope;
	// FindTrialSource: the asset it copies, for self-reference checks (null for a new asset).
	static bool IsTrialTarget(const UObject& Object);
	static const UObject* FindTrialSource(const UObject& Object);
	// Called by the trial target primitive (editor.scratch.trial_target.v1) for each target it issues.
	static void RegisterTrialTarget(const UObject& Target, const UObject* Source);

	// An asset class whose trial target neither the generic transient duplicate nor the generic
	// factory scratch can make (a Control Rig Blueprint: a Blueprint copy needs its own generated
	// class, and the factory makes Blueprints Public / Standalone) registers how its module makes
	// one, the way the editor's Duplicate / New asset would, in the transient package. Source is the
	// asset to copy, or null for a new asset (FactoryContexts: the create context objects, as the
	// generic factory takes them). The maker returns the trial target, which the primitive checks
	// (transient package, RF_Transient, not Public / Standalone) and registers, or null with
	// OutMessage. Game thread only. Keyed by class path (exactly that class), so a module's ShutdownModule unregisters
	// without touching the class: on a normal editor exit the UObject exit purge runs before ShutdownModule.
	using FTrialTargetMaker = TFunction<UObject*(UClass& AssetClass, UObject* Source, TConstArrayView<UObject*> FactoryContexts,
		FString& OutMessage)>;
	static void RegisterTrialTargetMaker(const FTopLevelAssetPath& AssetClassPath, FTrialTargetMaker Maker);
	static void UnregisterTrialTargetMaker(const FTopLevelAssetPath& AssetClassPath);
	static const FTrialTargetMaker* FindTrialTargetMaker(const UClass& AssetClass);

	// An asset class whose engine code needs its trial target to be a package asset (UObject::IsAsset(): the
	// MetaSound document builder asserts it) registers here. Its trial target is then made the generic way (the
	// editor factory, or the editor's Duplicate) as a Public / Standalone asset in a staged package of its own under
	// /Temp/AssetDocument/ValidateTrial/: never saved, never added to the asset registry, not guarded by the
	// isolation. Finish() calls Discard on each staged asset first (to drop what the class registered outside the
	// package, e.g. a MetaSound's frontend registration and builder), then discards the package. Keyed by class path
	// (that class and its subclasses); game thread only; ShutdownModule unregisters.
	using FStagedTrialDiscard = TFunction<void(UObject& StagedAsset)>;
	static void RegisterStagedTrialClass(const FTopLevelAssetPath& AssetClassPath, FStagedTrialDiscard Discard);
	static void UnregisterStagedTrialClass(const FTopLevelAssetPath& AssetClassPath);
	static bool IsStagedTrialClass(const UClass& AssetClass);
	// A new staged package for one trial target of AssetClass, owned by the open scope (null outside a scope).
	static UPackage* CreateStagedTrialPackage(const UClass& AssetClass);
	// The package is a staged trial package of the open scope.
	static bool IsStagedTrialPackage(const UPackage& Package);

private:
	TUniquePtr<FAssetDocumentEditorPreflightIsolation> Isolation;
	// UTransactor objects (held as UObject so this header needs no editor include).
	TStrongObjectPtr<UObject> SavedTransactor;
	TStrongObjectPtr<UObject> TrialTransactor;
	bool bFinished = false;
};
