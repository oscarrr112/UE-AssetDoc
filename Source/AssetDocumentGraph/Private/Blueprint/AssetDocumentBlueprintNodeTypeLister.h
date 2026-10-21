// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

namespace UE::AssetDocument::Graph
{
// list_types kind=blueprint_node (the pins of the node an action key places; AssetDocumentBlueprintNodeTypeLister.cpp).
void RegisterBlueprintNodeTypeLister();
void UnregisterBlueprintNodeTypeLister();
}
