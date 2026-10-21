// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "Runtime/AssetDocumentMutationPreparation.h"

bool HasAnimationSequenceRetargetSourceNameCapture();

FAssetDocumentPrimitiveDescriptor MakeAnimationSequenceRetargetSourceNameDescriptor();
bool IsSequenceRetargetSourceNameAdmissionShape(const FAssetDocumentPrimitiveDescriptor& Descriptor);
FAssetDocumentPrimitiveOutcome InvokeAnimationSequenceRetargetSourceName(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);
FAssetDocumentContractValidationResult PrepareAnimationSequenceRetargetSourceNameBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch);
