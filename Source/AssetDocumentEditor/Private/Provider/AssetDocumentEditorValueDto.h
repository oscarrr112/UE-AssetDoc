// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

class FJsonValue;

struct FAssetDocumentEditorValueField
{
	FString Name;
	FAssetDocumentRuntimeValueDto Value;
};

struct FAssetDocumentEditorValueFieldSpec
{
	FString Name;
	EAssetDocumentRuntimeValueKind Kind = EAssetDocumentRuntimeValueKind::Null;
};

ASSETDOCUMENTEDITOR_API bool TryBuildAssetDocumentEditorValueObject(
	TConstArrayView<FAssetDocumentEditorValueField> Fields,
	FAssetDocumentRuntimeValueDto& OutValue,
	FString& OutDiagnosticCode);

ASSETDOCUMENTEDITOR_API bool TryReadAssetDocumentEditorExactObject(
	const FAssetDocumentRuntimeValueDto& Value,
	TConstArrayView<FAssetDocumentEditorValueFieldSpec> Fields,
	TArray<const FAssetDocumentRuntimeValueDto*>& OutValues,
	FString& OutDiagnosticCode);

ASSETDOCUMENTEDITOR_API bool TryConvertAssetDocumentEditorValueToJson(
	const FAssetDocumentRuntimeValueDto& Value,
	TSharedPtr<FJsonValue>& OutJsonValue,
	FString& OutDiagnosticCode);

ASSETDOCUMENTEDITOR_API bool TryConvertAssetDocumentEditorJsonToValue(
	const TSharedPtr<FJsonValue>& JsonValue,
	FAssetDocumentRuntimeValueDto& OutValue,
	FString& OutDiagnosticCode);
