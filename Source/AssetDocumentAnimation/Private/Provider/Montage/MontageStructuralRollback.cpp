// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Provider/Montage/MontageStructuralPreparation.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectThreadContext.h"

namespace AssetDocumentMontage
{
FAssetDocumentContractValidationResult Result(bool bValid, const TCHAR* Code, const FString& Detail)
{
 FAssetDocumentContractValidationResult Out;
 Out.bValid = bValid;
 if (!bValid)
 {
  auto& D = Out.Diagnostics.AddDefaulted_GetRef();
  D.Code = Code; D.Path = TEXT("/Primitive");
  D.Message = Detail.IsEmpty() ? TEXT("Montage structural input, prepared state or native completion is invalid.") : Detail;
 }
 return Out;
}
bool IsStructuralContext()
{
 return IsInGameThread() && GIsEditor && !IsGarbageCollecting() && !UE::IsSavingPackage(nullptr)
  && !FUObjectThreadContext::Get().IsRoutingPostLoad;
}
bool TargetCurrent(const FPreparedStructuralEdit& State)
{
 const auto* Target = State.Target.Get();
 return IsValid(Target) && Target->GetClass() == UAnimMontage::StaticClass()
  && !Target->IsTemplate() && !Target->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad)
  && Target->GetPathName() == State.TargetPath && Target->GetUniqueID() == State.TargetRevision
  && Target->GetSkeleton() == State.TargetSkeleton
  && (!State.TargetSkeleton || State.TargetSkeleton->GetGuid() == State.TargetSkeletonGuid)
  && Target->GetOutermost()->GetName() == State.PackagePath && Target->IsDataModelValid()
  && Target->GetDataModelInterface().GetObject() == State.Model.Get()
  && Target->GetDataModel()->GetFrameRate() == State.FrameRate
  && !Target->ParentAsset && Target->ChildrenAssets.IsEmpty()
  && Target->SyncGroup == State.SyncGroup && Target->SyncSlotIndex == State.SyncSlot;
}
bool BaselineCurrent(const FPreparedStructuralEdit& State)
{
 if (!TargetCurrent(State)) return false;
 const auto& Target = *State.Target.Get();
 if (Target.GetDataModel()->GetNumberOfFrames() != State.OldFrames
  || Target.SlotAnimTracks.Num() != State.OldSlots.Num() || Target.CompositeSections.Num() != State.OldSections.Num()
  || Target.Notifies.Num() != State.OldNotifies.Num()) return false;
 // Exact typed read-only comparisons detect preflight drift; no field lookup or reflected write.
 for (int32 I = 0; I < State.OldSlots.Num(); ++I)
  if (!FSlotAnimationTrack::StaticStruct()->CompareScriptStruct(&Target.SlotAnimTracks[I], &State.OldSlots[I], 0)) return false;
 for (int32 I = 0; I < State.OldSections.Num(); ++I)
  if (!FCompositeSection::StaticStruct()->CompareScriptStruct(&Target.CompositeSections[I], &State.OldSections[I], 0)) return false;
 for (int32 I = 0; I < State.OldNotifies.Num(); ++I)
  if (!FAnimNotifyEvent::StaticStruct()->CompareScriptStruct(&Target.Notifies[I], &State.OldNotifies[I], 0)) return false;
 for (const auto& R : State.ReferenceBaselines)
 {
  if (!IsValid(R.Object) || R.Object->GetClass() != R.Class || R.Object->GetUniqueID() != R.Revision
   || R.Object->GetPathName() != R.Path || R.Object->GetPlayLength() != R.Length || R.Object->RateScale != R.RateScale
   || R.Object->GetSkeleton() != R.Skeleton || (R.Skeleton && (!IsValid(R.Skeleton) || R.Skeleton->GetGuid() != R.BaselineSkeletonGuid))) return false;
  if (const auto* Composite = Cast<UAnimComposite>(R.Object))
  {
   if (Composite->AnimationTrack.AnimSegments.Num() != R.CompositeSegments.Num()) return false;
   for (int32 I = 0; I < R.CompositeSegments.Num(); ++I)
    if (!FAnimSegment::StaticStruct()->CompareScriptStruct(&Composite->AnimationTrack.AnimSegments[I], &R.CompositeSegments[I], 0)) return false;
  }
 }
 return true;
}
void SetLinkPlacement(FAnimLinkableElement& Link, UAnimMontage& Target, const FLinkPlacement& Placement)
{
 check(Target.SlotAnimTracks.IsValidIndex(Placement.Slot));
 Link.Link(&Target, Placement.Time, Placement.Slot);
 Link.ChangeLinkMethod(Placement.Method);
 Link.SetTime(Placement.Time);
}
FAssetDocumentContractValidationResult CompleteMontageStructure(UAnimMontage& Target)
{
 if (!IsStructuralContext() || Target.ParentAsset || !Target.ChildrenAssets.IsEmpty())
  return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"));
 // Public virtual lifecycle dispatch; private marker/propagation algorithms stay in stock UE.
 Target.UpdateLinkableElements();
 static_cast<UAnimSequenceBase&>(Target).RefreshCacheData();
 FPropertyChangedEvent Changed(nullptr, EPropertyChangeType::ValueSet);
 static_cast<UObject&>(Target).PostEditChangeProperty(Changed);
 return Result(true);
}
FAssetDocumentContractValidationResult FStructuralInverse::Rollback(FStringView)
{
 return RestoreStructuralPreimageOnce();
}
FAssetDocumentContractValidationResult FStructuralInverse::RestoreStructuralPreimageOnce()
{
 if (State->bInverseConsumed || !IsStructuralContext() || !TargetCurrent(*State)
  || !State->Capture || !State->Capture->IsClosedForRollback())
  return Result(false, TEXT("AnimationProvider.Montage.RollbackFailed"));
 State->bInverseConsumed = true;
 // The prepared inverse and handler-local plan are separate; inspect the captured baseline.
 if (BaselineCurrent(*State)) return Result(true);
 auto& Target = *State->Target.Get();
 // Rollback-only assignments, unreachable from the forward publication callers.
 Target.SlotAnimTracks = State->OldSlots;
 if (Target.GetDataModel()->GetNumberOfFrames() != State->OldFrames)
  Target.GetController().SetNumberOfFrames(FFrameNumber(State->OldFrames), false);
 // The controller may move/clamp events. Install the native preimage only after its notification.
 Target.CompositeSections = State->OldSections;
 Target.Notifies = State->OldNotifies;
 auto Valid = CompleteMontageStructure(Target);
 if (!Valid.bValid) return Valid;
 if (Target.GetDataModel()->GetNumberOfFrames() != State->OldFrames
  || Target.SlotAnimTracks.Num() != State->OldSlots.Num() || Target.CompositeSections.Num() != State->OldSections.Num()
  || Target.Notifies.Num() != State->OldNotifies.Num()) return Result(false, TEXT("AnimationProvider.Montage.RollbackFailed"));
 // Compare authored behavior rather than requiring the old native cache representation.
 for (int32 I = 0; I < State->OldSections.Num(); ++I)
 {
  const auto& A = Target.CompositeSections[I]; const auto& B = State->OldSections[I];
  if (A.SectionName != B.SectionName || A.NextSectionName != B.NextSectionName || A.MetaData != B.MetaData
   || A.GetSlotIndex() != B.GetSlotIndex() || A.GetLinkMethod() != B.GetLinkMethod()
   || !FMath::IsNearlyEqual(A.GetTime(), B.GetTime())) return Result(false, TEXT("AnimationProvider.Montage.RollbackFailed"));
 }
 for (int32 I = 0; I < State->OldNotifies.Num(); ++I)
 {
  const auto& A = Target.Notifies[I]; const auto& B = State->OldNotifies[I];
  if (A.Notify != B.Notify || A.NotifyStateClass != B.NotifyStateClass || A.NotifyName != B.NotifyName
   || A.TrackIndex != B.TrackIndex || A.GetLinkMethod() != B.GetLinkMethod() || A.GetSlotIndex() != B.GetSlotIndex()
   || !FMath::IsNearlyEqual(A.GetTime(), B.GetTime()) || !FMath::IsNearlyEqual(A.GetDuration(), B.GetDuration())
   || (A.NotifyStateClass && (A.EndLink.GetSlotIndex() != B.EndLink.GetSlotIndex() || A.EndLink.GetLinkMethod() != B.EndLink.GetLinkMethod())))
   return Result(false, TEXT("AnimationProvider.Montage.RollbackFailed"));
 }
 return Result(true);
}
}
