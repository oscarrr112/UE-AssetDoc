// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGenericRuntimeRoute.h"
#include "AssetDocumentTypes.h"

class FAssetDocumentIngressQueue;
class FAssetDocumentSidecarApplyLedger;
class FAssetDocumentSidecarSync;
class FAssetDocumentSidecarSyncRunner;
struct FAssetDocumentRuntimeResultDto;
struct FAssetDocumentIngressEvent;

class ASSETDOCUMENT_API FAssetDocumentService
{
public:
	FAssetDocumentService();
	~FAssetDocumentService();

	void BindGenericRuntimeRoute(
		const TSharedRef<IAssetDocumentGenericRuntimeRoute>& Route);
	void UnbindGenericRuntimeRoute(
		const TSharedRef<IAssetDocumentGenericRuntimeRoute>& Route);

	// Applies a sidecar file. A failed apply by the file watcher (bTriggeredByWatcher) is kept as a pending notice of
	// kind WatcherApplyFailedNotice (AssetDocumentPendingNotices.h) until the file is applied again by an explicit apply
	// or a successful watcher apply.
	// With sidecar mirroring on, a saved apply of the sidecar of an asset in the mirroring scope runs the sync pass
	// instead (AssetDocumentSidecarSync.h): the file is applied only when the asset did not change differently since
	// the last sync, and is then rewritten as the asset's mirror.
	FAssetDocumentResult ApplyFile(const FAssetDocumentApplyFileRequest& Request);
	static constexpr const TCHAR* WatcherApplyFailedNotice = TEXT("sidecar_watcher_apply_failed");
	FAssetDocumentResult Inspect(const FAssetDocumentInspectRequest& Request) const;
	FAssetDocumentResult InspectProfile(const FAssetDocumentProfileRequest& Request) const;
	FAssetDocumentResult CreateTemplate(const FAssetDocumentTemplateRequest& Request) const;
	FAssetDocumentResult GetSchema() const;
	FAssetDocumentResult Extract(const FAssetDocumentExtractRequest& Request) const;
	FAssetDocumentResult Validate(const FAssetDocumentValidateRequest& Request) const;
	FAssetDocumentResult Diff(const FAssetDocumentDiffRequest& Request) const;
	// Read-only discovery (Asset Registry and reflection); query keys in Discovery/AssetDocumentDiscovery.h.
	FAssetDocumentResult FindAssets(const TMap<FString, FString>& Query) const;
	FAssetDocumentResult ListTypes(const TMap<FString, FString>& Query) const;
	// The Content Browser's Duplicate of any readable asset into an absent /Game path, saved
	// (AssetDocumentDuplicate.h).
	FAssetDocumentResult Duplicate(const FAssetDocumentDuplicateRequest& Request);

	// Sidecar mirroring (AssetDocumentSidecarSync.h): the sync pass of one asset.
	FAssetDocumentSidecarSync& GetSidecarSync() { return *SidecarSync; }
	// The queue of passes nobody waits on (saves); registered by the module in the interactive editor.
	FAssetDocumentSidecarSyncRunner& GetSidecarSyncRunner() { return *SidecarSyncRunner; }
	// Whether the Python runtime that sync passes need is bound.
	bool IsSidecarSyncAvailable() const { return GenericRuntimeRoute.IsValid(); }
	// sync_asset_document: Action status | accept_sidecar | accept_asset | materialize, for Targets (assets or /Game
	// folders) and, for accept_*, Regions (JSON Pointers; empty: the conflicted ones).
	FAssetDocumentResult SyncSidecars(const FString& Action, const TArray<FString>& Targets, const TArray<FString>& Regions);
	// The Python sync evidence (asset_document/sync_evidence.py) for Options {"Mode", "Text", ...}.
	FAssetDocumentRuntimeResultDto DispatchSidecarSync(const TSharedRef<FJsonObject>& Options) const;
	// The diff-only extract of a saved asset as sidecar bytes (CanonicalBytes), for its mirror.
	FAssetDocumentRuntimeResultDto ExtractForMirror(const FString& PackageName) const;
	// After the sync pass rewrote an applied sidecar as the mirror (ContentHash: the content it wrote): an explicit
	// apply of that content is the same apply.
	void NoteSidecarRewrite(const FString& SidecarPath, const FString& ContentHash);

	// Handles one file watcher or asset registry event as the ingress queue does, at once (tests).
	FAssetDocumentResult HandleIngressEventForTest(const FAssetDocumentIngressEvent& Event) { return HandleIngressEvent(Event); }

private:
	friend class FAssetDocumentIngressQueue;
	FAssetDocumentResult ApplyFileOnce(const FAssetDocumentApplyFileRequest& Request);
	// The sync pass in place of the apply (see ApplyFile); unset when the apply is not one for the pass.
	TOptional<FAssetDocumentResult> ApplyFileThroughSidecarSync(const FAssetDocumentApplyFileRequest& Request);
	FAssetDocumentResult ApplyParsedDocument(
		const FAssetDocumentApplyRequest& Request);
	FAssetDocumentResult HandleIngressEvent(
		const FAssetDocumentIngressEvent& Event);
	// With mirroring on, queues the sync pass of a mirrored asset's sidecar in the runner and returns the package;
	// unset when the sidecar is not one the runner syncs.
	TOptional<FString> QueueMirroredSidecar(const FString& SidecarPath);
	FAssetDocumentResult InspectReadOnlyAsset(
		const FAssetDocumentInspectRequest& Request,
		const FString& Path) const;
	// Document.StaleBase: a document whose BaseRevision is not the asset's revision any more (see
	// AssetDocumentBaseRevisionField), unless the document would change nothing. Any change counts, also one made by
	// an apply of another document with the same BaseRevision (two clients editing the same extract): a document's own
	// follow-up apply carries the base_revision its previous apply returned. Unset when the document has no
	// BaseRevision or its target does not exist.
	TOptional<FAssetDocumentResult> CheckBaseRevision(
		const TSharedPtr<FJsonObject>& Document,
		const FString& NormalizedFilePath) const;
	// After every successful apply (create included): payload base_revision is the asset's new revision, the BaseRevision
	// of the document's next apply. For a document with BaseRevision the editor also remembers which base it came from
	// (for the stale-base message).
	void RememberAppliedRevision(const FJsonObject& Document, FAssetDocumentResult& Result) const;
	FAssetDocumentResult SummarizeValidApply(
		FAssetDocumentResult Result,
		const TSharedPtr<FJsonObject>& Document,
		const FString& NormalizedFilePath) const;

	TWeakPtr<IAssetDocumentGenericRuntimeRoute> GenericRuntimeRoute;
	// One application per sidecar content shared by the file watcher and explicit applies.
	TUniquePtr<FAssetDocumentSidecarApplyLedger> SidecarApplyLedger;
	TUniquePtr<FAssetDocumentSidecarSync> SidecarSync;
	TUniquePtr<FAssetDocumentSidecarSyncRunner> SidecarSyncRunner;
};
