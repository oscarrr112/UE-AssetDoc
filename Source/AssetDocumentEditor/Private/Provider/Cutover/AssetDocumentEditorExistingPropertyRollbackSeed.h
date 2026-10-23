// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentEditorCutoverTransactionPreparation.h"

class FAssetDocumentEditorPropertyPreimage;

TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>>
MakeAssetDocumentEditorExistingPropertyRollbackSeeds(
	FAssetDocumentEditorPropertyPreimage&& PropertyPreimage);

TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>>
MakeAssetDocumentEditorCombinedRollbackSeeds(
	TSharedRef<IAssetDocumentEditorCutoverRollbackSeed> CreatedSeed);
