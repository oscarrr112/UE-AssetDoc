// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Sequence/AnimationAssetAuthoringRawEdits.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentAnimationAuthoringRaw
{
void PlaceNotify(UAnimSequenceBase& Sequence, FAnimNotifyEvent& Event, float Time, int32 TrackIndex, const FGuid& Guid)
{
#if WITH_EDITORONLY_DATA
 Event.Guid = Guid;
#endif
 Event.Link(&Sequence, Time);
 Event.TriggerTimeOffset = GetTriggerTimeOffsetForType(Sequence.CalculateOffsetForNotify(Time));
 Event.TrackIndex = TrackIndex;
}

void PlaceNotifyStateEnd(UAnimSequenceBase& Sequence, FAnimNotifyEvent& Event, float Duration)
{
 Event.SetDuration(Duration);
 Event.EndLink.Link(&Sequence, Event.EndLink.GetTime());
 Event.EndTriggerTimeOffset = GetTriggerTimeOffsetForType(Sequence.CalculateOffsetForNotify(Event.GetTime() + Duration));
}

FName MakeNotifyName(const UClass& NotifyClass)
{
 FString Name = NotifyClass.GetName();
 Name.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);
 Name = Name.Replace(TEXT("AnimNotify_"), TEXT(""), ESearchCase::CaseSensitive);
 Name = Name.Replace(TEXT("AnimNotifyState_"), TEXT(""), ESearchCase::CaseSensitive);
 return FName(*Name);
}

void SetNotifies(UAnimSequenceBase& Sequence, TArray<FAnimNotifyEvent>&& Notifies)
{
 Sequence.Notifies = MoveTemp(Notifies);
}

void SetNotifyTracks(UAnimSequenceBase& Sequence, const TArray<FName>& TrackNames)
{
#if WITH_EDITORONLY_DATA
 Sequence.AnimNotifyTracks.Reset(TrackNames.Num());
 for (const FName& Name : TrackNames) Sequence.AnimNotifyTracks.Add(FAnimNotifyTrack(Name, FLinearColor::White));
#endif
}

void SetAuthoredSyncMarkers(UAnimSequence& Sequence, const TArray<FSyncMarkerRow>& Rows)
{
 Sequence.AuthoredSyncMarkers.Reset(Rows.Num());
 for (const FSyncMarkerRow& Row : Rows)
 {
  FAnimSyncMarker& Marker = Sequence.AuthoredSyncMarkers.AddDefaulted_GetRef();
  Marker.MarkerName = Row.Name;
  Marker.Time = Row.Time;
#if WITH_EDITORONLY_DATA
  Marker.TrackIndex = Row.TrackIndex;
  Marker.Guid = Row.Guid.IsValid() ? Row.Guid : FGuid::NewGuid();
#endif
 }
 Sequence.SortSyncMarkers();
 Sequence.RefreshSyncMarkerDataFromAuthored();
}

void SetObjectReference(UObject& Owner, FProperty& Property, UObject* Value)
{
 FObjectPropertyBase* Reference = CastFieldChecked<FObjectPropertyBase>(&Property);
 Owner.Modify();
 Owner.PreEditChange(&Property);
 Reference->SetObjectPropertyValue_InContainer(&Owner, Value);
 FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
 Owner.PostEditChangeProperty(Event);
}

UObject* NewInstancedSubobject(UObject& Outer, UClass& Class)
{
 return NewObject<UObject>(&Outer, &Class, NAME_None, RF_Transactional);
}
}
