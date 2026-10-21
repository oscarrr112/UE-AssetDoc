// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

// Value helpers of the level tools: request JSON <-> AssetDocument document values,
// the read-back comparison of edit_level's verify step, JSON pointers.
namespace UE::AssetDocumentLevel::Private
{
using FLevelValue = FAssetDocumentRuntimeValueDto;

// A request JSON value as a document value. Integral numbers are integers, as the
// AssetDocument runtime reads documents.
ASSETDOCUMENTLEVEL_API FLevelValue JsonToLevelValue(const TSharedPtr<FJsonValue>& Json);

// A document value as JSON (shortest round-trip decimals).
ASSETDOCUMENTLEVEL_API TSharedPtr<FJsonValue> LevelValueToJson(const FLevelValue& Value);

// Compact JSON text of a value (digests, messages).
ASSETDOCUMENTLEVEL_API FString LevelValueToText(const FLevelValue& Value);

// The read-back comparison: numbers compare by value (an integer 5 equals 5.0), object
// members by name in any order, everything else exactly. On a difference
// OutFirstDifference is the JSON pointer of the first differing member inside the
// value ("" when the values differ at the top).
bool LevelValuesEqual(const FLevelValue& Expected, const FLevelValue& Actual, FString* OutFirstDifference = nullptr);

// Base with Authored written over it: object members Authored lists replace (merged
// recursively when both are objects, as struct edits are); any other value replaces.
// The planned "after" of a dry run.
FLevelValue MergeAuthoredValue(const FLevelValue& Base, const FLevelValue& Authored);

// One JSON pointer segment ("~" -> "~0", "/" -> "~1").
FString EscapePointerSegment(const FString& Segment);
}
