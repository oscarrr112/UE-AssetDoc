// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Runtime/AssetDocumentAtomicTransaction.h"

struct FAssetDocumentBlackboardFootprintProjectionStep
{
	int32 Index = INDEX_NONE;
	FString PrimitiveId;
	TArray<FAssetDocumentMutationFootprint> RawFootprint;
	TArray<FAssetDocumentMutationFootprint> QualifiedFootprint;
};

// This is deliberately module-private state.  It is the validated value-only
// bridge between Python Preflight and the request-local atomic adapter.
struct FAssetDocumentBlackboardFootprintProjection
{
	FGuid RequestId;
	uint64 RuntimeRevision = 0;
	FString ExactAssetClassPath;
	FString TargetAssetPath;
	FString Operation;
	FString SemanticHash;
	TArray<FAssetDocumentBlackboardFootprintProjectionStep> Steps;
	TArray<FAssetDocumentMutationFootprint> QualifiedFootprint;

	const TArray<FAssetDocumentMutationFootprint>& GetQualifiedFootprint() const
	{
		return QualifiedFootprint;
	}
};

FAssetDocumentContractValidationResult ParseAssetDocumentBlackboardFootprintProjection(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentRuntimeResultDto& Preflight,
	FAssetDocumentBlackboardFootprintProjection& OutProjection);
