// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"

#include "PropertyHandle.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback
{
namespace
{
// Same pure RuntimeValue byte ordering as the existing canonical DTO codec.
// This operates only on already projected values, never property memory.
bool AppendUint64(TArray<uint8>& Bytes, const uint64 Value, const int64 Limit)
{
	if (Limit < 8 || Bytes.Num() > Limit - 8) return false;
	for (int32 Shift = 56; Shift >= 0; Shift -= 8)
	{
		Bytes.Add(static_cast<uint8>(Value >> Shift));
	}
	return true;
}

bool EncodeCanonical(const FAssetDocumentRuntimeValueDto& Value, TArray<uint8>& Bytes, const int64 Limit)
{
	if (Bytes.Num() >= Limit) return false;
	Bytes.Add(static_cast<uint8>(Value.Kind));
	switch (Value.Kind)
	{
	case EAssetDocumentRuntimeValueKind::Null: return true;
	case EAssetDocumentRuntimeValueKind::Boolean:
		if (Bytes.Num() >= Limit) return false;
		Bytes.Add(Value.BooleanValue ? 1 : 0); return true;
	case EAssetDocumentRuntimeValueKind::Integer:
		return AppendUint64(Bytes, static_cast<uint64>(Value.IntegerValue), Limit);
	case EAssetDocumentRuntimeValueKind::Number:
		return AppendUint64(Bytes, BitCast<uint64>(Value.NumberValue == 0.0 ? 0.0 : Value.NumberValue), Limit);
	case EAssetDocumentRuntimeValueKind::String:
	{
		const FTCHARToUTF8 Utf8(*Value.StringValue, Value.StringValue.Len());
		if (!AppendUint64(Bytes, static_cast<uint64>(Utf8.Length()), Limit)
			|| Utf8.Length() > Limit - Bytes.Num()) return false;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return true;
	}
	case EAssetDocumentRuntimeValueKind::Array:
		if (!AppendUint64(Bytes, static_cast<uint64>(Value.Elements.Num()), Limit)) return false;
		for (const FAssetDocumentRuntimeValueDto& Child : Value.Elements)
		{
			if (!EncodeCanonical(Child, Bytes, Limit)) return false;
		}
		return true;
	case EAssetDocumentRuntimeValueKind::Object:
		if (Value.ObjectKeys.Num() != Value.Elements.Num()) return false;
		if (!AppendUint64(Bytes, static_cast<uint64>(Value.Elements.Num()), Limit)) return false;
		for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
		{
			FAssetDocumentRuntimeValueDto Key = StringValue(Value.ObjectKeys[Index]);
			if (!EncodeCanonical(Key, Bytes, Limit)
				|| !EncodeCanonical(Value.Elements[Index], Bytes, Limit)) return false;
		}
		return true;
	default: return false;
	}
}

bool BytesLess(const TArray<uint8>& Left, const TArray<uint8>& Right)
{
	const int32 Common = FMath::Min(Left.Num(), Right.Num());
	const int32 Comparison = Common == 0 ? 0 : FMemory::Memcmp(Left.GetData(), Right.GetData(), Common);
	return Comparison == 0 ? Left.Num() < Right.Num() : Comparison < 0;
}


bool MapKey(IPropertyHandle& Handle, FPropertyReadContext& Context,
	const int32 Depth, FString& OutKey)
{
	const FProperty* Property = Handle.GetProperty();
	if (!CastField<FEnumProperty>(Property) && !CastField<FByteProperty>(Property)
		&& !CastField<FStrProperty>(Property) && !CastField<FNameProperty>(Property)) return false;
	FAssetDocumentRuntimeValueDto Value;
	if (!ReadAuthoredProperty(Handle, Context, Depth, Value)) return false;
	if (Value.Kind == EAssetDocumentRuntimeValueKind::String)
		OutKey = MoveTemp(Value.StringValue);
	else if (Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
		OutKey = LexToString(Value.IntegerValue);
	else return false;
	return true;
}
}

bool EncodeAuthoredCanonicalValue(const FAssetDocumentRuntimeValueDto& Value, TArray<uint8>& Bytes, const int64 Limit)
{
	return IsAssetDocumentAICanonicalRuntimeValue(Value) && Limit <= MAX_int32
		&& EncodeCanonical(Value, Bytes, Limit);
}

bool ReadAuthoredContainer(IPropertyHandle& Handle, FPropertyReadContext& Context,
	const int32 Depth, FAssetDocumentRuntimeValueDto& OutValue)
{
	const TSharedPtr<IPropertyHandleArray> Array = Handle.AsArray();
	const TSharedPtr<IPropertyHandleSet> Set = Handle.AsSet();
	const TSharedPtr<IPropertyHandleMap> Map = Handle.AsMap();
	uint32 Count = 0;
	FPropertyAccess::Result Result = FPropertyAccess::Fail;
	if (Array.IsValid()) Result = Array->GetNumElements(Count);
	else if (Set.IsValid()) Result = Set->GetNumElements(Count);
	else if (Map.IsValid()) Result = Map->GetNumElements(Count);
	if (Result != FPropertyAccess::Success) return false;
	if (Count > static_cast<uint32>(FMath::Max(0, Context.RemainingNodes)))
	{ Context.bOverflow = true; return false; }
	if (Map.IsValid())
	{
		// Reject an unsupported key type even for an empty map.
		const FMapProperty* Property = CastField<FMapProperty>(Handle.GetProperty());
		if (!Property || (!CastField<FEnumProperty>(Property->KeyProp)
			&& !CastField<FByteProperty>(Property->KeyProp)
			&& !CastField<FStrProperty>(Property->KeyProp)
			&& !CastField<FNameProperty>(Property->KeyProp))) return false;
	}
	TArray<FAssetDocumentRuntimeValueDto> Values;
	TMap<FString, FAssetDocumentRuntimeValueDto> MapValues;
	struct FSetElement
	{
		TArray<uint8> Encoding;
		FAssetDocumentRuntimeValueDto Value;
	};
	TArray<FSetElement> SetValues;
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		const TSharedRef<IPropertyHandle> Element = Array.IsValid() ? Array->GetElement(Index)
			: (Set.IsValid() ? Set->GetElement(Index) : Map->GetElement(Index));
		FAssetDocumentRuntimeValueDto Value;
		if (!ReadAuthoredProperty(*Element, Context, Depth + 1, Value)) return false;
		if (Map.IsValid())
		{
			const TSharedPtr<IPropertyHandle> KeyHandle = Element->GetKeyHandle();
			FString Key;
			if (!KeyHandle.IsValid() || !MapKey(*KeyHandle, Context, Depth + 1, Key)
				|| MapValues.Contains(Key)) return false;
			MapValues.Add(MoveTemp(Key), MoveTemp(Value));
		}
		else if (Set.IsValid())
		{
			FSetElement Entry;
			if (!EncodeAuthoredCanonicalValue(Value, Entry.Encoding, MAX_int32))
			{ Context.bOverflow = true; return false; }
			Entry.Value = MoveTemp(Value);
			SetValues.Add(MoveTemp(Entry));
		}
		else Values.Add(MoveTemp(Value));
	}
	if (Map.IsValid())
	{
		TArray<FString> Keys;
		MapValues.GetKeys(Keys);
		Keys.Sort();
		if (Context.Version == EAuthoredGraphReadbackVersion::V2 && MapValues.Contains(FString()))
		{
			Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
			const auto* Property = CastField<FMapProperty>(Handle.GetProperty());
			if (!Property || !CastField<FStrProperty>(Property->KeyProp)) return false;
			for (const FString& Key : Keys)
				Values.Add(ObjectValue({TEXT("key"), TEXT("value")},
					{StringValue(Key), MoveTemp(MapValues.FindChecked(Key))}));
			OutValue = ArrayValue(MoveTemp(Values));
			return true;
		}
		for (const FString& Key : Keys) Values.Add(MoveTemp(MapValues.FindChecked(Key)));
		OutValue = ObjectValue(MoveTemp(Keys), MoveTemp(Values));
		return true;
	}
	if (Set.IsValid())
	{
		SetValues.Sort([](const FSetElement& A, const FSetElement& B) { return BytesLess(A.Encoding, B.Encoding); });
		for (int32 Index = 0; Index < SetValues.Num(); ++Index)
		{
			if (Index > 0 && SetValues[Index - 1].Encoding == SetValues[Index].Encoding) return false;
			Values.Add(MoveTemp(SetValues[Index].Value));
		}
	}
	OutValue = ArrayValue(MoveTemp(Values));
	return true;
}
}
