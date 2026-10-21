// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Provider/Curve/AssetDocumentAnimationFloatCurveOperations.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveState.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveExactState.h"
#include "Provider/Asset/AssetDocumentAnimationPreviewMeshState.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"

namespace AssetDocumentAnimationFloatCurveMutation
{
inline constexpr const TCHAR* Primitive = TEXT("animation.curve.float.replace.v2");
inline constexpr const TCHAR* Schema = TEXT("animation.curve.float.replace.v2.input.v2");
inline constexpr const TCHAR* RollbackId = TEXT("animation.curve.float.replace.v2.rollback-subaction.v2");
using FCurveState = FAssetDocumentAnimationFloatCurveState;
using AssetDocumentAnimationSkeleton::ContextValid;
using AssetDocumentAnimationSkeleton::CanonicalPath;
using AssetDocumentAnimationSkeleton::ExactObject;
using AssetDocumentAnimationSkeleton::Footprint;
using AssetDocumentAnimationSkeleton::Acquire;
using AssetDocumentAnimationPreviewMesh::FSkeletonIdentity;
inline FAssetDocumentContractValidationResult Result(bool Valid, const TCHAR* Code = TEXT("AnimationProvider.Curve.InvalidTarget"))
{
 FAssetDocumentContractValidationResult R; R.bValid = Valid;
 if (!Valid)
 {
  auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/Curves"); D.Code = Code;
  D.Message = TEXT("Exact float-curve state, stock model identity or prepared capture is invalid.");
 }
 return R;
}
inline bool Parse(const FAssetDocumentRuntimeValueDto& Value, TArray<FCurveState>& Out)
{
 Out.Reset();
 return ValidateAssetDocumentRuntimeValue(Value).bValid && Value.Kind == EAssetDocumentRuntimeValueKind::Object
  && Value.ObjectKeys.Num() == 1 && Value.ObjectKeys[0].Equals(TEXT("curves"), ESearchCase::CaseSensitive)
  && Value.Elements.Num() == 1
  && ParseAnimationFloatCurveStateV2(Value.Elements[0], Out);
}
inline bool ExactHandles(const FAssetDocumentPrimitiveInvocation& I)
{ return I.HandleArguments.Num() == 1 && I.HandleArguments[0].Name == TEXT("asset"); }
inline bool TargetClass(const UObject* Object)
{ return ExactObject(Object, UAnimMontage::StaticClass()) || ExactObject(Object, UAnimComposite::StaticClass()); }
inline bool EqualRich(const FRichCurve& A, const FRichCurve& B)
{
 FFloatCurve Left(FName(TEXT("RetainedChannel")), 0), Right(FName(TEXT("RetainedChannel")), 0);
 Left.FloatCurve = A; Right.FloatCurve = B;
 TArray<FCurveState> X, Y;
 return CaptureAnimationFloatCurveStateV2(MakeArrayView(&Left, 1), X)
  && CaptureAnimationFloatCurveStateV2(MakeArrayView(&Right, 1), Y) && EqualAnimationFloatCurveStateV2(X, Y);
}
inline bool EqualBase(const FAnimCurveBase& A, const FAnimCurveBase& B)
{
 return A.GetName() == B.GetName() && A.GetCurveTypeFlags() == B.GetCurveTypeFlags()
  && FMemory::Memcmp(&A.Color, &B.Color, sizeof(FLinearColor)) == 0 && A.Comment.Equals(B.Comment, ESearchCase::CaseSensitive);
}
inline bool EqualVector(const FVectorCurve& A, const FVectorCurve& B)
{
 if (!EqualBase(A, B)) return false;
 for (int32 I = 0; I < 3; ++I) if (!EqualRich(A.FloatCurves[I], B.FloatCurves[I])) return false;
 return true;
}
// Retained attribute bytes are observation of an owned typed copy through its
// public serializer. They are never deserialized or used to restore authored data.
inline bool AttributeObservation(const FAnimatedBoneAttribute& Attribute, TArray<uint8>& Out)
{
 Out.Reset();
 if (!Attribute.Identifier.IsValid() || !Attribute.Curve.GetScriptStruct()) return false;
 FAttributeCurve Copy(Attribute.Curve);
 FMemoryWriter Writer(Out); FObjectAndNameAsStringProxyArchive Archive(Writer, false);
 return Copy.Serialize(Archive) && !Archive.IsError();
}
PRAGMA_DISABLE_DEPRECATION_WARNINGS // Stock model exposes raw bone arrays through these public const queries.
struct FRetainedModel
{
 TArray<FBoneAnimationTrack> Bones;
 TArray<FTransformCurve> Transforms;
 TArray<FAnimationAttributeIdentifier> AttributeIds;
 TArray<TArray<uint8>> AttributeBytes;
 TArray<TStrongObjectPtr<UScriptStruct>> AttributeTypes;
 FFrameRate FrameRate;
 int32 Frames = 0, Keys = 0;
 double Length = 0;
 bool bValid = true;
 explicit FRetainedModel(const IAnimationDataModel& Model)
  : Bones(Model.GetBoneAnimationTracks()), Transforms(Model.GetTransformCurves()),
    FrameRate(Model.GetFrameRate()), Frames(Model.GetNumberOfFrames()), Keys(Model.GetNumberOfKeys()), Length(Model.GetPlayLength())
 {
  for (const auto& Attribute : Model.GetAttributes())
  {
   AttributeIds.Add(Attribute.Identifier); AttributeTypes.Emplace(Attribute.Identifier.GetType());
   bValid &= AttributeObservation(Attribute, AttributeBytes.AddDefaulted_GetRef());
  }
 }
 bool Matches(const IAnimationDataModel& Model) const
 {
  if (!bValid || Model.GetFrameRate() != FrameRate || Model.GetNumberOfFrames() != Frames
   || Model.GetNumberOfKeys() != Keys || Model.GetPlayLength() != Length
   || Model.GetBoneAnimationTracks().Num() != Bones.Num() || Model.GetTransformCurves().Num() != Transforms.Num()
   || Model.GetAttributes().Num() != AttributeIds.Num()) return false;
  for (int32 I = 0; I < Bones.Num(); ++I)
  {
   const auto& A = Bones[I]; const auto& B = Model.GetBoneAnimationTracks()[I];
   if (A.Name != B.Name || A.BoneTreeIndex != B.BoneTreeIndex || A.InternalTrackData.PosKeys != B.InternalTrackData.PosKeys
    || A.InternalTrackData.RotKeys != B.InternalTrackData.RotKeys || A.InternalTrackData.ScaleKeys != B.InternalTrackData.ScaleKeys) return false;
  }
  for (int32 I = 0; I < Transforms.Num(); ++I)
  {
   const auto& A = Transforms[I]; const auto& B = Model.GetTransformCurves()[I];
   if (!EqualBase(A, B) || !EqualVector(A.TranslationCurve, B.TranslationCurve)
    || !EqualVector(A.RotationCurve, B.RotationCurve) || !EqualVector(A.ScaleCurve, B.ScaleCurve)) return false;
  }
  for (int32 I = 0; I < AttributeIds.Num(); ++I)
  {
   const auto& A = Model.GetAttributes()[I]; TArray<uint8> Bytes;
   if (!(A.Identifier == AttributeIds[I]) || !AttributeObservation(A, Bytes) || Bytes != AttributeBytes[I]) return false;
  }
  return true;
 }
};
PRAGMA_ENABLE_DEPRECATION_WARNINGS
struct FPrepared
{
 TStrongObjectPtr<UAnimSequenceBase> Target;
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UObject> ModelObject, ControllerObject;
 TUniquePtr<FSkeletonIdentity> Skeleton;
 TStrongObjectPtr<USkeletalMesh> Preview;
 FString TargetPath, PackagePath, ClassPath;
 uint64 TargetRevision = 0;
 float RateScale = 1.f;
 TArray<FCurveState> Old, Desired;
 TUniquePtr<FRetainedModel> Retained;
 bool bCreate = false;
 bool Bind(UAnimSequenceBase& Asset)
 {
  if (!TargetClass(&Asset) || !IsExactAnimationFloatCurveTarget(Asset)) return false;
  Target.Reset(&Asset); Package.Reset(Asset.GetOutermost()); TargetRevision = Asset.GetUniqueID();
  ModelObject.Reset(Cast<UObject>(Asset.GetDataModel())); ControllerObject.Reset(Cast<UObject>(&Asset.GetController()));
  Skeleton = MakeUnique<FSkeletonIdentity>(Asset.GetSkeleton());
  Preview.Reset(static_cast<const UAnimationAsset&>(Asset).GetPreviewMesh()); RateScale = Asset.RateScale;
  Retained = MakeUnique<FRetainedModel>(*Asset.GetDataModel());
  return Retained->bValid && CaptureAnimationFloatCurveStateV2(Asset.GetDataModel()->GetFloatCurves(), Old);
 }
 bool IdentityCurrent() const
 {
  if (!TargetClass(Target.Get()) || !IsValid(ModelObject.Get()) || !IsValid(ControllerObject.Get())) return false;
  const auto Model = Target->GetDataModelInterface();
  const auto* Controller = Cast<IAnimationDataController>(ControllerObject.Get());
  // A wrong binding must fail before Asset.GetController(), whose stock getter
  // ensures that its controller is attached to this asset's model.
  if (!Controller || Model.GetObject() != ModelObject.Get()
   || Model.GetInterface() != Cast<IAnimationDataModel>(ModelObject.Get())
   || Controller->GetModelInterface().GetObject() != ModelObject.Get()
   || Controller->GetModelInterface().GetInterface() != Model.GetInterface()
   || Controller->GetModel() != Model.GetInterface()) return false;
  return TargetClass(Target.Get()) && Target->GetPathName() == TargetPath && Target->GetClass()->GetPathName() == ClassPath
   && Target->GetUniqueID() == TargetRevision && Target->GetOutermost() == Package.Get()
   && Package->GetName() == PackagePath && FSoftObjectPath(TargetPath).ResolveObject() == Target.Get()
   && IsExactAnimationFloatCurveTarget(*Target) && Cast<UObject>(Target->GetDataModel()) == ModelObject.Get()
   && Cast<UObject>(&Target->GetController()) == ControllerObject.Get();
 }
 bool RetainedCurrent() const
 {
  return IdentityCurrent() && Skeleton->Matches(Target->GetSkeleton()) && FMemory::Memcmp(&Target->RateScale, &RateScale, sizeof(float)) == 0
   && static_cast<const UAnimationAsset&>(*Target).GetPreviewMesh() == Preview.Get() && Retained->Matches(*Target->GetDataModel());
 }
 bool BaselineCurrent() const
 {
  return bCreate && !Target.IsValid() ? FSoftObjectPath(TargetPath).ResolveObject() == nullptr
   : RetainedCurrent() && MatchesAnimationFloatCurveState(*Target, Old);
 }
};
FAssetDocumentContractValidationResult AcquirePrepared(const FAssetDocumentPrimitiveInvocation&,
 IAssetDocumentHandleResolver&, TSharedPtr<FPrepared>& OutState);
FAssetDocumentContractValidationResult MarkWrite(const FAssetDocumentPrimitiveInvocation&,
 IAssetDocumentHandleResolver&, TSharedPtr<FPrepared>& OutState);
}
