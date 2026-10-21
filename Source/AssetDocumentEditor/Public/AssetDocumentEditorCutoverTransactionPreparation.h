// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

struct FAssetDocumentEditorCutoverMutationIntent
{
	FGuid RequestId;
	uint64 RuntimeRevision = 0;
	FString CanonicalObjectPath;
	FString ExactAssetClassPath;
	FString FactoryId;
	bool bCreateByFactory = false;
	bool bWriteAdmittedProperties = false;
};

class IAssetDocumentEditorCutoverMutationCapture
{
public:
	virtual ~IAssetDocumentEditorCutoverMutationCapture() = default;
};

class IAssetDocumentEditorCutoverRollbackSeed
{
public:
	virtual ~IAssetDocumentEditorCutoverRollbackSeed() = default;
	virtual FStringView GetRollbackComponentId() const = 0;
	virtual TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>
	OpenMutationCapture(const FGuid& TransactionId) = 0;
	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) = 0;
};

struct FAssetDocumentEditorCutoverTransactionPreparation
{
	bool bPrepared = false;
	TArray<FAssetDocumentMutationFootprint> CompleteFootprint;
	TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>> RollbackSeeds;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

ASSETDOCUMENTEDITOR_API
FAssetDocumentEditorCutoverTransactionPreparation
PrepareAssetDocumentEditorCutoverTransaction(
	const FAssetDocumentEditorCutoverMutationIntent& Intent);
