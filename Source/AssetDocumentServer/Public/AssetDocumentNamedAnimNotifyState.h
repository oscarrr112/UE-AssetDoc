// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AssetDocumentNamedAnimNotifyState.generated.h"

UCLASS()
class ASSETDOCUMENTSERVER_API UAssetDocumentNamedAnimNotifyState : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	virtual FString GetNotifyName_Implementation() const override;
};
