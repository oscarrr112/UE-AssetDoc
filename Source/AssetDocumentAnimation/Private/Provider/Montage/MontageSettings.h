// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentMutationPreparation.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

class UAnimMontage;

// Montage settings that need specific UE calls (FAlphaBlend setters, blend
// profiles, root motion, preview base pose, time stretch configuration,
// metadata objects). Plain reflected fields go through the generic Editor
// property batch instead. Rollback is Core's whole-package snapshot.
namespace AssetDocumentMontageSettings
{
inline constexpr const TCHAR* Primitive = TEXT("animation.montage.settings.set.v1");
}

FAssetDocumentPrimitiveDescriptor MakeAnimationMontageSettingsDescriptor();

FAssetDocumentContractValidationResult PrepareAnimationMontageSettingsBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch);

FAssetDocumentPrimitiveOutcome InvokeAnimationMontageSettings(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);

// Full observation of every settings field for the profile snapshot.
bool CaptureAnimationMontageSettings(UAnimMontage& Montage, FAssetDocumentRuntimeValueDto& OutSettings,
 TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
