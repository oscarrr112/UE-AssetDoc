// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Runtime/AssetDocumentMutationPreparation.h"

namespace AssetDocumentSequenceCompressionDefaultInputs
{
// Native preparation selection only. This is not a Body/null/reset schema.
enum class EKind : uint8 { Bone, Curve, FrameStripping };

// Family-private readonly inputs shared by default-only and document binding.
// Empty selection is permitted here only for explicit document supplied values;
// the original default-only Prepare still requires a nonempty selection.
struct FInputs;
FAssetDocumentContractValidationResult Resolve(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<EKind> Selection,
 int32 StepIndex, EAssetDocumentRuntimeRequestPhase Phase,
 FAssetDocumentRuntimeValueDto& OutFields, TSharedPtr<FInputs>& OutInputs);
FAssetDocumentContractValidationResult RetainReference(
 const TSharedPtr<FInputs>& Inputs, EKind Kind, UObject* Object, UClass* DeclaredClass = nullptr);
FAssetDocumentContractValidationResult GuardPreparedBatch(
 const TSharedPtr<FInputs>& Inputs, int32 StepIndex, FAssetDocumentPreparedMutationBatch& Batch);

#if WITH_DEV_AUTOMATION_TESTS
// Only Integration dev tests import this existing private producer.
ASSETDOCUMENTANIMATION_API
#endif
FAssetDocumentContractValidationResult Prepare(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<EKind> Selection,
 int32 StepIndex, EAssetDocumentRuntimeRequestPhase Phase,
 FAssetDocumentMutationIntent& OutSuppliedIntent, FAssetDocumentPreparedMutationBatch& OutBatch);
}
