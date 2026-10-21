// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "AssetDocumentAnimationMutationPreparation.h"

namespace AssetDocumentSequenceReferencesDocumentInputs
{
ASSETDOCUMENTANIMATION_API FAssetDocumentContractValidationResult Prepare(
 const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentAnimationRequestProvider& Owner,
 IAssetDocumentNativeRequestContext& ReadonlyContext, const FGuid& ReadonlyTransaction,
 const FAssetDocumentRuntimeValueDto& DocumentInputs, int32 NameStepIndex, int32 AssetStepIndex,
 EAssetDocumentRuntimeRequestPhase Phase, TConstArrayView<FAssetDocumentMutationIntent> OtherIntents,
 TArray<FAssetDocumentMutationIntent>& OutActualIntents, FAssetDocumentPreparedMutationBatch& OutBatch);
}
