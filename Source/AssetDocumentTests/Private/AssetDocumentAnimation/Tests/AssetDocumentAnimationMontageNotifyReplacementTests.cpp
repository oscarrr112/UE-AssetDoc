// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "Provider/Montage/MontageNotifyRequestOwner.h"
#include "Tests/Fixtures/AssetDocumentMontageNotifyCallbackTestObject.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentEditorNativeState.h"
#include "Provider/Montage/MontageProfileSnapshot.h"
#include "HAL/FileManager.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/SavePackage.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimNotifies/AnimNotify_PlaySound.h"
#include "Animation/AnimNotifies/AnimNotifyState_TimedParticleEffect.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "Utils/AssetDocumentEngineCompat.h"

namespace AssetDocumentMontageNotifyReplacementTests
{
using namespace AssetDocumentMontage;
// Real Editor-created native payloads with test-local handle storage. The owner
// and composite use actual verified leases; this is not registry/ApplyFile evidence.
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

class FStagingTransaction final : public IAssetDocumentAtomicTransaction
{
public:
 FAssetDocumentMutationToken Token;
 TSharedPtr<IAssetDocumentRollbackAction> InitialAction;
 TUniquePtr<FAssetDocumentCoreMutationCaptureLease> Capture;
 TArray<FAssetDocumentMutationFootprint> Expected;
 bool bEnlisted = false; int32 EnlistCalls = 0; int32 EnlistAttempts = 0;
 int32 LateCalls = 0;
 FStagingTransaction() { Token.RequestId = FGuid::NewGuid(); Token.TransactionId = FGuid::NewGuid(); Token.RuntimeRevision = 1; }
 bool Bind(FAssetDocumentPreparedMutationBatch& Batch)
 {
  if (Batch.Contributions.Num() != 1 || !Batch.Contributions[0].InitialAction || !Batch.OpenMutationCapture) return false;
  InitialAction = Batch.Contributions[0].InitialAction; Expected = Batch.Contributions[0].RawFootprint;
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  Capture = Batch.OpenMutationCapture(Token.TransactionId, Diagnostics);
  return Capture && Capture->IsValid();
 }
 FGuid GetTransactionId() const override { return Token.TransactionId; }
 EAssetDocumentAtomicState GetState() const override { return EAssetDocumentAtomicState::Mutating; }
 const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
 FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Rows) override
 {
  ++EnlistAttempts;
  if (bEnlisted || Rows.Num() != Expected.Num()) return Result(false);
  for (int32 I = 0; I < Rows.Num(); ++I)
   if (Rows[I].Kind != Expected[I].Kind || Rows[I].StableId != Expected[I].StableId || Rows[I].RollbackComponentId != Expected[I].RollbackComponentId) return Result(false);
  bEnlisted = true; ++EnlistCalls; return Result(true);
 }
 FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction>) override { ++LateCalls; return Result(false); }
 FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override { ++LateCalls; return Result(false); }
 FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Result(false); }
 FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return Result(false); }
 FAssetDocumentContractValidationResult Rollback(FStringView Failure) override
 { Capture.Reset(); return InitialAction ? InitialAction->Rollback(Failure) : Result(false); }
};
FAssetDocumentRuntimeValueDto Object() { FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Object; return V; }
FAssetDocumentRuntimeValueDto Text(const FString& S) { auto V = Object(); V.Kind = EAssetDocumentRuntimeValueKind::String; V.StringValue = S; return V; }
FAssetDocumentRuntimeValueDto Number(double N) { auto V = Object(); V.Kind = EAssetDocumentRuntimeValueKind::Number; V.NumberValue = N; return V; }
void Field(FAssetDocumentRuntimeValueDto& V, const TCHAR* Name, FAssetDocumentRuntimeValueDto E) { V.ObjectKeys.Add(Name); V.Elements.Add(MoveTemp(E)); }

struct FFixture
{
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<USkeleton> Skeleton;
 TStrongObjectPtr<UAnimSequence> Source;
 TStrongObjectPtr<UAnimMontage> Target;
 TArray<FAnimNotifyEvent> Before;
 FFixture() : Package(CreatePackage(*FString::Printf(TEXT("/Game/AssetDocumentTests/NotifyOwner_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))),
  Skeleton(NewObject<USkeleton>(Package.Get(), TEXT("Skeleton"))), Source(NewObject<UAnimSequence>(Package.Get(), TEXT("Source"))),
  Target(NewObject<UAnimMontage>(Package.Get(), TEXT("Target"), RF_Public | RF_Transactional)) {}
 bool Setup()
 {
  { FReferenceSkeletonModifier M(Skeleton.Get()); M.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); }
  Skeleton->RegisterSlotNode(TEXT("DefaultSlot")); Source->SetSkeleton(Skeleton.Get());
  auto& C = Source->GetController(); C.InitializeModel(); C.SetFrameRate(FFrameRate(30, 1), false); C.SetNumberOfFrames(FFrameNumber(30), false);
  if (!C.AddBoneCurve(TEXT("root"), false)) return false;
  TArray<FVector3f> P, S; TArray<FQuat4f> R; P.Init(FVector3f::ZeroVector, 31); S.Init(FVector3f::OneVector, 31); R.Init(FQuat4f::Identity, 31);
  if (!C.SetBoneTrackKeys(TEXT("root"), P, R, S, false)) return false;
  C.NotifyPopulated(); Source->WaitOnExistingCompression();
  Target->SetSkeleton(Skeleton.Get()); Target->GetController().InitializeModel(); Target->GetController().SetFrameRate(FFrameRate(30, 1), false);
  Target->GetController().SetNumberOfFrames(FFrameNumber(30), false); Target->GetController().NotifyPopulated();
  Target->SyncSlotIndex = INDEX_NONE;
  auto& Slot = Target->SlotAnimTracks[0]; Slot.AnimTrack.AnimSegments.Reset();
  auto& Seg = Slot.AnimTrack.AnimSegments.AddDefaulted_GetRef(); Seg.SetAnimReference(Source.Get()); Seg.AnimStartTime = 0.f; Seg.AnimEndTime = 1.f; Seg.StartPos = 0.f; Seg.AnimPlayRate = 1.f; Seg.LoopingCount = 1;
  Target->AnimNotifyTracks.Reset(); Target->AnimNotifyTracks.Add(FAnimNotifyTrack(TEXT("Events"), FLinearColor::White)); Target->AnimNotifyTracks.Add(FAnimNotifyTrack(TEXT("States"), FLinearColor::White));
  Target->Notifies.Reset();
  auto& E = Target->Notifies.AddDefaulted_GetRef(); E.NotifyName = TEXT("AssetDocument.Notify"); E.Guid = FGuid::NewGuid(); E.TrackIndex = 0;
  E.Notify = NewObject<UAnimNotify_PlaySound>(Target.Get(), TEXT("AssetDocumentManaged_Notify_Old"), RF_Transactional); E.Link(Target.Get(), .7f, 0);
  auto& N = Target->Notifies.AddDefaulted_GetRef(); N.NotifyName = TEXT("AssetDocument.NotifyState"); N.Guid = FGuid::NewGuid(); N.TrackIndex = 1;
  N.NotifyStateClass = NewObject<UAnimNotifyState_TimedParticleEffect>(Target.Get(), TEXT("AssetDocumentManaged_NotifyState_Old"), RF_Transactional);
  N.Link(Target.Get(), .8f, 0); N.EndLink.Link(Target.Get(), .9f, 0); N.SetDuration(.1f);
  // Finish the actual stock authoring lifecycle before freezing the fixture.
  // In particular UpdateLinkableElements normalizes instant EndLink offsets.
  Target->UpdateLinkableElements();
  static_cast<UAnimSequenceBase&>(*Target).RefreshCacheData();
  FPropertyChangedEvent Changed(nullptr, EPropertyChangeType::ValueSet);
  static_cast<UObject&>(*Target).PostEditChangeProperty(Changed);
  Before = Target->Notifies;
  FAssetRegistryModule::AssetCreated(Target.Get()); Package->SetDirtyFlag(false); return true;
 }
 ~FFixture() { FAssetRegistryModule::AssetDeleted(Target.Get()); Package->SetDirtyFlag(false); }
 void CheckRestored(FAutomationTestBase& Test)
 {
  if (!Test.TestEqual(TEXT("mixed baseline row count restored"), Target->Notifies.Num(), Before.Num())) return;
  for (int32 I = 0; I < Before.Num(); ++I)
   Test.TestTrue(TEXT("authored notify preimage restored"), FAnimNotifyEvent::StaticStruct()->CompareScriptStruct(&Target->Notifies[I], &Before[I], 0));
  Test.TestEqual(TEXT("nonempty length model retained"), Target->GetDataModel()->GetNumberOfFrames(), 30);
 }
};
FAssetDocumentMutationIntent Intent(bool States, bool Empty, bool InvalidProperty)
{
 FAssetDocumentMutationIntent I; I.StepIndex = 0; I.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 I.Requirement = {States ? TEXT("animation.montage.notify_states.replace.v1") : TEXT("animation.montage.notifies.replace.v1"), 1, TEXT("animation_profile")};
 I.InputSchemaId = I.Requirement.PrimitiveId + TEXT(".input.v1"); I.Intent = Object();
 FAssetDocumentRuntimeValueDto Rows; Rows.Kind = EAssetDocumentRuntimeValueKind::Array;
 if (!Empty)
 {
  auto Row = Object(); Field(Row, TEXT("time"), Number(.1)); if (States) Field(Row, TEXT("duration"), Number(.2));
  Field(Row, TEXT("track_index"), Number(States ? 1 : 0));
  auto Item = Object(); UClass* Class = States ? UAnimNotifyState_TimedParticleEffect::StaticClass() : UAnimNotify_PlaySound::StaticClass();
  Field(Item, TEXT("class"), Text(Class->GetPathName())); auto Properties = Object();
  if (InvalidProperty) Field(Properties, TEXT("DefinitelyNotAnAdmittedProperty"), Number(7));
  Field(Item, TEXT("properties"), MoveTemp(Properties)); Field(Row, TEXT("object"), MoveTemp(Item)); Rows.Elements.Add(MoveTemp(Row));
  if (!InvalidProperty) { auto Later = Rows.Elements[0]; Later.Elements[0] = Number(.4); Rows.Elements.Add(MoveTemp(Later)); }
 }
 Field(I.Intent, States ? TEXT("notify_states") : TEXT("notifies"), MoveTemp(Rows)); return I;
}
bool PrepareWithDiagnostics(FAutomationTestBase& Test, const TSharedRef<FMontageNotifyRequestOwner>& Owner,
 const FAssetDocumentRuntimeRequestDto& Request, FAssetDocumentMutationIntent& Edit, FAssetDocumentPreparedMutationBatch& Batch)
{
 const auto R = Owner->Prepare(Request, MakeArrayView(&Edit, 1), Batch);
 for (const auto& D : R.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 return R.bValid;
}
bool Resolve(FAutomationTestBase& Test, FFixture& F, FResolver& Resolver, FStagingTransaction& Transaction, FAssetDocumentOpaqueHandle& Out, bool bReadOnly = false)
{
 FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = TEXT("editor.asset.resolve_existing.v1"); I.Version = 1;
 I.RequestId = Transaction.Token.RequestId; I.TransactionId = Transaction.Token.TransactionId; I.RuntimeRevision = Transaction.Token.RuntimeRevision; I.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 I.ValueArguments = Object(); Field(I.ValueArguments, TEXT("path"), Text(F.Target->GetPathName())); Field(I.ValueArguments, TEXT("expected_class"), Text(TEXT("/Script/Engine.AnimMontage")));
 auto O = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(I, Resolver, &Transaction);
 if (!Test.TestEqual(TEXT("real Editor resolve succeeds"), O.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
 for (const auto& Row : O.HandleResults) if (Row.Name == (bReadOnly ? TEXT("asset_read") : TEXT("asset_mutable")) && Row.Handles.Num() == 1)
 { Out = Row.Handles[0]; Out.ProviderId = TEXT("animation"); return true; }
 Test.AddError(TEXT("Editor resolve omitted requested asset payload")); return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageNotifyOwnerReplacementTest,
 "UEAssetDoc.AnimationProvider.MontageStructural.NotifyReplacement.OwnerLifecycle",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageNotifyOwnerReplacementTest::RunTest(const FString&)
{
 using namespace AssetDocumentMontageNotifyReplacementTests;
 for (bool States : {false, true}) for (int32 Case = 0; Case < 3; ++Case)
 {
  FFixture F; if (!TestTrue(TEXT("real nonempty source/model and mixed fixtures"), F.Setup())) return false;
  FStagingTransaction Transaction; FResolver Resolver; Resolver.Token = Transaction.Token;
  auto Owner = CreateMontageNotifyRequestOwner(); auto Edit = Intent(States, Case == 1, Case == 2);
  FAssetDocumentRuntimeRequestDto Request; Request.RequestId = Transaction.Token.RequestId; Request.ExpectedRuntimeRevision = Transaction.Token.RuntimeRevision;
  Request.Operation = EAssetDocumentRuntimeOperation::Apply; Request.TargetAssetPath = F.Target->GetPathName(); Request.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
  FAssetDocumentPreparedMutationBatch Batch;
  if (!TestTrue(TEXT("owner prepares actual request once"), PrepareWithDiagnostics(*this, Owner, Request, Edit, Batch))) continue;
  FAssetDocumentPreparedMutationBatch Duplicate;
  TestFalse(TEXT("duplicate preparation rejected"), PrepareWithDiagnostics(*this, Owner, Request, Edit, Duplicate));
  TestTrue(TEXT("duplicate publishes no batch"), Duplicate.Contributions.IsEmpty());
  FAssetDocumentOpaqueHandle Handle; if (!Resolve(*this, F, Resolver, Transaction, Handle)) continue;
  FAssetDocumentPrimitiveInvocation Invoke; Invoke.PrimitiveId = Edit.Requirement.PrimitiveId; Invoke.Version = 1; Invoke.RequestId = Request.RequestId;
  Invoke.TransactionId = Transaction.Token.TransactionId; Invoke.RuntimeRevision = Request.ExpectedRuntimeRevision; Invoke.Phase = Edit.Phase; Invoke.ValueArguments = Edit.Intent;
  Invoke.HandleArguments = {{TEXT("asset"), Handle}};
  TestEqual(TEXT("invoke before capture rejected"), Owner->Invoke(Invoke, Resolver, &Transaction).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  // A rejected attempt is terminal for its owner, so use a fresh request owner
  // for the actual captured operation; never reuse opaque composite state.
  Owner = CreateMontageNotifyRequestOwner(); Batch = FAssetDocumentPreparedMutationBatch();
  if (!TestTrue(TEXT("fresh owner prepares same concrete test request"), PrepareWithDiagnostics(*this, Owner, Request, Edit, Batch))) continue;
  if (!TestTrue(TEXT("initial inverse and capture installed"), Transaction.Bind(Batch))) continue;
  auto Wrong = Invoke; Wrong.RequestId = FGuid::NewGuid();
  TestEqual(TEXT("wrong request rejected"), Owner->Invoke(Wrong, Resolver, &Transaction).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  Wrong = Invoke; ++Wrong.RuntimeRevision;
  TestEqual(TEXT("wrong revision rejected"), Owner->Invoke(Wrong, Resolver, &Transaction).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  TestTrue(TEXT("actual contribution validates real lease"), Batch.Contributions[0].ValidateInvocation(Invoke, Resolver).bValid);
  const auto Outcome = Owner->Invoke(Invoke, Resolver, &Transaction);
  TestEqual(TEXT("only invalid property case fails"), Outcome.Status, Case == 2 ? EAssetDocumentPrimitiveOutcomeStatus::Failed : EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  TestEqual(TEXT("one exact footprint consumption including empty replacement"), Transaction.EnlistCalls, 1);
  TestEqual(TEXT("no duplicate footprint request"), Transaction.EnlistAttempts, 1);
  TestEqual(TEXT("no late action/finalizer"), Transaction.LateCalls, 0);
  TestEqual(TEXT("duplicate invoke rejected"), Owner->Invoke(Invoke, Resolver, &Transaction).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  TArray<TWeakObjectPtr<UObject>> NewObjects; TArray<UObject*> Children; GetObjectsWithOuter(F.Target.Get(), Children, AssetDocumentObjects::DirectOnly);
  for (auto* Child : Children)
   if ((Child->IsA<UAnimNotify>() || Child->IsA<UAnimNotifyState>()) && Child != F.Before[0].Notify && Child != F.Before[1].NotifyStateClass) NewObjects.Emplace(Child);
  TestEqual(TEXT("only nonempty replacement allocates its expected new objects"), NewObjects.Num(), Case == 1 ? 0 : (Case == 0 ? 2 : 1));
  if (Case != 2)
  {
   if (TestEqual(TEXT("expected mixed published rows"), F.Target->Notifies.Num(), Case == 1 ? 1 : 3))
   {
    const auto& Retained = F.Target->Notifies.Last();
    TestTrue(TEXT("opposite family object identity retained"), States ? Retained.Notify == F.Before[0].Notify : Retained.NotifyStateClass == F.Before[1].NotifyStateClass);
    if (Case == 0)
    {
     TestTrue(TEXT("first new event has GUID"), F.Target->Notifies[0].Guid.IsValid());
     TestTrue(TEXT("second new event has GUID"), F.Target->Notifies[1].Guid.IsValid());
     TestNotEqual(TEXT("new events have distinct GUIDs"), F.Target->Notifies[0].Guid, F.Target->Notifies[1].Guid);
     TestTrue(TEXT("early new event sorted before retained later row"), FMath::IsNearlyEqual(F.Target->Notifies[0].GetTime(), .1f));
     UObject* New = States ? static_cast<UObject*>(F.Target->Notifies[0].NotifyStateClass.Get()) : static_cast<UObject*>(F.Target->Notifies[0].Notify.Get());
     if (TestNotNull(TEXT("new concrete notify object published"), New))
      TestEqual(TEXT("new object belongs to exact target"), New->GetOuter(), static_cast<UObject*>(F.Target.Get()));
    }
   }
  }
  TestTrue(TEXT("invalid property or post-publication failure rolls back"), Transaction.Rollback(TEXT("fixture-after-owner-invoke")).bValid);
  F.CheckRestored(*this);
  for (const auto& New : NewObjects) TestFalse(TEXT("new staged object retired by inverse"), New.IsValid());
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageNotifyOwnerCallbackRollbackTest,
 "UEAssetDoc.AnimationProvider.MontageStructural.NotifyReplacement.CallbackRollback",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageNotifyOwnerCallbackRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentMontageNotifyReplacementTests;
 FFixture F; if (!TestTrue(TEXT("callback regression has actual mixed baseline"), F.Setup())) return false;
 FStagingTransaction Transaction; FResolver Resolver; Resolver.Token = Transaction.Token;
 auto Owner = CreateMontageNotifyRequestOwner(); auto Edit = Intent(false, false, false);
 // First new object rejects placement after configuring; second remains staged.
 Edit.Intent.Elements[0].Elements[0].Elements.Last().Elements[0] = Text(UAssetDocumentMontageNotifyCallbackTestObject::StaticClass()->GetPathName());
 FAssetDocumentRuntimeRequestDto Request; Request.RequestId = Transaction.Token.RequestId; Request.ExpectedRuntimeRevision = Transaction.Token.RuntimeRevision;
 Request.Operation = EAssetDocumentRuntimeOperation::Apply; Request.TargetAssetPath = F.Target->GetPathName(); Request.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
 FAssetDocumentPreparedMutationBatch Batch;
 if (!TestTrue(TEXT("callback intent prepares"), PrepareWithDiagnostics(*this, Owner, Request, Edit, Batch))) return false;
 FAssetDocumentOpaqueHandle Handle; if (!Resolve(*this, F, Resolver, Transaction, Handle) || !TestTrue(TEXT("callback inverse/capture installed"), Transaction.Bind(Batch))) return false;
 FAssetDocumentPrimitiveInvocation Invoke; Invoke.PrimitiveId = Edit.Requirement.PrimitiveId; Invoke.Version = 1; Invoke.RequestId = Request.RequestId;
 Invoke.TransactionId = Transaction.Token.TransactionId; Invoke.RuntimeRevision = Request.ExpectedRuntimeRevision; Invoke.Phase = Edit.Phase; Invoke.ValueArguments = Edit.Intent; Invoke.HandleArguments = {{TEXT("asset"), Handle}};
 TestTrue(TEXT("callback invocation bound to prepared contribution"), Batch.Contributions[0].ValidateInvocation(Invoke, Resolver).bValid);
 const auto Outcome = Owner->Invoke(Invoke, Resolver, &Transaction);
 TestEqual(TEXT("callback rejects actual owner invocation"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
 TestTrue(TEXT("placement callback actually cleared timeline before publication"), F.Target->Notifies.IsEmpty());
 TArray<UObject*> Children; GetObjectsWithOuter(F.Target.Get(), Children, AssetDocumentObjects::DirectOnly);
 TArray<TWeakObjectPtr<UObject>> Created; bool Called = false;
 for (auto* Child : Children)
 {
  if (auto* Callback = Cast<UAssetDocumentMontageNotifyCallbackTestObject>(Child)) Called |= Callback->bPlacementCalled;
  if ((Child->IsA<UAnimNotify>() || Child->IsA<UAnimNotifyState>()) && Child != F.Before[0].Notify && Child != F.Before[1].NotifyStateClass) Created.Emplace(Child);
 }
 TestTrue(TEXT("non-CDO placement callback observed"), Called);
 TestEqual(TEXT("two actual new objects allocated before callback"), Created.Num(), 2);
 TestEqual(TEXT("callback operation enlists once"), Transaction.EnlistCalls, 1);
 TestTrue(TEXT("pre-publication rejection restores consumed baseline"), Transaction.Rollback(TEXT("fixture-placement-callback")).bValid);
 F.CheckRestored(*this);
 for (const auto& New : Created) TestFalse(TEXT("callback staged object retired"), New.IsValid());
 TestEqual(TEXT("callback registers no late action"), Transaction.LateCalls, 0);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageNotifyOwnerOffsetRejectionTest,
 "UEAssetDoc.AnimationProvider.MontageStructural.NotifyReplacement.OffsetRejection",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageNotifyOwnerOffsetRejectionTest::RunTest(const FString&)
{
 using namespace AssetDocumentMontageNotifyReplacementTests;
 for (int32 Index : {0, 1})
 {
  FFixture F; if (!TestTrue(TEXT("offset baseline fixture"), F.Setup())) return false;
  F.Target->Notifies[Index].TriggerTimeOffset = .01f;
  F.Before = F.Target->Notifies;
  FStagingTransaction Transaction; auto Owner = CreateMontageNotifyRequestOwner(); auto Edit = Intent(Index == 1, false, false);
  FAssetDocumentRuntimeRequestDto Request; Request.RequestId = Transaction.Token.RequestId; Request.ExpectedRuntimeRevision = Transaction.Token.RuntimeRevision;
  Request.Operation = EAssetDocumentRuntimeOperation::Apply; Request.TargetAssetPath = F.Target->GetPathName(); Request.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
  FAssetDocumentPreparedMutationBatch Batch;
  TestFalse(TEXT("normalization-changing midtime offset rejected before staging"), PrepareWithDiagnostics(*this, Owner, Request, Edit, Batch));
  TestTrue(TEXT("offset rejection publishes no contribution/capture"), Batch.Contributions.IsEmpty() && !Batch.OpenMutationCapture);
  TestEqual(TEXT("offset rejection has zero footprint"), Transaction.EnlistAttempts, 0);
  F.CheckRestored(*this);
  TestFalse(TEXT("offset rejection preserves clean package"), F.Package->IsDirty());
 }
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageProfileSnapshotTest,
 "UEAssetDoc.AnimationProvider.MontageProfileSnapshot",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageProfileSnapshotTest::RunTest(const FString&)
{
 using namespace AssetDocumentMontageNotifyReplacementTests;
 FFixture F; if (!TestTrue(TEXT("snapshot uses actual mixed Montage fixture"), F.Setup())) return false;
 FStagingTransaction Transaction; FResolver Resolver; Resolver.Token = Transaction.Token;
 FAssetDocumentOpaqueHandle Handle;
 if (!Resolve(*this, F, Resolver, Transaction, Handle, true)) return false;
 TestEqual(TEXT("real Editor payload is readonly"), Handle.Access, EAssetDocumentHandleAccess::ReadOnly);
 auto Owner = CreateMontageNotifyRequestOwner();
 FAssetDocumentRuntimeRequestDto Request;
 Request.RequestId = Transaction.Token.RequestId; Request.ExpectedRuntimeRevision = Transaction.Token.RuntimeRevision;
 Request.Operation = EAssetDocumentRuntimeOperation::Apply; Request.TargetAssetPath = F.Target->GetPathName();
 Request.RequestedAssetClassPath = UAnimMontage::StaticClass()->GetPathName();
 if (!TestTrue(TEXT("snapshot owner binds actual request"), Owner->BindRequest(Request).bValid)) return false;
 FAssetDocumentPrimitiveInvocation Invoke;
 Invoke.PrimitiveId = TEXT("animation.montage.profile.snapshot.v1"); Invoke.Version = 1;
 Invoke.RequestId = Request.RequestId; Invoke.TransactionId = Transaction.Token.TransactionId;
 Invoke.RuntimeRevision = Request.ExpectedRuntimeRevision; Invoke.Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
 Invoke.ValueArguments = Object(); Invoke.HandleArguments = {{TEXT("asset"), Handle}};
 const bool bDirtyBefore = F.Package->IsDirty();
 auto Read = [&](const FAssetDocumentPrimitiveInvocation& I)
 {
  auto Out = Owner->Invoke(I, Resolver, nullptr);
  for (const auto& D : Out.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  return Out;
 };
 const auto Before = Read(Invoke);
 if (!TestEqual(TEXT("Preflight actual snapshot succeeds"), Before.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
 const TArray<FString> Keys = {TEXT("skeleton"), TEXT("preview_mesh"), TEXT("curves"), TEXT("slots"), TEXT("sections"), TEXT("notifies"), TEXT("notify_states"), TEXT("notify_tracks"), TEXT("settings")};
 if (!TestTrue(TEXT("snapshot has exactly nine canonical keys"), Before.Value.ObjectKeys == Keys && Before.Value.Elements.Num() == 9)) return false;
 TestEqual(TEXT("actual skeleton path observed"), Before.Value.Elements[0].StringValue, F.Skeleton->GetPathName());
 TestEqual(TEXT("actual empty preview observed"), Before.Value.Elements[1].Kind, EAssetDocumentRuntimeValueKind::Null);
 TestEqual(TEXT("actual float curve count observed"), Before.Value.Elements[2].Elements.Num(), F.Target->GetDataModel()->GetFloatCurves().Num());
 const auto& Slots = Before.Value.Elements[3].Elements;
 if (TestEqual(TEXT("actual slots observed"), Slots.Num(), F.Target->SlotAnimTracks.Num()) && !Slots.IsEmpty())
 {
  const auto& Segments = Slots[0].Elements[1].Elements;
  if (TestEqual(TEXT("actual segment count observed"), Segments.Num(), 1))
   TestEqual(TEXT("actual segment source observed"), Segments[0].Elements[0].StringValue, F.Source->GetPathName());
 }
 TestEqual(TEXT("actual section count observed"), Before.Value.Elements[4].Elements.Num(), F.Target->CompositeSections.Num());
 auto* Sound = CastChecked<UAnimNotify_PlaySound>(F.Target->Notifies[0].Notify.Get());
 auto Volume = [&](const FAssetDocumentPrimitiveOutcome& Out, double& Value)
 {
  if (Out.Value.Elements.Num() != 7) return false;
  const auto& Rows = Out.Value.Elements[5].Elements;
  if (Rows.Num() != 1 || Rows[0].Elements.Num() != 3) return false;
  const auto& Item = Rows[0].Elements[2];
  if (Item.Elements.Num() != 2) return false;
  const auto& Properties = Item.Elements[1];
  const int32 Index = Properties.ObjectKeys.IndexOfByKey(TEXT("VolumeMultiplier"));
  if (!Properties.Elements.IsValidIndex(Index) || Properties.Elements[Index].Kind != EAssetDocumentRuntimeValueKind::Number) return false;
  Value = Properties.Elements[Index].NumberValue;
  return true;
 };
 double OldVolume = 0.0;
 if (!TestTrue(TEXT("actual admitted sound property is present"), Volume(Before, OldVolume))) return false;
 TestEqual(TEXT("Preflight reads actual sound volume"), OldVolume, double(Sound->VolumeMultiplier));
 for (int32 Family : {5, 6})
 {
  const auto& Rows = Before.Value.Elements[Family].Elements;
  if (TestEqual(TEXT("one actual row per managed family"), Rows.Num(), 1))
   TestTrue(TEXT("actual managed event time observed"), FMath::IsNearlyEqual(float(Rows[0].Elements[0].NumberValue), F.Target->Notifies[Family - 5].GetTime()));
 }
 Sound->VolumeMultiplier = 0.375f;
 Invoke.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
 const auto After = Read(Invoke);
 TestEqual(TEXT("Verify actual snapshot succeeds"), After.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 double NewVolume = 0.0;
 if (TestTrue(TEXT("Verify reads actual changed property"), Volume(After, NewVolume)))
 {
  TestEqual(TEXT("Verify observes live value rather than previous echo"), NewVolume, double(Sound->VolumeMultiplier));
  TestNotEqual(TEXT("fresh value differs from Preflight"), NewVolume, OldVolume);
 }
 TestEqual(TEXT("active transaction rejected"), Owner->Invoke(Invoke, Resolver, &Transaction).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
 auto Wrong = Invoke; Wrong.RequestId = FGuid::NewGuid();
 TestEqual(TEXT("other request rejected"), Read(Wrong).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
 Wrong = Invoke; ++Wrong.RuntimeRevision;
 TestEqual(TEXT("other revision rejected"), Read(Wrong).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
 Owner->CloseRequest();
 TestEqual(TEXT("closed request rejected"), Read(Invoke).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
 TestTrue(TEXT("snapshots publish no changed set"), Before.ChangedSet.IsEmpty() && After.ChangedSet.IsEmpty());
 TestEqual(TEXT("snapshot has zero footprint enlist attempts"), Transaction.EnlistAttempts, 0);
 TestEqual(TEXT("snapshot registers no action or finalizer"), Transaction.LateCalls, 0);
 TestEqual(TEXT("reads preserve package dirty state"), F.Package->IsDirty(), bDirtyBefore);
 TestEqual(TEXT("reader retains actual sound property"), Sound->VolumeMultiplier, .375f);
 F.CheckRestored(*this);
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageProductionStagedSnapshotTest,
 "UEAssetDoc.AnimationProvider.MontageProfileSnapshot.ProductionStagedPath",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageProductionStagedSnapshotTest::RunTest(const FString&)
{
 using namespace AssetDocumentMontageNotifyReplacementTests;
 const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<UPackage> OriginalPackage(CreatePackage(*(TEXT("/Game/AssetDocumentTests/StagedSnapshot_") + Id)));
 TStrongObjectPtr<UAnimMontage> Original(NewObject<UAnimMontage>(OriginalPackage.Get(), TEXT("Target"), RF_Public | RF_Standalone));
 Original->GetController().InitializeModel(); Original->GetController().NotifyPopulated();
 TestNull(TEXT("empty source carries no staged Skeleton reference"), Original->GetSkeleton());
 TestEqual(TEXT("empty source has zero authored model frames"), Original->GetDataModel()->GetNumberOfFrames(), 0);
 const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/MontageProductionStage") / Id);
 const FString Filename = Directory / TEXT("Target.uasset");
 IFileManager::Get().MakeDirectory(*Directory, true);
 TArray<TStrongObjectPtr<UPackage>> LoadedPackages;
 ON_SCOPE_EXIT
 {
  OriginalPackage->SetDirtyFlag(false);
  for (const auto& Package : LoadedPackages) Package->SetDirtyFlag(false);
  IFileManager::Get().DeleteDirectory(*Directory, false, true);
 };
 FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
 if (!TestTrue(TEXT("real empty source saved"), UPackage::SavePackage(OriginalPackage.Get(), Original.Get(), *Filename, Save))) return false;
 for (int32 Case = 0; Case < 4; ++Case)
 {
  const FString Prefix = Case == 3 ? TEXT("/Temp/AssetDocument/Runtime/NotStaging/") : TEXT("/Temp/AssetDocument/Runtime/Staging/");
  const FString StagePackageName = Prefix + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/Target");
  UPackage* StagePackage = CreatePackage(*StagePackageName); LoadedPackages.Emplace(StagePackage);
  if (!TestTrue(TEXT("actual staged file loads into requested package"), LoadPackage(StagePackage, FPackagePath::FromLocalPath(Filename), LOAD_None) == StagePackage)) return false;
  UAnimMontage* Staged = FindObject<UAnimMontage>(StagePackage, TEXT("Target"));
  if (!TestNotNull(TEXT("loaded stage has exact Montage"), Staged)) return false;
  TestNotEqual(TEXT("staged identity is distinct from original live target"), Staged, Original.Get());
  FStagingTransaction Transaction; FResolver Resolver; Resolver.Token = Transaction.Token;
  FAssetDocumentRuntimeRequestDto Request;
  Request.RequestId = Transaction.Token.RequestId; Request.ExpectedRuntimeRevision = Transaction.Token.RuntimeRevision;
  Request.Operation = EAssetDocumentRuntimeOperation::Apply; Request.TargetAssetPath = Original->GetPathName();
  Request.RequestedAssetClassPath = UAnimMontage::StaticClass()->GetPathName();
  auto Owner = CreateMontageNotifyRequestOwner();
  if (!TestTrue(TEXT("stage owner binds actual request"), Owner->BindRequest(Request).bValid)) return false;
  FAssetDocumentPreparedMutationBatch Batch;
  if (!TestTrue(TEXT("empty Body prepares without fabricated structural mutation"), Owner->Prepare(Request, {}, Batch).bValid)) return false;
  TestTrue(TEXT("empty Body has no mutation contribution"), Batch.Contributions.IsEmpty());
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  auto StageBinding = Owner->BindStagedRead(*Staged, Filename, Request.ExpectedRuntimeRevision, Diagnostics);
  for (const auto& D : Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  if (!TestTrue(TEXT("owner retains actual loaded stage binding"), StageBinding.IsValid())) return false;
  FAssetDocumentHandleValidationContext Verify;
  Verify.RequestId = Request.RequestId; Verify.TransactionId = Transaction.Token.TransactionId;
  Verify.RuntimeRevision = Request.ExpectedRuntimeRevision; Verify.ProviderId = TEXT("editor");
  Verify.Phase = EAssetDocumentRuntimeRequestPhase::Verify; Verify.RequiredKind = EAssetDocumentHandleKind::Asset; Verify.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
  auto ReadScope = ResolveAssetDocumentEditorStagedMontageRead(Request, Verify, Staged->GetPathName(), Filename, Resolver, Diagnostics);
  for (const auto& D : Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  if (!TestTrue(TEXT("Editor issues only real staged readonly payload"), ReadScope.IsValid())) return false;
  auto Handle = ReadScope->GetAssetReadHandle();
  TestEqual(TEXT("stage issuance has readonly authority"), Handle.Access, EAssetDocumentHandleAccess::ReadOnly);
  // Reuse this fixture's provider alias over the very same stored Editor payload;
  // production registry handoff/storage is exercised by Integration, not faked here.
  Handle.ProviderId = TEXT("animation");
  FAssetDocumentPrimitiveInvocation Invoke;
  Invoke.PrimitiveId = TEXT("animation.montage.profile.snapshot.v1"); Invoke.Version = 1;
  Invoke.RequestId = Request.RequestId; Invoke.TransactionId = Transaction.Token.TransactionId;
  Invoke.RuntimeRevision = Request.ExpectedRuntimeRevision; Invoke.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  Invoke.ValueArguments = Object(); Invoke.HandleArguments = {{TEXT("asset"), Handle}};
  TestEqual(TEXT("ordinary unbound snapshot never gains staged mount authority"),
   InvokeAnimationMontageProfileSnapshot(Invoke, Resolver, nullptr).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  if (Case == 1)
  {
   FAssetDocumentPrimitiveInvocation ResolveLive = Invoke;
   ResolveLive.PrimitiveId = TEXT("editor.reference.resolve_asset.v1"); ResolveLive.Phase = EAssetDocumentRuntimeRequestPhase::Preflight; ResolveLive.HandleArguments.Reset();
   ResolveLive.ValueArguments = Object();
   Field(ResolveLive.ValueArguments, TEXT("expected_class"), Text(UAnimMontage::StaticClass()->GetPathName()));
   Field(ResolveLive.ValueArguments, TEXT("path"), Text(Original->GetPathName()));
   const auto Live = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(ResolveLive, Resolver, nullptr);
   if (!TestEqual(TEXT("original live reference resolves readonly"), Live.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
   bool bFound = false;
   for (const auto& Group : Live.HandleResults) if (Group.Name == TEXT("asset") && Group.Handles.Num() == 1)
   {
    auto LiveHandle = Group.Handles[0]; LiveHandle.ProviderId = TEXT("animation");
    Invoke.HandleArguments = {{TEXT("asset"), LiveHandle}}; bFound = true;
   }
   if (!TestTrue(TEXT("original live payload was actually issued"), bFound)) return false;
  }
  if (Case == 0)
  {
   auto WrongPhase = Invoke; WrongPhase.Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
   TestEqual(TEXT("staged binding admits Verify only"), Owner->Invoke(WrongPhase, Resolver, nullptr).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  }
  if (Case == 2) ReadScope->Close();
  const auto Out = Owner->Invoke(Invoke, Resolver, nullptr);
  for (const auto& D : Out.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  TestEqual(TEXT("only live production staged binding succeeds"), Out.Status,
   Case == 0 ? EAssetDocumentPrimitiveOutcomeStatus::Succeeded : EAssetDocumentPrimitiveOutcomeStatus::Failed);
  if (Case == 0)
  {
   TestEqual(TEXT("production stage returns all nine actual profile keys"), Out.Value.ObjectKeys,
    TArray<FString>{TEXT("skeleton"), TEXT("preview_mesh"), TEXT("curves"), TEXT("slots"), TEXT("sections"), TEXT("notifies"), TEXT("notify_states"), TEXT("notify_tracks"), TEXT("settings")});
   if (TestEqual(TEXT("nine complete values returned"), Out.Value.Elements.Num(), 9))
   {
    TestEqual(TEXT("staged null Skeleton is read from actual object"), Out.Value.Elements[0].Kind, EAssetDocumentRuntimeValueKind::Null);
    TestEqual(TEXT("staged empty preview is read from actual object"), Out.Value.Elements[1].Kind, EAssetDocumentRuntimeValueKind::Null);
    TestEqual(TEXT("staged actual curve array"), Out.Value.Elements[2].Elements.Num(), Staged->GetDataModel()->GetFloatCurves().Num());
    TestEqual(TEXT("staged actual slot array"), Out.Value.Elements[3].Elements.Num(), Staged->SlotAnimTracks.Num());
   }
   StageBinding->Close();
   TestEqual(TEXT("closed owner staged binding rejects further verification"), Owner->Invoke(Invoke, Resolver, nullptr).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  }
  else TestTrue(TEXT("rejection carries diagnostics"), !Out.Diagnostics.IsEmpty());
  TestTrue(TEXT("staged reads never report native mutation or child handles"), Out.ChangedSet.IsEmpty() && Out.HandleResults.IsEmpty());
  TestEqual(TEXT("staged Verify never enlists a mutation"), Transaction.EnlistAttempts, 0);
  ReadScope->Close(); StageBinding->Close(); Owner->CloseRequest();
 }
 return !HasAnyErrors();
}

#endif
