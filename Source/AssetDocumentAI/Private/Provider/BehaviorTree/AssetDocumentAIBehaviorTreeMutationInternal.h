// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

#include <initializer_list>

// Value helpers defined in AssetDocumentAIBehaviorTreeBlackboardMutation.cpp and shared by the Behavior Tree
// cache-refresh and selector batch-resolve primitives (one declaration, so unity builds see no redeclared defaults).
namespace AssetDocumentAIBehaviorTreeMutation
{
FAssetDocumentPrimitiveOutcome Failure(const TCHAR* Code, const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics = {});
FAssetDocumentContractValidationResult Valid();
FAssetDocumentRuntimeValueDto StringValue(FString Value);
FAssetDocumentRuntimeValueDto IntegerValue(int64 Value);
FAssetDocumentRuntimeValueDto ObjectValue(TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values);
const FAssetDocumentRuntimeValueDto* FindField(const FAssetDocumentRuntimeValueDto& Object, const TCHAR* Name);
bool HasExactFields(const FAssetDocumentRuntimeValueDto& Object, std::initializer_list<const TCHAR*> Names);
bool IsCanonicalObjectPath(const FAssetDocumentRuntimeValueDto* Value);
}
