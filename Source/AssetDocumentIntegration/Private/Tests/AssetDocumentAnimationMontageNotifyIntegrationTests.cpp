// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/AssetDocumentSkeletonTestHarness.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimNotifies/AnimNotifyState_TimedParticleEffect.h"
#include "Animation/AnimNotifies/AnimNotify_PlaySound.h"
#include "Animation/AnimSequence.h"
#include "HAL/PlatformProcess.h"
#include "ReferenceSkeleton.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include <type_traits>
#include "Utils/AssetDocumentEngineCompat.h"
namespace AssetDocumentMontageNotifyIntegrationTests
{
using namespace AssetDocumentSkeletonTests;
constexpr const TCHAR* SlotsId = TEXT("animation.montage.notifies.replace.v1");
constexpr const TCHAR* SectionsId = TEXT("animation.montage.notify_states.replace.v1");
FOptions Options(const FString& Failure = {})
{
 FOptions O; O.FailAfter = Failure;
 O.bMontageNotifyRequestProvider = true;
 O.ExtraRequirements.Add({SlotsId, 1, TEXT("animation_profile")});
 O.ExtraRequirements.Add({SectionsId, 1, TEXT("animation_profile")});
 // This test-owned Body leaf exercises real providers and persistence. It is
 // not a production canonical Montage extraction/registration implementation.
 O.EntrySuffix = TEXT(R"PY(
SLOTS = 'animation.montage.notifies.replace.v1'
SECTIONS = 'animation.montage.notify_states.replace.v1'
for primitive in (SLOTS, SECTIONS):
    row = caller.PrimitiveRequirement(primitive, 1, 'animation_profile')
    caller.REQUIREMENTS += (row,)
    caller._BY_ID[primitive] = row
_original_interpret = caller.interpret

def interpret_montage(document):
    body = document.get('Body', {})
    if type(body) is not dict or set(body) != {'MontageNotify'}:
        raise caller._error('Montage fixture requires one explicit structural leaf.')
    properties, _ = _original_interpret(dict(document, Body={}))
    intent = body['MontageNotify']
    if type(intent) is not dict or not intent or set(intent) - {'notifies', 'notify_states'}:
        raise caller._error('Montage fixture requires slots and/or sections.')
    return properties, intent
caller.interpret = interpret_montage

class MontageDispatch(caller.SkeletonS1Dispatch):
    def registration_projection(self):
        result = super().registration_projection()
        result['BodySections'] = ['MontageNotify']
        return result

    def _plan(self, request, context, properties, structural):
        if request.canonical_document.get('Action') != 'Update':
            raise caller._error('Montage fixture requires existing target.')
        steps, exists, read, mutable = super()._plan(request, context, properties, None)
        steps.append(caller._step(len(steps), SLOTS if 'notifies' in structural else SECTIONS, structural))
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
  auto& Managed = M.Notifies.AddDefaulted_GetRef(); Managed.NotifyName = TEXT("AssetDocument.Notify");
  Managed.TrackIndex = 1; Managed.Notify = NewObject<UAnimNotify_PlaySound>(&M, TEXT("AssetDocumentManaged_Notify_Old"), RF_Transactional);
  Managed.Link(&M, 1.f, 0); Managed.Guid = FGuid::NewGuid();
  auto& ManagedState = M.Notifies.AddDefaulted_GetRef(); ManagedState.NotifyName = TEXT("AssetDocument.NotifyState");
  ManagedState.TrackIndex = 0;
  ManagedState.NotifyStateClass = NewObject<UAnimNotifyState_TimedParticleEffect>(&M, TEXT("AssetDocumentManaged_NotifyState_Old"), RF_Transactional);
  ManagedState.Link(&M, 1.25f, 0); ManagedState.EndLink.Link(&M, 1.75f, 0); ManagedState.SetDuration(.5f); ManagedState.Guid = FGuid::NewGuid();
  M.UpdateLinkableElements(); M.RefreshCacheData(); return Save(&M);
 }
 bool Document(bool bState, bool bEmpty = false, bool bBad = false)
 {
  const FString Props = bBad ? TEXT("{\"DoesNotExist\":1.0}")
   : bState ? TEXT("{\"bDestroyAtEnd\":false}") : TEXT("{\"VolumeMultiplier\":0.25}");
  const FString Row = FString::Printf(TEXT("{\"time\":0.125,%s\"track_index\":1.0,\"object\":{\"class\":\"%s\",\"properties\":%s}}"),
   bState ? TEXT("\"duration\":0.5,") : TEXT(""),
   bState ? TEXT("/Script/Engine.AnimNotifyState_TimedParticleEffect") : TEXT("/Script/Engine.AnimNotify_PlaySound"), *Props);
  return Base.Document(FString::Printf(TEXT("{\"MontageNotify\":{\"%s\":[%s]}}"),
   bState ? TEXT("notify_states") : TEXT("notifies"), bEmpty ? TEXT("") : *Row));
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
bool CheckEdited(FAutomationTestBase& T, const UAnimMontage& M, bool bState, bool bEmpty)
{
 const FName ManagedName(bState ? TEXT("AssetDocument.NotifyState") : TEXT("AssetDocument.Notify"));
 const auto* New = M.Notifies.FindByPredicate([&](const auto& E) { return E.NotifyName == ManagedName; });
 T.TestEqual(TEXT("managed family presence"), New != nullptr, !bEmpty);
 if (New)
 {
  T.TestEqual(TEXT("new configured time sorts ahead of retained"), New->GetTime(), .125f);
  T.TestEqual(TEXT("authored track"), New->TrackIndex, 1);
  if (bState)
  {
   const auto* Object = Cast<UAnimNotifyState_TimedParticleEffect>(New->NotifyStateClass);
   T.TestTrue(TEXT("state configured and duration saved"), Object && !Object->bDestroyAtEnd && New->GetDuration() == .5f);
  }
  else
  {
   const auto* Object = Cast<UAnimNotify_PlaySound>(New->Notify);
   T.TestTrue(TEXT("notify configured scalar saved"), Object && Object->VolumeMultiplier == .25f);
  }
 }
 T.TestTrue(TEXT("unmanaged state preserved"), M.Notifies.ContainsByPredicate([](const auto& E) { return E.NotifyName == TEXT("State") && E.GetTime() == .25f && E.GetDuration() == .5f; }));
 T.TestTrue(TEXT("unmanaged instant preserved"), M.Notifies.ContainsByPredicate([](const auto& E) { return E.NotifyName == TEXT("Event") && E.GetTime() == 1.5f; }));
 T.TestEqual(TEXT("tracks unchanged"), M.AnimNotifyTracks.Num(), 2);
 T.TestEqual(TEXT("scalar persisted"), M.RateScale, 2.f);
 return !T.HasAnyErrors();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageNotifyApplyFileSuccess, "UEAssetDoc.MontageNotifyIntegration.ApplyFile.Success", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageNotifyApplyFileSuccess::RunTest(const FString&)
{
 using namespace AssetDocumentMontageNotifyIntegrationTests;
 for (bool bState : {false, true}) for (bool bEmpty : {false, true})
 {
  FFixture F; if (!TestTrue(TEXT("saved native fixture and document"), F.Setup() && F.Document(bState, bEmpty))) return false;
  const auto A = Bytes(PackageFile(F.A->GetPathName())), B = Bytes(PackageFile(F.B->GetPathName()));
  FHarness H(Options()); if (!H.Initialize(*this)) return false;
  const auto R = H.Apply(F.Base.Sidecar); for (const auto& D : R.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  if (!TestTrue(TEXT("real same-provider prepared ApplyFile commits"), R.IsSuccess())) return false;
  CheckEdited(*this, *F.Base.Montage, bState, bEmpty);
  TestTrue(TEXT("source assets remain unchanged"), Bytes(PackageFile(F.A->GetPathName())) == A && Bytes(PackageFile(F.B->GetPathName())) == B);
  TestFalse(TEXT("target clean"), F.Base.Montage->GetOutermost()->IsDirty());
 }
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageNotifyApplyFileRollback, "UEAssetDoc.MontageNotifyIntegration.ApplyFile.Rollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageNotifyApplyFileRollback::RunTest(const FString&)
{
 using namespace AssetDocumentMontageNotifyIntegrationTests;
 for (bool bState : {false, true}) for (const FString Failure : {TEXT("repair"), TEXT("verify"), TEXT("bad_property")})
 {
  FFixture F; if (!TestTrue(TEXT("saved rollback fixture"), F.Setup() && F.Document(bState, false, Failure == TEXT("bad_property")))) return false;
  const auto Old = F.Base.Montage->Notifies;
  const auto Package = Bytes(PackageFile(F.Base.Target)), Sidecar = Bytes(F.Base.Sidecar);
  TArray<UObject*> OriginalObjects; GetObjectsWithOuter(F.Base.Montage.Get(), OriginalObjects, AssetDocumentObjects::DirectOnly);
  FHarness H(Options(Failure == TEXT("bad_property") ? FString() : Failure)); if (!H.Initialize(*this)) return false;
  const auto R = H.Apply(F.Base.Sidecar); for (const auto& D : R.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  TestFalse(TEXT("injected or property failure rejects"), R.IsSuccess());
  TestEqual(TEXT("old mixed timeline count restored"), F.Base.Montage->Notifies.Num(), Old.Num());
  for (int32 I = 0; I < Old.Num() && I < F.Base.Montage->Notifies.Num(); ++I)
   TestTrue(TEXT("old mixed row exactly restored"), FAnimNotifyEvent::StaticStruct()->CompareScriptStruct(&Old[I], &F.Base.Montage->Notifies[I], 0));
  TArray<UObject*> Remaining; GetObjectsWithOuter(F.Base.Montage.Get(), Remaining, AssetDocumentObjects::DirectOnly);
  for (auto* Object : Remaining) TestTrue(TEXT("new staging set retired"), !IsValid(Object) || OriginalObjects.Contains(Object));
  TestTrue(TEXT("real typed inverse used"), H.Inverses.Contains(bState ? SectionsId : SlotsId));
  TestEqual(TEXT("scalar restored"), F.Base.Montage->RateScale, 1.f);
  TestEqual(TEXT("track count restored"), F.Base.Montage->AnimNotifyTracks.Num(), 2);
  TestFalse(TEXT("dirty finalizer completes"), F.Base.Montage->GetOutermost()->IsDirty());
  TestTrue(TEXT("disk target and sidecar restored"), Bytes(PackageFile(F.Base.Target)) == Package && Bytes(F.Base.Sidecar) == Sidecar);
 }
 return !HasAnyErrors();
}

namespace AssetDocumentMontageNotifyCombinedTests
{
using namespace AssetDocumentMontageNotifyIntegrationTests;
constexpr const TCHAR* Replace = TEXT("animation.curve.float.replace.v2");
constexpr const TCHAR* Enumerate = TEXT("animation.curve.float.enumerate.v2");
uint32 Bits(float Value) { uint32 Out; FMemory::Memcpy(&Out, &Value, sizeof(Out)); return Out; }
FString FloatWire(float Value) { return FString::Printf(TEXT("{\"bits\":%u}"), Bits(Value)); }
struct FRow
{
 FString Name;
 int32 Flags = AACF_Editable;
 FLinearColor Color = FLinearColor::White;
 FString Comment;
 float Default = MAX_flt;
 ERichCurveExtrapolation Pre = RCCE_Constant, Post = RCCE_Constant;
 TArray<FRichCurveKey> Keys;
};
FString Wire(const TArray<FRow>& Curves)
{
 TArray<FString> Encoded;
 for (const auto& C : Curves)
 {
  TArray<FString> Keys;
  for (const auto& K : C.Keys)
   Keys.Add(FString::Printf(TEXT("{\"time\":%s,\"value\":%s,\"arrive_tangent\":%s,\"leave_tangent\":%s,\"interp_mode\":%d,\"tangent_mode\":%d,\"tangent_weight_mode\":%d,\"arrive_tangent_weight\":%s,\"leave_tangent_weight\":%s}"),
    *FloatWire(K.Time), *FloatWire(K.Value), *FloatWire(K.ArriveTangent), *FloatWire(K.LeaveTangent), static_cast<int32>(K.InterpMode), static_cast<int32>(K.TangentMode), static_cast<int32>(K.TangentWeightMode), *FloatWire(K.ArriveTangentWeight), *FloatWire(K.LeaveTangentWeight)));
  const FString Default = C.Default == MAX_flt ? TEXT("{\"has_value\":false,\"float32_bits\":null}") : FString::Printf(TEXT("{\"has_value\":true,\"float32_bits\":%u}"), Bits(C.Default));
  // Fixture strings are fixed safe ASCII literals, never user supplied.
  Encoded.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"flags\":%d,\"color\":{\"r\":%s,\"g\":%s,\"b\":%s,\"a\":%s},\"comment\":\"%s\",\"default_value\":%s,\"pre_extrapolation\":%d,\"post_extrapolation\":%d,\"keys\":[%s]}"),
   *C.Name, C.Flags, *FloatWire(C.Color.R), *FloatWire(C.Color.G), *FloatWire(C.Color.B), *FloatWire(C.Color.A), *C.Comment, *Default, static_cast<int32>(C.Pre), static_cast<int32>(C.Post), *FString::Join(Keys, TEXT(","))));
 }
 return TEXT("{\"Curves\":[") + FString::Join(Encoded, TEXT(",")) + TEXT("]}");
}

FRow Curve(bool Edited)
{
 FRow R; R.Name = TEXT("CombinedCurve");
 for (int32 I = 0; I < 2; ++I) { FRichCurveKey K(float(I), Edited ? float(3 + I * 2) : float(1 + I)); K.InterpMode = RCIM_Linear; R.Keys.Add(K); }
 return R;
}
FOptions CombinedOptions(const FString& Failure = {})
{
 using namespace AssetDocumentMontageNotifyIntegrationTests;
 auto O = Options(Failure);
 O.ExtraRequirements.Add({Replace, 2, TEXT("animation_profile")}); O.ExtraRequirements.Add({Enumerate, 2, TEXT("animation_profile")});
 O.EntrySuffix += TEXT(R"PY(
REPLACE = 'animation.curve.float.replace.v2'
ENUMERATE = 'animation.curve.float.enumerate.v2'
for primitive in (REPLACE, ENUMERATE):
    row = caller.PrimitiveRequirement(primitive, 2, 'animation_profile')
    caller.REQUIREMENTS += (row,)
    caller._BY_ID[primitive] = row

def curve_wire(value):
    if type(value) in (list, tuple):
        return [curve_wire(item) for item in value]
    if type(value) is dict:
        return {key: curve_wire(item) for key, item in value.items()}
    if type(value) is float:
        if not value.is_integer():
            raise caller._error('Curve integer wire is fractional.')
        return int(value)
    return value

def interpret_combined(document):
    body = document.get('Body', {})
    if type(body) is not dict or set(body) != {'Curves', 'MontageNotify'} or 'Properties' in document:
        raise caller._error('Combined fixture requires Curves and MontageNotify without scalar writes.')
    properties, _ = _original_interpret(dict(document, Body={}))
    notify = body['MontageNotify']
    if type(notify) is not dict or set(notify) != {'notifies', 'notify_states'}:
        raise caller._error('Combined fixture requires both complete notify families.')
    return properties, ({'curves': curve_wire(body['Curves'])}, notify)
caller.interpret = interpret_combined

class CombinedDispatch(caller.SkeletonS1Dispatch):
    def registration_projection(self):
        result = super().registration_projection()
        result['BodySections'] = ['Curves', 'MontageNotify']
        return result

    def _plan(self, request, context, properties, combined):
        if request.canonical_document.get('Action') != 'Update':
            raise caller._error('Combined fixture requires existing Montage.')
        steps, exists, read, mutable = super()._plan(request, context, properties, None)
        curves, notify = combined
        steps.append({'Index': len(steps), 'PrimitiveId': REPLACE, 'Version': 2,
                      'Phase': 'Apply', 'InputSchemaId': REPLACE + '.input.v2', 'Intent': curves})
        steps.append(caller._step(len(steps), SLOTS, notify))
        return steps, exists, read, mutable

    def _preflight(self, request, context, properties, combined, steps, exists, read):
        super()._preflight(request, context, properties, None, steps[:-2], exists, read)

    def _apply(self, request, context, properties, steps, read, mutable):
        changed = list(super()._apply(request, context, properties, steps[:-2], read, mutable))
        asset = context.handoff(source_handle=mutable, target_provider_id='animation', access='mutable')
        for step in steps[-2:]:
            changed.extend(caller._invoke(context, step['PrimitiveId'], step['Intent'], asset=asset).changed_set)
        return tuple(changed)

    def _verify(self, request, context, properties, combined):
        exists, read, _ = caller._resolve(context, request.target_asset_path, caller.EXACT_CLASS)
        if not exists:
            raise caller._error('Combined Verify target missing.')
        asset = context.handoff(source_handle=read, target_provider_id='animation', access='read_only')
        result = caller._invoke(context, ENUMERATE, {'curve_names': ()}, asset=asset)
        if curve_wire(result.value) != combined[0]:
            raise caller._error('Actual complete curve readback differs.', 'CurveVerificationMismatch')
        super()._verify(request, context, properties, None)

dispatch = CombinedDispatch(dispatch.options)
)PY");
 return O;
}
bool SetupCombined(FFixture& F)
{
 using namespace AssetDocumentMontageNotifyIntegrationTests;
 if (!F.Setup()) return false;
 const FAnimationCurveIdentifier Id(FName(TEXT("CombinedCurve")), ERawCurveTrackTypes::RCT_Float);
 auto& C = F.Base.Montage->GetController();
 return C.AddCurve(Id, AACF_Editable, false) && C.SetCurveKeys(Id, Curve(false).Keys, false) && Save(F.Base.Montage.Get());
}
bool CombinedDocument(FFixture& F, bool BadSecondFamily = false)
{
 FString Body = Wire({Curve(true)}); Body.LeftChopInline(1);
 Body += TEXT(R"JSON(,"MontageNotify":{"notifies":[{"time":0.125,"track_index":1.0,"object":{"class":"/Script/Engine.AnimNotify_PlaySound","properties":{"VolumeMultiplier":0.25}}}],"notify_states":[{"time":0.875,"duration":0.125,"track_index":0.0,"object":{"class":"/Script/Engine.AnimNotifyState_TimedParticleEffect","properties":)JSON");
 Body += BadSecondFamily ? TEXT("{\"DoesNotExist\":1.0}") : TEXT("{\"bDestroyAtEnd\":false}");
 Body += TEXT("}}]}}");
 return F.Base.Document(Body, TEXT("Update"), false);
}
bool CheckCurve(FAutomationTestBase& T, const UAnimMontage& M, bool Edited)
{
 const auto& Curves = M.GetDataModel()->GetFloatCurves();
 if (!T.TestEqual(TEXT("actual model has one combined curve"), Curves.Num(), 1)) return false;
 const auto E = Curve(Edited);
 T.TestEqual(TEXT("actual curve name"), Curves[0].GetName(), FName(*E.Name));
 T.TestEqual(TEXT("actual curve flags"), Curves[0].GetCurveTypeFlags(), E.Flags);
 T.TestTrue(TEXT("actual curve keys equal expected values and modes"), Curves[0].FloatCurve.GetConstRefOfKeys() == E.Keys);
 T.TestEqual(TEXT("curve default unchanged"), Curves[0].FloatCurve.GetDefaultValue(), MAX_flt);
 return !T.HasAnyErrors();
}
bool CheckCombined(FAutomationTestBase& T, const UAnimMontage& M, bool WithStructure = false)
{
 CheckCurve(T, M, true);
 T.TestEqual(TEXT("both managed families replaced and unmanaged rows retained"), M.Notifies.Num(), 4);
 int32 Events = 0, States = 0;
 for (const auto& E : M.Notifies)
 {
  if (E.NotifyName == TEXT("AssetDocument.Notify"))
  { ++Events; const auto* N = Cast<UAnimNotify_PlaySound>(E.Notify); T.TestTrue(TEXT("new event property/time"), N && N->VolumeMultiplier == .25f && E.GetTime() == .125f); T.TestTrue(TEXT("new event GUID"), E.Guid.IsValid()); }
  if (E.NotifyName == TEXT("AssetDocument.NotifyState"))
  { ++States; const auto* N = Cast<UAnimNotifyState_TimedParticleEffect>(E.NotifyStateClass); T.TestTrue(TEXT("new state property/time/duration"), N && !N->bDestroyAtEnd && E.GetTime() == .875f && E.GetDuration() == .125f); T.TestTrue(TEXT("new state GUID"), E.Guid.IsValid()); }
 }
 T.TestEqual(TEXT("one new event"), Events, 1); T.TestEqual(TEXT("one new state"), States, 1);
 T.TestTrue(TEXT("unmanaged state preserved"), M.Notifies.ContainsByPredicate([](const auto& E) { return E.NotifyName == TEXT("State") && E.GetTime() == .25f && E.GetDuration() == .5f; }));
 T.TestTrue(TEXT("unmanaged instant preserves its relative segment position"), M.Notifies.ContainsByPredicate([&](const auto& E) { return E.NotifyName == TEXT("Event") && E.GetTime() == (WithStructure ? 2.5f : 1.5f); }));
 T.TestEqual(TEXT("curve retained scalar baseline"), M.RateScale, 1.f);
 T.TestEqual(TEXT("combined intended length"), M.GetDataModel()->GetNumberOfFrames(), WithStructure ? 90 : 60);
 return !T.HasAnyErrors();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageNotifyCombinedApply, "UEAssetDoc.MontageNotifyIntegration.Combined.ApplyFile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageNotifyCombinedApply::RunTest(const FString&)
{
 using namespace AssetDocumentMontageNotifyIntegrationTests;
 using namespace AssetDocumentMontageNotifyCombinedTests;
 for (const FString Failure : {TEXT(""), TEXT("bad_property"), TEXT("repair"), TEXT("verify")})
 {
  FFixture F; if (!TestTrue(TEXT("combined saved baseline"), SetupCombined(F) && CombinedDocument(F, Failure == TEXT("bad_property")))) return false;
  const auto Old = F.Base.Montage->Notifies;
  const TArray<FString> Files{PackageFile(F.Base.Target), F.Base.Sidecar, PackageFile(F.A->GetPathName()), PackageFile(F.B->GetPathName()), PackageFile(F.Base.Old->GetPathName())};
  TArray<TArray<uint8>> Original; for (const auto& File : Files) Original.Add(Bytes(File));
  FHarness H(CombinedOptions(Failure == TEXT("bad_property") ? FString() : Failure)); if (!H.Initialize(*this)) return false;
  const auto R = H.Apply(F.Base.Sidecar); for (const auto& D : R.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  TestTrue(TEXT("actual curve forward ran first"), H.Changes.ContainsByPredicate([](const auto& C) { return C.AuthorityId == Replace; }));
  TestTrue(TEXT("curve invokes before the single combined notify operation"), H.Invocations.IndexOfByKey(Replace) != INDEX_NONE && H.Invocations.IndexOfByKey(Replace) < H.Invocations.IndexOfByKey(SlotsId));
  if (Failure.IsEmpty()) { if (!TestTrue(TEXT("combined request commits"), R.IsSuccess())) return false; CheckCombined(*this, *F.Base.Montage); }
  else
  {
   TestFalse(TEXT("combined injected failure rejects"), R.IsSuccess());
   TestEqual(TEXT("only late Verify failure reaches persistence"), H.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Persist), Failure == TEXT("verify"));
   if (Failure == TEXT("repair")) TestTrue(TEXT("actual repair injection observed"), R.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("S1Fixture.InjectedFailure"); }));
   if (Failure == TEXT("verify")) TestTrue(TEXT("actual staged verify failure observed"), R.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("Persistence.CanonicalMismatch"); }));
   TestTrue(TEXT("curve inverse observed"), H.Inverses.Contains(Replace)); TestTrue(TEXT("single combined timeline inverse observed"), H.Inverses.Contains(SlotsId));
   TestTrue(TEXT("timeline inverse precedes curve inverse"), H.Inverses.IndexOfByKey(SlotsId) < H.Inverses.IndexOfByKey(Replace));
   CheckCurve(*this, *F.Base.Montage, false);
   if (TestEqual(TEXT("mixed old timeline restored"), F.Base.Montage->Notifies.Num(), Old.Num()))
    for (int32 I = 0; I < Old.Num(); ++I) TestTrue(TEXT("exact mixed preimage"), FAnimNotifyEvent::StaticStruct()->CompareScriptStruct(&Old[I], &F.Base.Montage->Notifies[I], 0));
   for (int32 I = 0; I < Files.Num(); ++I) TestTrue(TEXT("combined rollback byte symmetry"), Bytes(Files[I]) == Original[I]);
  }
  TestFalse(TEXT("combined terminal package clean"), F.Base.Montage->GetOutermost()->IsDirty());
  for (int32 I = 2; I < Files.Num(); ++I) TestTrue(TEXT("combined dependencies remain unchanged"), Bytes(Files[I]) == Original[I]);
 }
 return !HasAnyErrors();
}

namespace AssetDocumentMontageNotifyStructuredTests
{
using namespace AssetDocumentMontageNotifyCombinedTests;
constexpr const TCHAR* SnapshotId = TEXT("animation.montage.profile.snapshot.v1");
FOptions StructuredOptions(const FString& Failure = {})
{
 using namespace AssetDocumentMontageNotifyIntegrationTests;
 auto O = CombinedOptions(Failure); O.ExtraRequirements.Add({SnapshotId, 1, TEXT("animation_profile")});
 O.EntrySuffix += TEXT(R"PY(
SNAPSHOT = 'animation.montage.profile.snapshot.v1'
row = caller.PrimitiveRequirement(SNAPSHOT, 1, 'animation_profile')
caller.REQUIREMENTS += (row,)
caller._BY_ID[SNAPSHOT] = row
_combined_interpret = caller.interpret

def interpret_structured(document):
    body = document.get('Body', {})
    notify = body.get('MontageNotify', {})
    if type(notify) is not dict or list(notify) != ['notifies', 'notify_states', 'structure']:
        raise caller._error('Structured fixture requires both families then structure.')
    stripped = dict(notify)
    del stripped['structure']
    curves, _ = _combined_interpret(dict(document, Body=dict(body, MontageNotify=stripped)))[1]
    return {}, (curves, notify)
caller.interpret = interpret_structured

class StructuredDispatch(CombinedDispatch):
    def _verify(self, request, context, properties, combined):
        exists, read, _ = caller._resolve(context, request.target_asset_path, caller.EXACT_CLASS)
        if not exists:
            raise caller._error('Structured Verify target missing.')
        asset = context.handoff(source_handle=read, target_provider_id='animation', access='read_only')
        actual = caller._invoke(context, SNAPSHOT, {}, asset=asset).value
        if type(actual) is not dict or set(actual) != {'skeleton', 'preview_mesh', 'curves', 'slots', 'sections', 'notifies', 'notify_states', 'notify_tracks', 'settings'}:
            raise caller._error('Actual profile snapshot must expose exactly nine fields.', 'ProfileSnapshotMismatch')
        expected_skeleton = request.target_asset_path.rsplit('/', 1)[0] + '/Old.Old'
        if actual['skeleton'] != expected_skeleton or actual['preview_mesh'] is not None:
            raise caller._error('Actual profile references differ.', 'ProfileSnapshotMismatch')
        expected = combined[1]
        if curve_wire(actual['curves']) != combined[0]['curves'] or actual['slots'] != expected['structure']['slots'] or actual['sections'] != expected['structure']['sections']:
            raise caller._error('Actual profile curve or structural values differ.', 'ProfileSnapshotMismatch')
        for family in ('notifies', 'notify_states'):
            if len(actual[family]) != len(expected[family]):
                raise caller._error('Actual profile family count differs.', 'ProfileSnapshotMismatch')
            for got, want in zip(actual[family], expected[family]):
                for key in ('time', 'track_index') + (('duration',) if family == 'notify_states' else ()):
                    if got[key] != want[key]:
                        raise caller._error('Actual notify placement differs.', 'ProfileSnapshotMismatch')
                if got['object']['class'] != want['object']['class']:
                    raise caller._error('Actual notify class differs.', 'ProfileSnapshotMismatch')
                for name, value in want['object']['properties'].items():
                    if got['object']['properties'].get(name) != value:
                        raise caller._error('Actual safe-property readback differs.', 'ProfileSnapshotMismatch')
        # Preserve full Curve enumerate verification and subsequent real late-failure hook.
        super()._verify(request, context, properties, combined)

dispatch = StructuredDispatch(dispatch.options)
)PY");
 return O;
}
FString Segment(const FString& Path, int32 Start, int32 Loops, const TCHAR* Slot, int32 Index)
{
 return FString::Printf(TEXT("{\"animation\":\"%s\",\"start\":%d.0,\"source_start\":0.0,\"source_end\":1.0,\"rate\":1.0,\"loops\":%d.0,\"previous_slot\":\"%s\",\"previous_segment\":%d.0}"), *Path, Start, Loops, Slot, Index);
}
bool StructuredDocument(FFixture& F, bool BadSecondFamily)
{
 if (!CombinedDocument(F, BadSecondFamily)) return false;
 FString Document; if (!FFileHelper::LoadFileToString(Document, *F.Base.Sidecar)) return false;
 // Append the exact structure leaf to the authored existing body, not to any readback.
 const FString Marker = TEXT("}}]}}"); const int32 At = Document.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
 if (At == INDEX_NONE) return false;
 FString Structure = TEXT(R"JSON(,"structure":{"slots":[{"name":"A","segments":[)JSON")
  + Segment(F.A->GetPathName(), 0, 2, TEXT("A"), 0) + TEXT(",") + Segment(F.B->GetPathName(), 2, 1, TEXT("A"), 1)
  + TEXT(R"JSON(]},{"name":"B","segments":[)JSON") + Segment(F.B->GetPathName(), 0, 1, TEXT("B"), 0)
  + TEXT(R"JSON(]}],"sections":[{"name":"First","next":"Second","time":0.0,"slot_name":"A","method":0.0},{"name":"Second","next":"First","time":1.0,"slot_name":"A","method":1.0}]})JSON");
 // Marker begins at object/row closes then array close. Insert after that array.
 Document.InsertAt(At + 3, Structure);
 return FFileHelper::SaveStringToFile(Document, *F.Base.Sidecar);
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageNotifyStructuredCombinedApply,
 "UEAssetDoc.MontageNotifyIntegration.Combined.StructuredApplyFile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageNotifyStructuredCombinedApply::RunTest(const FString&)
{
 using namespace AssetDocumentMontageNotifyIntegrationTests;
 using namespace AssetDocumentMontageNotifyStructuredTests;
 for (const FString Failure : {TEXT(""), TEXT("bad_property"), TEXT("repair"), TEXT("verify")})
 {
  FFixture F; if (!TestTrue(TEXT("structured combined input saved"), SetupCombined(F) && StructuredDocument(F, Failure == TEXT("bad_property")))) return false;
  const auto OldSlots = F.Base.Montage->SlotAnimTracks; const auto OldSections = F.Base.Montage->CompositeSections; const auto OldNotifies = F.Base.Montage->Notifies;
  const TArray<FString> Files{PackageFile(F.Base.Target), F.Base.Sidecar, PackageFile(F.A->GetPathName()), PackageFile(F.B->GetPathName()), PackageFile(F.Base.Old->GetPathName())};
  TArray<TArray<uint8>> Original; for (const auto& File : Files) Original.Add(Bytes(File));
  FHarness H(StructuredOptions(Failure == TEXT("bad_property") ? FString() : Failure)); if (!H.Initialize(*this)) return false;
  const auto R = H.Apply(F.Base.Sidecar); for (const auto& D : R.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  TestTrue(TEXT("structured request actually changed Curve before Notify"), H.Changes.ContainsByPredicate([](const auto& C) { return C.AuthorityId == Replace; }));
  if (Failure.IsEmpty())
  {
   if (!TestTrue(TEXT("structured combined request commits"), R.IsSuccess())) return false;
   CheckCombined(*this, *F.Base.Montage, true);
   // StructuredDispatch._verify invokes the readonly snapshot and rejects any
   // eight-field/value mismatch. R.IsSuccess() above requires that real Verify
   // to succeed; H.Invocations observes only mutating prepared invocations.
   TestEqual(TEXT("first segment loop expansion"), F.Base.Montage->SlotAnimTracks[0].AnimTrack.AnimSegments[0].LoopingCount, 2);
   TestEqual(TEXT("second segment actually moved"), F.Base.Montage->SlotAnimTracks[0].AnimTrack.AnimSegments[1].StartPos, 2.f);
   TestEqual(TEXT("section time actually changed"), F.Base.Montage->CompositeSections[1].GetTime(), 1.f);
   TestEqual(TEXT("section next now cycles"), F.Base.Montage->CompositeSections[1].NextSectionName, FName(TEXT("First")));
  }
  else
  {
   TestFalse(TEXT("structured combined failure rejects"), R.IsSuccess());
   TestTrue(TEXT("single full timeline inverse observed"), H.Inverses.Contains(SlotsId)); TestTrue(TEXT("curve inverse observed"), H.Inverses.Contains(Replace));
   TestTrue(TEXT("structure/timeline inverse runs before Curve"), H.Inverses.IndexOfByKey(SlotsId) < H.Inverses.IndexOfByKey(Replace));
   CheckCurve(*this, *F.Base.Montage, false);
   auto Same = [](const auto& A, const auto& B)
   {
    if (A.Num() != B.Num()) return false;
    for (int32 I = 0; I < A.Num(); ++I) if (!std::decay_t<decltype(A[I])>::StaticStruct()->CompareScriptStruct(&A[I], &B[I], 0)) return false;
    return true;
   };
   TestTrue(TEXT("all old slots restored"), Same(OldSlots, F.Base.Montage->SlotAnimTracks));
   TestTrue(TEXT("all old sections restored"), Same(OldSections, F.Base.Montage->CompositeSections));
   TestTrue(TEXT("all old mixed notifications restored"), Same(OldNotifies, F.Base.Montage->Notifies));
   TestEqual(TEXT("old model frame count restored"), F.Base.Montage->GetDataModel()->GetNumberOfFrames(), 60);
   for (int32 I = 0; I < Files.Num(); ++I) TestTrue(TEXT("structured rollback file symmetry"), Bytes(Files[I]) == Original[I]);
  }
  TestFalse(TEXT("structured terminal target clean"), F.Base.Montage->GetOutermost()->IsDirty());
 }
 return !HasAnyErrors();
}
#endif
