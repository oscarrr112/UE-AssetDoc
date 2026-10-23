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
CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
	const IAssetDocumentVerifiedHandleLease& AssetLease,
	UObject& ExactPropertyOwner,
	TSharedRef<IPropertyRowGenerator> PropertyRows,
	TSharedRef<IPropertyHandle> SelectorPropertyHandle,
	TConstArrayView<FString> RollbackComponentIds,
	IAssetDocumentAtomicTransaction& Transaction);
