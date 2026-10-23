// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageStructuralRollback.h"

namespace AssetDocumentMontage
{
void PublishMontageNotifyTracks(UAnimMontage& Target, const TArray<FAnimNotifyTrack>& Tracks)
{
 // The editor's Add/Insert/Remove/Rename Notify Track edit AnimNotifyTracks the same way; the caller refreshes the caches.
 Target.AnimNotifyTracks = Tracks;
}
void PublishMontageNotifyEvents(UAnimMontage& Target, const TArray<FAnimNotifyEvent>& Events,
 TConstArrayView<FLinkPlacement> Starts, TConstArrayView<FLinkPlacement> Ends, int32 RetainedPrefix)
{
 check(Events.Num() == Starts.Num() && Events.Num() == Ends.Num() && RetainedPrefix >= 0 && RetainedPrefix <= Events.Num());
 // AD.ANIMATION.MONTAGE.NOTIFIES-PUBLISH.v1: exact full mixed preimage-derived sequence only.
 Target.Notifies = Events;
 // Replacement retains existing rows byte-for-byte; only newly authored rows need link/offset initialization.
 for (int32 Index = RetainedPrefix; Index < Events.Num(); ++Index)
 {
  auto& Event = Target.Notifies[Index];
  SetLinkPlacement(Event, Target, Starts[Index]);
  if (Event.NotifyStateClass)
  {
   SetLinkPlacement(Event.EndLink, Target, Ends[Index]);
   Event.SetDuration(Ends[Index].Time - Starts[Index].Time);
  }
  Event.RefreshTriggerOffset(Target.CalculateOffsetForNotify(Event.GetTime()));
  if (Event.NotifyStateClass) Event.RefreshEndTriggerOffset(Target.CalculateOffsetForNotify(Event.EndLink.GetTime()));
 }
}
}
