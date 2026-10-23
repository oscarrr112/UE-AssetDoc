// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "Provider/Sequence/AssetDocumentAnimationSequenceAdditiveCompletion.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceAdditiveCompletionState.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentAnimationMutationPreparation.h"
#include "Tests/AssetDocumentAnimationSequenceAdditiveTestUserData.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Engine/AssetUserData.h"
#include "Misc/AutomationTest.h"

namespace AssetDocumentSequenceAdditiveContractTests
{
using namespace AssetDocumentSequenceAdditive;
FAssetDocumentRuntimeValueDto String(const TCHAR* V)
{ FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::String; R.StringValue = V; return R; }
FAssetDocumentRuntimeValueDto Integer(int64 V)
{ FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::Integer; R.IntegerValue = V; return R; }
FAssetDocumentRuntimeValueDto Object(TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values)
{ FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::Object; R.ObjectKeys = MoveTemp(Keys); R.Elements = MoveTemp(Values); return R; }
struct FFixture
{
 TStrongObjectPtr<USkeleton> Skeleton;
 TStrongObjectPtr<UAnimSequence> Sequence;
 FFixture()
 {
  const FString Root = TEXT("/Game/AssetDocumentSequenceAdditiveContract/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  { FReferenceSkeletonModifier Modifier(Skeleton.Get()); Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); }
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  Sequence->SetSkeleton(Skeleton.Get());
  auto& C = Sequence->GetController();
  C.OpenBracket(FText::FromString(TEXT("Sequence additive contract fixture")), false);
  C.InitializeModel(); C.SetFrameRate(FFrameRate(24, 1), false); C.SetNumberOfFrames(FFrameNumber(12), false);
  C.AddBoneCurve(TEXT("root"), false);
  TArray<FVector3f> P, S; TArray<FQuat4f> Q;
  P.Init(FVector3f::ZeroVector, 13); S.Init(FVector3f::OneVector, 13); Q.Init(FQuat4f::Identity, 13);
  C.SetBoneTrackKeys(TEXT("root"), P, Q, S, false); C.NotifyPopulated(); C.CloseBracket(false);
  Sequence->WaitOnExistingCompression();
  Sequence->GetOutermost()->SetDirtyFlag(false); Skeleton->GetOutermost()->SetDirtyFlag(false);
 }
 ~FFixture()
 {
  Sequence->WaitOnExistingCompression();
  Sequence->ClearFlags(RF_Public | RF_Standalone); Sequence->GetOutermost()->SetDirtyFlag(false);
  Skeleton->ClearFlags(RF_Public | RF_Standalone); Skeleton->GetOutermost()->SetDirtyFlag(false);
 }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 1;
  R.Operation = EAssetDocumentRuntimeOperation::Apply; R.TargetAssetPath = Sequence->GetPathName();
  R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence"); return R;
 }
 FAssetDocumentMutationIntent Intent(FAssetDocumentRuntimeValueDto Fields) const
 {
  FAssetDocumentMutationIntent I; I.StepIndex = 0; I.Requirement = {Primitive, 2, TEXT("animation_profile")};
  I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.InputSchemaId = Schema;
  I.Intent = Object({TEXT("completion_kind"), TEXT("additive")}, {String(TEXT("additive_root")), MoveTemp(Fields)}); return I;
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequenceAdditiveContractTest,
 "UEAssetDoc.AnimationProvider.SequenceAdditive.Contract",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSequenceAdditiveContractTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceAdditiveContractTests;
 const auto D = MakeAnimationSequenceAdditiveCompletionDescriptor();
 TestTrue(TEXT("exact additive-only descriptor admission"), ValidateAnimationAdmissionEvidence(D).bValid);
 TestTrue(TEXT("exact native authority shape"), IsSequenceAdditiveCompletionAdmissionShape(D));
 TestEqual(TEXT("canonical runtime handle name"), D.HandleArgumentSchemas[0].Name, FString(TEXT("sequence")));
 for (int32 Case = 0; Case < 6; ++Case)
 {
  auto Bad = D;
  if (Case == 0) Bad.HandleArgumentSchemas[0].Name = TEXT("asset");
  if (Case == 1) Bad.HandleArgumentSchemas[0].RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
  if (Case == 2) Bad.AllowedPhases.Add(EAssetDocumentRuntimeRequestPhase::Verify);
  if (Case == 3) Bad.Admission.ExportedAuthoritativeApi = TEXT("RefreshCacheData");
  if (Case == 4) Bad.Admission.RollbackContractId.Reset();
  if (Case == 5) Bad.InputSchemaId += TEXT(".drift");
  TestFalse(TEXT("descriptor drift cannot widen authority"), ValidateAnimationAdmissionEvidence(Bad).bValid);
 }
 FFixture F; const auto Request = F.Request(); const auto Before = FAuthorState::Read(*F.Sequence);
 TestTrue(TEXT("exact existing canonical fixture"), CanonicalSequence(F.Sequence.Get()));
 TestFalse(TEXT("stock none is not valid additive"), F.Sequence->IsValidAdditive());
 TestTrue(TEXT("inactive anim-frame/null tuple is preserved rather than misclassified as active"),
  StableAuthorState(*F.Sequence, FAuthorState{AAT_None, ABPT_AnimFrame, 0, nullptr}));
 const int32 Sampled = F.Sequence->GetNumberOfSampledKeys();
 const int32 Keys = Sampled > 0 ? Sampled : F.Sequence->GetDataModel()->GetNumberOfKeys();
 auto Valid = F.Intent(Object({TEXT("AdditiveAnimType"), TEXT("RefPoseType"), TEXT("RefFrameIndex")},
  {String(TEXT("AAT_LocalSpaceBase")), String(TEXT("ABPT_LocalAnimFrame")), Integer(Keys - 1)}));
 FAssetDocumentPreparedMutationBatch B;
 TestTrue(TEXT("last local sampled key is admitted"), PrepareAssetDocumentAnimationMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&Valid, 1), B).bValid);
 TestTrue(TEXT("preparation itself preserves all authored values"), Before.Matches(*F.Sequence));
 TestFalse(TEXT("preparation itself preserves clean package"), F.Sequence->GetOutermost()->IsDirty());
 for (int32 Case = 0; Case < 9; ++Case)
 {
  auto Bad = Valid; auto R = Request;
  if (Case == 0) Bad.Intent.Elements[1].Elements[2] = Integer(Keys);
  if (Case == 1) Bad.Intent.Elements[1].Elements[2] = Integer(-1);
  if (Case == 2) Bad.Intent.Elements[1].Elements[1] = String(TEXT("ABPT_None"));
  if (Case == 3) Bad.Intent.Elements[1].Elements[0] = String(TEXT("UnknownAdditive"));
  if (Case == 4) Bad.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == 5) R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
  if (Case == 6) Bad.Intent.Elements[0] = String(TEXT("compression"));
  if (Case == 7) Bad.Intent.Elements[1] = FAssetDocumentRuntimeValueDto();
  if (Case == 8) Bad.Intent.Elements[1].ObjectKeys.Add(TEXT("OtherField"));
  FAssetDocumentPreparedMutationBatch Rejected;
  TestFalse(TEXT("invalid tuple/context/null alias is rejected during preparation"), PrepareAssetDocumentAnimationMutationBatch(R, TConstArrayView<FAssetDocumentMutationIntent>(&Bad, 1), Rejected).bValid);
  TestTrue(TEXT("rejection has no partial prepared contribution"), Rejected.Contributions.IsEmpty());
  TestTrue(TEXT("rejection leaves exact author state"), Before.Matches(*F.Sequence));
  TestFalse(TEXT("rejection leaves original clean state"), F.Sequence->GetOutermost()->IsDirty());
 }
 FFixture Foreign; const auto ReferenceBefore=FAuthorState::Read(*Foreign.Sequence);
 auto ForeignReference=F.Intent(Object({TEXT("AdditiveAnimType"),TEXT("RefPoseType"),TEXT("RefPoseSeq")},
  {String(TEXT("AAT_LocalSpaceBase")),String(TEXT("ABPT_AnimScaled")),String(*Foreign.Sequence->GetPathName())}));
 FAssetDocumentPreparedMutationBatch RejectedReference;
 TestFalse(TEXT("foreign Skeleton reference rejected before mutation"),PrepareAssetDocumentAnimationMutationBatch(Request,TConstArrayView<FAssetDocumentMutationIntent>(&ForeignReference,1),RejectedReference).bValid);
 TestTrue(TEXT("foreign reference rejection has no contribution"),RejectedReference.Contributions.IsEmpty());
 auto Data = NewObject<UAssetDocumentSequenceAdditiveTestUserData>(F.Sequence.Get()); F.Sequence->AddAssetUserData(Data);
 for (const bool bDirty : {false,true})
 {
  F.Sequence->GetOutermost()->SetDirtyFlag(bDirty);
  FAssetDocumentPreparedMutationBatch Callback;
  const auto Rejected=PrepareAssetDocumentAnimationMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&Valid, 1), Callback);
  TestFalse(TEXT("unowned asset-user-data callbacks fail before mutation"),Rejected.bValid);
  TestTrue(TEXT("callback ownership diagnostic is explicit"),Rejected.Diagnostics.ContainsByPredicate([](const auto& D){return D.Code==TEXT("AnimationProvider.SequenceAdditive.CallbackOwnershipUnavailable");}));
  TestTrue(TEXT("callback rejection has no footprint contribution"),Callback.Contributions.IsEmpty());
  TestEqual(TEXT("rejected callback was not invoked"),Data->OwnerCallbackCalls,0);
  TestTrue(TEXT("callback rejection preserves target tuple"),Before.Matches(*F.Sequence));
  TestEqual(TEXT("callback rejection preserves original dirty state"),F.Sequence->GetOutermost()->IsDirty(),bDirty);
  TestTrue(TEXT("callback rejection preserves foreign reference tuple"),ReferenceBefore.Matches(*Foreign.Sequence));
  TestFalse(TEXT("callback rejection preserves reference clean state"),Foreign.Sequence->GetOutermost()->IsDirty());
  auto Noop=F.Intent(Object({}, {})); FAssetDocumentPreparedMutationBatch ReadOnly;
  TestTrue(TEXT("genuine notification-free noop permits attached data"),PrepareAssetDocumentAnimationMutationBatch(Request,TConstArrayView<FAssetDocumentMutationIntent>(&Noop,1),ReadOnly).bValid);
  TestEqual(TEXT("noop preparation does not invoke callback"),Data->OwnerCallbackCalls,0);
 }
 F.Sequence->RemoveUserDataOfClass(UAssetDocumentSequenceAdditiveTestUserData::StaticClass());
 return !HasAnyErrors();
}
#endif
