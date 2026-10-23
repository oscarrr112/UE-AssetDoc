// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "IDirectoryWatcher.h"

class FAssetDocumentIngressQueue;

struct FAssetDocumentWatchedFileState
{
	int64 FileSize = INDEX_NONE;
	FDateTime Timestamp;

	bool operator==(const FAssetDocumentWatchedFileState& Other) const
	{
		return FileSize == Other.FileSize && Timestamp == Other.Timestamp;
	}

	bool operator!=(const FAssetDocumentWatchedFileState& Other) const
	{
		return !(*this == Other);
	}
};

class ASSETDOCUMENT_API FAssetDocumentFileWatcher
{
public:
	explicit FAssetDocumentFileWatcher(
		const TSharedRef<FAssetDocumentIngressQueue>& InIngress);
	~FAssetDocumentFileWatcher();

	void Register();
	void Unregister();

	static bool IsAssetDocumentSidecarPath(const FString& FilePath);
	static void CollectAssetDocumentSidecarsUnderDirectory(const FString& Directory, TArray<FString>& OutFilePaths);
	static bool TryReadFileState(const FString& FilePath, FAssetDocumentWatchedFileState& OutState);
	static bool IsStableFileState(const FString& FilePath, const FAssetDocumentWatchedFileState& PreviousState);
	static bool ShouldSkipAutoApplyForDirtyPackage(const FString& Target, FString& OutWarning);
	FGuid GetIngressCoordinatorId() const;

private:
	void HandleDirectoryChanged(const TArray<FFileChangeData>& FileChanges);

	TSharedPtr<FAssetDocumentIngressQueue> Ingress;
	FString WatchDirectory;
	FDelegateHandle WatchHandle;
	bool bRegistered = false;
};
