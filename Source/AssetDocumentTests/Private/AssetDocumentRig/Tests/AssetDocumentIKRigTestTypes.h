// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Test types of the UEAssetDoc.Tolerant.IKRig and UEAssetDoc.Tolerant.IKRetargeter tests (AssetDocumentRig/Private/
// Tests): a project IK Rig solver and a project retarget op whose settings hold a field the document has no form for
// (a TFieldPath), as a project's custom solver or op may have. No engine solver or op has one.

#include "CoreMinimal.h"
#include "Retargeter/IKRetargetOps.h"
#include "Rig/Solvers/IKRigSolverBase.h"
#include "UObject/FieldPath.h"

#include "AssetDocumentIKRigTestTypes.generated.h"

USTRUCT()
struct FAssetDocumentTolerantTestSolverSettings : public FIKRigSolverSettingsBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Settings")
	float Alpha = 1.f;

	UPROPERTY(EditAnywhere, Category = "Settings")
	TFieldPath<FProperty> Filter;
};

USTRUCT(meta = (DisplayName = "AssetDoc Tolerant Test Solver"))
struct FAssetDocumentTolerantTestSolver : public FIKRigSolverBase
{
	GENERATED_BODY()

	UPROPERTY()
	FAssetDocumentTolerantTestSolverSettings Settings;

	virtual void Initialize(const FIKRigSkeleton& InIKRigSkeleton) override {}
	virtual void Solve(FIKRigSkeleton& InIKRigSkeleton, const FIKRigGoalContainer& InGoals) override {}
	virtual void GetRequiredBones(TSet<FName>& OutRequiredBones) const override {}
	virtual void GetRequiredGoals(TSet<FName>& OutRequiredGoals) const override {}
	virtual FIKRigSolverSettingsBase* GetSolverSettings() override { return &Settings; }
	virtual const UScriptStruct* GetSolverSettingsType() const override { return FAssetDocumentTolerantTestSolverSettings::StaticStruct(); }
	virtual void AddGoal(const UIKRigEffectorGoal& InNewGoal) override {}
	virtual void OnGoalRenamed(const FName& InOldName, const FName& InNewName) override {}
	virtual void OnGoalMovedToDifferentBone(const FName& InGoalName, const FName& InNewBoneName) override {}
	virtual void OnGoalRemoved(const FName& InGoalName) override {}
#if WITH_EDITOR
	virtual FText GetNiceName() const override { return FText::FromString(TEXT("AssetDoc Tolerant Test Solver")); }
#endif
};

USTRUCT()
struct FAssetDocumentTolerantTestOpSettings : public FIKRetargetOpSettingsBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Settings")
	float Alpha = 1.f;

	UPROPERTY(EditAnywhere, Category = "Settings")
	TFieldPath<FProperty> Filter;

	virtual void CopySettingsAtRuntime(const FIKRetargetOpSettingsBase* InSettingsToCopyFrom) override
	{
		const FAssetDocumentTolerantTestOpSettings* From = static_cast<const FAssetDocumentTolerantTestOpSettings*>(InSettingsToCopyFrom);
		Alpha = From->Alpha;
		Filter = From->Filter;
	}
};

USTRUCT(meta = (DisplayName = "AssetDoc Tolerant Test Op"))
struct FAssetDocumentTolerantTestOp : public FIKRetargetOpBase
{
	GENERATED_BODY()

	UPROPERTY()
	FAssetDocumentTolerantTestOpSettings Settings;

	virtual FIKRetargetOpSettingsBase* GetSettings() override { return &Settings; }
	virtual const UScriptStruct* GetSettingsType() const override { return FAssetDocumentTolerantTestOpSettings::StaticStruct(); }
	virtual const UScriptStruct* GetType() const override { return FAssetDocumentTolerantTestOp::StaticStruct(); }
};
