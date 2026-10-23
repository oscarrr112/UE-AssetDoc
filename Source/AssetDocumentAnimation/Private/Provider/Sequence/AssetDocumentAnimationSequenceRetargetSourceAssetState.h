// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimCompressionTypes.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AnimationRuntime.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/AssetUserData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "HAL/PlatformProperties.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonState.h"
#include "UObject/PerPlatformProperties.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentSequenceRetargetSource
{
inline constexpr const TCHAR* Primitive = TEXT("animation.sequence.retarget_source_asset.set.v2");
inline constexpr const TCHAR* Schema = TEXT("animation.sequence.retarget_source_asset.set.v2.input.v2");
inline constexpr const TCHAR* RollbackId = TEXT("animation.sequence.retarget_source_asset.set.v2.rollback-subaction.v1");
using AssetDocumentAnimationSkeleton::CanonicalPath;
using AssetDocumentAnimationSkeleton::Footprint;
inline bool ContextValid()
{
 return AssetDocumentAnimationSkeleton::ContextValid() && !IsRunningCookCommandlet()
  && !FPlatformProperties::RequiresCookedData();
}
inline FAssetDocumentContractValidationResult Result(bool bValid,
 const TCHAR* Code = TEXT("AnimationProvider.RetargetSource.InvalidState"))
{
 FAssetDocumentContractValidationResult R; R.bValid = bValid;
 if (!bValid)
 {
  auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/References/RetargetSourceAsset"); D.Code = Code;
  D.Message = TEXT("Exact prepared native reference, pose, identities and public compression completion are required.");
 }
 return R;
}
inline bool Loaded(const UObject* O)
{
 return IsValid(O) && !O->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects)
  && !O->HasAnyInternalFlags(EInternalObjectFlags::AsyncLoadingPhase1);
}
inline bool CanonicalAsset(const UObject* O, UClass* C, bool bReferenceSubtype = false)
{
 return Loaded(O) && !O->IsTemplate() && !O->HasAnyFlags(RF_Transient)
  && (bReferenceSubtype ? O->IsA(C) : O->GetClass() == C)
  && O->GetOuter() == O->GetOutermost() && CanonicalPath(O->GetPathName())
  && FSoftObjectPath(O->GetPathName()).ResolveObject() == O && Loaded(O->GetOutermost());
}
inline bool SamePose(const TArray<FTransform>& A, const TArray<FTransform>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I)
  if (A[I].GetTranslation() != B[I].GetTranslation() || A[I].GetRotation() != B[I].GetRotation()
   || A[I].GetScale3D() != B[I].GetScale3D()) return false;
 return true;
}
template <typename T> bool SameStructs(const TArray<T>& A, const TArray<T>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I)
  if (!T::StaticStruct()->CompareScriptStruct(&A[I], &B[I], 0)) return false;
 return true;
}
struct FIdentity
{
 TStrongObjectPtr<UObject> Object;
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UObject> Outer;
 TStrongObjectPtr<UClass> Class;
 FString Path, PackagePath;
 uint32 Revision;
 explicit FIdentity(UObject* O) : Object(O), Package(O->GetOutermost()), Outer(O->GetOuter()), Class(O->GetClass()),
  Path(O->GetPathName()), PackagePath(O->GetOutermost()->GetName()), Revision(O->GetUniqueID()) {}
 bool Current() const
 {
  return Loaded(Object.Get()) && Loaded(Package.Get()) && Object->GetClass() == Class.Get()
   && Object->GetUniqueID() == Revision && Object->GetPathName() == Path && Object->GetOuter() == Outer.Get()
   && Object->GetOutermost() == Package.Get() && Package->GetName() == PackagePath;
 }
};
// Public typed reference inputs, including the name-map observations consumed
// by Engine pose regeneration. This is a snapshot, never a pose implementation.
struct FReferenceInput
{
 TArray<FMeshBoneInfo> RawInfo, Info;
 TArray<FTransform> RawPose, Pose;
 TArray<int32> Lookup;
 explicit FReferenceInput(const FReferenceSkeleton& R)
  : RawInfo(R.GetRawRefBoneInfo()), Info(R.GetRefBoneInfo()), RawPose(R.GetRawRefBonePose()), Pose(R.GetRefBonePose())
 { for (const auto& B : Info) Lookup.Add(R.FindBoneIndex(B.Name)); }
 static bool SameInfo(const TArray<FMeshBoneInfo>& A, const TArray<FMeshBoneInfo>& B)
 {
  if (A.Num() != B.Num()) return false;
  for (int32 I = 0; I < A.Num(); ++I)
   if (A[I].Name != B[I].Name || A[I].ParentIndex != B[I].ParentIndex || A[I].ExportName != B[I].ExportName) return false;
  return true;
 }
 bool Matches(const FReferenceSkeleton& R) const
 {
  if (!SameInfo(RawInfo, R.GetRawRefBoneInfo()) || !SameInfo(Info, R.GetRefBoneInfo())
   || !SamePose(RawPose, R.GetRawRefBonePose()) || !SamePose(Pose, R.GetRefBonePose())) return false;
  for (int32 I = 0; I < Info.Num(); ++I) if (Lookup[I] != R.FindBoneIndex(Info[I].Name)) return false;
  return true;
 }
};
inline bool ReferenceReady(const FReferenceSkeleton& R)
{
 // Bounds for public helpers that index these native arrays. This validates
 // the source structure only; Engine remains the sole pose generator.
 const auto& Info = R.GetRefBoneInfo(); const auto& Pose = R.GetRefBonePose();
 if (Info.IsEmpty() || Info.Num() != Pose.Num() || R.GetRawRefBoneInfo().Num() != R.GetRawRefBonePose().Num()) return false;
 for (int32 I = 0; I < Info.Num(); ++I)
  if (Info[I].ParentIndex < INDEX_NONE || Info[I].ParentIndex >= I || Info[I].Name.IsNone()
   || R.FindBoneIndex(Info[I].Name) != I || !Pose[I].IsValid()) return false;
 return true;
}
inline bool CanonicalSequence(const UAnimSequence* S)
{
 if (!CanonicalAsset(S, UAnimSequence::StaticClass()) || !S->IsDataModelValid()
  || !Loaded(S->GetDataModelInterface().GetObject()) || !S->GetDataModel() || S->GetDataModel()->GetNumberOfKeys() <= 0) return false;
 const auto* Skeleton = S->GetSkeleton();
 return CanonicalAsset(Skeleton, USkeleton::StaticClass(), true) && S->GetSkeletonGuid() == Skeleton->GetGuid()
  && ReferenceReady(Skeleton->GetReferenceSkeleton());
}
inline bool CanonicalMesh(const USkeletalMesh* M, const USkeleton& S)
{
 return CanonicalAsset(M, USkeletalMesh::StaticClass(), true) && !M->IsCompiling() && ReferenceReady(M->GetRefSkeleton()) && S.IsCompatibleMesh(M);
}
struct FSkeletonInput
{
 FIdentity Identity;
 FGuid Guid, VirtualGuid;
 FReferenceInput Reference;
 FName Name;
 TArray<FTransform> NamedPose;
 TArray<FVirtualBone> VirtualBones;
 TArray<EBoneTranslationRetargetingMode::Type> TranslationModes;
 TArray<FIdentity> Sockets;
 TArray<FName> SocketBones;
 bool bDirty;
 FSkeletonInput(USkeleton& S, FName InName) : Identity(&S), Guid(S.GetGuid()), VirtualGuid(S.GetVirtualBoneGuid()),
  Reference(S.GetReferenceSkeleton()), Name(InName), NamedPose(S.GetRefLocalPoses(Name)),
  VirtualBones(S.GetVirtualBones()), bDirty(S.GetOutermost()->IsDirty())
 {
  for (int32 I = 0; I < Reference.Info.Num(); ++I) TranslationModes.Add(S.GetBoneTranslationRetargetingMode(I));
  for (const auto& Socket : S.Sockets) if (Socket) { Sockets.Emplace(Socket.Get()); SocketBones.Add(Socket->BoneName); }
 }
 bool Current() const
 {
  const auto* S = Cast<USkeleton>(Identity.Object.Get());
  if (!Identity.Current() || !CanonicalAsset(S, USkeleton::StaticClass(), true) || S->GetGuid() != Guid
   || S->GetVirtualBoneGuid() != VirtualGuid || !Reference.Matches(S->GetReferenceSkeleton())
   || !SamePose(NamedPose, S->GetRefLocalPoses(Name)) || !SameStructs(VirtualBones, S->GetVirtualBones())
   || S->GetOutermost()->IsDirty() != bDirty || S->Sockets.Num() != Sockets.Num()) return false;
  for (int32 I = 0; I < Sockets.Num(); ++I)
   if (!Sockets[I].Current() || Sockets[I].Object.Get() != S->Sockets[I] || SocketBones[I] != S->Sockets[I]->BoneName) return false;
  for (int32 I = 0; I < TranslationModes.Num(); ++I)
   if (TranslationModes[I] != S->GetBoneTranslationRetargetingMode(I)) return false;
  return true;
 }
};
struct FMeshInput
{
 FIdentity Identity;
 FReferenceInput Reference;
 TStrongObjectPtr<USkeleton> MeshSkeleton;
 TArray<int32> SkeletonLookup;
 bool bDirty;
 FMeshInput(USkeletalMesh& M, const USkeleton& S) : Identity(&M), Reference(M.GetRefSkeleton()),
  MeshSkeleton(M.GetSkeleton()), bDirty(M.GetOutermost()->IsDirty())
 { for (const auto& B : S.GetReferenceSkeleton().GetRefBoneInfo()) SkeletonLookup.Add(M.GetRefSkeleton().FindBoneIndex(B.Name)); }
 bool Current(const USkeleton& S) const
 {
  const auto* M = Cast<USkeletalMesh>(Identity.Object.Get());
  if (!Identity.Current() || !CanonicalMesh(M, S) || M->GetSkeleton() != MeshSkeleton.Get()
   || !Reference.Matches(M->GetRefSkeleton()) || M->GetOutermost()->IsDirty() != bDirty
   || SkeletonLookup.Num() != S.GetReferenceSkeleton().GetRefBoneInfo().Num()) return false;
  for (int32 I = 0; I < SkeletonLookup.Num(); ++I)
   if (SkeletonLookup[I] != M->GetRefSkeleton().FindBoneIndex(S.GetReferenceSkeleton().GetRefBoneInfo()[I].Name)) return false;
  return true;
 }
};
inline bool CompressedCurrent(const UAnimSequence& S)
{
 if (!S.CanBeCompressed() || !S.IsCompressedDataValid() || S.IsCompressedDataOutOfDate()) return false;
 const FIoHash Hash = S.GetDerivedDataKeyHash(nullptr);
 if (Hash.IsZero() || !S.HasCompressedDataForPlatform(nullptr)) return false;
 FAnimExtractContext C(0.0); C.bEnforceCompressedDataSampling = true; C.TargetPlatformHash = Hash;
 const auto Normal = S.GetCompressedData(); const auto ByHash = S.GetCompressedData(C);
 return &Normal.Get() == &ByHash.Get();
}
inline TArray<FAnimCurveBase> CurveDecorations(const IAnimationDataModel& Model)
{
 TArray<FAnimCurveBase> R;
 for (const auto& C : Model.GetFloatCurves()) R.Add(static_cast<const FAnimCurveBase&>(C));
 for (const auto& C : Model.GetTransformCurves()) R.Add(static_cast<const FAnimCurveBase&>(C));
 return R;
}
inline bool SettingsReady(const UAnimSequence& S)
{
 return Loaded(S.BoneCompressionSettings) && Loaded(S.CurveCompressionSettings) && Loaded(S.VariableFrameStrippingSettings)
  && S.BoneCompressionSettings->AreSettingsValid() && S.CurveCompressionSettings->AreSettingsValid();
}
// ASSETDOCUMENT_READONLY_AUTHORITY_EXCEPTION(AD.ANIMATION.SEQUENCE-RETARGET-RETAINED-PREVIEW-v1)
// Fixed observation of an unrelated retained soft reference. The public getter
// may load or repair; this probe does neither, grants no write authority, and
// exposes no property name or soft path to a request/runtime value.
inline bool ReadRetainedPreview(const UAnimSequence& S, FSoftObjectPtr& Out)
{
 const auto* P = FindFProperty<FSoftObjectProperty>(UAnimationAsset::StaticClass(), TEXT("PreviewSkeletalMesh"));
 if (!P || P->GetOwnerStruct() != UAnimationAsset::StaticClass() || P->PropertyClass != USkeletalMesh::StaticClass()
  || P->ArrayDim != 1) return false;
 Out = P->GetPropertyValue_InContainer(&S); return true;
}
inline bool SettingsReceipt(const UAnimSequence& S, TArray<FString>& Out)
{
 if (!SettingsReady(S)) return false;
 FArcToHexString B, C, V;
 S.BoneCompressionSettings->PopulateDDCKey(UE::Anim::Compression::FAnimDDCKeyArgs(S), B.Ar);
 S.CurveCompressionSettings->PopulateDDCKey(C.Ar);
 // Variable PopulateDDCKey writes nothing for a null TargetPlatform. Its
 // public typed serializer includes both Default and the PerPlatform map.
 auto Use = S.VariableFrameStrippingSettings->UseVariableFrameStripping;
 auto Rate = S.VariableFrameStrippingSettings->FrameStrippingRate;
 Use.Serialize(V.Ar); Rate.Serialize(V.Ar);
 Out = {B.MakeString(), C.MakeString(), V.MakeString()};
 return !B.Ar.IsError() && !C.Ar.IsError() && !V.Ar.IsError();
}
// Match the stock completion's six fixed roots and each root's owned objects;
// this never follows arbitrary external assets or runs a loading operation.
inline bool CompressionDependencies(const UAnimSequence& S, TArray<UObject*>& Out)
{
 Out.Reset();
 for (UObject* Root : {static_cast<UObject*>(const_cast<UAnimSequence*>(&S)), static_cast<UObject*>(S.GetSkeleton()),
  static_cast<UObject*>(S.BoneCompressionSettings.Get()), static_cast<UObject*>(S.CurveCompressionSettings.Get()),
  static_cast<UObject*>(S.RefPoseSeq.Get()), static_cast<UObject*>(S.VariableFrameStrippingSettings.Get())})
 {
  if (!Root) continue;
  if (!Loaded(Root)) return false;
  Out.AddUnique(Root); TArray<UObject*> Owned;
  FReferenceFinder(Owned, Root, false, true, true, true).FindReferences(Root);
  for (auto* O : Owned) { if (!Loaded(O)) return false; Out.AddUnique(O); }
 }
 return true;
}
// Everything authored except the two owned retarget-source fields. Model
// content is observed through its native GUID; no retained value is written.
struct FRetainedState
{
 FIdentity ModelIdentity;
 const IAnimationDataModel* Model;
 FGuid Guid;
 FFrameRate Rate, SamplingRate;
 TArray<FName> BoneNames;
 TArray<FAnimCurveBase> Curves;
 TArray<FAnimNotifyEvent> Notifies;
 TArray<FAnimNotifyTrack> Tracks;
 TArray<FAnimSyncMarker> Markers;
 TArray<UObject*> Settings, Metadata, UserData, Dependencies;
 TArray<FIdentity> Anchors;
 TArray<FString> Receipts;
 FSoftObjectPtr Preview;
 USkeleton* Skeleton;
 FGuid SkeletonGuid;
 FName RetargetSource;
 UAnimSequence* Reference;
 EAdditiveAnimationType Additive;
 EAdditiveBasePoseType Base;
 float Length, Playback, Error;
 int32 Frames, Keys, CompressionVersion, Frame;
 EAnimInterpolationType Interpolation;
 ERootMotionRootLock::Type RootLock;
 EStripAnimDataOnDedicatedServerSettings StripOnServer;
 bool bLoop, bRootMotion, bForceRoot, bNormalizedRoot, bRootFromMontage, bFrameStripping, bKeepCompression, bReady;
 static TArray<UObject*> ReadSettings(const UAnimSequence& S)
 { return {S.BoneCompressionSettings.Get(), S.CurveCompressionSettings.Get(), S.VariableFrameStrippingSettings.Get(), S.AssetImportData.Get()}; }
 explicit FRetainedState(const UAnimSequence& S) : ModelIdentity(S.GetDataModelInterface().GetObject()), Model(S.GetDataModel()),
  Guid(Model->GenerateGuid()), Rate(Model->GetFrameRate()), SamplingRate(S.GetSamplingFrameRate()), Curves(CurveDecorations(*Model)), Notifies(S.Notifies),
  Tracks(S.AnimNotifyTracks), Markers(S.AuthoredSyncMarkers), Settings(ReadSettings(S)),
  Skeleton(S.GetSkeleton()), SkeletonGuid(S.GetSkeletonGuid()), RetargetSource(S.RetargetSource), Reference(S.RefPoseSeq.Get()),
  Additive(S.AdditiveAnimType.GetValue()), Base(S.RefPoseType.GetValue()), Length(S.GetPlayLength()), Playback(S.RateScale),
  Error(S.CompressionErrorThresholdScale), Frames(Model->GetNumberOfFrames()), Keys(Model->GetNumberOfKeys()),
  CompressionVersion(S.CompressCommandletVersion), Frame(S.RefFrameIndex), Interpolation(S.Interpolation), RootLock(S.RootMotionRootLock),
  StripOnServer(S.StripAnimDataOnDedicatedServer), bLoop(S.bLoop), bRootMotion(S.bEnableRootMotion), bForceRoot(S.bForceRootLock),
  bNormalizedRoot(S.bUseNormalizedRootMotionScale), bRootFromMontage(S.bRootMotionSettingsCopiedFromMontage),
  bFrameStripping(S.bAllowFrameStripping), bKeepCompression(S.bDoNotOverrideCompression), bReady(false)
 {
  Model->GetBoneTrackNames(BoneNames);
  for (auto* O : S.GetMetaData()) Metadata.Add(O);
  if (const auto* D = S.GetAssetUserDataArray()) for (auto* O : *D) UserData.Add(O);
  bReady = CompressionDependencies(S, Dependencies) && SettingsReceipt(S, Receipts) && ReadRetainedPreview(S, Preview);
  TArray<UObject*> Objects = Dependencies; Objects.Append(Settings); Objects.Append(Metadata); Objects.Append(UserData);
  Objects.Add(Preview.ToSoftObjectPath().ResolveObject());
  for (const auto& N : Notifies) { Objects.Add(N.Notify.Get()); Objects.Add(N.NotifyStateClass.Get()); }
  TSet<UObject*> Seen;
  for (auto* O : Objects) if (O && !Seen.Contains(O)) { Seen.Add(O); bReady &= Loaded(O); Anchors.Emplace(O); }
 }
 bool Matches(const UAnimSequence& S) const
 {
  if (!bReady || !ModelIdentity.Current() || S.GetDataModel() != Model || S.GetDataModelInterface().GetObject() != ModelIdentity.Object.Get()) return false;
  for (const auto& A : Anchors) if (!A.Current()) return false;
  TArray<UObject*> M, D, Deps; TArray<FName> Names; TArray<FString> CurrentReceipts; FSoftObjectPtr CurrentPreview;
  Model->GetBoneTrackNames(Names);
  for (auto* O : S.GetMetaData()) M.Add(O);
  if (const auto* Data = S.GetAssetUserDataArray()) for (auto* O : *Data) D.Add(O);
  if (!CompressionDependencies(S, Deps) || Deps.Num() != Dependencies.Num()) return false;
  for (auto* O : Deps) if (!Dependencies.Contains(O)) return false;
  return SettingsReceipt(S, CurrentReceipts) && Receipts == CurrentReceipts && Settings == ReadSettings(S)
   && Model->GenerateGuid() == Guid && Model->GetFrameRate() == Rate && S.GetSamplingFrameRate() == SamplingRate && Model->GetNumberOfFrames() == Frames
   && Model->GetNumberOfKeys() == Keys && Names == BoneNames && SameStructs(Curves, CurveDecorations(*Model))
   && SameStructs(Notifies, S.Notifies) && SameStructs(Tracks, S.AnimNotifyTracks) && Markers == S.AuthoredSyncMarkers
   && M == Metadata && D == UserData && ReadRetainedPreview(S, CurrentPreview) && Preview == CurrentPreview && Skeleton == S.GetSkeleton()
   && SkeletonGuid == S.GetSkeletonGuid() && RetargetSource == S.RetargetSource && Reference == S.RefPoseSeq.Get()
   && Additive == S.AdditiveAnimType.GetValue() && Base == S.RefPoseType.GetValue() && Frame == S.RefFrameIndex
   && Length == S.GetPlayLength() && Playback == S.RateScale && Error == S.CompressionErrorThresholdScale
   && CompressionVersion == S.CompressCommandletVersion && Interpolation == S.Interpolation && RootLock == S.RootMotionRootLock
   && StripOnServer == S.StripAnimDataOnDedicatedServer && bLoop == S.bLoop && bRootMotion == S.bEnableRootMotion
   && bForceRoot == S.bForceRootLock && bNormalizedRoot == S.bUseNormalizedRootMotionScale
   && bRootFromMontage == S.bRootMotionSettingsCopiedFromMontage && bFrameStripping == S.bAllowFrameStripping
   && bKeepCompression == S.bDoNotOverrideCompression;
 }
};
struct FReferenceSequenceInput
{
 TUniquePtr<FIdentity> Identity;
 TUniquePtr<FRetainedState> Retained;
 TSoftObjectPtr<USkeletalMesh> Retarget;
 TArray<FTransform> Pose;
 bool bDirty;
 explicit FReferenceSequenceInput(UAnimSequence* S) : Identity(S ? MakeUnique<FIdentity>(S) : nullptr),
  Retained(S ? MakeUnique<FRetainedState>(*S) : nullptr), Retarget(S ? S->GetRetargetSourceAsset() : TSoftObjectPtr<USkeletalMesh>()),
  Pose(S ? S->RetargetSourceAssetReferencePose : TArray<FTransform>()), bDirty(S && S->GetOutermost()->IsDirty()) {}
 bool Current() const
 {
  if (!Identity) return true;
  const auto* S = Cast<UAnimSequence>(Identity->Object.Get());
  return Identity->Current() && CanonicalSequence(S) && CompressedCurrent(*S) && Retained->Matches(*S)
   && S->GetRetargetSourceAsset() == Retarget && SamePose(Pose, S->RetargetSourceAssetReferencePose)
   && S->GetOutermost()->IsDirty() == bDirty;
 }
};
}
