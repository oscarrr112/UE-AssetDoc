// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "BehaviorTree/BTTaskNode.h"
#include "StructUtils/InstancedStruct.h"
#include "Tests/Fixtures/AssetDocumentAIReadbackTestTypes.h"
#include "AssetDocumentAIScratchAuthoringTestTypes.generated.h"

UCLASS()
class UAssetDocumentAIScratchNestedTask : public UBTTaskNode
{
 GENERATED_BODY()
public:
 UPROPERTY(EditAnywhere, Instanced, Category="Test")
 TObjectPtr<UAssetDocumentAIInstallOwnedChild> Nested;
 UPROPERTY(EditAnywhere, Category="Test") FInstancedStruct Dynamic;
 UPROPERTY(EditAnywhere, Category="Test") TSet<int32> Numbers;
 UPROPERTY(EditAnywhere, Category="Test") TMap<FName, int32> Rows;
 UPROPERTY(EditAnywhere, Category="Test") TMap<FString, int32> StringRows;
 UPROPERTY(EditAnywhere, Category="Test") int32 FixedValues[3] = {1, 2, 3};
 UPROPERTY(EditAnywhere, Category="Test") TSet<FVector> Vectors;
};
