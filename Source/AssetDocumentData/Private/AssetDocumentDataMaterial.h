// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class IAssetDocumentDerivedStateRebuilder;

// Material / Material Function expression graphs (AssetDocumentDataMaterialPrimitives.cpp).
namespace UE::AssetDocument::Graph::Data::Material
{
// Core package-snapshot rebuilder: after a rollback restored a Material (or Material Function) package,
// recompile it as the Material Editor's Apply does (UMaterialEditingLibrary::RecompileMaterial /
// UpdateMaterialFunction), so the shader maps and the editor parameter list match the restored graph.
TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeMaterialRebuilder();

// Open-editor apply (AssetDocumentDataMaterialOpenEditor.cpp): the Material Editor (materials and functions) is closed for
// an apply and reopened; the Material Instance Editor is refreshed in place.
void RegisterOpenEditorHandlers();
void UnregisterOpenEditorHandlers();
} // namespace UE::AssetDocument::Graph::Data::Material
