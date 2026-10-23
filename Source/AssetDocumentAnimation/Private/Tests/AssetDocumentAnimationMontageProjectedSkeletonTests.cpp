// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS

#include "Provider/Montage/MontageStructuralRollback.h"
#include "Provider/Montage/MontageStructuralPreparation.h"
#include "Provider/Montage/MontageStructuralProvider.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonPreparation.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonOperations.h"
#include "AssetDocumentAnimationPrimitiveProvider.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimNotifies/AnimNotifyState_TimedParticleEffect.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentMontageProjectedSkeletonTests
{
using namespace AssetDocumentMontage;
struct FFixture
{
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<USkeleton> Skeleton;
 TStrongObjectPtr<UAnimSequence> Source;
 TStrongObjectPtr<UAnimMontage> Target;
 bool bReady = false;
 explicit FFixture(FAutomationTestBase& Test)
  : Package(CreatePackage(*FString::Printf(TEXT("/Game/AssetDocumentTests/MontageStructural_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))),
    Skeleton(NewObject<USkeleton>(Package.Get(), TEXT("Skeleton"))),
    Source(NewObject<UAnimSequence>(Package.Get(), TEXT("Source"))),
    Target(NewObject<UAnimMontage>(Package.Get(), TEXT("Target"), RF_Public | RF_Transactional))
 {
  {
   FReferenceSkeletonModifier Modifier(Skeleton.Get());
   Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
  }
  for (FName Slot : {FName(TEXT("A")), FName(TEXT("B")), FName(TEXT("C"))}) Skeleton->RegisterSlotNode(Slot);
  Source->SetSkeleton(Skeleton.Get());
  auto& Controller = Source->GetController();
  Controller.InitializeModel();
  Controller.SetFrameRate(FFrameRate(30, 1), false);
  Controller.SetNumberOfFrames(FFrameNumber(30), false);
  if (!Test.TestTrue(TEXT("source root curve added"), Controller.AddBoneCurve(TEXT("root"), false))) return;
  TArray<FVector3f> Positions, Scales; TArray<FQuat4f> Rotations;
  Positions.Init(FVector3f::ZeroVector, 31); Scales.Init(FVector3f::OneVector, 31); Rotations.Init(FQuat4f::Identity, 31);
  if (!Test.TestTrue(TEXT("source root keys populated"), Controller.SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false))) return;
  Controller.NotifyPopulated();
  Source->WaitOnExistingCompression();
  Target->SetSkeleton(Skeleton.Get());
  Target->SyncSlotIndex = INDEX_NONE;
  Target->GetController().InitializeModel();
  Target->GetController().SetFrameRate(FFrameRate(30, 1), false);
  Target->GetController().SetNumberOfFrames(FFrameNumber(30), false);
  Target->GetController().NotifyPopulated();
  // Test-authored preimage, independent of the publication helpers under test.
  Target->SlotAnimTracks.Reset();
  for (FName Name : {FName(TEXT("A")), FName(TEXT("B"))})
  {
   auto& Slot = Target->SlotAnimTracks.AddDefaulted_GetRef(); Slot.SlotName = Name;
   auto& Segment = Slot.AnimTrack.AnimSegments.AddDefaulted_GetRef();
   Segment.SetAnimReference(Source.Get()); Segment.StartPos = 0.f;
   Segment.AnimStartTime = 0.f; Segment.AnimEndTime = 1.f; Segment.AnimPlayRate = 1.f; Segment.LoopingCount = 1;
  }
  Target->CompositeSections.Reset();
  auto& Absolute = Target->CompositeSections.AddDefaulted_GetRef();
  Absolute.SectionName = TEXT("Absolute"); Absolute.NextSectionName = TEXT("Relative");
  Absolute.Link(Target.Get(), 0.f, 0);
  auto& Relative = Target->CompositeSections.AddDefaulted_GetRef();
  Relative.SectionName = TEXT("Relative"); Relative.NextSectionName = NAME_None;
  Relative.Link(Target.Get(), .5f, 0); Relative.ChangeLinkMethod(EAnimLinkMethod::Relative);
  // The abstract base CDO is only an immutable identity sentinel, never instantiated or modified.
  Relative.MetaData.Add(GetMutableDefault<UAnimMetaData>());
  Target->AnimNotifyTracks.Reset();
  Target->AnimNotifyTracks.Add(FAnimNotifyTrack(TEXT("Retained"), FLinearColor::White));
  Target->AnimNotifyTracks.Add(FAnimNotifyTrack(TEXT("Instant"), FLinearColor::White));
  auto& Notify = Target->Notifies.AddDefaulted_GetRef();
  Notify.NotifyName = TEXT("Retained"); Notify.Guid = FGuid::NewGuid();
  Notify.NotifyStateClass = NewObject<UAnimNotifyState_TimedParticleEffect>(Target.Get());
  Notify.TrackIndex = 0;
  Notify.Link(Target.Get(), .25f, 0); Notify.ChangeLinkMethod(EAnimLinkMethod::Relative);
  Notify.EndLink.Link(Target.Get(), .75f, 0); Notify.EndLink.ChangeLinkMethod(EAnimLinkMethod::Relative);
  Notify.SetDuration(.5f);
  auto& Instant = Target->Notifies.AddDefaulted_GetRef();
  Instant.NotifyName = TEXT("RetainedInstant"); Instant.Guid = FGuid::NewGuid(); Instant.TrackIndex = 1;
  Instant.Link(Target.Get(), .875f, 0); Instant.ChangeLinkMethod(EAnimLinkMethod::Relative);
  Target->RefreshCacheData();
  Package->SetDirtyFlag(false);
  bReady = Test.TestTrue(TEXT("nonempty montage has initialized frame model"), Target->GetDataModel()->GetNumberOfFrames() == 30)
   && Test.TestTrue(TEXT("source sequence has one second of actual data"), FMath::IsNearlyEqual(Source->GetPlayLength(), 1.f));
 }
 ~FFixture() { Package->SetDirtyFlag(false); }
 FStructuralIntent Slots() const
 {
  FStructuralIntent Intent; TArray<FSlotEdit> Rows;
  for (const auto& Native : Target->SlotAnimTracks)
  {
   FSlotEdit Row; Row.Name = Native.SlotName;
   for (int32 Index = 0; Index < Native.AnimTrack.AnimSegments.Num(); ++Index)
   {
    const auto& S = Native.AnimTrack.AnimSegments[Index]; FSegmentEdit Edit;
    Edit.Animation = Source.Get(); Edit.Start = S.StartPos; Edit.SourceStart = S.AnimStartTime;
    Edit.SourceEnd = S.AnimEndTime; Edit.Rate = S.AnimPlayRate; Edit.Loops = S.LoopingCount;
    Edit.PreviousSlot = Native.SlotName; Edit.PreviousSegment = Index;
    Row.Segments.Add(Edit);
   }
   Rows.Add(Row);
  }
  Intent.Slots = MoveTemp(Rows); return Intent;
 }
};

struct FSnapshot
{
 TArray<FSlotAnimationTrack> Slots;
 TArray<FCompositeSection> Sections;
 TArray<FAnimNotifyEvent> Notifies;
 int32 Frames;
 explicit FSnapshot(const UAnimMontage& M)
  : Slots(M.SlotAnimTracks), Sections(M.CompositeSections), Notifies(M.Notifies), Frames(M.GetDataModel()->GetNumberOfFrames()) {}
 template <typename T> static bool Same(const TArray<T>& A, const TArray<T>& B)
 {
  if (A.Num() != B.Num()) return false;
  for (int32 Index = 0; Index < A.Num(); ++Index)
   if (!T::StaticStruct()->CompareScriptStruct(&A[Index], &B[Index], 0)) return false;
  return true;
 }
 void Check(FAutomationTestBase& Test, const UAnimMontage& M) const
 {
  Test.TestTrue(TEXT("slots equal preimage"), Same(Slots, M.SlotAnimTracks));
  Test.TestTrue(TEXT("sections and metadata equal preimage"), Same(Sections, M.CompositeSections));
  Test.TestTrue(TEXT("notifies and links equal preimage"), Same(Notifies, M.Notifies));
  Test.TestEqual(TEXT("frame count equals preimage"), M.GetDataModel()->GetNumberOfFrames(), Frames);
 }
};

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

class FProjectedSkeletonTransaction final : public IAssetDocumentAtomicTransaction
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
 EAssetDocumentHandleAccess Access, FResolver& Resolver, FProjectedSkeletonTransaction& Transaction)
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageProjectedSkeletonRollback,
 "UEAssetDoc.AnimationProvider.MontageStructural.ProjectedSkeleton",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageProjectedSkeletonRollback::RunTest(const FString&)
{
 using namespace AssetDocumentMontageProjectedSkeletonTests;
 // Prefix failures exercise the pre-enlisted Skeleton inverse even though its
 // forward operation never ran. Later cases use actual provider setter/readback.
 for (int32 Case = 0; Case < 9; ++Case)
 {
  FFixture F(*this); if (!F.bReady) return false;
  F.Skeleton->SetFlags(RF_Public); F.Source->SetFlags(RF_Public);
  TStrongObjectPtr<USkeleton> Desired(NewObject<USkeleton>(F.Package.Get(), TEXT("Desired"), RF_Public));
  { FReferenceSkeletonModifier Modifier(Desired.Get());
    Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); }
  for (FName Slot : {FName(TEXT("A")), FName(TEXT("B"))}) Desired->RegisterSlotNode(Slot);
  Desired->AddCompatibleSkeleton(F.Skeleton.Get());
  if (!TestTrue(TEXT("fixture declares actual editor Skeleton compatibility"), Desired->IsCompatibleForEditor(F.Skeleton.Get()))) return false;
  const FSnapshot Before(*F.Target);
  FProjectedSkeletonTransaction Transaction; FResolver Resolver;
  Transaction.Token = {FGuid::NewGuid(), FGuid::NewGuid(), 1}; Resolver.Token = Transaction.Token;
  FAssetDocumentRuntimeRequestDto Request;
  Request.RequestId = Transaction.Token.RequestId; Request.ExpectedRuntimeRevision = 1;
  Request.Operation = EAssetDocumentRuntimeOperation::Apply; Request.TargetAssetPath = F.Target->GetPathName();
  Request.RequestedAssetClassPath = UAnimMontage::StaticClass()->GetPathName();
  auto Intent = F.Slots(); (*Intent.Slots)[0].Segments[0].Loops = 2;
  if (Case == 0)
  {
   Desired->RegisterSlotNode(TEXT("OnlyDesired"));
   auto Unsupported = Intent; (*Unsupported.Slots)[1].Name = TEXT("OnlyDesired");
   TSharedPtr<FPreparedStructuralEdit> Rejected;
   TestFalse(TEXT("slots accepted only by the new Skeleton remain outside this projection"),
    PrepareMontageStructuralEdit(*F.Target, Unsupported, Request.RequestId, 1, Rejected).bValid);
   TestFalse(TEXT("unsupported migration publishes no prepared plan"), Rejected.IsValid());
   Before.Check(*this, *F.Target);
  }
  TSharedPtr<FPreparedStructuralEdit> Structure;
  if (!TestTrue(TEXT("prepare original structural plan"), PrepareMontageStructuralEdit(*F.Target, Intent, Request.RequestId, 1, Structure).bValid)) return false;
  FAssetDocumentPreparedMutationBatch StructuralBatch, SkeletonBatch; FAssetDocumentMutationIntent StructuralIntent;
  if (!TestTrue(TEXT("prepare structural capture"), MakeMontageStructuralPreparedBatch(Structure.ToSharedRef(), 0,
   EAssetDocumentRuntimeRequestPhase::Apply, StructuralBatch, StructuralIntent).bValid)) return false;
  USkeleton* Reference = Case == 4 ? F.Skeleton.Get() : Desired.Get();
  FAssetDocumentMutationIntent SkeletonIntent;
  SkeletonIntent.StepIndex = 1; SkeletonIntent.Requirement = {TEXT("animation.asset.skeleton.set.v1"), 1, TEXT("animation_profile")};
  SkeletonIntent.InputSchemaId = TEXT("animation.asset.skeleton.set.v1.input.v1"); SkeletonIntent.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  SkeletonIntent.Intent.Kind = EAssetDocumentRuntimeValueKind::Object;
  SkeletonIntent.Intent.ObjectKeys = {TEXT("operation"), TEXT("skeleton_path")}; SkeletonIntent.Intent.Elements.SetNum(2);
  for (auto& Value : SkeletonIntent.Intent.Elements) Value.Kind = EAssetDocumentRuntimeValueKind::String;
  SkeletonIntent.Intent.Elements[0].StringValue = TEXT("set"); SkeletonIntent.Intent.Elements[1].StringValue = Reference->GetPathName();
  const auto SkeletonPreparation = PrepareAnimationSkeletonMutationBatchAfterStructure(
   Request, SkeletonIntent, Structure.ToSharedRef(), SkeletonBatch);
  if (!TestTrue(TEXT("prepare Skeleton against immutable desired tracks"), SkeletonPreparation.bValid))
  {
   for (const auto& Diagnostic : SkeletonPreparation.Diagnostics) AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
   return false;
  }
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  auto StructureCapture = StructuralBatch.OpenMutationCapture(Transaction.Token.TransactionId, Diagnostics);
  auto SkeletonCapture = SkeletonBatch.OpenMutationCapture(Transaction.Token.TransactionId, Diagnostics);
  if (!TestTrue(TEXT("both captures open against original preimage"), StructureCapture && SkeletonCapture)) return false;
  const auto Asset = Resolve(*this, *F.Target, EAssetDocumentHandleAccess::Mutable, Resolver, Transaction);
  const auto Ref = Resolve(*this, *Reference, EAssetDocumentHandleAccess::ReadOnly, Resolver, Transaction);
  if (!Asset.IsSet() || !Ref.IsSet()) return false;
  FAssetDocumentPrimitiveInvocation Invocation;
  Invocation.PrimitiveId = SkeletonIntent.Requirement.PrimitiveId; Invocation.Version = 1; Invocation.RequestId = Request.RequestId;
  Invocation.RuntimeRevision = 1; Invocation.TransactionId = Transaction.Token.TransactionId; Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  Invocation.HandleArguments = {{TEXT("asset"), Asset.GetValue()}, {TEXT("skeleton"), Ref.GetValue()}};
  Invocation.ValueArguments = SkeletonIntent.Intent; Invocation.ValueArguments.ObjectKeys.SetNum(1); Invocation.ValueArguments.Elements.SetNum(1);
  TestFalse(TEXT("Skeleton cannot run before its structural dependency"), SkeletonBatch.Contributions[0].ValidateInvocation(Invocation, Resolver).bValid);
  if (Case != 0)
  {
   const auto Failure = Case <= 3 ? static_cast<Testing::EFailurePoint>(Case) : Testing::EFailurePoint::None;
   const auto Applied = Testing::ApplyNative(Structure.ToSharedRef(), Transaction, Failure);
   TestEqual(TEXT("only injected structural prefix fails"), Applied.bValid, Case > 3);
   TestTrue(TEXT("actual publisher records the projected rollback phase"), Structure->bSlotsPublished);
  }
  if (Case >= 4)
  {
   if (Case == 8)
   {
    F.Source->SetSkeleton(Desired.Get());
    TestFalse(TEXT("projected source Skeleton drift rejects"), SkeletonBatch.Contributions[0].ValidateInvocation(Invocation, Resolver).bValid);
    F.Source->SetSkeleton(F.Skeleton.Get()); // restore only test-induced dependency drift
   }
   else if (Case == 7)
   {
    F.Target->SlotAnimTracks[0].AnimTrack.AnimSegments[0].LoopingCount = 3;
    TestFalse(TEXT("unplanned projected track drift rejects"), SkeletonBatch.Contributions[0].ValidateInvocation(Invocation, Resolver).bValid);
    F.Target->SlotAnimTracks[0].AnimTrack.AnimSegments[0].LoopingCount = 2; // fixture cleanup, not provider repair
   }
   else
   {
    if (!TestTrue(TEXT("published desired tracks satisfy Skeleton validator"), SkeletonBatch.Contributions[0].ValidateInvocation(Invocation, Resolver).bValid)) return false;
    AssetDocumentAnimationSkeletonTesting::FHooks Hooks; Hooks.bFailForwardReadback = Case == 6;
    AssetDocumentAnimationSkeletonTesting::SetHooks(&Hooks);
    const auto Outcome = CreateAssetDocumentAnimationPrimitiveProvider()->Invoke(Invocation, Resolver, &Transaction);
    AssetDocumentAnimationSkeletonTesting::SetHooks(nullptr);
    TestEqual(TEXT("Skeleton setter/noop and injected readback result"), Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded, Case != 6);
    TestEqual(TEXT("explicit noop does not set Skeleton"), Hooks.ForwardSetterCalls, Case == 4 ? 0 : 1);
   }
  }
  SkeletonCapture.Reset(); StructureCapture.Reset();
  if (!TestTrue(TEXT("Skeleton inverse accepts its exact old/projected phase first"), SkeletonBatch.Contributions[0].InitialAction->Rollback(TEXT("ProjectedTest")).bValid)) return false;
  TestTrue(TEXT("Skeleton restored before structural inverse"), F.Target->GetSkeleton() == F.Skeleton.Get());
  if (!TestTrue(TEXT("structure inverse then restores full original timeline"), StructuralBatch.Contributions[0].InitialAction->Rollback(TEXT("ProjectedTest")).bValid)) return false;
  Before.Check(*this, *F.Target);
 }
 return true;
}
#endif
