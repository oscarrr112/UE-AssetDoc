// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Engine/DataAsset.h"

#include "AssetDocumentGenericInspectContractTestTypes.generated.h"

UCLASS()
class UAssetDocumentGenericInspectContractTestDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	FText DisplayText;
};
