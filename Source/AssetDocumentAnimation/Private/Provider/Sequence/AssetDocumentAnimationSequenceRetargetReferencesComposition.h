// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "Runtime/AssetDocumentMutationPreparation.h"

// One request-local parent for the existing two explicit native Intents. This
// does not decode Body.References or register another mutation surface.
FAssetDocumentContractValidationResult PrepareAnimationSequenceRetargetReferencesBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch);
bool HasAnimationSequenceRetargetReferencesCapture();
FAssetDocumentPrimitiveOutcome InvokeAnimationSequenceRetargetReferences(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);
