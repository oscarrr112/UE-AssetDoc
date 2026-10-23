// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class UClass;

class FAssetDocumentClassResolver
{
public:
	static bool ResolveClassReference(const FString& ClassName, UClass*& OutClass, FString& OutError);
	static bool ResolveClass(const FString& ClassName, UClass*& OutClass, FString& OutError);
	// Same as ResolveClass; OutCode receives a stable diagnostic code on failure
	// (for example FactoryCreate.AbstractClass) so callers can surface it to the AI.
	static bool ResolveClass(const FString& ClassName, UClass*& OutClass, FString& OutError, FString& OutCode);

private:
	static bool ValidateResolvedClass(UClass* Class, FString& OutError, FString& OutCode);
};
