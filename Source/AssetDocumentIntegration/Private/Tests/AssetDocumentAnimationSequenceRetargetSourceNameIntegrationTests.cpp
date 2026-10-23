// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentMutationComposition.h"
#include "AssetDocumentAnimationRequestBinding.h"
#include "AssetDocumentEditorMutationPreparation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentAnimationPrimitiveProvider.h"
#include "AssetDocumentAnimationMutationPreparation.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/AssetUserData.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UObject/GarbageCollection.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

// Existing-target native leaf proof: genuine Editor-issued identities, the
// Animation participant and real Core persistence/inverse. No Profile route.
namespace AssetDocumentSequenceRetargetSourceNameIntegrationTests
{
constexpr const TCHAR* Primitive = TEXT("animation.sequence.retarget_source_name.set.v1");
enum class EBoundCase { Disabled, Forward, MissingTarget, ReadOnlyTarget, WrongTargetClass, ForeignTarget, ForeignTransaction,
 ForeignRequest, ForeignRevision, WrongPhase, ExtraValue, Closed, StagedRevision };
struct FScopedBoundRequest
{
 EBoundCase Case = EBoundCase::Disabled; FAutomationTestBase* Test = nullptr;
 bool bRefusalObserved = false, bStagedReadRejected = false, bCompletionObserved = false;
 TSharedPtr<FAssetDocumentAnimationRequestBinding> Binding;
 ~FScopedBoundRequest() { if (Binding) Binding->CloseRequest(); }
};
constexpr const TCHAR* Schema = TEXT("animation.sequence.retarget_source_name.set.v1.input.v1");
FString PackageFile(const FString& Path)
{ return FPackageName::LongPackageNameToFilename(FPackageName::ObjectPathToPackageName(Path), FPackageName::GetAssetPackageExtension()); }
TArray<uint8> Bytes(const FString& Path)
{ TArray<uint8> R; FFileHelper::LoadFileToArray(R, *Path); return R; }
bool Save(UObject* Asset)
{
 FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
 const FString Path = PackageFile(Asset->GetPathName()); IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
 return UPackage::SavePackage(Asset->GetOutermost(), Asset, *Path, Args);
}
FAssetDocumentRuntimeValueDto String(const FString& V)
{ FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::String; R.StringValue = V; return R; }
FAssetDocumentRuntimeValueDto Value(const FString& Name)
{
 FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::Object;
 R.ObjectKeys = {TEXT("retarget_source_name")}; R.Elements = {String(Name)}; return R;
}
bool ExactPose(const TArray<FTransform>& A, const TArray<FTransform>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I)
  if (A[I].GetTranslation() != B[I].GetTranslation() || A[I].GetRotation() != B[I].GetRotation()
   || A[I].GetScale3D() != B[I].GetScale3D()) return false;
 return true;
}
bool ExactMap(const TMap<FName, FReferencePose>& A, const TMap<FName, FReferencePose>& B)
{
 if (A.Num() != B.Num()) return false;
 TArray<FName> Keys; B.GetKeys(Keys);
 for (const auto& E : A)
 {
  const auto* Other = B.Find(E.Key); const int32 KeyIndex = Keys.IndexOfByKey(E.Key);
  if (!Other || KeyIndex == INDEX_NONE || !E.Key.IsEqual(Keys[KeyIndex], ENameCase::CaseSensitive, true)
   || !E.Value.PoseName.IsEqual(Other->PoseName, ENameCase::CaseSensitive, true)
   || E.Value.SourceReferenceMesh.ToSoftObjectPath() != Other->SourceReferenceMesh.ToSoftObjectPath()
   || !ExactPose(E.Value.ReferencePose, Other->ReferencePose)) return false;
 }
 return true;
}
FTransform Usage(const UAnimSequence& Sequence)
{
 FTransform R; FAnimExtractContext Context(0.25); Context.bEnforceCompressedDataSampling = true;
 Context.TargetPlatformHash = Sequence.GetDerivedDataKeyHash(nullptr);
 Sequence.GetBoneTransform(R, FSkeletonPoseBoneIndex(1), Context, false); return R;
}
bool CheckUsage(FAutomationTestBase& Test, const UAnimSequence& Sequence, double Expected)
{
 bool Ok = Test.TestTrue(TEXT("native compressed data valid"), Sequence.IsCompressedDataValid());
 Ok &= Test.TestFalse(TEXT("native compressed data current"), Sequence.IsCompressedDataOutOfDate());
 Ok &= Test.TestTrue(TEXT("current public platform cache available"), Sequence.HasCompressedDataForPlatform(static_cast<const ITargetPlatform*>(nullptr)));
 if (!Ok) return false;
 const auto Actual = Usage(Sequence).GetTranslation();
 Ok &= Test.TestEqual(TEXT("golden compressed child additive translation"), Actual.X, Expected, 1e-4);
 Ok &= Test.TestEqual(TEXT("compressed child Y"), Actual.Y, 0., 1e-4); Ok &= Test.TestEqual(TEXT("compressed child Z"), Actual.Z, 0., 1e-4);
 return Ok;
}
bool CheckEffectivePose(FAutomationTestBase& Test, const UAnimSequence& Sequence, double Expected)
{
 const auto& Pose = Sequence.GetRetargetTransforms(); if (!Test.TestEqual(TEXT("entire effective pose count"), Pose.Num(), 2)) return false;
 TArray<FTransform> Golden;
 if (Expected == 10.) Golden = Sequence.GetSkeleton()->GetReferenceSkeleton().GetRefBonePose();
 else
 {
  USkeletalMesh* Mesh = Expected == 20. ? Sequence.GetRetargetSourceAsset().Get() : nullptr;
  if (Expected == 30. || Expected == 40.)
   if (const auto* Entry = Sequence.GetSkeleton()->AnimRetargetSources.Find(Expected == 30. ? FName(TEXT("Default")) : FName(TEXT("Alternate"))))
    Mesh = Entry->SourceReferenceMesh.Get();
  if (!Test.TestNotNull(TEXT("independent golden fixture Mesh is already loaded"), Mesh)) return false;
  // Use the independent fixture Mesh, not the map pose returned by the getter.
  // Stock reference-bone normalization need not be bit-exact identity.
  FAnimationRuntime::MakeSkeletonRefPoseFromMesh(Mesh, Sequence.GetSkeleton(), Golden);
 }
 if (!Test.TestEqual(TEXT("entire independent golden pose count"), Golden.Num(), 2)) return false;
 bool Ok = Test.TestEqual(TEXT("independent golden child translation"), Golden[1].GetTranslation().X, Expected, 0.);
 Ok &= Test.TestTrue(TEXT("independent golden root translation"), Golden[0].GetTranslation() == FVector::ZeroVector);
 return Test.TestTrue(TEXT("exact native effective pose"), ExactPose(Pose, Golden)) && Ok;
}
struct FValues
{
 FString Name, Reference; TArray<FTransform> StoredPose;
 static FValues Read(const UAnimSequence& S)
 { return {S.RetargetSource.ToString(), S.GetRetargetSourceAsset().ToSoftObjectPath().ToString(), S.RetargetSourceAssetReferencePose}; }
 bool Matches(const UAnimSequence& S) const
 { return S.RetargetSource.IsEqual(FName(*Name), ENameCase::CaseSensitive, true)
   && S.GetRetargetSourceAsset().ToSoftObjectPath().ToString() == Reference && ExactPose(StoredPose, S.RetargetSourceAssetReferencePose); }
 bool Check(FAutomationTestBase& Test, const UAnimSequence& S) const
 {
  bool Ok = Test.TestEqual(TEXT("exact native name identity"), S.RetargetSource, FName(*Name));
  Ok &= Test.TestEqual(TEXT("native name display retained"), S.RetargetSource.ToString(), Name);
  Ok &= Test.TestEqual(TEXT("retained mesh soft reference"), S.GetRetargetSourceAsset().ToSoftObjectPath().ToString(), Reference);
  Ok &= Test.TestTrue(TEXT("entire retained stored pose exact"), ExactPose(StoredPose, S.RetargetSourceAssetReferencePose)); return Ok;
 }
};
enum class ECase
{
 SetDefault, SetAlternate, SetNone, Unknown, CaseOnly, NoopNone,
 MissingTarget, ReadOnlyTarget, WrongTargetClass, StaleTarget, ForeignTransaction, ForeignRevision,
 WrongPhase, ExtraValue, DifferentValue, InvalidValue, ExtraHandle
};
FString DesiredName(ECase Case)
{
 if (Case == ECase::SetDefault) return TEXT("Default");
 if (Case == ECase::SetNone || Case == ECase::NoopNone) return TEXT("None");
 if (Case == ECase::Unknown) return TEXT("UnlistedRetargetSource");
 if (Case == ECase::CaseOnly) return TEXT("dEfAuLt");
 return TEXT("Alternate");
}
double DesiredPose(ECase Case)
{
 if (Case == ECase::SetDefault || Case == ECase::CaseOnly) return 30.;
 if (Case == ECase::SetNone || Case == ECase::NoopNone) return 20.;
 if (Case == ECase::Unknown) return 10.;
 return 40.;
}
bool IsNoop(ECase Case) { return Case == ECase::CaseOnly || Case == ECase::NoopNone; }
struct FAssets
{
 FString Root = TEXT("/Game/AssetDocumentSequenceRetargetSourceName/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 FString DiskRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AssetDocumentSequenceRetargetSourceName") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
 TStrongObjectPtr<USkeleton> Skeleton; TStrongObjectPtr<USkeletalMesh> Stored, DefaultSource, AlternateSource; TStrongObjectPtr<UAnimSequence> Sequence;
 bool bKeep = false;
 bool Setup(bool bNone = false, bool bStoredMesh = true)
 {
  if (!IFileManager::Get().MakeDirectory(*(DiskRoot / TEXT("Installed")), true)) return false;
  // Raw construction and authored fixture fields belong only to this test.
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  auto Mesh = [&](const TCHAR* Name, double X)
  {
   auto* M = NewObject<USkeletalMesh>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone); M->SetSkeleton(Skeleton.Get());
   FReferenceSkeletonModifier Bones(M->GetRefSkeleton(), Skeleton.Get());
   Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
   Bones.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(X, 0., 0.))); return M;
  };
  Stored.Reset(Mesh(TEXT("Stored"), 10.));
  // Public merge establishes Skeleton's BoneTree and its default child pose10.
  if (!Skeleton->MergeAllBonesToBoneTree(Stored.Get(), false)) return false;
  Skeleton->SetBoneTranslationRetargetingMode(1, EBoneTranslationRetargetingMode::AnimationRelative);
  { FReferenceSkeletonModifier Bones(Stored->GetRefSkeleton(), Skeleton.Get()); Bones.UpdateRefPoseTransform(1, FTransform(FVector(20., 0., 0.))); }
  DefaultSource.Reset(Mesh(TEXT("DefaultSource"), 30.)); AlternateSource.Reset(Mesh(TEXT("AlternateSource"), 40.));
  for (const auto& E : TArray<TPair<FName, USkeletalMesh*>>{{TEXT("Default"), DefaultSource.Get()}, {TEXT("Alternate"), AlternateSource.Get()}})
  {
   auto& Pose = Skeleton->AnimRetargetSources.Add(E.Key); Pose.PoseName = E.Key; Pose.SourceReferenceMesh = E.Value;
   FAnimationRuntime::MakeSkeletonRefPoseFromMesh(E.Value, Skeleton.Get(), Pose.ReferencePose);
  }
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  Sequence->SetSkeleton(Skeleton.Get()); Sequence->RetargetSource = bNone ? NAME_None : FName(TEXT("Default"));
  Sequence->RateScale = 1.25f; Sequence->bLoop = true; Sequence->AdditiveAnimType = AAT_LocalSpaceBase;
  Sequence->RefPoseType = ABPT_RefPose; Sequence->RefFrameIndex = 0; Sequence->bEnableRootMotion = false;
  Sequence->RootMotionRootLock = ERootMotionRootLock::RefPose; Sequence->bForceRootLock = false; Sequence->bUseNormalizedRootMotionScale = false;
  if (bStoredMesh) Sequence->SetRetargetSourceAsset(Stored.Get()); else Sequence->ClearRetargetSourceAsset();
  Sequence->UpdateRetargetSourceAssetData(); // Fixture authoring only; the name leaf must retain this entire pose.
  auto& C = Sequence->GetController(); C.OpenBracket(FText::FromString(TEXT("Retarget name fixture")), false);
  C.InitializeModel(); C.SetFrameRate(FFrameRate(24, 1), false); C.SetNumberOfFrames(FFrameNumber(12), false);
  TArray<FVector3f> P, S; TArray<FQuat4f> Q; S.Init(FVector3f::OneVector, 13); Q.Init(FQuat4f::Identity, 13); bool Ok = true;
  for (const FName Name : {FName(TEXT("root")), FName(TEXT("child"))})
  {
   P.Init(Name == TEXT("root") ? FVector3f::ZeroVector : FVector3f(50.f, 0.f, 0.f), 13);
   Ok &= C.AddBoneCurve(Name, false) && C.SetBoneTrackKeys(Name, P, Q, S, false);
  }
  const FAnimationCurveIdentifier Curve(FName(TEXT("RetainedNameCurve")), ERawCurveTrackTypes::RCT_Float);
  FRichCurveKey A(0.f, 1.f), B(0.5f, -3.f); A.InterpMode = RCIM_Cubic; A.TangentMode = RCTM_Break; A.TangentWeightMode = RCTWM_WeightedBoth;
  A.ArriveTangentWeight = 0.2f; A.LeaveTangentWeight = 0.4f; Ok &= C.AddCurve(Curve, AACF_Editable, false) && C.SetCurveKeys(Curve, {A, B}, false);
  C.NotifyPopulated(); C.CloseBracket(false); Sequence->WaitOnExistingCompression(); if (!Ok) return false;
  Sequence->AnimNotifyTracks.SetNum(1); Sequence->AnimNotifyTracks[0].TrackName = TEXT("RetainedTrack");
  auto& N = Sequence->Notifies.AddDefaulted_GetRef(); N.NotifyName = TEXT("RetainedPoint"); N.Guid = FGuid(1, 2, 3, 4); N.TrackIndex = 0;
  auto& M = Sequence->AuthoredSyncMarkers.AddDefaulted_GetRef(); M.MarkerName = TEXT("RetainedMarker"); M.Time = 0.25f; M.TrackIndex = 0; M.Guid = FGuid(5, 6, 7, 8);
  Sequence->ClearAllCachedCookedPlatformData(); Sequence->CacheDerivedDataForCurrentPlatform(); Sequence->WaitOnExistingCompression();
  for (UObject* O : {static_cast<UObject*>(Skeleton.Get()), static_cast<UObject*>(Stored.Get()), static_cast<UObject*>(DefaultSource.Get()), static_cast<UObject*>(AlternateSource.Get()), static_cast<UObject*>(Sequence.Get())})
  {
   // Skip third-party Sequence creation callbacks for the existing asset fixture.
   if (O != Sequence.Get()) FAssetRegistryModule::AssetCreated(O); if (!Save(O)) return false;
  }
  if (const auto* Data = Sequence->GetAssetUserDataArray()) if (Data->ContainsByPredicate([](const auto* D) { return D != nullptr; })) return false;
  return true;
 }
 ~FAssets()
 {
  if (Sequence.IsValid()) Sequence->WaitOnExistingCompression(); if (bKeep) return;
  for (UObject* O : {static_cast<UObject*>(Sequence.Get()), static_cast<UObject*>(Stored.Get()), static_cast<UObject*>(DefaultSource.Get()), static_cast<UObject*>(AlternateSource.Get()), static_cast<UObject*>(Skeleton.Get())})
   if (IsValid(O))
   {
    UPackage* P = O->GetOutermost(); O->ClearFlags(RF_Public | RF_Standalone);
    O->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional); O->MarkAsGarbage(); P->SetDirtyFlag(false); P->MarkAsGarbage();
   }
  // Package fixture files remain until Editor exit and node-owned cleanup.
  IFileManager::Get().DeleteDirectory(*DiskRoot, false, true);
 }
};
double ExpectedPose(const FAssets& Assets, ECase Case)
{
 if ((Case == ECase::SetNone || Case == ECase::NoopNone) && Assets.Sequence->RetargetSourceAssetReferencePose.IsEmpty()) return 10.;
 return DesiredPose(Case);
}
FValues DesiredValues(const FAssets& Assets, ECase Case)
{
 auto R = FValues::Read(*Assets.Sequence); const FName Desired(*DesiredName(Case));
 if (Assets.Sequence->RetargetSource != Desired) R.Name = Desired.ToString(); return R;
}
bool Retained(FAutomationTestBase& Test, const UAnimSequence& Sequence, const FString& SkeletonPath, const FString& ModelGuid, const FString& SkeletonGuid)
{
 bool Ok = Test.TestNotNull(TEXT("retained Skeleton loaded"), Sequence.GetSkeleton()); if (!Ok) return false;
 Ok &= Test.TestEqual(TEXT("retained Skeleton path"), Sequence.GetSkeleton()->GetPathName(), SkeletonPath);
 Ok &= Test.TestEqual(TEXT("retained Skeleton GUID"), Sequence.GetSkeletonGuid().ToString(), SkeletonGuid);
 Ok &= Test.TestEqual(TEXT("native Skeleton GUID agrees"), Sequence.GetSkeletonGuid(), Sequence.GetSkeleton()->GetGuid());
 const auto* M = Sequence.GetDataModel(); if (!Test.TestNotNull(TEXT("retained authored model"), M)) return false;
 Ok &= Test.TestEqual(TEXT("complete retained model GUID"), M->GenerateGuid().ToString(), ModelGuid);
 TArray<FName> Names; M->GetBoneTrackNames(Names); Ok &= Test.TestTrue(TEXT("ordered native bone tracks"), Names == TArray<FName>{TEXT("root"), TEXT("child")});
 Ok &= Test.TestEqual(TEXT("retained frame rate"), M->GetFrameRate(), FFrameRate(24, 1)); Ok &= Test.TestEqual(TEXT("retained frame count"), M->GetNumberOfFrames(), 12);
 Ok &= Test.TestEqual(TEXT("retained curve count"), M->GetFloatCurves().Num(), 1);
 if (M->GetFloatCurves().Num() == 1)
 {
  const auto& Curve = M->GetFloatCurves()[0]; const auto& Keys = Curve.FloatCurve.GetConstRefOfKeys();
  Ok &= Test.TestEqual(TEXT("retained curve name"), Curve.GetName(), FName(TEXT("RetainedNameCurve"))); Ok &= Test.TestEqual(TEXT("retained curve flags"), Curve.GetCurveTypeFlags(), int32(AACF_Editable));
  Ok &= Test.TestEqual(TEXT("retained curve keys"), Keys.Num(), 2);
  if (Keys.Num() == 2)
  { Ok &= Test.TestEqual(TEXT("retained first value"), Keys[0].Value, 1.f); Ok &= Test.TestEqual(TEXT("retained second value"), Keys[1].Value, -3.f); Ok &= Test.TestEqual(TEXT("retained arrive weight"), Keys[0].ArriveTangentWeight, 0.2f); Ok &= Test.TestEqual(TEXT("retained leave weight"), Keys[0].LeaveTangentWeight, 0.4f); }
 }
 Ok &= Test.TestEqual(TEXT("retained child translation mode"), Sequence.GetSkeleton()->GetBoneTranslationRetargetingMode(1), EBoneTranslationRetargetingMode::AnimationRelative);
 Ok &= Test.TestEqual(TEXT("retained additive type"), Sequence.AdditiveAnimType.GetValue(), AAT_LocalSpaceBase);
 Ok &= Test.TestEqual(TEXT("retained additive base"), Sequence.RefPoseType.GetValue(), ABPT_RefPose); Ok &= Test.TestEqual(TEXT("retained frame index"), Sequence.RefFrameIndex, 0);
 Ok &= Test.TestNull(TEXT("retained empty additive reference"), Sequence.RefPoseSeq.Get()); Ok &= Test.TestEqual(TEXT("retained playback"), Sequence.RateScale, 1.25f);
 Ok &= Test.TestTrue(TEXT("retained loop"), Sequence.bLoop); Ok &= Test.TestFalse(TEXT("retained root motion off"), Sequence.bEnableRootMotion);
 Ok &= Test.TestFalse(TEXT("retained force root off"), Sequence.bForceRootLock); Ok &= Test.TestFalse(TEXT("retained normalized root scale off"), Sequence.bUseNormalizedRootMotionScale);
 Ok &= Test.TestEqual(TEXT("retained root lock"), Sequence.RootMotionRootLock.GetValue(), ERootMotionRootLock::RefPose);
 Ok &= Test.TestNull(TEXT("retained empty PreviewMesh"), static_cast<const UAnimationAsset&>(Sequence).GetPreviewMesh());
 Ok &= Test.TestEqual(TEXT("retained notify count"), Sequence.Notifies.Num(), 1); Ok &= Test.TestEqual(TEXT("retained notify track count"), Sequence.AnimNotifyTracks.Num(), 1);
 if (Sequence.Notifies.Num() == 1) { Ok &= Test.TestEqual(TEXT("retained point"), Sequence.Notifies[0].NotifyName, FName(TEXT("RetainedPoint"))); Ok &= Test.TestEqual(TEXT("retained point GUID"), Sequence.Notifies[0].Guid, FGuid(1, 2, 3, 4)); }
 if (Sequence.AnimNotifyTracks.Num() == 1) Ok &= Test.TestEqual(TEXT("retained track"), Sequence.AnimNotifyTracks[0].TrackName, FName(TEXT("RetainedTrack")));
 Ok &= Test.TestEqual(TEXT("retained marker count"), Sequence.AuthoredSyncMarkers.Num(), 1);
 if (Sequence.AuthoredSyncMarkers.Num() == 1) { const auto& N = Sequence.AuthoredSyncMarkers[0]; Ok &= Test.TestEqual(TEXT("retained marker"), N.MarkerName, FName(TEXT("RetainedMarker"))); Ok &= Test.TestEqual(TEXT("retained marker time"), N.Time, 0.25f); Ok &= Test.TestEqual(TEXT("retained marker GUID"), N.Guid, FGuid(5, 6, 7, 8)); }
 return Ok;
}
struct FSourceSnapshot
{
 TMap<FName, FReferencePose> Map; TArray<FTransform> SkeletonPose, StoredPose, DefaultPose, AlternatePose;
 TArray<TArray<uint8>> Files; TArray<bool> Dirty;
 explicit FSourceSnapshot(const FAssets& A) : Map(A.Skeleton->AnimRetargetSources), SkeletonPose(A.Skeleton->GetReferenceSkeleton().GetRefBonePose()),
  StoredPose(A.Stored->GetRefSkeleton().GetRefBonePose()), DefaultPose(A.DefaultSource->GetRefSkeleton().GetRefBonePose()), AlternatePose(A.AlternateSource->GetRefSkeleton().GetRefBonePose())
 {
  for (const UObject* O : {static_cast<const UObject*>(A.Skeleton.Get()), static_cast<const UObject*>(A.Stored.Get()), static_cast<const UObject*>(A.DefaultSource.Get()), static_cast<const UObject*>(A.AlternateSource.Get())})
  { Files.Add(Bytes(PackageFile(O->GetPathName()))); Dirty.Add(O->GetOutermost()->IsDirty()); }
 }
 void Check(FAutomationTestBase& Test, const FAssets& A) const
 {
  Test.TestTrue(TEXT("complete native retarget map retained"), ExactMap(Map, A.Skeleton->AnimRetargetSources));
  Test.TestTrue(TEXT("Skeleton entire reference pose retained"), ExactPose(SkeletonPose, A.Skeleton->GetReferenceSkeleton().GetRefBonePose()));
  Test.TestTrue(TEXT("stored Mesh entire reference pose retained"), ExactPose(StoredPose, A.Stored->GetRefSkeleton().GetRefBonePose()));
  Test.TestTrue(TEXT("Default Mesh entire reference pose retained"), ExactPose(DefaultPose, A.DefaultSource->GetRefSkeleton().GetRefBonePose()));
  Test.TestTrue(TEXT("Alternate Mesh entire reference pose retained"), ExactPose(AlternatePose, A.AlternateSource->GetRefSkeleton().GetRefBonePose()));
  int32 Index = 0;
  for (const UObject* O : {static_cast<const UObject*>(A.Skeleton.Get()), static_cast<const UObject*>(A.Stored.Get()), static_cast<const UObject*>(A.DefaultSource.Get()), static_cast<const UObject*>(A.AlternateSource.Get())})
  { Test.TestTrue(TEXT("readonly source package bytes retained"), Files[Index] == Bytes(PackageFile(O->GetPathName()))); Test.TestEqual(TEXT("readonly source dirty retained"), O->GetOutermost()->IsDirty(), Dirty[Index]); ++Index; }
  for (USkeletalMesh* M : {A.Stored.Get(), A.DefaultSource.Get(), A.AlternateSource.Get()}) Test.TestEqual(TEXT("readonly Mesh Skeleton identity retained"), M->GetSkeleton(), A.Skeleton.Get());
 }
};
class FResolver final : public IAssetDocumentHandleResolver
{
 struct FEntry { FAssetDocumentOpaqueHandle Handle; TUniquePtr<IAssetDocumentNativeHandlePayload> Payload; };
 class FLease final : public IAssetDocumentVerifiedHandleLease
 {
  FAssetDocumentOpaqueHandle Handle; IAssetDocumentNativeHandlePayload& Payload;
 public:
  FLease(const FAssetDocumentOpaqueHandle& H, IAssetDocumentNativeHandlePayload& P) : Handle(H), Payload(P) {}
  const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
  IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return Payload; }
 };
 TArray<FEntry> Entries;
public:
 FAssetDocumentMutationToken Token;
 FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access, TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
 {
  auto& E = Entries.AddDefaulted_GetRef(); E.Handle.RequestId = Token.RequestId; E.Handle.TransactionId = Token.TransactionId;
  E.Handle.RuntimeRevision = Token.RuntimeRevision; E.Handle.ProviderId = TEXT("editor"); E.Handle.Slot = Entries.Num();
  E.Handle.Generation = 1; E.Handle.Kind = Kind; E.Handle.Access = Access; E.Payload = MoveTemp(Payload);
  FAssetDocumentIssuedHandleResult R; R.Handle = E.Handle; return R;
 }
 FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& H, const FAssetDocumentHandleValidationContext& C) override
 {
  FAssetDocumentVerifiedLeaseResult R; auto V = ValidateAssetDocumentHandleMetadata(H, C); if (!V.bValid) { R.Diagnostics = MoveTemp(V.Diagnostics); return R; }
  const int32 Index = int32(H.Slot) - 1;
  if (!Entries.IsValidIndex(Index) || Entries[Index].Handle.Generation != H.Generation || Entries[Index].Handle.Kind != H.Kind
   || Entries[Index].Handle.Access != H.Access || !Entries[Index].Payload || !Entries[Index].Payload->IsAlive()) return R;
  R.Lease = MakeUnique<FLease>(H, *Entries[Index].Payload); return R;
 }
 FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override
 { FAssetDocumentHandleInvalidationResult R; R.InvalidatedHandleCount = Entries.Num(); Entries.Reset(); return R; }
};
TOptional<FAssetDocumentOpaqueHandle> Resolve(UObject& O, EAssetDocumentHandleAccess Access, FResolver& Handles, IAssetDocumentAtomicTransaction& Transaction)
{
 FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = TEXT("editor.asset.resolve_existing.v1"); I.Version = 1;
 I.RequestId = Handles.Token.RequestId; I.TransactionId = Handles.Token.TransactionId; I.RuntimeRevision = Handles.Token.RuntimeRevision; I.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 I.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object; I.ValueArguments.ObjectKeys = {TEXT("path"), TEXT("expected_class")};
 I.ValueArguments.Elements = {String(O.GetPathName()), String(O.GetClass()->GetPathName())};
 const auto Outcome = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(I, Handles, &Transaction);
 if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) return {};
 const FString Name = Access == EAssetDocumentHandleAccess::Mutable ? TEXT("asset_mutable") : TEXT("asset_read");
 for (const auto& Row : Outcome.HandleResults) if (Row.Name == Name && Row.Handles.Num() == 1) return Row.Handles[0]; return {};
}
class FReadContext final : public IAssetDocumentNativeRequestContext
{
 FAssetDocumentRequestContextMetadata Metadata;
public:
 explicit FReadContext(FAssetDocumentRequestContextMetadata M) : Metadata(M) {}
 const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
 FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return {}; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation&) override { return {}; }
};
class FContext final : public IAssetDocumentNativeRequestContext
{
 FAssetDocumentRequestContextMetadata Metadata; FAssets& Assets; ECase Case; FResolver& Handles;
 IAssetDocumentAtomicTransaction& Transaction; TSharedRef<FAssetDocumentMutationComposition> Composition; TSharedRef<IAssetDocumentPrimitiveProvider> Provider; int32& Calls;
 FScopedBoundRequest& Bound; TStrongObjectPtr<UAnimSequence> Foreign;
public:
 FContext(FAssetDocumentRequestContextMetadata M, FAssets& A, ECase C, FResolver& H, IAssetDocumentAtomicTransaction& T,
  TSharedRef<FAssetDocumentMutationComposition> P, TSharedRef<IAssetDocumentPrimitiveProvider> ProviderIn, int32& CallCount, FScopedBoundRequest& B)
  : Metadata(M), Assets(A), Case(C), Handles(H), Transaction(T), Composition(P), Provider(ProviderIn), Calls(CallCount), Bound(B) {}
 ~FContext()
 {
  if (!Foreign.IsValid()) return; UPackage* P = Foreign->GetOutermost();
  Foreign->ClearFlags(RF_Public | RF_Standalone); Foreign->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
  Foreign->MarkAsGarbage(); P->SetDirtyFlag(false); P->MarkAsGarbage();
 }
 const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
 FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return {}; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& I) override
 { return Composition->Invoke(TEXT("animation"), I, Handles, &Transaction, [&]() { return Provider->Invoke(I, Handles, &Transaction); }); }
 FAssetDocumentPrimitiveOutcome Apply()
 {
  FAssetDocumentPrimitiveOutcome Failed; Failed.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed; Handles.Token = Transaction.GetMutationToken();
  if (Bound.Case == EBoundCase::ForeignTarget)
   Foreign.Reset(NewObject<UAnimSequence>(CreatePackage(*(Assets.Root + TEXT("/ForeignTarget"))), TEXT("ForeignTarget"), RF_Public | RF_Standalone | RF_Transactional));
  UObject& Target = Case == ECase::WrongTargetClass || Bound.Case == EBoundCase::WrongTargetClass ? static_cast<UObject&>(*Assets.Stored)
   : Foreign.IsValid() ? static_cast<UObject&>(*Foreign) : static_cast<UObject&>(*Assets.Sequence);
  auto H = Resolve(Target, Case == ECase::ReadOnlyTarget || Bound.Case == EBoundCase::ReadOnlyTarget ? EAssetDocumentHandleAccess::ReadOnly : EAssetDocumentHandleAccess::Mutable, Handles, Transaction);
  if (!H.IsSet()) return Failed;
  FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = Primitive; I.Version = 1; I.RequestId = Metadata.RequestId;
  I.TransactionId = Transaction.GetTransactionId(); I.RuntimeRevision = Metadata.PinnedRuntimeRevision; I.Phase = Metadata.Phase;
  if (Case == ECase::StaleTarget) ++H->Generation;
  if (Case == ECase::ForeignTransaction || Bound.Case == EBoundCase::ForeignTransaction) H->TransactionId = FGuid::NewGuid();
  if (Case != ECase::MissingTarget && Bound.Case != EBoundCase::MissingTarget) I.HandleArguments = {{TEXT("sequence"), H.GetValue()}};
  I.ValueArguments = Value(DesiredName(Case));
  if (Case == ECase::ForeignRevision) ++I.RuntimeRevision;
  if (Case == ECase::WrongPhase) I.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == ECase::ExtraValue) { I.ValueArguments.ObjectKeys.Add(TEXT("operation")); I.ValueArguments.Elements.Add(String(TEXT("set"))); }
  if (Case == ECase::DifferentValue) I.ValueArguments = Value(TEXT("Default"));
  if (Case == ECase::InvalidValue) I.ValueArguments = Value(TEXT(""));
  if (Case == ECase::ExtraHandle)
  {
   auto Ref = Resolve(*Assets.Stored, EAssetDocumentHandleAccess::ReadOnly, Handles, Transaction); if (!Ref.IsSet()) return Failed;
   I.HandleArguments.Add({TEXT("retarget_source_asset"), Ref.GetValue()});
  }
  if (Bound.Binding && Bound.Case != EBoundCase::Forward)
  {
   auto& Test = *Bound.Test; const FValues Before = FValues::Read(*Assets.Sequence); const FSourceSnapshot Sources(Assets);
   UAnimSequence* Original = Assets.Sequence.Get(); UPackage* Package = Original->GetOutermost(); UObject* Model = Original->GetDataModelInterface().GetObject();
   USkeleton* Skeleton = Assets.Skeleton.Get(); USkeletalMesh* Stored = Assets.Stored.Get(); USkeletalMesh* DefaultSource = Assets.DefaultSource.Get(); USkeletalMesh* AlternateSource = Assets.AlternateSource.Get();
   const FString Guid = Original->GetDataModel()->GenerateGuid().ToString(), SkeletonGuid = Skeleton->GetGuid().ToString();
   const FTransform BeforeUsage = Usage(*Original); const double BeforePose = Original->GetRetargetTransforms()[1].GetTranslation().X;
   const bool Dirty = Package->IsDirty(); const auto Disk = Bytes(PackageFile(Original->GetPathName()));
   const auto ForeignBefore = Foreign.IsValid() ? FValues::Read(*Foreign) : FValues{};
   const bool ForeignDirty = Foreign.IsValid() && Foreign->GetOutermost()->IsDirty();
   if (Bound.Case == EBoundCase::ForeignRequest) I.RequestId = FGuid::NewGuid();
   if (Bound.Case == EBoundCase::ForeignRevision) ++I.RuntimeRevision;
   if (Bound.Case == EBoundCase::WrongPhase) I.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
   if (Bound.Case == EBoundCase::ExtraValue) { I.ValueArguments.ObjectKeys.Add(TEXT("unexpected")); I.ValueArguments.Elements.Add(String(TEXT("closed-shape refusal"))); }
   if (Bound.Case == EBoundCase::Closed) Bound.Binding->CloseRequest();
   if (Bound.Case == EBoundCase::StagedRevision)
   {
    // This real staged read API accepts Montage only. A refused lease for a
    // Sequence owner never licenses Invoke under the proposed fresh revision.
    TStrongObjectPtr<UAnimMontage> Probe(NewObject<UAnimMontage>(GetTransientPackage())); TArray<FAssetDocumentRuntimeDiagnostic> D;
    auto Lease = Bound.Binding->BindStagedRead(*Probe, PackageFile(Original->GetPathName()), I.RuntimeRevision + 1, D);
    Bound.bStagedReadRejected = !Lease && !D.IsEmpty();
    Test.TestTrue(TEXT("actual Montage-only staged API rejects this Sequence owner"), Bound.bStagedReadRejected);
    if (Lease) Lease->Close(); ++I.RuntimeRevision;
   }
   const auto Rejected = Bound.Binding->Invoke(I, Handles, &Transaction);
   Bound.bRefusalObserved = Rejected.Status == EAssetDocumentPrimitiveOutcomeStatus::Failed && Rejected.ChangedSet.IsEmpty() && !Rejected.Diagnostics.IsEmpty();
   Test.TestEqual(TEXT("actual bound negative probe returns failed Outcome"), Rejected.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
   Test.TestTrue(TEXT("actual bound Invoke refuses with diagnostics and no changed set"), Bound.bRefusalObserved);
   // Observe now: a subsequent real Core rollback cannot conceal a wrong
   // probe write to the name, retained storage, sources or cached native use.
   Before.Check(Test, *Original); CheckEffectivePose(Test, *Original, BeforePose); CheckUsage(Test, *Original, BeforeUsage.GetTranslation().X);
   Test.TestTrue(TEXT("immediate bound refusal preserves entire native sample"), Usage(*Original).Equals(BeforeUsage, 1e-5));
   Retained(Test, *Original, Skeleton->GetPathName(), Guid, SkeletonGuid); Sources.Check(Test, Assets);
   Test.TestEqual(TEXT("immediate bound refusal preserves target identity"), Assets.Sequence.Get(), Original);
   Test.TestEqual(TEXT("immediate bound refusal preserves package identity"), Original->GetOutermost(), Package);
   Test.TestEqual(TEXT("immediate bound refusal preserves model identity"), Original->GetDataModelInterface().GetObject(), Model);
   Test.TestEqual(TEXT("immediate bound refusal preserves Skeleton identity"), Original->GetSkeleton(), Skeleton);
   Test.TestEqual(TEXT("immediate bound refusal preserves retained Mesh identity"), Assets.Stored.Get(), Stored);
   Test.TestEqual(TEXT("immediate bound refusal preserves Default Mesh identity"), Assets.DefaultSource.Get(), DefaultSource);
   Test.TestEqual(TEXT("immediate bound refusal preserves Alternate Mesh identity"), Assets.AlternateSource.Get(), AlternateSource);
   Test.TestEqual(TEXT("immediate bound refusal preserves dirty"), Package->IsDirty(), Dirty);
   Test.TestTrue(TEXT("immediate bound refusal preserves installed target"), Bytes(PackageFile(Original->GetPathName())) == Disk);
   if (Foreign.IsValid())
   {
    Test.TestTrue(TEXT("immediate bound refusal preserves unrelated Sequence tuple"), ForeignBefore.Matches(*Foreign));
    Test.TestEqual(TEXT("immediate bound refusal preserves unrelated dirty"), Foreign->GetOutermost()->IsDirty(), ForeignDirty);
   }
  }
  ++Calls; return Invoke(I);
 }
};
class FHost final : public IAssetDocumentPythonRuntimeHost
{
public:
 FAssets& Assets; FValues Expected; double ExpectedUsage; bool bFailRepair; bool& CaptureOpen; FScopedBoundRequest& Bound;
 bool bForwardObserved = false, bOpenAtRepair = false; int32 ApplyCount = 0, RepairCount = 0, ChangedCount = 0;
 FHost(FAssets& A, FValues V, double U, bool Fail, bool& Open, FScopedBoundRequest& B) : Assets(A), Expected(MoveTemp(V)), ExpectedUsage(U), bFailRepair(Fail), CaptureOpen(Open), Bound(B) {}
 FAssetDocumentRuntimeResultDto Dispatch(const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentNativeRequestContext& Context) override
 {
  FAssetDocumentRuntimeResultDto R; R.RequestId = Request.RequestId; R.RuntimeRevision = Request.ExpectedRuntimeRevision;
  R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
  if (Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Apply)
  {
   ++ApplyCount; const auto Outcome = static_cast<FContext&>(Context).Apply(); R.ChangedSet = Outcome.ChangedSet; R.Diagnostics = Outcome.Diagnostics; ChangedCount = Outcome.ChangedSet.Num();
   if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) { R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed; }
  }
  else if (Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Repair)
  {
   ++RepairCount; bOpenAtRepair = CaptureOpen;
   if (Bound.Binding)
   {
    auto& S = *Assets.Sequence;
    Bound.bCompletionObserved = Expected.Matches(S) && S.IsCompressedDataValid() && !S.IsCompressedDataOutOfDate()
     && S.HasCompressedDataForPlatform(static_cast<const ITargetPlatform*>(nullptr)) && !S.HasResidency(static_cast<const ITargetPlatform*>(nullptr));
    Bound.Test->TestTrue(TEXT("bound leaf completes its current native cache before fixture Wait"), Bound.bCompletionObserved);
    CheckEffectivePose(*Bound.Test, S, 50. - ExpectedUsage); CheckUsage(*Bound.Test, S, ExpectedUsage);
   }
   Assets.Sequence->WaitOnExistingCompression();
   bForwardObserved = Expected.Matches(*Assets.Sequence) && Assets.Sequence->IsCompressedDataValid()
    && FMath::IsNearlyEqual(Usage(*Assets.Sequence).GetTranslation().X, ExpectedUsage, 1e-4);
   CollectGarbage(RF_NoFlags);
   if (bFailRepair)
   {
    R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
    auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/SequenceRetargetSourceNameFixture/Repair");
    D.Code = TEXT("SequenceRetargetSourceNameFixture.LateFailure"); D.Message = TEXT("Test-only later failure after real fixed name setter and native completion."); D.Severity = EAssetDocumentDiagnosticSeverity::Error;
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
class FVerifier final : public IAssetDocumentStagedArtifactVerifier
{
 FAutomationTestBase& Test; FAssets& Assets; FValues Expected; double ExpectedUsage; FString ModelGuid, SkeletonGuid; FSourceSnapshot Sources;
public:
 int32 Calls = 0;
 FVerifier(FAutomationTestBase& T, FAssets& A, FValues V, double U) : Test(T), Assets(A), Expected(MoveTemp(V)), ExpectedUsage(U),
  ModelGuid(A.Sequence->GetDataModel()->GenerateGuid().ToString()), SkeletonGuid(A.Skeleton->GetGuid().ToString()), Sources(A) {}
 FAssetDocumentFreshVerificationResult VerifyFresh(const FAssetDocumentFreshVerificationInput& Input) override
 {
  ++Calls; FAssetDocumentFreshVerificationResult R;
  const FString InstancedName = TEXT("/Temp/SequenceRetargetSourceNameVerify_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  FLinkerInstancingContext Instancing; Instancing.AddPackageMapping(FName(*Assets.Sequence->GetOutermost()->GetName()), FName(*InstancedName));
  TStrongObjectPtr<UPackage> Package(LoadPackage(nullptr, *Input.StagedPackagePath, LOAD_ForDiff | LOAD_NoWarn, nullptr, &Instancing));
  auto* Sequence = Package.IsValid() ? FindObject<UAnimSequence>(Package.Get(), TEXT("Sequence")) : nullptr;
  if (!Test.TestNotNull(TEXT("staged Sequence ordinarily loads"), Sequence)) return R;
  Sequence->WaitOnExistingCompression(); Sources.Check(Test, Assets);
  R.bFreshLoadProven = Sequence != Assets.Sequence.Get() && Sequence->GetOutermost() != Assets.Sequence->GetOutermost()
   && FPaths::IsSamePath(Package->GetLoadedPath().GetLocalFullPath(), Input.StagedPackagePath);
  R.bCanonicalMatch = R.bFreshLoadProven && Expected.Check(Test, *Sequence)
   && Retained(Test, *Sequence, Assets.Skeleton->GetPathName(), ModelGuid, SkeletonGuid) && CheckUsage(Test, *Sequence, ExpectedUsage)
   && CheckEffectivePose(Test, *Sequence, 50. - ExpectedUsage) && Bytes(Input.StagedSidecarPath) == TArray<uint8>{0x7b, 0x7d};
  if (R.bCanonicalMatch) { R.ActualSemanticHash = Input.ExpectedSemanticHash; R.ActualCodecVersion = Input.ExpectedCodecVersion; }
  Sequence->ClearFlags(RF_Public | RF_Standalone); Package->SetDirtyFlag(false); Package->ClearFlags(RF_Standalone); return R;
 }
};
FAssetDocumentRegionEvidence Evidence()
{ FAssetDocumentRegionEvidence E; E.Kind = EAssetDocumentEvidenceKind::Present; E.SemanticHash = TEXT("sequence-retarget-source-name-fixture:v1"); E.CodecVersion = 1; return E; }
struct FRun
{
 FAssetDocumentRuntimeResultDto Result; bool bCaptureOpen = false, bClosedBeforeRollback = false, bForwardObserved = false, bOpenAtRepair = false;
 bool bBoundRefusalObserved = false, bStagedReadRejected = false, bBoundCompletionObserved = false;
 int32 Closes = 0, Invalidations = 0, ApplyCount = 0, RepairCount = 0, BatchCalls = 0, ChangedCount = 0, VerifierCalls = 0;
};
FAssetDocumentRuntimeRequestDto RequestFor(const FAssets& A)
{
 FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 85;
 R.Operation = EAssetDocumentRuntimeOperation::Apply; R.TargetAssetPath = A.Sequence->GetPathName(); R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence"); return R;
}
FAssetDocumentMutationIntent IntentFor(const FString& Name)
{
 FAssetDocumentMutationIntent I; I.StepIndex = 0; I.Requirement = {Primitive, 1, TEXT("animation_profile")};
 I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.InputSchemaId = Schema; I.Intent = Value(Name); return I;
}
FRun Execute(FAutomationTestBase& Test, FAssets& Assets, ECase Case, bool bFailRepair, EBoundCase BoundCase = EBoundCase::Disabled)
{
 FRun Run; const auto Request = RequestFor(Assets); const auto Expected = DesiredValues(Assets, Case); const double U = 50. - ExpectedPose(Assets, Case);
 TArray<FAssetDocumentMutationIntent> Intents{IntentFor(DesiredName(Case))};
 FScopedBoundRequest Bound; Bound.Case = BoundCase; Bound.Test = &Test;
 if (BoundCase != EBoundCase::Disabled)
 {
  Bound.Binding = MakeShared<FAssetDocumentAnimationRequestBinding>();
  if (!Test.TestTrue(TEXT("one production request binding binds the actual Core request"), Bound.Binding->BindRequest(Request).bValid)) return Run;
 }
 TSharedRef<IAssetDocumentPrimitiveProvider> Provider = Bound.Binding
  ? StaticCastSharedRef<IAssetDocumentPrimitiveProvider>(Bound.Binding.ToSharedRef()) : CreateAssetDocumentAnimationPrimitiveProvider();
 auto EditorProvider = CreateAssetDocumentEditorPrimitiveProvider();
 TArray<FAssetDocumentMutationComposition::FProviderBatch> Batches;
 auto& Editor = Batches.AddDefaulted_GetRef(); Editor.ProviderId = TEXT("editor"); Editor.ActualProvider = EditorProvider;
 if (!Test.TestTrue(TEXT("real Editor parent prepares"), PrepareAssetDocumentEditorMutationBatch(Request, {}, Editor.Batch).bValid)) return Run;
 auto& Animation = Batches.AddDefaulted_GetRef(); Animation.ProviderId = TEXT("animation"); Animation.ActualProvider = Provider;
 const auto A = Bound.Binding ? Bound.Binding->Prepare(Request, Intents, Animation.Batch)
  : PrepareAssetDocumentAnimationMutationBatch(Request, Intents, Animation.Batch);
 for (const auto& D : A.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("real name participant prepares"), A.bValid)) return Run;
 if (!Test.TestEqual(TEXT("one Animation inverse contribution"), Animation.Batch.Contributions.Num(), 1)) return Run;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 const auto Composition = FAssetDocumentMutationComposition::Freeze(Request, MoveTemp(Intents), MoveTemp(Batches), {}, {}, {}, Diagnostics);
 for (const auto& D : Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("real production composition freezes"), Composition.IsValid())) return Run;
 FAssetDocumentPreflightReceipt Preflight; Preflight.RequestId = Request.RequestId; Preflight.RuntimeRevision = Request.ExpectedRuntimeRevision;
 Preflight.CompleteFootprint.Append(Composition->GetFootprint()); Preflight.ReceiptHash = TEXT("sha256:sequence-retarget-source-name-fixture"); Preflight.bSucceeded = true;
 FAssetDocumentPersistencePlan Plan; Plan.RequestId = Request.RequestId; Plan.TargetPackagePath = Assets.Sequence->GetOutermost()->GetName();
 Plan.TargetSidecarPath = Assets.DiskRoot / TEXT("Installed/RetargetSourceName.assetdoc"); Plan.StagingRoot = Assets.DiskRoot / TEXT("Staging");
 Plan.JournalId = FGuid::NewGuid().ToString(EGuidFormats::Digits); Plan.Sidecar.Utf8Bytes = {0x7b, 0x7d}; Plan.Sidecar.SemanticHash = Evidence().SemanticHash; Plan.Sidecar.CodecVersion = 1;
 Plan.Footprint.Append(Composition->GetFootprint()); FResolver Handles; FRegistry Registry; FCancellation Cancellation; FObserver Observer;
 FHost Host(Assets, Expected, U, bFailRepair, Run.bCaptureOpen, Bound); FVerifier Verifier(Test, Assets, Expected, U);
 FAssetDocumentCoreRuntimeComposition Core({Host, Registry, Cancellation, Observer,
  [&](const FAssetDocumentRequestContextMetadata& M, const FGuid&, IAssetDocumentAtomicTransaction* T, TArray<FAssetDocumentRuntimeDiagnostic>& Out) -> TUniquePtr<IAssetDocumentNativeRequestContext>
  {
   if (!T) return MakeUnique<FReadContext>(M); auto* Adapter = Composition->BindTransaction(*T, M, Out); if (!Adapter) return {};
   return MakeUnique<FContext>(M, Assets, Case, Handles, *Adapter, Composition.ToSharedRef(), Provider, Run.BatchCalls, Bound);
  },
  [&](const FAssetDocumentHandleInvalidation& E)
  { ++Run.Invalidations; if (E.Reason == EAssetDocumentHandleInvalidationReason::Rollback) Run.bClosedBeforeRollback = !Run.bCaptureOpen; return Handles.Invalidate(E); }});
 auto Opener = Composition->MakeCaptureOpener();
 FAssetDocumentCoreMutationInputs Mutation{Preflight, Plan, Verifier,
  [&](const FAssetDocumentCorePersistenceTargetRequest& I, TArray<FAssetDocumentRuntimeDiagnostic>&) -> TOptional<FAssetDocumentCorePersistenceTargetBinding>
  {
   if (!Composition->ValidateComplete().bValid || I.CanonicalObjectPath != Request.TargetAssetPath || I.ExactAssetClassPath != Request.RequestedAssetClassPath || I.TargetPackagePath != Plan.TargetPackagePath) return {};
   return FAssetDocumentCorePersistenceTargetBinding{I.RequestId, I.TransactionId, I.RuntimeRevision, Assets.Sequence.Get(), Assets.Sequence->GetOutermost()};
  },
  {Evidence(), Evidence()}, Assets.DiskRoot / TEXT("Journal"), Assets.DiskRoot / TEXT("Journal/Backup"), Assets.DiskRoot / TEXT("Sync"), TEXT("SequenceRetargetSourceNameFixture"), Request.ExpectedRuntimeRevision,
  Composition->MakeInitialActions(),
  [&, Opener = MoveTemp(Opener)](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Out) mutable -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
  {
   auto Lease = Opener(Id, Out); if (!Lease || !Lease->IsValid()) return {}; Run.bCaptureOpen = true;
   return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([&, Lease = MoveTemp(Lease)]() mutable { Lease->Close(); Lease.Reset(); Run.bCaptureOpen = false; ++Run.Closes; });
  }};
 Run.Result = Core.ExecuteMutation(Request, MoveTemp(Mutation)); for (const auto& D : Run.Result.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 Run.bForwardObserved = Host.bForwardObserved; Run.bOpenAtRepair = Host.bOpenAtRepair;
 Run.bBoundRefusalObserved = Bound.bRefusalObserved; Run.bStagedReadRejected = Bound.bStagedReadRejected; Run.bBoundCompletionObserved = Bound.bCompletionObserved;
 Run.ApplyCount = Host.ApplyCount; Run.RepairCount = Host.RepairCount; Run.ChangedCount = Host.ChangedCount; Run.VerifierCalls = Verifier.Calls; return Run;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceRetargetSourceNameCoreHandlesTest,
 "UEAssetDoc.AnimationProvider.SequenceRetargetSourceNameCore.HandlePreflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceRetargetSourceNameCoreHandlesTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceRetargetSourceNameIntegrationTests;
 for (const ECase Case : {ECase::MissingTarget, ECase::ReadOnlyTarget, ECase::WrongTargetClass, ECase::StaleTarget,
  ECase::ForeignTransaction, ECase::ForeignRevision, ECase::WrongPhase, ECase::ExtraValue, ECase::DifferentValue, ECase::InvalidValue, ECase::ExtraHandle})
  for (const bool bDirty : {false, true})
  {
   FAssets A; if (!TestTrue(TEXT("saved authority fixture"), A.Setup())) return false;
   const FValues Before = FValues::Read(*A.Sequence); const FSourceSnapshot Sources(A); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
   const FString Guid = A.Sequence->GetDataModel()->GenerateGuid().ToString(); A.Sequence->GetOutermost()->SetDirtyFlag(bDirty);
   const auto Run = Execute(*this, A, Case, false);
   TestEqual(TEXT("actual Editor handle/value/phase refusal"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
   TestEqual(TEXT("one actual primitive invocation"), Run.BatchCalls, 1); TestEqual(TEXT("zero native changed set"), Run.ChangedCount, 0);
   TestEqual(TEXT("no Repair after refusal"), Run.RepairCount, 0); TestEqual(TEXT("no staged save verification after refusal"), Run.VerifierCalls, 0);
   TestEqual(TEXT("Core closes native capture once"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
   Before.Check(*this, *A.Sequence); CheckEffectivePose(*this, *A.Sequence, 30.); CheckUsage(*this, *A.Sequence, 20.);
   Retained(*this, *A.Sequence, A.Skeleton->GetPathName(), Guid, A.Skeleton->GetGuid().ToString()); Sources.Check(*this, A);
   TestEqual(TEXT("refusal preserves original dirty"), A.Sequence->GetOutermost()->IsDirty(), bDirty);
   TestTrue(TEXT("refusal preserves installed target bytes"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
   TestFalse(TEXT("preflight refusal never poisons inverse"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
  }
 // Rejected native source state is inspected before a Core transaction exists.
 for (int32 Case = 0; Case < 6; ++Case)
 {
  FAssets A; if (!TestTrue(TEXT("saved source-state fixture"), A.Setup())) return false;
  if (Case == 0) A.Skeleton->AnimRetargetSources.FindChecked(TEXT("Alternate")).ReferencePose.Pop();
  if (Case == 1) A.Skeleton->AnimRetargetSources.FindChecked(TEXT("Default")).ReferencePose.Pop();
  if (Case == 2) A.Sequence->RetargetSourceAssetReferencePose[1].SetTranslation(FVector(20. + 1e-12, 0., 0.));
  if (Case == 3)
  {
   PRAGMA_DISABLE_DEPRECATION_WARNINGS
   A.Sequence->RetargetSourceAsset = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(A.Root + TEXT("/Unavailable.Unavailable")));
   PRAGMA_ENABLE_DEPRECATION_WARNINGS
  }
  if (Case == 4) A.Sequence->GetDataModelInterface().GetObject()->SetFlags(RF_NeedPostLoad);
  if (Case == 5) A.Sequence->ClearRetargetSourceAsset(); // Deliberately retain the now-unrepresentable nonempty pose.
  A.Sequence->WaitOnExistingCompression(); A.Sequence->GetOutermost()->SetDirtyFlag(true);
  const auto Before = FValues::Read(*A.Sequence); const FSourceSnapshot Sources(A); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
  const FGuid Model = A.Sequence->GetDataModel()->GenerateGuid(); const auto Request = RequestFor(A); const auto Intent = IntentFor(TEXT("Alternate"));
  FAssetDocumentPreparedMutationBatch Batch; const auto Prepared = PrepareAssetDocumentAnimationMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&Intent, 1), Batch);
  TestFalse(TEXT("invalid map/mesh/model rejected before capture"), Prepared.bValid); TestTrue(TEXT("invalid preparation publishes no contribution/footprint"), Batch.Contributions.IsEmpty());
  if (Case == 2 || Case == 3 || Case == 5)
   TestTrue(TEXT("save-unstable retained pose has specific refusal"), Prepared.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("AnimationProvider.RetargetSourceName.PreimageUnrepresentable"); }));
  Before.Check(*this, *A.Sequence); Sources.Check(*this, A); TestEqual(TEXT("invalid preparation preserves authored model"), A.Sequence->GetDataModel()->GenerateGuid(), Model);
  TestTrue(TEXT("invalid preparation preserves dirty target"), A.Sequence->GetOutermost()->IsDirty());
  TestTrue(TEXT("invalid preparation preserves installed file"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
  if (Case == 4) A.Sequence->GetDataModelInterface().GetObject()->ClearFlags(RF_NeedPostLoad);
 }
 // Drift after preparation must fail capture, with no transaction or inverse.
 for (int32 Case = 0; Case < 3; ++Case)
 {
  FAssets A; if (!TestTrue(TEXT("saved stale-source fixture"), A.Setup())) return false;
  const auto Request = RequestFor(A); const auto Intent = IntentFor(TEXT("Alternate")); FAssetDocumentPreparedMutationBatch Batch;
  if (!TestTrue(TEXT("valid state prepares before deliberate drift"), PrepareAssetDocumentAnimationMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&Intent, 1), Batch).bValid)) return false;
  if (Case == 0) A.Skeleton->AnimRetargetSources.FindChecked(TEXT("Default")).ReferencePose[1].SetTranslation(FVector(31., 0., 0.));
  if (Case == 1) { FReferenceSkeletonModifier Bones(A.Stored->GetRefSkeleton(), A.Skeleton.Get()); Bones.UpdateRefPoseTransform(1, FTransform(FVector(21., 0., 0.))); }
  if (Case == 2)
  {
   TArray<FVector3f> P, S; TArray<FQuat4f> Q; P.Init(FVector3f(51.f, 0.f, 0.f), 13); S.Init(FVector3f::OneVector, 13); Q.Init(FQuat4f::Identity, 13);
   A.Sequence->GetController().SetBoneTrackKeys(TEXT("child"), P, Q, S, false); A.Sequence->WaitOnExistingCompression();
  }
  const auto Before = FValues::Read(*A.Sequence); const FSourceSnapshot Sources(A); const FGuid Model = A.Sequence->GetDataModel()->GenerateGuid();
  const bool Dirty = A.Sequence->GetOutermost()->IsDirty(); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics; auto Lease = Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
  TestFalse(TEXT("stale map/used mesh/model publishes no valid capture"), Lease && Lease->IsValid()); if (Lease) Lease->Close();
  Before.Check(*this, *A.Sequence); Sources.Check(*this, A); TestEqual(TEXT("stale refusal preserves external model state"), A.Sequence->GetDataModel()->GenerateGuid(), Model);
  TestEqual(TEXT("stale refusal preserves target dirty"), A.Sequence->GetOutermost()->IsDirty(), Dirty);
  TestTrue(TEXT("stale refusal preserves installed file"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceRetargetSourceNameCoreSetNoneNoopTest,
 "UEAssetDoc.AnimationProvider.SequenceRetargetSourceNameCore.SetNoneNoop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceRetargetSourceNameCoreSetNoneNoopTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceRetargetSourceNameIntegrationTests;
 for (const ECase Case : {ECase::SetDefault, ECase::SetAlternate, ECase::SetNone, ECase::Unknown, ECase::CaseOnly, ECase::NoopNone})
 {
  FAssets A; if (!TestTrue(TEXT("saved set/None/noop fixture"), A.Setup(Case == ECase::SetDefault || Case == ECase::NoopNone))) return false;
  const FSourceSnapshot Sources(A); const FValues Expected = DesiredValues(A, Case); const FString Guid = A.Sequence->GetDataModel()->GenerateGuid().ToString();
  CheckUsage(*this, *A.Sequence, Case == ECase::SetDefault || Case == ECase::NoopNone ? 30. : 20.);
  const auto Run = Execute(*this, A, Case, false);
  TestEqual(TEXT("actual Core name mutation/save succeeds"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
  TestEqual(TEXT("one actual invocation"), Run.BatchCalls, 1); TestEqual(TEXT("precise native changed set"), Run.ChangedCount, IsNoop(Case) ? 0 : 1);
  TestTrue(TEXT("real Repair observes desired name/stored pose/compressed usage"), Run.bForwardObserved);
  TestEqual(TEXT("one actual staged native verifier"), Run.VerifierCalls, 1); TestEqual(TEXT("capture closes once before save"), Run.Closes, 1); TestFalse(TEXT("capture closed"), Run.bCaptureOpen);
  Expected.Check(*this, *A.Sequence); CheckEffectivePose(*this, *A.Sequence, DesiredPose(Case)); CheckUsage(*this, *A.Sequence, 50. - DesiredPose(Case));
  Retained(*this, *A.Sequence, A.Skeleton->GetPathName(), Guid, A.Skeleton->GetGuid().ToString()); Sources.Check(*this, A);
 }
 // The other save-stable stored-pose representation remains null + empty.
 FAssets Empty; if (!TestTrue(TEXT("null/empty stored pose fixture"), Empty.Setup(false, false))) return false;
 const auto Before = FValues::Read(*Empty.Sequence); const auto Sources = FSourceSnapshot(Empty); const auto Run = Execute(*this, Empty, ECase::SetNone, false);
 TestEqual(TEXT("None with null/empty retained storage succeeds"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
 auto Expected = Before; Expected.Name = FName(NAME_None).ToString(); Expected.Check(*this, *Empty.Sequence); Sources.Check(*this, Empty);
 CheckEffectivePose(*this, *Empty.Sequence, 10.); CheckUsage(*this, *Empty.Sequence, 40.);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceRetargetSourceNameCoreRollbackTest,
 "UEAssetDoc.AnimationProvider.SequenceRetargetSourceNameCore.LateFailureRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceRetargetSourceNameCoreRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceRetargetSourceNameIntegrationTests;
 for (const ECase Case : {ECase::SetAlternate, ECase::SetNone, ECase::CaseOnly})
  for (const bool bDirty : {false, true})
  {
   FAssets A; if (!TestTrue(TEXT("saved rollback fixture"), A.Setup())) return false;
   const auto Before = FValues::Read(*A.Sequence); const FSourceSnapshot Sources(A); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
   UAnimSequence* Original = A.Sequence.Get(); UPackage* Package = Original->GetOutermost(); UObject* Model = Original->GetDataModelInterface().GetObject();
   const FString Guid = Original->GetDataModel()->GenerateGuid().ToString(); Package->SetDirtyFlag(bDirty); const auto Run = Execute(*this, A, Case, true);
   TestEqual(TEXT("genuine Core later failure terminal"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
   TestTrue(TEXT("actual later Repair reached"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceRetargetSourceNameFixture.LateFailure"); }));
   TestEqual(TEXT("one Apply"), Run.ApplyCount, 1); TestEqual(TEXT("one late Repair"), Run.RepairCount, 1); TestTrue(TEXT("forward name/whole stored pose/native usage observed"), Run.bForwardObserved);
   TestTrue(TEXT("capture open at later failure"), Run.bOpenAtRepair); TestEqual(TEXT("real capture closes once"), Run.Closes, 1);
   TestTrue(TEXT("capture closes before inverse/invalidation"), Run.bClosedBeforeRollback); TestEqual(TEXT("actual rollback invalidation"), Run.Invalidations, 1);
   Before.Check(*this, *Original); CheckEffectivePose(*this, *Original, 30.); CheckUsage(*this, *Original, 20.);
   TestEqual(TEXT("original dirty restored last"), Package->IsDirty(), bDirty); TestEqual(TEXT("target identity retained"), A.Sequence.Get(), Original);
   TestEqual(TEXT("package identity retained"), Original->GetOutermost(), Package); TestEqual(TEXT("model identity retained"), Original->GetDataModelInterface().GetObject(), Model);
   Retained(*this, *Original, A.Skeleton->GetPathName(), Guid, A.Skeleton->GetGuid().ToString()); Sources.Check(*this, A);
   TestTrue(TEXT("installed package bytes unchanged by rollback"), Bytes(PackageFile(Original->GetPathName())) == Disk); TestEqual(TEXT("no verifier after late failure"), Run.VerifierCalls, 0);
   TestFalse(TEXT("no poisoned inverse"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
  }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceBoundRetargetSourceNameCoreTest,
 "UEAssetDoc.AnimationProvider.SequenceBoundCompletionCore.RetargetSourceName", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceBoundRetargetSourceNameCoreTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceRetargetSourceNameIntegrationTests;
 for (const ECase Case : {ECase::SetAlternate, ECase::SetNone, ECase::CaseOnly})
 {
  FAssets A; if (!TestTrue(TEXT("existing saved bound name/None/noop fixture"), A.Setup())) return false;
  const FSourceSnapshot Sources(A); const FValues Before = FValues::Read(*A.Sequence), Expected = DesiredValues(A, Case);
  UAnimSequence* Target = A.Sequence.Get(); UPackage* Package = Target->GetOutermost(); UObject* Model = Target->GetDataModelInterface().GetObject();
  USkeleton* Skeleton = A.Skeleton.Get(); USkeletalMesh* Stored = A.Stored.Get(); USkeletalMesh* DefaultSource = A.DefaultSource.Get(); USkeletalMesh* AlternateSource = A.AlternateSource.Get();
  const FGuid SkeletonGuid = Skeleton->GetGuid(); const FString Guid = Target->GetDataModel()->GenerateGuid().ToString();
  const auto Run = Execute(*this, A, Case, false, EBoundCase::Forward);
  TestEqual(TEXT("same bound owner name/None/noop commits through Core"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
  TestTrue(TEXT("real native completion observed before fixture Wait"), Run.bBoundCompletionObserved); TestTrue(TEXT("real forward observed in Repair"), Run.bForwardObserved);
  Expected.Check(*this, *Target); CheckEffectivePose(*this, *Target, DesiredPose(Case)); CheckUsage(*this, *Target, 50. - DesiredPose(Case));
  TestEqual(TEXT("case-only native FName equality is true noop"), Run.ChangedCount, Case == ECase::CaseOnly ? 0 : 1);
  if (Case == ECase::CaseOnly) TestTrue(TEXT("true noop keeps exact name display and entire stored pose"), Before.Matches(*Target));
  TestEqual(TEXT("one original Composition invocation"), Run.BatchCalls, 1); TestEqual(TEXT("existing real staged verifier runs"), Run.VerifierCalls, 1);
  TestEqual(TEXT("actual capture closes once"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  TestEqual(TEXT("target identity retained"), A.Sequence.Get(), Target); TestEqual(TEXT("package identity retained"), Target->GetOutermost(), Package);
  TestEqual(TEXT("model identity retained"), Target->GetDataModelInterface().GetObject(), Model); TestEqual(TEXT("Skeleton identity retained"), Target->GetSkeleton(), Skeleton);
  TestEqual(TEXT("Skeleton GUID retained"), Skeleton->GetGuid(), SkeletonGuid); TestEqual(TEXT("retained Mesh identity"), A.Stored.Get(), Stored);
  TestEqual(TEXT("Default Mesh identity retained"), A.DefaultSource.Get(), DefaultSource); TestEqual(TEXT("Alternate Mesh identity retained"), A.AlternateSource.Get(), AlternateSource);
  Retained(*this, *Target, Skeleton->GetPathName(), Guid, SkeletonGuid.ToString()); Sources.Check(*this, A); TestFalse(TEXT("Core saved target is clean"), Package->IsDirty());
 }
 for (const bool Dirty : {false, true})
 {
  FAssets A; if (!TestTrue(TEXT("existing saved bound name inverse fixture"), A.Setup())) return false;
  const FValues Before = FValues::Read(*A.Sequence); const FSourceSnapshot Sources(A); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
  UAnimSequence* Target = A.Sequence.Get(); UPackage* Package = Target->GetOutermost(); UObject* Model = Target->GetDataModelInterface().GetObject();
  USkeleton* Skeleton = A.Skeleton.Get(); USkeletalMesh* Stored = A.Stored.Get(); USkeletalMesh* DefaultSource = A.DefaultSource.Get(); USkeletalMesh* AlternateSource = A.AlternateSource.Get();
  const FGuid SkeletonGuid = Skeleton->GetGuid(); const FString Guid = Target->GetDataModel()->GenerateGuid().ToString(); const FTransform BeforeUsage = Usage(*Target);
  Package->SetDirtyFlag(Dirty); const auto Run = Execute(*this, A, ECase::SetAlternate, true, EBoundCase::Forward);
  TestEqual(TEXT("genuine bound later failure reaches Core"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestTrue(TEXT("specific existing late failure reached"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceRetargetSourceNameFixture.LateFailure"); }));
  TestTrue(TEXT("forward native completion precedes later failure"), Run.bBoundCompletionObserved); TestTrue(TEXT("desired name/storage/native use observed before inverse"), Run.bForwardObserved);
  TestEqual(TEXT("one actual Apply"), Run.ApplyCount, 1); TestEqual(TEXT("one later Repair"), Run.RepairCount, 1); TestEqual(TEXT("real forward changes one region"), Run.ChangedCount, 1);
  TestTrue(TEXT("capture open during later failure"), Run.bOpenAtRepair); TestTrue(TEXT("capture closes before actual Core inverse"), Run.bClosedBeforeRollback);
  TestEqual(TEXT("capture closes once"), Run.Closes, 1); TestEqual(TEXT("one real handle invalidation"), Run.Invalidations, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  Before.Check(*this, *Target); CheckEffectivePose(*this, *Target, 30.); CheckUsage(*this, *Target, 20.);
  TestTrue(TEXT("Core inverse restores complete native sample"), Usage(*Target).Equals(BeforeUsage, 1e-5)); TestEqual(TEXT("Core inverse restores dirty last"), Package->IsDirty(), Dirty);
  TestEqual(TEXT("inverse target identity"), A.Sequence.Get(), Target); TestEqual(TEXT("inverse package identity"), Target->GetOutermost(), Package);
  TestEqual(TEXT("inverse model identity"), Target->GetDataModelInterface().GetObject(), Model); TestEqual(TEXT("inverse Skeleton identity"), Target->GetSkeleton(), Skeleton);
  TestEqual(TEXT("inverse Skeleton GUID"), Skeleton->GetGuid(), SkeletonGuid); TestEqual(TEXT("inverse retained Mesh identity"), A.Stored.Get(), Stored);
  TestEqual(TEXT("inverse Default Mesh identity"), A.DefaultSource.Get(), DefaultSource); TestEqual(TEXT("inverse Alternate Mesh identity"), A.AlternateSource.Get(), AlternateSource);
  Retained(*this, *Target, Skeleton->GetPathName(), Guid, SkeletonGuid.ToString()); Sources.Check(*this, A);
  TestTrue(TEXT("later failure leaves installed target bytes"), Bytes(PackageFile(Target->GetPathName())) == Disk); TestEqual(TEXT("later failure precedes staged verifier"), Run.VerifierCalls, 0);
  TestFalse(TEXT("genuine Core inverse is not poisoned"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 for (const EBoundCase Case : {EBoundCase::MissingTarget, EBoundCase::ReadOnlyTarget, EBoundCase::WrongTargetClass, EBoundCase::ForeignTarget,
  EBoundCase::ForeignTransaction, EBoundCase::ForeignRequest, EBoundCase::ForeignRevision, EBoundCase::WrongPhase, EBoundCase::ExtraValue, EBoundCase::Closed, EBoundCase::StagedRevision})
 {
  FAssets A; if (!TestTrue(TEXT("existing saved bound refusal fixture"), A.Setup())) return false;
  const FValues Before = FValues::Read(*A.Sequence); const FSourceSnapshot Sources(A); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
  A.Sequence->GetOutermost()->SetDirtyFlag(true); const auto Run = Execute(*this, A, ECase::SetAlternate, false, Case);
  TestTrue(TEXT("actual bound refusal observed before Composition/Core recovery"), Run.bBoundRefusalObserved);
  if (Case == EBoundCase::StagedRevision) TestTrue(TEXT("actual staged API refusal observed"), Run.bStagedReadRejected);
  TestEqual(TEXT("original Core invocation fails"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestEqual(TEXT("one original Composition invocation"), Run.BatchCalls, 1); TestEqual(TEXT("no changed region"), Run.ChangedCount, 0);
  TestEqual(TEXT("refusal precedes Repair"), Run.RepairCount, 0); TestEqual(TEXT("refusal precedes staged verifier"), Run.VerifierCalls, 0);
  TestEqual(TEXT("actual capture closes once"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  Before.Check(*this, *A.Sequence); CheckEffectivePose(*this, *A.Sequence, 30.); CheckUsage(*this, *A.Sequence, 20.); Sources.Check(*this, A);
  TestTrue(TEXT("terminal refusal preserves original dirty"), A.Sequence->GetOutermost()->IsDirty());
  TestTrue(TEXT("terminal refusal leaves installed target bytes"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
  TestFalse(TEXT("refusal never poisons inverse"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 return !HasAnyErrors();
}
#endif
