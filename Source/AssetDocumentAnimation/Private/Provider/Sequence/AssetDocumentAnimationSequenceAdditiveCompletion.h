// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "Runtime/AssetDocumentMutationPreparation.h"

ASSETDOCUMENTANIMATION_API FAssetDocumentPrimitiveDescriptor MakeAnimationSequenceAdditiveCompletionDescriptor();
ASSETDOCUMENTANIMATION_API bool IsSequenceAdditiveCompletionAdmissionShape(const FAssetDocumentPrimitiveDescriptor& Descriptor);
FAssetDocumentPrimitiveOutcome InvokeAnimationSequenceAdditiveCompletion(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);
FAssetDocumentContractValidationResult PrepareAnimationSequenceAdditiveCompletionBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch);
