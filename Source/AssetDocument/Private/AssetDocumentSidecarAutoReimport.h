// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

struct FAutoReimportDirectoryConfig;

// Sidecar files and the editor's Auto Reimport (Editor Preferences > Loading & Saving > Auto Reimport).
//
// The editor watches the directories listed there for source files that an import factory reads; JSON is one (the
// Data Table / Curve Table importer), so a new or changed <AssetName>.assetdoc.json under Content is offered for import
// ("N changes to source content files have been detected"). With UAssetDocumentSettings::bExcludeSidecarsFromAutoReimport
// every listed directory that covers the project's Content folder carries an exclude wildcard for the sidecars and
// for the conflict files (<AssetName>.assetdoc.asset.json). Other entries and wildcards are left as they are.
namespace UE::AssetDocument::SidecarAutoReimport
{
// The exclude wildcards: "*.assetdoc.json", "*.assetdoc.asset.json".
TConstArrayView<const TCHAR*> GetWildcards();

// Whether the Auto Reimport directory covers the project's Content folder (it is that folder, inside it, or contains it).
bool CoversContent(const FAutoReimportDirectoryConfig& Directory);

// Adds (bExclude) or removes the exclude wildcards in Directories. True when anything changed.
bool UpdateDirectories(TArray<FAutoReimportDirectoryConfig>& Directories, bool bExclude);

// Brings the editor's Auto Reimport settings in line with the plugin setting. A change is made as an edit in Editor
// Preferences makes it: the settings are saved and the editor restarts its Auto Reimport watchers with them.
// True when the settings changed.
bool Sync();

// Sync at startup and whenever the plugin setting changes.
void Register();
void Unregister();
} // namespace UE::AssetDocument::SidecarAutoReimport
