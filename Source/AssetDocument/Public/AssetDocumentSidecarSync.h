// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentSidecarSyncState.h"
#include "AssetDocumentTypes.h"
#include "Dom/JsonObject.h"

class FAssetDocumentService;

// What started a sync pass.
enum class EAssetDocumentSidecarSyncTrigger : uint8
{
	// The package was saved in the editor.
	Save,
	// The file watcher saw the sidecar change.
	SidecarChanged,
	// apply_asset_document_file.
	Explicit,
	// Editor start, a rescan, or a status check of named assets.
	Reconcile,
	// Generating missing mirrors (background run or the materialize action).
	Materialize,
};

// A conflict resolution: which side wins for the named regions (all conflicted regions when empty).
enum class EAssetDocumentSidecarSyncAccept : uint8
{
	None,
	Sidecar,
	Asset,
};

struct ASSETDOCUMENT_API FAssetDocumentSidecarSyncRequest
{
	FString PackageName;
	EAssetDocumentSidecarSyncTrigger Trigger = EAssetDocumentSidecarSyncTrigger::Reconcile;
	EAssetDocumentSidecarSyncAccept Accept = EAssetDocumentSidecarSyncAccept::None;
	TArray<FString> AcceptRegions;
	// Only report what a pass would do (status): nothing is written, applied or posted.
	bool bDryRun = false;
};

// The outcome of one sync pass. State is one of:
//   in_sync        nothing to do (or only the stamp was refreshed);
//   wrote          the sidecar was written from the asset (a new mirror, or regions the asset changed);
//   applied        the sidecar was applied to the asset, then rewritten as its mirror;
//   conflict       both sides changed differently since the last sync (ConflictRegions, ConflictFile);
//   unavailable    one side's evidence could not be read (Code, Message);
//   failed         the apply of the sidecar failed (Apply);
//   asset_missing  the sidecar has sync state but its asset is gone;
//   deferred       the asset has unsaved changes; its next save syncs it;
//   skipped        no mirror was written (Reason: extract_failed, empty, too_large);
//   excluded       the asset is outside the mirroring scope (Reason);
//   absent         neither the asset nor the sidecar exists;
//   changed        the sidecar changed while the pass ran (after it was read): nothing more was written, the new
//                  content is kept, and another pass of the asset is queued;
// and, for a dry run, would_apply / would_write.
struct ASSETDOCUMENT_API FAssetDocumentSidecarSyncOutcome
{
	FString PackageName;
	FString SidecarPath;
	FString State;
	FString Code;
	FString Reason;
	FString Message;
	TArray<FString> ConflictRegions;
	FString ConflictFile;
	TArray<FString> SidecarChangedRegions;
	TArray<FString> AssetChangedRegions;
	TOptional<FAssetDocumentResult> Apply;
	bool bWroteSidecar = false;

	bool IsProblem() const;
	TSharedRef<FJsonObject> ToJson() const;
};

// The sync pass of one asset between its .uasset and its mirrored sidecar (docs/architecture/
// asset-document-sidecar-sync.md §3). Per region (/Class, /Properties, each /Body key) it compares the current
// sidecar and asset evidence with _meta.sync:
//   - unchanged on both sides: nothing;
//   - only the sidecar changed: the sidecar is applied (after the regions the asset changed were written into it);
//   - only the asset changed: the region is rewritten from the asset;
//   - both changed to the same content: converged, stamped;
//   - both changed differently: conflict; nothing is written except <AssetName>.assetdoc.asset.json (the asset's
//     mirror), and a sidecar_sync_conflict notice is posted.
// Without trustworthy state a region equal on both sides is stamped, one absent in the sidecar is taken from the
// asset, and a different one is an initialization conflict. A completed pass leaves the sidecar byte-identical to
// the asset's mirror (diff-only extract plus _meta). Every write of the sidecar, and the apply, first checks that the
// file still has the content the pass read (or last wrote); if it does not, the pass stops ("changed"). Game thread
// only.
class ASSETDOCUMENT_API FAssetDocumentSidecarSync
{
public:
	static constexpr const TCHAR* ConflictNotice = TEXT("sidecar_sync_conflict");
	static constexpr const TCHAR* FailedNotice = TEXT("sidecar_sync_failed");

	explicit FAssetDocumentSidecarSync(FAssetDocumentService& InService);
	// Saves the local index when it changed.
	~FAssetDocumentSidecarSync();

	FAssetDocumentSidecarSyncOutcome Sync(const FAssetDocumentSidecarSyncRequest& Request);

	// The result apply_asset_document_file returns for a sync pass it ran.
	static FAssetDocumentResult ToApplyResult(const FAssetDocumentSidecarSyncOutcome& Outcome);

	// Whether a pass for this package is running (its own saves and writes are not new changes).
	static bool IsSyncing(const FString& PackageName);

	FAssetDocumentSidecarSyncIndex& GetIndex() { return Index; }
	// Records the outcome in the local index (also for outcomes the runner decides without a pass).
	void RecordIndex(const FAssetDocumentSidecarSyncOutcome& Outcome);

	// Test hooks: extracts made by passes since the editor started, and a step after which the next pass stops as if
	// the editor had been closed ("merge": after the asset's regions were written into the sidecar; "apply": after the
	// apply, before the mirror was written).
	static int32 GetExtractCountForTest();
	static void StopAfterStepForTest(const TCHAR* Step);
	// Called by passes at "before_apply", "after_apply" and "before_write" (each sidecar write), to change files
	// while a pass runs. Empty to remove.
	static void SetStepHookForTest(TFunction<void(const FString& Step)> Hook);

private:
	FAssetDocumentService& Service;
	FAssetDocumentSidecarSyncIndex Index;
	// After a pass applied the sidecar and stopped because the file changed before its mirror was written: the state
	// that mirror would have recorded (keyed by MakeKey). The next pass uses it, while the package is unchanged, in
	// place of the file's _meta.sync, so the new content counts as a sidecar change since that apply.
	TMap<FString, FAssetDocumentSidecarSyncMeta> PendingBaselines;
};
