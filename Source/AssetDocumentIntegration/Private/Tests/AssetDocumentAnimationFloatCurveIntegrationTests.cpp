// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/AssetDocumentSkeletonTestHarness.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/TimeStretchCurve.h"
#include "HAL/PlatformProcess.h"
#include "ReferenceSkeleton.h"

namespace AssetDocumentFloatCurveIntegrationTests
{
using namespace AssetDocumentSkeletonTests;
constexpr const TCHAR* Replace = TEXT("animation.curve.float.replace.v2");
constexpr const TCHAR* Enumerate = TEXT("animation.curve.float.enumerate.v2");
FOptions Options(const FString& Failure = {})
{
 FOptions O; O.FailAfter = Failure;
 O.ExtraRequirements.Add({Replace, 2, TEXT("animation_profile")});
 O.ExtraRequirements.Add({Enumerate, 2, TEXT("animation_profile")});
 // Reuse the real host, preparation, transaction and persistence. Authority
 // admission still comes from the production catalog consumed by S1.
 O.EntrySuffix = TEXT(R"PY(
REPLACE = 'animation.curve.float.replace.v2'
ENUMERATE = 'animation.curve.float.enumerate.v2'
for primitive in (REPLACE, ENUMERATE):
    row = caller.PrimitiveRequirement(primitive, 2, 'animation_profile')
    caller.REQUIREMENTS += (row,)
    caller._BY_ID[primitive] = row
_original_interpret = caller.interpret

def curve_wire(value):
    # Canonical JSON adapters may supply integer-valued doubles. Every numeric
    # leaf of C1's bit-preserving wire schema is an integer, never a float value.
    if type(value) in (list, tuple):
        return [curve_wire(item) for item in value]
    if type(value) is dict:
        return {key: curve_wire(item) for key, item in value.items()}
    if type(value) is float:
        if not value.is_integer():
            raise caller._error('Curve wire integer is fractional.')
        return int(value)
    return value

def interpret_curves(document):
    body = document.get('Body', {})
    if type(body) is not dict or set(body) != {'Curves'}:
        raise caller._error('Curve fixture requires explicit complete Curves.')
    properties, _ = _original_interpret(dict(document, Body={}))
    return properties, {'curves': curve_wire(body['Curves'])}
caller.interpret = interpret_curves

class CurveDispatch(caller.SkeletonS1Dispatch):
    def registration_projection(self):
        result = super().registration_projection()
        result['BodySections'] = ['Curves']
        return result

    def _plan(self, request, context, properties, curves):
        if request.canonical_document.get('Action') != 'Update':
            raise caller._error('Curve fixture requires an existing target.')
        steps, exists, read, mutable = super()._plan(request, context, properties, None)
        steps.append({'Index': len(steps), 'PrimitiveId': REPLACE, 'Version': 2,
                      'Phase': 'Apply', 'InputSchemaId': REPLACE + '.input.v2', 'Intent': curves})
        return steps, exists, read, mutable

    def _preflight(self, request, context, properties, curves, steps, exists, read):
        super()._preflight(request, context, properties, None, steps[:-1], exists, read)

    def _apply(self, request, context, properties, steps, read, mutable):
        changed = list(super()._apply(request, context, properties, steps[:-1], read, mutable))
        asset = context.handoff(source_handle=mutable, target_provider_id='animation', access='mutable')
        changed.extend(caller._invoke(context, REPLACE, steps[-1]['Intent'], asset=asset).changed_set)
        return tuple(changed)

    def _verify(self, request, context, properties, curves):
        exists, read, _ = caller._resolve(context, request.target_asset_path, caller.EXACT_CLASS)
        if not exists:
            raise caller._error('Curve Verify target missing.')
        asset = context.handoff(source_handle=read, target_provider_id='animation', access='read_only')
        result = caller._invoke(context, ENUMERATE, {'curve_names': ()}, asset=asset)
        if curve_wire(result.value) != curves:
            raise caller._error('Full C1 snapshot differs from authored curves.', 'CurveVerificationMismatch')
        # Staged verify injection follows an actual successful C1 comparison.
        super()._verify(request, context, properties, None)

dispatch = CurveDispatch(dispatch.options)
)PY");
 return O;
}
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
TArray<FRow> Rows(int32 Variant)
{
 if (Variant == 3) return {};
 const bool Edited = Variant != 0;
 FRow Default; Default.Name = TEXT("DefaultOnly"); Default.Default = Variant == 2 ? MAX_flt : Edited ? 3.f : 2.f;
 Default.Comment = Edited ? TEXT("edited default") : TEXT("original default");
 Default.Color = Edited ? FLinearColor(.25f, .5f, .75f, 1.f) : FLinearColor(1.f, .25f, .5f, 1.f);
 FRow Weighted; Weighted.Name = TEXT("WeightedAuto"); Weighted.Flags = AACF_Editable | AACF_Metadata;
 Weighted.Comment = TEXT("full Auto tangents and two weights"); Weighted.Color = FLinearColor(.125f, .25f, .5f, .75f);
 Weighted.Pre = RCCE_Linear; Weighted.Post = RCCE_CycleWithOffset;
 for (int32 I = 0; I < 2; ++I)
 {
  FRichCurveKey K(static_cast<float>(I) * 2.f, I == 0 ? 1.f : 4.f);
  K.InterpMode = RCIM_Cubic; K.TangentMode = RCTM_Auto; K.TangentWeightMode = RCTWM_WeightedBoth;
  K.ArriveTangent = Edited ? .75f : .25f; K.LeaveTangent = Edited ? -.75f : -.25f;
  K.ArriveTangentWeight = Edited ? .8f : .2f; K.LeaveTangentWeight = Edited ? .3f : .4f;
  Weighted.Keys.Add(K);
 }
 FRow Stretch; Stretch.Name = TEXT("MontageTimeStretchCurve"); Stretch.Comment = TEXT("stock playback source");
 const TArray<float> Values = Edited ? TArray<float>{1.f, .7f, .2f, 0.f, 0.f} : TArray<float>{0.f, 0.f, .2f, .7f, 1.f};
 for (int32 I = 0; I < Values.Num(); ++I) { FRichCurveKey K(I * .5f, Values[I]); K.InterpMode = RCIM_Linear; Stretch.Keys.Add(K); }
 TArray<FRow> Result{Default, Weighted, Stretch};
 if (!Edited) { FRow Delete; Delete.Name = TEXT("DeleteMe"); Delete.Comment = TEXT("removed by full replacement"); Result.Add(Delete); }
 return Result;
}
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
bool Check(FAutomationTestBase& T, const UAnimMontage& M, int32 Variant)
{
 const auto Expected = Rows(Variant);
 const IAnimationDataModel* Model = M.GetDataModel();
 if (!T.TestNotNull(TEXT("authoritative model present"), Model)) return false;
 const auto& Actual = Model->GetFloatCurves();
 if (!T.TestEqual(TEXT("exact curve count includes deletion"), Actual.Num(), Expected.Num())) return false;
 for (int32 I = 0; I < Actual.Num(); ++I)
 {
  const auto& A = Actual[I]; const auto& E = Expected[I];
  T.TestEqual(TEXT("name and order"), A.GetName().ToString(), E.Name); T.TestEqual(TEXT("flags"), A.GetCurveTypeFlags(), E.Flags);
  T.TestEqual(TEXT("comment"), A.Comment, E.Comment);
  const FLinearColor Color = A.GetColor();
  T.TestEqual(TEXT("color R"), Bits(Color.R), Bits(E.Color.R)); T.TestEqual(TEXT("color G"), Bits(Color.G), Bits(E.Color.G));
  T.TestEqual(TEXT("color B"), Bits(Color.B), Bits(E.Color.B)); T.TestEqual(TEXT("color A"), Bits(Color.A), Bits(E.Color.A));
  T.TestEqual(TEXT("default including explicit unset"), Bits(A.FloatCurve.GetDefaultValue()), Bits(E.Default));
  T.TestEqual(TEXT("pre extrapolation"), A.FloatCurve.PreInfinityExtrap.GetValue(), E.Pre);
  T.TestEqual(TEXT("post extrapolation"), A.FloatCurve.PostInfinityExtrap.GetValue(), E.Post);
  const auto& Keys = A.FloatCurve.GetConstRefOfKeys();
  if (!T.TestEqual(TEXT("key count"), Keys.Num(), E.Keys.Num())) return false;
  for (int32 J = 0; J < Keys.Num(); ++J)
  {
   const auto& K = Keys[J]; const auto& Want = E.Keys[J];
   T.TestEqual(TEXT("key time bits"), Bits(K.Time), Bits(Want.Time)); T.TestEqual(TEXT("key value bits"), Bits(K.Value), Bits(Want.Value));
   T.TestEqual(TEXT("arrive tangent bits"), Bits(K.ArriveTangent), Bits(Want.ArriveTangent)); T.TestEqual(TEXT("leave tangent bits"), Bits(K.LeaveTangent), Bits(Want.LeaveTangent));
   T.TestEqual(TEXT("interp mode"), K.InterpMode, Want.InterpMode); T.TestEqual(TEXT("tangent mode"), K.TangentMode, Want.TangentMode);
   T.TestEqual(TEXT("weight mode"), K.TangentWeightMode, Want.TangentWeightMode);
   T.TestEqual(TEXT("arrive weight bits"), Bits(K.ArriveTangentWeight), Bits(Want.ArriveTangentWeight)); T.TestEqual(TEXT("leave weight bits"), Bits(K.LeaveTangentWeight), Bits(Want.LeaveTangentWeight));
  }
  if (E.Name == TEXT("DefaultOnly")) T.TestEqual(TEXT("empty curve evaluates its real default or caller fallback"), A.FloatCurve.Eval(.5f, -11.f), E.Default == MAX_flt ? -11.f : E.Default);
 }
 T.TestEqual(TEXT("two-second source duration retained"), M.GetPlayLength(), 2.f);
 return !T.HasAnyErrors();
}
bool Mapping(FAutomationTestBase& T, UAnimMontage& M, bool Advance, TArray<float>& Out)
{
 if (Advance) { FAnimMontageInstance Instance; Instance.Initialize(&M); Instance.EditorOnly_PreAdvance(); }
 FTimeStretchCurveInstance Map; Map.InitializeFromPlayRate(2.f, M.TimeStretchCurve);
 if (!T.TestTrue(TEXT("stock TimeStretch rate-two mapping valid"), Map.HasValidData())) return false;
 Out.Reset();
 for (float Original : {.3f, .7f, 1.1f, 1.7f})
 {
  const float Target = Map.Convert_P_Original_To_Target(Map.BinarySearchMarkerIndex(Original, Map.GetMarkers_Original()), Original);
  Out.Add(Target);
  T.TestTrue(TEXT("playback map round trip"), FMath::IsNearlyEqual(Map.Convert_P_Target_To_Original(Map.BinarySearchMarkerIndex(Target, Map.GetMarkers_Target()), Target), Original, .0001f));
 }
 return true;
}
void SameMapping(FAutomationTestBase& T, const TArray<float>& A, const TArray<float>& B)
{
 if (!T.TestEqual(TEXT("mapping sample count"), A.Num(), B.Num())) return;
 for (int32 I = 0; I < A.Num(); ++I) T.TestTrue(TEXT("mapping sample restored"), FMath::IsNearlyEqual(A[I], B[I], .0001f));
}
struct FFixture
{
 FAssets Base;
 TStrongObjectPtr<UAnimSequence> Source;
 bool Setup()
 {
  if (!Base.Setup()) return false;
  { FReferenceSkeletonModifier M(Base.Old.Get()); M.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); }
  if (!Save(Base.Old.Get())) return false;
  Source.Reset(NewObject<UAnimSequence>(CreatePackage(*(Base.Root + TEXT("/Source"))), TEXT("Source"), RF_Public | RF_Standalone | RF_Transactional));
  Source->SetSkeleton(Base.Old.Get()); FAssetRegistryModule::AssetCreated(Source.Get());
  auto& C = Source->GetController(); C.InitializeModel(); C.SetFrameRate(FFrameRate(30, 1), false); C.SetNumberOfFrames(FFrameNumber(60), false);
  if (!C.AddBoneCurve(TEXT("root"), false)) return false;
  TArray<FVector3f> P, S; TArray<FQuat4f> R;
  P.Init(FVector3f::ZeroVector, 61); S.Init(FVector3f::OneVector, 61); R.Init(FQuat4f::Identity, 61);
  if (!C.SetBoneTrackKeys(TEXT("root"), P, R, S, false)) return false;
  C.NotifyPopulated(); Source->WaitOnExistingCompression(); if (!Save(Source.Get())) return false;
  auto& M = *Base.Montage; auto& MC = M.GetController(); MC.InitializeModel(); MC.SetFrameRate(FFrameRate(30, 1), false); MC.SetNumberOfFrames(FFrameNumber(60), false); MC.NotifyPopulated();
  M.TimeStretchCurveName = TEXT("MontageTimeStretchCurve"); M.SyncSlotIndex = INDEX_NONE;
  M.SlotAnimTracks.Reset(); auto& Slot = M.SlotAnimTracks.AddDefaulted_GetRef(); Slot.SlotName = TEXT("DefaultSlot");
  FAnimSegment Segment; Segment.SetAnimReference(Source.Get()); Segment.StartPos = 0.f; Segment.AnimStartTime = 0.f; Segment.AnimEndTime = 2.f; Segment.AnimPlayRate = 1.f; Segment.LoopingCount = 1;
  Slot.AnimTrack.AnimSegments.Add(Segment);
  M.CompositeSections.Reset(); auto& Section = M.CompositeSections.AddDefaulted_GetRef(); Section.SectionName = TEXT("Start"); Section.Link(&M, 0.f, 0);
  M.RefreshCacheData(); return Save(&M);
 }
 bool Document(int32 Variant) { return Base.Document(Wire(Rows(Variant)), TEXT("Update"), false); }
 ~FFixture()
 {
  if (Base.bKeep || !Source.IsValid()) return;
  auto* P = Source->GetOutermost(); FAssetRegistryModule::AssetDeleted(Source.Get()); Source->ClearFlags(RF_Public | RF_Standalone);
  Source->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional); Source->MarkAsGarbage(); P->SetDirtyFlag(false); P->MarkAsGarbage();
 }
};
bool Apply(FAutomationTestBase& T, FHarness& H, FFixture& F, int32 Variant)
{
 if (!T.TestTrue(TEXT("complete curve sidecar written"), F.Document(Variant))) return false;
 const auto Result = H.Apply(F.Base.Sidecar);
 for (const auto& D : Result.Diagnostics) T.AddInfo(D.Code + TEXT(": ") + D.Message);
 return T.TestTrue(TEXT("real C0 ApplyFile and C1 staged canonical verification commit"), Result.IsSuccess()) && Check(T, *F.Base.Montage, Variant);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloatCurveIntegrationApplyTest, "UEAssetDoc.FloatCurveIntegration.ApplyFile.Success", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFloatCurveIntegrationApplyTest::RunTest(const FString&)
{
 using namespace AssetDocumentFloatCurveIntegrationTests;
 FFixture F; if (!TestTrue(TEXT("saved continuous two-second montage fixture"), F.Setup())) return false;
 const auto SourceBytes = Bytes(PackageFile(F.Source->GetPathName())), SkeletonBytes = Bytes(PackageFile(F.Base.Old->GetPathName()));
 const FGuid Guid = F.Base.Montage->GetSkeletonGuid();
 FHarness H(Options()); if (!H.Initialize(*this) || !Apply(*this, H, F, 0)) return false;
 TArray<float> Before, Edited, Advanced;
 if (!Mapping(*this, *F.Base.Montage, false, Before) || !Apply(*this, H, F, 1) || !Mapping(*this, *F.Base.Montage, false, Edited)) return false;
 TestTrue(TEXT("ordinary PreSave source edit changes actual playback mapping"), Before != Edited);
 if (!Mapping(*this, *F.Base.Montage, true, Advanced)) return false;
 SameMapping(*this, Edited, Advanced);
 if (!Apply(*this, H, F, 2) || !Apply(*this, H, F, 3)) return false;
 TestFalse(TEXT("saved source deletion clears derived TimeStretch"), F.Base.Montage->TimeStretchCurve.IsValid());
 FAnimMontageInstance Instance; Instance.Initialize(F.Base.Montage.Get()); Instance.EditorOnly_PreAdvance();
 TestFalse(TEXT("public preview lifecycle also keeps deleted mapping empty"), F.Base.Montage->TimeStretchCurve.IsValid());
 TestEqual(TEXT("Skeleton identity retained"), F.Base.Montage->GetSkeleton(), F.Base.Old.Get()); TestEqual(TEXT("Skeleton GUID retained"), F.Base.Montage->GetSkeletonGuid(), Guid);
 TestFalse(TEXT("target clean after commits"), F.Base.Montage->GetOutermost()->IsDirty());
 TestTrue(TEXT("reference source package immutable"), Bytes(PackageFile(F.Source->GetPathName())) == SourceBytes);
 TestTrue(TEXT("Skeleton package immutable"), Bytes(PackageFile(F.Base.Old->GetPathName())) == SkeletonBytes);
 TestTrue(TEXT("real replace ChangedSet"), H.Changes.ContainsByPredicate([&](const auto& C) { return C.AuthorityId == Replace && C.Path == TEXT("/Curves") && C.StableObjectId == F.Base.Target; }));
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloatCurveIntegrationRollbackTest, "UEAssetDoc.FloatCurveIntegration.ApplyFile.Rollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFloatCurveIntegrationRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentFloatCurveIntegrationTests;
 for (const FString Failure : {TEXT("repair"), TEXT("verify")}) for (int32 Desired : {1, 3})
 {
  FFixture F; if (!TestTrue(TEXT("saved rollback fixture"), F.Setup())) return false;
  { FHarness Seed(Options()); if (!Seed.Initialize(*this) || !Apply(*this, Seed, F, 0)) return false; }
  TArray<float> Before, Restored; if (!Mapping(*this, *F.Base.Montage, false, Before) || !F.Document(Desired)) return false;
  const FGuid Guid = F.Base.Montage->GetSkeletonGuid();
  const auto Package = Bytes(PackageFile(F.Base.Target)), Sidecar = Bytes(F.Base.Sidecar);
  FHarness H(Options(Failure)); if (!H.Initialize(*this)) return false;
  const auto Result = H.Apply(F.Base.Sidecar); for (const auto& D : Result.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  TestFalse(TEXT("late failure rejects curve ApplyFile"), Result.IsSuccess());
  const bool Verify = Failure == TEXT("verify");
  TestEqual(TEXT("only staged verify reaches Persist"), H.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Persist), Verify);
  TestTrue(TEXT("concrete phase failure mapping"), Result.Diagnostics.ContainsByPredicate([&](const auto& D) { return D.Code == (Verify ? TEXT("Persistence.CanonicalMismatch") : TEXT("S1Fixture.InjectedFailure")); }));
  if (Verify) TestTrue(TEXT("invalid durable receipt rejected"), Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("RequestCoordinator.InvalidDurableReceipt"); }));
  TestTrue(TEXT("real curve forward preceded failure"), H.Changes.ContainsByPredicate([](const auto& C) { return C.AuthorityId == Replace; }));
  TestTrue(TEXT("typed inverse executed"), H.Inverses.Contains(Replace));
  Check(*this, *F.Base.Montage, 0);
  if (!Mapping(*this, *F.Base.Montage, true, Restored)) return false;
  SameMapping(*this, Before, Restored);
  TestEqual(TEXT("Skeleton restored/retained"), F.Base.Montage->GetSkeleton(), F.Base.Old.Get()); TestEqual(TEXT("GUID retained"), F.Base.Montage->GetSkeletonGuid(), Guid);
  TestFalse(TEXT("dirty restored"), F.Base.Montage->GetOutermost()->IsDirty());
  TestTrue(TEXT("package bytes restored"), Bytes(PackageFile(F.Base.Target)) == Package); TestTrue(TEXT("sidecar bytes restored"), Bytes(F.Base.Sidecar) == Sidecar);
  TestTrue(TEXT("rollback handle invalidation"), H.Invalidations.ContainsByPredicate([](const auto& I) { return I.Reason == EAssetDocumentHandleInvalidationReason::Rollback; }));
  TestFalse(TEXT("failure never commits"), H.Invalidations.ContainsByPredicate([](const auto& I) { return I.Reason == EAssetDocumentHandleInvalidationReason::Commit; }));
 }
 return !HasAnyErrors();
}

#endif
