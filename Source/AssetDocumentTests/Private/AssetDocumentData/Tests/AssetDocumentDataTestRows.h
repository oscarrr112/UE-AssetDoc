// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Internationalization/Text.h"
#include "Templates/SubclassOf.h"
#include "UObject/SoftObjectPtr.h"
#include "AssetDocumentDataTestRows.generated.h"

// Row types for the data table family end-to-end tests (AssetDocumentDataTablePythonTests.cpp).
// Production code has no dependency on them.

UENUM()
enum class EAssetDocumentDataTestKind : uint8
{
	Alpha,
	Beta,
	Gamma
};

USTRUCT()
struct FAssetDocumentDataTestRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Label;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Count = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	float Weight = 1.f;

	UPROPERTY(EditAnywhere, Category = "Test")
	double Precise = 0.0;

	UPROPERTY(EditAnywhere, Category = "Test")
	bool bEnabled = false;

	UPROPERTY(EditAnywhere, Category = "Test")
	FName Tag;

	UPROPERTY(EditAnywhere, Category = "Test")
	EAssetDocumentDataTestKind Kind = EAssetDocumentDataTestKind::Alpha;

	UPROPERTY(EditAnywhere, Category = "Test")
	FVector Offset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<int32> Levels;

	UPROPERTY(EditAnywhere, Category = "Test")
	TSoftObjectPtr<UObject> Icon;

	UPROPERTY(EditAnywhere, Category = "Test")
	FText DisplayName;

	UPROPERTY(EditAnywhere, Category = "Test")
	TObjectPtr<UObject> Source;

	// The row editor cannot create instanced subobjects (no owning object in a struct
	// view), so this field is not extracted and refused when authored.
	UPROPERTY(EditAnywhere, Instanced, Category = "Test")
	TObjectPtr<UObject> Inline;
};

// A second row struct: composite parents must share the row struct; RowStruct is immutable.
USTRUCT()
struct FAssetDocumentDataTestOtherRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Value = 0;
};

// Not a valid row struct (does not derive from FTableRowBase).
USTRUCT()
struct FAssetDocumentDataTestNotARow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Value = 0;
};

// A wide row (20 scalar fields) for the large-table test.
USTRUCT()
struct FAssetDocumentDataTestWideRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test") FString Name;
	UPROPERTY(EditAnywhere, Category = "Test") FString Description;
	UPROPERTY(EditAnywhere, Category = "Test") FName Category;
	UPROPERTY(EditAnywhere, Category = "Test") FName Rarity;
	UPROPERTY(EditAnywhere, Category = "Test") int32 Level = 0;
	UPROPERTY(EditAnywhere, Category = "Test") int32 Price = 0;
	UPROPERTY(EditAnywhere, Category = "Test") int32 StackSize = 1;
	UPROPERTY(EditAnywhere, Category = "Test") int32 Durability = 0;
	UPROPERTY(EditAnywhere, Category = "Test") int64 Id = 0;
	UPROPERTY(EditAnywhere, Category = "Test") float Weight = 0.f;
	UPROPERTY(EditAnywhere, Category = "Test") float Damage = 0.f;
	UPROPERTY(EditAnywhere, Category = "Test") float Speed = 0.f;
	UPROPERTY(EditAnywhere, Category = "Test") float Range = 0.f;
	UPROPERTY(EditAnywhere, Category = "Test") double Chance = 0.0;
	UPROPERTY(EditAnywhere, Category = "Test") bool bTradable = false;
	UPROPERTY(EditAnywhere, Category = "Test") bool bQuest = false;
	UPROPERTY(EditAnywhere, Category = "Test") bool bConsumable = false;
	UPROPERTY(EditAnywhere, Category = "Test") EAssetDocumentDataTestKind Kind = EAssetDocumentDataTestKind::Alpha;
	UPROPERTY(EditAnywhere, Category = "Test") uint8 Tier = 0;
	UPROPERTY(EditAnywhere, Category = "Test") FString Icon;
};

// A 20-field row with FText, hard and soft object references (slower value codecs) for the
// large-table test.
USTRUCT()
struct FAssetDocumentDataTestRichRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test") FText DisplayName;
	UPROPERTY(EditAnywhere, Category = "Test") FText Description;
	UPROPERTY(EditAnywhere, Category = "Test") FText Tooltip;
	UPROPERTY(EditAnywhere, Category = "Test") FText Flavor;
	UPROPERTY(EditAnywhere, Category = "Test") TObjectPtr<UObject> Mesh;
	UPROPERTY(EditAnywhere, Category = "Test") TObjectPtr<UObject> Material;
	UPROPERTY(EditAnywhere, Category = "Test") TObjectPtr<UObject> Texture;
	UPROPERTY(EditAnywhere, Category = "Test") TSubclassOf<UObject> SpawnClass;
	UPROPERTY(EditAnywhere, Category = "Test") TSoftObjectPtr<UObject> SoftMesh;
	UPROPERTY(EditAnywhere, Category = "Test") TSoftObjectPtr<UObject> SoftMaterial;
	UPROPERTY(EditAnywhere, Category = "Test") TSoftObjectPtr<UObject> SoftTexture;
	UPROPERTY(EditAnywhere, Category = "Test") FString Name;
	UPROPERTY(EditAnywhere, Category = "Test") FName Category;
	UPROPERTY(EditAnywhere, Category = "Test") int32 Level = 0;
	UPROPERTY(EditAnywhere, Category = "Test") int32 Price = 0;
	UPROPERTY(EditAnywhere, Category = "Test") int32 StackSize = 1;
	UPROPERTY(EditAnywhere, Category = "Test") float Weight = 0.f;
	UPROPERTY(EditAnywhere, Category = "Test") float Damage = 0.f;
	UPROPERTY(EditAnywhere, Category = "Test") bool bTradable = false;
	UPROPERTY(EditAnywhere, Category = "Test") EAssetDocumentDataTestKind Kind = EAssetDocumentDataTestKind::Alpha;
};
