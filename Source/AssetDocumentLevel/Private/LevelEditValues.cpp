// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "LevelEditValues.h"

#include "AssetDocumentTypes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::AssetDocumentLevel::Private
{
namespace
{
using EKind = EAssetDocumentRuntimeValueKind;

bool IsNumeric(const FLevelValue& Value)
{
	return Value.Kind == EKind::Integer || Value.Kind == EKind::Number;
}

double AsDouble(const FLevelValue& Value)
{
	return Value.Kind == EKind::Integer ? static_cast<double>(Value.IntegerValue) : Value.NumberValue;
}

int32 FindKey(const FLevelValue& Object, const FString& Key)
{
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Key)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool EqualAt(const FLevelValue& Expected, const FLevelValue& Actual, const FString& Path, FString* OutDifference)
{
	auto Differ = [&Path, OutDifference]()
	{
		if (OutDifference != nullptr)
		{
			*OutDifference = Path;
		}
		return false;
	};
	if (IsNumeric(Expected) && IsNumeric(Actual))
	{
		return AsDouble(Expected) == AsDouble(Actual) ? true : Differ();
	}
	if (Expected.Kind != Actual.Kind)
	{
		return Differ();
	}
	switch (Expected.Kind)
	{
	case EKind::Null:
		return true;
	case EKind::Boolean:
		return Expected.BooleanValue == Actual.BooleanValue ? true : Differ();
	case EKind::String:
		return Expected.StringValue.Equals(Actual.StringValue, ESearchCase::CaseSensitive) ? true : Differ();
	case EKind::Array:
		if (Expected.Elements.Num() != Actual.Elements.Num())
		{
			return Differ();
		}
		for (int32 Index = 0; Index < Expected.Elements.Num(); ++Index)
		{
			if (!EqualAt(Expected.Elements[Index], Actual.Elements[Index], Path + TEXT("/") + FString::FromInt(Index), OutDifference))
			{
				return false;
			}
		}
		return true;
	case EKind::Object:
		if (Expected.ObjectKeys.Num() != Actual.ObjectKeys.Num())
		{
			// Report the first member the other side lacks.
			for (int32 Index = 0; Index < Expected.ObjectKeys.Num(); ++Index)
			{
				if (FindKey(Actual, Expected.ObjectKeys[Index]) == INDEX_NONE)
				{
					if (OutDifference != nullptr)
					{
						*OutDifference = Path + TEXT("/") + EscapePointerSegment(Expected.ObjectKeys[Index]);
					}
					return false;
				}
			}
			return Differ();
		}
		for (int32 Index = 0; Index < Expected.ObjectKeys.Num() && Index < Expected.Elements.Num(); ++Index)
		{
			const FString MemberPath = Path + TEXT("/") + EscapePointerSegment(Expected.ObjectKeys[Index]);
			const int32 Other = FindKey(Actual, Expected.ObjectKeys[Index]);
			if (Other == INDEX_NONE || !Actual.Elements.IsValidIndex(Other))
			{
				if (OutDifference != nullptr)
				{
					*OutDifference = MemberPath;
				}
				return false;
			}
			if (!EqualAt(Expected.Elements[Index], Actual.Elements[Other], MemberPath, OutDifference))
			{
				return false;
			}
		}
		return true;
	default:
		return Expected.BytesValue == Actual.BytesValue ? true : Differ();
	}
}
}

FLevelValue JsonToLevelValue(const TSharedPtr<FJsonValue>& Json)
{
	FLevelValue Value;
	switch (Json.IsValid() ? Json->Type : EJson::Null)
	{
	case EJson::Boolean:
		Value.Kind = EKind::Boolean;
		Value.BooleanValue = Json->AsBool();
		break;
	case EJson::Number:
	{
		const double Number = Json->AsNumber();
		if (FMath::IsFinite(Number) && FMath::Abs(Number) < 9.0e15 && FMath::RoundToDouble(Number) == Number)
		{
			Value.Kind = EKind::Integer;
			Value.IntegerValue = static_cast<int64>(Number);
		}
		else
		{
			Value.Kind = EKind::Number;
			Value.NumberValue = Number;
		}
		break;
	}
	case EJson::String:
		Value.Kind = EKind::String;
		Value.StringValue = Json->AsString();
		break;
	case EJson::Array:
		Value.Kind = EKind::Array;
		for (const TSharedPtr<FJsonValue>& Element : Json->AsArray())
		{
			Value.Elements.Add(JsonToLevelValue(Element));
		}
		break;
	case EJson::Object:
		Value.Kind = EKind::Object;
		for (const auto& Member : Json->AsObject()->Values)
		{
			Value.ObjectKeys.Add(FString(*Member.Key));
			Value.Elements.Add(JsonToLevelValue(Member.Value));
		}
		break;
	default:
		break;
	}
	return Value;
}

TSharedPtr<FJsonValue> LevelValueToJson(const FLevelValue& Value)
{
	TSharedPtr<FJsonValue> Json = ProjectAssetDocumentRuntimeValue(Value);
	return Json.IsValid() ? Json : MakeShared<FJsonValueNull>();
}

FString LevelValueToText(const FLevelValue& Value)
{
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(LevelValueToJson(Value), FString(), Writer);
	Writer->Close();
	return Text;
}

bool LevelValuesEqual(const FLevelValue& Expected, const FLevelValue& Actual, FString* OutFirstDifference)
{
	return EqualAt(Expected, Actual, FString(), OutFirstDifference);
}

FLevelValue MergeAuthoredValue(const FLevelValue& Base, const FLevelValue& Authored)
{
	if (Base.Kind != EKind::Object || Authored.Kind != EKind::Object)
	{
		return Authored;
	}
	FLevelValue Merged = Base;
	for (int32 Index = 0; Index < Authored.ObjectKeys.Num() && Index < Authored.Elements.Num(); ++Index)
	{
		const int32 Existing = FindKey(Merged, Authored.ObjectKeys[Index]);
		if (Existing == INDEX_NONE || !Merged.Elements.IsValidIndex(Existing))
		{
			Merged.ObjectKeys.Add(Authored.ObjectKeys[Index]);
			Merged.Elements.Add(Authored.Elements[Index]);
		}
		else
		{
			Merged.Elements[Existing] = MergeAuthoredValue(Merged.Elements[Existing], Authored.Elements[Index]);
		}
	}
	return Merged;
}

FString EscapePointerSegment(const FString& Segment)
{
	return Segment.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
}
}
