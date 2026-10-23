// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentMcpJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace AssetDocumentMcpJsonLocal
{
// An array index key ("0", "17"; no sign, no leading zero, below 2^32 - 1) and its value.
bool IsArrayIndexKey(const FString& Key, uint64& OutIndex)
{
	if (Key.IsEmpty() || Key.Len() > 10 || (Key.Len() > 1 && Key[0] == TEXT('0')))
	{
		return false;
	}
	uint64 Value = 0;
	for (const TCHAR Char : Key)
	{
		if (Char < TEXT('0') || Char > TEXT('9'))
		{
			return false;
		}
		Value = Value * 10 + static_cast<uint64>(Char - TEXT('0'));
	}
	if (Value >= 4294967295ull)
	{
		return false;
	}
	OutIndex = Value;
	return true;
}

void AppendHex4(FString& Out, uint32 CodeUnit)
{
	static const TCHAR Digits[] = TEXT("0123456789abcdef");
	Out += TEXT("\\u");
	for (int32 Shift = 12; Shift >= 0; Shift -= 4)
	{
		Out.AppendChar(Digits[(CodeUnit >> Shift) & 0xF]);
	}
}

void AppendStringified(FString& Out, const TSharedPtr<FJsonValue>& Value);

void AppendObject(FString& Out, const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
	{
		Out += TEXT("null");
		return;
	}
	Out.AppendChar(TEXT('{'));
	bool bFirst = true;
	for (const FString& Key : AssetDocumentMcpJson::OrderedKeys(Object))
	{
		if (!bFirst)
		{
			Out.AppendChar(TEXT(','));
		}
		bFirst = false;
		Out += AssetDocumentMcpJson::Quote(Key);
		Out.AppendChar(TEXT(':'));
		AppendStringified(Out, Object->TryGetField(Key));
	}
	Out.AppendChar(TEXT('}'));
}

void AppendStringified(FString& Out, const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		Out += TEXT("null");
		return;
	}
	switch (Value->Type)
	{
	case EJson::Boolean:
		Out += Value->AsBool() ? TEXT("true") : TEXT("false");
		return;
	case EJson::Number:
	{
		const double Number = Value->AsNumber();
		Out += std::isfinite(Number) ? AssetDocumentMcpJson::FormatNumber(Number) : FString(TEXT("null"));
		return;
	}
	case EJson::String:
		Out += AssetDocumentMcpJson::Quote(Value->AsString());
		return;
	case EJson::Array:
	{
		Out.AppendChar(TEXT('['));
		bool bFirst = true;
		for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
		{
			if (!bFirst)
			{
				Out.AppendChar(TEXT(','));
			}
			bFirst = false;
			AppendStringified(Out, Element);
		}
		Out.AppendChar(TEXT(']'));
		return;
	}
	case EJson::Object:
		AppendObject(Out, Value->AsObject());
		return;
	default:
		Out += TEXT("null");
		return;
	}
}
} // namespace AssetDocumentMcpJsonLocal

namespace AssetDocumentMcpJson
{
FString Stringify(const TSharedPtr<FJsonValue>& Value)
{
	FString Out;
	AssetDocumentMcpJsonLocal::AppendStringified(Out, Value);
	return Out;
}

FString StringifyObject(const TSharedPtr<FJsonObject>& Object)
{
	FString Out;
	AssetDocumentMcpJsonLocal::AppendObject(Out, Object);
	return Out;
}

FString FormatNumber(double Value)
{
	if (std::isnan(Value))
	{
		return TEXT("NaN");
	}
	if (std::isinf(Value))
	{
		return Value > 0 ? TEXT("Infinity") : TEXT("-Infinity");
	}
	if (Value == 0.0)
	{
		return TEXT("0");
	}
	if (Value < 0.0)
	{
		return TEXT("-") + FormatNumber(-Value);
	}

	// The shortest decimal significand that reads back as the same double, and its exponent.
	char Buffer[64];
	ANSICHAR Digits[32] = {};
	int32 DigitCount = 0;
	int32 Exponent = 0;
	for (int32 Precision = 1; Precision <= 17; ++Precision)
	{
		std::snprintf(Buffer, sizeof(Buffer), "%.*e", Precision - 1, Value);
		if (std::strtod(Buffer, nullptr) != Value && Precision < 17)
		{
			continue;
		}
		DigitCount = 0;
		const char* Cursor = Buffer;
		for (; *Cursor != '\0' && *Cursor != 'e'; ++Cursor)
		{
			if (*Cursor >= '0' && *Cursor <= '9')
			{
				Digits[DigitCount++] = *Cursor;
			}
		}
		Exponent = *Cursor == 'e' ? std::atoi(Cursor + 1) : 0;
		break;
	}
	while (DigitCount > 1 && Digits[DigitCount - 1] == '0')
	{
		--DigitCount;
	}

	// Number::toString: n is the position of the decimal point relative to the digits.
	const int32 K = DigitCount;
	const int32 N = Exponent + 1;
	FString Out;
	auto AppendDigits = [&Out, &Digits](int32 From, int32 To)
	{
		for (int32 Index = From; Index < To; ++Index)
		{
			Out.AppendChar(static_cast<TCHAR>(Digits[Index]));
		}
	};
	if (K <= N && N <= 21)
	{
		AppendDigits(0, K);
		for (int32 Index = 0; Index < N - K; ++Index)
		{
			Out.AppendChar(TEXT('0'));
		}
	}
	else if (0 < N && N <= 21)
	{
		AppendDigits(0, N);
		Out.AppendChar(TEXT('.'));
		AppendDigits(N, K);
	}
	else if (-6 < N && N <= 0)
	{
		Out += TEXT("0.");
		for (int32 Index = 0; Index < -N; ++Index)
		{
			Out.AppendChar(TEXT('0'));
		}
		AppendDigits(0, K);
	}
	else
	{
		AppendDigits(0, 1);
		if (K > 1)
		{
			Out.AppendChar(TEXT('.'));
			AppendDigits(1, K);
		}
		const int32 E = N - 1;
		Out += FString::Printf(TEXT("e%c%d"), E < 0 ? TEXT('-') : TEXT('+'), FMath::Abs(E));
	}
	return Out;
}

FString Quote(const FString& Text)
{
	FString Out;
	Out.Reserve(Text.Len() + 2);
	Out.AppendChar(TEXT('"'));
	const int32 Length = Text.Len();
	for (int32 Index = 0; Index < Length; ++Index)
	{
		const uint32 Unit = static_cast<uint32>(Text[Index]) & 0xFFFF;
		switch (Unit)
		{
		case 0x08: Out += TEXT("\\b"); continue;
		case 0x09: Out += TEXT("\\t"); continue;
		case 0x0A: Out += TEXT("\\n"); continue;
		case 0x0C: Out += TEXT("\\f"); continue;
		case 0x0D: Out += TEXT("\\r"); continue;
		case 0x22: Out += TEXT("\\\""); continue;
		case 0x5C: Out += TEXT("\\\\"); continue;
		default: break;
		}
		if (Unit < 0x20)
		{
			AssetDocumentMcpJsonLocal::AppendHex4(Out, Unit);
			continue;
		}
		if (Unit >= 0xD800 && Unit <= 0xDBFF)
		{
			const uint32 Next = Index + 1 < Length ? static_cast<uint32>(Text[Index + 1]) & 0xFFFF : 0;
			if (Next >= 0xDC00 && Next <= 0xDFFF)
			{
				Out.AppendChar(Text[Index]);
				Out.AppendChar(Text[Index + 1]);
				++Index;
			}
			else
			{
				AssetDocumentMcpJsonLocal::AppendHex4(Out, Unit);
			}
			continue;
		}
		if (Unit >= 0xDC00 && Unit <= 0xDFFF)
		{
			AssetDocumentMcpJsonLocal::AppendHex4(Out, Unit);
			continue;
		}
		Out.AppendChar(Text[Index]);
	}
	Out.AppendChar(TEXT('"'));
	return Out;
}

FString ToJsString(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return TEXT("undefined");
	}
	switch (Value->Type)
	{
	case EJson::Boolean:
		return Value->AsBool() ? TEXT("true") : TEXT("false");
	case EJson::Number:
		return FormatNumber(Value->AsNumber());
	case EJson::String:
		return Value->AsString();
	case EJson::Array:
	{
		TArray<FString> Parts;
		for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
		{
			Parts.Add(!Element.IsValid() || Element->IsNull() ? FString() : ToJsString(Element));
		}
		return FString::Join(Parts, TEXT(","));
	}
	case EJson::Object:
		return TEXT("[object Object]");
	default:
		return TEXT("null");
	}
}

TArray<FString> OrderedKeys(const TSharedPtr<FJsonObject>& Object)
{
	TArray<TPair<uint64, FString>> IndexKeys;
	TArray<FString> OtherKeys;
	if (Object.IsValid())
	{
		for (const auto& Pair : Object->Values)
		{
			const FString Key(*Pair.Key);
			uint64 Index = 0;
			if (AssetDocumentMcpJsonLocal::IsArrayIndexKey(Key, Index))
			{
				IndexKeys.Emplace(Index, Key);
			}
			else
			{
				OtherKeys.Add(Key);
			}
		}
	}
	IndexKeys.Sort([](const TPair<uint64, FString>& A, const TPair<uint64, FString>& B) { return A.Key < B.Key; });
	TArray<FString> Keys;
	Keys.Reserve(IndexKeys.Num() + OtherKeys.Num());
	for (const TPair<uint64, FString>& Entry : IndexKeys)
	{
		Keys.Add(Entry.Value);
	}
	Keys.Append(OtherKeys);
	return Keys;
}

FString EncodeUriComponent(const FString& Text)
{
	static const ANSICHAR Hex[] = "0123456789ABCDEF";
	const FTCHARToUTF8 Utf8(*Text, Text.Len());
	FString Out;
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		const uint8 Byte = static_cast<uint8>(Utf8.Get()[Index]);
		const bool bUnreserved = (Byte >= 'A' && Byte <= 'Z') || (Byte >= 'a' && Byte <= 'z') || (Byte >= '0' && Byte <= '9')
			|| Byte == '-' || Byte == '_' || Byte == '.' || Byte == '!' || Byte == '~' || Byte == '*' || Byte == '\''
			|| Byte == '(' || Byte == ')';
		if (bUnreserved)
		{
			Out.AppendChar(static_cast<TCHAR>(Byte));
		}
		else
		{
			Out.AppendChar(TEXT('%'));
			Out.AppendChar(static_cast<TCHAR>(Hex[Byte >> 4]));
			Out.AppendChar(static_cast<TCHAR>(Hex[Byte & 0xF]));
		}
	}
	return Out;
}

TSharedPtr<FJsonObject> ParseObject(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return nullptr;
	}
	return Object;
}
} // namespace AssetDocumentMcpJson
