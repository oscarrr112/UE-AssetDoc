// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompression.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompressionDefaultInputs.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompressionDefaultInputsTestHooks.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentAnimationMutationPreparation.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimCompress_BitwiseCompressOnly.h"
#include "Animation/AnimCurveCompressionCodec_UniformlySampled.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "AnimationUtils.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

// Admission and prepared readonly observations only. Actual invocation,
// Core rollback and distinct-process save/fresh-read are integration scopes.
namespace AssetDocumentSequenceCompressionContractTests
{
constexpr const TCHAR* Primitive = TEXT("animation.sequence.compression.settings.set.v1");
constexpr const TCHAR* Schema = TEXT("animation.sequence.compression.settings.set.v1.input.v1");
FAssetDocumentRuntimeValueDto String(const FString& S)
{ FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::String; V.StringValue = S; return V; }
FAssetDocumentRuntimeValueDto Number(double N)
{ FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Number; V.NumberValue = N; return V; }
FAssetDocumentRuntimeValueDto BooleanDto(bool B)
{ FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Boolean; V.BooleanValue = B; return V; }
FAssetDocumentRuntimeValueDto Projection(TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values)
{
 FAssetDocumentRuntimeValueDto Inner, Outer; Inner.Kind = Outer.Kind = EAssetDocumentRuntimeValueKind::Object;
 Inner.ObjectKeys = MoveTemp(Keys); Inner.Elements = MoveTemp(Values); Outer.ObjectKeys = {TEXT("fields")}; Outer.Elements.Add(MoveTemp(Inner)); return Outer;
}
bool ExactPose(const TArray<FTransform>& A, const TArray<FTransform>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I) if (A[I].GetTranslation() != B[I].GetTranslation() || A[I].GetRotation() != B[I].GetRotation()
  || A[I].GetScale3D() != B[I].GetScale3D()) return false;
 return true;
}
struct FFixture
{
 FString Root = TEXT("/Game/AssetDocumentSequenceCompressionContract/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<USkeleton> Skeleton; TStrongObjectPtr<USkeletalMesh> Mesh; TStrongObjectPtr<UAnimSequence> Sequence;
 TStrongObjectPtr<UAnimBoneCompressionSettings> Bone[2]; TStrongObjectPtr<UAnimCurveCompressionSettings> Curve[2];
 TStrongObjectPtr<UVariableFrameStrippingSettings> Variable[2];
 bool Setup()
 {
  // Only newly constructed fixture assets and their owned codecs are authored.
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  { FReferenceSkeletonModifier B(Skeleton.Get()); B.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); }
  Mesh.Reset(NewObject<USkeletalMesh>(CreatePackage(*(Root + TEXT("/SourceMesh"))), TEXT("SourceMesh"), RF_Public | RF_Standalone));
  Mesh->SetSkeleton(Skeleton.Get());
  { FReferenceSkeletonModifier B(Mesh->GetRefSkeleton(), Skeleton.Get()); B.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform(FVector(7., 0., 0.))); }
  for (int32 I = 0; I < 2; ++I)
  {
   Bone[I].Reset(NewObject<UAnimBoneCompressionSettings>(CreatePackage(*FString::Printf(TEXT("%s/Bone%d"), *Root, I)), TEXT("Settings"), RF_Public | RF_Standalone));
   Bone[I]->Codecs.Add(NewObject<UAnimCompress_BitwiseCompressOnly>(Bone[I].Get(), TEXT("Codec"))); Bone[I]->ErrorThreshold = 0.1f + I;
   Curve[I].Reset(NewObject<UAnimCurveCompressionSettings>(CreatePackage(*FString::Printf(TEXT("%s/Curve%d"), *Root, I)), TEXT("Settings"), RF_Public | RF_Standalone));
   Curve[I]->Codec = NewObject<UAnimCurveCompressionCodec_UniformlySampled>(Curve[I].Get(), TEXT("Codec"));
   Variable[I].Reset(NewObject<UVariableFrameStrippingSettings>(CreatePackage(*FString::Printf(TEXT("%s/Variable%d"), *Root, I)), TEXT("Settings"), RF_Public | RF_Standalone));
   Variable[I]->UseVariableFrameStripping.Default = false; Variable[I]->FrameStrippingRate.Default = 2 + I;
  }
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  Sequence->SetSkeleton(Skeleton.Get()); Sequence->BoneCompressionSettings = Bone[0].Get(); Sequence->CurveCompressionSettings = Curve[0].Get();
  Sequence->VariableFrameStrippingSettings = Variable[0].Get(); Sequence->CompressionErrorThresholdScale = 1.f; Sequence->bDoNotOverrideCompression = false;
  Sequence->RateScale = 1.25f; auto& C = Sequence->GetController(); C.OpenBracket(FText::FromString(TEXT("Compression contract fixture")), false);
  C.InitializeModel(); C.SetFrameRate(FFrameRate(24, 1), false); C.SetNumberOfFrames(FFrameNumber(12), false);
  const bool Track = C.AddBoneCurve(TEXT("root"), false); TArray<FVector3f> P, S; TArray<FQuat4f> Q;
  P.Init(FVector3f::ZeroVector, 13); S.Init(FVector3f::OneVector, 13); Q.Init(FQuat4f::Identity, 13);
  const bool Keys = Track && C.SetBoneTrackKeys(TEXT("root"), P, Q, S, false); C.NotifyPopulated(); C.CloseBracket(false);
  Sequence->WaitOnExistingCompression(); Sequence->SetRetargetSourceAsset(Mesh.Get()); Sequence->UpdateRetargetSourceAssetData();
  Sequence->RetargetSource = TEXT("RetainedName_7"); Sequence->ClearAllCachedCookedPlatformData();
  Sequence->CacheDerivedDataForCurrentPlatform(); Sequence->WaitOnExistingCompression();
  for (UObject* O : Assets()) O->GetOutermost()->SetDirtyFlag(false);
  return Keys && Bone[0]->AreSettingsValid() && Bone[1]->AreSettingsValid() && Curve[0]->AreSettingsValid() && Curve[1]->AreSettingsValid();
 }
 TArray<UObject*> Assets() const
 {
  TArray<UObject*> A{Skeleton.Get(), Mesh.Get(), Sequence.Get()};
  for (int32 I = 0; I < 2; ++I) { A.Add(Bone[I].Get()); A.Add(Curve[I].Get()); A.Add(Variable[I].Get()); } return A;
 }
 ~FFixture()
 { if (Sequence.IsValid()) Sequence->WaitOnExistingCompression(); for (UObject* O : Assets()) if (IsValid(O)) { O->ClearFlags(RF_Public | RF_Standalone); O->GetOutermost()->SetDirtyFlag(false); } }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 1; R.Operation = EAssetDocumentRuntimeOperation::Apply;
  R.TargetAssetPath = Sequence->GetPathName(); R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence"); return R;
 }
 FAssetDocumentMutationIntent Intent(FAssetDocumentRuntimeValueDto V) const
 {
  FAssetDocumentMutationIntent I; I.StepIndex = 0; I.Requirement = {Primitive, 1, TEXT("animation_profile")};
  I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.InputSchemaId = Schema; I.Intent = MoveTemp(V); return I;
 }
 FAssetDocumentRuntimeValueDto All() const
 {
  return Projection({TEXT("CompressionErrorThresholdScale"), TEXT("BoneCompressionSettings"), TEXT("CurveCompressionSettings"),
   TEXT("VariableFrameStrippingSettings"), TEXT("bDoNotOverrideCompression")},
   {Number(2.5), String(Bone[1]->GetPathName()), String(Curve[1]->GetPathName()), String(Variable[1]->GetPathName()), BooleanDto(true)});
 }
};
struct FSnapshot
{
 float Scale, Rate; bool Keep; UAnimBoneCompressionSettings* Bone; UAnimCurveCompressionSettings* Curve; UVariableFrameStrippingSettings* Variable;
 const IAnimationDataModel* Model; FGuid ModelGuid, SkeletonGuid; FName Name, EffectiveName;
 FSoftObjectPath Source; TArray<FTransform> Pose, Effective; bool TargetDirty, SkeletonDirty, MeshDirty;
 explicit FSnapshot(const FFixture& F) : Scale(F.Sequence->CompressionErrorThresholdScale), Rate(F.Sequence->RateScale), Keep(F.Sequence->bDoNotOverrideCompression),
  Bone(F.Sequence->BoneCompressionSettings.Get()), Curve(F.Sequence->CurveCompressionSettings.Get()), Variable(F.Sequence->VariableFrameStrippingSettings.Get()),
  Model(F.Sequence->GetDataModel()), ModelGuid(Model->GenerateGuid()), SkeletonGuid(F.Sequence->GetSkeletonGuid()), Name(F.Sequence->RetargetSource),
  EffectiveName(F.Sequence->GetRetargetTransformsSourceName()), Source(F.Sequence->GetRetargetSourceAsset().ToSoftObjectPath()),
  Pose(F.Sequence->RetargetSourceAssetReferencePose), Effective(F.Sequence->GetRetargetTransforms()), TargetDirty(F.Sequence->GetOutermost()->IsDirty()),
  SkeletonDirty(F.Skeleton->GetOutermost()->IsDirty()), MeshDirty(F.Mesh->GetOutermost()->IsDirty()) {}
 void Check(FAutomationTestBase& T, const FFixture& F, bool CheckRate = true) const
 {
  T.TestEqual(TEXT("preparation does not set scale"), F.Sequence->CompressionErrorThresholdScale, Scale);
  T.TestEqual(TEXT("preparation does not set keep flag"), bool(F.Sequence->bDoNotOverrideCompression), Keep);
  T.TestEqual(TEXT("preparation preserves original bone settings identity"), F.Sequence->BoneCompressionSettings.Get(), Bone);
  T.TestEqual(TEXT("preparation preserves original curve settings identity"), F.Sequence->CurveCompressionSettings.Get(), Curve);
  T.TestEqual(TEXT("preparation preserves original variable settings identity"), F.Sequence->VariableFrameStrippingSettings.Get(), Variable);
  T.TestTrue(TEXT("preparation preserves exact retained name"), F.Sequence->RetargetSource.IsEqual(Name, ENameCase::CaseSensitive, true));
  T.TestTrue(TEXT("preparation preserves exact effective source name"), F.Sequence->GetRetargetTransformsSourceName().IsEqual(EffectiveName, ENameCase::CaseSensitive, true));
  T.TestEqual(TEXT("preparation preserves retained source reference"), F.Sequence->GetRetargetSourceAsset().ToSoftObjectPath(), Source);
  T.TestTrue(TEXT("preparation preserves entire hidden stored pose"), ExactPose(F.Sequence->RetargetSourceAssetReferencePose, Pose));
  T.TestTrue(TEXT("preparation preserves native retarget pose"), ExactPose(F.Sequence->GetRetargetTransforms(), Effective));
  T.TestTrue(TEXT("preparation preserves model identity"), F.Sequence->GetDataModel() == Model);
  T.TestEqual(TEXT("preparation preserves model content"), F.Sequence->GetDataModel()->GenerateGuid(), ModelGuid);
  T.TestEqual(TEXT("preparation preserves skeleton GUID"), F.Sequence->GetSkeletonGuid(), SkeletonGuid);
  if (CheckRate) T.TestEqual(TEXT("preparation preserves unrelated scalar"), F.Sequence->RateScale, Rate);
  T.TestEqual(TEXT("preparation preserves target dirty preimage"), F.Sequence->GetOutermost()->IsDirty(), TargetDirty);
  T.TestEqual(TEXT("preparation preserves skeleton package"), F.Skeleton->GetOutermost()->IsDirty(), SkeletonDirty);
  T.TestEqual(TEXT("preparation preserves source mesh package"), F.Mesh->GetOutermost()->IsDirty(), MeshDirty);
 }
};
FAssetDocumentContractValidationResult Prepare(const FAssetDocumentRuntimeRequestDto& R, const FAssetDocumentMutationIntent& I, FAssetDocumentPreparedMutationBatch& B)
{ return PrepareAssetDocumentAnimationMutationBatch(R, TConstArrayView<FAssetDocumentMutationIntent>(&I, 1), B); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequenceCompressionContractTest,
 "UEAssetDoc.AnimationProvider.SequenceCompression.Contract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSequenceCompressionContractTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceCompressionContractTests;
 const auto D = MakeAnimationSequenceCompressionDescriptor();
 TestTrue(TEXT("exact supplied settings leaf is admitted"), ValidateAnimationAdmissionEvidence(D).bValid);
 TestTrue(TEXT("exact closed compression admission shape"), IsSequenceCompressionAdmissionShape(D));
 TestEqual(TEXT("frozen primitive"), D.PrimitiveId, FString(Primitive)); TestEqual(TEXT("frozen version"), D.Version, uint32(1));
 TestEqual(TEXT("frozen schema"), D.InputSchemaId, FString(Schema)); TestEqual(TEXT("Animation owns leaf"), D.ProviderId, FString(TEXT("animation")));
 TestEqual(TEXT("fixed mutating access"), D.Access, EAssetDocumentPrimitiveAccess::Mutating); TestTrue(TEXT("parent transaction is required"), D.bRequiresActiveTransaction);
 if (!TestEqual(TEXT("one target and three supplied readonly roles"), D.HandleArgumentSchemas.Num(), 4)) return false;
 TestEqual(TEXT("target role"), D.HandleArgumentSchemas[0].Name, FString(TEXT("sequence")));
 TestEqual(TEXT("target mutable access"), D.HandleArgumentSchemas[0].RequiredAccess, EAssetDocumentHandleAccess::Mutable);
 TestTrue(TEXT("target required"), D.HandleArgumentSchemas[0].bRequired);
 const TCHAR* Roles[] = {TEXT("bone_compression_settings"), TEXT("curve_compression_settings"), TEXT("frame_stripping_settings")};
 for (int32 I = 0; I < 3; ++I)
 { TestEqual(TEXT("exact settings role"), D.HandleArgumentSchemas[I + 1].Name, FString(Roles[I]));
   TestEqual(TEXT("supplied settings readonly"), D.HandleArgumentSchemas[I + 1].RequiredAccess, EAssetDocumentHandleAccess::ReadOnly);
   TestFalse(TEXT("role occurrence is governed by the frozen fields projection"), D.HandleArgumentSchemas[I + 1].bRequired); }
 TestTrue(TEXT("no minted handle outputs"), D.HandleResultSchemas.IsEmpty()); TestTrue(TEXT("no raw lifecycle allowlist"), D.Admission.RawLifecycleAllowlistId.IsEmpty());
 for (int32 Case = 0; Case < 13; ++Case)
 {
  auto Bad = D;
  if (Case == 0) Bad.HandleArgumentSchemas[0].RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
  if (Case == 1) Bad.HandleArgumentSchemas[1].RequiredAccess = EAssetDocumentHandleAccess::Mutable;
  if (Case == 2) Bad.HandleArgumentSchemas[1].Name = TEXT("settings");
  if (Case == 3) Bad.HandleArgumentSchemas.Add({TEXT("generic_asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, false});
  if (Case == 4) Bad.AllowedPhases.Add(EAssetDocumentRuntimeRequestPhase::Verify);
  if (Case == 5) Bad.InputSchemaId += TEXT(".drift");
  if (Case == 6) Bad.Admission.ExportedAuthoritativeApi = TEXT("FProperty::CopyCompleteValue");
  if (Case == 7) Bad.Admission.RollbackContractId.Reset();
  if (Case == 8) Bad.Version = 2;
  if (Case == 9) Bad.Admission.RawLifecycleAllowlistId = TEXT("foreign-lifecycle");
  if (Case == 10) Bad.RequiredConsumerIds.Add(TEXT("generic_property"));
  if (Case == 11) Bad.Admission.PrimaryOperationId = TEXT("editor.property.set_values.v1");
  if (Case == 12) Bad.bRequiresActiveTransaction = false;
  TestFalse(FString::Printf(TEXT("closed descriptor rejects widened authority (case %d)"), Case), IsSequenceCompressionAdmissionShape(Bad));
  TestFalse(FString::Printf(TEXT("admission rejects widened authority (case %d)"), Case), ValidateAnimationAdmissionEvidence(Bad).bValid);
 }
 const auto Decisions = GetAssetDocumentAnimationDecisions(); const auto Descriptors = GetAssetDocumentAnimationPrimitiveDescriptors(); const auto Bindings = GetAssetDocumentAnimationPrimitiveBindings();
 TestTrue(TEXT("current decision/descriptor/binding bijection"), ValidateAnimationDescriptorBindingBijection(Decisions, Descriptors, Bindings).bValid);
 int32 DecisionCount = 0, DescriptorCount = 0, BindingCount = 0;
 for (const auto& R : Decisions) if (R.PrimitiveId == Primitive && R.Version == 1)
 { ++DecisionCount; TestEqual(TEXT("new exact row is admitted"), R.Disposition, EAssetDocumentAnimationDecisionDisposition::Admitted); }
 for (const auto& R : Descriptors) if (R.PrimitiveId == Primitive && R.Version == 1) { ++DescriptorCount; TestTrue(TEXT("catalog retains exact descriptor"), IsSequenceCompressionAdmissionShape(R)); }
 for (const auto& R : Bindings) if (R.PrimitiveId == Primitive && R.Version == 1) { ++BindingCount; TestTrue(TEXT("binding uses dedicated fixed leaf"), R.Handler == &InvokeAnimationSequenceCompression); }
 TestEqual(TEXT("one decision"), DecisionCount, 1); TestEqual(TEXT("one descriptor"), DescriptorCount, 1); TestEqual(TEXT("one binding"), BindingCount, 1);
 for (const TCHAR* Blocked : {TEXT("animation.sequence.retarget_source_asset.set.v1"), TEXT("animation.sequence.complete_edit.v1")})
 {
  int32 Count = 0; for (const auto& R : Decisions) if (R.PrimitiveId == Blocked && R.Version == 1)
  { ++Count; TestEqual(TEXT("predecessor blocker remains stable"), R.Disposition, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker);
    TestEqual(TEXT("predecessor blocker diagnostic is unchanged"), R.StableBlockerDiagnostic,
     FString(FString(Blocked).Contains(TEXT("retarget_source_asset")) ? TEXT("AD.PANIM.BLOCK.RETARGET_SOURCE_AUTHORITY") : TEXT("AD.PANIM.BLOCK.SEQUENCE_COMPLETION_AUTHORITY"))); }
  TestEqual(TEXT("one predecessor blocker"), Count, 1);
  for (const auto& R : Descriptors) TestFalse(TEXT("no predecessor descriptor widening"), R.PrimitiveId == Blocked && R.Version == 1);
  for (const auto& R : Bindings) TestFalse(TEXT("no predecessor binding widening"), R.PrimitiveId == Blocked && R.Version == 1);
 }
 FFixture F; if (!TestTrue(TEXT("test-owned settings and save-stable native fixture"), F.Setup())) return false;
 const auto Request = F.Request();
 TArray<FAssetDocumentRuntimeValueDto> ValidInputs = {
  F.All(), Projection({TEXT("CompressionErrorThresholdScale")}, {Number(2.5)}), Projection({TEXT("bDoNotOverrideCompression")}, {BooleanDto(true)}),
  Projection({TEXT("BoneCompressionSettings")}, {String(F.Bone[1]->GetPathName())}), Projection({TEXT("CurveCompressionSettings")}, {String(F.Curve[1]->GetPathName())}),
  Projection({TEXT("VariableFrameStrippingSettings")}, {String(F.Variable[1]->GetPathName())}), Projection({TEXT("BoneCompressionSettings")}, {String(F.Bone[0]->GetPathName())}),
  Projection({TEXT("CompressionErrorThresholdScale")}, {Number(0.)}), Projection({TEXT("CompressionErrorThresholdScale")}, {Number(-2.)})};
 for (const bool Dirty : {false, true}) for (int32 Case = 0; Case < ValidInputs.Num(); ++Case)
 {
  F.Sequence->GetOutermost()->SetDirtyFlag(Dirty); const FSnapshot Before(F); auto I = F.Intent(ValidInputs[Case]); if (Dirty) I.Phase = EAssetDocumentRuntimeRequestPhase::Repair;
  FAssetDocumentPreparedMutationBatch B;
  if (!TestTrue(FString::Printf(TEXT("minimal supplied projection prepares without positive-range policy (case %d)"), Case), Prepare(Request, I, B).bValid)) return false;
  if (!TestEqual(TEXT("one bounded contribution"), B.Contributions.Num(), 1)) return false;
  const TArray<FString> Expected = {Request.TargetAssetPath + TEXT("::Compression.BoneCompressionSettings"), Request.TargetAssetPath + TEXT("::Compression.CurveCompressionSettings"),
   Request.TargetAssetPath + TEXT("::Compression.VariableFrameStrippingSettings"), Request.TargetAssetPath + TEXT("::Compression.CompressionErrorThresholdScale"), Request.TargetAssetPath + TEXT("::Compression.bDoNotOverrideCompression")};
  TestTrue(TEXT("one fixed five-field tuple owns no foreign surface"), B.Contributions[0].OwnedSurfaceIds == Expected);
  TestTrue(TEXT("prepared parent inverse exists"), B.Contributions[0].InitialAction.IsValid()); Before.Check(*this, F);
  if (!TestTrue(TEXT("capture opener exists"), bool(B.OpenMutationCapture))) return false;
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics; auto Capture = B.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
  TestTrue(TEXT("unchanged readonly inputs open capture"), Capture && Capture->IsValid()); if (Capture) Capture->Close(); Before.Check(*this, F);
 }
 F.Sequence->GetOutermost()->SetDirtyFlag(false); const auto Valid = F.Intent(F.All());
 FString Embedded = F.Bone[1]->GetPathName(); Embedded[5] = 0;
 for (int32 Case = 0; Case < 20; ++Case)
 {
  auto Bad = Valid; auto R = Request;
  if (Case == 0) Bad.Intent = Projection({}, {});
  if (Case == 1) Bad.Intent = Projection({TEXT("BoneCompressionSettings")}, {FAssetDocumentRuntimeValueDto()});
  if (Case == 2) Bad.Intent = Projection({TEXT("CompressionErrorThresholdScale")}, {Number(1.e100)});
  if (Case == 3) Bad.Intent = Projection({TEXT("CompressionErrorThresholdScale")}, {Number(1.e-100)});
  if (Case == 4) Bad.Intent = Projection({TEXT("CompressionErrorThresholdScale")}, {Number(std::numeric_limits<double>::quiet_NaN())});
  if (Case == 5) Bad.Intent = Projection({TEXT("BoneCompressionSettings")}, {String(Embedded)});
  if (Case == 6) Bad.Intent = Projection({TEXT("BoneCompressionSettings")}, {String(F.Root + TEXT("/NeverLoaded.NeverLoaded"))});
  if (Case == 7) Bad.Intent = Projection({TEXT("BoneCompressionSettings")}, {String(F.Curve[1]->GetPathName())});
  if (Case == 8) Bad.Intent = Projection({TEXT("BoneCompressionSettings")}, {String(F.Bone[1]->GetPathName().ToLower())});
  if (Case == 9) Bad.Intent = Projection({TEXT("bDoNotOverrideCompression")}, {String(TEXT("true"))});
  if (Case == 10) { auto V = Number(1.); V.Kind = EAssetDocumentRuntimeValueKind::Integer; V.NumberValue = 0.; V.IntegerValue = 1; Bad.Intent = Projection({TEXT("CompressionErrorThresholdScale")}, {V}); }
  if (Case == 11) Bad.Intent = Projection({TEXT("BoneCompressionSettings"), TEXT("null_policy")}, {String(F.Bone[1]->GetPathName()), String(TEXT("default"))});
  if (Case == 12) Bad.Intent.Elements[0].ObjectKeys[0] = TEXT("compressionerrorthresholdscale");
  if (Case == 13) Bad.Intent.ObjectKeys[0] = TEXT("Fields");
  if (Case == 14) Bad.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == 15) R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
  if (Case == 16) R.TargetAssetPath = F.Skeleton->GetPathName();
  if (Case == 17) Bad.InputSchemaId += TEXT(".drift");
  if (Case == 18) Bad.Requirement.RequiredByRouteId = TEXT("generic_property");
  if (Case == 19) Bad.Intent = Projection({TEXT("CompressionErrorThresholdScale"), TEXT("CompressionErrorThresholdScale")}, {Number(2.5), Number(2.5)});
  const FSnapshot Before(F); FAssetDocumentPreparedMutationBatch Rejected;
  TestFalse(FString::Printf(TEXT("closed shape/representation rejects before capture (case %d)"), Case), Prepare(R, Bad, Rejected).bValid);
  TestTrue(TEXT("rejected preparation has zero contribution"), Rejected.Contributions.IsEmpty()); Before.Check(*this, F);
 }
 const auto Stored = F.Sequence->RetargetSourceAssetReferencePose;
 for (int32 Case = 0; Case < 2; ++Case)
 {
  if (Case == 0) F.Sequence->RetargetSourceAssetReferencePose[0].SetTranslation(FVector(7. + 1.e-12, 0., 0.));
  if (Case == 1) F.Sequence->RetargetSourceAssetReferencePose.Empty();
  const auto Corrupt = F.Sequence->RetargetSourceAssetReferencePose; FAssetDocumentPreparedMutationBatch Rejected;
  TestFalse(FString::Printf(TEXT("ordinary save-unstable pose is refused, not repaired (case %d)"), Case), Prepare(Request, Valid, Rejected).bValid);
  TestTrue(TEXT("invalid preimage publishes zero contribution"), Rejected.Contributions.IsEmpty());
  TestTrue(TEXT("refusal does not repair stored pose"), ExactPose(F.Sequence->RetargetSourceAssetReferencePose, Corrupt));
  F.Sequence->RetargetSourceAssetReferencePose = Stored;
 }
 for (int32 Case = 0; Case < 10; ++Case)
 {
  const FSnapshot Before(F); FAssetDocumentPreparedMutationBatch B; if (!TestTrue(TEXT("readonly old/new settings snapshot prepares"), Prepare(Request, Valid, B).bValid)) return false;
  const float Error0 = F.Bone[0]->ErrorThreshold, Error1 = F.Bone[1]->ErrorThreshold;
  auto* Codec0 = CastChecked<UAnimCurveCompressionCodec_UniformlySampled>(F.Curve[0]->Codec.Get());
  auto* Codec1 = CastChecked<UAnimCurveCompressionCodec_UniformlySampled>(F.Curve[1]->Codec.Get());
  const float Sample0 = Codec0->SampleRate, Sample1 = Codec1->SampleRate;
  const auto Use0 = F.Variable[0]->UseVariableFrameStripping; const auto Rate1 = F.Variable[1]->FrameStrippingRate;
  const auto Members1 = F.Bone[1]->Codecs; const bool Force1 = F.Bone[1]->bForceBelowThreshold;
  if (Case == 0) F.Bone[0]->ErrorThreshold += 0.5f;
  if (Case == 1) F.Bone[1]->ErrorThreshold += 0.5f;
  if (Case == 2) Codec0->SampleRate += 1.f;
  if (Case == 3) Codec1->SampleRate += 1.f;
  if (Case == 4) F.Variable[0]->UseVariableFrameStripping.Default = !Use0.Default;
  if (Case == 5) F.Variable[1]->FrameStrippingRate.PerPlatform.Add(TEXT("UnselectedPlatform"), 7);
  if (Case == 6) F.Bone[1]->Codecs.Add(nullptr);
  if (Case == 7) F.Bone[1]->bForceBelowThreshold = !Force1;
  if (Case == 8) F.Variable[1]->SetFlags(RF_NeedPostLoad);
  if (Case == 9) F.Curve[1]->Codec->SetFlags(RF_NeedPostLoad);
  TestFalse(TEXT("direct settings edit does not need dirty to be detected"), F.Bone[0]->GetOutermost()->IsDirty());
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics; auto Capture = B.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
  TestFalse(FString::Printf(TEXT("readonly content/closure/flags drift rejects capture (case %d)"), Case), Capture.IsValid());
  if (Capture) Capture->Close(); Before.Check(*this, F);
  F.Bone[0]->ErrorThreshold = Error0; F.Bone[1]->ErrorThreshold = Error1; Codec0->SampleRate = Sample0; Codec1->SampleRate = Sample1;
  F.Variable[0]->UseVariableFrameStripping = Use0; F.Variable[1]->FrameStrippingRate = Rate1; F.Bone[1]->Codecs = Members1; F.Bone[1]->bForceBelowThreshold = Force1;
  F.Variable[1]->ClearFlags(RF_NeedPostLoad); F.Curve[1]->Codec->ClearFlags(RF_NeedPostLoad);
 }
 {
  const FSnapshot Before(F); FAssetDocumentPreparedMutationBatch B; if (!Prepare(Request, Valid, B).bValid) return false;
  F.Sequence->RateScale = 9.f; TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics; auto Capture = B.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
  TestFalse(TEXT("retained author drift rejects before any compression setter"), Capture.IsValid()); if (Capture) Capture->Close(); Before.Check(*this, F, false);
  TestEqual(TEXT("rejection does not revert external authored drift"), F.Sequence->RateScale, 9.f); F.Sequence->RateScale = Before.Rate;
 }
 {
  const auto Saved = F.Bone[0]->Codecs; F.Bone[0]->Codecs.Empty(); FAssetDocumentPreparedMutationBatch Rejected;
  TestFalse(TEXT("invalid original settings refuse preparation instead of default replacement"), Prepare(Request, Valid, Rejected).bValid);
  TestTrue(TEXT("invalid original has zero contribution"), Rejected.Contributions.IsEmpty()); TestTrue(TEXT("invalid original remains untouched"), F.Bone[0]->Codecs.IsEmpty()); F.Bone[0]->Codecs = Saved;
 }
 for (int32 Case = 0; Case < 3; ++Case)
 {
  if (Case == 0) F.Sequence->BoneCompressionSettings = nullptr;
  if (Case == 1) F.Sequence->CurveCompressionSettings = nullptr;
  if (Case == 2) F.Sequence->VariableFrameStrippingSettings = nullptr;
  FAssetDocumentPreparedMutationBatch Rejected;
  TestFalse(FString::Printf(TEXT("null original settings cannot acquire implicit defaults (case %d)"), Case), Prepare(Request, Valid, Rejected).bValid);
  TestTrue(TEXT("null-original rejection has zero contribution"), Rejected.Contributions.IsEmpty());
  TestTrue(TEXT("null-original rejection does not materialize defaults"), Case == 0 ? F.Sequence->BoneCompressionSettings.Get() == nullptr
   : Case == 1 ? F.Sequence->CurveCompressionSettings.Get() == nullptr : F.Sequence->VariableFrameStrippingSettings.Get() == nullptr);
  F.Sequence->BoneCompressionSettings = F.Bone[0].Get(); F.Sequence->CurveCompressionSettings = F.Curve[0].Get(); F.Sequence->VariableFrameStrippingSettings = F.Variable[0].Get();
 }
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequenceCompressionDefaultInputsPreparationTest,
 "UEAssetDoc.AnimationProvider.SequenceCompression.DefaultInputsPreparation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSequenceCompressionDefaultInputsPreparationTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceCompressionContractTests;
 namespace Defaults = AssetDocumentSequenceCompressionDefaultInputs;
 using FHooks = AssetDocumentSequenceCompressionDefaultInputsTesting::FHooks;
 FFixture F; if (!TestTrue(TEXT("default preparation fixture uses only test-owned assets"), F.Setup())) return false;
 FHooks Hooks; TGuardValue<FHooks*> Observe(AssetDocumentSequenceCompressionDefaultInputsTesting::ActiveHooks, &Hooks);
 const auto Request = F.Request();
 for (const bool Dirty : {false, true}) for (int32 Mask = 1; Mask < 8; ++Mask)
 {
  F.Sequence->GetOutermost()->SetDirtyFlag(Dirty); const FSnapshot Before(F);
  TArray<Defaults::EKind> Selection;
  // Reverse input order must not change the fixed output/getter order.
  for (int32 I = 2; I >= 0; --I) if (Mask & (1 << I)) Selection.Add(static_cast<Defaults::EKind>(I));
  Hooks.Queries.Reset(); FAssetDocumentMutationIntent Supplied; FAssetDocumentPreparedMutationBatch Batch;
  const auto Phase = Dirty ? EAssetDocumentRuntimeRequestPhase::Repair : EAssetDocumentRuntimeRequestPhase::Apply;
  if (!TestTrue(TEXT("selected actual stock defaults prepare unchanged supplied-v1"), Defaults::Prepare(Request, Selection, 7, Phase, Supplied, Batch).bValid)) return false;
  TestEqualSensitive(TEXT("default preparation uses existing primitive"), Supplied.Requirement.PrimitiveId, Primitive);
  TestEqualSensitive(TEXT("default preparation uses existing schema"), Supplied.InputSchemaId, Schema);
  TestEqual(TEXT("private native step identity preserved"), Supplied.StepIndex, 7); TestEqual(TEXT("prepared phase preserved"), Supplied.Phase, Phase);
  if (!TestEqual(TEXT("one existing supplied contribution"), Batch.Contributions.Num(), 1)) return false;
  TestEqual(TEXT("only selected getters queried"), Hooks.Queries.Num(), Selection.Num());
  const auto& Fields = Supplied.Intent.Elements[0]; TestEqual(TEXT("only selected nonnull references supplied"), Fields.ObjectKeys.Num(), Selection.Num());
  int32 N = 0;
  for (int32 I = 0; I < 3; ++I)
  {
   if (!(Mask & (1 << I))) continue;
   UObject* Expected = I == 0 ? static_cast<UObject*>(FAnimationUtils::GetDefaultAnimationBoneCompressionSettings())
    : I == 1 ? static_cast<UObject*>(FAnimationUtils::GetDefaultAnimationCurveCompressionSettings())
    : static_cast<UObject*>(FAnimationUtils::GetDefaultVariableFrameStrippingSettings());
   const TCHAR* Name = I == 0 ? TEXT("BoneCompressionSettings") : I == 1 ? TEXT("CurveCompressionSettings") : TEXT("VariableFrameStrippingSettings");
   if (!TestNotNull(TEXT("stock expected default exists"), Expected)) return false;
   TestEqual(TEXT("selected getter identity/order"), Hooks.Queries[N], static_cast<Defaults::EKind>(I));
   TestEqualSensitive(TEXT("fixed selected native field"), Fields.ObjectKeys[N], Name);
   TestEqual(TEXT("default becomes supplied String, not null"), Fields.Elements[N].Kind, EAssetDocumentRuntimeValueKind::String);
   TestEqualSensitive(TEXT("actual Engine default path, no example or CDO substitute"), Fields.Elements[N].StringValue, Expected->GetPathName()); ++N;
  }
  Before.Check(*this, F); const auto QueriesBeforeOpen = Hooks.Queries;
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics; auto Capture = Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
  TestTrue(TEXT("frozen actual defaults open the existing capture"), Capture && Capture->IsValid()); if (Capture) Capture->Close();
  TestTrue(TEXT("capture never re-queries defaults"), Hooks.Queries == QueriesBeforeOpen); Before.Check(*this, F);
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequenceCompressionDefaultInputsFailuresTest,
 "UEAssetDoc.AnimationProvider.SequenceCompression.DefaultInputsFailures", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSequenceCompressionDefaultInputsFailuresTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceCompressionContractTests;
 namespace Defaults = AssetDocumentSequenceCompressionDefaultInputs;
 using FHooks = AssetDocumentSequenceCompressionDefaultInputsTesting::FHooks;
 FFixture F; if (!TestTrue(TEXT("default refusal fixture uses only test-owned assets"), F.Setup())) return false;
 FHooks Hooks; TGuardValue<FHooks*> Observe(AssetDocumentSequenceCompressionDefaultInputsTesting::ActiveHooks, &Hooks);
 const auto Request = F.Request();
 for (int32 Case = 0; Case < 12; ++Case)
 {
  const FSnapshot Before(F); auto R = Request; int32 Step = 0; auto Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  TArray<Defaults::EKind> Selection{Defaults::EKind::Bone}; Hooks.Queries.Reset(); Hooks.bForceNotReady = false;
  if (Case == 0) Selection.Reset();
  if (Case == 1) Selection.Add(Defaults::EKind::Bone);
  if (Case == 2) Selection = {static_cast<Defaults::EKind>(255)};
  if (Case == 3) Hooks.bForceNotReady = true;
  if (Case == 4) R.RequestId.Invalidate();
  if (Case == 5) R.ExpectedRuntimeRevision = 0;
  if (Case == 6) R.Operation = EAssetDocumentRuntimeOperation::Validate;
  if (Case == 7) R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
  if (Case == 8) R.TargetAssetPath = F.Skeleton->GetPathName();
  if (Case == 9) R.TargetAssetPath = F.Root + TEXT("/NeverLoaded.NeverLoaded");
  if (Case == 10) Step = INDEX_NONE;
  if (Case == 11) Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  FAssetDocumentMutationIntent Supplied; FAssetDocumentPreparedMutationBatch Batch;
  TestFalse(FString::Printf(TEXT("invalid selection/readiness/context refuses (case %d)"), Case), Defaults::Prepare(R, Selection, Step, Phase, Supplied, Batch).bValid);
  TestTrue(TEXT("rejected preparation calls zero default getters"), Hooks.Queries.IsEmpty());
  TestTrue(TEXT("rejected preparation publishes no batch or literal"), Batch.Contributions.IsEmpty() && !Batch.OpenMutationCapture && Supplied.Intent.ObjectKeys.IsEmpty());
  Before.Check(*this, F);
 }
 Hooks.bForceNotReady = false;
 // Only a test-owned target is edited. Real stock defaults remain readonly.
 FAssetDocumentMutationIntent Supplied; FAssetDocumentPreparedMutationBatch Batch;
 const TArray<Defaults::EKind> Selection{Defaults::EKind::Bone, Defaults::EKind::FrameStripping};
 if (!TestTrue(TEXT("actual default preparation precedes retained-target drift"), Defaults::Prepare(Request, Selection, 0, EAssetDocumentRuntimeRequestPhase::Apply, Supplied, Batch).bValid)) return false;
 const auto QueriesBeforeOpen = Hooks.Queries; const float Rate = F.Sequence->RateScale; F.Sequence->RateScale = 9.f;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics; auto Capture = Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
 TestFalse(TEXT("existing readonly target preimage gate rejects drift"), Capture.IsValid()); if (Capture) Capture->Close();
 TestEqual(TEXT("rejection does not revert external target edit"), F.Sequence->RateScale, 9.f);
 TestTrue(TEXT("rejected capture does not re-query defaults"), Hooks.Queries == QueriesBeforeOpen); F.Sequence->RateScale = Rate;
 return true;
}
#endif
