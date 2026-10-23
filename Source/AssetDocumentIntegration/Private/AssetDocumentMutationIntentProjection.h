// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Runtime/AssetDocumentMutationPreparation.h"
#include "Runtime/AssetDocumentPrimitiveRegistry.h"

// Request-local, value-only projection. Provider preparation owns intent semantics.
struct FAssetDocumentMutationIntentProjection
{
	FGuid RequestId;
	uint64 RuntimeRevision = 0;
	FString ExactAssetClassPath;
	FString TargetAssetPath;
	FString SemanticHash;
	TArray<FAssetDocumentMutationIntent> Steps;
 // SnapshotCoveredMutationV1: no declared steps; the handler's mutating
 // invocations are covered by Core's package snapshot or the factory rollback.
 bool bSnapshotCovered = false;
};

FAssetDocumentContractValidationResult ParseAssetDocumentMutationIntentProjection(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentRuntimeResultDto& Preflight,
	FStringView ValidatedSemanticHash,
	const IAssetDocumentPrimitiveRegistry& Registry,
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Requirements,
	FAssetDocumentMutationIntentProjection& OutProjection);
