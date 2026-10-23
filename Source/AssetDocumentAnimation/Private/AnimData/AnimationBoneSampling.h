// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentTypes.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UObject;

// Read-only animation data queries (MCP sample_animation_bones, Inspect's Motion facts): bone and socket transforms of
// an AnimSequence, AnimComposite or AnimMontage at given times, evaluated with the engine's pose evaluation
// (UAnimPoseExtensions::GetAnimPoseAtTimeIntervals, what AnimationLibrary.get_bone_poses_for_time uses).
namespace UE::AssetDocument::AnimData
{
// POST /assetdoc/animation/sample-bones. Request fields: asset, bones[], times[] | start/end/step, space
// (component | local | root), root (pose | extracted), mesh, slot (montages), speed. Never changes anything.
FAssetDocumentResult SampleBones(const TSharedPtr<FJsonObject>& Request);

// Inspect facts "Motion" of an AnimSequence: root displacement and the pelvis at the first and last frame (component
// space, root as authored). nullptr for other assets.
TSharedPtr<FJsonObject> DescribeMotion(const UObject& Asset);

TSharedPtr<FJsonObject> HandleSampleBonesRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode);
}
