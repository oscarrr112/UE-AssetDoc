// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceAssetState.h"
#include "AssetDocumentEditorSequenceRetargetSourceNameMutation.h"

namespace AssetDocumentSequenceRetargetSourceName
{
namespace Retained = AssetDocumentSequenceRetargetSource;
inline constexpr const TCHAR* Primitive = TEXT("animation.sequence.retarget_source_name.set.v1");
inline constexpr const TCHAR* Schema = TEXT("animation.sequence.retarget_source_name.set.v1.input.v1");
inline constexpr const TCHAR* RollbackId = TEXT("animation.sequence.retarget_source_name.set.v1.rollback-subaction.v1");
inline FAssetDocumentContractValidationResult Result(bool Valid, const TCHAR* Code = TEXT("AnimationProvider.RetargetSourceName.InvalidState"))
{
 FAssetDocumentContractValidationResult R; R.bValid = Valid;
 if (!Valid) { auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/References/RetargetSource"); D.Code = Code;
  D.Message = TEXT("Fixed native FName, frozen readonly inputs, save-stable retained pose and public completion are required."); }
 return R;
}
inline bool ExactName(FName A, FName B) { return A.IsEqual(B, ENameCase::CaseSensitive, true); }
// The existing typed snapshot retains all other authored fields. Bind only
// its one name comparison to the current owned name; the Editor participant
// separately verifies exact old/desired FName readback. No live value is changed.
struct FRetainedAuthorState
{
 mutable Retained::FRetainedState Snapshot;
 explicit FRetainedAuthorState(const UAnimSequence& S) : Snapshot(S) {}
 bool Matches(const UAnimSequence& S) const
 { TGuardValue<FName> OwnedName(Snapshot.RetargetSource, S.RetargetSource); return Snapshot.Matches(S); }
};
struct FRetargetMapInput
{
 struct FEntry { FName Key, PoseName; TArray<FTransform> Pose; FSoftObjectPath Mesh; };
 TArray<FEntry> Entries;
 explicit FRetargetMapInput(const USkeleton& S)
 {
  for (const auto& P : S.AnimRetargetSources)
   Entries.Add({P.Key, P.Value.PoseName, P.Value.ReferencePose, P.Value.SourceReferenceMesh.ToSoftObjectPath()});
 }
 bool Matches(const USkeleton& S) const
 {
  if (S.AnimRetargetSources.Num() != Entries.Num()) return false;
  for (const auto& P : S.AnimRetargetSources)
  {
   const auto* E = Entries.FindByPredicate([&](const auto& V) { return V.Key == P.Key; });
   if (!E || !ExactName(E->Key, P.Key) || !ExactName(E->PoseName, P.Value.PoseName)
    || !Retained::SamePose(E->Pose, P.Value.ReferencePose)
    || E->Mesh.ToString() != P.Value.SourceReferenceMesh.ToSoftObjectPath().ToString()) return false;
  }
  return true;
 }
};
inline bool PoseReady(const TArray<FTransform>& Pose, int32 Bones)
{ if (Pose.Num() != Bones) return false; for (const auto& T : Pose) if (!T.IsValid()) return false; return true; }
struct FEffectiveSourceInput
{
 TArray<FTransform> Pose;
 FName Source;
};
// Freeze the selected public source pose. Both callers retain its exact inputs;
// neither reconstructs, mutates or repairs the Skeleton's retarget map.
inline FEffectiveSourceInput FreezeEffectiveSource(const UAnimSequence& S, FName Name, const TArray<FTransform>& StoredPose)
{
 return {Name.IsNone() && !StoredPose.IsEmpty() ? StoredPose : S.GetSkeleton()->GetRefLocalPoses(Name),
  Name.IsNone() && !StoredPose.IsEmpty() ? S.GetOutermost()->GetFName() : Name};
}
// Normal stock PreSave calls UpdateRetargetSourceAssetData even for a named
// source. Accept only a complete retained pose already stable under that public
// generator; this leaf never repairs or writes either retained source field.
inline bool ReadSaveStableMesh(const UAnimSequence& S, USkeletalMesh*& Mesh)
{
 Mesh = nullptr; const auto Ref = S.GetRetargetSourceAsset();
 if (Ref.IsNull()) return S.RetargetSourceAssetReferencePose.IsEmpty();
 const auto Path = Ref.ToSoftObjectPath();
 if (!Retained::CanonicalPath(Path.ToString())) return false;
 Mesh = Cast<USkeletalMesh>(Path.ResolveObject());
 if (!Retained::CanonicalMesh(Mesh, *S.GetSkeleton()) || Mesh->GetPathName() != Path.ToString()) return false;
 TArray<FTransform> Generated; FAnimationRuntime::MakeSkeletonRefPoseFromMesh(Mesh, S.GetSkeleton(), Generated);
 return Retained::SamePose(Generated, S.RetargetSourceAssetReferencePose);
}
struct FPreparedState
{
 Retained::FIdentity Target;
 Retained::FSkeletonInput Skeleton;
 FRetargetMapInput Map;
 FRetainedAuthorState Author;
 Retained::FReferenceSequenceInput Reference;
 TUniquePtr<Retained::FMeshInput> Mesh;
 TSharedPtr<IAssetDocumentEditorSequenceRetargetSourceNameMutation> Editor;
 FSoftObjectPath RetargetAsset;
 TArray<FTransform> StoredPose, OriginalEffective, DesiredEffective;
 FName OriginalSource, DesiredSource;
 FPreparedState(UAnimSequence& S, USkeletalMesh* Source, TSharedPtr<IAssetDocumentEditorSequenceRetargetSourceNameMutation> E)
  : Target(&S), Skeleton(*S.GetSkeleton(), S.RetargetSource), Map(*S.GetSkeleton()), Author(S), Reference(S.RefPoseSeq.Get()),
  Mesh(Source ? MakeUnique<Retained::FMeshInput>(*Source, *S.GetSkeleton()) : nullptr), Editor(MoveTemp(E)),
  RetargetAsset(S.GetRetargetSourceAsset().ToSoftObjectPath()), StoredPose(S.RetargetSourceAssetReferencePose),
  OriginalEffective(S.GetRetargetTransforms()), OriginalSource(S.GetRetargetTransformsSourceName())
 {
  const FName Name = Editor->GetDesiredName();
  const auto Effective = FreezeEffectiveSource(S, Name, StoredPose);
  DesiredEffective = Effective.Pose; DesiredSource = Effective.Source;
 }
 UAnimSequence* Sequence() const { return Cast<UAnimSequence>(Target.Object.Get()); }
 bool TargetCurrent() const
 { return Target.Current() && Retained::CanonicalSequence(Sequence()) && Sequence()->GetSkeleton() == Skeleton.Identity.Object.Get(); }
 bool InputsCurrent() const
 {
  const auto* S = Cast<USkeleton>(Skeleton.Identity.Object.Get());
  return Skeleton.Current() && Map.Matches(*S) && Reference.Current() && (!Mesh || Mesh->Current(*S));
 }
 bool RetainedCurrent() const
 {
  const auto* S = Sequence();
  return Author.Matches(*S) && RetargetAsset.ToString() == S->GetRetargetSourceAsset().ToSoftObjectPath().ToString()
   && Retained::SamePose(StoredPose, S->RetargetSourceAssetReferencePose);
 }
 bool EffectiveCurrent(bool Original) const
 {
  const auto* S = Sequence();
  return Retained::SamePose(Original ? OriginalEffective : DesiredEffective, S->GetRetargetTransforms())
   && ExactName(Original ? OriginalSource : DesiredSource, S->GetRetargetTransformsSourceName());
 }
 bool UnchangedInputs() const { return TargetCurrent() && InputsCurrent() && RetainedCurrent(); }
 bool Completed(bool Original) const
 {
  return UnchangedInputs() && (Original ? Editor->IsOriginalCurrent() : Editor->IsDesiredCurrent()) && EffectiveCurrent(Original)
   && Retained::CompressedCurrent(*Sequence()) && !Sequence()->HasResidency(static_cast<const ITargetPlatform*>(nullptr));
 }
 bool BaselineCurrent() const { return Completed(true); }
};
inline bool Complete(FPreparedState& S, bool Original)
{
 if (!S.UnchangedInputs() || !(Original ? S.Editor->IsOriginalCurrent() : S.Editor->IsDesiredCurrent()) || !S.EffectiveCurrent(Original)) return false;
 S.Sequence()->ClearAllCachedCookedPlatformData();
 S.Sequence()->CacheDerivedDataForCurrentPlatform(); S.Sequence()->WaitOnExistingCompression();
 return S.Completed(Original);
}
}
