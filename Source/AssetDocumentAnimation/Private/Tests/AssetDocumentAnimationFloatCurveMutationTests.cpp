// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "Provider/Curve/AssetDocumentAnimationFloatCurveOperations.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveExactState.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveSnapshot.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/TimeStretchCurve.h"
#include "Misc/AutomationTest.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace AssetDocumentAnimationFloatCurveMutationTests
{
constexpr const TCHAR* Primitive = TEXT("animation.curve.float.replace.v2");
// Real Editor-issued payloads and real prepared inverse. This transaction double
// is focused provider evidence, not a Core persistence/composition replacement.
class FResolver final : public IAssetDocumentHandleResolver
{
 struct FEntry { FAssetDocumentOpaqueHandle Handle; TUniquePtr<IAssetDocumentNativeHandlePayload> Payload; };
 class FLease final : public IAssetDocumentVerifiedHandleLease
 {
 public:
  FLease(const FAssetDocumentOpaqueHandle& H, IAssetDocumentNativeHandlePayload& P) : Handle(H), Payload(P) {}
  const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
  IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return Payload; }
 private:
  FAssetDocumentOpaqueHandle Handle; IAssetDocumentNativeHandlePayload& Payload;
 };
 TArray<FEntry> Entries;
public:
 FAssetDocumentMutationToken Token;
 FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access,
  TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
 {
  auto& Entry = Entries.AddDefaulted_GetRef();
  Entry.Handle.RequestId = Token.RequestId; Entry.Handle.TransactionId = Token.TransactionId;
  Entry.Handle.RuntimeRevision = Token.RuntimeRevision; Entry.Handle.ProviderId = TEXT("editor");
  Entry.Handle.Slot = Entries.Num(); Entry.Handle.Generation = 1; Entry.Handle.Kind = Kind; Entry.Handle.Access = Access;
  Entry.Payload = MoveTemp(Payload);
  FAssetDocumentIssuedHandleResult Result; Result.Handle = Entry.Handle; return Result;
 }
 FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& Handle,
  const FAssetDocumentHandleValidationContext& Context) override
 {
  FAssetDocumentVerifiedLeaseResult Result;
  auto Validation = ValidateAssetDocumentHandleMetadata(Handle, Context);
  if (!Validation.bValid) { Result.Diagnostics = MoveTemp(Validation.Diagnostics); return Result; }
  const int32 Index = int32(Handle.Slot) - 1;
  if (!Entries.IsValidIndex(Index) || Entries[Index].Handle.Generation != Handle.Generation
   || Entries[Index].Handle.Kind != Handle.Kind || Entries[Index].Handle.Access != Handle.Access
   || !Entries[Index].Payload || !Entries[Index].Payload->IsAlive()) return Result;
  Result.Lease = MakeUnique<FLease>(Handle, *Entries[Index].Payload); return Result;
 }
 FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override { return {}; }
};

class FFloatCurveTransaction final : public IAssetDocumentAtomicTransaction
{
public:
 FAssetDocumentMutationToken Token;
 int32 FootprintCalls = 0;
 FGuid GetTransactionId() const override { return Token.TransactionId; }
 EAssetDocumentAtomicState GetState() const override { return EAssetDocumentAtomicState::Mutating; }
 const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
 FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint>) override
 { ++FootprintCalls; return Valid(); }
 FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Valid(); }
 FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return Valid(); }
 FAssetDocumentContractValidationResult Rollback(FStringView) override { return Valid(); }
 FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction>) override { return Valid(); }
 FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override { return Valid(); }
 static FAssetDocumentContractValidationResult Valid() { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
};

TOptional<FAssetDocumentOpaqueHandle> Resolve(FAutomationTestBase& Test, UObject& Object,
 EAssetDocumentHandleAccess Access, FResolver& Resolver, FFloatCurveTransaction& Transaction)
{
 FAssetDocumentPrimitiveInvocation I;
 I.PrimitiveId = TEXT("editor.asset.resolve_existing.v1"); I.Version = 1;
 I.RequestId = Resolver.Token.RequestId; I.TransactionId = Resolver.Token.TransactionId;
 I.RuntimeRevision = Resolver.Token.RuntimeRevision; I.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 I.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
 I.ValueArguments.ObjectKeys = {TEXT("path"), TEXT("expected_class")};
 I.ValueArguments.Elements.SetNum(2);
 I.ValueArguments.Elements[0].Kind = I.ValueArguments.Elements[1].Kind = EAssetDocumentRuntimeValueKind::String;
 I.ValueArguments.Elements[0].StringValue = Object.GetPathName();
 I.ValueArguments.Elements[1].StringValue = Object.GetClass()->GetPathName();
 const auto Outcome = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(I, Resolver, &Transaction);
 if (!Test.TestEqual(TEXT("real Editor resolves fixture"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return {};
 const FString Name = Access == EAssetDocumentHandleAccess::Mutable ? TEXT("asset_mutable") : TEXT("asset_read");
 for (const auto& Row : Outcome.HandleResults) if (Row.Name == Name && Row.Handles.Num() == 1) { auto Handle = Row.Handles[0]; Handle.ProviderId = TEXT("animation"); return Handle; }
 Test.AddError(TEXT("real Editor resolve omitted expected Asset handle")); return {};
}
struct FFixture
{
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UAnimSequenceBase> Asset;
 explicit FFixture(bool Composite = false)
  : Package(CreatePackage(*FString::Printf(TEXT("/Game/AssetDocumentTests/CurveMutation_%s/Target"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))))
 {
  Asset.Reset(Composite ? static_cast<UAnimSequenceBase*>(NewObject<UAnimComposite>(Package.Get(), TEXT("Target"), RF_Public | RF_Transactional))
   : static_cast<UAnimSequenceBase*>(NewObject<UAnimMontage>(Package.Get(), TEXT("Target"), RF_Public | RF_Transactional)));
  auto& C = Asset->GetController(); C.InitializeModel(); C.SetFrameRate(FFrameRate(30, 1), false);
  C.SetNumberOfFrames(FFrameNumber(60), false); C.NotifyPopulated();
  Package->SetDirtyFlag(false); FAssetRegistryModule::AssetCreated(Asset.Get());
 }
 ~FFixture() { if (Asset.IsValid()) FAssetRegistryModule::AssetDeleted(Asset.Get()); if (Package.IsValid()) Package->SetDirtyFlag(false); }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 1;
  R.Operation = EAssetDocumentRuntimeOperation::Apply; R.TargetAssetPath = Asset->GetPathName();
  R.RequestedAssetClassPath = Asset->GetClass()->GetPathName(); return R;
 }
};
FAssetDocumentRuntimeValueDto Dto(TConstArrayView<FFloatCurve> Rows)
{
 FAssetDocumentRuntimeValueDto Curves, Value;
 if (!CaptureAnimationFloatCurveSnapshotV2(Rows, Curves)) return {};
 Value.Kind = EAssetDocumentRuntimeValueKind::Object; Value.ObjectKeys = {TEXT("curves")}; Value.Elements.Add(MoveTemp(Curves)); return Value;
}
struct FRun
{
 FResolver Resolver;
 FFloatCurveTransaction Transaction;
 FAssetDocumentPreparedMutationBatch Batch;
 TUniquePtr<FAssetDocumentCoreMutationCaptureLease> Capture;
 FAssetDocumentPrimitiveInvocation Invocation;
 bool Open(FAutomationTestBase& Test, FFixture& F, const FAssetDocumentRuntimeValueDto& Value)
 {
  auto Request = F.Request(); Resolver.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision}; Transaction.Token = Resolver.Token;
  const auto Handle = Resolve(Test, *F.Asset, EAssetDocumentHandleAccess::Mutable, Resolver, Transaction); if (!Handle.IsSet()) return false;
  FAssetDocumentMutationIntent Intent; Intent.StepIndex = 0; Intent.Requirement = {Primitive, 2, TEXT("animation_profile")};
  Intent.InputSchemaId = TEXT("animation.curve.float.replace.v2.input.v2"); Intent.Phase = EAssetDocumentRuntimeRequestPhase::Apply; Intent.Intent = Value;
  const TArray<FAssetDocumentMutationIntent> Intents = {Intent};
  const auto Prepared = PrepareAnimationFloatCurveMutationBatch(Request, Intents, Batch);
  if (!Test.TestTrue(TEXT("real curve preparation"), Prepared.bValid))
  { for (const auto& D : Prepared.Diagnostics) Test.AddInfo(D.Code); return false; }
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  Capture = Batch.OpenMutationCapture(Transaction.Token.TransactionId, Diagnostics);
  if (!Test.TestTrue(TEXT("real curve capture"), Capture.IsValid())) return false;
  Invocation.PrimitiveId = Primitive; Invocation.Version = 2; Invocation.RequestId = Request.RequestId;
  Invocation.RuntimeRevision = Request.ExpectedRuntimeRevision; Invocation.TransactionId = Transaction.Token.TransactionId;
  Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply; Invocation.HandleArguments = {{TEXT("asset"), Handle.GetValue()}}; Invocation.ValueArguments = Value;
  return Test.TestTrue(TEXT("exact prepared invocation"), Batch.Contributions[0].ValidateInvocation(Invocation, Resolver).bValid);
 }
 FAssetDocumentPrimitiveOutcome Invoke() { return InvokeAnimationFloatCurveReplace(Invocation, Resolver, &Transaction); }
 bool Inverse() { Capture->Close(); return Batch.Contributions[0].InitialAction->Rollback(TEXT("focused curve failure")).bValid; }
};
struct FBracketEvents
{
 IAnimationDataModel& Model;
 FDelegateHandle Handle;
 int32 Opened = 0, Closed = 0;
 explicit FBracketEvents(IAnimationDataModel& InModel) : Model(InModel)
 {
  Handle = Model.GetModifiedEvent().AddLambda([this](const EAnimDataModelNotifyType& Type, IAnimationDataModel*, const FAnimDataModelNotifPayload&)
  { Opened += Type == EAnimDataModelNotifyType::BracketOpened; Closed += Type == EAnimDataModelNotifyType::BracketClosed; });
 }
 ~FBracketEvents() { Model.GetModifiedEvent().Remove(Handle); }
};
struct FHooks
{
 AssetDocumentAnimationFloatCurveExactStateTesting::FHooks Hooks;
 FHooks() { AssetDocumentAnimationFloatCurveExactStateTesting::SetHooks(&Hooks); }
 ~FHooks() { AssetDocumentAnimationFloatCurveExactStateTesting::SetHooks(nullptr); }
};
FFloatCurve DefaultCurve(float Value)
{
 FFloatCurve C(FName(TEXT("Default")), AACF_Editable); C.FloatCurve.SetDefaultValue(Value); return C;
}
FFloatCurve AutoCurve()
{
 FFloatCurve C(FName(TEXT("AutoShape")), AACF_Editable);
 FRichCurveKey A(0.f, 0.f), B(1.f, 1.f); A.InterpMode = B.InterpMode = RCIM_Cubic;
 A.TangentMode = B.TangentMode = RCTM_Auto; A.LeaveTangent = 2.f; B.ArriveTangent = 0.f;
 C.FloatCurve.SetKeys({A, B});
 // Test-owned value DTO seed intentionally holds a valid noncanonical Auto shape.
 C.FloatCurve.GetKey(C.FloatCurve.FindKey(0.f)) = A; C.FloatCurve.GetKey(C.FloatCurve.FindKey(1.f)) = B;
 return C;
}
FFloatCurve WeightedCurve()
{
 FFloatCurve C(FName(TEXT("Weighted")), AACF_Editable | AACF_Metadata);
 C.Color = FLinearColor(0.1f, 0.2f, 0.3f, 0.4f); C.Comment = TEXT("保留非对称权重");
 FRichCurveKey A(0.f, 0.f), B(1.f, 2.f);
 for (auto* K : {&A, &B})
 { K->InterpMode = RCIM_Cubic; K->TangentMode = RCTM_Break; K->TangentWeightMode = RCTWM_WeightedBoth; }
 A.ArriveTangent = 0.25f; A.LeaveTangent = 2.f; A.ArriveTangentWeight = 0.125f; A.LeaveTangentWeight = 0.75f;
 B.ArriveTangent = -1.f; B.LeaveTangent = 0.5f; B.ArriveTangentWeight = 0.375f; B.LeaveTangentWeight = 1.25f;
 C.FloatCurve.SetKeys({A, B}); C.FloatCurve.PreInfinityExtrap = RCCE_CycleWithOffset; C.FloatCurve.PostInfinityExtrap = RCCE_Oscillate;
 return C;
}
bool Seed(FAutomationTestBase& Test, FFixture& F, TConstArrayView<FFloatCurve> Rows)
{
 FRun Run; if (!Run.Open(Test, F, Dto(Rows))) return false;
 const bool Ok = Test.TestEqual(TEXT("seed uses real full replacement handler"), Run.Invoke().Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 Run.Capture->Close(); return Ok;
}
const FFloatCurve* Find(const FFixture& F, const TCHAR* Name)
{ return F.Asset->GetDataModel()->FindFloatCurve(FAnimationCurveIdentifier(FName(Name), ERawCurveTrackTypes::RCT_Float)); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloatCurveMutationDescriptorTest,
 "UEAssetDoc.AnimationProvider.FloatCurveMutation.Descriptor", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFloatCurveMutationDescriptorTest::RunTest(const FString&)
{
 const auto D = MakeAnimationFloatCurveReplaceDescriptor();
 TestTrue(TEXT("bridge descriptor structurally validates; review is separate"), ValidateAssetDocumentPrimitiveDescriptor(D).bValid);
 TestTrue(TEXT("exact curve descriptor"), IsFloatCurveReplaceAdmissionShape(D));
 auto Drift = D; Drift.Admission.GovernanceId = TEXT("unapproved");
 TestFalse(TEXT("governance drift rejected"), IsFloatCurveReplaceAdmissionShape(Drift));
 Drift = D; Drift.HandleArgumentSchemas[0].RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
 TestFalse(TEXT("readonly target drift rejected"), IsFloatCurveReplaceAdmissionShape(Drift));
 TestEqual(TEXT("version two"), D.Version, uint32(2));
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloatCurveMutationReplacementTest,
 "UEAssetDoc.AnimationProvider.FloatCurveMutation.FullReplacement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFloatCurveMutationReplacementTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationFloatCurveMutationTests;
 for (bool Composite : {false, true})
 {
  FFixture F(Composite);
  const TArray<FFloatCurve> Rows{DefaultCurve(2.f), AutoCurve(), WeightedCurve()};
  if (!Seed(*this, F, Rows)) return false;
  const auto* Auto = Find(F, TEXT("AutoShape")); const auto* Default = Find(F, TEXT("Default")); const auto* Weighted = Find(F, TEXT("Weighted"));
  if (!TestNotNull(TEXT("Auto curve exists"), Auto) || !TestNotNull(TEXT("default curve exists"), Default) || !TestNotNull(TEXT("weighted curve exists"), Weighted)) return false;
  TestEqual(TEXT("noncanonical Auto shape remains 0.75"), Auto->FloatCurve.Eval(0.5f), 0.75f);
  TestEqual(TEXT("empty curve honors default"), Default->FloatCurve.Eval(0.5f, 7.f), 2.f);
  TestEqual(TEXT("asymmetric arrive weight"), Weighted->FloatCurve.GetConstRefOfKeys()[0].ArriveTangentWeight, 0.125f);
  TestEqual(TEXT("asymmetric leave weight"), Weighted->FloatCurve.GetConstRefOfKeys()[0].LeaveTangentWeight, 0.75f);
  {
   FRun NoOp; if (!NoOp.Open(*this, F, Dto(Rows))) return false; FHooks H;
   const auto Outcome = NoOp.Invoke(); TestEqual(TEXT("no-op succeeds"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
   TestTrue(TEXT("no-op emits no ChangedSet"), Outcome.ChangedSet.IsEmpty());
   TestEqual(TEXT("no-op publishes nothing"), H.Hooks.PublishCalls, 0); TestEqual(TEXT("no-op enlists nothing"), NoOp.Transaction.FootprintCalls, 0);
   TestTrue(TEXT("no-op inverse succeeds"), NoOp.Inverse()); TestEqual(TEXT("no-op inverse publishes nothing"), H.Hooks.PublishCalls, 0);
  }
  {
   FRun Delete; const TArray<FFloatCurve> Empty;
   if (!Delete.Open(*this, F, Dto(Empty))) return false;
   TestEqual(TEXT("delete entire managed float collection"), Delete.Invoke().Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
   TestEqual(TEXT("no float rows remain"), F.Asset->GetDataModel()->GetFloatCurves().Num(), 0);
   TestTrue(TEXT("delete inverse restores full rows"), Delete.Inverse());
   TestEqual(TEXT("deleted Auto shape returns"), Find(F, TEXT("AutoShape"))->FloatCurve.Eval(0.5f), 0.75f);
  }
  {
   const TArray<FFloatCurve> Unset{FFloatCurve(FName(TEXT("Default")), AACF_Editable)};
   FRun Run; if (!Run.Open(*this, F, Dto(Unset))) return false;
   TestEqual(TEXT("explicit unset replaces default"), Run.Invoke().Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
   TestEqual(TEXT("unset uses fallback"), Find(F, TEXT("Default"))->FloatCurve.Eval(0.5f, 7.f), 7.f);
   TestTrue(TEXT("unset inverse restores default"), Run.Inverse());
   TestEqual(TEXT("restored default is two"), Find(F, TEXT("Default"))->FloatCurve.Eval(0.5f, 7.f), 2.f);
  }
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloatCurveMutationFailureTest,
 "UEAssetDoc.AnimationProvider.FloatCurveMutation.FailureInverse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFloatCurveMutationFailureTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationFloatCurveMutationTests;
 for (bool AfterPatch : {false, true})
 {
  FFixture F; const TArray<FFloatCurve> Old{DefaultCurve(2.f), AutoCurve(), WeightedCurve()}, Desired{DefaultCurve(3.f), WeightedCurve()};
  if (!Seed(*this, F, Old)) return false;
  FRun Run; if (!Run.Open(*this, F, Dto(Desired))) return false;
  FBracketEvents Events(*F.Asset->GetDataModel());
  FHooks H; H.Hooks.bFailAfterFirstPatch = AfterPatch; H.Hooks.bFailAfterControllerKeys = !AfterPatch;
  const auto Outcome = Run.Invoke();
  TestEqual(TEXT("publication failure reported"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  TestTrue(TEXT("failed publication emits no change claims"), Outcome.ChangedSet.IsEmpty());
  TestEqual(TEXT("failure closes bracket"), H.Hooks.OpenBrackets, H.Hooks.CloseBrackets);
  TestTrue(TEXT("real model emitted bracket open"), Events.Opened > 0);
  TestEqual(TEXT("real model failure bracket events balance"), Events.Opened, Events.Closed);
  if (AfterPatch) TestEqual(TEXT("default really reached three before failure"), Find(F, TEXT("Default"))->FloatCurve.Eval(0.5f, 7.f), 3.f);
  TWeakObjectPtr<UAnimSequenceBase> Target(F.Asset.Get()); F.Asset.Reset();
  CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS); TestTrue(TEXT("prepared state anchors target across GC"), Target.IsValid());
  H.Hooks.bFailAfterFirstPatch = false; H.Hooks.bFailAfterControllerKeys = false;
  TestTrue(TEXT("typed inverse succeeds"), Run.Inverse()); F.Asset.Reset(Target.Get());
  TestEqual(TEXT("inverse default returns two"), Find(F, TEXT("Default"))->FloatCurve.Eval(0.5f, 7.f), 2.f);
  TestEqual(TEXT("inverse Auto shape returns 0.75"), Find(F, TEXT("AutoShape"))->FloatCurve.Eval(0.5f), 0.75f);
  TestEqual(TEXT("inverse also closes bracket"), H.Hooks.OpenBrackets, H.Hooks.CloseBrackets);
  TestEqual(TEXT("real model inverse bracket events balance"), Events.Opened, Events.Closed);
  TestEqual(TEXT("one forward and one inverse publication"), H.Hooks.PublishCalls, 2);
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloatCurveMutationGuardTest,
 "UEAssetDoc.AnimationProvider.FloatCurveMutation.Guards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFloatCurveMutationGuardTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationFloatCurveMutationTests;
 for (int32 Case = 0; Case < 7; ++Case)
 {
  FFixture F, Other; FRun Run; const TArray<FFloatCurve> Desired{DefaultCurve(3.f)};
  if (!Run.Open(*this, F, Dto(Desired))) return false;
  IAnimationDataController* OriginalController = &F.Asset->GetController();
  if (Case == 0) Run.Capture->Close();
  if (Case == 1) Run.Invocation.HandleArguments[0].Handle.Access = EAssetDocumentHandleAccess::ReadOnly;
  if (Case == 2) Run.Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == 3) Run.Invocation.ValueArguments.Elements[0].Elements.Reset();
  if (Case == 4) F.Asset->GetController().SetNumberOfFrames(FFrameNumber(90), false);
  if (Case == 5) OriginalController->SetModel(Other.Asset->GetDataModelInterface());
  if (Case == 6) Run.Invocation.ValueArguments.ObjectKeys[0] = TEXT("CURVES");
  FHooks H; TestEqual(TEXT("invalid prepared invocation rejects before leaf"), Run.Invoke().Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  TestEqual(TEXT("invalid input publishes nothing"), H.Hooks.PublishCalls, 0);
  TestTrue(TEXT("unwritten inverse does not poison other changes"), Run.Inverse());
  if (Case == 5) OriginalController->SetModel(F.Asset->GetDataModelInterface());
 }
 TStrongObjectPtr<UAnimSequence> Sequence(NewObject<UAnimSequence>(GetTransientPackage())); FHooks H;
 TestFalse(TEXT("non-frozen Sequence model is rejected"), IsExactAnimationFloatCurveTarget(*Sequence));
 TestEqual(TEXT("unsupported target has zero publication"), H.Hooks.PublishCalls, 0);
 return true;
}

namespace AssetDocumentAnimationFloatCurveMutationTests
{
FFloatCurve Stretch(FName Name, bool Reverse)
{
 FFloatCurve C(Name, AACF_Editable);
 const TArray<float> Values = Reverse ? TArray<float>{1.f, 0.7f, 0.2f, 0.f, 0.f} : TArray<float>{0.f, 0.f, 0.2f, 0.7f, 1.f};
 TArray<FRichCurveKey> Keys;
 for (int32 I = 0; I < Values.Num(); ++I) { FRichCurveKey K(I * 0.5f, Values[I]); K.InterpMode = RCIM_Linear; Keys.Add(K); }
 C.FloatCurve.SetKeys(Keys); return C;
}
bool StretchMapping(FAutomationTestBase& Test, UAnimMontage& Montage, TArray<float>& Out)
{
 // The same public editor lifecycle as stock AnimPreviewInstance. No private bake.
 FAnimMontageInstance Instance; Instance.Initialize(&Montage); Instance.EditorOnly_PreAdvance();
 FTimeStretchCurveInstance Mapping; Mapping.InitializeFromPlayRate(2.f, Montage.TimeStretchCurve);
 if (!Test.TestTrue(TEXT("nonconstant TimeStretch produces valid rate-two mapping"), Mapping.HasValidData())) return false;
 Out.Reset();
 for (float Original : {0.3f, 0.7f, 1.1f, 1.7f})
 {
  const int32 Index = Mapping.BinarySearchMarkerIndex(Original, Mapping.GetMarkers_Original());
  const float Target = Mapping.Convert_P_Original_To_Target(Index, Original); Out.Add(Target);
  const int32 BackIndex = Mapping.BinarySearchMarkerIndex(Target, Mapping.GetMarkers_Target());
  Test.TestTrue(TEXT("time mapping round trip"), FMath::IsNearlyEqual(Mapping.Convert_P_Target_To_Original(BackIndex, Target), Original, 0.0001f));
 }
 return true;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloatCurveMutationTimeStretchTest,
 "UEAssetDoc.AnimationProvider.FloatCurveMutation.TimeStretch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFloatCurveMutationTimeStretchTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationFloatCurveMutationTests;
 FFixture F; auto& Montage = *CastChecked<UAnimMontage>(F.Asset.Get());
 const FName Name = Montage.TimeStretchCurveName;
 const TArray<FFloatCurve> Original{Stretch(Name, false)}, Changed{Stretch(Name, true)};
 if (!Seed(*this, F, Original)) return false;
 if (!TestTrue(TEXT("real montage duration covers sampled positions"), Montage.GetPlayLength() >= 2.f)) return false;
 TArray<float> Before, After, Restored;
 if (!StretchMapping(*this, Montage, Before)) return false;
 {
  FRun Run; if (!Run.Open(*this, F, Dto(Changed))) return false;
  if (!TestEqual(TEXT("real TimeStretch source replacement"), Run.Invoke().Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
  if (!StretchMapping(*this, Montage, After)) return false;
  TestTrue(TEXT("changed authored source changes actual playback mapping"), Before != After);
  if (!TestTrue(TEXT("typed curve inverse after editor lifecycle"), Run.Inverse())) return false;
  if (!StretchMapping(*this, Montage, Restored)) return false;
  for (int32 I = 0; I < Before.Num(); ++I) TestTrue(TEXT("inverse restores playback mapping"), FMath::IsNearlyEqual(Before[I], Restored[I], 0.0001f));
 }
 {
  FRun Run; const TArray<FFloatCurve> Empty; if (!Run.Open(*this, F, Dto(Empty))) return false;
  if (!TestEqual(TEXT("delete named source"), Run.Invoke().Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
  FAnimMontageInstance Instance; Instance.Initialize(&Montage); Instance.EditorOnly_PreAdvance();
  TestFalse(TEXT("deletion clears derived TimeStretch through public lifecycle"), Montage.TimeStretchCurve.IsValid());
  if (!TestTrue(TEXT("delete inverse restores named source"), Run.Inverse())) return false;
  if (!StretchMapping(*this, Montage, Restored)) return false;
  for (int32 I = 0; I < Before.Num(); ++I) TestTrue(TEXT("delete inverse restores mapping"), FMath::IsNearlyEqual(Before[I], Restored[I], 0.0001f));
 }
 return true;
}
#endif
