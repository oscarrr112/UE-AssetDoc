// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentGraphModel.h"

#include "AssetDocumentGraphAdapters.h"
#include "AssetDocumentGraphNodeSize.h"
#include "AssetDocumentGraphPropertyCodec.h"
#include "AssetDocumentGraphRawEdits.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AssetDocument::Graph
{
namespace
{
TArray<UEdGraph*> OwnedSubgraphs(const TSharedPtr<IAssetDocumentGraphSchemaAdapter>& Schema, const UEdGraphNode& Node)
{
	TArray<UEdGraph*> Graphs;
	if (Schema.IsValid())
	{
		Schema->GetOwnedSubgraphs(Node, Graphs);
	}
	else
	{
		Graphs = Node.GetSubGraphs();
	}
	return Graphs;
}

// "Container/" for every UEdGraph outer between the node and a graph it reports
// through GetOwnedSubgraphs (empty for ordinary node-owned subgraphs).
FString IntermediateGraphPath(const UEdGraph& Subgraph, const UEdGraphNode& Node)
{
	TArray<FString> Names;
	for (const UObject* Outer = Subgraph.GetOuter(); Outer != nullptr && Outer != &Node; Outer = Outer->GetOuter())
	{
		const UEdGraph* Container = Cast<UEdGraph>(Outer);
		if (Container == nullptr)
		{
			return FString();
		}
		Names.Insert(Container->GetName(), 0);
	}
	FString Path;
	for (const FString& Name : Names)
	{
		Path += Name + TEXT("/");
	}
	return Path;
}

void AppendSubgraphs(FAssetGraphs& Out, const FGraphEntry& Parent, TSet<UEdGraph*>& Seen)
{
	if (Parent.Graph == nullptr)
	{
		return;
	}
	for (UEdGraphNode* Node : Parent.Graph->Nodes)
	{
		if (!IsValid(Node))
		{
			continue;
		}
		for (UEdGraph* Subgraph : OwnedSubgraphs(Parent.Schema, *Node))
		{
			if (!IsValid(Subgraph) || Seen.Contains(Subgraph))
			{
				continue;
			}
			Seen.Add(Subgraph);
			FGraphEntry Entry;
			Entry.Graph = Subgraph;
			Entry.Path = Parent.Path + TEXT("/") + IntermediateGraphPath(*Subgraph, *Node) + Subgraph->GetName();
			Entry.ParentGraph = Parent.Graph;
			Entry.OwnerNode = Node;
			Entry.Schema = FindAssetDocumentGraphSchemaAdapter(Subgraph->GetSchema());
			Entry.SchemaTag = Entry.Schema.IsValid() ? Entry.Schema->Tag(*Subgraph) : FString();
			const TSharedPtr<IAssetDocumentGraphSchemaAdapter> ParentSchema = Parent.Schema;
			Entry.Role = ParentSchema.IsValid() ? ParentSchema->ClassifySubgraph(*Subgraph, *Node) : TEXT("other");
			Entry.bCanSpawn = Subgraph->bEditable && Entry.Schema.IsValid();
			Entry.bCanRename = false;
			Entry.bCanRemove = false;
			const int32 Index = Out.Graphs.Add(MoveTemp(Entry));
			const FGraphEntry Copy = Out.Graphs[Index];
			AppendSubgraphs(Out, Copy, Seen);
		}
	}
}

TArray<FString> CollectPinNames(const UEdGraphNode& Node, TOptional<EEdGraphPinDirection> Direction)
{
	TArray<FString> Names;
	for (const UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin != nullptr && (!Direction.IsSet() || Pin->Direction == Direction.GetValue()))
		{
			Names.Add(FString::Printf(TEXT("%s (%s)"), *Pin->PinName.ToString(), *DirectionToString(Pin->Direction)));
		}
	}
	return Names;
}

FString ContainerToString(EPinContainerType Type)
{
	switch (Type)
	{
	case EPinContainerType::Array: return TEXT("array");
	case EPinContainerType::Set: return TEXT("set");
	case EPinContainerType::Map: return TEXT("map");
	default: return TEXT("none");
	}
}

FString EnabledStateToString(ENodeEnabledState State)
{
	switch (State)
	{
	case ENodeEnabledState::Disabled: return TEXT("disabled");
	case ENodeEnabledState::DevelopmentOnly: return TEXT("development_only");
	default: return TEXT("enabled");
	}
}

UObject* ResolveTypeObject(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	UObject* Object = FindObject<UObject>(nullptr, *Path);
	if (Object == nullptr)
	{
		Object = FSoftObjectPath(Path).TryLoad();
	}
	return Object;
}
// Why an animation node may lack a pin: optional pins (FOptionalPinFromProperty lists: an anim node's Details "show
// as pin" toggles, a Control Rig node's rig variables and controls) are pins only once shown, by "ShowPin.<Name>" in
// the node's Properties. Empty for other nodes.
FString GraphModelOptionalPinHint(const UEdGraphNode& Node, FName Name)
{
	const UClass* AnimNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimGraphNode_Base"));
	if (AnimNodeClass == nullptr || !Node.GetClass()->IsChildOf(AnimNodeClass))
	{
		return FString();
	}
	for (TFieldIterator<FArrayProperty> It(Node.GetClass()); It; ++It)
	{
		const FStructProperty* Inner = CastField<FStructProperty>(It->Inner);
		if (Inner == nullptr || Inner->Struct == nullptr || Inner->Struct->GetFName() != TEXT("OptionalPinFromProperty"))
		{
			continue;
		}
		const FNameProperty* NameProperty = CastField<FNameProperty>(Inner->Struct->FindPropertyByName(TEXT("PropertyName")));
		const FBoolProperty* ShownProperty = CastField<FBoolProperty>(Inner->Struct->FindPropertyByName(TEXT("bShowPin")));
		if (NameProperty == nullptr || ShownProperty == nullptr)
		{
			continue;
		}
		FScriptArrayHelper Pins(*It, It->ContainerPtrToValuePtr<void>(&Node));
		for (int32 Index = 0; Index < Pins.Num(); ++Index)
		{
			const uint8* Entry = Pins.GetRawPtr(Index);
			if (NameProperty->GetPropertyValue_InContainer(Entry) == Name && !ShownProperty->GetPropertyValue_InContainer(Entry))
			{
				return FString::Printf(TEXT(" ('%s' is an optional pin the node does not show: set \"ShowPin.%s\": true in the node's "
					"Properties)"), *Name.ToString(), *Name.ToString());
			}
		}
	}
	return TEXT(" (an animation node shows optional pins, a Control Rig node's rig variables and controls among them, through "
		"\"ShowPin.<Name>\": true in its Properties)");
}

} // namespace

FString ObjectPathOrEmpty(const UObject* Object)
{
	return Object != nullptr ? Object->GetPathName() : FString();
}

FGuid FGraphEntry::GuidOf(const UEdGraph* InGraph) const
{
	if (InGraph == nullptr)
	{
		return FGuid();
	}
	const FGuid* Reported = ReportedGuids.IsValid() ? ReportedGuids->Find(InGraph) : nullptr;
	return Reported != nullptr ? *Reported : InGraph->GraphGuid;
}

const FGraphEntry* FAssetGraphs::FindEntry(const UEdGraph* Graph) const
{
	return Graphs.FindByPredicate([Graph](const FGraphEntry& Entry) { return Entry.Graph == Graph; });
}

UEdGraphNode* FAssetGraphs::FindNode(const FGuid& NodeGuid, const FGraphEntry** OutEntry) const
{
	for (const FGraphEntry& Entry : Graphs)
	{
		for (UEdGraphNode* Node : Entry.Graph->Nodes)
		{
			if (IsValid(Node) && Node->NodeGuid == NodeGuid)
			{
				if (OutEntry != nullptr)
				{
					*OutEntry = &Entry;
				}
				return Node;
			}
		}
	}
	return nullptr;
}

bool CollectGraphs(UObject& Asset, FAssetGraphs& OutGraphs, FAssetDocumentGraphError& OutError)
{
	OutGraphs = FAssetGraphs();
	OutGraphs.Asset = &Asset;
	OutGraphs.Host = FindAssetDocumentGraphHostAdapter(&Asset);
	if (!OutGraphs.Host.IsValid())
	{
		OutError.Set(TEXT("Graph.GraphOperationUnsupported"),
			FString::Printf(TEXT("No graph host adapter is registered for asset class %s."),
				*Asset.GetClass()->GetPathName()));
		return false;
	}
	TArray<FAssetDocumentGraphInfo> TopLevel;
	OutGraphs.Host->EnumerateGraphs(Asset, TopLevel);
	TSet<UEdGraph*> Seen;
	for (FAssetDocumentGraphInfo& Info : TopLevel)
	{
		if (!IsValid(Info.Graph) || Seen.Contains(Info.Graph))
		{
			continue;
		}
		Seen.Add(Info.Graph);
		FGraphEntry Entry;
		static_cast<FAssetDocumentGraphInfo&>(Entry) = Info;
		Entry.Schema = FindAssetDocumentGraphSchemaAdapter(Info.Graph->GetSchema());
		Entry.SchemaTag = Entry.Schema.IsValid() ? Entry.Schema->Tag(*Info.Graph) : FString();
		Entry.bCanSpawn = Entry.bCanSpawn && Entry.Schema.IsValid();
		OutGraphs.Graphs.Add(MoveTemp(Entry));
	}
	const int32 TopCount = OutGraphs.Graphs.Num();
	for (int32 Index = 0; Index < TopCount; ++Index)
	{
		const FGraphEntry Copy = OutGraphs.Graphs[Index];
		AppendSubgraphs(OutGraphs, Copy, Seen);
	}
	TSharedRef<TMap<const UEdGraph*, FGuid>> Reported = MakeShared<TMap<const UEdGraph*, FGuid>>();
	TSet<FGuid> Used;
	for (const FGraphEntry& Entry : OutGraphs.Graphs)
	{
		FGuid Guid = Entry.Graph->GraphGuid;
		for (int32 Salt = 0; !Guid.IsValid() || Used.Contains(Guid); ++Salt)
		{
			Guid = FGuid::NewDeterministicGuid(FString::Printf(TEXT("%s|%s|%d"),
				*Entry.Graph->GraphGuid.ToString(), *Entry.Path, Salt));
		}
		Used.Add(Guid);
		Reported->Add(Entry.Graph, Guid);
	}
	for (FGraphEntry& Entry : OutGraphs.Graphs)
	{
		Entry.ReportedGuids = Reported;
	}
	return true;
}

bool ResolveGraphAddress(const FAssetGraphs& Graphs, FReader& Address, const FGraphEntry*& OutEntry)
{
	OutEntry = nullptr;
	if (!Address.OnlyFields({TEXT("graph_guid"), TEXT("graph_path")}))
	{
		return false;
	}
	FGuid Guid;
	FString Path;
	if (!Address.Guid(TEXT("graph_guid"), Guid, false) || !Address.String(TEXT("graph_path"), Path, false))
	{
		return false;
	}
	if (!Guid.IsValid() && Path.IsEmpty())
	{
		return Address.Fail(TEXT("Graph.InvalidArgument"), TEXT("A graph address needs graph_guid or graph_path."));
	}
	for (const FGraphEntry& Entry : Graphs.Graphs)
	{
		const bool bGuidOk = !Guid.IsValid() || Entry.Guid() == Guid;
		const bool bPathOk = Path.IsEmpty() || Entry.Path.Equals(Path, ESearchCase::IgnoreCase);
		if (bGuidOk && bPathOk)
		{
			OutEntry = &Entry;
			return true;
		}
	}
	TArray<FString> Known;
	for (const FGraphEntry& Entry : Graphs.Graphs)
	{
		Known.Add(Entry.Path);
	}
	Address.Fail(TEXT("Graph.GraphNotFound"),
		FString::Printf(TEXT("No graph matches %s. Known graph paths: %s."),
			Guid.IsValid() ? *FormatAssetDocumentGraphGuid(Guid) : *Path, *FString::Join(Known, TEXT(", "))));
	return false;
}

bool ResolveNodeGuid(
	const FAssetGraphs& Graphs, FReader& Args, FStringView Key, UEdGraphNode*& OutNode, const FGraphEntry** OutEntry)
{
	FGuid Guid;
	if (!Args.Guid(Key, Guid))
	{
		return false;
	}
	OutNode = Graphs.FindNode(Guid, OutEntry);
	if (OutNode == nullptr)
	{
		return Args.Fail(TEXT("Graph.NodeNotFound"),
			FString::Printf(TEXT("No node with guid %s exists in this asset; query the graph again."),
				*FormatAssetDocumentGraphGuid(Guid)), Key);
	}
	return true;
}

FString DirectionToString(EEdGraphPinDirection Direction)
{
	return Direction == EGPD_Output ? TEXT("output") : TEXT("input");
}

bool ParseDirection(FStringView Text, EEdGraphPinDirection& OutDirection)
{
	if (Text.Equals(TEXTVIEW("input"), ESearchCase::IgnoreCase))
	{
		OutDirection = EGPD_Input;
		return true;
	}
	if (Text.Equals(TEXTVIEW("output"), ESearchCase::IgnoreCase))
	{
		OutDirection = EGPD_Output;
		return true;
	}
	return false;
}

bool ResolvePinAddress(const FAssetGraphs& Graphs, FReader& Address, UEdGraphPin*& OutPin)
{
	OutPin = nullptr;
	if (!Address.OnlyFields({TEXT("node_guid"), TEXT("pin_name"), TEXT("direction")}))
	{
		return false;
	}
	UEdGraphNode* Node = nullptr;
	FString PinName;
	FString DirectionText;
	if (!ResolveNodeGuid(Graphs, Address, TEXT("node_guid"), Node)
		|| !Address.String(TEXT("pin_name"), PinName)
		|| !Address.String(TEXT("direction"), DirectionText, false))
	{
		return false;
	}
	TOptional<EEdGraphPinDirection> Direction;
	if (!DirectionText.IsEmpty())
	{
		EEdGraphPinDirection Parsed = EGPD_Input;
		if (!ParseDirection(DirectionText, Parsed))
		{
			return Address.Fail(TEXT("Graph.InvalidArgument"), TEXT("direction must be 'input' or 'output'."), TEXT("direction"));
		}
		Direction = Parsed;
	}
	const FName Name(*PinName);
	TArray<UEdGraphPin*> Matches;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin != nullptr && Pin->PinName == Name && (!Direction.IsSet() || Pin->Direction == Direction.GetValue()))
		{
			Matches.Add(Pin);
		}
	}
	if (Matches.Num() == 1)
	{
		OutPin = Matches[0];
		return true;
	}
	if (Matches.Num() > 1)
	{
		return Address.Fail(TEXT("Graph.PinNotFound"),
			FString::Printf(TEXT("Pin '%s' exists in both directions; pass direction."), *PinName), TEXT("pin_name"));
	}
	if (Direction.IsSet())
	{
		const TSharedPtr<IAssetDocumentGraphSchemaAdapter> Adapter = FindAssetDocumentGraphSchemaAdapter(Node->GetSchema());
		if (UEdGraphPin* Alias = Adapter.IsValid() ? Adapter->ResolvePinAlias(*Node, Name, Direction.GetValue()) : nullptr)
		{
			OutPin = Alias;
			return true;
		}
	}
	const TArray<FString> Available = CollectPinNames(*Node, Direction);
	Address.Fail(TEXT("Graph.PinNotFound"),
		FString::Printf(TEXT("Node %s (%s) has no pin '%s'%s. Available: %s.%s"),
			*FormatAssetDocumentGraphGuid(Node->NodeGuid), *Node->GetClass()->GetName(), *PinName,
			Direction.IsSet() ? *FString::Printf(TEXT(" (%s)"), *DirectionText) : TEXT(""),
			*FString::Join(Available, TEXT(", ")), *GraphModelOptionalPinHint(*Node, Name)), TEXT("pin_name"));
	return false;
}

FValue MakePinTypeValue(const FEdGraphPinType& Type)
{
	FObjectBuilder Builder;
	Builder.AddString(TEXT("category"), Type.PinCategory.ToString())
		.AddString(TEXT("sub_category"), Type.PinSubCategory.IsNone() ? FString() : Type.PinSubCategory.ToString())
		.AddString(TEXT("sub_category_object"), ObjectPathOrEmpty(Type.PinSubCategoryObject.Get()))
		.AddString(TEXT("container"), ContainerToString(Type.ContainerType))
		.AddBool(TEXT("is_ref"), Type.bIsReference)
		.AddBool(TEXT("is_const"), Type.bIsConst);
	if (Type.ContainerType == EPinContainerType::Map)
	{
		FObjectBuilder Value;
		Value.AddString(TEXT("category"), Type.PinValueType.TerminalCategory.ToString())
			.AddString(TEXT("sub_category"), Type.PinValueType.TerminalSubCategory.IsNone()
				? FString() : Type.PinValueType.TerminalSubCategory.ToString())
			.AddString(TEXT("sub_category_object"), ObjectPathOrEmpty(Type.PinValueType.TerminalSubCategoryObject.Get()));
		Builder.Add(TEXT("value_type"), Value.Build());
	}
	return Builder.Build();
}

bool ReadPinType(FReader& Reader, FEdGraphPinType& OutType)
{
	if (!Reader.OnlyFields({TEXT("category"), TEXT("sub_category"), TEXT("sub_category_object"), TEXT("container"),
		TEXT("is_ref"), TEXT("is_const"), TEXT("value_type")}))
	{
		return false;
	}
	FString Category;
	FString SubCategory;
	FString SubObject;
	FString Container = TEXT("none");
	bool bRef = false;
	bool bConst = false;
	if (!Reader.String(TEXT("category"), Category) || !Reader.String(TEXT("sub_category"), SubCategory, false)
		|| !Reader.String(TEXT("sub_category_object"), SubObject, false)
		|| !Reader.String(TEXT("container"), Container, false)
		|| !Reader.Bool(TEXT("is_ref"), bRef) || !Reader.Bool(TEXT("is_const"), bConst))
	{
		return false;
	}
	OutType = FEdGraphPinType();
	OutType.PinCategory = FName(*Category);
	OutType.PinSubCategory = SubCategory.IsEmpty() ? NAME_None : FName(*SubCategory);
	if (!SubObject.IsEmpty())
	{
		UObject* Object = ResolveTypeObject(SubObject);
		if (Object == nullptr)
		{
			return Reader.Fail(TEXT("Graph.InvalidArgument"),
				FString::Printf(TEXT("sub_category_object '%s' does not resolve."), *SubObject), TEXT("sub_category_object"));
		}
		OutType.PinSubCategoryObject = Object;
	}
	if (Container.IsEmpty() || Container == TEXT("none"))
	{
		OutType.ContainerType = EPinContainerType::None;
	}
	else if (Container == TEXT("array"))
	{
		OutType.ContainerType = EPinContainerType::Array;
	}
	else if (Container == TEXT("set"))
	{
		OutType.ContainerType = EPinContainerType::Set;
	}
	else if (Container == TEXT("map"))
	{
		OutType.ContainerType = EPinContainerType::Map;
		const FValue* ValueType = nullptr;
		if (!Reader.Object(TEXT("value_type"), ValueType))
		{
			return false;
		}
		FReader Value = Reader.Child(TEXT("value_type"), *ValueType);
		FString VCategory;
		FString VSub;
		FString VObject;
		if (!Value.OnlyFields({TEXT("category"), TEXT("sub_category"), TEXT("sub_category_object")})
			|| !Value.String(TEXT("category"), VCategory) || !Value.String(TEXT("sub_category"), VSub, false)
			|| !Value.String(TEXT("sub_category_object"), VObject, false))
		{
			return false;
		}
		OutType.PinValueType.TerminalCategory = FName(*VCategory);
		OutType.PinValueType.TerminalSubCategory = VSub.IsEmpty() ? NAME_None : FName(*VSub);
		OutType.PinValueType.TerminalSubCategoryObject = ResolveTypeObject(VObject);
	}
	else
	{
		return Reader.Fail(TEXT("Graph.InvalidArgument"), TEXT("container must be none/array/set/map."), TEXT("container"));
	}
	OutType.bIsReference = bRef;
	OutType.bIsConst = bConst;
	return true;
}

FValue MakePinRef(const UEdGraphPin& Pin)
{
	const UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
	return FObjectBuilder()
		.AddString(TEXT("node_guid"), Node != nullptr ? FormatAssetDocumentGraphGuid(Node->NodeGuid) : FString())
		.AddString(TEXT("pin_name"), Pin.PinName.ToString())
		.Build();
}

FValue MakeLinkValue(const UEdGraphPin& A, const UEdGraphPin& B)
{
	const UEdGraphPin& From = A.Direction == EGPD_Output ? A : B;
	const UEdGraphPin& To = A.Direction == EGPD_Output ? B : A;
	return FObjectBuilder().Add(TEXT("from"), MakePinRef(From)).Add(TEXT("to"), MakePinRef(To)).Build();
}

FValue MakeNodeSnapshot(const FGraphEntry& Entry, UEdGraphNode& Node, bool bIncludeProperties, bool bIncludeSize)
{
	FAssetDocumentGraphNodeDescription Description;
	if (Entry.Schema.IsValid())
	{
		Entry.Schema->DescribeNode(Node, Description);
	}
	TArray<FValue> SubgraphGuids;
	for (const UEdGraph* Subgraph : OwnedSubgraphs(Entry.Schema, Node))
	{
		if (Subgraph != nullptr)
		{
			SubgraphGuids.Add(MakeString(FormatAssetDocumentGraphGuid(Entry.GuidOf(Subgraph))));
		}
	}
	TArray<FValue> Pins;
	for (const UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin == nullptr)
		{
			continue;
		}
		TArray<FValue> Linked;
		for (const UEdGraphPin* Other : Pin->LinkedTo)
		{
			if (Other != nullptr)
			{
				Linked.Add(MakePinRef(*Other));
			}
		}
		FObjectBuilder PinBuilder;
		PinBuilder.AddString(TEXT("name"), Pin->PinName.ToString())
			.AddString(TEXT("direction"), DirectionToString(Pin->Direction))
			.Add(TEXT("type"), MakePinTypeValue(Pin->PinType))
			.AddString(TEXT("default_value"), Pin->DefaultValue)
			.AddString(TEXT("default_object"), ObjectPathOrEmpty(Pin->DefaultObject))
			.AddString(TEXT("default_text"), Pin->DefaultTextValue.ToString())
			.AddBool(TEXT("is_autogenerated_default"), Pin->DoesDefaultValueMatchAutogenerated())
			.AddBool(TEXT("hidden"), Pin->bHidden)
			.AddBool(TEXT("advanced"), Pin->bAdvancedView)
			.AddBool(TEXT("orphaned"), Pin->bOrphanedPin)
			.Add(TEXT("linked_to"), MakeArray(MoveTemp(Linked)));
		// The editor's label when the node names the pin apart from its name (a user struct member's
		// friendly name on its GUID-named pin, Get Data Table Row's "Row Found"): authors may use it.
		if (!Pin->PinFriendlyName.IsEmpty() && Pin->PinFriendlyName.ToString() != Pin->PinName.ToString())
		{
			PinBuilder.AddString(TEXT("display_name"), Pin->PinFriendlyName.ToString());
		}
		// An enum pin's value as the editor's dropdown shows it, when that differs from the stored
		// enumerator name (a User Defined Enum's display name "Ground" for NewEnumerator0).
		if (const UEnum* Enum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()); Enum != nullptr && !Pin->DefaultValue.IsEmpty())
		{
			const int32 EnumIndex = Enum->GetIndexByNameString(Pin->DefaultValue);
			const FString Authored = EnumIndex != INDEX_NONE ? Enum->GetAuthoredNameStringByIndex(EnumIndex) : FString();
			if (!Authored.IsEmpty() && Authored != Pin->DefaultValue)
			{
				PinBuilder.AddString(TEXT("default_display"), Authored);
			}
		}
		Pins.Add(PinBuilder.Build());
	}
	FValue Extras = Description.Extras.Kind == EAssetDocumentRuntimeValueKind::Object
		? Description.Extras : MakeEmptyObject();
	const bool bCanAddPin = Entry.Schema.IsValid() && Entry.Schema->CanAddDynamicPin(Node);
	FObjectBuilder Builder;
	Builder.AddString(TEXT("node_guid"), FormatAssetDocumentGraphGuid(Node.NodeGuid))
		.AddString(TEXT("class"), Node.GetClass()->GetPathName())
		.Add(TEXT("action_key"), Description.ActionKey.IsEmpty() ? MakeNull() : MakeString(Description.ActionKey))
		.AddString(TEXT("title"), Node.GetNodeTitle(ENodeTitleType::ListView).ToString())
		.Add(TEXT("implicit_role"), Description.ImplicitRole.IsEmpty() ? MakeNull() : MakeString(Description.ImplicitRole))
		.Add(TEXT("position"), FObjectBuilder().AddInt(TEXT("x"), Node.NodePosX).AddInt(TEXT("y"), Node.NodePosY).Build())
		.AddString(TEXT("comment"), Node.NodeComment)
		.AddString(TEXT("enabled_state"), EnabledStateToString(Node.GetDesiredEnabledState()))
		.Add(TEXT("parent_node_guid"), Description.ParentNode != nullptr
			? MakeString(FormatAssetDocumentGraphGuid(Description.ParentNode->NodeGuid)) : MakeNull())
		.Add(TEXT("subnode_index"), Description.SubnodeIndex.IsSet() ? MakeInt(Description.SubnodeIndex.GetValue()) : MakeNull())
		.Add(TEXT("subgraph_guids"), MakeArray(MoveTemp(SubgraphGuids)))
		.AddBool(TEXT("can_add_pin"), bCanAddPin)
		.Add(TEXT("extras"), MoveTemp(Extras))
		.Add(TEXT("pins"), MakeArray(MoveTemp(Pins)));
	// The document Id and authored action key the node was last applied with (graph.finalize.v1 node_meta).
	const FString DocumentId = RawEdits::GetNodeDocumentId(Node);
	if (!DocumentId.IsEmpty())
	{
		Builder.AddString(TEXT("doc_id"), DocumentId);
		const FString DocumentAction = RawEdits::GetNodeDocumentAction(Node);
		if (!DocumentAction.IsEmpty())
		{
			Builder.AddString(TEXT("doc_action"), DocumentAction);
		}
	}
#if WITH_EDITORONLY_DATA
	// The node's comment bubble pinned open (SCommentBubble's toggle; the document's CommentBubbleVisible),
	// reported when shown.
	if (Node.bCommentBubbleVisible)
	{
		Builder.AddBool(TEXT("comment_bubble_visible"), true);
	}
	if (Node.bCanResizeNode)
	{
		// A resizable node (a comment box): its size is the user's (SGraphNodeResizable).
		Builder.Add(TEXT("box_size"), FObjectBuilder().AddInt(TEXT("width"), Node.NodeWidth).AddInt(TEXT("height"), Node.NodeHeight).Build());
	}
#endif
	FVector2D WidgetSize;
	if (bIncludeSize && MeasureGraphNodeWidget(Node, WidgetSize))
	{
		Builder.Add(TEXT("widget_size"), FObjectBuilder()
			.AddInt(TEXT("width"), FMath::CeilToInt(WidgetSize.X)).AddInt(TEXT("height"), FMath::CeilToInt(WidgetSize.Y)).Build());
	}
	if (bIncludeProperties)
	{
		FObjectBuilder Properties;
		Properties.Add(TEXT("node"), EncodeEditableProperties(Node));
		if (UObject* Template = Entry.Schema.IsValid() ? Entry.Schema->ResolvePropertyContainer(Node, TEXTVIEW("template")) : nullptr)
		{
			Properties.Add(TEXT("template"), EncodeEditableProperties(*Template));
		}
		Builder.Add(TEXT("properties"), Properties.Build());
	}
	return Builder.Build();
}

FValue MakeGraphListEntry(const FGraphEntry& Entry)
{
	const UEdGraphSchema* Schema = Entry.Graph->GetSchema();
	FObjectBuilder Builder;
	Builder.AddString(TEXT("graph_guid"), FormatAssetDocumentGraphGuid(Entry.Guid()))
		.AddString(TEXT("name"), Entry.Graph->GetName())
		.AddString(TEXT("path"), Entry.Path)
		.AddString(TEXT("role"), Entry.Role)
		.AddString(TEXT("schema_class"), ObjectPathOrEmpty(Schema != nullptr ? Schema->GetClass() : nullptr))
		.AddString(TEXT("schema_tag"), Entry.SchemaTag);
	Builder.Add(TEXT("parent_graph_guid"), Entry.ParentGraph != nullptr
		? MakeString(FormatAssetDocumentGraphGuid(Entry.GuidOf(Entry.ParentGraph))) : MakeNull());
	Builder.Add(TEXT("owner_node_guid"), Entry.OwnerNode != nullptr
		? MakeString(FormatAssetDocumentGraphGuid(Entry.OwnerNode->NodeGuid)) : MakeNull());
	Builder.Add(TEXT("caps"), FObjectBuilder()
		.AddBool(TEXT("spawn"), Entry.bCanSpawn)
		.AddBool(TEXT("rename"), Entry.bCanRename)
		.AddBool(TEXT("remove"), Entry.bCanRemove)
		.Build());
	return Builder.Build();
}
} // namespace UE::AssetDocument::Graph
