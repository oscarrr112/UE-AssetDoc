// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TestActorBase.generated.h"

USTRUCT(BlueprintType)
struct FAssetDocumentTestEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Nested")
	FName Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Nested")
	float Value = 0.0f;
};

USTRUCT(BlueprintType)
struct FAssetDocumentTestConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Nested")
	TArray<FAssetDocumentTestEntry> Entries;
};

/**
 * Test Actor base class for validating Blueprint generation with default properties
 */
UCLASS(Blueprintable)
class ATestActorBase : public AActor
{
	GENERATED_BODY()

public:
	ATestActorBase();

	/** Test health property */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Stats")
	float MaxHealth = 100.0f;

	/** Test damage property */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Stats")
	float BaseDamage = 10.0f;

	/** Test speed property */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Stats")
	float MoveSpeed = 600.0f;

	/** Test name property */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Info")
	FString DisplayName;

	/** Test enabled flag */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Info")
	bool bIsEnabled = true;

	/** Test level property */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Info")
	int32 Level = 1;

	/** Nested test config for validating Blueprint CDO default persistence */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test|Nested")
	FAssetDocumentTestConfig Config;
};
