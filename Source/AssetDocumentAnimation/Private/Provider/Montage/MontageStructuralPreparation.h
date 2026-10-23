// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Provider/Montage/MontageStructuralOperations.h"
#include "Runtime/AssetDocumentMutationPreparation.h"

FAssetDocumentContractValidationResult PrepareAnimationMontageStructuralMutationBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch);

namespace AssetDocumentMontage
{
// Request-local batch construction; no registry or shared preparation entry is installed here.
FAssetDocumentContractValidationResult MakeMontageStructuralPreparedBatch(
 TSharedRef<FPreparedStructuralEdit> State, int32 StepIndex,
 EAssetDocumentRuntimeRequestPhase Phase,
 FAssetDocumentPreparedMutationBatch& OutBatch, FAssetDocumentMutationIntent& OutIntent);
FAssetDocumentRuntimeValueDto FreezeMontageStructuralArguments(const FStructuralIntent& Intent);

class FStructuralCapture final
{
public:
 bool IsOpenFor(const FAssetDocumentMutationToken& Token) const;
 bool IsClosedForRollback() const;
 EAssetDocumentRuntimeRequestPhase GetPhase() const { return Phase; }
private:
 FGuid RequestId, TransactionId;
 uint64 Revision = 0;
 EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 bool bOpened = false, bClosed = false;
 friend FAssetDocumentContractValidationResult MakeMontageStructuralPreparedBatch(
  TSharedRef<FPreparedStructuralEdit>, int32, EAssetDocumentRuntimeRequestPhase,
  FAssetDocumentPreparedMutationBatch&, FAssetDocumentMutationIntent&);
};
}
