// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "BehaviorTree/BTTaskNode.h"
#include "AssetDocumentAIBehaviorTreeLegacyEmptyKeyProbeTypes.generated.h"

UCLASS()
class UAssetDocumentAILegacyEmptyKeyProbeTask : public UBTTaskNode
{
 GENERATED_BODY()
public:
 UPROPERTY(EditAnywhere, Category="Probe") TMap<FString, int32> Map;
};
