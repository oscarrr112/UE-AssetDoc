// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentSyncCoordinator.h"

struct FAssetDocumentSyncStateKey
{
	FString CanonicalAssetPath;
	FString RegionId;
};

struct FAssetDocumentTypedSyncStateRecord
{
	FAssetDocumentSyncEvidencePair LastDurable;
	FGuid RequestId;
	FGuid TransactionId;
	FString JournalId;
};

class IAssetDocumentTypedSyncStateStore
{
public:
	virtual ~IAssetDocumentTypedSyncStateStore() = default;
	virtual FAssetDocumentContractValidationResult Load(
		const FAssetDocumentSyncStateKey& Key,
		TOptional<FAssetDocumentTypedSyncStateRecord>& OutRecord,
		TArray<uint8>& OutEncodedBytes,
		bool& bOutExisted) = 0;
	virtual FAssetDocumentContractValidationResult Store(
		const FAssetDocumentSyncStateKey& Key,
		const FAssetDocumentTypedSyncStateRecord& Record) = 0;
	virtual FAssetDocumentContractValidationResult Restore(
		const FAssetDocumentSyncStateKey& Key,
		TArrayView64<const uint8> EncodedPreimage,
		bool bExisted) = 0;
};

class ASSETDOCUMENT_API FAssetDocumentJournaledTypedSyncStateStore final : public IAssetDocumentTypedSyncStateStore
{
public:
	explicit FAssetDocumentJournaledTypedSyncStateStore(FString InStateRoot);
	virtual FAssetDocumentContractValidationResult Load(
		const FAssetDocumentSyncStateKey& Key,
		TOptional<FAssetDocumentTypedSyncStateRecord>& OutRecord,
		TArray<uint8>& OutEncodedBytes,
		bool& bOutExisted) override;
	virtual FAssetDocumentContractValidationResult Store(
		const FAssetDocumentSyncStateKey& Key,
		const FAssetDocumentTypedSyncStateRecord& Record) override;
	virtual FAssetDocumentContractValidationResult Restore(
		const FAssetDocumentSyncStateKey& Key,
		TArrayView64<const uint8> EncodedPreimage,
		bool bExisted) override;

private:
	FString GetStatePath(const FAssetDocumentSyncStateKey& Key) const;
	FString StateRoot;
};

class FAssetDocumentSyncCoordinatorRuntime final : public IAssetDocumentSyncCoordinator
{
public:
	FAssetDocumentSyncCoordinatorRuntime(
		FAssetDocumentSyncStateKey InKey,
		IAssetDocumentTypedSyncStateStore& InStore);
	virtual FAssetDocumentSyncDecision Compare(
		const FAssetDocumentSyncComparisonInput& Input) const override;
	virtual FAssetDocumentContractValidationResult AdvanceAfterDurableCompletion(
		const FAssetDocumentSyncEvidencePair& Installed,
		const FAssetDocumentDurableCompletionReceipt& Receipt) override;

private:
	FAssetDocumentSyncStateKey Key;
	IAssetDocumentTypedSyncStateStore& Store;
};
