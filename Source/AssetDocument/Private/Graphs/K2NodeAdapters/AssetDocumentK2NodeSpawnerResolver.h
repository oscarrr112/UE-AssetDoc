// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Graphs/AssetDocumentNodeAdapter.h"
#include "UObject/StrongObjectPtr.h"

class UBlueprintNodeSpawner;
class UClass;
class UEdGraph;

struct FAssetDocumentK2ResolvedNodeSpawner
{
	TStrongObjectPtr<UBlueprintNodeSpawner> Spawner;
	UClass* ExpectedNodeClass = nullptr;
};

class FAssetDocumentK2NodeSpawnerResolver final
{
public:
	FAssetDocumentK2NodeSpawnerResolver() = delete;

	static FAssetDocumentCapabilityResult Resolve(
		const FAssetDocumentNodeApplyContext& Context,
		UEdGraph* Graph,
		const FAssetDocumentNodeSpec& NodeSpec,
		bool bRequireUEPlacementAdmission,
		FAssetDocumentK2ResolvedNodeSpawner& OutResolved);
};
