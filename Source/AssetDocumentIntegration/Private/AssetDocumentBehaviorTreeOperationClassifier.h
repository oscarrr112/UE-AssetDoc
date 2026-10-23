// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

enum class EAssetDocumentBehaviorTreeOperationShape : uint8
{
	NotBehaviorTree,
	FullTreeLifecycle,
	RetainedTreeOverlay,
	InvalidBehaviorTreeShape,
};

enum class EAssetDocumentBehaviorTreeTopLevelPropertyMode : uint8
{
	NoTopLevelPropertyWrite,
	GenericTopLevelPropertyWrite,
};

struct FAssetDocumentBehaviorTreeOperationClassification
{
	EAssetDocumentBehaviorTreeOperationShape Shape =
		EAssetDocumentBehaviorTreeOperationShape::NotBehaviorTree;
	EAssetDocumentBehaviorTreeTopLevelPropertyMode PropertyMode =
		EAssetDocumentBehaviorTreeTopLevelPropertyMode::GenericTopLevelPropertyWrite;
};

FAssetDocumentBehaviorTreeOperationClassification
ClassifyAssetDocumentBehaviorTreeOperation(
	FStringView RequestedAssetClassPath,
	const FAssetDocumentRuntimeValueDto& CanonicalDocument);
