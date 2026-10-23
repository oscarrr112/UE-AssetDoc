// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#if WITH_DEV_AUTOMATION_TESTS

#include "Provider/Curve/AssetDocumentAnimationCurveOperations.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveSnapshot.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimMontage.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace AssetDocumentAnimationCurveSnapshotTests
{
float FromBits(uint32 Bits)
{
    float Value;
    FMemory::Memcpy(&Value, &Bits, sizeof(Value));
    return Value;
}

const FAssetDocumentRuntimeValueDto* Field(
    const FAssetDocumentRuntimeValueDto& Object, const TCHAR* Name)
{
    const int32 Index = Object.ObjectKeys.IndexOfByKey(Name);
    return Object.Kind == EAssetDocumentRuntimeValueKind::Object
        && Object.Elements.IsValidIndex(Index) ? &Object.Elements[Index] : nullptr;
}

void AssertBits(FAutomationTestBase& Test, const FAssetDocumentRuntimeValueDto& Object,
    const TCHAR* Name, uint32 Expected)
{
    const FAssetDocumentRuntimeValueDto* Value = Field(Object, Name);
    if (!Test.TestNotNull(FString::Printf(TEXT("%s float field exists"), Name), Value)) return;
    Test.TestEqual(TEXT("float is exactly one bits object"), Value->ObjectKeys,
        TArray<FString>{TEXT("bits")});
    const FAssetDocumentRuntimeValueDto* Bits = Field(*Value, TEXT("bits"));
    if (!Test.TestNotNull(TEXT("float bits exist"), Bits)) return;
    Test.TestEqual(TEXT("float bits use integer transport"), Bits->Kind,
        EAssetDocumentRuntimeValueKind::Integer);
    Test.TestEqual(FString::Printf(TEXT("%s bits are exact"), Name),
        Bits->IntegerValue, static_cast<int64>(Expected));
}

FAssetDocumentPrimitiveInvocation Invocation(TArray<FString> Names = {})
{
    FAssetDocumentPrimitiveInvocation Result;
    Result.PrimitiveId = TEXT("animation.curve.float.enumerate.v2");
    Result.Version = 2;
    Result.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
    Result.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
    Result.ValueArguments.ObjectKeys = {TEXT("curve_names")};
    FAssetDocumentRuntimeValueDto Array;
    Array.Kind = EAssetDocumentRuntimeValueKind::Array;
    for (const FString& Name : Names)
    {
        FAssetDocumentRuntimeValueDto Value;
        Value.Kind = EAssetDocumentRuntimeValueKind::String;
        Value.StringValue = Name;
        Array.Elements.Add(MoveTemp(Value));
    }
    Result.ValueArguments.Elements.Add(MoveTemp(Array));
    return Result;
}

FRichCurveKey WeightedKey(float Time, float Value)
{
    FRichCurveKey Key(Time, Value);
    Key.InterpMode = RCIM_Cubic;
    Key.TangentMode = RCTM_Break;
    Key.TangentWeightMode = RCTWM_WeightedBoth;
    Key.ArriveTangent = 2.25f;
    Key.LeaveTangent = -3.5f;
    Key.ArriveTangentWeight = 0.75f;
    Key.LeaveTangentWeight = 1.75f;
    return Key;
}

void AssertRejectedSnapshot(FAutomationTestBase& Test, TConstArrayView<FFloatCurve> Curves)
{
    FAssetDocumentRuntimeValueDto Snapshot;
    Snapshot.Kind = EAssetDocumentRuntimeValueKind::String;
    Snapshot.StringValue = TEXT("stale output must be cleared");
    Test.TestFalse(TEXT("invalid native state is rejected"),
        CaptureAnimationFloatCurveSnapshotV2(Curves, Snapshot));
    Test.TestEqual(TEXT("no partial value survives rejection"), Snapshot.Kind,
        EAssetDocumentRuntimeValueKind::Null);
    Test.TestTrue(TEXT("no partial rows survive rejection"), Snapshot.Elements.IsEmpty());
}
} // namespace AssetDocumentAnimationCurveSnapshotTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationCurveSnapshotBitsTest,
    "UEAssetDoc.AnimationProvider.CurveSnapshot.BitsAndNativeValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationCurveSnapshotBitsTest::RunTest(const FString&)
{
    using namespace AssetDocumentAnimationCurveSnapshotTests;
    // Plain test-owned value structs exercise otherwise unconstructible native
    // preimages. They never enter an animation model or a production write path.
    FFloatCurve Curve(FName(TEXT("Zeta")), MIN_int32 | AACF_Editable);
    Curve.Color = FLinearColor(FromBits(0x80000000u), FromBits(1u),
        FromBits(0x7f800000u), FromBits(0x7fc12345u));
    Curve.Comment = TEXT("精确曲线备注");
    Curve.FloatCurve.SetKeys({WeightedKey(0.25f, 1.5f), WeightedKey(1.f, -4.f)});
    Curve.FloatCurve.SetDefaultValue(2.f);
    Curve.FloatCurve.PreInfinityExtrap = RCCE_CycleWithOffset;
    Curve.FloatCurve.PostInfinityExtrap = RCCE_Oscillate;
    FFloatCurve Empty(FName(TEXT("Alpha")), 0);
    TArray<FFloatCurve> Curves{Curve, Empty};
    FAssetDocumentRuntimeValueDto Snapshot;
    if (!TestTrue(TEXT("complete native snapshot captured"),
        CaptureAnimationFloatCurveSnapshotV2(Curves, Snapshot))) return false;
    if (!TestEqual(TEXT("native order and both curves retained"), Snapshot.Elements.Num(), 2)) return false;
    const FAssetDocumentRuntimeValueDto& Row = Snapshot.Elements[0];
    TestEqual(TEXT("exact ordered v2 row fields"), Row.ObjectKeys,
        TArray<FString>{TEXT("name"), TEXT("flags"), TEXT("color"), TEXT("comment"),
            TEXT("default_value"), TEXT("pre_extrapolation"), TEXT("post_extrapolation"), TEXT("keys")});
    if (!TestEqual(TEXT("complete row field count"), Row.Elements.Num(), 8)) return false;
    TestEqual(TEXT("row order preserved"), Row.Elements[0].StringValue, FString(TEXT("Zeta")));
    TestEqual(TEXT("unknown signed flags retained"), Row.Elements[1].IntegerValue,
        static_cast<int64>(MIN_int32 | AACF_Editable));
    TestEqual(TEXT("comment retained"), Row.Elements[3].StringValue, FString(TEXT("精确曲线备注")));
    const FAssetDocumentRuntimeValueDto& Color = Row.Elements[2];
    TestEqual(TEXT("color retains RGBA order"), Color.ObjectKeys,
        TArray<FString>{TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")});
    AssertBits(*this, Color, TEXT("r"), 0x80000000u);
    AssertBits(*this, Color, TEXT("g"), 1u);
    AssertBits(*this, Color, TEXT("b"), 0x7f800000u);
    AssertBits(*this, Color, TEXT("a"), 0x7fc12345u);
    const auto* HasDefault = Field(Row.Elements[4], TEXT("has_value"));
    const auto* DefaultBits = Field(Row.Elements[4], TEXT("float32_bits"));
    if (!TestNotNull(TEXT("default presence field"), HasDefault)
        || !TestNotNull(TEXT("default bits field"), DefaultBits)) return false;
    TestTrue(TEXT("explicit default retained"), HasDefault->BooleanValue);
    TestEqual(TEXT("default 2 native bits"), DefaultBits->IntegerValue, int64(0x40000000u));
    TestEqual(TEXT("pre extrapolation retained"), Row.Elements[5].IntegerValue, int64(1));
    TestEqual(TEXT("post extrapolation retained"), Row.Elements[6].IntegerValue, int64(2));
    if (!TestEqual(TEXT("both weighted keys retained"), Row.Elements[7].Elements.Num(), 2)) return false;
    const FAssetDocumentRuntimeValueDto& Key = Row.Elements[7].Elements[0];
    TestEqual(TEXT("nine exact ordered key fields"), Key.ObjectKeys,
        TArray<FString>{TEXT("time"), TEXT("value"), TEXT("arrive_tangent"),
            TEXT("leave_tangent"), TEXT("interp_mode"), TEXT("tangent_mode"),
            TEXT("tangent_weight_mode"), TEXT("arrive_tangent_weight"), TEXT("leave_tangent_weight")});
    AssertBits(*this, Key, TEXT("time"), 0x3e800000u);
    AssertBits(*this, Key, TEXT("value"), 0x3fc00000u);
    AssertBits(*this, Key, TEXT("arrive_tangent"), 0x40100000u);
    AssertBits(*this, Key, TEXT("leave_tangent"), 0xc0600000u);
    AssertBits(*this, Key, TEXT("arrive_tangent_weight"), 0x3f400000u);
    AssertBits(*this, Key, TEXT("leave_tangent_weight"), 0x3fe00000u);
    for (const auto& Expected : TArray<TPair<FString, int64>>{
        {TEXT("interp_mode"), 2}, {TEXT("tangent_mode"), 2}, {TEXT("tangent_weight_mode"), 3}})
    {
        const auto* Mode = Field(Key, *Expected.Key);
        if (TestNotNull(TEXT("mode exists"), Mode)) TestEqual(TEXT("native mode retained"), Mode->IntegerValue, Expected.Value);
    }
    const auto* EmptyDefault = Field(Snapshot.Elements[1], TEXT("default_value"));
    if (!TestNotNull(TEXT("empty curve default state"), EmptyDefault)) return false;
    const auto* EmptyHasDefault = Field(*EmptyDefault, TEXT("has_value"));
    const auto* EmptyBits = Field(*EmptyDefault, TEXT("float32_bits"));
    if (!TestNotNull(TEXT("unset presence"), EmptyHasDefault)
        || !TestNotNull(TEXT("unset bits"), EmptyBits)) return false;
    TestFalse(TEXT("MAX_flt is unset"), EmptyHasDefault->BooleanValue);
    TestEqual(TEXT("unset has null bits"), EmptyBits->Kind, EAssetDocumentRuntimeValueKind::Null);
    for (uint32 Bits : {0x80000000u, 1u, 0x7f800000u, 0x7fc12345u})
    {
        Empty.FloatCurve.SetDefaultValue(FromBits(Bits));
        TArray<FFloatCurve> One{Empty};
        TestTrue(TEXT("non-sentinel default encodes"), CaptureAnimationFloatCurveSnapshotV2(One, Snapshot));
        const auto* Default = Field(Snapshot.Elements[0], TEXT("default_value"));
        const auto* ActualBits = Default ? Field(*Default, TEXT("float32_bits")) : nullptr;
        if (TestNotNull(TEXT("non-sentinel bits exist"), ActualBits))
            TestEqual(TEXT("non-sentinel default bits exact"), ActualBits->IntegerValue, static_cast<int64>(Bits));
    }
    FFloatCurve Special(FName(TEXT("SpecialBits")), 0);
    FRichCurveKey SpecialKey = WeightedKey(FromBits(0x80000000u), FromBits(0x7fc12345u));
    SpecialKey.ArriveTangent = FromBits(1u);
    SpecialKey.LeaveTangent = FromBits(0xff800000u);
    SpecialKey.ArriveTangentWeight = FromBits(0x80000000u);
    SpecialKey.LeaveTangentWeight = FromBits(0x7f800000u);
    Special.FloatCurve.SetKeys({SpecialKey});
    TestTrue(TEXT("non-time float payloads encode without normalization"),
        CaptureAnimationFloatCurveSnapshotV2(TArray<FFloatCurve>{Special}, Snapshot));
    const auto* SpecialKeys = Field(Snapshot.Elements[0], TEXT("keys"));
    if (!TestNotNull(TEXT("special keys exist"), SpecialKeys) || SpecialKeys->Elements.IsEmpty()) return false;
    AssertBits(*this, SpecialKeys->Elements[0], TEXT("time"), 0x80000000u);
    AssertBits(*this, SpecialKeys->Elements[0], TEXT("value"), 0x7fc12345u);
    AssertBits(*this, SpecialKeys->Elements[0], TEXT("arrive_tangent"), 1u);
    AssertBits(*this, SpecialKeys->Elements[0], TEXT("leave_tangent"), 0xff800000u);
    AssertBits(*this, SpecialKeys->Elements[0], TEXT("arrive_tangent_weight"), 0x80000000u);
    AssertBits(*this, SpecialKeys->Elements[0], TEXT("leave_tangent_weight"), 0x7f800000u);
    TestEqual(TEXT("capture never changed original default"), Curve.FloatCurve.GetDefaultValue(), 2.f);
    TArray<FFloatCurve> Duplicate{Curve, Curve};
    AssertRejectedSnapshot(*this, Duplicate);
    FFloatCurve Nameless;
    AssertRejectedSnapshot(*this, TArray<FFloatCurve>{Nameless});
    FFloatCurve Invalid = Curve;
    Invalid.FloatCurve.PreInfinityExtrap = static_cast<ERichCurveExtrapolation>(6);
    AssertRejectedSnapshot(*this, TArray<FFloatCurve>{Invalid});
    Invalid = Curve;
    Invalid.FloatCurve.PostInfinityExtrap = static_cast<ERichCurveExtrapolation>(6);
    AssertRejectedSnapshot(*this, TArray<FFloatCurve>{Invalid});
    for (int32 InvalidField = 0; InvalidField < 7; ++InvalidField)
    {
        Invalid = Curve;
        Invalid.SetName(FName(TEXT("InvalidNativeState")));
        FRichCurveKey First = WeightedKey(0.25f, 1.f);
        FRichCurveKey Second = WeightedKey(1.f, 2.f);
        if (InvalidField == 0) Second.Time = First.Time;
        if (InvalidField == 1) Second.Time = -1.f;
        if (InvalidField == 2) First.Time = FromBits(0x7f800000u);
        if (InvalidField == 3) First.InterpMode = static_cast<ERichCurveInterpMode>(4);
        if (InvalidField == 4) First.TangentMode = static_cast<ERichCurveTangentMode>(5);
        if (InvalidField == 6) First.Time = FromBits(0x7fc12345u);
        if (InvalidField == 5) First.TangentWeightMode = static_cast<ERichCurveTangentWeightMode>(4);
        Invalid.FloatCurve.SetKeys({First, Second});
        AssertRejectedSnapshot(*this, TArray<FFloatCurve>{Curve, Invalid});
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationCurveSnapshotModelTest,
    "UEAssetDoc.AnimationProvider.CurveSnapshot.ModelEnumeration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationCurveSnapshotModelTest::RunTest(const FString&)
{
    using namespace AssetDocumentAnimationCurveSnapshotTests;
    // Same transient Montage + public Controller fixture lifecycle as DataControllerTests.
    UAnimMontage* Montage = NewObject<UAnimMontage>(GetTransientPackage(), NAME_None, RF_Transient);
    if (!TestNotNull(TEXT("transient montage exists"), Montage)) return false;
    IAnimationDataController& Controller = Montage->GetController();
    Controller.OpenBracket(FText::FromString(TEXT("snapshot fixture")), false);
    Controller.InitializeModel();
    Controller.SetFrameRate(FFrameRate(30, 1), false);
    Controller.SetNumberOfFrames(FFrameNumber(60), false);
    const FAnimationCurveIdentifier Zeta(FName(TEXT("Zeta")), ERawCurveTrackTypes::RCT_Float);
    const FAnimationCurveIdentifier Alpha(FName(TEXT("Alpha")), ERawCurveTrackTypes::RCT_Float);
    TestTrue(TEXT("first curve added"), Controller.AddCurve(Zeta, AACF_Editable, false));
    TestTrue(TEXT("second curve added"), Controller.AddCurve(Alpha, AACF_Editable, false));
    TestTrue(TEXT("public bulk weighted keys accepted"),
        Controller.SetCurveKeys(Zeta, {WeightedKey(0.25f, 1.5f), WeightedKey(1.f, -4.f)}, false));
    TestTrue(TEXT("public color accepted"), Controller.SetCurveColor(Zeta, FLinearColor(0.25f, 0.5f, 0.75f, 1.f), false));
    TestTrue(TEXT("public comment accepted"), Controller.SetCurveComment(Zeta, TEXT("snapshot"), false));
    Controller.NotifyPopulated();
    Controller.CloseBracket(false);
    const bool bDirtyBefore = Montage->GetOutermost()->IsDirty();
    auto Result = BuildAnimationFloatCurveEnumerationV2(*Montage, Invocation());
    if (!TestEqual(TEXT("v2 enumeration succeeds"), Result.Status,
        EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
    TestTrue(TEXT("v2 returns no child handles"), Result.HandleResults.IsEmpty());
    TestTrue(TEXT("v2 has no changed records"), Result.ChangedSet.IsEmpty());
    const auto* Rows = Field(Result.Value, TEXT("curves"));
    if (!TestNotNull(TEXT("v2 curves array"), Rows)
        || !TestEqual(TEXT("v2 retains both curves"), Rows->Elements.Num(), 2)) return false;
    const auto* Keys = Field(Rows->Elements[0], TEXT("keys"));
    if (!TestNotNull(TEXT("live keys"), Keys) || Keys->Elements.IsEmpty()) return false;
    AssertBits(*this, Keys->Elements[0], TEXT("arrive_tangent_weight"), 0x3f400000u);
    AssertBits(*this, Keys->Elements[0], TEXT("leave_tangent_weight"), 0x3fe00000u);
    Result = BuildAnimationFloatCurveEnumerationV2(*Montage, Invocation({TEXT("Alpha"), TEXT("Zeta")}));
    Rows = Field(Result.Value, TEXT("curves"));
    if (!TestNotNull(TEXT("filtered rows"), Rows) || Rows->Elements.Num() != 2) return false;
    TestEqual(TEXT("filter retains model order"), Field(Rows->Elements[0], TEXT("name"))->StringValue, FString(TEXT("Zeta")));
    Result = BuildAnimationFloatCurveEnumerationV2(*Montage, Invocation({TEXT("Absent")}));
    Rows = Field(Result.Value, TEXT("curves"));
    if (TestNotNull(TEXT("absent-name result"), Rows)) TestTrue(TEXT("unknown name yields no row"), Rows->Elements.IsEmpty());
    Result = BuildAnimationFloatCurveEnumerationV2(*Montage, Invocation({TEXT("Zeta"), TEXT("zeta")}));
    TestEqual(TEXT("duplicate FName requests rejected"), Result.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
    TestTrue(TEXT("invalid request has no partial rows"), Result.Value.Elements.IsEmpty());
    TestEqual(TEXT("observation did not dirty the package"), Montage->GetOutermost()->IsDirty(), bDirtyBefore);
    return true;
}

#endif
