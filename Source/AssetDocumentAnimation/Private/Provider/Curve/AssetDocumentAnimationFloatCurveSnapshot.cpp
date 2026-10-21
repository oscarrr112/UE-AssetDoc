// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Curve/AssetDocumentAnimationFloatCurveSnapshot.h"

#include "Animation/AnimCurveTypes.h"

namespace AssetDocumentAnimationFloatCurveSnapshot
{
static_assert(RCIM_Linear == 0 && RCIM_Constant == 1 && RCIM_Cubic == 2 && RCIM_None == 3);
static_assert(RCTM_Auto == 0 && RCTM_User == 1 && RCTM_Break == 2
    && RCTM_None == 3 && RCTM_SmartAuto == 4);
static_assert(RCTWM_WeightedNone == 0 && RCTWM_WeightedArrive == 1
    && RCTWM_WeightedLeave == 2 && RCTWM_WeightedBoth == 3);
static_assert(RCCE_Cycle == 0 && RCCE_CycleWithOffset == 1 && RCCE_Oscillate == 2
    && RCCE_Linear == 3 && RCCE_Constant == 4 && RCCE_None == 5);

FAssetDocumentRuntimeValueDto Integer(int64 Value)
{
    FAssetDocumentRuntimeValueDto Result;
    Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
    Result.IntegerValue = Value;
    return Result;
}

FAssetDocumentRuntimeValueDto String(const FString& Value)
{
    FAssetDocumentRuntimeValueDto Result;
    Result.Kind = EAssetDocumentRuntimeValueKind::String;
    Result.StringValue = Value;
    return Result;
}

FAssetDocumentRuntimeValueDto Object(TArray<FString> Names,
    TArray<FAssetDocumentRuntimeValueDto> Values)
{
    FAssetDocumentRuntimeValueDto Result;
    Result.Kind = EAssetDocumentRuntimeValueKind::Object;
    Result.ObjectKeys = MoveTemp(Names);
    Result.Elements = MoveTemp(Values);
    return Result;
}

uint32 FloatBits(const float& Value)
{
    uint32 Bits;
    static_assert(sizeof(Bits) == sizeof(Value));
    FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
    return Bits;
}

FAssetDocumentRuntimeValueDto Float(const float& Value)
{
    return Object({TEXT("bits")}, {Integer(FloatBits(Value))});
}

FAssetDocumentRuntimeValueDto Default(float Value)
{
    const uint32 Bits = FloatBits(Value);
    FAssetDocumentRuntimeValueDto HasValue;
    HasValue.Kind = EAssetDocumentRuntimeValueKind::Boolean;
    HasValue.BooleanValue = Bits != 0x7f7fffffu;
    return Object({TEXT("has_value"), TEXT("float32_bits")},
        {HasValue, HasValue.BooleanValue ? Integer(Bits) : FAssetDocumentRuntimeValueDto{}});
}

bool IsValid(const FFloatCurve& Curve)
{
    if (Curve.GetName().IsNone()
        || static_cast<uint8>(Curve.FloatCurve.PreInfinityExtrap) > 5
        || static_cast<uint8>(Curve.FloatCurve.PostInfinityExtrap) > 5)
    {
        return false;
    }
    bool bFirst = true;
    float PreviousTime = 0.f;
    for (const FRichCurveKey& Key : Curve.FloatCurve.GetConstRefOfKeys())
    {
        if (!FMath::IsFinite(Key.Time) || (!bFirst && Key.Time <= PreviousTime)
            || static_cast<uint8>(Key.InterpMode) > 3
            || static_cast<uint8>(Key.TangentMode) > 4
            || static_cast<uint8>(Key.TangentWeightMode) > 3)
        {
            return false;
        }
        PreviousTime = Key.Time;
        bFirst = false;
    }
    return true;
}

FAssetDocumentRuntimeValueDto KeyValue(const FRichCurveKey& Key)
{
    return Object(
        {TEXT("time"), TEXT("value"), TEXT("arrive_tangent"),
            TEXT("leave_tangent"), TEXT("interp_mode"), TEXT("tangent_mode"),
            TEXT("tangent_weight_mode"), TEXT("arrive_tangent_weight"),
            TEXT("leave_tangent_weight")},
        {Float(Key.Time), Float(Key.Value), Float(Key.ArriveTangent), Float(Key.LeaveTangent),
            Integer(Key.InterpMode), Integer(Key.TangentMode), Integer(Key.TangentWeightMode),
            Float(Key.ArriveTangentWeight), Float(Key.LeaveTangentWeight)});
}

FAssetDocumentRuntimeValueDto CurveValue(const FFloatCurve& Curve)
{
    FAssetDocumentRuntimeValueDto Keys;
    Keys.Kind = EAssetDocumentRuntimeValueKind::Array;
    for (const FRichCurveKey& Key : Curve.FloatCurve.GetConstRefOfKeys())
    {
        Keys.Elements.Add(KeyValue(Key));
    }
    const FLinearColor Color = Curve.GetColor();
    return Object(
        {TEXT("name"), TEXT("flags"), TEXT("color"), TEXT("comment"),
            TEXT("default_value"), TEXT("pre_extrapolation"), TEXT("post_extrapolation"), TEXT("keys")},
        {String(Curve.GetName().ToString()), Integer(Curve.GetCurveTypeFlags()),
            Object({TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")},
                {Float(Color.R), Float(Color.G), Float(Color.B), Float(Color.A)}),
            String(Curve.Comment), Default(Curve.FloatCurve.GetDefaultValue()),
            Integer(Curve.FloatCurve.PreInfinityExtrap), Integer(Curve.FloatCurve.PostInfinityExtrap),
            MoveTemp(Keys)});
}
} // namespace AssetDocumentAnimationFloatCurveSnapshot

bool CaptureAnimationFloatCurveSnapshotV2(
    TConstArrayView<FFloatCurve> Curves,
    FAssetDocumentRuntimeValueDto& OutCurves)
{
    using namespace AssetDocumentAnimationFloatCurveSnapshot;
    OutCurves = {};
    TSet<FName> Names;
    for (const FFloatCurve& Curve : Curves)
    {
        if (!IsValid(Curve) || Names.Contains(Curve.GetName()))
        {
            return false;
        }
        Names.Add(Curve.GetName());
    }
    FAssetDocumentRuntimeValueDto Snapshot;
    Snapshot.Kind = EAssetDocumentRuntimeValueKind::Array;
    Snapshot.Elements.Reserve(Curves.Num());
    for (const FFloatCurve& Curve : Curves)
    {
        Snapshot.Elements.Add(CurveValue(Curve));
    }
    OutCurves = MoveTemp(Snapshot);
    return true;
}
