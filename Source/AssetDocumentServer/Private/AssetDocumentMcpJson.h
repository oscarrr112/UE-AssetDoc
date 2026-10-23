// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

/**
 * JSON text as a JavaScript MCP server writes it, so that tool results served by the editor are byte-identical to the
 * ones a JavaScript client of the same HTTP routes would produce: compact JSON (JSON.stringify), numbers in their
 * shortest round-trip form, object keys in JavaScript property order (array-index keys first, ascending, then the
 * others in insertion order) and String() of values inside messages.
 */
namespace AssetDocumentMcpJson
{
	/** JSON.stringify(Value) without indentation; a null pointer is "null". */
	FString Stringify(const TSharedPtr<FJsonValue>& Value);

	/** JSON.stringify(Object) without indentation. */
	FString StringifyObject(const TSharedPtr<FJsonObject>& Object);

	/** Number.prototype.toString() of a double: shortest digits that read back as the same value. */
	FString FormatNumber(double Value);

	/** A JSON string literal with JSON.stringify's escapes. */
	FString Quote(const FString& Text);

	/** String(Value) of a parsed JSON value, as a template literal inserts it; a null pointer is "undefined". */
	FString ToJsString(const TSharedPtr<FJsonValue>& Value);

	/** The object's keys in JavaScript property order. */
	TArray<FString> OrderedKeys(const TSharedPtr<FJsonObject>& Object);

	/** encodeURIComponent(Text). */
	FString EncodeUriComponent(const FString& Text);

	/** Parse a JSON object; null on invalid JSON or a non-object root. */
	TSharedPtr<FJsonObject> ParseObject(const FString& Text);
}
