// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "Runtime/AssetDocumentMutationPreparation.h"

FAssetDocumentPrimitiveDescriptor MakeAnimationFloatCurveReplaceDescriptor();
bool IsFloatCurveReplaceAdmissionShape(const FAssetDocumentPrimitiveDescriptor& Descriptor);
FAssetDocumentPrimitiveOutcome InvokeAnimationFloatCurveReplace(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);
FAssetDocumentContractValidationResult PrepareAnimationFloatCurveMutationBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch);
