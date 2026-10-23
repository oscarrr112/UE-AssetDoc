// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "Runtime/AssetDocumentMutationPreparation.h"

class UAnimSequence;
class USkeletalMesh;
// The same fixed public publication chain serves standalone and paired forward
// and inverse calls after their native identity/preimage guards.
void PublishAnimationSequenceRetargetSourceAsset(UAnimSequence& Sequence, USkeletalMesh* Mesh);
bool HasAnimationSequenceRetargetSourceAssetCapture();

FAssetDocumentPrimitiveDescriptor MakeAnimationSequenceRetargetSourceAssetDescriptor();
bool IsSequenceRetargetSourceAssetAdmissionShape(const FAssetDocumentPrimitiveDescriptor& Descriptor);
FAssetDocumentPrimitiveOutcome InvokeAnimationSequenceRetargetSourceAsset(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);
FAssetDocumentContractValidationResult PrepareAnimationSequenceRetargetSourceAssetBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch);
