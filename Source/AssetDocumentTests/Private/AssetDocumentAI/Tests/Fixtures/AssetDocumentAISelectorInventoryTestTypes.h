// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "AssetDocumentAISelectorInventoryTestTypes.generated.h"

UCLASS(EditInlineNew, DefaultToInstanced)
class UAssetDocumentAISelectorInlineObject : public UObject
{
 GENERATED_BODY()
public:
 UPROPERTY(EditAnywhere, Category="Test") FBlackboardKeySelector Selector;
};

UCLASS()
class UAssetDocumentAISelectorFixedArrayTask : public UBTTaskNode
{
 GENERATED_BODY()
public:
 UPROPERTY(EditAnywhere, Category="Test") FBlackboardKeySelector Fixed[2];
 UPROPERTY(EditAnywhere, Instanced, Category="Test") TObjectPtr<UAssetDocumentAISelectorInlineObject> First;
 UPROPERTY(EditAnywhere, Instanced, Category="Test") TObjectPtr<UAssetDocumentAISelectorInlineObject> Second;
};
