// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// ai.state_tree.read.v1: the editor data of a StateTree as paged records (DTO in
// AssetDocumentStateTreeCatalog.cpp). Records come in a fixed order: tree, tree parameters,
// evaluators, global tasks, then every state in hierarchy order (state, its parameters, enter
// conditions, tasks, considerations, transition conditions), then the property bindings.

#include "StateTree/AssetDocumentStateTreePrimitives.h"

#include "AssetDocumentEditorPropertyValueCodec.h"
#include "AssetDocumentGraphModel.h"
#include "PropertyBagDetails.h"
#include "StateTree.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeExecutionTypes.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTasksStatus.h"
#include "StructUtils/PropertyBag.h"

namespace UE::AssetDocument::Graph::StateTree
{
namespace
{
FValue MakeEventValue(const FStateTreeEventDesc& Event)
{
	FObjectBuilder Value;
	Value.AddString(TEXT("tag"), Event.Tag.IsValid() ? Event.Tag.ToString() : FString());
	Value.AddString(TEXT("payload"), PathOf(Event.PayloadStruct.Get()));
	Value.AddBool(TEXT("consume"), Event.bConsumeEventOnSelect);
	return Value.Build();
}

FValue MakeTransitionValue(const FStateTreeTransition& Transition)
{
	FObjectBuilder Value;
	Value.AddString(TEXT("id"), GuidString(Transition.ID));
	Value.AddString(TEXT("trigger"), EnumToName(*StaticEnum<EStateTreeTransitionTrigger>(), static_cast<int64>(Transition.Trigger)));
	Value.Add(TEXT("event"), MakeEventValue(Transition.RequiredEvent));
	Value.AddString(TEXT("event_id"), GuidString(Transition.GetEventID()));
	FObjectBuilder To;
	To.AddString(TEXT("type"), EnumToName(*StaticEnum<EStateTreeTransitionType>(), static_cast<int64>(Transition.State.LinkType)));
	To.AddString(TEXT("state"), GuidString(Transition.State.ID));
	To.AddString(TEXT("name"), Transition.State.Name.ToString());
	Value.Add(TEXT("to"), To.Build());
	Value.AddString(TEXT("priority"), EnumToName(*StaticEnum<EStateTreeTransitionPriority>(), static_cast<int64>(Transition.Priority)));
	Value.AddBool(TEXT("delay"), Transition.bDelayTransition);
	Value.Add(TEXT("delay_duration"), MakeNumber(ShortestFloat(Transition.DelayDuration)));
	Value.Add(TEXT("delay_variance"), MakeNumber(ShortestFloat(Transition.DelayRandomVariance)));
	Value.AddBool(TEXT("enabled"), Transition.bTransitionEnabled);
	Value.AddBool(TEXT("delegate"), Transition.Trigger == EStateTreeTransitionTrigger::OnDelegate);
	return Value.Build();
}

FValue MakeStateRecord(const UStateTreeState& State, int32 Index)
{
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("state"));
	Record.AddString(TEXT("id"), GuidString(State.ID));
	Record.AddString(TEXT("parent"), State.Parent != nullptr ? GuidString(State.Parent->ID) : FString());
	Record.AddInt(TEXT("index"), Index);
	Record.AddString(TEXT("name"), State.Name.ToString());
	Record.AddString(TEXT("description"), State.Description);
	Record.AddString(TEXT("tag"), State.Tag.IsValid() ? State.Tag.ToString() : FString());
	Record.AddString(TEXT("type"), EnumToName(*StaticEnum<EStateTreeStateType>(), static_cast<int64>(State.Type)));
	Record.AddString(TEXT("selection"), EnumToName(*StaticEnum<EStateTreeStateSelectionBehavior>(), static_cast<int64>(State.SelectionBehavior)));
	Record.AddString(TEXT("tasks_completion"), EnumToName(*StaticEnum<EStateTreeTaskCompletionType>(), static_cast<int64>(State.TasksCompletion)));
	Record.AddBool(TEXT("enabled"), State.bEnabled);
	Record.Add(TEXT("weight"), MakeNumber(ShortestFloat(State.Weight)));
	Record.AddBool(TEXT("has_tick_rate"), State.bHasCustomTickRate);
	Record.Add(TEXT("tick_rate"), MakeNumber(ShortestFloat(State.CustomTickRate)));
	Record.AddBool(TEXT("check_prerequisites"), State.bCheckPrerequisitesWhenActivatingChildDirectly);
	Record.AddBool(TEXT("has_enter_event"), State.bHasRequiredEventToEnter);
	Record.Add(TEXT("enter_event"), MakeEventValue(State.RequiredEventToEnter));
	Record.AddString(TEXT("enter_event_id"), GuidString(State.GetEventID()));
	Record.AddString(TEXT("linked_subtree"), GuidString(State.LinkedSubtree.ID));
	Record.AddString(TEXT("linked_asset"), PathOf(State.LinkedAsset.Get()));
	TArray<FValue> Transitions;
	for (const FStateTreeTransition& Transition : State.Transitions)
	{
		Transitions.Add(MakeTransitionValue(Transition));
	}
	Record.Add(TEXT("transitions"), MakeArray(MoveTemp(Transitions)));
	return Record.Build();
}

void AppendNodes(TArray<FValue>& Out, TConstArrayView<FStateTreeEditorNode> Nodes, ENodeList List, const UStateTreeState* State,
	const FStateTreeTransition* Transition)
{
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		AppendNodeRecord(Out, Nodes[Index], List, Index, State, Transition);
	}
}

void AppendState(TArray<FValue>& Out, const UStateTreeEditorData& EditorData, const UStateTreeState& State, int32 Index)
{
	Out.Add(MakeStateRecord(State, Index));
	Out.Add(MakeParametersRecord(EditorData, &State));
	AppendNodes(Out, State.EnterConditions, ENodeList::EnterConditions, &State, nullptr);
	AppendNodes(Out, State.Tasks, ENodeList::Tasks, &State, nullptr);
	if (State.SingleTask.Node.IsValid())
	{
		AppendNodeRecord(Out, State.SingleTask, ENodeList::SingleTask, 0, &State, nullptr);
	}
	AppendNodes(Out, State.Considerations, ENodeList::Considerations, &State, nullptr);
	for (const FStateTreeTransition& Transition : State.Transitions)
	{
		AppendNodes(Out, Transition.Conditions, ENodeList::TransitionConditions, &State, &Transition);
	}
	for (int32 Child = 0; Child < State.Children.Num(); ++Child)
	{
		if (State.Children[Child] != nullptr)
		{
			AppendState(Out, EditorData, *State.Children[Child], Child);
		}
	}
}

FValue MakeTreeRecord(const UStateTree& Tree, const UStateTreeEditorData& EditorData)
{
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("tree"));
	const UStateTreeSchema* Schema = EditorData.Schema;
	Record.AddString(TEXT("schema_class"), PathOf(Schema != nullptr ? Schema->GetClass() : nullptr));
	TArray<FString> Defaults;
	TArray<FString> Unsupported;
	Record.Add(TEXT("schema"), Schema != nullptr
		? EncodeValues(*Schema->GetClass(), Schema, Schema->GetClass()->GetDefaultObject(), EValueOwner::Schema, Defaults, Unsupported)
		: MakeEmptyObject());
	Record.Add(TEXT("schema_defaults"), MakeStringArray(Defaults));
	Record.Add(TEXT("schema_unsupported"), MakeStringArray(Unsupported));
	Record.AddString(TEXT("editor_data_class"), EditorData.GetClass()->GetPathName());
	Record.AddString(TEXT("global_tasks_completion"),
		EnumToName(*StaticEnum<EStateTreeTaskCompletionType>(), static_cast<int64>(EditorData.GlobalTasksCompletion)));
	TArray<FValue> Context;
	if (Schema != nullptr)
	{
		for (const FStateTreeExternalDataDesc& Desc : Schema->GetContextDataDescs())
		{
			Context.Add(FObjectBuilder()
				.AddString(TEXT("name"), Desc.Name.ToString())
				.AddString(TEXT("id"), GuidString(Desc.ID))
				.AddString(TEXT("struct"), PathOf(Desc.Struct.Get()))
				.Build());
		}
	}
	Record.Add(TEXT("context"), MakeArray(MoveTemp(Context)));
	FObjectBuilder Allows;
	Allows.AddBool(TEXT("evaluators"), Schema == nullptr || Schema->AllowEvaluators());
	Allows.AddBool(TEXT("enter_conditions"), Schema == nullptr || Schema->AllowEnterConditions());
	Allows.AddBool(TEXT("considerations"), Schema == nullptr || Schema->AllowUtilityConsiderations());
	Allows.AddBool(TEXT("multiple_tasks"), Schema == nullptr || Schema->AllowMultipleTasks());
	Allows.AddBool(TEXT("global_parameters"), Schema == nullptr || Schema->AllowGlobalParameters());
	Allows.AddBool(TEXT("tasks_completion"), Schema == nullptr || Schema->AllowTasksCompletion());
	Record.Add(TEXT("allows"), Allows.Build());
	Record.AddInt(TEXT("compiled_hash"), static_cast<int64>(Tree.LastCompiledEditorDataHash));
	Record.AddInt(TEXT("editor_hash"), static_cast<int64>(UStateTreeEditingSubsystem::CalculateStateTreeHash(&Tree)));
	Record.AddBool(TEXT("ready"), Tree.IsReadyToRun());
	return Record.Build();
}
} // namespace

FValue MakeParametersRecord(const UStateTreeEditorData& EditorData, const UStateTreeState* State)
{
	const FInstancedPropertyBag& Bag = State != nullptr ? State->Parameters.Parameters : EditorData.GetRootParametersPropertyBag();
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("parameters"));
	Record.AddString(TEXT("owner"), State != nullptr ? GuidString(State->ID) : FString());
	Record.AddString(TEXT("id"), GuidString(State != nullptr ? State->Parameters.ID : EditorData.GetRootParametersGuid()));
	Record.AddBool(TEXT("fixed"), State != nullptr && State->Parameters.bFixedLayout);
	TArray<FValue> Props;
	if (const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct())
	{
		const void* Memory = Bag.GetValue().GetMemory();
		for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
		{
			FObjectBuilder Prop;
			Prop.AddString(TEXT("name"), Desc.Name.ToString());
			Prop.AddString(TEXT("id"), GuidString(Desc.ID));
			Prop.Add(TEXT("type"), MakePinTypeValue(UE::StructUtils::GetPropertyDescAsPin(Desc)));
			FProperty* Property = const_cast<FProperty*>(Desc.CachedProperty);
			FValue Value;
			FString Code;
			FString CodecId;
			const bool bSupported = Property != nullptr && Memory != nullptr && EditorPropertyValueCodec::Inspect(*Property, CodecId)
				&& EditorPropertyValueCodec::Encode(*Property, Property->ContainerPtrToValuePtr<void>(Memory), Value, Code);
			Prop.AddBool(TEXT("supported"), bSupported);
			if (bSupported)
			{
				if (Property->IsA<FFloatProperty>() && Value.Kind == EAssetDocumentRuntimeValueKind::Number)
				{
					Value.NumberValue = ShortestFloat(static_cast<float>(Value.NumberValue));
				}
				Prop.Add(TEXT("value"), MoveTemp(Value));
			}
			Props.Add(Prop.Build());
		}
	}
	Record.Add(TEXT("props"), MakeArray(MoveTemp(Props)));
	TArray<FString> Overrides;
	if (State != nullptr)
	{
		for (const FGuid& Id : State->Parameters.PropertyOverrides)
		{
			Overrides.Add(GuidString(Id));
		}
	}
	Record.Add(TEXT("overrides"), MakeStringArray(Overrides));
	return Record.Build();
}

bool HandleRead(FCall& Call, FObjectBuilder& Result)
{
	UStateTree* Tree = nullptr;
	UStateTreeEditorData* EditorData = nullptr;
	int64 Offset = 0;
	if (!GetTree(Call, Tree, EditorData) || !Call.Args.OnlyFields({TEXT("offset")}) || !Call.Args.Int(TEXT("offset"), Offset, false))
	{
		return false;
	}
	if (Offset < 0)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0."), TEXT("offset"));
	}
	TArray<FValue> Records;
	Records.Add(MakeTreeRecord(*Tree, *EditorData));
	Records.Add(MakeParametersRecord(*EditorData, nullptr));
	AppendNodes(Records, EditorData->Evaluators, ENodeList::Evaluators, nullptr, nullptr);
	AppendNodes(Records, EditorData->GlobalTasks, ENodeList::GlobalTasks, nullptr, nullptr);
	for (int32 Index = 0; Index < EditorData->SubTrees.Num(); ++Index)
	{
		if (EditorData->SubTrees[Index] != nullptr)
		{
			AppendState(Records, *EditorData, *EditorData->SubTrees[Index], Index);
		}
	}
	AppendBindingRecords(Records, *EditorData);

	TArray<FValue> Page;
	int32 NodeCount = 8;
	int32 Index = static_cast<int32>(FMath::Min<int64>(Offset, Records.Num()));
	for (; Index < Records.Num(); ++Index)
	{
		const int32 RecordNodes = CountValueNodes(Records[Index]);
		if (!Page.IsEmpty() && NodeCount + RecordNodes > QueryValueNodeBudget)
		{
			break;
		}
		if (Page.IsEmpty() && RecordNodes > QueryValueNodeBudget)
		{
			return Fail(Call, TEXT("Graph.ResultTooLarge"), FString::Printf(
				TEXT("Record %d of the StateTree read exceeds the runtime value budget (a node with very large values)."), Index), TEXT("/offset"));
		}
		NodeCount += RecordNodes;
		Page.Add(MoveTemp(Records[Index]));
	}
	Result.AddInt(TEXT("total"), Records.Num());
	Result.AddInt(TEXT("next_offset"), Index < Records.Num() ? Index : -1);
	Result.Add(TEXT("records"), MakeArray(MoveTemp(Page)));
	return true;
}
} // namespace UE::AssetDocument::Graph::StateTree
