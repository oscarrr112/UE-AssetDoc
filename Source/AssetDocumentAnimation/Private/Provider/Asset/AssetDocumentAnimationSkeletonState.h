// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Animation/AnimMontage.h"
#include "Misc/PackageName.h"
#include "Animation/Skeleton.h"
#include "AssetDocumentEditorNativeState.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonOperations.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectThreadContext.h"

namespace AssetDocumentAnimationSkeleton
{
inline constexpr const TCHAR* Primitive = TEXT("animation.asset.skeleton.set.v1");
inline constexpr const TCHAR* Schema = TEXT("animation.asset.skeleton.set.v1.input.v1");
inline constexpr const TCHAR* RollbackId = TEXT("animation.asset.skeleton.set.v1.rollback-subaction.v1");
inline FAssetDocumentContractValidationResult Result(bool bValid, const TCHAR* Code = TEXT("AnimationProvider.Skeleton.InvalidTarget"))
{
 FAssetDocumentContractValidationResult R;
 R.bValid = bValid;
 if (!bValid)
 {
  auto& D = R.Diagnostics.AddDefaulted_GetRef();
  D.Path = TEXT("/Skeleton"); D.Code = Code;
  D.Message = TEXT("The exact Skeleton operation, prepared identity or authoritative readback is invalid.");
 }
 return R;
}
inline bool ContextValid()
{
 return IsInGameThread() && GIsEditor && !IsGarbageCollecting() && !UE::IsSavingPackage(nullptr)
  && !FUObjectThreadContext::Get().IsRoutingPostLoad;
}
inline bool CanonicalPath(const FString& Path)
{
 const FSoftObjectPath Soft(Path);
 return Soft.IsValid() && Soft.GetSubPathString().IsEmpty() && Path == Soft.ToString()
  && FPackageName::IsValidObjectPath(Path) && !Soft.GetAssetName().IsEmpty()
  && FPackageName::IsValidLongPackageName(Soft.GetLongPackageName());
}
inline bool ExactObject(const UObject* Object, UClass* Class)
{
 return IsValid(Object) && !Object->IsTemplate() && Object->GetClass() == Class
  && !Object->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad);
}
inline bool CanonicalSkeleton(const UAnimMontage& Target)
{
 const USkeleton* S = Target.GetSkeleton();
 return S ? ExactObject(S, USkeleton::StaticClass()) && Target.GetSkeletonGuid() == S->GetGuid()
  : !Target.GetSkeletonGuid().IsValid();
}
inline bool ParseOperation(const FAssetDocumentRuntimeValueDto& Value, bool bPreparation, FString& Operation, FString& Path)
{
 if (!ValidateAssetDocumentRuntimeValue(Value).bValid || Value.Kind != EAssetDocumentRuntimeValueKind::Object
  || Value.ObjectKeys.Num() != (bPreparation ? 2 : 1) || Value.Elements.Num() != Value.ObjectKeys.Num()) return false;
 const int32 Op = Value.ObjectKeys.IndexOfByKey(TEXT("operation"));
 if (!Value.Elements.IsValidIndex(Op) || Value.Elements[Op].Kind != EAssetDocumentRuntimeValueKind::String) return false;
 Operation = Value.Elements[Op].StringValue;
 if (Operation != TEXT("set") && Operation != TEXT("clear")) return false;
 if (!bPreparation) return true;
 const int32 Ref = Value.ObjectKeys.IndexOfByKey(TEXT("skeleton_path"));
 if (!Value.Elements.IsValidIndex(Ref)) return false;
 if (Operation == TEXT("clear")) return Value.Elements[Ref].Kind == EAssetDocumentRuntimeValueKind::Null;
 if (Value.Elements[Ref].Kind != EAssetDocumentRuntimeValueKind::String) return false;
 Path = Value.Elements[Ref].StringValue;
 return CanonicalPath(Path);
}
inline TArray<FAssetDocumentMutationFootprint> Footprint(const FString& Path, const FString& Package)
{
 return {{EAssetDocumentMutationFootprintKind::UObjectState, TEXT("Object:") + Path, TEXT("rollback.uobject")},
  {EAssetDocumentMutationFootprintKind::Package, TEXT("Package:") + Package, TEXT("rollback.package")}};
}

// Read-only retained content snapshot; never published back to the montage.
struct FRetainedState
{
 TArray<FSlotAnimationTrack> Tracks;
 TArray<TStrongObjectPtr<UAnimSequenceBase>> Animations;
 TArray<TStrongObjectPtr<USkeleton>> Skeletons;
 TArray<FString> AnimationPaths;
 TArray<FString> SkeletonPaths;
 TArray<FGuid> SkeletonGuids;
 explicit FRetainedState(const UAnimMontage& Target) : FRetainedState(Target.SlotAnimTracks) {}
 explicit FRetainedState(const TArray<FSlotAnimationTrack>& InTracks) : Tracks(InTracks)
 {
  for (const auto& Slot : Tracks) for (const auto& Segment : Slot.AnimTrack.AnimSegments)
  {
   UAnimSequenceBase* A = Segment.GetAnimReference();
   USkeleton* S = A ? A->GetSkeleton() : nullptr;
   Animations.Emplace(A); Skeletons.Emplace(S);
   AnimationPaths.Add(A ? A->GetPathName() : FString());
   SkeletonPaths.Add(S ? S->GetPathName() : FString());
   SkeletonGuids.Add(S ? S->GetGuid() : FGuid());
  }
 }
 bool Matches(const UAnimMontage& Target) const
 {
  if (Tracks.Num() != Target.SlotAnimTracks.Num()) return false;
  int32 Ref = 0;
  for (int32 I = 0; I < Tracks.Num(); ++I)
  {
   const auto& A = Tracks[I]; const auto& B = Target.SlotAnimTracks[I];
   if (A.SlotName != B.SlotName || A.AnimTrack.AnimSegments.Num() != B.AnimTrack.AnimSegments.Num()) return false;
   for (int32 J = 0; J < A.AnimTrack.AnimSegments.Num(); ++J, ++Ref)
   {
    const auto& X = A.AnimTrack.AnimSegments[J]; const auto& Y = B.AnimTrack.AnimSegments[J];
    if (X.GetAnimReference() != Y.GetAnimReference() || X.StartPos != Y.StartPos || X.AnimStartTime != Y.AnimStartTime
     || X.AnimEndTime != Y.AnimEndTime || X.AnimPlayRate != Y.AnimPlayRate || X.LoopingCount != Y.LoopingCount
     || X.CachedPlayLength != Y.CachedPlayLength || X.IsValid() != Y.IsValid()) return false;
    UAnimSequenceBase* Animation = Y.GetAnimReference(); USkeleton* S = Animation ? Animation->GetSkeleton() : nullptr;
    if (Animation != Animations[Ref].Get() || S != Skeletons[Ref].Get()
     || (Animation && (!IsValid(Animation) || Animation->GetPathName() != AnimationPaths[Ref]))
     || (S && (!IsValid(S) || S->GetPathName() != SkeletonPaths[Ref] || S->GetGuid() != SkeletonGuids[Ref]))) return false;
   }
  }
  return true;
 }
};
inline bool Compatible(const TArray<FSlotAnimationTrack>& Tracks, const USkeleton* Desired)
{
 if (!Desired) return true;
 for (const auto& Slot : Tracks)
 {
  if (!Desired->ContainsSlotName(Slot.SlotName)) return false;
  for (const auto& Segment : Slot.AnimTrack.AnimSegments)
  {
   const UAnimSequenceBase* Animation = Segment.GetAnimReference();
   if (!IsValid(Animation) || !IsValid(Animation->GetSkeleton()) || !Segment.IsValid()
    || !Desired->IsCompatibleForEditor(Animation->GetSkeleton())) return false;
  }
 }
 return true;
}
inline bool Compatible(const UAnimMontage& Target, const USkeleton* Desired)
{ return Compatible(Target.SlotAnimTracks, Desired); }
// Why Compatible() refuses Desired: the first slot it lacks (with the slots it has) or the first
// animation whose skeleton it does not accept, so an author can fix the document or the Skeleton.
inline FString IncompatibleReason(const TArray<FSlotAnimationTrack>& Tracks, const USkeleton* Desired)
{
 if (!Desired) return FString();
 const FString SkeletonPath = Desired->GetPathName();
 for (int32 Index = 0; Index < Tracks.Num(); ++Index)
 {
  const FSlotAnimationTrack& Slot = Tracks[Index];
  if (!Desired->ContainsSlotName(Slot.SlotName))
  {
   TArray<FString> Slots;
   for (const FAnimSlotGroup& Group : Desired->GetSlotGroups())
   {
    for (const FName SlotName : Group.SlotNames) Slots.Add(FString::Printf(TEXT("%s (group %s)"), *SlotName.ToString(), *Group.GroupName.ToString()));
   }
   return FString::Printf(TEXT("Skeleton %s has no slot '%s' (slot track %d of the montage; a new montage starts with one DefaultSlot track). Its slots: %s. ")
    TEXT("Author SlotAnimTracks with one of its slots, or add the slot to the Skeleton first with a Skeleton document (Body.SlotGroups).%s"),
    *SkeletonPath, *Slot.SlotName.ToString(), Index, Slots.IsEmpty() ? TEXT("none") : *FString::Join(Slots, TEXT(", ")),
    SkeletonPath.StartsWith(TEXT("/Game/")) ? TEXT("")
     : TEXT(" This Skeleton is engine or plugin content (read-only): copy it into /Game with duplicate_asset, add the slot there and use the copy."));
  }
  for (const auto& Segment : Slot.AnimTrack.AnimSegments)
  {
   const UAnimSequenceBase* Animation = Segment.GetAnimReference();
   if (!IsValid(Animation) || !IsValid(Animation->GetSkeleton()) || !Segment.IsValid()
    || !Desired->IsCompatibleForEditor(Animation->GetSkeleton()))
   {
    return FString::Printf(TEXT("Skeleton %s does not accept the animation %s of slot '%s' (its skeleton is %s)."),
     *SkeletonPath, IsValid(Animation) ? *Animation->GetPathName() : TEXT("(missing)"), *Slot.SlotName.ToString(),
     IsValid(Animation) && IsValid(Animation->GetSkeleton()) ? *Animation->GetSkeleton()->GetPathName() : TEXT("none"));
   }
  }
 }
 return FString();
}
// Compatible() as a validation result; the refusal says why (IncompatibleReason).
inline FAssetDocumentContractValidationResult CompatibleResult(const TArray<FSlotAnimationTrack>& Tracks, const USkeleton* Desired)
{
 FAssetDocumentContractValidationResult R;
 R.bValid = Compatible(Tracks, Desired);
 if (!R.bValid)
 {
  auto& D = R.Diagnostics.AddDefaulted_GetRef();
  D.Path = TEXT("/Skeleton"); D.Code = TEXT("AnimationProvider.Skeleton.IncompatibleReference");
  D.Message = IncompatibleReason(Tracks, Desired);
 }
 return R;
}
inline FAssetDocumentContractValidationResult Acquire(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 const TCHAR* Name, UClass* Class, EAssetDocumentHandleAccess Access,
 TUniquePtr<IAssetDocumentVerifiedHandleLease>& Lease, UObject*& Object)
{
 auto Acquired = AcquireAnimationInputLease(Handles, Invocation, Name, EAssetDocumentHandleKind::Asset, Access);
 if (!Acquired.Lease)
 {
  if (Acquired.Diagnostics.IsEmpty()) return Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
  FAssetDocumentContractValidationResult Failure;
  Failure.Diagnostics = MoveTemp(Acquired.Diagnostics);
  return Failure;
 }
 Lease = MoveTemp(Acquired.Lease);
 return WithAssetDocumentEditorAssetIdentity(*Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
 {
  UObject& Native = Identity.GetObject();
  if (!ExactObject(&Native, Class)) return Result(false, TEXT("AnimationProvider.TargetClassMismatch"));
  if (Identity.GetTypeRevision() != Native.GetUniqueID() || Identity.GetCanonicalObjectPath() != Native.GetPathName())
   return Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
  Object = &Native;
  return Result(true);
 });
}
inline bool ExactHandles(const FAssetDocumentPrimitiveInvocation& Invocation, bool bSet)
{
 if (Invocation.HandleArguments.Num() != (bSet ? 2 : 1)) return false;
 int32 Assets = 0, Skeletons = 0;
 for (const auto& Arg : Invocation.HandleArguments)
 {
  Assets += Arg.Name == TEXT("asset"); Skeletons += Arg.Name == TEXT("skeleton");
 }
 return Assets == 1 && Skeletons == (bSet ? 1 : 0);
}
#if WITH_DEV_AUTOMATION_TESTS
extern AssetDocumentAnimationSkeletonTesting::FHooks* TestHooks;
#endif
}
