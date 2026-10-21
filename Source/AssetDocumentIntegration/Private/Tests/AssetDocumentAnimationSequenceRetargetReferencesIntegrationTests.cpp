// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentAnimationRequestBinding.h"
#include "AssetDocumentMutationComposition.h"
#include "AssetDocumentEditorMutationPreparation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/AssetUserData.h"
#include "Engine/SkeletalMesh.h"
#include "EditorFramework/AssetImportData.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/GarbageCollection.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

// Two existing explicit native Intents, one real request owner and Core. This
// fixture has no inverse and does not publish a References Profile/ApplyFile.
namespace AssetDocumentSequenceRetargetReferencesIntegrationTests
{
constexpr const TCHAR* NamePrimitive = TEXT("animation.sequence.retarget_source_name.set.v1");
constexpr const TCHAR* AssetPrimitive = TEXT("animation.sequence.retarget_source_asset.set.v2");
enum class ECase { SetBoth, NoneClear, BothNoop, NameNoop, AssetNoop };
enum class EFailure { None, Late, DriftBeforeCapture, DriftAfterForward };
FString PackageFile(const FString& Path)
{ return FPackageName::LongPackageNameToFilename(FPackageName::ObjectPathToPackageName(Path), FPackageName::GetAssetPackageExtension()); }
TArray<uint8> Bytes(const FString& Path)
{ TArray<uint8> B; FFileHelper::LoadFileToArray(B, *Path); return B; }
TArray<uint8> NativeState(UObject& O)
{ TArray<uint8> B; FMemoryWriter W(B, true); FObjectAndNameAsStringProxyArchive Ar(W, false); O.Serialize(Ar); return B; }
bool Save(UObject* O)
{
 FSavePackageArgs A; A.TopLevelFlags = RF_Public | RF_Standalone; A.SaveFlags = SAVE_NoError;
 const FString File = PackageFile(O->GetPathName()); IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
 return UPackage::SavePackage(O->GetOutermost(), O, *File, A);
}
FAssetDocumentRuntimeValueDto String(const FString& S)
{ FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::String; V.StringValue = S; return V; }
FAssetDocumentRuntimeValueDto NameValue(const FName N)
{ FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Object; V.ObjectKeys = {TEXT("retarget_source_name")}; V.Elements = {String(N.ToString())}; return V; }
FAssetDocumentRuntimeValueDto AssetValue(USkeletalMesh* M, bool bPreparation)
{
 FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Object; V.ObjectKeys = {TEXT("operation")};
 V.Elements = {String(M ? TEXT("set") : TEXT("clear"))};
 if (bPreparation) { V.ObjectKeys.Add(TEXT("retarget_source_asset_path")); V.Elements.Add(M ? String(M->GetPathName()) : FAssetDocumentRuntimeValueDto()); }
 return V;
}
bool ExactPose(const TArray<FTransform>& A, const TArray<FTransform>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I)
  if (A[I].GetTranslation() != B[I].GetTranslation() || A[I].GetRotation() != B[I].GetRotation() || A[I].GetScale3D() != B[I].GetScale3D()) return false;
 return true;
}
bool ExactMap(const TMap<FName, FReferencePose>& A, const TMap<FName, FReferencePose>& B)
{
 if (A.Num() != B.Num()) return false; TArray<FName> Keys; B.GetKeys(Keys);
 for (const auto& E : A)
 {
  const auto* V = B.Find(E.Key); const int32 I = Keys.IndexOfByKey(E.Key);
  if (!V || I == INDEX_NONE || !E.Key.IsEqual(Keys[I], ENameCase::CaseSensitive, true)
   || !E.Value.PoseName.IsEqual(V->PoseName, ENameCase::CaseSensitive, true)
   || !E.Value.SourceReferenceMesh.ToSoftObjectPath().ToString().Equals(V->SourceReferenceMesh.ToSoftObjectPath().ToString(), ESearchCase::CaseSensitive)
   || !ExactPose(E.Value.ReferencePose, V->ReferencePose)) return false;
 }
 return true;
}
template <typename T> bool SameStructs(const TArray<T>& A, const TArray<T>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I) if (!T::StaticStruct()->CompareScriptStruct(&A[I], &B[I], 0)) return false;
 return true;
}
struct FAssets
{
 FString Root = TEXT("/Game/AssetDocumentSequenceRetargetReferences/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 FString DiskRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AssetDocumentSequenceRetargetReferences") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
 TStrongObjectPtr<USkeleton> Skeleton; TStrongObjectPtr<USkeletalMesh> Old, Desired, DefaultSource, AlternateSource;
 TStrongObjectPtr<UAnimSequence> Sequence;
 TArray<UObject*> Sources() const { return {Skeleton.Get(), Old.Get(), Desired.Get(), DefaultSource.Get(), AlternateSource.Get()}; }
 bool Setup(bool bReferenceSubtypes = false)
 {
  if (!IFileManager::Get().MakeDirectory(*(DiskRoot / TEXT("Installed")), true)) return false;
  // Direct authored construction is confined to these new test-owned assets.
  UClass* SkeletonClass = bReferenceSubtypes ? FSoftClassPath(TEXT("/Script/AssetDocumentTests.AssetDocumentSequenceReferencesTestSkeleton")).ResolveClass() : USkeleton::StaticClass();
  UClass* MeshClass = bReferenceSubtypes ? FSoftClassPath(TEXT("/Script/AssetDocumentTests.AssetDocumentSequenceReferencesTestMesh")).ResolveClass() : USkeletalMesh::StaticClass();
  if (!SkeletonClass || !SkeletonClass->IsChildOf(USkeleton::StaticClass()) || !MeshClass || !MeshClass->IsChildOf(USkeletalMesh::StaticClass())) return false;
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), SkeletonClass, TEXT("Skeleton"), RF_Public | RF_Standalone));
  auto Mesh = [&](const TCHAR* N, double X)
  {
   auto* M = NewObject<USkeletalMesh>(CreatePackage(*(Root + TEXT("/") + N)), MeshClass, N, RF_Public | RF_Standalone); M->SetSkeleton(Skeleton.Get());
   FReferenceSkeletonModifier B(M->GetRefSkeleton(), Skeleton.Get()); B.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
   B.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(X, 0., 0.))); return M;
  };
  Old.Reset(Mesh(TEXT("Old"), 10.)); if (!Skeleton->MergeAllBonesToBoneTree(Old.Get(), false)) return false;
  Skeleton->SetBoneTranslationRetargetingMode(1, EBoneTranslationRetargetingMode::AnimationRelative);
  { FReferenceSkeletonModifier B(Old->GetRefSkeleton(), Skeleton.Get()); B.UpdateRefPoseTransform(1, FTransform(FVector(20., 0., 0.))); }
  Desired.Reset(Mesh(TEXT("Desired"), 45.)); DefaultSource.Reset(Mesh(TEXT("DefaultSource"), 30.)); AlternateSource.Reset(Mesh(TEXT("AlternateSource"), 40.));
  for (const auto& E : TArray<TPair<FName, USkeletalMesh*>>{{TEXT("Default"), DefaultSource.Get()}, {TEXT("Alternate"), AlternateSource.Get()}})
  {
   auto& P = Skeleton->AnimRetargetSources.Add(E.Key); P.PoseName = E.Key; P.SourceReferenceMesh = E.Value;
   FAnimationRuntime::MakeSkeletonRefPoseFromMesh(E.Value, Skeleton.Get(), P.ReferencePose);
  }
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  auto& S = *Sequence; S.SetSkeleton(Skeleton.Get()); S.RetargetSource = TEXT("Default"); S.SetRetargetSourceAsset(Old.Get()); S.UpdateRetargetSourceAssetData();
  S.AdditiveAnimType = AAT_LocalSpaceBase; S.RefPoseType = ABPT_RefPose; S.RefFrameIndex = 0; S.RateScale = 1.25f; S.bLoop = true;
  S.bEnableRootMotion = false; S.RootMotionRootLock = ERootMotionRootLock::RefPose; S.bForceRootLock = false; S.bUseNormalizedRootMotionScale = false;
  auto& C = S.GetController(); C.OpenBracket(FText::FromString(TEXT("Paired retarget fixture")), false); C.InitializeModel();
  C.SetFrameRate(FFrameRate(24, 1), false); C.SetNumberOfFrames(FFrameNumber(12), false);
  TArray<FVector3f> P, Scale; TArray<FQuat4f> Q; Scale.Init(FVector3f::OneVector, 13); Q.Init(FQuat4f::Identity, 13); bool Ok = true;
  for (const FName N : {FName(TEXT("root")), FName(TEXT("child"))})
  { P.Init(N == TEXT("root") ? FVector3f::ZeroVector : FVector3f(50.f, 0.f, 0.f), 13); Ok &= C.AddBoneCurve(N, false) && C.SetBoneTrackKeys(N, P, Q, Scale, false); }
  const FAnimationCurveIdentifier Curve(FName(TEXT("RetainedReferencesCurve")), ERawCurveTrackTypes::RCT_Float);
  FRichCurveKey K0(0.f, 2.f), K1(0.5f, 6.f); K0.InterpMode = RCIM_Linear;
  Ok &= C.AddCurve(Curve, AACF_Editable, false) && C.SetCurveKeys(Curve, {K0, K1}, false); C.NotifyPopulated(); C.CloseBracket(false); if (!Ok) return false;
  S.AnimNotifyTracks.SetNum(1); S.AnimNotifyTracks[0].TrackName = TEXT("RetainedTrack");
  auto& N = S.Notifies.AddDefaulted_GetRef(); N.NotifyName = TEXT("RetainedPoint"); N.Guid = FGuid(1, 2, 3, 4); N.TrackIndex = 0;
  N.Link(Sequence.Get(), 0.25f); N.EndLink.Link(Sequence.Get(), 0.25f);
  auto& M = S.AuthoredSyncMarkers.AddDefaulted_GetRef(); M.MarkerName = TEXT("RetainedMarker"); M.Time = 0.25f; M.TrackIndex = 0; M.Guid = FGuid(5, 6, 7, 8);
  Skeleton->RegisterMarkerName(M.MarkerName);
  S.ClearAllCachedCookedPlatformData(); S.CacheDerivedDataForCurrentPlatform(); S.WaitOnExistingCompression();
  for (UObject* O : Sources()) { FAssetRegistryModule::AssetCreated(O); if (!Save(O)) return false; }
  // Avoid a separate third-party creation callback route for the existing target.
  if (!Save(Sequence.Get())) return false;
  if (const auto* D = S.GetAssetUserDataArray()) if (D->ContainsByPredicate([](const auto* O) { return O != nullptr; })) return false;
  return true;
 }
 ~FAssets()
 {
  if (Sequence.IsValid()) Sequence->WaitOnExistingCompression(); auto All = Sources(); All.Insert(Sequence.Get(), 0);
  for (UObject* O : All) if (IsValid(O))
  { UPackage* P = O->GetOutermost(); O->ClearFlags(RF_Public | RF_Standalone); O->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional); O->MarkAsGarbage(); P->SetDirtyFlag(false); P->MarkAsGarbage(); }
  // Saved native fixture packages remain for node-owned cleanup after exit.
  IFileManager::Get().DeleteDirectory(*DiskRoot, false, true);
 }
};
struct FTuple
{
 FName Name; FString Asset; TArray<FTransform> Pose;
 static FTuple Read(const UAnimSequence& S) { return {S.RetargetSource, S.GetRetargetSourceAsset().ToSoftObjectPath().ToString(), S.RetargetSourceAssetReferencePose}; }
 bool Check(FAutomationTestBase& T, const UAnimSequence& S) const
 {
  bool Ok = T.TestTrue(TEXT("exact owned native name/display"), S.RetargetSource.IsEqual(Name, ENameCase::CaseSensitive, true));
  Ok &= T.TestTrue(TEXT("exact owned soft asset reference"), S.GetRetargetSourceAsset().ToSoftObjectPath().ToString().Equals(Asset, ESearchCase::CaseSensitive));
  Ok &= T.TestTrue(TEXT("entire owned stored pose exact"), ExactPose(Pose, S.RetargetSourceAssetReferencePose)); return Ok;
 }
};
struct FExpected
{
 FName Name = TEXT("Default"); USkeletalMesh* Stored = nullptr; USkeletalMesh* Effective = nullptr; double EffectiveX = 30.;
 FTuple Tuple(const FAssets& A) const
 {
  FTuple R{Name, Stored ? Stored->GetPathName() : FString(), {}};
  if (Stored) FAnimationRuntime::MakeSkeletonRefPoseFromMesh(Stored, A.Skeleton.Get(), R.Pose); return R;
 }
};
FExpected Desired(const FAssets& A, ECase C)
{
 if (C == ECase::NoneClear) return {NAME_None, nullptr, nullptr, 10.};
 if (C == ECase::BothNoop) return {TEXT("Default"), A.Old.Get(), A.DefaultSource.Get(), 30.};
 if (C == ECase::NameNoop) return {TEXT("Default"), A.Desired.Get(), A.DefaultSource.Get(), 30.};
 if (C == ECase::AssetNoop) return {TEXT("Alternate"), A.Old.Get(), A.AlternateSource.Get(), 40.};
 return {TEXT("Alternate"), A.Desired.Get(), A.AlternateSource.Get(), 40.};
}
bool CheckNative(FAutomationTestBase& T, const FAssets& A, const UAnimSequence& S, const FExpected& E)
{
 bool Ok = E.Tuple(A).Check(T, S); TArray<FTransform> Golden;
 if (E.Effective) FAnimationRuntime::MakeSkeletonRefPoseFromMesh(E.Effective, A.Skeleton.Get(), Golden);
 else Golden = A.Skeleton->GetReferenceSkeleton().GetRefBonePose();
 if (!T.TestEqual(TEXT("independent public effective golden has two bones"), Golden.Num(), 2)) return false;
 Ok &= T.TestEqual(TEXT("independent effective child golden"), Golden[1].GetTranslation().X, E.EffectiveX, 0.);
 Ok &= T.TestTrue(TEXT("entire native effective pose equals independent Mesh/Skeleton golden"), ExactPose(S.GetRetargetTransforms(), Golden));
 Ok &= T.TestTrue(TEXT("native bones compressed and valid"), S.IsCompressedDataValid()); Ok &= T.TestFalse(TEXT("native cache current"), S.IsCompressedDataOutOfDate());
 Ok &= T.TestTrue(TEXT("public current-platform compressed cache exists"), S.HasCompressedDataForPlatform(static_cast<const ITargetPlatform*>(nullptr)));
 Ok &= T.TestFalse(TEXT("completion leaves no current-platform residency request"), S.HasResidency(static_cast<const ITargetPlatform*>(nullptr)));
 if (!Ok) return false;
 FAnimExtractContext C(0.25); C.bEnforceCompressedDataSampling = true; C.TargetPlatformHash = S.GetDerivedDataKeyHash(nullptr);
 {
  const auto D = S.GetCompressedData(C); Ok &= T.TestTrue(TEXT("current hash has actual curve codec"), D.Get().CurveCompressionCodec != nullptr);
  Ok &= T.TestFalse(TEXT("nonempty authored curve has actual compressed stream"), D.Get().CompressedCurveByteStream.IsEmpty());
 }
 if (!Ok) return false;
 FTransform Bone; S.GetBoneTransform(Bone, FSkeletonPoseBoneIndex(1), C, false);
 Ok &= T.TestEqual(TEXT("golden compressed additive child X"), Bone.GetTranslation().X, 50. - E.EffectiveX, 1e-4);
 Ok &= T.TestEqual(TEXT("golden compressed additive child Y"), Bone.GetTranslation().Y, 0., 1e-4); Ok &= T.TestEqual(TEXT("golden compressed additive child Z"), Bone.GetTranslation().Z, 0., 1e-4);
 // The stock ACL curve codec admits 0.001 loss at interior samples. Keep
 // strict known range extrema of this nonconstant 2-to-6 authored curve,
 // rather than treating its lossy compressed midpoint as exact raw 4.0.
 FAnimExtractContext Start(0.); Start.bEnforceCompressedDataSampling = true; Start.TargetPlatformHash = C.TargetPlatformHash;
 FAnimExtractContext End(0.5); End.bEnforceCompressedDataSampling = true; End.TargetPlatformHash = C.TargetPlatformHash;
 Ok &= T.TestEqual(TEXT("retained compressed curve first key"), S.EvaluateCurveData(TEXT("RetainedReferencesCurve"), Start, false), 2.f, 1e-4f);
 Ok &= T.TestEqual(TEXT("retained compressed curve last key"), S.EvaluateCurveData(TEXT("RetainedReferencesCurve"), End, false), 6.f, 1e-4f); return Ok;
}
struct FSourceSnapshot
{
 TArray<UObject*> Objects; TArray<UPackage*> Packages; TArray<FString> Paths; TArray<bool> Dirty; TArray<TArray<uint8>> Files, Native;
 TArray<TArray<FTransform>> Poses; TMap<FName, FReferencePose> Map; FGuid Guid;
 explicit FSourceSnapshot(const FAssets& A) : Objects(A.Sources()), Map(A.Skeleton->AnimRetargetSources), Guid(A.Skeleton->GetGuid())
 {
  for (UObject* O : Objects) { Packages.Add(O->GetOutermost()); Paths.Add(O->GetPathName()); Dirty.Add(O->GetOutermost()->IsDirty()); Files.Add(Bytes(PackageFile(O->GetPathName()))); Native.Add(NativeState(*O)); }
  Poses.Add(A.Skeleton->GetReferenceSkeleton().GetRefBonePose());
  for (USkeletalMesh* M : {A.Old.Get(), A.Desired.Get(), A.DefaultSource.Get(), A.AlternateSource.Get()}) Poses.Add(M->GetRefSkeleton().GetRefBonePose());
 }
 bool Check(FAutomationTestBase& T, const FAssets& A) const
 {
  bool Ok = T.TestTrue(TEXT("readonly exact source identities"), A.Sources() == Objects);
  Ok &= T.TestEqual(TEXT("readonly Skeleton GUID"), A.Skeleton->GetGuid(), Guid); Ok &= T.TestTrue(TEXT("entire readonly named map"), ExactMap(Map, A.Skeleton->AnimRetargetSources));
  for (int32 I = 0; I < Objects.Num(); ++I)
  {
   auto* O = Objects[I]; Ok &= T.TestEqual(TEXT("readonly package identity"), O->GetOutermost(), Packages[I]); Ok &= T.TestEqual(TEXT("readonly path"), O->GetPathName(), Paths[I]);
   Ok &= T.TestEqual(TEXT("readonly original dirty"), O->GetOutermost()->IsDirty(), Dirty[I]); Ok &= T.TestTrue(TEXT("readonly installed bytes"), Bytes(PackageFile(Paths[I])) == Files[I]);
   Ok &= T.TestTrue(TEXT("readonly entire persistent native source state"), NativeState(*O) == Native[I]);
  }
  Ok &= T.TestTrue(TEXT("readonly Skeleton full pose"), ExactPose(Poses[0], A.Skeleton->GetReferenceSkeleton().GetRefBonePose()));
  int32 I = 1; for (USkeletalMesh* M : {A.Old.Get(), A.Desired.Get(), A.DefaultSource.Get(), A.AlternateSource.Get()})
  { Ok &= T.TestTrue(TEXT("readonly Mesh full pose"), ExactPose(Poses[I++], M->GetRefSkeleton().GetRefBonePose())); Ok &= T.TestEqual(TEXT("readonly Mesh Skeleton identity"), M->GetSkeleton(), A.Skeleton.Get()); }
  return Ok;
 }
};
struct FRetained
{
 FString ModelGuid; FGuid SkeletonGuid; TArray<FFloatCurve> Curves; TArray<FTransformCurve> TransformCurves;
 TArray<FAnimNotifyEvent> Notifies; TArray<FAnimNotifyTrack> Tracks; TArray<FAnimSyncMarker> Markers;
 UObject* BoneSettings; UObject* CurveSettings; UObject* FrameSettings; float Error; bool bKeep, bStrip;
 TObjectPtr<UAssetImportData> Import; TArray<uint8> ImportBytes; int32 CompressionVersion; EAnimInterpolationType Interpolation;
 FFrameRate SamplingRate; EStripAnimDataOnDedicatedServerSettings ServerStrip; bool bRootFromMontage;
 explicit FRetained(const UAnimSequence& S) : ModelGuid(S.GetDataModel()->GenerateGuid().ToString()), SkeletonGuid(S.GetSkeletonGuid()), Curves(S.GetDataModel()->GetFloatCurves()),
  TransformCurves(S.GetDataModel()->GetTransformCurves()), Notifies(S.Notifies), Tracks(S.AnimNotifyTracks), Markers(S.AuthoredSyncMarkers), BoneSettings(S.BoneCompressionSettings),
  CurveSettings(S.CurveCompressionSettings), FrameSettings(S.VariableFrameStrippingSettings), Error(S.CompressionErrorThresholdScale), bKeep(S.bDoNotOverrideCompression), bStrip(S.bAllowFrameStripping),
  Import(S.AssetImportData), ImportBytes(Import ? NativeState(*Import) : TArray<uint8>{}), CompressionVersion(S.CompressCommandletVersion), Interpolation(S.Interpolation),
  SamplingRate(S.GetSamplingFrameRate()), ServerStrip(S.StripAnimDataOnDedicatedServer), bRootFromMontage(S.bRootMotionSettingsCopiedFromMontage) {}
 bool Check(FAutomationTestBase& T, const UAnimSequence& S, bool bFresh = false) const
 {
  bool Ok = T.TestEqual(TEXT("full unowned model GUID"), S.GetDataModel()->GenerateGuid().ToString(), ModelGuid);
  Ok &= T.TestEqual(TEXT("unowned Skeleton GUID"), S.GetSkeletonGuid(), SkeletonGuid); Ok &= T.TestTrue(TEXT("entire float curves retained"), SameStructs(Curves, S.GetDataModel()->GetFloatCurves()));
  Ok &= T.TestTrue(TEXT("entire transform curves retained"), SameStructs(TransformCurves, S.GetDataModel()->GetTransformCurves()));
  auto ExpectedNotifies = Notifies;
  if (bFresh)
  {
   for (auto& N : ExpectedNotifies) { const float Time = N.GetTime(); N.Link(const_cast<UAnimSequence*>(&S), Time, N.GetSlotIndex()); N.EndLink.Link(const_cast<UAnimSequence*>(&S), Time + N.Duration, N.GetSlotIndex()); }
   for (const auto& N : S.Notifies) Ok &= T.TestTrue(TEXT("fresh point links bind actual loaded self"), N.GetLinkedSequence() == &S && N.EndLink.GetLinkedSequence() == &S);
  }
  Ok &= T.TestTrue(TEXT("full notify structs retained"), SameStructs(ExpectedNotifies, S.Notifies)); Ok &= T.TestTrue(TEXT("full tracks retained"), SameStructs(Tracks, S.AnimNotifyTracks));
  Ok &= T.TestTrue(TEXT("full markers retained"), SameStructs(Markers, S.AuthoredSyncMarkers));
  Ok &= T.TestTrue(TEXT("compression settings identities retained"), S.BoneCompressionSettings == BoneSettings && S.CurveCompressionSettings == CurveSettings && S.VariableFrameStrippingSettings == FrameSettings);
  Ok &= T.TestEqual(TEXT("compression scalar retained"), S.CompressionErrorThresholdScale, Error); Ok &= T.TestEqual(TEXT("compression bool retained"), static_cast<bool>(S.bDoNotOverrideCompression), bKeep);
  Ok &= T.TestEqual(TEXT("frame stripping retained"), static_cast<bool>(S.bAllowFrameStripping), bStrip); Ok &= T.TestEqual(TEXT("compression version retained"), S.CompressCommandletVersion, CompressionVersion);
  Ok &= T.TestEqual(TEXT("interpolation retained"), S.Interpolation, Interpolation);
  Ok &= T.TestEqual(TEXT("sampling rate retained"), S.GetSamplingFrameRate(), SamplingRate); Ok &= T.TestEqual(TEXT("server stripping retained"), S.StripAnimDataOnDedicatedServer, ServerStrip);
  Ok &= T.TestEqual(TEXT("montage root-motion flag retained"), static_cast<bool>(S.bRootMotionSettingsCopiedFromMontage), bRootFromMontage);
  Ok &= T.TestTrue(TEXT("owned import object retained"), bFresh ? (S.AssetImportData != nullptr) == (Import != nullptr) : S.AssetImportData == Import);
  if (Import && S.AssetImportData) Ok &= T.TestTrue(TEXT("entire import authored state retained"), NativeState(*S.AssetImportData) == ImportBytes);
  TArray<FName> Names; S.GetDataModel()->GetBoneTrackNames(Names); Ok &= T.TestTrue(TEXT("ordered bone tracks retained"), Names == TArray<FName>{TEXT("root"), TEXT("child")});
  Ok &= T.TestEqual(TEXT("frame rate retained"), S.GetDataModel()->GetFrameRate(), FFrameRate(24, 1)); Ok &= T.TestEqual(TEXT("frame count retained"), S.GetDataModel()->GetNumberOfFrames(), 12);
  Ok &= T.TestEqual(TEXT("key count retained"), S.GetDataModel()->GetNumberOfKeys(), 13); Ok &= T.TestEqual(TEXT("length retained"), S.GetPlayLength(), 0.5f);
  Ok &= T.TestEqual(TEXT("additive type retained"), S.AdditiveAnimType.GetValue(), AAT_LocalSpaceBase); Ok &= T.TestEqual(TEXT("additive base retained"), S.RefPoseType.GetValue(), ABPT_RefPose);
  Ok &= T.TestEqual(TEXT("additive frame retained"), S.RefFrameIndex, 0); Ok &= T.TestNull(TEXT("additive reference remains empty"), S.RefPoseSeq.Get());
  Ok &= T.TestEqual(TEXT("playback retained"), S.RateScale, 1.25f); Ok &= T.TestTrue(TEXT("loop retained"), S.bLoop); Ok &= T.TestFalse(TEXT("root motion remains off"), S.bEnableRootMotion);
  Ok &= T.TestFalse(TEXT("force root remains off"), S.bForceRootLock); Ok &= T.TestFalse(TEXT("normalized root remains off"), S.bUseNormalizedRootMotionScale);
  Ok &= T.TestEqual(TEXT("root lock retained"), S.RootMotionRootLock.GetValue(), ERootMotionRootLock::RefPose); Ok &= T.TestNull(TEXT("PreviewMesh remains empty"), static_cast<const UAnimationAsset&>(S).GetPreviewMesh());
  Ok &= T.TestTrue(TEXT("metadata remains empty"), S.GetMetaData().IsEmpty()); if (const auto* U = S.GetAssetUserDataArray()) Ok &= T.TestTrue(TEXT("UserData remains empty"), U->IsEmpty()); return Ok;
 }
};
FExpected Intermediate(const FAssets& A, ECase C, bool bNameFirst)
{
 if (bNameFirst)
 {
  if (C == ECase::NoneClear) return {NAME_None, A.Old.Get(), A.Old.Get(), 20.};
  if (C == ECase::SetBoth || C == ECase::AssetNoop) return {TEXT("Alternate"), A.Old.Get(), A.AlternateSource.Get(), 40.};
  return {TEXT("Default"), A.Old.Get(), A.DefaultSource.Get(), 30.};
 }
 if (C == ECase::NoneClear) return {TEXT("Default"), nullptr, A.DefaultSource.Get(), 30.};
 if (C == ECase::SetBoth || C == ECase::NameNoop) return {TEXT("Default"), A.Desired.Get(), A.DefaultSource.Get(), 30.};
 return {TEXT("Default"), A.Old.Get(), A.DefaultSource.Get(), 30.};
}
bool Changed(ECase C, bool bName)
{ return C == ECase::SetBoth || C == ECase::NoneClear || (bName ? C == ECase::AssetNoop : C == ECase::NameNoop); }
class FResolver final : public IAssetDocumentHandleResolver
{
 struct FEntry { FAssetDocumentOpaqueHandle Handle; TUniquePtr<IAssetDocumentNativeHandlePayload> Payload; };
 class FLease final : public IAssetDocumentVerifiedHandleLease
 {
  FAssetDocumentOpaqueHandle H; IAssetDocumentNativeHandlePayload& P;
 public:
  FLease(FAssetDocumentOpaqueHandle In, IAssetDocumentNativeHandlePayload& Payload) : H(In), P(Payload) {}
  const FAssetDocumentOpaqueHandle& GetHandle() const override { return H; }
  IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return P; }
 };
 TArray<FEntry> Entries;
public:
 FAssetDocumentMutationToken Token;
 FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access, TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
 {
  auto& E = Entries.AddDefaulted_GetRef(); E.Handle.RequestId = Token.RequestId; E.Handle.TransactionId = Token.TransactionId;
  E.Handle.RuntimeRevision = Token.RuntimeRevision; E.Handle.ProviderId = TEXT("editor"); E.Handle.Slot = Entries.Num(); E.Handle.Generation = 1;
  E.Handle.Kind = Kind; E.Handle.Access = Access; E.Payload = MoveTemp(Payload); FAssetDocumentIssuedHandleResult R; R.Handle = E.Handle; return R;
 }
 FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& H, const FAssetDocumentHandleValidationContext& C) override
 {
  FAssetDocumentVerifiedLeaseResult R; auto V = ValidateAssetDocumentHandleMetadata(H, C); if (!V.bValid) { R.Diagnostics = MoveTemp(V.Diagnostics); return R; }
  const int32 I = int32(H.Slot) - 1;
  if (!Entries.IsValidIndex(I) || Entries[I].Handle.Generation != H.Generation || Entries[I].Handle.Kind != H.Kind
   || Entries[I].Handle.Access != H.Access || !Entries[I].Payload || !Entries[I].Payload->IsAlive()) return R;
  R.Lease = MakeUnique<FLease>(H, *Entries[I].Payload); return R;
 }
 FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override
 { FAssetDocumentHandleInvalidationResult R; R.InvalidatedHandleCount = Entries.Num(); Entries.Reset(); return R; }
};
TOptional<FAssetDocumentOpaqueHandle> Resolve(UObject& O, EAssetDocumentHandleAccess Access, FResolver& H, IAssetDocumentAtomicTransaction& T)
{
 FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = TEXT("editor.asset.resolve_existing.v1"); I.Version = 1;
 I.RequestId = H.Token.RequestId; I.TransactionId = H.Token.TransactionId; I.RuntimeRevision = H.Token.RuntimeRevision; I.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 I.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object; I.ValueArguments.ObjectKeys = {TEXT("path"), TEXT("expected_class")};
 I.ValueArguments.Elements = {String(O.GetPathName()), String(O.GetClass()->GetPathName())};
 const auto R = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(I, H, &T); if (R.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) return {};
 for (const auto& E : R.HandleResults) if (E.Name == (Access == EAssetDocumentHandleAccess::Mutable ? TEXT("asset_mutable") : TEXT("asset_read")) && E.Handles.Num() == 1) return E.Handles[0];
 return {};
}
struct FRun
{
 FAssetDocumentRuntimeResultDto Result; TArray<int32> ChangedCounts; int32 Invocations = 0, ApplyCount = 0, RepairCount = 0, VerifierCalls = 0, Closes = 0, Invalidations = 0;
 bool bCaptureOpen = false, bClosedBeforeRollback = false, bOpenAtRepair = false, bFinalObservedBeforeWait = false; TArray<bool> ObservedBeforeWait;
 int32 NativeOwnersCreated = 0, NativePrepared = 0;
};
class FCountedNativeOwner final : public IAssetDocumentAnimationRequestProvider
{
 TSharedRef<IAssetDocumentAnimationRequestProvider> Original = CreateAssetDocumentAnimationRequestProvider();
 FRun& Run;
public:
 explicit FCountedNativeOwner(FRun& In) : Run(In) { ++Run.NativeOwnersCreated; }
 FString GetProviderId() const override { return Original->GetProviderId(); }
 TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override { return Original->DescribePrimitives(); }
 FAssetDocumentProviderAvailability InspectAvailability() const override { return Original->InspectAvailability(); }
 FAssetDocumentContractValidationResult BindRequest(const FAssetDocumentRuntimeRequestDto& R) override { return Original->BindRequest(R); }
 void CloseRequest() override { Original->CloseRequest(); }
 TUniquePtr<FAssetDocumentAnimationStagedReadLease> BindStagedRead(UAnimMontage& T, const FString& P, uint64 V, TArray<FAssetDocumentRuntimeDiagnostic>& D) override
 { return Original->BindStagedRead(T, P, V, D); }
 FAssetDocumentContractValidationResult Prepare(const FAssetDocumentRuntimeRequestDto& R, TConstArrayView<FAssetDocumentMutationIntent> I, FAssetDocumentPreparedMutationBatch& B) override
 { ++Run.NativePrepared; return Original->Prepare(R, I, B); }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& H, IAssetDocumentAtomicTransaction* T) override
 { return Original->Invoke(I, H, T); }
};
class FReferencesPreflightContext final : public IAssetDocumentNativeRequestContext
{
 FAssetDocumentRequestContextMetadata Metadata;
 TSharedRef<IAssetDocumentPrimitiveProvider> Editor;
 FResolver& Handles;
public:
 FReferencesPreflightContext(const FAssetDocumentRuntimeRequestDto& R, TSharedRef<IAssetDocumentPrimitiveProvider> E, FResolver& H)
  : Editor(MoveTemp(E)), Handles(H)
 { Metadata.RequestId = R.RequestId; Metadata.PinnedRuntimeRevision = R.ExpectedRuntimeRevision; Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Preflight; }
 const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
 FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override
 { FAssetDocumentPrimitiveRegistrySnapshot S; S.Descriptors.Append(Editor->DescribePrimitives()); return S; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& I) override
 {
  auto Pinned = I; Pinned.RequestId = Metadata.RequestId; Pinned.TransactionId = Handles.Token.TransactionId;
  Pinned.RuntimeRevision = Metadata.PinnedRuntimeRevision; Pinned.Phase = Metadata.Phase; return Editor->Invoke(Pinned, Handles, nullptr);
 }
};
FAssetDocumentRuntimeValueDto DocumentInputs(const FAssets& A, ECase Case)
{
 auto Object = [](TArray<FString> Keys = {}, TArray<FAssetDocumentRuntimeValueDto> Values = {})
 { FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Object; V.ObjectKeys = MoveTemp(Keys); V.Elements = MoveTemp(Values); return V; };
 FAssetDocumentRuntimeValueDto Present, Clear, Defaults; Present.Kind = Clear.Kind = EAssetDocumentRuntimeValueKind::Boolean;
 Defaults.Kind = EAssetDocumentRuntimeValueKind::Array;
 if (Case == ECase::NoneClear)
 {
  Defaults.Elements = {String(TEXT("RetargetSource")), String(TEXT("RetargetSourceAsset"))};
  return Object({TEXT("region_present"), TEXT("supplied_name"), TEXT("reference_fragments"), TEXT("explicit_asset_clear"), TEXT("native_default_fields"), TEXT("definitions")},
   {Present, {}, Object(), Clear, Defaults, Object()});
 }
 Present.BooleanValue = true; const auto Final = Desired(A, Case);
 const auto Skeleton = Object({TEXT("Kind"), TEXT("Path"), TEXT("Class")}, {String(TEXT("assetref")), String(A.Skeleton->GetPathName()), String(TEXT("/Script/Engine.Skeleton"))});
 const auto Mesh = Object({TEXT("Kind"), TEXT("Path"), TEXT("Class"), TEXT("extra")}, {
  String(TEXT("AssetRef")), String(Final.Stored->GetOutermost()->GetName()), String(Final.Stored->GetClass()->GetPathName()), String(TEXT("preserved selector metadata"))});
 const auto Alias = Object({TEXT("Kind"), TEXT("Id")}, {String(TEXT("DefinitionRef")), String(TEXT(" exact mesh "))});
 return Object({TEXT("region_present"), TEXT("supplied_name"), TEXT("reference_fragments"), TEXT("explicit_asset_clear"), TEXT("native_default_fields"), TEXT("definitions")},
  {Present, String(Final.Name.ToString()), Object({TEXT("Skeleton"), TEXT("RetargetSourceAsset")}, {Skeleton, Alias}), Clear, Defaults, Object({TEXT(" exact mesh ")}, {Mesh})});
}
class FReadContext final : public IAssetDocumentNativeRequestContext
{
 FAssetDocumentRequestContextMetadata M;
public:
 explicit FReadContext(FAssetDocumentRequestContextMetadata In) : M(In) {}
 const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return M; }
 FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return {}; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation&) override { return {}; }
};
class FContext final : public IAssetDocumentNativeRequestContext
{
 FAssetDocumentRequestContextMetadata M; FAutomationTestBase& Test; FAssets& A; ECase Case; bool bNameFirst; FRun& Run;
 FResolver& Handles; IAssetDocumentAtomicTransaction& Transaction; TSharedRef<FAssetDocumentMutationComposition> Composition;
 TSharedRef<FAssetDocumentAnimationRequestBinding> Binding; const FSourceSnapshot& Sources; const FRetained& Retained;
public:
 FContext(FAssetDocumentRequestContextMetadata In, FAutomationTestBase& T, FAssets& Assets, ECase C, bool NameFirst, FRun& R, FResolver& H,
  IAssetDocumentAtomicTransaction& Tx, TSharedRef<FAssetDocumentMutationComposition> P, TSharedRef<FAssetDocumentAnimationRequestBinding> B,
  const FSourceSnapshot& S, const FRetained& Unowned)
  : M(In), Test(T), A(Assets), Case(C), bNameFirst(NameFirst), Run(R), Handles(H), Transaction(Tx), Composition(P), Binding(B), Sources(S), Retained(Unowned) {}
 const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return M; }
 FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return {}; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& I) override
 { return Composition->Invoke(TEXT("animation"), I, Handles, &Transaction, [&] { return Binding->Invoke(I, Handles, &Transaction); }); }
 FAssetDocumentRuntimeResultDto Apply()
 {
  FAssetDocumentRuntimeResultDto R; R.RequestId = M.RequestId; R.RuntimeRevision = M.PinnedRuntimeRevision;
  R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
  Handles.Token = Transaction.GetMutationToken(); const auto Target = Resolve(*A.Sequence, EAssetDocumentHandleAccess::Mutable, Handles, Transaction);
  if (!Target.IsSet()) { R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; return R; }
  const auto Final = Desired(A, Case); UObject* Model = A.Sequence->GetDataModelInterface().GetObject(); UPackage* Package = A.Sequence->GetOutermost();
  for (int32 Step = 0; Step < 2; ++Step)
  {
   const bool bName = bNameFirst ? Step == 0 : Step == 1;
   FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = bName ? NamePrimitive : AssetPrimitive; I.Version = bName ? 1 : 2;
   I.RequestId = M.RequestId; I.TransactionId = Transaction.GetTransactionId(); I.RuntimeRevision = M.PinnedRuntimeRevision; I.Phase = M.Phase;
   I.HandleArguments = {{TEXT("sequence"), Target.GetValue()}}; I.ValueArguments = bName ? NameValue(Final.Name) : AssetValue(Final.Stored, false);
   if (!bName && Final.Stored)
   {
    const auto H = Resolve(*Final.Stored, EAssetDocumentHandleAccess::ReadOnly, Handles, Transaction);
    if (!H.IsSet()) { R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; return R; }
    I.HandleArguments.Add({TEXT("retarget_source_asset"), H.GetValue()});
   }
   ++Run.Invocations; const auto Outcome = Invoke(I); R.ChangedSet.Append(Outcome.ChangedSet); R.Diagnostics.Append(Outcome.Diagnostics); Run.ChangedCounts.Add(Outcome.ChangedSet.Num());
   Test.TestEqual(TEXT("actual bound paired leaf succeeds"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
   if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) { R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed; return R; }
   Test.TestEqual(TEXT("exact per-leaf changed set including true noop"), Outcome.ChangedSet.Num(), Changed(Case, bName) ? 1 : 0);
   // No fixture Wait, setter or regeneration runs here. Full intermediate and
   // final observations test the production paired owner's permitted states.
   const bool Native = CheckNative(Test, A, *A.Sequence, Step == 0 ? Intermediate(A, Case, bNameFirst) : Final);
   Run.ObservedBeforeWait.Add(Native); Retained.Check(Test, *A.Sequence); Sources.Check(Test, A);
   Test.TestEqual(TEXT("leaf preserves live authored model identity"), A.Sequence->GetDataModelInterface().GetObject(), Model);
   Test.TestEqual(TEXT("leaf preserves live target package identity"), A.Sequence->GetOutermost(), Package); Test.TestEqual(TEXT("leaf preserves exact Skeleton identity"), A.Sequence->GetSkeleton(), A.Skeleton.Get());
  }
  return R;
 }
};
void DriftDesired(FAssets& A)
{
 // Deliberate external change to a test-owned readonly input. Never restored
 // by the fixture; the unrepresentable inverse must surface real Poisoned.
 FReferenceSkeletonModifier B(A.Desired->GetRefSkeleton(), A.Skeleton.Get()); B.UpdateRefPoseTransform(1, FTransform(FVector(46., 0., 0.)));
 A.Desired->GetOutermost()->SetDirtyFlag(true);
}
class FHost final : public IAssetDocumentPythonRuntimeHost
{
 FAutomationTestBase& Test; FAssets& A; ECase Case; EFailure Failure; FRun& Run; const FRetained& Retained; const FSourceSnapshot& Sources;
public:
 FHost(FAutomationTestBase& T, FAssets& Assets, ECase C, EFailure F, FRun& R, const FRetained& U, const FSourceSnapshot& S)
  : Test(T), A(Assets), Case(C), Failure(F), Run(R), Retained(U), Sources(S) {}
 FAssetDocumentRuntimeResultDto Dispatch(const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentNativeRequestContext& Context) override
 {
  if (Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Apply) { ++Run.ApplyCount; return static_cast<FContext&>(Context).Apply(); }
  FAssetDocumentRuntimeResultDto R; R.RequestId = Request.RequestId; R.RuntimeRevision = Request.ExpectedRuntimeRevision;
  R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
  if (Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Repair)
  {
   ++Run.RepairCount; Run.bOpenAtRepair = Run.bCaptureOpen; Run.bFinalObservedBeforeWait = CheckNative(Test, A, *A.Sequence, Desired(A, Case));
   Retained.Check(Test, *A.Sequence); Sources.Check(Test, A); CollectGarbage(RF_NoFlags);
   if (Failure == EFailure::DriftAfterForward) DriftDesired(A);
   if (Failure == EFailure::Late || Failure == EFailure::DriftAfterForward)
   {
    R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
    auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/SequenceRetargetReferencesFixture/Repair"); D.Code = TEXT("SequenceRetargetReferencesFixture.LateFailure");
    D.Message = TEXT("Test-only later failure after both actual bound leaves and native completion."); D.Severity = EAssetDocumentDiagnosticSeverity::Error;
   }
  }
  return R;
 }
};
class FRegistry final : public IAssetDocumentPrimitiveRegistry
{
 FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const override { return {}; }
 FAssetDocumentPrimitiveResolution Resolve(const FAssetDocumentPrimitiveRequirement&) const override { return {}; }
};
class FCancellation final : public IAssetDocumentCancellationView { bool IsCancellationRequested() const override { return false; } };
class FObserver final : public IAssetDocumentRequestObserver { void OnPhaseChanged(FGuid, EAssetDocumentRuntimeRequestPhase) override {} };
FAssetDocumentRegionEvidence Evidence()
{ FAssetDocumentRegionEvidence E; E.Kind = EAssetDocumentEvidenceKind::Present; E.SemanticHash = TEXT("sequence-retarget-references-fixture:v1"); E.CodecVersion = 1; return E; }
class FVerifier final : public IAssetDocumentStagedArtifactVerifier
{
 FAutomationTestBase& Test; FAssets& A; FExpected Expected; const FRetained& Retained; const FSourceSnapshot& Sources; FRun& Run;
public:
 FVerifier(FAutomationTestBase& T, FAssets& Assets, FExpected E, const FRetained& U, const FSourceSnapshot& S, FRun& R)
  : Test(T), A(Assets), Expected(E), Retained(U), Sources(S), Run(R) {}
 FAssetDocumentFreshVerificationResult VerifyFresh(const FAssetDocumentFreshVerificationInput& Input) override
 {
  ++Run.VerifierCalls; FAssetDocumentFreshVerificationResult R;
  const FString FreshName = TEXT("/Temp/SequenceRetargetReferencesVerify_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  FLinkerInstancingContext Instancing; Instancing.AddPackageMapping(FName(*A.Sequence->GetOutermost()->GetName()), FName(*FreshName));
  TStrongObjectPtr<UPackage> P(LoadPackage(nullptr, *Input.StagedPackagePath, LOAD_ForDiff | LOAD_NoWarn, nullptr, &Instancing));
  auto* S = P.IsValid() ? FindObject<UAnimSequence>(P.Get(), TEXT("Sequence")) : nullptr;
  if (!Test.TestNotNull(TEXT("staged paired native Sequence ordinarily loads"), S)) return R;
  S->WaitOnExistingCompression(); R.bFreshLoadProven = S != A.Sequence.Get() && S->GetOutermost() != A.Sequence->GetOutermost()
   && FPaths::IsSamePath(P->GetLoadedPath().GetLocalFullPath(), Input.StagedPackagePath);
  R.bCanonicalMatch = R.bFreshLoadProven && CheckNative(Test, A, *S, Expected) && Retained.Check(Test, *S, true) && Sources.Check(Test, A)
   && Test.TestEqual(TEXT("fresh native Skeleton identity"), S->GetSkeleton(), A.Skeleton.Get()) && Bytes(Input.StagedSidecarPath) == TArray<uint8>{0x7b, 0x7d};
  if (R.bCanonicalMatch) { R.ActualSemanticHash = Input.ExpectedSemanticHash; R.ActualCodecVersion = Input.ExpectedCodecVersion; }
  S->ClearFlags(RF_Public | RF_Standalone); P->SetDirtyFlag(false); P->ClearFlags(RF_Standalone); return R;
 }
};
FRun Execute(FAutomationTestBase& Test, FAssets& A, ECase Case, bool bNameFirst, EFailure Failure, const FRetained& Retained, const FSourceSnapshot& Sources, bool bDocument = false)
{
 FRun Run; FAssetDocumentRuntimeRequestDto Request; Request.RequestId = FGuid::NewGuid(); Request.ExpectedRuntimeRevision = 86;
 Request.Operation = EAssetDocumentRuntimeOperation::Apply; Request.TargetAssetPath = A.Sequence->GetPathName(); Request.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence");
 struct FBound
 {
  TSharedRef<FAssetDocumentAnimationRequestBinding> Provider;
  FBound(FRun& Run, bool Document) : Provider(Document ? MakeShared<FAssetDocumentAnimationRequestBinding>([&Run]() -> TSharedRef<IAssetDocumentAnimationRequestProvider>
   { return MakeShared<FCountedNativeOwner>(Run); }) : MakeShared<FAssetDocumentAnimationRequestBinding>()) {}
  ~FBound() { Provider->CloseRequest(); }
 } Bound(Run, bDocument);
 if (!Test.TestTrue(TEXT("one real request owner binds paired request"), Bound.Provider->BindRequest(Request).bValid)) return Run;
 const auto Final = Desired(A, Case); TArray<FAssetDocumentMutationIntent> Intents;
 for (int32 Step = 0; Step < 2; ++Step)
 {
  const bool bName = bNameFirst ? Step == 0 : Step == 1; auto& I = Intents.AddDefaulted_GetRef(); I.StepIndex = Step;
  I.Requirement = {bName ? NamePrimitive : AssetPrimitive, bName ? 1u : 2u, TEXT("animation_profile")}; I.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  I.InputSchemaId = bName ? TEXT("animation.sequence.retarget_source_name.set.v1.input.v1") : TEXT("animation.sequence.retarget_source_asset.set.v2.input.v2");
  I.Intent = bName ? NameValue(Final.Name) : AssetValue(Final.Stored, true);
 }
 TArray<FAssetDocumentMutationComposition::FProviderBatch> Batches; auto& Editor = Batches.AddDefaulted_GetRef(); Editor.ProviderId = TEXT("editor"); Editor.ActualProvider = CreateAssetDocumentEditorPrimitiveProvider();
 if (!Test.TestTrue(TEXT("real Editor parent and unique package finalizer prepare"), PrepareAssetDocumentEditorMutationBatch(Request, {}, Editor.Batch).bValid)) return Run;
 Test.TestTrue(TEXT("only real Editor batch owns package finalizer"), Editor.Batch.PackageFinalizer.IsValid());
 auto& Animation = Batches.AddDefaulted_GetRef(); Animation.ProviderId = TEXT("animation"); Animation.ActualProvider = Bound.Provider;
 FResolver ReadHandles; ReadHandles.Token.RequestId = Request.RequestId; ReadHandles.Token.TransactionId = FGuid::NewGuid(); ReadHandles.Token.RuntimeRevision = Request.ExpectedRuntimeRevision;
 FReferencesPreflightContext Readonly(Request, Editor.ActualProvider.ToSharedRef(), ReadHandles);
 FAssetDocumentContractValidationResult Prepared;
 if (bDocument)
 {
  TArray<FAssetDocumentMutationIntent> Actual;
  Prepared = Bound.Provider->PrepareSequenceReferencesDocument(Request, Readonly, ReadHandles.Token.TransactionId, DocumentInputs(A, Case),
   bNameFirst ? 0 : 1, bNameFirst ? 1 : 0, EAssetDocumentRuntimeRequestPhase::Apply, {}, Actual, Animation.Batch);
  Intents = MoveTemp(Actual);
  Test.TestEqual(TEXT("References uses only original catalog plus original Active owner"), Run.NativeOwnersCreated, 2);
  Test.TestEqual(TEXT("References delegates exactly once to real Active Prepare"), Run.NativePrepared, 1);
 }
 else Prepared = Bound.Provider->Prepare(Request, Intents, Animation.Batch);
 for (const auto& D : Prepared.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("actual bound paired preparation succeeds"), Prepared.bValid)) return Run;
 if (!Test.TestEqual(TEXT("two original native mutation contributions"), Animation.Batch.Contributions.Num(), 2)) return Run;
 for (int32 Step = 0; Step < 2; ++Step) Test.TestEqual(TEXT("original requested contribution StepIndex retained"), Animation.Batch.Contributions[Step].StepIndex, Step);
 Test.TestFalse(TEXT("Animation never adds a second package finalizer"), Animation.Batch.PackageFinalizer.IsValid());
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 const auto Composition = FAssetDocumentMutationComposition::Freeze(Request, MoveTemp(Intents), MoveTemp(Batches), {}, {}, {}, Diagnostics);
 for (const auto& D : Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("original paired Intents/Batch freeze through real Core composition"), Composition.IsValid())) return Run;
 if (Failure == EFailure::DriftBeforeCapture) DriftDesired(A);
 FAssetDocumentPreflightReceipt Preflight; Preflight.RequestId = Request.RequestId; Preflight.RuntimeRevision = Request.ExpectedRuntimeRevision;
 Preflight.CompleteFootprint.Append(Composition->GetFootprint()); Preflight.ReceiptHash = TEXT("sha256:sequence-retarget-references-fixture"); Preflight.bSucceeded = true;
 FAssetDocumentPersistencePlan Plan; Plan.RequestId = Request.RequestId; Plan.TargetPackagePath = A.Sequence->GetOutermost()->GetName();
 Plan.TargetSidecarPath = A.DiskRoot / TEXT("Installed/References.assetdoc"); Plan.StagingRoot = A.DiskRoot / TEXT("Staging"); Plan.JournalId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
 Plan.Sidecar.Utf8Bytes = {0x7b, 0x7d}; Plan.Sidecar.SemanticHash = Evidence().SemanticHash; Plan.Sidecar.CodecVersion = 1; Plan.Footprint.Append(Composition->GetFootprint());
 FResolver Handles; FRegistry Registry; FCancellation Cancellation; FObserver Observer; FHost Host(Test, A, Case, Failure, Run, Retained, Sources); FVerifier Verifier(Test, A, Final, Retained, Sources, Run);
 FAssetDocumentCoreRuntimeComposition Core({Host, Registry, Cancellation, Observer,
  [&](const FAssetDocumentRequestContextMetadata& M, const FGuid&, IAssetDocumentAtomicTransaction* T, TArray<FAssetDocumentRuntimeDiagnostic>& Out) -> TUniquePtr<IAssetDocumentNativeRequestContext>
  {
   if (!T) return MakeUnique<FReadContext>(M); auto* Adapter = Composition->BindTransaction(*T, M, Out); if (!Adapter) return {};
   return MakeUnique<FContext>(M, Test, A, Case, bNameFirst, Run, Handles, *Adapter, Composition.ToSharedRef(), Bound.Provider, Sources, Retained);
  },
  [&](const FAssetDocumentHandleInvalidation& E) { ++Run.Invalidations; if (E.Reason == EAssetDocumentHandleInvalidationReason::Rollback) Run.bClosedBeforeRollback = !Run.bCaptureOpen; return Handles.Invalidate(E); }});
 auto Opener = Composition->MakeCaptureOpener();
 FAssetDocumentCoreMutationInputs Mutation{Preflight, Plan, Verifier,
  [&](const FAssetDocumentCorePersistenceTargetRequest& I, TArray<FAssetDocumentRuntimeDiagnostic>&) -> TOptional<FAssetDocumentCorePersistenceTargetBinding>
  {
   if (!Composition->ValidateComplete().bValid || I.CanonicalObjectPath != Request.TargetAssetPath || I.ExactAssetClassPath != Request.RequestedAssetClassPath || I.TargetPackagePath != Plan.TargetPackagePath) return {};
   return FAssetDocumentCorePersistenceTargetBinding{I.RequestId, I.TransactionId, I.RuntimeRevision, A.Sequence.Get(), A.Sequence->GetOutermost()};
  },
  {Evidence(), Evidence()}, A.DiskRoot / TEXT("Journal"), A.DiskRoot / TEXT("Journal/Backup"), A.DiskRoot / TEXT("Sync"), TEXT("SequenceRetargetReferencesFixture"), Request.ExpectedRuntimeRevision,
  Composition->MakeInitialActions(),
  [&, Opener = MoveTemp(Opener)](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Out) mutable -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
  {
   auto Lease = Opener(Id, Out); if (!Lease || !Lease->IsValid()) return {}; Run.bCaptureOpen = true;
   return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([&, Lease = MoveTemp(Lease)]() mutable { Lease->Close(); Lease.Reset(); Run.bCaptureOpen = false; ++Run.Closes; });
  }};
 Run.Result = Core.ExecuteMutation(Request, MoveTemp(Mutation)); for (const auto& D : Run.Result.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message); return Run;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceBoundRetargetReferencesCoreTest,
 "UEAssetDoc.AnimationProvider.SequenceBoundCompletionCore.RetargetReferences", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceBoundRetargetReferencesCoreTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceRetargetReferencesIntegrationTests;
 // Ten successful requests: preserve both original orderings, two real writes,
 // None+clear, both noops and either individual leaf's true noop.
 for (const ECase Case : {ECase::SetBoth, ECase::NoneClear, ECase::BothNoop, ECase::NameNoop, ECase::AssetNoop})
 for (const bool bNameFirst : {false, true})
 {
  AddInfo(FString::Printf(TEXT("Paired success case=%d nameFirst=%d"), int32(Case), bNameFirst));
  FAssets A; if (!TestTrue(TEXT("new saved paired fixture"), A.Setup())) return false;
  UAnimSequence* Target = A.Sequence.Get(); UPackage* Package = Target->GetOutermost(); UObject* Model = Target->GetDataModelInterface().GetObject();
  const FRetained Retained(*Target); const FSourceSnapshot Sources(A); const FExpected Before{TEXT("Default"), A.Old.Get(), A.DefaultSource.Get(), 30.};
  CheckNative(*this, A, *Target, Before); const auto Run = Execute(*this, A, Case, bNameFirst, EFailure::None, Retained, Sources);
  TestEqual(TEXT("actual paired request commits without rollback"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
  TestEqual(TEXT("two actual bound leaf invocations"), Run.Invocations, 2); TestEqual(TEXT("one actual Core Apply"), Run.ApplyCount, 1); TestEqual(TEXT("one actual Core Repair"), Run.RepairCount, 1);
  TestEqual(TEXT("both native leaf returns observed before fixture Wait"), Run.ObservedBeforeWait.Num(), 2);
  TestTrue(TEXT("intermediate/final public pose and compression proven before Wait"), Run.ObservedBeforeWait.Num() == 2 && Run.ObservedBeforeWait[0] && Run.ObservedBeforeWait[1]);
  TestTrue(TEXT("final native tuple/usage observed at Repair"), Run.bFinalObservedBeforeWait); TestTrue(TEXT("real capture open at Repair"), Run.bOpenAtRepair);
  TestEqual(TEXT("existing native staged verifier runs once"), Run.VerifierCalls, 1); TestEqual(TEXT("original Core capture closes once"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  TestFalse(TEXT("successful request never enters rollback invalidation"), Run.bClosedBeforeRollback);
  CheckNative(*this, A, *Target, Desired(A, Case)); Retained.Check(*this, *Target); Sources.Check(*this, A);
  TestEqual(TEXT("original target identity retained"), A.Sequence.Get(), Target); TestEqual(TEXT("original package identity retained"), Target->GetOutermost(), Package);
  TestEqual(TEXT("original authored model identity retained"), Target->GetDataModelInterface().GetObject(), Model); TestEqual(TEXT("original Skeleton identity retained"), Target->GetSkeleton(), A.Skeleton.Get());
  TestFalse(TEXT("normal saved original target clean"), Package->IsDirty());
  TestFalse(TEXT("success has no poisoned inverse"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 // Eight actual Core joint inverses: changed named+Mesh and None+clear, both
 // StepIndex orders, original clean and dirty states. No fixture restoration.
 for (const ECase Case : {ECase::SetBoth, ECase::NoneClear})
 for (const bool bNameFirst : {false, true})
 for (const bool Dirty : {false, true})
 {
  AddInfo(FString::Printf(TEXT("Paired inverse case=%d nameFirst=%d dirty=%d"), int32(Case), bNameFirst, Dirty));
  FAssets A; if (!TestTrue(TEXT("new saved joint inverse fixture"), A.Setup())) return false;
  UAnimSequence* Target = A.Sequence.Get(); UPackage* Package = Target->GetOutermost(); UObject* Model = Target->GetDataModelInterface().GetObject();
  const FTuple Tuple = FTuple::Read(*Target); const FRetained Retained(*Target); const FSourceSnapshot Sources(A); const auto Disk = Bytes(PackageFile(Target->GetPathName()));
  const FExpected Before{TEXT("Default"), A.Old.Get(), A.DefaultSource.Get(), 30.}; CheckNative(*this, A, *Target, Before); Package->SetDirtyFlag(Dirty);
  const auto Run = Execute(*this, A, Case, bNameFirst, EFailure::Late, Retained, Sources);
  TestEqual(TEXT("genuine later phase fails actual Core"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestTrue(TEXT("failure has exact fixture later-phase provenance"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceRetargetReferencesFixture.LateFailure"); }));
  TestEqual(TEXT("both real leaf writers completed"), Run.Invocations, 2); TestEqual(TEXT("one real Apply"), Run.ApplyCount, 1); TestEqual(TEXT("one later Repair"), Run.RepairCount, 1);
  TestTrue(TEXT("both native states completed before first fixture Wait"), Run.ObservedBeforeWait.Num() == 2 && Run.ObservedBeforeWait[0] && Run.ObservedBeforeWait[1]);
  TestTrue(TEXT("desired final tuple/compressed use observed before joint inverse"), Run.bFinalObservedBeforeWait); TestTrue(TEXT("capture still open at late failure"), Run.bOpenAtRepair);
  TestEqual(TEXT("Core closes actual capture once"), Run.Closes, 1); TestTrue(TEXT("capture closed before inverse invalidation"), Run.bClosedBeforeRollback);
  TestEqual(TEXT("one real rollback handle invalidation"), Run.Invalidations, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  Tuple.Check(*this, *Target); CheckNative(*this, A, *Target, Before); Retained.Check(*this, *Target); Sources.Check(*this, A);
  TestEqual(TEXT("joint inverse preserves original target"), A.Sequence.Get(), Target); TestEqual(TEXT("joint inverse preserves original package"), Target->GetOutermost(), Package);
  TestEqual(TEXT("joint inverse preserves original model"), Target->GetDataModelInterface().GetObject(), Model); TestEqual(TEXT("joint inverse preserves exact Skeleton"), Target->GetSkeleton(), A.Skeleton.Get());
  TestEqual(TEXT("unique product finalizer restores original dirty last"), Package->IsDirty(), Dirty); TestTrue(TEXT("joint inverse leaves installed native target"), Bytes(PackageFile(Target->GetPathName())) == Disk);
  TestEqual(TEXT("later failure precedes staged save verifier"), Run.VerifierCalls, 0);
  TestFalse(TEXT("valid original joint preimage is actually recoverable"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 // One exact readonly drift refusal after the real frozen preparation. The
 // external test-owned change remains; no direct pose inverse is supplied.
 {
  FAssets A; if (!TestTrue(TEXT("new pre-capture readonly drift fixture"), A.Setup())) return false;
  const FTuple Before = FTuple::Read(*A.Sequence); const FRetained Retained(*A.Sequence); const FSourceSnapshot Sources(A);
  const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName())); UObject* Model = A.Sequence->GetDataModelInterface().GetObject();
  A.Sequence->GetOutermost()->SetDirtyFlag(true); const auto Run = Execute(*this, A, ECase::SetBoth, true, EFailure::DriftBeforeCapture, Retained, Sources);
  TestEqual(TEXT("post-Prepare readonly drift rejects the actual frozen request"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestFalse(TEXT("readonly drift refusal has diagnostics"), Run.Result.Diagnostics.IsEmpty()); TestEqual(TEXT("frozen readonly drift reaches zero native leaf invocations"), Run.Invocations, 0);
  TestTrue(TEXT("frozen readonly drift produces no native changed set"), Run.ChangedCounts.IsEmpty() && Run.Result.ChangedSet.IsEmpty());
  TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen); TestEqual(TEXT("readonly refusal never verifies a saved mutation"), Run.VerifierCalls, 0);
  Before.Check(*this, *A.Sequence); CheckNative(*this, A, *A.Sequence, {TEXT("Default"), A.Old.Get(), A.DefaultSource.Get(), 30.}); Retained.Check(*this, *A.Sequence);
  TestEqual(TEXT("pre-capture refusal preserves model identity"), A.Sequence->GetDataModelInterface().GetObject(), Model); TestTrue(TEXT("pre-capture refusal preserves target dirty"), A.Sequence->GetOutermost()->IsDirty());
  TestTrue(TEXT("pre-capture refusal leaves installed native target"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
  TestEqual(TEXT("external readonly drift is retained, never adopted/restored"), A.Desired->GetRefSkeleton().GetRefBonePose()[1].GetTranslation().X, 46., 0.);
  TestTrue(TEXT("external readonly source dirty retained"), A.Desired->GetOutermost()->IsDirty());
  TestFalse(TEXT("pre-capture refusal is not a poisoned owned inverse"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 // A genuine post-forward used-Mesh drift makes the closed preimage unsafe.
 // Both production leaves ran first; only real Core actions attempt inverse.
 {
  FAssets A; if (!TestTrue(TEXT("new actual poisoned inverse fixture"), A.Setup())) return false;
  const FRetained Retained(*A.Sequence); const FSourceSnapshot Sources(A); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
  const auto Run = Execute(*this, A, ECase::SetBoth, false, EFailure::DriftAfterForward, Retained, Sources);
  TestEqual(TEXT("used source drift makes actual Core fail"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestEqual(TEXT("both actual forward leaves completed before external drift"), Run.Invocations, 2);
  TestTrue(TEXT("actual final native completion/use observed before drift"), Run.bFinalObservedBeforeWait);
  TestTrue(TEXT("specific actual later failure reached"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceRetargetReferencesFixture.LateFailure"); }));
  TestTrue(TEXT("real unsafe joint inverse is Poisoned rather than silently accepted"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
  TestTrue(TEXT("capture closes before unsafe inverse"), Run.bClosedBeforeRollback); TestEqual(TEXT("capture closes once"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  TestEqual(TEXT("no staged verifier after unsafe inverse"), Run.VerifierCalls, 0); TestTrue(TEXT("unsafe inverse never installs new target bytes"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
  TestEqual(TEXT("external changed Mesh remains changed, no fixture repair"), A.Desired->GetRefSkeleton().GetRefBonePose()[1].GetTranslation().X, 46., 0.);
  TestTrue(TEXT("external Mesh dirty remains changed"), A.Desired->GetOutermost()->IsDirty()); Retained.Check(*this, *A.Sequence);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceReferencesDocumentCoreTest,
 "UEAssetDoc.AnimationProvider.SequenceBoundCompletionCore.RetargetReferencesDocumentInputs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceReferencesDocumentCoreTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceRetargetReferencesIntegrationTests;
 // Keep the original twenty-scenario entry unchanged. This new exact scope
 // reaches the actual private document binding and actual Core paired inverse.
 for (const ECase Case : {ECase::SetBoth, ECase::NoneClear, ECase::BothNoop})
 {
  FAssets A; if (!TestTrue(TEXT("new document fixture with legitimate reference subtypes"), A.Setup(true))) return false;
  const FRetained Retained(*A.Sequence); const FSourceSnapshot Sources(A); const auto* Skeleton = A.Sequence->GetSkeleton();
  const auto Run = Execute(*this, A, Case, true, EFailure::None, Retained, Sources, true);
  TestEqual(TEXT("actual document pair commits"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
  TestEqual(TEXT("original two native leaf invocations"), Run.Invocations, 2);
  TestEqual(TEXT("original staged native verifier once"), Run.VerifierCalls, 1);
  TestTrue(TEXT("native completed states observed before fixture waits"), Run.ObservedBeforeWait.Num() == 2 && Run.ObservedBeforeWait[0] && Run.ObservedBeforeWait[1]);
  TestTrue(TEXT("readonly original Skeleton retained"), A.Sequence->GetSkeleton() == Skeleton);
  CheckNative(*this, A, *A.Sequence, Desired(A, Case)); Retained.Check(*this, *A.Sequence); Sources.Check(*this, A);
  TestFalse(TEXT("document projection is not a second package finalizer"), A.Sequence->GetOutermost()->IsDirty());
 }
 for (const ECase Case : {ECase::SetBoth, ECase::NoneClear})
 for (bool Dirty : {false, true})
 {
  FAssets A; if (!TestTrue(TEXT("new late inverse document fixture"), A.Setup(true))) return false;
  const FTuple Before = FTuple::Read(*A.Sequence); const FRetained Retained(*A.Sequence); const FSourceSnapshot Sources(A);
  const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName())); A.Sequence->GetOutermost()->SetDirtyFlag(Dirty);
  const auto Run = Execute(*this, A, Case, false, EFailure::Late, Retained, Sources, true);
  TestEqual(TEXT("actual Core late failure after document pair"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestTrue(TEXT("specific genuine later failure"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceRetargetReferencesFixture.LateFailure"); }));
  TestEqual(TEXT("both original native writers ran"), Run.Invocations, 2); TestEqual(TEXT("one original capture closed"), Run.Closes, 1);
  TestTrue(TEXT("closed original capture precedes actual inverse"), Run.bClosedBeforeRollback);
  Before.Check(*this, *A.Sequence); CheckNative(*this, A, *A.Sequence, {TEXT("Default"), A.Old.Get(), A.DefaultSource.Get(), 30.});
  Retained.Check(*this, *A.Sequence); Sources.Check(*this, A);
  TestEqual(TEXT("original unique finalizer restores clean/dirty last"), A.Sequence->GetOutermost()->IsDirty(), Dirty);
  TestTrue(TEXT("actual inverse preserves installed native package bytes"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
  TestFalse(TEXT("valid document preimage is recoverable"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 return !HasAnyErrors();
}
#endif
