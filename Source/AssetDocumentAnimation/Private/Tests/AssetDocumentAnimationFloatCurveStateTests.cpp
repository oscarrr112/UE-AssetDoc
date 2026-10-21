// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "Provider/Curve/AssetDocumentAnimationFloatCurveState.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveSnapshot.h"
#include "Misc/AutomationTest.h"

namespace AssetDocumentAnimationFloatCurveStateTests
{
float FromBits(uint32 Bits)
{
    float Result;
    FMemory::Memcpy(&Result, &Bits, sizeof(Result));
    return Result;
}

uint32 ToBits(const float& Value)
{
    uint32 Bits;
    FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
    return Bits;
}

FAssetDocumentRuntimeValueDto& Field(FAssetDocumentRuntimeValueDto& Object, const TCHAR* Name)
{
    return Object.Elements[Object.ObjectKeys.IndexOfByKey(Name)];
}

TArray<FFloatCurve> NativeFixture()
{
    // Test-owned values only: no UObject/model mutation or authoring bypass.
    FFloatCurve Curve(FName(TEXT("Zeta")), MIN_int32 | AACF_Editable);
    Curve.Color = FLinearColor(FromBits(0x80000000u), FromBits(1u), 0.5f, 1.f);
    Curve.Comment = TEXT("Curve 注释");
    FRichCurveKey Key(-0.f, FromBits(0x7fc12345u));
    Key.InterpMode = RCIM_Cubic;
    Key.TangentMode = RCTM_Break;
    Key.TangentWeightMode = RCTWM_WeightedBoth;
    Key.ArriveTangent = FromBits(0xff800000u);
    Key.LeaveTangent = FromBits(1u);
    Key.ArriveTangentWeight = 0.75f;
    Key.LeaveTangentWeight = 1.75f;
    Curve.FloatCurve.SetKeys({Key});
    Curve.FloatCurve.SetDefaultValue(3.f);
    Curve.FloatCurve.PreInfinityExtrap = RCCE_CycleWithOffset;
    Curve.FloatCurve.PostInfinityExtrap = RCCE_Oscillate;
    FFloatCurve Empty(FName(TEXT("Alpha")), 0);
    return {Curve, Empty};
}

void ReorderObjects(FAssetDocumentRuntimeValueDto& Value)
{
    for (FAssetDocumentRuntimeValueDto& Element : Value.Elements)
    {
        ReorderObjects(Element);
    }
    if (Value.Kind == EAssetDocumentRuntimeValueKind::Object)
    {
        for (int32 Index = 0; Index < Value.ObjectKeys.Num() / 2; ++Index)
        {
            const int32 Other = Value.ObjectKeys.Num() - Index - 1;
            Value.ObjectKeys.Swap(Index, Other);
            Value.Elements.Swap(Index, Other);
        }
    }
}

void Reject(FAutomationTestBase& Test, const TCHAR* Reason,
    const FAssetDocumentRuntimeValueDto& Invalid)
{
    TArray<FAssetDocumentAnimationFloatCurveState> States;
    States.AddDefaulted();
    Test.TestFalse(Reason, ParseAnimationFloatCurveStateV2(Invalid, States));
    Test.TestTrue(TEXT("failure discards stale and partial output"), States.IsEmpty());
}
} // namespace AssetDocumentAnimationFloatCurveStateTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationFloatCurveStateRoundTripTest,
    "UEAssetDoc.AnimationProvider.FloatCurveState.RoundTripAndEquality",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationFloatCurveStateRoundTripTest::RunTest(const FString&)
{
    using namespace AssetDocumentAnimationFloatCurveStateTests;
    const TArray<FFloatCurve> Native = NativeFixture();
    FAssetDocumentRuntimeValueDto Wire;
    if (!TestTrue(TEXT("C1 produces wire"), CaptureAnimationFloatCurveSnapshotV2(Native, Wire))) return false;
    TArray<FAssetDocumentAnimationFloatCurveState> Parsed, Captured;
    if (!TestTrue(TEXT("wire parses"), ParseAnimationFloatCurveStateV2(Wire, Parsed))
        || !TestTrue(TEXT("native captures typed"), CaptureAnimationFloatCurveStateV2(Native, Captured))) return false;
    if (!TestEqual(TEXT("both native rows retained"), Parsed.Num(), 2)
        || !TestEqual(TEXT("native key retained"), Parsed[0].Keys.Num(), 1)) return false;
    TestTrue(TEXT("captured typed state is valid"), IsValidAnimationFloatCurveStateV2(Captured));
    TestTrue(TEXT("two capture routes agree including NaN bits"), EqualAnimationFloatCurveStateV2(Parsed, Captured));
    auto ReorderedWire = Wire;
    ReorderObjects(ReorderedWire);
    TestTrue(TEXT("row and nested object field order is not semantic"),
        ParseAnimationFloatCurveStateV2(ReorderedWire, Captured));
    TestTrue(TEXT("reordered objects preserve all typed fields and array order"),
        EqualAnimationFloatCurveStateV2(Parsed, Captured));
    TestEqual(TEXT("native order is retained"), Parsed[0].Name, FName(TEXT("Zeta")));
    TestEqual(TEXT("unknown signed flags retained"), Parsed[0].Flags, MIN_int32 | AACF_Editable);
    TestTrue(TEXT("case and Unicode comment retained"), Parsed[0].Comment.Equals(TEXT("Curve 注释"), ESearchCase::CaseSensitive));
    TestEqual(TEXT("color negative zero retained"), ToBits(Parsed[0].Color.R), 0x80000000u);
    TestEqual(TEXT("color subnormal retained"), ToBits(Parsed[0].Color.G), 1u);
    TestTrue(TEXT("explicit default present"), Parsed[0].bHasDefault);
    TestEqual(TEXT("default 3 bits"), ToBits(Parsed[0].DefaultValue), 0x40400000u);
    TestFalse(TEXT("empty curve default unset"), Parsed[1].bHasDefault);
    TestEqual(TEXT("unset is canonical sentinel"), ToBits(Parsed[1].DefaultValue), 0x7f7fffffu);
    TestEqual(TEXT("pre extrapolation retained"), Parsed[0].PreExtrapolation, RCCE_CycleWithOffset);
    TestEqual(TEXT("post extrapolation retained"), Parsed[0].PostExtrapolation, RCCE_Oscillate);
    TestEqual(TEXT("key signed zero"), ToBits(Parsed[0].Keys[0].Time), 0x80000000u);
    TestEqual(TEXT("key NaN payload"), ToBits(Parsed[0].Keys[0].Value), 0x7fc12345u);
    TestEqual(TEXT("key infinity tangent"), ToBits(Parsed[0].Keys[0].ArriveTangent), 0xff800000u);
    TestEqual(TEXT("key subnormal tangent"), ToBits(Parsed[0].Keys[0].LeaveTangent), 1u);
    TestEqual(TEXT("arrive weight"), ToBits(Parsed[0].Keys[0].ArriveTangentWeight), 0x3f400000u);
    TestEqual(TEXT("leave weight"), ToBits(Parsed[0].Keys[0].LeaveTangentWeight), 0x3fe00000u);
    TestEqual(TEXT("interpolation mode"), static_cast<int32>(Parsed[0].Keys[0].InterpMode), 2);
    TestEqual(TEXT("tangent mode"), static_cast<int32>(Parsed[0].Keys[0].TangentMode), 2);
    TestEqual(TEXT("weight mode"), static_cast<int32>(Parsed[0].Keys[0].TangentWeightMode), 3);

#if WITH_CASE_PRESERVING_NAME
    auto CaseOnly = Parsed;
    CaseOnly[0].Name = FName(TEXT("zeta"));
    TestTrue(TEXT("case-only names share native identity"), CaseOnly[0].Name == Parsed[0].Name);
    TestFalse(TEXT("editor case-only display name change is not a no-op"),
        EqualAnimationFloatCurveStateV2(Parsed, CaseOnly));
    CaseOnly[1].Name = Parsed[0].Name;
    TestFalse(TEXT("case-only duplicates still share one FName identity"),
        IsValidAnimationFloatCurveStateV2(CaseOnly));
#endif

    const TArray<TFunction<void(FAssetDocumentAnimationFloatCurveState&)>> Changes = {
        [](auto& S) { S.Name = TEXT("Changed"); },
        [](auto& S) { S.Flags ^= 0x40; },
        [](auto& S) { S.Comment = TEXT("curve 注释"); },
        [](auto& S) { S.Color.R = 0.f; },
        [](auto& S) { S.Color.G = 0.f; },
        [](auto& S) { S.Color.B = 0.f; },
        [](auto& S) { S.Color.A = 0.f; },
        [](auto& S) { S.bHasDefault = false; S.DefaultValue = MAX_flt; },
        [](auto& S) { S.DefaultValue = 2.f; },
        [](auto& S) { S.PreExtrapolation = RCCE_Linear; },
        [](auto& S) { S.PostExtrapolation = RCCE_Linear; },
        [](auto& S) { S.Keys[0].Time = 0.f; },
        [](auto& S) { S.Keys[0].Value = FromBits(0x7fc12346u); },
        [](auto& S) { S.Keys[0].ArriveTangent = 0.f; },
        [](auto& S) { S.Keys[0].LeaveTangent = 0.f; },
        [](auto& S) { S.Keys[0].ArriveTangentWeight = 0.f; },
        [](auto& S) { S.Keys[0].LeaveTangentWeight = 0.f; },
        [](auto& S) { S.Keys[0].InterpMode = RCIM_Linear; },
        [](auto& S) { S.Keys[0].TangentMode = RCTM_User; },
        [](auto& S) { S.Keys[0].TangentWeightMode = RCTWM_WeightedNone; },
        [](auto& S) { S.Keys.Reset(); },
    };
    for (int32 Index = 0; Index < Changes.Num(); ++Index)
    {
        auto Different = Parsed;
        Changes[Index](Different[0]);
        TestFalse(FString::Printf(TEXT("equality detects changed field %d"), Index),
            EqualAnimationFloatCurveStateV2(Parsed, Different));
    }
    auto Reordered = Parsed;
    Reordered.Swap(0, 1);
    TestFalse(TEXT("equality retains curve order"), EqualAnimationFloatCurveStateV2(Parsed, Reordered));
    Reordered.Pop();
    TestFalse(TEXT("equality retains curve count"), EqualAnimationFloatCurveStateV2(Parsed, Reordered));
    for (uint32 Bits : {0x80000000u, 1u, 0x7f800000u, 0x7fc12345u})
    {
        auto DefaultWire = Wire;
        Field(Field(DefaultWire.Elements[0], TEXT("default_value")), TEXT("float32_bits")).IntegerValue = Bits;
        TestTrue(TEXT("special default parses"), ParseAnimationFloatCurveStateV2(DefaultWire, Captured));
        if (!Captured.IsEmpty()) TestEqual(TEXT("special default bits preserved"), ToBits(Captured[0].DefaultValue), Bits);
    }
    auto LongNameWire = Wire;
    const FString LongName = FString::ChrN(NAME_SIZE - 1, TEXT('q')) + TEXT("_12345");
    Field(LongNameWire.Elements[0], TEXT("name")).StringValue = LongName;
    TestTrue(TEXT("new long name with native numeric suffix parses"),
        ParseAnimationFloatCurveStateV2(LongNameWire, Captured));
    if (!Captured.IsEmpty())
    {
        TestTrue(TEXT("long name remains exact"), Captured[0].Name.ToString().Equals(LongName, ESearchCase::CaseSensitive));
        TArray<FFloatCurve> LongNative{FFloatCurve(Captured[0].Name, 0)};
        TestTrue(TEXT("long native name capture is reversible"), CaptureAnimationFloatCurveStateV2(LongNative, Captured));
    }
    FAssetDocumentRuntimeValueDto Empty;
    Empty.Kind = EAssetDocumentRuntimeValueKind::Array;
    TestTrue(TEXT("empty managed set is valid"), ParseAnimationFloatCurveStateV2(Empty, Captured));
    TestTrue(TEXT("empty managed set clears old rows"), Captured.IsEmpty());
    TestTrue(TEXT("empty typed set is valid"), IsValidAnimationFloatCurveStateV2(Captured));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationFloatCurveStateInvalidTest,
    "UEAssetDoc.AnimationProvider.FloatCurveState.InvalidInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationFloatCurveStateInvalidTest::RunTest(const FString&)
{
    using namespace AssetDocumentAnimationFloatCurveStateTests;
    FAssetDocumentRuntimeValueDto Wire;
    if (!TestTrue(TEXT("C1 fixture wire"), CaptureAnimationFloatCurveSnapshotV2(NativeFixture(), Wire))) return false;
    // Prefix the malformed row with a valid differently named row to exercise
    // all-or-nothing output after one row has already been parsed.
    Wire.Elements.Swap(0, 1);
    const TArray<TFunction<void(FAssetDocumentRuntimeValueDto&)>> InvalidRows = {
        [](auto& R) { R.ObjectKeys.Pop(); R.Elements.Pop(); },
        [](auto& R) { R.ObjectKeys.Add(TEXT("extra")); R.Elements.AddDefaulted(); },
        [](auto& R) { R.ObjectKeys[1] = TEXT("name"); },
        [](auto& R) { R.ObjectKeys[0] = TEXT("NAME"); },
        [](auto& R) { R.Elements.Pop(); },
        [](auto& R) { Field(R, TEXT("name")).StringValue.Empty(); },
        [](auto& R) { Field(R, TEXT("name")).StringValue = TEXT("None"); },
        [](auto& R) { Field(R, TEXT("name")).StringValue = TEXT("alpha"); },
        [](auto& R) { Field(R, TEXT("name")).StringValue = FString::ChrN(NAME_SIZE, TEXT('q')); },
        [](auto& R) { Field(R, TEXT("name")).StringValue = FString::ChrN(NAME_SIZE - 1, TEXT('q')) + TEXT("_0012"); },
        [](auto& R) { Field(R, TEXT("name")).StringValue = FString::ChrN(NAME_SIZE, TEXT('q')) + TEXT("_12"); },
        [](auto& R) { Field(R, TEXT("flags")).IntegerValue = int64(MAX_int32) + 1; },
        [](auto& R) { Field(R, TEXT("flags")).IntegerValue = int64(MIN_int32) - 1; },
        [](auto& R) { Field(R, TEXT("flags")).Kind = EAssetDocumentRuntimeValueKind::Number; },
        [](auto& R) { Field(R, TEXT("comment")).Kind = EAssetDocumentRuntimeValueKind::Null; },
        [](auto& R) { Field(R, TEXT("color")).ObjectKeys[3] = TEXT("r"); },
        [](auto& R) { Field(R, TEXT("color")).Elements.Pop(); },
        [](auto& R) { Field(Field(R, TEXT("color")), TEXT("r")).Elements[0].IntegerValue = -1; },
        [](auto& R) { Field(Field(R, TEXT("color")), TEXT("r")).Elements[0].IntegerValue = int64(MAX_uint32) + 1; },
        [](auto& R) { Field(Field(R, TEXT("color")), TEXT("r")).Elements[0].Kind = EAssetDocumentRuntimeValueKind::Number; },
        [](auto& R) { Field(Field(R, TEXT("color")), TEXT("r")).ObjectKeys.Add(TEXT("extra")); },
        [](auto& R) { Field(Field(R, TEXT("default_value")), TEXT("has_value")).Kind = EAssetDocumentRuntimeValueKind::Integer; },
        [](auto& R) { Field(Field(R, TEXT("default_value")), TEXT("has_value")).BooleanValue = false; },
        [](auto& R) { Field(Field(R, TEXT("default_value")), TEXT("float32_bits")).Kind = EAssetDocumentRuntimeValueKind::Null; },
        [](auto& R) { Field(Field(R, TEXT("default_value")), TEXT("float32_bits")).IntegerValue = 0x7f7fffff; },
        [](auto& R) { Field(R, TEXT("pre_extrapolation")).IntegerValue = -1; },
        [](auto& R) { Field(R, TEXT("post_extrapolation")).IntegerValue = 6; },
        [](auto& R) { Field(R, TEXT("keys")).Kind = EAssetDocumentRuntimeValueKind::Object; },
        [](auto& R) { Field(R, TEXT("keys")).Elements[0].ObjectKeys.Pop(); },
        [](auto& R) { Field(Field(R, TEXT("keys")).Elements[0], TEXT("interp_mode")).IntegerValue = 4; },
        [](auto& R) { Field(Field(R, TEXT("keys")).Elements[0], TEXT("tangent_mode")).IntegerValue = 5; },
        [](auto& R) { Field(Field(R, TEXT("keys")).Elements[0], TEXT("tangent_weight_mode")).IntegerValue = -1; },
        [](auto& R) { Field(Field(Field(R, TEXT("keys")).Elements[0], TEXT("time")), TEXT("bits")).IntegerValue = 0x7f800000; },
        [](auto& R) { Field(Field(Field(R, TEXT("keys")).Elements[0], TEXT("time")), TEXT("bits")).IntegerValue = 0x7fc12345; },
        [](auto& R) { auto& K = Field(R, TEXT("keys")).Elements; auto Duplicate = K[0]; K.Add(MoveTemp(Duplicate)); },
        [](auto& R) { auto& K = Field(R, TEXT("keys")).Elements; auto Earlier = K[0]; Field(Field(Earlier, TEXT("time")), TEXT("bits")).IntegerValue = 0xbf800000; K.Add(MoveTemp(Earlier)); },
    };
    for (int32 Index = 0; Index < InvalidRows.Num(); ++Index)
    {
        auto Invalid = Wire;
        InvalidRows[Index](Invalid.Elements[1]);
        Reject(*this, *FString::Printf(TEXT("invalid row %d rejected"), Index), Invalid);
    }
    auto WrongRoot = Wire;
    WrongRoot.Kind = EAssetDocumentRuntimeValueKind::Object;
    Reject(*this, TEXT("array root required"), WrongRoot);
    TArray<FAssetDocumentAnimationFloatCurveState> States;
    if (!TestTrue(TEXT("typed fixture captured"), CaptureAnimationFloatCurveStateV2(NativeFixture(), States))) return false;
    auto BadTyped = States;
    BadTyped[0].DefaultValue = MAX_flt;
    TestFalse(TEXT("typed set sentinel rejected"), IsValidAnimationFloatCurveStateV2(BadTyped));
    BadTyped = States;
    BadTyped[1].DefaultValue = 0.f;
    TestFalse(TEXT("typed unset must be canonical"), IsValidAnimationFloatCurveStateV2(BadTyped));
    BadTyped = States;
    BadTyped[1].Name = BadTyped[0].Name;
    TestFalse(TEXT("typed duplicate names rejected"), IsValidAnimationFloatCurveStateV2(BadTyped));
    BadTyped = States;
    BadTyped[0].Keys[0].Time = FromBits(0x7fc12345u);
    TestFalse(TEXT("typed NaN time rejected"), IsValidAnimationFloatCurveStateV2(BadTyped));
    BadTyped = States;
    BadTyped[0].PostExtrapolation = static_cast<ERichCurveExtrapolation>(6);
    TestFalse(TEXT("typed invalid extrapolation rejected"), IsValidAnimationFloatCurveStateV2(BadTyped));
    TArray<FFloatCurve> BadNative = NativeFixture();
    BadNative[1].SetName(BadNative[0].GetName());
    TestFalse(TEXT("invalid native capture rejected"), CaptureAnimationFloatCurveStateV2(BadNative, States));
    TestTrue(TEXT("invalid capture clears previous typed state"), States.IsEmpty());
    return true;
}

#endif
