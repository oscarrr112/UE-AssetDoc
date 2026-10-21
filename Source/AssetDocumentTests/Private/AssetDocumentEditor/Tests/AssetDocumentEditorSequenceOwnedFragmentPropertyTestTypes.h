// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Animation/AnimMetaData.h"
#include "Engine/AssetUserData.h"
#include "StructUtils/SharedStruct.h"
#include "UObject/SoftObjectPtr.h"
#include "AssetDocumentEditorSequenceOwnedFragmentPropertyTestTypes.generated.h"

USTRUCT()
struct FAssetDocumentEditorSequenceFragmentReferenceTest
{
	GENERATED_BODY()
	UPROPERTY()
	TObjectPtr<UObject> Object = nullptr;
};

USTRUCT()
struct FAssetDocumentEditorSequenceFragmentSharedValueTest
{
	GENERATED_BODY()
	UPROPERTY()
	FSharedStruct Value;
	bool operator==(const FAssetDocumentEditorSequenceFragmentSharedValueTest& Other) const { return Value == Other.Value; }
	friend uint32 GetTypeHash(const FAssetDocumentEditorSequenceFragmentSharedValueTest& V) { return PointerHash(V.Value.GetMemory()); }
};

// No reflected members: the existing struct codec decodes {} by constructing
// this native value. The weak test seed exposes desired-buffer ARO lifetime.
USTRUCT()
struct FAssetDocumentEditorSequenceFragmentDesiredReferenceTest
{
	GENERATED_BODY()
	FAssetDocumentEditorSequenceFragmentDesiredReferenceTest();
	static void SetDefaultReference(UObject* Object);
	TObjectPtr<UObject> Reference = nullptr;
	void AddStructReferencedObjects(FReferenceCollector& Collector);
	bool Identical(const FAssetDocumentEditorSequenceFragmentDesiredReferenceTest* Other, uint32) const
	{ return Other && Reference == Other->Reference; }
};
template<> struct TStructOpsTypeTraits<FAssetDocumentEditorSequenceFragmentDesiredReferenceTest>
	: TStructOpsTypeTraitsBase2<FAssetDocumentEditorSequenceFragmentDesiredReferenceTest>
{
	enum { WithCopy = true, WithIdentical = true, WithAddStructReferencedObjects = true };
};

UCLASS()
class UAssetDocumentEditorSequenceFragmentMetadataTest final : public UAnimMetaData
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	int32 First = 3;
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	int32 Second = 5;
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	TArray<int32> Values;
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	int32 StaticValues[2] = {1, 6};
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	TSoftObjectPtr<UObject> SoftReference;
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	FSharedStruct SharedValue;
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	FAssetDocumentEditorSequenceFragmentSharedValueTest NestedSharedValue;
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	TArray<FSharedStruct> SharedValues;
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	TMap<FString, FAssetDocumentEditorSequenceFragmentSharedValueTest> SharedMap;
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	TSet<FAssetDocumentEditorSequenceFragmentSharedValueTest> SharedSet;
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	FSharedStruct SharedStaticValues[2];
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	FAssetDocumentEditorSequenceFragmentDesiredReferenceTest NativeDesired;
	UPROPERTY(VisibleAnywhere, Category = "FragmentTest")
	int32 ReadOnly = 11;
	UPROPERTY(EditInstanceOnly, Category = "FragmentTest")
	int32 InstanceOnly = 13;
	// Hard references remain rejected by the existing typed codec.
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	TObjectPtr<UObject> UnsupportedHardReference = nullptr;
	int32 PreEditCount = 0, PostEditCount = 0, ListenerEffect = 0;
#if WITH_EDITOR
	void PreEditChange(FProperty* Property) override;
	void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
};

UCLASS()
class UAssetDocumentEditorSequenceFragmentUserDataTest final : public UAssetUserData
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	int32 First = 7;
	UPROPERTY(EditAnywhere, Category = "FragmentTest")
	FString Label = TEXT("before");
	int32 PostEditCount = 0, OwnerEditCount = 0;
#if WITH_EDITOR
	void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
	void PostEditChangeOwner(const FPropertyChangedEvent& Event) override;
};
