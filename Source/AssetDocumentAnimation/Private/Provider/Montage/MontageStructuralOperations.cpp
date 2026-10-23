// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageStructuralOperations.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Provider/Montage/MontageStructuralPreparation.h"
#include "Provider/Montage/MontageManagedNotifyIdentity.h"
#include "AssetDocumentEditorNativeState.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimMetaData.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimComposite.h"
#include "UObject/Package.h"

namespace AssetDocumentMontage
{
namespace
{
int32 FindSlot(const TArray<FSlotAnimationTrack>& Slots, FName Name)
{
 return Slots.IndexOfByPredicate([Name](const FSlotAnimationTrack& Slot) { return Slot.SlotName == Name; });
}
bool ValidMethod(EAnimLinkMethod::Type Method)
{
 return Method == EAnimLinkMethod::Absolute || Method == EAnimLinkMethod::Relative || Method == EAnimLinkMethod::Proportional;
}
const TCHAR* MontageLinkMethodName(EAnimLinkMethod::Type Method)
{
 return Method == EAnimLinkMethod::Relative ? TEXT("Relative (1)") : Method == EAnimLinkMethod::Proportional ? TEXT("Proportional (2)") : TEXT("Absolute (0)");
}
// Retained (unauthored) sections and notifies follow the Montage editor's link model: a link remembers its slot and
// its segment index (FAnimLinkableElement); editing segment i in place keeps the link on segment i (Relative and
// Proportional links move with it, Absolute ones keep their time); a link on a removed segment has nothing to follow.
// What names the retained row for the message ("section 'Default'", "notify at 0.2 s on track 0").
FAssetDocumentContractValidationResult ResolveRetainedLink(const FAnimLinkableElement& Old,
 const FPreparedStructuralEdit& State, const FStructuralIntent& Intent, FLinkPlacement& Out, const FString& What)
{
 Out.Time = Old.GetTime(); Out.Method = Old.GetLinkMethod(); Out.Slot = Old.GetSlotIndex();
 if (!ValidMethod(Out.Method) || !FMath::IsFinite(Out.Time))
  return Result(false, TEXT("AnimationProvider.Montage.InvalidIntent"), FString::Printf(
   TEXT("The Montage's %s has an unsupported link (method %d, time %g); author it in the document."), *What, int32(Out.Method), Out.Time));
 if (!State.OldSlots.IsValidIndex(Out.Slot))
  return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), FString::Printf(
   TEXT("The Montage's %s links to slot track %d, which it does not have; author it in the document."), *What, Out.Slot));
 const FName OldSlotName = State.OldSlots[Out.Slot].SlotName;
 if (State.bReplaceSlots)
 {
  Out.Slot = FindSlot(State.DesiredSlots, OldSlotName);
  if (Out.Slot == INDEX_NONE)
   return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), FString::Printf(
    TEXT("Retained link references a removed slot: the Montage's %s (kept: the document does not list it) is on slot '%s'. Author CompositeSections (and Notifies / NotifyStates) on the new slots in the same document (a new Montage's Default section is on DefaultSlot)."),
    *What, *OldSlotName.ToString()));
  const int32 OldSegment = Old.GetSegmentIndex();
  if (OldSegment != INDEX_NONE)
  {
   const auto& OldSegments = State.OldSlots[Old.GetSlotIndex()].AnimTrack.AnimSegments;
   if (!OldSegments.IsValidIndex(OldSegment)) return Result(false, TEXT("AnimationProvider.Montage.PreparedStateDrift"));
   int32 FoundSlot = INDEX_NONE, FoundSegment = INDEX_NONE;
   for (int32 S = 0; S < Intent.Slots->Num(); ++S)
    for (int32 N = 0; N < (*Intent.Slots)[S].Segments.Num(); ++N)
    {
     const auto& Candidate = (*Intent.Slots)[S].Segments[N];
     if (Candidate.PreviousSlot == OldSlotName && Candidate.PreviousSegment == OldSegment)
     {
      if (FoundSlot != INDEX_NONE) return Result(false, TEXT("AnimationProvider.Montage.SegmentCorrespondence"), FString::Printf(
       TEXT("Two document segments claim segment %d of slot '%s', which the Montage's %s follows."), OldSegment, *OldSlotName.ToString(), *What));
      FoundSlot = S; FoundSegment = N;
     }
    }
   if (FoundSlot == INDEX_NONE)
   {
    // An absolute link keeps its time whatever segment lies there (FAnimLinkableElement::Update relinks it).
    if (Out.Method != EAnimLinkMethod::Absolute)
     return Result(false, TEXT("AnimationProvider.Montage.SegmentCorrespondence"), FString::Printf(
      TEXT("The Montage's %s (kept: the document does not list it) has link method %s on segment %d of slot '%s', which the document removes (segment i of a slot is segment i of the Montage). List CompositeSections / Notifies / NotifyStates in the same document."),
      *What, MontageLinkMethodName(Out.Method), OldSegment, *OldSlotName.ToString()));
   }
   else
   {
    Out.Slot = FoundSlot;
    const auto& Segment = State.DesiredSlots[FoundSlot].AnimTrack.AnimSegments[FoundSegment];
    if (Out.Method == EAnimLinkMethod::Relative) Out.Time = Segment.StartPos + Old.GetTime(EAnimLinkMethod::Relative);
    if (Out.Method == EAnimLinkMethod::Proportional) Out.Time = Segment.StartPos + Old.GetTime(EAnimLinkMethod::Proportional) * Segment.GetLength();
    // Do not silently select a different segment in an overlap or after a trim.
    if (Out.Method != EAnimLinkMethod::Absolute
     && State.DesiredSlots[FoundSlot].AnimTrack.GetSegmentIndexAtTime(Out.Time) != FoundSegment)
     return Result(false, TEXT("AnimationProvider.Montage.SegmentCorrespondence"), FString::Printf(
      TEXT("The Montage's %s (kept: the document does not list it, link method %s) would move to %g s, off its segment %d of slot '%s'. List CompositeSections / Notifies / NotifyStates in the same document."),
      *What, MontageLinkMethodName(Out.Method), Out.Time, FoundSegment, *OldSlotName.ToString()));
   }
  }
 }
 if (Out.Method != EAnimLinkMethod::Absolute && (!State.DesiredSlots.IsValidIndex(Out.Slot)
  || State.DesiredSlots[Out.Slot].AnimTrack.GetSegmentIndexAtTime(Out.Time) == INDEX_NONE))
  return Result(false, TEXT("AnimationProvider.Montage.SegmentCorrespondence"), FString::Printf(
   TEXT("The Montage's %s (link method %s) at %g s lies on no segment; list it in the document."), *What, MontageLinkMethodName(Out.Method), Out.Time));
 if (!FMath::IsFinite(Out.Time) || Out.Time < 0.f || Out.Time > State.DesiredLength)
  return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), FString::Printf(
   TEXT("The Montage's %s (kept: the document does not list it) at %g s is after the new Montage length %g s; list it in the document (or make the segments longer)."),
   *What, Out.Time, State.DesiredLength));
 return Result(true);
}
FAssetDocumentContractValidationResult MontageSectionFailure(int32 Index, const FString& Message)
{
 auto Out = Result(false, TEXT("AnimationProvider.Montage.InvalidIntent"), Message);
 Out.Diagnostics[0].Path = FString::Printf(TEXT("/Body/CompositeSections/%d"), Index);
 return Out;
}
FAssetDocumentContractValidationResult PrepareSections(FPreparedStructuralEdit& State, const FStructuralIntent& Intent)
{
 if (Intent.Sections.IsSet())
 {
  TSet<FName> Names;
  for (int32 Index = 0; Index < Intent.Sections->Num(); ++Index)
  {
   const auto& Row = (*Intent.Sections)[Index];
   if (Row.Name.IsNone() || Names.Contains(Row.Name))
    return MontageSectionFailure(Index, FString::Printf(TEXT("CompositeSections[%d] needs a unique, non-empty Name ('%s')."), Index, *Row.Name.ToString()));
   if (!FMath::IsFinite(Row.Time) || Row.Time < 0.f || Row.Time > State.DesiredLength)
    return MontageSectionFailure(Index, FString::Printf(
     TEXT("CompositeSections[%d] '%s' at LinkableTime %g s is after the end of the Montage (%g s, the length its SlotAnimTracks give): move it to at most %g s or make the segments longer."),
     Index, *Row.Name.ToString(), Row.Time, State.DesiredLength, State.DesiredLength));
   if (!ValidMethod(Row.Method))
    return MontageSectionFailure(Index, FString::Printf(TEXT("CompositeSections[%d] '%s' LinkMethod must be 0, 1 or 2."), Index, *Row.Name.ToString()));
   Names.Add(Row.Name);
  }
  float PreviousTime = -1.f;
  for (int32 Index = 0; Index < Intent.Sections->Num(); ++Index)
  {
   const auto& Row = (*Intent.Sections)[Index];
   // UE derives section lengths from the next row; negative intervals cannot play correctly.
   if (!Row.Next.IsNone() && !Names.Contains(Row.Next))
    return MontageSectionFailure(Index, FString::Printf(TEXT("CompositeSections[%d] '%s' NextSectionName '%s' names no section."), Index, *Row.Name.ToString(), *Row.Next.ToString()));
   if (Row.Time < PreviousTime)
    return MontageSectionFailure(Index, FString::Printf(
     TEXT("CompositeSections[%d] '%s' at %g s comes before the previous section (%g s): list sections in time order (the editor keeps them sorted)."),
     Index, *Row.Name.ToString(), Row.Time, PreviousTime));
   PreviousTime = Row.Time;
   const int32 Slot = Row.Slot.IsNone() ? 0 : FindSlot(State.DesiredSlots, Row.Slot);
   if (!State.DesiredSlots.IsValidIndex(Slot))
   {
    auto Missing = Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), FString::Printf(
     TEXT("CompositeSections[%d] '%s' is on slot '%s', which SlotAnimTracks does not list."), Index, *Row.Name.ToString(), *Row.Slot.ToString()));
    Missing.Diagnostics[0].Path = FString::Printf(TEXT("/Body/CompositeSections/%d/SlotName"), Index);
    return Missing;
   }
   if (Row.Method != EAnimLinkMethod::Absolute && State.DesiredSlots[Slot].AnimTrack.GetSegmentIndexAtTime(Row.Time) == INDEX_NONE)
    return MontageSectionFailure(Index, FString::Printf(
     TEXT("CompositeSections[%d] '%s' (LinkMethod %s) at %g s lies on no segment of slot '%s'."),
     Index, *Row.Name.ToString(), MontageLinkMethodName(Row.Method), Row.Time, *State.DesiredSlots[Slot].SlotName.ToString()));
   const int32 Existing = State.OldSections.IndexOfByPredicate([&](const FCompositeSection& S) { return S.SectionName == Row.Name; });
   FCompositeSection Section = BuildMontageSection(Existing == INDEX_NONE ? nullptr : &State.OldSections[Existing], Row.Name, Row.Next);
   State.DesiredSections.Add(MoveTemp(Section));
   State.SectionLinks.Add({Row.Time, Slot, Row.Method});
  }
 }
 else
 {
  State.DesiredSections = State.OldSections;
  float PreviousTime = -1.f;
  for (const auto& Section : State.OldSections)
  {
   FLinkPlacement Placement;
   auto Valid = ResolveRetainedLink(Section, State, Intent, Placement, FString::Printf(TEXT("section '%s'"), *Section.SectionName.ToString()));
   if (!Valid.bValid) return Valid;
   if (Placement.Time < PreviousTime)
    return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), FString::Printf(
     TEXT("The new segments would move the Montage's section '%s' (kept: the document does not list CompositeSections) before the previous section; list CompositeSections in the same document."),
     *Section.SectionName.ToString()));
   PreviousTime = Placement.Time;
   State.SectionLinks.Add(Placement);
  }
 }
 return Result(true);
}
FAssetDocumentContractValidationResult PrepareRetainedNotifies(FPreparedStructuralEdit& State, const FStructuralIntent& Intent)
{
 State.DesiredNotifies.Reset();
 for (const auto& Event : State.OldNotifies)
 {
  if ((EnumHasAnyFlags(Intent.ReplaceManagedFamilies, EManagedNotifyReplacement::Events)
    && IsManagedMontageNotifyEvent(*State.Target, Event, false))
   || (EnumHasAnyFlags(Intent.ReplaceManagedFamilies, EManagedNotifyReplacement::States)
    && IsManagedMontageNotifyEvent(*State.Target, Event, true))) continue;
  FLinkPlacement Begin, End;
  const FString What = FString::Printf(TEXT("%s at %g s on notify track %d"), Event.NotifyStateClass ? TEXT("notify state") : TEXT("notify"),
   Event.GetTime(), Event.TrackIndex);
  auto Valid = ResolveRetainedLink(Event, State, Intent, Begin, What);
  if (!Valid.bValid) return Valid;
  End = Begin;
  if (Event.NotifyStateClass)
  {
   Valid = ResolveRetainedLink(Event.EndLink, State, Intent, End, What + TEXT(" (its end)"));
   if (!Valid.bValid) return Valid;
   if (End.Time <= Begin.Time) return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), FString::Printf(
    TEXT("The new segments would make the Montage's %s (kept: the document does not list it) end before it starts; list it in the document."), *What));
  }
  if (!State.Target->AnimNotifyTracks.IsValidIndex(Event.TrackIndex))
   return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), FString::Printf(
    TEXT("The Montage's %s is on a notify track it does not have; list it in the document."), *What));
  if ((Event.Notify && !Event.Notify->CanBePlaced(State.Target.Get()))
   || (Event.NotifyStateClass && !Event.NotifyStateClass->CanBePlaced(State.Target.Get()))) return Result(false);
  State.DesiredNotifies.Add(Event);
  State.NotifyStarts.Add(Begin); State.NotifyEnds.Add(End);
 }
 // RefreshCacheData relocates nearly coincident starts on one track. Reject that dependency before mutation.
 // Overlapping state intervals with distinct starts remain supported.
 for (int32 I = 0; I < State.DesiredNotifies.Num(); ++I)
  for (int32 J = 0; J < I; ++J)
  {
   if (State.DesiredNotifies[I].TrackIndex != State.DesiredNotifies[J].TrackIndex) continue;
   const float A = State.NotifyStarts[I].Time, C = State.NotifyStarts[J].Time;
   if (FMath::IsNearlyEqual(A, C))
    return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), TEXT("Stock refresh would relocate coincident notify starts; track authoring is not owned here."));
  }
 return Result(true);
}
FAssetDocumentContractValidationResult ValidateSync(const FPreparedStructuralEdit& State)
{
 const bool OldValid = State.OldSlots.IsValidIndex(State.SyncSlot), NewValid = State.DesiredSlots.IsValidIndex(State.SyncSlot);
 // Without a SyncGroup the montage takes part in no marker sync and SyncSlotIndex only names a track index (the
 // Montage editor's slot dropdown changes that track freely); e.g. a new montage whose first track is not DefaultSlot.
 if (!State.SyncGroup.IsNone()
  && (OldValid != NewValid || (OldValid && State.OldSlots[State.SyncSlot].SlotName != State.DesiredSlots[State.SyncSlot].SlotName)))
  return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), TEXT("SyncSlotIndex requires an explicit Sync caller before changing its source slot."));
 // The stock sync-marker collector asserts for negative-rate marked segments.
 if (!State.SyncGroup.IsNone() && NewValid)
  for (const auto& Segment : State.DesiredSlots[State.SyncSlot].AnimTrack.AnimSegments)
   if (Segment.GetValidPlayRate() < 0.f)
    return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), TEXT("Negative-rate marker-sync completion needs separate stock behavior evidence."));
 return Result(true);
}
FAssetDocumentContractValidationResult ValidateDurableShape(const FPreparedStructuralEdit& State)
{
 // UE 5.7.4 PostLoad requires contiguous segment starts. ValidateSegmentTimes
 // is public but not ENGINE_API; use only its minimal read-only shape predicate,
 // never a copied repair routine or a call that mutates the live target.
 for (int32 S = 0; S < State.DesiredSlots.Num(); ++S)
 {
  const auto& Slot = State.DesiredSlots[S];
  for (int32 I = 0; I < Slot.AnimTrack.AnimSegments.Num(); ++I)
  {
   const auto& A = Slot.AnimTrack.AnimSegments[I];
   const auto* Animation = A.GetAnimReference().Get();
   auto Valid = ValidateMontageSegmentStart(Slot.AnimTrack.AnimSegments, S, I);
   if (!Valid.bValid) return Valid;
   if (!IsValid(Animation) || A.AnimEndTime > Animation->GetPlayLength())
    return Result(false, TEXT("AnimationProvider.Montage.InvalidSegment"), FString::Printf(
     TEXT("SlotAnimTracks[%d].Segments[%d]: a reload would clamp its AnimEndTime %g to the animation length."), S, I, A.AnimEndTime));
  }
 }
 // PostLoad relinks sections and both notify endpoints with default slot index 0.
 // Unlinked other slots may still be freely reordered.
 for (const auto* Links : {&State.SectionLinks, &State.NotifyStarts, &State.NotifyEnds})
  for (const auto& Link : *Links)
   if (Link.Slot != 0)
    return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), TEXT("Stock reload would rebind the requested section or notify endpoint to slot zero."));
 return Result(true);
}
bool LeaseMatches(const IAssetDocumentVerifiedHandleLease& Lease, const FPreparedStructuralEdit& State,
 const IAssetDocumentAtomicTransaction& Transaction, EAssetDocumentHandleAccess Access)
{
 const auto& H = Lease.GetHandle();
 return H.RequestId == State.RequestId && H.TransactionId == Transaction.GetTransactionId()
  && H.RuntimeRevision == State.RuntimeRevision && H.Kind == EAssetDocumentHandleKind::Asset && H.Access == Access;
}
FAssetDocumentContractValidationResult ApplyNativeImpl(TSharedRef<FPreparedStructuralEdit> State,
 IAssetDocumentAtomicTransaction& Transaction, uint8 Failure)
{
 const auto& Token = Transaction.GetMutationToken();
 if (!IsStructuralContext() || State->bApplied || Transaction.GetState() != EAssetDocumentAtomicState::Mutating
  || !Token.TransactionId.IsValid() || Token.TransactionId != Transaction.GetTransactionId()
  || Token.RequestId != State->RequestId || Token.RuntimeRevision != State->RuntimeRevision)
  return Result(false, TEXT("AnimationProvider.Montage.InvalidContext"));
 if (State->ReplaceManagedFamilies != EManagedNotifyReplacement::None)
  return Result(false, TEXT("AnimationProvider.Montage.InvalidContext"), TEXT("Managed replacement projection requires the full notify composite."));
 if (!BaselineCurrent(*State)) return Result(false, TEXT("AnimationProvider.Montage.PreparedStateDrift"));
 if (!State->bChanged) { State->bApplied = true; return Result(true); }
 State->bApplied = true;
 auto Valid = Transaction.EnlistFootprint(GetMontageStructuralFootprint(*State));
 if (!Valid.bValid) return Valid;
 Valid = ApplyPreparedStructurePrefixNoEnlist(State, Transaction, Failure);
 if (!Valid.bValid) return Valid;
 auto& Target = *State->Target.Get();
 // Reinstall the complete retained timeline after the controller's resize notifications.
 PublishMontageNotifyEvents(Target, State->DesiredNotifies, State->NotifyStarts, State->NotifyEnds);
 if (Failure == 4) return Result(false, TEXT("AnimationProvider.Montage.InjectedFailure"));
 Valid = CompleteMontageStructure(Target);
 if (!Valid.bValid) return Valid;
 if (Failure == 5) return Result(false, TEXT("AnimationProvider.Montage.InjectedFailure"));
 Valid = ReadbackPreparedStructure(*State);
 if (!Valid.bValid) return Valid;
 if (Target.Notifies.Num() != State->DesiredNotifies.Num()) return Result(false, TEXT("AnimationProvider.Montage.ReadbackMismatch"));
 for (int32 I = 0; I < Target.Notifies.Num(); ++I)
 {
  const auto& Event = Target.Notifies[I];
  // Sorting by stock refresh is acceptable only if it preserves this authored ordered sequence.
  if (Event.Notify != State->DesiredNotifies[I].Notify || Event.NotifyStateClass != State->DesiredNotifies[I].NotifyStateClass
   || Event.NotifyName != State->DesiredNotifies[I].NotifyName || Event.TrackIndex != State->DesiredNotifies[I].TrackIndex
   || !FMath::IsNearlyEqual(Event.GetTime(), State->NotifyStarts[I].Time)
   || (Event.NotifyStateClass && !FMath::IsNearlyEqual(Event.EndLink.GetTime(), State->NotifyEnds[I].Time)))
   return Result(false, TEXT("AnimationProvider.Montage.ReadbackMismatch"));
 }
 return Result(true);
}
}

FAssetDocumentContractValidationResult ApplyPreparedStructurePrefixNoEnlist(
 TSharedRef<FPreparedStructuralEdit> State, IAssetDocumentAtomicTransaction& Transaction, uint8 Failure)
{
 const auto& Token = Transaction.GetMutationToken();
 if (!IsStructuralContext() || Transaction.GetState() != EAssetDocumentAtomicState::Mutating
  || !Token.TransactionId.IsValid() || Token.TransactionId != Transaction.GetTransactionId()
  || Token.RequestId != State->RequestId || Token.RuntimeRevision != State->RuntimeRevision
  || (State->Capture && !State->Capture->IsOpenFor(Token)))
  return Result(false, TEXT("AnimationProvider.Montage.InvalidContext"));
 if (!BaselineCurrent(*State)) return Result(false, TEXT("AnimationProvider.Montage.PreparedStateDrift"));
 auto& Target = *State->Target.Get();
 State->bMutated = true;
 if (State->bReplaceSlots)
 {
  PublishMontageSlots(Target, State->DesiredSlots);
  State->bSlotsPublished = true;
 }
 if (Failure == 1) return Result(false, TEXT("AnimationProvider.Montage.InjectedFailure"));
 if (Target.GetDataModel()->GetNumberOfFrames() != State->DesiredFrames)
  static_cast<UAnimCompositeBase&>(Target).SetCompositeLength(State->DesiredLength);
 if (Failure == 2) return Result(false, TEXT("AnimationProvider.Montage.InjectedFailure"));
 if (Target.GetDataModel()->GetNumberOfFrames() != State->DesiredFrames)
  return Result(false, TEXT("AnimationProvider.Montage.CompletionFailed"), TEXT("Public length setter did not produce the prepared frame count."));
 PublishMontageSections(Target, State->DesiredSections, State->SectionLinks);
 if (Failure == 3) return Result(false, TEXT("AnimationProvider.Montage.InjectedFailure"));
 return Result(true);
}

FAssetDocumentContractValidationResult ReadbackPreparedStructure(const FPreparedStructuralEdit& State)
{
 if (!IsStructuralContext() || !TargetCurrent(State)) return Result(false, TEXT("AnimationProvider.Montage.ReadbackMismatch"));
 const auto& Target = *State.Target.Get();
 if (Target.GetDataModel()->GetNumberOfFrames() != State.DesiredFrames
  || State.SectionLinks.Num() != State.DesiredSections.Num()) return Result(false, TEXT("AnimationProvider.Montage.ReadbackMismatch"));
 if (Target.SlotAnimTracks.Num() != State.DesiredSlots.Num() || Target.CompositeSections.Num() != State.DesiredSections.Num())
  return Result(false, TEXT("AnimationProvider.Montage.ReadbackMismatch"));
 for (int32 I = 0; I < Target.SlotAnimTracks.Num(); ++I)
 {
  const auto& A = Target.SlotAnimTracks[I]; const auto& B = State.DesiredSlots[I];
  if (A.SlotName != B.SlotName || A.AnimTrack.AnimSegments.Num() != B.AnimTrack.AnimSegments.Num()) return Result(false);
  for (int32 J = 0; J < A.AnimTrack.AnimSegments.Num(); ++J)
  {
   const auto& X = A.AnimTrack.AnimSegments[J]; const auto& Y = B.AnimTrack.AnimSegments[J];
   if (X.GetAnimReference() != Y.GetAnimReference() || X.StartPos != Y.StartPos || X.AnimStartTime != Y.AnimStartTime
    || X.AnimEndTime != Y.AnimEndTime || X.AnimPlayRate != Y.AnimPlayRate || X.LoopingCount != Y.LoopingCount) return Result(false);
  }
 }
 for (int32 I = 0; I < Target.CompositeSections.Num(); ++I)
 {
  const auto& A = Target.CompositeSections[I]; const auto& B = State.DesiredSections[I]; const auto& L = State.SectionLinks[I];
  if (A.SectionName != B.SectionName || A.NextSectionName != B.NextSectionName || A.MetaData != B.MetaData
   || A.GetSlotIndex() != L.Slot || A.GetLinkMethod() != L.Method || !FMath::IsNearlyEqual(A.GetTime(), L.Time))
   return Result(false, TEXT("AnimationProvider.Montage.ReadbackMismatch"));
 }
 return Result(true);
}

FAssetDocumentContractValidationResult PrepareMontageStructuralEdit(UAnimMontage& Target,
 const FStructuralIntent& InputIntent, const FGuid& RequestId, uint64 RuntimeRevision,
 TSharedPtr<FPreparedStructuralEdit>& OutPrepared)
{
 FStructuralIntent Intent = InputIntent;
 OutPrepared.Reset();
 if (!IsStructuralContext() || !IsValid(&Target) || Target.GetClass() != UAnimMontage::StaticClass()
  || Target.IsTemplate() || Target.HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad) || !Target.IsDataModelValid()
  || !RequestId.IsValid() || RuntimeRevision == 0 || (!Intent.Slots.IsSet() && !Intent.Sections.IsSet())
  || uint8(Intent.ReplaceManagedFamilies) > uint8(EManagedNotifyReplacement::Both))
  return Result(false, TEXT("AnimationProvider.Montage.InvalidContext"));
 if (Target.ParentAsset || !Target.ChildrenAssets.IsEmpty())
  return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), TEXT("Parent/child montage mutation requires the shared dependency footprint owner."));
 auto State = MakeShared<FPreparedStructuralEdit>();
 State->Target.Reset(&Target); State->Model.Reset(Target.GetDataModelInterface().GetObject());
 State->TargetPath = Target.GetPathName(); State->PackagePath = Target.GetOutermost()->GetName();
 State->TargetRevision = Target.GetUniqueID(); State->RequestId = RequestId; State->RuntimeRevision = RuntimeRevision;
 State->FrameRate = Target.GetDataModel()->GetFrameRate(); State->OldFrames = Target.GetDataModel()->GetNumberOfFrames();
 State->TargetSkeleton = Target.GetSkeleton();
 State->TargetSkeletonGuid = Target.GetSkeleton() ? Target.GetSkeleton()->GetGuid() : FGuid();
 State->SyncGroup = Target.SyncGroup; State->SyncSlot = Target.SyncSlotIndex;
 State->OldSlots = Target.SlotAnimTracks; State->OldSections = Target.CompositeSections; State->OldNotifies = Target.Notifies;
 State->bReplaceSlots = Intent.Slots.IsSet(); State->bReplaceSections = Intent.Sections.IsSet();
 State->ReplaceManagedFamilies = Intent.ReplaceManagedFamilies;
 if (!State->FrameRate.IsValid() || State->OldFrames < 0) return Result(false);
 if (Intent.Slots.IsSet())
 {
  // Infer only unique correspondence from this immutable preimage. Explicit claims
  // reserve their old rows first; inference must never depend on desired-row order.
  TSet<FString> ExplicitClaims;
  for (const auto& Slot : *Intent.Slots) for (const auto& Segment : Slot.Segments)
  {
   if (Segment.PreviousSegment == INDEX_NONE)
   {
    if (!Segment.PreviousSlot.IsNone()) return Result(false);
    continue;
   }
   const int32 OldSlot = FindSlot(State->OldSlots, Segment.PreviousSlot);
   const FString Key = FString::Printf(TEXT("%d:%d"), OldSlot, Segment.PreviousSegment);
   if (!State->OldSlots.IsValidIndex(OldSlot)
    || !State->OldSlots[OldSlot].AnimTrack.AnimSegments.IsValidIndex(Segment.PreviousSegment)
    || ExplicitClaims.Contains(Key)) return Result(false);
   ExplicitClaims.Add(Key);
  }
  // Ordered one-to-one correspondence, as the Montage editor edits segments in place: segment N of a slot is
  // segment N of the live slot with the same name (its links follow it; FAnimLinkableElement keeps a slot and a
  // segment index). Later segments are new; live segments past the document's count are removed. This never
  // depends on which animations the segments play, so several segments of one animation are never ambiguous.
  for (auto& Slot : *Intent.Slots)
  {
   const int32 OldSlot = FindSlot(State->OldSlots, Slot.Name);
   for (int32 N = 0; N < Slot.Segments.Num(); ++N)
   {
    auto& Segment = Slot.Segments[N];
    if (Segment.PreviousSegment != INDEX_NONE || !State->OldSlots.IsValidIndex(OldSlot)
     || !State->OldSlots[OldSlot].AnimTrack.AnimSegments.IsValidIndex(N)
     || ExplicitClaims.Contains(FString::Printf(TEXT("%d:%d"), OldSlot, N))) continue;
    Segment.PreviousSlot = Slot.Name;
    Segment.PreviousSegment = N;
   }
  }
  auto Valid = BuildMontageSlots(*Intent.Slots, Target, State->DesiredSlots);
  if (!Valid.bValid) return Valid;
  AlignMontageSegmentStarts(State->DesiredSlots);
  for (int32 S = 0; S < State->DesiredSlots.Num(); ++S)
   for (int32 I = 0; I < State->DesiredSlots[S].AnimTrack.AnimSegments.Num(); ++I)
   {
    Valid = ValidateMontageSegmentStart(State->DesiredSlots[S].AnimTrack.AnimSegments, S, I);
    if (!Valid.bValid) return Valid;
   }
  TSet<FString> Correspondences;
  for (const auto& Slot : *Intent.Slots) for (const auto& Segment : Slot.Segments)
  {
   if (Segment.PreviousSegment != INDEX_NONE)
   {
    const int32 OldSlot = FindSlot(State->OldSlots, Segment.PreviousSlot);
    const FString Key = FString::Printf(TEXT("%d:%d"), OldSlot, Segment.PreviousSegment);
    if (!State->OldSlots.IsValidIndex(OldSlot) || !State->OldSlots[OldSlot].AnimTrack.AnimSegments.IsValidIndex(Segment.PreviousSegment)
     || Correspondences.Contains(Key)) return Result(false);
    Correspondences.Add(Key);
   }
   else if (!Segment.PreviousSlot.IsNone()) return Result(false);
  }
 }
 else State->DesiredSlots = State->OldSlots;
 float Length = 0.f;
 for (const auto& Slot : State->DesiredSlots) for (const auto& Segment : Slot.AnimTrack.AnimSegments)
  Length = FMath::Max(Length, Segment.StartPos + Segment.GetLength());
 if (!FMath::IsFinite(Length) || State->FrameRate.AsDecimal() * double(Length) > double(MAX_int32)) return Result(false);
 State->DesiredFrames = State->bReplaceSlots ? State->FrameRate.AsFrameTime(Length).RoundToFrame().Value : State->OldFrames;
 State->DesiredLength = float(State->FrameRate.AsSeconds(FFrameNumber(State->DesiredFrames)));
 // A montage that has never had a notify track (one just made by the factory) gets the default track "1" from
 // the length change (UAnimSequenceBase::InitializeNotifyTrack), as in the Montage editor and on every load.
 auto Valid = State->bReplaceSlots ? ValidateSync(*State) : Result(true);
 if (!Valid.bValid) return Valid;
 Valid = PrepareSections(*State, Intent); if (!Valid.bValid) return Valid;
 Valid = PrepareRetainedNotifies(*State, Intent); if (!Valid.bValid) return Valid;
 Valid = ValidateDurableShape(*State); if (!Valid.bValid) return Valid;
 auto Anchor = [&](UObject* Object) { if (Object) State->Anchors.Emplace(Object); };
 Anchor(Target.GetSkeleton());
 TFunction<void(UAnimSequenceBase*)> PinReference = [&](UAnimSequenceBase* Animation)
 {
  if (!Animation || State->ReferenceBaselines.ContainsByPredicate([&](const FAnimationBaseline& R) { return R.Object == Animation; })) return;
  Anchor(Animation); Anchor(Animation->GetSkeleton());
  FAnimationBaseline R;
  R.Object = Animation; R.Skeleton = Animation->GetSkeleton(); R.Path = Animation->GetPathName();
  R.Length = Animation->GetPlayLength(); R.RateScale = Animation->RateScale;
  R.BaselineSkeletonGuid = R.Skeleton ? R.Skeleton->GetGuid() : FGuid();
  R.Class = Animation->GetClass(); R.Revision = Animation->GetUniqueID();
  const auto* Composite = Cast<UAnimComposite>(Animation);
  if (Composite) R.CompositeSegments = Composite->AnimationTrack.AnimSegments;
  State->ReferenceBaselines.Add(MoveTemp(R));
  if (Composite) for (const auto& Segment : Composite->AnimationTrack.AnimSegments) PinReference(Segment.GetAnimReference().Get());
 };
 for (const auto* Slots : {&State->OldSlots, &State->DesiredSlots})
  for (const auto& Slot : *Slots) for (const auto& Segment : Slot.AnimTrack.AnimSegments)
  {
   auto* Animation = Segment.GetAnimReference().Get();
   PinReference(Animation);
   if (Slots == &State->DesiredSlots && Animation && !State->InputAnimations.Contains(Animation))
   {
    State->InputAnimations.Add(Animation); State->InputAnimationPaths.Add(Animation->GetPathName());
   }
  }
 for (const auto& Section : State->OldSections) for (const auto& Metadata : Section.MetaData) Anchor(Metadata.Get());
 for (const auto& Event : State->OldNotifies) { Anchor(Event.Notify); Anchor(Event.NotifyStateClass); }
 State->bChanged = State->DesiredFrames != State->OldFrames || State->DesiredSlots.Num() != State->OldSlots.Num()
  || State->DesiredSections.Num() != State->OldSections.Num();
 if (!State->bChanged)
 {
  for (int32 I = 0; I < State->OldSlots.Num(); ++I)
  {
   const auto& A = State->OldSlots[I]; const auto& B = State->DesiredSlots[I];
   if (A.SlotName != B.SlotName || A.AnimTrack.AnimSegments.Num() != B.AnimTrack.AnimSegments.Num()) { State->bChanged = true; break; }
   for (int32 J = 0; J < A.AnimTrack.AnimSegments.Num(); ++J)
   {
    const auto& X = A.AnimTrack.AnimSegments[J]; const auto& Y = B.AnimTrack.AnimSegments[J];
    State->bChanged |= X.GetAnimReference() != Y.GetAnimReference() || X.StartPos != Y.StartPos || X.AnimStartTime != Y.AnimStartTime
     || X.AnimEndTime != Y.AnimEndTime || X.AnimPlayRate != Y.AnimPlayRate || X.LoopingCount != Y.LoopingCount;
   }
  }
  for (int32 I = 0; I < State->OldSections.Num(); ++I)
  {
   const auto& A = State->OldSections[I]; const auto& B = State->DesiredSections[I]; const auto& L = State->SectionLinks[I];
   State->bChanged |= A.SectionName != B.SectionName || A.NextSectionName != B.NextSectionName || A.GetTime() != L.Time
    || A.GetSlotIndex() != L.Slot || A.GetLinkMethod() != L.Method;
  }
 }
 State->FrozenArguments = FreezeMontageStructuralArguments(Intent);
 OutPrepared = State;
 return Result(true);
}

TArray<FAssetDocumentMutationFootprint> GetMontageStructuralFootprint(const FPreparedStructuralEdit& State)
{
 return {{EAssetDocumentMutationFootprintKind::UObjectState, TEXT("Object:") + State.TargetPath, RollbackId},
  {EAssetDocumentMutationFootprintKind::Package, TEXT("Package:") + State.PackagePath, TEXT("rollback.package")}};
}

FAssetDocumentContractValidationResult ApplyMontageStructuralEdit(TSharedRef<FPreparedStructuralEdit> State,
 const IAssetDocumentVerifiedHandleLease& TargetLease,
 EAssetDocumentRuntimeRequestPhase Phase, IAssetDocumentAtomicTransaction& Transaction)
{
 if ((Phase != EAssetDocumentRuntimeRequestPhase::Apply && Phase != EAssetDocumentRuntimeRequestPhase::Repair)
  || !LeaseMatches(TargetLease, *State, Transaction, EAssetDocumentHandleAccess::Mutable)) return Result(false, TEXT("AnimationProvider.Montage.InvalidContext"));
 auto Valid = WithAssetDocumentEditorAssetIdentity(TargetLease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  return Result(&View.GetObject() == State->Target.Get() && View.GetCanonicalObjectPath() == State->TargetPath
   && View.GetTypeRevision() == State->TargetRevision, TEXT("AnimationProvider.Montage.PreparedStateDrift"));
 });
 if (!Valid.bValid) return Valid;
 return ApplyNativeImpl(State, Transaction, 0);
}
#if WITH_DEV_AUTOMATION_TESTS
FAssetDocumentContractValidationResult Testing::ApplyNative(TSharedRef<FPreparedStructuralEdit> State,
 IAssetDocumentAtomicTransaction& Transaction, EFailurePoint Failure)
{
 if (!State->Capture || !State->Capture->IsOpenFor(Transaction.GetMutationToken()))
  return Result(false, TEXT("AnimationProvider.Montage.InvalidContext"));
 return ApplyNativeImpl(State, Transaction, static_cast<uint8>(Failure));
}
#endif
}
