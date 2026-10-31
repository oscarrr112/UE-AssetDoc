// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The Property Access node of Animation Blueprints (K2Node_PropertyAccess, PropertyAccessNode plugin; its header is
// private, so the node is reached by reflection). Node settings:
//   "Path"       the path picked on the node, one segment per entry (["GetOwningActor", "CharacterMovement",
//                "Velocity"]; a segment can be "Array[2]"), [] for none;
//   "ContextId"  the call site picked in the node's context menu: "" (Automatic), "UnBatched_ThreadSafe",
//                "Batched_WorkerThreadPreEventGraph", ... (omitted while Automatic).
// Written as the node's path picker does (UK2Node_PropertyAccess::SetPath: the path, then a node reconstruction
// that resolves the output pin's type) and as its Call Site menu does (SetContextId).

#include "CoreMinimal.h"
#include "AssetDocumentGraphValue.h"

class UEdGraphNode;

namespace UE::AssetDocument::Graph::PropertyAccessNode
{
bool IsPropertyAccessNode(const UEdGraphNode& Node);

// Adds the node's settings to Settings (no defaults: a document that leaves one out keeps it).
void Describe(const UEdGraphNode& Node, FObjectBuilder& Settings);

// Sets "Path" or "ContextId"; bOutHandled is false for any other path.
bool SetSetting(UEdGraphNode& Node, FStringView Path, const FValue& Value, bool& bOutHandled, FAssetDocumentGraphError& OutError);
} // namespace UE::AssetDocument::Graph::PropertyAccessNode
