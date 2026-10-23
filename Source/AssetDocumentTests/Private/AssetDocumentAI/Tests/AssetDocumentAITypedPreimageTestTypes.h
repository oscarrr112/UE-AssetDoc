// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BlackboardData.h"
#include "Engine/DataAsset.h"
#include "Misc/Optional.h"
#include "UObject/UnrealType.h"

#include "AssetDocumentAITypedPreimageTestTypes.generated.h"

USTRUCT()
struct FAssetDocumentAITypedPreimageTestWrongStruct
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	int32 Value = 0;
};

UENUM()
enum class EAssetDocumentAITypedPreimageTestEnum : uint8
{
	Original,
	Replacement,
};

UENUM()
enum EAssetDocumentAITypedPreimageLegacyEnum : uint8
{
	LegacyOriginal,
	LegacyReplacement,
};

USTRUCT()
struct FAssetDocumentAITypedPreimageTestNestedSelector
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FBlackboardKeySelector Selector;
};

UCLASS(EditInlineNew)
class UAssetDocumentAITypedPreimageTestKeyType : public UBlackboardKeyType
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	bool BoolValue = false;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	int32 Int32Value = 0;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	int8 Int8Value = 0;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	int16 Int16Value = 0;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	int64 Int64Value = 0;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	uint8 ByteValue = 0;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	uint16 UInt16Value = 0;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	uint32 UInt32Value = 0;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	uint64 UInt64Value = 0;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	float FloatValue = 0.0f;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	double DoubleValue = 0.0;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FString StringValue;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FText TextValue;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FName NameValue;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FVector VectorValue = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FVector2D Vector2DValue = FVector2D::ZeroVector;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FVector4 Vector4Value = FVector4(0.0, 0.0, 0.0, 0.0);

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FQuat QuatValue = FQuat::Identity;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FRotator RotatorValue = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	TObjectPtr<UObject> ObjectValue;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	TSubclassOf<UObject> ClassValue;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	TObjectPtr<UEnum> EnumValue;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	EAssetDocumentAITypedPreimageTestEnum EnumScalarValue =
		EAssetDocumentAITypedPreimageTestEnum::Original;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	TEnumAsByte<EAssetDocumentAITypedPreimageLegacyEnum> LegacyEnumValue =
		LegacyOriginal;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	TArray<int32> UnsupportedArrayValue;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	TOptional<int32> UnsupportedOptionalValue;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FAssetDocumentAITypedPreimageTestWrongStruct UnsupportedStructValue;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	bool BoolValueMirror = false;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	int32 Int32ValueMirror = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	FString StringValueMirror;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	FName NameValueMirror;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	FVector VectorValueMirror = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	FRotator RotatorValueMirror = FRotator::ZeroRotator;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	int32 PostEditChangePropertyCount = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	bool bPackageDirtyDuringPostEditChangeProperty = false;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override
	{
		++PostEditChangePropertyCount;
		bPackageDirtyDuringPostEditChangeProperty = GetOutermost() && GetOutermost()->IsDirty();
		BoolValueMirror = BoolValue;
		Int32ValueMirror = Int32Value;
		StringValueMirror = StringValue;
		NameValueMirror = NameValue;
		VectorValueMirror = VectorValue;
		RotatorValueMirror = RotatorValue;

		Super::PostEditChangeProperty(PropertyChangedEvent);
	}
#endif
};

UCLASS(EditInlineNew)
class UAssetDocumentAITypedPreimageTestZeroFieldKeyType
	: public UBlackboardKeyType
{
	GENERATED_BODY()
};

UCLASS(EditInlineNew)
class UAssetDocumentAITypedPreimageTestLocalKeyType : public UBlackboardKeyType
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	int32 Int32Value = 0;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FString StringValue;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	int32 PostEditChangePropertyCount = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	bool bPackageDirtyDuringPostEditChangeProperty = false;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override
	{
		++PostEditChangePropertyCount;
		bPackageDirtyDuringPostEditChangeProperty =
			GetOutermost() && GetOutermost()->IsDirty();
		Super::PostEditChangeProperty(PropertyChangedEvent);
	}
#endif
};

UCLASS()
class UAssetDocumentAITypedPreimageTestSelectorOwner : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FBlackboardKeySelector Selector;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FVector WrongVector = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FAssetDocumentAITypedPreimageTestWrongStruct WrongStruct;

	UPROPERTY(EditAnywhere, Category = TypedPreimage)
	FAssetDocumentAITypedPreimageTestNestedSelector Nested;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	int32 PostEditChangePropertyCount = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	bool bPackageDirtyDuringPostEditChangeProperty = false;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override
	{
		++PostEditChangePropertyCount;
		bPackageDirtyDuringPostEditChangeProperty = GetOutermost() && GetOutermost()->IsDirty();

		Super::PostEditChangeProperty(PropertyChangedEvent);
	}
#endif
};

UCLASS()
class UAssetDocumentAITypedPreimageTestBlackboardData : public UBlackboardData
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	int32 PostEditChangePropertyCount = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = TypedPreimage)
	bool bPackageDirtyDuringPostEditChangeProperty = false;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override
	{
		++PostEditChangePropertyCount;
		bPackageDirtyDuringPostEditChangeProperty = GetOutermost() && GetOutermost()->IsDirty();

		Super::PostEditChangeProperty(PropertyChangedEvent);
	}
#endif
};
