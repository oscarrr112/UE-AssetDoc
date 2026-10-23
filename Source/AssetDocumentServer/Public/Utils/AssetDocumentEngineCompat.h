// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"
#include "UObject/Class.h"
#include "UObject/UObjectHash.h"

/**
 * The "include nested objects" argument of GetObjectsWithOuter / GetObjectsWithPackage / ForEachObjectWithOuter /
 * ForEachObjectWithPackage, in the form the engine takes it (a flag set on newer engines, a bool before). Calls that
 * pass only the default (nested objects included) omit the argument.
 */
namespace AssetDocumentObjects
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	inline constexpr EGetObjectsFlags IncludeNested = EGetObjectsFlags::IncludeNestedObjects;
	inline constexpr EGetObjectsFlags DirectOnly = EGetObjectsFlags::None;
#else
	inline constexpr bool IncludeNested = true;
	inline constexpr bool DirectOnly = false;
#endif
}

namespace AssetDocumentEnums
{
	/** Enum.SetEnums (virtual, so a user-defined enum's override runs) with 64-bit values. */
	inline bool SetEnums(UEnum& Enum, TArray<TPair<FName, int64>>& Names, UEnum::ECppForm CppForm,
		EEnumFlags Flags = EEnumFlags::None, bool bAddMaxKeyIfMissing = true)
	{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		return Enum.SetEnums(Names, CppForm, UEnum::EUnderlyingType::int64, Flags,
			bAddMaxKeyIfMissing ? UEnum::EAddMaxKeyIfMissing::Yes : UEnum::EAddMaxKeyIfMissing::No);
#else
		return Enum.SetEnums(Names, CppForm, Flags, bAddMaxKeyIfMissing);
#endif
	}
}
