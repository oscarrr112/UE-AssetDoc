// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Object.h"

#include "AssetDocumentEditorPropertyInspectTestTypes.generated.h"

UENUM()
enum class EAssetDocumentEditorPropertyInspectTestEnum : uint8
{
	First,
	Second,
};

USTRUCT()
struct FAssetDocumentEditorPropertyInspectPlainValue
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	int32 Count = 7;
};

USTRUCT()
struct FAssetDocumentEditorPropertyInspectNestedValue
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FAssetDocumentEditorPropertyInspectPlainValue Plain;

	UPROPERTY(EditAnywhere)
	FString Label = TEXT("nested");
};

USTRUCT()
struct FAssetDocumentEditorPropertyInspectUnsupportedValue
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	TObjectPtr<UObject> InlineObject = nullptr;
};

UCLASS(EditInlineNew)
class UAssetDocumentEditorPropertyInspectInlineObject : public UObject
{
	GENERATED_BODY()
};

UCLASS()
class UAssetDocumentEditorPropertyInspectTestObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere)
	bool BooleanValue = false;

	UPROPERTY(EditAnywhere)
	int32 IntegerValue = 17;

	UPROPERTY(EditAnywhere)
	double FloatValue = 1.5;

	UPROPERTY(EditAnywhere)
	EAssetDocumentEditorPropertyInspectTestEnum EnumValue =
		EAssetDocumentEditorPropertyInspectTestEnum::First;

	UPROPERTY(EditAnywhere)
	FString StringValue = TEXT("default string");

	UPROPERTY(EditAnywhere)
	FName NameValue = TEXT("DefaultName");

	UPROPERTY(EditAnywhere)
	FText TextValue = FText::FromString(TEXT("default text"));

	UPROPERTY(EditAnywhere)
	TObjectPtr<UObject> HardObjectValue = nullptr;

	UPROPERTY(EditAnywhere)
	TSoftObjectPtr<UObject> SoftObjectValue;

	UPROPERTY(EditAnywhere)
	TSubclassOf<UObject> HardClassValue;

	UPROPERTY(EditAnywhere)
	TSoftClassPtr<UObject> SoftClassValue;

	UPROPERTY(EditAnywhere)
	FAssetDocumentEditorPropertyInspectPlainValue PlainValue;

	UPROPERTY(EditAnywhere)
	FAssetDocumentEditorPropertyInspectNestedValue NestedValue;

	UPROPERTY(EditAnywhere)
	int32 FixedArrayValue[2] = {3, 5};

	UPROPERTY(EditAnywhere)
	TArray<int32> ArrayValue;

	UPROPERTY(EditAnywhere)
	TMap<FString, int32> StringMapValue;

	UPROPERTY(EditAnywhere)
	TMap<FName, int32> NameMapValue;

	UPROPERTY(EditAnywhere)
	TMap<int32, FString> NumericMapValue;

	UPROPERTY(EditAnywhere)
	TSet<FString> SetValue;

	UPROPERTY(EditAnywhere)
	FInstancedStruct DynamicValue;

	UPROPERTY(EditAnywhere, Instanced)
	TObjectPtr<UAssetDocumentEditorPropertyInspectInlineObject> InlineObject;

	UPROPERTY(EditAnywhere)
	int32 EditConstValue = 23;

	UPROPERTY(EditAnywhere, Transient)
	int32 TransientValue = 29;

	UPROPERTY(EditAnywhere, DuplicateTransient)
	int32 DuplicateTransientValue = 31;

	UPROPERTY(EditAnywhere, meta = (DeprecatedProperty, DeprecationMessage = "Fixture-only deprecated field."))
	int32 DeprecatedValue = 37;

	UPROPERTY(EditAnywhere, SkipSerialization)
	int32 SkipSerializationValue = 41;
};
