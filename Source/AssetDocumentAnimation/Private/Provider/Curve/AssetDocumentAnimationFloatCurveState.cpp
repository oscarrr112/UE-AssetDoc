// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Curve/AssetDocumentAnimationFloatCurveState.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveSnapshot.h"

namespace AssetDocumentAnimationFloatCurveState
{
using FObjectFields = TArray<const FAssetDocumentRuntimeValueDto*>;

bool ExactObject(const FAssetDocumentRuntimeValueDto& Value, TArray<FString> Fields,
    FObjectFields& OutFields)
{
    if (Value.Kind != EAssetDocumentRuntimeValueKind::Object
        || Value.ObjectKeys.Num() != Fields.Num() || Value.Elements.Num() != Fields.Num())
    {
        return false;
    }
    for (const FString& Field : Fields)
    {
        const FAssetDocumentRuntimeValueDto* Found = nullptr;
        for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
        {
            if (Value.ObjectKeys[Index].Equals(Field, ESearchCase::CaseSensitive))
            {
                if (Found != nullptr)
                {
                    return false;
                }
                Found = &Value.Elements[Index];
            }
        }
        if (Found == nullptr)
        {
            return false;
        }
        OutFields.Add(Found);
    }
    return true;
}

bool IntegerInRange(const FAssetDocumentRuntimeValueDto& Value, int64 Minimum, int64 Maximum)
{
    return Value.Kind == EAssetDocumentRuntimeValueKind::Integer
        && Value.IntegerValue >= Minimum && Value.IntegerValue <= Maximum;
}

uint32 Bits(const float& Value)
{
    uint32 Result;
    FMemory::Memcpy(&Result, &Value, sizeof(Result));
    return Result;
}

bool ValidKey(const FRichCurveKey& Value)
{
    return FMath::IsFinite(Value.Time)
        && static_cast<uint8>(Value.InterpMode) <= 3
        && static_cast<uint8>(Value.TangentMode) <= 4
        && static_cast<uint8>(Value.TangentWeightMode) <= 3;
}

bool FloatBits(const FAssetDocumentRuntimeValueDto& Value, float& OutValue)
{
    if (!IntegerInRange(Value, 0, MAX_uint32))
    {
        return false;
    }
    const uint32 Bits = static_cast<uint32>(Value.IntegerValue);
    static_assert(sizeof(Bits) == sizeof(OutValue));
    FMemory::Memcpy(&OutValue, &Bits, sizeof(Bits));
    return true;
}

bool Float(const FAssetDocumentRuntimeValueDto& Value, float& OutValue)
{
    FObjectFields Fields;
    return ExactObject(Value, {TEXT("bits")}, Fields) && FloatBits(*Fields[0], OutValue);
}

bool Default(const FAssetDocumentRuntimeValueDto& Value,
    FAssetDocumentAnimationFloatCurveState& OutState)
{
    FObjectFields Fields;
    if (!ExactObject(Value, {TEXT("has_value"), TEXT("float32_bits")}, Fields)
        || Fields[0]->Kind != EAssetDocumentRuntimeValueKind::Boolean)
    {
        return false;
    }
    OutState.bHasDefault = Fields[0]->BooleanValue;
    if (!OutState.bHasDefault)
    {
        OutState.DefaultValue = MAX_flt;
        return Fields[1]->Kind == EAssetDocumentRuntimeValueKind::Null;
    }
    return FloatBits(*Fields[1], OutState.DefaultValue)
        && Fields[1]->IntegerValue != 0x7f7fffff;
}

bool Color(const FAssetDocumentRuntimeValueDto& Value, FLinearColor& OutColor)
{
    FObjectFields Fields;
    return ExactObject(Value, {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")}, Fields)
        && Float(*Fields[0], OutColor.R)
        && Float(*Fields[1], OutColor.G)
        && Float(*Fields[2], OutColor.B)
        && Float(*Fields[3], OutColor.A);
}

bool Key(const FAssetDocumentRuntimeValueDto& Value, FRichCurveKey& OutKey)
{
    FObjectFields Fields;
    if (!ExactObject(Value,
        {TEXT("time"), TEXT("value"), TEXT("arrive_tangent"), TEXT("leave_tangent"),
            TEXT("interp_mode"), TEXT("tangent_mode"), TEXT("tangent_weight_mode"),
            TEXT("arrive_tangent_weight"), TEXT("leave_tangent_weight")}, Fields)
        || !Float(*Fields[0], OutKey.Time) || !FMath::IsFinite(OutKey.Time)
        || !Float(*Fields[1], OutKey.Value)
        || !Float(*Fields[2], OutKey.ArriveTangent)
        || !Float(*Fields[3], OutKey.LeaveTangent)
        || !IntegerInRange(*Fields[4], 0, 3)
        || !IntegerInRange(*Fields[5], 0, 4)
        || !IntegerInRange(*Fields[6], 0, 3)
        || !Float(*Fields[7], OutKey.ArriveTangentWeight)
        || !Float(*Fields[8], OutKey.LeaveTangentWeight))
    {
        return false;
    }
    OutKey.InterpMode = static_cast<ERichCurveInterpMode>(Fields[4]->IntegerValue);
    OutKey.TangentMode = static_cast<ERichCurveTangentMode>(Fields[5]->IntegerValue);
    OutKey.TangentWeightMode = static_cast<ERichCurveTangentWeightMode>(Fields[6]->IntegerValue);
    return ValidKey(OutKey);
}

bool Keys(const FAssetDocumentRuntimeValueDto& Value, TArray<FRichCurveKey>& OutKeys)
{
    if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
    {
        return false;
    }
    OutKeys.Reserve(Value.Elements.Num());
    for (const FAssetDocumentRuntimeValueDto& Element : Value.Elements)
    {
        FRichCurveKey Parsed;
        if (!Key(Element, Parsed) || (!OutKeys.IsEmpty() && Parsed.Time <= OutKeys.Last().Time))
        {
            return false;
        }
        OutKeys.Add(Parsed);
    }
    return true;
}

bool Name(const FString& Value, FName& OutName)
{
    if (Value.IsEmpty() || Value.Len() != FCString::Strlen(*Value)
        || Value.Len() >= FName::StringBufferSize)
    {
        return false;
    }
    if (Value.Len() >= NAME_SIZE)
    {
        // Let the public FName constructor interpret a short numeric suffix;
        // do not duplicate its private number parser or risk a long-name checkf.
        int32 Underscore = INDEX_NONE;
        if (!Value.FindLastChar(TEXT('_'), Underscore) || Underscore >= NAME_SIZE)
        {
            return false;
        }
        const FString Suffix = Value.Mid(Underscore);
        if (Suffix.Len() > 11)
        {
            return false;
        }
        const FName Probe(*(FString(TEXT("C0NameProbe")) + Suffix));
        if (Probe.GetNumber() == NAME_NO_NUMBER_INTERNAL
            || !Probe.GetPlainNameString().Equals(TEXT("C0NameProbe"), ESearchCase::CaseSensitive))
        {
            return false;
        }
    }
    OutName = FName(*Value);
    return !OutName.IsNone();
}

bool Row(const FAssetDocumentRuntimeValueDto& Value, FAssetDocumentAnimationFloatCurveState& OutState)
{
    FObjectFields Fields;
    if (!ExactObject(Value,
        {TEXT("name"), TEXT("flags"), TEXT("color"), TEXT("comment"),
            TEXT("default_value"), TEXT("pre_extrapolation"), TEXT("post_extrapolation"), TEXT("keys")}, Fields)
        || Fields[0]->Kind != EAssetDocumentRuntimeValueKind::String
        || !IntegerInRange(*Fields[1], MIN_int32, MAX_int32)
        || Fields[3]->Kind != EAssetDocumentRuntimeValueKind::String
        || !IntegerInRange(*Fields[5], 0, 5)
        || !IntegerInRange(*Fields[6], 0, 5))
    {
        return false;
    }
    if (!Name(Fields[0]->StringValue, OutState.Name) || !Color(*Fields[2], OutState.Color)
        || !Default(*Fields[4], OutState) || !Keys(*Fields[7], OutState.Keys))
    {
        return false;
    }
    OutState.Flags = static_cast<int32>(Fields[1]->IntegerValue);
    OutState.Comment = Fields[3]->StringValue;
    OutState.PreExtrapolation = static_cast<ERichCurveExtrapolation>(Fields[5]->IntegerValue);
    OutState.PostExtrapolation = static_cast<ERichCurveExtrapolation>(Fields[6]->IntegerValue);
    return true;
}
} // namespace AssetDocumentAnimationFloatCurveState

bool ParseAnimationFloatCurveStateV2(const FAssetDocumentRuntimeValueDto& Curves,
    TArray<FAssetDocumentAnimationFloatCurveState>& OutStates)
{
    using namespace AssetDocumentAnimationFloatCurveState;
    OutStates.Reset();
    if (Curves.Kind != EAssetDocumentRuntimeValueKind::Array)
    {
        return false;
    }
    TArray<FAssetDocumentAnimationFloatCurveState> Parsed;
    TSet<FName> Names;
    Parsed.Reserve(Curves.Elements.Num());
    for (const FAssetDocumentRuntimeValueDto& Value : Curves.Elements)
    {
        FAssetDocumentAnimationFloatCurveState State;
        if (!Row(Value, State) || Names.Contains(State.Name))
        {
            return false;
        }
        Names.Add(State.Name);
        Parsed.Add(MoveTemp(State));
    }
    OutStates = MoveTemp(Parsed);
    return true;
}

bool CaptureAnimationFloatCurveStateV2(TConstArrayView<FFloatCurve> Curves,
    TArray<FAssetDocumentAnimationFloatCurveState>& OutStates)
{
    OutStates.Reset();
    FAssetDocumentRuntimeValueDto Snapshot;
    return CaptureAnimationFloatCurveSnapshotV2(Curves, Snapshot)
        && ParseAnimationFloatCurveStateV2(Snapshot, OutStates);
}

bool IsValidAnimationFloatCurveStateV2(TConstArrayView<FAssetDocumentAnimationFloatCurveState> States)
{
    using namespace AssetDocumentAnimationFloatCurveState;
    TSet<FName> Names;
    for (const FAssetDocumentAnimationFloatCurveState& State : States)
    {
        if (State.Name.IsNone() || Names.Contains(State.Name)
            || static_cast<int64>(State.PreExtrapolation) < 0 || static_cast<int64>(State.PreExtrapolation) > 5
            || static_cast<int64>(State.PostExtrapolation) < 0 || static_cast<int64>(State.PostExtrapolation) > 5
            || State.bHasDefault != (Bits(State.DefaultValue) != 0x7f7fffffu))
        {
            return false;
        }
        Names.Add(State.Name);
        for (int32 Index = 0; Index < State.Keys.Num(); ++Index)
        {
            if (!ValidKey(State.Keys[Index])
                || (Index > 0 && State.Keys[Index].Time <= State.Keys[Index - 1].Time))
            {
                return false;
            }
        }
    }
    return true;
}

bool EqualAnimationFloatCurveStateV2(TConstArrayView<FAssetDocumentAnimationFloatCurveState> A,
    TConstArrayView<FAssetDocumentAnimationFloatCurveState> B)
{
    using namespace AssetDocumentAnimationFloatCurveState;
    if (A.Num() != B.Num())
    {
        return false;
    }
    for (int32 Index = 0; Index < A.Num(); ++Index)
    {
        const FAssetDocumentAnimationFloatCurveState& Left = A[Index];
        const FAssetDocumentAnimationFloatCurveState& Right = B[Index];
        if (!Left.Name.ToString().Equals(Right.Name.ToString(), ESearchCase::CaseSensitive)
            || Left.Flags != Right.Flags
            || !Left.Comment.Equals(Right.Comment, ESearchCase::CaseSensitive)
            || Bits(Left.Color.R) != Bits(Right.Color.R) || Bits(Left.Color.G) != Bits(Right.Color.G)
            || Bits(Left.Color.B) != Bits(Right.Color.B) || Bits(Left.Color.A) != Bits(Right.Color.A)
            || Left.bHasDefault != Right.bHasDefault || Bits(Left.DefaultValue) != Bits(Right.DefaultValue)
            || Left.PreExtrapolation != Right.PreExtrapolation || Left.PostExtrapolation != Right.PostExtrapolation
            || Left.Keys.Num() != Right.Keys.Num())
        {
            return false;
        }
        for (int32 KeyIndex = 0; KeyIndex < Left.Keys.Num(); ++KeyIndex)
        {
            const FRichCurveKey& L = Left.Keys[KeyIndex];
            const FRichCurveKey& R = Right.Keys[KeyIndex];
            if (Bits(L.Time) != Bits(R.Time) || Bits(L.Value) != Bits(R.Value)
                || Bits(L.ArriveTangent) != Bits(R.ArriveTangent) || Bits(L.LeaveTangent) != Bits(R.LeaveTangent)
                || L.InterpMode != R.InterpMode || L.TangentMode != R.TangentMode
                || L.TangentWeightMode != R.TangentWeightMode
                || Bits(L.ArriveTangentWeight) != Bits(R.ArriveTangentWeight)
                || Bits(L.LeaveTangentWeight) != Bits(R.LeaveTangentWeight))
            {
                return false;
            }
        }
    }
    return true;
}
