// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

enum class EAssetDocumentUMGDecisionDisposition : uint8
{
	Ordinary = 0,
	StableBlocker = 1,
};

struct FAssetDocumentUMGDecisionRow
{
	FString DecisionId;
	uint32 Version = 1;
	EAssetDocumentPrimitiveOwner Owner = EAssetDocumentPrimitiveOwner::UMG;
	EAssetDocumentUMGDecisionDisposition Disposition =
		EAssetDocumentUMGDecisionDisposition::Ordinary;
	TArray<FString> ConsumerIds;
	FString StableBlockerDiagnostic;
};

ASSETDOCUMENTUMG_API TArray<FAssetDocumentUMGDecisionRow>
BuildAssetDocumentUMGDecisionProjection();

ASSETDOCUMENTUMG_API TArray<FAssetDocumentPrimitiveDescriptor>
BuildAssetDocumentUMGPrimitiveCatalog();

TConstArrayView<FAssetDocumentUMGDecisionRow>
GetAssetDocumentUMGDecisionProjection();

ASSETDOCUMENTUMG_API TConstArrayView<FAssetDocumentPrimitiveDescriptor>
GetAssetDocumentUMGPrimitiveDescriptors();

const FAssetDocumentUMGDecisionRow*
FindAssetDocumentUMGDecision(FStringView DecisionId, uint32 Version);

const FAssetDocumentUMGDecisionRow*
FindAssetDocumentUMGStableBlocker(FStringView DecisionId, uint32 Version);

bool IsAssetDocumentUMGDecisionProjectionValid();
