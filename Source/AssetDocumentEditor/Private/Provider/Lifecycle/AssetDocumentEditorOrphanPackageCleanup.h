// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

// IAssetTools::CreateAsset creates the target package before it calls
// UFactory::FactoryCreateNew and leaves that package in memory when the factory
// returns null. This discards such a never-saved package the way the editor
// discards an unwanted in-memory package (reset loaders, drop Standalone/Public,
// rename it out of the way and mark it garbage) so the target name is free again
// and rollback sees no residue. Returns true when nothing is left under the name.
bool DiscardAssetDocumentEditorOrphanCreatedPackage(const FString& PackageName);

// A package under PackageName that is loaded but holds no live object, was never saved
// (not on disk) and is not dirty: what a failed load of a missing asset leaves behind
// (StaticLoadObject creates the outer package first). It holds no asset, so it is
// discarded like an orphan created package. Returns true when such a package was discarded.
bool DiscardAssetDocumentEditorEmptyUnsavedPackage(const FString& PackageName);
