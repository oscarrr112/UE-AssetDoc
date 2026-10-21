// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/ValueOrBBKey.h"
#include "StructUtils/InstancedStruct.h"
#include "AssetDocumentAIReadbackTestTypes.generated.h"

UENUM()
enum class EAssetDocumentAIReadbackEnum : uint8 { First = 3, Second = 7 };

USTRUCT()
struct FAssetDocumentAIReadbackEmpty
{
	GENERATED_BODY()
	UPROPERTY() int32 NonAuthored = 91;
};

USTRUCT()
struct FAssetDocumentAIReadbackFields
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category="Test") int32 Count = 37;
	UPROPERTY(EditAnywhere, Category="Test") FString Label = TEXT("literal");
	UPROPERTY(EditAnywhere, Transient, Category="Test") int32 Cache = 91;
	UPROPERTY(VisibleAnywhere, Category="Test") int32 ReadOnly = 92;
	UPROPERTY() int32 NonAuthored = 93;
};

UCLASS()
class UAssetDocumentAIReadbackDynamicFixture : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="Test") FInstancedStruct Dynamic;
};

UCLASS()
class UAssetDocumentAIReadbackValuesFixture : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="Test") FAssetDocumentAIReadbackFields Ordinary;
	UPROPERTY(EditAnywhere, Category="Test") FVector Vector = FVector(1.25, -2.5, 3.75);
	UPROPERTY(EditAnywhere, Category="Test") FBlackboardKeySelector Selector;
	UPROPERTY(EditAnywhere, Category="Test") TSoftObjectPtr<UObject> SoftObject;
	UPROPERTY(EditAnywhere, Category="Test") TSoftClassPtr<UObject> SoftClass;
	UPROPERTY(EditAnywhere, Category="Test") TArray<int32> Array;
	UPROPERTY(EditAnywhere, Category="Test") TMap<FString, int32> StringMap;
	UPROPERTY(EditAnywhere, Category="Test") TMap<FName, int32> NameMap;
	UPROPERTY(EditAnywhere, Category="Test") TMap<uint8, int32> ByteMap;
	UPROPERTY(EditAnywhere, Category="Test") TMap<EAssetDocumentAIReadbackEnum, int32> EnumMap;
	UPROPERTY(EditAnywhere, Category="Test") TSet<FString> Set;
};

UCLASS()
class UAssetDocumentAIReadbackFamiliesFixture : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Bool BoolValue;
	UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Class ClassValue;
	UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Enum EnumValue;
	UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Float FloatValue;
	UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Int32 IntValue;
	UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Name NameValue;
	UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_String StringValue;
	UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Object ObjectValue;
	UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Rotator RotatorValue;
	UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Struct StructValue;
	UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Vector VectorValue;
};

UCLASS()
class UAssetDocumentAIReadbackUnsupportedMapFixture : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="Test") TMap<int32, int32> Unsupported;
};

UCLASS()
class UAssetDocumentAIReadbackUnsupportedLeafFixture : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="Test") TWeakObjectPtr<UObject> Unsupported;
};

UCLASS()
class UAssetDocumentAIReadbackOverflowFixture : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="Test") uint64 TooLarge = MAX_uint64;
};

USTRUCT()
struct FAssetDocumentAIValueOrContainer
{
 GENERATED_BODY()
 UPROPERTY(EditAnywhere, Category="Test") int32 Identity = 0;
 UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Int32 Value;
};

UCLASS()
class UAssetDocumentAIValueOrTask : public UBTTaskNode
{
 GENERATED_BODY()
public:
 UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Bool BoolValue;
 UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Class ClassValue;
 UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Enum EnumValue;
 UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Float FloatValue;
 UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Int32 Int32Value;
 UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Name NameValue;
 UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Object ObjectValue;
 UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Rotator RotatorValue;
 UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_String StringValue;
 UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Struct StructValue;
 UPROPERTY(EditAnywhere, Category="Test") FValueOrBBKey_Vector VectorValue;
 UPROPERTY(EditAnywhere, Category="Test") FInstancedStruct Containers;
};

/** A real legal task with no ValueOr fields, including inherited authored fields. */
UCLASS()
class UAssetDocumentAIValueOrEmptyTask : public UBTTaskNode
{
 GENERATED_BODY()
};

UCLASS(EditInlineNew,DefaultToInstanced)
class UAssetDocumentAIInstallOwnedChild : public UObject
{
 GENERATED_BODY()
public:
 UPROPERTY(EditAnywhere,Category="Test") int32 AuthoredValue=73;
};

UCLASS(Blueprintable)
class UAssetDocumentAIInstallFamiliesTask : public UAssetDocumentAIValueOrTask
{
 GENERATED_BODY()
public:
 UAssetDocumentAIInstallFamiliesTask()
 {
  Nested=CreateDefaultSubobject<UAssetDocumentAIInstallOwnedChild>(TEXT("OwnedNested"));
 }
 UPROPERTY(EditAnywhere,Instanced,Category="Test") TObjectPtr<UAssetDocumentAIInstallOwnedChild> Nested;
};

USTRUCT()
struct FAssetDocumentAIValueOrContainers
{
 GENERATED_BODY()
 UPROPERTY(EditAnywhere, Category="Test") FAssetDocumentAIValueOrContainer Nested;
 UPROPERTY(EditAnywhere, Category="Test") FAssetDocumentAIValueOrContainer StaticValues[2];
 UPROPERTY(EditAnywhere, Category="Test") TArray<FAssetDocumentAIValueOrContainer> Array;
 UPROPERTY(EditAnywhere, Category="Test") TMap<FName, FAssetDocumentAIValueOrContainer> Map;
 UPROPERTY(EditAnywhere, Category="Test") FInstancedStruct Dynamic;
};
