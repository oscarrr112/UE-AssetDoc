// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Components/SceneComponent.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPtr.h"

#include "LevelEditTestTypes.generated.h"

// Test-only types of the edit_level tests (UEAssetDoc.Level.Edit.*).

// An instanced settings object, edited inline on the component below.
UCLASS(EditInlineNew, DefaultToInstanced, CollapseCategories, HideDropdown)
class UAssetDocumentLevelTestSettings : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	float Strength = 1.0f;
};

// An EditInlineNew component placed in a level: PropertyAccessUtil::IsObjectTemplate
// treats it as a template, which the instance-semantics write path covers.
UCLASS(EditInlineNew, HideDropdown, ClassGroup = (AssetDocumentLevelTest))
class UAssetDocumentLevelTestComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(EditInstanceOnly, Instanced, Category = "AssetDocument Test")
	TObjectPtr<UAssetDocumentLevelTestSettings> Settings;

	UPROPERTY(EditInstanceOnly, Category = "AssetDocument Test")
	float InstanceOnlyValue = 0.0f;

	// PostEditChangeProperty clamps it to [0, 10], as engine components normalise values.
	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	float Clamped = 0.0f;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};

// A scene component with one socket, "Tip", 50 units above it: a socket parent for the
// component and attach tests without a mesh asset.
UCLASS(HideDropdown, ClassGroup = (AssetDocumentLevelTest), meta = (BlueprintSpawnableComponent))
class UAssetDocumentLevelTestSocketComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	virtual bool HasAnySockets() const override { return true; }
	virtual bool DoesSocketExist(FName InSocketName) const override;
	virtual FTransform GetSocketTransform(FName InSocketName, ERelativeTransformSpace TransformSpace = RTS_World) const override;
	virtual void QuerySupportedSockets(TArray<FComponentSocketDescription>& OutSockets) const override;
};

UCLASS(HideDropdown)
class AAssetDocumentLevelTestActor : public AActor
{
	GENERATED_BODY()

public:
	AAssetDocumentLevelTestActor();

	UPROPERTY(VisibleAnywhere, Category = "AssetDocument Test")
	TObjectPtr<UAssetDocumentLevelTestComponent> TestComponent;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	TObjectPtr<AActor> Target;

	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	TSoftObjectPtr<AActor> SoftTarget;

	UPROPERTY(EditDefaultsOnly, Category = "AssetDocument Test")
	bool bDefaultsOnly = false;

	// The construction script (OnConstruction) caps it at 100: a value the editor
	// rewrites after the write.
	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	float Score = 0.0f;

	virtual void OnConstruction(const FTransform& Transform) override;
};

// An actor whose construction script (OnConstruction) makes a scene component, as a
// Blueprint's construction script does (CreationMethod UserConstructionScript).
UCLASS(HideDropdown)
class AAssetDocumentLevelTestBuiltActor : public AActor
{
	GENERATED_BODY()

public:
	AAssetDocumentLevelTestBuiltActor();

	virtual void OnConstruction(const FTransform& Transform) override;
};
