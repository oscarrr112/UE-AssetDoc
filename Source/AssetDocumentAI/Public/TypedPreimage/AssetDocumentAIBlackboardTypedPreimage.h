// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentEditorNativeState.h"
#include "CoreMinimal.h"

class IAssetDocumentAtomicTransaction;
class IAssetDocumentVerifiedHandleLease;
class IPropertyHandle;
class IPropertyRowGenerator;
class UObject;

ASSETDOCUMENTAI_API FAssetDocumentEditorTypedPreimageResult
CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
	const IAssetDocumentVerifiedHandleLease& AssetLease,
	UObject& ExactKeyType,
	TSharedRef<IPropertyRowGenerator> PropertyRows,
	TConstArrayView<TSharedRef<IPropertyHandle>> PropertyHandles,
	TConstArrayView<FString> RollbackComponentIds,
	IAssetDocumentAtomicTransaction& Transaction);

ASSETDOCUMENTAI_API FAssetDocumentEditorTypedPreimageResult
CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
	const IAssetDocumentVerifiedHandleLease& AssetLease,
	UObject& ExactBlackboardData,
	TConstArrayView<FString> RollbackComponentIds,
	IAssetDocumentAtomicTransaction& Transaction);
