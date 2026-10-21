// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UObject;

// Compact facts about content an AI places or times (what the editor's viewport, asset tooltips and Details show):
// mesh bounds around the pivot, animation length / root motion / notifies, sound duration. Read-only; inspect
// (AssetDocument) and the import tool report them.
namespace AssetDocumentServerAssetFacts
{
// {"Min": [x, y, z], "Max": [...], "Size": [...], "Center": [...]} in the asset's own space, centimetres: the pivot (the
// placed actor's location) is the origin, so Min.Z = 0 means the pivot is at the bottom.
ASSETDOCUMENTSERVER_API TSharedRef<FJsonObject> DescribeBounds(const FBox& Box);

// The material slots of a StaticMesh or skinned mesh (SkeletalMesh), in slot order:
// [{"Index": 0, "SlotName": "Body", "Material": "/Game/M/MI_Body.MI_Body" | null}]. Empty for other assets.
// A mesh component's OverrideMaterials[Index] replaces the slot's Material.
ASSETDOCUMENTSERVER_API TArray<TSharedPtr<FJsonValue>> DescribeMaterialSlots(const UObject& Mesh);

// The facts of a StaticMesh, SkeletalMesh, animation (sequence, montage, composite) or sound asset: {"Kind": "StaticMesh"
// | "SkeletalMesh" | "Animation" | "Sound", ...}. Null for other assets.
//   StaticMesh: Bounds, Sockets (names), MaterialSlots (DescribeMaterialSlots), LODs.
//   SkeletalMesh: Bounds (the imported bounds), MaterialSlots.
//   Animation: PlayLength (seconds), RootMotion, NotifyCount and Notifies [{Time, Name, Duration?, Track}] (the first 32).
//   Sound: Duration (seconds) or Looping: true (indefinite), and for a Sound Wave Channels and SampleRate.
ASSETDOCUMENTSERVER_API TSharedPtr<FJsonObject> Describe(const UObject& Asset);
}
