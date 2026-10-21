// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class IAssetDocumentDerivedStateRebuilder;

namespace UE::AssetDocument::Graph
{
// Core package-snapshot rebuilder for exact UBlueprint, UAnimBlueprint and registered
// IAssetDocumentBlueprintRebuildAdapter Blueprint classes: reconstructs nodes, recompiles and
// restores the captured class-default values.
TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeBlueprintDerivedStateRebuilder();
// Blueprints hosted inside another asset (RegisterAssetDocumentHostedBlueprint).
TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeHostedBlueprintDerivedStateRebuilder();
} // namespace UE::AssetDocument::Graph
