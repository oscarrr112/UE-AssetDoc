// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "AssetDocumentAnimationSequenceReferencesDocumentTestTypes.generated.h"

// Reflection types exist only to exercise the admitted reference IsA contract.
// Production has no class-name whitelist or dependency on these fixtures.
UCLASS()
class UAssetDocumentSequenceReferencesTestSkeleton : public USkeleton
{
 GENERATED_BODY()
public:
 // Stock MinimalAPI does not export these virtuals. This test subclass owns
 // its preview state and declares postload work on the Game Thread.
 USkeletalMesh* GetPreviewMesh(bool bFindIfNotSet = false) override { return FixturePreviewMesh.Get(); }
 USkeletalMesh* GetPreviewMesh() const override { return FixturePreviewMesh.Get(); }
 void SetPreviewMesh(USkeletalMesh* Mesh, bool bMarkAsDirty = true) override
 { FixturePreviewMesh = Mesh; if (bMarkAsDirty) MarkPackageDirty(); }
 bool IsPostLoadThreadSafe() const override { return false; }
private:
 UPROPERTY()
 TSoftObjectPtr<USkeletalMesh> FixturePreviewMesh;
};
UCLASS()
class UAssetDocumentSequenceReferencesTestMesh : public USkeletalMesh
{
 GENERATED_BODY()
};
UCLASS()
class UAssetDocumentSequenceReferencesTestSequence : public UAnimSequence
{
 GENERATED_BODY()
};
