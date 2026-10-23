// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimComposite.h"

namespace AssetDocumentMontage
{
namespace
{
bool AcyclicReference(const UAnimSequenceBase& Animation, const UAnimMontage& Target, TSet<const UAnimSequenceBase*>& Visiting)
{
 if (&Animation == &Target || Visiting.Contains(&Animation)) return false;
 const auto* Composite = Cast<UAnimComposite>(&Animation);
 if (!Composite) return true;
 Visiting.Add(&Animation);
 for (const auto& Segment : Composite->AnimationTrack.AnimSegments)
 {
  const auto* Reference = Segment.GetAnimReference().Get();
  if (!IsValid(Reference) || !AcyclicReference(*Reference, Target, Visiting)) return false;
 }
 Visiting.Remove(&Animation);
 return true;
}
// Names the Skeleton and lists its slots (the montage editor's slot picker offers exactly these).
FString InvalidSlotMessage(const FName SlotName, const bool bDuplicate, const USkeleton* Skeleton)
{
 if (SlotName.IsNone()) return TEXT("A slot track needs a SlotName.");
 if (bDuplicate) return FString::Printf(TEXT("Slot '%s' has more than one slot track."), *SlotName.ToString());
 if (!IsValid(Skeleton)) return FString::Printf(TEXT("Slot '%s' needs the montage's Skeleton (Body.Skeleton)."), *SlotName.ToString());
 TArray<FString> Slots;
 for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups())
 {
  for (const FName Name : Group.SlotNames) Slots.Add(Name.ToString());
 }
 const bool bReadOnly = !Skeleton->GetPathName().StartsWith(TEXT("/Game/"));
 return FString::Printf(TEXT("Skeleton %s has no slot '%s'. Its slots: %s. Use one of them, or add the slot to the Skeleton first with a Skeleton document (Body.SlotGroups).%s"),
  *Skeleton->GetPathName(), *SlotName.ToString(), Slots.IsEmpty() ? TEXT("none") : *FString::Join(Slots, TEXT(", ")),
  bReadOnly ? TEXT(" This Skeleton is engine or plugin content (read-only): copy it into /Game with duplicate_asset, add the slot there and use the copy.") : TEXT(""));
}
}
FString MontageShortFloat(float Value)
{
 for (int32 Digits = 1; Digits <= 9; ++Digits)
 {
  const FString Text = FString::Printf(TEXT("%.*g"), Digits, double(Value));
  if (float(FCString::Atod(*Text)) == Value) return Text;
 }
 return FString::Printf(TEXT("%.9g"), double(Value));
}
void AlignMontageSegmentStarts(TArray<FSlotAnimationTrack>& Slots)
{
 for (auto& Slot : Slots)
 {
  auto& Segments = Slot.AnimTrack.AnimSegments;
  for (int32 I = 0; I < Segments.Num(); ++I)
  {
   const float Expected = I == 0 ? 0.f : Segments[I - 1].StartPos + Segments[I - 1].GetLength();
   if (Segments[I].StartPos != Expected && FMath::IsFinite(Expected)
    && FMath::Abs(double(Segments[I].StartPos) - double(Expected)) <= MontageSegmentStartTolerance)
    Segments[I].StartPos = Expected;
  }
 }
}
FAssetDocumentContractValidationResult ValidateMontageSegmentStart(const TArray<FAnimSegment>& Segments, int32 Slot, int32 Index)
{
 const auto& Segment = Segments[Index];
 const float Expected = Index == 0 ? 0.f : Segments[Index - 1].StartPos + Segments[Index - 1].GetLength();
 if (Segment.StartPos == Expected) return Result(true);
 FString Message;
 if (Index == 0)
  Message = FString::Printf(TEXT("SlotAnimTracks[%d].Segments[0] StartPos %s: the first segment of a slot starts at 0 (the editor and every load move it there). Use StartPos 0."),
   Slot, *MontageShortFloat(Segment.StartPos));
 else
 {
  const FString Amount = FString::Printf(TEXT("%.3g"), FMath::Abs(double(Segment.StartPos) - double(Expected)));
  const FString Relation = Segment.StartPos > Expected
   ? FString::Printf(TEXT("leaves a gap of %s s after Segments[%d]"), *Amount, Index - 1)
   : FString::Printf(TEXT("overlaps Segments[%d] by %s s"), Index - 1, *Amount);
  Message = FString::Printf(TEXT("SlotAnimTracks[%d].Segments[%d] StartPos %s %s, which ends at %s (its StartPos + LoopingCount * (AnimEndTime - AnimStartTime) / |AnimPlayRate * the animation's RateScale|, in float32). A slot's segments are contiguous (the editor and every load move a segment to the previous one's end; a start within %g s of it is accepted as that end). Use StartPos %s."),
   Slot, Index, *MontageShortFloat(Segment.StartPos), *Relation, *MontageShortFloat(Expected), MontageSegmentStartTolerance,
   *MontageShortFloat(Expected));
 }
 auto Out = Result(false, TEXT("AnimationProvider.Montage.SegmentGap"), Message);
 Out.Diagnostics[0].Path = FString::Printf(TEXT("/Body/SlotAnimTracks/%d/Segments/%d/StartPos"), Slot, Index);
 return Out;
}
namespace
{
FAssetDocumentContractValidationResult MontageSegmentFailure(int32 Slot, int32 Index, const TCHAR* Field, const FString& Message,
 const TCHAR* Code = TEXT("AnimationProvider.Montage.InvalidSegment"))
{
 auto Out = Result(false, Code, FString::Printf(TEXT("SlotAnimTracks[%d].Segments[%d]: %s"), Slot, Index, *Message));
 Out.Diagnostics[0].Path = FString::Printf(TEXT("/Body/SlotAnimTracks/%d/Segments/%d/%s"), Slot, Index, Field);
 return Out;
}
}
FAssetDocumentContractValidationResult BuildMontageSlots(
 const TArray<FSlotEdit>& Input, const UAnimMontage& Target, TArray<FSlotAnimationTrack>& Output)
{
 Output.Reset();
 TSet<FName> Names;
 for (int32 SlotIndex = 0; SlotIndex < Input.Num(); ++SlotIndex)
 {
  const auto& Slot = Input[SlotIndex];
  if (Slot.Name.IsNone() || Names.Contains(Slot.Name) || !IsValid(Target.GetSkeleton())
   || !Target.GetSkeleton()->ContainsSlotName(Slot.Name))
   return Result(false, TEXT("AnimationProvider.Montage.InvalidSlot"), InvalidSlotMessage(Slot.Name, Names.Contains(Slot.Name), Target.GetSkeleton()));
  Names.Add(Slot.Name);
  FSlotAnimationTrack Native;
  Native.SlotName = Slot.Name;
  for (int32 Index = 0; Index < Slot.Segments.Num(); ++Index)
  {
   const auto& Segment = Slot.Segments[Index];
   auto* Animation = Segment.Animation;
   if (!IsValid(Animation) || Animation == &Target || Animation->IsTemplate()
    || Animation->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad) || !Animation->CanBeUsedInComposition()
    || !IsValid(Animation->GetSkeleton()) || !Target.GetSkeleton()->IsCompatibleForEditor(Animation->GetSkeleton()))
    return MontageSegmentFailure(SlotIndex, Index, TEXT("AnimReference"), FString::Printf(
     TEXT("%s cannot be used in this Montage (it must be another animation of a Skeleton compatible with %s)."),
     IsValid(Animation) ? *Animation->GetPathName() : TEXT("the animation"), *Target.GetSkeleton()->GetPathName()),
     TEXT("AnimationProvider.Montage.IncompatibleAnimation"));
   const float Length = Animation->GetPlayLength();
   if (!FMath::IsFinite(Segment.Start) || Segment.Start < 0.f)
    return MontageSegmentFailure(SlotIndex, Index, TEXT("StartPos"), FString::Printf(TEXT("StartPos %g must be a time >= 0."), Segment.Start));
   if (!FMath::IsFinite(Segment.SourceStart) || Segment.SourceStart < 0.f || Segment.SourceStart >= Length)
    return MontageSegmentFailure(SlotIndex, Index, TEXT("AnimStartTime"), FString::Printf(
     TEXT("AnimStartTime %s must be in [0, %s) (%s is %s s long)."), *MontageShortFloat(Segment.SourceStart), *MontageShortFloat(Length),
     *Animation->GetPathName(), *MontageShortFloat(Length)));
   if (!FMath::IsFinite(Segment.SourceEnd) || Segment.SourceEnd <= Segment.SourceStart)
    return MontageSegmentFailure(SlotIndex, Index, TEXT("AnimEndTime"), FString::Printf(
     TEXT("AnimEndTime %s must be greater than AnimStartTime %s."), *MontageShortFloat(Segment.SourceEnd), *MontageShortFloat(Segment.SourceStart)));
   if (Segment.SourceEnd > Length)
    return MontageSegmentFailure(SlotIndex, Index, TEXT("AnimEndTime"), FString::Printf(
     TEXT("AnimEndTime %s is after the end of %s (%s s long) by %s s: use at most %s (Extract and inspect_asset_document_target give the sequence length)."),
     *MontageShortFloat(Segment.SourceEnd), *Animation->GetPathName(), *MontageShortFloat(Length),
     *FString::Printf(TEXT("%.3g"), double(Segment.SourceEnd) - double(Length)), *MontageShortFloat(Length)));
   if (!FMath::IsFinite(Segment.Rate) || Segment.Rate == 0.f)
    return MontageSegmentFailure(SlotIndex, Index, TEXT("AnimPlayRate"), TEXT("AnimPlayRate must be a non-zero number (negative plays backwards)."));
   if (Segment.Loops <= 0)
    return MontageSegmentFailure(SlotIndex, Index, TEXT("LoopingCount"), FString::Printf(TEXT("LoopingCount %d must be >= 1."), Segment.Loops));
   TSet<const UAnimSequenceBase*> Visiting;
   if (!AcyclicReference(*Animation, Target, Visiting))
    return MontageSegmentFailure(SlotIndex, Index, TEXT("AnimReference"), FString::Printf(
     TEXT("%s plays this Montage (directly or through a composite): a Montage cannot contain itself."), *Animation->GetPathName()),
     TEXT("AnimationProvider.Montage.RecursiveAnimation"));
   FAnimSegment Row;
   Row.SetAnimReference(Animation);
   Row.StartPos = Segment.Start; Row.AnimStartTime = Segment.SourceStart; Row.AnimEndTime = Segment.SourceEnd;
   Row.AnimPlayRate = Segment.Rate; Row.LoopingCount = Segment.Loops;
   if (!FMath::IsFinite(Row.GetLength()) || !FMath::IsFinite(Row.StartPos + Row.GetLength()) || !Row.IsValid())
    return MontageSegmentFailure(SlotIndex, Index, TEXT("AnimPlayRate"), FString(
     TEXT("the segment length LoopingCount * (AnimEndTime - AnimStartTime) / |AnimPlayRate| is not a finite time.")));
   Native.AnimTrack.AnimSegments.Add(MoveTemp(Row));
  }
  Output.Add(MoveTemp(Native));
 }
 return Result(true);
}
void PublishMontageSlots(UAnimMontage& Target, const TArray<FSlotAnimationTrack>& Slots)
{
 // AD.ANIMATION.MONTAGE.SLOTS-PUBLISH.v1: sole forward assignment, exact typed family.
 Target.SlotAnimTracks = Slots;
}
}
