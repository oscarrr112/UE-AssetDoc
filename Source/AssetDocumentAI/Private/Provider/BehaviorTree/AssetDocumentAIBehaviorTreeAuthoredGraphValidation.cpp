// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredGraphValidation.h"
#include "AIGraphNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeEditorTypes.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_SimpleParallel.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"

namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback
{
namespace AIBehaviorTreeAuthoredGraphValidationLocal
{
struct FMainGraphConnection
{
	const UEdGraphPin* Output;
	const UEdGraphPin* Input;
	FString Path;
};

struct FInventory
{
	EAuthoredGraphReadbackVersion Version = EAuthoredGraphReadbackVersion::V1;
	TSet<FGuid> Guids;
	TSet<const UEdGraphNode*> Seen;
	FString Identity;
	FString Diagnostic = TEXT("AIProvider.BehaviorTree.GraphReadbackTopologyRejected");
	FAuthoredGraphValidationFailure Failure;

	bool MainFault(EAuthoredGraphValidationFault Fault, const FString& Path)
	{
		if (Version == EAuthoredGraphReadbackVersion::V2) Failure = {Fault, Path};
		return false;
	}

	bool Object(const UObject* Value)
	{
		if (!IsValid(Value)) return false;
		Identity += FString::Printf(TEXT("O:%p:%p:%s:%d;"), Value, Value->GetOuter(),
			*Value->GetPathName(), Value->GetOutermost()->IsDirty());
		return true;
	}
	bool Guid(const FGuid& Value)
	{
		if (!Value.IsValid() || Guids.Contains(Value)) return false;
		Guids.Add(Value);
		Identity += Value.ToString(EGuidFormats::Digits);
		return true;
	}
	bool Node(const UEdGraphNode* Value, const UEdGraph& Graph)
	{
		if (!Object(Value) || Value->GetGraph() != &Graph || Seen.Contains(Value)
			|| !Guid(Value->NodeGuid)) return false;
		if (Seen.Num() >= 4096) { Diagnostic = TEXT("AIProvider.BehaviorTree.GraphReadbackProjectionOverflow"); return false; }
		Seen.Add(Value);
		Identity += FString::Printf(TEXT("P:%d:%d;"), Value->NodePosX, Value->NodePosY);
		for (const UEdGraphPin* Pin : Value->Pins)
		{
			if (!Pin || Pin->GetOwningNode() != Value) return false;
			Identity += FString::Printf(TEXT("pin:%p:%d;"), Pin, static_cast<int32>(Pin->Direction));
			Identity += Pin->PinId.ToString(EGuidFormats::Digits) + Pin->PinType.PinCategory.ToString()
				+ Pin->PinType.PinSubCategory.ToString();
			Identity += FString::Printf(TEXT("type:%p:%d;"), Pin->PinType.PinSubCategoryObject.Get(), static_cast<int32>(Pin->PinType.ContainerType));
			TSet<const UEdGraphPin*> Links;
			for (const UEdGraphPin* Link : Pin->LinkedTo)
			{
				if (!Link || Links.Contains(Link)) return false;
				Links.Add(Link);
				Identity += FString::Printf(TEXT("link:%p;"), Link);
			}
		}
		return true;
	}
	bool Links(const UEdGraphNode& Node, const TSet<const UEdGraphNode*>& Members,
		bool bCheckInputCardinality = true)
	{
		TSet<const UEdGraphPin*> Pins;
		TSet<FGuid> PinIds;
		for (const UEdGraphPin* Pin : Node.Pins)
		{
			if (!Pin || !Pin->PinId.IsValid() || PinIds.Contains(Pin->PinId) || Pins.Contains(Pin) || Pin->GetOwningNode() != &Node
				|| (Pin->Direction != EGPD_Input && Pin->Direction != EGPD_Output)) return false;
			Pins.Add(Pin);
			PinIds.Add(Pin->PinId);
			if (bCheckInputCardinality && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 1) return false;
			for (const UEdGraphPin* Link : Pin->LinkedTo)
			{
				if (!Link || !Members.Contains(Link->GetOwningNode()) || Link->Direction == Pin->Direction
					|| !Link->GetOwningNode()->Pins.Contains(Link) || !Link->LinkedTo.Contains(Pin)) return false;
			}
		}
		return true;
	}
	bool Reach(const UEdGraphNode& Node, TSet<const UEdGraphNode*>& Visited,
		EEdGraphPinDirection Direction, int32 Depth)
	{
		if (Depth > 64)
		{
			Diagnostic = TEXT("AIProvider.BehaviorTree.GraphReadbackProjectionOverflow");
			return false;
		}
		if (Visited.Contains(&Node)) return false;
		Visited.Add(&Node);
		for (const UEdGraphPin* Pin : Node.Pins)
			if (Pin->Direction == Direction)
				for (const UEdGraphPin* Link : Pin->LinkedTo)
					if (!Reach(*Link->GetOwningNode(), Visited, Direction, Depth + 1)) return false;
		return true;
	}
	bool ReachMainV2(const UEdGraphNode& Node, const FString& Path,
		TSet<const UEdGraphNode*>& Visited, TSet<const UEdGraphNode*>& Active,
		TArray<FMainGraphConnection>& Connections, int32 Depth)
	{
		if (Depth > 64)
		{
			Diagnostic = TEXT("AIProvider.BehaviorTree.GraphReadbackProjectionOverflow");
			return false;
		}
		if (Active.Contains(&Node)) return MainFault(EAuthoredGraphValidationFault::Cycle, Path);
		if (Visited.Contains(&Node)) return false;
		Visited.Add(&Node);
		Active.Add(&Node);
		int32 ChildIndex = 0;
		// Match projection order: stock output-pin order (Task then Out for
		// SimpleParallel), with each output's links sorted by node X.
		for (const UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin->Direction != EGPD_Output) continue;
			TArray<UEdGraphPin*> SortedLinks = Pin->LinkedTo;
			SortedLinks.StableSort(FCompareNodeXLocation());
			for (const UEdGraphPin* Link : SortedLinks)
			{
				const FString ChildPath = FString::Printf(TEXT("%s/Children/%d"), *Path, ChildIndex++);
				if (!ReachMainV2(*Link->GetOwningNode(), ChildPath, Visited, Active, Connections, Depth + 1)) return false;
				Connections.Add({Pin, Link, ChildPath});
			}
		}
		Active.Remove(&Node);
		return true;
	}
	bool MainTreeV2(const UBehaviorTreeGraphNode_Root& Root, const UBehaviorTreeGraph& Graph,
		const TSet<const UEdGraphNode*>& MainNodes)
	{
		// First prove pointer/ownership/reciprocity safety. Defer cardinality
		// until the active walk can distinguish a cycle from extra parents.
		for (const UEdGraphNode* N : MainNodes) if (!Links(*N, MainNodes, false)) return false;
		const UEdGraphPin* RootOutput = Root.Pins[0];
		if (RootOutput->LinkedTo.Num() > 1)
			return MainFault(EAuthoredGraphValidationFault::MultipleRoots, TEXT("/Body/Tree/Root"));
		TSet<const UEdGraphNode*> Visited, Active;
		Visited.Add(&Root);
		TArray<FMainGraphConnection> Connections;
		if (!RootOutput->LinkedTo.IsEmpty())
		{
			const UEdGraphPin* Input = RootOutput->LinkedTo[0];
			if (!ReachMainV2(*Input->GetOwningNode(), TEXT("/Body/Tree/Root"), Visited, Active, Connections, 1)) return false;
			Connections.Add({RootOutput, Input, TEXT("/Body/Tree/Root")});
		}
		if (Visited.Num() != MainNodes.Num())
			return MainFault(EAuthoredGraphValidationFault::Unreachable, TEXT("/Body/Tree/Nodes"));
		for (const UEdGraphNode* N : MainNodes) if (!Links(*N, MainNodes)) return false;
		if (Connections.IsEmpty()) return true;
		const UEdGraphSchema* Schema = Graph.GetSchema();
		if (!Schema) return false;
		for (const FMainGraphConnection& Connection : Connections)
		{
			// Existing legal edges may return BREAK_OTHERS_*; query only,
			// never ask the schema to create or replace a live connection.
			if (Schema->CanCreateConnection(Connection.Output, Connection.Input).Response == CONNECT_RESPONSE_DISALLOW)
				return MainFault(EAuthoredGraphValidationFault::InvalidConnection, Connection.Path);
		}
		return true;
	}
	bool Bound(const UEdGraph& Graph, const UObject& Owner)
	{
		if (!Object(&Graph) || Graph.GetOuter() != &Owner || !Guid(Graph.GraphGuid)) return false;
		TSet<const UEdGraphNode*> Members;
		const UEdGraphNode* Sink = nullptr;
		for (const UEdGraphNode* N : Graph.Nodes)
		{
			if (!Node(N, Graph)) return false;
			Members.Add(N);
			if (Version == EAuthoredGraphReadbackVersion::V2 && Cast<UEdGraphNode_Comment>(N))
			{
				if (!N->Pins.IsEmpty()) return false;
				continue;
			}
			int32 Inputs = 0, Outputs = 0;
			for (const UEdGraphPin* Pin : N->Pins)
			{
				if (Pin->PinType.PinCategory != TEXT("Transition")) return false;
				if (Pin->Direction == EGPD_Input) ++Inputs;
				else if (Pin->Direction == EGPD_Output) ++Outputs;
				else return false;
			}
			if (const auto* Logic = Cast<UBehaviorTreeDecoratorGraphNode_Logic>(N))
			{
				if (Logic->LogicMode == EDecoratorLogicMode::Sink)
				{
					if (Sink) return false;
					Sink = N;
				}
				else if (Logic->LogicMode != EDecoratorLogicMode::And
					&& Logic->LogicMode != EDecoratorLogicMode::Or
					&& Logic->LogicMode != EDecoratorLogicMode::Not) return false;
				const bool bVariadic = Logic->LogicMode == EDecoratorLogicMode::And || Logic->LogicMode == EDecoratorLogicMode::Or;
				if ((bVariadic ? Inputs < 2 : Inputs != 1)
					|| Outputs != (Logic->LogicMode == EDecoratorLogicMode::Sink ? 0 : 1)) return false;
				Identity += FString::Printf(TEXT("logic:%d;"), static_cast<int32>(Logic->LogicMode));
			}
			else if (const auto* Test = Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(N))
			{
				if (Inputs != 0 || Outputs != 1 || !Cast<UBTDecorator>(Test->NodeInstance) || !Object(Test->NodeInstance)) return false;
			}
			else { Diagnostic = TEXT("AIProvider.BehaviorTree.GraphReadbackUnsupportedNode"); return false; }
		}
		if (!Sink) return false;
		for (const UEdGraphNode* N : Members) if (!Links(*N, Members)) return false;
		// Bound DTOs retain disconnected authored nodes. Validate every component,
		// without requiring the editor's unfinished expression to reach the sink.
		TMap<const UEdGraphNode*, int32> Incoming;
		for (const UEdGraphNode* N : Members) Incoming.Add(N, 0);
		for (const UEdGraphNode* N : Members)
			for (const UEdGraphPin* Pin : N->Pins)
			{
				if (Pin->Direction != EGPD_Output) continue;
				if (Pin->LinkedTo.Num() > 1) return false;
				for (const UEdGraphPin* Link : Pin->LinkedTo) ++Incoming.FindChecked(Link->GetOwningNode());
			}
		TArray<const UEdGraphNode*> Ready;
		for (const auto& Row : Incoming) if (Row.Value == 0) Ready.Add(Row.Key);
		int32 Processed = 0;
		while (!Ready.IsEmpty())
		{
			const UEdGraphNode* N = Ready.Pop(EAllowShrinking::No);
			++Processed;
			for (const UEdGraphPin* Pin : N->Pins)
				if (Pin->Direction == EGPD_Output)
					for (const UEdGraphPin* Link : Pin->LinkedTo)
						if (--Incoming.FindChecked(Link->GetOwningNode()) == 0) Ready.Add(Link->GetOwningNode());
		}
		if (Processed != Members.Num()) return false;
		return true;
	}
	bool Subnode(const UBehaviorTreeGraphNode* N, const UBehaviorTreeGraphNode& Parent,
		const UEdGraph& Graph, bool bDecorator)
	{
		if (!IsValid(N) || (N->ParentNode && N->ParentNode != &Parent)) return false;
		if (!N->ParentNode)
		{
			// ParentNode is a stock transient cache, rebuilt by UpdateAsset, not
			// PostLoad. A fresh read establishes ownership from persisted arrays
			// without rebuilding or writing that cache on the observed graph.
			if (N->GetOuter() != &Graph || N->GetGraph() != &Graph) return false;
			int32 SubnodeReferences = 0, TypedReferences = 0;
			for (const auto& Sub : Parent.SubNodes) if (Sub == N) ++SubnodeReferences;
			for (const auto& Sub : (bDecorator ? Parent.Decorators : Parent.Services))
				if (Sub == N) ++TypedReferences;
			if (SubnodeReferences != 1 || TypedReferences != 1
				|| (bDecorator ? Parent.Services.Contains(N) : Parent.Decorators.Contains(N))) return false;
			for (const UEdGraphNode* Candidate : Graph.Nodes)
			{
				const auto* Other = Cast<UBehaviorTreeGraphNode>(Candidate);
				if (Other && Other != &Parent && (Other->SubNodes.Contains(N)
					|| Other->Decorators.Contains(N) || Other->Services.Contains(N))) return false;
			}
		}
		if (N->bInjectedNode)
		{
			// Stock subtree imports are derived display nodes; preserve identity
			// evidence without admitting them into authored GUID/property inventory.
			if (!Object(N) || !N->Pins.IsEmpty()) return false;
			Identity += N->NodeGuid.ToString(EGuidFormats::Digits);
			if (const auto* Composite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(N))
			{
				if (!Object(Composite->BoundGraph)) return false;
				Identity += Composite->BoundGraph->GraphGuid.ToString(EGuidFormats::Digits);
				for (const UEdGraphNode* BoundNode : Composite->BoundGraph->Nodes)
				{
					if (!Object(BoundNode)) return false;
					Identity += BoundNode->NodeGuid.ToString(EGuidFormats::Digits);
				}
				return true;
			}
			return Object(N->NodeInstance);
		}
		if (!Node(N, Graph) || !N->Pins.IsEmpty()
			|| !N->SubNodes.IsEmpty() || !N->Decorators.IsEmpty() || !N->Services.IsEmpty()) return false;
		if (bDecorator)
		{
			if (const auto* Composite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(N))
				return Composite->BoundGraph && Bound(*Composite->BoundGraph, *Composite);
			return Cast<UBehaviorTreeGraphNode_Decorator>(N) && Cast<UBTDecorator>(N->NodeInstance)
				&& Object(N->NodeInstance);
		}
		return Cast<UBehaviorTreeGraphNode_Service>(N) && Cast<UBTService>(N->NodeInstance)
			&& Object(N->NodeInstance);
	}
	bool Main(const UBehaviorTreeGraphNode& N, const UEdGraph& Graph)
	{
		if (!Node(&N, Graph) || N.ParentNode) return false;
		const bool bRoot = Cast<UBehaviorTreeGraphNode_Root>(&N) != nullptr;
		const bool bTask = Cast<UBehaviorTreeGraphNode_Task>(&N) != nullptr;
		const int32 ExpectedPins = bRoot || bTask ? 1 : (Cast<UBehaviorTreeGraphNode_SimpleParallel>(&N) ? 3 : 2);
		if (N.Pins.Num() != ExpectedPins) return false;
		for (int32 Index = 0; Index < N.Pins.Num(); ++Index)
		{
			const EEdGraphPinDirection Direction = Index == 0 && !bRoot ? EGPD_Input : EGPD_Output;
			if (N.Pins[Index]->Direction != Direction) return false;
		}
		if (!Cast<UBehaviorTreeGraphNode_Root>(&N))
		{
			if (!((Cast<UBehaviorTreeGraphNode_Composite>(&N) && Cast<UBTCompositeNode>(N.NodeInstance))
				|| (Cast<UBehaviorTreeGraphNode_Task>(&N) && Cast<UBTTaskNode>(N.NodeInstance))))
			{ Diagnostic = TEXT("AIProvider.BehaviorTree.GraphReadbackUnsupportedNode"); return false; }
			if (!Object(N.NodeInstance)) return false;
		}
		TSet<const UAIGraphNode*> Subs;
		for (const auto& D : N.Decorators) { if (!Subnode(D, N, Graph, true)) return false; Subs.Add(D); }
		for (const auto& S : N.Services) { if (!Subnode(S, N, Graph, false)) return false; Subs.Add(S); }
		if (N.SubNodes.Num() != Subs.Num()) return false;
		for (const auto& S : N.SubNodes)
		{
			if (!Subs.Remove(S)) return false;
			Identity += FString::Printf(TEXT("sub:%p;"), S.Get());
		}
		return true;
	}
	bool Tree(const UBehaviorTree& Tree, const UBehaviorTreeGraph* ExplicitScratchGraph = nullptr)
	{
		if (!Object(&Tree)) return false;
		if (ExplicitScratchGraph && (Tree.BTGraph != nullptr || ExplicitScratchGraph->GetOuter() != &Tree)) return false;
		const auto* Graph = ExplicitScratchGraph ? ExplicitScratchGraph : Cast<UBehaviorTreeGraph>(Tree.BTGraph);
		Identity += FString::Printf(TEXT("graph:%p;bb:%p;"), ExplicitScratchGraph ? ExplicitScratchGraph : Tree.BTGraph.Get(), Tree.BlackboardAsset.Get());
		if (!Tree.BTGraph && !ExplicitScratchGraph) return true;
		if (!Graph || !Object(Graph) || Graph->GetOuter() != &Tree || !Guid(Graph->GraphGuid))
		{ Diagnostic = TEXT("AIProvider.BehaviorTree.GraphReadbackInvalid"); return false; }
		const UBehaviorTreeGraphNode_Root* Root = nullptr;
		TSet<const UEdGraphNode*> MainNodes;
		for (const UEdGraphNode* N : Graph->Nodes)
		{
			if (Cast<UEdGraphNode_Comment>(N)) { if (!Node(N, *Graph) || !N->Pins.IsEmpty()) return false; continue; }
			const auto* BT = Cast<UBehaviorTreeGraphNode>(N);
			if (!BT) { Diagnostic = TEXT("AIProvider.BehaviorTree.GraphReadbackUnsupportedNode"); return false; }
			if (!Main(*BT, *Graph)) return false;
			MainNodes.Add(BT);
			if (const auto* R = Cast<UBehaviorTreeGraphNode_Root>(BT))
			{
				if (Root) return MainFault(EAuthoredGraphValidationFault::MultipleRoots, TEXT("/Body/Tree/Root"));
				Root = R;
			}
		}
		if (!Root) return false;
		if (Version == EAuthoredGraphReadbackVersion::V2) return MainTreeV2(*Root, *Graph, MainNodes);
		for (const UEdGraphNode* N : MainNodes) if (!Links(*N, MainNodes)) return false;
		TSet<const UEdGraphNode*> Visited;
		return Reach(*Root, Visited, EGPD_Output, 0) && Visited.Num() == MainNodes.Num();
	}
};
}
bool CaptureGraphIdentity(const UBehaviorTree& Tree, FString& OutDigest, FString& OutDiagnostic,
	EAuthoredGraphReadbackVersion Version, FAuthoredGraphValidationFailure* OutFailure)
{
	using namespace AIBehaviorTreeAuthoredGraphValidationLocal;
	if (OutFailure) *OutFailure = {};
	FInventory Inventory;
	Inventory.Version = Version;
	if (!Inventory.Tree(Tree))
	{
		OutDiagnostic = Inventory.Diagnostic;
		if (OutFailure) *OutFailure = MoveTemp(Inventory.Failure);
		return false;
	}
	uint8 Bytes[FSHA1::DigestSize];
	FSHA1::HashBuffer(*Inventory.Identity, Inventory.Identity.Len() * sizeof(TCHAR), Bytes);
	OutDigest = BytesToHex(Bytes, FSHA1::DigestSize);
	return true;
}
bool CaptureGraphIdentity(const UBehaviorTree& ScratchTree, const UBehaviorTreeGraph& ScratchGraph,
	FString& OutDigest, FString& OutDiagnostic, EAuthoredGraphReadbackVersion Version)
{
	using namespace AIBehaviorTreeAuthoredGraphValidationLocal;
	FInventory Inventory;
	Inventory.Version = Version;
	if (!Inventory.Tree(ScratchTree, &ScratchGraph)) { OutDiagnostic = Inventory.Diagnostic; return false; }
	uint8 Bytes[FSHA1::DigestSize];
	FSHA1::HashBuffer(*Inventory.Identity, Inventory.Identity.Len() * sizeof(TCHAR), Bytes);
	OutDigest = BytesToHex(Bytes, FSHA1::DigestSize);
	return true;
}
}
