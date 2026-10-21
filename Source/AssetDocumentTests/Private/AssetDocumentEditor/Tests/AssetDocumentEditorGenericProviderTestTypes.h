// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"

#include "AssetDocumentEditorGenericProviderTestTypes.generated.h"

UENUM()
enum class EAssetDocumentEditorCodecTestEnum : uint8
{
	First,
	Second,
};

USTRUCT()
struct FAssetDocumentEditorCodecNestedValue
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Count = 0;

	UPROPERTY()
	FString Label;
};

USTRUCT()
struct FAssetDocumentEditorCodecTrackedValue
{
	GENERATED_BODY()

	FAssetDocumentEditorCodecTrackedValue()
	{
		++ConstructionCount();
	}

	FAssetDocumentEditorCodecTrackedValue(
		const FAssetDocumentEditorCodecTrackedValue& Other)
		: Value(Other.Value)
	{
		++ConstructionCount();
	}

	~FAssetDocumentEditorCodecTrackedValue()
	{
		++DestructionCount();
	}

	static int32& ConstructionCount()
	{
		static int32 Count = 0;
		return Count;
	}

	static int32& DestructionCount()
	{
		static int32 Count = 0;
		return Count;
	}

	static void ResetLifetimeCounts()
	{
		ConstructionCount() = 0;
		DestructionCount() = 0;
	}

	UPROPERTY()
	int32 Value = 0;

	UPROPERTY()
	EAssetDocumentEditorCodecTestEnum EnumValue =
		EAssetDocumentEditorCodecTestEnum::First;
};

UINTERFACE()
class UAssetDocumentEditorCodecTestInterface : public UInterface
{
	GENERATED_BODY()
};

class IAssetDocumentEditorCodecTestInterface
{
	GENERATED_BODY()
};

DECLARE_DYNAMIC_DELEGATE(FAssetDocumentEditorCodecTestDelegate);

UCLASS()
class UAssetDocumentEditorCodecTestObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	bool BooleanValue = false;

	UPROPERTY()
	int64 IntegerValue = 0;

	UPROPERTY()
	double NumberValue = 0.0;

	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	FName NameValue = NAME_None;

	UPROPERTY()
	EAssetDocumentEditorCodecTestEnum EnumValue =
		EAssetDocumentEditorCodecTestEnum::First;

	UPROPERTY()
	TSoftObjectPtr<UObject> SoftObjectValue;

	UPROPERTY()
	TSoftClassPtr<UObject> SoftClassValue;

	UPROPERTY()
	int32 FixedArrayValue[2];

	UPROPERTY()
	TArray<int32> ArrayValue;

	UPROPERTY()
	TSet<FString> SetValue;

	UPROPERTY()
	TMap<FString, int32> StringMapValue;

	UPROPERTY()
	FAssetDocumentEditorCodecNestedValue NestedValue;

	UPROPERTY()
	FAssetDocumentEditorCodecTrackedValue TrackedValue;

	UPROPERTY()
	TObjectPtr<UObject> HardObjectValue = nullptr;

	UPROPERTY()
	FAssetDocumentEditorCodecTestDelegate DelegateValue;

	UPROPERTY()
	TScriptInterface<IAssetDocumentEditorCodecTestInterface> InterfaceValue;

	UPROPERTY()
	TWeakObjectPtr<UObject> WeakObjectValue;

	UPROPERTY()
	TLazyObjectPtr<UObject> LazyObjectValue;

	UPROPERTY()
	TMap<int32, FString> IntegerMapValue;
};
