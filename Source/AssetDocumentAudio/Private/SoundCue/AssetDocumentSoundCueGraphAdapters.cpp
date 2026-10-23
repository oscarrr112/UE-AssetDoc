// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// SoundCue adapters of the shared graph primitives (provider "graph", AssetDocumentGraph; design §4.1):
//   schema adapter  USoundCueGraphSchema graphs. Actions are the palette's (SoundCue:Node:<sound node class path>, one
//                   per class the palette lists, and SoundCue:Comment) and spawn through the schema's own actions:
//                   FSoundCueGraphSchemaAction_NewNode::PerformAction (the struct is reflected; its header includes a
//                   private AudioEditor header, so it is created through its UScriptStruct and called through the
//                   FEdGraphSchemaAction interface) and the schema's comment action (GetCreateCommentAction). Delete is
//                   the editor's Delete (raw replica, AssetDocumentSoundCueRawEdits.cpp); Add / Delete Input are the
//                   node's context menu (USoundCueGraphNode::AddInputPin / RemoveInputPin); the node Details panel is
//                   graph.node.property.set.v1 with container "sound_node" (the graph node's SoundNode); a comment
//                   box's size is the node setting "Size" ([w, h], UEdGraphNode_Comment::ResizeNode).
//                   graph.node.rename.v1 renames the node's identity: the sound node object (comment: the comment
//                   node) gets the document key as its name.
//   host adapter    USoundCue: its one graph (path "SoundCue", role "other"). Finalize is the compile every editor
//                   edit ends with plus the validation of AssetDocumentSoundCueValidation.cpp.
//
// graph.query extras of a sound node: {name (sound node object name = key), class (document class label),
//   input_pins [names in slot order], child_limits {min, max}, reachable (linked to the Output node),
//   properties (Details values that differ from a new node with as many inputs), comment_bubble_visible};
// of a comment box: {name, size [w, h], properties (CommentColor, FontSize, bCommentBubbleVisible_InDetailsPanel,
//   bColorCommentBubble, MoveMode, NodeDetails that differ from a new comment)}; the Output node has
//   implicit_role "root". Input pin i also answers to the pin name "#<i>" (pin names are derived labels).

#include "AssetDocumentSoundCue.h"

#include "AssetDocumentGraphAdapters.h"
#include "AssetDocumentGraphValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "SoundCueGraph/SoundCueGraph.h"
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "SoundCueGraph/SoundCueGraphNode_Root.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Audio::SoundCue
{
namespace
{
const TCHAR* const NodeKeyPrefix = TEXT("SoundCue:Node:");
const TCHAR* const CommentKey = TEXT("SoundCue:Comment");

FString NodeActionKey(const UClass& Class)
{
	return FString(NodeKeyPrefix) + Class.GetPathName();
}

// FSoundCueGraphSchemaAction_NewNode for Class, made through its reflected struct (constructor, SoundNodeClass) and
// destroyed with it.
class FNewNodeAction
{
public:
	explicit FNewNodeAction(UClass& Class)
	{
		Struct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/AudioEditor.SoundCueGraphSchemaAction_NewNode"));
		if (Struct == nullptr || !Struct->IsChildOf(FEdGraphSchemaAction::StaticStruct()))
		{
			Struct = nullptr;
			return;
		}
		const FClassProperty* Property = CastField<FClassProperty>(Struct->FindPropertyByName(TEXT("SoundNodeClass")));
		if (Property == nullptr)
		{
			Struct = nullptr;
			return;
		}
		Memory = static_cast<uint8*>(FMemory::Malloc(Struct->GetStructureSize(), Struct->GetMinAlignment()));
		Struct->InitializeStruct(Memory);
		Property->SetObjectPropertyValue_InContainer(Memory, &Class);
	}

	~FNewNodeAction()
	{
		if (Memory != nullptr)
		{
			Struct->DestroyStruct(Memory);
			FMemory::Free(Memory);
		}
	}

	FNewNodeAction(const FNewNodeAction&) = delete;
	FNewNodeAction& operator=(const FNewNodeAction&) = delete;

	FEdGraphSchemaAction* Get() const { return reinterpret_cast<FEdGraphSchemaAction*>(Memory); }

private:
	UScriptStruct* Struct = nullptr;
	uint8* Memory = nullptr;
};

bool IsPaletteClass(const UClass& Class)
{
	return GetPaletteClasses().Contains(&Class);
}

FValue Pair(int32 X, int32 Y)
{
	return MakeArray({MakeInt(X), MakeInt(Y)});
}

// Graph nodes reachable from the Output node through input links.
TSet<const UEdGraphNode*> ReachableNodes(const UEdGraph& Graph)
{
	TSet<const UEdGraphNode*> Reachable;
	TArray<const UEdGraphNode*> Pending;
	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr && Node->IsA<USoundCueGraphNode_Root>())
		{
			Pending.Add(Node);
		}
	}
	while (!Pending.IsEmpty())
	{
		const UEdGraphNode* Node = Pending.Pop();
		if (Reachable.Contains(Node))
		{
			continue;
		}
		Reachable.Add(Node);
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Input)
			{
				continue;
			}
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked != nullptr && Linked->GetOwningNodeUnchecked() != nullptr)
				{
					Pending.Add(Linked->GetOwningNodeUnchecked());
				}
			}
		}
	}
	return Reachable;
}

bool ParseSize(const FValue& Value, FVector2f& OutSize)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Array || Value.Elements.Num() != 2)
	{
		return false;
	}
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const FValue& Element = Value.Elements[Index];
		double Number = 0.0;
		if (Element.Kind == EAssetDocumentRuntimeValueKind::Integer)
		{
			Number = static_cast<double>(Element.IntegerValue);
		}
		else if (Element.Kind == EAssetDocumentRuntimeValueKind::Number)
		{
			Number = Element.NumberValue;
		}
		else
		{
			return false;
		}
		if (!FMath::IsFinite(Number) || Number < 1.0 || Number > 1.0e6)
		{
			return false;
		}
		OutSize[Index] = static_cast<float>(Number);
	}
	return true;
}

class FSoundCueSchemaAdapter final : public IAssetDocumentGraphSchemaAdapter
{
public:
	virtual UClass* GetSchemaClass() const override
	{
		return SoundCue::GetSchemaClass();
	}

	virtual FString Tag(const UEdGraph& Graph) const override
	{
		return TEXT("SoundCue");
	}

	virtual bool EnumerateActions(const FAssetDocumentGraphActionQuery& Query, TArray<FAssetDocumentGraphActionRecord>& OutActions,
		FAssetDocumentGraphError& OutError) const override
	{
		for (UClass* Class : GetPaletteClasses())
		{
			// USoundCueGraphSchema::GetAllSoundNodeActions: from an output pin every class but the players (no inputs).
			if (Query.FromPin != nullptr && Query.FromPin->Direction == EGPD_Output && GetMaxInputs(*Class) == 0)
			{
				continue;
			}
			FAssetDocumentGraphActionRecord& Record = OutActions.AddDefaulted_GetRef();
			Record.ActionKey = NodeActionKey(*Class);
			Record.DisplayName = Class->GetDescription();
			Record.Category = TEXT("Sound Node");
			Record.Tooltip = FString::Printf(TEXT("Adds %s node here"), *Record.DisplayName);
			Record.NodeClassPath = USoundCueGraphNode::StaticClass()->GetPathName();
		}
		if (Query.FromPin == nullptr)
		{
			FAssetDocumentGraphActionRecord& Record = OutActions.AddDefaulted_GetRef();
			Record.ActionKey = CommentKey;
			Record.DisplayName = TEXT("Add Comment...");
			Record.Tooltip = TEXT("Creates a comment.");
			Record.NodeClassPath = UEdGraphNode_Comment::StaticClass()->GetPathName();
		}
		return true;
	}

	virtual bool ResolveAndSpawn(const FAssetDocumentGraphSpawnRequest& Request, FAssetDocumentGraphSpawnResult& OutResult,
		FAssetDocumentGraphError& OutError) const override
	{
		USoundCueGraph* Graph = Cast<USoundCueGraph>(Request.Graph);
		if (Graph == nullptr || CueOfGraph(Graph) == nullptr)
		{
			OutError.Set(TEXT("Graph.GraphNotFound"), TEXT("Not a Sound Cue graph."));
			return false;
		}
		const FVector2D Position = Request.Position.Get(FVector2D::ZeroVector);
		const FVector2f Location(static_cast<float>(Position.X), static_cast<float>(Position.Y));
		UClass* Class = nullptr;
		if (Request.ActionKey != CommentKey)
		{
			Class = Request.ActionKey.StartsWith(NodeKeyPrefix) ? ResolveClassLabel(Request.ActionKey.RightChop(FCString::Strlen(NodeKeyPrefix))) : nullptr;
			if (Class == nullptr || !IsPaletteClass(*Class))
			{
				OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("No action has key %s in the Sound Cue palette."), *Request.ActionKey));
				return false;
			}
		}
		UEdGraphNode* Node = PerformPaletteAction(*Graph, Class, Location);
		if (Node == nullptr)
		{
			OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("Action %s did not produce a node."), *Request.ActionKey));
			return false;
		}
		OutResult.Node = Node;
		return true;
	}

	virtual void DescribeNode(const UEdGraphNode& Node, FAssetDocumentGraphNodeDescription& Out) const override
	{
		FObjectBuilder Extras;
		if (Node.IsA<USoundCueGraphNode_Root>())
		{
			Out.ImplicitRole = TEXT("root");
		}
		else if (const USoundCueGraphNode* SoundGraphNode = Cast<USoundCueGraphNode>(&Node))
		{
			const USoundNode* SoundNode = SoundGraphNode->SoundNode;
			if (SoundNode != nullptr)
			{
				Out.ActionKey = NodeActionKey(*SoundNode->GetClass());
				Extras.AddString(TEXT("name"), SoundNode->GetName());
				Extras.AddString(TEXT("class"), ClassLabel(*SoundNode->GetClass()));
				Extras.Add(TEXT("child_limits"), FObjectBuilder().AddInt(TEXT("min"), SoundNode->GetMinChildNodes())
					.AddInt(TEXT("max"), SoundNode->GetMaxChildNodes()).Build());
				Extras.Add(TEXT("properties"), EncodeChangedProperties(*SoundNode));
			}
			TArray<UEdGraphPin*> Inputs;
			const_cast<USoundCueGraphNode*>(SoundGraphNode)->GetInputPins(Inputs);
			TArray<FString> Names;
			for (const UEdGraphPin* Pin : Inputs)
			{
				Names.Add(Pin->PinName.ToString());
			}
			Extras.Add(TEXT("input_pins"), MakeStringArray(Names));
			const UEdGraph* Graph = Node.GetGraph();
			Extras.AddBool(TEXT("reachable"), Graph != nullptr && ReachableNodes(*Graph).Contains(&Node));
			Extras.AddBool(TEXT("comment_bubble_visible"), Node.bCommentBubbleVisible);
		}
		else if (Node.IsA<UEdGraphNode_Comment>())
		{
			Out.ActionKey = CommentKey;
			Extras.AddString(TEXT("name"), Node.GetName());
			Extras.Add(TEXT("size"), Pair(Node.NodeWidth, Node.NodeHeight));
			Extras.Add(TEXT("properties"), EncodeChangedCommentProperties(Node));
		}
		Out.Extras = Extras.Build();
	}

	virtual UObject* ResolvePropertyContainer(UEdGraphNode& Node, FStringView Container) const override
	{
		if (Container == TEXTVIEW("sound_node"))
		{
			USoundCueGraphNode* SoundGraphNode = Cast<USoundCueGraphNode>(&Node);
			return SoundGraphNode != nullptr ? SoundGraphNode->SoundNode.Get() : nullptr;
		}
		return nullptr;
	}

	virtual bool RemoveNode(UObject& Asset, UEdGraphNode& Node, FAssetDocumentGraphError& OutError) const override
	{
		USoundCue* Cue = CueOfGraph(Node.GetGraph());
		FString Error;
		if (Cue == nullptr || !DeleteNodeAsTheEditorDoes(*Cue, Node, Error))
		{
			OutError.Set(TEXT("Graph.NodeNotDeletable"), Error.IsEmpty() ? FString(TEXT("Not a Sound Cue graph node.")) : Error, TEXT("/node_guid"));
			return false;
		}
		return true;
	}

	virtual bool CanAddDynamicPin(const UEdGraphNode& Node) const override
	{
		const USoundCueGraphNode* SoundGraphNode = Cast<USoundCueGraphNode>(&Node);
		return SoundGraphNode != nullptr && SoundGraphNode->CanAddInputPin();
	}

	virtual bool AddDynamicPin(UEdGraphNode& Node, FAssetDocumentGraphError& OutError) const override
	{
		USoundCueGraphNode* SoundGraphNode = Cast<USoundCueGraphNode>(&Node);
		if (SoundGraphNode == nullptr || SoundGraphNode->SoundNode == nullptr)
		{
			OutError.Set(TEXT("Graph.DynamicPinUnsupported"), TEXT("Only sound nodes have inputs to add."), TEXT("/node_guid"));
			return false;
		}
		if (!SoundGraphNode->CanAddInputPin())
		{
			const USoundNode* SoundNode = SoundGraphNode->SoundNode;
			OutError.Set(TEXT("SoundCue.ChildCount"), FString::Printf(TEXT("%s takes at most %d input(s) (it has %d)."),
				*ClassLabel(*SoundNode->GetClass()), SoundNode->GetMaxChildNodes(), SoundNode->ChildNodes.Num()), TEXT("/node_guid"));
			OutError.AddContext(TEXT("min"), MakeInt(SoundNode->GetMinChildNodes()));
			OutError.AddContext(TEXT("max"), MakeInt(SoundNode->GetMaxChildNodes()));
			return false;
		}
		// The node context menu's "Add Input" (USoundCueGraphNode::GetNodeContextMenuActions).
		SoundGraphNode->AddInputPin();
		return true;
	}

	virtual bool RemoveDynamicPin(UEdGraphNode& Node, UEdGraphPin* Pin, FAssetDocumentGraphError& OutError) const override
	{
		USoundCueGraphNode* SoundGraphNode = Cast<USoundCueGraphNode>(&Node);
		if (SoundGraphNode == nullptr || SoundGraphNode->SoundNode == nullptr || Pin == nullptr || Pin->Direction != EGPD_Input)
		{
			OutError.Set(TEXT("Graph.DynamicPinUnsupported"), TEXT("Only a sound node's input pins can be deleted."), TEXT("/pin_name"));
			return false;
		}
		const USoundNode* SoundNode = SoundGraphNode->SoundNode;
		// The context menu offers "Delete Input" only above the node's minimum (USoundCueGraphNode::GetNodeContextMenuActions).
		if (SoundNode->ChildNodes.Num() <= SoundNode->GetMinChildNodes())
		{
			OutError.Set(TEXT("SoundCue.ChildCount"), FString::Printf(TEXT("%s takes at least %d input(s)."),
				*ClassLabel(*SoundNode->GetClass()), SoundNode->GetMinChildNodes()), TEXT("/pin_name"));
			OutError.AddContext(TEXT("min"), MakeInt(SoundNode->GetMinChildNodes()));
			OutError.AddContext(TEXT("max"), MakeInt(SoundNode->GetMaxChildNodes()));
			return false;
		}
		SoundGraphNode->RemoveInputPin(Pin);
		return true;
	}

	virtual bool SetNodeSetting(UObject& Asset, UEdGraphNode& Node, FStringView Path, const FAssetDocumentRuntimeValueDto& Value,
		bool& bOutHandled, FAssetDocumentGraphError& OutError) const override
	{
		bOutHandled = false;
		if (Path != TEXTVIEW("Size") || !Node.IsA<UEdGraphNode_Comment>())
		{
			return true;
		}
		bOutHandled = true;
		FVector2f Size;
		if (!ParseSize(Value, Size))
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), TEXT("Size is [width, height] (positive numbers)."), TEXT("/value"));
			return false;
		}
		// Dragging the comment box's corner (SGraphNodeComment -> UEdGraphNode_Comment::ResizeNode).
		Node.Modify();
		Node.ResizeNode(Size);
		return true;
	}

	virtual UEdGraphPin* ResolvePinAlias(UEdGraphNode& Node, FName Name, EEdGraphPinDirection Direction) const override
	{
		USoundCueGraphNode_Base* Base = Cast<USoundCueGraphNode_Base>(&Node);
		const FString Text = Name.ToString();
		if (Base == nullptr || Direction != EGPD_Input || !Text.StartsWith(TEXT("#")) || !Text.RightChop(1).IsNumeric())
		{
			return nullptr;
		}
		const int32 Index = FCString::Atoi(*Text.RightChop(1));
		TArray<UEdGraphPin*> Inputs;
		Base->GetInputPins(Inputs);
		return Inputs.IsValidIndex(Index) ? Inputs[Index] : nullptr;
	}

	virtual bool RenameNode(UObject& Asset, UEdGraphNode& Node, const FString& Name, bool& bOutHandled, FAssetDocumentGraphError& OutError) const override
	{
		bOutHandled = true;
		USoundCue* Cue = CueOfGraph(Node.GetGraph());
		FString Error;
		if (Cue == nullptr || !RenameNodeIdentity(*Cue, Node, Name, Error))
		{
			OutError.Set(TEXT("SoundCue.InvalidKey"), Error.IsEmpty() ? FString(TEXT("Not a Sound Cue graph node.")) : Error, TEXT("/name"));
			return false;
		}
		return true;
	}
};

class FSoundCueHostAdapter final : public IAssetDocumentGraphHostAdapter
{
public:
	virtual UClass* GetAssetClass() const override
	{
		return USoundCue::StaticClass();
	}

	virtual void EnumerateGraphs(UObject& Asset, TArray<FAssetDocumentGraphInfo>& OutGraphs) const override
	{
		USoundCue* Cue = Cast<USoundCue>(&Asset);
		if (Cue == nullptr || Cue->SoundCueGraph == nullptr)
		{
			return;
		}
		FAssetDocumentGraphInfo& Info = OutGraphs.AddDefaulted_GetRef();
		Info.Graph = Cue->SoundCueGraph;
		Info.Path = TEXT("SoundCue");
		Info.Role = TEXT("other");
		// Template cues rebuild their graph from their settings (design §1): readable, not editable.
		Info.bCanSpawn = IsEditableCue(Asset);
		Info.bCanRename = false;
		Info.bCanRemove = false;
	}

	virtual bool Finalize(UObject& Asset, FAssetDocumentGraphFinalizeResult& OutResult, FAssetDocumentGraphError& OutError) const override
	{
		USoundCue* Cue = Cast<USoundCue>(&Asset);
		if (Cue == nullptr || Cue->SoundCueGraph == nullptr)
		{
			OutError.Set(TEXT("Graph.GraphNotFound"), TEXT("Not a Sound Cue with a graph."));
			return false;
		}
		// What every structural edit of the Sound Cue editor ends with, once more, then the cue's PostEditChange
		// (CacheAggregateValues, RecursivelySetExcludeBranchCulling) and the graph panel refresh.
		Cue->CompileSoundNodesFromGraphNodes();
		Cue->PostEditChange();
		Cue->SoundCueGraph->NotifyGraphChanged();
		TArray<FValidationMessage> Messages;
		Validate(*Cue, Messages);
		bool bErrors = false;
		bool bWarnings = false;
		for (const FValidationMessage& Message : Messages)
		{
			FAssetDocumentGraphFinalizeMessage& Entry = OutResult.Messages.AddDefaulted_GetRef();
			Entry.Severity = Message.Severity;
			Entry.Message = Message.Message;
			Entry.NodeGuid = Message.Node != nullptr ? Message.Node->NodeGuid : FGuid();
			Entry.PinName = Message.Pin;
			bErrors |= Message.Severity == TEXT("error");
			bWarnings |= Message.Severity == TEXT("warning");
		}
		OutResult.Status = bErrors ? TEXT("errors") : bWarnings ? TEXT("warnings") : TEXT("ok");
		return true;
	}

	virtual void NotifyGraphChanged(UObject& Asset, UEdGraph* Graph, bool bStructural) const override
	{
		// FSoundCueEditor::NotifyPostChange / PostUndo: the open graph panel refreshes.
		if (Graph != nullptr)
		{
			Graph->NotifyGraphChanged();
		}
	}
};

TSharedPtr<FSoundCueSchemaAdapter> SchemaAdapter;
TSharedPtr<FSoundCueHostAdapter> HostAdapter;
} // namespace

UEdGraphNode* PerformPaletteAction(UEdGraph& Graph, UClass* NodeClass, const FVector2f& Location)
{
	if (NodeClass == nullptr)
	{
		const UEdGraphSchema* Schema = Graph.GetSchema();
		const TSharedPtr<FEdGraphSchemaAction> Action = Schema != nullptr ? Schema->GetCreateCommentAction() : nullptr;
		return Action.IsValid() ? Action->PerformAction(&Graph, nullptr, Location, /*bSelectNewNode=*/false) : nullptr;
	}
	const FNewNodeAction Action(*NodeClass);
	// The palette / context menu: no FromPin (no wire dragged), nothing selected (no editor open).
	return Action.Get() != nullptr ? Action.Get()->PerformAction(&Graph, nullptr, Location, /*bSelectNewNode=*/false) : nullptr;
}

void RegisterGraphAdapters()
{
	SchemaAdapter = MakeShared<FSoundCueSchemaAdapter>();
	HostAdapter = MakeShared<FSoundCueHostAdapter>();
	RegisterAssetDocumentGraphSchemaAdapter(SchemaAdapter.ToSharedRef());
	RegisterAssetDocumentGraphHostAdapter(HostAdapter.ToSharedRef());
}

void UnregisterGraphAdapters()
{
	if (HostAdapter.IsValid())
	{
		UnregisterAssetDocumentGraphHostAdapter(HostAdapter.ToSharedRef());
		HostAdapter.Reset();
	}
	if (SchemaAdapter.IsValid())
	{
		UnregisterAssetDocumentGraphSchemaAdapter(SchemaAdapter.ToSharedRef());
		SchemaAdapter.Reset();
	}
}
} // namespace UE::AssetDocument::Graph::Audio::SoundCue
