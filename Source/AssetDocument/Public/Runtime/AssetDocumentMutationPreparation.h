// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once

#include "AssetDocumentCoreRuntimeComposition.h"
#include "Runtime/AssetDocumentPrimitiveRegistry.h"

struct FAssetDocumentMutationIntent
{
 int32 StepIndex = INDEX_NONE;
 FAssetDocumentPrimitiveRequirement Requirement;
 EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 FString InputSchemaId;
 FAssetDocumentRuntimeValueDto Intent;
};

struct FAssetDocumentPreparedMutationContribution
{
 int32 StepIndex = INDEX_NONE;
 TArray<FAssetDocumentMutationFootprint> RawFootprint;
 TArray<FString> OwnedSurfaceIds;
 TUniqueFunction<FAssetDocumentContractValidationResult(
  const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&)> ValidateInvocation;
 TSharedPtr<IAssetDocumentRollbackAction> InitialAction;
 // No typed inverse: the whole target is restored by Core's package snapshot
 // (existing target) or removed by the created-asset rollback (created target),
 // so provider enlistments during this step are absorbed.
 bool bCoveredByPackageSnapshot = false;
};

// Request-owned after publication. Shared action state permits only transaction
// binding, activation and terminal consumption; it is not a mutable DTO alias.
struct FAssetDocumentPreparedMutationBatch
{
 FAssetDocumentPreparedMutationBatch() = default;
 FAssetDocumentPreparedMutationBatch(const FAssetDocumentPreparedMutationBatch&) = delete;
 FAssetDocumentPreparedMutationBatch& operator=(const FAssetDocumentPreparedMutationBatch&) = delete;
 FAssetDocumentPreparedMutationBatch(FAssetDocumentPreparedMutationBatch&&) = default;
 FAssetDocumentPreparedMutationBatch& operator=(FAssetDocumentPreparedMutationBatch&&) = default;
 TArray<FAssetDocumentPreparedMutationContribution> Contributions;
 TSharedPtr<IAssetDocumentPostRollbackFinalizer> PackageFinalizer;
 FAssetDocumentCoreMutationCaptureOpener OpenMutationCapture;
};
