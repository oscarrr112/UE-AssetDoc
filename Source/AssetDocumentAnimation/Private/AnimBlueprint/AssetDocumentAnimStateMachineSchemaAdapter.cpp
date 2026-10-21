// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// State machine schema adapter (UAnimationStateMachineSchema). Keys:
//   SM:State, SM:Conduit, SM:StateAlias   the graph context menu's "Add State/Conduit/State Alias"
//   SM:Transition                          the transition node the editor creates when two states
//                                          are wired; it is linked FromState:Out -> T:In, T:Out -> ToState:In
//   SM:Entry                               the implicit entry node (implicit role "sm_entry", pin "Entry")
// States, conduits and aliases report extras.name (their state name; renamed with
// graph.node.rename.v1); transitions report extras.from / extras.to (state names).
// extras.properties holds the editable node properties that differ from a newly placed node's and, for
// aliases, "AliasedStates" (state names), which SetNodeSetting applies; extras.property_defaults the new
// node's value of each (an omitted setting is reset to it, as "reset to default" in the Details panel).

#include "AnimBlueprint/AssetDocumentAnimBlueprintAdapters.h"

#include "Algo/Find.h"
#include "Animation/AnimTypes.h"
#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineSchema.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::AnimBlueprint
{
namespace StateMachineAdapterDetail
{
struct FStateKind
{
	const TCHAR* Key;
	UClass* (*Class)();
	const TCHAR* Display;
};

const FStateKind Kinds[] = {
	{TEXT("SM:State"), &UAnimStateNode::StaticClass, TEXT("Add State")},
	{TEXT("SM:Conduit"), &UAnimStateConduitNode::StaticClass, TEXT("Add Conduit")},
	{TEXT("SM:StateAlias"), &UAnimStateAliasNode::StaticClass, TEXT("Add State Alias")},
	{TEXT("SM:Transition"), &UAnimStateTransitionNode::StaticClass, TEXT("Transition (wire two states)")},
};

FString KeyOf(const UEdGraphNode& Node)
{
	if (Node.IsA<UAnimStateEntryNode>())
	{
		return TEXT("SM:Entry");
	}
	for (const FStateKind& Kind : Kinds)
	{
		if (Node.GetClass() == Kind.Class())
		{
			return Kind.Key;
		}
	}
	return FString();
}

bool IsEditable(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_Edit)
		&& !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated | CPF_DisableEditOnInstance);
}

TArray<UAnimStateNodeBase*> NamedStates(const UEdGraph& Graph)
{
	TArray<UAnimStateNodeBase*> States;
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr && (Node->IsA<UAnimStateNode>() || Node->IsA<UAnimStateConduitNode>()))
		{
			States.Add(CastChecked<UAnimStateNodeBase>(Node));
		}
	}
	return States;
}

FValue AliasedStates(const UAnimStateAliasNode& Alias)
{
	TArray<FString> Names;
	for (const TWeakObjectPtr<UAnimStateNodeBase>& State : Alias.GetAliasedStates())
	{
		if (State.IsValid())
		{
			Names.Add(State->GetStateName());
		}
	}
	Names.Sort();
	TArray<FValue> Values;
	for (FString& Name : Names)
	{
		Values.Add(MakeStringValue(MoveTemp(Name)));
	}
	return MakeArrayValue(MoveTemp(Values));
}

// Transition events (FAnimNotifyEvent) carry an editor Guid that every construction
// regenerates; it is identity, not authored state, so it does not make an event
// "changed" (a reloaded or snapshot-restored transition would otherwise report all
// three events).
bool IsDefault(const FProperty& Property, const void* Value, const void* Default)
{
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	if (Struct == nullptr || Struct->Struct != FAnimNotifyEvent::StaticStruct())
	{
		return Property.Identical(Value, Default, PPF_None);
	}
	for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
	{
		if (It->GetFName() != TEXT("Guid")
			&& !It->Identical(It->ContainerPtrToValuePtr<void>(Value), It->ContainerPtrToValuePtr<void>(Default), PPF_None))
		{
			return false;
		}
	}
	return true;
}

// A newly placed node's value of Property: the class default, but what PostPlacedNewNode sets (a new transition
// allows inertialization for self transitions). Scratch holds an overridden bool.
const void* NewNodeValue(const UEdGraphNode& Node, const FProperty& Property, bool& Scratch)
{
	if (Node.IsA<UAnimStateTransitionNode>() && Property.GetFName() == FName(TEXT("bAllowInertializationForSelfTransitions"))
		&& CastField<FBoolProperty>(&Property) != nullptr && CastField<FBoolProperty>(&Property)->IsNativeBool())
	{
		Scratch = true;
		return &Scratch;
	}
	return Property.ContainerPtrToValuePtr<void>(Node.GetClass()->GetDefaultObject());
}

// The editable properties that differ from a newly placed node's (and, for aliases, "AliasedStates"); OutDefaults:
// the new node's value of each (a setting the document leaves out is reset to it).
FValue CollectProperties(const UEdGraphNode& Node, FValue& OutDefaults)
{
	FValue Out = MakeObjectValue();
	OutDefaults = MakeObjectValue();
	for (TFieldIterator<FProperty> It(Node.GetClass()); It; ++It)
	{
		if (!IsEditable(**It))
		{
			continue;
		}
		bool Scratch = false;
		const void* Value = It->ContainerPtrToValuePtr<void>(&Node);
		const void* Default = NewNodeValue(Node, **It, Scratch);
		if (!IsDefault(**It, Value, Default))
		{
			SetField(Out, It->GetName(), EncodeAssetDocumentGraphPropertyValue(**It, Value));
			SetField(OutDefaults, It->GetName(), EncodeAssetDocumentGraphPropertyValue(**It, Default));
		}
	}
	if (const UAnimStateAliasNode* Alias = Cast<UAnimStateAliasNode>(&Node); Alias != nullptr && !Alias->GetAliasedStates().IsEmpty())
	{
		SetField(Out, TEXT("AliasedStates"), AliasedStates(*Alias));
		SetField(OutDefaults, TEXT("AliasedStates"), MakeArrayValue());
	}
	return Out;
}

class FStateMachineSchemaAdapter final : public IAssetDocumentGraphSchemaAdapter
{
public:
	virtual UClass* GetSchemaClass() const override
	{
		return UAnimationStateMachineSchema::StaticClass();
	}

	virtual FString Tag(const UEdGraph& Graph) const override
	{
		return TEXT("SM");
	}

	virtual bool EnumerateActions(
		const FAssetDocumentGraphActionQuery& Query,
		TArray<FAssetDocumentGraphActionRecord>& OutActions,
		FAssetDocumentGraphError& OutError) const override
	{
		const TSet<FString> Exact(Query.ExactKeys);
		for (const FStateKind& Kind : Kinds)
		{
			const FString Key = Kind.Key;
			if ((!Query.KeyPrefix.IsEmpty() && !Key.StartsWith(Query.KeyPrefix)) || (!Exact.IsEmpty() && !Exact.Contains(Key)))
			{
				continue;
			}
			FAssetDocumentGraphActionRecord& Record = OutActions.AddDefaulted_GetRef();
			Record.ActionKey = Key;
			Record.NodeClassPath = Kind.Class()->GetPathName();
			Record.DisplayName = Kind.Display;
			Record.Category = TEXT("State Machine");
		}
		return true;
	}

	virtual bool ResolveAndSpawn(
		const FAssetDocumentGraphSpawnRequest& Request,
		FAssetDocumentGraphSpawnResult& OutResult,
		FAssetDocumentGraphError& OutError) const override
	{
		const FStateKind* Kind = Algo::FindByPredicate(Kinds, [&Request](const FStateKind& Item) { return Request.ActionKey == Item.Key; });
		if (Kind == nullptr || Request.Graph == nullptr)
		{
			OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(
				TEXT("Action %s is not a state machine action (SM:State, SM:Conduit, SM:StateAlias, SM:Transition)."), *Request.ActionKey));
			return false;
		}
		const FVector2f Location = Request.Position.IsSet() ? FVector2f(Request.Position.GetValue()) : FVector2f(0.0f, 0.0f);
		// UAnimationStateMachineSchema's context menu actions (FEdGraphSchemaAction_NewStateNode)
		// and, for transitions, CreateAutomaticConversionNodeAndConnections.
		UEdGraphNode* Node = nullptr;
		if (Kind->Class() == UAnimStateNode::StaticClass())
		{
			Node = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(Request.Graph, NewObject<UAnimStateNode>(), Location, false);
		}
		else if (Kind->Class() == UAnimStateConduitNode::StaticClass())
		{
			Node = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateConduitNode>(Request.Graph, NewObject<UAnimStateConduitNode>(), Location, false);
		}
		else if (Kind->Class() == UAnimStateAliasNode::StaticClass())
		{
			Node = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateAliasNode>(Request.Graph, NewObject<UAnimStateAliasNode>(), Location, false);
		}
		else
		{
			Node = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateTransitionNode>(Request.Graph, NewObject<UAnimStateTransitionNode>(), Location, false);
		}
		if (Node == nullptr)
		{
			OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("Action %s did not produce a node."), *Request.ActionKey));
			return false;
		}
		OutResult.Node = Node;
		OutResult.bReused = false;
		return true;
	}

	virtual void DescribeNode(const UEdGraphNode& Node, FAssetDocumentGraphNodeDescription& Out) const override
	{
		Out.ActionKey = KeyOf(Node);
		FValue Extras = MakeObjectValue();
		if (Node.IsA<UAnimStateEntryNode>())
		{
			Out.ImplicitRole = TEXT("sm_entry");
		}
		else if (const UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(&Node))
		{
			if (const UAnimStateNodeBase* From = Transition->GetPreviousState())
			{
				SetField(Extras, TEXT("from"), MakeStringValue(From->GetStateName()));
			}
			if (const UAnimStateNodeBase* To = Transition->GetNextState())
			{
				SetField(Extras, TEXT("to"), MakeStringValue(To->GetStateName()));
			}
		}
		else if (const UAnimStateNodeBase* State = Cast<UAnimStateNodeBase>(&Node))
		{
			SetField(Extras, TEXT("name"), MakeStringValue(State->GetStateName()));
		}
		FValue Defaults;
		SetField(Extras, TEXT("properties"), CollectProperties(Node, Defaults));
		SetField(Extras, TEXT("property_defaults"), MoveTemp(Defaults));
		Out.Extras = MoveTemp(Extras);
	}

	virtual bool RemoveNode(UObject& Asset, UEdGraphNode& Node, FAssetDocumentGraphError& OutError) const override
	{
		if (!Node.CanUserDeleteNode())
		{
			OutError.Set(TEXT("Graph.NodeNotDeletable"), FString::Printf(TEXT("Node %s (%s) cannot be deleted by the user."),
				*FormatAssetDocumentGraphGuid(Node.NodeGuid), *Node.GetClass()->GetName()));
			return false;
		}
		UBlueprint* Blueprint = Cast<UBlueprint>(&Asset);
		if (Blueprint == nullptr)
		{
			Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(&Node);
		}
		if (Blueprint == nullptr)
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("State machine node removal needs a Blueprint."));
			return false;
		}
		// FBlueprintEditor::DeleteSelectedNodes; a removed state's transitions go with it.
		FBlueprintEditorUtils::RemoveNode(Blueprint, &Node, /*bDontRecompile=*/true);
		return true;
	}

	virtual FString ClassifySubgraph(const UEdGraph& Subgraph, const UEdGraphNode& OwnerNode) const override
	{
		if (const UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(&OwnerNode))
		{
			return Transition->GetCustomTransitionGraph() == &Subgraph ? TEXT("custom_transition") : TEXT("transition");
		}
		if (OwnerNode.IsA<UAnimStateConduitNode>())
		{
			return TEXT("conduit");
		}
		return OwnerNode.IsA<UAnimStateNode>() ? TEXT("state") : TEXT("other");
	}

	virtual bool SetNodeSetting(UObject& Asset, UEdGraphNode& Node, FStringView Path, const FAssetDocumentRuntimeValueDto& Value,
		bool& bOutHandled, FAssetDocumentGraphError& OutError) const override
	{
		bOutHandled = false;
		UAnimStateAliasNode* Alias = Cast<UAnimStateAliasNode>(&Node);
		if (Path != TEXTVIEW("AliasedStates"))
		{
			return true;
		}
		bOutHandled = true;
		if (Alias == nullptr || Value.Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), TEXT("AliasedStates is a state alias setting: an array of state names."), TEXT("/value"));
			return false;
		}
		const TArray<UAnimStateNodeBase*> States = NamedStates(*Node.GetGraph());
		TSet<TWeakObjectPtr<UAnimStateNodeBase>> Wanted;
		for (const FValue& Item : Value.Elements)
		{
			UAnimStateNodeBase* const* State = Item.Kind == EAssetDocumentRuntimeValueKind::String
				? States.FindByPredicate([&Item](const UAnimStateNodeBase* Candidate) { return Candidate->GetStateName() == Item.StringValue; })
				: nullptr;
			if (State == nullptr)
			{
				TArray<FString> Names;
				for (const UAnimStateNodeBase* Candidate : States)
				{
					Names.Add(Candidate->GetStateName());
				}
				OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("AliasedStates: '%s' is not a state or conduit of this state machine (%s)."),
					*Item.StringValue, *FString::Join(Names, TEXT(", "))), TEXT("/value"));
				return false;
			}
			Wanted.Add(*State);
		}
		// The alias details panel's per-state checkboxes.
		Alias->Modify();
		Alias->GetAliasedStates() = MoveTemp(Wanted);
		return true;
	}
};
} // namespace StateMachineAdapterDetail

TSharedRef<IAssetDocumentGraphSchemaAdapter> MakeStateMachineSchemaAdapter()
{
	return MakeShared<StateMachineAdapterDetail::FStateMachineSchemaAdapter>();
}
} // namespace UE::AssetDocument::AnimBlueprint
