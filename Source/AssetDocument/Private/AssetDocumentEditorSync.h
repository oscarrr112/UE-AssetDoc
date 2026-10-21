// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

class FAssetDocumentIngressQueue;

class FAssetDocumentEditorSync
{
public:
	class ASSETDOCUMENT_API FScopedSidecarWrite
	{
	public:
		explicit FScopedSidecarWrite(const TArray<FString>& InFilePaths);
		explicit FScopedSidecarWrite(const FString& InFilePath);
		~FScopedSidecarWrite();

		FScopedSidecarWrite(const FScopedSidecarWrite&) = delete;
		FScopedSidecarWrite& operator=(const FScopedSidecarWrite&) = delete;

	private:
		TArray<FString> FilePaths;
	};

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
