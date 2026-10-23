// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "LevelToolResult.h"

class UWorld;

// query_level (POST /assetdoc/level/query): read-only view of the editor world.
// Request and response: docs/architecture/level-editing-tools-design.md §2.2 and
// MCP/schemas/LevelEditing.md. Game thread only.
namespace UE::AssetDocumentLevel::Private
{
FLevelToolResult RunLevelQuery(const TSharedPtr<FJsonObject>& Request);

// Parses RequestBody (empty = {}), then RunLevelQuery.
FLevelToolResult RunLevelQueryFromBody(const FString& RequestBody);

// query_level's "world" view of World (levels, folders, layers, data layers).
TSharedRef<FJsonObject> MakeLevelWorldSummary(UWorld& World);

// The admission shared by the level tools: no play session, an editor world.
bool CheckLevelToolAdmission(TArray<FAssetDocumentDiagnostic>& OutDiagnostics);
}
