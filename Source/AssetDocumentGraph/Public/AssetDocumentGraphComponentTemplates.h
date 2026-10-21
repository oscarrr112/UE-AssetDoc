// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

namespace UE::AssetDocument::Graph
{
// Safety net after a Blueprint apply: the component templates of every loaded Blueprint (Simple
// Construction Script nodes and inherited-component overrides) must be instances of their current
// component class, never of a class a compile has since replaced (REINST_ / TRASHCLASS_, a class
// with a newer version). Each stale template is replaced the way the editor's reinstancing does
// (FBlueprintCompileReinstancer::BatchReplaceInstancesOfClass) and reported as a warning
// (Graph.DependentTemplateRepaired); one that cannot be repaired is reported as a warning
// Graph.DependentTemplateStale naming the Blueprint and the component. Dirty flags are kept.
ASSETDOCUMENTGRAPH_API void CheckComponentTemplates(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

// True when a template of this class must be reinstanced: a class a compile replaced.
ASSETDOCUMENTGRAPH_API bool IsOutdatedComponentClass(const UClass* Class);
}
