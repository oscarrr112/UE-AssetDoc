// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Animation/AnimMetaData.h"
#include "Engine/AssetUserData.h"
#include "AssetDocumentAnimationSequenceOwnedFragmentLifecycleTestTypes.generated.h"

UCLASS()
class UAssetDocumentSequenceLifecycleMetadataTest final : public UAnimMetaData
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "LifecycleTest")
	int32 First = 3;
	UPROPERTY(EditAnywhere, Category = "LifecycleTest")
	FString Label = TEXT("before");
	UPROPERTY(EditAnywhere, Category = "LifecycleTest")
	TArray<int32> Values;
};

UCLASS()
class UAssetDocumentSequenceLifecycleUserDataTest : public UAssetUserData
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "LifecycleTest")
	int32 First = 7;
	UPROPERTY(EditAnywhere, Category = "LifecycleTest")
	FString Label = TEXT("before");
};

UCLASS()
class UAssetDocumentSequenceLifecycleDerivedUserDataTest final : public UAssetDocumentSequenceLifecycleUserDataTest
{
	GENERATED_BODY()
};

UCLASS()
class UAssetDocumentSequenceLifecycleOtherUserDataTest final : public UAssetUserData
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "LifecycleTest")
	int32 First = 11;
};
