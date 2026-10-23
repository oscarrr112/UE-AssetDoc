// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "LevelToolResult.h"

// Editor asset operations an MCP client otherwise scripts with execute_python: the editor's File >
// Save, Asset Actions > Reload, Open / Close of asset editors and Asset Actions > Export, without
// dialogs. Game thread only; refused during a Play-In-Editor session. "assets" are package or
// object paths (/Game/Audio/Beep or /Game/Audio/Beep.Beep); every asset is answered on its own.
//
// save_assets (POST /assetdoc/assets/save):
//   {"assets": [...], "force"?: bool}   saves those assets' packages that have unsaved changes;
//                                       force also re-saves clean ones (a resave from memory).
//   {"all_dirty": true, "only"?: "all" | "maps" | "content"}
//                                       saves every package with unsaved changes (the editor's Save
//                                       All; "maps": maps and their actor packages, "content": the
//                                       rest). A never-saved map (Untitled) has no file name and is
//                                       reported, not saved.
//   Payload: {saved[], not_saved[{package, reason}], clean[]}.
// reload_assets (POST /assetdoc/assets/reload): {"assets": [...], "force"?: bool}
//   Reloads the packages from disk (UPackageTools::ReloadPackages, no dialog). A package with
//   unsaved changes is refused unless force (which discards them); the open map is refused (use
//   open_level). Payload: {reloaded[], not_loaded[], refused[{asset, reason}]}.
// asset_editor (POST /assetdoc/assets/editor):
//   {"action": "open" | "close" | "apply_editor" | "list", "assets"?: [...], "discard_unapplied"?: bool (close)}
//   Opens or closes the asset editors of the assets, or lists the assets that have an editor open. An editor that
//   holds edits not yet applied to its asset (Material, Niagara: AssetDocumentOpenEditor.h pending edits) asks before
//   it closes: close refuses it (Assets.EditorNotClosed, naming the ways out) unless discard_unapplied (the prompt's
//   "No"); apply_editor presses the editor's own Apply. Every close is checked: an editor still open is refused.
//   Payload: {opened[] | closed[] (+ discarded_unapplied[]) | applied[], not_open[], open[], refused[{asset, reason}]}.
// export_assets (POST /assetdoc/assets/export):
//   {"assets": [...], "folder"?: "Saved/Exports" (relative to the project, or an absolute path inside
//    it), "format"?: "wav" | "png" | ..., "overwrite"?: bool}
//   Writes each asset with the editor's exporter for the format (default: wav for sounds, png for
//   textures; other classes name a format) to <folder>/<AssetName>.<format>. Payload:
//   {folder, exported[{asset, file, exporter}], refused[{asset, reason, formats?}]}.
namespace UE::AssetDocumentLevel::Private
{
ASSETDOCUMENTLEVEL_API FLevelToolResult RunSaveAssets(const TSharedPtr<FJsonObject>& Request);
ASSETDOCUMENTLEVEL_API FLevelToolResult RunReloadAssets(const TSharedPtr<FJsonObject>& Request);
ASSETDOCUMENTLEVEL_API FLevelToolResult RunAssetEditor(const TSharedPtr<FJsonObject>& Request);
ASSETDOCUMENTLEVEL_API FLevelToolResult RunExportAssets(const TSharedPtr<FJsonObject>& Request);

FLevelToolResult RunSaveAssetsFromBody(const FString& RequestBody);
FLevelToolResult RunReloadAssetsFromBody(const FString& RequestBody);
FLevelToolResult RunAssetEditorFromBody(const FString& RequestBody);
FLevelToolResult RunExportAssetsFromBody(const FString& RequestBody);
}
