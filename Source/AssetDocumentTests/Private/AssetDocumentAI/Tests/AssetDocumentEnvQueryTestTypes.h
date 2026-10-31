// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Test types of the UEAssetDoc.Tolerant.EnvQuery test (AssetDocumentAI/Private/Tests): a project EQS test with a
// Details property whose type has no document form (a field path), beside an ordinary one.

#include "CoreMinimal.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_VectorBase.h"
#include "UObject/FieldPath.h"

#include "AssetDocumentEnvQueryTestTypes.generated.h"

UCLASS()
class UAssetDocumentEnvQueryFieldTest : public UEnvQueryTest
{
	GENERATED_BODY()

public:
	UAssetDocumentEnvQueryFieldTest(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
		: Super(ObjectInitializer)
	{
		ValidItemType = UEnvQueryItemType_VectorBase::StaticClass();
	}

	// Scores nothing: the test only stands in for a project test's Details.
	virtual void RunTest(FEnvQueryInstance& QueryInstance) const override {}

	UPROPERTY(EditDefaultsOnly, Category = "Test")
	TFieldPath<FProperty> Field;

	UPROPERTY(EditDefaultsOnly, Category = "Test")
	float Bias = 0.f;
};
