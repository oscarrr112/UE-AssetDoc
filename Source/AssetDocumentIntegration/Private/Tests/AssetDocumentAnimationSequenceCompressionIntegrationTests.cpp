// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentMutationComposition.h"
#include "AssetDocumentAnimationRequestBinding.h"
#include "AssetDocumentEditorMutationPreparation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentAnimationPrimitiveProvider.h"
#include "AssetDocumentAnimationMutationPreparation.h"
#include "../../../AssetDocumentAnimation/Private/Provider/Sequence/AssetDocumentAnimationSequenceCompressionDefaultInputs.h"
#include "../../../AssetDocumentEditor/Private/Provider/Sequence/AssetDocumentEditorSequenceCompressionMutationTestHooks.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimCurveCompressionCodec_UniformIndexable.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "AnimationUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/AssetUserData.h"
#include "Engine/SkeletalMesh.h"
#include "EditorFramework/AssetImportData.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "String/LexFromString.h"
#include "UObject/GarbageCollection.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"

// One supplied-settings native batch, real Editor identities and Core inverse.
// Test-owned authoring is not a Profile or public ApplyFile entry.
namespace AssetDocumentSequenceCompressionIntegrationTests
{
constexpr const TCHAR* Primitive = TEXT("animation.sequence.compression.settings.set.v1");
constexpr const TCHAR* Schema = TEXT("animation.sequence.compression.settings.set.v1.input.v1");
constexpr const TCHAR* CompressionFieldNames[] = {TEXT("BoneCompressionSettings"), TEXT("CurveCompressionSettings"), TEXT("VariableFrameStrippingSettings")};
constexpr const TCHAR* ReferenceRoles[] = {TEXT("bone_compression_settings"), TEXT("curve_compression_settings"), TEXT("frame_stripping_settings")};
constexpr const TCHAR* SettingNames[] = {TEXT("OldBone"), TEXT("NewBone"), TEXT("OldCurve"), TEXT("NewCurve"), TEXT("OldFrame"), TEXT("NewFrame")};
FString PackageFile(const FString& Path)
{ return FPackageName::LongPackageNameToFilename(FPackageName::ObjectPathToPackageName(Path), FPackageName::GetAssetPackageExtension()); }
TArray<uint8> Bytes(const FString& Path)
{ TArray<uint8> R; FFileHelper::LoadFileToArray(R, *Path); return R; }
FString FileHash(const FString& Path)
{ const auto B = Bytes(Path); return FSHA1::HashBuffer(B.GetData(), B.Num()).ToString(); }
bool Save(UObject* Asset)
{
 FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
 const FString Path = PackageFile(Asset->GetPathName()); IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
 return UPackage::SavePackage(Asset->GetOutermost(), Asset, *Path, Args);
}
FAssetDocumentRuntimeValueDto String(const FString& V)
{ FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::String; R.StringValue = V; return R; }
FAssetDocumentRuntimeValueDto Number(double V)
{ FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::Number; R.NumberValue = V; return R; }
FAssetDocumentRuntimeValueDto BooleanDto(bool V)
{ FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::Boolean; R.BooleanValue = V; return R; }
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
 for (int32 I = 0; I < A.Num(); ++I) if (!T::StaticStruct()->CompareScriptStruct(&A[I], &B[I], 0)) return false; return true;
}
FAnimExtractContext CompressedContext(const UAnimSequence& S)
{ FAnimExtractContext C(0.25); C.bEnforceCompressedDataSampling = true; C.TargetPlatformHash = S.GetDerivedDataKeyHash(nullptr); return C; }
bool CheckUsage(FAutomationTestBase& Test, const UAnimSequence& S)
{
 bool Ok = Test.TestTrue(TEXT("native compressed bones valid"), S.IsCompressedDataValid());
 Ok &= Test.TestFalse(TEXT("native current compression is up to date"), S.IsCompressedDataOutOfDate());
 Ok &= Test.TestTrue(TEXT("public current-platform cache exists"), S.HasCompressedDataForPlatform(static_cast<const ITargetPlatform*>(nullptr)));
 const auto C = CompressedContext(S);
 {
  const auto D = S.GetCompressedData(C);
  Ok &= Test.TestTrue(TEXT("current hash selects a real curve codec"), D.Get().CurveCompressionCodec != nullptr);
  Ok &= Test.TestFalse(TEXT("authored float curve has a compressed stream"), D.Get().CompressedCurveByteStream.IsEmpty());
  Ok &= Test.TestTrue(TEXT("native compressed curves valid before sampling"), D.Get().IsCurveDataValid(&S));
 }
 if (!Ok) return false;
 FTransform Bone; S.GetBoneTransform(Bone, FSkeletonPoseBoneIndex(1), C, false);
 Ok &= Test.TestEqual(TEXT("golden baked child additive translation"), Bone.GetTranslation().X, 30., 1e-4);
 Ok &= Test.TestEqual(TEXT("golden baked child Y"), Bone.GetTranslation().Y, 0., 1e-4); Ok &= Test.TestEqual(TEXT("golden baked child Z"), Bone.GetTranslation().Z, 0., 1e-4);
 Ok &= Test.TestEqual(TEXT("actual compressed linear curve at .25"), S.EvaluateCurveData(TEXT("RetainedCompressionCurve"), C, false), 4.f, 1e-4f); return Ok;
}
struct FValues
{
 TArray<FString> References; float Error = 1.f; bool bKeep = false;
 static FValues Read(const UAnimSequence& S)
 { return {{S.BoneCompressionSettings->GetPathName(), S.CurveCompressionSettings->GetPathName(), S.VariableFrameStrippingSettings->GetPathName()}, S.CompressionErrorThresholdScale, static_cast<bool>(S.bDoNotOverrideCompression)}; }
 bool Matches(const UAnimSequence& S) const
 {
  if (!S.BoneCompressionSettings || !S.CurveCompressionSettings || !S.VariableFrameStrippingSettings) return false;
  const auto V = Read(S); if (References.Num() != V.References.Num() || V.Error != Error || V.bKeep != bKeep) return false;
  for (int32 I = 0; I < References.Num(); ++I) if (!References[I].Equals(V.References[I], ESearchCase::CaseSensitive)) return false; return true;
 }
 bool Check(FAutomationTestBase& Test, const UAnimSequence& S) const
 { return Test.TestTrue(TEXT("entire owned tuple including omitted fields exact"), Matches(S)); }
};
struct FPreparedDefaults
{
 FAssetDocumentRuntimeRequestDto Request;
 FAssetDocumentMutationIntent Intent;
 FAssetDocumentPreparedMutationBatch Batch;
 FValues Expected;
};
// Only the document proof uses this actual bound production provider. The
// invocation view is a read-only copy; ActualIntents and Batch are never rebuilt.
struct FPreparedDocument
{
 TSharedRef<FAssetDocumentAnimationRequestBinding> Binding = MakeShared<FAssetDocumentAnimationRequestBinding>();
 FPreparedDefaults Invocation;
 TArray<FAssetDocumentMutationIntent> ActualIntents;
 ~FPreparedDocument() { Binding->CloseRequest(); }
};
enum class ECase
{
 All, SparseScalar, NoopAll, NoopBone, MissingReference, MissingNoopReference, MutableReference, WrongReferenceClass,
 WrongReferenceIdentity, StaleReference, ForeignReferenceTransaction, MissingTarget, ReadOnlyTarget, WrongTargetClass,
 StaleTarget, ForeignTargetTransaction, ForeignRevision, WrongPhase, ExtraReference, RuntimeDrift
};
bool Sparse(ECase C) { return C == ECase::SparseScalar || C == ECase::ExtraReference; }
bool Noop(ECase C) { return C == ECase::NoopAll || C == ECase::NoopBone || C == ECase::MissingNoopReference; }
struct FAssets
{
 FString Root = TEXT("/Game/AssetDocumentSequenceCompression/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 FString DiskRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AssetDocumentSequenceCompression") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
 TStrongObjectPtr<USkeleton> Skeleton; TStrongObjectPtr<USkeletalMesh> Mesh; TStrongObjectPtr<UAnimSequence> Sequence;
 TStrongObjectPtr<UAnimBoneCompressionSettings> OldBone, NewBone; TStrongObjectPtr<UAnimCurveCompressionSettings> OldCurve, NewCurve;
 TStrongObjectPtr<UVariableFrameStrippingSettings> OldFrame, NewFrame; bool bKeep = false;
 TArray<UObject*> Settings() const { return {OldBone.Get(), NewBone.Get(), OldCurve.Get(), NewCurve.Get(), OldFrame.Get(), NewFrame.Get()}; }
 UObject* Reference(int32 I, bool New) const { return Settings()[I * 2 + (New ? 1 : 0)]; }
 bool Setup(bool bDefaultCompletion = false)
 {
  if (!IFileManager::Get().MakeDirectory(*(DiskRoot / TEXT("Installed")), true)) return false;
  // Test-owned initialization; neither Engine settings nor existing codecs are edited.
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  Mesh.Reset(NewObject<USkeletalMesh>(CreatePackage(*(Root + TEXT("/Mesh"))), TEXT("Mesh"), RF_Public | RF_Standalone)); Mesh->SetSkeleton(Skeleton.Get());
  { FReferenceSkeletonModifier B(Mesh->GetRefSkeleton(), Skeleton.Get()); B.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); B.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(10., 0., 0.))); }
  if (!Skeleton->MergeAllBonesToBoneTree(Mesh.Get(), false)) return false; Skeleton->SetBoneTranslationRetargetingMode(1, EBoneTranslationRetargetingMode::AnimationRelative);
  { FReferenceSkeletonModifier B(Mesh->GetRefSkeleton(), Skeleton.Get()); B.UpdateRefPoseTransform(1, FTransform(FVector(20., 0., 0.))); }
  auto& Retarget = Skeleton->AnimRetargetSources.Add(TEXT("RetainedSource")); Retarget.PoseName = TEXT("RetainedSource"); Retarget.SourceReferenceMesh = Mesh.Get();
  FAnimationRuntime::MakeSkeletonRefPoseFromMesh(Mesh.Get(), Skeleton.Get(), Retarget.ReferencePose);
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  Sequence->SetSkeleton(Skeleton.Get()); Sequence->RetargetSource = TEXT("RetainedSource"); Sequence->SetRetargetSourceAsset(Mesh.Get()); Sequence->UpdateRetargetSourceAssetData();
  Sequence->RateScale = 1.25f; Sequence->bLoop = true; Sequence->AdditiveAnimType = AAT_LocalSpaceBase; Sequence->RefPoseType = ABPT_RefPose; Sequence->RefFrameIndex = 0;
  Sequence->bEnableRootMotion = false; Sequence->RootMotionRootLock = ERootMotionRootLock::RefPose; Sequence->bForceRootLock = false; Sequence->bUseNormalizedRootMotionScale = false;
  Sequence->bAllowFrameStripping = bDefaultCompletion; Sequence->CompressionErrorThresholdScale = 1.f; Sequence->bDoNotOverrideCompression = false;
  auto& C = Sequence->GetController(); C.OpenBracket(FText::FromString(TEXT("Compression settings fixture")), false); C.InitializeModel(); C.SetFrameRate(FFrameRate(24, 1), false); C.SetNumberOfFrames(FFrameNumber(12), false);
  TArray<FVector3f> P, Scale; TArray<FQuat4f> Q; Scale.Init(FVector3f::OneVector, 13); Q.Init(FQuat4f::Identity, 13); bool Ok = true;
  for (const FName N : {FName(TEXT("root")), FName(TEXT("child"))})
  { P.Init(N == TEXT("root") ? FVector3f::ZeroVector : FVector3f(50.f, 0.f, 0.f), 13); Ok &= C.AddBoneCurve(N, false) && C.SetBoneTrackKeys(N, P, Q, Scale, false); }
  const FAnimationCurveIdentifier Curve(FName(TEXT("RetainedCompressionCurve")), ERawCurveTrackTypes::RCT_Float);
  // The default-codec fixture has the independent constant golden 4. The old
  // supplied-codec fixture retains its linear 2 -> 6 golden and all assertions.
  FRichCurveKey A(0.f, bDefaultCompletion ? 4.f : 2.f), B(0.5f, bDefaultCompletion ? 4.f : 6.f); A.InterpMode = B.InterpMode = RCIM_Linear;
  Ok &= C.AddCurve(Curve, AACF_Editable, false) && C.SetCurveKeys(Curve, {A, B}, false); C.NotifyPopulated(); C.CloseBracket(false); Sequence->WaitOnExistingCompression(); if (!Ok) return false;
  Sequence->ClearAllCachedCookedPlatformData(); Sequence->CacheDerivedDataForCurrentPlatform(); Sequence->WaitOnExistingCompression();
  if (!Sequence->BoneCompressionSettings || !Sequence->CurveCompressionSettings || !Sequence->VariableFrameStrippingSettings
   || !Sequence->BoneCompressionSettings->AreSettingsValid() || !Sequence->CurveCompressionSettings->AreSettingsValid()) return false;
  auto Copy = [&](UObject* Source, const TCHAR* Name)
  { auto* O = DuplicateObject(Source, CreatePackage(*(Root + TEXT("/") + Name)), FName(Name)); if (O) { O->ClearFlags(RF_Transient | RF_ArchetypeObject); O->SetFlags(RF_Public | RF_Standalone); } return O; };
  OldBone.Reset(Cast<UAnimBoneCompressionSettings>(Copy(Sequence->BoneCompressionSettings.Get(), SettingNames[0]))); NewBone.Reset(Cast<UAnimBoneCompressionSettings>(Copy(Sequence->BoneCompressionSettings.Get(), SettingNames[1])));
  OldCurve.Reset(Cast<UAnimCurveCompressionSettings>(Copy(Sequence->CurveCompressionSettings.Get(), SettingNames[2]))); NewCurve.Reset(Cast<UAnimCurveCompressionSettings>(Copy(Sequence->CurveCompressionSettings.Get(), SettingNames[3])));
  OldFrame.Reset(Cast<UVariableFrameStrippingSettings>(Copy(Sequence->VariableFrameStrippingSettings.Get(), SettingNames[4]))); NewFrame.Reset(Cast<UVariableFrameStrippingSettings>(Copy(Sequence->VariableFrameStrippingSettings.Get(), SettingNames[5])));
  for (UObject* O : Settings()) if (!O) return false;
  // Store the linear golden at native sample times without key-time quantization.
  // Only these newly created fixture settings receive a new owned stock codec.
  for (UAnimCurveCompressionSettings* FixtureCurve : {OldCurve.Get(), NewCurve.Get()})
   FixtureCurve->Codec = NewObject<UAnimCurveCompressionCodec_UniformIndexable>(FixtureCurve, TEXT("GoldenUniformCurveCodec"));
  if (!OldBone->AreSettingsValid() || !NewBone->AreSettingsValid() || !OldCurve->AreSettingsValid() || !NewCurve->AreSettingsValid()) return false;
  Sequence->BoneCompressionSettings = OldBone.Get(); Sequence->CurveCompressionSettings = OldCurve.Get(); Sequence->VariableFrameStrippingSettings = OldFrame.Get();
  Sequence->AnimNotifyTracks.SetNum(1); Sequence->AnimNotifyTracks[0].TrackName = TEXT("RetainedTrack");
  auto& Notify = Sequence->Notifies.AddDefaulted_GetRef(); Notify.NotifyName = TEXT("RetainedPoint"); Notify.Guid = FGuid(1, 2, 3, 4); Notify.TrackIndex = 0;
  Notify.Link(Sequence.Get(), 0.25f); Notify.EndLink.Link(Sequence.Get(), 0.25f);
  auto& Marker = Sequence->AuthoredSyncMarkers.AddDefaulted_GetRef(); Marker.MarkerName = TEXT("RetainedMarker"); Marker.Time = 0.25f; Marker.TrackIndex = 0; Marker.Guid = FGuid(5, 6, 7, 8);
  Skeleton->RegisterMarkerName(Marker.MarkerName);
  Sequence->ClearAllCachedCookedPlatformData(); Sequence->CacheDerivedDataForCurrentPlatform(); Sequence->WaitOnExistingCompression();
  auto Assets = Settings(); Assets.Insert(Skeleton.Get(), 0); Assets.Insert(Mesh.Get(), 1); Assets.Add(Sequence.Get());
  for (UObject* O : Assets) { if (O != Sequence.Get()) FAssetRegistryModule::AssetCreated(O); if (!Save(O)) return false; }
  if (const auto* Data = Sequence->GetAssetUserDataArray()) if (Data->ContainsByPredicate([](const auto* O) { return O != nullptr; })) return false; return true;
 }
 ~FAssets()
 {
  if (Sequence.IsValid()) Sequence->WaitOnExistingCompression(); if (bKeep) return;
  auto Assets = Settings(); Assets.Insert(Sequence.Get(), 0); Assets.Add(Mesh.Get()); Assets.Add(Skeleton.Get());
  for (UObject* O : Assets) if (IsValid(O))
  { UPackage* P = O->GetOutermost(); O->ClearFlags(RF_Public | RF_Standalone); O->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional); O->MarkAsGarbage(); P->SetDirtyFlag(false); P->MarkAsGarbage(); }
  // Saved packages remain for node-owned cleanup after Editor exit.
  IFileManager::Get().DeleteDirectory(*DiskRoot, false, true);
 }
};
FValues Desired(const FAssets& A, ECase C)
{
 auto V = FValues::Read(*A.Sequence); if (Noop(C)) return V;
 V.Error = 2.f; if (Sparse(C)) return V;
 for (int32 I = 0; I < 3; ++I) V.References[I] = A.Reference(I, true)->GetPathName(); V.bKeep = true; return V;
}
FAssetDocumentRuntimeValueDto Value(ECase Case, const FValues& V)
{
 FAssetDocumentRuntimeValueDto FieldsValue; FieldsValue.Kind = EAssetDocumentRuntimeValueKind::Object;
 auto Add = [&](const TCHAR* N, FAssetDocumentRuntimeValueDto F) { FieldsValue.ObjectKeys.Add(N); FieldsValue.Elements.Add(MoveTemp(F)); };
 if (!Sparse(Case)) for (int32 I = 0; I < (Case == ECase::NoopBone ? 1 : 3); ++I) Add(CompressionFieldNames[I], String(V.References[I]));
 if (Case != ECase::NoopBone) { Add(TEXT("CompressionErrorThresholdScale"), Number(V.Error)); if (!Sparse(Case)) Add(TEXT("bDoNotOverrideCompression"), BooleanDto(V.bKeep)); }
 FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::Object; R.ObjectKeys = {TEXT("fields")}; R.Elements = {MoveTemp(FieldsValue)}; return R;
}
TArray<uint8> NativePersistentState(UObject& O)
{ TArray<uint8> B; FMemoryWriter W(B, true); FObjectAndNameAsStringProxyArchive Ar(W, false); O.Serialize(Ar); return B; }
struct FRetained
{
 FString SkeletonPath, ModelGuid; FGuid SkeletonGuid; FName RetargetSource; FSoftObjectPath RetargetAsset;
 TArray<FTransform> StoredPose; TArray<FFloatCurve> Curves; TArray<FAnimNotifyEvent> Notifies; TArray<FAnimNotifyTrack> Tracks; TArray<FAnimSyncMarker> Markers;
 FFrameRate SamplingRate; int32 CompressionVersion, Keys; EAnimInterpolationType Interpolation; EStripAnimDataOnDedicatedServerSettings StripOnServer;
 TObjectPtr<UAssetImportData> ImportData; TArray<uint8> ImportState; bool bRootFromMontage, bFrameStripping; TArray<FTransformCurve> TransformCurves;
 explicit FRetained(const UAnimSequence& S) : SkeletonPath(S.GetSkeleton()->GetPathName()), ModelGuid(S.GetDataModel()->GenerateGuid().ToString()), SkeletonGuid(S.GetSkeletonGuid()), RetargetSource(S.RetargetSource),
  RetargetAsset(S.GetRetargetSourceAsset().ToSoftObjectPath()), StoredPose(S.RetargetSourceAssetReferencePose), Curves(S.GetDataModel()->GetFloatCurves()), Notifies(S.Notifies), Tracks(S.AnimNotifyTracks), Markers(S.AuthoredSyncMarkers),
  SamplingRate(S.GetSamplingFrameRate()), CompressionVersion(S.CompressCommandletVersion), Keys(S.GetDataModel()->GetNumberOfKeys()), Interpolation(S.Interpolation), StripOnServer(S.StripAnimDataOnDedicatedServer),
  ImportData(S.AssetImportData), ImportState(ImportData ? NativePersistentState(*ImportData) : TArray<uint8>{}), bRootFromMontage(S.bRootMotionSettingsCopiedFromMontage), bFrameStripping(S.bAllowFrameStripping), TransformCurves(S.GetDataModel()->GetTransformCurves()) {}
 bool Check(FAutomationTestBase& T, const UAnimSequence& S, bool bFresh = false) const
 {
  bool Ok = T.TestNotNull(TEXT("retained Skeleton loaded"), S.GetSkeleton()); if (!Ok) return false;
  Ok &= T.TestEqual(TEXT("Skeleton path retained"), S.GetSkeleton()->GetPathName(), SkeletonPath); Ok &= T.TestEqual(TEXT("Skeleton GUID retained"), S.GetSkeletonGuid(), SkeletonGuid);
  Ok &= T.TestEqual(TEXT("Skeleton native GUID agrees"), S.GetSkeleton()->GetGuid(), SkeletonGuid); Ok &= T.TestTrue(TEXT("native RetargetSource exact display/identity retained"), S.RetargetSource.IsEqual(RetargetSource, ENameCase::CaseSensitive, true));
  Ok &= T.TestTrue(TEXT("retarget asset path retained exactly"), S.GetRetargetSourceAsset().ToSoftObjectPath().ToString().Equals(RetargetAsset.ToString(), ESearchCase::CaseSensitive)); Ok &= T.TestTrue(TEXT("ENTIRE stored reference pose exact"), ExactPose(StoredPose, S.RetargetSourceAssetReferencePose));
  Ok &= T.TestEqual(TEXT("full native model content retained"), S.GetDataModel()->GenerateGuid().ToString(), ModelGuid); Ok &= T.TestTrue(TEXT("full native float curves retained"), SameStructs(Curves, S.GetDataModel()->GetFloatCurves()));
  auto ExpectedNotifies = Notifies;
  if (bFresh)
  {
   // A staged package has a distinct owning Sequence. Rebind only the local
   // expected copy's native self links, then compare the complete notify structs.
   for (auto& N : ExpectedNotifies)
   {
    const float Time = N.GetTime(); N.Link(const_cast<UAnimSequence*>(&S), Time, N.GetSlotIndex());
    N.EndLink.Link(const_cast<UAnimSequence*>(&S), Time + N.Duration, N.GetSlotIndex());
   }
   for (const auto& N : S.Notifies)
    Ok &= T.TestTrue(TEXT("fresh notify start/end links bind the loaded target"), N.GetLinkedSequence() == &S && N.EndLink.GetLinkedSequence() == &S);
  }
  if (!SameStructs(ExpectedNotifies, S.Notifies) && Notifies.Num() == 1 && S.Notifies.Num() == 1)
   T.AddInfo(FString::Printf(TEXT("Notify link diagnostic: before=%s after=%s beforeTime=%f afterTime=%f fresh=%d"),
    *GetPathNameSafe(Notifies[0].GetLinkedSequence()), *GetPathNameSafe(S.Notifies[0].GetLinkedSequence()), Notifies[0].GetTime(), S.Notifies[0].GetTime(), bFresh));
  Ok &= T.TestTrue(TEXT("full notifies retained"), SameStructs(ExpectedNotifies, S.Notifies)); Ok &= T.TestTrue(TEXT("full notify tracks retained"), SameStructs(Tracks, S.AnimNotifyTracks)); Ok &= T.TestTrue(TEXT("full markers retained"), SameStructs(Markers, S.AuthoredSyncMarkers));
  TArray<FName> Names; S.GetDataModel()->GetBoneTrackNames(Names); Ok &= T.TestTrue(TEXT("ordered bone names retained"), Names == TArray<FName>{TEXT("root"), TEXT("child")});
  Ok &= T.TestEqual(TEXT("native frame rate retained"), S.GetDataModel()->GetFrameRate(), FFrameRate(24, 1)); Ok &= T.TestEqual(TEXT("native frames retained"), S.GetDataModel()->GetNumberOfFrames(), 12);
  Ok &= T.TestEqual(TEXT("sampling rate retained"), S.GetSamplingFrameRate(), SamplingRate); Ok &= T.TestEqual(TEXT("number of native keys retained"), S.GetDataModel()->GetNumberOfKeys(), Keys);
  Ok &= T.TestEqual(TEXT("native length retained"), S.GetPlayLength(), 0.5f); Ok &= T.TestTrue(TEXT("full transform curves retained"), SameStructs(TransformCurves, S.GetDataModel()->GetTransformCurves()));
  Ok &= T.TestEqual(TEXT("compression version retained"), S.CompressCommandletVersion, CompressionVersion); Ok &= T.TestEqual(TEXT("interpolation retained"), S.Interpolation, Interpolation);
  Ok &= T.TestEqual(TEXT("dedicated-server stripping retained"), S.StripAnimDataOnDedicatedServer, StripOnServer);
  Ok &= T.TestTrue(TEXT("import-data identity or fresh owned object retained"), bFresh ? ((S.AssetImportData != nullptr) == (ImportData != nullptr)) : S.AssetImportData == ImportData);
  if (S.AssetImportData && ImportData)
  {
   Ok &= T.TestEqual(TEXT("import-data native class retained"), S.AssetImportData->GetClass(), ImportData->GetClass());
   Ok &= T.TestTrue(TEXT("full import-data authored content retained"), NativePersistentState(*S.AssetImportData) == ImportState);
  }
  Ok &= T.TestEqual(TEXT("montage root-motion setting retained"), S.bRootMotionSettingsCopiedFromMontage, bRootFromMontage);
  Ok &= T.TestEqual(TEXT("additive type retained"), S.AdditiveAnimType.GetValue(), AAT_LocalSpaceBase); Ok &= T.TestEqual(TEXT("additive base retained"), S.RefPoseType.GetValue(), ABPT_RefPose);
  Ok &= T.TestEqual(TEXT("additive frame retained"), S.RefFrameIndex, 0); Ok &= T.TestNull(TEXT("additive reference remains empty"), S.RefPoseSeq.Get()); Ok &= T.TestEqual(TEXT("playback retained"), S.RateScale, 1.25f);
  Ok &= T.TestTrue(TEXT("loop retained"), S.bLoop); Ok &= T.TestFalse(TEXT("root motion retained off"), S.bEnableRootMotion); Ok &= T.TestFalse(TEXT("force root retained off"), S.bForceRootLock);
  Ok &= T.TestFalse(TEXT("normalized root retained off"), S.bUseNormalizedRootMotionScale); Ok &= T.TestEqual(TEXT("root lock retained"), S.RootMotionRootLock.GetValue(), ERootMotionRootLock::RefPose);
  Ok &= T.TestEqual(TEXT("unowned frame stripping flag retained"), static_cast<bool>(S.bAllowFrameStripping), bFrameStripping); Ok &= T.TestNull(TEXT("PreviewMesh remains empty"), static_cast<const UAnimationAsset&>(S).GetPreviewMesh());
  Ok &= T.TestEqual(TEXT("metadata remains empty"), S.GetMetaData().Num(), 0); if (const auto* U = S.GetAssetUserDataArray()) Ok &= T.TestTrue(TEXT("user data remains empty"), U->IsEmpty()); return Ok;
 }
};
struct FInputs
{
 struct FObject
 {
  TStrongObjectPtr<UObject> Object; UObject* Outer; UClass* Class; uint32 Id; FString Path; TArray<uint8> Native;
  explicit FObject(UObject* O) : Object(O), Outer(O->GetOuter()), Class(O->GetClass()), Id(O->GetUniqueID()), Path(O->GetPathName()), Native(NativePersistentState(*O)) {}
 };
 TArray<FObject> Objects; TArray<TArray<uint8>> Files; TArray<bool> Dirty; TArray<FTransform> SkeletonPose, MeshPose; TMap<FName, FReferencePose> Map;
 TArray<TStrongObjectPtr<UObject>> ExtraInputs;
 TArray<UObject*> Sources(const FAssets& A) const
 { auto R = A.Settings(); R.Insert(A.Skeleton.Get(), 0); R.Insert(A.Mesh.Get(), 1); for (const auto& O : ExtraInputs) R.Add(O.Get()); return R; }
 explicit FInputs(const FAssets& A, TConstArrayView<UObject*> Extra = {}) : SkeletonPose(A.Skeleton->GetReferenceSkeleton().GetRefBonePose()), MeshPose(A.Mesh->GetRefSkeleton().GetRefBonePose()), Map(A.Skeleton->AnimRetargetSources)
 {
  for (UObject* O : Extra) ExtraInputs.Emplace(O);
  for (UObject* O : Sources(A))
  { Files.Add(Bytes(PackageFile(O->GetPathName()))); Dirty.Add(O->GetOutermost()->IsDirty()); Objects.Emplace(O); TArray<UObject*> Owned; GetObjectsWithOuter(O, Owned); for (UObject* Child : Owned) Objects.Emplace(Child); }
 }
 void Check(FAutomationTestBase& T, const FAssets& A) const
 {
  T.TestTrue(TEXT("full Skeleton reference pose unchanged"), ExactPose(SkeletonPose, A.Skeleton->GetReferenceSkeleton().GetRefBonePose())); T.TestTrue(TEXT("full Mesh reference pose unchanged"), ExactPose(MeshPose, A.Mesh->GetRefSkeleton().GetRefBonePose()));
  T.TestTrue(TEXT("full retarget map unchanged"), ExactMap(Map, A.Skeleton->AnimRetargetSources)); T.TestEqual(TEXT("Mesh Skeleton identity unchanged"), A.Mesh->GetSkeleton(), A.Skeleton.Get());
  const auto Inputs = Sources(A);
  for (int32 I = 0; I < Inputs.Num(); ++I)
  { T.TestTrue(TEXT("readonly settings/source package bytes unchanged"), Files[I] == Bytes(PackageFile(Inputs[I]->GetPathName()))); T.TestEqual(TEXT("readonly source dirty unchanged"), Inputs[I]->GetOutermost()->IsDirty(), Dirty[I]); }
  for (const auto& O : Objects)
  { T.TestEqual(TEXT("readonly identity revision retained"), O.Object->GetUniqueID(), O.Id); T.TestEqual(TEXT("readonly outer retained"), O.Object->GetOuter(), O.Outer); T.TestEqual(TEXT("readonly class retained"), O.Object->GetClass(), O.Class);
   T.TestTrue(TEXT("readonly canonical path retained"), O.Object->GetPathName().Equals(O.Path, ESearchCase::CaseSensitive));
   const auto CurrentNative = NativePersistentState(*O.Object);
   if (CurrentNative != O.Native)
   {
    int32 FirstDifference = 0; while (FirstDifference < FMath::Min(CurrentNative.Num(), O.Native.Num()) && CurrentNative[FirstDifference] == O.Native[FirstDifference]) ++FirstDifference;
    T.AddInfo(FString::Printf(TEXT("Readonly native diagnostic: %s class=%s oldSize=%d newSize=%d firstDifference=%d"), *O.Path, *O.Class->GetPathName(), O.Native.Num(), CurrentNative.Num(), FirstDifference));
   }
   T.TestTrue(TEXT("readonly native settings/codec/source authored state retained"), CurrentNative == O.Native); }
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
TOptional<FAssetDocumentOpaqueHandle> Resolve(UObject& O, EAssetDocumentHandleAccess Access, FResolver& Handles, IAssetDocumentAtomicTransaction& Transaction,
 EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Apply)
{
 FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = TEXT("editor.asset.resolve_existing.v1"); I.Version = 1;
 I.RequestId = Handles.Token.RequestId; I.TransactionId = Handles.Token.TransactionId; I.RuntimeRevision = Handles.Token.RuntimeRevision; I.Phase = Phase;
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
 FAssetDocumentRequestContextMetadata Metadata; FAssets& Assets; ECase Case; int32 ReferenceIndex; FResolver& Resolver;
 IAssetDocumentAtomicTransaction& Transaction; TSharedRef<FAssetDocumentMutationComposition> Composition; TSharedRef<IAssetDocumentPrimitiveProvider> Provider; int32& Calls;
 const FPreparedDefaults* Defaults;
public:
 FContext(FAssetDocumentRequestContextMetadata M, FAssets& A, ECase C, int32 Ref, FResolver& H, IAssetDocumentAtomicTransaction& T,
  TSharedRef<FAssetDocumentMutationComposition> P, TSharedRef<IAssetDocumentPrimitiveProvider> ProviderIn, int32& CallCount, const FPreparedDefaults* D = nullptr)
  : Metadata(M), Assets(A), Case(C), ReferenceIndex(Ref), Resolver(H), Transaction(T), Composition(P), Provider(ProviderIn), Calls(CallCount), Defaults(D) {}
 const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
 FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return {}; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& I) override
 { return Composition->Invoke(TEXT("animation"), I, Resolver, &Transaction, [&]() { return Provider->Invoke(I, Resolver, &Transaction); }); }
 FAssetDocumentPrimitiveOutcome Apply()
 {
  FAssetDocumentPrimitiveOutcome Failed; Failed.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed; Resolver.Token = Transaction.GetMutationToken();
  UObject& Target = Case == ECase::WrongTargetClass ? static_cast<UObject&>(*Assets.Mesh) : static_cast<UObject&>(*Assets.Sequence);
  auto H = Resolve(Target, Case == ECase::ReadOnlyTarget ? EAssetDocumentHandleAccess::ReadOnly : EAssetDocumentHandleAccess::Mutable, Resolver, Transaction, Metadata.Phase);
  if (!H.IsSet()) return Failed;
  FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = Primitive; I.Version = 1; I.RequestId = Metadata.RequestId;
  I.TransactionId = Transaction.GetTransactionId(); I.RuntimeRevision = Metadata.PinnedRuntimeRevision; I.Phase = Metadata.Phase;
  if (Case == ECase::StaleTarget) ++H->Generation;
  if (Case == ECase::ForeignTargetTransaction) H->TransactionId = FGuid::NewGuid();
  if (Case != ECase::MissingTarget) I.HandleArguments.Add({TEXT("sequence"), H.GetValue()});
  const auto Expected = Defaults ? Defaults->Expected : Desired(Assets, Case);
  I.ValueArguments = Defaults ? Defaults->Intent.Intent : Value(Case, Expected);
  if (Case == ECase::ForeignRevision) ++I.RuntimeRevision;
  if (Case == ECase::WrongPhase) I.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == ECase::RuntimeDrift) { auto V = Expected; V.Error = 3.f; I.ValueArguments = Value(Case, V); }
  const int32 Supplied = Defaults ? 3 : (Sparse(Case) ? 0 : (Case == ECase::NoopBone ? 1 : 3));
  for (int32 Index = 0; Index < Supplied; ++Index)
  {
   if (Defaults && !Defaults->Intent.Intent.Elements[0].ObjectKeys.Contains(CompressionFieldNames[Index])) continue;
   if (Index == ReferenceIndex && (Case == ECase::MissingReference || Case == ECase::MissingNoopReference)) continue;
   // Resolve the already-loaded actual prepared reference; never reconstruct
   // an intent from a settings path or replace the prepared contribution.
   UObject* Reference = Defaults ? FindObject<UObject>(nullptr, *Expected.References[Index]) : Assets.Reference(Index, !Noop(Case));
   if (!Reference) return Failed;
   if (Index == ReferenceIndex && Case == ECase::WrongReferenceClass) Reference = Assets.Mesh.Get();
   if (Index == ReferenceIndex && Case == ECase::WrongReferenceIdentity) Reference = Assets.Reference(Index, false);
   auto Ref = Resolve(*Reference, Index == ReferenceIndex && Case == ECase::MutableReference ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, Resolver, Transaction, Metadata.Phase);
   if (!Ref.IsSet()) return Failed;
   if (Index == ReferenceIndex && Case == ECase::StaleReference) ++Ref->Generation;
   if (Index == ReferenceIndex && Case == ECase::ForeignReferenceTransaction) Ref->TransactionId = FGuid::NewGuid();
   I.HandleArguments.Add({ReferenceRoles[Index], Ref.GetValue()});
  }
  if (Case == ECase::ExtraReference)
  {
   auto Ref = Resolve(*Assets.OldBone, EAssetDocumentHandleAccess::ReadOnly, Resolver, Transaction); if (!Ref.IsSet()) return Failed;
   I.HandleArguments.Add({ReferenceRoles[0], Ref.GetValue()});
  }
  ++Calls; return Invoke(I);
 }
};
class FHost final : public IAssetDocumentPythonRuntimeHost
{
 FAutomationTestBase& Test;
public:
 FAssets& Assets; FValues Expected; bool bFailRepair; bool& CaptureOpen;
 bool bForwardObserved = false, bOpenAtRepair = false; int32 ApplyCount = 0, RepairCount = 0, ChangedCount = 0;
 EAssetDocumentRuntimeRequestPhase MutationPhase;
 FHost(FAutomationTestBase& T, FAssets& A, FValues V, bool Fail, bool& Open, EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Apply)
  : Test(T), Assets(A), Expected(MoveTemp(V)), bFailRepair(Fail), CaptureOpen(Open), MutationPhase(Phase) {}
 FAssetDocumentRuntimeResultDto Dispatch(const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentNativeRequestContext& Context) override
 {
  FAssetDocumentRuntimeResultDto R; R.RequestId = Request.RequestId; R.RuntimeRevision = Request.ExpectedRuntimeRevision;
  R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
  auto InvokeMutation = [&]()
  {
   const auto Outcome = static_cast<FContext&>(Context).Apply(); R.ChangedSet = Outcome.ChangedSet; R.Diagnostics = Outcome.Diagnostics; ChangedCount = Outcome.ChangedSet.Num();
   if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) { R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed; }
  };
  if (Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Apply)
  {
   ++ApplyCount; if (MutationPhase == EAssetDocumentRuntimeRequestPhase::Apply) InvokeMutation();
  }
  else if (Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Repair)
  {
   ++RepairCount; bOpenAtRepair = CaptureOpen;
   if (MutationPhase == EAssetDocumentRuntimeRequestPhase::Repair)
   { InvokeMutation(); if (R.TerminalStatus == EAssetDocumentRuntimeTerminalStatus::Failed) return R; }
   // The leaf, not the observer, must already have completed its current cache.
   bForwardObserved = Expected.Matches(*Assets.Sequence) && CheckUsage(Test, *Assets.Sequence);
   CollectGarbage(RF_NoFlags);
   if (bFailRepair)
   {
    R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
    auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/SequenceCompressionFixture/Repair");
    D.Code = TEXT("SequenceCompressionFixture.LateFailure"); D.Message = TEXT("Test-only later failure after the actual supplied-settings writer and native completion."); D.Severity = EAssetDocumentDiagnosticSeverity::Error;
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
 FAutomationTestBase& Test; FAssets& Assets; FValues Expected; FRetained Retained; FInputs Sources;
public:
 int32 Calls = 0;
 FVerifier(FAutomationTestBase& T, FAssets& A, FValues V) : Test(T), Assets(A), Expected(MoveTemp(V)), Retained(*A.Sequence), Sources(A) {}
 FAssetDocumentFreshVerificationResult VerifyFresh(const FAssetDocumentFreshVerificationInput& Input) override
 {
  ++Calls; FAssetDocumentFreshVerificationResult R;
  const FString InstancedName = TEXT("/Temp/SequenceCompressionVerify_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  FLinkerInstancingContext Instancing; Instancing.AddPackageMapping(FName(*Assets.Sequence->GetOutermost()->GetName()), FName(*InstancedName));
  TStrongObjectPtr<UPackage> Package(LoadPackage(nullptr, *Input.StagedPackagePath, LOAD_ForDiff | LOAD_NoWarn, nullptr, &Instancing));
  auto* Sequence = Package.IsValid() ? FindObject<UAnimSequence>(Package.Get(), TEXT("Sequence")) : nullptr;
  if (!Test.TestNotNull(TEXT("staged Sequence ordinarily loads"), Sequence)) return R;
  Sequence->WaitOnExistingCompression(); Sources.Check(Test, Assets);
  R.bFreshLoadProven = Sequence != Assets.Sequence.Get() && Sequence->GetOutermost() != Assets.Sequence->GetOutermost()
   && FPaths::IsSamePath(Package->GetLoadedPath().GetLocalFullPath(), Input.StagedPackagePath);
  R.bCanonicalMatch = R.bFreshLoadProven && Expected.Check(Test, *Sequence) && Retained.Check(Test, *Sequence, true)
   && CheckUsage(Test, *Sequence) && Bytes(Input.StagedSidecarPath) == TArray<uint8>{0x7b, 0x7d};
  if (R.bCanonicalMatch) { R.ActualSemanticHash = Input.ExpectedSemanticHash; R.ActualCodecVersion = Input.ExpectedCodecVersion; }
  Sequence->ClearFlags(RF_Public | RF_Standalone); Package->SetDirtyFlag(false); Package->ClearFlags(RF_Standalone); return R;
 }
};
FAssetDocumentRegionEvidence Evidence()
{ FAssetDocumentRegionEvidence E; E.Kind = EAssetDocumentEvidenceKind::Present; E.SemanticHash = TEXT("sequence-compression-supplied-settings-fixture:v1"); E.CodecVersion = 1; return E; }
struct FRun
{
 FAssetDocumentRuntimeResultDto Result; bool bCaptureOpen = false, bClosedBeforeRollback = false, bForwardObserved = false, bOpenAtRepair = false;
 int32 Closes = 0, Invalidations = 0, ApplyCount = 0, RepairCount = 0, BatchCalls = 0, ChangedCount = 0, VerifierCalls = 0;
};
FAssetDocumentRuntimeRequestDto RequestFor(const FAssets& A)
{
 FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 85;
 R.Operation = EAssetDocumentRuntimeOperation::Apply; R.TargetAssetPath = A.Sequence->GetPathName(); R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence"); return R;
}
FAssetDocumentMutationIntent IntentFor(ECase Case, const FValues& Expected)
{
 FAssetDocumentMutationIntent I; I.StepIndex = 0; I.Requirement = {Primitive, 1, TEXT("animation_profile")};
 I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.InputSchemaId = Schema; I.Intent = Value(Case, Expected); return I;
}
TArray<UObject*> DefaultReferences()
{
 return {FAnimationUtils::GetDefaultAnimationBoneCompressionSettings(), FAnimationUtils::GetDefaultAnimationCurveCompressionSettings(),
  FAnimationUtils::GetDefaultVariableFrameStrippingSettings()};
}
bool CheckDefaults(FAutomationTestBase& Test, TConstArrayView<UObject*> Defaults)
{
 if (!Test.TestEqual(TEXT("three actual stock default inputs"), Defaults.Num(), 3)) return false;
 UClass* Classes[] = {UAnimBoneCompressionSettings::StaticClass(), UAnimCurveCompressionSettings::StaticClass(), UVariableFrameStrippingSettings::StaticClass()};
 for (int32 I = 0; I < Defaults.Num(); ++I)
  if (!Test.TestNotNull(TEXT("actual stock getter returns a loaded asset"), Defaults[I])
   || !Test.TestEqual(TEXT("actual default has exact native settings class"), Defaults[I]->GetClass(), Classes[I])
   || !Test.TestTrue(TEXT("actual stock default remains rooted"), Defaults[I]->IsRooted())
   || !Test.TestTrue(TEXT("actual stock default package exists"), FPaths::FileExists(PackageFile(Defaults[I]->GetPathName())))) return false;
 return true;
}
bool PrepareDefaults(FAutomationTestBase& Test, FAssets& Assets, TConstArrayView<AssetDocumentSequenceCompressionDefaultInputs::EKind> Selection,
 EAssetDocumentRuntimeRequestPhase Phase, TConstArrayView<UObject*> Defaults, FPreparedDefaults& Out)
{
 Out.Request = RequestFor(Assets); Out.Expected = FValues::Read(*Assets.Sequence);
 const auto Result = AssetDocumentSequenceCompressionDefaultInputs::Prepare(Out.Request, Selection, 0, Phase, Out.Intent, Out.Batch);
 for (const auto& D : Result.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("actual private default producer prepares the unchanged batch"), Result.bValid)) return false;
 if (!Test.TestEqual(TEXT("default literal has its original fields object"), Out.Intent.Intent.Elements.Num(), 1)) return false;
 const auto& Fields = Out.Intent.Intent.Elements[0];
 if (!Test.TestEqual(TEXT("only selected reference fields are supplied"), Fields.ObjectKeys.Num(), Selection.Num())
  || !Test.TestEqual(TEXT("one original inverse contribution"), Out.Batch.Contributions.Num(), 1)) return false;
 for (const auto Kind : Selection)
 {
  const int32 I = static_cast<int32>(Kind), Field = Fields.ObjectKeys.IndexOfByKey(CompressionFieldNames[I]);
  if (!Test.TestTrue(TEXT("selected native field exists"), Fields.Elements.IsValidIndex(Field))) return false;
  const auto& Literal = Fields.Elements[Field];
  if (!Test.TestEqual(TEXT("default remains a nonnull string input"), Literal.Kind, EAssetDocumentRuntimeValueKind::String)
   || !Test.TestTrue(TEXT("literal is the exact actual stock return path"), Literal.StringValue.Equals(Defaults[I]->GetPathName(), ESearchCase::CaseSensitive))) return false;
  Out.Expected.References[I] = Literal.StringValue;
 }
 return true;
}
FAssetDocumentRuntimeValueDto ObjectDto(TArray<FString> Keys = {}, TArray<FAssetDocumentRuntimeValueDto> Values = {})
{ FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::Object; R.ObjectKeys = MoveTemp(Keys); R.Elements = MoveTemp(Values); return R; }
FAssetDocumentRuntimeValueDto AssetRefDto(const UObject& Asset, bool bPackagePath, bool bClass = true)
{
 auto R = ObjectDto({TEXT("Kind"), TEXT("Path")}, {String(TEXT("AssetRef")), String(bPackagePath ? Asset.GetOutermost()->GetName() : Asset.GetPathName())});
 if (bClass) { R.ObjectKeys.Add(TEXT("Class")); R.Elements.Add(String(Asset.GetClass()->GetPathName())); } return R;
}
bool ExactDocumentDto(const FAssetDocumentRuntimeValueDto& A, const FAssetDocumentRuntimeValueDto& B)
{
 if (A.Kind != B.Kind || A.BooleanValue != B.BooleanValue || A.IntegerValue != B.IntegerValue || A.NumberValue != B.NumberValue
  || !A.StringValue.Equals(B.StringValue, ESearchCase::CaseSensitive) || A.BytesValue != B.BytesValue
  || A.ObjectKeys.Num() != B.ObjectKeys.Num() || A.Elements.Num() != B.Elements.Num()) return false;
 for (int32 I = 0; I < A.ObjectKeys.Num(); ++I) if (!A.ObjectKeys[I].Equals(B.ObjectKeys[I], ESearchCase::CaseSensitive)) return false;
 for (int32 I = 0; I < A.Elements.Num(); ++I) if (!ExactDocumentDto(A.Elements[I], B.Elements[I])) return false; return true;
}
// Modes 0/1 deliberately omit owned fields. Mode 2 changes all five fields for
// the genuine later-failure inverse; modes 3/4 only prepare scalar/ref inputs.
FAssetDocumentRuntimeValueDto CompressionDocument(const FAssets& A, int32 Mode)
{
 if (Mode == 3 || Mode == 4)
 {
  FAssetDocumentRuntimeValueDto Defaults; Defaults.Kind = EAssetDocumentRuntimeValueKind::Array;
  return ObjectDto({TEXT("supplied_values"), TEXT("reference_fragments"), TEXT("native_default_fields")},
   {Mode == 3 ? ObjectDto({TEXT("CompressionErrorThresholdScale")}, {Number(2.)}) : ObjectDto(),
    Mode == 4 ? ObjectDto({CompressionFieldNames[0]}, {AssetRefDto(*A.NewBone, false)}) : ObjectDto(), MoveTemp(Defaults)});
 }
 auto Supplied = Mode == 1 ? ObjectDto() : ObjectDto({TEXT("CompressionErrorThresholdScale"), TEXT("bDoNotOverrideCompression")}, {Number(2.), BooleanDto(true)});
 auto References = Mode == 0 ? ObjectDto({CompressionFieldNames[1]}, {AssetRefDto(*A.NewCurve, true)})
  : Mode == 1 ? ObjectDto({CompressionFieldNames[0]}, {AssetRefDto(*A.NewBone, false, false)})
  : ObjectDto({CompressionFieldNames[0], CompressionFieldNames[1]}, {AssetRefDto(*A.NewBone, true), AssetRefDto(*A.NewCurve, false)});
 FAssetDocumentRuntimeValueDto Defaults; Defaults.Kind = EAssetDocumentRuntimeValueKind::Array;
 Defaults.Elements.Add(String(CompressionFieldNames[Mode == 0 ? 0 : 2]));
 return ObjectDto({TEXT("supplied_values"), TEXT("reference_fragments"), TEXT("native_default_fields")}, {MoveTemp(Supplied), MoveTemp(References), MoveTemp(Defaults)});
}
FValues DocumentExpected(const FAssets& A, TConstArrayView<UObject*> Defaults, int32 Mode)
{
 auto R = FValues::Read(*A.Sequence);
 if (Mode == 3) { R.Error = 2.f; return R; }
 if (Mode == 4) { R.References[0] = A.NewBone->GetPathName(); return R; }
 if (Mode == 0) { R.References[0] = Defaults[0]->GetPathName(); R.References[1] = A.NewCurve->GetPathName(); }
 else { R.References[0] = A.NewBone->GetPathName(); R.References[2] = Defaults[2]->GetPathName(); if (Mode == 2) R.References[1] = A.NewCurve->GetPathName(); }
 if (Mode != 1) { R.Error = 2.f; R.bKeep = true; } return R;
}
bool PrepareDocument(FAutomationTestBase& Test, FAssets& A, const FAssetDocumentRuntimeValueDto& DocumentInputs,
 const FValues& Expected, int32 Mode, EAssetDocumentRuntimeRequestPhase Phase, FPreparedDocument& Out,
 TConstArrayView<FAssetDocumentMutationIntent> OtherIntents = {}, int32 Step = 0)
{
 Out.Invocation.Request = RequestFor(A); Out.Invocation.Expected = Expected;
 if (!Test.TestTrue(TEXT("actual production document binding owns the request"), Out.Binding->BindRequest(Out.Invocation.Request).bValid)) return false;
 const auto R = Out.Binding->PrepareSequenceCompressionDocument(Out.Invocation.Request, DocumentInputs, Step, Phase, OtherIntents, Out.ActualIntents, Out.Invocation.Batch);
 for (const auto& D : R.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("nondev bound document preparation succeeds"), R.bValid)
  || !Test.TestEqual(TEXT("original merged occurrences and no extra intents"), Out.ActualIntents.Num(), OtherIntents.Num() + 1)
  || !Test.TestEqual(TEXT("original owner supplies one contribution per occurrence"), Out.Invocation.Batch.Contributions.Num(), OtherIntents.Num() + 1)) return false;
 const int32 CompressionIndex = Out.ActualIntents.IndexOfByPredicate([](const auto& I) { return I.Requirement.PrimitiveId.Equals(Primitive, ESearchCase::CaseSensitive); });
 const int32 ContributionIndex = Out.Invocation.Batch.Contributions.IndexOfByPredicate([&](const auto& C) { return C.StepIndex == Step; });
 if (!Test.TestTrue(TEXT("exactly one Compression occurrence and its original contribution"), CompressionIndex != INDEX_NONE && ContributionIndex != INDEX_NONE
  && Out.ActualIntents.FilterByPredicate([](const auto& I) { return I.Requirement.PrimitiveId.Equals(Primitive, ESearchCase::CaseSensitive); }).Num() == 1)) return false;
 int32 ContributionCount = 0; for (const auto& C : Out.Invocation.Batch.Contributions) if (C.StepIndex == Step) ++ContributionCount;
 if (!Test.TestEqual(TEXT("one original Compression contribution at the requested step"), ContributionCount, 1)) return false;
 const auto& I = Out.ActualIntents[CompressionIndex];
 bool Ok = Test.TestEqual(TEXT("requested occurrence retained"), I.StepIndex, Step);
 Ok &= Test.TestEqual(TEXT("original contribution uses the requested occurrence"), Out.Invocation.Batch.Contributions[ContributionIndex].StepIndex, Step);
 Ok &= Test.TestTrue(TEXT("original supplied-v1 primitive retained"), I.Requirement.PrimitiveId.Equals(Primitive, ESearchCase::CaseSensitive));
 Ok &= Test.TestEqual(TEXT("original supplied-v1 version retained"), I.Requirement.Version, 1u);
 Ok &= Test.TestTrue(TEXT("original consumer retained"), I.Requirement.RequiredByRouteId.Equals(TEXT("animation_profile"), ESearchCase::CaseSensitive));
 Ok &= Test.TestTrue(TEXT("original input schema retained"), I.InputSchemaId.Equals(Schema, ESearchCase::CaseSensitive)); Ok &= Test.TestEqual(TEXT("requested phase retained"), I.Phase, Phase);
 if (!Test.TestTrue(TEXT("actual runtime literal is exactly one fields object"), I.Intent.Kind == EAssetDocumentRuntimeValueKind::Object
  && I.Intent.ObjectKeys.Num() == 1 && I.Intent.ObjectKeys[0].Equals(TEXT("fields"), ESearchCase::CaseSensitive)
  && I.Intent.Elements.Num() == 1 && I.Intent.Elements[0].Kind == EAssetDocumentRuntimeValueKind::Object)) return false;
 const auto& Fields = I.Intent.Elements[0];
 const TArray<FString> Keys = Mode == 0 ? TArray<FString>{CompressionFieldNames[0], CompressionFieldNames[1], TEXT("CompressionErrorThresholdScale"), TEXT("bDoNotOverrideCompression")}
  : Mode == 1 ? TArray<FString>{CompressionFieldNames[0], CompressionFieldNames[2]}
  : Mode == 3 ? TArray<FString>{TEXT("CompressionErrorThresholdScale")}
  : Mode == 4 ? TArray<FString>{CompressionFieldNames[0]}
  : TArray<FString>{CompressionFieldNames[0], CompressionFieldNames[1], CompressionFieldNames[2], TEXT("CompressionErrorThresholdScale"), TEXT("bDoNotOverrideCompression")};
 Ok &= Test.TestEqual(TEXT("only explicit fields appear in the merged literal"), Fields.ObjectKeys.Num(), Keys.Num());
 Ok &= Test.TestEqual(TEXT("actual field values form a complete object"), Fields.Elements.Num(), Keys.Num());
 for (const auto& Key : Keys)
 {
  const int32 Index = Fields.ObjectKeys.IndexOfByPredicate([&](const FString& K) { return K.Equals(Key, ESearchCase::CaseSensitive); });
  if (!Test.TestTrue(TEXT("each explicitly supplied reference/default/scalar occurs once"), Fields.Elements.IsValidIndex(Index)
   && Fields.ObjectKeys.FilterByPredicate([&](const FString& K) { return K.Equals(Key, ESearchCase::CaseSensitive); }).Num() == 1)) return false;
  const auto& V = Fields.Elements[Index];
  const int32 Reference = MakeArrayView(CompressionFieldNames).IndexOfByPredicate([&](const TCHAR* K) { return Key.Equals(K, ESearchCase::CaseSensitive); });
  if (Reference != INDEX_NONE) Ok &= Test.TestTrue(TEXT("actual resolved reference is its canonical loaded-object string"), V.Kind == EAssetDocumentRuntimeValueKind::String && V.StringValue.Equals(Expected.References[Reference], ESearchCase::CaseSensitive));
  else if (Key == TEXT("CompressionErrorThresholdScale")) Ok &= Test.TestTrue(TEXT("explicit Number lane preserved"), V.Kind == EAssetDocumentRuntimeValueKind::Number && V.NumberValue == Expected.Error);
  else Ok &= Test.TestTrue(TEXT("explicit Boolean lane preserved"), V.Kind == EAssetDocumentRuntimeValueKind::Boolean && V.BooleanValue == Expected.bKeep);
 }
 // Only used to issue the exact returned runtime values and readonly handles.
 // Freeze receives ActualIntents and the original Batch below, without patching.
 Out.Invocation.Intent = I; return Ok;
}
bool FreezeDocumentPreparation(FAutomationTestBase& Test, FPreparedDocument& Prepared)
{
 TArray<FAssetDocumentMutationComposition::FProviderBatch> Batches;
 auto& Editor = Batches.AddDefaulted_GetRef(); Editor.ProviderId = TEXT("editor"); Editor.ActualProvider = CreateAssetDocumentEditorPrimitiveProvider();
 if (!Test.TestTrue(TEXT("preparation proof uses the actual Editor parent batch"), PrepareAssetDocumentEditorMutationBatch(Prepared.Invocation.Request, {}, Editor.Batch).bValid)) return false;
 auto& Animation = Batches.AddDefaulted_GetRef(); Animation.ProviderId = TEXT("animation");
 Animation.ActualProvider = StaticCastSharedRef<IAssetDocumentPrimitiveProvider>(Prepared.Binding); Animation.Batch = MoveTemp(Prepared.Invocation.Batch);
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 const auto Composition = FAssetDocumentMutationComposition::Freeze(Prepared.Invocation.Request, MoveTemp(Prepared.ActualIntents), MoveTemp(Batches), {}, {}, {}, Diagnostics);
 for (const auto& D : Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 return Test.TestTrue(TEXT("original native intents/batch and same binding enter real Core Freeze"), Composition.IsValid());
}
FString FileState(const FString& Path)
{ return FPaths::FileExists(Path) ? TEXT("present:") + FileHash(Path) : TEXT("absent"); }
FString PackageState(const FString& Path)
{ return FileState(PackageFile(Path)) + TEXT("|") + FileState(FPackageName::LongPackageNameToFilename(FPackageName::ObjectPathToPackageName(Path), TEXT(".assetdoc.json"))); }
TArray<FString> SourceFiles(const FAssets& A, TConstArrayView<UObject*> Defaults)
{
 TArray<FString> R; auto Sources = A.Settings(); Sources.Insert(A.Skeleton.Get(), 0); Sources.Insert(A.Mesh.Get(), 1); Sources.Append(Defaults.GetData(), Defaults.Num());
 for (UObject* O : Sources) R.Add(PackageState(O->GetPathName())); return R;
}
FRun Execute(FAutomationTestBase& Test, FAssets& Assets, ECase Case, bool bFailRepair = false, int32 ReferenceIndex = 0,
 FPreparedDefaults* Defaults = nullptr, FPreparedDocument* Document = nullptr)
{
 FRun Run; const auto* Prepared = Document ? &Document->Invocation : Defaults;
 const auto Request = Prepared ? Prepared->Request : RequestFor(Assets); const auto Expected = Prepared ? Prepared->Expected : Desired(Assets, Case);
 TArray<FAssetDocumentMutationIntent> Intents;
 if (Document) Intents = MoveTemp(Document->ActualIntents);
 else Intents.Add(Prepared ? Prepared->Intent : IntentFor(Case, Expected));
 TSharedRef<IAssetDocumentPrimitiveProvider> Provider = Document
  ? StaticCastSharedRef<IAssetDocumentPrimitiveProvider>(Document->Binding) : CreateAssetDocumentAnimationPrimitiveProvider();
 auto EditorProvider = CreateAssetDocumentEditorPrimitiveProvider();
 TArray<FAssetDocumentMutationComposition::FProviderBatch> Batches;
 auto& Editor = Batches.AddDefaulted_GetRef(); Editor.ProviderId = TEXT("editor"); Editor.ActualProvider = EditorProvider;
 if (!Test.TestTrue(TEXT("real Editor parent prepares"), PrepareAssetDocumentEditorMutationBatch(Request, {}, Editor.Batch).bValid)) return Run;
 auto& Animation = Batches.AddDefaulted_GetRef(); Animation.ProviderId = TEXT("animation"); Animation.ActualProvider = Provider;
 if (Prepared) Animation.Batch = MoveTemp(Document ? Document->Invocation.Batch : Defaults->Batch);
 else
 {
  const auto A = PrepareAssetDocumentAnimationMutationBatch(Request, Intents, Animation.Batch);
  for (const auto& D : A.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
  if (!Test.TestTrue(TEXT("real compression participant prepares"), A.bValid)) return Run;
 }
 if (!Test.TestEqual(TEXT("one five-field Animation inverse contribution"), Animation.Batch.Contributions.Num(), 1)) return Run;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 const auto Composition = FAssetDocumentMutationComposition::Freeze(Request, MoveTemp(Intents), MoveTemp(Batches), {}, {}, {}, Diagnostics);
 for (const auto& D : Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("real production composition freezes"), Composition.IsValid())) return Run;
 FAssetDocumentPreflightReceipt Preflight; Preflight.RequestId = Request.RequestId; Preflight.RuntimeRevision = Request.ExpectedRuntimeRevision;
 Preflight.CompleteFootprint.Append(Composition->GetFootprint()); Preflight.ReceiptHash = TEXT("sha256:sequence-compression-supplied-settings-fixture"); Preflight.bSucceeded = true;
 FAssetDocumentPersistencePlan Plan; Plan.RequestId = Request.RequestId; Plan.TargetPackagePath = Assets.Sequence->GetOutermost()->GetName();
 Plan.TargetSidecarPath = Assets.DiskRoot / TEXT("Installed/Compression.assetdoc"); Plan.StagingRoot = Assets.DiskRoot / TEXT("Staging");
 Plan.JournalId = FGuid::NewGuid().ToString(EGuidFormats::Digits); Plan.Sidecar.Utf8Bytes = {0x7b, 0x7d}; Plan.Sidecar.SemanticHash = Evidence().SemanticHash; Plan.Sidecar.CodecVersion = 1;
 Plan.Footprint.Append(Composition->GetFootprint()); FResolver Resolver; FRegistry Registry; FCancellation Cancellation; FObserver Observer;
 FHost Host(Test, Assets, Expected, bFailRepair, Run.bCaptureOpen, Prepared ? Prepared->Intent.Phase : EAssetDocumentRuntimeRequestPhase::Apply); FVerifier Verifier(Test, Assets, Expected);
 FAssetDocumentCoreRuntimeComposition Core({Host, Registry, Cancellation, Observer,
  [&](const FAssetDocumentRequestContextMetadata& M, const FGuid&, IAssetDocumentAtomicTransaction* T, TArray<FAssetDocumentRuntimeDiagnostic>& Out) -> TUniquePtr<IAssetDocumentNativeRequestContext>
  {
   if (!T) return MakeUnique<FReadContext>(M); auto* Adapter = Composition->BindTransaction(*T, M, Out); if (!Adapter) return {};
   return MakeUnique<FContext>(M, Assets, Case, ReferenceIndex, Resolver, *Adapter, Composition.ToSharedRef(), Provider, Run.BatchCalls, Prepared);
  },
  [&](const FAssetDocumentHandleInvalidation& E)
  { ++Run.Invalidations; if (E.Reason == EAssetDocumentHandleInvalidationReason::Rollback) Run.bClosedBeforeRollback = !Run.bCaptureOpen; return Resolver.Invalidate(E); }});
 auto Opener = Composition->MakeCaptureOpener();
 FAssetDocumentCoreMutationInputs Mutation{Preflight, Plan, Verifier,
  [&](const FAssetDocumentCorePersistenceTargetRequest& I, TArray<FAssetDocumentRuntimeDiagnostic>&) -> TOptional<FAssetDocumentCorePersistenceTargetBinding>
  {
   if (!Composition->ValidateComplete().bValid || I.CanonicalObjectPath != Request.TargetAssetPath || I.ExactAssetClassPath != Request.RequestedAssetClassPath || I.TargetPackagePath != Plan.TargetPackagePath) return {};
   return FAssetDocumentCorePersistenceTargetBinding{I.RequestId, I.TransactionId, I.RuntimeRevision, Assets.Sequence.Get(), Assets.Sequence->GetOutermost()};
  },
  {Evidence(), Evidence()}, Assets.DiskRoot / TEXT("Journal"), Assets.DiskRoot / TEXT("Journal/Backup"), Assets.DiskRoot / TEXT("Sync"), TEXT("SequenceCompressionFixture"), Request.ExpectedRuntimeRevision,
  Composition->MakeInitialActions(),
  [&, Opener = MoveTemp(Opener)](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Out) mutable -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
  {
   auto Lease = Opener(Id, Out); if (!Lease || !Lease->IsValid()) return {}; Run.bCaptureOpen = true;
   return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([&, Lease = MoveTemp(Lease)]() mutable { Lease->Close(); Lease.Reset(); Run.bCaptureOpen = false; ++Run.Closes; });
  }};
 Run.Result = Core.ExecuteMutation(Request, MoveTemp(Mutation)); for (const auto& D : Run.Result.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 Run.bForwardObserved = Host.bForwardObserved; Run.bOpenAtRepair = Host.bOpenAtRepair;
 Run.ApplyCount = Host.ApplyCount; Run.RepairCount = Host.RepairCount; Run.ChangedCount = Host.ChangedCount; Run.VerifierCalls = Verifier.Calls; return Run;
}
class FScopedHooks
{
public:
 AssetDocumentEditorSequenceCompressionTesting::FHooks Hooks;
 explicit FScopedHooks(int32 FailAfter = INDEX_NONE) { Hooks.FailAfterField = FailAfter; AssetDocumentEditorSequenceCompressionTesting::SetHooks(&Hooks); }
 ~FScopedHooks() { AssetDocumentEditorSequenceCompressionTesting::SetHooks(nullptr); }
};
class FScopedDocumentSourceDrift
{
 FAutomationTestBase& Test; FAssets& Assets; bool bRename; FString Name; int32 Rate; bool Dirty;
public:
 bool bApplied = false;
 FScopedDocumentSourceDrift(FAutomationTestBase& T, FAssets& A, bool Rename)
  : Test(T), Assets(A), bRename(Rename), Name(A.NewCurve->GetName()), Rate(A.NewFrame->FrameStrippingRate.Default),
   Dirty((Rename ? A.NewCurve->GetOutermost() : A.NewFrame->GetOutermost())->IsDirty())
 {
  if (bRename) bApplied = Assets.NewCurve->Rename(TEXT("DriftedCurve"), nullptr, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
  else { Assets.NewFrame->FrameStrippingRate.Default += 1; bApplied = true; }
 }
 ~FScopedDocumentSourceDrift()
 {
  if (bRename && bApplied) Test.TestTrue(TEXT("test-owned reference name restored by fixture RAII"), Assets.NewCurve->Rename(*Name, nullptr, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty));
  if (!bRename) Assets.NewFrame->FrameStrippingRate.Default = Rate;
  (bRename ? Assets.NewCurve->GetOutermost() : Assets.NewFrame->GetOutermost())->SetDirtyFlag(Dirty);
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceCompressionCoreHandlesTest,
 "UEAssetDoc.AnimationProvider.SequenceCompressionCore.HandlePreflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceCompressionCoreHandlesTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceCompressionIntegrationTests;
 // Each named readonly role is independently required, including same-ref noops.
 for (const ECase Case : {ECase::MissingReference, ECase::MissingNoopReference, ECase::MutableReference, ECase::WrongReferenceClass,
  ECase::WrongReferenceIdentity, ECase::StaleReference, ECase::ForeignReferenceTransaction, ECase::MissingTarget, ECase::ReadOnlyTarget,
  ECase::WrongTargetClass, ECase::StaleTarget, ECase::ForeignTargetTransaction, ECase::ForeignRevision, ECase::WrongPhase, ECase::ExtraReference, ECase::RuntimeDrift})
 {
  const bool bReferenceCase = Case == ECase::MissingReference || Case == ECase::MissingNoopReference || Case == ECase::MutableReference
   || Case == ECase::WrongReferenceClass || Case == ECase::WrongReferenceIdentity || Case == ECase::StaleReference || Case == ECase::ForeignReferenceTransaction;
  for (int32 Reference = 0; Reference < (bReferenceCase ? 3 : 1); ++Reference)
  {
   FAssets A; if (!TestTrue(TEXT("saved handle authority fixture"), A.Setup())) return false;
   const auto Before = FValues::Read(*A.Sequence); const FRetained Retained(*A.Sequence); const FInputs Sources(A);
   const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName())); const bool Dirty = (Reference & 1) != 0; A.Sequence->GetOutermost()->SetDirtyFlag(Dirty);
   FScopedHooks Hooks; const auto Run = Execute(*this, A, Case, false, Reference);
   TestEqual(TEXT("actual Editor handle/value/phase refusal"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
   TestEqual(TEXT("one actual primitive invocation"), Run.BatchCalls, 1); TestEqual(TEXT("zero native field writes on refusal"), Hooks.Hooks.FieldWrites, 0);
   TestEqual(TEXT("zero native changed set"), Run.ChangedCount, 0); TestEqual(TEXT("no Repair after refusal"), Run.RepairCount, 0);
   TestEqual(TEXT("no staged verifier after refusal"), Run.VerifierCalls, 0); TestEqual(TEXT("Core closes native capture once"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
   Before.Check(*this, *A.Sequence); Retained.Check(*this, *A.Sequence); CheckUsage(*this, *A.Sequence); Sources.Check(*this, A);
   TestEqual(TEXT("refusal preserves original dirty"), A.Sequence->GetOutermost()->IsDirty(), Dirty);
   TestTrue(TEXT("refusal preserves installed target bytes"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
   TestFalse(TEXT("handle refusal never poisons inverse"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
  }
 }
 // These deliberately broken test-owned configurations never enter a transaction.
 for (int32 Case = 0; Case < 5; ++Case)
 {
  FAssets A; if (!TestTrue(TEXT("saved invalid settings fixture"), A.Setup())) return false;
  if (Case == 0) A.OldBone->Codecs.Reset();
  if (Case == 1) A.NewCurve->Codec = nullptr;
  if (Case == 2) A.OldFrame->SetFlags(RF_NeedPostLoad);
  if (Case == 3) A.NewFrame->SetFlags(RF_NeedPostLoad);
  if (Case == 4) A.Sequence->GetDataModelInterface().GetObject()->SetFlags(RF_NeedPostLoad);
  const auto Before = FValues::Read(*A.Sequence); const FRetained Retained(*A.Sequence); const FInputs Sources(A); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
  const bool Dirty = A.Sequence->GetOutermost()->IsDirty(); const auto Intent = IntentFor(ECase::All, Desired(A, ECase::All));
  FAssetDocumentPreparedMutationBatch Batch;
  const auto Prepared = PrepareAssetDocumentAnimationMutationBatch(RequestFor(A), TConstArrayView<FAssetDocumentMutationIntent>(&Intent, 1), Batch);
  TestFalse(TEXT("invalid old/new settings/model rejected before capture"), Prepared.bValid); TestTrue(TEXT("invalid preparation publishes no contribution or footprint"), Batch.Contributions.IsEmpty());
  Before.Check(*this, *A.Sequence); Retained.Check(*this, *A.Sequence); Sources.Check(*this, A); TestEqual(TEXT("invalid preparation preserves target dirty"), A.Sequence->GetOutermost()->IsDirty(), Dirty);
  TestTrue(TEXT("invalid preparation preserves installed file"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
  if (Case == 2) A.OldFrame->ClearFlags(RF_NeedPostLoad); if (Case == 3) A.NewFrame->ClearFlags(RF_NeedPostLoad);
  if (Case == 4) A.Sequence->GetDataModelInterface().GetObject()->ClearFlags(RF_NeedPostLoad);
 }
 for (int32 Case = 0; Case < 4; ++Case)
 {
  FAssets A; if (!TestTrue(TEXT("saved stale readonly fixture"), A.Setup())) return false;
  const auto Intent = IntentFor(ECase::All, Desired(A, ECase::All)); FAssetDocumentPreparedMutationBatch Batch;
  if (!TestTrue(TEXT("valid settings prepare before external drift"), PrepareAssetDocumentAnimationMutationBatch(RequestFor(A), TConstArrayView<FAssetDocumentMutationIntent>(&Intent, 1), Batch).bValid)) return false;
  if (Case == 0) A.OldBone->Codecs.Reset();
  if (Case == 1) A.NewFrame->FrameStrippingRate.Default += 1;
  if (Case == 2) { FReferenceSkeletonModifier B(A.Mesh->GetRefSkeleton(), A.Skeleton.Get()); B.UpdateRefPoseTransform(1, FTransform(FVector(21., 0., 0.))); }
  if (Case == 3) A.Sequence->CompressionErrorThresholdScale = 3.f;
  const auto Before = FValues::Read(*A.Sequence); const FRetained Retained(*A.Sequence); const FInputs Sources(A); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName())); const bool Dirty = A.Sequence->GetOutermost()->IsDirty();
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics; auto Lease = Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
  TestFalse(TEXT("stale old/new settings/mesh/tuple cannot open capture"), Lease && Lease->IsValid()); if (Lease) Lease->Close();
  Before.Check(*this, *A.Sequence); Retained.Check(*this, *A.Sequence); Sources.Check(*this, A); TestEqual(TEXT("stale refusal preserves external target dirty"), A.Sequence->GetOutermost()->IsDirty(), Dirty);
  TestTrue(TEXT("stale refusal preserves installed file"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceCompressionCoreSetTest,
 "UEAssetDoc.AnimationProvider.SequenceCompressionCore.SetSuppliedNoop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceCompressionCoreSetTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceCompressionIntegrationTests;
 for (const ECase Case : {ECase::All, ECase::SparseScalar, ECase::NoopAll, ECase::NoopBone})
 {
  FAssets A; if (!TestTrue(TEXT("saved supplied/noop fixture"), A.Setup())) return false;
  const auto Expected = Desired(A, Case); const FRetained Retained(*A.Sequence); const FInputs Sources(A);
  if (!CheckUsage(*this, *A.Sequence)) return false;
  FScopedHooks Hooks; const auto Run = Execute(*this, A, Case);
  TestEqual(TEXT("actual Core supplied-settings mutation/save succeeds"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
  TestEqual(TEXT("one actual invocation"), Run.BatchCalls, 1); TestEqual(TEXT("precise native changed set"), Run.ChangedCount, Noop(Case) ? 0 : 1);
  TestEqual(TEXT("fixed five-field writer or real noop"), Hooks.Hooks.FieldWrites, Noop(Case) ? 0 : 5);
  TestTrue(TEXT("Repair observes completed desired tuple/bone/curve usage"), Run.bForwardObserved);
  TestEqual(TEXT("one actual staged native verifier"), Run.VerifierCalls, 1); TestEqual(TEXT("capture closes once before save"), Run.Closes, 1); TestFalse(TEXT("capture closed"), Run.bCaptureOpen);
  Expected.Check(*this, *A.Sequence); Retained.Check(*this, *A.Sequence); CheckUsage(*this, *A.Sequence); Sources.Check(*this, A);
  TestFalse(TEXT("installed target is clean"), A.Sequence->GetOutermost()->IsDirty());
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceCompressionCorePartialRollbackTest,
 "UEAssetDoc.AnimationProvider.SequenceCompressionCore.PartialFailureRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceCompressionCorePartialRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceCompressionIntegrationTests;
 for (int32 Field = 0; Field < 5; ++Field)
  for (const bool Dirty : {false, true})
  {
   FAssets A; if (!TestTrue(TEXT("saved partial-writer rollback fixture"), A.Setup())) return false;
   const auto Before = FValues::Read(*A.Sequence); const FRetained Retained(*A.Sequence); const FInputs Sources(A);
   const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName())); UAnimSequence* Original = A.Sequence.Get(); UPackage* Package = Original->GetOutermost(); UObject* Model = Original->GetDataModelInterface().GetObject(); Package->SetDirtyFlag(Dirty);
   FScopedHooks Hooks(Field); const auto Run = Execute(*this, A, ECase::All);
   TestEqual(TEXT("real Core partial setter failure terminal"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
   TestEqual(TEXT("actual forward setter reached requested field"), Hooks.Hooks.FieldWrites, Field + 1);
   TestEqual(TEXT("one Apply"), Run.ApplyCount, 1); TestEqual(TEXT("no later Repair after partial write"), Run.RepairCount, 0);
   TestEqual(TEXT("partial writer publishes no changed set"), Run.ChangedCount, 0); TestEqual(TEXT("no verifier after partial write"), Run.VerifierCalls, 0);
   TestEqual(TEXT("capture closes once"), Run.Closes, 1); TestTrue(TEXT("closed capture precedes real inverse"), Run.bClosedBeforeRollback); TestEqual(TEXT("actual rollback invalidation"), Run.Invalidations, 1);
   Before.Check(*this, *Original); Retained.Check(*this, *Original); CheckUsage(*this, *Original); Sources.Check(*this, A);
   TestEqual(TEXT("target identity survives partial rollback"), A.Sequence.Get(), Original); TestEqual(TEXT("package identity survives partial rollback"), Original->GetOutermost(), Package);
   TestEqual(TEXT("model identity survives partial rollback"), Original->GetDataModelInterface().GetObject(), Model); TestEqual(TEXT("original dirty restored last"), Package->IsDirty(), Dirty);
   TestTrue(TEXT("partial rollback preserves installed file"), Bytes(PackageFile(Original->GetPathName())) == Disk);
   TestFalse(TEXT("partial writer inverse not poisoned"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
  }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceCompressionCoreLateRollbackTest,
 "UEAssetDoc.AnimationProvider.SequenceCompressionCore.LateFailureRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceCompressionCoreLateRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceCompressionIntegrationTests;
 for (const ECase Case : {ECase::All, ECase::SparseScalar, ECase::NoopAll})
  for (const bool Dirty : {false, true})
  {
   FAssets A; if (!TestTrue(TEXT("saved late-failure fixture"), A.Setup())) return false;
   const auto Before = FValues::Read(*A.Sequence); const FRetained Retained(*A.Sequence); const FInputs Sources(A); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
   UAnimSequence* Original = A.Sequence.Get(); UPackage* Package = Original->GetOutermost(); UObject* Model = Original->GetDataModelInterface().GetObject(); Package->SetDirtyFlag(Dirty);
   FScopedHooks Hooks; const auto Run = Execute(*this, A, Case, true);
   TestEqual(TEXT("genuine Core later failure terminal"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
   TestTrue(TEXT("later failure has exact fixture provenance"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceCompressionFixture.LateFailure"); }));
   TestEqual(TEXT("forward completes exactly one fixed batch"), Hooks.Hooks.FieldWrites, Noop(Case) ? 0 : 5);
   TestEqual(TEXT("one Apply"), Run.ApplyCount, 1); TestEqual(TEXT("one late Repair"), Run.RepairCount, 1); TestTrue(TEXT("completed forward tuple and compressed golden observed"), Run.bForwardObserved);
   TestTrue(TEXT("capture open at later failure"), Run.bOpenAtRepair); TestEqual(TEXT("capture closes once"), Run.Closes, 1);
   TestTrue(TEXT("capture closes before inverse/invalidation"), Run.bClosedBeforeRollback); TestEqual(TEXT("actual rollback invalidation"), Run.Invalidations, 1);
   Before.Check(*this, *Original); Retained.Check(*this, *Original); CheckUsage(*this, *Original); Sources.Check(*this, A);
   TestEqual(TEXT("original dirty restored last"), Package->IsDirty(), Dirty); TestEqual(TEXT("target identity retained"), A.Sequence.Get(), Original);
   TestEqual(TEXT("package identity retained"), Original->GetOutermost(), Package); TestEqual(TEXT("model identity retained"), Original->GetDataModelInterface().GetObject(), Model);
   TestTrue(TEXT("installed package bytes unchanged by rollback"), Bytes(PackageFile(Original->GetPathName())) == Disk); TestEqual(TEXT("no verifier after late failure"), Run.VerifierCalls, 0);
   TestFalse(TEXT("late inverse not poisoned"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
  }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceCompressionDefaultCoreApplyTest,
 "UEAssetDoc.AnimationProvider.SequenceCompressionDefaultCore.ApplyDefaultsAndNoop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceCompressionDefaultCoreApplyTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceCompressionIntegrationTests;
 using EKind = AssetDocumentSequenceCompressionDefaultInputs::EKind;
 const TArray<TArray<EKind>> Selections{{EKind::Bone}, {EKind::Curve}, {EKind::FrameStripping}, {EKind::FrameStripping, EKind::Curve, EKind::Bone}};
 for (int32 I = 0; I < Selections.Num(); ++I)
 {
  FAssets A; if (!TestTrue(TEXT("new GUID-owned default completion fixture"), A.Setup(true))) return false;
  const auto Defaults = DefaultReferences(); if (!CheckDefaults(*this, Defaults)) return false;
  const FRetained Retained(*A.Sequence); const FInputs Sources(A, Defaults); const auto Files = SourceFiles(A, Defaults);
  const auto Phase = I < 2 ? EAssetDocumentRuntimeRequestPhase::Apply : EAssetDocumentRuntimeRequestPhase::Repair;
  const bool Dirty = (I & 1) != 0; A.Sequence->GetOutermost()->SetDirtyFlag(Dirty);
  FPreparedDefaults Prepared; if (!PrepareDefaults(*this, A, Selections[I], Phase, Defaults, Prepared)) return false;
  const auto Expected = Prepared.Expected;
  {
   FScopedHooks Hooks; const auto Run = Execute(*this, A, ECase::All, false, 0, &Prepared);
   TestEqual(TEXT("actual default forward/Core ordinary save succeeds"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
   TestEqual(TEXT("one actual prepared literal invocation"), Run.BatchCalls, 1); TestEqual(TEXT("default forward changes one target"), Run.ChangedCount, 1);
   TestEqual(TEXT("original fixed five-field writer"), Hooks.Hooks.FieldWrites, 5);
   TestEqual(TEXT("one Apply phase"), Run.ApplyCount, 1); TestEqual(TEXT("one Repair phase"), Run.RepairCount, 1);
   TestTrue(TEXT("real completed default tuple and compressed goldens observed"), Run.bForwardObserved);
   TestEqual(TEXT("one real staged native verifier"), Run.VerifierCalls, 1); TestEqual(TEXT("real capture closes once before save"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  }
  Expected.Check(*this, *A.Sequence); Retained.Check(*this, *A.Sequence); CheckUsage(*this, *A.Sequence); Sources.Check(*this, A);
  UObject* Actual[] = {A.Sequence->BoneCompressionSettings.Get(), A.Sequence->CurveCompressionSettings.Get(), A.Sequence->VariableFrameStrippingSettings.Get()};
  for (const auto Kind : Selections[I]) TestEqual(TEXT("selected field is the actual stock return object"), Actual[static_cast<int32>(Kind)], Defaults[static_cast<int32>(Kind)]);
  TestTrue(TEXT("default reference retains enabled frame stripping"), A.Sequence->bAllowFrameStripping);
  TestTrue(TEXT("all source/default packages and companion sidecars unchanged"), SourceFiles(A, Defaults) == Files);
  TestFalse(TEXT("ordinary saved default target clean"), A.Sequence->GetOutermost()->IsDirty());

  A.Sequence->GetOutermost()->SetDirtyFlag(Dirty);
  FPreparedDefaults Noop; if (!PrepareDefaults(*this, A, Selections[I], Phase, Defaults, Noop)) return false;
  {
   FScopedHooks Hooks; const auto Run = Execute(*this, A, ECase::All, false, 0, &Noop);
   TestEqual(TEXT("actual default same-reference noop succeeds"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
   TestEqual(TEXT("noop still consumes one original literal"), Run.BatchCalls, 1); TestEqual(TEXT("noop has no changed target"), Run.ChangedCount, 0);
   TestEqual(TEXT("noop performs zero native setters"), Hooks.Hooks.FieldWrites, 0); TestTrue(TEXT("noop retains usable completed data"), Run.bForwardObserved);
   TestEqual(TEXT("noop ordinary save verifies once"), Run.VerifierCalls, 1); TestEqual(TEXT("noop closes capture once"), Run.Closes, 1); TestFalse(TEXT("noop capture closed"), Run.bCaptureOpen);
  }
  Expected.Check(*this, *A.Sequence); Retained.Check(*this, *A.Sequence); CheckUsage(*this, *A.Sequence); Sources.Check(*this, A);
  TestTrue(TEXT("noop keeps readonly packages and sidecars exact"), SourceFiles(A, Defaults) == Files);
  TestFalse(TEXT("ordinary saved noop target clean"), A.Sequence->GetOutermost()->IsDirty());
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceCompressionDefaultCoreRollbackTest,
 "UEAssetDoc.AnimationProvider.SequenceCompressionDefaultCore.LateFailureRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceCompressionDefaultCoreRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceCompressionIntegrationTests;
 using EKind = AssetDocumentSequenceCompressionDefaultInputs::EKind;
 for (const TArray<EKind>& Selection : {TArray<EKind>{EKind::FrameStripping}, TArray<EKind>{EKind::Bone, EKind::Curve, EKind::FrameStripping}})
  for (const bool Dirty : {false, true})
  {
   FAssets A; if (!TestTrue(TEXT("new GUID-owned default inverse fixture"), A.Setup(true))) return false;
   const auto Defaults = DefaultReferences(); if (!CheckDefaults(*this, Defaults)) return false;
   const auto Before = FValues::Read(*A.Sequence); const FRetained Retained(*A.Sequence); const FInputs Sources(A, Defaults); const auto Files = SourceFiles(A, Defaults);
   UAnimSequence* Original = A.Sequence.Get(); UPackage* Package = Original->GetOutermost(); UObject* Model = Original->GetDataModelInterface().GetObject();
   const auto Disk = Bytes(PackageFile(Original->GetPathName())); const auto Sidecar = FileState(A.DiskRoot / TEXT("Installed/Compression.assetdoc")); Package->SetDirtyFlag(Dirty);
   FPreparedDefaults Prepared; if (!PrepareDefaults(*this, A, Selection, EAssetDocumentRuntimeRequestPhase::Apply, Defaults, Prepared)) return false;
   FScopedHooks Hooks; const auto Run = Execute(*this, A, ECase::All, true, 0, &Prepared);
   TestEqual(TEXT("real later Core failure after completed default forward"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
   TestTrue(TEXT("exact later failure provenance"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceCompressionFixture.LateFailure"); }));
   TestEqual(TEXT("completed forward uses all five fixed setters"), Hooks.Hooks.FieldWrites, 5);
   TestEqual(TEXT("one real default invocation"), Run.BatchCalls, 1); TestEqual(TEXT("one Apply"), Run.ApplyCount, 1); TestEqual(TEXT("one later Repair"), Run.RepairCount, 1);
   TestTrue(TEXT("real completed default goldens observed before failure"), Run.bForwardObserved); TestTrue(TEXT("capture was open at genuine later failure"), Run.bOpenAtRepair);
   TestEqual(TEXT("Core closes capture once"), Run.Closes, 1); TestFalse(TEXT("terminal rollback capture closed"), Run.bCaptureOpen);
   TestTrue(TEXT("Core revokes capture before native inverse"), Run.bClosedBeforeRollback); TestEqual(TEXT("one actual rollback invalidation"), Run.Invalidations, 1);
   Before.Check(*this, *Original); Retained.Check(*this, *Original); CheckUsage(*this, *Original); Sources.Check(*this, A);
   TestEqual(TEXT("original dirty restored"), Package->IsDirty(), Dirty); TestEqual(TEXT("original target identity restored"), A.Sequence.Get(), Original);
   TestEqual(TEXT("original package identity restored"), Original->GetOutermost(), Package); TestEqual(TEXT("original model identity restored"), Original->GetDataModelInterface().GetObject(), Model);
   TestTrue(TEXT("rollback preserves installed target bytes"), Bytes(PackageFile(Original->GetPathName())) == Disk);
   TestTrue(TEXT("rollback preserves installed sidecar existence and bytes"), FileState(A.DiskRoot / TEXT("Installed/Compression.assetdoc")) == Sidecar);
   TestTrue(TEXT("rollback preserves all source/default packages and sidecars"), SourceFiles(A, Defaults) == Files); TestEqual(TEXT("late failure does not enter verifier"), Run.VerifierCalls, 0);
   TestFalse(TEXT("real default inverse not poisoned"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
  }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceCompressionDocumentCoreMixedTest,
 "UEAssetDoc.AnimationProvider.SequenceCompressionDocumentCore.MixedInputs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceCompressionDocumentCoreMixedTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceCompressionIntegrationTests;
 for (int32 Mode = 0; Mode < 2; ++Mode)
 {
  FAssets A; if (!TestTrue(TEXT("GUID-owned document fixture with constant compressed goldens"), A.Setup(true))) return false;
  const auto Defaults = DefaultReferences(); if (!CheckDefaults(*this, Defaults)) return false;
  const auto Before = FValues::Read(*A.Sequence), Expected = DocumentExpected(A, Defaults, Mode);
  const FRetained Retained(*A.Sequence); const FInputs Sources(A, Defaults); const auto Files = SourceFiles(A, Defaults);
  const auto DocumentInputs = CompressionDocument(A, Mode);
  const auto Phase = Mode == 0 ? EAssetDocumentRuntimeRequestPhase::Apply : EAssetDocumentRuntimeRequestPhase::Repair;
  for (const bool bNoop : {false, true})
  {
   A.Sequence->GetOutermost()->SetDirtyFlag(Mode != 0);
   FPreparedDocument Prepared; if (!PrepareDocument(*this, A, DocumentInputs, Expected, Mode, Phase, Prepared)) return false;
   FScopedHooks Hooks; const auto Run = Execute(*this, A, ECase::All, false, 0, nullptr, &Prepared);
   TestEqual(TEXT("bound document forward and ordinary Core save succeed"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
   TestEqual(TEXT("merged document consumes one original occurrence"), Run.BatchCalls, 1);
   TestEqual(TEXT("single target changed only on the first forward"), Run.ChangedCount, bNoop ? 0 : 1);
   TestEqual(TEXT("native fixed writer runs once and true noop writes zero fields"), Hooks.Hooks.FieldWrites, bNoop ? 0 : 5);
   TestEqual(TEXT("one actual Apply phase"), Run.ApplyCount, 1); TestEqual(TEXT("one actual Repair phase"), Run.RepairCount, 1);
   TestTrue(TEXT("actual merged tuple and compressed bone/curve goldens completed"), Run.bForwardObserved);
   TestEqual(TEXT("ordinary staged native verifier runs once"), Run.VerifierCalls, 1); TestEqual(TEXT("real capture closes once"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
   Expected.Check(*this, *A.Sequence); Retained.Check(*this, *A.Sequence); CheckUsage(*this, *A.Sequence); Sources.Check(*this, A);
   TestTrue(TEXT("readonly declared settings/defaults/source packages and sidecars remain exact"), SourceFiles(A, Defaults) == Files);
   TestFalse(TEXT("ordinary document target save finishes clean"), A.Sequence->GetOutermost()->IsDirty());
   if (Mode == 0)
   {
    TestEqual(TEXT("explicit native Bone default is the actual stock object"), static_cast<UObject*>(A.Sequence->BoneCompressionSettings.Get()), Defaults[0]);
    TestEqual(TEXT("package-only declared Curve ref resolves to the actual loaded fixture"), A.Sequence->CurveCompressionSettings.Get(), A.NewCurve.Get());
    TestTrue(TEXT("omitted Frame reference preserves the original value"), A.Sequence->VariableFrameStrippingSettings->GetPathName().Equals(Before.References[2], ESearchCase::CaseSensitive));
   }
   else
   {
    TestEqual(TEXT("object-path declared Bone ref resolves to the actual loaded fixture"), A.Sequence->BoneCompressionSettings.Get(), A.NewBone.Get());
    TestEqual(TEXT("explicit native Frame default is the actual stock object"), static_cast<UObject*>(A.Sequence->VariableFrameStrippingSettings.Get()), Defaults[2]);
    TestTrue(TEXT("omitted Curve reference preserves the original value"), A.Sequence->CurveCompressionSettings->GetPathName().Equals(Before.References[1], ESearchCase::CaseSensitive));
    TestEqual(TEXT("omitted Number preserves original scale"), A.Sequence->CompressionErrorThresholdScale, Before.Error);
    TestEqual(TEXT("omitted Boolean preserves original Never value"), A.Sequence->bDoNotOverrideCompression != 0, Before.bKeep);
   }
  }
 }
 // These additional cases prove preparation/Freeze only. They do not dispatch
 // Preview or claim another completed forward. Sparse coordinates are checked
 // independently because real Core Freeze requires contiguous input indices.
 for (int32 Case = 0; Case < 5; ++Case)
 {
  const int32 Mode = Case == 1 ? 4 : 3; const bool bPreview = Case == 2 || Case == 3;
  FAssets A; if (!TestTrue(TEXT("GUID-owned isolated document preparation fixture"), A.Setup(true))) return false;
  if (bPreview) A.Sequence->SetPreviewMesh(A.Mesh.Get(), false);
  const auto Before = FValues::Read(*A.Sequence); const auto PreviewBefore = static_cast<const UAnimationAsset&>(*A.Sequence).GetPreviewMesh();
  const auto NativeBefore = NativePersistentState(*A.Sequence); const FInputs Sources(A); const auto Files = SourceFiles(A, {});
  const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName())); const bool Dirty = A.Sequence->GetOutermost()->IsDirty();
  const auto Inputs = CompressionDocument(A, Mode); const auto Expected = DocumentExpected(A, {}, Mode);
  TestTrue(TEXT("scalar/ref-only inputs explicitly select no native defaults"), Inputs.Elements[2].Kind == EAssetDocumentRuntimeValueKind::Array && Inputs.Elements[2].Elements.IsEmpty());
  TArray<FAssetDocumentMutationIntent> Other;
  if (bPreview)
  {
   TestEqual(TEXT("other admitted Preview intent is a genuine current-mesh noop"), PreviewBefore, A.Mesh.Get());
   auto& Preview = Other.AddDefaulted_GetRef(); Preview.StepIndex = Case == 2 ? 0 : 1;
   Preview.Requirement = {TEXT("animation.asset.preview_mesh.set.v1"), 1, TEXT("animation_profile")};
   Preview.Phase = EAssetDocumentRuntimeRequestPhase::Apply; Preview.InputSchemaId = TEXT("animation.asset.preview_mesh.set.v1.input.v1");
   Preview.Intent = ObjectDto({TEXT("operation"), TEXT("preview_mesh_path")}, {String(TEXT("set")), String(A.Mesh->GetPathName())});
  }
  const int32 CompressionStep = Case == 2 ? 1 : Case == 4 ? 7 : 0;
  FPreparedDocument Prepared; FScopedHooks Hooks;
  if (!PrepareDocument(*this, A, Inputs, Expected, Mode, EAssetDocumentRuntimeRequestPhase::Apply, Prepared, Other, CompressionStep)) return false;
  if (bPreview)
  {
   const int32 PreviewIndex = Case == 2 ? 0 : 1; const auto& Actual = Prepared.ActualIntents[PreviewIndex]; const auto& Original = Other[0];
   TestTrue(TEXT("original Preview Requirement/schema/phase/StepIndex/entire DTO retained exactly"), Actual.StepIndex == Original.StepIndex
    && Actual.Requirement.PrimitiveId.Equals(Original.Requirement.PrimitiveId, ESearchCase::CaseSensitive) && Actual.Requirement.Version == Original.Requirement.Version
    && Actual.Requirement.RequiredByRouteId.Equals(Original.Requirement.RequiredByRouteId, ESearchCase::CaseSensitive)
    && Actual.InputSchemaId.Equals(Original.InputSchemaId, ESearchCase::CaseSensitive) && Actual.Phase == Original.Phase && ExactDocumentDto(Actual.Intent, Original.Intent));
   TestEqual(TEXT("actual intents retain contiguous original ordering"), Prepared.ActualIntents[0].StepIndex, 0);
   TestEqual(TEXT("actual second occurrence retains its original index"), Prepared.ActualIntents[1].StepIndex, 1);
   TestTrue(TEXT("one original Preview contribution retained alongside Compression"), Prepared.Invocation.Batch.Contributions.ContainsByPredicate([&](const auto& C) { return C.StepIndex == Original.StepIndex; }));
  }
  if (Case < 4)
  {
   if (!FreezeDocumentPreparation(*this, Prepared)) return false;
  }
  else
  {
   TestEqual(TEXT("preparation-only nonzero occurrence remains exactly seven"), Prepared.ActualIntents[0].StepIndex, 7);
   TestEqual(TEXT("preparation-only original inverse contribution remains exactly seven"), Prepared.Invocation.Batch.Contributions[0].StepIndex, 7);
  }
  TestEqual(TEXT("preparation/Freeze performs zero native writers"), Hooks.Hooks.FieldWrites, 0);
  Before.Check(*this, *A.Sequence); Sources.Check(*this, A); CheckUsage(*this, *A.Sequence);
  TestEqual(TEXT("preparation/Freeze preserves live Preview exactly"), static_cast<const UAnimationAsset&>(*A.Sequence).GetPreviewMesh(), PreviewBefore);
  TestTrue(TEXT("preparation/Freeze preserves complete native target state"), NativePersistentState(*A.Sequence) == NativeBefore);
  TestEqual(TEXT("preparation/Freeze preserves target dirty"), A.Sequence->GetOutermost()->IsDirty(), Dirty);
  TestTrue(TEXT("preparation/Freeze preserves target file"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
  TestTrue(TEXT("preparation/Freeze preserves readonly package and sidecar bytes"), SourceFiles(A, {}) == Files);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceCompressionDocumentCoreGuardsTest,
 "UEAssetDoc.AnimationProvider.SequenceCompressionDocumentCore.InputGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceCompressionDocumentCoreGuardsTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceCompressionIntegrationTests;
 // Rejected snapshots use fresh actual request owners; no prepared output from
 // another owner or a dev-only producer is accepted as document preparation.
 {
  FAssets A; if (!TestTrue(TEXT("GUID-owned document input guard fixture"), A.Setup(true))) return false;
  const auto Defaults = DefaultReferences(); if (!CheckDefaults(*this, Defaults)) return false;
  const auto Before = FValues::Read(*A.Sequence); const FRetained Retained(*A.Sequence); const FInputs Sources(A, Defaults);
  const auto Files = SourceFiles(A, Defaults); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
  const bool Dirty = A.Sequence->GetOutermost()->IsDirty(); FScopedHooks Hooks;
  for (int32 Case = 0; Case < 19; ++Case)
  {
   auto Request = RequestFor(A); const auto BoundRequest = Request; auto Inputs = CompressionDocument(A, 2);
   auto Binding = MakeShared<FAssetDocumentAnimationRequestBinding>();
   if (Case != 0 && !TestTrue(TEXT("guard uses the actual bound request owner"), Binding->BindRequest(BoundRequest).bValid)) return false;
   if (Case == 1) Request.RequestId = FGuid::NewGuid();
   if (Case == 2) ++Request.ExpectedRuntimeRevision;
   if (Case == 3) Binding->CloseRequest();
   if (Case == 4) Inputs.Elements[1].Elements[0].Elements[1] = {};
   if (Case == 5) Inputs.Elements[1].Elements[0].Elements[0] = String(TEXT("DefinitionRef"));
   if (Case == 6) Inputs.Elements[1].Elements[0].Elements[2] = String(TEXT("/Script/Engine.SkeletalMesh"));
   if (Case == 7) Inputs.Elements[1].Elements[0].Elements[1] = String(A.Root + TEXT("/NeverCreated.NeverCreated"));
   if (Case == 8) Inputs.Elements[1].Elements[0].Elements[2] = String(TEXT("AnimBoneCompressionSettings"));
   if (Case == 9) Inputs.Elements[2].Elements[0] = String(TEXT("CompressionErrorThresholdScale"));
   if (Case == 10) Inputs.Elements[2].Elements.Add(String(CompressionFieldNames[0]));
   if (Case == 11) { Inputs.Elements[0] = ObjectDto(); Inputs.Elements[1] = ObjectDto(); Inputs.Elements[2].Elements.Reset(); }
   if (Case == 12) { Inputs.ObjectKeys.Add(TEXT("new_alias")); Inputs.Elements.Add(ObjectDto()); }
   if (Case == 13) Inputs.Elements[0].Elements[0] = BooleanDto(true);
   if (Case == 14) Inputs.Elements[1].Elements[0] = AssetRefDto(*A.Mesh, false, false);
   if (Case == 16 || Case == 17)
   {
    FString& Selector = Case == 16 ? Inputs.Elements[1].Elements[1].Elements[1].StringValue : Inputs.Elements[1].Elements[0].Elements[2].StringValue;
    const FString Original = Case == 16 ? A.NewCurve->GetPathName() : A.NewBone->GetClass()->GetPathName();
    TestTrue(TEXT("embedded-NUL guard starts from the complete actual loaded Path or Class"), Selector.Equals(Original, ESearchCase::CaseSensitive));
    // Stock AppendChar ignores NUL; the public counted append retains it.
    const int32 PrefixLength = Selector.Len(); const TCHAR Nul = TEXT('\0'); Selector.AppendChars(&Nul, 1); Selector.Append(TEXT("EmbeddedNulSuffix"));
    TestTrue(TEXT("native FString retains embedded NUL and its nonempty suffix"), Selector.Len() > PrefixLength + 1 && Selector[PrefixLength] == TEXT('\0'));
   }
   if (Case == 18) { Inputs.Elements[1].Elements[0].ObjectKeys[2] = TEXT("class"); Inputs.Elements[1].Elements[0].Elements[2] = String(TEXT("/Script/Engine.SkeletalMesh")); }
   TArray<FAssetDocumentMutationIntent> Other;
   if (Case == 15) Other.Add(IntentFor(ECase::All, Desired(A, ECase::All)));
   // Seeded outputs must be erased on every refusal, including binding guards.
   TArray<FAssetDocumentMutationIntent> Actual{IntentFor(ECase::All, Before)}; FAssetDocumentPreparedMutationBatch Batch; Batch.Contributions.AddDefaulted();
   const auto R = Binding->PrepareSequenceCompressionDocument(Request, Inputs, 7, EAssetDocumentRuntimeRequestPhase::Apply, Other, Actual, Batch);
   TestFalse(TEXT("invalid binding/closed DTO/reference/default selection/duplicate occurrence rejected"), R.bValid);
   TestTrue(TEXT("refusal publishes neither intents nor capture nor footprint nor inverse"), Actual.IsEmpty() && Batch.Contributions.IsEmpty() && !Batch.OpenMutationCapture && !Batch.PackageFinalizer.IsValid());
   TestFalse(TEXT("refusal carries an actual stable diagnostic"), R.Diagnostics.IsEmpty());
   TestEqual(TEXT("preparation refuses before every native setter"), Hooks.Hooks.FieldWrites, 0);
   Before.Check(*this, *A.Sequence); Retained.Check(*this, *A.Sequence); CheckUsage(*this, *A.Sequence); Sources.Check(*this, A);
   TestEqual(TEXT("refusal retains target dirty"), A.Sequence->GetOutermost()->IsDirty(), Dirty);
   TestTrue(TEXT("refusal retains installed target bytes"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
   TestTrue(TEXT("refusal retains every readonly package and sidecar"), SourceFiles(A, Defaults) == Files); Binding->CloseRequest();
  }
 }
 // Real Editor-issued handles must identify the mutable Sequence target and
 // selected readonly stock default/declared reference after document resolution.
 for (const ECase Case : {ECase::MissingReference, ECase::WrongReferenceIdentity, ECase::WrongReferenceClass, ECase::ReadOnlyTarget, ECase::WrongTargetClass})
 {
  FAssets A; if (!TestTrue(TEXT("document actual-handle refusal fixture"), A.Setup(true))) return false;
  const auto Defaults = DefaultReferences(); if (!CheckDefaults(*this, Defaults)) return false;
  const auto Before = FValues::Read(*A.Sequence); const FRetained Retained(*A.Sequence); const FInputs Sources(A, Defaults); const auto Files = SourceFiles(A, Defaults);
  const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName())); const bool Dirty = A.Sequence->GetOutermost()->IsDirty();
  FPreparedDocument Prepared; if (!PrepareDocument(*this, A, CompressionDocument(A, 0), DocumentExpected(A, Defaults, 0), 0, EAssetDocumentRuntimeRequestPhase::Apply, Prepared)) return false;
  FScopedHooks Hooks; const auto Run = Execute(*this, A, Case, false, Case == ECase::WrongReferenceClass ? 1 : 0, nullptr, &Prepared);
  TestEqual(TEXT("actual target/default/reference handle refusal reaches Core"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestEqual(TEXT("one bound-provider validation attempt"), Run.BatchCalls, 1); TestEqual(TEXT("refused handles perform zero fields"), Hooks.Hooks.FieldWrites, 0); TestEqual(TEXT("refused handles change no target"), Run.ChangedCount, 0);
  TestEqual(TEXT("refused handles never reach later Repair"), Run.RepairCount, 0); TestEqual(TEXT("refused handles never reach persistence verifier"), Run.VerifierCalls, 0);
  TestEqual(TEXT("Core closes actual capture once"), Run.Closes, 1); TestFalse(TEXT("terminal refusal capture closed"), Run.bCaptureOpen);
  Before.Check(*this, *A.Sequence); Retained.Check(*this, *A.Sequence); CheckUsage(*this, *A.Sequence); Sources.Check(*this, A);
  TestEqual(TEXT("handle refusal preserves target dirty"), A.Sequence->GetOutermost()->IsDirty(), Dirty);
  TestTrue(TEXT("handle refusal retains target file"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
  TestTrue(TEXT("handle refusal retains all readonly packages/sidecars"), SourceFiles(A, Defaults) == Files);
  TestFalse(TEXT("bound refusal does not poison the native inverse"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 // Both changes are external fixture edits after preparation, restored before
 // leaving the case. Stock default assets are never modified for these guards.
 for (const bool bRename : {false, true})
 {
  FAssets A; if (!TestTrue(TEXT("document pre-capture readonly drift fixture"), A.Setup(true))) return false;
  const auto Defaults = DefaultReferences(); if (!CheckDefaults(*this, Defaults)) return false;
  const auto Before = FValues::Read(*A.Sequence); const FRetained Retained(*A.Sequence); const FInputs Sources(A, Defaults); const auto Files = SourceFiles(A, Defaults);
  const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName())); const bool Dirty = A.Sequence->GetOutermost()->IsDirty();
  auto Inputs = CompressionDocument(A, bRename ? 0 : 1); auto Expected = DocumentExpected(A, Defaults, bRename ? 0 : 1);
  if (!bRename)
  {
   Inputs.Elements[1] = ObjectDto({CompressionFieldNames[2]}, {AssetRefDto(*A.NewFrame, false)}); Inputs.Elements[2].Elements[0] = String(CompressionFieldNames[0]);
   Expected.References[0] = Defaults[0]->GetPathName(); Expected.References[2] = A.NewFrame->GetPathName();
  }
  FPreparedDocument Prepared; if (!PrepareDocument(*this, A, Inputs, Expected, bRename ? 0 : 1, EAssetDocumentRuntimeRequestPhase::Apply, Prepared)) return false;
  FScopedHooks Hooks;
  {
   FScopedDocumentSourceDrift Drift(*this, A, bRename); if (!TestTrue(TEXT("test-owned external drift actually occurred"), Drift.bApplied)) return false;
   const FInputs DriftedSources(A, Defaults); TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
   auto Lease = Prepared.Invocation.Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
   TestFalse(TEXT("original document batch refuses content or renamed identity drift before capture"), Lease && Lease->IsValid()); if (Lease) Lease->Close();
   TestFalse(TEXT("pre-capture drift produces an actual diagnostic"), Diagnostics.IsEmpty());
   TestEqual(TEXT("drift refusal writes zero native fields"), Hooks.Hooks.FieldWrites, 0);
   Before.Check(*this, *A.Sequence); Retained.Check(*this, *A.Sequence); CheckUsage(*this, *A.Sequence); DriftedSources.Check(*this, A);
  }
  Sources.Check(*this, A); TestEqual(TEXT("source-drift refusal retains target dirty"), A.Sequence->GetOutermost()->IsDirty(), Dirty);
  TestTrue(TEXT("source-drift refusal retains installed target"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
  TestTrue(TEXT("restored external fixture keeps readonly packages and sidecars exact"), SourceFiles(A, Defaults) == Files);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceCompressionDocumentCoreRollbackTest,
 "UEAssetDoc.AnimationProvider.SequenceCompressionDocumentCore.LateFailureRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceCompressionDocumentCoreRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceCompressionIntegrationTests;
 for (const bool Dirty : {false, true})
 {
  FAssets A; if (!TestTrue(TEXT("GUID-owned mixed five-field inverse fixture"), A.Setup(true))) return false;
  const auto Defaults = DefaultReferences(); if (!CheckDefaults(*this, Defaults)) return false;
  const auto Before = FValues::Read(*A.Sequence); const FRetained Retained(*A.Sequence); const FInputs Sources(A, Defaults); const auto Files = SourceFiles(A, Defaults);
  UAnimSequence* Original = A.Sequence.Get(); UPackage* Package = Original->GetOutermost(); UObject* Model = Original->GetDataModelInterface().GetObject();
  const auto Disk = Bytes(PackageFile(Original->GetPathName())); const auto Sidecar = FileState(A.DiskRoot / TEXT("Installed/Compression.assetdoc")); Package->SetDirtyFlag(Dirty);
  const auto Expected = DocumentExpected(A, Defaults, 2);
  TestTrue(TEXT("fixture genuinely changes all three refs and both scalar values"), Expected.References[0] != Before.References[0] && Expected.References[1] != Before.References[1]
   && Expected.References[2] != Before.References[2] && Expected.Error != Before.Error && Expected.bKeep != Before.bKeep);
  FPreparedDocument Prepared; if (!PrepareDocument(*this, A, CompressionDocument(A, 2), Expected, 2, EAssetDocumentRuntimeRequestPhase::Apply, Prepared)) return false;
  FScopedHooks Hooks; const auto Run = Execute(*this, A, ECase::All, true, 0, nullptr, &Prepared);
  TestEqual(TEXT("genuine post-completion Core failure"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestTrue(TEXT("exact later-failure provenance retained"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceCompressionFixture.LateFailure"); }));
  TestEqual(TEXT("completed forward invokes all five fixed writers"), Hooks.Hooks.FieldWrites, 5); TestEqual(TEXT("one merged occurrence invoked through the same binding"), Run.BatchCalls, 1);
  TestEqual(TEXT("one Apply then one later Repair"), Run.ApplyCount, 1); TestEqual(TEXT("later Repair actually runs"), Run.RepairCount, 1);
  TestEqual(TEXT("completed forward changes one target"), Run.ChangedCount, 1); TestTrue(TEXT("full tuple and compressed goldens observed before failure"), Run.bForwardObserved);
  TestTrue(TEXT("capture still open at the genuine later failure"), Run.bOpenAtRepair); TestEqual(TEXT("Core closes capture once"), Run.Closes, 1);
  TestFalse(TEXT("terminal rollback capture closed"), Run.bCaptureOpen); TestTrue(TEXT("capture closed before actual native inverse"), Run.bClosedBeforeRollback); TestEqual(TEXT("one rollback invalidation"), Run.Invalidations, 1);
  Before.Check(*this, *Original); Retained.Check(*this, *Original); CheckUsage(*this, *Original); Sources.Check(*this, A);
  TestEqual(TEXT("original dirty restored"), Package->IsDirty(), Dirty); TestEqual(TEXT("original target identity restored"), A.Sequence.Get(), Original);
  TestEqual(TEXT("original package identity restored"), Original->GetOutermost(), Package); TestEqual(TEXT("original model identity restored"), Original->GetDataModelInterface().GetObject(), Model);
  TestTrue(TEXT("late rollback retains installed target bytes"), Bytes(PackageFile(Original->GetPathName())) == Disk);
  TestTrue(TEXT("late rollback retains sidecar existence and bytes"), FileState(A.DiskRoot / TEXT("Installed/Compression.assetdoc")) == Sidecar);
  TestTrue(TEXT("late rollback retains every readonly settings/default/source package and sidecar"), SourceFiles(A, Defaults) == Files);
  TestEqual(TEXT("genuine later failure never enters persistence verifier"), Run.VerifierCalls, 0);
  TestFalse(TEXT("same-owner actual inverse not poisoned"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 return !HasAnyErrors();
}
#endif
