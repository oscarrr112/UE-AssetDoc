// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Graphs/K2GraphAdapter.h"

#include "Graphs/AssetDocumentGraphEncodingUtils.h"
#include "Graphs/K2NodeAdapters/CallFunctionNodeAdapter.h"
#include "Graphs/K2NodeAdapters/EventNodeAdapter.h"
#include "Graphs/K2NodeAdapters/AssetDocumentK2NodeSpawnerResolver.h"
#include "Graphs/K2NodeAdapters/SelfNodeAdapter.h"
#include "Graphs/K2NodeAdapters/VariableGetSetNodeAdapter.h"

#include "BlueprintEventNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Self.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
FString& ActiveGraphRegionPath()
{
	static FString Path(TEXT("/Body/UbergraphPages"));
	return Path;
}

FString GetGraphRegionPath(EAssetDocumentK2GraphRegion Region)
{
	switch (Region)
	{
	case EAssetDocumentK2GraphRegion::FunctionGraphs:
		return TEXT("/Body/FunctionGraphs");
	case EAssetDocumentK2GraphRegion::MacroGraphs:
		return TEXT("/Body/MacroGraphs");
	case EAssetDocumentK2GraphRegion::DelegateSignatureGraphs:
		return TEXT("/Body/DelegateSignatureGraphs");
	case EAssetDocumentK2GraphRegion::UbergraphPages:
	default:
		return TEXT("/Body/UbergraphPages");
	}
}

class FScopedGraphRegionPath
{
public:
	explicit FScopedGraphRegionPath(EAssetDocumentK2GraphRegion Region)
		: PreviousPath(ActiveGraphRegionPath())
	{
		ActiveGraphRegionPath() = GetGraphRegionPath(Region);
	}

	~FScopedGraphRegionPath()
	{
		ActiveGraphRegionPath() = PreviousPath;
	}

private:
	FString PreviousPath;
};

using FBlueprintGraphArray = TArray<TObjectPtr<UEdGraph>>;

FBlueprintGraphArray& GetMutableGraphArray(UBlueprint* Blueprint, EAssetDocumentK2GraphRegion Region)
{
	check(Blueprint);
	switch (Region)
	{
	case EAssetDocumentK2GraphRegion::FunctionGraphs:
		return Blueprint->FunctionGraphs;
	case EAssetDocumentK2GraphRegion::MacroGraphs:
		return Blueprint->MacroGraphs;
	case EAssetDocumentK2GraphRegion::DelegateSignatureGraphs:
		return Blueprint->DelegateSignatureGraphs;
	case EAssetDocumentK2GraphRegion::UbergraphPages:
	default:
		return Blueprint->UbergraphPages;
	}
}

const FBlueprintGraphArray& GetGraphArray(const UBlueprint* Blueprint, EAssetDocumentK2GraphRegion Region)
{
	check(Blueprint);
	switch (Region)
	{
	case EAssetDocumentK2GraphRegion::FunctionGraphs:
		return Blueprint->FunctionGraphs;
	case EAssetDocumentK2GraphRegion::MacroGraphs:
		return Blueprint->MacroGraphs;
	case EAssetDocumentK2GraphRegion::DelegateSignatureGraphs:
		return Blueprint->DelegateSignatureGraphs;
	case EAssetDocumentK2GraphRegion::UbergraphPages:
	default:
		return Blueprint->UbergraphPages;
	}
}

bool IsManagedK2Graph(const UEdGraph* Graph, EAssetDocumentK2GraphRegion Region)
{
	if (!Graph)
	{
		return false;
	}

	if (Region == EAssetDocumentK2GraphRegion::FunctionGraphs
		|| Region == EAssetDocumentK2GraphRegion::MacroGraphs
		|| Region == EAssetDocumentK2GraphRegion::DelegateSignatureGraphs)
	{
		UClass* AnimationGraphSchemaClass =
			FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimationGraphSchema"));
		const UClass* SchemaClass = Graph->Schema;
		return !AnimationGraphSchemaClass
			|| !SchemaClass
			|| !SchemaClass->IsChildOf(AnimationGraphSchemaClass);
	}

	return true;
}

bool IsEmptyFunctionOrMacroNoop(EAssetDocumentK2GraphRegion Region, const TArray<FAssetDocumentGraphSpec>& DesiredGraphs)
{
	return DesiredGraphs.IsEmpty()
		&& (Region == EAssetDocumentK2GraphRegion::FunctionGraphs
			|| Region == EAssetDocumentK2GraphRegion::MacroGraphs
			|| Region == EAssetDocumentK2GraphRegion::DelegateSignatureGraphs);
}

FAssetDocumentCapabilityResult K2GraphFailure(const FString& Message, const FString& Path, const FString& Code)
{
	return FAssetDocumentCapabilityResult::Failure(Message, Path, Code);
}

FString K2GraphJoinPath(const FString& Left, const FString& Right)
{
	return Left.IsEmpty() ? Right : Left / Right;
}

FString K2GraphPath(const FAssetDocumentGraphSpec& Graph)
{
	return K2GraphJoinPath(ActiveGraphRegionPath(), Graph.Name);
}

FString K2GraphNodePath(const FAssetDocumentGraphSpec& Graph, const FAssetDocumentNodeSpec& Node)
{
	return K2GraphJoinPath(K2GraphJoinPath(K2GraphPath(Graph), TEXT("Nodes")), Node.Id);
}

FString K2GraphLinkPath(const FAssetDocumentGraphSpec& Graph, const FAssetDocumentLinkSpec& Link)
{
	return FString::Printf(
		TEXT("%s/Links/%s:%s->%s:%s"),
		*K2GraphPath(Graph),
		*Link.From.Node,
		*Link.From.Pin,
		*Link.To.Node,
		*Link.To.Pin);
}

UClass* ResolveClass(const FString& ClassPath, UClass* RequiredBaseClass)
{
	UClass* Class = ClassPath.IsEmpty() ? nullptr : StaticLoadClass(RequiredBaseClass ? RequiredBaseClass : UObject::StaticClass(), nullptr, *ClassPath);
	return Class && (!RequiredBaseClass || Class->IsChildOf(RequiredBaseClass)) ? Class : nullptr;
}

FAssetDocumentCapabilityResult ConfigureNodeWithAdapter(
	UBlueprint* Blueprint,
	const IAssetDocumentNodeAdapter& Adapter,
	UEdGraphNode* Node,
	const FAssetDocumentGraphSpec& GraphSpec,
	const FAssetDocumentNodeSpec& NodeSpec)
{
	if (!Node)
	{
		return K2GraphFailure(
			FString::Printf(TEXT("Graph node '%s' could not be created"), *NodeSpec.Id),
			K2GraphNodePath(GraphSpec, NodeSpec),
			TEXT("UnresolvedGraphNodeClass"));
	}

	Node->Modify();
	Node->NodePosX = 0;
	Node->NodePosY = 0;
	if (NodeSpec.Position.IsValid())
	{
		double X = 0.0;
		double Y = 0.0;
		NodeSpec.Position->TryGetNumberField(TEXT("X"), X);
		NodeSpec.Position->TryGetNumberField(TEXT("Y"), Y);
		Node->NodePosX = static_cast<int32>(X);
		Node->NodePosY = static_cast<int32>(Y);
	}
	Node->NodeComment = NodeSpec.bHasComment ? NodeSpec.Comment : FString();

	FAssetDocumentNodeApplyContext Context;
	Context.Blueprint = Blueprint;
	Context.GraphPath = K2GraphPath(GraphSpec);
	Context.NodePath = K2GraphNodePath(GraphSpec, NodeSpec);
	const FAssetDocumentCapabilityResult AdapterResult = Adapter.ConfigureNodeForApply(Context, Node, NodeSpec);
	if (!AdapterResult.bSuccess)
	{
		return AdapterResult;
	}
	return FAssetDocumentCapabilityResult::Success();
}

FVector2D GetNodeLocation(const FAssetDocumentNodeSpec& NodeSpec)
{
	double X = 0.0;
	double Y = 0.0;
	if (NodeSpec.Position.IsValid())
	{
		NodeSpec.Position->TryGetNumberField(TEXT("X"), X);
		NodeSpec.Position->TryGetNumberField(TEXT("Y"), Y);
	}
	return FVector2D(X, Y);
}

FAssetDocumentCapabilityResult MaterializeNodeWithSpawner(
	UBlueprint* Blueprint,
	const IAssetDocumentNodeAdapter& Adapter,
	UEdGraph* Graph,
	const FAssetDocumentGraphSpec& GraphSpec,
	const FAssetDocumentNodeSpec& NodeSpec,
	TSet<UEdGraphNode*>& UsedNodes,
	UEdGraphNode*& OutNode,
	bool& bOutCreated,
	bool bRequireUEPlacementAdmission)
{
	OutNode = nullptr;
	bOutCreated = false;

	FAssetDocumentNodeApplyContext Context;
	Context.Blueprint = Blueprint;
	Context.GraphPath = K2GraphPath(GraphSpec);
	Context.NodePath = K2GraphNodePath(GraphSpec, NodeSpec);
	FAssetDocumentK2ResolvedNodeSpawner Resolved;
	const FAssetDocumentCapabilityResult ResolveResult =
		FAssetDocumentK2NodeSpawnerResolver::Resolve(
			Context,
			Graph,
			NodeSpec,
			bRequireUEPlacementAdmission,
			Resolved);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}

	IBlueprintNodeBinder::FBindingSet EmptyBindings;
	if (const UBlueprintEventNodeSpawner* EventSpawner = Cast<UBlueprintEventNodeSpawner>(Resolved.Spawner.Get()))
	{
		const UK2Node_Event* ExistingEvent = EventSpawner->FindPreExistingEvent(Blueprint, EmptyBindings);
		if (ExistingEvent && ExistingEvent->GetGraph() != Graph)
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Graph event node '%s' is already owned by a different Ubergraph page"), *NodeSpec.Id),
				Context.NodePath,
				TEXT("GraphEventOwnedByDifferentUbergraph"));
		}
	}

	TSet<UEdGraphNode*> NodesBefore;
	for (UEdGraphNode* ExistingNode : Graph->Nodes)
	{
		NodesBefore.Add(ExistingNode);
	}
	UEdGraphNode* Node = Resolved.Spawner->Invoke(Graph, EmptyBindings, GetNodeLocation(NodeSpec));
	if (!Node)
	{
		return K2GraphFailure(
			FString::Printf(TEXT("Graph node '%s' spawner did not return a node"), *NodeSpec.Id),
			Context.NodePath,
			TEXT("GraphNodeSpawnerFailed"));
	}
	if (Cast<UK2Node_FunctionEntry>(Node))
	{
		return K2GraphFailure(
			FString::Printf(TEXT("Graph event node '%s' conflicts with an existing function graph"), *NodeSpec.Id),
			Context.NodePath,
			TEXT("GraphEventConflictsWithFunction"));
	}
	if (Node->GetClass() != Resolved.ExpectedNodeClass)
	{
		return K2GraphFailure(
			FString::Printf(
				TEXT("Graph node '%s' spawner returned class '%s' instead of '%s'"),
				*NodeSpec.Id,
				*Node->GetClass()->GetPathName(),
				*Resolved.ExpectedNodeClass->GetPathName()),
			Context.NodePath,
			TEXT("GraphNodeSpawnerClassMismatch"));
	}
	if (Node->GetGraph() != Graph)
	{
		return K2GraphFailure(
			FString::Printf(TEXT("Graph node '%s' spawner returned a node owned by a different graph"), *NodeSpec.Id),
			Context.NodePath,
			TEXT("GraphNodeSpawnerOwnershipMismatch"));
	}
	if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
		CallNode && !CallNode->CanPasteHere(Graph))
	{
		return K2GraphFailure(
			FString::Printf(TEXT("Graph call function node '%s' cannot be placed in graph '%s'"), *NodeSpec.Id, *Graph->GetName()),
			Context.NodePath / TEXT("Member"),
			TEXT("UnsupportedGraphFunction"));
	}
	if (UsedNodes.Contains(Node))
	{
		return K2GraphFailure(
			FString::Printf(TEXT("Graph node '%s' resolves to the same event as another desired node"), *NodeSpec.Id),
			Context.NodePath,
			TEXT("DuplicateGraphEvent"));
	}
	if (!Adapter.DoesNodeMatchSpec(Blueprint, Node, NodeSpec))
	{
		return K2GraphFailure(
			FString::Printf(TEXT("Graph node '%s' spawner returned a node with a different semantic member"), *NodeSpec.Id),
			Context.NodePath / TEXT("Member"),
			TEXT("GraphNodeSpawnerMemberMismatch"));
	}

	UsedNodes.Add(Node);
	OutNode = Node;
	bOutCreated = !NodesBefore.Contains(Node);
	return FAssetDocumentCapabilityResult::Success();
}

FString JsonScalarToString(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		return FString();
	}
	if (Value->Type == EJson::String)
	{
		return Value->AsString();
	}
	if (Value->Type == EJson::Number)
	{
		return FString::SanitizeFloat(Value->AsNumber());
	}
	if (Value->Type == EJson::Boolean)
	{
		return Value->AsBool() ? TEXT("true") : TEXT("false");
	}
	return FString();
}

void ResetPinDefaultToBaseline(UEdGraphPin* Pin)
{
	if (!Pin
		|| Pin->Direction != EGPD_Input
		|| Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
		|| Pin->PinName == UEdGraphSchema_K2::PN_Self)
	{
		return;
	}
	Pin->DefaultValue = Pin->AutogeneratedDefaultValue;
	Pin->DefaultObject = nullptr;
	Pin->DefaultTextValue = FText::GetEmpty();
}

bool IsObjectBackedPin(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return false;
	}

	const FName& Category = Pin->PinType.PinCategory;
	return Category == UEdGraphSchema_K2::PC_Object
		|| Category == UEdGraphSchema_K2::PC_Class
		|| Category == UEdGraphSchema_K2::PC_SoftObject
		|| Category == UEdGraphSchema_K2::PC_SoftClass
		|| Category == UEdGraphSchema_K2::PC_Interface;
}

bool IsSupportedAuthoredPinDefault(const UEdGraphPin* Pin, const FAssetDocumentPinOverrideSpec& PinOverride, FString& OutReason)
{
	if (!Pin)
	{
		OutReason = TEXT("pin is missing");
		return false;
	}
	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
	{
		OutReason = TEXT("wildcard pins require node-specific type expansion before defaults can be authored safely");
		return false;
	}
	if (Pin->PinType.ContainerType != EPinContainerType::None)
	{
		OutReason = TEXT("container pin defaults are not supported by Tier 1 graph apply");
		return false;
	}
	if (PinOverride.DefaultObject.IsValid() && !IsObjectBackedPin(Pin))
	{
		OutReason = FString::Printf(
			TEXT("DefaultObject is only supported for object-backed pins, but pin '%s' has category '%s'"),
			*PinOverride.Pin,
			*Pin->PinType.PinCategory.ToString());
		return false;
	}
	if (PinOverride.DefaultTextValue.IsValid() && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Text)
	{
		OutReason = FString::Printf(
			TEXT("DefaultTextValue is only supported for text pins, but pin '%s' has category '%s'"),
			*PinOverride.Pin,
			*Pin->PinType.PinCategory.ToString());
		return false;
	}
	return true;
}

FAssetDocumentCapabilityResult ApplyPinDefaults(const FAssetDocumentGraphSpec& GraphSpec, const FAssetDocumentNodeSpec& NodeSpec, UEdGraphNode* Node)
{
	TSet<FString> AuthoredPins;
	for (const FAssetDocumentPinOverrideSpec& PinOverride : NodeSpec.PinOverrides)
	{
		AuthoredPins.Add(PinOverride.Pin);
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && !AuthoredPins.Contains(Pin->PinName.ToString()))
		{
			ResetPinDefaultToBaseline(Pin);
		}
	}

	for (const FAssetDocumentPinOverrideSpec& PinOverride : NodeSpec.PinOverrides)
	{
		UEdGraphPin* Pin = Node->FindPin(FName(*PinOverride.Pin));
		if (!Pin)
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Graph pin '%s' does not exist on node '%s' after reconstruction"), *PinOverride.Pin, *NodeSpec.Id),
				K2GraphJoinPath(K2GraphNodePath(GraphSpec, NodeSpec), TEXT("PinOverrides") / PinOverride.Pin),
				TEXT("InvalidGraphPin"));
		}
		if (Pin->Direction != EGPD_Input)
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Graph pin '%s' is not an input pin"), *PinOverride.Pin),
				K2GraphJoinPath(K2GraphNodePath(GraphSpec, NodeSpec), TEXT("PinOverrides") / PinOverride.Pin),
				TEXT("InvalidGraphPin"));
		}

		FString UnsupportedReason;
		if (!IsSupportedAuthoredPinDefault(Pin, PinOverride, UnsupportedReason))
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Graph pin '%s' default is not supported: %s"), *PinOverride.Pin, *UnsupportedReason),
				K2GraphJoinPath(K2GraphNodePath(GraphSpec, NodeSpec), TEXT("PinOverrides") / PinOverride.Pin),
				TEXT("UnsupportedGraphPinDefault"));
		}

		ResetPinDefaultToBaseline(Pin);
		if (PinOverride.DefaultValue.IsValid())
		{
			Pin->DefaultValue = JsonScalarToString(PinOverride.DefaultValue);
		}
		if (PinOverride.DefaultObject.IsValid())
		{
			const FString ObjectPath = JsonScalarToString(PinOverride.DefaultObject);
			Pin->DefaultObject = ObjectPath.IsEmpty() ? nullptr : StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
			if (!ObjectPath.IsEmpty() && !Pin->DefaultObject)
			{
				return K2GraphFailure(
					FString::Printf(TEXT("Graph pin '%s' DefaultObject '%s' could not be loaded"), *PinOverride.Pin, *ObjectPath),
					K2GraphJoinPath(K2GraphNodePath(GraphSpec, NodeSpec), TEXT("PinOverrides") / PinOverride.Pin / TEXT("DefaultObject")),
					TEXT("InvalidGraphPinDefault"));
			}
		}
		if (PinOverride.DefaultTextValue.IsValid())
		{
			Pin->DefaultTextValue = FText::FromString(JsonScalarToString(PinOverride.DefaultTextValue));
		}
	}

	return FAssetDocumentCapabilityResult::Success();
}

UEdGraphPin* FindPinBySpec(const TMap<FString, UEdGraphNode*>& NodesById, const FAssetDocumentGraphEndpoint& Endpoint)
{
	UEdGraphNode* const* Node = NodesById.Find(Endpoint.Node);
	return Node && *Node ? (*Node)->FindPin(FName(*Endpoint.Pin)) : nullptr;
}

FAssetDocumentCapabilityResult ValidateLinks(const FAssetDocumentGraphSpec& GraphSpec, const UEdGraphSchema_K2* Schema, const TMap<FString, UEdGraphNode*>& NodesById)
{
	for (const FAssetDocumentLinkSpec& Link : GraphSpec.Links)
	{
		UEdGraphPin* FromPin = FindPinBySpec(NodesById, Link.From);
		UEdGraphPin* ToPin = FindPinBySpec(NodesById, Link.To);
		if (!FromPin || !ToPin)
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Graph link endpoint '%s' could not be resolved after node reconstruction"), *Link.ToKey()),
				K2GraphLinkPath(GraphSpec, Link),
				TEXT("UnresolvedGraphLinkEndpoint"));
		}
		if (FromPin->Direction != EGPD_Output || ToPin->Direction != EGPD_Input)
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Graph link '%s' must connect output to input"), *Link.ToKey()),
				K2GraphLinkPath(GraphSpec, Link),
				TEXT("InvalidGraphLinkType"));
		}
		const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Graph schema rejected link '%s': %s"), *Link.ToKey(), *Response.Message.ToString()),
				K2GraphLinkPath(GraphSpec, Link),
				TEXT("InvalidGraphLinkType"));
		}
	}
	return FAssetDocumentCapabilityResult::Success();
}

UEdGraph* FindGraphByName(UBlueprint* Blueprint, EAssetDocumentK2GraphRegion Region, const FString& GraphName);
UEdGraph* FindOrCreateGraph(
	UBlueprint* Blueprint,
	EAssetDocumentK2GraphRegion Region,
	const FAssetDocumentGraphSpec& GraphSpec,
	UClass* SchemaClass,
	bool& bOutChanged);

FAssetDocumentCapabilityResult PreflightGraphSpec(
	UBlueprint* Blueprint,
	EAssetDocumentK2GraphRegion Region,
	const FAssetDocumentGraphSpec& GraphSpec,
	UClass*& OutSchemaClass)
{
	OutSchemaClass = ResolveClass(GraphSpec.Schema, UEdGraphSchema::StaticClass());
	if (!OutSchemaClass || !OutSchemaClass->IsChildOf(UEdGraphSchema_K2::StaticClass()))
	{
		return K2GraphFailure(
			FString::Printf(TEXT("Graph schema '%s' could not be resolved as EdGraphSchema_K2"), *GraphSpec.Schema),
			K2GraphJoinPath(K2GraphPath(GraphSpec), TEXT("Schema")),
			TEXT("InvalidGraphSchema"));
	}

	const FAssetDocumentNodeAdapterRegistry Registry = FAssetDocumentK2GraphAdapter::CreateTier1NodeAdapterRegistry();
	bool bGraphChanged = false;
	UEdGraph* PreflightGraph = FindOrCreateGraph(Blueprint, Region, GraphSpec, OutSchemaClass, bGraphChanged);
	const UEdGraphSchema_K2* Schema = PreflightGraph ? Cast<UEdGraphSchema_K2>(PreflightGraph->GetSchema()) : nullptr;
	if (!Schema)
	{
		return K2GraphFailure(TEXT("K2 graph schema could not be initialized"), K2GraphPath(GraphSpec), TEXT("InvalidGraphSchema"));
	}

	TMap<FString, UEdGraphNode*> TempNodesById;
	for (const FAssetDocumentNodeSpec& NodeSpec : GraphSpec.Nodes)
	{
		UClass* NodeClass = ResolveClass(NodeSpec.Class, UEdGraphNode::StaticClass());
		if (!NodeClass)
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Graph node class '%s' could not be loaded"), *NodeSpec.Class),
				K2GraphJoinPath(K2GraphNodePath(GraphSpec, NodeSpec), TEXT("Class")),
				TEXT("UnresolvedGraphNodeClass"));
		}
		const TSharedPtr<IAssetDocumentNodeAdapter> Adapter = Registry.FindAdapter(NodeClass);
		if (!Adapter.IsValid())
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Graph node class '%s' has no Tier 1 adapter"), *NodeSpec.Class),
				K2GraphJoinPath(K2GraphNodePath(GraphSpec, NodeSpec), TEXT("Class")),
				TEXT("UnsupportedGraphNodeClass"));
		}

		UEdGraphNode* TempNode = nullptr;
		bool bNodeCreated = false;
		TSet<UEdGraphNode*> UsedNodes;
		for (const TPair<FString, UEdGraphNode*>& Pair : TempNodesById)
		{
			UsedNodes.Add(Pair.Value);
		}
		const FAssetDocumentCapabilityResult MaterializeResult = MaterializeNodeWithSpawner(
			Blueprint,
			*Adapter,
			PreflightGraph,
			GraphSpec,
			NodeSpec,
			UsedNodes,
			TempNode,
			bNodeCreated,
			true);
		if (!MaterializeResult.bSuccess)
		{
			return MaterializeResult;
		}
		const FAssetDocumentCapabilityResult ConfigureResult = ConfigureNodeWithAdapter(Blueprint, *Adapter, TempNode, GraphSpec, NodeSpec);
		if (!ConfigureResult.bSuccess)
		{
			return ConfigureResult;
		}
		const FAssetDocumentCapabilityResult PinResult = ApplyPinDefaults(GraphSpec, NodeSpec, TempNode);
		if (!PinResult.bSuccess)
		{
			return PinResult;
		}
		TempNodesById.Add(NodeSpec.Id, TempNode);
	}

	return ValidateLinks(GraphSpec, Schema, TempNodesById);
}

UEdGraph* FindGraphByName(UBlueprint* Blueprint, EAssetDocumentK2GraphRegion Region, const FString& GraphName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	for (UEdGraph* Graph : GetMutableGraphArray(Blueprint, Region))
	{
		if (IsManagedK2Graph(Graph, Region) && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}
	return nullptr;
}

UEdGraph* FindOrCreateGraph(UBlueprint* Blueprint, EAssetDocumentK2GraphRegion Region, const FAssetDocumentGraphSpec& GraphSpec, UClass* SchemaClass, bool& bOutChanged)
{
	UEdGraph* Graph = FindGraphByName(Blueprint, Region, GraphSpec.Name);
	if (Graph)
	{
		return Graph;
	}

	Graph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		FName(*GraphSpec.Name),
		UEdGraph::StaticClass(),
		SchemaClass);
	if (Region == EAssetDocumentK2GraphRegion::UbergraphPages)
	{
		FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
	}
	else if (Region == EAssetDocumentK2GraphRegion::FunctionGraphs)
	{
		FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, false, nullptr);
		if (const UEdGraphSchema* Schema = Graph->GetSchema())
		{
			Schema->CreateDefaultNodesForGraph(*Graph);
		}
	}
	else
	{
		FBlueprintEditorUtils::AddMacroGraph(Blueprint, Graph, true, nullptr);
	}
	bOutChanged = true;
	return Graph;
}

bool NodeMatchesMemberSpec(
	const UBlueprint* Blueprint,
	const FAssetDocumentNodeAdapterRegistry& Registry,
	const UEdGraphNode* Node,
	const FAssetDocumentNodeSpec& NodeSpec)
{
	if (!Node || Node->GetClass()->GetPathName() != NodeSpec.Class)
	{
		return false;
	}

	const TSharedPtr<IAssetDocumentNodeAdapter> Adapter = Registry.FindAdapter(Node->GetClass());
	return Adapter.IsValid() && Adapter->DoesNodeMatchSpec(Blueprint, Node, NodeSpec);
}

UEdGraphNode* FindReusableNode(
	const UBlueprint* Blueprint,
	const FAssetDocumentNodeAdapterRegistry& Registry,
	UEdGraph* Graph,
	const FAssetDocumentNodeSpec& NodeSpec,
	TSet<UEdGraphNode*>& UsedNodes)
{
	if (!NodeSpec.NodeGuid.IsEmpty())
	{
		FGuid DesiredGuid;
		if (FGuid::ParseExact(NodeSpec.NodeGuid, EGuidFormats::Digits, DesiredGuid) || FGuid::Parse(NodeSpec.NodeGuid, DesiredGuid))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && !UsedNodes.Contains(Node) && Node->NodeGuid == DesiredGuid)
				{
					if (NodeMatchesMemberSpec(Blueprint, Registry, Node, NodeSpec))
					{
						UsedNodes.Add(Node);
						return Node;
					}
				}
			}
		}
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && !UsedNodes.Contains(Node) && NodeMatchesMemberSpec(Blueprint, Registry, Node, NodeSpec))
		{
			UsedNodes.Add(Node);
			return Node;
		}
	}
	return nullptr;
}

FString ExistingGraphPath(const UEdGraph* Graph)
{
	return K2GraphJoinPath(ActiveGraphRegionPath(), Graph ? Graph->GetName() : FString(TEXT("UnknownGraph")));
}

FString ExistingNodePath(const UEdGraph* Graph, const UEdGraphNode* Node)
{
	const FString NodeIdentity = Node && Node->NodeGuid.IsValid()
		? Node->NodeGuid.ToString(EGuidFormats::Digits)
		: Node ? Node->GetName() : FString(TEXT("UnknownNode"));
	return K2GraphJoinPath(K2GraphJoinPath(ExistingGraphPath(Graph), TEXT("Nodes")), NodeIdentity);
}

FAssetDocumentCapabilityResult PreflightDeleteExistingNode(
	UBlueprint* Blueprint,
	const FAssetDocumentNodeAdapterRegistry& Registry,
	const UEdGraph* Graph,
	const UEdGraphNode* Node)
{
	if (!Node)
	{
		return FAssetDocumentCapabilityResult::Success();
	}

	const TSharedPtr<IAssetDocumentNodeAdapter> Adapter = Registry.FindAdapter(Node->GetClass());
	if (!Adapter.IsValid())
	{
		return K2GraphFailure(
			FString::Printf(
				TEXT("Existing graph node class '%s' is outside Tier 1 and cannot be deleted by graph sidecar apply"),
				*FAssetDocumentGraphEncodingUtils::GetClassPath(Node->GetClass())),
			ExistingNodePath(Graph, Node),
			TEXT("UnsupportedGraphNodeClass"));
	}

	FAssetDocumentNodeApplyContext Context;
	Context.Blueprint = Blueprint;
	Context.GraphPath = ExistingGraphPath(Graph);
	Context.NodePath = ExistingNodePath(Graph, Node);
	return Adapter->CanRepresentExistingNode(Context, Node);
}

bool IsPreservedFrameworkNode(EAssetDocumentK2GraphRegion Region, const UEdGraphNode* Node);

FAssetDocumentCapabilityResult PreflightDeleteExistingGraph(
	UBlueprint* Blueprint,
	const FAssetDocumentNodeAdapterRegistry& Registry,
	EAssetDocumentK2GraphRegion Region,
	const UEdGraph* Graph)
{
	if (!Graph)
	{
		return FAssetDocumentCapabilityResult::Success();
	}

	if (!Cast<UEdGraphSchema_K2>(Graph->GetSchema()))
	{
		return K2GraphFailure(
			FString::Printf(TEXT("Existing graph '%s' does not use EdGraphSchema_K2 and cannot be deleted by Tier 1 graph sidecar apply"), *Graph->GetName()),
			ExistingGraphPath(Graph),
			TEXT("InvalidGraphSchema"));
	}

	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (IsPreservedFrameworkNode(Region, Node))
		{
			continue;
		}
		const FAssetDocumentCapabilityResult NodeResult = PreflightDeleteExistingNode(Blueprint, Registry, Graph, Node);
		if (!NodeResult.bSuccess)
		{
			return NodeResult;
		}
	}
	return FAssetDocumentCapabilityResult::Success();
}

bool IsPreservedFrameworkNode(EAssetDocumentK2GraphRegion Region, const UEdGraphNode* Node)
{
	if (Region == EAssetDocumentK2GraphRegion::FunctionGraphs)
	{
		return Cast<UK2Node_FunctionEntry>(Node) || Cast<UK2Node_FunctionResult>(Node);
	}
	if (Region == EAssetDocumentK2GraphRegion::MacroGraphs)
	{
		return Cast<UK2Node_Tunnel>(Node) != nullptr;
	}
	return false;
}

void BreakAllLinksForManagedNodes(const TMap<FString, UEdGraphNode*>& NodesById)
{
	for (const TPair<FString, UEdGraphNode*>& Pair : NodesById)
	{
		if (!Pair.Value)
		{
			continue;
		}
		for (UEdGraphPin* Pin : Pair.Value->Pins)
		{
			if (Pin)
			{
				Pin->BreakAllPinLinks();
			}
		}
	}
}

FAssetDocumentCapabilityResult CreateLinks(const FAssetDocumentGraphSpec& GraphSpec, const UEdGraphSchema_K2* Schema, const TMap<FString, UEdGraphNode*>& NodesById)
{
	for (const FAssetDocumentLinkSpec& Link : GraphSpec.Links)
	{
		UEdGraphPin* FromPin = FindPinBySpec(NodesById, Link.From);
		UEdGraphPin* ToPin = FindPinBySpec(NodesById, Link.To);
		if (!FromPin || !ToPin)
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Graph link endpoint '%s' could not be resolved after node reconstruction"), *Link.ToKey()),
				K2GraphLinkPath(GraphSpec, Link),
				TEXT("UnresolvedGraphLinkEndpoint"));
		}
		if (!Schema->TryCreateConnection(FromPin, ToPin))
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Graph schema rejected link '%s'"), *Link.ToKey()),
				K2GraphLinkPath(GraphSpec, Link),
				TEXT("InvalidGraphLinkType"));
		}
	}
	return FAssetDocumentCapabilityResult::Success();
}

FAssetDocumentCapabilityResult ApplyGraphsNoCompile(
	UBlueprint* Blueprint,
	EAssetDocumentK2GraphRegion Region,
	const TArray<FAssetDocumentGraphSpec>& DesiredGraphs,
	bool& bOutChanged)
{
	const FAssetDocumentNodeAdapterRegistry Registry = FAssetDocumentK2GraphAdapter::CreateTier1NodeAdapterRegistry();
	TSet<FString> DesiredGraphNames;
	TMap<FString, UClass*> SchemaClassesByGraph;
	const FName PreflightName = MakeUniqueObjectName(GetTransientPackage(), Blueprint->GetClass(), TEXT("AssetDocumentK2ApplyPreflight"));
	TStrongObjectPtr<UBlueprint> PreflightBlueprint(DuplicateObject<UBlueprint>(Blueprint, GetTransientPackage(), PreflightName));
	if (!PreflightBlueprint)
	{
		return K2GraphFailure(
			TEXT("Failed to duplicate Blueprint for graph preflight"),
			ActiveGraphRegionPath(),
			TEXT("GraphPreflightFailed"));
	}
	PreflightBlueprint->SetFlags(RF_Transient);
	for (const FAssetDocumentGraphSpec& GraphSpec : DesiredGraphs)
	{
		UClass* SchemaClass = nullptr;
		const FAssetDocumentCapabilityResult PreflightResult =
			PreflightGraphSpec(PreflightBlueprint.Get(), Region, GraphSpec, SchemaClass);
		if (!PreflightResult.bSuccess)
		{
			return PreflightResult;
		}
		DesiredGraphNames.Add(GraphSpec.Name);
		SchemaClassesByGraph.Add(GraphSpec.Name, SchemaClass);
	}

	for (UEdGraph* ExistingGraph : FBlueprintGraphArray(GetMutableGraphArray(Blueprint, Region)))
	{
		if (IsEmptyFunctionOrMacroNoop(Region, DesiredGraphs))
		{
			continue;
		}
		if (!IsManagedK2Graph(ExistingGraph, Region) || DesiredGraphNames.Contains(ExistingGraph->GetName()))
		{
			continue;
		}

		if (Region == EAssetDocumentK2GraphRegion::UbergraphPages && ExistingGraph == FBlueprintEditorUtils::FindEventGraph(Blueprint))
		{
			const FAssetDocumentCapabilityResult DeleteResult = PreflightDeleteExistingGraph(Blueprint, Registry, Region, ExistingGraph);
			if (!DeleteResult.bSuccess)
			{
				return DeleteResult;
			}
			for (UEdGraphNode* Node : TArray<UEdGraphNode*>(ExistingGraph->Nodes))
			{
				if (Node)
				{
					Node->DestroyNode();
					bOutChanged = true;
				}
			}
		}
		else
		{
			const FAssetDocumentCapabilityResult DeleteResult = PreflightDeleteExistingGraph(Blueprint, Registry, Region, ExistingGraph);
			if (!DeleteResult.bSuccess)
			{
				return DeleteResult;
			}
			FBlueprintEditorUtils::RemoveGraph(Blueprint, ExistingGraph, EGraphRemoveFlags::MarkTransient);
			bOutChanged = true;
		}
	}

	for (const FAssetDocumentGraphSpec& GraphSpec : DesiredGraphs)
	{
		UClass* SchemaClass = SchemaClassesByGraph.FindRef(GraphSpec.Name);
		bool bGraphChanged = false;
		UEdGraph* Graph = FindOrCreateGraph(Blueprint, Region, GraphSpec, SchemaClass, bGraphChanged);
		bOutChanged |= bGraphChanged;
		if (!Graph)
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Failed to create graph '%s'"), *GraphSpec.Name),
				K2GraphPath(GraphSpec),
				TEXT("InvalidGraphRegion"));
		}

		Graph->Modify();
		TSet<UEdGraphNode*> UsedExistingNodes;
		TMap<FString, UEdGraphNode*> NodesById;
		for (const FAssetDocumentNodeSpec& NodeSpec : GraphSpec.Nodes)
		{
			const TSharedPtr<IAssetDocumentNodeAdapter> Adapter = Registry.FindAdapter(NodeSpec.Class);
			if (!Adapter.IsValid())
			{
				return K2GraphFailure(
					FString::Printf(TEXT("Graph node class '%s' has no Tier 1 adapter"), *NodeSpec.Class),
					K2GraphJoinPath(K2GraphNodePath(GraphSpec, NodeSpec), TEXT("Class")),
					TEXT("UnsupportedGraphNodeClass"));
			}

			UClass* NodeClass = ResolveClass(NodeSpec.Class, UEdGraphNode::StaticClass());
			const bool bEventNode = NodeClass && NodeClass->IsChildOf(UK2Node_Event::StaticClass());
			UEdGraphNode* Node = bEventNode
				? nullptr
				: FindReusableNode(Blueprint, Registry, Graph, NodeSpec, UsedExistingNodes);
			if (!Node)
			{
				bool bNodeCreated = false;
				const FAssetDocumentCapabilityResult MaterializeResult = MaterializeNodeWithSpawner(
					Blueprint,
					*Adapter,
					Graph,
					GraphSpec,
					NodeSpec,
					UsedExistingNodes,
					Node,
					bNodeCreated,
					false);
				if (!MaterializeResult.bSuccess)
				{
					return MaterializeResult;
				}
				bOutChanged |= bNodeCreated;
			}

			const FAssetDocumentCapabilityResult ConfigureResult = ConfigureNodeWithAdapter(Blueprint, *Adapter, Node, GraphSpec, NodeSpec);
			if (!ConfigureResult.bSuccess)
			{
				return ConfigureResult;
			}
			const FAssetDocumentCapabilityResult PinResult = ApplyPinDefaults(GraphSpec, NodeSpec, Node);
			if (!PinResult.bSuccess)
			{
				return PinResult;
			}
			NodesById.Add(NodeSpec.Id, Node);
		}

		for (UEdGraphNode* ExistingNode : TArray<UEdGraphNode*>(Graph->Nodes))
		{
			if (ExistingNode && !UsedExistingNodes.Contains(ExistingNode))
			{
				if (IsPreservedFrameworkNode(Region, ExistingNode))
				{
					continue;
				}
				const FAssetDocumentCapabilityResult DeleteResult = PreflightDeleteExistingNode(Blueprint, Registry, Graph, ExistingNode);
				if (!DeleteResult.bSuccess)
				{
					return DeleteResult;
				}
				ExistingNode->DestroyNode();
				bOutChanged = true;
			}
		}

		BreakAllLinksForManagedNodes(NodesById);
		const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
		const FAssetDocumentCapabilityResult LinkResult = CreateLinks(GraphSpec, Schema, NodesById);
		if (!LinkResult.bSuccess)
		{
			return LinkResult;
		}
	}

	return FAssetDocumentCapabilityResult::Success();
}

class FLosslessUbergraphPagesSnapshot
{
public:
	FLosslessUbergraphPagesSnapshot() = default;
	FLosslessUbergraphPagesSnapshot(const FLosslessUbergraphPagesSnapshot&) = delete;
	FLosslessUbergraphPagesSnapshot& operator=(const FLosslessUbergraphPagesSnapshot&) = delete;

	bool Capture(const UBlueprint* Blueprint, EAssetDocumentK2GraphRegion InRegion)
	{
		if (!Blueprint)
		{
			return false;
		}

		const FName SnapshotName = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("AssetDocumentGraphSnapshot"));
		SnapshotBlueprint.Reset(DuplicateObject<UBlueprint>(Blueprint, GetTransientPackage(), SnapshotName));
		if (!SnapshotBlueprint)
		{
			return false;
		}
		SnapshotBlueprint->SetFlags(RF_Transient);
		Region = InRegion;
		SnapshotGraphs = GetGraphArray(SnapshotBlueprint.Get(), Region);
		return true;
	}

	bool Restore(UBlueprint* Blueprint) const
	{
		if (!Blueprint || !SnapshotBlueprint)
		{
			return false;
		}

		FBlueprintGraphArray& TargetGraphs = GetMutableGraphArray(Blueprint, Region);
		for (UEdGraph* ExistingGraph : FBlueprintGraphArray(TargetGraphs))
		{
			if (ExistingGraph)
			{
				ExistingGraph->Modify();
				ExistingGraph->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
			}
		}
		TargetGraphs.Reset();

		for (UEdGraph* SnapshotGraph : SnapshotGraphs)
		{
			if (!SnapshotGraph)
			{
				TargetGraphs.Add(nullptr);
				continue;
			}

			UEdGraph* RestoredGraph = DuplicateObject<UEdGraph>(SnapshotGraph, Blueprint, SnapshotGraph->GetFName());
			if (!RestoredGraph)
			{
				return false;
			}
			TargetGraphs.Add(RestoredGraph);
		}
		return true;
	}

private:
	TStrongObjectPtr<UBlueprint> SnapshotBlueprint;
	FBlueprintGraphArray SnapshotGraphs;
	EAssetDocumentK2GraphRegion Region = EAssetDocumentK2GraphRegion::UbergraphPages;
};

FString SanitizeSidecarId(const FString& Value, const FString& Fallback)
{
	FString Result = Value.IsEmpty() ? Fallback : Value;
	for (int32 Index = 0; Index < Result.Len(); ++Index)
	{
		const TCHAR Character = Result[Index];
		if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
		{
			Result[Index] = TEXT('_');
		}
	}

	if (Result.IsEmpty() || (!FChar::IsAlpha(Result[0]) && Result[0] != TEXT('_')))
	{
		Result = FString(TEXT("Node_")) + Result;
	}
	return Result;
}

FString MakeUniqueId(const FString& BaseId, TSet<FString>& UsedIds)
{
	FString Candidate = SanitizeSidecarId(BaseId, TEXT("Node"));
	if (!UsedIds.Contains(Candidate))
	{
		UsedIds.Add(Candidate);
		return Candidate;
	}

	for (int32 Suffix = 2;; ++Suffix)
	{
		const FString Suffixed = FString::Printf(TEXT("%s_%d"), *Candidate, Suffix);
		if (!UsedIds.Contains(Suffixed))
		{
			UsedIds.Add(Suffixed);
			return Suffixed;
		}
	}
}

FString MakeNodeBaseId(const UEdGraphNode* Node)
{
	if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		return EventNode->GetFunctionName().ToString();
	}
	if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		return CallNode->GetFunctionName().ToString();
	}
	if (const UK2Node_VariableGet* VariableGet = Cast<UK2Node_VariableGet>(Node))
	{
		return FString::Printf(TEXT("Get_%s"), *VariableGet->GetVarNameString());
	}
	if (const UK2Node_VariableSet* VariableSet = Cast<UK2Node_VariableSet>(Node))
	{
		return FString::Printf(TEXT("Set_%s"), *VariableSet->GetVarNameString());
	}
	if (Node && Node->IsA<UK2Node_Self>())
	{
		return TEXT("Self");
	}
	return Node ? Node->GetName() : FString(TEXT("Node"));
}

TSharedRef<FJsonObject> K2GraphMakeSkippedNodeObject(const UEdGraph* Graph, const UEdGraphNode* Node)
{
	TSharedRef<FJsonObject> Skipped = MakeShared<FJsonObject>();
	Skipped->SetStringField(TEXT("Reason"), TEXT("UnsupportedGraphNodeClass"));
	Skipped->SetStringField(TEXT("Graph"), Graph ? Graph->GetName() : FString());
	Skipped->SetStringField(TEXT("Class"), FAssetDocumentGraphEncodingUtils::GetClassPath(Node ? Node->GetClass() : nullptr));
	Skipped->SetStringField(TEXT("NodeTitle"), FAssetDocumentGraphEncodingUtils::GetNodeTitle(Node));
	Skipped->SetStringField(TEXT("NodeGuid"), FAssetDocumentGraphEncodingUtils::GetNodeGuid(Node));
	return Skipped;
}

bool IsSupportedPinName(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return false;
	}
	const FString PinName = Pin->PinName.ToString();
	return !PinName.IsEmpty() && SanitizeSidecarId(PinName, TEXT("")) == PinName;
}

void AddSupportedLinks(
	const TArray<UEdGraphNode*>& GraphNodes,
	const TMap<const UEdGraphNode*, FString>& NodeIds,
	FAssetDocumentGraphSpec& GraphSpec)
{
	TSet<FString> LinkKeys;
	for (UEdGraphNode* Node : GraphNodes)
	{
		const FString* FromNodeId = NodeIds.Find(Node);
		if (!FromNodeId)
		{
			continue;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || !IsSupportedPinName(Pin))
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				const FString* ToNodeId = NodeIds.Find(LinkedNode);
				if (!ToNodeId || !IsSupportedPinName(LinkedPin))
				{
					continue;
				}

				FAssetDocumentLinkSpec Link;
				Link.From.Node = *FromNodeId;
				Link.From.Pin = Pin->PinName.ToString();
				Link.To.Node = *ToNodeId;
				Link.To.Pin = LinkedPin->PinName.ToString();
				if (!LinkKeys.Contains(Link.ToKey()))
				{
					LinkKeys.Add(Link.ToKey());
					GraphSpec.Links.Add(MoveTemp(Link));
				}
			}
		}
	}
}
}

FAssetDocumentNodeAdapterRegistry FAssetDocumentK2GraphAdapter::CreateTier1NodeAdapterRegistry()
{
	FAssetDocumentNodeAdapterRegistry Registry;
	Registry.RegisterAdapter(TEXT("/Script/BlueprintGraph.K2Node_Event"), MakeShared<FAssetDocumentK2EventNodeAdapter>());
	Registry.RegisterAdapter(TEXT("/Script/BlueprintGraph.K2Node_CallFunction"), MakeShared<FAssetDocumentK2CallFunctionNodeAdapter>());
	Registry.RegisterAdapter(TEXT("/Script/BlueprintGraph.K2Node_VariableGet"), MakeShared<FAssetDocumentK2VariableGetNodeAdapter>());
	Registry.RegisterAdapter(TEXT("/Script/BlueprintGraph.K2Node_VariableSet"), MakeShared<FAssetDocumentK2VariableSetNodeAdapter>());
	Registry.RegisterAdapter(TEXT("/Script/BlueprintGraph.K2Node_Self"), MakeShared<FAssetDocumentK2SelfNodeAdapter>());
	return Registry;
}

FAssetDocumentCapabilityResult FAssetDocumentK2GraphAdapter::PreflightUbergraphPages(
	UBlueprint* Blueprint,
	const TArray<FAssetDocumentGraphSpec>& DesiredGraphs) const
{
	return PreflightGraphRegion(Blueprint, EAssetDocumentK2GraphRegion::UbergraphPages, DesiredGraphs);
}

FAssetDocumentCapabilityResult FAssetDocumentK2GraphAdapter::PreflightGraphRegion(
	UBlueprint* Blueprint,
	EAssetDocumentK2GraphRegion Region,
	const TArray<FAssetDocumentGraphSpec>& DesiredGraphs) const
{
	UBlueprint* PreflightBlueprint = nullptr;
	TStrongObjectPtr<UBlueprint> PreflightBlueprintGuard;
	if (Blueprint)
	{
		const FName PreflightName = MakeUniqueObjectName(
			GetTransientPackage(),
			Blueprint->GetClass(),
			TEXT("AssetDocumentK2Preflight"));
		PreflightBlueprintGuard.Reset(DuplicateObject<UBlueprint>(Blueprint, GetTransientPackage(), PreflightName));
		PreflightBlueprint = PreflightBlueprintGuard.Get();
	}
	else
	{
		PreflightBlueprintGuard.Reset(NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient));
		PreflightBlueprint = PreflightBlueprintGuard.Get();
	}
	if (!PreflightBlueprint)
	{
		return K2GraphFailure(
			TEXT("Failed to duplicate Blueprint for graph preflight"),
			GetGraphRegionPath(Region),
			TEXT("GraphPreflightFailed"));
	}
	PreflightBlueprint->SetFlags(RF_Transient);
	return PreflightGraphRegion(Blueprint, PreflightBlueprint, Region, DesiredGraphs);
}

FAssetDocumentCapabilityResult FAssetDocumentK2GraphAdapter::PreflightGraphRegion(
	UBlueprint* CurrentBlueprint,
	UBlueprint* DesiredStateBlueprint,
	EAssetDocumentK2GraphRegion Region,
	const TArray<FAssetDocumentGraphSpec>& DesiredGraphs) const
{
	const FScopedGraphRegionPath ScopedRegion(Region);
	TStrongObjectPtr<UBlueprint> OwnedDesiredStateBlueprint;
	if (!DesiredStateBlueprint || DesiredStateBlueprint == CurrentBlueprint)
	{
		if (CurrentBlueprint)
		{
			const FName PreflightName = MakeUniqueObjectName(
				GetTransientPackage(),
				CurrentBlueprint->GetClass(),
				TEXT("AssetDocumentK2DesiredStatePreflight"));
			OwnedDesiredStateBlueprint.Reset(
				DuplicateObject<UBlueprint>(CurrentBlueprint, GetTransientPackage(), PreflightName));
		}
		else
		{
			OwnedDesiredStateBlueprint.Reset(NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient));
		}
		DesiredStateBlueprint = OwnedDesiredStateBlueprint.Get();
	}
	if (!DesiredStateBlueprint)
	{
		return K2GraphFailure(
			TEXT("Failed to initialize isolated desired-state Blueprint for graph preflight"),
			ActiveGraphRegionPath(),
			TEXT("GraphPreflightFailed"));
	}
	DesiredStateBlueprint->SetFlags(RF_Transient);

	const FAssetDocumentNodeAdapterRegistry Registry = FAssetDocumentK2GraphAdapter::CreateTier1NodeAdapterRegistry();
	TSet<FString> DesiredGraphNames;
	for (const FAssetDocumentGraphSpec& GraphSpec : DesiredGraphs)
	{
		UClass* SchemaClass = nullptr;
		const FAssetDocumentCapabilityResult PreflightResult =
			PreflightGraphSpec(DesiredStateBlueprint, Region, GraphSpec, SchemaClass);
		if (!PreflightResult.bSuccess)
		{
			return PreflightResult;
		}
		DesiredGraphNames.Add(GraphSpec.Name);
	}

	if (!CurrentBlueprint)
	{
		return FAssetDocumentCapabilityResult::Success();
	}

	for (UEdGraph* ExistingGraph : FBlueprintGraphArray(GetMutableGraphArray(CurrentBlueprint, Region)))
	{
		if (IsEmptyFunctionOrMacroNoop(Region, DesiredGraphs))
		{
			continue;
		}
		if (!IsManagedK2Graph(ExistingGraph, Region) || DesiredGraphNames.Contains(ExistingGraph->GetName()))
		{
			continue;
		}

		const FAssetDocumentCapabilityResult DeleteResult = PreflightDeleteExistingGraph(CurrentBlueprint, Registry, Region, ExistingGraph);
		if (!DeleteResult.bSuccess)
		{
			return DeleteResult;
		}
	}

	for (const FAssetDocumentGraphSpec& GraphSpec : DesiredGraphs)
	{
		UEdGraph* ExistingGraph = FindGraphByName(CurrentBlueprint, Region, GraphSpec.Name);
		if (!ExistingGraph)
		{
			continue;
		}

		if (!Cast<UEdGraphSchema_K2>(ExistingGraph->GetSchema()))
		{
			return K2GraphFailure(
				FString::Printf(TEXT("Existing graph '%s' does not use EdGraphSchema_K2 and cannot be overwritten by Tier 1 graph sidecar apply"), *ExistingGraph->GetName()),
				ExistingGraphPath(ExistingGraph),
				TEXT("InvalidGraphSchema"));
		}

		TSet<UEdGraphNode*> UsedExistingNodes;
		for (const FAssetDocumentNodeSpec& NodeSpec : GraphSpec.Nodes)
		{
			FindReusableNode(CurrentBlueprint, Registry, ExistingGraph, NodeSpec, UsedExistingNodes);
		}

		for (const UEdGraphNode* ExistingNode : ExistingGraph->Nodes)
		{
			if (!ExistingNode || UsedExistingNodes.Contains(const_cast<UEdGraphNode*>(ExistingNode)) || IsPreservedFrameworkNode(Region, ExistingNode))
			{
				continue;
			}
			const FAssetDocumentCapabilityResult DeleteResult = PreflightDeleteExistingNode(CurrentBlueprint, Registry, ExistingGraph, ExistingNode);
			if (!DeleteResult.bSuccess)
			{
				return DeleteResult;
			}
		}
	}

	return FAssetDocumentCapabilityResult::Success();
}

FAssetDocumentK2GraphExtractResult FAssetDocumentK2GraphAdapter::ExtractUbergraphPages(const UBlueprint* Blueprint) const
{
	return ExtractGraphRegion(Blueprint, EAssetDocumentK2GraphRegion::UbergraphPages);
}

FAssetDocumentK2GraphExtractResult FAssetDocumentK2GraphAdapter::ExtractGraphRegion(const UBlueprint* Blueprint, EAssetDocumentK2GraphRegion Region) const
{
	FAssetDocumentK2GraphExtractResult Result;
	if (!Blueprint)
	{
		return Result;
	}

	const FScopedGraphRegionPath ScopedRegion(Region);
	for (UEdGraph* Graph : GetGraphArray(Blueprint, Region))
	{
		if (!IsManagedK2Graph(Graph, Region))
		{
			continue;
		}

		FAssetDocumentGraphSpec GraphSpec;
		GraphSpec.Name = Graph->GetName();
		GraphSpec.Schema = Graph->Schema ? Graph->Schema->GetPathName() : FString(TEXT("/Script/BlueprintGraph.EdGraphSchema_K2"));
		if (Graph->GraphGuid.IsValid())
		{
			GraphSpec.GraphGuid = Graph->GraphGuid.ToString(EGuidFormats::Digits);
		}

		TSet<FString> UsedIds;
		TMap<const UEdGraphNode*, FString> NodeIds;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			FAssetDocumentNodeSpec NodeSpec;
			if (!ExtractNode(Blueprint, Node, NodeSpec))
			{
				if (IsPreservedFrameworkNode(Region, Node))
				{
					continue;
				}
				Result.SkippedNodes.Add(MakeShared<FJsonValueObject>(K2GraphMakeSkippedNodeObject(Graph, Node)));
				continue;
			}

			NodeSpec.Id = MakeUniqueId(MakeNodeBaseId(Node), UsedIds);
			NodeSpec.Class = FAssetDocumentGraphEncodingUtils::GetClassPath(Node->GetClass());
			if (Node->NodeGuid.IsValid())
			{
				NodeSpec.NodeGuid = Node->NodeGuid.ToString(EGuidFormats::Digits);
			}
			NodeSpec.Position = FAssetDocumentGraphEncodingUtils::MakeNodePositionObject(Node);
			NodeIds.Add(Node, NodeSpec.Id);
			GraphSpec.Nodes.Add(MoveTemp(NodeSpec));
		}

		AddSupportedLinks(Graph->Nodes, NodeIds, GraphSpec);
		if (!GraphSpec.Nodes.IsEmpty() || !GraphSpec.Links.IsEmpty())
		{
			Result.Graphs.Add(MoveTemp(GraphSpec));
		}
	}

	return Result;
}

bool FAssetDocumentK2GraphAdapter::ExtractNode(const UBlueprint* Blueprint, const UEdGraphNode* Node, FAssetDocumentNodeSpec& OutNode) const
{
	if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		return FAssetDocumentK2EventNodeAdapter().ExtractNode(Blueprint, EventNode, OutNode);
	}
	if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		return FAssetDocumentK2CallFunctionNodeAdapter().ExtractNode(Blueprint, CallNode, OutNode);
	}
	if (const UK2Node_VariableGet* VariableGet = Cast<UK2Node_VariableGet>(Node))
	{
		return FAssetDocumentK2VariableGetNodeAdapter().ExtractNode(Blueprint, VariableGet, OutNode);
	}
	if (const UK2Node_VariableSet* VariableSet = Cast<UK2Node_VariableSet>(Node))
	{
		return FAssetDocumentK2VariableSetNodeAdapter().ExtractNode(Blueprint, VariableSet, OutNode);
	}
	if (const UK2Node_Self* SelfNode = Cast<UK2Node_Self>(Node))
	{
		return FAssetDocumentK2SelfNodeAdapter().ExtractNode(Blueprint, SelfNode, OutNode);
	}
	return false;
}

FAssetDocumentK2GraphApplyResult FAssetDocumentK2GraphAdapter::ApplyUbergraphPages(
	UBlueprint* Blueprint,
	const TArray<FAssetDocumentGraphSpec>& DesiredGraphs) const
{
	return ApplyGraphRegion(Blueprint, EAssetDocumentK2GraphRegion::UbergraphPages, DesiredGraphs);
}

FAssetDocumentK2GraphApplyResult FAssetDocumentK2GraphAdapter::ApplyGraphRegion(
	UBlueprint* Blueprint,
	EAssetDocumentK2GraphRegion Region,
	const TArray<FAssetDocumentGraphSpec>& DesiredGraphs) const
{
	FAssetDocumentK2GraphApplyResult ApplyResult;
	if (!Blueprint)
	{
		ApplyResult.Result = K2GraphFailure(
			TEXT("UBlueprint graph apply requires exact UBlueprint asset"),
			TEXT("/Body"),
			TEXT("UnsupportedAsset"));
		return ApplyResult;
	}

	const FScopedGraphRegionPath ScopedRegion(Region);
	FLosslessUbergraphPagesSnapshot Snapshot;
	if (!Snapshot.Capture(Blueprint, Region))
	{
		ApplyResult.Result = K2GraphFailure(
			TEXT("Failed to snapshot UBlueprint graph state before applying graph regions"),
			ActiveGraphRegionPath(),
			TEXT("GraphSnapshotFailed"));
		return ApplyResult;
	}

	bool bChanged = false;
	ApplyResult.Result = ApplyGraphsNoCompile(Blueprint, Region, DesiredGraphs, bChanged);
	if (!ApplyResult.Result.bSuccess)
	{
		Snapshot.Restore(Blueprint);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		return ApplyResult;
	}

	if (bChanged || !DesiredGraphs.IsEmpty())
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (Blueprint->Status == BS_Error)
		{
			Snapshot.Restore(Blueprint);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			ApplyResult.Result = K2GraphFailure(
				TEXT("Failed to compile UBlueprint after applying graph regions"),
				ActiveGraphRegionPath(),
				TEXT("BlueprintCompileFailed"));
			return ApplyResult;
		}
	}

	ApplyResult.bChanged = bChanged;
	ApplyResult.Result = FAssetDocumentCapabilityResult::Success(TEXT("Applied UBlueprint graph regions"));
	return ApplyResult;
}
