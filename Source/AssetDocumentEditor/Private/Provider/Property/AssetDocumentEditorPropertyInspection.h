// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentEditorPropertyObservation.h"

class UClass;
class UObject;

ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyInspectionResult
InspectAssetDocumentEditorProperties(UClass& ExactClass, UObject* LiveAsset,
	TConstArrayView<FString> DeclaredNames = {});

// Saved properties of a live asset that no document form covers: not Details-editable (and not declared), saved
// (not transient, deprecated or skipped), and holding a value other than the class default. Value:
// {"subject", "omitted": [{"name", "declaring_class", "ue_cpp_type"}]}.
ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyInspectionResult
InspectAssetDocumentEditorOmittedSavedState(UClass& ExactClass, UObject& LiveAsset,
	TConstArrayView<FString> DeclaredNames = {});
