// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentMutationPreparation.h"

ASSETDOCUMENTEDITOR_API FAssetDocumentContractValidationResult
PrepareAssetDocumentEditorMutationBatch(
 const FAssetDocumentRuntimeRequestDto& Request,
 TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch);
