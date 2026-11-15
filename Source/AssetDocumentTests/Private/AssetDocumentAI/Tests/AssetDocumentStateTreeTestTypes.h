// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Test node types of the UEAssetDoc.StateTree.* tests: the engine ships no struct
// evaluator, so the tests author this one (instance data with a parameter and outputs to bind from).
// meta=(Hidden) keeps it out of the StateTree node picker (SStateTreeNodeTypePicker skips Hidden
// structs) and the profile refuses it like the picker; the tests lift Hidden only while they run
// (FScopedTestTypesVisible).

#include "CoreMinimal.h"
#include "StateTreeDelegate.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreePropertyRef.h"
#include "StateTreeTaskBase.h"
#include "UObject/Object.h"

#include "AssetDocumentStateTreeTestTypes.generated.h"

// An edit-inline settings object: an instanced object value, which the document codec has no form for.
UCLASS(EditInlineNew)
class UAssetDocumentStateTreeTestConfig : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Config")
	float Range = 0.f;
};

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

	UPROPERTY(EditAnywhere, Instanced, Category = "Parameter")
	TObjectPtr<UAssetDocumentStateTreeTestConfig> Config = nullptr;
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

// A task whose instance data holds property references: a native one typed by its RefType metadata and a
// Blueprint one typed by its RefType value.
USTRUCT()
struct FAssetDocumentStateTreeTestRefTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Parameter", meta = (RefType = "int32"))
	FStateTreePropertyRef CountRef;

	UPROPERTY(EditAnywhere, Category = "Parameter")
	FStateTreeBlueprintPropertyRef Tally;
};

USTRUCT(meta = (Hidden, DisplayName = "AssetDocument Test Ref Task", Category = "AssetDocument Tests"))
struct FAssetDocumentStateTreeTestRefTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FAssetDocumentStateTreeTestRefTaskInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};

// An evaluator with a delegate dispatcher an OnDelegate transition can listen to.
USTRUCT()
struct FAssetDocumentStateTreeTestDispatcherInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Output")
	FStateTreeDelegateDispatcher OnDone;
};

USTRUCT(meta = (Hidden, DisplayName = "AssetDocument Test Dispatcher", Category = "AssetDocument Tests"))
struct FAssetDocumentStateTreeTestDispatcherEvaluator : public FStateTreeEvaluatorCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FAssetDocumentStateTreeTestDispatcherInstanceData;

	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }
};
