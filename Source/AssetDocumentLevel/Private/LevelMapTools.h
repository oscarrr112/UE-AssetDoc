// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "LevelToolResult.h"

// open_level (POST /assetdoc/level/open) and new_level (POST /assetdoc/level/new): the
// editor's File > Open Level and File > New Level for an MCP client. Game thread only.
//
// Neither discards work: when the editor world has unsaved changes (its map, sublevel,
// external actor or actor folder packages are dirty) the request is refused with
// Level.UnsavedChangesWouldBeLost listing them, unless it passes "save_first": true, which
// saves exactly those packages first (the Save of the editor's "Save Content" prompt). A
// world that was never saved (Untitled) and has changes cannot be saved by path here; that
// is refused either way. Changing the map clears the Undo history, as in the editor.
//
// open_level {"path": "/Game/Maps/Room" (package or object path), "save_first"?: bool}
// new_level  {"path": "/Game/Maps/NewRoom", "template"?: "empty" (default) | "empty_open_world" |
//             a New Level dialog template ("basic", "open_world", ...), "save_first"?: bool}:
//             the new map is created from the template and saved at path, then open.
// Payload: {level, template?, already_open?, saved_before[], world: <query_level what:"world">}.
namespace UE::AssetDocumentLevel::Private
{
FLevelToolResult RunOpenLevel(const TSharedPtr<FJsonObject>& Request);
FLevelToolResult RunOpenLevelFromBody(const FString& RequestBody);

FLevelToolResult RunNewLevel(const TSharedPtr<FJsonObject>& Request);
FLevelToolResult RunNewLevelFromBody(const FString& RequestBody);
}
