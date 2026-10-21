// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGenericRuntimeRoute.h"
#include "AssetDocumentTypes.h"

class FAssetDocumentIngressQueue;
class FAssetDocumentSidecarApplyLedger;
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

private:
	friend class FAssetDocumentIngressQueue;
	FAssetDocumentResult ApplyFileOnce(const FAssetDocumentApplyFileRequest& Request);
	FAssetDocumentResult ApplyParsedDocument(
		const FAssetDocumentApplyRequest& Request);
	FAssetDocumentResult HandleIngressEvent(
		const FAssetDocumentIngressEvent& Event);
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
};
