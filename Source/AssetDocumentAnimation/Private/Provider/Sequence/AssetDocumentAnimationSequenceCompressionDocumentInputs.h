// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AssetDocumentAnimationMutationPreparation.h"

namespace AssetDocumentSequenceCompressionDocumentInputs
{
FAssetDocumentContractValidationResult Prepare(
 const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentAnimationRequestProvider& Owner,
 const FAssetDocumentRuntimeValueDto& DocumentInputs, int32 StepIndex, EAssetDocumentRuntimeRequestPhase Phase,
 TConstArrayView<FAssetDocumentMutationIntent> OtherIntents,
 TArray<FAssetDocumentMutationIntent>& OutActualIntents, FAssetDocumentPreparedMutationBatch& OutBatch);
}
