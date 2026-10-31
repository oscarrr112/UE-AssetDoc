// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Test types of the UEAssetDoc.Tolerant.Material test (AssetDocumentData/Private/Tests): a project material
// expression the test flags deprecated after the material is saved, as an older material holds a node of a class
// that was deprecated since. Hidden from the Material Editor palette.

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"

#include "AssetDocumentMaterialTestTypes.generated.h"

UCLASS(collapsecategories, hidecategories = Object, meta = (Private))
class UAssetDocumentMaterialTestExpression : public UMaterialExpression
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Test")
	float Scale = 1.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	float Bias = 0.f;
};
