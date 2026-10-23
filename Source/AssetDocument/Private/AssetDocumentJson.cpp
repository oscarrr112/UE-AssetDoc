// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentJson.h"

#include "AssetDocumentAtomicFile.h"

#include "Containers/StringConv.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
bool NormalizeCanonicalNumberShape(TSharedPtr<FJsonValue>& Value);

bool NormalizeCanonicalNumber(
	TSharedPtr<FJsonValue>& Value)
{
	if (!Value->PreferStringRepresentation())
	{
		return true;
	}
	FString Lexeme;
	if (!Value->TryGetString(Lexeme))
	{
		return false;
	}
	const bool bFloatingPointLexeme =
		Lexeme.Contains(TEXT("."))
		|| Lexeme.Contains(TEXT("e"))
		|| Lexeme.Contains(TEXT("E"));
	if (!bFloatingPointLexeme)
	{
		return true;
	}
	double Number = 0.0;
	if (!Value->TryGetNumber(Number))
	{
		return false;
	}
	Value = MakeShared<FJsonValueNumber>(Number);
	return true;
}

bool NormalizeCanonicalNumberArray(
	TSharedPtr<FJsonValue>& Value)
{
	TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
	if (!Value->TryGetArray(Elements) || Elements == nullptr)
	{
		return false;
	}
	for (TSharedPtr<FJsonValue>& Element : *Elements)
	{
		if (!NormalizeCanonicalNumberShape(Element))
		{
			return false;
		}
	}
	return true;
}

bool NormalizeCanonicalNumberObject(FJsonObject& Object)
{
	for (auto& Field : Object.Values)
	{
		if (!NormalizeCanonicalNumberShape(Field.Value))
		{
			return false;
		}
	}
	return true;
}

bool NormalizeCanonicalNumberObjectValue(
	TSharedPtr<FJsonValue>& Value)
{
	TSharedPtr<FJsonObject>* Object = nullptr;
	return Value->TryGetObject(Object)
		&& Object != nullptr
		&& Object->IsValid()
		&& NormalizeCanonicalNumberObject(**Object);
}

bool NormalizeCanonicalNumberShape(TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return false;
	}
	switch (Value->Type)
	{
	case EJson::Number:
		return NormalizeCanonicalNumber(Value);
	case EJson::Array:
		return NormalizeCanonicalNumberArray(Value);
	case EJson::Object:
		return NormalizeCanonicalNumberObjectValue(Value);
	default:
		return true;
	}
}

bool NormalizeCanonicalNumberShapes(const TSharedPtr<FJsonObject>& Object)
{
	return Object.IsValid()
		&& NormalizeCanonicalNumberObject(*Object);
}
}

bool FAssetDocumentJson::LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *FilePath))
	{
		OutError = FString::Printf(TEXT("Failed to read JSON file '%s'"), *FilePath);
		return false;
	}

	if (!ParseJsonString(Contents, OutJson))
	{
		FString Path;
		FString First;
		FString Second;
		OutError = FindDuplicateKey(Contents, Path, First, Second)
			? FString::Printf(TEXT("Document.DuplicateKey: %s (at %s)"), *DuplicateKeyMessage(Path, First, Second), *Path)
			: FString::Printf(TEXT("Failed to parse JSON file '%s'"), *FilePath);
		return false;
	}
	return true;
}

bool FAssetDocumentJson::FindDuplicateKey(const FString& Text, FString& OutPath, FString& OutFirst, FString& OutSecond)
{
	struct FJsonScope
	{
		bool bObject = false;
		FString Path;
		TMap<FString, FString> Keys; // FString keys hash and compare ignoring case
		int32 Index = 0;
	};
	const auto Escape = [](const FString& Key) { return Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); };
	TArray<FJsonScope> Stack;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	EJsonNotation Notation = EJsonNotation::Null;
	while (Reader->ReadNext(Notation))
	{
		if (Notation == EJsonNotation::Error)
		{
			return false;
		}
		if (Notation == EJsonNotation::ObjectEnd || Notation == EJsonNotation::ArrayEnd)
		{
			if (!Stack.IsEmpty())
			{
				Stack.Pop();
			}
			continue;
		}
		FString ValuePath;
		if (!Stack.IsEmpty())
		{
			FJsonScope& Scope = Stack.Last();
			if (Scope.bObject)
			{
				const FString& Key = Reader->GetIdentifier();
				if (const FString* Existing = Scope.Keys.Find(Key))
				{
					OutPath = Scope.Path + TEXT("/") + Escape(Key);
					OutFirst = *Existing;
					OutSecond = Key;
					return true;
				}
				Scope.Keys.Add(Key, Key);
				ValuePath = Scope.Path + TEXT("/") + Escape(Key);
			}
			else
			{
				ValuePath = Scope.Path + TEXT("/") + FString::FromInt(Scope.Index++);
			}
		}
		if (Notation == EJsonNotation::ObjectStart || Notation == EJsonNotation::ArrayStart)
		{
			FJsonScope& Pushed = Stack.AddDefaulted_GetRef();
			Pushed.bObject = Notation == EJsonNotation::ObjectStart;
			Pushed.Path = ValuePath;
		}
	}
	return false;
}

FString FAssetDocumentJson::DuplicateKeyMessage(const FString& Path, const FString& First, const FString& Second)
{
	FString Object = Path;
	int32 Slash = INDEX_NONE;
	if (Object.FindLastChar(TEXT('/'), Slash))
	{
		Object.LeftInline(Slash);
	}
	return FString::Printf(TEXT("Keys '%s' and '%s' of %s %s: a key names one entry (Unreal names ignore letter case), so one of "
		"them would be lost. Rename or merge them."), *First, *Second, Object.IsEmpty() ? TEXT("the document") : *Object,
		First.Equals(Second, ESearchCase::CaseSensitive) ? TEXT("are the same key") : TEXT("differ only by letter case"));
}

bool FAssetDocumentJson::ParseJsonString(const FString& Text, TSharedPtr<FJsonObject>& OutJson)
{
	OutJson.Reset();
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(
		Reader,
		OutJson,
		FJsonSerializer::EFlags::StoreNumbersAsStrings)
		|| !OutJson.IsValid()
		|| !NormalizeCanonicalNumberShapes(OutJson))
	{
		OutJson.Reset();
		return false;
	}
	FString Path;
	FString First;
	FString Second;
	if (FindDuplicateKey(Text, Path, First, Second))
	{
		OutJson.Reset();
		return false;
	}
	return true;
}

bool FAssetDocumentJson::WriteJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Json, FString& OutError)
{
	if (!Json.IsValid())
	{
		OutError = TEXT("Cannot write an invalid JSON object");
		return false;
	}

	FString Contents;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Contents);
	if (!FJsonSerializer::Serialize(Json.ToSharedRef(), Writer))
	{
		OutError = FString::Printf(TEXT("Failed to serialize JSON file '%s'"), *FilePath);
		return false;
	}

	FTCHARToUTF8 Utf8Contents(*Contents);
	const TArrayView64<const uint8> Bytes(
		reinterpret_cast<const uint8*>(Utf8Contents.Get()),
		Utf8Contents.Length());
	FString AtomicError;
	if (!FAssetDocumentAtomicFile::WriteBytesAtomically(FilePath, Bytes, AtomicError))
	{
		OutError = FString::Printf(
			TEXT("Failed to atomically write JSON file '%s': %s"),
			*FilePath,
			*AtomicError);
		return false;
	}

	return true;
}
