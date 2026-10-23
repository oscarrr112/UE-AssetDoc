// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Runtime/AssetDocumentRuntimeTypes.h"

class UBlueprint;

// Synchronous scalar default leaf. The Blueprint caller owns exact operation
// binding, preimage, Core registration, propagation and compilation. No handles,
// transaction or reflected pointers are retained. Both forward and inverse use
// this same leaf, with the currently existing generated-class CDO.
ASSETDOCUMENTEDITOR_API FAssetDocumentContractValidationResult
PreflightAssetDocumentEditorBlueprintVariableDefault(
 UBlueprint& Blueprint, FName VariableName, const FGuid& VariableGuid,
 const FAssetDocumentRuntimeValueDto& DesiredValue);

ASSETDOCUMENTEDITOR_API FAssetDocumentContractValidationResult
SetAssetDocumentEditorBlueprintVariableDefault(
 UBlueprint& Blueprint, FName VariableName, const FGuid& VariableGuid,
 const FAssetDocumentRuntimeValueDto& DesiredValue);
