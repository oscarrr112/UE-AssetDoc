// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "AssetDocumentFidelityGenericTestTypes.generated.h"

// A data asset with saved state the Details panel does not show: a custom asset editor or code sets it.
UCLASS()
class UAssetDocumentFidelityGenericTestAsset final : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Fidelity")
	float Gain = 1.0f;

	UPROPERTY()
	TArray<float> EditorKeys;
};
