// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;

// UE-editor-equivalent direct field writes, used only where no exported editor
// API performs the edit. Each mirrors
// what the stock graph editor does; covered by
// UEAssetDoc.Graph.RawEdits.*.
namespace UE::AssetDocument::Graph::RawEdits
{
// SGraphPanel / SNodePanel::MoveNodes: Modify + NodePosX/NodePosY.
void SetNodePosition(UEdGraphNode& Node, int32 X, int32 Y);
// SGraphNode comment bubble edit: Modify + OnUpdateCommentText / bCommentBubbleVisible.
void SetNodeComment(UEdGraphNode& Node, const FString& Comment);
void SetCommentBubbleVisible(UEdGraphNode& Node, bool bVisible);
// SGraphNodeResizable drag of a resize handle: Modify + ResizeNode. False (nothing changed) when the node
// cannot be resized (only comment boxes can).
bool SetNodeSize(UEdGraphNode& Node, int32 Width, int32 Height);
// The document Id (and the action key as authored) a node was last applied with, kept where the editor keeps
// per-object metadata: the package's FMetaData (editor-only data saved with the package, like the metadata the
// editor's metadata tags write). Extract writes them back, so a round trip keeps the author's Ids. Copy / paste
// makes new nodes without it. Writes only what differs; an empty Action removes the stored one. True when
// something was written.
bool SetNodeDocumentIdentity(UEdGraphNode& Node, const FString& Id, const FString& Action);
FString GetNodeDocumentId(const UEdGraphNode& Node);
FString GetNodeDocumentAction(const UEdGraphNode& Node);
// UEdGraphPin::ConvertConnectedGhostNodesToRealNodes (not exported): turns an
// automatically placed ghost node into a normal enabled node.
bool ConvertGhostNodeToRealNode(UEdGraphNode& Node);
} // namespace UE::AssetDocument::Graph::RawEdits
