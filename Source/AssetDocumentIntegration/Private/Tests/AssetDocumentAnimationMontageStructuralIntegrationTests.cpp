// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/AssetDocumentSkeletonTestHarness.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimNotifies/AnimNotifyState_TimedParticleEffect.h"
#include "Animation/AnimSequence.h"
#include "HAL/PlatformProcess.h"
#include "ReferenceSkeleton.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentMontageStructuralIntegrationTests
{
using namespace AssetDocumentSkeletonTests;
constexpr const TCHAR* SlotsId = TEXT("animation.montage.slot_tracks.replace.v1");
constexpr const TCHAR* SectionsId = TEXT("animation.montage.sections.mutate.v1");
FOptions Options(const FString& Failure = {})
{
 FOptions O; O.FailAfter = Failure;
 O.ExtraRequirements.Add({SlotsId, 1, TEXT("animation_profile")});
 O.ExtraRequirements.Add({SectionsId, 1, TEXT("animation_profile")});
 // This test-owned Body leaf exercises real providers and persistence. It is
 // not a production canonical Montage extraction/registration implementation.
 O.EntrySuffix = TEXT(R"PY(
SLOTS = 'animation.montage.slot_tracks.replace.v1'
SECTIONS = 'animation.montage.sections.mutate.v1'
for primitive in (SLOTS, SECTIONS):
    row = caller.PrimitiveRequirement(primitive, 1, 'animation_profile')
    caller.REQUIREMENTS += (row,)
    caller._BY_ID[primitive] = row
_original_interpret = caller.interpret

def interpret_montage(document):
    body = document.get('Body', {})
    if type(body) is not dict or set(body) != {'MontageStructural'}:
        raise caller._error('Montage fixture requires one explicit structural leaf.')
    properties, _ = _original_interpret(dict(document, Body={}))
    intent = body['MontageStructural']
    if type(intent) is not dict or not intent or set(intent) - {'slots', 'sections'}:
        raise caller._error('Montage fixture requires slots and/or sections.')
    return properties, intent
caller.interpret = interpret_montage

class MontageDispatch(caller.SkeletonS1Dispatch):
    def registration_projection(self):
        result = super().registration_projection()
        result['BodySections'] = ['MontageStructural']
        return result

    def _plan(self, request, context, properties, structural):
        if request.canonical_document.get('Action') != 'Update':
            raise caller._error('Montage fixture requires existing target.')
        steps, exists, read, mutable = super()._plan(request, context, properties, None)
        steps.append(caller._step(len(steps), SLOTS if 'slots' in structural else SECTIONS, structural))
        return steps, exists, read, mutable

    def _preflight(self, request, context, properties, structural, steps, exists, read):
        super()._preflight(request, context, properties, None, steps[:-1], exists, read)

    def _apply(self, request, context, properties, steps, read, mutable):
        changed = list(super()._apply(request, context, properties, steps[:-1], read, mutable))
        asset = context.handoff(source_handle=mutable, target_provider_id='animation', access='mutable')
        changed.extend(caller._invoke(context, steps[-1]['PrimitiveId'], steps[-1]['Intent'], asset=asset).changed_set)
        return tuple(changed)

    def _verify(self, request, context, properties, structural):
        # Native assertions check the structural projection, including in a
        # separate fresh process. S1 still verifies its real admitted scalar.
        super()._verify(request, context, properties, None)

dispatch = MontageDispatch(dispatch.options)
)PY");
 return O;
}
struct FFixture
{
 FAssets Base;
 TStrongObjectPtr<UAnimSequence> A, B;
 bool Setup()
 {
  if (!Base.Setup()) return false;
  {
   FReferenceSkeletonModifier Modifier(Base.Old.Get());
   Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
  }
  for (FName Name : {FName(TEXT("A")), FName(TEXT("B")), FName(TEXT("C"))}) Base.Old->RegisterSlotNode(Name);
  if (!Save(Base.Old.Get())) return false;
  auto Source = [&](const TCHAR* Name) -> UAnimSequence*
  {
   auto* S = NewObject<UAnimSequence>(CreatePackage(*(Base.Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone | RF_Transactional);
   S->SetSkeleton(Base.Old.Get()); FAssetRegistryModule::AssetCreated(S);
   auto& C = S->GetController(); C.InitializeModel(); C.SetFrameRate(FFrameRate(30, 1), false); C.SetNumberOfFrames(FFrameNumber(30), false);
   if (!C.AddBoneCurve(TEXT("root"), false)) return nullptr;
   TArray<FVector3f> P, Scale; TArray<FQuat4f> R;
   P.Init(FVector3f::ZeroVector, 31); Scale.Init(FVector3f::OneVector, 31); R.Init(FQuat4f::Identity, 31);
   if (!C.SetBoneTrackKeys(TEXT("root"), P, R, Scale, false)) return nullptr;
   C.NotifyPopulated(); S->WaitOnExistingCompression();
   return Save(S) ? S : nullptr;
  };
  A.Reset(Source(TEXT("SourceA"))); B.Reset(Source(TEXT("SourceB"))); if (!A.IsValid() || !B.IsValid()) return false;
  auto& M = *Base.Montage; auto& C = M.GetController();
  C.InitializeModel(); C.SetFrameRate(FFrameRate(30, 1), false); C.SetNumberOfFrames(FFrameNumber(60), false); C.NotifyPopulated();
  M.SyncSlotIndex = INDEX_NONE; M.SlotAnimTracks.Reset();
  auto Segment = [](UAnimSequence* S, float Start)
  {
   FAnimSegment R; R.SetAnimReference(S); R.StartPos = Start; R.AnimStartTime = 0.f;
   R.AnimEndTime = 1.f; R.AnimPlayRate = 1.f; R.LoopingCount = 1; return R;
  };
  auto& SlotA = M.SlotAnimTracks.AddDefaulted_GetRef(); SlotA.SlotName = TEXT("A");
  SlotA.AnimTrack.AnimSegments = {Segment(A.Get(), 0.f), Segment(B.Get(), 1.f)};
  auto& SlotB = M.SlotAnimTracks.AddDefaulted_GetRef(); SlotB.SlotName = TEXT("B"); SlotB.AnimTrack.AnimSegments = {Segment(B.Get(), 0.f)};
  M.CompositeSections.Reset();
  auto& First = M.CompositeSections.AddDefaulted_GetRef(); First.SectionName = TEXT("First"); First.NextSectionName = TEXT("Second"); First.Link(&M, 0.f, 0);
  auto& Second = M.CompositeSections.AddDefaulted_GetRef(); Second.SectionName = TEXT("Second"); Second.NextSectionName = NAME_None;
  Second.Link(&M, .5f, 0); Second.ChangeLinkMethod(EAnimLinkMethod::Relative);
  // Immutable script CDO sentinel: this pointer must survive save/load unchanged.
  Second.MetaData.Add(GetMutableDefault<UAnimMetaData>());
  M.AnimNotifyTracks.Reset(); M.AnimNotifyTracks.Add(FAnimNotifyTrack(TEXT("State"), FLinearColor::White)); M.AnimNotifyTracks.Add(FAnimNotifyTrack(TEXT("Event"), FLinearColor::White));
  M.Notifies.Reset();
  auto& State = M.Notifies.AddDefaulted_GetRef(); State.NotifyName = TEXT("State"); State.Guid = FGuid::NewGuid(); State.TrackIndex = 0;
  State.NotifyStateClass = NewObject<UAnimNotifyState_TimedParticleEffect>(&M); State.Link(&M, .25f, 0); State.ChangeLinkMethod(EAnimLinkMethod::Relative);
  State.EndLink.Link(&M, .75f, 0); State.EndLink.ChangeLinkMethod(EAnimLinkMethod::Relative); State.SetDuration(.5f);
  auto& Event = M.Notifies.AddDefaulted_GetRef(); Event.NotifyName = TEXT("Event"); Event.Guid = FGuid::NewGuid(); Event.TrackIndex = 1;
  Event.Link(&M, 1.5f, 0); Event.ChangeLinkMethod(EAnimLinkMethod::Relative);
  M.RefreshCacheData(); return Save(&M);
 }
 FString Segment(const FString& Path, double Start, const TCHAR* PreviousSlot, int32 PreviousSegment, int32 Loops = 1) const
 {
  return FString::Printf(TEXT("{\"animation\":\"%s\",\"start\":%.1f,\"source_start\":0.0,\"source_end\":1.0,\"rate\":1.0,\"loops\":%d.0,\"previous_slot\":\"%s\",\"previous_segment\":%d.0}"), *Path, Start, Loops, PreviousSlot, PreviousSegment);
 }
 FString Sections(bool bCycle) const
 {
  return FString::Printf(TEXT("\"sections\":[{\"name\":\"First\",\"next\":\"Second\",\"time\":0.0,\"slot_name\":\"A\",\"method\":0.0},{\"name\":\"Second\",\"next\":\"%s\",\"time\":1.0,\"slot_name\":\"A\",\"method\":1.0}]"), bCycle ? TEXT("First") : TEXT("None"));
 }
 bool Document(int32 Case, bool bCombined = false)
 {
  FString Intent;
  if (Case == 4) Intent = TEXT("{") + Sections(false) + TEXT("}");
  else
  {
   // Case 6 preserves the original fresh-read failure as a rejected gap.
   const double Shift = Case == 6 ? .5 : 0.;
   const bool Grow = Case == 1 || bCombined;
   const FString SlotA = TEXT("{\"name\":\"A\",\"segments\":[") + Segment(A->GetPathName(), Shift, TEXT("A"), 0, Grow ? 2 : 1) + TEXT(",") + Segment(B->GetPathName(), (Grow ? 2. : 1.) + Shift, TEXT("A"), 1) + TEXT("]}");
   const FString SlotB = TEXT("{\"name\":\"B\",\"segments\":[") + Segment(B->GetPathName(), 0., TEXT("B"), 0) + TEXT("]}");
   const FString SlotC = TEXT("{\"name\":\"C\",\"segments\":[") + Segment(A->GetPathName(), 0., TEXT("None"), INDEX_NONE) + TEXT("]}");
   FString Rows = Case == 3 ? SlotB + TEXT(",") + SlotA : SlotA;
   if (Case != 2 && Case != 3) Rows += TEXT(",") + SlotB;
   if (Case == 0) Rows += TEXT(",") + SlotC;
   if (Case == 7) Rows = SlotA + TEXT(",") + SlotC + TEXT(",") + SlotB;
   Intent = TEXT("{\"slots\":[") + Rows + TEXT("]") + (bCombined ? TEXT(",") + Sections(true) : FString()) + TEXT("}");
  }
  return Base.Document(TEXT("{\"MontageStructural\":") + Intent + TEXT("}"));
 }
 ~FFixture()
 {
  for (auto* S : {A.Get(), B.Get()}) if (IsValid(S))
  {
   auto* P = S->GetOutermost(); FAssetRegistryModule::AssetDeleted(S); S->ClearFlags(RF_Public | RF_Standalone);
   S->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional); S->MarkAsGarbage(); P->SetDirtyFlag(false); P->MarkAsGarbage();
  }
 }
};
struct FSnapshot
{
 TArray<FSlotAnimationTrack> Slots; TArray<FCompositeSection> Sections; TArray<FAnimNotifyEvent> Notifies; int32 Frames;
 explicit FSnapshot(const UAnimMontage& M) : Slots(M.SlotAnimTracks), Sections(M.CompositeSections), Notifies(M.Notifies), Frames(M.GetDataModel()->GetNumberOfFrames()) {}
 template <typename T> static bool Same(const TArray<T>& A, const TArray<T>& B)
 {
  if (A.Num() != B.Num()) return false;
  for (int32 I = 0; I < A.Num(); ++I) if (!T::StaticStruct()->CompareScriptStruct(&A[I], &B[I], 0)) return false;
  return true;
 }
 void Check(FAutomationTestBase& T, const UAnimMontage& M) const
 {
  T.TestTrue(TEXT("slots restored exactly"), Same(Slots, M.SlotAnimTracks)); T.TestTrue(TEXT("sections/metadata restored exactly"), Same(Sections, M.CompositeSections));
  T.TestTrue(TEXT("mixed notifies restored exactly"), Same(Notifies, M.Notifies)); T.TestEqual(TEXT("frames restored"), M.GetDataModel()->GetNumberOfFrames(), Frames);
 }
};
bool CheckEdited(FAutomationTestBase& T, const UAnimMontage& M, const FString& SourceA, const FString& SourceB, int32 Case, bool Combined = false)
{
 const bool Grow = Case == 1 || Combined; constexpr int32 SlotIndex = 0;
 if (!T.TestEqual(TEXT("edited slot count"), M.SlotAnimTracks.Num(), (Case == 0 || Case == 7) ? 3 : (Case == 2 ? 1 : 2))) return false;
 const auto& Slot = M.SlotAnimTracks[SlotIndex]; T.TestEqual(TEXT("A slot resolves in expected order"), Slot.SlotName, FName(TEXT("A")));
 if (!T.TestEqual(TEXT("nonempty two-segment track retained"), Slot.AnimTrack.AnimSegments.Num(), 2)) return false;
 for (int32 I = 0; I < 2; ++I)
 {
  const auto& S = Slot.AnimTrack.AnimSegments[I];
  if (!T.TestNotNull(TEXT("segment source present"), S.GetAnimReference().Get())) return false;
  T.TestEqual(TEXT("source exact path"), S.GetAnimReference()->GetPathName(), I == 0 ? SourceA : SourceB);
  T.TestEqual(TEXT("segment authored start"), S.StartPos, I == 0 ? 0.f : (Grow ? 2.f : 1.f));
  T.TestEqual(TEXT("canonical first segment loops"), S.LoopingCount, I == 0 && Grow ? 2 : 1);
  T.TestEqual(TEXT("source end retained"), S.AnimEndTime, 1.f); T.TestEqual(TEXT("source rate retained"), S.AnimPlayRate, 1.f);
 }
 T.TestEqual(TEXT("controller frames"), M.GetDataModel()->GetNumberOfFrames(), Grow ? 90 : 60);
 if (!T.TestEqual(TEXT("sections retained"), M.CompositeSections.Num(), 2) || !T.TestEqual(TEXT("mixed notifies retained"), M.Notifies.Num(), 2)) return false;
 T.TestEqual(TEXT("absolute section"), M.CompositeSections[0].GetTime(), 0.f);
 T.TestEqual(TEXT("relative section"), M.CompositeSections[1].GetTime(), Combined || Case == 4 ? 1.f : .5f);
 T.TestEqual(TEXT("section next cycle/terminal"), M.CompositeSections[1].NextSectionName, Combined ? FName(TEXT("First")) : NAME_None);
 T.TestEqual(TEXT("relative section slot"), M.CompositeSections[1].GetSlotIndex(), SlotIndex);
 T.TestEqual(TEXT("relative method retained"), M.CompositeSections[1].GetLinkMethod(), EAnimLinkMethod::Relative);
 T.TestTrue(TEXT("metadata reference survives"), M.CompositeSections[1].MetaData.Num() == 1 && M.CompositeSections[1].MetaData[0] == GetMutableDefault<UAnimMetaData>());
 T.TestNotNull(TEXT("real notify state retained"), M.Notifies[0].NotifyStateClass.Get());
 T.TestEqual(TEXT("notify state start"), M.Notifies[0].GetTime(), .25f);
 T.TestEqual(TEXT("notify state end"), M.Notifies[0].EndLink.GetTime(), .75f);
 T.TestEqual(TEXT("instant notify from second segment"), M.Notifies[1].GetTime(), Grow ? 2.5f : 1.5f);
 T.TestEqual(TEXT("notify track retained"), M.Notifies[1].TrackIndex, 1);
 T.TestEqual(TEXT("state start reload-stable slot"), M.Notifies[0].GetSlotIndex(), 0);
 T.TestEqual(TEXT("state end reload-stable slot"), M.Notifies[0].EndLink.GetSlotIndex(), 0);
 T.TestEqual(TEXT("instant event reload-stable slot"), M.Notifies[1].GetSlotIndex(), 0);
 T.TestEqual(TEXT("admitted scalar persisted"), M.RateScale, 2.f);
 return !T.HasAnyErrors();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageStructuralApplyFileTest, "UEAssetDoc.MontageStructuralIntegration.ApplyFile.Success", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageStructuralApplyFileTest::RunTest(const FString&)
{
 using namespace AssetDocumentMontageStructuralIntegrationTests;
 for (int32 Case : {0, 1, 2, 4, 5, 7})
 {
  FFixture F; const bool Combined = Case == 5; const int32 Edit = Combined ? 1 : Case;
  if (!TestTrue(TEXT("saved real multi-segment fixture"), F.Setup())) return false;
  if (Case == 7)
  {
   auto C = F.Base.Montage->SlotAnimTracks[1]; C.SlotName = TEXT("C");
   F.Base.Montage->SlotAnimTracks.Add(C);
   if (!TestTrue(TEXT("unlinked third slot baseline saved"), Save(F.Base.Montage.Get()))) return false;
  }
  if (!TestTrue(TEXT("structural document saved"), F.Document(Edit, Combined))) return false;
  const auto A = Bytes(PackageFile(F.A->GetPathName())), B = Bytes(PackageFile(F.B->GetPathName())), Skeleton = Bytes(PackageFile(F.Base.Old->GetPathName()));
  FHarness H(Options()); if (!H.Initialize(*this)) return false;
  const auto Result = H.Apply(F.Base.Sidecar); for (const auto& D : Result.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  if (!TestTrue(TEXT("real structural ApplyFile persisted"), Result.IsSuccess())) return false;
  CheckEdited(*this, *F.Base.Montage, F.A->GetPathName(), F.B->GetPathName(), Edit, Combined);
  if (Case == 7)
  {
   TestEqual(TEXT("unlinked C moves before B"), F.Base.Montage->SlotAnimTracks[1].SlotName, FName(TEXT("C")));
   TestEqual(TEXT("unlinked B moves after C"), F.Base.Montage->SlotAnimTracks[2].SlotName, FName(TEXT("B")));
  }
  const FString Primitive = Edit == 4 ? SectionsId : SlotsId;
  TestTrue(TEXT("real structural ChangedSet recorded"), H.Changes.ContainsByPredicate([&](const auto& C) { return C.AuthorityId == Primitive && C.StableObjectId == F.Base.Target; }));
  TestFalse(TEXT("persisted target clean"), F.Base.Montage->GetOutermost()->IsDirty());
  TestTrue(TEXT("source A immutable"), Bytes(PackageFile(F.A->GetPathName())) == A); TestTrue(TEXT("source B immutable"), Bytes(PackageFile(F.B->GetPathName())) == B);
  TestTrue(TEXT("Skeleton immutable"), Bytes(PackageFile(F.Base.Old->GetPathName())) == Skeleton);
  TestEqual(TEXT("one sidecar entry"), H.SourcePaths.Num(), 1);
 }
 return !HasAnyErrors();
}

// Preserve both actual fresh-read regressions: stock PostLoad canonicalizes
// gaps and re-links sections/notifies to slot zero. Neither can be published.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageStructuralApplyFileReloadRejectionTest, "UEAssetDoc.MontageStructuralIntegration.ApplyFile.ReloadRejection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageStructuralApplyFileReloadRejectionTest::RunTest(const FString&)
{
 using namespace AssetDocumentMontageStructuralIntegrationTests;
 for (int32 Case : {6, 3})
 {
  FFixture F; if (!TestTrue(TEXT("saved gap or linked-slot reorder regression"), F.Setup() && F.Document(Case))) return false;
  FSnapshot Before(*F.Base.Montage);
  const TArray<FString> Files{PackageFile(F.Base.Target), F.Base.Sidecar, PackageFile(F.A->GetPathName()), PackageFile(F.B->GetPathName()), PackageFile(F.Base.Old->GetPathName())};
  TArray<TArray<uint8>> Original;
  for (const auto& File : Files) Original.Add(Bytes(File));
  FHarness H(Options()); if (!H.Initialize(*this)) return false;
  const auto Result = H.Apply(F.Base.Sidecar); for (const auto& D : Result.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  TestFalse(TEXT("non-reload-stable intent rejects ApplyFile"), Result.IsSuccess());
  TestTrue(TEXT("rejection has zero changed records"), H.Changes.IsEmpty());
  TestFalse(TEXT("rejected slots never invoke mutation"), H.Invocations.Contains(SlotsId));
  TestFalse(TEXT("rejected request never writes scalar"), H.Invocations.Contains(TEXT("editor.property.write_admitted_batch.v1")));
  TestFalse(TEXT("rejection never persists"), H.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Persist));
  Before.Check(*this, *F.Base.Montage);
  TestEqual(TEXT("rejection leaves scalar untouched"), F.Base.Montage->RateScale, 1.f);
  TestFalse(TEXT("rejection leaves package clean"), F.Base.Montage->GetOutermost()->IsDirty());
  for (int32 I = 0; I < Files.Num(); ++I) TestTrue(TEXT("rejection leaves all asset and sidecar bytes untouched"), Bytes(Files[I]) == Original[I]);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageStructuralApplyFileRollbackTest, "UEAssetDoc.MontageStructuralIntegration.ApplyFile.Rollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageStructuralApplyFileRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentMontageStructuralIntegrationTests;
 for (const FString Failure : {TEXT("repair"), TEXT("verify")}) for (int32 Case : {1, 4})
 {
  FFixture F; if (!TestTrue(TEXT("saved rollback baseline"), F.Setup() && F.Document(Case))) return false;
  FSnapshot Before(*F.Base.Montage); const auto Package = Bytes(PackageFile(F.Base.Target)), Sidecar = Bytes(F.Base.Sidecar);
  FHarness H(Options(Failure)); if (!H.Initialize(*this)) return false;
  const auto Result = H.Apply(F.Base.Sidecar); for (const auto& D : Result.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  TestFalse(TEXT("late failure rejects ApplyFile"), Result.IsSuccess());
  const FString Primitive = Case == 4 ? SectionsId : SlotsId;
  TestTrue(TEXT("real forward completed before failure"), H.Changes.ContainsByPredicate([&](const auto& C) { return C.AuthorityId == Primitive; }));
  TestTrue(TEXT("real prepared inverse observed"), H.Inverses.Contains(Primitive));
  const bool Verify = Failure == TEXT("verify"); TestEqual(TEXT("only verify failure reaches persistence"), H.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Persist), Verify);
  TestTrue(TEXT("expected failure mapping"), Result.Diagnostics.ContainsByPredicate([&](const auto& D) { return D.Code == (Verify ? TEXT("Persistence.CanonicalMismatch") : TEXT("S1Fixture.InjectedFailure")); }));
  if (Verify) TestTrue(TEXT("invalid durable receipt rejected"), Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("RequestCoordinator.InvalidDurableReceipt"); }));
  Before.Check(*this, *F.Base.Montage); TestEqual(TEXT("scalar inverse"), F.Base.Montage->RateScale, 1.f);
  TestFalse(TEXT("dirty inverse"), F.Base.Montage->GetOutermost()->IsDirty());
  TestTrue(TEXT("package bytes restored"), Bytes(PackageFile(F.Base.Target)) == Package); TestTrue(TEXT("sidecar bytes restored"), Bytes(F.Base.Sidecar) == Sidecar);
  TestTrue(TEXT("rollback invalidates handles"), H.Invalidations.ContainsByPredicate([](const auto& I) { return I.Reason == EAssetDocumentHandleInvalidationReason::Rollback; }));
  TestFalse(TEXT("failed request never commits"), H.Invalidations.ContainsByPredicate([](const auto& I) { return I.Reason == EAssetDocumentHandleInvalidationReason::Commit; }));
 }
 return !HasAnyErrors();
}


#endif
