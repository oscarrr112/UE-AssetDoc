// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphAdapters.h"
#include "AssetDocumentGraphValue.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

namespace UE::AssetDocument::Graph
{
struct FGraphEntry : FAssetDocumentGraphInfo
{
	FString SchemaTag;
	TSharedPtr<IAssetDocumentGraphSchemaAdapter> Schema;
	// The identity graph_guid reports. A graph's GraphGuid, except when another graph of the asset has the
	// same one (content that duplicated a collapsed graph keeps its GUID): later ones get a GUID derived
	// from their path. Shared by every entry of one collection.
	TSharedPtr<const TMap<const UEdGraph*, FGuid>> ReportedGuids;

	ASSETDOCUMENTGRAPH_API FGuid GuidOf(const UEdGraph* InGraph) const;
	FGuid Guid() const { return GuidOf(Graph); }
};

// Every graph of one asset (host top-level graphs + node-owned subgraphs).
struct ASSETDOCUMENTGRAPH_API FAssetGraphs
{
	UObject* Asset = nullptr;
	TSharedPtr<IAssetDocumentGraphHostAdapter> Host;
	TArray<FGraphEntry> Graphs;

	const FGraphEntry* FindEntry(const UEdGraph* Graph) const;
	UEdGraphNode* FindNode(const FGuid& NodeGuid, const FGraphEntry** OutEntry = nullptr) const;
};

ASSETDOCUMENTGRAPH_API bool CollectGraphs(UObject& Asset, FAssetGraphs& OutGraphs, FAssetDocumentGraphError& OutError);

// addr = {"graph_guid": "..."} | {"graph_path": "..."}
ASSETDOCUMENTGRAPH_API bool ResolveGraphAddress(
	const FAssetGraphs& Graphs, FReader& Address, const FGraphEntry*& OutEntry);
ASSETDOCUMENTGRAPH_API bool ResolveNodeGuid(
	const FAssetGraphs& Graphs, FReader& Args, FStringView Key, UEdGraphNode*& OutNode,
	const FGraphEntry** OutEntry = nullptr);
// pin = {"node_guid", "pin_name", "direction"?}
ASSETDOCUMENTGRAPH_API bool ResolvePinAddress(const FAssetGraphs& Graphs, FReader& Address, UEdGraphPin*& OutPin);

ASSETDOCUMENTGRAPH_API FString DirectionToString(EEdGraphPinDirection Direction);
ASSETDOCUMENTGRAPH_API bool ParseDirection(FStringView Text, EEdGraphPinDirection& OutDirection);

ASSETDOCUMENTGRAPH_API FValue MakePinTypeValue(const FEdGraphPinType& Type);
ASSETDOCUMENTGRAPH_API bool ReadPinType(FReader& Reader, FEdGraphPinType& OutType);

ASSETDOCUMENTGRAPH_API FValue MakePinRef(const UEdGraphPin& Pin);
// Oriented output -> input.
ASSETDOCUMENTGRAPH_API FValue MakeLinkValue(const UEdGraphPin& A, const UEdGraphPin& B);
// bIncludeSize: also "widget_size", the node's size as the graph editor draws it (when Slate runs).
ASSETDOCUMENTGRAPH_API FValue MakeNodeSnapshot(const FGraphEntry& Entry, UEdGraphNode& Node, bool bIncludeProperties,
	bool bIncludeSize = false);
ASSETDOCUMENTGRAPH_API FValue MakeGraphListEntry(const FGraphEntry& Entry);

ASSETDOCUMENTGRAPH_API FString ObjectPathOrEmpty(const UObject* Object);
} // namespace UE::AssetDocument::Graph
