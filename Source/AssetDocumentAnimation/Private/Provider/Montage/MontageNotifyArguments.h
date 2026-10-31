// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Animation/AnimLinkableElement.h"
#include "Animation/AnimTypes.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

class UClass;

namespace AssetDocumentMontage
{
// The notify panel's per-notify settings a row gives; an unset one keeps the notify's value (a new notify: the UE default).
struct FNotifyTrigger
{
 TOptional<float> WeightThreshold, Chance;
 TOptional<ENotifyFilterType::Type> FilterType;
 TOptional<int32> FilterLOD;
 TOptional<bool> FilterableByRequest, DedicatedServer, Follower;
 TOptional<EMontageNotifyTickType::Type> TickType;
};
struct FNotifyEdit
{
 // Null for a skeleton notify, which has only its Name.
 UClass* Class = nullptr;
 FName Name;
 bool bState = false;
 // Position of the row in its family (Notifies or NotifyStates), for diagnostics.
 int32 Row = 0;
 float Time = 0.f;
 float Duration = 0.f;
 int32 TrackIndex = 0;
 FAssetDocumentRuntimeValueDto Properties;
 FNotifyTrigger Trigger;
 // Link to the Montage timeline; a None slot is the first slot track.
 EAnimLinkMethod::Type Method = EAnimLinkMethod::Absolute, EndMethod = EAnimLinkMethod::Absolute;
 FName Slot, EndSlot;
};

// Ordered canonical DTO keys:
// event: {notifies:[{time,track_index,object:{class,properties}|null,[name],trigger:{...},link:{method,slot}}]}
// state: {notify_states:[{time,duration,track_index,object:{class,properties},trigger:{...},link:{method,slot,end_method,end_slot}}]}
// name is present exactly when object is null (a skeleton notify); trigger holds any subset of the notify panel's settings.
// notifies.replace may contain both keys in order {notifies: [...], notify_states: [...]}.
// Parsed rows retain their actual family; notify_states.replace accepts only the state root.
// Properties is an opaque Object DTO for the existing Editor property preparation.
// Classes must already be loaded; the caller pins them during preparation and validates
// target-specific time bounds, existing tracks, slots, placement and safe property authority.
// Empty arrays are valid. Failure always leaves OutEdits empty.
FAssetDocumentContractValidationResult ParseMontageNotifyArguments(
 const FAssetDocumentRuntimeValueDto& Value, bool bStates, TArray<FNotifyEdit>& OutEdits);
}
