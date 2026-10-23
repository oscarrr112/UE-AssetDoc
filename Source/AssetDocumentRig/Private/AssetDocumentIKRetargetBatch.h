// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentTypes.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Batch retarget export (MCP retarget_animations): the IK Retargeter editor's "Export Selected Animations" and the
// Content Browser's "Retarget Animations" window, i.e. UIKRetargetBatchOperation::RunRetarget with the window's
// options (target folder or next to the sources, prefix / suffix / search / replace, overwrite, referenced assets).
namespace UE::AssetDocument::Graph::Rig::Batch
{
// POST /assetdoc/animation/retarget. Request fields: retargeter, assets[] | folder (+ recursive), source_mesh,
// target_mesh, target_folder, prefix, suffix, search, replace, existing (error | overwrite | unique),
// include_referenced, save.
FAssetDocumentResult RetargetAnimations(const TSharedPtr<FJsonObject>& Request);

TSharedPtr<FJsonObject> HandleRetargetRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode);
}
