// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Provider/Montage/MontageStructuralOperations.h"
#include "AssetDocumentRollbackAction.h"
#include "UObject/StrongObjectPtr.h"

namespace AssetDocumentMontage
{
inline constexpr const TCHAR* RollbackId = TEXT("animation.montage.structural.rollback.v1");
struct FLinkPlacement
{
 float Time = 0.f;
 int32 Slot = 0;
 EAnimLinkMethod::Type Method = EAnimLinkMethod::Absolute;
};
struct FAnimationBaseline
{
 UAnimSequenceBase* Object = nullptr;
 USkeleton* Skeleton = nullptr;
 FString Path;
 float Length = 0.f, RateScale = 1.f;
 FGuid BaselineSkeletonGuid;
 UClass* Class = nullptr;
 uint32 Revision = 0;
 TArray<FAnimSegment> CompositeSegments;
};
class FStructuralCapture;
struct FPreparedStructuralEdit
{
 TSharedPtr<FStructuralCapture> Capture;
 FAssetDocumentRuntimeValueDto FrozenArguments;
 TStrongObjectPtr<UAnimMontage> Target;
 TStrongObjectPtr<UObject> Model;
 TArray<TStrongObjectPtr<UObject>> Anchors;
 TArray<UAnimSequenceBase*> InputAnimations;
 TArray<FString> InputAnimationPaths;
 TArray<FAnimationBaseline> ReferenceBaselines;
 FString TargetPath, PackagePath;
 FGuid RequestId;
 uint64 RuntimeRevision = 0;
 uint32 TargetRevision = 0;
 USkeleton* TargetSkeleton = nullptr;
 FGuid TargetSkeletonGuid;
 FFrameRate FrameRate;
 int32 OldFrames = 0, DesiredFrames = 0;
 float DesiredLength = 0.f;
 FName SyncGroup;
 int32 SyncSlot = 0;
 EManagedNotifyReplacement ReplaceManagedFamilies = EManagedNotifyReplacement::None;
 TArray<FSlotAnimationTrack> OldSlots, DesiredSlots;
 TArray<FCompositeSection> OldSections, DesiredSections;
 // OldNotifies is always the complete rollback preimage. DesiredNotifies and its
 // aligned link arrays contain only rows retained by the declared composite families.
 TArray<FAnimNotifyEvent> OldNotifies, DesiredNotifies;
 TArray<FLinkPlacement> SectionLinks, NotifyStarts, NotifyEnds;
 bool bReplaceSlots = false, bReplaceSections = false, bChanged = false, bApplied = false, bMutated = false;
 bool bSlotsPublished = false;
 bool bInverseConsumed = false;
};
ASSETDOCUMENTANIMATION_API FAssetDocumentContractValidationResult Result(bool bValid, const TCHAR* Code = TEXT("AnimationProvider.Montage.InvalidIntent"), const FString& Detail = FString());
bool IsStructuralContext();
bool TargetCurrent(const FPreparedStructuralEdit& State);
bool BaselineCurrent(const FPreparedStructuralEdit& State);
FAssetDocumentContractValidationResult CompleteMontageStructure(UAnimMontage& Target);
void SetLinkPlacement(FAnimLinkableElement& Link, UAnimMontage& Target, const FLinkPlacement& Placement);

// Exact approved forward publications. Only the structural operation calls them.
FAssetDocumentContractValidationResult BuildMontageSlots(
 const TArray<FSlotEdit>& Input, const UAnimMontage& Target, TArray<FSlotAnimationTrack>& Output);
void PublishMontageSlots(UAnimMontage& Target, const TArray<FSlotAnimationTrack>& Slots);
// The shortest decimal that reads back as Value (float32): 0.1, 1.1193277.
FString MontageShortFloat(float Value);
// A slot's segments are contiguous: UE places segment I at the previous segment's float32 end (FAnimTrack::
// ValidateSegmentTimes on every load and in the editor). A start within MontageSegmentStartTolerance seconds of that end
// (e.g. computed in double precision) is that end: AlignMontageSegmentStarts moves it there.
inline constexpr double MontageSegmentStartTolerance = 1e-5;
void AlignMontageSegmentStarts(TArray<FSlotAnimationTrack>& Slots);
// AnimationProvider.Montage.SegmentGap at /Body/SlotAnimTracks/<Slot>/Segments/<Index>/StartPos, naming the expected start.
FAssetDocumentContractValidationResult ValidateMontageSegmentStart(const TArray<FAnimSegment>& Segments, int32 Slot, int32 Index);
FCompositeSection BuildMontageSection(const FCompositeSection* Preimage, FName Name, FName Next);
void PublishMontageSections(UAnimMontage& Target, const TArray<FCompositeSection>& Sections,
 TConstArrayView<FLinkPlacement> Links);
// NotifyTracks: the whole notify track list (names and colours), published before the events that use it.
void PublishMontageNotifyTracks(UAnimMontage& Target, const TArray<FAnimNotifyTrack>& Tracks);
void PublishMontageNotifyEvents(UAnimMontage& Target, const TArray<FAnimNotifyEvent>& Events,
 TConstArrayView<FLinkPlacement> Starts, TConstArrayView<FLinkPlacement> Ends, int32 RetainedPrefix = 0);

// No public inverse function: only this existing-Atomic action can restore the typed preimage.
class FStructuralInverse final : public IAssetDocumentRollbackAction
{
public:
 explicit FStructuralInverse(TSharedRef<FPreparedStructuralEdit> InState) : State(MoveTemp(InState)) {}
 FStringView GetRollbackComponentId() const override { return RollbackId; }
 FAssetDocumentContractValidationResult Rollback(FStringView FailureCode) override;
private:
 FAssetDocumentContractValidationResult RestoreStructuralPreimageOnce();
 TSharedRef<FPreparedStructuralEdit> State;
};
}
