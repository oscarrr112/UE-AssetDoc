// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceNameState.h"
#include "AssetDocumentEditorSequenceCompressionMutation.h"
#include "AssetCompilingManager.h"
#include "IAssetCompilingManager.h"
#include "Animation/AnimBoneCompressionCodec.h"
#include "Animation/AnimCurveCompressionCodec.h"

namespace AssetDocumentSequenceCompression
{
namespace Retained = AssetDocumentSequenceRetargetSource;
namespace NameInputs = AssetDocumentSequenceRetargetSourceName;
inline constexpr const TCHAR* Primitive = TEXT("animation.sequence.compression.settings.set.v1");
inline constexpr const TCHAR* Schema = TEXT("animation.sequence.compression.settings.set.v1.input.v1");
inline constexpr const TCHAR* RollbackId = TEXT("animation.sequence.compression.settings.set.v1.rollback-subaction.v1");
inline FAssetDocumentContractValidationResult Result(bool Valid, const TCHAR* Code = TEXT("AnimationProvider.SequenceCompression.InvalidState"))
{
 FAssetDocumentContractValidationResult R; R.bValid = Valid;
 if (!Valid) { auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/Compression"); D.Code = Code;
  D.Message = TEXT("Supplied fixed configuration, frozen readonly assets and owned public completion are required."); }
 return R;
}
// Stock sequence task queries are protected. The actual public manager API
// exposes a readonly queue count, not a per-sequence query. Conservatively
// require its fixed animation queue to be idle; never wait/cancel foreign work.
inline bool CompressionQueueQuiescent()
{
 bool Found = false;
 const FName AnimationType(TEXT("UE-AnimationSequence"));
 for (const auto* Manager : FAssetCompilingManager::Get().GetRegisteredManagers())
 {
  if (!Manager) return false;
  if (!Manager->GetAssetTypeName().IsEqual(AnimationType, ENameCase::CaseSensitive, true)) continue;
  if (Found || Manager->GetNumRemainingAssets() != 0) return false;
  Found = true;
 }
 return Found;
}
inline bool UsableCurrent(const UAnimSequence& S)
{
 if (!CompressionQueueQuiescent() || !Retained::CompressedCurrent(S)) return false;
 const auto Data = S.GetCompressedData();
 // Stock IsCurveDataValid also accepts an additive sequence with no curve
 // stream. This supplied-settings leaf must prove an actual selected codec
 // and stream whenever its unchanged model contains authored float curves.
 return S.GetDataModel()->GetNumberOfFloatCurves() == 0
  || (Data.Get().CurveCompressionCodec != nullptr && !Data.Get().CompressedCurveByteStream.IsEmpty() && Data.Get().IsCurveDataValid(&S));
}
// This is a readonly receipt of compression-effective public settings content,
// not a whole-asset checksum or a copied compression implementation. Each old
// and new object is observed independently of the live target's selected refs.
struct FSettingsInput
{
 Retained::FIdentity Identity;
 TArray<Retained::FIdentity> Owned;
 TArray<EObjectFlags> Flags;
 TArray<UObject*> CodecMembers;
 FString Receipt;
 bool bDirty, bReady = false;
 static bool Digest(UObject* O, const UAnimSequence& S, FString& Out)
 {
  FArcToHexString Arc;
  if (auto* Bone = Cast<UAnimBoneCompressionSettings>(O))
  {
   if (!Bone->AreSettingsValid()) return false;
   Bone->PopulateDDCKey(UE::Anim::Compression::FAnimDDCKeyArgs(S), Arc.Ar);
  }
  else if (auto* Curve = Cast<UAnimCurveCompressionSettings>(O))
  {
   if (!Curve->AreSettingsValid()) return false;
   Curve->PopulateDDCKey(Arc.Ar);
  }
  else if (auto* Variable = Cast<UVariableFrameStrippingSettings>(O))
  {
   // PopulateDDCKey writes no content at nullptr platform. These public typed
   // serializers retain both Default and the complete PerPlatform map.
   auto Use = Variable->UseVariableFrameStripping; auto Rate = Variable->FrameStrippingRate;
   Use.Serialize(Arc.Ar); Rate.Serialize(Arc.Ar);
  }
  else return false;
  Out = Arc.MakeString(); return !Arc.Ar.IsError();
 }
 static TArray<UObject*> ReadCodecMembers(UObject* O)
 {
  TArray<UObject*> R;
  if (const auto* B = Cast<UAnimBoneCompressionSettings>(O)) for (const auto& C : B->Codecs) R.Add(C.Get());
  else if (const auto* C = Cast<UAnimCurveCompressionSettings>(O)) R.Add(C->Codec.Get());
  return R;
 }
 FSettingsInput(UObject* O, const UAnimSequence& S) : Identity(O), CodecMembers(ReadCodecMembers(O)), bDirty(O->GetOutermost()->IsDirty())
 {
  TArray<UObject*> Objects{O}; TArray<UObject*> Children;
  FReferenceFinder(Children, O, false, true, true, true).FindReferences(O);
  for (auto* C : Children) Objects.AddUnique(C);
  bReady = O->GetOutermost() != S.GetOutermost();
  for (auto* C : Objects)
  {
   if (!Retained::Loaded(C)) { bReady = false; continue; }
   Owned.Emplace(C); Flags.Add(C->GetFlags());
  }
  if (bReady) bReady = Digest(O, S, Receipt);
 }
 bool Current(const UAnimSequence& S) const
 {
  if (!bReady || !Identity.Current() || Identity.Package->IsDirty() != bDirty
   || CodecMembers != ReadCodecMembers(Identity.Object.Get())) return false;
  TArray<UObject*> Objects{Identity.Object.Get()}, Children;
  FReferenceFinder(Children, Identity.Object.Get(), false, true, true, true).FindReferences(Identity.Object.Get());
  for (auto* C : Children) Objects.AddUnique(C);
  if (Objects.Num() != Owned.Num()) return false;
  for (int32 I = 0; I < Owned.Num(); ++I)
   if (!Owned[I].Current() || !Objects.Contains(Owned[I].Object.Get()) || Owned[I].Object->GetFlags() != Flags[I]) return false;
  FString Now; return Digest(Identity.Object.Get(), S, Now) && Receipt.Equals(Now, ESearchCase::CaseSensitive);
 }
};
// All non-compression authored state stays independently frozen. The five
// owned values are checked by the fixed Editor participant, never masked in a
// shared predecessor snapshot. No retained value is written by this observer.
struct FRetainedAuthorState
{
 Retained::FIdentity ModelIdentity;
 const IAnimationDataModel* Model;
 FGuid Guid;
 FFrameRate Rate, SamplingRate;
 TArray<FName> BoneNames;
 TArray<FFloatCurve> FloatCurves;
 TArray<FTransformCurve> TransformCurves;
 TArray<FAnimNotifyEvent> Notifies;
 TArray<FAnimNotifyTrack> Tracks;
 TArray<FAnimSyncMarker> Markers;
 TArray<UObject*> Metadata, UserData;
 TArray<Retained::FIdentity> Anchors;
 FSoftObjectPtr Preview;
 TObjectPtr<UAssetImportData> ImportData;
 USkeleton* Skeleton;
 FGuid SkeletonGuid;
 FName RetargetSource;
 UAnimSequence* Reference;
 EAdditiveAnimationType Additive;
 EAdditiveBasePoseType Base;
 float Length, Playback;
 int32 Frames, Keys, CompressionVersion, Frame;
 EAnimInterpolationType Interpolation;
 ERootMotionRootLock::Type RootLock;
 EStripAnimDataOnDedicatedServerSettings StripOnServer;
 bool bLoop, bRootMotion, bForceRoot, bNormalizedRoot, bRootFromMontage, bFrameStripping, bReady;
 explicit FRetainedAuthorState(const UAnimSequence& S) : ModelIdentity(S.GetDataModelInterface().GetObject()), Model(S.GetDataModel()),
  Guid(Model->GenerateGuid()), Rate(Model->GetFrameRate()), SamplingRate(S.GetSamplingFrameRate()), FloatCurves(Model->GetFloatCurves()),
  TransformCurves(Model->GetTransformCurves()), Notifies(S.Notifies), Tracks(S.AnimNotifyTracks), Markers(S.AuthoredSyncMarkers),
  ImportData(S.AssetImportData), Skeleton(S.GetSkeleton()), SkeletonGuid(S.GetSkeletonGuid()), RetargetSource(S.RetargetSource),
  Reference(S.RefPoseSeq.Get()), Additive(S.AdditiveAnimType.GetValue()), Base(S.RefPoseType.GetValue()), Length(S.GetPlayLength()),
  Playback(S.RateScale), Frames(Model->GetNumberOfFrames()), Keys(Model->GetNumberOfKeys()), CompressionVersion(S.CompressCommandletVersion),
  Frame(S.RefFrameIndex), Interpolation(S.Interpolation), RootLock(S.RootMotionRootLock), StripOnServer(S.StripAnimDataOnDedicatedServer),
  bLoop(S.bLoop), bRootMotion(S.bEnableRootMotion), bForceRoot(S.bForceRootLock), bNormalizedRoot(S.bUseNormalizedRootMotionScale),
  bRootFromMontage(S.bRootMotionSettingsCopiedFromMontage), bFrameStripping(S.bAllowFrameStripping), bReady(Retained::ReadRetainedPreview(S, Preview))
 {
  Model->GetBoneTrackNames(BoneNames);
  for (auto* O : S.GetMetaData()) Metadata.Add(O);
  if (const auto* D = S.GetAssetUserDataArray()) for (auto* O : *D) UserData.Add(O);
  TArray<UObject*> Objects = Metadata; Objects.Append(UserData); Objects.Add(ImportData.Get());
  Objects.Add(Preview.ToSoftObjectPath().ResolveObject());
  for (const auto& N : Notifies) { Objects.Add(N.Notify.Get()); Objects.Add(N.NotifyStateClass.Get()); }
  TSet<UObject*> Seen;
  for (auto* O : Objects) if (O && !Seen.Contains(O)) { Seen.Add(O); bReady &= Retained::Loaded(O); Anchors.Emplace(O); }
 }
 bool Matches(const UAnimSequence& S) const
 {
  if (!bReady || !ModelIdentity.Current() || S.GetDataModel() != Model || S.GetDataModelInterface().GetObject() != ModelIdentity.Object.Get()) return false;
  for (const auto& A : Anchors) if (!A.Current()) return false;
  TArray<UObject*> M, D; TArray<FName> Names; FSoftObjectPtr CurrentPreview;
  Model->GetBoneTrackNames(Names);
  for (auto* O : S.GetMetaData()) M.Add(O);
  if (const auto* Data = S.GetAssetUserDataArray()) for (auto* O : *Data) D.Add(O);
  return Model->GenerateGuid() == Guid && Model->GetFrameRate() == Rate && S.GetSamplingFrameRate() == SamplingRate
   && Model->GetNumberOfFrames() == Frames && Model->GetNumberOfKeys() == Keys && Names == BoneNames
   && Retained::SameStructs(FloatCurves, Model->GetFloatCurves()) && Retained::SameStructs(TransformCurves, Model->GetTransformCurves())
   && Retained::SameStructs(Notifies, S.Notifies) && Retained::SameStructs(Tracks, S.AnimNotifyTracks) && Markers == S.AuthoredSyncMarkers
   && M == Metadata && D == UserData && Retained::ReadRetainedPreview(S, CurrentPreview) && Preview == CurrentPreview
   && ImportData == S.AssetImportData && Skeleton == S.GetSkeleton() && SkeletonGuid == S.GetSkeletonGuid()
   && NameInputs::ExactName(RetargetSource, S.RetargetSource) && Reference == S.RefPoseSeq.Get()
   && Additive == S.AdditiveAnimType.GetValue() && Base == S.RefPoseType.GetValue() && Frame == S.RefFrameIndex
   && Length == S.GetPlayLength() && Playback == S.RateScale && CompressionVersion == S.CompressCommandletVersion
   && Interpolation == S.Interpolation && RootLock == S.RootMotionRootLock && StripOnServer == S.StripAnimDataOnDedicatedServer
   && bLoop == S.bLoop && bRootMotion == S.bEnableRootMotion && bForceRoot == S.bForceRootLock
   && bNormalizedRoot == S.bUseNormalizedRootMotionScale && bRootFromMontage == S.bRootMotionSettingsCopiedFromMontage
   && bFrameStripping == S.bAllowFrameStripping;
 }
};
struct FPreparedState
{
 Retained::FIdentity Target;
 Retained::FSkeletonInput Skeleton;
 NameInputs::FRetargetMapInput Map;
 FRetainedAuthorState Author;
 Retained::FReferenceSequenceInput Reference;
 TUniquePtr<Retained::FMeshInput> Mesh;
 TArray<FSettingsInput> Settings;
 TSharedPtr<IAssetDocumentEditorSequenceCompressionMutation> Editor;
 FSoftObjectPath RetargetAsset;
 TArray<FTransform> StoredPose, EffectivePose;
 FName EffectiveSource;
 FPreparedState(UAnimSequence& S, USkeletalMesh* M, TSharedPtr<IAssetDocumentEditorSequenceCompressionMutation> E)
  : Target(&S), Skeleton(*S.GetSkeleton(), S.RetargetSource), Map(*S.GetSkeleton()), Author(S), Reference(S.RefPoseSeq.Get()),
  Mesh(M ? MakeUnique<Retained::FMeshInput>(*M, *S.GetSkeleton()) : nullptr), Editor(MoveTemp(E)),
  RetargetAsset(S.GetRetargetSourceAsset().ToSoftObjectPath()), StoredPose(S.RetargetSourceAssetReferencePose),
  EffectivePose(S.GetRetargetTransforms()), EffectiveSource(S.GetRetargetTransformsSourceName()) {}
 UAnimSequence* Sequence() const { return Cast<UAnimSequence>(Target.Object.Get()); }
 void AddSettings(UObject* O)
 { if (O && !Settings.ContainsByPredicate([&](const auto& V) { return V.Identity.Object.Get() == O; })) Settings.Emplace(O, *Sequence()); }
 bool UnchangedInputs() const
 {
  const auto* S = Sequence(); const auto* Sk = Cast<USkeleton>(Skeleton.Identity.Object.Get());
  if (!Target.Current() || !Retained::CanonicalSequence(S) || !Skeleton.Current() || !Map.Matches(*Sk)
   || !Reference.Current() || (Mesh && !Mesh->Current(*Sk)) || !Author.Matches(*S)
   || !RetargetAsset.ToString().Equals(S->GetRetargetSourceAsset().ToSoftObjectPath().ToString(), ESearchCase::CaseSensitive)
   || !Retained::SamePose(StoredPose, S->RetargetSourceAssetReferencePose) || !Retained::SamePose(EffectivePose, S->GetRetargetTransforms())
   || !NameInputs::ExactName(EffectiveSource, S->GetRetargetTransformsSourceName())) return false;
  // Public completion can preload its fixed roots and owned objects. Admit
  // only a fully loaded closure, including a quiescent retained reference.
  TArray<UObject*> Dependencies;
  if (!Retained::CompressionDependencies(*S, Dependencies) || !CompressionQueueQuiescent()) return false;
  for (const auto& Input : Settings) if (!Input.Current(*S)) return false;
  return true;
 }
 bool Completed(bool Original) const
 {
  return UnchangedInputs() && (Original ? Editor->IsOriginalCurrent() : Editor->IsDesiredCurrent())
   && Retained::SettingsReady(*Sequence()) && UsableCurrent(*Sequence())
   && !Sequence()->HasResidency(static_cast<const ITargetPlatform*>(nullptr));
 }
 bool BaselineCurrent() const { return Completed(true); }
};
inline bool Complete(FPreparedState& S, bool Original)
{
 if (!S.UnchangedInputs() || !(Original ? S.Editor->IsOriginalCurrent() : S.Editor->IsDesiredCurrent())) return false;
 S.Sequence()->ClearAllCachedCookedPlatformData();
 S.Sequence()->CacheDerivedDataForCurrentPlatform(); S.Sequence()->WaitOnExistingCompression();
 return S.Completed(Original);
}
}
