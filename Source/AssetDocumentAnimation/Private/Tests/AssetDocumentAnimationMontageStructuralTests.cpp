// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS

#include "Provider/Montage/MontageStructuralRollback.h"
#include "Provider/Montage/MontageStructuralPreparation.h"
#include "Provider/Montage/MontageStructuralProvider.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimNotifies/AnimNotifyState_TimedParticleEffect.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentMontageStructuralTests
{
using namespace AssetDocumentMontage;

// Exercises native publication/inverse only. Verified leases, preparation transport,
// durable commit and shared P-ANIM wiring require the later integration fixture.
class FMontageStructuralTransaction final : public IAssetDocumentAtomicTransaction
{
public:
 FAssetDocumentMutationToken Token;
 TArray<FAssetDocumentMutationFootprint> Footprints;
 TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
 TArray<FAssetDocumentMutationFootprint> PreparedFootprint;
 TUniquePtr<FAssetDocumentCoreMutationCaptureLease> Capture;
 int32 LateActionCalls = 0;
 bool BindPrepared(TSharedRef<FPreparedStructuralEdit> Prepared)
 {
  FAssetDocumentPreparedMutationBatch Batch; FAssetDocumentMutationIntent Intent;
  if (!MakeMontageStructuralPreparedBatch(Prepared, 0, EAssetDocumentRuntimeRequestPhase::Apply, Batch, Intent).bValid
   || Batch.Contributions.Num() != 1 || !Batch.Contributions[0].InitialAction || !Batch.OpenMutationCapture) return false;
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  Capture = Batch.OpenMutationCapture(Token.TransactionId, Diagnostics);
  if (!Capture || !Capture->IsValid()) return false;
  // Test-local adapter harness uses the real prepared contribution and capture.
  // Shared composition's full registry/transport invocation remains a later integration test.
  Actions.Add(Batch.Contributions[0].InitialAction.ToSharedRef());
  PreparedFootprint = Batch.Contributions[0].RawFootprint;
  State = EAssetDocumentAtomicState::Mutating;
  return true;
 }
 EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Live;
 FMontageStructuralTransaction()
 {
  Token.RequestId = FGuid::NewGuid(); Token.TransactionId = FGuid::NewGuid(); Token.RuntimeRevision = 1;
 }
 FGuid GetTransactionId() const override { return Token.TransactionId; }
 EAssetDocumentAtomicState GetState() const override { return State; }
 const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
 FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Items) override
 {
  if (State != EAssetDocumentAtomicState::Mutating || !Footprints.IsEmpty() || Items.Num() != PreparedFootprint.Num()) return Result(false);
  for (int32 I = 0; I < Items.Num(); ++I)
   if (Items[I].Kind != PreparedFootprint[I].Kind || Items[I].StableId != PreparedFootprint[I].StableId
    || Items[I].RollbackComponentId != PreparedFootprint[I].RollbackComponentId) return Result(false);
  for (const auto& Item : Items) Footprints.Add(Item);
  return Result(true);
 }
 FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction> Action) override
 { ++LateActionCalls; return Result(false, TEXT("MutationComposition.LateAction")); }
 FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
 { return Result(false); } // This structural slice must own its inverse action.
 FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Result(false); }
 FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return Result(false); }
 FAssetDocumentContractValidationResult Rollback(FStringView Failure) override
 {
  Capture.Reset();
  for (int32 Index = Actions.Num() - 1; Index >= 0; --Index)
  {
   auto R = Actions[Index]->Rollback(Failure);
   if (!R.bValid) { State = EAssetDocumentAtomicState::Poisoned; return R; }
  }
  State = EAssetDocumentAtomicState::RolledBack;
  return Result(true);
 }
};

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

TSharedPtr<FPreparedStructuralEdit> Prepare(FAutomationTestBase& Test, FFixture& F,
 const FStructuralIntent& Intent, FMontageStructuralTransaction& Transaction)
{
 TSharedPtr<FPreparedStructuralEdit> Prepared;
 const auto R = PrepareMontageStructuralEdit(*F.Target, Intent, Transaction.Token.RequestId,
  Transaction.Token.RuntimeRevision, Prepared);
 Test.TestTrue(TEXT("valid structural intent prepares"), R.bValid);
 Test.TestTrue(TEXT("prepared native state exists"), Prepared.IsValid());
 if (Prepared.IsValid() && !Test.TestTrue(TEXT("real prepared capture binds before Mutating apply"), Transaction.BindPrepared(Prepared.ToSharedRef()))) return {};
 return Prepared;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageStructuralSlotEdits,
 "UEAssetDoc.AnimationProvider.MontageStructural.Slots",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageStructuralSlotEdits::RunTest(const FString&)
{
 using namespace AssetDocumentMontageStructuralTests;
 for (int32 Case = 0; Case < 4; ++Case)
 {
  FFixture F(*this); if (!F.bReady) return false;
  FSnapshot Before(*F.Target); FMontageStructuralTransaction Transaction; auto Intent = F.Slots(); auto& Slots = Intent.Slots.GetValue();
  if (Case == 0) { auto Added = Slots[1]; Added.Name = TEXT("C"); Added.Segments[0].PreviousSlot = NAME_None; Added.Segments[0].PreviousSegment = INDEX_NONE; Slots.Add(Added); }
  if (Case == 1) Slots[0].Segments[0].Loops = 2;
  if (Case == 2) Slots.RemoveAt(1);
  if (Case == 3)
  {
   Slots.Swap(0, 1);
   TSharedPtr<FPreparedStructuralEdit> Rejected;
   TestFalse(TEXT("slot reorder requiring retained links on nonzero slot rejects before mutation"),
    PrepareMontageStructuralEdit(*F.Target, Intent, Transaction.Token.RequestId, Transaction.Token.RuntimeRevision, Rejected).bValid);
   TestFalse(TEXT("non-durable slot reorder publishes no prepared operation"), Rejected.IsValid());
   Before.Check(*this, *F.Target);
   TestTrue(TEXT("rejected reorder preserves clean package"), !F.Package->IsDirty());
   TestTrue(TEXT("rejected reorder enlists no action or footprint"), Transaction.Actions.IsEmpty() && Transaction.Footprints.IsEmpty());
   continue;
  }
  auto Prepared = Prepare(*this, F, Intent, Transaction); if (!Prepared.IsValid()) continue;
  Before.Check(*this, *F.Target);
  if (!TestTrue(TEXT("slot edit applies"), Testing::ApplyNative(Prepared.ToSharedRef(), Transaction).bValid)) continue;
  TestEqual(TEXT("requested slot count"), F.Target->SlotAnimTracks.Num(), Slots.Num());
  for (int32 Index = 0; Index < Slots.Num(); ++Index)
   TestEqual(TEXT("requested slot order"), F.Target->SlotAnimTracks[Index].SlotName, Slots[Index].Name);
  TestEqual(TEXT("length controller frame count"), F.Target->GetDataModel()->GetNumberOfFrames(), Case == 1 ? 60 : 30);
  TestEqual(TEXT("requested segment loop count"), F.Target->SlotAnimTracks[0].AnimTrack.AnimSegments[0].LoopingCount, Case == 1 ? 2 : 1);
  TestEqual(TEXT("absolute section retains absolute time"), F.Target->CompositeSections[0].GetTime(), 0.f);
  TestEqual(TEXT("relative section time survives loop-count edit"), F.Target->CompositeSections[1].GetTime(), .5f);
  TestEqual(TEXT("retained section stays on durable slot zero"), F.Target->CompositeSections[1].GetSlotIndex(), 0);
  TestEqual(TEXT("retained relative link method"), F.Target->CompositeSections[1].GetLinkMethod(), EAnimLinkMethod::Relative);
  TestEqual(TEXT("notify start survives loop-count edit"), F.Target->Notifies[0].GetTime(), .25f);
  TestEqual(TEXT("notify end survives loop-count edit"), F.Target->Notifies[0].EndLink.GetTime(), .75f);
  TestEqual(TEXT("notify stays on durable slot zero"), F.Target->Notifies[0].GetSlotIndex(), 0);
  TestEqual(TEXT("mixed notify rows retained"), F.Target->Notifies.Num(), 2);
  TestEqual(TEXT("instant event retains its time within repeated segment"), F.Target->Notifies[1].GetTime(), .875f);
  TestEqual(TEXT("instant notify identity retained"), F.Target->Notifies[1].Guid, Before.Notifies[1].Guid);
  TestTrue(TEXT("retained nonempty metadata identity"), F.Target->CompositeSections[1].MetaData == Before.Sections[1].MetaData);
  TestTrue(TEXT("mutation footprint recorded"), !Transaction.Footprints.IsEmpty());
  TestEqual(TEXT("one typed inverse prepared"), Transaction.Actions.Num(), 1);
  TestEqual(TEXT("handler never late-enlists an inverse"), Transaction.LateActionCalls, 0);
  FSnapshot Applied(*F.Target);
  FMontageStructuralTransaction Replay; Replay.Token = Transaction.Token;
  TestFalse(TEXT("same prepared operation cannot run twice even with live matching token"), Testing::ApplyNative(Prepared.ToSharedRef(), Replay).bValid);
  TestTrue(TEXT("replay enlists no mutation"), Replay.Actions.IsEmpty() && Replay.Footprints.IsEmpty());
  Applied.Check(*this, *F.Target);
  TestTrue(TEXT("successful edit can roll back"), Transaction.Rollback(TEXT("fixture-after-success")).bValid);
  Before.Check(*this, *F.Target);
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageStructuralInferredCorrespondence,
 "UEAssetDoc.AnimationProvider.MontageStructural.InferredCorrespondence",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageStructuralInferredCorrespondence::RunTest(const FString&)
{
 using namespace AssetDocumentMontageStructuralTests;
 FFixture F(*this); if (!F.bReady) return false;
 const FSnapshot Before(*F.Target);
 auto Explicit = F.Slots(); Explicit.Slots.GetValue()[0].Segments[0].Loops = 2;
 FMontageStructuralTransaction ExplicitTransaction;
 auto ExplicitPrepared = Prepare(*this, F, Explicit, ExplicitTransaction);
 if (!ExplicitPrepared.IsValid() || !TestTrue(TEXT("explicit correspondence applies"),
  Testing::ApplyNative(ExplicitPrepared.ToSharedRef(), ExplicitTransaction).bValid)) return false;
 const FSnapshot ExplicitProjection(*F.Target);
 if (!TestTrue(TEXT("explicit edit restores before comparison"), ExplicitTransaction.Rollback(TEXT("CompareNoHint")).bValid)) return false;
 Before.Check(*this, *F.Target);
 auto Inferred = Explicit;
 for (auto& Slot : *Inferred.Slots) for (auto& Segment : Slot.Segments)
 { Segment.PreviousSlot = NAME_None; Segment.PreviousSegment = INDEX_NONE; }
 const auto InputBefore = Inferred;
 FMontageStructuralTransaction InferredTransaction;
 auto Prepared = Prepare(*this, F, Inferred, InferredTransaction);
 if (!Prepared.IsValid()) return false;
 TestEqual(TEXT("preparation preserves caller slot count"), Inferred.Slots->Num(), InputBefore.Slots->Num());
 for (int32 S = 0; S < Inferred.Slots->Num(); ++S)
 {
  const auto& Actual = (*Inferred.Slots)[S]; const auto& Original = (*InputBefore.Slots)[S];
  TestEqual(TEXT("preparation preserves caller slot name"), Actual.Name, Original.Name);
  TestEqual(TEXT("preparation preserves caller segment count"), Actual.Segments.Num(), Original.Segments.Num());
  for (int32 I = 0; I < Actual.Segments.Num(); ++I)
  {
   const auto& A = Actual.Segments[I]; const auto& B = Original.Segments[I];
   TestTrue(TEXT("inference does not write hints into caller input"), A.PreviousSlot == B.PreviousSlot && A.PreviousSegment == B.PreviousSegment);
   TestTrue(TEXT("inference preserves caller animation and timing"), A.Animation == B.Animation && A.Start == B.Start
    && A.SourceStart == B.SourceStart && A.SourceEnd == B.SourceEnd && A.Rate == B.Rate && A.Loops == B.Loops);
  }
 }
 Before.Check(*this, *F.Target);
 if (!TestTrue(TEXT("unique same-slot correspondence applies without hints"), Testing::ApplyNative(Prepared.ToSharedRef(), InferredTransaction).bValid)) return false;
 // Compare actual publication on the same object after restoring the first edit:
 // the shared Source occurs in both old slots, so global-only matching would fail.
 ExplicitProjection.Check(*this, *F.Target);
 TestEqual(TEXT("relative section keeps its actual method"), F.Target->CompositeSections[1].GetLinkMethod(), EAnimLinkMethod::Relative);
 TestEqual(TEXT("relative section keeps its actual segment"), F.Target->CompositeSections[1].GetSegmentIndex(), 0);
 TestEqual(TEXT("state endpoint remains attached to retained segment"), F.Target->Notifies[0].EndLink.GetTime(), .75f);
 TestEqual(TEXT("instant GUID remains original"), F.Target->Notifies[1].Guid, Before.Notifies[1].Guid);
 TestEqual(TEXT("no-hint edit installs exactly one inverse"), InferredTransaction.Actions.Num(), 1);
 TestTrue(TEXT("inferred edit rolls back complete timeline"), InferredTransaction.Rollback(TEXT("InferredCorrespondence")).bValid);
 Before.Check(*this, *F.Target);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageStructuralExplicitReservation,
 "UEAssetDoc.AnimationProvider.MontageStructural.ExplicitReservation",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageStructuralExplicitReservation::RunTest(const FString&)
{
 using namespace AssetDocumentMontageStructuralTests;
 FFixture F(*this); if (!F.bReady) return false;
 const FSnapshot Before(*F.Target); FMontageStructuralTransaction Transaction;
 auto Intent = F.Slots(); auto& Rows = *Intent.Slots;
 auto Added = Rows[1]; Added.Name = TEXT("C");
 Added.Segments[0].PreviousSlot = NAME_None; Added.Segments[0].PreviousSegment = INDEX_NONE;
 // The unhinted new slot precedes a later explicit claim. Explicit ownership
 // must already be reserved, independent of desired-row iteration order.
 Rows.Insert(Added, 1);
 auto Prepared = Prepare(*this, F, Intent, Transaction); if (!Prepared.IsValid()) return false;
 if (!TestTrue(TEXT("explicit claims leave unhinted new slot new"), Testing::ApplyNative(Prepared.ToSharedRef(), Transaction).bValid)) return false;
 TestEqual(TEXT("new slot is published in requested position"), F.Target->SlotAnimTracks[1].SlotName, FName(TEXT("C")));
 TestEqual(TEXT("later explicit old slot retained"), F.Target->SlotAnimTracks[2].SlotName, FName(TEXT("B")));
 TestTrue(TEXT("retained sections and metadata survive new-slot addition"), FSnapshot::Same(Before.Sections, F.Target->CompositeSections));
 TestTrue(TEXT("retained notify links and identities survive new-slot addition"), FSnapshot::Same(Before.Notifies, F.Target->Notifies));
 TestTrue(TEXT("new-row input is not rewritten with an old claim"), Rows[1].Segments[0].PreviousSlot.IsNone() && Rows[1].Segments[0].PreviousSegment == INDEX_NONE);
 TestTrue(TEXT("new-slot addition rolls back"), Transaction.Rollback(TEXT("ExplicitReservation")).bValid);
 Before.Check(*this, *F.Target);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageStructuralPositionalCorrespondence,
 "UEAssetDoc.AnimationProvider.MontageStructural.PositionalCorrespondence",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageStructuralPositionalCorrespondence::RunTest(const FString&)
{
 using namespace AssetDocumentMontageStructuralTests;
 // Several segments of one animation used to be "ambiguous" / "competing" (correspondence by animation). The editor
 // edits segments in place: segment N of a slot stays segment N, so unhinted rows correspond by position.
 for (const bool bAdded : {false, true})
 {
  FFixture F(*this); if (!F.bReady) return false;
  if (!bAdded)
  {
   auto Second = F.Target->SlotAnimTracks[0].AnimTrack.AnimSegments[0]; Second.StartPos = 1.f;
   F.Target->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(Second);
   F.Target->GetController().SetNumberOfFrames(FFrameNumber(60), false);
   F.Target->RefreshCacheData(); F.Package->SetDirtyFlag(false);
  }
  auto Intent = F.Slots(); auto& A = (*Intent.Slots)[0];
  if (bAdded) { auto Second = A.Segments[0]; Second.Start = 1.f; A.Segments.Add(Second); }
  else A.Segments.SetNum(1);
  for (auto& Segment : A.Segments) { Segment.PreviousSlot = NAME_None; Segment.PreviousSegment = INDEX_NONE; }
  const FSnapshot Before(*F.Target); FMontageStructuralTransaction Transaction; TSharedPtr<FPreparedStructuralEdit> Prepared;
  const auto Valid = PrepareMontageStructuralEdit(*F.Target, Intent, Transaction.Token.RequestId, Transaction.Token.RuntimeRevision, Prepared);
  for (const auto& D : Valid.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  TestTrue(bAdded ? TEXT("a second segment of the same animation is a new segment") : TEXT("removing the second segment of the same animation keeps the first"), Valid.bValid);
  if (Prepared.IsValid())
  {
   TestEqual(TEXT("desired segments"), Prepared->DesiredSlots[0].AnimTrack.AnimSegments.Num(), bAdded ? 2 : 1);
   TestTrue(TEXT("the relative section on segment 0 keeps its time"), Prepared->SectionLinks.Num() > 1
    && FMath::IsNearlyEqual(Prepared->SectionLinks[1].Time, F.Target->CompositeSections[1].GetTime()));
  }
  TestTrue(TEXT("preparation enlists no mutation"), Transaction.Actions.IsEmpty() && Transaction.Footprints.IsEmpty());
  TestTrue(TEXT("preparation leaves the package clean"), !F.Package->IsDirty());
  for (const auto& Segment : A.Segments)
   TestTrue(TEXT("inference leaves input hints absent"), Segment.PreviousSlot.IsNone() && Segment.PreviousSegment == INDEX_NONE);
  Before.Check(*this, *F.Target);
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageStructuralFailureRollback,
 "UEAssetDoc.AnimationProvider.MontageStructural.FailureRollback",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageStructuralFailureRollback::RunTest(const FString&)
{
 using namespace AssetDocumentMontageStructuralTests;
 for (auto Point : {Testing::EFailurePoint::AfterSlots, Testing::EFailurePoint::AfterLength,
  Testing::EFailurePoint::AfterSections, Testing::EFailurePoint::AfterNotifies, Testing::EFailurePoint::AfterCompletion})
 {
  FFixture F(*this); if (!F.bReady) return false;
  FSnapshot Before(*F.Target); FMontageStructuralTransaction Transaction; auto Intent = F.Slots();
  Intent.Slots.GetValue()[0].Segments[0].Loops = 2;
  auto Prepared = Prepare(*this, F, Intent, Transaction); if (!Prepared.IsValid()) continue;
  const auto Injected = Testing::ApplyNative(Prepared.ToSharedRef(), Transaction, Point);
  TestFalse(TEXT("injected boundary fails"), Injected.bValid);
  TestTrue(TEXT("operation reaches the requested failure boundary"), Injected.Diagnostics.ContainsByPredicate(
   [](const FAssetDocumentRuntimeDiagnostic& D) { return D.Code == TEXT("AnimationProvider.Montage.InjectedFailure"); }));
  TestEqual(TEXT("length setter is reached after the slot-only failure boundary"), F.Target->GetDataModel()->GetNumberOfFrames(),
   Point == Testing::EFailurePoint::AfterSlots ? 30 : 60);
  TestEqual(TEXT("inverse prepared before injected mutation"), Transaction.Actions.Num(), 1);
  TestEqual(TEXT("failure handler never late-enlists inverse"), Transaction.LateActionCalls, 0);
  TestTrue(TEXT("explicit transaction rollback succeeds"), Transaction.Rollback(TEXT("fixture-injected-boundary")).bValid);
  Before.Check(*this, *F.Target);
  TestFalse(TEXT("inverse cannot be consumed twice"), Transaction.Rollback(TEXT("fixture-repeat")).bValid);
  Before.Check(*this, *F.Target);
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageStructuralSectionEdits,
 "UEAssetDoc.AnimationProvider.MontageStructural.Sections",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageStructuralSectionEdits::RunTest(const FString&)
{
 using namespace AssetDocumentMontageStructuralTests;
 for (bool bCycle : {false, true})
 {
  FFixture F(*this); if (!F.bReady) return false;
  FSnapshot Before(*F.Target); FMontageStructuralTransaction Transaction; FStructuralIntent Intent;
  FSectionEdit A; A.Name = TEXT("Absolute"); A.Next = TEXT("Relative"); A.Slot = TEXT("A");
  FSectionEdit B; B.Name = TEXT("Relative"); B.Next = bCycle ? FName(TEXT("Absolute")) : NAME_None;
  B.Time = .625f; B.Slot = TEXT("A"); B.Method = EAnimLinkMethod::Relative;
  Intent.Sections = TArray<FSectionEdit>{A, B};
  auto Prepared = Prepare(*this, F, Intent, Transaction); if (!Prepared.IsValid()) continue;
  if (!TestTrue(TEXT("section graph applies"), Testing::ApplyNative(Prepared.ToSharedRef(), Transaction).bValid)) continue;
  TestEqual(TEXT("first next link"), F.Target->CompositeSections[0].NextSectionName, A.Next);
  TestEqual(TEXT("cycle or empty terminal next retained exactly"), F.Target->CompositeSections[1].NextSectionName, B.Next);
  TestEqual(TEXT("requested section time"), F.Target->CompositeSections[1].GetTime(), B.Time);
  TestTrue(TEXT("matching section retains metadata"), F.Target->CompositeSections[1].MetaData == Before.Sections[1].MetaData);
  TestTrue(TEXT("section edit retains notify collection"), FSnapshot::Same(Before.Notifies, F.Target->Notifies));
  TestTrue(TEXT("section graph rollback"), Transaction.Rollback(TEXT("fixture-section")).bValid);
  Before.Check(*this, *F.Target);
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageStructuralInvalidIntent,
 "UEAssetDoc.AnimationProvider.MontageStructural.InvalidIntent",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageStructuralInvalidIntent::RunTest(const FString&)
{
 using namespace AssetDocumentMontageStructuralTests;
 for (int32 Case = 0; Case < 13; ++Case)
 {
  FFixture F(*this); if (!F.bReady) return false;
  auto Intent = F.Slots(); auto& Slots = Intent.Slots.GetValue();
  if (Case == 0) Slots[0].Segments[0].Start = -1.f;
  if (Case == 1) Slots[0].Segments[0].Rate = 0.f;
  if (Case == 2) Slots[1].Name = Slots[0].Name;
  if (Case == 3) { F.Target->SyncGroup = TEXT("Sync"); F.Target->SyncSlotIndex = 0; Slots.Swap(0, 1); }
  if (Case == 4) F.Target->ChildrenAssets.Add(F.Source.Get());
  if (Case >= 5 && Case <= 8)
  {
   Intent.Slots.Reset(); FSectionEdit A; A.Name = TEXT("Bad"); A.Slot = TEXT("A");
   A.Time = Case == 5 ? -1.f : (Case == 6 ? 2.f : 0.f);
   if (Case == 8) A.Next = TEXT("MissingSection");
   Intent.Sections = TArray<FSectionEdit>{A};
   if (Case == 7) Intent.Sections.GetValue().Add(A);
  }
  if (Case == 9)
  {
   // Both preimage and desired tracks are contiguous. Extending the first segment
   // moves a relative section on the second past an absolute tail, independent of gap rejection.
   auto Second = F.Target->SlotAnimTracks[0].AnimTrack.AnimSegments[0];
   Second.StartPos = 1.f;
   F.Target->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(Second);
   F.Target->GetController().SetNumberOfFrames(FFrameNumber(60), false);
   F.Target->CompositeSections[1].Link(F.Target.Get(), 1.25f, 0);
   auto& Tail = F.Target->CompositeSections.AddDefaulted_GetRef();
   Tail.SectionName = TEXT("Tail"); Tail.Link(F.Target.Get(), 1.5f, 0);
   auto SecondEdit = Slots[0].Segments[0];
   SecondEdit.Start = 2.f; SecondEdit.PreviousSegment = 1;
   Slots[0].Segments[0].Loops = 2;
   Slots[0].Segments.Add(SecondEdit);
   F.Package->SetDirtyFlag(false);
  }
  if (Case == 10) Slots[0].Segments[0].Start = .25f; // PostLoad would remove the leading gap.
  if (Case == 11)
  {
   auto Second = Slots[0].Segments[0];
   Second.Start = 1.25f; Second.PreviousSlot = NAME_None; Second.PreviousSegment = INDEX_NONE;
   Slots[0].Segments.Add(Second); // PostLoad would close this internal gap.
  }
  if (Case == 12)
  {
   Intent.Slots.Reset(); FSectionEdit Section;
   Section.Name = TEXT("Absolute"); Section.Slot = TEXT("B");
   Intent.Sections = TArray<FSectionEdit>{Section}; // PostLoad would relink to slot zero.
  }
  FSnapshot Before(*F.Target); FMontageStructuralTransaction Transaction; TSharedPtr<FPreparedStructuralEdit> Prepared;
  const auto Rejected = PrepareMontageStructuralEdit(*F.Target,
   Intent, Transaction.Token.RequestId, Transaction.Token.RuntimeRevision, Prepared);
  TestFalse(TEXT("invalid or unsupported intent rejected before mutation"), Rejected.bValid);
  if (Case == 9)
   TestTrue(TEXT("contiguous crossing reaches retained section ordering guard"), Rejected.Diagnostics.ContainsByPredicate(
    [](const FAssetDocumentRuntimeDiagnostic& D) { return D.Message.Contains(TEXT("before the previous section")); }));
  TestFalse(TEXT("rejection publishes no prepared operation"), Prepared.IsValid());
  Before.Check(*this, *F.Target);
  TestTrue(TEXT("rejection preserves clean package"), !F.Package->IsDirty());
  TestTrue(TEXT("rejection has no enlisted mutations"), Transaction.Footprints.IsEmpty() && Transaction.Actions.IsEmpty());
 }
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageStructuralCaptureContract,
 "UEAssetDoc.AnimationProvider.MontageStructural.PreparedCapture",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageStructuralCaptureContract::RunTest(const FString&)
{
 using namespace AssetDocumentMontageStructuralTests;
 FFixture F(*this); if (!F.bReady) return false;
 FSnapshot Before(*F.Target); FMontageStructuralTransaction Transaction; auto Intent = F.Slots();
 Intent.Slots.GetValue()[0].Segments[0].Loops = 2;
 TSharedPtr<FPreparedStructuralEdit> Prepared;
 if (!TestTrue(TEXT("native state prepares"), PrepareMontageStructuralEdit(*F.Target, Intent,
  Transaction.Token.RequestId, Transaction.Token.RuntimeRevision, Prepared).bValid)) return false;
 Transaction.State = EAssetDocumentAtomicState::Mutating;
 TestFalse(TEXT("Mutating alone cannot fabricate preparation"), Testing::ApplyNative(Prepared.ToSharedRef(), Transaction).bValid);
 Before.Check(*this, *F.Target);
 if (!TestTrue(TEXT("real capture binds"), Transaction.BindPrepared(Prepared.ToSharedRef()))) return false;
 FAssetDocumentPreparedMutationBatch Duplicate; FAssetDocumentMutationIntent DuplicateIntent;
 TestFalse(TEXT("same prepared state cannot republish a second batch"), MakeMontageStructuralPreparedBatch(Prepared.ToSharedRef(), 0,
  EAssetDocumentRuntimeRequestPhase::Apply, Duplicate, DuplicateIntent).bValid);
 const FGuid Original = Transaction.Token.TransactionId;
 Transaction.Token.TransactionId = FGuid::NewGuid();
 TestFalse(TEXT("different transaction cannot consume capture"), Testing::ApplyNative(Prepared.ToSharedRef(), Transaction).bValid);
 Transaction.Token.TransactionId = Original;
 Transaction.Capture.Reset();
 TestFalse(TEXT("closed capture cannot mutate"), Testing::ApplyNative(Prepared.ToSharedRef(), Transaction).bValid);
 TestTrue(TEXT("no forward footprint on rejection"), Transaction.Footprints.IsEmpty());
 TestEqual(TEXT("no late inverse registration"), Transaction.LateActionCalls, 0);
 Before.Check(*this, *F.Target);
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageStructuralLoadedDependencies,
 "UEAssetDoc.AnimationProvider.MontageStructural.LoadedDependencies",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageStructuralLoadedDependencies::RunTest(const FString&)
{
 using namespace AssetDocumentMontageStructuralTests;
 FFixture F(*this); if (!F.bReady) return false;
 FSnapshot Before(*F.Target); FMontageStructuralTransaction Transaction;
 auto Intent = F.Slots();
 auto Frozen = FreezeMontageStructuralArguments(Intent);
 FStructuralIntent Parsed;
 TestTrue(TEXT("loaded typed paths decode"), ParseMontageStructuralArguments(Frozen,
  TEXT("animation.montage.slot_tracks.replace.v1"), Parsed).bValid);
 for (const FString& InvalidPath : {FString(TEXT("/Game/Missing.Missing")), F.Skeleton->GetPathName()})
 {
  auto Bad = Frozen;
  Bad.Elements[0].Elements[0].Elements[1].Elements[0].Elements[0].StringValue = InvalidPath;
  TestFalse(TEXT("missing or wrong-class path rejected without load"), ParseMontageStructuralArguments(Bad,
   TEXT("animation.montage.slot_tracks.replace.v1"), Parsed).bValid);
 }
 Intent.Slots.GetValue()[0].Segments[0].Loops = 2;
 TSharedPtr<FPreparedStructuralEdit> Prepared;
 if (!TestTrue(TEXT("dependency preimage prepared"), PrepareMontageStructuralEdit(*F.Target, Intent,
  Transaction.Token.RequestId, Transaction.Token.RuntimeRevision, Prepared).bValid)) return false;
 if (!TestTrue(TEXT("real batch binds"), Transaction.BindPrepared(Prepared.ToSharedRef()))) return false;
 const float OldRate = F.Source->RateScale;
 F.Source->RateScale = 2.f;
 TestFalse(TEXT("dependency drift rejects before write"), Testing::ApplyNative(Prepared.ToSharedRef(), Transaction).bValid);
 TestTrue(TEXT("drift has no footprint"), Transaction.Footprints.IsEmpty());
 F.Source->RateScale = OldRate;
 Before.Check(*this, *F.Target);
 TestTrue(TEXT("rejected mutation inverse is a no-op"), Transaction.Rollback(TEXT("DependencyDrift")).bValid);
 return true;
}
#endif
