// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class UPackage;

namespace AssetDocumentAssetCompilation
{
// Finishes the asynchronous builds (textures, meshes, materials ...) of the objects in Packages before the packages are
// unloaded, reloaded, deleted or detached from their files, as the editor does before it unloads a package. A texture
// whose resource was made while its data was still building can keep that stand-in after the build and read as still
// compiling, which the engine reports when it finishes the package's builds: its resource is made again from the
// built data first. Game thread only.
ASSETDOCUMENTSERVER_API void FinishPackageCompilation(TConstArrayView<UPackage*> Packages);
}
