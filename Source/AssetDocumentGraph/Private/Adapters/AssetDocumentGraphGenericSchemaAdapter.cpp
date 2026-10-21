// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Fallback schema adapter for any UEdGraphSchema without a dedicated adapter:
// actions come from UEdGraphSchema::GetGraphContextActions (what the stock
// graph editor's context menu shows) and spawn through PerformAction.

#include "Adapters/AssetDocumentGraphBuiltinAdapters.h"

#include "AssetDocumentGraphValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "Misc/Crc.h"

namespace UE::AssetDocument::Graph
{
namespace
{
struct FGenericAction
{
	FString Key;
	TSharedPtr<FEdGraphSchemaAction> Action;
	FString NodeClassPath;
	bool bStable = true;
};

FString SchemaTag(const UEdGraph& Graph)
{
	const UEdGraphSchema* Schema = Graph.GetSchema();
	FString Tag = Schema != nullptr ? Schema->GetClass()->GetName() : TEXT("Graph");
	Tag.RemoveFromStart(TEXT("EdGraphSchema_"));
	return Tag;
}

FString Crc8(const FString& Text)
{
	return FString::Printf(TEXT("%08X"), FCrc::StrCrc32(*Text));
}

void CollectActions(UEdGraph& Graph, const UEdGraphPin* FromPin, TArray<FGenericAction>& Out)
{
	Out.Reset();
	const UEdGraphSchema* Schema = Graph.GetSchema();
	if (Schema == nullptr)
	{
		return;
	}
	const FString Tag = SchemaTag(Graph);
	FGraphContextMenuBuilder Builder(&Graph);
	Builder.FromPin = FromPin;
	Schema->GetGraphContextActions(Builder);
	for (int32 Index = 0; Index < Builder.GetNumActions(); ++Index)
	{
		TSharedPtr<FEdGraphSchemaAction>& Action = Builder.GetSchemaAction(Index);
		if (!Action.IsValid())
		{
			continue;
		}
		FGenericAction& Entry = Out.AddDefaulted_GetRef();
		Entry.Action = Action;
		const FString Menu = Action->GetCategory().ToString() + TEXT("|") + Action->GetMenuDescription().ToString();
		if (Action->GetTypeId() == FEdGraphSchemaAction_NewNode::StaticGetTypeId())
		{
			const FEdGraphSchemaAction_NewNode* NewNode = static_cast<const FEdGraphSchemaAction_NewNode*>(Action.Get());
			if (NewNode->NodeTemplate != nullptr)
			{
				Entry.NodeClassPath = NewNode->NodeTemplate->GetClass()->GetPathName();
				Entry.Key = Tag + TEXT(":Node:") + Entry.NodeClassPath;
				continue;
			}
		}
		Entry.Key = Tag + TEXT(":Action:") + Action->GetTypeId().ToString() + TEXT(":") + Menu;
		Entry.bStable = false;
	}
	TMap<FString, int32> Counts;
	for (const FGenericAction& Entry : Out)
	{
		++Counts.FindOrAdd(Entry.Key);
	}
	for (FGenericAction& Entry : Out)
	{
		if (Counts.FindChecked(Entry.Key) > 1)
		{
			Entry.Key += TEXT("#") + Crc8(Entry.Action->GetCategory().ToString() + TEXT("|") + Entry.Action->GetMenuDescription().ToString());
		}
	}
	Out.StableSort([](const FGenericAction& A, const FGenericAction& B) { return A.Key < B.Key; });
}

class FGenericSchemaAdapter final : public IAssetDocumentGraphSchemaAdapter
{
public:
	virtual UClass* GetSchemaClass() const override
	{
		return UEdGraphSchema::StaticClass();
	}

	virtual FString Tag(const UEdGraph& Graph) const override
	{
		return SchemaTag(Graph);
	}

	virtual bool EnumerateActions(
		const FAssetDocumentGraphActionQuery& Query,
		TArray<FAssetDocumentGraphActionRecord>& OutActions,
		FAssetDocumentGraphError& OutError) const override
	{
		if (Query.Graph == nullptr)
		{
			OutError.Set(TEXT("Graph.GraphNotFound"), TEXT("No graph."));
			return false;
		}
		TArray<FGenericAction> Actions;
		CollectActions(*Query.Graph, Query.FromPin, Actions);
		for (const FGenericAction& Action : Actions)
		{
			FAssetDocumentGraphActionRecord& Record = OutActions.AddDefaulted_GetRef();
			Record.ActionKey = Action.Key;
			Record.DisplayName = Action.Action->GetMenuDescription().ToString();
			Record.Category = Action.Action->GetCategory().ToString();
			Record.Tooltip = Action.Action->GetTooltipDescription().ToString();
			Record.NodeClassPath = Action.NodeClassPath;
			Record.bStable = Action.bStable;
		}
		return true;
	}

	virtual bool ResolveAndSpawn(
		const FAssetDocumentGraphSpawnRequest& Request,
		FAssetDocumentGraphSpawnResult& OutResult,
		FAssetDocumentGraphError& OutError) const override
	{
		TArray<FGenericAction> Actions;
		CollectActions(*Request.Graph, Request.FromPin, Actions);
		const FGenericAction* Match = Actions.FindByPredicate(
			[&Request](const FGenericAction& Action) { return Action.Key == Request.ActionKey; });
		if (Match == nullptr)
		{
			OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("No action has key %s in graph %s."),
				*Request.ActionKey, *Request.Graph->GetName()));
			return false;
		}
		const FVector2D Location = Request.Position.Get(FVector2D::ZeroVector);
		TSet<UEdGraphNode*> Before(Request.Graph->Nodes);
		UEdGraphNode* Node = Match->Action->PerformAction(Request.Graph, Request.FromPin,
			FVector2f(static_cast<float>(Location.X), static_cast<float>(Location.Y)), /*bSelectNewNode=*/false);
		if (Node == nullptr)
		{
			OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("Action %s did not produce a node."), *Request.ActionKey));
			return false;
		}
		OutResult.Node = Node;
		OutResult.bReused = Before.Contains(Node);
		return true;
	}

	virtual void DescribeNode(const UEdGraphNode& Node, FAssetDocumentGraphNodeDescription& Out) const override
	{
		if (const UEdGraph* Graph = Node.GetGraph())
		{
			Out.ActionKey = SchemaTag(*Graph) + TEXT(":Node:") + Node.GetClass()->GetPathName();
		}
		FObjectBuilder Extras;
		if (Node.GetCanRenameNode())
		{
			Extras.AddString(TEXT("name"), Node.IsA<UEdGraphNode_Comment>()
				? Node.NodeComment : Node.GetNodeTitle(ENodeTitleType::EditableTitle).ToString());
		}
		Out.Extras = Extras.Build();
	}

	virtual bool RemoveNode(UObject& Asset, UEdGraphNode& Node, FAssetDocumentGraphError& OutError) const override
	{
		if (!Node.CanUserDeleteNode())
		{
			OutError.Set(TEXT("Graph.NodeNotDeletable"), FString::Printf(
				TEXT("Node %s (%s) cannot be deleted by the user."),
				*FormatAssetDocumentGraphGuid(Node.NodeGuid), *Node.GetClass()->GetName()));
			return false;
		}
		if (UEdGraph* Graph = Node.GetGraph())
		{
			Graph->Modify();
		}
		Node.Modify();
		Node.DestroyNode();
		return true;
	}
};
} // namespace

TSharedRef<IAssetDocumentGraphSchemaAdapter> MakeGenericSchemaAdapter()
{
	return MakeShared<FGenericSchemaAdapter>();
}
} // namespace UE::AssetDocument::Graph
