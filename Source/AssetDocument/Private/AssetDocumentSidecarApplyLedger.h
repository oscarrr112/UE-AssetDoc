// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentTypes.h"
#include "IO/IoHash.h"

// One application per sidecar content (direction doc §4.5).
//
// Writing a sidecar under Content makes the editor's file watcher apply it (after a
// 500 ms debounce) and an AI client then usually calls apply_asset_document_file for the
// same file. Both run ApplyFile on the game thread, one after the other. The ledger keeps,
// per sidecar file, the content hash that was applied, the target package state right
// after that run and its result. A second run for the same content while the target is
// unchanged since then does not apply again:
//   - an explicit apply after a watcher run that succeeded returns that recorded result
//     (marked applied_by = "file_watcher");
//   - a watcher run after an explicit apply (any result), or after a successful watcher
//     run, is skipped.
// Different content, a changed target (saved, edited, deleted) or an explicit apply after
// an explicit apply always runs normally.
struct FAssetDocumentSidecarTargetState
{
	bool bOnDisk = false;
	int64 FileSize = INDEX_NONE;
	FDateTime FileTimestamp;
	bool bLoaded = false;
	bool bDirty = false;
	FIoHash SavedHash;

	bool operator==(const FAssetDocumentSidecarTargetState& Other) const
	{
		return bOnDisk == Other.bOnDisk && FileSize == Other.FileSize && FileTimestamp == Other.FileTimestamp
			&& bLoaded == Other.bLoaded && bDirty == Other.bDirty && SavedHash == Other.SavedHash;
	}
};

class FAssetDocumentSidecarApplyLedger
{
public:
	struct FRecord
	{
		TArray<FString> ContentHashes;
		FString PackageName;
		FAssetDocumentSidecarTargetState TargetState;
		FAssetDocumentResult Result;
		bool bByWatcher = false;
		FDateTime AppliedAtUtc;
	};

	// SHA-1 of the file bytes (hex); empty when the file cannot be read.
	static FString HashFile(const FString& FilePath);
	static FAssetDocumentSidecarTargetState CaptureTargetState(const FString& PackageName);

	// The record for this sidecar when its content hash matches and the target package
	// is in the state the recorded run left it in.
	const FRecord* FindMatching(const FString& SidecarPath, const FString& ContentHash, const FString& PackageName) const;
	void Record(const FString& SidecarPath, TArray<FString> ContentHashes, const FString& PackageName,
		const FAssetDocumentResult& Result, bool bByWatcher);

	// What an explicit apply returns for a run the watcher already made.
	static FAssetDocumentResult MakeReplayedWatcherResult(const FRecord& Record);
	// What a watcher run returns when the content was already applied.
	static FAssetDocumentResult MakeSkippedWatcherResult(const FRecord& Record, const FString& SidecarPath);

private:
	TMap<FString, FRecord> Records;
};
