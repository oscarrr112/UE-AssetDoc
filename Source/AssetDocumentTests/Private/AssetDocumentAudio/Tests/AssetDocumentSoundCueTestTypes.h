// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Test types of the Sound Cue tests in AssetDocumentAudio/Private/Tests: a project sound node with an instanced
// settings object (UEAssetDoc.Tolerant.SoundCue) and one with localizable texts (UEAssetDoc.Codec.SoundCue.TextLocalization),
// as a project's custom nodes may have. No engine sound node has either.

#include "CoreMinimal.h"
#include "Sound/SoundNode.h"
#include "UObject/Object.h"

#include "AssetDocumentSoundCueTestTypes.generated.h"

UCLASS(EditInlineNew)
class UAssetDocumentSoundCueTestSettings : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Settings")
	float Range = 0.f;
};

UCLASS(hidecategories = Object, EditInlineNew)
class UAssetDocumentSoundCueTestNode : public USoundNode
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Settings")
	float Gain = 1.f;

	UPROPERTY(EditAnywhere, Instanced, Category = "Settings")
	TObjectPtr<UAssetDocumentSoundCueTestSettings> Settings = nullptr;
};

UCLASS(hidecategories = Object, EditInlineNew)
class UAssetDocumentSoundCueTextTestNode : public USoundNode
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Text")
	FText Caption;

	UPROPERTY(EditAnywhere, Category = "Text")
	FText Note;
};
