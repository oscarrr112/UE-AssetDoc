// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonState.h"

namespace AssetDocumentSequenceAdditive
{
inline constexpr const TCHAR* Primitive = TEXT("animation.sequence.complete_edit.v2");
inline constexpr const TCHAR* Schema = TEXT("animation.sequence.complete_edit.input.v2");
inline constexpr const TCHAR* RollbackId = TEXT("animation.sequence.complete_edit.v2.additive.rollback-subaction.v1");
using AssetDocumentAnimationSkeleton::ContextValid;
using AssetDocumentAnimationSkeleton::CanonicalPath;
using AssetDocumentAnimationSkeleton::ExactObject;
using AssetDocumentAnimationSkeleton::Acquire;
using AssetDocumentAnimationSkeleton::Footprint;

inline FAssetDocumentContractValidationResult Result(bool bValid,
 const TCHAR* Code = TEXT("AnimationProvider.SequenceAdditive.InvalidState"))
{
 FAssetDocumentContractValidationResult R; R.bValid = bValid;
 if (!bValid)
 {
  auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/Additive"); D.Code = Code;
  D.Message = TEXT("Exact prepared Sequence additive author state and public completion are required.");
 }
 return R;
}

// Native authored values only. The null/reset alias has no inferred defaults.
struct FAuthorState
{
 EAdditiveAnimationType Type = AAT_None;
 EAdditiveBasePoseType Base = ABPT_None;
 int32 Frame = 0;
 UAnimSequence* Reference = nullptr;
 static FAuthorState Read(const UAnimSequence& S)
 { return {S.AdditiveAnimType.GetValue(), S.RefPoseType.GetValue(), S.RefFrameIndex, S.RefPoseSeq.Get()}; }
 bool Matches(const UAnimSequence& S) const
 { const auto V = Read(S); return Type == V.Type && Base == V.Base && Frame == V.Frame && Reference == V.Reference; }
 bool operator==(const FAuthorState& V) const
 { return Type == V.Type && Base == V.Base && Frame == V.Frame && Reference == V.Reference; }
};

inline bool CanonicalSequence(const UAnimSequence* S)
{
 if (!ExactObject(S, UAnimSequence::StaticClass()) || S->GetOuter() != S->GetOutermost()
  || !CanonicalPath(S->GetPathName()) || FSoftObjectPath(S->GetPathName()).ResolveObject() != S) return false;
 const USkeleton* Skeleton = S->GetSkeleton();
 const auto* Model = S->GetDataModel();
 return ExactObject(Skeleton, USkeleton::StaticClass()) && S->GetSkeletonGuid() == Skeleton->GetGuid()
  && CanonicalPath(Skeleton->GetPathName()) && FSoftObjectPath(Skeleton->GetPathName()).ResolveObject() == Skeleton
  && S->IsDataModelValid() && Model && Model->GetNumberOfKeys() > 0;
}

// Declared author bounds and the fixed public event's observable normalization
// refusals; this does not duplicate a private Engine validity/compression routine.
inline bool StableAuthorState(const UAnimSequence& Target, const FAuthorState& V)
{
 if (V.Type != AAT_None && V.Type != AAT_LocalSpaceBase && V.Type != AAT_RotationOffsetMeshSpace) return false;
 if (V.Base != ABPT_None && V.Base != ABPT_RefPose && V.Base != ABPT_AnimScaled
  && V.Base != ABPT_AnimFrame && V.Base != ABPT_LocalAnimFrame) return false;
 if (V.Frame < 0 || (V.Type != AAT_None && V.Base == ABPT_None)) return false;
 if (V.Reference && (!CanonicalSequence(V.Reference) || V.Reference == &Target
  || V.Reference->GetSkeleton() != Target.GetSkeleton())) return false;
 if (V.Reference && (V.Type == AAT_None || V.Base == ABPT_RefPose || V.Base == ABPT_LocalAnimFrame)) return false;
 if (V.Type != AAT_None && (V.Base == ABPT_AnimScaled || V.Base == ABPT_AnimFrame) && !V.Reference) return false;
 if (V.Type != AAT_None && (V.Base == ABPT_AnimFrame || V.Base == ABPT_LocalAnimFrame))
 {
  const UAnimSequence* Source = V.Base == ABPT_AnimFrame ? V.Reference : &Target;
  if (!Source) return false;
  const int32 Sampled = Source->GetNumberOfSampledKeys();
  const int32 Keys = Sampled > 0 ? Sampled : Source->GetDataModel()->GetNumberOfKeys();
  if (Keys <= 0 || V.Frame >= Keys) return false;
 }
 return true;
}
inline bool CompletedAuthorState(const UAnimSequence& S, const FAuthorState& V)
{
 return CanonicalSequence(&S) && StableAuthorState(S, V) && V.Matches(S) && S.IsCompressedDataValid()
  && (V.Type == AAT_None ? !S.IsValidAdditive() : S.IsValidAdditive());
}
}
