// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/StaticMesh.h"

#include "AssetDocumentVerificationSemanticsTestTypes.generated.h"

// Fixture: a component whose property is an array of structs holding a float and maps,
// like a stance palette (BufferTolerance, Swap: TMap<NiagaraSystem*, NiagaraSystem*>).
USTRUCT(BlueprintType)
struct FAssetDocumentVerificationPalette
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	FName Stance;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	float BufferTolerance = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	TMap<TObjectPtr<UStaticMesh>, TObjectPtr<UStaticMesh>> Swap;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	TMap<FName, float> Weights;
};

UCLASS(ClassGroup = (AssetDocumentTest), meta = (BlueprintSpawnableComponent))
class UAssetDocumentVerificationPaletteComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	TArray<FAssetDocumentVerificationPalette> Palettes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	float ChargeFullScale = 1.f;
};
