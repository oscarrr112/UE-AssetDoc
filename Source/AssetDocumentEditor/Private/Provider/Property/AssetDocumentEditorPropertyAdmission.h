// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Provider/Property/AssetDocumentEditorPropertyEdit.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

class UObject;
class UClass;
#include "Provider/Property/AssetDocumentEditorPropertyBatchTokenPayload.h"

struct FAssetDocumentEditorPropertyAdmissionResult
{
	bool bSucceeded = false;
	FAssetDocumentRuntimeValueDto Value;
	TArray<FAssetDocumentOpaqueHandle> Handles;
	TOptional<FAssetDocumentOpaqueHandle> PropertyBatchHandle;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

// Profile-declared admission: a Profile may name serialized non-edit properties
// (per exact class) that it authors. They travel as the optional value argument
// "admit_properties" (array of unique non-empty names) of the property primitives.
// Returns false for a malformed argument; an absent argument yields no names.
bool TryReadAssetDocumentEditorDeclaredPropertyNames(
	const FAssetDocumentRuntimeValueDto& Arguments,
	TArray<FString>& OutNames);

// Splits value arguments into the declared names and the remaining object.
bool TrySplitAssetDocumentEditorDeclaredPropertyArguments(
	const FAssetDocumentRuntimeValueDto& Arguments,
	FAssetDocumentRuntimeValueDto& OutRemaining,
	TArray<FString>& OutNames);

FAssetDocumentEditorPropertyAdmissionResult
EnumerateAssetDocumentEditorAdmittedProperties(
	FStringView ExactAssetClassPath,
	IAssetDocumentHandleResolver& Handles,
	TConstArrayView<FString> DeclaredNames = {});

ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyAdmissionResult
AdmitAssetDocumentEditorPropertyBatch(
	UObject& ExactTarget,
	IAssetDocumentHandleResolver& Handles,
	TConstArrayView<FString> DeclaredNames = {});

ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyPreimageCaptureResult
CaptureAssetDocumentEditorWritablePropertyPreimage(
	UObject& ExactTarget);

// The same admission projection as live batch tokens, without issuing handles.
ASSETDOCUMENTEDITOR_API TArray<FAssetDocumentEditorPropertyBatchTokenFieldView>
GetAssetDocumentEditorAdmittedPropertyFields(UClass& ExactClass);

// Edit-admitted fields plus the declared ones; false (with diagnostics) when a
// declared name is not admissible on ExactClass.
ASSETDOCUMENTEDITOR_API bool TryGetAssetDocumentEditorAdmittedPropertyFields(
	UClass& ExactClass,
	TConstArrayView<FString> DeclaredNames,
	TArray<FAssetDocumentEditorPropertyBatchTokenFieldView>& OutFields,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
