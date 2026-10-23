// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "Provider/Asset/AssetDocumentAnimationPreviewMeshOperations.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonOperations.h"
#include "AssetDocumentAnimationMutationPreparation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentAnimationReferenceTestLogs
{
// Loading a missing package's object warns that the object was not found; older engines also warn that the package
// was skipped (newer ones resolve the object's package quietly).
inline void ExpectMissingPackageWarnings(FAutomationTestBase& Test, const TCHAR* PackageName, const TCHAR* ObjectPath)
{
#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
 Test.AddExpectedMessagePlain(*FString::Printf(TEXT("LoadPackage: SkipPackage: %s ("), PackageName), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
#endif
 Test.AddExpectedMessagePlain(*FString::Printf(TEXT("Failed to find object 'Object %s'"), ObjectPath), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
}
} // namespace AssetDocumentAnimationReferenceTestLogs

namespace AssetDocumentAnimationPreviewMeshTests
{
constexpr const TCHAR* Primitive = TEXT("animation.asset.preview_mesh.set.v1");
USkeletalMesh* Read(const UAnimationAsset& Asset) { return Asset.GetPreviewMesh(); }
struct FFixture
{
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UAnimMontage> Target;
 TStrongObjectPtr<USkeleton> Skeleton;
 TStrongObjectPtr<USkeletalMesh> Old, Desired;
 FFixture()
  : Package(CreatePackage(*FString::Printf(TEXT("/Game/AssetDocumentTests/Preview_%s/Target"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))),
    Target(NewObject<UAnimMontage>(Package.Get(), TEXT("Target"), RF_Public | RF_Transactional)),
    Skeleton(NewObject<USkeleton>(CreatePackage(*(Package->GetName() + TEXT("_Skeleton"))), TEXT("Target_Skeleton"), RF_Public)),
    Old(NewObject<USkeletalMesh>(CreatePackage(*(Package->GetName() + TEXT("_Old"))), TEXT("Target_Old"), RF_Public)),
    Desired(NewObject<USkeletalMesh>(CreatePackage(*(Package->GetName() + TEXT("_Desired"))), TEXT("Target_Desired"), RF_Public))
 {
  FReferenceSkeletonModifier Modifier(Skeleton.Get());
  Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
  Target->SetSkeleton(Skeleton.Get()); Old->SetSkeleton(Skeleton.Get()); Desired->SetSkeleton(Skeleton.Get());
  Target->SetPreviewMesh(Old.Get(), false); Package->SetDirtyFlag(false);
  FAssetRegistryModule::AssetCreated(Target.Get()); FAssetRegistryModule::AssetCreated(Old.Get()); FAssetRegistryModule::AssetCreated(Desired.Get());
 }
 ~FFixture()
 {
  if (Target.IsValid()) FAssetRegistryModule::AssetDeleted(Target.Get());
  if (Old.IsValid()) FAssetRegistryModule::AssetDeleted(Old.Get());
  if (Desired.IsValid()) FAssetRegistryModule::AssetDeleted(Desired.Get());
  if (Package.IsValid()) Package->SetDirtyFlag(false);
 }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 1;
  R.Operation = EAssetDocumentRuntimeOperation::Apply; R.TargetAssetPath = Target->GetPathName();
  R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage"); return R;
 }
 FAssetDocumentMutationIntent Intent(USkeletalMesh* Mesh) const
 {
  FAssetDocumentMutationIntent I; I.StepIndex = 0; I.Requirement = {Primitive, 1, TEXT("animation_profile")};
  I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.InputSchemaId = FString(Primitive) + TEXT(".input.v1");
  I.Intent.Kind = EAssetDocumentRuntimeValueKind::Object; I.Intent.ObjectKeys = {TEXT("operation"), TEXT("preview_mesh_path")};
  I.Intent.Elements.SetNum(2); I.Intent.Elements[0].Kind = EAssetDocumentRuntimeValueKind::String;
  I.Intent.Elements[0].StringValue = Mesh ? TEXT("set") : TEXT("clear");
  if (Mesh) { I.Intent.Elements[1].Kind = EAssetDocumentRuntimeValueKind::String; I.Intent.Elements[1].StringValue = Mesh->GetPathName(); }
  return I;
 }
};
// Real Editor-issued identity payloads and real family preparation/inverse.
// The transaction below is a test double; Integration owns Core composition evidence.
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

class FAssetReferenceTransaction final : public IAssetDocumentAtomicTransaction
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
 EAssetDocumentHandleAccess Access, FResolver& Resolver, IAssetDocumentAtomicTransaction& Transaction)
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
struct FLiveFixture
{
 FFixture Objects;
 FResolver Resolver;
 FAssetReferenceTransaction Transaction;
 FAssetDocumentPreparedMutationBatch Batch;
 TUniquePtr<FAssetDocumentCoreMutationCaptureLease> Capture;
 FAssetDocumentPrimitiveInvocation Invocation;
 bool Initialize(FAutomationTestBase& Test, USkeletalMesh* Desired, UAnimationAsset* ActualTarget = nullptr)
 {
  auto Request = Objects.Request();
  if (!ActualTarget) ActualTarget = Objects.Target.Get();
  Request.TargetAssetPath = ActualTarget->GetPathName();
  Request.RequestedAssetClassPath = ActualTarget->GetClass()->GetPathName();
  Resolver.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision}; Transaction.Token = Resolver.Token;
  const auto Target = Resolve(Test, *ActualTarget, EAssetDocumentHandleAccess::Mutable, Resolver, Transaction);
  if (!Target.IsSet()) return false;
  const TArray<FAssetDocumentMutationIntent> Intents = {Objects.Intent(Desired)};
  if (!Test.TestTrue(TEXT("prepare real PreviewMesh batch"), PrepareAnimationPreviewMeshMutationBatch(Request, Intents, Batch).bValid)) return false;
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  Capture = Batch.OpenMutationCapture(Transaction.Token.TransactionId, Diagnostics);
  if (!Test.TestTrue(TEXT("open real PreviewMesh capture"), Capture.IsValid())) return false;
  Invocation.PrimitiveId = Primitive; Invocation.Version = 1; Invocation.RequestId = Request.RequestId;
  Invocation.TransactionId = Transaction.Token.TransactionId; Invocation.RuntimeRevision = Request.ExpectedRuntimeRevision;
  Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  Invocation.HandleArguments.Add({TEXT("asset"), Target.GetValue()});
  if (Desired)
  {
   const auto Reference = Resolve(Test, *Desired, EAssetDocumentHandleAccess::ReadOnly, Resolver, Transaction);
   if (!Reference.IsSet()) return false;
   Invocation.HandleArguments.Add({TEXT("preview_mesh"), Reference.GetValue()});
  }
  Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
  Invocation.ValueArguments.ObjectKeys = {TEXT("operation")}; Invocation.ValueArguments.Elements.SetNum(1);
  Invocation.ValueArguments.Elements[0].Kind = EAssetDocumentRuntimeValueKind::String;
  Invocation.ValueArguments.Elements[0].StringValue = Desired ? TEXT("set") : TEXT("clear");
  return Test.TestTrue(TEXT("prepared invocation accepts real payloads"), Batch.Contributions[0].ValidateInvocation(Invocation, Resolver).bValid);
 }
 FAssetDocumentPrimitiveOutcome Invoke() { return InvokeAnimationPreviewMeshSet(Invocation, Resolver, &Transaction); }
};
struct FScopedHooks
{
 AssetDocumentAnimationPreviewMeshTesting::FHooks Hooks;
 FScopedHooks() { AssetDocumentAnimationPreviewMeshTesting::SetHooks(&Hooks); }
 ~FScopedHooks() { AssetDocumentAnimationPreviewMeshTesting::SetHooks(nullptr); }
};
// Test-only persisted bad-state fixture. The sole reflected write in this family
// lives here; production uses only SetPreviewMesh(mesh, false).
FSoftObjectProperty* BadReferenceFixtureProperty()
{
 return FindFProperty<FSoftObjectProperty>(UAnimationAsset::StaticClass(), TEXT("PreviewSkeletalMesh"));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewMeshDescriptorTest,
 "UEAssetDoc.AnimationProvider.PreviewMesh.Descriptor", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPreviewMeshDescriptorTest::RunTest(const FString&)
{
 const auto D = MakeAnimationPreviewMeshSetDescriptor();
 TestTrue(TEXT("descriptor validates"), ValidateAssetDocumentPrimitiveDescriptor(D).bValid);
 TestTrue(TEXT("exact admission"), IsPreviewMeshSetAdmissionShape(D));
 for (int32 Case = 0; Case < 6; ++Case)
 {
  auto Drift = D;
  if (Case == 0) Drift.AllowedPhases.Add(EAssetDocumentRuntimeRequestPhase::Verify);
  if (Case == 1) Drift.Admission.ExportedAuthoritativeApi = TEXT("UAnimationAsset::GetPreviewMesh mutable");
  if (Case == 2) Drift.HandleArgumentSchemas[1].RequiredAccess = EAssetDocumentHandleAccess::Mutable;
  if (Case == 3) Drift.Admission.ExactEngineVersion = TEXT("5.8");
  if (Case == 4) Drift.InputSchemaId += TEXT("bad");
  if (Case == 5) Drift.Admission.SourceMarker = TEXT("generic-property-bypass");
  TestFalse(TEXT("descriptor drift rejected"), IsPreviewMeshSetAdmissionShape(Drift));
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewMeshForwardInverseTest,
 "UEAssetDoc.AnimationProvider.PreviewMesh.ForwardInverse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPreviewMeshForwardInverseTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationPreviewMeshTests;
 // Empty->mesh, mesh->mesh, mesh->null, mesh no-op, Empty no-op.
 for (int32 Case = 0; Case < 5; ++Case)
 {
  FLiveFixture F;
  if (Case == 0 || Case == 4) F.Objects.Target->SetPreviewMesh(nullptr, false);
  USkeletalMesh* Baseline = Read(*F.Objects.Target);
  USkeletalMesh* Desired = Case == 2 || Case == 4 ? nullptr : Case == 3 ? F.Objects.Old.Get() : F.Objects.Desired.Get();
  if (!F.Initialize(*this, Desired)) return false;
  FScopedHooks Scope; const bool Changed = Baseline != Desired;
  const auto Outcome = F.Invoke();
  TestEqual(TEXT("forward success"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  TestEqual(TEXT("authoritative const readback"), Read(*F.Objects.Target), Desired);
  TestEqual(TEXT("exact forward setter count"), Scope.Hooks.ForwardSetterCalls, Changed ? 1 : 0);
  TestEqual(TEXT("changed set count"), Outcome.ChangedSet.Num(), Changed ? 1 : 0);
  TestEqual(TEXT("output keys"), Outcome.Value.ObjectKeys, TArray<FString>{TEXT("changed"), TEXT("preview_mesh")});
  if (Outcome.Value.Elements.Num() == 2)
  {
   TestEqual(TEXT("output changed"), Outcome.Value.Elements[0].BooleanValue, Changed);
   TestEqual(TEXT("output reference kind"), Outcome.Value.Elements[1].Kind, Desired ? EAssetDocumentRuntimeValueKind::String : EAssetDocumentRuntimeValueKind::Null);
   if (Desired) TestEqual(TEXT("output reference"), Outcome.Value.Elements[1].StringValue, Desired->GetPathName());
  }
  F.Capture->Close();
  TestTrue(TEXT("prepared inverse"), F.Batch.Contributions[0].InitialAction->Rollback(TEXT("focused")).bValid);
  TestEqual(TEXT("inverse restores baseline"), Read(*F.Objects.Target), Baseline);
  TestEqual(TEXT("inverse skips no-op setter"), Scope.Hooks.InverseSetterCalls, Changed ? 1 : 0);
  TestFalse(TEXT("setter false leaves package dirty to Core"), F.Objects.Package->IsDirty());
  TestEqual(TEXT("Skeleton retained"), F.Objects.Target->GetSkeleton(), F.Objects.Skeleton.Get());
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewMeshFailureInverseTest,
 "UEAssetDoc.AnimationProvider.PreviewMesh.FailureInverse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPreviewMeshFailureInverseTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationPreviewMeshTests;
 for (const bool Poison : {false, true})
 {
  FLiveFixture F; if (!F.Initialize(*this, F.Objects.Desired.Get())) return false;
  TWeakObjectPtr<USkeletalMesh> Old(F.Objects.Old.Get()); F.Objects.Old.Reset();
  FScopedHooks Scope; Scope.Hooks.bFailForwardReadback = true;
  Scope.Hooks.AfterSetter = []() { CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS); };
  TestEqual(TEXT("post setter failure"), F.Invoke().Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  TestTrue(TEXT("strong preimage survives GC"), Old.IsValid());
  F.Capture->Close(); Scope.Hooks.bFailRollbackReadback = Poison;
  TestEqual(TEXT("inverse failure remains visible"), F.Batch.Contributions[0].InitialAction->Rollback(TEXT("focused")).bValid, !Poison);
  TestEqual(TEXT("inverse restores old reference"), Read(*F.Objects.Target), Old.Get());
  TestEqual(TEXT("one inverse setter"), Scope.Hooks.InverseSetterCalls, 1);
  F.Objects.Old.Reset(Old.Get());
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewMeshPreparationTest,
 "UEAssetDoc.AnimationProvider.PreviewMesh.Preparation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPreviewMeshPreparationTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationPreviewMeshTests;
 FFixture F; auto* Property = BadReferenceFixtureProperty();
 if (!TestNotNull(TEXT("exact test-only soft field"), Property)) return false;
 const FSoftObjectPtr Missing(FSoftObjectPath(TEXT("/Game/AssetDocumentTests/DefinitelyMissing.DefinitelyMissing")));
 AssetDocumentAnimationReferenceTestLogs::ExpectMissingPackageWarnings(*this, TEXT("/Game/AssetDocumentTests/DefinitelyMissing"), TEXT("/Game/AssetDocumentTests/DefinitelyMissing.DefinitelyMissing"));
 Property->SetPropertyValue_InContainer(F.Target.Get(), Missing);
 const TArray<FAssetDocumentMutationIntent> Set = {F.Intent(F.Desired.Get())}, Clear = {F.Intent(nullptr)};
 FAssetDocumentPreparedMutationBatch Batch;
 for (const auto* Intents : {&Set, &Clear})
 {
  const auto Result = PrepareAnimationPreviewMeshMutationBatch(F.Request(), *Intents, Batch);
  TestFalse(TEXT("bad nonempty reference rejected"), Result.bValid);
  TestTrue(TEXT("unsupported preimage diagnostic"), Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("AnimationProvider.PreviewMesh.UnsupportedPreimage"); }));
  TestTrue(TEXT("bad reference unchanged"), Property->GetPropertyValue_InContainer(F.Target.Get()).ToSoftObjectPath() == Missing.ToSoftObjectPath());
 }
 F.Target->SetPreviewMesh(F.Old.Get(), false); F.Old->SetSkeleton(nullptr);
 TestFalse(TEXT("incompatible baseline rejected without getter repair"), PrepareAnimationPreviewMeshMutationBatch(F.Request(), Clear, Batch).bValid);
 TestEqual(TEXT("incompatible baseline still present"), Read(*F.Target), F.Old.Get());
 F.Old->SetSkeleton(F.Skeleton.Get()); F.Desired->SetSkeleton(nullptr);
 TestFalse(TEXT("incompatible desired rejected"), PrepareAnimationPreviewMeshMutationBatch(F.Request(), Set, Batch).bValid);
 TestFalse(TEXT("all preflight rejections preserve clean package"), F.Package->IsDirty());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewMeshInvocationGuardTest,
 "UEAssetDoc.AnimationProvider.PreviewMesh.InvocationGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPreviewMeshInvocationGuardTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationPreviewMeshTests;
 for (int32 Case = 0; Case < 7; ++Case)
 {
  FLiveFixture F; if (!F.Initialize(*this, F.Objects.Desired.Get())) return false;
  if (Case == 0) F.Capture->Close();
  if (Case == 1) F.Invocation.TransactionId = FGuid::NewGuid();
  if (Case == 2) F.Invocation.HandleArguments[0].Handle.Access = EAssetDocumentHandleAccess::ReadOnly;
  if (Case == 3) F.Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == 4) F.Objects.Desired->SetSkeleton(nullptr);
  if (Case == 5) F.Objects.Target->SetSkeleton(nullptr);
  if (Case == 6)
  {
   AssetDocumentAnimationReferenceTestLogs::ExpectMissingPackageWarnings(*this, TEXT("/Game/AssetDocumentTests/MissingAfterPrepare"), TEXT("/Game/AssetDocumentTests/MissingAfterPrepare.MissingAfterPrepare"));
   auto* Property = BadReferenceFixtureProperty(); if (!Property) return false;
   Property->SetPropertyValue_InContainer(F.Objects.Target.Get(), FSoftObjectPtr(FSoftObjectPath(TEXT("/Game/AssetDocumentTests/MissingAfterPrepare.MissingAfterPrepare"))));
  }
  FScopedHooks Scope;
  TestEqual(TEXT("direct handler cannot bypass prepared baseline/context"), F.Invoke().Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  TestEqual(TEXT("rejection has zero writes"), Scope.Hooks.ForwardSetterCalls, 0);
  F.Capture->Close();
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewMeshCreateCaptureTest,
 "UEAssetDoc.AnimationProvider.PreviewMesh.CreateCapture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPreviewMeshCreateCaptureTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationPreviewMeshTests;
 FFixture F; auto Request = F.Request(); Request.TargetAssetPath = F.Package->GetName() + TEXT(".Created");
 FAssetDocumentPreparedMutationBatch Batch;
 const TArray<FAssetDocumentMutationIntent> Intents = {F.Intent(F.Desired.Get())};
 if (!TestTrue(TEXT("absent target prepares before factory creation"), PrepareAnimationPreviewMeshMutationBatch(Request, Intents, Batch).bValid)) return false;
 TestFalse(TEXT("create relies on existing Core factory inverse"), Batch.Contributions[0].InitialAction.IsValid());
 FResolver Resolver; FAssetReferenceTransaction Transaction;
 Resolver.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision}; Transaction.Token = Resolver.Token;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 auto Capture = Batch.OpenMutationCapture(Transaction.Token.TransactionId, Diagnostics);
 if (!TestTrue(TEXT("capture opens while create target absent"), Capture.IsValid())) return false;
 // Test-owned materialization models the existing factory result, not a provider writer.
 TStrongObjectPtr<UAnimMontage> Created(NewObject<UAnimMontage>(F.Package.Get(), TEXT("Created"), RF_Public | RF_Transactional));
 Created->SetSkeleton(F.Skeleton.Get()); FAssetRegistryModule::AssetCreated(Created.Get());
 const auto Target = Resolve(*this, *Created, EAssetDocumentHandleAccess::Mutable, Resolver, Transaction);
 const auto Mesh = Resolve(*this, *F.Desired, EAssetDocumentHandleAccess::ReadOnly, Resolver, Transaction);
 if (!Target.IsSet() || !Mesh.IsSet()) { FAssetRegistryModule::AssetDeleted(Created.Get()); return false; }
 FAssetDocumentPrimitiveInvocation Invocation;
 Invocation.PrimitiveId = Primitive; Invocation.Version = 1; Invocation.RequestId = Request.RequestId;
 Invocation.TransactionId = Transaction.Token.TransactionId; Invocation.RuntimeRevision = Request.ExpectedRuntimeRevision;
 Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 Invocation.HandleArguments = {{TEXT("asset"), Target.GetValue()}, {TEXT("preview_mesh"), Mesh.GetValue()}};
 Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
 Invocation.ValueArguments.ObjectKeys = {TEXT("operation")}; Invocation.ValueArguments.Elements.SetNum(1);
 Invocation.ValueArguments.Elements[0].Kind = EAssetDocumentRuntimeValueKind::String; Invocation.ValueArguments.Elements[0].StringValue = TEXT("set");
 FScopedHooks Scope;
 TestEqual(TEXT("created Empty target receives preview"), InvokeAnimationPreviewMeshSet(Invocation, Resolver, &Transaction).Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 TestEqual(TEXT("created target desired reference"), Read(*Created), F.Desired.Get());
 TestEqual(TEXT("one create preview setter"), Scope.Hooks.ForwardSetterCalls, 1);
 Capture->Close(); FAssetRegistryModule::AssetDeleted(Created.Get());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewMeshUnwrittenInverseTest,
 "UEAssetDoc.AnimationProvider.PreviewMesh.UnwrittenInverse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPreviewMeshUnwrittenInverseTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationPreviewMeshTests;
 FFixture F;
 TStrongObjectPtr<USkeleton> NewSkeleton(NewObject<USkeleton>(CreatePackage(*(F.Package->GetName() + TEXT("_NewSkeleton"))), TEXT("Target_NewSkeleton"), RF_Public));
 FReferenceSkeletonModifier Modifier(NewSkeleton.Get());
 Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
 for (const auto& Slot : F.Target->SlotAnimTracks) NewSkeleton->RegisterSlotNode(Slot.SlotName);
 FAssetRegistryModule::AssetCreated(NewSkeleton.Get());
 const auto Request = F.Request();
 auto SkeletonIntent = F.Intent(nullptr);
 SkeletonIntent.Requirement.PrimitiveId = TEXT("animation.asset.skeleton.set.v1");
 SkeletonIntent.InputSchemaId = TEXT("animation.asset.skeleton.set.v1.input.v1");
 SkeletonIntent.Intent.ObjectKeys[1] = TEXT("skeleton_path");
 SkeletonIntent.Intent.Elements[0].StringValue = TEXT("set");
 SkeletonIntent.Intent.Elements[1].Kind = EAssetDocumentRuntimeValueKind::String;
 SkeletonIntent.Intent.Elements[1].StringValue = NewSkeleton->GetPathName();
 auto PreviewIntent = F.Intent(F.Desired.Get()); PreviewIntent.StepIndex = 1;
 const TArray<FAssetDocumentMutationIntent> Intents = {SkeletonIntent, PreviewIntent};
 FAssetDocumentPreparedMutationBatch Batch;
 if (!TestTrue(TEXT("shared wrapper accepts distinct prepared surfaces"), PrepareAssetDocumentAnimationMutationBatch(Request, Intents, Batch).bValid))
 { FAssetRegistryModule::AssetDeleted(NewSkeleton.Get()); return false; }
 FResolver Resolver; FAssetReferenceTransaction Transaction;
 Resolver.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision}; Transaction.Token = Resolver.Token;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 auto Capture = Batch.OpenMutationCapture(Transaction.Token.TransactionId, Diagnostics);
 if (!TestTrue(TEXT("both real family captures open"), Capture.IsValid()))
 { FAssetRegistryModule::AssetDeleted(NewSkeleton.Get()); return false; }
 const auto Target = Resolve(*this, *F.Target, EAssetDocumentHandleAccess::Mutable, Resolver, Transaction);
 const auto Skeleton = Resolve(*this, *NewSkeleton, EAssetDocumentHandleAccess::ReadOnly, Resolver, Transaction);
 const auto Mesh = Resolve(*this, *F.Desired, EAssetDocumentHandleAccess::ReadOnly, Resolver, Transaction);
 if (!Target.IsSet() || !Skeleton.IsSet() || !Mesh.IsSet())
 { FAssetRegistryModule::AssetDeleted(NewSkeleton.Get()); return false; }
 FAssetDocumentPrimitiveInvocation Invocation;
 Invocation.PrimitiveId = TEXT("animation.asset.skeleton.set.v1"); Invocation.Version = 1;
 Invocation.RequestId = Request.RequestId; Invocation.TransactionId = Transaction.Token.TransactionId;
 Invocation.RuntimeRevision = Request.ExpectedRuntimeRevision; Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 Invocation.HandleArguments = {{TEXT("asset"), Target.GetValue()}, {TEXT("skeleton"), Skeleton.GetValue()}};
 Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
 Invocation.ValueArguments.ObjectKeys = {TEXT("operation")}; Invocation.ValueArguments.Elements.SetNum(1);
 Invocation.ValueArguments.Elements[0].Kind = EAssetDocumentRuntimeValueKind::String; Invocation.ValueArguments.Elements[0].StringValue = TEXT("set");
 TestTrue(TEXT("Skeleton prepared validator accepts initial baseline"), Batch.Contributions[0].ValidateInvocation(Invocation, Resolver).bValid);
 TestEqual(TEXT("real Skeleton handler writes first surface"), InvokeAnimationSkeletonSet(Invocation, Resolver, &Transaction).Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 TestEqual(TEXT("dependency really changed"), F.Target->GetSkeleton(), NewSkeleton.Get());
 Invocation.PrimitiveId = Primitive; Invocation.HandleArguments[1] = {TEXT("preview_mesh"), Mesh.GetValue()};
 FScopedHooks Scope;
 TestFalse(TEXT("Preview validator rejects Skeleton drift"), Batch.Contributions[1].ValidateInvocation(Invocation, Resolver).bValid);
 TestEqual(TEXT("direct Preview handler also rejects"), InvokeAnimationPreviewMeshSet(Invocation, Resolver, &Transaction).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
 TestEqual(TEXT("Preview forward never ran"), Scope.Hooks.ForwardSetterCalls, 0);
 Capture->Close();
 TestTrue(TEXT("unwritten Preview inverse succeeds despite dependency drift"), Batch.Contributions[1].InitialAction->Rollback(TEXT("dependency-rejected")).bValid);
 TestEqual(TEXT("unwritten Preview inverse has zero setters"), Scope.Hooks.InverseSetterCalls, 0);
 TestTrue(TEXT("preceding Skeleton inverse restores its own write"), Batch.Contributions[0].InitialAction->Rollback(TEXT("dependency-rejected")).bValid);
 TestEqual(TEXT("original Skeleton restored"), F.Target->GetSkeleton(), F.Skeleton.Get());
 TestEqual(TEXT("original Preview retained"), Read(*F.Target), F.Old.Get());
 TestFalse(TEXT("focused sequence preserves original clean package"), F.Package->IsDirty());
 FAssetRegistryModule::AssetDeleted(NewSkeleton.Get());
 return true;
}
// Sequence leaf fixtures reuse the existing real Editor payload machinery.
// These focused tests call the production prepared inverse directly; the separate
// Integration test owns the actual Core late-failure composition proof.
namespace AssetDocumentAnimationPreviewMeshTests
{
struct FSequenceLiveFixture : FLiveFixture
{
 TStrongObjectPtr<UAnimSequence> Sequence;
 FSequenceLiveFixture()
  : Sequence(NewObject<UAnimSequence>(CreatePackage(*(Objects.Package->GetName() + TEXT("/Sequence"))),
     TEXT("Sequence"), RF_Public | RF_Transactional))
 {
  {
   FReferenceSkeletonModifier Modifier(Objects.Skeleton.Get());
   Modifier.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform::Identity);
  }
  Sequence->SetSkeleton(Objects.Skeleton.Get()); Sequence->SetPreviewMesh(Objects.Old.Get(), false);
  Sequence->RateScale = 1.25f; Sequence->bLoop = true;
  auto& Controller = Sequence->GetController();
  Controller.InitializeModel(); Controller.SetFrameRate(FFrameRate(24, 1), false);
  Controller.SetNumberOfFrames(FFrameNumber(12), false);
  Controller.AddBoneCurve(TEXT("root"), false);
  TArray<FVector3f> Positions, Scales; TArray<FQuat4f> Rotations;
  Positions.Init(FVector3f::ZeroVector, 13); Scales.Init(FVector3f::OneVector, 13); Rotations.Init(FQuat4f::Identity, 13);
  Controller.SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false);
  const FAnimationCurveIdentifier Id(FName(TEXT("UntouchedPreviewCurve")), ERawCurveTrackTypes::RCT_Float);
  Controller.AddCurve(Id, AACF_Editable, false);
  FRichCurveKey A(0.0f, 1.0f), B(0.5f, -3.0f);
  A.TangentWeightMode = RCTWM_WeightedBoth; A.ArriveTangentWeight = 0.2f; A.LeaveTangentWeight = 0.4f;
  Controller.SetCurveKeys(Id, {A, B}, false); Controller.NotifyPopulated();
  Sequence->AnimNotifyTracks.SetNum(1);
  Sequence->AnimNotifyTracks[0].TrackName = TEXT("RetainedTrack");
  auto& Event = Sequence->Notifies.AddDefaulted_GetRef();
  Event.NotifyName = TEXT("RetainedPoint"); Event.Guid = FGuid::NewGuid(); Event.TrackIndex = 0;
  auto& Marker = Sequence->AuthoredSyncMarkers.AddDefaulted_GetRef();
  Marker.MarkerName = TEXT("RetainedMarker"); Marker.Time = 0.25f; Marker.TrackIndex = 0; Marker.Guid = FGuid::NewGuid();
  Sequence->GetOutermost()->SetDirtyFlag(false); FAssetRegistryModule::AssetCreated(Sequence.Get());
 }
 ~FSequenceLiveFixture() { Capture.Reset(); FAssetRegistryModule::AssetDeleted(Sequence.Get()); Sequence->GetOutermost()->SetDirtyFlag(false); }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  auto R = Objects.Request(); R.TargetAssetPath = Sequence->GetPathName();
  R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence"); return R;
 }
 bool Initialize(FAutomationTestBase& Test, USkeletalMesh* Desired)
 { return FLiveFixture::Initialize(Test, Desired, Sequence.Get()); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencePreviewSetClearNoopTest,
 "UEAssetDoc.AnimationProvider.SequencePreview.SetClearNoop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequencePreviewSetClearNoopTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationPreviewMeshTests;
 for (int32 Case = 0; Case < 5; ++Case)
 {
  FSequenceLiveFixture F;
  if (Case == 0 || Case == 4) F.Sequence->SetPreviewMesh(nullptr, false);
  USkeletalMesh* Baseline = Read(*F.Sequence);
  USkeletalMesh* Desired = Case < 2 ? F.Objects.Desired.Get() : Case == 3 ? Baseline : nullptr;
  const FGuid Content = F.Sequence->GetDataModel()->GenerateGuid();
  const auto Keys = F.Sequence->GetDataModel()->GetFloatCurves()[0].FloatCurve.GetConstRefOfKeys();
  if (!F.Initialize(*this, Desired)) return false;
  FScopedHooks Scope;
  const auto Outcome = F.Invoke();
  const bool Changed = Desired != Baseline;
  TestEqual(TEXT("exact Sequence leaf succeeds"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  TestEqual(TEXT("Sequence authoritative readback"), Read(*F.Sequence), Desired);
  TestEqual(TEXT("only actual change invokes setter"), Scope.Hooks.ForwardSetterCalls, Changed ? 1 : 0);
  TestEqual(TEXT("only actual change enlists footprint"), F.Transaction.FootprintCalls, Changed ? 1 : 0);
  TestEqual(TEXT("changed record count"), Outcome.ChangedSet.Num(), Changed ? 1 : 0);
  TestEqual(TEXT("original Skeleton retained"), F.Sequence->GetSkeleton(), F.Objects.Skeleton.Get());
  TestEqual(TEXT("content GUID retained"), F.Sequence->GetDataModel()->GenerateGuid(), Content);
  TestEqual(TEXT("playback retained"), F.Sequence->RateScale, 1.25f);
  TestEqual(TEXT("frame rate retained"), F.Sequence->GetDataModel()->GetFrameRate(), FFrameRate(24, 1));
  const auto& Actual = F.Sequence->GetDataModel()->GetFloatCurves()[0].FloatCurve.GetConstRefOfKeys();
  TestTrue(TEXT("curve values and tangents retained"), Actual == Keys);
  TestEqual(TEXT("curve arrive weight retained"), Actual[0].ArriveTangentWeight, 0.2f);
  TestEqual(TEXT("curve leave weight retained"), Actual[0].LeaveTangentWeight, 0.4f);
  F.Capture->Close(); CollectGarbage(RF_NoFlags);
  TestTrue(TEXT("production typed inverse survives closed capture and GC"), F.Batch.Contributions[0].InitialAction->Rollback(TEXT("sequence-focused")).bValid);
  TestEqual(TEXT("original preview restored"), Read(*F.Sequence), Baseline);
  TestEqual(TEXT("no-op inverse performs no setter"), Scope.Hooks.InverseSetterCalls, Changed ? 1 : 0);
  TestFalse(TEXT("leaf leaves package dirty authority with Core"), F.Sequence->GetOutermost()->IsDirty());
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencePreviewPreparationTest,
 "UEAssetDoc.AnimationProvider.SequencePreview.Preparation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequencePreviewPreparationTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationPreviewMeshTests;
 FSequenceLiveFixture F; FScopedHooks Scope;
 FAssetDocumentPreparedMutationBatch Batch;
 const auto Request = F.Request();
 TestTrue(TEXT("omission prepares without mutation"), PrepareAssetDocumentAnimationMutationBatch(Request, {}, Batch).bValid);
 TestTrue(TEXT("omission has no contribution or capture"), Batch.Contributions.IsEmpty() && !Batch.OpenMutationCapture);
 TestEqual(TEXT("omission preserves preview"), Read(*F.Sequence), F.Objects.Old.Get());
 for (int32 Case = 0; Case < 6; ++Case)
 {
  auto R = Request; auto Intent = F.Objects.Intent(F.Objects.Desired.Get());
  if (Case == 0) R.TargetAssetPath = F.Sequence->GetOutermost()->GetName() + TEXT("_Missing.Missing");
  if (Case == 1) R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
  if (Case == 2) R.TargetAssetPath = F.Objects.Target->GetPathName();
  if (Case == 3) Intent.Requirement.PrimitiveId = TEXT("animation.asset.skeleton.set.v1");
  if (Case == 4)
  {
   Intent.Intent.Elements[1].StringValue = TEXT("/Game/MissingPreview.Mesh");
   AssetDocumentAnimationReferenceTestLogs::ExpectMissingPackageWarnings(*this, TEXT("/Game/MissingPreview"), TEXT("/Game/MissingPreview.Mesh"));
  }
  if (Case == 5) R.TargetAssetPath = UAnimSequence::StaticClass()->GetDefaultObject()->GetPathName();
  const TArray<FAssetDocumentMutationIntent> Intents{Intent};
  TestFalse(TEXT("missing/mismatch/non-preview/unresolved desired fails before write"), PrepareAssetDocumentAnimationMutationBatch(R, Intents, Batch).bValid);
  TestTrue(TEXT("failed preparation publishes no batch"), Batch.Contributions.IsEmpty() && !Batch.OpenMutationCapture);
 }
 {
  auto Provider = CreateAssetDocumentAnimationRequestProvider();
  TestTrue(TEXT("private request owner accepts exact Sequence omission"), Provider->Prepare(Request, {}, Batch).bValid);
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  TestFalse(TEXT("Sequence bound owner never opens Montage staged read"), Provider->BindStagedRead(
   *F.Objects.Target, TEXT("/sequence-test/unused-staged-file"), 2, Diagnostics).IsValid());
  Provider->CloseRequest();
 }
 const TArray<FAssetDocumentMutationIntent> Clear{F.Objects.Intent(nullptr)};
 auto* Property = BadReferenceFixtureProperty();
 if (!TestNotNull(TEXT("test-only stored reference fixture"), Property)) return false;
 Property->SetPropertyValue_InContainer(F.Sequence.Get(), FSoftObjectPtr(FSoftObjectPath(TEXT("/Game/MissingPreview.Mesh"))));
 TestFalse(TEXT("unresolved preimage fails before clear"), PrepareAnimationPreviewMeshMutationBatch(Request, Clear, Batch).bValid);
 F.Sequence->SetPreviewMesh(F.Objects.Old.Get(), false); F.Objects.Old->SetSkeleton(nullptr);
 TestFalse(TEXT("incompatible preimage fails without getter repair"), PrepareAnimationPreviewMeshMutationBatch(Request, Clear, Batch).bValid);
 TestEqual(TEXT("all failed preparations use zero forward setters"), Scope.Hooks.ForwardSetterCalls, 0);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencePreviewInvocationGuardsTest,
 "UEAssetDoc.AnimationProvider.SequencePreview.InvocationGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequencePreviewInvocationGuardsTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationPreviewMeshTests;
 for (int32 Case = 0; Case < 11; ++Case)
 {
  FSequenceLiveFixture F; if (!F.Initialize(*this, F.Objects.Desired.Get())) return false;
  FScopedHooks Scope;
  if (Case == 0) F.Invocation.RequestId = FGuid::NewGuid();
  if (Case == 1) ++F.Invocation.RuntimeRevision;
  if (Case == 2) ++F.Invocation.HandleArguments[0].Handle.Generation;
  if (Case == 3) F.Invocation.HandleArguments[0].Handle.RequestId = FGuid::NewGuid();
  if (Case == 4) F.Invocation.HandleArguments[0].Handle.ProviderId = TEXT("editor");
  if (Case == 5) F.Capture->Close();
  if (Case == 6) F.Sequence->RateScale = 2.0f;
  if (Case == 7) F.Sequence->SetSkeleton(nullptr);
  if (Case == 8) F.Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Repair;
  if (Case == 9)
  {
   auto Other = Resolve(*this, *F.Objects.Target, EAssetDocumentHandleAccess::Mutable, F.Resolver, F.Transaction);
   if (!Other.IsSet()) return false;
   F.Invocation.HandleArguments[0].Handle = Other.GetValue();
  }
  if (Case == 10)
  {
   auto Other = Resolve(*this, *F.Objects.Old, EAssetDocumentHandleAccess::ReadOnly, F.Resolver, F.Transaction);
   if (!Other.IsSet()) return false;
   F.Invocation.HandleArguments[1].Handle = Other.GetValue();
  }
  TestEqual(TEXT("foreign/stale/closed/retained/phase drift rejected"), F.Invoke().Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  TestEqual(TEXT("guard failure has zero setters"), Scope.Hooks.ForwardSetterCalls, 0);
  TestEqual(TEXT("guard failure has zero enlistments"), F.Transaction.FootprintCalls, 0);
  TestEqual(TEXT("guard failure preserves preview"), Read(*F.Sequence), F.Objects.Old.Get());
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencePreviewRetainedBaselineTest,
 "UEAssetDoc.AnimationProvider.SequencePreview.RetainedBaseline", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequencePreviewRetainedBaselineTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationPreviewMeshTests;
 for (int32 Case = 0; Case < 5; ++Case)
 {
  FSequenceLiveFixture F;
  F.Sequence->RetargetSourceAssetReferencePose.Add(FTransform::Identity);
  FAssetDocumentPreparedMutationBatch Batch;
  const TArray<FAssetDocumentMutationIntent> Intents{F.Objects.Intent(F.Objects.Desired.Get())};
  if (!TestTrue(TEXT("freeze Sequence preimage"), PrepareAnimationPreviewMeshMutationBatch(F.Request(), Intents, Batch).bValid)) return false;
  if (Case == 0)
  {
   auto& Controller = F.Sequence->GetController();
   if (!TestTrue(TEXT("remove fixture root track"), Controller.RemoveBoneTrack(TEXT("root"), false))
    || !TestTrue(TEXT("add known Skeleton child track"), Controller.AddBoneCurve(TEXT("child"), false))) return false;
   TArray<FVector3f> Positions, Scales; TArray<FQuat4f> Rotations;
   Positions.Init(FVector3f::ZeroVector, 13); Scales.Init(FVector3f::OneVector, 13); Rotations.Init(FQuat4f::Identity, 13);
   if (!TestTrue(TEXT("populate renamed fixture track"), Controller.SetBoneTrackKeys(TEXT("child"), Positions, Rotations, Scales, false))) return false;
   TArray<FName> Names; F.Sequence->GetDataModel()->GetBoneTrackNames(Names);
   TestTrue(TEXT("stock Controller renamed the retained bone track"), Names.Num() == 1 && Names[0] == TEXT("child"));
  }
  if (Case == 1) F.Sequence->Notifies[0].NotifyName = TEXT("ChangedRetainedPoint");
  if (Case == 2) F.Sequence->bForceRootLock = !F.Sequence->bForceRootLock;
  if (Case == 3) F.Sequence->RetargetSourceAssetReferencePose[0].SetTranslation(FVector(1.e-12, 0., 0.));
  if (Case == 4) F.Sequence->RetargetSourceAssetReferencePose[0].SetRotation(FQuat(0., 0., 0., -1.));
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics; FScopedHooks Scope;
  auto Capture = Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
  TestFalse(TEXT("retained name/timeline/settings/exact-pose drift rejects capture"), Capture.IsValid());
  TestEqual(TEXT("retained drift uses zero setters"), Scope.Hooks.ForwardSetterCalls, 0);
  TestEqual(TEXT("retained drift preserves Preview"), Read(*F.Sequence), F.Objects.Old.Get());
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencePreviewFailureInverseTest,
 "UEAssetDoc.AnimationProvider.SequencePreview.FailureInverse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequencePreviewFailureInverseTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationPreviewMeshTests;
 for (const bool Poison : {false, true})
 {
  FSequenceLiveFixture F; if (!F.Initialize(*this, F.Objects.Desired.Get())) return false;
  FScopedHooks Scope; Scope.Hooks.bFailForwardReadback = true;
  Scope.Hooks.AfterSetter = []() { CollectGarbage(RF_NoFlags); };
  TestEqual(TEXT("post-setter failure surfaced"), F.Invoke().Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  TestEqual(TEXT("forward was actual Sequence setter"), Scope.Hooks.ForwardSetterCalls, 1);
  TestEqual(TEXT("failed forward changed preview before inverse"), Read(*F.Sequence), F.Objects.Desired.Get());
  F.Capture->Close(); Scope.Hooks.bFailRollbackReadback = Poison;
  auto Inverse = F.Batch.Contributions[0].InitialAction;
  TestEqual(TEXT("production inverse readback reports exact outcome"), Inverse->Rollback(TEXT("focused-sequence-failure")).bValid, !Poison);
  TestEqual(TEXT("inverse public setter restored baseline"), Read(*F.Sequence), F.Objects.Old.Get());
  TestEqual(TEXT("production inverse setter called once"), Scope.Hooks.InverseSetterCalls, 1);
  TestFalse(TEXT("single-use inverse cannot be replayed"), Inverse->Rollback(TEXT("replay")).bValid);
 }
 return !HasAnyErrors();
}
#endif
