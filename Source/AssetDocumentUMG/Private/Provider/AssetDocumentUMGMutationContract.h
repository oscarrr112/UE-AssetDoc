// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

class UObject;
class UWidgetBlueprint;

TArray<FAssetDocumentMutationFootprint>
BuildWidgetBlueprintMutationFootprint(
	UWidgetBlueprint& Blueprint,
	TConstArrayView<UObject*> TouchedObjects);

FAssetDocumentContractValidationResult
EnlistWidgetBlueprintMutationFootprint(
	IAssetDocumentAtomicTransaction& Transaction,
	UWidgetBlueprint& Blueprint,
	TConstArrayView<UObject*> TouchedObjects);

bool ValidateWidgetBlueprintMutationCompletion(
	bool bPostcondition,
	const TCHAR* FailureMessage,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

bool RollbackAssetDocumentUMGMutation(
	IAssetDocumentAtomicTransaction& Transaction,
	FStringView StableFailureCode,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

FAssetDocumentChangedRecord MakeAssetDocumentUMGChangedRecord(
	UObject& Object,
	const TCHAR* Path,
	const TCHAR* ChangeKind);
