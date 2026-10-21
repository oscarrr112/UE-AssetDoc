// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once

#include "Runtime/AssetDocumentRuntimeTypes.h"

class UClass;

namespace AssetDocumentMontage
{
struct FNotifyEdit
{
 UClass* Class = nullptr;
 bool bState = false;
 float Time = 0.f;
 float Duration = 0.f;
 int32 TrackIndex = 0;
 FAssetDocumentRuntimeValueDto Properties;
};

// Ordered canonical DTO keys:
// event: {notifies:[{time,track_index,object:{class,properties}}]}
// state: {notify_states:[{time,duration,track_index,object:{class,properties}}]}
// M3 may contain both keys in order {notifies: [...], notify_states: [...]}.
// Parsed rows retain their actual family; M4 accepts only the state root.
// Properties is an opaque Object DTO for the existing Editor property preparation.
// Classes must already be loaded; the caller pins them during preparation and validates
// target-specific time bounds, existing tracks, placement and safe property authority.
// Empty arrays are valid. Failure always leaves OutEdits empty.
FAssetDocumentContractValidationResult ParseMontageNotifyArguments(
 const FAssetDocumentRuntimeValueDto& Value, bool bStates, TArray<FNotifyEdit>& OutEdits);
}
