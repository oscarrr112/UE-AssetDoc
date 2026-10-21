// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentMutationComposition.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMutationCompositionTests
{
FAssetDocumentContractValidationResult Valid(bool bValid = true)
{
 FAssetDocumentContractValidationResult R; R.bValid = bValid;
 if (!bValid) { auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Code = TEXT("TestProvider.InverseFailure"); D.Path = TEXT("/Test"); D.Message = TEXT("Injected inverse failure"); }
 return R;
}
class FHandles : public IAssetDocumentHandleResolver
{
public:
 FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind, EAssetDocumentHandleAccess, TUniquePtr<IAssetDocumentNativeHandlePayload>&&) override { return {}; }
 FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle&, const FAssetDocumentHandleValidationContext&) override { return {}; }
 FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override { return {}; }
};
class FCancellation : public IAssetDocumentCancellationView { bool IsCancellationRequested() const override { return false; } };
class FAction : public IAssetDocumentRollbackAction
{
 FString Id; TArray<FString>& Trace; bool bFail;
public:
 FAction(FString InId, TArray<FString>& InTrace, bool bInFail = false) : Id(MoveTemp(InId)), Trace(InTrace), bFail(bInFail) {}
 FStringView GetRollbackComponentId() const override { return Id; }
 FAssetDocumentContractValidationResult Rollback(FStringView) override { Trace.Add(Id); return Valid(!bFail); }
};
class FFinalizer : public IAssetDocumentPostRollbackFinalizer
{
 TArray<FString>& Trace; bool bFail;
public:
 FFinalizer(TArray<FString>& InTrace, bool bInFail = false) : Trace(InTrace), bFail(bInFail) {}
 FStringView GetRollbackComponentId() const override { return TEXT("rollback.package"); }
 FAssetDocumentContractValidationResult FinalizePostRollback(FStringView) override { Trace.Add(TEXT("dirty-last")); auto R = Valid(!bFail); if (bFail) R.Diagnostics[0].Code = TEXT("TestProvider.DirtyFailure"); return R; }
};
struct FFixture
{
 FAssetDocumentRuntimeRequestDto Request;
 TArray<FString> Trace;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 FHandles Handles;
 FCancellation Cancellation;
 TSharedPtr<FAssetDocumentMutationComposition> Composition;
 TUniquePtr<FAssetDocumentRollbackActionComposition> Factory;
 TUniquePtr<IAssetDocumentAtomicTransaction> Transaction;
 TUniquePtr<FAssetDocumentCoreMutationCaptureLease> Capture;
 IAssetDocumentAtomicTransaction* Adapter = nullptr;
 FFixture()
 {
  Request.RequestId = FGuid::NewGuid(); Request.ExpectedRuntimeRevision = 19;
  Request.TargetAssetPath = TEXT("/Game/A0/Target.Target"); Request.RequestedAssetClassPath = TEXT("/Script/Engine.DataAsset"); Request.Operation = EAssetDocumentRuntimeOperation::Apply;
 }
 ~FFixture() { Capture.Reset(); if (Transaction && Transaction->GetState() != EAssetDocumentAtomicState::RolledBack && Transaction->GetState() != EAssetDocumentAtomicState::Poisoned) Transaction->Rollback(TEXT("test.cleanup")); }
 TArray<FAssetDocumentMutationFootprint> Rows(FString Component = TEXT("test.raw")) const
 {
  return {{EAssetDocumentMutationFootprintKind::UObjectState, TEXT("Object:") + Request.TargetAssetPath, Component},
   {EAssetDocumentMutationFootprintKind::Package, TEXT("Package:/Game/A0/Target"), Component}};
 }
 FAssetDocumentMutationIntent Intent(int32 Index, const TCHAR* Provider) const
 {
  FAssetDocumentMutationIntent I; I.StepIndex = Index; I.Requirement = {FString(Provider) + TEXT(".test.write.v1"), 1, TEXT("test.consumer")};
  I.InputSchemaId = TEXT("test.input.v1"); I.Intent.Kind = EAssetDocumentRuntimeValueKind::Object; return I;
 }
 FAssetDocumentMutationComposition::FProviderBatch Batch(int32 Index, const TCHAR* Provider, const TCHAR* Surface, bool bInverseFailure = false, bool bValidationFailure = false)
 {
  FAssetDocumentMutationComposition::FProviderBatch B; B.ProviderId = Provider;
  FAssetDocumentPreparedMutationContribution C; C.StepIndex = Index; C.RawFootprint = Rows();
  C.OwnedSurfaceIds.Add(Request.TargetAssetPath + TEXT("::") + Surface);
  C.InitialAction = MakeShared<FAction>(FString(Provider), Trace, bInverseFailure);
  C.ValidateInvocation = [bValidationFailure](const auto&, auto&) { return Valid(!bValidationFailure); };
  B.Batch.Contributions.Add(MoveTemp(C));
  if (B.ProviderId == TEXT("editor")) B.Batch.PackageFinalizer = MakeShared<FFinalizer>(Trace);
  return B;
 }
 bool Prepare(bool bInverseFailure = false, bool bValidationFailure = false)
 {
  TArray<FAssetDocumentMutationComposition::FProviderBatch> B;
  B.Add(Batch(0, TEXT("editor"), TEXT("Property")));
  B.Add(Batch(1, TEXT("animation"), TEXT("TestSecondSurface"), bInverseFailure, bValidationFailure));
  Composition = FAssetDocumentMutationComposition::Freeze(Request, {Intent(0, TEXT("editor")), Intent(1, TEXT("animation"))}, MoveTemp(B), {}, {}, {}, Diagnostics);
  return Composition.IsValid();
 }
 bool Begin(bool bOccupiedFinalizer = false)
 {
  if (!Composition) return false;
  Factory = MakeUnique<FAssetDocumentRollbackActionComposition>(Handles, Composition->MakeInitialActions(),
   [this](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic& D) { Diagnostics.Add(D); });
  FAssetDocumentPreflightReceipt Receipt; Receipt.RequestId = Request.RequestId; Receipt.RuntimeRevision = Request.ExpectedRuntimeRevision;
  Receipt.ReceiptHash = TEXT("sha256:test-a0"); Receipt.bSucceeded = true; Receipt.CompleteFootprint.Append(Composition->GetFootprint());
  Transaction = Factory->BeginLiveAfterPreflight(Receipt, Cancellation); if (!Transaction) return false;
  Capture = Composition->MakeCaptureOpener()(Transaction->GetTransactionId(), Diagnostics); if (!Capture) return false;
  if (bOccupiedFinalizer) Transaction->EnlistPostRollbackFinalizer(MakeShared<FFinalizer>(Trace));
  FAssetDocumentRequestContextMetadata M; M.RequestId = Request.RequestId; M.PinnedRuntimeRevision = Request.ExpectedRuntimeRevision; M.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  Adapter = Composition->BindTransaction(*Transaction, M, Diagnostics); return Adapter != nullptr;
 }
 FAssetDocumentPrimitiveOutcome Call(const TCHAR* Provider, bool bFail = false, bool bNoop = false, bool bSkipEnlist = false)
 {
  FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = FString(Provider) + TEXT(".test.write.v1"); I.Version = 1;
  I.RequestId = Request.RequestId; I.RuntimeRevision = Request.ExpectedRuntimeRevision; I.TransactionId = Transaction->GetTransactionId(); I.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  return Composition->Invoke(Provider, I, Handles, Adapter, [&]()
  {
   FAssetDocumentPrimitiveOutcome O; O.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
   if (!bNoop && !bSkipEnlist && !Adapter->EnlistFootprint(Rows()).bValid) return O;
   Trace.Add(FString(TEXT("write:")) + Provider);
   O.Status = bFail ? EAssetDocumentPrimitiveOutcomeStatus::Failed : EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
   if (!bNoop) O.ChangedSet.AddDefaulted();
   return O;
  });
 }
 FAssetDocumentContractValidationResult Rollback() { Capture.Reset(); return Transaction->Rollback(TEXT("test.failure")); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationCompositionOrderTest, "UEAssetDoc.MutationComposition.OrderAndFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMutationCompositionOrderTest::RunTest(const FString&)
{
 using namespace AssetDocumentMutationCompositionTests;
 FFixture F; if (!TestTrue(TEXT("freeze and bind real Core transaction"), F.Prepare(true) && F.Begin())) return false;
 TestEqual(TEXT("two physical resource rows"), F.Composition->GetFootprint().Num(), 2);
 TestEqual(TEXT("one composed initial object action"), F.Composition->MakeInitialActions().Num(), 1);
 TestTrue(TEXT("editor write"), F.Call(TEXT("editor")).Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 TestTrue(TEXT("second provider writes then fails"), F.Call(TEXT("animation"), true).Status == EAssetDocumentPrimitiveOutcomeStatus::Failed);
 const auto RolledBack = F.Rollback();
 TestTrue(TEXT("reverse armed order and unconditional dirty-last"), F.Trace == TArray<FString>{TEXT("write:editor"), TEXT("write:animation"), TEXT("animation"), TEXT("editor"), TEXT("dirty-last")});
 TestTrue(TEXT("inverse failure poisons Core"), F.Transaction->GetState() == EAssetDocumentAtomicState::Poisoned);
 TestFalse(TEXT("inverse failure reaches Core rollback result"), RolledBack.bValid);
 TestTrue(TEXT("stable aggregate followed by exact provider failure exactly once"),
  RolledBack.Diagnostics.Num() == 2
  && RolledBack.Diagnostics[0].Code == TEXT("Atomic.Poisoned.RollbackFailed")
  && RolledBack.Diagnostics[1].Code == TEXT("TestProvider.InverseFailure")
  && RolledBack.Diagnostics[1].Path == TEXT("/Test")
  && RolledBack.Diagnostics[1].Message == TEXT("Injected inverse failure"));
 TestFalse(TEXT("atomic sink does not duplicate provider details forwarded by coordinator"),
  F.Diagnostics.ContainsByPredicate([](const auto& Item){ return Item.Code == TEXT("TestProvider.InverseFailure"); }));
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationCompositionValidationTest, "UEAssetDoc.MutationComposition.ValidationAndCompletion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMutationCompositionValidationTest::RunTest(const FString&)
{
 using namespace AssetDocumentMutationCompositionTests;
 {
  FFixture F; if (!TestTrue(TEXT("prepare"), F.Prepare(false, true) && F.Begin())) return false;
  F.Call(TEXT("editor")); F.Call(TEXT("animation")); F.Rollback();
  TestTrue(TEXT("failed validator never arms or invokes second provider"), F.Trace == TArray<FString>{TEXT("write:editor"), TEXT("editor"), TEXT("dirty-last")});
 }
 {
  FFixture F; if (!TestTrue(TEXT("prepare no-op"), F.Prepare() && F.Begin())) return false;
  TestFalse(TEXT("unconsumed steps fail before readiness"), F.Composition->ValidateComplete().bValid);
  TestFalse(TEXT("adapter refuses readiness"), F.Adapter->MarkReadyToCommit().bValid);
  TestTrue(TEXT("first no-op consumed"), F.Call(TEXT("editor"), false, true).Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  TestTrue(TEXT("second no-op consumed"), F.Call(TEXT("animation"), false, true).Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  TestTrue(TEXT("completed no-op ready"), F.Composition->ValidateComplete().bValid && F.Adapter->MarkReadyToCommit().bValid);
 }
 {
  FFixture F; if (!TestTrue(TEXT("prepare mismatched invocation"), F.Prepare() && F.Begin())) return false;
  TestTrue(TEXT("out of order rejected"), F.Call(TEXT("animation")).Status == EAssetDocumentPrimitiveOutcomeStatus::Failed);
  F.Rollback(); TestTrue(TEXT("no arm or setter on wrong order"), F.Trace == TArray<FString>{TEXT("dirty-last")});
 }
 {
  FFixture F; if (!TestTrue(TEXT("prepare uncovered effect"), F.Prepare() && F.Begin())) return false;
  TestTrue(TEXT("changed outcome without enlist rejected"), F.Call(TEXT("editor"), false, false, true).Status == EAssetDocumentPrimitiveOutcomeStatus::Failed);
  F.Rollback(); TestTrue(TEXT("failed effect still has inverse"), F.Trace.Contains(TEXT("editor")));
 }
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationCompositionFreezeTest, "UEAssetDoc.MutationComposition.CoverageAndBodyOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMutationCompositionFreezeTest::RunTest(const FString&)
{
 using namespace AssetDocumentMutationCompositionTests;
 for (int32 Case = 0; Case < 6; ++Case)
 {
  FFixture F; TArray<FAssetDocumentMutationComposition::FProviderBatch> B;
  B.Add(F.Batch(0, TEXT("editor"), TEXT("Property"))); B.Add(F.Batch(1, TEXT("animation"), TEXT("TestSecondSurface")));
  if (Case == 0) B[1].Batch.Contributions[0].OwnedSurfaceIds = B[0].Batch.Contributions[0].OwnedSurfaceIds;
  if (Case == 1) B[1].Batch.Contributions[0].RawFootprint[0].StableId += TEXT("#Skeleton");
  if (Case == 2) B[1].Batch.Contributions[0].StepIndex = 0;
  if (Case == 3) B[1].Batch.Contributions[0].InitialAction.Reset();
  if (Case == 4) B[0].Batch.PackageFinalizer.Reset();
  if (Case == 5) B[1].Batch.Contributions[0].OwnedSurfaceIds = {F.Request.TargetAssetPath + TEXT("/Body/References/Skeleton")};
  TestFalse(TEXT("invalid overlap/alias/coverage rejected"), FAssetDocumentMutationComposition::Freeze(F.Request, {F.Intent(0, TEXT("editor")), F.Intent(1, TEXT("animation"))}, MoveTemp(B), {}, {}, {}, F.Diagnostics).IsValid());
 }
 FFixture F; TArray<FAssetDocumentMutationComposition::FProviderBatch> B;
 FAssetDocumentMutationComposition::FProviderBatch Editor; Editor.ProviderId = TEXT("editor"); Editor.Batch.PackageFinalizer = MakeShared<FFinalizer>(F.Trace); B.Add(MoveTemp(Editor));
 B.Add(F.Batch(0, TEXT("animation"), TEXT("TestSecondSurface")));
 F.Composition = FAssetDocumentMutationComposition::Freeze(F.Request, {F.Intent(0, TEXT("animation"))}, MoveTemp(B), {}, {}, {}, F.Diagnostics);
 if (!TestTrue(TEXT("Body-only still prepares Editor package"), F.Begin())) return false;
 F.Call(TEXT("animation")); F.Rollback(); TestTrue(TEXT("Body-only inverse precedes dirty finalizer"), F.Trace == TArray<FString>{TEXT("write:animation"), TEXT("animation"), TEXT("dirty-last")});
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationCompositionCreateTest, "UEAssetDoc.MutationComposition.FactoryCoverage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMutationCompositionCreateTest::RunTest(const FString&)
{
 using namespace AssetDocumentMutationCompositionTests;
 FFixture F;
 const FString Component = TEXT("editor.asset.rollback_created.v1");
 TArray<FAssetDocumentMutationFootprint> FactoryRows = {
  {EAssetDocumentMutationFootprintKind::UObjectState, Component + TEXT("|UObjectState|") + F.Request.TargetAssetPath, Component},
  {EAssetDocumentMutationFootprintKind::Package, Component + TEXT("|Package|/Game/A0/Target"), Component},
  {EAssetDocumentMutationFootprintKind::AssetRegistry, Component + TEXT("|AssetRegistry|") + F.Request.TargetAssetPath, Component},
  {EAssetDocumentMutationFootprintKind::File, Component + TEXT("|File|/test/Target.uasset"), Component},
  {EAssetDocumentMutationFootprintKind::Handle, Component + TEXT("|Handle|") + F.Request.TargetAssetPath, Component}};
 auto FactoryBatch = F.Batch(0, TEXT("editor"), TEXT("@lifecycle.create"));
 FactoryBatch.Batch.PackageFinalizer.Reset();
 FactoryBatch.Batch.Contributions[0].InitialAction.Reset();
 FactoryBatch.Batch.Contributions[0].RawFootprint = FactoryRows;
 auto Property = F.Batch(1, TEXT("editor"), TEXT("Property"));
 Property.Batch.Contributions[0].InitialAction.Reset();
 Property.Batch.Contributions[0].RawFootprint = F.Rows();
 Property.Batch.Contributions[0].RawFootprint[0].RollbackComponentId = TEXT("rollback.uobject");
 Property.Batch.Contributions[0].RawFootprint[1].RollbackComponentId = TEXT("rollback.package");
 const auto PropertyRows = Property.Batch.Contributions[0].RawFootprint;
 FactoryBatch.Batch.Contributions.Add(MoveTemp(Property.Batch.Contributions[0]));
 TArray<FAssetDocumentMutationComposition::FProviderBatch> B; B.Add(MoveTemp(FactoryBatch));
 FAssetDocumentCoreMutationCaptureOpener Opener = [](const FGuid&, TArray<FAssetDocumentRuntimeDiagnostic>&)
 { return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([](){}); };
 F.Composition = FAssetDocumentMutationComposition::Freeze(F.Request,
  {F.Intent(0, TEXT("editor")), F.Intent(1, TEXT("editor"))}, MoveTemp(B), FactoryRows,
  {MakeShared<FAction>(Component, F.Trace)}, MoveTemp(Opener), F.Diagnostics);
 if (!TestTrue(TEXT("factory and property can compose"), F.Begin())) return false;
 TestEqual(TEXT("factory coverage count unchanged"), F.Composition->GetFootprint().Num(), 5);
 for (const auto& Row : F.Composition->GetFootprint()) TestEqual(TEXT("only original factory component"), Row.RollbackComponentId, Component);
 TestEqual(TEXT("canonical object row"), F.Composition->GetFootprint()[0].StableId, FString(TEXT("Object:")) + F.Request.TargetAssetPath);
 TestEqual(TEXT("canonical package row"), F.Composition->GetFootprint()[1].StableId, FString(TEXT("Package:/Game/A0/Target")));
 for (int32 I = 0; I < 2; ++I)
 {
  FAssetDocumentPrimitiveInvocation Invocation; Invocation.PrimitiveId = TEXT("editor.test.write.v1"); Invocation.Version = 1;
  Invocation.RequestId = F.Request.RequestId; Invocation.RuntimeRevision = F.Request.ExpectedRuntimeRevision;
  Invocation.TransactionId = F.Transaction->GetTransactionId(); Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  auto Outcome = F.Composition->Invoke(TEXT("editor"), Invocation, F.Handles, F.Adapter, [&]()
  {
   FAssetDocumentPrimitiveOutcome O; O.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
   if (F.Adapter->EnlistFootprint(I == 0 ? FactoryRows : PropertyRows).bValid) O.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
   return O;
  });
  TestTrue(TEXT("raw factory/property batches consumed unchanged"), Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 }
 TestTrue(TEXT("all factory-covered steps consumed"), F.Composition->ValidateComplete().bValid);
 F.Rollback(); TestTrue(TEXT("one factory inverse and no dirty finalizer"), F.Trace == TArray<FString>{Component});
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMutationCompositionFinalizerTest, "UEAssetDoc.MutationComposition.FinalizerFailureAndInstallation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMutationCompositionFinalizerTest::RunTest(const FString&)
{
 using namespace AssetDocumentMutationCompositionTests;
 {
  FFixture F; TArray<FAssetDocumentMutationComposition::FProviderBatch> B;
  auto Editor = F.Batch(0, TEXT("editor"), TEXT("Property"));
  Editor.Batch.PackageFinalizer = MakeShared<FFinalizer>(F.Trace, true); B.Add(MoveTemp(Editor));
  F.Composition = FAssetDocumentMutationComposition::Freeze(F.Request, {F.Intent(0, TEXT("editor"))}, MoveTemp(B), {}, {}, {}, F.Diagnostics);
  if (!TestTrue(TEXT("finalizer failure fixture binds"), F.Begin())) return false;
  F.Call(TEXT("editor")); const auto RolledBack = F.Rollback();
  TestTrue(TEXT("dirty failure poisons real Core transaction"), F.Transaction->GetState() == EAssetDocumentAtomicState::Poisoned);
  TestTrue(TEXT("package finalizer detail reaches Core result exactly once after stable aggregate"),
   !RolledBack.bValid && RolledBack.Diagnostics.Num() == 2
   && RolledBack.Diagnostics[0].Code == TEXT("Atomic.Poisoned.RollbackFailed")
   && RolledBack.Diagnostics[1].Code == TEXT("TestProvider.DirtyFailure")
   && RolledBack.Diagnostics[1].Path == TEXT("/Test")
   && RolledBack.Diagnostics[1].Message == TEXT("Injected inverse failure"));
  TestFalse(TEXT("atomic sink does not duplicate finalizer detail forwarded by coordinator"),
   F.Diagnostics.ContainsByPredicate([](const auto& Item){ return Item.Code == TEXT("TestProvider.DirtyFailure"); }));
  TestTrue(TEXT("Core finalizer poison summary retained"), F.Diagnostics.ContainsByPredicate([](const auto& Item){ return Item.Code == TEXT("Atomic.Poisoned.PostRollbackFinalizerFailed"); }));
 }
 {
  FFixture F; if (!TestTrue(TEXT("freeze install rejection"), F.Prepare())) return false;
  TestFalse(TEXT("occupied finalizer rejects entire composition installation"), F.Begin(true));
  TestTrue(TEXT("installation failure performs no provider call"), F.Trace.IsEmpty());
  TestFalse(TEXT("installation failure cannot complete"), F.Composition->ValidateComplete().bValid);
 }
 return true;
}
#endif
