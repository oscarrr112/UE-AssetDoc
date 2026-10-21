// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "AssetDocumentEditorNativeStateTestTypes.generated.h"

class UEdGraph;
class UEdGraphNode;
class UPackage;

UCLASS()
class UAssetDocumentEditorNativeStateTestObject final : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "TypedPreimage")
	TObjectPtr<UObject> AuthoredReference = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UObject> DerivedMirror = nullptr;

	UPROPERTY(EditAnywhere, Category = "TypedPreimage")
	int32 ScalarValue = 0;

	UPROPERTY(EditAnywhere, Category = "TypedPreimage")
	TArray<TObjectPtr<UObject>> AuthoredReferences;

	UPROPERTY(EditAnywhere, Category = "TypedPreimage")
	TOptional<TObjectPtr<UObject>> OptionalReference;

	UPROPERTY(EditAnywhere, Category = "TypedPreimage")
	TSubclassOf<UObject> ClassReference;

	UPROPERTY(EditAnywhere, Category = "TypedPreimage")
	TObjectPtr<UPackage> PackageReference = nullptr;

	UPROPERTY(EditAnywhere, Category = "TypedPreimage")
	TObjectPtr<UEdGraph> GraphReference = nullptr;

	UPROPERTY(EditAnywhere, Category = "TypedPreimage")
	TObjectPtr<UEdGraphNode> NodeReference = nullptr;

	int32 AuthoredReferencePostEditCount = 0;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(
		FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
