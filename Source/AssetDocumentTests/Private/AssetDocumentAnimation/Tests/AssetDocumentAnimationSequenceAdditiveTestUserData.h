// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Engine/AssetUserData.h"
#include "AssetDocumentAnimationSequenceAdditiveTestUserData.generated.h"

UCLASS()
class UAssetDocumentSequenceAdditiveTestUserData : public UAssetUserData
{
 GENERATED_BODY()
public:
 int32 OwnerCallbackCalls = 0;
 void PostEditChangeOwner(const FPropertyChangedEvent&) override { ++OwnerCallbackCalls; }
};
