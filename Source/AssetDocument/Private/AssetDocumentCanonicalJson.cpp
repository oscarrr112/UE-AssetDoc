// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentCanonicalJson.h"

#include "Dom/JsonObject.h"
#include "Misc/SecureHash.h"

#include <charconv>
#include <system_error>
#include "Utils/AssetDocumentJsonKeys.h"

namespace
{
bool IsHighSurrogate(TCHAR Character)
{
	return Character >= static_cast<TCHAR>(0xD800) && Character <= static_cast<TCHAR>(0xDBFF);
}

bool IsLowSurrogate(TCHAR Character)
{
	return Character >= static_cast<TCHAR>(0xDC00) && Character <= static_cast<TCHAR>(0xDFFF);
}

void AddDefaultExtractOnlyFields(TSet<FString>& OutFields)
{
	OutFields.Add(TEXT("_Skipped"));
	OutFields.Add(TEXT("_meta"));
}

TSet<FString> MakeExtractOnlyFieldSet(const FAssetDocumentRegionPolicy* Policy)
{
	TSet<FString> Fields;
	AddDefaultExtractOnlyFields(Fields);

	if (Policy)
	{
		for (const FString& Field : Policy->ExtractOnlyFields)
		{
			Fields.Add(Field);
		}
	}

	return Fields;
}

void AppendUnicodeEscape(TCHAR Character, FString& Out)
{
	Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Character) & 0xffff);
}

void AppendQuotedJsonString(const FString& String, FString& Out)
{
	Out += TEXT("\"");
	for (int32 Index = 0; Index < String.Len(); ++Index)
	{
		const TCHAR Character = String[Index];
		if (IsHighSurrogate(Character))
		{
			AppendUnicodeEscape(Character, Out);
			if (Index + 1 < String.Len() && IsLowSurrogate(String[Index + 1]))
			{
				++Index;
				AppendUnicodeEscape(String[Index], Out);
			}
			continue;
		}
		if (IsLowSurrogate(Character))
		{
			AppendUnicodeEscape(Character, Out);
			continue;
		}

		switch (Character)
		{
		case TEXT('"'):
			Out += TEXT("\\\"");
			break;
		case TEXT('\\'):
			Out += TEXT("\\\\");
			break;
		case TEXT('\b'):
			Out += TEXT("\\b");
			break;
		case TEXT('\f'):
			Out += TEXT("\\f");
			break;
		case TEXT('\n'):
			Out += TEXT("\\n");
			break;
		case TEXT('\r'):
			Out += TEXT("\\r");
			break;
		case TEXT('\t'):
			Out += TEXT("\\t");
			break;
		default:
			if (Character < 0x20)
			{
				AppendUnicodeEscape(Character, Out);
			}
			else
			{
				Out.AppendChar(Character);
			}
			break;
		}
	}
	Out += TEXT("\"");
}

FString NormalizeCanonicalExponent(FString NumberText)
{
	int32 ExponentIndex = INDEX_NONE;
	if (!NumberText.FindChar(TEXT('e'), ExponentIndex))
	{
		NumberText.FindChar(TEXT('E'), ExponentIndex);
	}

	if (ExponentIndex == INDEX_NONE)
	{
		return NumberText;
	}

	FString Mantissa = NumberText.Left(ExponentIndex);
	FString Exponent = NumberText.Mid(ExponentIndex + 1);
	FString Sign;

	if (Exponent.StartsWith(TEXT("+")))
	{
		Exponent.RightChopInline(1, EAllowShrinking::No);
	}
	else if (Exponent.StartsWith(TEXT("-")))
	{
		Sign = TEXT("-");
		Exponent.RightChopInline(1, EAllowShrinking::No);
	}

	while (Exponent.Len() > 1 && Exponent[0] == TEXT('0'))
	{
		Exponent.RightChopInline(1, EAllowShrinking::No);
	}

	return FString::Printf(TEXT("%se%s%s"), *Mantissa, *Sign, *Exponent);
}

FString WriteCanonicalNumber(double Number)
{
	if (!FMath::IsFinite(Number))
	{
		return TEXT("null");
	}

	if (Number == 0.0)
	{
		return TEXT("0");
	}

	char Buffer[128];
	const std::to_chars_result Result = std::to_chars(Buffer, Buffer + UE_ARRAY_COUNT(Buffer), Number, std::chars_format::general);
	if (Result.ec != std::errc())
	{
		return TEXT("null");
	}

	const int32 Length = static_cast<int32>(Result.ptr - Buffer);
	const auto Converted = StringCast<TCHAR>(Buffer, Length);
	return NormalizeCanonicalExponent(FString(Length, Converted.Get()));
}

void AppendCanonicalJsonValue(const TSharedPtr<FJsonValue>& Value, FString& Out)
{
	if (!Value.IsValid() || Value->Type == EJson::Null || Value->Type == EJson::None)
	{
		Out += TEXT("null");
		return;
	}

	switch (Value->Type)
	{
	case EJson::String:
		AppendQuotedJsonString(Value->AsString(), Out);
		break;
	case EJson::Number:
		Out += WriteCanonicalNumber(Value->AsNumber());
		break;
	case EJson::Boolean:
		Out += Value->AsBool() ? TEXT("true") : TEXT("false");
		break;
	case EJson::Array:
		{
			Out += TEXT("[");
			const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
			for (int32 Index = 0; Index < Array.Num(); ++Index)
			{
				if (Index > 0)
				{
					Out += TEXT(",");
				}
				AppendCanonicalJsonValue(Array[Index], Out);
			}
			Out += TEXT("]");
			break;
		}
	case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				Out += TEXT("null");
				break;
			}

			TArray<FString> Keys;
			AssetDocumentServerJson::GetKeys(*Object, Keys);
			Keys.Sort();

			Out += TEXT("{");
			for (int32 Index = 0; Index < Keys.Num(); ++Index)
			{
				if (Index > 0)
				{
					Out += TEXT(",");
				}
				AppendQuotedJsonString(Keys[Index], Out);
				Out += TEXT(":");
				const TSharedPtr<FJsonValue>* FieldValue = AssetDocumentServerJson::FindField(*Object, Keys[Index]);
				AppendCanonicalJsonValue(FieldValue ? *FieldValue : MakeShared<FJsonValueNull>(), Out);
			}
			Out += TEXT("}");
			break;
		}
	default:
		Out += TEXT("null");
		break;
	}
}

TSharedPtr<FJsonValue> CloneJsonValueWithoutFields(const TSharedPtr<FJsonValue>& Value, const TSet<FString>& ExtractOnlyFields)
{
	if (!Value.IsValid() || Value->Type == EJson::Null || Value->Type == EJson::None)
	{
		return MakeShared<FJsonValueNull>();
	}

	switch (Value->Type)
	{
	case EJson::String:
		return MakeShared<FJsonValueString>(Value->AsString());
	case EJson::Number:
		return MakeShared<FJsonValueNumber>(Value->AsNumber());
	case EJson::Boolean:
		return MakeShared<FJsonValueBoolean>(Value->AsBool());
	case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> ClonedArray;
			for (const TSharedPtr<FJsonValue>& Entry : Value->AsArray())
			{
				ClonedArray.Add(CloneJsonValueWithoutFields(Entry, ExtractOnlyFields));
			}
			return MakeShared<FJsonValueArray>(MoveTemp(ClonedArray));
		}
	case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				return MakeShared<FJsonValueNull>();
			}

			TSharedRef<FJsonObject> ClonedObject = MakeShared<FJsonObject>();
			for (const auto& Pair : Object->Values)
			{
				if (!ExtractOnlyFields.Contains(FString(*Pair.Key)))
				{
					ClonedObject->SetField(Pair.Key, CloneJsonValueWithoutFields(Pair.Value, ExtractOnlyFields));
				}
			}
			return MakeShared<FJsonValueObject>(ClonedObject);
		}
	default:
		return MakeShared<FJsonValueNull>();
	}
}
}

FString FAssetDocumentCanonicalJson::WriteCanonicalJson(const TSharedPtr<FJsonValue>& Value, const FAssetDocumentRegionPolicy* Policy)
{
	FString JsonText;
	AppendCanonicalJsonValue(CloneWithoutExtractOnlyFields(Value, Policy), JsonText);
	return JsonText;
}

FString FAssetDocumentCanonicalJson::HashJsonValue(const TSharedPtr<FJsonValue>& Value, const FAssetDocumentRegionPolicy* Policy)
{
	const FString CanonicalJson = WriteCanonicalJson(Value, Policy);
	const FTCHARToUTF8 Utf8CanonicalJson(*CanonicalJson);

	const FSHAHash Hash = FSHA1::HashBuffer(Utf8CanonicalJson.Get(), static_cast<uint64>(Utf8CanonicalJson.Length()));
	return FString::Printf(TEXT("sha1:%s"), *Hash.ToString().ToLower());
}

TSharedPtr<FJsonValue> FAssetDocumentCanonicalJson::CloneWithoutExtractOnlyFields(
	const TSharedPtr<FJsonValue>& Value,
	const FAssetDocumentRegionPolicy* Policy)
{
	return CloneJsonValueWithoutFields(Value, MakeExtractOnlyFieldSet(Policy));
}
