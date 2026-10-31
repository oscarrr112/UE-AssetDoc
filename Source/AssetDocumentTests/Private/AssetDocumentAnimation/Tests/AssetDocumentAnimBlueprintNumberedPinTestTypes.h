// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Test types of the UEAssetDoc.AnimBlueprint.Adapters.NodeBindingRawEdit test (AssetDocumentAnimation/Private/Tests): an
// anim graph node whose optional pins include a plain (non-array) property named like an array element ("Weight_1")
// next to "Weight".

#include "CoreMinimal.h"
#include "AnimGraphNode_Base.h"
#include "Animation/AnimNodeBase.h"

#include "AssetDocumentAnimBlueprintNumberedPinTestTypes.generated.h"

// Outputs the reference pose; Weight and Weight_1 are optional float pins.
USTRUCT()
struct FAnimNode_AssetDocumentNumberedPin : public FAnimNode_Base
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Settings", meta = (PinHiddenByDefault))
	float Weight = 1.f;

	UPROPERTY(EditAnywhere, Category = "Settings", meta = (PinHiddenByDefault))
	float Weight_1 = 1.f;

	virtual void Evaluate_AnyThread(FPoseContext& Output) override
	{
		Output.ResetToRefPose();
	}
};

UCLASS()
class UAnimGraphNode_AssetDocumentNumberedPin : public UAnimGraphNode_Base
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Settings")
	FAnimNode_AssetDocumentNumberedPin Node;

	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override
	{
		return NSLOCTEXT("AssetDocumentTests", "NumberedPinNodeTitle", "Numbered Pin (test)");
	}
};
