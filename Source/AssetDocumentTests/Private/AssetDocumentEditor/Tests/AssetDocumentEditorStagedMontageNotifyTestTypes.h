// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AssetDocumentEditorStagedMontageNotifyTestTypes.generated.h"

UCLASS()
class UAssetDocumentEditorStagedNotifyTest final : public UAnimNotify
{
 GENERATED_BODY()
public:
 UPROPERTY(EditAnywhere, Category = "StagedNotifyTest")
 int32 First = 0;
 UPROPERTY(EditAnywhere, Category = "StagedNotifyTest")
 int32 Second = 0;
 bool bInvalidateAfterFirstWrite = false;
#if WITH_EDITOR
 virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
};

UCLASS()
class UAssetDocumentEditorStagedNotifyStateTest final : public UAnimNotifyState
{
 GENERATED_BODY()
public:
 UPROPERTY(EditAnywhere, Category = "StagedNotifyTest")
 int32 First = 0;
};
