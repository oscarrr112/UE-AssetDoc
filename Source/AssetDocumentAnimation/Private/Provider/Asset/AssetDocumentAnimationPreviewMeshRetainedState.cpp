// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Asset/AssetDocumentAnimationPreviewMeshRetainedState.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Engine/AssetUserData.h"
#include "EditorFramework/AssetImportData.h"
#include "UObject/Class.h"

namespace AssetDocumentAnimationPreviewMesh
{
namespace
{
// Fixed native struct types only; zero port flags, no deep UObject comparison.
template <typename T> bool SameStoredStructs(const TArray<T>& A, const TArray<T>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I)
  if (!T::StaticStruct()->CompareScriptStruct(&A[I], &B[I], 0)) return false;
 return true;
}
bool SameStoredTransforms(const TArray<FTransform>& A, const TArray<FTransform>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I)
  if (A[I].GetTranslation() != B[I].GetTranslation() || A[I].GetRotation() != B[I].GetRotation()
   || A[I].GetScale3D() != B[I].GetScale3D()) return false;
 return true;
}
TArray<FAnimCurveBase> CurveDecorations(const IAnimationDataModel* Model)
{
 TArray<FAnimCurveBase> Result;
 if (Model)
 {
  for (const auto& Curve : Model->GetFloatCurves()) Result.Add(static_cast<const FAnimCurveBase&>(Curve));
  for (const auto& Curve : Model->GetTransformCurves()) Result.Add(static_cast<const FAnimCurveBase&>(Curve));
 }
 return Result;
}
TArray<UObject*> SettingsReferences(const UAnimSequence& Target)
{
 return {Target.RefPoseSeq.Get(), Target.BoneCompressionSettings.Get(), Target.CurveCompressionSettings.Get(),
  Target.VariableFrameStrippingSettings.Get(), Target.AssetImportData.Get()};
}
}
struct FRetainedState::FSequenceState
{
 TStrongObjectPtr<UObject> ModelObject;
 const IAnimationDataModel* Model;
 FGuid ContentGuid;
 FFrameRate FrameRate;
 TArray<FName> BoneNames;
 TArray<FAnimCurveBase> Curves;
 TArray<FAnimNotifyEvent> Notifies;
 TArray<FAnimNotifyTrack> Tracks;
 TArray<FAnimSyncMarker> Markers;
 TArray<UObject*> Settings, Metadata, UserData;
 TArray<TStrongObjectPtr<UObject>> Anchors;
 TSoftObjectPtr<USkeletalMesh> RetargetAsset;
 TArray<FTransform> RetargetPose;
 FName RetargetSource;
 float PlayLength, RateScale, CompressionError;
 int32 Frames, Keys, FloatCurves, RefFrame, CompressionVersion;
 EAnimInterpolationType Interpolation;
 EAdditiveAnimationType Additive;
 EAdditiveBasePoseType RefPose;
 ERootMotionRootLock::Type RootLock;
 EStripAnimDataOnDedicatedServerSettings StripOnServer;
 bool bLoop, bRootMotion, bForceRootLock, bNormalizedRootMotion, bRootFromMontage, bFrameStripping, bKeepCompression;
 explicit FSequenceState(const UAnimSequence& Target)
  : ModelObject(Target.GetDataModelInterface().GetObject()), Model(Target.GetDataModel()),
    ContentGuid(Model ? Model->GenerateGuid() : FGuid()), FrameRate(Model ? Model->GetFrameRate() : FFrameRate()),
    Curves(CurveDecorations(Model)), Notifies(Target.Notifies), Tracks(Target.AnimNotifyTracks), Markers(Target.AuthoredSyncMarkers),
    Settings(SettingsReferences(Target)), RetargetAsset(Target.GetRetargetSourceAsset()), RetargetPose(Target.RetargetSourceAssetReferencePose),
    RetargetSource(Target.RetargetSource), PlayLength(Target.GetPlayLength()), RateScale(Target.RateScale),
    CompressionError(Target.CompressionErrorThresholdScale), Frames(Model ? Model->GetNumberOfFrames() : 0),
    Keys(Model ? Model->GetNumberOfKeys() : 0), FloatCurves(Model ? Model->GetNumberOfFloatCurves() : 0),
    RefFrame(Target.RefFrameIndex), CompressionVersion(Target.CompressCommandletVersion), Interpolation(Target.Interpolation),
    Additive(Target.AdditiveAnimType), RefPose(Target.RefPoseType), RootLock(Target.RootMotionRootLock),
    StripOnServer(Target.StripAnimDataOnDedicatedServer), bLoop(Target.bLoop), bRootMotion(Target.bEnableRootMotion),
    bForceRootLock(Target.bForceRootLock), bNormalizedRootMotion(Target.bUseNormalizedRootMotionScale),
    bRootFromMontage(Target.bRootMotionSettingsCopiedFromMontage), bFrameStripping(Target.bAllowFrameStripping),
    bKeepCompression(Target.bDoNotOverrideCompression)
 {
  if (Model) Model->GetBoneTrackNames(BoneNames);
  for (auto* Object : Target.GetMetaData()) Metadata.Add(Object);
  if (const auto* Objects = Target.GetAssetUserDataArray()) for (auto* Object : *Objects) UserData.Add(Object);
  for (const auto* Objects : {&Settings, &Metadata, &UserData}) for (auto* Object : *Objects) Anchors.Emplace(Object);
  for (const auto& Notify : Notifies) { Anchors.Emplace(Notify.Notify.Get()); Anchors.Emplace(Notify.NotifyStateClass.Get()); }
 }
 bool Matches(const UAnimSequence& Target) const
 {
  const auto* Current = Target.GetDataModel();
  TArray<FName> Names; if (Current) Current->GetBoneTrackNames(Names);
  TArray<UObject*> CurrentMetadata, CurrentUserData;
  for (auto* Object : Target.GetMetaData()) CurrentMetadata.Add(Object);
  if (const auto* Objects = Target.GetAssetUserDataArray()) for (auto* Object : *Objects) CurrentUserData.Add(Object);
  return Current == Model && Target.GetDataModelInterface().GetObject() == ModelObject.Get()
   && (!Current || (Current->GenerateGuid() == ContentGuid && Current->GetFrameRate() == FrameRate
    && Current->GetNumberOfFrames() == Frames && Current->GetNumberOfKeys() == Keys
    && Current->GetNumberOfFloatCurves() == FloatCurves))
   && Names == BoneNames && SameStoredStructs(Curves, CurveDecorations(Current))
   && SameStoredStructs(Notifies, Target.Notifies) && SameStoredStructs(Tracks, Target.AnimNotifyTracks)
   && Markers == Target.AuthoredSyncMarkers && Settings == SettingsReferences(Target)
   && Metadata == CurrentMetadata && UserData == CurrentUserData
   && RetargetAsset == Target.GetRetargetSourceAsset() && SameStoredTransforms(RetargetPose, Target.RetargetSourceAssetReferencePose)
   && RetargetSource == Target.RetargetSource && PlayLength == Target.GetPlayLength() && RateScale == Target.RateScale
   && bLoop == Target.bLoop && Interpolation == Target.Interpolation && Additive == Target.AdditiveAnimType
   && RefPose == Target.RefPoseType && RefFrame == Target.RefFrameIndex && bRootMotion == Target.bEnableRootMotion
   && RootLock == Target.RootMotionRootLock && bForceRootLock == Target.bForceRootLock
   && bNormalizedRootMotion == Target.bUseNormalizedRootMotionScale && bRootFromMontage == Target.bRootMotionSettingsCopiedFromMontage
   && bFrameStripping == Target.bAllowFrameStripping && CompressionError == Target.CompressionErrorThresholdScale
   && bKeepCompression == Target.bDoNotOverrideCompression && CompressionVersion == Target.CompressCommandletVersion
   && StripOnServer == Target.StripAnimDataOnDedicatedServer;
 }
};
FRetainedState::FRetainedState(const UAnimationAsset& Target)
{
 if (Target.GetClass() == UAnimMontage::StaticClass())
  Montage = MakeUnique<AssetDocumentAnimationSkeleton::FRetainedState>(static_cast<const UAnimMontage&>(Target));
 else if (Target.GetClass() == UAnimSequence::StaticClass())
  Sequence = MakeUnique<FSequenceState>(static_cast<const UAnimSequence&>(Target));
}
FRetainedState::~FRetainedState() = default;
bool FRetainedState::Matches(const UAnimationAsset& Target) const
{
 return (Montage && Target.GetClass() == UAnimMontage::StaticClass() && Montage->Matches(static_cast<const UAnimMontage&>(Target)))
  || (Sequence && Target.GetClass() == UAnimSequence::StaticClass() && Sequence->Matches(static_cast<const UAnimSequence&>(Target)));
}
}
