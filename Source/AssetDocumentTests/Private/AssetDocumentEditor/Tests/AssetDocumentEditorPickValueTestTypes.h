// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"

#include "AssetDocumentEditorPickValueTestTypes.generated.h"

// Test-only types of the picker-value checks (AssetDocumentEditorPickerValues.h): a table row handle and an input
// key in every place a document writes them. Production code has no dependency on them.

// A DataTable row struct whose cells are a row handle and a key (DataTable cells).
USTRUCT()
struct FAssetDocumentPickTestRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Amount = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	FDataTableRowHandle Next;

	UPROPERTY(EditAnywhere, Category = "Test")
	FKey Key;
};

// A generic asset (Properties).
UCLASS(BlueprintType)
class UAssetDocumentPickTestAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Test")
	FDataTableRowHandle Pickup;

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FDataTableRowHandle> Rewards;

	UPROPERTY(EditAnywhere, Category = "Test")
	FKey Key;
};

// A level actor and Blueprint parent (edit_level, ClassDefaults).
UCLASS(Blueprintable)
class AAssetDocumentPickTestActor : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	FDataTableRowHandle Pickup;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	FKey Key;
};

// A Blueprint component (component template Properties).
UCLASS(ClassGroup = (AssetDocumentTest), meta = (BlueprintSpawnableComponent))
class UAssetDocumentPickTestComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	FDataTableRowHandle Pickup;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	FKey Key;
};
