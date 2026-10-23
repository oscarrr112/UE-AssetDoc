// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"

class UObject;
class UPackage;

// How a package snapshot treats one object of the package.
enum class EAssetDocumentSnapshotObjectRole : uint8
{
	// Serialized state is captured and restored; new objects are removed and
	// the object set is verified.
	Authored,
	// Regenerated from authored state (generated classes, CDOs, ...). Never
	// captured, restored or removed; a matching rebuilder brings it back in sync.
	Derived,
};

// Provider hook that regenerates derived state after authored objects of a
// package were restored from a snapshot (e.g. recompile a Blueprint).
class ASSETDOCUMENT_API IAssetDocumentDerivedStateRebuilder
{
public:
	virtual ~IAssetDocumentDerivedStateRebuilder() = default;
	virtual FName GetId() const = 0;
	virtual bool Supports(const UObject& PrimaryAsset) const = 0;
	// The primary asset itself is Authored regardless of this result, unless
	// IsPrimaryAssetDerived() says otherwise.
	virtual EAssetDocumentSnapshotObjectRole Classify(const UObject& Object) const;
	// True when the primary asset is itself compiled from authored subobjects (a User
	// Defined Struct is compiled from its editor data): it is then neither captured nor
	// byte-restored, and Rebuild regenerates it.
	virtual bool IsPrimaryAssetDerived() const { return false; }
	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) = 0;
	// Optional: derived state that must survive a restore, captured together with
	// the snapshot (e.g. Blueprint class defaults, which live on the derived CDO and
	// which a recompile copies forward from whatever the current CDO holds).
	virtual TSharedPtr<void> CaptureDerivedState(UObject& PrimaryAsset) const { return nullptr; }
	// Called instead of Rebuild when CaptureDerivedState returned (non-null) state.
	virtual FAssetDocumentContractValidationResult RebuildFromState(UObject& PrimaryAsset, const TSharedPtr<void>& State)
	{
		return Rebuild(PrimaryAsset);
	}
};

// Registration is game-thread only. Returns false for an empty or duplicate id.
ASSETDOCUMENT_API bool RegisterAssetDocumentDerivedStateRebuilder(
	TSharedRef<IAssetDocumentDerivedStateRebuilder> Rebuilder);
ASSETDOCUMENT_API bool UnregisterAssetDocumentDerivedStateRebuilder(FName RebuilderId);

// Whole-package in-memory object snapshot. Restore() returns the package to a
// functionally equivalent pre-capture state (not necessarily the same object
// identities, as with the editor's own Undo):
//   remove new authored objects -> restore identity -> restore bytes ->
//   PostEditUndo (deepest first) -> derived-state rebuilders -> verify object
//   set -> restore dirty flag.
// Objects created by PostEditUndo or a rebuilder (editor-derived subobjects
// rebuilt from the restored state, as the editor's Undo also does) are accepted.
// If verification fails, Restore() fails with PackageSnapshot.RestoreFailed and a
// remedy; it never reloads the package from disk (a reload replaces the objects
// that editor systems, e.g. a running Niagara simulation, still point to).
//
// Objects the editor replaces while the snapshot lives (FCoreUObjectDelegates::OnObjectsReinstanced /
// OnObjectsReplaced:
// a Blueprint compile reinstancing the component templates of the Blueprints that use the
// compiled class) are followed, as the editor's transaction buffer follows them: the restore
// targets the replacement and remaps captured references to the replaced object, so it never
// revives an instance of an outdated (REINST_) class. A package changed only by such replacements
// is unchanged (HasChangedSinceCapture).
//
// The captured objects are kept alive as garbage-collector references, not as
// reference-counted (strong) pointers: editor code rebuilds some subobjects in
// place by creating a new object of the same class and name (NewObject over the
// existing one, e.g. a Niagara Set Parameters node regenerating its function
// script), which the engine refuses while the object is reference-counted. Such an
// object keeps its address and path, so the snapshot restores the captured state
// into the rebuilt object like into any other.
class ASSETDOCUMENT_API FAssetDocumentPackageSnapshot final : public FGCObject
{
public:
	static TUniquePtr<FAssetDocumentPackageSnapshot> Capture(
		UPackage& Package,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

	// Default object roles: UStruct/UClass objects and class default objects
	// (including their subobjects) are Derived, everything else is Authored.
	static EAssetDocumentSnapshotObjectRole ClassifyDefault(const UObject& Object);

	// True when a restore can be functionally complete: the package holds no
	// derived objects, or a registered rebuilder supports its primary asset.
	static bool CanRestorePackage(UPackage& Package);

	virtual ~FAssetDocumentPackageSnapshot() override;
	FAssetDocumentPackageSnapshot(const FAssetDocumentPackageSnapshot&) = delete;
	FAssetDocumentPackageSnapshot& operator=(const FAssetDocumentPackageSnapshot&) = delete;

	// Single use.
	FAssetDocumentContractValidationResult Restore();

	// True when the package's authored objects differ from the capture: an object was added or
	// removed, renamed or re-outered, or its serialized state changed. Read only.
	bool HasChangedSinceCapture() const;

	// Single use, exclusive with Restore(). Writes the captured bytes back (only
	// objects whose state differs), removes new objects and restores identities
	// and the dirty flag, WITHOUT PostEditUndo or rebuilders.
	// For undoing side effects of a probe on packages it must not change: their
	// editor callbacks could repeat the side effect (e.g. a Paper2D atlas rebuilds
	// itself on PostEditChange). Fails unless the object set matches exactly.
	FAssetDocumentContractValidationResult RestoreWithoutEditorCallbacks();

	const FString& GetPackageName() const { return PackageName; }
	int32 NumAuthoredObjects() const { return Objects.Num(); }

	// FGCObject: keeps the package, the primary asset and every captured object
	// (and its original outer) alive, garbage-marked ones included.
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;

#if WITH_DEV_AUTOMATION_TESTS
	// Forces the post-restore verification to fail.
	void ForceVerificationFailureForTests() { bForceVerificationFailure = true; }
#endif

private:
	struct FObjectRecord
	{
		TObjectPtr<UObject> Object;
		TObjectPtr<UObject> OriginalOuter;
		FName OriginalName;
		EObjectFlags OriginalFlags = RF_NoFlags;
		bool bWasRooted = false;
		int32 Depth = 0;
		TArray<uint8> Bytes;
		// Graph nodes: the ids of the captured pins (see RestoreBytes).
		TSet<FGuid> PinIds;
	};

	FAssetDocumentPackageSnapshot() = default;

	EAssetDocumentSnapshotObjectRole Classify(const UObject& Object) const;
	TArray<UObject*> CollectCurrentAuthoredObjects() const;
	void RemoveNewObjects(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
	void RestoreIdentities(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
	void RestoreBytes(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics, bool bEditorCallbacks = true);
	bool BeginRestore(FAssetDocumentContractValidationResult& OutResult);
	void RunRebuilders(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
	bool VerifyObjectSet(
		const TSet<const UObject*>& BeforeCallbacks,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics) const;
	void ReportUnverifiedRestore(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics) const;
	void OnObjectsReplaced(const TMap<UObject*, UObject*>& Replaced);
	// The latest valid replacement of Object (Object itself when it was not replaced).
	UObject* ResolveReplacement(UObject* Object) const;
	// Every replaced object -> its latest valid replacement; and the inverse (each replacement ->
	// the object the capture saw).
	void BuildReplacementMaps(TMap<UObject*, UObject*>& OutLatest, TMap<UObject*, UObject*>& OutCaptured) const;
	// Retargets the records (and the authored set) to the objects that replaced them.
	void FollowReplacements();

	TObjectPtr<UPackage> Package;
	TObjectPtr<UObject> PrimaryAsset;
	FString PackageName;
	TArray<FObjectRecord> Objects;
	TSet<const UObject*> AuthoredObjectSet;
	TArray<TSharedRef<IAssetDocumentDerivedStateRebuilder>> Rebuilders;
	TArray<TSharedPtr<void>> RebuilderStates;
	// Replaced object -> its replacement, for the objects this snapshot follows (Tracked).
	TMap<UObject*, TObjectPtr<UObject>> Replacements;
	TSet<const UObject*> Tracked;
	FDelegateHandle ObjectsReplacedHandle;
	FDelegateHandle ObjectsReinstancedHandle;
	bool bWasDirty = false;
	bool bConsumed = false;
#if WITH_DEV_AUTOMATION_TESTS
	bool bForceVerificationFailure = false;
#endif
};

// Snapshots of the packages an apply may change as a side effect, captured together with the
// primary package's snapshot (by its rebuilder's CaptureDerivedState): the loaded dependents the
// editor updates when a User Defined Type or a Blueprint changes. After a failed apply, Restore()
// brings each one the apply changed back to its captured in-memory state, like the primary package
// (FAssetDocumentPackageSnapshot::Restore: its own rebuilder recompiles a dependent Blueprint). Nothing
// is reloaded from disk: a reload replaces objects that editor systems and the running request still
// point to.
class ASSETDOCUMENT_API FAssetDocumentDependentSnapshots final
{
public:
	struct FRestoreResult
	{
		// Packages the apply had changed, restored.
		TArray<FString> Restored;
		// Packages the apply changed that could not be restored: "<package>: <reason>".
		TArray<FString> NotRestored;
		// Diagnostics of the failed restores.
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	};

	// Captures every package (duplicates and null ignored). A package whose restore could not be
	// complete (FAssetDocumentPackageSnapshot::CanRestorePackage) or whose capture fails is only
	// recorded with its dirty flag. While this runs, IsCapturing() is true: rebuilders capture no
	// dependents of their own for these packages (the caller's list already holds the transitive ones).
	static TSharedRef<FAssetDocumentDependentSnapshots> Capture(TConstArrayView<UPackage*> Packages);
	static bool IsCapturing();

	FAssetDocumentDependentSnapshots() = default;
	// Not copyable (owns the snapshots; an exported class would otherwise instantiate a copy).
	FAssetDocumentDependentSnapshots(const FAssetDocumentDependentSnapshots&) = delete;
	FAssetDocumentDependentSnapshots& operator=(const FAssetDocumentDependentSnapshots&) = delete;

	// Single use. Restores each captured package that changed since the capture; a captured
	// package the apply only marked dirty gets its dirty flag back. An uncaptured package that is
	// dirty now but was not then, and each of Others (dependents found only now, e.g. loaded
	// during the apply) that is dirty now, is reported in NotRestored.
	FRestoreResult Restore(TConstArrayView<UPackage*> Others = {});

	int32 NumCaptured() const { return Captured.Num(); }

private:
	struct FCaptured
	{
		FString PackageName;
		TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot;
	};
	struct FUncaptured
	{
		FString PackageName;
		bool bWasDirty = false;
		FString Reason;
	};
	TArray<FCaptured> Captured;
	TArray<FUncaptured> Uncaptured;
	bool bConsumed = false;
};
