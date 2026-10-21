// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class UObject;
class UPackage;

// What keeps the editor's asset deletion from deleting an asset, as its Delete Assets dialog lists it.
struct FAssetDocumentDeleteBlocker
{
	// "package": a package that references the asset on disk (asset registry referencer);
	// "object": an object outside the asset's package that references it in memory (Referencer is its full name);
	// "subobject": a subobject of the asset kept alive by its own flags;
	// "registry": the asset registry is still discovering assets.
	FString Kind;
	FString Referencer;
	// The referencing properties of an "object" blocker.
	TArray<FString> Properties;
	// The referencer is an asset of the same delete batch that is not deleted (it is refused itself).
	bool bRefusedInBatch = false;

	ASSETDOCUMENTSERVER_API FString Describe() const;
};

// The result of DeleteAssetAsEditor.
struct FAssetDocumentDeleteOutcome
{
	bool bDeleted = false;
	// The editor's Undo history was cleared: only it still referenced the asset (as the Delete Assets dialog does).
	bool bUndoCleared = false;
	// Editor caches in the transient package whose references into the asset's package were released.
	TArray<FString> Released;
	// Why the deletion was refused (empty when it deleted the asset).
	TArray<FAssetDocumentDeleteBlocker> Blockers;
};

namespace AssetDocumentAssetDelete
{
// Why the editor's asset deletion would refuse Asset: packages that reference it on disk, objects that reference it in
// memory (with the referencing properties), subobjects kept alive by their own flags (FAssetDeleteModel, as the Delete
// Assets dialog).
ASSETDOCUMENTSERVER_API TArray<FAssetDocumentDeleteBlocker> FindDeleteBlockers(UObject& Asset);

// "; "-joined descriptions of at most Limit blockers ("N more" after them).
ASSETDOCUMENTSERVER_API FString DescribeDeleteBlockers(const TArray<FAssetDocumentDeleteBlocker>& Blockers, int32 Limit = 12);

// Objects in the transient package that still reference Asset (or an object inside Package): editor-side caches,
// never content (e.g. a Niagara script's rooted compilation copy). Their references into Package are nulled, as a
// force delete does for every referencer; references from other packages and from the undo buffer are left to the
// deletion's own checks. Returns the released referencers.
ASSETDOCUMENTSERVER_API TArray<FString> ReleaseTransientReferencers(UObject& Asset, UPackage& Package);

// Deletes a batch as the editor's Delete Assets dialog does for a multi-selection, without forcing it: references
// between assets of the batch do not block (they are deleted together, a reference cycle included); an asset
// referenced from outside the batch is refused with its blockers, and so is every batch asset it references (it
// stays, so they stay referenced). The rest is deleted together, with the same preparation as DeleteAssetAsEditor.
// One outcome per entry of Assets, in order (a repeated asset gets the same outcome). Game thread only.
ASSETDOCUMENTSERVER_API TArray<FAssetDocumentDeleteOutcome> DeleteAssetsAsEditor(const TArray<UObject*>& Assets);

// Deletes an asset as the editor's Delete Assets dialog does without forcing it: systems drop their bookkeeping
// references (OnAssetsPreDelete), a garbage collection runs, transient editor caches are released, an animation
// asset's data controller is unbound from its model (the controller is a transient helper the asset owns), and when
// only the Undo history still references the asset it is cleared (ObjectTools::DeleteSingleObject); then
// ObjectTools::DeleteObjects. A refusal restores the controller and lists the blockers. Game thread only.
ASSETDOCUMENTSERVER_API FAssetDocumentDeleteOutcome DeleteAssetAsEditor(UObject& Asset);
}
