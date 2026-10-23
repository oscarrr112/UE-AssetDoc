// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Runtime/AssetDocumentRuntimeTypes.h"

class UObject;

struct FAssetDocumentEditorPropertyInspectionResult
{
	bool bSucceeded = false;
	FAssetDocumentRuntimeValueDto Value;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

// Parent VisibleState's synchronous observation leaf. The caller owns exact
// Blueprint membership and operation binding; this function creates no CDO.
// Value retains both admitted and rejected rows from the existing inspection.
ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyInspectionResult
InspectAssetDocumentEditorExistingObjectProperties(UObject& ExactObject);
