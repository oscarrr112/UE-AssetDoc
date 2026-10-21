// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// StateTree primitives (ai.state_tree.*, served by the AI provider "ai", owner AI). Editor-equivalent paths:
//   states             -> FStateTreeViewModel (AddState / AddChildState / MoveSelectedStates /
//                         RemoveSelectedStates: Modify, array edits, Parent links, Outer = parent state);
//   state / transition -> the state Details panel (Modify, PreEditChange, member write,
//   fields                PostEditChangeChainProperty: type changes reset tasks / links, linked
//                         states take the linked subtree's parameter layout);
//   nodes              -> the node picker / array editor (a node equal to the picker's for the
//                         same type, built in AssetDocumentStateTreeNodes.cpp);
//   parameters         -> the property bag Details (UPropertyBag::GetOrCreateFromDescs,
//                         MigrateToNewBagStruct, UStateTreeState::SetParametersPropertyOverridden);
//   bindings           -> the binding menu (FStateTreeEditorPropertyBindings::AddBinding /
//                         AddFunctionBinding / RemoveBindings);
//   compile            -> the Compile button (UStateTreeEditingSubsystem::CompileStateTree, which
//                         runs ValidateStateTree first).
// There is no inverse code: Core rolls back the whole package snapshot; the StateTree derived-state
// rebuilder recompiles the restored editor data (AssetDocumentStateTreeCompile.cpp).
//
// DTOs (all carry "schema_version": 1; ids are FGuid digit strings):
//   ai.state_tree.read.v1    {offset?} -> {total, next_offset, records[]} in a fixed order:
//     {kind: tree, schema_class, schema{}, schema_defaults[], schema_unsupported[], editor_data_class,
//      global_tasks_completion, context[{name, id, struct}], allows{evaluators, enter_conditions,
//      considerations, multiple_tasks, global_parameters, tasks_completion}, compiled_hash, editor_hash, ready}
//     {kind: parameters, owner ("" = tree, else state id), id, fixed, props[{name, id, type{pin type},
//      supported, value?, default}], overrides[]}
//     {kind: state, id, parent, index, name, description, tag, type, selection, tasks_completion, enabled,
//      weight, has_tick_rate, tick_rate, check_prerequisites, has_enter_event, enter_event{tag, payload,
//      consume}, enter_event_id, linked_subtree, linked_asset, transitions[{id, trigger, event{}, event_id,
//      to{type, state}, priority, delay, delay_duration, delay_variance, enabled, delegate}]}
//     {kind: node, list, state, transition, index, id, node_id, type, instance_type, name, enabled?,
//      considered?, considered_editable?, evaluation_mode?, indent, operand, node{}, node_defaults[],
//      node_reset{}, instance{}, instance_defaults[], instance_reset{}, unsupported[]} (*_reset: the default of
//      each value that differs from it)
//     {kind: binding, source{struct, path}, target{struct, path}, output, function?{id, node_id, type,
//      instance{}, instance_defaults[], node{}, node_defaults[]}}
//   ai.state_tree.edit.v1    {ops[]} (one editor transaction per call) -> {ops}:
//     {op: schema, values{}} | {op: tree, global_tasks_completion} |
//     {op: parameters, state?, props[{id, name, type{pin type}, value?}]} |
//     {op: parameter_values, state, values{}} (linked states: the overridden values) |
//     {op: hierarchy, subtrees[{id, name, children[...]}]} |
//     {op: state, id, name?, description?, tag?, type?, selection?, tasks_completion?, enabled?, weight?,
//      tick_rate? (number | null), check_prerequisites?, enter_event? ({tag, payload, consume} | null),
//      linked_subtree?, linked_asset?} |
//     {op: transitions, state, transitions[{id, trigger, event?, to{type, state?}, priority, delay?
//      ({duration, variance} | null), enabled}]} |
//     {op: nodes, list, state?, transition?, nodes[{id, type, name?, indent?, operand?, enabled?, considered?,
//      evaluation_mode?, node?{}, instance?{}}]} |
//     {op: bindings, targets[], bindings[{target{struct, path}, source{struct, path}} |
//      {target, function{id?, type, instance?{}, bindings[...]}}]}
//   ai.state_tree.compile.v1 {} -> {succeeded, status, messages[{severity, message, state_id, state_path,
//                           item_id, item_name}]} (Mutating: compiling writes the runtime data)

#include "StateTree/AssetDocumentStateTreePrimitives.h"

#include "AssetDocumentEditorPropertyValueCodec.h"
#include "AssetDocumentGraphProvider.h"
#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Engine/Blueprint.h"
#include "Blueprint/StateTreeConsiderationBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "StateTree.h"
#include "StateTreeConditionBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreeEditorData.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeNodeBase.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "UObject/EnumProperty.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::StateTree
{
namespace
{
constexpr uint32 StateTreeProviderVersion = 1;

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = StateTreeProviderVersion;
	Descriptor.ProviderId = ProviderId;
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = bMutating ? EAssetDocumentPrimitiveAccess::Mutating : EAssetDocumentPrimitiveAccess::ReadOnly;
	if (bMutating)
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
		Descriptor.MutationFootprintSchemaId = TEXT("graph.footprint.whole-package.v1");
	}
	else
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify};
	}
	Descriptor.bRequiresActiveTransaction = bMutating;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.StableDiagnosticCodes = {
		TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), TEXT("Graph.UnknownField"), TEXT("Graph.InvalidArgument"),
		TEXT("Graph.UnsupportedSchemaVersion"), TEXT("Graph.PieActive"), TEXT("Graph.ResultTooLarge"),
		TEXT("StateTree.UnsupportedAsset"), TEXT("StateTree.MissingEditorData"), TEXT("StateTree.InvalidOp"),
		TEXT("StateTree.StateNotFound"), TEXT("StateTree.TransitionNotFound"), TEXT("StateTree.DuplicateId"),
		TEXT("StateTree.InvalidName"), TEXT("StateTree.UnknownGameplayTag"), TEXT("StateTree.UnknownType"),
		TEXT("StateTree.NodeNotAllowed"), TEXT("StateTree.ListNotAllowed"), TEXT("StateTree.UnknownProperty"),
		TEXT("StateTree.UnsupportedProperty"), TEXT("StateTree.InvalidValue"), TEXT("StateTree.InvalidParameter"),
		TEXT("StateTree.ParameterLayoutFixed"), TEXT("StateTree.LinkedAssetNotFound"), TEXT("StateTree.InvalidBinding"),
		TEXT("StateTree.BindingSourceNotAccessible"), TEXT("StateTree.BindingTypeMismatch"),
		TEXT("StateTree.InvalidPropertyFunction"), TEXT("StateTree.InvalidHierarchy")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset,
		bMutating ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true});

	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	Evidence.PublicModule = TEXT("AssetDocumentAI");
	Evidence.PublicHeader = TEXT("AssetDocumentAIPrimitiveProvider.h");
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.core-package-snapshot.v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = TEXT("UEAssetDoc.StateTree.Python");
	Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.StateTree.Python")};
	Evidence.VerificationTestIds = {TEXT("UEAssetDoc.StateTree.Python")};
	Evidence.AuthorityFingerprint = FString::Printf(TEXT("ue-5.7.4|state_tree|%s|%s"), PrimitiveId, AuthoritativeApi);
	return Descriptor;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	return {
		MakeDescriptor(TEXT("ai.state_tree.read.v1"), false,
			TEXT("UStateTreeEditorData::VisitHierarchy;UStateTreeEditorData::GetRootParametersPropertyBag;UStateTreeSchema::GetContextDataDescs;FStateTreeEditorPropertyBindings::GetBindings;UStateTreeEditingSubsystem::CalculateStateTreeHash")),
		MakeDescriptor(TEXT("ai.state_tree.edit.v1"), true,
			TEXT("UObject::PreEditChange;UObject::PostEditChangeChainProperty;UStateTreeState::SetParametersPropertyOverridden;UStateTreeState::UpdateParametersFromLinkedSubtree;UPropertyBag::GetOrCreateFromDescs;FInstancedPropertyBag::MigrateToNewBagStruct;FStateTreeEditorPropertyBindings::AddFunctionBinding;FPropertyBindingBindingCollection::AddBinding;FPropertyBindingBindingCollection::RemoveBindings;UE::PropertyBinding::GetPropertyCompatibility")),
		MakeDescriptor(TEXT("ai.state_tree.compile.v1"), true,
			TEXT("UStateTreeEditingSubsystem::CompileStateTree;UStateTreeEditingSubsystem::ValidateStateTree")),
	};
}

const FBinding Bindings[] = {
	{TEXT("ai.state_tree.read.v1"), &HandleRead},
	{TEXT("ai.state_tree.edit.v1"), &HandleEdit},
	{TEXT("ai.state_tree.compile.v1"), &HandleCompile},
};

const TCHAR* const NodeListNames[] = {
	TEXT("evaluators"), TEXT("global_tasks"), TEXT("enter_conditions"), TEXT("tasks"), TEXT("single_task"),
	TEXT("considerations"), TEXT("transition_conditions")};

// Float members: shortest float32 rendering on encode, integral numbers accepted on decode.
void Walk(const FProperty& Property, FValue& Value, bool bElement, bool bEncode)
{
	if (!bElement && Property.ArrayDim > 1)
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Array)
		{
			for (FValue& Element : Value.Elements)
			{
				Walk(Property, Element, true, bEncode);
			}
		}
		return;
	}
	if (Property.IsA<FFloatProperty>() || Property.IsA<FDoubleProperty>())
	{
		if (!bEncode && Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
		{
			Value.Kind = EAssetDocumentRuntimeValueKind::Number;
			Value.NumberValue = static_cast<double>(Value.IntegerValue);
			Value.IntegerValue = 0;
		}
		if (bEncode && Property.IsA<FFloatProperty>() && Value.Kind == EAssetDocumentRuntimeValueKind::Number)
		{
			Value.NumberValue = ShortestFloat(static_cast<float>(Value.NumberValue));
		}
		return;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Object && Struct->Struct != nullptr)
		{
			for (int32 Index = 0; Index < Value.ObjectKeys.Num() && Index < Value.Elements.Num(); ++Index)
			{
				for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
				{
					if (FStringView(It->GetAuthoredName()).Equals(Value.ObjectKeys[Index], ESearchCase::CaseSensitive))
					{
						Walk(**It, Value.Elements[Index], false, bEncode);
						break;
					}
				}
			}
		}
		return;
	}
	const FProperty* Inner = nullptr;
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		Inner = Array->Inner;
	}
	else if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		Inner = Set->ElementProp;
	}
	else if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		Inner = Map->ValueProp;
	}
	if (Inner != nullptr && (Value.Kind == EAssetDocumentRuntimeValueKind::Array || Value.Kind == EAssetDocumentRuntimeValueKind::Object))
	{
		for (FValue& Element : Value.Elements)
		{
			Walk(*Inner, Element, false, bEncode);
		}
	}
}

FProperty* FindAuthored(const UStruct& Type, FStringView Name, EValueOwner Owner, bool& bOutExists)
{
	bOutExists = false;
	for (TFieldIterator<FProperty> It(&Type); It; ++It)
	{
		if (FStringView(It->GetAuthoredName()).Equals(Name, ESearchCase::CaseSensitive))
		{
			bOutExists = true;
			return IsAuthoredProperty(**It, Owner) ? *It : nullptr;
		}
	}
	return nullptr;
}

const UScriptStruct* FindStructByShortName(const FString& Name, const UScriptStruct* BaseStruct)
{
	const FString Bare = Name.StartsWith(TEXT("F")) ? Name.RightChop(1) : Name;
	const UScriptStruct* Found = nullptr;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		if (BaseStruct != nullptr && !It->IsChildOf(BaseStruct))
		{
			continue;
		}
		if (It->GetName() == Name || It->GetName() == Bare)
		{
			if (Found != nullptr && Found != *It)
			{
				return nullptr;
			}
			Found = *It;
		}
	}
	return Found;
}
} // namespace

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path)
{
	if (Call.Error.Code.IsEmpty())
	{
		Call.Error.Set(Code, Message, Path);
	}
	return false;
}

bool GetTree(FCall& Call, UStateTree*& OutTree, UStateTreeEditorData*& OutEditorData)
{
	OutTree = Cast<UStateTree>(&Call.Asset);
	OutEditorData = OutTree != nullptr ? Cast<UStateTreeEditorData>(OutTree->EditorData) : nullptr;
	if (OutTree == nullptr)
	{
		return Fail(Call, TEXT("StateTree.UnsupportedAsset"), FString::Printf(TEXT("%s is not a StateTree."), *Call.Asset.GetPathName()));
	}
	if (OutEditorData == nullptr)
	{
		return Fail(Call, TEXT("StateTree.MissingEditorData"), FString::Printf(
			TEXT("%s has no StateTree editor data (open and resave it in the StateTree editor)."), *Call.Asset.GetPathName()));
	}
	return true;
}

FString GuidString(const FGuid& Guid)
{
	return Guid.IsValid() ? Guid.ToString(EGuidFormats::Digits) : FString();
}

FString PathOf(const UObject* Object)
{
	return Object != nullptr ? Object->GetPathName() : FString();
}

double ShortestFloat(float Value)
{
	if (!FMath::IsFinite(Value))
	{
		return Value;
	}
	for (int32 Digits = 1; Digits <= 9; ++Digits)
	{
		const FString Text = FString::Printf(TEXT("%.*g"), Digits, static_cast<double>(Value));
		const double Parsed = FCString::Atod(*Text);
		if (static_cast<float>(Parsed) == Value)
		{
			return Parsed;
		}
	}
	return Value;
}

FString EnumToName(const UEnum& Enum, int64 Value)
{
	return Enum.GetNameStringByValue(Value);
}

bool ReadEnum(FReader& Reader, FStringView Key, const UEnum& Enum, int64& OutValue, bool bRequired)
{
	if (!bRequired && !Reader.Has(Key))
	{
		return true;
	}
	FString Name;
	if (!Reader.String(Key, Name, true))
	{
		return false;
	}
	const int64 Value = Enum.GetValueByNameString(Name, EGetByNameFlags::None);
	const int32 Index = Value == INDEX_NONE ? INDEX_NONE : Enum.GetIndexByValue(Value);
	if (Value == INDEX_NONE || (Index != INDEX_NONE && Enum.HasMetaData(TEXT("Hidden"), Index)))
	{
		TArray<FString> Names;
		for (int32 Each = 0; Each < Enum.NumEnums() - (Enum.ContainsExistingMax() ? 1 : 0); ++Each)
		{
			if (!Enum.HasMetaData(TEXT("Hidden"), Each))
			{
				Names.Add(Enum.GetNameStringByIndex(Each));
			}
		}
		return Reader.Fail(TEXT("StateTree.InvalidValue"), FString::Printf(TEXT("'%s' is not a %s value; expected one of: %s."),
			*Name, *Enum.GetName(), *FString::Join(Names, TEXT(", "))), Key);
	}
	OutValue = Value;
	return true;
}

const TCHAR* NodeListName(ENodeList List)
{
	return NodeListNames[static_cast<int32>(List)];
}

bool ParseNodeList(FStringView Name, ENodeList& OutList)
{
	for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(NodeListNames)); ++Index)
	{
		if (Name.Equals(NodeListNames[Index], ESearchCase::CaseSensitive))
		{
			OutList = static_cast<ENodeList>(Index);
			return true;
		}
	}
	return false;
}

const UScriptStruct* GetNodeBaseStruct(ENodeList List)
{
	switch (List)
	{
	case ENodeList::Evaluators:
		return FStateTreeEvaluatorBase::StaticStruct();
	case ENodeList::GlobalTasks:
	case ENodeList::Tasks:
	case ENodeList::SingleTask:
		return FStateTreeTaskBase::StaticStruct();
	case ENodeList::Considerations:
		return FStateTreeConsiderationBase::StaticStruct();
	default:
		return FStateTreeConditionBase::StaticStruct();
	}
}

const UClass* GetNodeBaseClass(ENodeList List)
{
	switch (List)
	{
	case ENodeList::Evaluators:
		return UStateTreeEvaluatorBlueprintBase::StaticClass();
	case ENodeList::GlobalTasks:
	case ENodeList::Tasks:
	case ENodeList::SingleTask:
		return UStateTreeTaskBlueprintBase::StaticClass();
	case ENodeList::Considerations:
		return UStateTreeConsiderationBlueprintBase::StaticClass();
	default:
		return UStateTreeConditionBlueprintBase::StaticClass();
	}
}

UStateTreeState* FindState(const UStateTreeEditorData& EditorData, const FGuid& StateId)
{
	return StateId.IsValid() ? const_cast<UStateTreeEditorData&>(EditorData).GetMutableStateByID(StateId) : nullptr;
}

FStateTreeTransition* FindTransition(UStateTreeState& State, const FGuid& TransitionId)
{
	return State.Transitions.FindByPredicate([&TransitionId](const FStateTreeTransition& Transition)
	{
		return Transition.ID == TransitionId;
	});
}

bool ResolveNodeList(UStateTreeEditorData& EditorData, ENodeList List, const FGuid& StateId, const FGuid& TransitionId, FNodeListRef& Out)
{
	Out = FNodeListRef();
	Out.List = List;
	if (List == ENodeList::Evaluators || List == ENodeList::GlobalTasks)
	{
		Out.Owner = &EditorData;
		Out.Nodes = List == ENodeList::Evaluators ? &EditorData.Evaluators : &EditorData.GlobalTasks;
		Out.MemberName = List == ENodeList::Evaluators ? GET_MEMBER_NAME_CHECKED(UStateTreeEditorData, Evaluators)
			: GET_MEMBER_NAME_CHECKED(UStateTreeEditorData, GlobalTasks);
		return true;
	}
	Out.State = FindState(EditorData, StateId);
	if (Out.State == nullptr)
	{
		return false;
	}
	Out.Owner = Out.State;
	switch (List)
	{
	case ENodeList::EnterConditions:
		Out.Nodes = &Out.State->EnterConditions;
		Out.MemberName = GET_MEMBER_NAME_CHECKED(UStateTreeState, EnterConditions);
		return true;
	case ENodeList::Tasks:
		Out.Nodes = &Out.State->Tasks;
		Out.MemberName = GET_MEMBER_NAME_CHECKED(UStateTreeState, Tasks);
		return true;
	case ENodeList::SingleTask:
		Out.Single = &Out.State->SingleTask;
		Out.MemberName = GET_MEMBER_NAME_CHECKED(UStateTreeState, SingleTask);
		return true;
	case ENodeList::Considerations:
		Out.Nodes = &Out.State->Considerations;
		Out.MemberName = GET_MEMBER_NAME_CHECKED(UStateTreeState, Considerations);
		return true;
	default:
		Out.Transition = FindTransition(*Out.State, TransitionId);
		Out.Nodes = Out.Transition != nullptr ? &Out.Transition->Conditions : nullptr;
		Out.MemberName = GET_MEMBER_NAME_CHECKED(UStateTreeState, Transitions);
		return Out.Nodes != nullptr;
	}
}

void EditMember(UObject& Owner, FName MemberName, TFunctionRef<void()> Write)
{
	FProperty* Member = Owner.GetClass()->FindPropertyByName(MemberName);
	Owner.Modify();
	if (Member == nullptr)
	{
		Write();
		return;
	}
	FEditPropertyChain Chain;
	Chain.AddHead(Member);
	Chain.SetActivePropertyNode(Member);
	Chain.SetActiveMemberPropertyNode(Member);
	Owner.PreEditChange(Chain);
	Write();
	FPropertyChangedEvent Event(Member, EPropertyChangeType::ValueSet);
	FPropertyChangedChainEvent ChainEvent(Chain, Event);
	Owner.PostEditChangeChainProperty(ChainEvent);
}

bool IsAuthoredProperty(const FProperty& Property, EValueOwner Owner)
{
	if (Property.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst))
	{
		return false;
	}
	if (Owner == EValueOwner::PropertyBag)
	{
		return true;
	}
	if (!Property.HasAnyPropertyFlags(CPF_Edit))
	{
		return false;
	}
	if (Owner == EValueOwner::InstanceObject || Owner == EValueOwner::Schema)
	{
		if (Property.HasAnyPropertyFlags(CPF_DisableEditOnInstance))
		{
			return false;
		}
	}
	if (Owner == EValueOwner::NodeStruct && Property.GetOwnerStruct() == FStateTreeNodeBase::StaticStruct())
	{
		return false;
	}
	if (Owner == EValueOwner::InstanceStruct || Owner == EValueOwner::InstanceObject)
	{
		const EStateTreePropertyUsage Usage = UE::StateTree::GetUsageFromMetaData(&Property);
		if (Usage == EStateTreePropertyUsage::Output || Usage == EStateTreePropertyUsage::Context)
		{
			return false;
		}
	}
	return true;
}

FValue EncodeValues(const UStruct& Type, const void* Memory, const void* DefaultMemory, EValueOwner Owner,
	TArray<FString>& OutDefaults, TArray<FString>& OutUnsupported)
{
	FObjectBuilder Values;
	for (TFieldIterator<FProperty> It(&Type); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsAuthoredProperty(*Property, Owner))
		{
			continue;
		}
		FString CodecId;
		FValue Value;
		FString Code;
		if (!EditorPropertyValueCodec::Inspect(*Property, CodecId)
			|| !EditorPropertyValueCodec::Encode(*Property, Property->ContainerPtrToValuePtr<void>(Memory), Value, Code))
		{
			OutUnsupported.Add(Property->GetAuthoredName());
			continue;
		}
		Walk(*Property, Value, false, true);
		if (DefaultMemory != nullptr && Property->Identical_InContainer(Memory, DefaultMemory, 0, PPF_None))
		{
			OutDefaults.Add(Property->GetAuthoredName());
		}
		Values.Add(Property->GetAuthoredName(), MoveTemp(Value));
	}
	return Values.Build();
}

FValue EncodeChangedDefaults(const UStruct& Type, const void* Memory, const void* DefaultMemory, EValueOwner Owner)
{
	FObjectBuilder Values;
	if (DefaultMemory == nullptr)
	{
		return Values.Build();
	}
	for (TFieldIterator<FProperty> It(&Type); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsAuthoredProperty(*Property, Owner) || Property->Identical_InContainer(Memory, DefaultMemory, 0, PPF_None))
		{
			continue;
		}
		FString CodecId;
		FValue Value;
		FString Code;
		if (!EditorPropertyValueCodec::Inspect(*Property, CodecId)
			|| !EditorPropertyValueCodec::Encode(*Property, Property->ContainerPtrToValuePtr<void>(DefaultMemory), Value, Code))
		{
			continue;
		}
		Walk(*Property, Value, false, true);
		Values.Add(Property->GetAuthoredName(), MoveTemp(Value));
	}
	return Values.Build();
}

bool DecodeValues(FReader& Reader, FStringView Key, const UStruct& Type, void* Memory, UObject* Owner, EValueOwner ValueOwner,
	TArray<FProperty*>* OutWritten)
{
	const FValue* Values = nullptr;
	if (!Reader.Object(Key, Values, false))
	{
		return false;
	}
	if (Values == nullptr)
	{
		return true;
	}
	FReader ValuesReader = Reader.Child(Key, *Values);
	for (int32 Index = 0; Index < Values->ObjectKeys.Num(); ++Index)
	{
		const FString& Name = Values->ObjectKeys[Index];
		bool bExists = false;
		FProperty* Property = FindAuthored(Type, Name, ValueOwner, bExists);
		if (Property == nullptr)
		{
			if (bExists)
			{
				return ValuesReader.Fail(TEXT("StateTree.UnsupportedProperty"), FString::Printf(
					TEXT("'%s' of %s is not an authored value (outputs, context data and read-only properties are not set by value; "
						"bind context data or leave it to the compiler)."), *Name, *Type.GetName()), Name);
			}
			TArray<FString> Names;
			for (TFieldIterator<FProperty> It(&Type); It; ++It)
			{
				if (IsAuthoredProperty(**It, ValueOwner))
				{
					Names.Add(It->GetAuthoredName());
				}
			}
			return ValuesReader.Fail(TEXT("StateTree.UnknownProperty"), FString::Printf(TEXT("%s has no property '%s'. Properties: %s."),
				*Type.GetName(), *Name, Names.IsEmpty() ? TEXT("(none)") : *FString::Join(Names, TEXT(", "))), Name);
		}
		FString CodecId;
		if (!EditorPropertyValueCodec::Inspect(*Property, CodecId))
		{
			return ValuesReader.Fail(TEXT("StateTree.UnsupportedProperty"), FString::Printf(
				TEXT("'%s' (%s) has no document value form (instanced object properties are edited in the editor)."),
				*Name, *Property->GetCPPType()), Name);
		}
		FValue Coerced = Values->Elements[Index];
		Walk(*Property, Coerced, false, false);
		FString Code;
		if (!EditorPropertyValueCodec::Decode(*Property, Coerced, Property->ContainerPtrToValuePtr<void>(Memory), Code, Owner))
		{
			return ValuesReader.Fail(TEXT("StateTree.InvalidValue"), FString::Printf(TEXT("'%s' (%s) does not accept this value (%s)."),
				*Name, *Property->GetCPPType(), *Code), Name);
		}
		if (OutWritten != nullptr)
		{
			OutWritten->Add(Property);
		}
	}
	return true;
}

const UStruct* ResolveType(const FString& Text, const UScriptStruct* BaseStruct, const UClass* BaseClass)
{
	if (Text.IsEmpty())
	{
		return nullptr;
	}
	if (Text.StartsWith(TEXT("/")))
	{
		UObject* Object = FindObject<UObject>(nullptr, *Text);
		if (Object == nullptr)
		{
			Object = LoadObject<UObject>(nullptr, *Text, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object))
		{
			Object = Blueprint->GeneratedClass;
		}
		if (const UScriptStruct* Struct = Cast<UScriptStruct>(Object))
		{
			return BaseStruct == nullptr || Struct->IsChildOf(BaseStruct) ? Struct : nullptr;
		}
		if (const UClass* Class = Cast<UClass>(Object))
		{
			return BaseClass == nullptr || Class->IsChildOf(BaseClass) ? Class : nullptr;
		}
		return nullptr;
	}
	return FindStructByShortName(Text, BaseStruct);
}

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors = BuildDescriptors();
	return Descriptors;
}

TConstArrayView<FBinding> GetBindings()
{
	return Bindings;
}
} // namespace UE::AssetDocument::Graph::StateTree

TSharedRef<IAssetDocumentPrimitiveProvider> UE::AssetDocument::Graph::StateTree::CreateProvider()
{
	return CreateAssetDocumentAssetTablePrimitiveProvider(ProviderId, &GetDescriptors, &GetBindings);
}
