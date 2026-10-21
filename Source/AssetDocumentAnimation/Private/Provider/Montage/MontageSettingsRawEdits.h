// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "CoreMinimal.h"

class UAnimMontage;
class UAnimMetaData;
class UClass;

// Raw edits for Montage settings that have no exported setter. Each leaves the
// Montage as the Montage details panel (or the engine's own save-time step)
// leaves it. No inverse is written: the caller's contribution is
// covered by Core's whole-package snapshot, which restores the Montage on rollback.
namespace AssetDocumentMontageSettingsRaw
{
// FTimeStretchCurve::SamplingRate / CurveValueMinPrecision are private
// EditAnywhere fields; the details panel writes them through reflection.
ASSETDOCUMENTANIMATION_API bool GetTimeStretchConfig(const UAnimMontage& Montage, float& OutSamplingRate, float& OutPrecision);
ASSETDOCUMENTANIMATION_API bool SetTimeStretchConfig(UAnimMontage& Montage, const float* SamplingRate, const float* Precision);
// Bakes the time-stretch curve as saving the Montage does: the named curve's markers, else a reset curve.
ASSETDOCUMENTANIMATION_API void BakeTimeStretchCurve(UAnimMontage& Montage);
// Routes the details-panel SyncGroup change notification: CollectMarkers when a
// sync group is set, common frame rate update and child propagation.
ASSETDOCUMENTANIMATION_API void RefreshDerivedSyncState(UAnimMontage& Montage);
// Allocates an instanced metadata object exactly like the details panel "+"
// on an Instanced UAnimMetaData array: outer = Montage, RF_Transactional.
ASSETDOCUMENTANIMATION_API UAnimMetaData* NewInstancedMetaData(UAnimMontage& Montage, UClass& Class);
}
