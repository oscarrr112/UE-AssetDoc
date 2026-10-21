// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceAsset.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentAnimationMutationPreparation.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// Native preparation contract only. Integration owns actual Editor handles,
// invocation, Core rollback and distinct-process save/fresh-read evidence.
namespace AssetDocumentSequenceRetargetSourceAssetContractTests
{
constexpr const TCHAR* Primitive = TEXT("animation.sequence.retarget_source_asset.set.v2");
constexpr const TCHAR* Schema = TEXT("animation.sequence.retarget_source_asset.set.v2.input.v2");
FAssetDocumentRuntimeValueDto String(const FString& Value)
{
 FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::String; R.StringValue = Value; return R;
}
bool ExactPose(const TArray<FTransform>& A, const TArray<FTransform>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I)
  if (A[I].GetTranslation() != B[I].GetTranslation() || A[I].GetRotation() != B[I].GetRotation()
   || A[I].GetScale3D() != B[I].GetScale3D()) return false;
 return true;
}
struct FFixture
{
 FString Root = TEXT("/Game/AssetDocumentSequenceRetargetSourceContract/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<USkeleton> Skeleton;
 TStrongObjectPtr<USkeletalMesh> Old, Desired;
 TStrongObjectPtr<UAnimSequence> Sequence;
 bool Setup()
 {
  // Test-owned authored fixtures, not a product construction route.
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  {
   FReferenceSkeletonModifier Bones(Skeleton.Get());
   Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
   Bones.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(1., 0., 0.)));
  }
  auto Mesh = [&](const TCHAR* Name, double Offset)
  {
   auto* M = NewObject<USkeletalMesh>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone);
   M->SetSkeleton(Skeleton.Get());
   FReferenceSkeletonModifier Bones(M->GetRefSkeleton(), Skeleton.Get());
   Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
   Bones.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(Offset, 0., 0.)));
   return M;
  };
  Old.Reset(Mesh(TEXT("Old"), 13.)); Desired.Reset(Mesh(TEXT("Desired"), 29.));
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  Sequence->SetSkeleton(Skeleton.Get()); Sequence->RateScale = 1.25f;
  auto& C = Sequence->GetController();
  C.OpenBracket(FText::FromString(TEXT("Retarget source contract fixture")), false);
  C.InitializeModel(); C.SetFrameRate(FFrameRate(24, 1), false); C.SetNumberOfFrames(FFrameNumber(12), false);
  const bool bTrack = C.AddBoneCurve(TEXT("root"), false);
  TArray<FVector3f> P, S; TArray<FQuat4f> Q;
  P.Init(FVector3f::ZeroVector, 13); S.Init(FVector3f::OneVector, 13); Q.Init(FQuat4f::Identity, 13);
  const bool bKeys = bTrack && C.SetBoneTrackKeys(TEXT("root"), P, Q, S, false);
  C.NotifyPopulated(); C.CloseBracket(false); Sequence->WaitOnExistingCompression();
  SetOld(false);
  return bTrack && bKeys;
 }
 void Clean()
 {
  for (UObject* O : {static_cast<UObject*>(Skeleton.Get()), static_cast<UObject*>(Old.Get()),
   static_cast<UObject*>(Desired.Get()), static_cast<UObject*>(Sequence.Get())}) O->GetOutermost()->SetDirtyFlag(false);
 }
 void SetOld(bool bMesh)
 {
  Sequence->RetargetSource = NAME_None;
  if (bMesh) Sequence->SetRetargetSourceAsset(Old.Get()); else Sequence->ClearRetargetSourceAsset();
  Sequence->UpdateRetargetSourceAssetData();
  Sequence->ClearAllCachedCookedPlatformData(); Sequence->CacheDerivedDataForCurrentPlatform(); Sequence->WaitOnExistingCompression();
  Clean();
 }
 ~FFixture()
 {
  if (Sequence.IsValid()) Sequence->WaitOnExistingCompression();
  for (UObject* O : {static_cast<UObject*>(Sequence.Get()), static_cast<UObject*>(Old.Get()),
   static_cast<UObject*>(Desired.Get()), static_cast<UObject*>(Skeleton.Get())})
   if (IsValid(O)) { O->ClearFlags(RF_Public | RF_Standalone); O->GetOutermost()->SetDirtyFlag(false); }
 }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 1;
  R.Operation = EAssetDocumentRuntimeOperation::Apply; R.TargetAssetPath = Sequence->GetPathName();
  R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence"); return R;
 }
 FAssetDocumentMutationIntent Intent(USkeletalMesh* Mesh) const
 {
  FAssetDocumentMutationIntent I; I.StepIndex = 0; I.Requirement = {Primitive, 2, TEXT("animation_profile")};
  I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.InputSchemaId = Schema;
  I.Intent.Kind = EAssetDocumentRuntimeValueKind::Object;
  I.Intent.ObjectKeys = {TEXT("operation"), TEXT("retarget_source_asset_path")};
  I.Intent.Elements = {String(Mesh ? TEXT("set") : TEXT("clear")), Mesh ? String(Mesh->GetPathName()) : FAssetDocumentRuntimeValueDto()};
  return I;
 }
};
struct FSnapshot
{
 FSoftObjectPath Reference;
 TArray<FTransform> Pose;
 USkeleton* Skeleton;
 FGuid SkeletonGuid, ModelGuid;
 FName RetargetSource;
 bool bDirty, bSkeletonDirty, bOldDirty, bDesiredDirty;
 explicit FSnapshot(const FFixture& F)
  : Reference(F.Sequence->GetRetargetSourceAsset().ToSoftObjectPath()), Pose(F.Sequence->RetargetSourceAssetReferencePose),
    Skeleton(F.Sequence->GetSkeleton()), SkeletonGuid(F.Sequence->GetSkeletonGuid()), ModelGuid(F.Sequence->GetDataModel()->GenerateGuid()),
    RetargetSource(F.Sequence->RetargetSource), bDirty(F.Sequence->GetOutermost()->IsDirty()),
    bSkeletonDirty(F.Skeleton->GetOutermost()->IsDirty()), bOldDirty(F.Old->GetOutermost()->IsDirty()),
    bDesiredDirty(F.Desired->GetOutermost()->IsDirty()) {}
 void Check(FAutomationTestBase& T, const FFixture& F) const
 {
  T.TestEqual(TEXT("preparation preserves exact soft reference"), F.Sequence->GetRetargetSourceAsset().ToSoftObjectPath(), Reference);
  T.TestTrue(TEXT("preparation preserves complete stored pose exactly"), ExactPose(F.Sequence->RetargetSourceAssetReferencePose, Pose));
  T.TestEqual(TEXT("preparation preserves Skeleton identity"), F.Sequence->GetSkeleton(), Skeleton);
  T.TestEqual(TEXT("preparation preserves Skeleton GUID"), F.Sequence->GetSkeletonGuid(), SkeletonGuid);
  T.TestEqual(TEXT("preparation preserves RetargetSource"), F.Sequence->RetargetSource, RetargetSource);
  T.TestEqual(TEXT("preparation preserves authored model"), F.Sequence->GetDataModel()->GenerateGuid(), ModelGuid);
  T.TestEqual(TEXT("preparation preserves original target dirty state"), F.Sequence->GetOutermost()->IsDirty(), bDirty);
  T.TestEqual(TEXT("preparation preserves Skeleton dirty state"), F.Skeleton->GetOutermost()->IsDirty(), bSkeletonDirty);
  T.TestEqual(TEXT("preparation preserves old mesh dirty state"), F.Old->GetOutermost()->IsDirty(), bOldDirty);
  T.TestEqual(TEXT("preparation preserves desired mesh dirty state"), F.Desired->GetOutermost()->IsDirty(), bDesiredDirty);
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequenceRetargetSourceAssetContractTest,
 "UEAssetDoc.AnimationProvider.SequenceRetargetSourceAsset.Contract",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSequenceRetargetSourceAssetContractTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceRetargetSourceAssetContractTests;
 const auto D = MakeAnimationSequenceRetargetSourceAssetDescriptor();
 TestTrue(TEXT("exact RetargetSourceAsset v2 admitted"), ValidateAnimationAdmissionEvidence(D).bValid);
 TestTrue(TEXT("exact native authority shape"), IsSequenceRetargetSourceAssetAdmissionShape(D));
 TestEqual(TEXT("frozen v2 primitive"), D.PrimitiveId, FString(Primitive));
 TestEqual(TEXT("frozen v2 version"), D.Version, uint32(2));
 TestEqual(TEXT("frozen v2 input schema"), D.InputSchemaId, FString(Schema));
 if (!TestEqual(TEXT("only target and optional source handles"), D.HandleArgumentSchemas.Num(), 2)) return false;
 TestEqual(TEXT("target handle name"), D.HandleArgumentSchemas[0].Name, FString(TEXT("sequence")));
 TestEqual(TEXT("source handle name"), D.HandleArgumentSchemas[1].Name, FString(TEXT("retarget_source_asset")));
 TestEqual(TEXT("target handle is mutable"), D.HandleArgumentSchemas[0].RequiredAccess, EAssetDocumentHandleAccess::Mutable);
 TestEqual(TEXT("source handle is readonly"), D.HandleArgumentSchemas[1].RequiredAccess, EAssetDocumentHandleAccess::ReadOnly);
 TestFalse(TEXT("clear omits source handle"), D.HandleArgumentSchemas[1].bRequired);
 for (int32 Case = 0; Case < 8; ++Case)
 {
  auto Bad = D;
  if (Case == 0) Bad.HandleArgumentSchemas[0].Name = TEXT("asset");
  if (Case == 1) Bad.HandleArgumentSchemas[1].RequiredAccess = EAssetDocumentHandleAccess::Mutable;
  if (Case == 2) Bad.HandleArgumentSchemas[1].bRequired = true;
  if (Case == 3) Bad.AllowedPhases.Add(EAssetDocumentRuntimeRequestPhase::Verify);
  if (Case == 4) Bad.InputSchemaId += TEXT(".drift");
  if (Case == 5) Bad.Admission.ExportedAuthoritativeApi = TEXT("SetRetargetSourceAsset");
  if (Case == 6) Bad.Admission.RollbackContractId.Reset();
  if (Case == 7) Bad.Version = 1;
  TestFalse(TEXT("closed descriptor rejects drift"), IsSequenceRetargetSourceAssetAdmissionShape(Bad));
  TestFalse(TEXT("admission rejects descriptor drift"), ValidateAnimationAdmissionEvidence(Bad).bValid);
 }
 FFixture F; if (!TestTrue(TEXT("valid existing native Sequence fixture"), F.Setup())) return false;
 TArray<FTransform> OldPose, DesiredPose;
 FAnimationRuntime::MakeSkeletonRefPoseFromMesh(F.Old.Get(), F.Skeleton.Get(), OldPose);
 FAnimationRuntime::MakeSkeletonRefPoseFromMesh(F.Desired.Get(), F.Skeleton.Get(), DesiredPose);
 if (!TestEqual(TEXT("public helper produces the whole two-bone pose"), OldPose.Num(), 2)) return false;
 TestFalse(TEXT("old and desired source poses are observably distinct"), ExactPose(OldPose, DesiredPose));
 const auto Request = F.Request();
 for (const bool bOldMesh : {false, true})
 {
  F.SetOld(bOldMesh);
  TestTrue(TEXT("public Set/Clear+Update yields exact representable preimage"),
   ExactPose(F.Sequence->RetargetSourceAssetReferencePose, bOldMesh ? OldPose : TArray<FTransform>()));
  if (bOldMesh) F.Sequence->RetargetSource = TEXT("RetainedSource"); // Getter masking cannot erase the stored preimage.
  TArray<USkeletalMesh*> Operations{F.Desired.Get(), nullptr};
  if (bOldMesh) Operations.Add(F.Old.Get());
  for (USkeletalMesh* Mesh : Operations)
  {
   const FSnapshot Before(F); const auto I = F.Intent(Mesh); FAssetDocumentPreparedMutationBatch B;
   TestTrue(TEXT("loaded compatible set/clear/equal intents prepare"),
    PrepareAssetDocumentAnimationMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&I, 1), B).bValid);
   TestEqual(TEXT("one bounded native contribution"), B.Contributions.Num(), 1);
   Before.Check(*this, F);
  }
 }
 F.SetOld(true);
 const auto Valid = F.Intent(F.Desired.Get());
 for (int32 Case = 0; Case < 13; ++Case)
 {
  auto Bad = Valid; auto R = Request;
  if (Case == 0) { Bad.Intent.ObjectKeys.Add(TEXT("pose")); Bad.Intent.Elements.Add(FAssetDocumentRuntimeValueDto()); }
  if (Case == 1) Bad.Intent.Elements[0] = String(TEXT("reset"));
  if (Case == 2) Bad.Intent.Elements[0] = FAssetDocumentRuntimeValueDto();
  if (Case == 3) Bad.Intent.Elements[1] = String(F.Desired->GetOutermost()->GetName());
  if (Case == 4) Bad.Intent.Elements[1] = String(F.Desired->GetPathName() + TEXT(":SubObject"));
  if (Case == 5) Bad.Intent.Elements[1] = String(F.Skeleton->GetPathName());
  if (Case == 6) Bad.Intent.Elements[1] = String(F.Root + TEXT("/Missing.Missing"));
  if (Case == 7) Bad.Intent.Elements[0] = String(TEXT("clear"));
  if (Case == 8) Bad.Intent.Elements[1] = FAssetDocumentRuntimeValueDto();
  if (Case == 9) Bad.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == 10) R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
  if (Case == 11) Bad.InputSchemaId += TEXT(".drift");
  if (Case == 12) R.TargetAssetPath = F.Skeleton->GetPathName();
  const FSnapshot Before(F); FAssetDocumentPreparedMutationBatch Rejected;
  TestFalse(TEXT("closed operation/path/class/context rejects invalid preparation"),
   PrepareAssetDocumentAnimationMutationBatch(R, TConstArrayView<FAssetDocumentMutationIntent>(&Bad, 1), Rejected).bValid);
  TestTrue(TEXT("invalid preparation leaves no contribution"), Rejected.Contributions.IsEmpty());
  Before.Check(*this, F);
 }
 {
  TStrongObjectPtr<USkeleton> ForeignSkeleton(NewObject<USkeleton>(CreatePackage(*(F.Root + TEXT("/ForeignSkeleton"))), TEXT("ForeignSkeleton"), RF_Public | RF_Standalone));
  { FReferenceSkeletonModifier Bones(ForeignSkeleton.Get()); Bones.Add(FMeshBoneInfo(TEXT("foreign_root"), TEXT("foreign_root"), INDEX_NONE), FTransform::Identity); }
  TStrongObjectPtr<USkeletalMesh> ForeignMesh(NewObject<USkeletalMesh>(CreatePackage(*(F.Root + TEXT("/ForeignMesh"))), TEXT("ForeignMesh"), RF_Public | RF_Standalone));
  ForeignMesh->SetSkeleton(ForeignSkeleton.Get());
  { FReferenceSkeletonModifier Bones(ForeignMesh->GetRefSkeleton(), ForeignSkeleton.Get()); Bones.Add(FMeshBoneInfo(TEXT("foreign_root"), TEXT("foreign_root"), INDEX_NONE), FTransform::Identity); }
  ForeignSkeleton->GetOutermost()->SetDirtyFlag(false); ForeignMesh->GetOutermost()->SetDirtyFlag(false);
  const FSnapshot Before(F); const auto I = F.Intent(ForeignMesh.Get()); FAssetDocumentPreparedMutationBatch Rejected;
  TestFalse(TEXT("incompatible loaded exact Mesh rejects before mutation"),
   PrepareAssetDocumentAnimationMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&I, 1), Rejected).bValid);
  TestTrue(TEXT("incompatible Mesh leaves no contribution"), Rejected.Contributions.IsEmpty());
  Before.Check(*this, F);
  TestFalse(TEXT("foreign Skeleton remains clean"), ForeignSkeleton->GetOutermost()->IsDirty());
  TestFalse(TEXT("foreign Mesh remains clean"), ForeignMesh->GetOutermost()->IsDirty());
  ForeignMesh->ClearFlags(RF_Public | RF_Standalone); ForeignSkeleton->ClearFlags(RF_Public | RF_Standalone);
 }
 for (int32 Case = 0; Case < 7; ++Case)
  for (const bool bDirty : {false, true})
  {
   F.SetOld(Case != 0 && Case != 6);
   // Deliberately malformed test-owned preimages. Product has no direct pose writer.
   if (Case == 0) F.Sequence->RetargetSourceAssetReferencePose = OldPose;
   if (Case == 1) F.Sequence->RetargetSourceAssetReferencePose.Empty();
   if (Case == 2) F.Sequence->RetargetSourceAssetReferencePose[1].SetTranslation(FVector(13. + 1.e-12, 0., 0.));
   if (Case == 3) F.Sequence->RetargetSourceAssetReferencePose[0].SetRotation(FQuat(0., 0., 0., -1.));
   if (Case == 4) F.Sequence->RetargetSourceAssetReferencePose.SetNum(1);
   if (Case == 5) F.Sequence->RetargetSourceAssetReferencePose = DesiredPose;
   if (Case == 6)
   {
    // Public setters only accept loaded meshes; this represents a pre-existing
    // serialized but unavailable soft reference and never calls a loader.
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    F.Sequence->RetargetSourceAsset = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(F.Root + TEXT("/Unavailable.Unavailable")));
    PRAGMA_ENABLE_DEPRECATION_WARNINGS
   }
   F.Sequence->RetargetSource = TEXT("RetainedSource");
   F.Sequence->GetOutermost()->SetDirtyFlag(bDirty);
   const FSnapshot Before(F); FAssetDocumentPreparedMutationBatch Rejected;
   const auto Result = PrepareAssetDocumentAnimationMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&Valid, 1), Rejected);
   TestFalse(TEXT("unrepresentable complete preimage rejects before mutation"), Result.bValid);
   TestTrue(TEXT("unrepresentable diagnostic is explicit"), Result.Diagnostics.ContainsByPredicate([](const auto& Diagnostic)
    { return Diagnostic.Code == TEXT("AnimationProvider.RetargetSource.PreimageUnrepresentable"); }));
   TestTrue(TEXT("unrepresentable state has zero prepared footprints/contributions"), Rejected.Contributions.IsEmpty());
   Before.Check(*this, F);
  }
 return !HasAnyErrors();
}
#endif
