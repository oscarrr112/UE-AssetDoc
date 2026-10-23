// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Runtime/AssetDocumentMutationPreparation.h"
namespace AssetDocumentMontage { struct FPreparedStructuralEdit; }
// Only the request owner binds the actual earlier structural plan, before any capture opens.
FAssetDocumentContractValidationResult PrepareAnimationSkeletonMutationBatchAfterStructure(
 const FAssetDocumentRuntimeRequestDto& Request, const FAssetDocumentMutationIntent& SkeletonIntent,
 TSharedRef<const AssetDocumentMontage::FPreparedStructuralEdit> Structure,
 FAssetDocumentPreparedMutationBatch& OutBatch);
