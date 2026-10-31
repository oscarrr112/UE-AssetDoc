// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Test types of the UEAssetDoc.Codec.BehaviorTree.TextLocalization test (AssetDocumentAI/Private/Tests): a project
// task with localizable texts, as a project's custom task may have. No engine Behavior Tree node has an FText property.

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"

#include "AssetDocumentAITextTestTypes.generated.h"

UCLASS()
class UAssetDocumentAITextTestTask : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Text")
	FText Label;

	UPROPERTY(EditAnywhere, Category = "Text")
	FText Hint;
};
