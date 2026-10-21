// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "EditorAssetOpsTestTypes.generated.h"

// Test-only asset of the editor asset operation tests (UEAssetDoc.AssetOps.*):
// a data asset that holds hard references to other assets, so tests can build reference chains
// and cycles between assets of one delete batch.
UCLASS(HideDropdown)
class UAssetDocumentAssetOpsTestRefAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	TArray<TObjectPtr<UObject>> Refs;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	int32 Value = 0;
};
