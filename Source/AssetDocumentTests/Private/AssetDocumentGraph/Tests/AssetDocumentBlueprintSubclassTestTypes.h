// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Blueprint asset classes of the kind a plugin adds (UEAssetDoc.Tolerant.BlueprintSubclass): one that saves values of
// its own the Blueprint profile has no region for, and one that also keeps graphs of its own beside the K2 graphs.

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"

#include "AssetDocumentBlueprintSubclassTestTypes.generated.h"

UCLASS()
class UAssetDocumentTestStatefulBlueprint : public UBlueprint
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 SavedCount = 0;

	UPROPERTY()
	FString SavedLabel;

	// Not saved: not part of the asset's state.
	UPROPERTY(Transient)
	int32 Scratch = 0;
};

UCLASS()
class UAssetDocumentTestOwnGraphsBlueprint : public UBlueprint
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TArray<TObjectPtr<UEdGraph>> StateGraphs;
};
