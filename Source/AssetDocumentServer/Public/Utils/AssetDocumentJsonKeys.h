// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

#include <type_traits>

/**
 * FJsonObject field access by FString name that does not depend on the key type the engine stores in
 * FJsonObject::Values (FString, or a shared string with an FStringView interface). Iterate Values with
 * `const auto&` and read a key as FString with `FString(*Pair.Key)`; use these helpers for lookups by name and
 * for the list of names.
 */
namespace AssetDocumentServerJson
{
	/** The key type of FJsonObject::Values. */
	using FKey = std::decay_t<decltype((*DeclVal<const FJsonObject&>().Values.begin()).Key)>;

	/** The field names of Object, in map order (replaces OutKeys). */
	inline void GetKeys(const FJsonObject& Object, TArray<FString>& OutKeys)
	{
		OutKeys.Reset(Object.Values.Num());
		for (const auto& Pair : Object.Values)
		{
			OutKeys.Emplace(*Pair.Key);
		}
	}

	inline TArray<FString> GetKeys(const FJsonObject& Object)
	{
		TArray<FString> Keys;
		GetKeys(Object, Keys);
		return Keys;
	}

	/** The value slot of the field Name, or nullptr when Object has no such field. */
	inline const TSharedPtr<FJsonValue>* FindField(const FJsonObject& Object, FStringView Name)
	{
		return Object.Values.Find(FKey(Name));
	}

	inline TSharedPtr<FJsonValue>* FindField(FJsonObject& Object, FStringView Name)
	{
		return Object.Values.Find(FKey(Name));
	}

	/** The value of the field Name, or null when Object has no such field. */
	inline TSharedPtr<FJsonValue> FindFieldRef(const FJsonObject& Object, FStringView Name)
	{
		return Object.Values.FindRef(FKey(Name));
	}

	/** Sorts Object's fields by name, case-insensitively (the order of TLess<FString>). */
	inline void SortByKey(FJsonObject& Object)
	{
		Object.Values.KeySort([](const FKey& A, const FKey& B) { return FCString::Stricmp(*A, *B) < 0; });
	}
}
