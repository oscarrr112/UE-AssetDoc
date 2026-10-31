// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"

#include "AssetDocumentEditorCodecTestTypes.generated.h"

// A plain struct used as a map key (no native text form): the map reads it by its export text.
USTRUCT()
struct FAssetDocumentCodecTestCellKey
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Codec")
	FName Layer;

	UPROPERTY(EditAnywhere, Category = "Codec")
	int32 Index = 0;

	bool operator==(const FAssetDocumentCodecTestCellKey& Other) const
	{
		return Layer == Other.Layer && Index == Other.Index;
	}

	friend uint32 GetTypeHash(const FAssetDocumentCodecTestCellKey& Key)
	{
		return HashCombine(GetTypeHash(Key.Layer), ::GetTypeHash(Key.Index));
	}
};

// A struct with an optional member, as the Details panel shows it (a check box beside the value).
USTRUCT()
struct FAssetDocumentCodecTestOptionalSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Codec")
	FString Label;

	UPROPERTY(EditAnywhere, Category = "Codec")
	TOptional<float> Scale;
};

// A struct holding a map keyed by soft references.
USTRUCT()
struct FAssetDocumentCodecTestKeyedSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Codec")
	FString Label;

	UPROPERTY(EditAnywhere, Category = "Codec")
	TMap<TSoftObjectPtr<UObject>, FString> SoftNames;
};

// An instanced subobject as the Details panel edits it inline: an editable value, a DuplicateTransient value (saved,
// edited in Details, reset only on duplicate), and a saved value the Details panel does not show.
UCLASS(EditInlineNew, DefaultToInstanced)
class UAssetDocumentCodecTestEffect final : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Codec")
	float Strength = 0.0f;

	UPROPERTY(EditAnywhere, DuplicateTransient, Category = "Codec")
	FString Preview;

	UPROPERTY()
	int32 SavedState = 0;
};

UCLASS()
class UAssetDocumentCodecTestAsset final : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Codec")
	TMap<FGameplayTag, float> TagWeights;

	UPROPERTY(EditAnywhere, Category = "Codec")
	TMap<TSoftObjectPtr<UObject>, int32> SoftCounts;

	UPROPERTY(EditAnywhere, Category = "Codec")
	TMap<FAssetDocumentCodecTestCellKey, FString> CellNames;

	UPROPERTY(EditAnywhere, Category = "Codec")
	TOptional<int32> OptionalCount;

	UPROPERTY(EditAnywhere, Category = "Codec")
	TOptional<FAssetDocumentCodecTestOptionalSettings> OptionalSettings;

	UPROPERTY(EditAnywhere, Category = "Codec")
	FAssetDocumentCodecTestOptionalSettings Settings;

	UPROPERTY(EditAnywhere, Category = "Codec")
	FAssetDocumentCodecTestKeyedSettings Keyed;

	UPROPERTY(EditAnywhere, Instanced, Category = "Codec")
	TObjectPtr<UAssetDocumentCodecTestEffect> Effect;
};
