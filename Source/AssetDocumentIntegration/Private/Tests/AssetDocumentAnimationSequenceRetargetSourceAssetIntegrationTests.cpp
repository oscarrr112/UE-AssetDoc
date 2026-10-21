// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
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
#include "Engine/AssetUserData.h"
#include "Engine/SkeletalMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

// Existing-target fixture caller only: real Editor-issued Asset identities,
// Animation participant, Core transaction/persistence and ordinary native loads.
// No Sequence Profile registration or second public mutation ingress.
namespace AssetDocumentSequenceRetargetSourceAssetIntegrationTests
{
constexpr const TCHAR* Primitive = TEXT("animation.sequence.retarget_source_asset.set.v2");
enum class EBoundCase { Disabled, Forward, MissingTarget, ReadOnlyTarget, WrongTargetClass, ForeignTarget, ForeignTransaction,
 ForeignRequest, ForeignRevision, WrongPhase, ExtraValue, Closed, StagedRevision };
struct FScopedBoundRequest
{
 EBoundCase Case = EBoundCase::Disabled; FAutomationTestBase* Test = nullptr;
 bool bRefusalObserved = false, bStagedReadRejected = false, bCompletionObserved = false;
 TSharedPtr<FAssetDocumentAnimationRequestBinding> Binding;
 ~FScopedBoundRequest() { if (Binding) Binding->CloseRequest(); }
};
constexpr const TCHAR* Schema = TEXT("animation.sequence.retarget_source_asset.set.v2.input.v2");
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
FAssetDocumentRuntimeValueDto RuntimeValue(bool bSet)
{
 FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::Object;
 R.ObjectKeys = {TEXT("operation")}; R.Elements = {String(bSet ? TEXT("set") : TEXT("clear"))}; return R;
}
FAssetDocumentRuntimeValueDto PreparedValue(USkeletalMesh* Mesh)
{
 auto R = RuntimeValue(Mesh != nullptr); R.ObjectKeys.Add(TEXT("retarget_source_asset_path"));
 R.Elements.Add(Mesh ? String(Mesh->GetPathName()) : FAssetDocumentRuntimeValueDto()); return R;
}
bool ExactPose(const TArray<FTransform>& A, const TArray<FTransform>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I)
  if (A[I].GetTranslation() != B[I].GetTranslation() || A[I].GetRotation() != B[I].GetRotation()
   || A[I].GetScale3D() != B[I].GetScale3D()) return false;
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
 bool Ok = Test.TestTrue(TEXT("actual compressed native data valid"), Sequence.IsCompressedDataValid());
 Ok &= Test.TestFalse(TEXT("actual compressed data is current"), Sequence.IsCompressedDataOutOfDate());
 Ok &= Test.TestTrue(TEXT("current native platform has compressed data"), Sequence.HasCompressedDataForPlatform(static_cast<const ITargetPlatform*>(nullptr)));
 if (!Ok) return false;
 const FTransform U = Usage(Sequence);
 Ok &= Test.TestEqual(TEXT("golden compressed child additive translation"), U.GetTranslation().X, Expected, 1e-4);
 Ok &= Test.TestEqual(TEXT("compressed child Y"), U.GetTranslation().Y, 0., 1e-4);
 Ok &= Test.TestEqual(TEXT("compressed child Z"), U.GetTranslation().Z, 0., 1e-4);
 return Ok;
}
struct FValues
{
 FString Reference; TArray<FTransform> Pose;
 static FValues Read(const UAnimSequence& S)
 { return {S.GetRetargetSourceAsset().ToSoftObjectPath().ToString(), S.RetargetSourceAssetReferencePose}; }
 bool Matches(const UAnimSequence& S) const
 { return Reference == S.GetRetargetSourceAsset().ToSoftObjectPath().ToString() && ExactPose(Pose, S.RetargetSourceAssetReferencePose); }
 bool Check(FAutomationTestBase& Test, const UAnimSequence& S) const
 {
  bool Ok = Test.TestEqual(TEXT("exact owned soft reference"), S.GetRetargetSourceAsset().ToSoftObjectPath().ToString(), Reference);
  Ok &= Test.TestTrue(TEXT("complete stored pose matches exactly"), ExactPose(Pose, S.RetargetSourceAssetReferencePose)); return Ok;
 }
};
enum class ECase
{
 Set, Clear, NoopSet, NoopClear, MissingReference, MutableReference, WrongReferenceClass, WrongReferenceIdentity,
 StaleReference, ForeignReferenceProvider, ForeignReferenceTransaction, WrongTargetClass, StaleTarget,
 ForeignTargetTransaction, ForeignRevision, WrongPhase, RuntimePath, ExtraReferenceOnClear
};
struct FAssets
{
 FString Root = TEXT("/Game/AssetDocumentSequenceRetargetSourceAsset/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 FString DiskRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AssetDocumentSequenceRetargetSourceAsset") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
 TStrongObjectPtr<USkeleton> Skeleton; TStrongObjectPtr<USkeletalMesh> Old, Desired; TStrongObjectPtr<UAnimSequence> Sequence;
 bool bKeep = false;
 bool Setup(bool bOldReference = true)
 {
  if (!IFileManager::Get().MakeDirectory(*(DiskRoot / TEXT("Installed")), true)) return false;
  // Test-owned authoring before the first save; product never constructs assets.
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  {
   FReferenceSkeletonModifier Bones(Skeleton.Get());
   Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
   Bones.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(10., 0., 0.)));
  }
  auto Mesh = [&](const TCHAR* Name, double X)
  {
   auto* M = NewObject<USkeletalMesh>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone);
   M->SetSkeleton(Skeleton.Get());
   FReferenceSkeletonModifier Bones(M->GetRefSkeleton(), Skeleton.Get());
   Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
   Bones.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(X, 0., 0.))); return M;
  };
  Old.Reset(Mesh(TEXT("Old"), 10.)); Desired.Reset(Mesh(TEXT("Desired"), 20.));
  // ReferenceSkeletonModifier populates reference bones only. Stock public
  // merge initializes BoneTree before the mode setter indexes its child row.
  if (!Skeleton->MergeAllBonesToBoneTree(Old.Get(), false)) return false;
  Skeleton->SetBoneTranslationRetargetingMode(1, EBoneTranslationRetargetingMode::AnimationRelative);
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  Sequence->SetSkeleton(Skeleton.Get()); Sequence->RateScale = 1.25f; Sequence->bLoop = true;
  Sequence->bEnableRootMotion = false; Sequence->RootMotionRootLock = ERootMotionRootLock::RefPose;
  Sequence->bForceRootLock = false; Sequence->bUseNormalizedRootMotionScale = false;
  Sequence->AdditiveAnimType = AAT_LocalSpaceBase; Sequence->RefPoseType = ABPT_RefPose; Sequence->RefFrameIndex = 0;
  Sequence->RetargetSource = NAME_None;
  if (bOldReference) Sequence->SetRetargetSourceAsset(Old.Get()); else Sequence->ClearRetargetSourceAsset();
  Sequence->UpdateRetargetSourceAssetData();
  auto& C = Sequence->GetController(); C.OpenBracket(FText::FromString(TEXT("Retarget source additive fixture")), false);
  C.InitializeModel(); C.SetFrameRate(FFrameRate(24, 1), false); C.SetNumberOfFrames(FFrameNumber(12), false);
  TArray<FVector3f> P, S; TArray<FQuat4f> Q; S.Init(FVector3f::OneVector, 13); Q.Init(FQuat4f::Identity, 13);
  bool bTracks = true;
  for (const FName Name : {FName(TEXT("root")), FName(TEXT("child"))})
  {
   P.Init(Name == TEXT("root") ? FVector3f::ZeroVector : FVector3f(30.f, 0.f, 0.f), 13);
   bTracks &= C.AddBoneCurve(Name, false) && C.SetBoneTrackKeys(Name, P, Q, S, false);
  }
  const FAnimationCurveIdentifier Curve(FName(TEXT("RetainedRetargetCurve")), ERawCurveTrackTypes::RCT_Float);
  FRichCurveKey A(0.f, 1.f), B(0.5f, -3.f); A.InterpMode = RCIM_Cubic; A.TangentMode = RCTM_Break; A.TangentWeightMode = RCTWM_WeightedBoth;
  A.ArriveTangentWeight = 0.2f; A.LeaveTangentWeight = 0.4f;
  const bool bCurve = C.AddCurve(Curve, AACF_Editable, false) && C.SetCurveKeys(Curve, {A, B}, false);
  C.NotifyPopulated(); C.CloseBracket(false); Sequence->WaitOnExistingCompression();
  if (!bTracks || !bCurve) return false;
  Sequence->AnimNotifyTracks.SetNum(1); Sequence->AnimNotifyTracks[0].TrackName = TEXT("RetainedTrack");
  auto& Notify = Sequence->Notifies.AddDefaulted_GetRef(); Notify.NotifyName = TEXT("RetainedPoint"); Notify.Guid = FGuid(1, 2, 3, 4); Notify.TrackIndex = 0;
  auto& Marker = Sequence->AuthoredSyncMarkers.AddDefaulted_GetRef(); Marker.MarkerName = TEXT("RetainedMarker"); Marker.Time = 0.25f; Marker.TrackIndex = 0; Marker.Guid = FGuid(5, 6, 7, 8);
  // Finish the actual public compression lifecycle before freezing the
  // existing-target preimage; subsequent true noops never call this chain.
  Sequence->ClearAllCachedCookedPlatformData(); Sequence->CacheDerivedDataForCurrentPlatform(); Sequence->WaitOnExistingCompression();
  for (UObject* O : {static_cast<UObject*>(Skeleton.Get()), static_cast<UObject*>(Old.Get()), static_cast<UObject*>(Desired.Get()), static_cast<UObject*>(Sequence.Get())})
  {
   // Creation broadcast attaches third-party AnimationModifier UserData to
   // Sequence; this is instead an already-existing, natively saved fixture.
   if (O != Sequence.Get()) FAssetRegistryModule::AssetCreated(O);
   if (!Save(O)) return false;
  }
  if (const auto* Data = Sequence->GetAssetUserDataArray())
   if (Data->ContainsByPredicate([](const auto* D) { return D != nullptr; })) return false;
  return true;
 }
 ~FAssets()
 {
  if (Sequence.IsValid()) Sequence->WaitOnExistingCompression();
  if (bKeep) return;
  for (UObject* O : {static_cast<UObject*>(Sequence.Get()), static_cast<UObject*>(Old.Get()), static_cast<UObject*>(Desired.Get()), static_cast<UObject*>(Skeleton.Get())})
   if (IsValid(O))
   {
    // Disk fixtures remain until Editor exit and external node-owned cleanup.
    UPackage* P = O->GetOutermost(); O->ClearFlags(RF_Public | RF_Standalone);
    O->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional); O->MarkAsGarbage(); P->SetDirtyFlag(false); P->MarkAsGarbage();
   }
  IFileManager::Get().DeleteDirectory(*DiskRoot, false, true);
 }
};
USkeletalMesh* DesiredMesh(FAssets& Assets, ECase Case)
{
 if (Case == ECase::Clear || Case == ECase::NoopClear || Case == ECase::ExtraReferenceOnClear) return nullptr;
 return Case == ECase::NoopSet ? Assets.Old.Get() : Assets.Desired.Get();
}
FValues DesiredValues(FAssets& Assets, ECase Case)
{
 FValues R; if (const auto* M = DesiredMesh(Assets, Case))
 { R.Reference = M->GetPathName(); FAnimationRuntime::MakeSkeletonRefPoseFromMesh(M, Assets.Skeleton.Get(), R.Pose); } return R;
}
double DesiredUsage(ECase Case) { return Case == ECase::Set ? 10. : 20.; }
bool Retained(FAutomationTestBase& Test, const UAnimSequence& Sequence, const FString& SkeletonPath, const FString& ModelGuid, const FString& SkeletonGuid)
{
 bool Ok = Test.TestNotNull(TEXT("retained Skeleton loaded"), Sequence.GetSkeleton()); if (!Ok) return false;
 Ok &= Test.TestEqual(TEXT("retained Skeleton path"), Sequence.GetSkeleton()->GetPathName(), SkeletonPath);
 Ok &= Test.TestEqual(TEXT("retained Skeleton GUID"), Sequence.GetSkeletonGuid().ToString(), SkeletonGuid);
 Ok &= Test.TestEqual(TEXT("native Skeleton GUID agrees"), Sequence.GetSkeletonGuid(), Sequence.GetSkeleton()->GetGuid());
 const auto* M = Sequence.GetDataModel(); if (!Test.TestNotNull(TEXT("retained authored model"), M)) return false;
 Ok &= Test.TestEqual(TEXT("retained complete model content"), M->GenerateGuid().ToString(), ModelGuid);
 TArray<FName> Names; M->GetBoneTrackNames(Names);
 Ok &= Test.TestTrue(TEXT("retained ordered bone names"), Names == TArray<FName>{TEXT("root"), TEXT("child")});
 Ok &= Test.TestEqual(TEXT("retained rate"), M->GetFrameRate(), FFrameRate(24, 1)); Ok &= Test.TestEqual(TEXT("retained frame count"), M->GetNumberOfFrames(), 12);
 Ok &= Test.TestEqual(TEXT("retained curve count"), M->GetFloatCurves().Num(), 1);
 if (M->GetFloatCurves().Num() == 1)
 {
  const auto& Curve = M->GetFloatCurves()[0]; const auto& Keys = Curve.FloatCurve.GetConstRefOfKeys();
  Ok &= Test.TestEqual(TEXT("retained curve name"), Curve.GetName(), FName(TEXT("RetainedRetargetCurve")));
  Ok &= Test.TestEqual(TEXT("retained curve flags"), Curve.GetCurveTypeFlags(), int32(AACF_Editable));
  Ok &= Test.TestEqual(TEXT("retained curve key count"), Keys.Num(), 2);
  if (Keys.Num() == 2)
  {
   Ok &= Test.TestEqual(TEXT("retained first curve value"), Keys[0].Value, 1.f); Ok &= Test.TestEqual(TEXT("retained second curve value"), Keys[1].Value, -3.f);
   Ok &= Test.TestEqual(TEXT("retained arrive tangent weight"), Keys[0].ArriveTangentWeight, 0.2f); Ok &= Test.TestEqual(TEXT("retained leave tangent weight"), Keys[0].LeaveTangentWeight, 0.4f);
  }
 }
 Ok &= Test.TestEqual(TEXT("retained RetargetSource name"), Sequence.RetargetSource, NAME_None);
 Ok &= Test.TestEqual(TEXT("retained child translation retarget mode"), Sequence.GetSkeleton()->GetBoneTranslationRetargetingMode(1), EBoneTranslationRetargetingMode::AnimationRelative);
 Ok &= Test.TestEqual(TEXT("retained additive type"), Sequence.AdditiveAnimType.GetValue(), AAT_LocalSpaceBase);
 Ok &= Test.TestEqual(TEXT("retained additive base"), Sequence.RefPoseType.GetValue(), ABPT_RefPose); Ok &= Test.TestEqual(TEXT("retained frame index"), Sequence.RefFrameIndex, 0);
 Ok &= Test.TestNull(TEXT("retained empty additive reference"), Sequence.RefPoseSeq.Get());
 Ok &= Test.TestEqual(TEXT("retained playback"), Sequence.RateScale, 1.25f); Ok &= Test.TestTrue(TEXT("retained loop"), Sequence.bLoop);
 Ok &= Test.TestFalse(TEXT("retained root motion off"), Sequence.bEnableRootMotion); Ok &= Test.TestFalse(TEXT("retained force root off"), Sequence.bForceRootLock);
 Ok &= Test.TestFalse(TEXT("retained normalized root scale off"), Sequence.bUseNormalizedRootMotionScale);
 Ok &= Test.TestEqual(TEXT("retained root lock"), Sequence.RootMotionRootLock.GetValue(), ERootMotionRootLock::RefPose);
 Ok &= Test.TestNull(TEXT("retained empty PreviewMesh"), static_cast<const UAnimationAsset&>(Sequence).GetPreviewMesh());
 Ok &= Test.TestEqual(TEXT("retained notify count"), Sequence.Notifies.Num(), 1);
 if (Sequence.Notifies.Num() == 1)
 { Ok &= Test.TestEqual(TEXT("retained notify name"), Sequence.Notifies[0].NotifyName, FName(TEXT("RetainedPoint"))); Ok &= Test.TestEqual(TEXT("retained notify GUID"), Sequence.Notifies[0].Guid, FGuid(1, 2, 3, 4)); }
 Ok &= Test.TestEqual(TEXT("retained notify track count"), Sequence.AnimNotifyTracks.Num(), 1);
 if (Sequence.AnimNotifyTracks.Num() == 1) Ok &= Test.TestEqual(TEXT("retained track name"), Sequence.AnimNotifyTracks[0].TrackName, FName(TEXT("RetainedTrack")));
 Ok &= Test.TestEqual(TEXT("retained marker count"), Sequence.AuthoredSyncMarkers.Num(), 1);
 if (Sequence.AuthoredSyncMarkers.Num() == 1)
 { const auto& Marker = Sequence.AuthoredSyncMarkers[0]; Ok &= Test.TestEqual(TEXT("retained marker name"), Marker.MarkerName, FName(TEXT("RetainedMarker"))); Ok &= Test.TestEqual(TEXT("retained marker time"), Marker.Time, 0.25f); Ok &= Test.TestEqual(TEXT("retained marker GUID"), Marker.Guid, FGuid(5, 6, 7, 8)); }
 return Ok;
}
struct FSourceSnapshot
{
 TArray<uint8> SkeletonBytes, OldBytes, DesiredBytes;
 TArray<FTransform> SkeletonPose, OldPose, DesiredPose;
 explicit FSourceSnapshot(const FAssets& A)
  : SkeletonBytes(Bytes(PackageFile(A.Skeleton->GetPathName()))), OldBytes(Bytes(PackageFile(A.Old->GetPathName()))), DesiredBytes(Bytes(PackageFile(A.Desired->GetPathName()))),
    SkeletonPose(A.Skeleton->GetReferenceSkeleton().GetRefBonePose()), OldPose(A.Old->GetRefSkeleton().GetRefBonePose()), DesiredPose(A.Desired->GetRefSkeleton().GetRefBonePose()) {}
 void Check(FAutomationTestBase& Test, const FAssets& A) const
 {
  Test.TestTrue(TEXT("referenced Skeleton bytes unchanged"), Bytes(PackageFile(A.Skeleton->GetPathName())) == SkeletonBytes);
  Test.TestTrue(TEXT("old Mesh bytes unchanged"), Bytes(PackageFile(A.Old->GetPathName())) == OldBytes);
  Test.TestTrue(TEXT("desired Mesh bytes unchanged"), Bytes(PackageFile(A.Desired->GetPathName())) == DesiredBytes);
  Test.TestTrue(TEXT("referenced Skeleton pose unchanged"), ExactPose(A.Skeleton->GetReferenceSkeleton().GetRefBonePose(), SkeletonPose));
  Test.TestTrue(TEXT("old Mesh pose unchanged"), ExactPose(A.Old->GetRefSkeleton().GetRefBonePose(), OldPose));
  Test.TestTrue(TEXT("desired Mesh pose unchanged"), ExactPose(A.Desired->GetRefSkeleton().GetRefBonePose(), DesiredPose));
  Test.TestEqual(TEXT("old Mesh Skeleton unchanged"), A.Old->GetSkeleton(), A.Skeleton.Get()); Test.TestEqual(TEXT("desired Mesh Skeleton unchanged"), A.Desired->GetSkeleton(), A.Skeleton.Get());
  Test.TestFalse(TEXT("referenced Skeleton remains clean"), A.Skeleton->GetOutermost()->IsDirty());
  Test.TestFalse(TEXT("old Mesh remains clean"), A.Old->GetOutermost()->IsDirty()); Test.TestFalse(TEXT("desired Mesh remains clean"), A.Desired->GetOutermost()->IsDirty());
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
  FAssetDocumentVerifiedLeaseResult R; auto V = ValidateAssetDocumentHandleMetadata(H, C);
  if (!V.bValid) { R.Diagnostics = MoveTemp(V.Diagnostics); return R; }
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
  FAssetDocumentPrimitiveOutcome Failed; Failed.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
  Handles.Token = Transaction.GetMutationToken();
  if (Bound.Case == EBoundCase::ForeignTarget)
   Foreign.Reset(NewObject<UAnimSequence>(CreatePackage(*(Assets.Root + TEXT("/ForeignTarget"))), TEXT("ForeignTarget"), RF_Public | RF_Standalone | RF_Transactional));
  UObject& Target = Case == ECase::WrongTargetClass || Bound.Case == EBoundCase::WrongTargetClass ? static_cast<UObject&>(*Assets.Desired)
   : Foreign.IsValid() ? static_cast<UObject&>(*Foreign) : static_cast<UObject&>(*Assets.Sequence);
  auto H = Resolve(Target, Bound.Case == EBoundCase::ReadOnlyTarget ? EAssetDocumentHandleAccess::ReadOnly : EAssetDocumentHandleAccess::Mutable, Handles, Transaction); if (!H.IsSet()) return Failed;
  FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = Primitive; I.Version = 2; I.RequestId = Metadata.RequestId;
  I.TransactionId = Transaction.GetTransactionId(); I.RuntimeRevision = Metadata.PinnedRuntimeRevision; I.Phase = Metadata.Phase;
  if (Case == ECase::StaleTarget) ++H->Generation;
  if (Case == ECase::ForeignTargetTransaction || Bound.Case == EBoundCase::ForeignTransaction) H->TransactionId = FGuid::NewGuid();
  if (Bound.Case != EBoundCase::MissingTarget) I.HandleArguments = {{TEXT("sequence"), H.GetValue()}};
  USkeletalMesh* Mesh = DesiredMesh(Assets, Case);
  I.ValueArguments = RuntimeValue(Mesh != nullptr);
  if (Case == ECase::ForeignRevision) ++I.RuntimeRevision;
  if (Case == ECase::WrongPhase) I.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == ECase::RuntimePath) { I.ValueArguments.ObjectKeys.Add(TEXT("retarget_source_asset_path")); I.ValueArguments.Elements.Add(String(Assets.Desired->GetPathName())); }
  if ((Mesh && Case != ECase::MissingReference) || Case == ECase::ExtraReferenceOnClear)
  {
   UObject* Ref = Case == ECase::WrongReferenceClass ? static_cast<UObject*>(Assets.Skeleton.Get())
    : Case == ECase::WrongReferenceIdentity ? static_cast<UObject*>(Assets.Old.Get()) : static_cast<UObject*>(Mesh ? Mesh : Assets.Desired.Get());
   auto RH = Resolve(*Ref, Case == ECase::MutableReference ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, Handles, Transaction);
   if (!RH.IsSet()) return Failed;
   if (Case == ECase::StaleReference) ++RH->Generation;
   if (Case == ECase::ForeignReferenceProvider) RH->ProviderId = TEXT("animation");
   if (Case == ECase::ForeignReferenceTransaction) RH->TransactionId = FGuid::NewGuid();
   I.HandleArguments.Add({TEXT("retarget_source_asset"), RH.GetValue()});
  }
  if (Bound.Binding && Bound.Case != EBoundCase::Forward)
  {
   auto& Test = *Bound.Test; const FValues Before = FValues::Read(*Assets.Sequence); const FSourceSnapshot Sources(Assets);
   UAnimSequence* Original = Assets.Sequence.Get(); UPackage* Package = Original->GetOutermost(); UObject* Model = Original->GetDataModelInterface().GetObject();
   USkeleton* Skeleton = Assets.Skeleton.Get(); USkeletalMesh* Old = Assets.Old.Get(); USkeletalMesh* Desired = Assets.Desired.Get();
   const FString Guid = Original->GetDataModel()->GenerateGuid().ToString(), SkeletonGuid = Skeleton->GetGuid().ToString();
   const FTransform BeforeUsage = Usage(*Original); const bool Dirty = Package->IsDirty(); const auto Disk = Bytes(PackageFile(Original->GetPathName()));
   const auto ForeignBefore = Foreign.IsValid() ? FValues::Read(*Foreign) : FValues{};
   const bool ForeignDirty = Foreign.IsValid() && Foreign->GetOutermost()->IsDirty();
   if (Bound.Case == EBoundCase::ForeignRequest) I.RequestId = FGuid::NewGuid();
   if (Bound.Case == EBoundCase::ForeignRevision) ++I.RuntimeRevision;
   if (Bound.Case == EBoundCase::WrongPhase) I.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
   if (Bound.Case == EBoundCase::ExtraValue) { I.ValueArguments.ObjectKeys.Add(TEXT("unexpected")); I.ValueArguments.Elements.Add(String(TEXT("closed-shape refusal"))); }
   if (Bound.Case == EBoundCase::Closed) Bound.Binding->CloseRequest();
   if (Bound.Case == EBoundCase::StagedRevision)
   {
    // The actual staged read API is Montage-only. No Sequence staged state
    // is injected; its refused lease does not authorize a fresh revision.
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
   // Observe before Composition's refusal/Core inverse, never after a
   // fixture restoration. This catches an erroneous bound-owner write.
   Before.Check(Test, *Original); CheckUsage(Test, *Original, BeforeUsage.GetTranslation().X);
   Test.TestTrue(TEXT("immediate bound refusal preserves entire native sample"), Usage(*Original).Equals(BeforeUsage, 1e-5));
   Retained(Test, *Original, Skeleton->GetPathName(), Guid, SkeletonGuid); Sources.Check(Test, Assets);
   Test.TestEqual(TEXT("immediate bound refusal preserves target identity"), Assets.Sequence.Get(), Original);
   Test.TestEqual(TEXT("immediate bound refusal preserves package identity"), Original->GetOutermost(), Package);
   Test.TestEqual(TEXT("immediate bound refusal preserves model identity"), Original->GetDataModelInterface().GetObject(), Model);
   Test.TestEqual(TEXT("immediate bound refusal preserves Skeleton identity"), Original->GetSkeleton(), Skeleton);
   Test.TestEqual(TEXT("immediate bound refusal preserves old Mesh identity"), Assets.Old.Get(), Old);
   Test.TestEqual(TEXT("immediate bound refusal preserves supplied Mesh identity"), Assets.Desired.Get(), Desired);
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
    CheckUsage(*Bound.Test, S, ExpectedUsage);
   }
   Assets.Sequence->WaitOnExistingCompression();
   bForwardObserved = Expected.Matches(*Assets.Sequence) && Assets.Sequence->IsCompressedDataValid()
    && FMath::IsNearlyEqual(Usage(*Assets.Sequence).GetTranslation().X, ExpectedUsage, 1e-4);
   CollectGarbage(RF_NoFlags);
   if (bFailRepair)
   {
    R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
    auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/SequenceRetargetSourceAssetFixture/Repair");
    D.Code = TEXT("SequenceRetargetSourceAssetFixture.LateFailure"); D.Message = TEXT("Test-only failure after real retarget Update and compressed completion."); D.Severity = EAssetDocumentDiagnosticSeverity::Error;
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
 FAutomationTestBase& Test; FAssets& Assets; FValues Expected; double ExpectedUsage; FString ModelGuid, SkeletonGuid;
public:
 int32 Calls = 0;
 FVerifier(FAutomationTestBase& T, FAssets& A, FValues V, double U) : Test(T), Assets(A), Expected(MoveTemp(V)), ExpectedUsage(U),
  ModelGuid(A.Sequence->GetDataModel()->GenerateGuid().ToString()), SkeletonGuid(A.Skeleton->GetGuid().ToString()) {}
 FAssetDocumentFreshVerificationResult VerifyFresh(const FAssetDocumentFreshVerificationInput& Input) override
 {
  ++Calls; FAssetDocumentFreshVerificationResult R;
  const FString InstancedName = TEXT("/Temp/SequenceRetargetSourceAssetVerify_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  FLinkerInstancingContext Instancing; Instancing.AddPackageMapping(FName(*Assets.Sequence->GetOutermost()->GetName()), FName(*InstancedName));
  TStrongObjectPtr<UPackage> Package(LoadPackage(nullptr, *Input.StagedPackagePath, LOAD_ForDiff | LOAD_NoWarn, nullptr, &Instancing));
  auto* Sequence = Package.IsValid() ? FindObject<UAnimSequence>(Package.Get(), TEXT("Sequence")) : nullptr;
  if (!Test.TestNotNull(TEXT("staged Sequence ordinarily loads"), Sequence)) return R;
  Sequence->WaitOnExistingCompression();
  R.bFreshLoadProven = Sequence != Assets.Sequence.Get() && Sequence->GetOutermost() != Assets.Sequence->GetOutermost()
   && FPaths::IsSamePath(Package->GetLoadedPath().GetLocalFullPath(), Input.StagedPackagePath);
  R.bCanonicalMatch = R.bFreshLoadProven && Expected.Check(Test, *Sequence)
   && Retained(Test, *Sequence, Assets.Skeleton->GetPathName(), ModelGuid, SkeletonGuid) && CheckUsage(Test, *Sequence, ExpectedUsage)
   && Bytes(Input.StagedSidecarPath) == TArray<uint8>{0x7b, 0x7d};
  if (R.bCanonicalMatch) { R.ActualSemanticHash = Input.ExpectedSemanticHash; R.ActualCodecVersion = Input.ExpectedCodecVersion; }
  Sequence->ClearFlags(RF_Public | RF_Standalone); Package->SetDirtyFlag(false); Package->ClearFlags(RF_Standalone); return R;
 }
};
FAssetDocumentRegionEvidence Evidence()
{ FAssetDocumentRegionEvidence E; E.Kind = EAssetDocumentEvidenceKind::Present; E.SemanticHash = TEXT("sequence-retarget-source-asset-fixture:v1"); E.CodecVersion = 1; return E; }
struct FRun
{
 FAssetDocumentRuntimeResultDto Result; bool bCaptureOpen = false, bClosedBeforeRollback = false, bForwardObserved = false, bOpenAtRepair = false;
 bool bBoundRefusalObserved = false, bStagedReadRejected = false, bBoundCompletionObserved = false;
 int32 Closes = 0, Invalidations = 0, ApplyCount = 0, RepairCount = 0, BatchCalls = 0, ChangedCount = 0, VerifierCalls = 0;
};
FAssetDocumentRuntimeRequestDto RequestFor(FAssets& A)
{
 FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 84;
 R.Operation = EAssetDocumentRuntimeOperation::Apply; R.TargetAssetPath = A.Sequence->GetPathName(); R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence"); return R;
}
FAssetDocumentMutationIntent IntentFor(USkeletalMesh* Mesh)
{
 FAssetDocumentMutationIntent I; I.StepIndex = 0; I.Requirement = {Primitive, 2, TEXT("animation_profile")};
 I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.InputSchemaId = Schema; I.Intent = PreparedValue(Mesh); return I;
}
FRun Execute(FAutomationTestBase& Test, FAssets& Assets, ECase Case, bool bFailRepair, EBoundCase BoundCase = EBoundCase::Disabled)
{
 FRun Run; const auto Request = RequestFor(Assets); const auto Expected = DesiredValues(Assets, Case); const double U = DesiredUsage(Case);
 TArray<FAssetDocumentMutationIntent> Intents{IntentFor(DesiredMesh(Assets, Case))};
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
 const auto P = PrepareAssetDocumentEditorMutationBatch(Request, {}, Editor.Batch);
 if (!Test.TestTrue(TEXT("real Editor target/package preimage prepares"), P.bValid)) return Run;
 auto& Animation = Batches.AddDefaulted_GetRef(); Animation.ProviderId = TEXT("animation"); Animation.ActualProvider = Provider;
 const auto A = Bound.Binding ? Bound.Binding->Prepare(Request, Intents, Animation.Batch)
  : PrepareAssetDocumentAnimationMutationBatch(Request, Intents, Animation.Batch);
 for (const auto& D : A.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("real retarget-source parent prepares"), A.bValid)) return Run;
 if (!Test.TestEqual(TEXT("one genuine Animation inverse contribution"), Animation.Batch.Contributions.Num(), 1)) return Run;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 const auto Composition = FAssetDocumentMutationComposition::Freeze(Request, MoveTemp(Intents), MoveTemp(Batches), {}, {}, {}, Diagnostics);
 for (const auto& D : Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("freeze real production composition"), Composition.IsValid())) return Run;
 FAssetDocumentPreflightReceipt Preflight; Preflight.RequestId = Request.RequestId; Preflight.RuntimeRevision = Request.ExpectedRuntimeRevision;
 Preflight.CompleteFootprint.Append(Composition->GetFootprint()); Preflight.ReceiptHash = TEXT("sha256:sequence-retarget-source-asset-fixture"); Preflight.bSucceeded = true;
 FAssetDocumentPersistencePlan Plan; Plan.RequestId = Request.RequestId; Plan.TargetPackagePath = Assets.Sequence->GetOutermost()->GetName();
 Plan.TargetSidecarPath = Assets.DiskRoot / TEXT("Installed/RetargetSourceAsset.assetdoc"); Plan.StagingRoot = Assets.DiskRoot / TEXT("Staging");
 Plan.JournalId = FGuid::NewGuid().ToString(EGuidFormats::Digits); Plan.Sidecar.Utf8Bytes = {0x7b, 0x7d}; Plan.Sidecar.SemanticHash = Evidence().SemanticHash; Plan.Sidecar.CodecVersion = 1;
 Plan.Footprint.Append(Composition->GetFootprint());
 FResolver Handles; FRegistry Registry; FCancellation Cancellation; FObserver Observer;
 FHost Host(Assets, Expected, U, bFailRepair, Run.bCaptureOpen, Bound); FVerifier Verifier(Test, Assets, Expected, U);
 FAssetDocumentCoreRuntimeComposition Core({Host, Registry, Cancellation, Observer,
  [&](const FAssetDocumentRequestContextMetadata& M, const FGuid&, IAssetDocumentAtomicTransaction* T, TArray<FAssetDocumentRuntimeDiagnostic>& Out) -> TUniquePtr<IAssetDocumentNativeRequestContext>
  {
   if (!T) return MakeUnique<FReadContext>(M);
   auto* Adapter = Composition->BindTransaction(*T, M, Out); if (!Adapter) return {};
   return MakeUnique<FContext>(M, Assets, Case, Handles, *Adapter, Composition.ToSharedRef(), Provider, Run.BatchCalls, Bound);
  },
  [&](const FAssetDocumentHandleInvalidation& E)
  {
   ++Run.Invalidations; if (E.Reason == EAssetDocumentHandleInvalidationReason::Rollback) Run.bClosedBeforeRollback = !Run.bCaptureOpen;
   return Handles.Invalidate(E);
  }});
 auto Opener = Composition->MakeCaptureOpener();
 FAssetDocumentCoreMutationInputs Mutation{Preflight, Plan, Verifier,
  [&](const FAssetDocumentCorePersistenceTargetRequest& I, TArray<FAssetDocumentRuntimeDiagnostic>&) -> TOptional<FAssetDocumentCorePersistenceTargetBinding>
  {
   if (!Composition->ValidateComplete().bValid || I.CanonicalObjectPath != Request.TargetAssetPath || I.ExactAssetClassPath != Request.RequestedAssetClassPath || I.TargetPackagePath != Plan.TargetPackagePath) return {};
   return FAssetDocumentCorePersistenceTargetBinding{I.RequestId, I.TransactionId, I.RuntimeRevision, Assets.Sequence.Get(), Assets.Sequence->GetOutermost()};
  },
  {Evidence(), Evidence()}, Assets.DiskRoot / TEXT("Journal"), Assets.DiskRoot / TEXT("Journal/Backup"), Assets.DiskRoot / TEXT("Sync"), TEXT("SequenceRetargetSourceAssetFixture"), Request.ExpectedRuntimeRevision,
  Composition->MakeInitialActions(),
  [&, Opener = MoveTemp(Opener)](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Out) mutable -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
  {
   auto Lease = Opener(Id, Out); if (!Lease || !Lease->IsValid()) return {}; Run.bCaptureOpen = true;
   return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([&, Lease = MoveTemp(Lease)]() mutable { Lease->Close(); Lease.Reset(); Run.bCaptureOpen = false; ++Run.Closes; });
  }};
 Run.Result = Core.ExecuteMutation(Request, MoveTemp(Mutation));
 for (const auto& D : Run.Result.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 Run.bForwardObserved = Host.bForwardObserved; Run.bOpenAtRepair = Host.bOpenAtRepair;
 Run.bBoundRefusalObserved = Bound.bRefusalObserved; Run.bStagedReadRejected = Bound.bStagedReadRejected; Run.bBoundCompletionObserved = Bound.bCompletionObserved;
 Run.ApplyCount = Host.ApplyCount; Run.RepairCount = Host.RepairCount; Run.ChangedCount = Host.ChangedCount; Run.VerifierCalls = Verifier.Calls;
 return Run;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceRetargetSourceAssetCoreHandlesTest,
 "UEAssetDoc.AnimationProvider.SequenceRetargetSourceAssetCore.HandlePreflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceRetargetSourceAssetCoreHandlesTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceRetargetSourceAssetIntegrationTests;
 for (const ECase Case : {ECase::MissingReference, ECase::MutableReference, ECase::WrongReferenceClass, ECase::WrongReferenceIdentity,
  ECase::StaleReference, ECase::ForeignReferenceProvider, ECase::ForeignReferenceTransaction, ECase::WrongTargetClass, ECase::StaleTarget,
  ECase::ForeignTargetTransaction, ECase::ForeignRevision, ECase::WrongPhase, ECase::RuntimePath, ECase::ExtraReferenceOnClear})
  for (const bool bDirty : {false, true})
  {
   FAssets A; if (!TestTrue(TEXT("saved native preflight fixture"), A.Setup())) return false;
   CheckUsage(*this, *A.Sequence, 20.); const FValues Before = FValues::Read(*A.Sequence); const FSourceSnapshot Sources(A);
   const auto DiskBefore = Bytes(PackageFile(A.Sequence->GetPathName())); const FString ModelGuid = A.Sequence->GetDataModel()->GenerateGuid().ToString();
   A.Sequence->GetOutermost()->SetDirtyFlag(bDirty); const auto Run = Execute(*this, A, Case, false);
   TestEqual(TEXT("invalid Editor handle/value/context fails actual Core Apply"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
   TestEqual(TEXT("actual invocation attempted once"), Run.BatchCalls, 1); TestEqual(TEXT("no native changed set"), Run.ChangedCount, 0);
   TestEqual(TEXT("no Repair after failed preflight"), Run.RepairCount, 0); TestEqual(TEXT("no staged verifier after failed preflight"), Run.VerifierCalls, 0);
   TestEqual(TEXT("Core closes capture once"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
   Before.Check(*this, *A.Sequence); CheckUsage(*this, *A.Sequence, 20.); Retained(*this, *A.Sequence, A.Skeleton->GetPathName(), ModelGuid, A.Skeleton->GetGuid().ToString());
   TestEqual(TEXT("original dirty state unchanged"), A.Sequence->GetOutermost()->IsDirty(), bDirty);
   TestTrue(TEXT("installed target bytes unchanged"), Bytes(PackageFile(A.Sequence->GetPathName())) == DiskBefore); Sources.Check(*this, A);
   TestFalse(TEXT("preflight failure never poisons inverse"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
  }
 // Invalid old state is rejected before a Core transaction/capture or footprint.
 for (int32 Case = 0; Case < 3; ++Case)
  for (const bool bDirty : {false, true})
  {
   FAssets A; if (!TestTrue(TEXT("saved old-state fixture"), A.Setup(Case != 0))) return false;
   if (Case == 0) FAnimationRuntime::MakeSkeletonRefPoseFromMesh(A.Old.Get(), A.Skeleton.Get(), A.Sequence->RetargetSourceAssetReferencePose);
   if (Case == 1) A.Sequence->RetargetSourceAssetReferencePose[1].SetTranslation(FVector(10. + 1e-12, 0., 0.));
   if (Case == 2)
   {
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    A.Sequence->RetargetSourceAsset = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(A.Root + TEXT("/Unavailable.Unavailable")));
    PRAGMA_ENABLE_DEPRECATION_WARNINGS
   }
   A.Sequence->GetOutermost()->SetDirtyFlag(bDirty); const auto Before = FValues::Read(*A.Sequence); const FSourceSnapshot Sources(A);
   const auto DiskBefore = Bytes(PackageFile(A.Sequence->GetPathName())); const auto R = RequestFor(A); const auto I = IntentFor(A.Desired.Get()); FAssetDocumentPreparedMutationBatch B;
   const auto Prepared = PrepareAssetDocumentAnimationMutationBatch(R, TConstArrayView<FAssetDocumentMutationIntent>(&I, 1), B);
   TestFalse(TEXT("unrepresentable old state rejects before BeginLive"), Prepared.bValid);
   TestTrue(TEXT("specific old-state diagnostic"), Prepared.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("AnimationProvider.RetargetSource.PreimageUnrepresentable"); }));
   TestTrue(TEXT("zero prepared native contributions/footprints"), B.Contributions.IsEmpty()); Before.Check(*this, *A.Sequence);
   TestEqual(TEXT("rejected old state keeps dirty flag"), A.Sequence->GetOutermost()->IsDirty(), bDirty);
   TestTrue(TEXT("rejected old state leaves installed file"), Bytes(PackageFile(A.Sequence->GetPathName())) == DiskBefore); Sources.Check(*this, A);
  }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceRetargetSourceAssetCoreSetClearNoopTest,
 "UEAssetDoc.AnimationProvider.SequenceRetargetSourceAssetCore.SetClearNoop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceRetargetSourceAssetCoreSetClearNoopTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceRetargetSourceAssetIntegrationTests;
 for (const ECase Case : {ECase::Set, ECase::Clear, ECase::NoopSet, ECase::NoopClear})
 {
  FAssets A; if (!TestTrue(TEXT("saved native set/clear/noop fixture"), A.Setup(Case != ECase::NoopClear))) return false;
  const FSourceSnapshot Sources(A); const FString Guid = A.Sequence->GetDataModel()->GenerateGuid().ToString(); UObject* Model = A.Sequence->GetDataModelInterface().GetObject();
  CheckUsage(*this, *A.Sequence, 20.); const auto Expected = DesiredValues(A, Case); const auto Run = Execute(*this, A, Case, false);
  if (!TestEqual(TEXT("real Core operation commits"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded)) return false;
  Expected.Check(*this, *A.Sequence); CheckUsage(*this, *A.Sequence, DesiredUsage(Case));
  TestTrue(TEXT("real forward state/usage observed in Repair"), Run.bForwardObserved);
  TestEqual(TEXT("exact changed set only for set/clear"), Run.ChangedCount, Case == ECase::NoopSet || Case == ECase::NoopClear ? 0 : 1);
  TestEqual(TEXT("one actual invocation"), Run.BatchCalls, 1); TestEqual(TEXT("one ordinary staged native verification"), Run.VerifierCalls, 1);
  TestEqual(TEXT("capture closes once"), Run.Closes, 1); TestFalse(TEXT("capture closed at terminal"), Run.bCaptureOpen);
  TestEqual(TEXT("authored model identity preserved"), A.Sequence->GetDataModelInterface().GetObject(), Model);
  Retained(*this, *A.Sequence, A.Skeleton->GetPathName(), Guid, A.Skeleton->GetGuid().ToString()); Sources.Check(*this, A);
  TestFalse(TEXT("committed target is clean"), A.Sequence->GetOutermost()->IsDirty());
  if (Case == ECase::Set)
  {
   // Actual sequential set then clear is the compression-cache regression.
   const auto ClearRun = Execute(*this, A, ECase::Clear, false);
   TestEqual(TEXT("same target clear commits after changed base"), ClearRun.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
   DesiredValues(A, ECase::Clear).Check(*this, *A.Sequence); CheckUsage(*this, *A.Sequence, 20.);
   Retained(*this, *A.Sequence, A.Skeleton->GetPathName(), Guid, A.Skeleton->GetGuid().ToString()); Sources.Check(*this, A);
  }
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceRetargetSourceAssetCoreRollbackTest,
 "UEAssetDoc.AnimationProvider.SequenceRetargetSourceAssetCore.LateFailureRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceRetargetSourceAssetCoreRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceRetargetSourceAssetIntegrationTests;
 for (const ECase Case : {ECase::Set, ECase::Clear, ECase::NoopSet, ECase::NoopClear})
  for (const bool bDirty : {false, true})
  {
   FAssets A; if (!TestTrue(TEXT("saved native rollback fixture"), A.Setup(Case != ECase::NoopClear))) return false;
   const FValues Before = FValues::Read(*A.Sequence); const FSourceSnapshot Sources(A); const auto DiskBefore = Bytes(PackageFile(A.Sequence->GetPathName()));
   UAnimSequence* Original = A.Sequence.Get(); UPackage* Package = Original->GetOutermost(); UObject* Model = Original->GetDataModelInterface().GetObject();
   const FString Guid = Original->GetDataModel()->GenerateGuid().ToString(); CheckUsage(*this, *Original, 20.); Package->SetDirtyFlag(bDirty);
   const auto Run = Execute(*this, A, Case, true);
   TestEqual(TEXT("actual Core late failure terminal"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
   TestTrue(TEXT("specific late failure reached real Repair"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceRetargetSourceAssetFixture.LateFailure"); }));
   TestEqual(TEXT("one Apply"), Run.ApplyCount, 1); TestEqual(TEXT("one late Repair"), Run.RepairCount, 1);
   TestTrue(TEXT("desired complete pose and baked usage observed before rollback"), Run.bForwardObserved); TestTrue(TEXT("capture open during late Repair"), Run.bOpenAtRepair);
   TestEqual(TEXT("Core closes actual capture once"), Run.Closes, 1); TestTrue(TEXT("capture closed before rollback invalidation/inverse"), Run.bClosedBeforeRollback);
   TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen); TestEqual(TEXT("one actual rollback invalidation"), Run.Invalidations, 1);
   Before.Check(*this, *Original); CheckUsage(*this, *Original, 20.); TestEqual(TEXT("original package dirty restored last"), Package->IsDirty(), bDirty);
   TestEqual(TEXT("target identity retained"), A.Sequence.Get(), Original); TestEqual(TEXT("package identity retained"), Original->GetOutermost(), Package);
   TestEqual(TEXT("model identity retained"), Original->GetDataModelInterface().GetObject(), Model);
   Retained(*this, *Original, A.Skeleton->GetPathName(), Guid, A.Skeleton->GetGuid().ToString()); Sources.Check(*this, A);
   TestTrue(TEXT("installed target bytes unchanged by late rollback"), Bytes(PackageFile(Original->GetPathName())) == DiskBefore);
   TestEqual(TEXT("failure precedes staged verifier"), Run.VerifierCalls, 0);
   TestFalse(TEXT("no poisoned rollback"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
  }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceBoundRetargetSourceAssetCoreTest,
 "UEAssetDoc.AnimationProvider.SequenceBoundCompletionCore.RetargetSourceAsset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceBoundRetargetSourceAssetCoreTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceRetargetSourceAssetIntegrationTests;
 for (const ECase Case : {ECase::Set, ECase::Clear, ECase::NoopSet, ECase::NoopClear})
 {
  FAssets A; if (!TestTrue(TEXT("existing saved bound set/clear/noop fixture"), A.Setup(Case != ECase::NoopClear))) return false;
  const FSourceSnapshot Sources(A); const FValues Before = FValues::Read(*A.Sequence), Expected = DesiredValues(A, Case);
  UAnimSequence* Target = A.Sequence.Get(); UPackage* Package = Target->GetOutermost(); UObject* Model = Target->GetDataModelInterface().GetObject();
  USkeleton* Skeleton = A.Skeleton.Get(); USkeletalMesh* Old = A.Old.Get(); USkeletalMesh* Desired = A.Desired.Get(); const FGuid SkeletonGuid = Skeleton->GetGuid();
  const FString Guid = Target->GetDataModel()->GenerateGuid().ToString(); const bool Noop = Case == ECase::NoopSet || Case == ECase::NoopClear;
  const auto Run = Execute(*this, A, Case, false, EBoundCase::Forward);
  TestEqual(TEXT("same bound owner set/clear/noop commits through Core"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
  TestTrue(TEXT("real native completion observed before fixture Wait"), Run.bBoundCompletionObserved); TestTrue(TEXT("real forward observed in Repair"), Run.bForwardObserved);
  Expected.Check(*this, *Target); CheckUsage(*this, *Target, DesiredUsage(Case));
  TestEqual(TEXT("true set/clear noops have no changed region"), Run.ChangedCount, Noop ? 0 : 1);
  if (Noop) TestTrue(TEXT("true noop preserves entire stored pose and soft reference"), Before.Matches(*Target));
  TestEqual(TEXT("one original Composition invocation"), Run.BatchCalls, 1); TestEqual(TEXT("existing real staged verifier runs"), Run.VerifierCalls, 1);
  TestEqual(TEXT("actual capture closes once"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  TestEqual(TEXT("target identity retained"), A.Sequence.Get(), Target); TestEqual(TEXT("package identity retained"), Target->GetOutermost(), Package);
  TestEqual(TEXT("model identity retained"), Target->GetDataModelInterface().GetObject(), Model); TestEqual(TEXT("Skeleton identity retained"), Target->GetSkeleton(), Skeleton);
  TestEqual(TEXT("Skeleton GUID retained"), Skeleton->GetGuid(), SkeletonGuid); TestEqual(TEXT("old Mesh identity retained"), A.Old.Get(), Old);
  TestEqual(TEXT("supplied Mesh identity retained"), A.Desired.Get(), Desired);
  Retained(*this, *Target, Skeleton->GetPathName(), Guid, SkeletonGuid.ToString()); Sources.Check(*this, A);
  TestFalse(TEXT("Core saved target is clean"), Package->IsDirty());
 }
 for (const bool Dirty : {false, true})
 {
  FAssets A; if (!TestTrue(TEXT("existing saved bound inverse fixture"), A.Setup())) return false;
  const FValues Before = FValues::Read(*A.Sequence); const FSourceSnapshot Sources(A); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
  UAnimSequence* Target = A.Sequence.Get(); UPackage* Package = Target->GetOutermost(); UObject* Model = Target->GetDataModelInterface().GetObject();
  USkeleton* Skeleton = A.Skeleton.Get(); USkeletalMesh* Old = A.Old.Get(); USkeletalMesh* Desired = A.Desired.Get(); const FGuid SkeletonGuid = Skeleton->GetGuid();
  const FString Guid = Target->GetDataModel()->GenerateGuid().ToString(); const FTransform BeforeUsage = Usage(*Target);
  Package->SetDirtyFlag(Dirty); const auto Run = Execute(*this, A, ECase::Set, true, EBoundCase::Forward);
  TestEqual(TEXT("genuine bound later failure reaches Core"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestTrue(TEXT("specific existing late failure reached"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceRetargetSourceAssetFixture.LateFailure"); }));
  TestTrue(TEXT("forward native completion precedes later failure"), Run.bBoundCompletionObserved); TestTrue(TEXT("desired pose and usage observed before inverse"), Run.bForwardObserved);
  TestEqual(TEXT("one actual Apply"), Run.ApplyCount, 1); TestEqual(TEXT("one later Repair"), Run.RepairCount, 1); TestEqual(TEXT("real forward changes one region"), Run.ChangedCount, 1);
  TestTrue(TEXT("capture open during later failure"), Run.bOpenAtRepair); TestTrue(TEXT("capture closes before actual Core inverse"), Run.bClosedBeforeRollback);
  TestEqual(TEXT("capture closes once"), Run.Closes, 1); TestEqual(TEXT("one real handle invalidation"), Run.Invalidations, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  Before.Check(*this, *Target); CheckUsage(*this, *Target, 20.); TestTrue(TEXT("Core inverse restores complete native sample"), Usage(*Target).Equals(BeforeUsage, 1e-5));
  TestEqual(TEXT("Core inverse restores dirty last"), Package->IsDirty(), Dirty);
  TestEqual(TEXT("inverse target identity"), A.Sequence.Get(), Target); TestEqual(TEXT("inverse package identity"), Target->GetOutermost(), Package);
  TestEqual(TEXT("inverse model identity"), Target->GetDataModelInterface().GetObject(), Model); TestEqual(TEXT("inverse Skeleton identity"), Target->GetSkeleton(), Skeleton);
  TestEqual(TEXT("inverse Skeleton GUID"), Skeleton->GetGuid(), SkeletonGuid); TestEqual(TEXT("inverse old Mesh identity"), A.Old.Get(), Old); TestEqual(TEXT("inverse supplied Mesh identity"), A.Desired.Get(), Desired);
  Retained(*this, *Target, Skeleton->GetPathName(), Guid, SkeletonGuid.ToString()); Sources.Check(*this, A);
  TestTrue(TEXT("later failure leaves installed target bytes"), Bytes(PackageFile(Target->GetPathName())) == Disk); TestEqual(TEXT("later failure precedes staged verifier"), Run.VerifierCalls, 0);
  TestFalse(TEXT("genuine Core inverse is not poisoned"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 for (const EBoundCase Case : {EBoundCase::MissingTarget, EBoundCase::ReadOnlyTarget, EBoundCase::WrongTargetClass, EBoundCase::ForeignTarget,
  EBoundCase::ForeignTransaction, EBoundCase::ForeignRequest, EBoundCase::ForeignRevision, EBoundCase::WrongPhase, EBoundCase::ExtraValue, EBoundCase::Closed, EBoundCase::StagedRevision})
 {
  FAssets A; if (!TestTrue(TEXT("existing saved bound refusal fixture"), A.Setup())) return false;
  const FValues Before = FValues::Read(*A.Sequence); const FSourceSnapshot Sources(A); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
  A.Sequence->GetOutermost()->SetDirtyFlag(true); const auto Run = Execute(*this, A, ECase::Set, false, Case);
  TestTrue(TEXT("actual bound refusal observed before Composition/Core recovery"), Run.bBoundRefusalObserved);
  if (Case == EBoundCase::StagedRevision) TestTrue(TEXT("actual staged API refusal observed"), Run.bStagedReadRejected);
  TestEqual(TEXT("original Core invocation fails"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestEqual(TEXT("one original Composition invocation"), Run.BatchCalls, 1); TestEqual(TEXT("no changed region"), Run.ChangedCount, 0);
  TestEqual(TEXT("refusal precedes Repair"), Run.RepairCount, 0); TestEqual(TEXT("refusal precedes staged verifier"), Run.VerifierCalls, 0);
  TestEqual(TEXT("actual capture closes once"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  Before.Check(*this, *A.Sequence); CheckUsage(*this, *A.Sequence, 20.); Sources.Check(*this, A);
  TestTrue(TEXT("terminal refusal preserves original dirty"), A.Sequence->GetOutermost()->IsDirty());
  TestTrue(TEXT("terminal refusal leaves installed target bytes"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
  TestFalse(TEXT("refusal never poisons inverse"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 return !HasAnyErrors();
}
#endif
