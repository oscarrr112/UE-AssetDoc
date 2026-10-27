// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

class FAssetDocumentIngressQueue;

class FAssetDocumentEditorSync
{
public:
	// An editor write of sidecar files: while it runs, changes of the files are not taken as edits (file watcher);
	// for a few seconds after it, only the content it left is not.
	class ASSETDOCUMENT_API FScopedSidecarWrite
	{
	public:
		// What the scope hides after it ends.
		enum class ERecord : uint8
		{
			// The content its writes left (NoteSidecarWritten); for a file it changed otherwise (a move or a
			// delete), the file's content when the scope ends.
			FileAtEnd,
			// Only the content its writes left; nothing when it wrote nothing. A change made by someone else while
			// the scope runs is then not taken for the editor's.
			OwnWrites,
		};

		explicit FScopedSidecarWrite(const TArray<FString>& InFilePaths);
		explicit FScopedSidecarWrite(const FString& InFilePath);
		// With ExpectedDigest (DigestSidecarFile form; empty: no file), writes in the scope through
		// FAssetDocumentSidecar replace the file only while it still has that content, which then becomes the
		// content each write leaves.
		FScopedSidecarWrite(const FString& InFilePath, ERecord InRecord, const TOptional<FString>& InExpectedDigest = TOptional<FString>());
		~FScopedSidecarWrite();

		FScopedSidecarWrite(const FScopedSidecarWrite&) = delete;
		FScopedSidecarWrite& operator=(const FScopedSidecarWrite&) = delete;

	private:
		TArray<FString> FilePaths;
		ERecord Record = ERecord::FileAtEnd;
	};

	// For writers of sidecar bytes: false (with OutError) when a write scope of the file expects other content than
	// the file has now, i.e. it changed after the content the write is based on was read. Nothing is written then.
	static bool CheckSidecarUnchanged(const FString& FilePath, FString& OutError);
	// Records the bytes an editor write left in the file (Data null: it removed the file), inside a write scope of
	// the file; outside one nothing is recorded.
	static void NoteSidecarWritten(const FString& FilePath, const uint8* Data, int64 Num);
	// A count of recorded writes, and the content digest the last recorded write of the file left after the count
	// was Serial (unset when there was none).
	static uint64 GetSidecarWriteSerial();
	static TOptional<FString> FindSidecarWriteSince(const FString& FilePath, uint64 Serial);

	FAssetDocumentEditorSync() = default;
	explicit FAssetDocumentEditorSync(
		const TSharedRef<FAssetDocumentIngressQueue>& InIngress);
	~FAssetDocumentEditorSync();

	void Register();
	void Unregister();

	static bool IsSidecarWriteSuppressed(const FString& FilePath);
	static FString InferDuplicateSourceObjectPath(const FString& NewObjectPath);
	FGuid GetIngressCoordinatorId() const;

	static bool SyncSidecarForRenameOrMove(const FString& OldObjectPath, const FString& NewObjectPath);
	static bool SyncSidecarForDuplicate(const FString& SourceObjectPath, const FString& NewObjectPath);
	static bool ArchiveSidecarForDeletedAsset(const FString& ObjectPath, FString* OutArchivePath = nullptr);

private:
	void HandleAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath);
	void HandleAssetAdded(const FAssetData& AssetData);
	void HandleAssetRemoved(const FAssetData& AssetData);

	FDelegateHandle AssetRenamedHandle;
	FDelegateHandle AssetAddedHandle;
	FDelegateHandle AssetRemovedHandle;
	TSharedPtr<FAssetDocumentIngressQueue> Ingress;
	bool bRegistered = false;
};
