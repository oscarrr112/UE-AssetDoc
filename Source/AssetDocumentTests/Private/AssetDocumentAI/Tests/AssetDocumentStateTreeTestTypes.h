// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

// Test node types of the UEAssetDoc.StateTree.* tests: the engine ships no struct
// evaluator, so the tests author this one (instance data with a parameter and outputs to bind from).
// meta=(Hidden) keeps it out of the StateTree node picker (SStateTreeNodeTypePicker skips Hidden
// structs) and the profile refuses it like the picker; the tests lift Hidden only while they run
// (FScopedTestTypesVisible).

#include "CoreMinimal.h"
#include "StateTreeEvaluatorBase.h"

#include "AssetDocumentStateTreeTestTypes.generated.h"

USTRUCT()
struct FAssetDocumentStateTreeTestEvaluatorInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter")
	float Threshold = 0.f;

	UPROPERTY(EditAnywhere, Category = "Output")
	float Distance = 0.f;

	UPROPERTY(EditAnywhere, Category = "Output")
	int32 Count = 0;
};

USTRUCT(meta = (Hidden, DisplayName = "AssetDocument Test Evaluator", Category = "AssetDocument Tests"))
struct FAssetDocumentStateTreeTestEvaluator : public FStateTreeEvaluatorCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FAssetDocumentStateTreeTestEvaluatorInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	UPROPERTY(EditAnywhere, Category = "Parameter")
	bool bVerbose = false;
};
