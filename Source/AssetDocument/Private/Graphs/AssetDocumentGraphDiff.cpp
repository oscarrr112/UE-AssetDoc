// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Graphs/AssetDocumentGraphDiff.h"

#include "AssetDocumentJsonRegionUtils.h"
#include "Graphs/AssetDocumentGraphDefinitionResolver.h"
#include "Graphs/AssetDocumentGraphParser.h"
#include "Utils/AssetDocumentJsonKeys.h"

namespace
{
FString LinkPathToken(const FAssetDocumentLinkSpec& Link)
{
	return FString::Printf(
		TEXT("%s:%s->%s:%s"),
		*Link.From.Node,
		*Link.From.Pin,
		*Link.To.Node,
		*Link.To.Pin);
}

void AddEntry(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& Path,
	const FString& Status,
	const TSharedPtr<FJsonValue>& Desired,
	const TSharedPtr<FJsonValue>& Current,
	const FString& Message = FString())
{
	FAssetDocumentGraphDiffEntry Entry;
	Entry.Path = Path;
	Entry.Status = Status;
	Entry.Desired = FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Desired);
	Entry.Current = FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Current);
	Entry.Message = Message;
	Entries.Add(MoveTemp(Entry));
}

void AddComparisonEntry(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& Path,
	const TSharedPtr<FJsonValue>& Desired,
	const TSharedPtr<FJsonValue>& Current)
{
	AddEntry(
		Entries,
		Path,
		AssetDocumentGraphJson::AreJsonValuesEqual(Desired, Current) ? TEXT("unchanged") : TEXT("changed"),
		Desired,
		Current);
}

TMap<FString, const FAssetDocumentGraphSpec*> MapGraphsByName(const TArray<FAssetDocumentGraphSpec>& Graphs)
{
	TMap<FString, const FAssetDocumentGraphSpec*> GraphMap;
	for (const FAssetDocumentGraphSpec& Graph : Graphs)
	{
		GraphMap.Add(Graph.Name, &Graph);
	}
	return GraphMap;
}

TMap<FString, const FAssetDocumentGraphSpec*> MapGraphsById(const TArray<FAssetDocumentGraphSpec>& Graphs)
{
	TMap<FString, const FAssetDocumentGraphSpec*> GraphMap;
	for (const FAssetDocumentGraphSpec& Graph : Graphs)
	{
		GraphMap.Add(Graph.Id, &Graph);
	}
	return GraphMap;
}

TMap<FString, const FAssetDocumentNodeSpec*> MapNodesById(const FAssetDocumentGraphSpec& Graph)
{
	TMap<FString, const FAssetDocumentNodeSpec*> NodeMap;
	for (const FAssetDocumentNodeSpec& Node : Graph.Nodes)
	{
		NodeMap.Add(Node.Id, &Node);
	}
	return NodeMap;
}

TMap<FString, const FAssetDocumentPinOverrideSpec*> MapPinsById(const FAssetDocumentNodeSpec& Node)
{
	TMap<FString, const FAssetDocumentPinOverrideSpec*> PinMap;
	for (const FAssetDocumentPinOverrideSpec& Pin : Node.PinOverrides)
	{
		PinMap.Add(Pin.ToSemanticKey(), &Pin);
	}
	return PinMap;
}

TMap<FString, const FAssetDocumentLinkSpec*> MapLinksByPathToken(const FAssetDocumentGraphSpec& Graph)
{
	TMap<FString, const FAssetDocumentLinkSpec*> LinkMap;
	for (const FAssetDocumentLinkSpec& Link : Graph.Links)
	{
		LinkMap.Add(LinkPathToken(Link), &Link);
	}
	return LinkMap;
}

TArray<FString> SortedUnionKeys(const TArray<FString>& LeftKeys, const TArray<FString>& RightKeys)
{
	TSet<FString> KeySet;
	for (const FString& Key : LeftKeys)
	{
		KeySet.Add(Key);
	}
	for (const FString& Key : RightKeys)
	{
		KeySet.Add(Key);
	}

	TArray<FString> Keys = KeySet.Array();
	Keys.Sort();
	return Keys;
}

TArray<FString> SortedJsonObjectKeys(const TSharedPtr<FJsonObject>& Object)
{
	TArray<FString> Keys;
	if (Object.IsValid())
	{
		AssetDocumentServerJson::GetKeys(*Object, Keys);
		Keys.Sort();
	}
	return Keys;
}

template <typename ValueType>
TArray<FString> SortedMapKeys(const TMap<FString, ValueType>& Map)
{
	TArray<FString> Keys;
	Map.GetKeys(Keys);
	Keys.Sort();
	return Keys;
}

TSharedPtr<FJsonValue> MakeJsonObjectValue(const TSharedPtr<FJsonObject>& Object)
{
	const TSharedPtr<FJsonObject> Clone = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Object);
	if (!Clone.IsValid())
	{
		return nullptr;
	}
	return MakeShared<FJsonValueObject>(Clone.ToSharedRef());
}

TSharedRef<FJsonObject> MakeAuthoredNodeJson(const FAssetDocumentNodeSpec& Node)
{
	TSharedRef<FJsonObject> Object = Node.ToJsonObject();
	Object->RemoveField(TEXT("Evidence"));
	return Object;
}

FAssetDocumentNodeSpec AssetDocumentGraphDiffMakeCurrentNodeComparable(
	const FAssetDocumentNodeSpec& DesiredNode,
	const FAssetDocumentNodeSpec& CurrentNode)
{
	FAssetDocumentNodeSpec Comparable = CurrentNode;
	if (DesiredNode.NodeGuid.IsEmpty())
	{
		Comparable.NodeGuid.Reset();
	}
	if (!DesiredNode.CommentBubblePinned.IsSet()
		&& Comparable.CommentBubblePinned.IsSet()
		&& !Comparable.CommentBubblePinned.GetValue())
	{
		Comparable.CommentBubblePinned.Reset();
	}
	if (!DesiredNode.CommentBubbleVisible.IsSet()
		&& Comparable.CommentBubbleVisible.IsSet()
		&& !Comparable.CommentBubbleVisible.GetValue())
	{
		Comparable.CommentBubbleVisible.Reset();
	}
	return Comparable;
}

FAssetDocumentGraphSpec AssetDocumentGraphDiffMakeCurrentGraphComparable(
	const FAssetDocumentGraphSpec& DesiredGraph,
	const FAssetDocumentGraphSpec& CurrentGraph)
{
	FAssetDocumentGraphSpec Comparable = CurrentGraph;
	if (DesiredGraph.GraphGuid.IsEmpty())
	{
		Comparable.GraphGuid.Reset();
	}

	const TMap<FString, const FAssetDocumentNodeSpec*> DesiredNodes = MapNodesById(DesiredGraph);
	for (FAssetDocumentNodeSpec& CurrentNode : Comparable.Nodes)
	{
		if (const FAssetDocumentNodeSpec* const* DesiredNode = DesiredNodes.Find(CurrentNode.Id))
		{
			CurrentNode = AssetDocumentGraphDiffMakeCurrentNodeComparable(**DesiredNode, CurrentNode);
		}
	}

	const TMap<FString, const FAssetDocumentGraphSpec*> DesiredSubgraphs = MapGraphsById(DesiredGraph.Subgraphs);
	for (FAssetDocumentGraphSpec& CurrentSubgraph : Comparable.Subgraphs)
	{
		if (const FAssetDocumentGraphSpec* const* DesiredSubgraph = DesiredSubgraphs.Find(CurrentSubgraph.Id))
		{
			CurrentSubgraph = AssetDocumentGraphDiffMakeCurrentGraphComparable(**DesiredSubgraph, CurrentSubgraph);
		}
	}
	return Comparable;
}

void RemoveGraphEvidence(const TSharedRef<FJsonObject>& Graph)
{
	Graph->RemoveField(TEXT("Evidence"));

	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (Graph->TryGetArrayField(TEXT("Nodes"), Nodes) && Nodes)
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
		{
			if (NodeValue.IsValid() && NodeValue->Type == EJson::Object && NodeValue->AsObject().IsValid())
			{
				NodeValue->AsObject()->RemoveField(TEXT("Evidence"));
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Subgraphs = nullptr;
	if (Graph->TryGetArrayField(TEXT("Subgraphs"), Subgraphs) && Subgraphs)
	{
		for (const TSharedPtr<FJsonValue>& SubgraphValue : *Subgraphs)
		{
			if (SubgraphValue.IsValid() && SubgraphValue->Type == EJson::Object && SubgraphValue->AsObject().IsValid())
			{
				RemoveGraphEvidence(SubgraphValue->AsObject().ToSharedRef());
			}
		}
	}
}

TSharedRef<FJsonObject> MakeAuthoredGraphJson(const FAssetDocumentGraphSpec& Graph)
{
	TSharedRef<FJsonObject> Object = Graph.ToJsonObject();
	RemoveGraphEvidence(Object);
	return Object;
}

void CompareObjectAsWhole(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& Path,
	const TSharedPtr<FJsonObject>& DesiredObject,
	const TSharedPtr<FJsonObject>& CurrentObject,
	const FString& Message = FString())
{
	if (!DesiredObject.IsValid() && !CurrentObject.IsValid())
	{
		return;
	}

	const FString Status = !DesiredObject.IsValid()
		? TEXT("extra")
		: !CurrentObject.IsValid()
			? TEXT("missing")
			: AssetDocumentGraphJson::AreJsonObjectsEqual(DesiredObject, CurrentObject)
				? TEXT("unchanged")
				: TEXT("changed");
	AddEntry(Entries, Path, Status, MakeJsonObjectValue(DesiredObject), MakeJsonObjectValue(CurrentObject), Message);
}

void CompareObjectFields(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& ObjectPath,
	const TSharedPtr<FJsonObject>& DesiredObject,
	const TSharedPtr<FJsonObject>& CurrentObject)
{
	if (!DesiredObject.IsValid() || !CurrentObject.IsValid())
	{
		CompareObjectAsWhole(Entries, ObjectPath, DesiredObject, CurrentObject);
		return;
	}

	const TArray<FString> Keys = SortedUnionKeys(SortedJsonObjectKeys(DesiredObject), SortedJsonObjectKeys(CurrentObject));
	for (const FString& Key : Keys)
	{
		const TSharedPtr<FJsonValue>* DesiredValue = AssetDocumentServerJson::FindField(*DesiredObject, Key);
		const TSharedPtr<FJsonValue>* CurrentValue = AssetDocumentServerJson::FindField(*CurrentObject, Key);
		const FString FieldPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(ObjectPath, Key);
		if (!DesiredValue)
		{
			AddEntry(Entries, FieldPath, TEXT("extra"), nullptr, FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(*CurrentValue));
		}
		else if (!CurrentValue)
		{
			AddEntry(Entries, FieldPath, TEXT("missing"), FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(*DesiredValue), nullptr);
		}
		else
		{
			AddComparisonEntry(Entries, FieldPath, *DesiredValue, *CurrentValue);
		}
	}
}

void CompareOptionalValue(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& Path,
	const TSharedPtr<FJsonValue>& Desired,
	const TSharedPtr<FJsonValue>& Current)
{
	if (!Desired.IsValid() && !Current.IsValid())
	{
		return;
	}
	AddComparisonEntry(Entries, Path, Desired, Current);
}

void ComparePins(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& NodePath,
	const FAssetDocumentNodeSpec& DesiredNode,
	const FAssetDocumentNodeSpec& CurrentNode)
{
	const TMap<FString, const FAssetDocumentPinOverrideSpec*> DesiredPins = MapPinsById(DesiredNode);
	const TMap<FString, const FAssetDocumentPinOverrideSpec*> CurrentPins = MapPinsById(CurrentNode);
	const TArray<FString> PinKeys = SortedUnionKeys(SortedMapKeys(DesiredPins), SortedMapKeys(CurrentPins));

	for (const FString& PinKey : PinKeys)
	{
		const FAssetDocumentPinOverrideSpec* const* DesiredPin = DesiredPins.Find(PinKey);
		const FAssetDocumentPinOverrideSpec* const* CurrentPin = CurrentPins.Find(PinKey);
		const FString PinPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("PinOverrides")), PinKey);
		if (!DesiredPin)
		{
			AddEntry(
				Entries,
				PinPath,
				TEXT("extra"),
				nullptr,
				MakeShared<FJsonValueObject>((*CurrentPin)->ToJsonObject()));
		}
		else if (!CurrentPin)
		{
			AddEntry(
				Entries,
				PinPath,
				TEXT("missing"),
				MakeShared<FJsonValueObject>((*DesiredPin)->ToJsonObject()),
				nullptr);
		}
		else
		{
			AddComparisonEntry(
				Entries,
				PinPath,
				MakeShared<FJsonValueObject>((*DesiredPin)->ToJsonObject()),
				MakeShared<FJsonValueObject>((*CurrentPin)->ToJsonObject()));
		}
	}
}

void CompareNodeDetails(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& NodePath,
	const FAssetDocumentNodeSpec& DesiredNode,
	const FAssetDocumentNodeSpec& CurrentNode)
{
	AddComparisonEntry(
		Entries,
		NodePath,
		MakeShared<FJsonValueObject>(MakeAuthoredNodeJson(DesiredNode)),
		MakeShared<FJsonValueObject>(MakeAuthoredNodeJson(AssetDocumentGraphDiffMakeCurrentNodeComparable(DesiredNode, CurrentNode))));
	CompareObjectFields(Entries, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("Fields")), DesiredNode.Fields, CurrentNode.Fields);
	CompareObjectFields(Entries, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("Pins")), DesiredNode.Pins, CurrentNode.Pins);
	CompareObjectAsWhole(
		Entries,
		FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("Position")),
		DesiredNode.Position,
		CurrentNode.Position,
		TEXT("layout"));
	CompareObjectFields(Entries, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("Spawner")), DesiredNode.Spawner, CurrentNode.Spawner);
	CompareObjectFields(Entries, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("SubgraphRefs")), DesiredNode.SubgraphRefs, CurrentNode.SubgraphRefs);
}

void CompareLinks(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& GraphPath,
	const FAssetDocumentGraphSpec& DesiredGraph,
	const FAssetDocumentGraphSpec& CurrentGraph)
{
	const TMap<FString, const FAssetDocumentLinkSpec*> DesiredLinks = MapLinksByPathToken(DesiredGraph);
	const TMap<FString, const FAssetDocumentLinkSpec*> CurrentLinks = MapLinksByPathToken(CurrentGraph);
	const TArray<FString> LinkKeys = SortedUnionKeys(SortedMapKeys(DesiredLinks), SortedMapKeys(CurrentLinks));

	for (const FString& LinkKey : LinkKeys)
	{
		const FAssetDocumentLinkSpec* const* DesiredLink = DesiredLinks.Find(LinkKey);
		const FAssetDocumentLinkSpec* const* CurrentLink = CurrentLinks.Find(LinkKey);
		const FString LinkPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Links")), LinkKey);
		if (!DesiredLink)
		{
			AddEntry(
				Entries,
				LinkPath,
				TEXT("extra"),
				nullptr,
				MakeShared<FJsonValueObject>((*CurrentLink)->ToJsonObject()));
		}
		else if (!CurrentLink)
		{
			AddEntry(
				Entries,
				LinkPath,
				TEXT("missing"),
				MakeShared<FJsonValueObject>((*DesiredLink)->ToJsonObject()),
				nullptr);
		}
		else
		{
			AddComparisonEntry(
				Entries,
				LinkPath,
				MakeShared<FJsonValueObject>((*DesiredLink)->ToJsonObject()),
				MakeShared<FJsonValueObject>((*CurrentLink)->ToJsonObject()));
		}
	}
}

void CompareNodes(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& GraphPath,
	const FAssetDocumentGraphSpec& DesiredGraph,
	const FAssetDocumentGraphSpec& CurrentGraph)
{
	const TMap<FString, const FAssetDocumentNodeSpec*> DesiredNodes = MapNodesById(DesiredGraph);
	const TMap<FString, const FAssetDocumentNodeSpec*> CurrentNodes = MapNodesById(CurrentGraph);
	const TArray<FString> NodeKeys = SortedUnionKeys(SortedMapKeys(DesiredNodes), SortedMapKeys(CurrentNodes));

	for (const FString& NodeKey : NodeKeys)
	{
		const FAssetDocumentNodeSpec* const* DesiredNode = DesiredNodes.Find(NodeKey);
		const FAssetDocumentNodeSpec* const* CurrentNode = CurrentNodes.Find(NodeKey);
		const FString NodePath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Nodes")), NodeKey);
		if (!DesiredNode)
		{
			AddEntry(
				Entries,
				NodePath,
				TEXT("extra"),
				nullptr,
				MakeShared<FJsonValueObject>(MakeAuthoredNodeJson(**CurrentNode)));
		}
		else if (!CurrentNode)
		{
			AddEntry(
				Entries,
				NodePath,
				TEXT("missing"),
				MakeShared<FJsonValueObject>(MakeAuthoredNodeJson(**DesiredNode)),
				nullptr);
		}
		else
		{
			CompareNodeDetails(Entries, NodePath, **DesiredNode, **CurrentNode);
			ComparePins(Entries, NodePath, **DesiredNode, **CurrentNode);
		}
	}
}

void CompareGraphListById(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& GraphsPath,
	const TArray<FAssetDocumentGraphSpec>& DesiredGraphs,
	const TArray<FAssetDocumentGraphSpec>& CurrentGraphs);

void CompareGraphDetails(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& GraphPath,
	const FAssetDocumentGraphSpec& DesiredGraph,
	const FAssetDocumentGraphSpec& CurrentGraph)
{
	AddComparisonEntry(
		Entries,
		GraphPath,
		MakeShared<FJsonValueObject>(MakeAuthoredGraphJson(DesiredGraph)),
		MakeShared<FJsonValueObject>(MakeAuthoredGraphJson(AssetDocumentGraphDiffMakeCurrentGraphComparable(DesiredGraph, CurrentGraph))));
	CompareObjectFields(Entries, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Owner")), DesiredGraph.Owner, CurrentGraph.Owner);
	if (DesiredGraph.OwnerNodeId != CurrentGraph.OwnerNodeId)
	{
		AddComparisonEntry(
			Entries,
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("OwnerNodeId")),
			MakeShared<FJsonValueString>(DesiredGraph.OwnerNodeId),
			MakeShared<FJsonValueString>(CurrentGraph.OwnerNodeId));
	}
	if (DesiredGraph.OwnerPin != CurrentGraph.OwnerPin)
	{
		AddComparisonEntry(
			Entries,
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("OwnerPin")),
			MakeShared<FJsonValueString>(DesiredGraph.OwnerPin),
			MakeShared<FJsonValueString>(CurrentGraph.OwnerPin));
	}
	CompareOptionalValue(Entries, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("EntryPins")), DesiredGraph.EntryPins, CurrentGraph.EntryPins);
	CompareOptionalValue(Entries, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("ResultPins")), DesiredGraph.ResultPins, CurrentGraph.ResultPins);
	CompareObjectAsWhole(
		Entries,
		FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Position")),
		DesiredGraph.Position,
		CurrentGraph.Position,
		TEXT("layout"));
	CompareOptionalValue(Entries, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Metadata")), DesiredGraph.Metadata, CurrentGraph.Metadata);
	CompareNodes(Entries, GraphPath, DesiredGraph, CurrentGraph);
	CompareLinks(Entries, GraphPath, DesiredGraph, CurrentGraph);
	CompareGraphListById(
		Entries,
		FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Subgraphs")),
		DesiredGraph.Subgraphs,
		CurrentGraph.Subgraphs);
}

void CompareGraphListById(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& GraphsPath,
	const TArray<FAssetDocumentGraphSpec>& DesiredGraphs,
	const TArray<FAssetDocumentGraphSpec>& CurrentGraphs)
{
	const TMap<FString, const FAssetDocumentGraphSpec*> DesiredGraphMap = MapGraphsById(DesiredGraphs);
	const TMap<FString, const FAssetDocumentGraphSpec*> CurrentGraphMap = MapGraphsById(CurrentGraphs);
	const TArray<FString> GraphKeys = SortedUnionKeys(SortedMapKeys(DesiredGraphMap), SortedMapKeys(CurrentGraphMap));

	for (const FString& GraphKey : GraphKeys)
	{
		const FAssetDocumentGraphSpec* const* DesiredGraph = DesiredGraphMap.Find(GraphKey);
		const FAssetDocumentGraphSpec* const* CurrentGraph = CurrentGraphMap.Find(GraphKey);
		const FString GraphPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphsPath, GraphKey);
		if (!DesiredGraph)
		{
			AddEntry(
				Entries,
				GraphPath,
				TEXT("extra"),
				nullptr,
				MakeShared<FJsonValueObject>(MakeAuthoredGraphJson(**CurrentGraph)));
		}
		else if (!CurrentGraph)
		{
			AddEntry(
				Entries,
				GraphPath,
				TEXT("missing"),
				MakeShared<FJsonValueObject>(MakeAuthoredGraphJson(**DesiredGraph)),
				nullptr);
		}
		else
		{
			CompareGraphDetails(Entries, GraphPath, **DesiredGraph, **CurrentGraph);
		}
	}
}

void AddResolveDiagnosticsAsUnsupported(
	TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const TArray<FAssetDocumentGraphDiagnostic>& Diagnostics)
{
	for (const FAssetDocumentGraphDiagnostic& Diagnostic : Diagnostics)
	{
		Entries.Add(FAssetDocumentGraphDiff::MakeUnsupported(
			Diagnostic.Path,
			FString::Printf(TEXT("%s: %s"), *Diagnostic.Code, *Diagnostic.Message)));
	}
}

FAssetDocumentGraphGuidCompletionResult MakeGuidCompletionFailure(
	const FString& Code,
	const FString& Path,
	const FString& Message)
{
	FAssetDocumentGraphGuidCompletionResult Result;
	Result.Code = Code;
	Result.Path = Path;
	Result.Message = Message;
	return Result;
}

bool TryGetObjectId(const TSharedPtr<FJsonValue>& Value, FString& OutId)
{
	OutId.Reset();
	const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
	return Object.IsValid() && Object->TryGetStringField(TEXT("Id"), OutId);
}

TSharedPtr<FJsonObject> FindObjectById(
	const TArray<TSharedPtr<FJsonValue>>& Values,
	const FString& Id)

{
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		FString CandidateId;
		if (TryGetObjectId(Value, CandidateId) && CandidateId == Id)
		{
			return Value->AsObject();
		}
	}
	return nullptr;
}

bool CompleteGuidField(
	const TSharedPtr<FJsonObject>& Authored,
	const TSharedPtr<FJsonObject>& Fresh,
	const TCHAR* FieldName,
	const TCHAR* Kind,
	const FString& Path,
	FAssetDocumentGraphGuidCompletionResult& OutResult)
{
	FString AuthoredValue;
	Authored->TryGetStringField(FieldName, AuthoredValue);
	FString FreshValue;
	Fresh->TryGetStringField(FieldName, FreshValue);
	const FString FieldPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, FieldName);
	if (AuthoredValue.IsEmpty())
	{
		if (FreshValue.IsEmpty())
		{
			OutResult = MakeGuidCompletionFailure(
				FString::Printf(TEXT("MissingFresh%sGuid"), Kind), FieldPath, TEXT("Fresh evidence GUID is required"));
			return false;
		}
		Authored->SetStringField(FieldName, FreshValue);
		return true;
	}
	if (AuthoredValue != FreshValue)
	{
		OutResult = MakeGuidCompletionFailure(
			FString::Printf(TEXT("%sGuidMismatch"), Kind), FieldPath, TEXT("Explicit authored GUID differs from fresh evidence"));
		return false;
	}
	return true;
}

bool CompleteNodePair(
	const FString& NodeId,
	const TMap<FString, const FAssetDocumentNodeSpec*>& FreshNodes,
	const TArray<TSharedPtr<FJsonValue>>& AuthoredValues,
	const TArray<TSharedPtr<FJsonValue>>& FreshValues,
	const FString& GraphPath,
	FAssetDocumentGraphGuidCompletionResult& OutResult)
{
	const FString NodePath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(
		FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Nodes")), NodeId);
	if (!FreshNodes.Contains(NodeId))
	{
		OutResult = MakeGuidCompletionFailure(TEXT("NodeStableIdMismatch"), NodePath, TEXT("Fresh evidence node stable-ID set differs"));
		return false;
	}
	return CompleteGuidField(
		FindObjectById(AuthoredValues, NodeId),
		FindObjectById(FreshValues, NodeId),
		TEXT("NodeGuid"), TEXT("Node"), NodePath, OutResult);
}

bool CompleteNodePairs(
	const FAssetDocumentGraphSpec& AuthoredGraph,
	const FAssetDocumentGraphSpec& FreshGraph,
	const TSharedPtr<FJsonObject>& AuthoredJson,
	const TSharedPtr<FJsonObject>& FreshJson,
	const FString& GraphPath,
	FAssetDocumentGraphGuidCompletionResult& OutResult)
{
	const TMap<FString, const FAssetDocumentNodeSpec*> AuthoredNodes = MapNodesById(AuthoredGraph);
	const TMap<FString, const FAssetDocumentNodeSpec*> FreshNodes = MapNodesById(FreshGraph);
	if (AuthoredNodes.Num() != FreshNodes.Num())
	{
		OutResult = MakeGuidCompletionFailure(TEXT("NodeStableIdMismatch"), GraphPath, TEXT("Fresh evidence node stable-ID set differs"));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& AuthoredValues = AuthoredJson->GetArrayField(TEXT("Nodes"));
	const TArray<TSharedPtr<FJsonValue>>& FreshValues = FreshJson->GetArrayField(TEXT("Nodes"));
	for (const TPair<FString, const FAssetDocumentNodeSpec*>& Pair : AuthoredNodes)
	{
		if (!CompleteNodePair(Pair.Key, FreshNodes, AuthoredValues, FreshValues, GraphPath, OutResult))
		{
			return false;
		}
	}
	return true;
}

bool CompleteGraphPairs(
	const TArray<FAssetDocumentGraphSpec>& AuthoredGraphs,
	const TArray<FAssetDocumentGraphSpec>& FreshGraphs,
	const TArray<TSharedPtr<FJsonValue>>& AuthoredValues,
	const TArray<TSharedPtr<FJsonValue>>& FreshValues,
	const FString& GraphsPath,
	FAssetDocumentGraphGuidCompletionResult& OutResult);

bool CompleteGraphPair(
	const FAssetDocumentGraphSpec& AuthoredGraph,
	const FAssetDocumentGraphSpec& FreshGraph,
	const TSharedPtr<FJsonObject>& AuthoredJson,
	const TSharedPtr<FJsonObject>& FreshJson,
	const FString& GraphPath,
	FAssetDocumentGraphGuidCompletionResult& OutResult)
{
	if (!CompleteGuidField(AuthoredJson, FreshJson, TEXT("GraphGuid"), TEXT("Graph"), GraphPath, OutResult)
		|| !CompleteNodePairs(AuthoredGraph, FreshGraph, AuthoredJson, FreshJson, GraphPath, OutResult))
	{
		return false;
	}
	return CompleteGraphPairs(
		AuthoredGraph.Subgraphs,
		FreshGraph.Subgraphs,
		AuthoredJson->GetArrayField(TEXT("Subgraphs")),
		FreshJson->GetArrayField(TEXT("Subgraphs")),
		FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Subgraphs")),
		OutResult);
}

bool CompleteGraphPairs(
	const TArray<FAssetDocumentGraphSpec>& AuthoredGraphs,
	const TArray<FAssetDocumentGraphSpec>& FreshGraphs,
	const TArray<TSharedPtr<FJsonValue>>& AuthoredValues,
	const TArray<TSharedPtr<FJsonValue>>& FreshValues,
	const FString& GraphsPath,
	FAssetDocumentGraphGuidCompletionResult& OutResult)
{
	const TMap<FString, const FAssetDocumentGraphSpec*> AuthoredMap = MapGraphsById(AuthoredGraphs);
	const TMap<FString, const FAssetDocumentGraphSpec*> FreshMap = MapGraphsById(FreshGraphs);
	if (AuthoredMap.Num() != FreshMap.Num())
	{
		OutResult = MakeGuidCompletionFailure(TEXT("GraphStableIdMismatch"), GraphsPath, TEXT("Fresh evidence graph stable-ID set differs"));
		return false;
	}
	for (const TPair<FString, const FAssetDocumentGraphSpec*>& Pair : AuthoredMap)
	{
		const FAssetDocumentGraphSpec* const* FreshGraph = FreshMap.Find(Pair.Key);
		const FString GraphPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphsPath, Pair.Key);
		if (!FreshGraph)
		{
			OutResult = MakeGuidCompletionFailure(TEXT("GraphStableIdMismatch"), GraphPath, TEXT("Fresh evidence graph stable-ID set differs"));
			return false;
		}
		if (!CompleteGraphPair(*Pair.Value, **FreshGraph, FindObjectById(AuthoredValues, Pair.Key), FindObjectById(FreshValues, Pair.Key), GraphPath, OutResult))
		{
			return false;
		}
	}
	return true;
}

bool ValidateGraphRegionObjects(
	const TSharedPtr<FJsonValue>& AuthoredRegion,
	const TSharedPtr<FJsonValue>& FreshRegion,
	const FString& RegionPath,
	FAssetDocumentGraphGuidCompletionResult& OutResult)
{
	if (!AuthoredRegion.IsValid() || AuthoredRegion->Type != EJson::Object
		|| !FreshRegion.IsValid() || FreshRegion->Type != EJson::Object)
	{
		OutResult = MakeGuidCompletionFailure(
			TEXT("InvalidGraphRegion"), RegionPath, TEXT("Authored and fresh graph regions must be objects"));
		return false;
	}
	return true;
}

bool ParseGraphRegionForCompletion(
	const TSharedPtr<FJsonValue>& Region,
	const FString& RegionPath,
	FAssetDocumentGraphParseResult& OutParse,
	FAssetDocumentGraphGuidCompletionResult& OutResult)
{
	FAssetDocumentGraphParseOptions ParseOptions;
	ParseOptions.Path = RegionPath;
	OutParse = FAssetDocumentGraphParser::ParseGraphRegion(Region->AsObject().ToSharedRef(), ParseOptions);
	if (OutParse.IsValid())
	{
		return true;
	}
	const FAssetDocumentGraphDiagnostic& Diagnostic = OutParse.Diagnostics[0];
	OutResult = MakeGuidCompletionFailure(Diagnostic.Code, Diagnostic.Path, Diagnostic.Message);
	return false;
}
}

TSharedRef<FJsonObject> FAssetDocumentGraphDiffEntry::ToJsonObject() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Path"), Path);
	Object->SetStringField(TEXT("Status"), Status);
	if (!Message.IsEmpty())
	{
		Object->SetStringField(TEXT("Message"), Message);
	}
	if (Desired.IsValid())
	{
		Object->SetField(TEXT("Desired"), FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Desired));
	}
	if (Current.IsValid())
	{
		Object->SetField(TEXT("Current"), FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Current));
	}
	return Object;
}

TSharedRef<FJsonObject> FAssetDocumentGraphDiffEntry::ToBodyDiffEntryJsonObject() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("path"), Path);
	Object->SetStringField(TEXT("status"), Status);
	if (!Message.IsEmpty())
	{
		Object->SetStringField(TEXT("message"), Message);
	}
	Object->SetField(
		TEXT("current"),
		Current.IsValid()
			? FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Current)
			: MakeShared<FJsonValueNull>());
	Object->SetField(
		TEXT("desired"),
		Desired.IsValid()
			? FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Desired)
			: MakeShared<FJsonValueNull>());
	return Object;
}

TArray<FAssetDocumentGraphDiffEntry> FAssetDocumentGraphDiff::CompareUbergraphPages(
	const TArray<FAssetDocumentGraphSpec>& DesiredGraphs,
	const TArray<FAssetDocumentGraphSpec>& CurrentGraphs,
	const TSharedPtr<FJsonObject>& Definitions)
{
	FAssetDocumentGraphDefinitionResolveOptions ResolveOptions;
	ResolveOptions.GraphsPath = TEXT("/Body/UbergraphPages");
	const FAssetDocumentGraphDefinitionResolveResult DesiredResolveResult =
		FAssetDocumentGraphDefinitionResolver::ResolveGraphArray(DesiredGraphs, Definitions, ResolveOptions);
	const FAssetDocumentGraphDefinitionResolveResult CurrentResolveResult =
		FAssetDocumentGraphDefinitionResolver::ResolveGraphArray(CurrentGraphs, Definitions, ResolveOptions);

	TArray<FAssetDocumentGraphDiffEntry> Entries;
	AddResolveDiagnosticsAsUnsupported(Entries, DesiredResolveResult.Diagnostics);
	AddResolveDiagnosticsAsUnsupported(Entries, CurrentResolveResult.Diagnostics);
	if (!DesiredResolveResult.IsValid() || !CurrentResolveResult.IsValid())
	{
		return Entries;
	}

	const TMap<FString, const FAssetDocumentGraphSpec*> DesiredGraphMap = MapGraphsByName(DesiredResolveResult.Graphs);
	const TMap<FString, const FAssetDocumentGraphSpec*> CurrentGraphMap = MapGraphsByName(CurrentResolveResult.Graphs);
	const TArray<FString> GraphKeys = SortedUnionKeys(SortedMapKeys(DesiredGraphMap), SortedMapKeys(CurrentGraphMap));

	for (const FString& GraphKey : GraphKeys)
	{
		const FAssetDocumentGraphSpec* const* DesiredGraph = DesiredGraphMap.Find(GraphKey);
		const FAssetDocumentGraphSpec* const* CurrentGraph = CurrentGraphMap.Find(GraphKey);
		const FString GraphPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(TEXT("/Body/UbergraphPages"), GraphKey);
		if (!DesiredGraph)
		{
			AddEntry(
				Entries,
				GraphPath,
				TEXT("extra"),
				nullptr,
				MakeShared<FJsonValueObject>(MakeAuthoredGraphJson(**CurrentGraph)));
		}
		else if (!CurrentGraph)
		{
			AddEntry(
				Entries,
				GraphPath,
				TEXT("missing"),
				MakeShared<FJsonValueObject>(MakeAuthoredGraphJson(**DesiredGraph)),
				nullptr);
		}
		else
		{
			AddComparisonEntry(
				Entries,
				GraphPath,
				MakeShared<FJsonValueObject>(MakeAuthoredGraphJson(**DesiredGraph)),
				MakeShared<FJsonValueObject>(MakeAuthoredGraphJson(AssetDocumentGraphDiffMakeCurrentGraphComparable(**DesiredGraph, **CurrentGraph))));
			CompareNodes(Entries, GraphPath, **DesiredGraph, **CurrentGraph);
			CompareLinks(Entries, GraphPath, **DesiredGraph, **CurrentGraph);
		}
	}

	return Entries;
}

TArray<FAssetDocumentGraphDiffEntry> FAssetDocumentGraphDiff::CompareGraphRegion(
	const TArray<FAssetDocumentGraphSpec>& DesiredGraphs,
	const TArray<FAssetDocumentGraphSpec>& CurrentGraphs,
	const FString& RegionPath)
{
	TArray<FAssetDocumentGraphDiffEntry> Entries;
	CompareGraphListById(Entries, FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(RegionPath, TEXT("Graphs")), DesiredGraphs, CurrentGraphs);
	return Entries;
}

FAssetDocumentGraphGuidCompletionResult FAssetDocumentGraphDiff::CompleteMissingGuids(
	const TSharedPtr<FJsonValue>& AuthoredRegion,
	const TSharedPtr<FJsonValue>& FreshRegion,
	const FString& RegionPath)
{
	FAssetDocumentGraphGuidCompletionResult Result;
	if (!ValidateGraphRegionObjects(AuthoredRegion, FreshRegion, RegionPath, Result))
	{
		return Result;
	}
	FAssetDocumentGraphParseResult AuthoredParse;
	if (!ParseGraphRegionForCompletion(AuthoredRegion, RegionPath, AuthoredParse, Result))
	{
		return Result;
	}
	FAssetDocumentGraphParseResult FreshParse;
	if (!ParseGraphRegionForCompletion(FreshRegion, RegionPath, FreshParse, Result))
	{
		return Result;
	}
	const TSharedPtr<FJsonValue> Clone = FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(AuthoredRegion);
	if (!CompleteGraphPairs(AuthoredParse.Graphs, FreshParse.Graphs, Clone->AsObject()->GetArrayField(TEXT("Graphs")), FreshRegion->AsObject()->GetArrayField(TEXT("Graphs")), FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(RegionPath, TEXT("Graphs")), Result))
	{
		return Result;
	}
	Result.bSuccess = true;
	Result.CompletedRegion = Clone;
	return Result;
}

FAssetDocumentGraphDiffEntry FAssetDocumentGraphDiff::MakeUnsupported(const FString& Path, const FString& Message)
{
	FAssetDocumentGraphDiffEntry Entry;
	Entry.Path = Path;
	Entry.Status = TEXT("unsupported");
	Entry.Message = Message;
	return Entry;
}
