// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "LevelToolResult.h"

class UObject;

// import_assets (POST /assetdoc/import): the Content Browser's Import (and drag and
// drop) and Reimport for an MCP client, unattended. Game thread only.
//
// Import runs IAssetTools::ImportAssetTasks as an automated task, as an editor script does:
// Interchange with the project's default pipeline stack for the file (Project Settings >
// Interchange), or the legacy factory the editor uses for formats Interchange does not
// translate (WAV while Interchange audio import is off). The import dialog's options are the
// stack's pipeline properties: "options" sets the common ones by friendly name, "pipeline" any
// property by name ({"MeshPipeline": {"bCombineStaticMeshes": true}}); the pipelines are
// generated, edited and passed as override pipelines, as the dialog passes its edited
// pipelines. Texture options (srgb, compression, normal_map) are applied to the imported
// textures afterwards, as the texture editor does. Reimport runs FReimportManager (the
// Content Browser's Reimport) with the pipelines stored in the asset, optionally from a new
// source file. Nothing shows modal UI.
//
// Request: {"action": "import" (default) | "reimport" | "options", "files": [absolute local
// paths], "destination": "/Game/...", "name"?, "options"?, "pipeline"?, "replace_existing"?,
// "assets": [asset paths] (reimport), "source"? (reimport), "save"? (default true)}.
// Result: {assets: [{path, class, status: created|updated, source, facts}], files: [{file,
// importer, stack?}], warnings: [...], saved_packages}.
namespace UE::AssetDocumentLevel::Private
{
ASSETDOCUMENTLEVEL_API FLevelToolResult RunAssetImport(const TSharedPtr<FJsonObject>& Request);
FLevelToolResult RunAssetImportFromBody(const FString& RequestBody);

// Key facts of an imported asset, as the import result lists them: inspect's facts
// (AssetDocumentServerAssetFacts: Kind, Bounds, PlayLength, Duration, ...) plus Bones, Skeleton,
// Triangles, CollisionShapes, texture Size / SRGB / Compression, Source.
TSharedRef<FJsonObject> MakeImportedAssetFacts(UObject& Asset);
}
