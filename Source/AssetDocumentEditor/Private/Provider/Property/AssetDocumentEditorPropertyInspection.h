// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentEditorPropertyObservation.h"

class UClass;
class UObject;

ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyInspectionResult
InspectAssetDocumentEditorProperties(UClass& ExactClass, UObject* LiveAsset,
	TConstArrayView<FString> DeclaredNames = {});
