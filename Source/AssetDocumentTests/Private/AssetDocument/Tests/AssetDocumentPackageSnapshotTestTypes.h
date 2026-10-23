// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Engine/DataAsset.h"

#include "AssetDocumentPackageSnapshotTestTypes.generated.h"

UCLASS()
class UAssetDocumentPackageSnapshotTestNode : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FString Label;

	UPROPERTY()
	int32 Value = 0;

	UPROPERTY()
	TObjectPtr<UAssetDocumentPackageSnapshotTestNode> Child;
};

UCLASS()
class UAssetDocumentPackageSnapshotTestAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	FString Title;

	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	TArray<int32> Numbers;

	UPROPERTY()
	TArray<TObjectPtr<UAssetDocumentPackageSnapshotTestNode>> Nodes;

	// Not reflected, so it never takes part in snapshot serialization.
	int32 PostEditUndoCount = 0;

	virtual void PostEditUndo() override
	{
		Super::PostEditUndo();
		++PostEditUndoCount;
	}
};

// Mirrors editor classes that rebuild a subobject on every change, e.g.
// UPaperSprite::RebuildCollisionData (a new BodySetup per PostEditChange, so also
// per PostEditUndo, which calls PostEditChange).
UCLASS()
class UAssetDocumentPackageSnapshotRebuildingTestAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "AssetDocumentTest")
	FString Title;

	UPROPERTY()
	TObjectPtr<UAssetDocumentPackageSnapshotTestNode> Derived;

	void RebuildDerived()
	{
		Derived = NewObject<UAssetDocumentPackageSnapshotTestNode>(this);
		Derived->Label = Title;
	}

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override
	{
		Super::PostEditChangeProperty(Event);
		RebuildDerived();
	}
#endif
};
