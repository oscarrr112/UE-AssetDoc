// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"

class FProperty;
class UAnimSequenceBase;
class UAnimSequence;
class UClass;
class UObject;
struct FAnimNotifyEvent;

// Raw edits for animation authoring surfaces that have no exported setter. Each
// leaves the asset as the sequence editor (notify panel, details panel) leaves
// it. No inverse is written: the caller's contribution is
// covered by Core's package snapshot or the created-asset rollback (§3).
namespace AssetDocumentAnimationAuthoringRaw
{
// SAnimNotifyTrack::CreateNewNotify: link, trigger offset, track, guid.
ASSETDOCUMENTANIMATION_API void PlaceNotify(UAnimSequenceBase& Sequence, FAnimNotifyEvent& Event, float Time, int32 TrackIndex, const FGuid& Guid);
// Notify state end link and end trigger offset after SetDuration.
ASSETDOCUMENTANIMATION_API void PlaceNotifyStateEnd(UAnimSequenceBase& Sequence, FAnimNotifyEvent& Event, float Duration);
// SAnimNotifyTrack::MakeBlueprintNotifyName applied to the notify class name.
ASSETDOCUMENTANIMATION_API FName MakeNotifyName(const UClass& NotifyClass);
// Replaces the whole notify array / track list (the panel edits them in place).
ASSETDOCUMENTANIMATION_API void SetNotifies(UAnimSequenceBase& Sequence, TArray<FAnimNotifyEvent>&& Notifies);
ASSETDOCUMENTANIMATION_API void SetNotifyTracks(UAnimSequenceBase& Sequence, const TArray<FName>& TrackNames);
// SAnimNotifyTrack::CreateNewSyncMarkerAtCursor for a whole marker list.
struct FSyncMarkerRow { FName Name; float Time = 0.f; int32 TrackIndex = 0; FGuid Guid; };
ASSETDOCUMENTANIMATION_API void SetAuthoredSyncMarkers(UAnimSequence& Sequence, const TArray<FSyncMarkerRow>& Rows);
// Details-panel object picker on a hard (non-instanced) object/class reference:
// Modify, PreEditChange, set, PostEditChangeProperty(ValueSet).
ASSETDOCUMENTANIMATION_API void SetObjectReference(UObject& Owner, FProperty& Property, UObject* Value);
// Details-panel "+" on an Instanced object array: outer = asset, transactional.
ASSETDOCUMENTANIMATION_API UObject* NewInstancedSubobject(UObject& Outer, UClass& Class);
}
