// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "LevelToolResult.h"

// settings (POST /assetdoc/settings): the World Settings panel of the open level and
// the Project Settings dialog for an MCP client. Game thread only.
//
// Request: {"action": "list" | "read" (default) | "write", "section": "world" | a Project
// Settings section, "properties": [names] (read) | {name: value} (write), "diff_only"?,
// "description"?, "save"?}.
// - "world": the open level's WorldSettings actor, as the World Settings panel edits it:
//   Details-equivalent writes in one editor transaction (one Undo entry titled by
//   description; undo_editor undoes it), read back and rolled back on a mismatch; the map
//   stays dirty unless save: true.
// - A Project Settings section (the "Project" settings container: Maps & Modes, Input,
//   Engine/Plugins sections, every UDeveloperSettings), named by section name, display name,
//   "<Category>/<Section>", or settings class path / name: writes go to the section's
//   default config file the way the Project Settings editor writes them (check out or make
//   writable, then UpdateSinglePropertyInConfigFile / TryUpdateDefaultConfigFile), only
//   when that file is in the project's Config/ directory; the written keys are read back
//   from the file. Not in the Undo history (as in the editor); a failed write restores the
//   values and the file.
// - "list": the Project Settings sections with their class and config file.
namespace UE::AssetDocumentLevel::Private
{
FLevelToolResult RunEditorSettings(const TSharedPtr<FJsonObject>& Request);
FLevelToolResult RunEditorSettingsFromBody(const FString& RequestBody);

// True when Filename is an .ini file inside the project's Config/ directory (the only
// config files settings writes); OutRelative is the project-relative path. Links are followed:
// a Config/ directory, or a file or folder in it, that leads out of the project is refused.
bool IsProjectConfigFile(const FString& Filename, FString* OutRelative = nullptr);

// IsProjectConfigFile for a given project and Config/ directory.
bool IsConfigFileInProject(const FString& ProjectDir, const FString& ConfigDir, const FString& Filename,
	FString* OutRelative = nullptr);
}
