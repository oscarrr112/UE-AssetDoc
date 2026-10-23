// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "TestDataAsset.generated.h"

/**
 * Test DataAsset class for validating UEAssetDoc functionality
 * Contains common property types for testing JSON-to-asset conversion
 */
UCLASS(BlueprintType)
class UTestDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Test string property */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	FString TestString;

	/** Test integer property */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	int32 TestInt = 0;

	/** Test float property */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	float TestFloat = 0.0f;

	/** Test boolean property */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	bool bTestBool = false;

	/** Test FName property */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	FName TestName;

	/** Test FText property */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	FText TestText;
};
