// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Test types of the UEAssetDoc.Codec.AnimSequence.NotifyIdentity test (AssetDocumentAnimation/Private/Tests): a
// project notify with a Details property whose type has no document form (a field path), beside an ordinary one.

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "UObject/FieldPath.h"

#include "AssetDocumentAnimationSequenceNotifyIdentityTestTypes.generated.h"

UCLASS()
class UAssetDocumentSequenceFieldNotify : public UAnimNotify
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Test")
	TFieldPath<FProperty> Field;

	UPROPERTY(EditAnywhere, Category = "Test")
	float Bias = 0.f;
};
