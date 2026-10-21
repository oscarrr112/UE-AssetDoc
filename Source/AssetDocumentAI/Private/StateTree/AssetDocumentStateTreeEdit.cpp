// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// ai.state_tree.edit.v1: the ops of one call run in one editor transaction, in the order given (DTO in
// AssetDocumentStateTreeCatalog.cpp). Every write goes through the owner's Details-panel change
// path (EditMember), so the engine's own handlers keep the editor data consistent: a state type
// change resets tasks and links, a linked state takes the linked subtree's parameter layout,
// parameter edits broadcast to linked states, schema edits broadcast OnSchemaChanged.

#include "StateTree/AssetDocumentStateTreePrimitives.h"

#include "AssetDocumentEditorPropertyValueCodec.h"
#include "AssetDocumentGraphModel.h"
#include "GameplayTagContainer.h"
#include "PropertyBagDetails.h"
#include "ScopedTransaction.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTasksStatus.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "AssetDocumentStateTree"

namespace UE::AssetDocument::Graph::StateTree
{
namespace
{
bool ReadTag(FReader& Reader, FStringView Key, FGameplayTag& OutTag)
{
	FString Text;
	if (!Reader.String(Key, Text))
	{
		return false;
	}
	OutTag = FGameplayTag();
	if (Text.IsEmpty())
	{
		return true;
	}
	OutTag = FGameplayTag::RequestGameplayTag(FName(*Text), false);
	return OutTag.IsValid() ? true : Reader.Fail(TEXT("StateTree.UnknownGameplayTag"), FString::Printf(
		TEXT("'%s' is not a registered gameplay tag (Project Settings > GameplayTags)."), *Text), Key);
}

bool ReadEvent(FReader& Reader, FStringView Key, FStateTreeEventDesc& InOutEvent)
{
	const FValue* Value = nullptr;
	if (!Reader.Object(Key, Value))
	{
		return false;
	}
	FReader Event = Reader.Child(Key, *Value);
	FGameplayTag Tag;
	FString Payload;
	bool bConsume = InOutEvent.bConsumeEventOnSelect;
	if (!Event.OnlyFields({TEXT("tag"), TEXT("payload"), TEXT("consume")}) || !ReadTag(Event, TEXT("tag"), Tag)
		|| !Event.String(TEXT("payload"), Payload, false) || !Event.Bool(TEXT("consume"), bConsume))
	{
		return false;
	}
	const UScriptStruct* PayloadStruct = nullptr;
	if (!Payload.IsEmpty())
	{
		PayloadStruct = Cast<UScriptStruct>(ResolveType(Payload, nullptr, nullptr));
		if (PayloadStruct == nullptr)
		{
			return Event.Fail(TEXT("StateTree.UnknownType"), FString::Printf(TEXT("Payload '%s' is not a struct."), *Payload), TEXT("payload"));
		}
	}
	InOutEvent.Tag = Tag;
	InOutEvent.PayloadStruct = PayloadStruct;
	InOutEvent.bConsumeEventOnSelect = bConsume;
	return true;
}

bool ReadName(FReader& Reader, FStringView Key, FName& OutName)
{
	FString Text;
	if (!Reader.String(Key, Text))
	{
		return false;
	}
	if (Text.TrimStartAndEnd().IsEmpty() || Text.Len() >= NAME_SIZE)
	{
		return Reader.Fail(TEXT("StateTree.InvalidName"), TEXT("A name is 1 to 1023 characters."), Key);
	}
	OutName = FName(*Text);
	return true;
}

// ---- schema / tree --------------------------------------------------------------------------------

bool EditSchema(FReader& Op, UStateTreeEditorData& EditorData)
{
	if (!Op.OnlyFields({TEXT("op"), TEXT("values")}))
	{
		return false;
	}
	UStateTreeSchema* Schema = EditorData.Schema;
	if (Schema == nullptr)
	{
		return Op.Fail(TEXT("StateTree.InvalidOp"), TEXT("The tree has no schema."), TEXT("values"));
	}
	bool bDecoded = true;
	EditMember(EditorData, GET_MEMBER_NAME_CHECKED(UStateTreeEditorData, Schema), [&]()
	{
		Schema->Modify();
		TArray<FProperty*> Written;
		bDecoded = DecodeValues(Op, TEXT("values"), *Schema->GetClass(), Schema, Schema, EValueOwner::Schema, &Written);
		// The schema's own Details notification for each value, as the Details panel sends it: a schema refreshes
		// what depends on its properties there (UStateTreeComponentSchema sets its Actor context data to
		// ContextActorClass, so that bindings to that class's members resolve in the same apply).
		for (FProperty* Property : Written)
		{
			FEditPropertyChain Chain;
			Chain.AddHead(Property);
			Chain.SetActivePropertyNode(Property);
			Chain.SetActiveMemberPropertyNode(Property);
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			FPropertyChangedChainEvent ChainEvent(Chain, Event);
			Schema->PostEditChangeChainProperty(ChainEvent);
		}
	});
	return bDecoded;
}

bool EditTree(FReader& Op, UStateTreeEditorData& EditorData)
{
	int64 Completion = 0;
	if (!Op.OnlyFields({TEXT("op"), TEXT("global_tasks_completion")})
		|| !ReadEnum(Op, TEXT("global_tasks_completion"), *StaticEnum<EStateTreeTaskCompletionType>(), Completion))
	{
		return false;
	}
	EditMember(EditorData, GET_MEMBER_NAME_CHECKED(UStateTreeEditorData, GlobalTasksCompletion), [&]()
	{
		EditorData.GlobalTasksCompletion = static_cast<EStateTreeTaskCompletionType>(Completion);
	});
	return true;
}

// ---- parameters ------------------------------------------------------------------------------------

// The tree's parameter bag is a private UPROPERTY the Details panel edits through reflection.
FInstancedPropertyBag* GetRootBag(UStateTreeEditorData& EditorData)
{
	const FStructProperty* Property = CastField<FStructProperty>(
		UStateTreeEditorData::StaticClass()->FindPropertyByName(TEXT("RootParameterPropertyBag")));
	return Property != nullptr && Property->Struct == FInstancedPropertyBag::StaticStruct()
		? Property->ContainerPtrToValuePtr<FInstancedPropertyBag>(&EditorData) : nullptr;
}

bool DecodeBagValue(FReader& Reader, FStringView Key, FInstancedPropertyBag& Bag, FName Name, UObject& Owner)
{
	const FPropertyBagPropertyDesc* Desc = Bag.FindPropertyDescByName(Name);
	FProperty* Property = Desc != nullptr ? const_cast<FProperty*>(Desc->CachedProperty) : nullptr;
	void* Memory = Bag.GetMutableValue().GetMemory();
	const FValue* Value = Reader.Find(Key);
	if (Property == nullptr || Memory == nullptr || Value == nullptr)
	{
		return Reader.Fail(TEXT("StateTree.UnknownProperty"), FString::Printf(TEXT("There is no parameter '%s'."), *Name.ToString()), Key);
	}
	FValue Coerced = *Value;
	if ((Property->IsA<FFloatProperty>() || Property->IsA<FDoubleProperty>()) && Coerced.Kind == EAssetDocumentRuntimeValueKind::Integer)
	{
		Coerced.Kind = EAssetDocumentRuntimeValueKind::Number;
		Coerced.NumberValue = static_cast<double>(Coerced.IntegerValue);
	}
	FString Code;
	if (!EditorPropertyValueCodec::Decode(*Property, Coerced, Property->ContainerPtrToValuePtr<void>(Memory), Code, &Owner))
	{
		return Reader.Fail(TEXT("StateTree.InvalidValue"), FString::Printf(TEXT("Parameter '%s' (%s) does not accept this value (%s)."),
			*Name.ToString(), *Property->GetCPPType(), *Code), Key);
	}
	return true;
}

bool EditParameters(FReader& Op, UStateTreeEditorData& EditorData)
{
	FGuid StateId;
	const FValue* Props = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("state"), TEXT("props")}) || !Op.Guid(TEXT("state"), StateId, false) || !Op.Array(TEXT("props"), Props))
	{
		return false;
	}
	UStateTreeState* State = nullptr;
	FInstancedPropertyBag* Bag = nullptr;
	UObject* Owner = &EditorData;
	FName Member = TEXT("RootParameterPropertyBag");
	if (StateId.IsValid())
	{
		State = FindState(EditorData, StateId);
		if (State == nullptr)
		{
			return Op.Fail(TEXT("StateTree.StateNotFound"), TEXT("The state does not exist."), TEXT("state"));
		}
		if (State->Parameters.bFixedLayout)
		{
			return Op.Fail(TEXT("StateTree.ParameterLayoutFixed"), TEXT("A linked state's parameters take the linked subtree's layout; "
				"set their values instead."), TEXT("props"));
		}
		Bag = &State->Parameters.Parameters;
		Owner = State;
		Member = GET_MEMBER_NAME_CHECKED(UStateTreeState, Parameters);
	}
	else
	{
		Bag = GetRootBag(EditorData);
		if (Bag == nullptr)
		{
			return Op.Fail(TEXT("StateTree.InvalidOp"), TEXT("The tree's parameter bag is not reachable."), TEXT("props"));
		}
		if (!Props->Elements.IsEmpty() && EditorData.Schema != nullptr && !EditorData.Schema->AllowGlobalParameters())
		{
			return Op.Fail(TEXT("StateTree.ListNotAllowed"), TEXT("The schema does not allow tree parameters."), TEXT("props"));
		}
	}
	const UPropertyBag* OldStruct = Bag->GetPropertyBagStruct();
	TArray<FPropertyBagPropertyDesc> Descs;
	TSet<FName> Names;
	TSet<FGuid> Ids;
	FReader PropsReader = Op.Child(TEXT("props"), *Props);
	for (int32 Index = 0; Index < Props->Elements.Num(); ++Index)
	{
		FReader Prop = PropsReader.Child(FString::FromInt(Index), Props->Elements[Index]);
		FGuid Id;
		FName Name;
		const FValue* TypeValue = nullptr;
		if (!Prop.OnlyFields({TEXT("id"), TEXT("name"), TEXT("type"), TEXT("value")}) || !Prop.Guid(TEXT("id"), Id)
			|| !ReadName(Prop, TEXT("name"), Name) || !Prop.Object(TEXT("type"), TypeValue))
		{
			return false;
		}
		FReader TypeReader = Prop.Child(TEXT("type"), *TypeValue);
		FEdGraphPinType PinType;
		if (!ReadPinType(TypeReader, PinType))
		{
			return false;
		}
		if (Names.Contains(Name) || Ids.Contains(Id))
		{
			return Prop.Fail(TEXT("StateTree.DuplicateId"), FString::Printf(TEXT("Parameter '%s' appears more than once."), *Name.ToString()),
				TEXT("name"));
		}
		Names.Add(Name);
		Ids.Add(Id);
		const FPropertyBagPropertyDesc* Existing = OldStruct != nullptr ? OldStruct->FindPropertyDescByID(Id) : nullptr;
		FPropertyBagPropertyDesc Desc = Existing != nullptr ? *Existing : FPropertyBagPropertyDesc();
		Desc.CachedProperty = nullptr;
		Desc.ID = Id;
		Desc.Name = Name;
		UE::StructUtils::SetPropertyDescFromPin(Desc, PinType);
		if (Desc.ValueType == EPropertyBagPropertyType::None)
		{
			return Prop.Fail(TEXT("StateTree.InvalidParameter"), FString::Printf(TEXT("Parameter '%s' has a type a property bag cannot hold."),
				*Name.ToString()), TEXT("type"));
		}
		Descs.Add(MoveTemp(Desc));
	}
	const UPropertyBag* NewStruct = Descs.IsEmpty() ? nullptr : UPropertyBag::GetOrCreateFromDescs(Descs);
	if (!Descs.IsEmpty() && NewStruct == nullptr)
	{
		return Op.Fail(TEXT("StateTree.InvalidParameter"), TEXT("The parameter layout could not be created."), TEXT("props"));
	}
	bool bDecoded = true;
	EditMember(*Owner, Member, [&]()
	{
		if (NewStruct != Bag->GetPropertyBagStruct())
		{
			Bag->MigrateToNewBagStruct(NewStruct);
		}
		for (int32 Index = 0; Index < Props->Elements.Num() && bDecoded; ++Index)
		{
			FReader Prop = PropsReader.Child(FString::FromInt(Index), Props->Elements[Index]);
			if (Prop.Has(TEXT("value")))
			{
				bDecoded = DecodeBagValue(Prop, TEXT("value"), *Bag, Descs[Index].Name, *Owner);
			}
		}
	});
	if (State != nullptr && bDecoded)
	{
		// UStateTreeEditorMode::OnStateParametersChanged (the open StateTree editor): the Linked states
		// of this subtree take its new parameter layout, keeping their overridden values.
		const FGuid ChangedId = State->ID;
		EditorData.VisitHierarchy([&ChangedId](UStateTreeState& Each, UStateTreeState* /*Parent*/)
		{
			if (Each.Type == EStateTreeStateType::Linked && Each.LinkedSubtree.ID == ChangedId)
			{
				Each.Modify();
				Each.UpdateParametersFromLinkedSubtree();
			}
			return EStateTreeVisitor::Continue;
		});
	}
	return bDecoded;
}

bool EditParameterValues(FReader& Op, UStateTreeEditorData& EditorData)
{
	FGuid StateId;
	const FValue* Values = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("state"), TEXT("values")}) || !Op.Guid(TEXT("state"), StateId) || !Op.Object(TEXT("values"), Values))
	{
		return false;
	}
	UStateTreeState* State = FindState(EditorData, StateId);
	if (State == nullptr)
	{
		return Op.Fail(TEXT("StateTree.StateNotFound"), TEXT("The state does not exist."), TEXT("state"));
	}
	if (!State->Parameters.bFixedLayout)
	{
		return Op.Fail(TEXT("StateTree.InvalidOp"), TEXT("Only linked states override parameter values."), TEXT("values"));
	}
	FReader ValuesReader = Op.Child(TEXT("values"), *Values);
	TArray<FGuid> Overridden;
	for (const FString& Name : Values->ObjectKeys)
	{
		const FPropertyBagPropertyDesc* Desc = State->Parameters.Parameters.FindPropertyDescByName(FName(*Name));
		if (Desc == nullptr)
		{
			TArray<FString> Known;
			if (const UPropertyBag* BagStruct = State->Parameters.Parameters.GetPropertyBagStruct())
			{
				for (const FPropertyBagPropertyDesc& Each : BagStruct->GetPropertyDescs())
				{
					Known.Add(Each.Name.ToString());
				}
			}
			return ValuesReader.Fail(TEXT("StateTree.UnknownProperty"), FString::Printf(TEXT("The linked subtree has no parameter '%s'. Parameters: %s."),
				*Name, Known.IsEmpty() ? TEXT("(none)") : *FString::Join(Known, TEXT(", "))), Name);
		}
		Overridden.Add(Desc->ID);
	}
	bool bDecoded = true;
	EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, Parameters), [&]()
	{
		// UStateTreeState::SetParametersPropertyOverridden: a cleared override takes the linked default
		// back and drops the bindings of that parameter.
		const TArray<FGuid> Before = State->Parameters.PropertyOverrides;
		for (const FGuid& Id : Before)
		{
			if (!Overridden.Contains(Id))
			{
				State->SetParametersPropertyOverridden(Id, false);
			}
		}
		for (int32 Index = 0; Index < Values->ObjectKeys.Num() && bDecoded; ++Index)
		{
			State->SetParametersPropertyOverridden(Overridden[Index], true);
			bDecoded = DecodeBagValue(ValuesReader, Values->ObjectKeys[Index], State->Parameters.Parameters,
				FName(*Values->ObjectKeys[Index]), *State);
		}
	});
	return bDecoded;
}

// ---- hierarchy -------------------------------------------------------------------------------------

struct FDesiredState
{
	FGuid Id;
	FName Name;
	FGuid Parent;
	TArray<FGuid> Children;
};

bool ReadHierarchy(FReader& Reader, const FValue& Items, const FGuid& Parent, TArray<FDesiredState>& Out, TArray<FGuid>& OutLevel)
{
	for (int32 Index = 0; Index < Items.Elements.Num(); ++Index)
	{
		FReader Item = Reader.Child(FString::FromInt(Index), Items.Elements[Index]);
		FDesiredState Desired;
		const FValue* Children = nullptr;
		if (!Item.IsObject())
		{
			return Item.Fail(TEXT("StateTree.InvalidHierarchy"), TEXT("Each state is an object."));
		}
		if (!Item.OnlyFields({TEXT("id"), TEXT("name"), TEXT("children")}) || !Item.Guid(TEXT("id"), Desired.Id)
			|| !ReadName(Item, TEXT("name"), Desired.Name) || !Item.Array(TEXT("children"), Children, false))
		{
			return false;
		}
		if (Out.ContainsByPredicate([&Desired](const FDesiredState& Each) { return Each.Id == Desired.Id; }))
		{
			return Item.Fail(TEXT("StateTree.DuplicateId"), TEXT("Two states share an id."), TEXT("id"));
		}
		Desired.Parent = Parent;
		// A copy: Out grows while the children are read, so a reference into it would dangle.
		const FGuid Id = Desired.Id;
		OutLevel.Add(Id);
		const int32 Slot = Out.Add(MoveTemp(Desired));
		if (Children != nullptr)
		{
			FReader ChildrenReader = Item.Child(TEXT("children"), *Children);
			TArray<FGuid> Level;
			if (!ReadHierarchy(ChildrenReader, *Children, Id, Out, Level))
			{
				return false;
			}
			Out[Slot].Children = MoveTemp(Level);
		}
	}
	return true;
}

bool EditHierarchy(FReader& Op, UStateTreeEditorData& EditorData)
{
	const FValue* Subtrees = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("subtrees")}) || !Op.Array(TEXT("subtrees"), Subtrees))
	{
		return false;
	}
	TArray<FDesiredState> Desired;
	TArray<FGuid> Roots;
	FReader SubtreesReader = Op.Child(TEXT("subtrees"), *Subtrees);
	if (!ReadHierarchy(SubtreesReader, *Subtrees, FGuid(), Desired, Roots))
	{
		return false;
	}
	TMap<FGuid, UStateTreeState*> Existing;
	EditorData.VisitHierarchy([&Existing](UStateTreeState& State, UStateTreeState* /*Parent*/)
	{
		Existing.Add(State.ID, &State);
		return EStateTreeVisitor::Continue;
	});
	// Editor transaction scope: every owner whose array or Parent changes is modified first
	// (FStateTreeViewModel::AddState / MoveSelectedStates / RemoveSelectedStates).
	EditorData.Modify();
	for (const TPair<FGuid, UStateTreeState*>& Pair : Existing)
	{
		Pair.Value->Modify();
	}
	TMap<FGuid, UStateTreeState*> States;
	for (const FDesiredState& Each : Desired)
	{
		UStateTreeState* State = Existing.FindRef(Each.Id);
		if (State == nullptr)
		{
			// FStateTreeViewModel::AddChildState: outered to the parent state (the subtree roots to the editor data).
			UObject* Outer = Each.Parent.IsValid() ? static_cast<UObject*>(States.FindChecked(Each.Parent)) : &EditorData;
			State = NewObject<UStateTreeState>(Outer, FName(), RF_Transactional);
			State->ID = Each.Id;
			State->Name = Each.Name;
		}
		States.Add(Each.Id, State);
	}
	for (const TPair<FGuid, UStateTreeState*>& Pair : Existing)
	{
		if (!States.Contains(Pair.Key))
		{
			// Removed (FStateTreeViewModel::RemoveSelectedStates): detached, left to garbage collection.
			Pair.Value->Children.Reset();
			Pair.Value->Parent = nullptr;
		}
	}
	EditorData.SubTrees.Reset();
	for (const FGuid& Root : Roots)
	{
		UStateTreeState* State = States.FindChecked(Root);
		State->Parent = nullptr;
		EditorData.SubTrees.Add(State);
	}
	for (const FDesiredState& Each : Desired)
	{
		UStateTreeState* State = States.FindChecked(Each.Id);
		State->Children.Reset();
		for (const FGuid& Child : Each.Children)
		{
			UStateTreeState* ChildState = States.FindChecked(Child);
			ChildState->Parent = State;
			State->Children.Add(ChildState);
		}
	}
	EditorData.ReparentStates();
	return true;
}

// ---- state fields ----------------------------------------------------------------------------------

template <typename TEnum>
bool EditEnumField(FReader& Op, const TCHAR* Key, UStateTreeState& State, FName Member, TEnum& Field)
{
	if (!Op.Has(Key))
	{
		return true;
	}
	int64 Value = 0;
	if (!ReadEnum(Op, Key, *StaticEnum<TEnum>(), Value))
	{
		return false;
	}
	if (static_cast<int64>(Field) != Value)
	{
		EditMember(State, Member, [&Field, Value]() { Field = static_cast<TEnum>(Value); });
	}
	return true;
}

bool EditStateFields(FReader& Op, UStateTreeEditorData& EditorData)
{
	FGuid StateId;
	if (!Op.OnlyFields({TEXT("op"), TEXT("id"), TEXT("name"), TEXT("description"), TEXT("tag"), TEXT("type"), TEXT("selection"),
			TEXT("tasks_completion"), TEXT("enabled"), TEXT("weight"), TEXT("tick_rate"), TEXT("check_prerequisites"),
			TEXT("enter_event"), TEXT("linked_subtree"), TEXT("linked_asset")})
		|| !Op.Guid(TEXT("id"), StateId))
	{
		return false;
	}
	UStateTreeState* State = FindState(EditorData, StateId);
	if (State == nullptr)
	{
		return Op.Fail(TEXT("StateTree.StateNotFound"), TEXT("The state does not exist."), TEXT("id"));
	}
	if (!EditEnumField(Op, TEXT("type"), *State, GET_MEMBER_NAME_CHECKED(UStateTreeState, Type), State->Type))
	{
		return false;
	}
	if (Op.Has(TEXT("linked_subtree")))
	{
		FGuid TargetId;
		if (!Op.Guid(TEXT("linked_subtree"), TargetId, false))
		{
			return false;
		}
		FStateTreeStateLink Link;
		if (TargetId.IsValid())
		{
			const UStateTreeState* Target = FindState(EditorData, TargetId);
			if (Target == nullptr)
			{
				return Op.Fail(TEXT("StateTree.StateNotFound"), TEXT("The linked subtree does not exist."), TEXT("linked_subtree"));
			}
			Link = Target->GetLinkToState();
		}
		if (Link.ID != State->LinkedSubtree.ID)
		{
			EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, LinkedSubtree), [State, &Link]() { State->LinkedSubtree = Link; });
		}
	}
	if (Op.Has(TEXT("linked_asset")))
	{
		FString Path;
		if (!Op.String(TEXT("linked_asset"), Path, false))
		{
			return false;
		}
		UStateTree* Asset = nullptr;
		if (!Path.IsEmpty())
		{
			Asset = LoadObject<UStateTree>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (Asset == nullptr)
			{
				return Op.Fail(TEXT("StateTree.LinkedAssetNotFound"), FString::Printf(TEXT("'%s' is not a StateTree asset."), *Path), TEXT("linked_asset"));
			}
		}
		if (Asset != State->LinkedAsset)
		{
			EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, LinkedAsset), [State, Asset]() { State->LinkedAsset = Asset; });
		}
	}
	if (Op.Has(TEXT("name")))
	{
		FName Name;
		if (!ReadName(Op, TEXT("name"), Name))
		{
			return false;
		}
		if (!Name.IsEqual(State->Name, ENameCase::CaseSensitive))
		{
			EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, Name), [State, Name]() { State->Name = Name; });
		}
	}
	if (Op.Has(TEXT("description")))
	{
		FString Description;
		if (!Op.String(TEXT("description"), Description, false))
		{
			return false;
		}
		if (Description != State->Description)
		{
			EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, Description), [State, &Description]() { State->Description = Description; });
		}
	}
	if (Op.Has(TEXT("tag")))
	{
		FGameplayTag Tag;
		if (!ReadTag(Op, TEXT("tag"), Tag))
		{
			return false;
		}
		if (Tag != State->Tag)
		{
			EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, Tag), [State, Tag]() { State->Tag = Tag; });
		}
	}
	if (!EditEnumField(Op, TEXT("selection"), *State, GET_MEMBER_NAME_CHECKED(UStateTreeState, SelectionBehavior), State->SelectionBehavior)
		|| !EditEnumField(Op, TEXT("tasks_completion"), *State, GET_MEMBER_NAME_CHECKED(UStateTreeState, TasksCompletion), State->TasksCompletion))
	{
		return false;
	}
	if (Op.Has(TEXT("enabled")))
	{
		bool bEnabled = true;
		if (!Op.Bool(TEXT("enabled"), bEnabled))
		{
			return false;
		}
		if (bEnabled != State->bEnabled)
		{
			EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, bEnabled), [State, bEnabled]() { State->bEnabled = bEnabled; });
		}
	}
	if (Op.Has(TEXT("check_prerequisites")))
	{
		bool bCheck = true;
		if (!Op.Bool(TEXT("check_prerequisites"), bCheck))
		{
			return false;
		}
		if (bCheck != State->bCheckPrerequisitesWhenActivatingChildDirectly)
		{
			EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, bCheckPrerequisitesWhenActivatingChildDirectly),
				[State, bCheck]() { State->bCheckPrerequisitesWhenActivatingChildDirectly = bCheck; });
		}
	}
	if (Op.Has(TEXT("weight")))
	{
		double Weight = 1.0;
		if (!Op.Number(TEXT("weight"), Weight))
		{
			return false;
		}
		if (Weight < 0.0)
		{
			return Op.Fail(TEXT("StateTree.InvalidValue"), TEXT("Weight must be >= 0."), TEXT("weight"));
		}
		const float NewWeight = static_cast<float>(Weight);
		if (NewWeight != State->Weight)
		{
			EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, Weight), [State, NewWeight]() { State->Weight = NewWeight; });
		}
	}
	// Find, not Has: null (no custom tick rate / no required event) is a value here (Has() is false for null).
	if (Op.Find(TEXT("tick_rate")) != nullptr)
	{
		const FValue* TickRate = Op.Find(TEXT("tick_rate"));
		const bool bHas = TickRate->Kind != EAssetDocumentRuntimeValueKind::Null;
		double Rate = State->CustomTickRate;
		if (bHas && !Op.Number(TEXT("tick_rate"), Rate))
		{
			return false;
		}
		if (Rate < 0.0)
		{
			return Op.Fail(TEXT("StateTree.InvalidValue"), TEXT("The custom tick rate must be >= 0."), TEXT("tick_rate"));
		}
		const float NewRate = static_cast<float>(Rate);
		if (bHas != State->bHasCustomTickRate)
		{
			EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, bHasCustomTickRate), [State, bHas]() { State->bHasCustomTickRate = bHas; });
		}
		if (NewRate != State->CustomTickRate)
		{
			EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, CustomTickRate), [State, NewRate]() { State->CustomTickRate = NewRate; });
		}
	}
	if (Op.Find(TEXT("enter_event")) != nullptr)
	{
		const FValue* Event = Op.Find(TEXT("enter_event"));
		const bool bHas = Event->Kind != EAssetDocumentRuntimeValueKind::Null;
		FStateTreeEventDesc Desc = State->RequiredEventToEnter;
		if (bHas && !ReadEvent(Op, TEXT("enter_event"), Desc))
		{
			return false;
		}
		if (bHas != State->bHasRequiredEventToEnter)
		{
			EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, bHasRequiredEventToEnter),
				[State, bHas]() { State->bHasRequiredEventToEnter = bHas; });
		}
		if (bHas && !(Desc == State->RequiredEventToEnter && Desc.bConsumeEventOnSelect == State->RequiredEventToEnter.bConsumeEventOnSelect))
		{
			EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, RequiredEventToEnter),
				[State, &Desc]() { State->RequiredEventToEnter = Desc; });
		}
	}
	return true;
}

// ---- transitions -----------------------------------------------------------------------------------

bool ReadTransition(FReader& Item, UStateTreeEditorData& EditorData, FStateTreeTransition& Transition)
{
	int64 Trigger = 0;
	int64 Priority = 0;
	const FValue* To = nullptr;
	bool bEnabled = true;
	if (!Item.OnlyFields({TEXT("id"), TEXT("trigger"), TEXT("event"), TEXT("to"), TEXT("priority"), TEXT("delay"), TEXT("enabled")})
		|| !ReadEnum(Item, TEXT("trigger"), *StaticEnum<EStateTreeTransitionTrigger>(), Trigger)
		|| !ReadEnum(Item, TEXT("priority"), *StaticEnum<EStateTreeTransitionPriority>(), Priority)
		|| !Item.Object(TEXT("to"), To) || !Item.Bool(TEXT("enabled"), bEnabled))
	{
		return false;
	}
	Transition.Trigger = static_cast<EStateTreeTransitionTrigger>(Trigger);
	Transition.Priority = static_cast<EStateTreeTransitionPriority>(Priority);
	Transition.bTransitionEnabled = bEnabled;
	if (Item.Has(TEXT("event")) && !ReadEvent(Item, TEXT("event"), Transition.RequiredEvent))
	{
		return false;
	}
	FReader ToReader = Item.Child(TEXT("to"), *To);
	int64 LinkType = 0;
	FGuid TargetId;
	if (!ToReader.OnlyFields({TEXT("type"), TEXT("state")}) || !ReadEnum(ToReader, TEXT("type"), *StaticEnum<EStateTreeTransitionType>(), LinkType)
		|| !ToReader.Guid(TEXT("state"), TargetId, false))
	{
		return false;
	}
	if (static_cast<EStateTreeTransitionType>(LinkType) == EStateTreeTransitionType::GotoState)
	{
		const UStateTreeState* Target = FindState(EditorData, TargetId);
		if (Target == nullptr)
		{
			return ToReader.Fail(TEXT("StateTree.StateNotFound"), TEXT("The transition target state does not exist."), TEXT("state"));
		}
		Transition.State = Target->GetLinkToState();
	}
	else
	{
		Transition.State = FStateTreeStateLink(static_cast<EStateTreeTransitionType>(LinkType));
	}
	const FValue* Delay = Item.Find(TEXT("delay"));
	if (Delay == nullptr || Delay->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		Transition.bDelayTransition = false;
		return true;
	}
	const FValue* DelayObject = nullptr;
	if (!Item.Object(TEXT("delay"), DelayObject))
	{
		return false;
	}
	FReader DelayReader = Item.Child(TEXT("delay"), *DelayObject);
	double Duration = 0.0;
	double Variance = 0.0;
	if (!DelayReader.OnlyFields({TEXT("duration"), TEXT("variance")}) || !DelayReader.Number(TEXT("duration"), Duration)
		|| !DelayReader.Number(TEXT("variance"), Variance, false))
	{
		return false;
	}
	// The Details panel clears the delay of completion transitions (UStateTreeState::PostEditChangeChainProperty).
	if (EnumHasAnyFlags(Transition.Trigger, EStateTreeTransitionTrigger::OnStateCompleted))
	{
		return Item.Fail(TEXT("StateTree.InvalidValue"), TEXT("Completion transitions (OnStateCompleted / Succeeded / Failed) cannot be delayed."),
			TEXT("delay"));
	}
	if (Duration < 0.0 || Duration > 25.0 || Variance < 0.0 || Variance > 25.0)
	{
		return DelayReader.Fail(TEXT("StateTree.InvalidValue"), TEXT("Delay duration and variance are 0..25 seconds."), TEXT("duration"));
	}
	Transition.bDelayTransition = true;
	Transition.DelayDuration = static_cast<float>(Duration);
	Transition.DelayRandomVariance = static_cast<float>(Variance);
	return true;
}

bool EditTransitions(FReader& Op, UStateTreeEditorData& EditorData)
{
	FGuid StateId;
	const FValue* Items = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("state"), TEXT("transitions")}) || !Op.Guid(TEXT("state"), StateId)
		|| !Op.Array(TEXT("transitions"), Items))
	{
		return false;
	}
	UStateTreeState* State = FindState(EditorData, StateId);
	if (State == nullptr)
	{
		return Op.Fail(TEXT("StateTree.StateNotFound"), TEXT("The state does not exist."), TEXT("state"));
	}
	TArray<FStateTreeTransition> Desired;
	FReader ItemsReader = Op.Child(TEXT("transitions"), *Items);
	for (int32 Index = 0; Index < Items->Elements.Num(); ++Index)
	{
		FReader Item = ItemsReader.Child(FString::FromInt(Index), Items->Elements[Index]);
		FGuid Id;
		if (!Item.IsObject() || !Item.Guid(TEXT("id"), Id))
		{
			return Item.IsObject() ? false : Item.Fail(TEXT("StateTree.InvalidOp"), TEXT("Each transition is an object."));
		}
		if (Desired.ContainsByPredicate([&Id](const FStateTreeTransition& Each) { return Each.ID == Id; }))
		{
			return Item.Fail(TEXT("StateTree.DuplicateId"), TEXT("Two transitions share an id."), TEXT("id"));
		}
		const FStateTreeTransition* Existing = FindTransition(*State, Id);
		FStateTreeTransition& Transition = Desired.Add_GetRef(Existing != nullptr ? *Existing : FStateTreeTransition());
		Transition.ID = Id;
		if (!ReadTransition(Item, EditorData, Transition))
		{
			return false;
		}
	}
	EditMember(*State, GET_MEMBER_NAME_CHECKED(UStateTreeState, Transitions), [State, &Desired]() { State->Transitions = MoveTemp(Desired); });
	return true;
}
} // namespace

bool HandleEdit(FCall& Call, FObjectBuilder& Result)
{
	UStateTree* Tree = nullptr;
	UStateTreeEditorData* EditorData = nullptr;
	const FValue* Ops = nullptr;
	if (!GetTree(Call, Tree, EditorData) || !Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	// One editor transaction for the call; the editor's per-edit transactions nest into it.
	const FScopedTransaction Transaction(LOCTEXT("EditStateTree", "Edit State Tree"));
	Tree->Modify();
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("StateTree.InvalidOp"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind))
		{
			return false;
		}
		bool bOk = false;
		if (Kind == TEXT("schema"))
		{
			bOk = EditSchema(Op, *EditorData);
		}
		else if (Kind == TEXT("tree"))
		{
			bOk = EditTree(Op, *EditorData);
		}
		else if (Kind == TEXT("parameters"))
		{
			bOk = EditParameters(Op, *EditorData);
		}
		else if (Kind == TEXT("parameter_values"))
		{
			bOk = EditParameterValues(Op, *EditorData);
		}
		else if (Kind == TEXT("hierarchy"))
		{
			bOk = EditHierarchy(Op, *EditorData);
		}
		else if (Kind == TEXT("state"))
		{
			bOk = EditStateFields(Op, *EditorData);
		}
		else if (Kind == TEXT("transitions"))
		{
			bOk = EditTransitions(Op, *EditorData);
		}
		else if (Kind == TEXT("nodes"))
		{
			bOk = EditNodes(Call, Op, *EditorData);
		}
		else if (Kind == TEXT("bindings"))
		{
			bOk = EditBindings(Call, Op, *EditorData);
		}
		else
		{
			return Op.Fail(TEXT("StateTree.InvalidOp"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
		}
		if (!bOk)
		{
			if (Call.Error.Code.IsEmpty())
			{
				Op.Fail(TEXT("StateTree.InvalidOp"), FString::Printf(TEXT("The %s op failed."), *Kind));
			}
			return false;
		}
	}
	Result.AddInt(TEXT("ops"), Ops->Elements.Num());
	return true;
}
} // namespace UE::AssetDocument::Graph::StateTree

#undef LOCTEXT_NAMESPACE
