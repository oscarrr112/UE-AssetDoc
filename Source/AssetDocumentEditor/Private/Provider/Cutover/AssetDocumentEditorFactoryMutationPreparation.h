// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Runtime/AssetDocumentMutationPreparation.h"

// Exact admitted factory value/handle verification. No factory invocation,
// late preimage or seed ownership; Integration retains the existing factory seed.
FAssetDocumentContractValidationResult PrepareAssetDocumentEditorFactoryMutationContribution(
 const FAssetDocumentRuntimeRequestDto& Request,
 const FAssetDocumentMutationIntent& Intent,
 FAssetDocumentPreparedMutationContribution& OutContribution);
