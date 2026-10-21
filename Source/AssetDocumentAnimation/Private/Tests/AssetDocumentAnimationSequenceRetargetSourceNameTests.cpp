// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceName.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentAnimationMutationPreparation.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// Preparation/admission only. Genuine Core rollback, invocation completion and
// distinct-process save/fresh-read belong to the integration tests.
namespace AssetDocumentSequenceRetargetSourceNameContractTests
{
constexpr const TCHAR* Primitive = TEXT("animation.sequence.retarget_source_name.set.v1");
constexpr const TCHAR* Schema = TEXT("animation.sequence.retarget_source_name.set.v1.input.v1");
FAssetDocumentRuntimeValueDto String(const FString& Text)
{ FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::String; V.StringValue = Text; return V; }
FAssetDocumentRuntimeValueDto NameValue(const FString& Text)
{
 FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Object;
 V.ObjectKeys = {TEXT("retarget_source_name")}; V.Elements = {String(Text)}; return V;
}
bool ExactName(FName A, FName B) { return A.IsEqual(B, ENameCase::CaseSensitive, true); }
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
 for (const auto& P : A)
 {
  const auto* Value = B.Find(P.Key);
  // Locate the stored key directly; native lookup alone ignores display case.
  bool bExactKey = false;
  for (const auto& Other : B) if (P.Key == Other.Key) bExactKey = ExactName(P.Key, Other.Key);
  if (!Value || !bExactKey || !ExactName(P.Value.PoseName, Value->PoseName)
   || !ExactPose(P.Value.ReferencePose, Value->ReferencePose)
   || P.Value.SourceReferenceMesh.ToSoftObjectPath().ToString() != Value->SourceReferenceMesh.ToSoftObjectPath().ToString()) return false;
 }
 return true;
}
struct FFixture
{
 FString Root = TEXT("/Game/AssetDocumentSequenceRetargetNameContract/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<USkeleton> Skeleton;
 TStrongObjectPtr<USkeletalMesh> SourceMesh;
 TStrongObjectPtr<UAnimSequence> Sequence;
 FSoftObjectPath UnloadedMesh;
 bool Setup()
 {
  // Test-owned authored fixtures; the product has no construction route here.
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  { FReferenceSkeletonModifier Bones(Skeleton.Get());
   Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
   Bones.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(1., 0., 0.))); }
  SourceMesh.Reset(NewObject<USkeletalMesh>(CreatePackage(*(Root + TEXT("/Source"))), TEXT("Source"), RF_Public | RF_Standalone));
  SourceMesh->SetSkeleton(Skeleton.Get());
  { FReferenceSkeletonModifier Bones(SourceMesh->GetRefSkeleton(), Skeleton.Get());
   Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
   Bones.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(13., 0., 0.))); }
  UnloadedMesh = FSoftObjectPath(Root + TEXT("/NeverLoaded.NeverLoaded"));
  FReferencePose Named; Named.PoseName = TEXT("DisplayMetadata_3");
  Named.ReferencePose = {FTransform::Identity, FTransform(FVector(29., 0., 0.))};
  Named.SourceReferenceMesh = TSoftObjectPtr<USkeletalMesh>(UnloadedMesh);
  Skeleton->AnimRetargetSources.Add(TEXT("NamedPose_7"), Named);
  Named.PoseName = TEXT("UnrelatedMetadata");
  Skeleton->AnimRetargetSources.Add(TEXT("UnrelatedPose"), Named);
  Named.PoseName = TEXT("LiteralDefaultMetadata");
  Named.ReferencePose[1].SetTranslation(FVector(41., 0., 0.));
  Skeleton->AnimRetargetSources.Add(TEXT("Default"), Named);
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  Sequence->SetSkeleton(Skeleton.Get()); Sequence->RateScale = 1.25f;
  auto& C = Sequence->GetController();
  C.OpenBracket(FText::FromString(TEXT("Retarget name preparation fixture")), false);
  C.InitializeModel(); C.SetFrameRate(FFrameRate(24, 1), false); C.SetNumberOfFrames(FFrameNumber(12), false);
  const bool bTrack = C.AddBoneCurve(TEXT("root"), false);
  TArray<FVector3f> P, S; TArray<FQuat4f> Q;
  P.Init(FVector3f::ZeroVector, 13); S.Init(FVector3f::OneVector, 13); Q.Init(FQuat4f::Identity, 13);
  const bool bKeys = bTrack && C.SetBoneTrackKeys(TEXT("root"), P, Q, S, false);
  C.NotifyPopulated(); C.CloseBracket(false); Sequence->WaitOnExistingCompression();
  Sequence->SetRetargetSourceAsset(SourceMesh.Get()); Sequence->UpdateRetargetSourceAssetData();
  Sequence->RetargetSource = FName(TEXT("RetainedSource_9"));
  Sequence->ClearAllCachedCookedPlatformData(); Sequence->CacheDerivedDataForCurrentPlatform(); Sequence->WaitOnExistingCompression();
  for (UObject* O : {static_cast<UObject*>(Sequence.Get()), static_cast<UObject*>(Skeleton.Get()), static_cast<UObject*>(SourceMesh.Get())})
   O->GetOutermost()->SetDirtyFlag(false);
  return bKeys;
 }
 ~FFixture()
 {
  if (Sequence.IsValid()) Sequence->WaitOnExistingCompression();
  for (UObject* O : {static_cast<UObject*>(Sequence.Get()), static_cast<UObject*>(SourceMesh.Get()), static_cast<UObject*>(Skeleton.Get())})
   if (IsValid(O)) { O->ClearFlags(RF_Public | RF_Standalone); O->GetOutermost()->SetDirtyFlag(false); }
 }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 1;
  R.Operation = EAssetDocumentRuntimeOperation::Apply; R.TargetAssetPath = Sequence->GetPathName();
  R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence"); return R;
 }
 FAssetDocumentMutationIntent Intent(const FString& Text) const
 {
  FAssetDocumentMutationIntent I; I.StepIndex = 0; I.Requirement = {Primitive, 1, TEXT("animation_profile")};
  I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.InputSchemaId = Schema; I.Intent = NameValue(Text); return I;
 }
};
struct FSnapshot
{
 FSoftObjectPath Asset; TArray<FTransform> Pose, Effective; FName Name, EffectiveSource;
 USkeleton* Skeleton; FGuid SkeletonGuid, Model; float Rate;
 TMap<FName, FReferencePose> Map;
 bool bTargetDirty, bSkeletonDirty, bMeshDirty;
 explicit FSnapshot(const FFixture& F)
  : Asset(F.Sequence->GetRetargetSourceAsset().ToSoftObjectPath()), Pose(F.Sequence->RetargetSourceAssetReferencePose),
    Effective(F.Sequence->GetRetargetTransforms()), Name(F.Sequence->RetargetSource), EffectiveSource(F.Sequence->GetRetargetTransformsSourceName()),
    Skeleton(F.Sequence->GetSkeleton()), SkeletonGuid(F.Sequence->GetSkeletonGuid()), Model(F.Sequence->GetDataModel()->GenerateGuid()),
    Rate(F.Sequence->RateScale), Map(F.Skeleton->AnimRetargetSources), bTargetDirty(F.Sequence->GetOutermost()->IsDirty()),
    bSkeletonDirty(F.Skeleton->GetOutermost()->IsDirty()), bMeshDirty(F.SourceMesh->GetOutermost()->IsDirty()) {}
 void Check(FAutomationTestBase& T, const FFixture& F, bool bCheckMap = true) const
 {
  T.TestTrue(TEXT("preparation preserves exact authored FName"), ExactName(F.Sequence->RetargetSource, Name));
  T.TestEqual(TEXT("retained source asset is unchanged"), F.Sequence->GetRetargetSourceAsset().ToSoftObjectPath(), Asset);
  T.TestTrue(TEXT("entire hidden stored pose is unchanged"), ExactPose(F.Sequence->RetargetSourceAssetReferencePose, Pose));
  T.TestTrue(TEXT("effective native pose is unchanged"), ExactPose(F.Sequence->GetRetargetTransforms(), Effective));
  T.TestTrue(TEXT("effective native source name is unchanged"), ExactName(F.Sequence->GetRetargetTransformsSourceName(), EffectiveSource));
  T.TestEqual(TEXT("Skeleton identity is unchanged"), F.Sequence->GetSkeleton(), Skeleton);
  T.TestEqual(TEXT("Skeleton GUID is unchanged"), F.Sequence->GetSkeletonGuid(), SkeletonGuid);
  T.TestEqual(TEXT("authored model is unchanged"), F.Sequence->GetDataModel()->GenerateGuid(), Model);
  T.TestEqual(TEXT("unrelated scalar is unchanged"), F.Sequence->RateScale, Rate);
  if (bCheckMap) T.TestTrue(TEXT("full named map is unchanged"), ExactMap(F.Skeleton->AnimRetargetSources, Map));
  T.TestEqual(TEXT("target dirty preimage is unchanged"), F.Sequence->GetOutermost()->IsDirty(), bTargetDirty);
  T.TestEqual(TEXT("Skeleton package is unchanged"), F.Skeleton->GetOutermost()->IsDirty(), bSkeletonDirty);
  T.TestEqual(TEXT("source mesh package is unchanged"), F.SourceMesh->GetOutermost()->IsDirty(), bMeshDirty);
  T.TestNull(TEXT("map soft mesh was not loaded"), F.UnloadedMesh.ResolveObject());
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequenceRetargetSourceNameContractTest,
 "UEAssetDoc.AnimationProvider.SequenceRetargetSourceName.Contract",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSequenceRetargetSourceNameContractTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceRetargetSourceNameContractTests;
 const auto D = MakeAnimationSequenceRetargetSourceNameDescriptor();
 TestTrue(TEXT("exact native leaf is admitted"), ValidateAnimationAdmissionEvidence(D).bValid);
 TestTrue(TEXT("exact native admission shape"), IsSequenceRetargetSourceNameAdmissionShape(D));
 TestEqual(TEXT("frozen primitive"), D.PrimitiveId, FString(Primitive));
 TestEqual(TEXT("frozen version"), D.Version, uint32(1));
 TestEqual(TEXT("frozen input schema"), D.InputSchemaId, FString(Schema));
 TestEqual(TEXT("Animation provider remains owner"), D.ProviderId, FString(TEXT("animation")));
 TestEqual(TEXT("fixed mutation access"), D.Access, EAssetDocumentPrimitiveAccess::Mutating);
 TestTrue(TEXT("parent transaction is required"), D.bRequiresActiveTransaction);
 if (!TestEqual(TEXT("exactly one target handle"), D.HandleArgumentSchemas.Num(), 1)) return false;
 TestEqual(TEXT("sole native role"), D.HandleArgumentSchemas[0].Name, FString(TEXT("sequence")));
 TestEqual(TEXT("Editor target access is mutable"), D.HandleArgumentSchemas[0].RequiredAccess, EAssetDocumentHandleAccess::Mutable);
 TestTrue(TEXT("target is required"), D.HandleArgumentSchemas[0].bRequired);
 TestTrue(TEXT("no reference role or handle output"), D.HandleResultSchemas.IsEmpty());
 for (int32 Case = 0; Case < 12; ++Case)
 {
  auto Bad = D;
  if (Case == 0) Bad.HandleArgumentSchemas[0].Name = TEXT("asset");
  if (Case == 1) Bad.HandleArgumentSchemas[0].RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
  if (Case == 2) Bad.HandleArgumentSchemas[0].bRequired = false;
  if (Case == 3) Bad.HandleArgumentSchemas.Add({TEXT("retarget_source_asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, false});
  if (Case == 4) Bad.AllowedPhases.Add(EAssetDocumentRuntimeRequestPhase::Verify);
  if (Case == 5) Bad.InputSchemaId += TEXT(".drift");
  if (Case == 6) Bad.Admission.ExportedAuthoritativeApi = TEXT("FNameProperty::SetPropertyValue");
  if (Case == 7) Bad.Admission.RollbackContractId.Reset();
  if (Case == 8) Bad.Version = 2;
  if (Case == 9) Bad.Admission.RawLifecycleAllowlistId = TEXT("foreign-lifecycle");
  if (Case == 10) Bad.RequiredConsumerIds.Add(TEXT("generic_property"));
  if (Case == 11) Bad.Admission.PrimaryOperationId = TEXT("editor.property.set_values.v1");
  TestFalse(TEXT("closed descriptor rejects a wider authority contract"), IsSequenceRetargetSourceNameAdmissionShape(Bad));
  TestFalse(TEXT("admission also rejects the widened contract"), ValidateAnimationAdmissionEvidence(Bad).bValid);
 }
 const auto Decisions = GetAssetDocumentAnimationDecisions();
 const auto Descriptors = GetAssetDocumentAnimationPrimitiveDescriptors();
 const auto Bindings = GetAssetDocumentAnimationPrimitiveBindings();
 TestTrue(TEXT("current decision/descriptor/binding bijection"), ValidateAnimationDescriptorBindingBijection(Decisions, Descriptors, Bindings).bValid);
 int32 DecisionCount = 0, DescriptorCount = 0, BindingCount = 0;
 for (const auto& Row : Decisions) if (Row.PrimitiveId == Primitive && Row.Version == 1)
 { ++DecisionCount; TestEqual(TEXT("new exact row is admitted"), Row.Disposition, EAssetDocumentAnimationDecisionDisposition::Admitted);
  TestEqual(TEXT("new row consumer is animation_profile"), Row.ConsumerId, FString(TEXT("animation_profile"))); }
 for (const auto& Row : Descriptors) if (Row.PrimitiveId == Primitive && Row.Version == 1)
 { ++DescriptorCount; TestTrue(TEXT("catalog uses exact new descriptor"), IsSequenceRetargetSourceNameAdmissionShape(Row)); }
 for (const auto& Row : Bindings) if (Row.PrimitiveId == Primitive && Row.Version == 1)
 { ++BindingCount; TestTrue(TEXT("new binding invokes the dedicated fixed leaf"), Row.Handler == &InvokeAnimationSequenceRetargetSourceName); }
 TestEqual(TEXT("one new decision"), DecisionCount, 1); TestEqual(TEXT("one new descriptor"), DescriptorCount, 1); TestEqual(TEXT("one new binding"), BindingCount, 1);
 for (const TCHAR* Blocked : {TEXT("animation.sequence.retarget_source_asset.set.v1"), TEXT("animation.sequence.complete_edit.v1")})
 {
  int32 Rows = 0;
  for (const auto& Row : Decisions) if (Row.PrimitiveId == Blocked && Row.Version == 1)
  {
   ++Rows; TestEqual(TEXT("legacy v1 remains an ordinary stable blocker"), Row.Disposition, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker);
   TestEqual(TEXT("legacy blocker diagnostic is unchanged"), Row.StableBlockerDiagnostic,
    FString(FString(Blocked).Contains(TEXT("retarget_source_asset")) ? TEXT("AD.PANIM.BLOCK.RETARGET_SOURCE_AUTHORITY") : TEXT("AD.PANIM.BLOCK.SEQUENCE_COMPLETION_AUTHORITY")));
  }
  TestEqual(TEXT("exactly one legacy blocker row"), Rows, 1);
  for (const auto& Row : Descriptors) TestFalse(TEXT("legacy v1 has no callable descriptor"), Row.PrimitiveId == Blocked && Row.Version == 1);
  for (const auto& Row : Bindings) TestFalse(TEXT("legacy v1 has no callable binding"), Row.PrimitiveId == Blocked && Row.Version == 1);
 }
 FFixture F; if (!TestTrue(TEXT("valid existing native Sequence fixture"), F.Setup())) return false;
 TestNull(TEXT("fixture starts with unresolved map source mesh"), F.UnloadedMesh.ResolveObject());
 TestTrue(TEXT("unknown original name uses stock Skeleton default pose"), ExactPose(F.Sequence->GetRetargetTransforms(), F.Skeleton->GetReferenceSkeleton().GetRefBonePose()));
 TestTrue(TEXT("unknown original name retains its native identity"), ExactName(F.Sequence->GetRetargetTransformsSourceName(), FName(TEXT("RetainedSource_9"))));
 TestTrue(TEXT("literal Default is a named map key"), ExactPose(F.Skeleton->GetRefLocalPoses(TEXT("Default")), F.Skeleton->AnimRetargetSources.FindChecked(TEXT("Default")).ReferencePose));
 TestFalse(TEXT("named Default differs from default Skeleton pose"), ExactPose(F.Skeleton->GetRefLocalPoses(TEXT("Default")), F.Skeleton->GetReferenceSkeleton().GetRefBonePose()));
 const auto Request = F.Request();
 for (const bool bInitiallyDirty : {false, true})
  for (const TCHAR* Text : {TEXT("NamedPose_7"), TEXT("None"), TEXT("Default"), TEXT("UnknownName_12"), TEXT("retainedsource_9"), TEXT("Name_01")})
  {
   F.Sequence->GetOutermost()->SetDirtyFlag(bInitiallyDirty); const FSnapshot Before(F);
   auto I = F.Intent(Text); if (bInitiallyDirty) I.Phase = EAssetDocumentRuntimeRequestPhase::Repair;
   FAssetDocumentPreparedMutationBatch B;
   if (!TestTrue(TEXT("known/None/Default/unknown/numbered native literal prepares"), PrepareAssetDocumentAnimationMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&I, 1), B).bValid)) return false;
   if (!TestEqual(TEXT("one bounded contribution"), B.Contributions.Num(), 1)) return false;
   TestTrue(TEXT("one owned name surface"), B.Contributions[0].OwnedSurfaceIds == TArray<FString>{Request.TargetAssetPath + TEXT("::RetargetSource")});
   TestTrue(TEXT("prepared parent inverse exists"), B.Contributions[0].InitialAction.IsValid());
   Before.Check(*this, F);
   if (!TestTrue(TEXT("fixed mutation capture opener exists"), bool(B.OpenMutationCapture))) return false;
   TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
   auto Lease = B.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
   TestTrue(TEXT("unchanged native input opens a capture"), Lease && Lease->IsValid());
   if (Lease) Lease->Close(); Before.Check(*this, F);
  }
 F.Sequence->GetOutermost()->SetDirtyFlag(false);
 const auto Valid = F.Intent(TEXT("NamedPose_7"));
 FString EmbeddedNull = TEXT("Name_Suffix"); EmbeddedNull[4] = 0;
 TestEqual(TEXT("embedded NUL fixture retains its full length"), EmbeddedNull.Len(), 11);
 TestEqual(TEXT("embedded NUL fixture contains the rejected character"), EmbeddedNull[4], TCHAR(0));
 FString TooLong; for (int32 I = 0; I < NAME_SIZE; ++I) TooLong.AppendChar(TEXT('x'));
 for (int32 Case = 0; Case < 14; ++Case)
 {
  auto Bad = Valid; auto R = Request;
  if (Case == 0) Bad.Intent = NameValue(TEXT(""));
  if (Case == 1) Bad.Intent = NameValue(EmbeddedNull);
  if (Case == 2) Bad.Intent = NameValue(TooLong);
  if (Case == 3) Bad.Intent.Elements[0] = FAssetDocumentRuntimeValueDto();
  if (Case == 4) { Bad.Intent.ObjectKeys.Add(TEXT("property")); Bad.Intent.Elements.Add(String(TEXT("RetargetSource"))); }
  if (Case == 5) Bad.Intent.ObjectKeys[0] = TEXT("RetargetSource");
  if (Case == 6) Bad.InputSchemaId += TEXT(".drift");
  if (Case == 7) Bad.Requirement.RequiredByRouteId = TEXT("generic_property");
  if (Case == 8) Bad.Requirement.Version = 2;
  if (Case == 9) Bad.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == 10) R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
  if (Case == 11) R.TargetAssetPath = F.Skeleton->GetPathName();
  if (Case == 12) Bad.Intent.Kind = EAssetDocumentRuntimeValueKind::Array;
  if (Case == 13) { Bad.Intent.ObjectKeys.Add(TEXT("source_mesh_path")); Bad.Intent.Elements.Add(String(F.SourceMesh->GetPathName())); }
  const FSnapshot Before(F); FAssetDocumentPreparedMutationBatch Rejected;
  TestFalse(FString::Printf(TEXT("closed input shape rejects fallback and drift (case %d)"), Case), PrepareAssetDocumentAnimationMutationBatch(R, TConstArrayView<FAssetDocumentMutationIntent>(&Bad, 1), Rejected).bValid);
  TestTrue(FString::Printf(TEXT("rejected preparation publishes no contribution (case %d)"), Case), Rejected.Contributions.IsEmpty()); Before.Check(*this, F);
 }
 const auto OriginalMap = F.Skeleton->AnimRetargetSources;
 for (int32 Case = 0; Case < 7; ++Case)
 {
  const FSnapshot Before(F); FAssetDocumentPreparedMutationBatch B;
  if (!TestTrue(TEXT("full map snapshot prepares"), PrepareAssetDocumentAnimationMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&Valid, 1), B).bValid)) return false;
  // Fixture-only direct changes intentionally keep Skeleton GUID and dirty
  // state fixed, including an unrelated entry never selected by this intent.
  auto& Entry = F.Skeleton->AnimRetargetSources.FindChecked(TEXT("UnrelatedPose"));
  if (Case == 0) { const auto Saved = Entry; F.Skeleton->AnimRetargetSources.Add(TEXT("AddedPose"), Saved); }
  if (Case == 1) F.Skeleton->AnimRetargetSources.Remove(TEXT("UnrelatedPose"));
  if (Case == 2)
  { const auto Saved = Entry; F.Skeleton->AnimRetargetSources.Remove(TEXT("UnrelatedPose")); F.Skeleton->AnimRetargetSources.Add(FName(TEXT("UNRELATEDPOSE")), Saved); }
  if (Case == 3) Entry.PoseName = TEXT("ChangedMetadata");
  if (Case == 4) Entry.ReferencePose[0].SetRotation(FQuat(0., 0., 0., -1.));
  if (Case == 5) Entry.ReferencePose.SetNum(1);
  if (Case == 6) Entry.SourceReferenceMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(F.Root + TEXT("/OtherUnloaded.OtherUnloaded")));
  TestEqual(TEXT("direct map change does not update Skeleton GUID"), F.Skeleton->GetGuid(), Before.SkeletonGuid);
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  auto Lease = B.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
  TestFalse(FString::Printf(TEXT("whole-map drift rejects before any name write (case %d)"), Case), Lease.IsValid());
  if (Lease) Lease->Close();
  Before.Check(*this, F, false);
  F.Skeleton->AnimRetargetSources = OriginalMap; Before.Check(*this, F);
 }
 {
  const FSnapshot Before(F);
  F.Skeleton->AnimRetargetSources.FindChecked(TEXT("NamedPose_7")).ReferencePose.SetNum(1);
  FAssetDocumentPreparedMutationBatch Rejected;
  TestFalse(TEXT("selected named pose must match complete Skeleton shape"), PrepareAssetDocumentAnimationMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&Valid, 1), Rejected).bValid);
  TestTrue(TEXT("malformed selected pose has zero contribution"), Rejected.Contributions.IsEmpty());
  F.Skeleton->AnimRetargetSources = OriginalMap; Before.Check(*this, F);
 }
 const auto OriginalStoredPose = F.Sequence->RetargetSourceAssetReferencePose;
 for (int32 Case = 0; Case < 2; ++Case)
 {
  // A named source masks this array in the getter, but ordinary stock PreSave
  // still regenerates it. Preparation must refuse, never repair, that state.
  if (Case == 0) F.Sequence->RetargetSourceAssetReferencePose[1].SetTranslation(FVector(13. + 1.e-12, 0., 0.));
  if (Case == 1) F.Sequence->RetargetSourceAssetReferencePose[0].SetRotation(FQuat(0., 0., 0., -1.));
  const FSnapshot Malformed(F); FAssetDocumentPreparedMutationBatch Rejected;
  const auto R = PrepareAssetDocumentAnimationMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&Valid, 1), Rejected);
  TestFalse(TEXT("hidden stored pose must already be exactly save stable"), R.bValid);
  TestTrue(TEXT("save-stability rejection has a specific diagnostic"), R.Diagnostics.ContainsByPredicate([](const auto& Diagnostic)
   { return Diagnostic.Code == TEXT("AnimationProvider.RetargetSourceName.PreimageUnrepresentable"); }));
  TestTrue(TEXT("unrepresentable retained pose publishes no contribution"), Rejected.Contributions.IsEmpty());
  Malformed.Check(*this, F); F.Sequence->RetargetSourceAssetReferencePose = OriginalStoredPose;
 }
 return !HasAnyErrors();
}
#endif
