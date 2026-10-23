// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;

namespace UE::AssetDocument::Graph
{
// The size the graph editor draws the node at zoom 1: the desired size of the node's SGraphNode widget
// (FNodeFactory::CreateNodeWidget, as SGraphPanel::AddNode makes it) after a Slate prepass. Used to lay
// nodes out without overlaps. False when Slate is not running (a commandlet), the node has no widget, or its
// widget is not one that can be measured outside a graph panel (only Blueprint (K2) node widgets are: others,
// such as an Anim Blueprint state's, read their owner panel while they lay out); callers then estimate.
bool MeasureGraphNodeWidget(UEdGraphNode& Node, FVector2D& OutSize);
} // namespace UE::AssetDocument::Graph
