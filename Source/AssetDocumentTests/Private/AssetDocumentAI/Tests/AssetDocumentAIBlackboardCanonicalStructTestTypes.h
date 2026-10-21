// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"

#include "AssetDocumentAIBlackboardCanonicalStructTestTypes.generated.h"

UENUM()
enum class EAssetDocumentAIBlackboardCanonicalStructTestEnum : uint8
{
	First = 3,
	Second = 7,
	HiddenValue = 8 UMETA(Hidden),
	MAX = 9,
};

USTRUCT()
struct FAssetDocumentAIBlackboardCanonicalStructNested
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Count = 0;

	UPROPERTY()
	FString Label;
};

USTRUCT()
struct FAssetDocumentAIBlackboardCanonicalStructAdmitted
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<int32> ArrayValues;

	UPROPERTY()
	bool BoolValue = false;

	UPROPERTY()
	uint8 ByteValue = 0;

	UPROPERTY()
	TSubclassOf<UObject> ClassValue;

	UPROPERTY()
	double DoubleValue = 0.0;

	UPROPERTY()
	FInstancedStruct DynamicValue;

	UPROPERTY()
	EAssetDocumentAIBlackboardCanonicalStructTestEnum EnumValue =
		EAssetDocumentAIBlackboardCanonicalStructTestEnum::First;

	UPROPERTY()
	float FloatValue = 0.0f;

	UPROPERTY()
	TObjectPtr<UObject> HardObject;

	UPROPERTY()
	int64 Int64Value = 0;

	UPROPERTY()
	int32 IntValue = 0;

	UPROPERTY()
	TMap<FString, int32> MapValues;

	UPROPERTY()
	FName NameValue;

	UPROPERTY()
	FAssetDocumentAIBlackboardCanonicalStructNested NestedValue;

	UPROPERTY()
	TSet<FString> SetValues;

	UPROPERTY()
	TSoftClassPtr<UObject> SoftClassValue;

	UPROPERTY()
	TSoftObjectPtr<UObject> SoftObjectValue;

	UPROPERTY()
	int32 StaticValues[3] = {0, 0, 0};

	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	FText TextValue;

	UPROPERTY()
	uint16 UInt16Value = 0;

	UPROPERTY()
	uint32 UInt32Value = 0;

	UPROPERTY()
	uint64 UInt64Value = 0;
};

USTRUCT()
struct FAssetDocumentAIBlackboardCanonicalStructUnsupported
{
	GENERATED_BODY()

	UPROPERTY()
	TWeakObjectPtr<UObject> WeakObject;
};

USTRUCT()
struct FAssetDocumentAIBlackboardCanonicalStructBase
{
	GENERATED_BODY()

	UPROPERTY()
	int32 ZuluBase = 0;
};

USTRUCT()
struct FAssetDocumentAIBlackboardCanonicalStructDerived
	: public FAssetDocumentAIBlackboardCanonicalStructBase
{
	GENERATED_BODY()

	UPROPERTY()
	int32 AlphaDerived = 0;
};

USTRUCT()
struct FAssetDocumentAIBlackboardCanonicalStructCollisionKey
{
	GENERATED_BODY()

	UPROPERTY()
	float Value = 0.0f;

	bool operator==(const FAssetDocumentAIBlackboardCanonicalStructCollisionKey& Other) const
	{
		return BitCast<uint32>(Value) == BitCast<uint32>(Other.Value);
	}

	friend uint32 GetTypeHash(const FAssetDocumentAIBlackboardCanonicalStructCollisionKey& Key)
	{
		return ::GetTypeHash(BitCast<uint32>(Key.Value));
	}
};

template <>
struct TStructOpsTypeTraits<FAssetDocumentAIBlackboardCanonicalStructCollisionKey>
	: public TStructOpsTypeTraitsBase2<FAssetDocumentAIBlackboardCanonicalStructCollisionKey>
{
	enum
	{
		WithIdenticalViaEquality = true,
		WithGetTypeHash = true,
	};
};

USTRUCT()
struct FAssetDocumentAIBlackboardCanonicalStructCollisionSet
{
	GENERATED_BODY()

	UPROPERTY()
	TSet<FAssetDocumentAIBlackboardCanonicalStructCollisionKey> Values;
};

USTRUCT()
struct FAssetDocumentAIBlackboardCanonicalStructCollisionMap
{
	GENERATED_BODY()

	UPROPERTY()
	TMap<FAssetDocumentAIBlackboardCanonicalStructCollisionKey, int32> Values;
};

USTRUCT()
struct FAssetDocumentAIBlackboardCanonicalStructTransient
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	int32 Value = 0;
};

USTRUCT()
struct FAssetDocumentAIBlackboardCanonicalStructDepth
{
	GENERATED_BODY()

	UPROPERTY()
	FInstancedStruct Next;
};

USTRUCT()
struct FAssetDocumentAIBlackboardCanonicalStructArrayOnly
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<int32> Values;
};

USTRUCT()
struct FAssetDocumentAIBlackboardCanonicalStructUInt64Only
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 Value = 0;
};
