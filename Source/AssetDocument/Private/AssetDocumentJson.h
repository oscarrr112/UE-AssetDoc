// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FAssetDocumentJson
{
public:
	static bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
	// Parses JSON text keeping the authored number kind, as LoadJsonFile does: an
	// integer literal (1) stays an integer and a fraction or exponent literal (1.0)
	// a floating-point number. Used for inline documents (HTTP request bodies) so
	// they reach the profiles exactly like the same document read from a sidecar.
	static bool ParseJsonString(const FString& Text, TSharedPtr<FJsonObject>& OutJson);
	// JSON text whose object has two keys that are equal ignoring letter case (or equal): a JSON object read into
	// FJsonObject keeps one of them, and Unreal names ignore case, so the document would silently lose an entry
	// (Material expressions "DropS" / "Drops"). Parsing such text fails; OutPath is the JSON pointer of the second
	// key, OutFirst / OutSecond the two keys as written. False for text without duplicates (or not valid JSON).
	static bool FindDuplicateKey(const FString& Text, FString& OutPath, FString& OutFirst, FString& OutSecond);
	// "Keys 'A' and 'a' of <object> ..." for the Document.DuplicateKey diagnostic.
	static FString DuplicateKeyMessage(const FString& Path, const FString& First, const FString& Second);
	static bool WriteJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Json, FString& OutError);
};
