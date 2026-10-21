// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// StateTree node lists (evaluators, global tasks, enter conditions, tasks, considerations, transition
// conditions). A node is built so that it equals what the editor's node picker produces for the same
// type: a fresh ID, the node struct (for a Blueprint node class, the matching Blueprint wrapper struct
// naming that class), and the node's instance data and execution runtime data, each either a struct
// value or an object outered to the owner of the list (the editor data or the state). The list itself
// is written as the Details panel's array editor writes it (EditMember: Modify, PreEditChange, write,
// PostEditChangeChainProperty on the owner).
// Behavior test: UEAssetDoc.StateTree.NodeConstruction.

#include "StateTree/AssetDocumentStateTreePrimitives.h"

#include "Algo/Find.h"
#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeConsiderationBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeNodeBase.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"

namespace UE::AssetDocument::Graph::StateTree
{
namespace
{
// The Blueprint node kinds: the Blueprint base class a node class derives from, the wrapper struct that runs such a
// class inside a StateTree, and access to the wrapper's class member.
struct FBlueprintNodeKind
{
	UClass* (*BaseClass)();
	UScriptStruct* (*WrapperStruct)();
	const UClass* (*ReadClass)(const FInstancedStruct& Wrapper);
	void (*WriteClass)(FInstancedStruct& Wrapper, UClass* Class);
};

template <typename TWrapper, typename TBase, TSubclassOf<TBase> TWrapper::*ClassMember>
FBlueprintNodeKind MakeBlueprintNodeKind()
{
	return {
		&TBase::StaticClass,
		&TWrapper::StaticStruct,
		[](const FInstancedStruct& Wrapper) -> const UClass*
		{
			const TWrapper* Typed = Wrapper.GetPtr<TWrapper>();
			return Typed != nullptr ? (Typed->*ClassMember).Get() : nullptr;
		},
		[](FInstancedStruct& Wrapper, UClass* Class) { Wrapper.GetMutable<TWrapper>().*ClassMember = Class; },
	};
}

TConstArrayView<FBlueprintNodeKind> BlueprintNodeKinds()
{
	static const FBlueprintNodeKind Kinds[] = {
		MakeBlueprintNodeKind<FStateTreeBlueprintTaskWrapper, UStateTreeTaskBlueprintBase, &FStateTreeBlueprintTaskWrapper::TaskClass>(),
		MakeBlueprintNodeKind<FStateTreeBlueprintEvaluatorWrapper, UStateTreeEvaluatorBlueprintBase,
			&FStateTreeBlueprintEvaluatorWrapper::EvaluatorClass>(),
		MakeBlueprintNodeKind<FStateTreeBlueprintConditionWrapper, UStateTreeConditionBlueprintBase,
			&FStateTreeBlueprintConditionWrapper::ConditionClass>(),
		MakeBlueprintNodeKind<FStateTreeBlueprintConsiderationWrapper, UStateTreeConsiderationBlueprintBase,
			&FStateTreeBlueprintConsiderationWrapper::ConsiderationClass>(),
	};
	return Kinds;
}

const UClass* GetWrappedClass(const FStateTreeEditorNode& Node)
{
	const UScriptStruct* Struct = Node.Node.GetScriptStruct();
	for (const FBlueprintNodeKind& Kind : BlueprintNodeKinds())
	{
		if (Struct != nullptr && Struct->IsChildOf(Kind.WrapperStruct()))
		{
			return Kind.ReadClass(Node.Node);
		}
	}
	return nullptr;
}

// Fills one data slot of a node (instance data or execution runtime data) for the type the node declares: a struct
// type becomes the struct value, a class becomes a new object outered to Outer; no type leaves the slot empty.
void CreateNodeData(const UStruct* DataType, FInstancedStruct& OutStruct, TObjectPtr<UObject>& OutObject, UObject& Outer)
{
	if (DataType == nullptr)
	{
		return;
	}
	if (const UClass* DataClass = Cast<UClass>(DataType))
	{
		OutObject = NewObject<UObject>(&Outer, const_cast<UClass*>(DataClass));
		return;
	}
	if (const UScriptStruct* DataStruct = Cast<UScriptStruct>(DataType))
	{
		OutStruct.InitializeAs(DataStruct);
	}
}

const UStruct* GetNodeType(const FStateTreeEditorNode& Node)
{
	if (const UClass* Class = GetWrappedClass(Node))
	{
		return Class;
	}
	return Node.Node.GetScriptStruct();
}

bool IsConditionList(ENodeList List)
{
	return List == ENodeList::EnterConditions || List == ENodeList::TransitionConditions;
}

bool IsTaskList(ENodeList List)
{
	return List == ENodeList::GlobalTasks || List == ENodeList::Tasks || List == ENodeList::SingleTask;
}

// What the schema lets the Details panel show (StateTreeEditorDataDetails / StateTreeStateDetails).
bool IsListAllowed(const UStateTreeSchema* Schema, ENodeList List, FString& OutWhy)
{
	if (Schema == nullptr)
	{
		return true;
	}
	if (List == ENodeList::Evaluators && !Schema->AllowEvaluators())
	{
		OutWhy = TEXT("does not allow evaluators");
		return false;
	}
	if (List == ENodeList::EnterConditions && !Schema->AllowEnterConditions())
	{
		OutWhy = TEXT("does not allow enter conditions");
		return false;
	}
	if (List == ENodeList::Considerations && !Schema->AllowUtilityConsiderations())
	{
		OutWhy = TEXT("does not allow utility considerations");
		return false;
	}
	if (List == ENodeList::Tasks && !Schema->AllowMultipleTasks())
	{
		OutWhy = TEXT("allows one task per state (single_task)");
		return false;
	}
	if (List == ENodeList::SingleTask && Schema->AllowMultipleTasks())
	{
		OutWhy = TEXT("allows several tasks per state (tasks)");
		return false;
	}
	return true;
}

bool IsTypeAllowed(const UStateTreeSchema* Schema, const UStruct& Type)
{
	if (Schema == nullptr)
	{
		return true;
	}
	if (const UScriptStruct* Struct = Cast<UScriptStruct>(&Type))
	{
		return Schema->IsStructAllowed(Struct);
	}
	return Schema->IsClassAllowed(Cast<UClass>(&Type));
}

// The values of a node: node struct + instance data (struct or Blueprint instance object).
bool DecodeNodeValues(FReader& Item, FStateTreeEditorNode& Node, UObject& Owner)
{
	if (Item.Has(TEXT("node")))
	{
		const UScriptStruct* NodeStruct = Node.Node.GetScriptStruct();
		if (NodeStruct == nullptr || !DecodeValues(Item, TEXT("node"), *NodeStruct, Node.Node.GetMutableMemory(), &Owner, EValueOwner::NodeStruct))
		{
			return NodeStruct != nullptr ? false : Item.Fail(TEXT("StateTree.InvalidValue"), TEXT("The node has no struct."), TEXT("node"));
		}
	}
	if (Item.Has(TEXT("instance")))
	{
		if (Node.InstanceObject != nullptr)
		{
			Node.InstanceObject->Modify();
			return DecodeValues(Item, TEXT("instance"), *Node.InstanceObject->GetClass(), Node.InstanceObject, Node.InstanceObject,
				EValueOwner::InstanceObject);
		}
		const UScriptStruct* InstanceStruct = Node.Instance.GetScriptStruct();
		if (InstanceStruct == nullptr)
		{
			const FValue* Values = Item.Find(TEXT("instance"));
			return Values == nullptr || Values->ObjectKeys.IsEmpty() ? true
				: Item.Fail(TEXT("StateTree.UnknownProperty"), TEXT("This node has no instance data."), TEXT("instance"));
		}
		return DecodeValues(Item, TEXT("instance"), *InstanceStruct, Node.Instance.GetMutableMemory(), &Owner, EValueOwner::InstanceStruct);
	}
	return true;
}

bool ApplyNodeFields(FReader& Item, FStateTreeEditorNode& Node, ENodeList List)
{
	FString Name;
	if (Item.Has(TEXT("name")))
	{
		if (!Item.String(TEXT("name"), Name))
		{
			return false;
		}
		if (FStateTreeNodeBase* NodeBase = Node.Node.GetMutablePtr<FStateTreeNodeBase>())
		{
			NodeBase->Name = Name.IsEmpty() ? FName() : FName(*Name);
		}
	}
	if (Item.Has(TEXT("indent")))
	{
		int64 Indent = 0;
		if (!Item.Int(TEXT("indent"), Indent))
		{
			return false;
		}
		if (Indent < 0 || Indent > UE::StateTree::MaxExpressionIndent)
		{
			return Item.Fail(TEXT("StateTree.InvalidValue"), FString::Printf(TEXT("Indent must be 0..%d."), UE::StateTree::MaxExpressionIndent),
				TEXT("indent"));
		}
		Node.ExpressionIndent = static_cast<uint8>(Indent);
	}
	if (Item.Has(TEXT("operand")))
	{
		int64 Operand = 0;
		if (!ReadEnum(Item, TEXT("operand"), *StaticEnum<EStateTreeExpressionOperand>(), Operand))
		{
			return false;
		}
		Node.ExpressionOperand = static_cast<EStateTreeExpressionOperand>(Operand);
	}
	if (Item.Has(TEXT("enabled")))
	{
		bool bEnabled = true;
		FStateTreeTaskBase* Task = Node.Node.GetMutablePtr<FStateTreeTaskBase>();
		if (!Item.Bool(TEXT("enabled"), bEnabled))
		{
			return false;
		}
		if (Task == nullptr)
		{
			return Item.Fail(TEXT("StateTree.InvalidValue"), TEXT("Only tasks have an Enabled toggle."), TEXT("enabled"));
		}
		Task->bTaskEnabled = bEnabled;
	}
	if (Item.Has(TEXT("considered")))
	{
		bool bConsidered = true;
		bool bCurrent = false;
		bool bEditable = false;
		if (!Item.Bool(TEXT("considered"), bConsidered))
		{
			return false;
		}
		if (!IsTaskConsideredForCompletion(Node, bCurrent, bEditable))
		{
			return Item.Fail(TEXT("StateTree.InvalidValue"), TEXT("Only tasks have a Considered For Completion toggle."), TEXT("considered"));
		}
		if (bConsidered != bCurrent)
		{
			if (!bEditable)
			{
				return Item.Fail(TEXT("StateTree.InvalidValue"),
					TEXT("This task does not let the editor change Considered For Completion (bCanEditConsideredForCompletion is false)."),
					TEXT("considered"));
			}
			// UE::StateTreeEditor::EditorNodeUtils::SetTaskConsideredForCompletion.
			if (Node.Node.GetPtr<FStateTreeBlueprintTaskWrapper>() != nullptr)
			{
				if (UStateTreeTaskBlueprintBase* Instance = Cast<UStateTreeTaskBlueprintBase>(Node.InstanceObject))
				{
					Instance->Modify();
					Instance->bConsideredForCompletion = bConsidered;
				}
			}
			else if (FStateTreeTaskBase* Task = Node.Node.GetMutablePtr<FStateTreeTaskBase>())
			{
				Task->bConsideredForCompletion = bConsidered;
			}
		}
	}
	if (Item.Has(TEXT("evaluation_mode")))
	{
		int64 Mode = 0;
		FStateTreeConditionBase* Condition = Node.Node.GetMutablePtr<FStateTreeConditionBase>();
		if (!ReadEnum(Item, TEXT("evaluation_mode"), *StaticEnum<EStateTreeConditionEvaluationMode>(), Mode))
		{
			return false;
		}
		if (Condition == nullptr)
		{
			return Item.Fail(TEXT("StateTree.InvalidValue"), TEXT("Only conditions have an evaluation mode."), TEXT("evaluation_mode"));
		}
		Condition->EvaluationMode = static_cast<EStateTreeConditionEvaluationMode>(Mode);
	}
	(void)List;
	return true;
}
} // namespace

FString GetNodeTypePath(const FStateTreeEditorNode& Node)
{
	return PathOf(GetNodeType(Node));
}

bool IsTaskEnabled(const FStateTreeEditorNode& Node, bool& bOutEnabled)
{
	if (const FStateTreeTaskBase* Task = Node.Node.GetPtr<FStateTreeTaskBase>())
	{
		bOutEnabled = Task->bTaskEnabled;
		return true;
	}
	return false;
}

bool IsTaskConsideredForCompletion(const FStateTreeEditorNode& Node, bool& bOutConsidered, bool& bOutEditable)
{
	if (Node.Node.GetPtr<FStateTreeBlueprintTaskWrapper>() != nullptr)
	{
		if (const UStateTreeTaskBlueprintBase* Instance = Cast<UStateTreeTaskBlueprintBase>(Node.InstanceObject))
		{
			bOutConsidered = Instance->bConsideredForCompletion;
			bOutEditable = Instance->bCanEditConsideredForCompletion;
			return true;
		}
		return false;
	}
	if (const FStateTreeTaskBase* Task = Node.Node.GetPtr<FStateTreeTaskBase>())
	{
		bOutConsidered = Task->bConsideredForCompletion;
		bOutEditable = Task->bCanEditConsideredForCompletion;
		return true;
	}
	return false;
}

bool GetConditionEvaluationMode(const FStateTreeEditorNode& Node, int64& OutMode)
{
	if (const FStateTreeConditionBase* Condition = Node.Node.GetPtr<FStateTreeConditionBase>())
	{
		OutMode = static_cast<int64>(Condition->EvaluationMode);
		return true;
	}
	return false;
}

bool InitializeNode(FStateTreeEditorNode& Node, const UStruct& Type, const FGuid& Id, UObject& Outer)
{
	Node.Reset();
	if (const UClass* NodeClass = Cast<UClass>(&Type))
	{
		// A Blueprint node: the wrapper struct of its kind runs an instance of the class.
		const FBlueprintNodeKind* Kind = Algo::FindByPredicate(BlueprintNodeKinds(), [NodeClass](const FBlueprintNodeKind& Candidate)
		{
			return NodeClass->IsChildOf(Candidate.BaseClass());
		});
		if (Kind == nullptr)
		{
			return false;
		}
		UClass* MutableClass = const_cast<UClass*>(NodeClass);
		Node.Node.InitializeAs(Kind->WrapperStruct());
		Kind->WriteClass(Node.Node, MutableClass);
		Node.InstanceObject = NewObject<UObject>(&Outer, MutableClass);
		Node.ID = Id;
		return true;
	}
	const UScriptStruct* NodeStruct = Cast<UScriptStruct>(&Type);
	if (NodeStruct == nullptr)
	{
		return false;
	}
	Node.ID = Id;
	Node.Node.InitializeAs(NodeStruct);
	if (const FStateTreeNodeBase* Base = Node.Node.GetPtr<FStateTreeNodeBase>())
	{
		CreateNodeData(Base->GetInstanceDataType(), Node.Instance, Node.InstanceObject, Outer);
		CreateNodeData(Base->GetExecutionRuntimeDataType(), Node.ExecutionRuntimeData, Node.ExecutionRuntimeDataObject, Outer);
	}
	return true;
}

void AppendNodeRecord(TArray<FValue>& Out, const FStateTreeEditorNode& Node, ENodeList List, int32 Index,
	const UStateTreeState* State, const FStateTreeTransition* Transition)
{
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("node"));
	Record.AddString(TEXT("list"), NodeListName(List));
	Record.AddString(TEXT("state"), State != nullptr ? GuidString(State->ID) : FString());
	Record.AddString(TEXT("transition"), Transition != nullptr ? GuidString(Transition->ID) : FString());
	Record.AddInt(TEXT("index"), Index);
	Record.AddString(TEXT("id"), GuidString(Node.ID));
	Record.AddString(TEXT("node_id"), GuidString(Node.GetNodeID()));
	Record.AddString(TEXT("type"), GetNodeTypePath(Node));
	const FStateTreeNodeBase* NodeBase = Node.Node.GetPtr<FStateTreeNodeBase>();
	Record.AddString(TEXT("name"), NodeBase != nullptr && !NodeBase->Name.IsNone() ? NodeBase->Name.ToString() : FString());
	bool bFlag = false;
	bool bEditable = false;
	if (IsTaskEnabled(Node, bFlag))
	{
		Record.AddBool(TEXT("enabled"), bFlag);
	}
	if (IsTaskConsideredForCompletion(Node, bFlag, bEditable))
	{
		Record.AddBool(TEXT("considered"), bFlag);
		Record.AddBool(TEXT("considered_editable"), bEditable);
	}
	int64 Mode = 0;
	if (GetConditionEvaluationMode(Node, Mode))
	{
		Record.AddString(TEXT("evaluation_mode"), EnumToName(*StaticEnum<EStateTreeConditionEvaluationMode>(), Mode));
	}
	Record.AddInt(TEXT("indent"), Node.ExpressionIndent);
	Record.AddString(TEXT("operand"), EnumToName(*StaticEnum<EStateTreeExpressionOperand>(), static_cast<int64>(Node.ExpressionOperand)));
	TArray<FString> NodeDefaults;
	TArray<FString> InstanceDefaults;
	TArray<FString> Unsupported;
	// node_reset / instance_reset: the default of each value that differs from it (an omitted value is reset).
	FValue NodeValues = MakeEmptyObject();
	FValue NodeReset = MakeEmptyObject();
	if (const UScriptStruct* NodeStruct = Node.Node.GetScriptStruct())
	{
		FInstancedStruct Default(NodeStruct);
		NodeValues = EncodeValues(*NodeStruct, Node.Node.GetMemory(), Default.GetMemory(), EValueOwner::NodeStruct, NodeDefaults, Unsupported);
		NodeReset = EncodeChangedDefaults(*NodeStruct, Node.Node.GetMemory(), Default.GetMemory(), EValueOwner::NodeStruct);
	}
	FValue InstanceValues = MakeEmptyObject();
	FValue InstanceReset = MakeEmptyObject();
	FString InstanceType;
	if (Node.InstanceObject != nullptr)
	{
		const UClass* Class = Node.InstanceObject->GetClass();
		InstanceType = Class->GetPathName();
		InstanceValues = EncodeValues(*Class, Node.InstanceObject, Class->GetDefaultObject(), EValueOwner::InstanceObject,
			InstanceDefaults, Unsupported);
		InstanceReset = EncodeChangedDefaults(*Class, Node.InstanceObject, Class->GetDefaultObject(), EValueOwner::InstanceObject);
	}
	else if (const UScriptStruct* InstanceStruct = Node.Instance.GetScriptStruct())
	{
		InstanceType = InstanceStruct->GetPathName();
		FInstancedStruct Default(InstanceStruct);
		InstanceValues = EncodeValues(*InstanceStruct, Node.Instance.GetMemory(), Default.GetMemory(), EValueOwner::InstanceStruct,
			InstanceDefaults, Unsupported);
		InstanceReset = EncodeChangedDefaults(*InstanceStruct, Node.Instance.GetMemory(), Default.GetMemory(), EValueOwner::InstanceStruct);
	}
	Record.AddString(TEXT("instance_type"), InstanceType);
	Record.Add(TEXT("node"), MoveTemp(NodeValues));
	Record.Add(TEXT("node_defaults"), MakeStringArray(NodeDefaults));
	Record.Add(TEXT("node_reset"), MoveTemp(NodeReset));
	Record.Add(TEXT("instance"), MoveTemp(InstanceValues));
	Record.Add(TEXT("instance_defaults"), MakeStringArray(InstanceDefaults));
	Record.Add(TEXT("instance_reset"), MoveTemp(InstanceReset));
	Record.Add(TEXT("unsupported"), MakeStringArray(Unsupported));
	Out.Add(Record.Build());
}

bool EditNodes(FCall& Call, FReader& Op, UStateTreeEditorData& EditorData)
{
	FString ListName;
	FGuid StateId;
	FGuid TransitionId;
	const FValue* Items = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("list"), TEXT("state"), TEXT("transition"), TEXT("nodes")})
		|| !Op.String(TEXT("list"), ListName) || !Op.Guid(TEXT("state"), StateId, false) || !Op.Guid(TEXT("transition"), TransitionId, false)
		|| !Op.Array(TEXT("nodes"), Items))
	{
		return false;
	}
	ENodeList List = ENodeList::Tasks;
	if (!ParseNodeList(ListName, List))
	{
		return Op.Fail(TEXT("StateTree.InvalidOp"), FString::Printf(TEXT("Unknown node list '%s'."), *ListName), TEXT("list"));
	}
	FNodeListRef Ref;
	if (!ResolveNodeList(EditorData, List, StateId, TransitionId, Ref))
	{
		return FindState(EditorData, StateId) == nullptr
			? Op.Fail(TEXT("StateTree.StateNotFound"), TEXT("The state of this node list does not exist."), TEXT("state"))
			: Op.Fail(TEXT("StateTree.TransitionNotFound"), TEXT("The transition of this condition list does not exist."), TEXT("transition"));
	}
	FString Why;
	if (!Items->Elements.IsEmpty() && !IsListAllowed(EditorData.Schema, List, Why))
	{
		return Op.Fail(TEXT("StateTree.ListNotAllowed"), FString::Printf(TEXT("The schema %s %s."),
			*PathOf(EditorData.Schema != nullptr ? EditorData.Schema->GetClass() : nullptr), *Why), TEXT("nodes"));
	}
	if (List == ENodeList::SingleTask && Items->Elements.Num() > 1)
	{
		return Op.Fail(TEXT("StateTree.ListNotAllowed"), TEXT("This schema allows one task per state."), TEXT("nodes"));
	}
	TArray<FStateTreeEditorNode> Current;
	if (Ref.Nodes != nullptr)
	{
		Current = *Ref.Nodes;
	}
	else if (Ref.Single != nullptr && Ref.Single->Node.IsValid())
	{
		Current.Add(*Ref.Single);
	}
	TArray<FStateTreeEditorNode> Desired;
	TSet<FGuid> SeenIds;
	FReader ItemsReader = Op.Child(TEXT("nodes"), *Items);
	for (int32 Index = 0; Index < Items->Elements.Num(); ++Index)
	{
		FReader Item = ItemsReader.Child(FString::FromInt(Index), Items->Elements[Index]);
		FGuid Id;
		FString TypeText;
		if (!Item.IsObject() || !Item.OnlyFields({TEXT("id"), TEXT("type"), TEXT("name"), TEXT("indent"), TEXT("operand"), TEXT("enabled"),
				TEXT("considered"), TEXT("evaluation_mode"), TEXT("node"), TEXT("instance")})
			|| !Item.Guid(TEXT("id"), Id) || !Item.String(TEXT("type"), TypeText))
		{
			return Item.IsObject() ? false : Item.Fail(TEXT("StateTree.InvalidOp"), TEXT("Each node is an object."));
		}
		if (SeenIds.Contains(Id))
		{
			return Item.Fail(TEXT("StateTree.DuplicateId"), TEXT("Two nodes share an id."), TEXT("id"));
		}
		SeenIds.Add(Id);
		const UScriptStruct* BaseStruct = GetNodeBaseStruct(List);
		const UClass* BaseClass = GetNodeBaseClass(List);
		const UStruct* Type = ResolveType(TypeText, BaseStruct, BaseClass);
		if (Type == nullptr)
		{
			return Item.Fail(TEXT("StateTree.UnknownType"), FString::Printf(
				TEXT("'%s' is not a %s struct or a Blueprint class derived from %s. list_types (kind struct, base %s; or kind class, base %s) "
					"lists the candidates."), *TypeText, *BaseStruct->GetName(), *BaseClass->GetName(), *BaseStruct->GetPathName(),
				*BaseClass->GetPathName()), TEXT("type"));
		}
		// The picker only governs choosing a type: an existing node keeps its type even when that type is
		// (or became) hidden, abstract or deprecated, as in the editor. Only a new node or a type change
		// must pick a type the picker offers.
		FStateTreeEditorNode* Existing = Current.FindByPredicate([&Id](const FStateTreeEditorNode& Node) { return Node.ID == Id; });
		const bool bKeepsType = Existing != nullptr && GetNodeType(*Existing) == Type;
		const UClass* TypeClass = Cast<UClass>(Type);
		if (!bKeepsType
			&& (Type->HasMetaData(TEXT("Hidden")) || (TypeClass != nullptr && TypeClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))))
		{
			return Item.Fail(TEXT("StateTree.UnknownType"), FString::Printf(TEXT("'%s' is hidden, abstract or deprecated; the node picker does not offer it."),
				*Type->GetPathName()), TEXT("type"));
		}
		if (!IsTypeAllowed(EditorData.Schema, *Type))
		{
			return Item.Fail(TEXT("StateTree.NodeNotAllowed"), FString::Printf(TEXT("The schema %s does not allow %s."),
				*PathOf(EditorData.Schema != nullptr ? EditorData.Schema->GetClass() : nullptr), *Type->GetPathName()), TEXT("type"));
		}
		FStateTreeEditorNode& Node = Desired.AddDefaulted_GetRef();
		if (bKeepsType)
		{
			Node = *Existing;
		}
		else if (!InitializeNode(Node, *Type, Id, *Ref.Owner))
		{
			return Item.Fail(TEXT("StateTree.UnknownType"), FString::Printf(TEXT("%s cannot be created as a node."), *Type->GetPathName()), TEXT("type"));
		}
		if (!ApplyNodeFields(Item, Node, List) || !DecodeNodeValues(Item, Node, *Ref.Owner))
		{
			return false;
		}
	}
	(void)Call;
	EditMember(*Ref.Owner, Ref.MemberName, [&Ref, &Desired]()
	{
		if (Ref.Nodes != nullptr)
		{
			*Ref.Nodes = MoveTemp(Desired);
		}
		else if (Ref.Single != nullptr)
		{
			if (Desired.IsEmpty())
			{
				Ref.Single->Reset();
			}
			else
			{
				*Ref.Single = MoveTemp(Desired[0]);
			}
		}
	});
	return true;
}
} // namespace UE::AssetDocument::Graph::StateTree
