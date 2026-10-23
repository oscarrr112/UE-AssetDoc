// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "GameFramework/Actor.h"
#include "StructUtils/InstancedStruct.h"

#include "AssetDocumentReflectedPropertyTestTypes.generated.h"

USTRUCT()
struct FAssetDocumentReflectedPropertyNestedTestValue
{
	GENERATED_BODY()

	FAssetDocumentReflectedPropertyNestedTestValue()
	{
		Selector.AddObjectFilter(
			nullptr,
			GET_MEMBER_NAME_CHECKED(FAssetDocumentReflectedPropertyNestedTestValue, Selector),
			AActor::StaticClass());
		Selector.AllowNoneAsValue(true);
	}

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	FName Identity = NAME_None;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	FString PreservedText;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	FBlackboardKeySelector Selector;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	FInstancedStruct NestedValue;

	bool operator==(const FAssetDocumentReflectedPropertyNestedTestValue& Other) const
	{
		return Identity == Other.Identity;
	}
};

FORCEINLINE uint32 GetTypeHash(const FAssetDocumentReflectedPropertyNestedTestValue& Value)
{
	return GetTypeHash(Value.Identity);
}

template<>
struct TStructOpsTypeTraits<FAssetDocumentReflectedPropertyNestedTestValue>
	: public TStructOpsTypeTraitsBase2<FAssetDocumentReflectedPropertyNestedTestValue>
{
	enum
	{
		WithIdenticalViaEquality = true,
		WithGetTypeHash = true
	};
};

UCLASS()
class UAssetDocumentReflectedPropertyTestObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	FInstancedStruct DynamicValue;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest", meta = (BaseStruct = "/Script/AIModule.BlackboardEntry"))
	FInstancedStruct ConstrainedValue;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest", meta = (BaseStruct = "/Script/AssetDocument.DoesNotExist"))
	FInstancedStruct InvalidConstraintValue;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	TArray<FAssetDocumentReflectedPropertyNestedTestValue> ArrayValues;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	TMap<FName, FAssetDocumentReflectedPropertyNestedTestValue> MapValues;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	TMap<uint8, FAssetDocumentReflectedPropertyNestedTestValue> ByteMapValues;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	TMap<uint8, float> PlainByteMapValues;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	TSet<FAssetDocumentReflectedPropertyNestedTestValue> SetValues;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	TSet<FName> PlainNameSetValues;
};

/**
 * Concrete fixture whose constructor-owned selector filters must survive
 * transient graph materialization. It intentionally covers direct, nested,
 * container, and FInstancedStruct selector locations on one UBTNode instance.
 */
UCLASS(Blueprintable)
class UAssetDocumentSelectorTaskTestNode : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UAssetDocumentSelectorTaskTestNode(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
		: Super(ObjectInitializer)
	{
		DirectSelector.AddObjectFilter(
			this,
			GET_MEMBER_NAME_CHECKED(UAssetDocumentSelectorTaskTestNode, DirectSelector),
			AActor::StaticClass());
		OptionalSelector.AddBoolFilter(
			this,
			GET_MEMBER_NAME_CHECKED(UAssetDocumentSelectorTaskTestNode, OptionalSelector));
		OptionalSelector.AllowNoneAsValue(true);
		DynamicValue.InitializeAs<FAssetDocumentReflectedPropertyNestedTestValue>();
	}

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	FBlackboardKeySelector DirectSelector;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	FBlackboardKeySelector OptionalSelector;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	FAssetDocumentReflectedPropertyNestedTestValue NestedValue;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	TArray<FAssetDocumentReflectedPropertyNestedTestValue> ArrayValues;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	TMap<FName, FAssetDocumentReflectedPropertyNestedTestValue> MapValues;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	TSet<FAssetDocumentReflectedPropertyNestedTestValue> SetValues;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	FInstancedStruct DynamicValue;
};

/**
 * Blueprintable stock-UE bases used to prove that project-authored composite
 * and service classes participate in the same dynamic class path as tasks.
 */
UCLASS(Blueprintable)
class UAssetDocumentProjectCompositeTestNode : public UBTComposite_Selector
{
	GENERATED_BODY()
};

UCLASS(Blueprintable)
class UAssetDocumentProjectServiceTestNode : public UBTService_DefaultFocus
{
	GENERATED_BODY()
};

/**
 * Safe regression fixture for sparse Blackboard-only updates. When armed, its
 * lifecycle moves the old array allocation into retained storage before
 * replacing the live topology, so stale interior pointers remain valid while
 * the test proves that arbitrary project lifecycle code was invoked.
 */
UCLASS()
class UAssetDocumentSelectorLifecycleMutationTaskTestNode : public UBTTaskNode
{
	GENERATED_BODY()

public:
	virtual void InitializeFromAsset(UBehaviorTree& Asset) override
	{
		Super::InitializeFromAsset(Asset);
		++InitializeFromAssetCallCountForTest;
		++GlobalInitializeFromAssetCallCountForTest();
		if (bMutateContainersOnInitializeForTest)
		{
			RetiredArrayValues = MoveTemp(ArrayValues);
			FAssetDocumentReflectedPropertyNestedTestValue Replacement;
			Replacement.Identity = TEXT("LifecycleReplacement");
			ArrayValues = {MoveTemp(Replacement)};
		}
	}

	static int32& GlobalInitializeFromAssetCallCountForTest()
	{
		static int32 CallCount = 0;
		return CallCount;
	}

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	TArray<FAssetDocumentReflectedPropertyNestedTestValue> ArrayValues;

	bool bMutateContainersOnInitializeForTest = false;
	int32 InitializeFromAssetCallCountForTest = 0;
	TArray<FAssetDocumentReflectedPropertyNestedTestValue> RetiredArrayValues;
};

/**
 * Canonicalization fail-closed fixture. Integer map keys are valid UE authored
 * data but intentionally unsupported by the reflected JSON extractor.
 */
UCLASS()
class UAssetDocumentUnsupportedExtractTaskTestNode : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	TMap<int32, float> UnsupportedMap;
};
