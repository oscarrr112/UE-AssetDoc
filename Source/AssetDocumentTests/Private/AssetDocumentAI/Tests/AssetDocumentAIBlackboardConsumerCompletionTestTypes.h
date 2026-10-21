// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/ValueOrBBKey.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/SoftObjectPath.h"

#include "AssetDocumentAIBlackboardConsumerCompletionTestTypes.generated.h"

USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionInstancedTestValue
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FBlackboardKeySelector Selector;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Name ValueOrName;
};

USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionTestMatrix
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FBlackboardKeySelector NestedSelector;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Int32 NestedValueOrInt32;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FInstancedStruct NestedInstancedValue;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	TArray<FBlackboardKeySelector> UniqueSelectors;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	TMap<FName, FBlackboardKeySelector> StableSelectorValues;
};

/** Single-leaf fixtures keep each excluded-policy branch independently observable. */
USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionExcludedSelector
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	FBlackboardKeySelector Selector;
};

USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionExcludedValueOr
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	FValueOrBBKey_Int32 ValueOrInt32;
};

USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionExcludedMapKey
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FBlackboardKeySelector Selector;

	bool operator==(const FAssetDocumentAIBlackboardConsumerCompletionExcludedMapKey& Other) const
	{
		return Selector.SelectedKeyName == Other.Selector.SelectedKeyName;
	}
};

FORCEINLINE uint32 GetTypeHash(
	const FAssetDocumentAIBlackboardConsumerCompletionExcludedMapKey& Value)
{
	return GetTypeHash(Value.Selector.SelectedKeyName);
}

template <>
struct TStructOpsTypeTraits<FAssetDocumentAIBlackboardConsumerCompletionExcludedMapKey>
	: TStructOpsTypeTraitsBase2<FAssetDocumentAIBlackboardConsumerCompletionExcludedMapKey>
{
	enum { WithIdenticalViaEquality = true, WithGetTypeHash = true };
};

USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionExcludedMapKeyContainer
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TMap<FAssetDocumentAIBlackboardConsumerCompletionExcludedMapKey, int32> LeafKeys;
};

/** Empty is admissible until a negative container shape is selected by a test. */
USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionEmptyContainer
{
	GENERATED_BODY()
};

/** Runtime-selected negatives keep the ordinary fixture capture-valid. */
USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionUnsupportedContainer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	TSet<FName> UnsupportedSet;
};

USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionObjectPathMapContainer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	TMap<FSoftObjectPath, FBlackboardKeySelector> ObjectPathKeyedSelectors;
};

USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionCodecFieldBudgetElement
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	int32 Value = 0;
};

USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionCodecRecursiveValue
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FInstancedStruct Next;
};

USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionCodecBudgetEnvelope
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FInstancedStruct RecursiveValue;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	TArray<FAssetDocumentAIBlackboardConsumerCompletionCodecFieldBudgetElement> FieldBudgetValues;
};

/** Scalar codec vectors only; test behavior and all expected digests remain in .cpp. */
USTRUCT()
struct FAssetDocumentAIBlackboardConsumerCompletionCodecScalars
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	int32 EndianInt = 0;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FName CaseAndNumber;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FString Utf8Text;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	float NegativeZero = 0.0f;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	double NonFinite = 0.0;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	TArray<int32> OrderedValues;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	TMap<FName, int32> StableValues;
};

/**
 * Test-only loaded BT node.  Its public fields make the complete fixture
 * topology inspectable by the private owner without introducing a shared test
 * type or a production surface.
 */
UCLASS()
class UAssetDocumentAIBlackboardConsumerCompletionTestTask final : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FBlackboardKeySelector DirectSelector;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Bool ValueOrBool;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Class ValueOrClass;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Enum ValueOrEnum;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Float ValueOrFloat;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Int32 ValueOrInt32;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Name ValueOrName;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_String ValueOrString;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Object ValueOrObject;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Rotator ValueOrRotator;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Struct ValueOrStruct;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Vector ValueOrVector;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FAssetDocumentAIBlackboardConsumerCompletionTestMatrix Matrix;

	/** A valid no-op contained type is replaced only for container-policy REDs. */
	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FInstancedStruct ConditionalNegativeContainer;

	/** Two individually valid leaves must not evade the receipt-wide byte budget. */
	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_String CaptureBudgetStringA;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_String CaptureBudgetStringB;

	/** The empty Struct ValueOr is still an admitted direct exact-family leaf. */
	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FValueOrBBKey_Struct EmptyDefaultValueOrStruct;

	/** A non-null reference is an intentional rejection; it must never be traversed. */
	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	TObjectPtr<UObject> RejectedTraversalReference = nullptr;
};

/** Bound-graph fixture proves composite decorator leaves are inventoried. */
UCLASS()
class UAssetDocumentAIBlackboardConsumerCompletionTestDecorator final : public UBTDecorator
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	FBlackboardKeySelector Selector;
};
