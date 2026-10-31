// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Test types of the UEAssetDoc.Codec.AnimBlueprint.InstancedStructSetting test (AssetDocumentAnimation/Private/Tests): an
// anim graph node with an instanced struct setting (as a Chooser Player node holds its chooser), restricted to a base
// struct with one derived struct.

#include "CoreMinimal.h"
#include "AnimGraphNode_Base.h"
#include "Animation/AnimNodeBase.h"
#include "StructUtils/InstancedStruct.h"

#include "AssetDocumentAnimBlueprintInstancedSettingTestTypes.generated.h"

USTRUCT()
struct FAssetDocumentInstancedSettingBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	float Weight = 1.f;
};

USTRUCT()
struct FAssetDocumentInstancedSettingCurve : public FAssetDocumentInstancedSettingBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	FName Curve;
};

// Outputs the reference pose; Setting is a Details-only setting (no pin).
USTRUCT()
struct FAnimNode_AssetDocumentInstancedSetting : public FAnimNode_Base
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Settings", meta = (BaseStruct = "/Script/AssetDocumentTests.AssetDocumentInstancedSettingBase"))
	FInstancedStruct Setting;

	virtual void Evaluate_AnyThread(FPoseContext& Output) override
	{
		Output.ResetToRefPose();
	}
};

UCLASS()
class UAnimGraphNode_AssetDocumentInstancedSetting : public UAnimGraphNode_Base
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Settings")
	FAnimNode_AssetDocumentInstancedSetting Node;

	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override
	{
		return NSLOCTEXT("AssetDocumentTests", "InstancedSettingNodeTitle", "Instanced Setting (test)");
	}
};
