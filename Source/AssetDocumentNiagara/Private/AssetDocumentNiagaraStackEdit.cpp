// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Module stack ops of fx.niagara.edit.v1 (N3), made through the Niagara stack view models the way the stack widgets
// make them. Every op re-resolves its stack, group, module and input by name (an edit refreshes or rebuilds the
// entries), so nothing is cached across ops.
//
// A stack location is {emitter?, group, event?, stage?}: emitter omitted = the system stack; group SystemSpawn,
// SystemUpdate, EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, EventHandler (event: handler index)
// or SimulationStage (stage: SimulationStageName).
//   {op: "add_module", <location>, name, script, version?, after?}
//        the group's "+" for a module script asset: FNiagaraStackGraphUtilities::AddScriptModuleToStack (exported) with
//        name as the suggested name and the version, inserted after the module "after" (omitted: first), then the
//        add action's initialization (every resettable input of the new module's input hierarchy reset, as
//        FNiagaraStackGraphUtilities::InitializeStackFunctionInputs does; not exported, replayed here). The usage
//        filter of the "+" menu applies (Niagara.ModuleUsage). script "ScratchPad:<name>" is a scratch pad module (the
//        system's scratch pads first, then those of the emitter whose stack it is), as the "+" menu offers them.
//   {op: "add_assignment", <location>, after?, targets: [{name, type}]}
//        "Set new or existing parameter directly": AddParameterModuleToStack (exported) with no target, then the
//        module's "+" per target (UNiagaraStackModuleItem::AddInput, exported).
//   {op: "delete_module", <location>, name}      UNiagaraStackModuleItem::Delete (inherited modules refused)
//   {op: "move_module", <location>, name, after?} drag and drop: UNiagaraStackEntry::Drop with an
//        FNiagaraStackEntryDragDropOp of the module onto the group (first) or below the module "after"; the module
//        may come from another group of the same graph (system graph, or the emitter graph holding all its stacks).
//   {op: "module", <location>, name, enabled?, version?, display_name?, note?}
//        the checkbox (UNiagaraStackItem::SetIsEnabled), the version menu (ChangeScriptVersion; "" = the exposed
//        version), rename (UNiagaraStackEntry::OnRenamed; "" = none) and the stack note
//        (UNiagaraStackEntry::SetStackNoteData; null = DeleteStackNoteData).
//   {op: "assignment", <location>, name, remove?: [target names], add?: [{name, type}]}
//        Set Parameters targets: the input's Delete (UNiagaraStackFunctionInput::DeleteInput) and the module's "+"
//        (UNiagaraStackModuleItem::AddInput).
//   {op: "inputs", <location>, module, set?: {input: value}, reset?: [input]}
//        value forms (MCP/schemas/assetdocument/Niagara.md): literal (SetLocalValue), {Link, Type?}
//        (SetLinkedParameterValue), {Dynamic: {Script, Version?, Inputs}} (SetDynamicInput, or ChangeScriptVersion
//        when the input holds that script already; the nested Inputs are complete), {Expression}
//        (SetCustomExpression), {DataInterface: {Class?, Properties?}} (SetDataInterfaceValue when the class differs,
//        then the data interface's Details: Modify, PreEditChange, write, PostEditChangeProperty and the stack
//        object's NotifyPostChange; Properties complete), {Object} (SetObjectAssetValue); reset = the reset arrow
//        (UNiagaraStackFunctionInput::Reset). Values equal to the input's current one change nothing. Inputs that
//        do not exist yet (a static switch set in the same op reveals them) are retried after the others.
//   {op: "add_event_handler", emitter}           the emitter "+ Stage" menu's "Event Handler" (add utilities)
//   {op: "delete_event_handler", emitter, event} UNiagaraStackEventScriptItemGroup::Delete
//   {op: "event_handler", emitter, event, properties?, reset?}
//        the handler's Details (FNiagaraEventScriptProperties; SourceEmitter: the source emitter's name, "" = this
//        emitter): Modify, write, UNiagaraEmitter::PostEditChangeVersionedProperty.
//   {op: "add_stage", emitter, name, class, after?}  the "+ Stage" menu's simulation stage class (add utilities,
//        at its index), then the stage's SimulationStageName Details edit.
//   {op: "delete_stage", emitter, stage}         UNiagaraStackSimulationStageGroup::Delete
//   {op: "move_stage", emitter, stage, after?}   drag and drop of the stage group above the first / below "after"
//   {op: "stage", emitter, stage, enabled?, name?, properties?, reset?}
//        the stage's checkbox (SetIsEnabled) and Details (Modify, PreEditChange, write, PostEditChangeProperty).

#include "AssetDocumentNiagaraPrimitives.h"

#include "EdGraphSchema_Niagara.h"
#include "Framework/Application/SlateApplication.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEditorCommon.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraMessages.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraStackEditorData.h"
#include "NiagaraSystem.h"
#include "ScopedTransaction.h"
#include "UObject/StructOnScope.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraEmitterViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/INiagaraStackItemGroupAddUtilities.h"
#include "ViewModels/Stack/NiagaraStackEmitterPropertiesGroup.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackEventScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackObject.h"
#include "ViewModels/Stack/NiagaraStackScriptHierarchyRoot.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackSimulationStageGroup.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#define LOCTEXT_NAMESPACE "AssetDocumentNiagaraStackEdit"

namespace UE::AssetDocument::Niagara
{
namespace
{
using Graph::FReader;
using EValueMode = UNiagaraStackFunctionInput::EValueMode;

const TCHAR* const ScratchPadPrefix = TEXT("ScratchPad:");

struct FStackEdit
{
	FNiagaraSystemViewModel& ViewModel;
	bool& bChanged;
};

// The dynamic input menu's type rule (UNiagaraStackFunctionInput::GetAvailableDynamicInputs): the script's one output
// node has one input pin whose type is assignable to the input's type.
bool OutputMatches(UNiagaraScript& Script, const FNiagaraTypeDefinition& InputType)
{
	const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script.GetLatestSource());
	if (Source == nullptr || Source->NodeGraph == nullptr)
	{
		return false;
	}
	TArray<UNiagaraNodeOutput*> Outputs;
	Source->NodeGraph->GetNodesOfClass<UNiagaraNodeOutput>(Outputs);
	if (Outputs.Num() != 1)
	{
		return false;
	}
	FPinCollectorArray Pins;
	Outputs[0]->GetInputPins(Pins);
	return Pins.Num() == 1 && FNiagaraEditorUtilities::AreTypesAssignable(UEdGraphSchema_Niagara::PinToTypeDefinition(Pins[0]), InputType);
}

// ---- locations ----------------------------------------------------------------------------------------------

const TCHAR* GroupKey(ENiagaraScriptUsage Usage)
{
	switch (Usage)
	{
	case ENiagaraScriptUsage::SystemSpawnScript: return TEXT("SystemSpawn");
	case ENiagaraScriptUsage::SystemUpdateScript: return TEXT("SystemUpdate");
	case ENiagaraScriptUsage::EmitterSpawnScript: return TEXT("EmitterSpawn");
	case ENiagaraScriptUsage::EmitterUpdateScript: return TEXT("EmitterUpdate");
	case ENiagaraScriptUsage::ParticleSpawnScript:
	case ENiagaraScriptUsage::ParticleSpawnScriptInterpolated: return TEXT("ParticleSpawn");
	case ENiagaraScriptUsage::ParticleUpdateScript: return TEXT("ParticleUpdate");
	case ENiagaraScriptUsage::ParticleEventScript: return TEXT("EventHandler");
	case ENiagaraScriptUsage::ParticleSimulationStageScript: return TEXT("SimulationStage");
	default: return nullptr;
	}
}

TSharedPtr<FNiagaraEmitterHandleViewModel> FindHandle(FNiagaraSystemViewModel& ViewModel, const FString& Name)
{
	for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle : ViewModel.GetEmitterHandleViewModels())
	{
		if (Handle->GetEmitterHandle() != nullptr && Handle->GetName().ToString() == Name)
		{
			return Handle.ToSharedPtr();
		}
	}
	return nullptr;
}

bool RequireHandle(FReader& Op, FStackEdit& Edit, const TCHAR* Field, TSharedPtr<FNiagaraEmitterHandleViewModel>& OutHandle)
{
	FString Name;
	if (!Op.String(Field, Name))
	{
		return false;
	}
	OutHandle = FindHandle(Edit.ViewModel, Name);
	if (!OutHandle.IsValid())
	{
		return Op.Fail(TEXT("Niagara.EmitterNotFound"), FString::Printf(TEXT("The system has no emitter '%s'."), *Name), Field);
	}
	const FNiagaraEmitterHandle* EmitterHandle = OutHandle->GetEmitterHandle();
	if (EmitterHandle->GetEmitterMode() != ENiagaraEmitterMode::Standard || EmitterHandle->GetEmitterData() == nullptr)
	{
		return Op.Fail(TEXT("Niagara.StatelessEmitter"), FString::Printf(TEXT(
			"'%s' is a lightweight (stateless) emitter: it has no module stacks (author its Stateless value)."), *Name), Field);
	}
	return true;
}

// The stack root of the location's graph (the system stack, or the emitter's stack holding all of its groups),
// refreshed so that it lists what the graph holds now.
struct FLocation
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	UNiagaraStackEntry* Root = nullptr;
	FString Group;
	FGuid UsageId;
	bool bHasUsageId = false;
	FString Where;
};

bool ResolveLocation(FReader& Op, FStackEdit& Edit, FLocation& Out)
{
	if (!Op.String(TEXT("group"), Out.Group))
	{
		return false;
	}
	UNiagaraStackViewModel* Stack = nullptr;
	if (Op.Has(TEXT("emitter")))
	{
		if (!RequireHandle(Op, Edit, TEXT("emitter"), Out.Handle))
		{
			return false;
		}
		Stack = Out.Handle->GetEmitterStackViewModel();
		Out.Where = Out.Handle->GetName().ToString();
	}
	else
	{
		Stack = Edit.ViewModel.GetSystemStackViewModel();
		Out.Where = TEXT("the system");
	}
	Out.Root = Stack != nullptr ? Stack->GetRootEntry() : nullptr;
	if (Out.Root == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ViewModelUnavailable"), FString::Printf(TEXT("The stack of %s could not be built."), *Out.Where));
	}
	Out.Root->RefreshChildren();
	const bool bSystem = !Out.Handle.IsValid();
	const bool bSystemGroup = Out.Group == TEXT("SystemSpawn") || Out.Group == TEXT("SystemUpdate");
	const bool bEmitterGroup = Out.Group == TEXT("EmitterSpawn") || Out.Group == TEXT("EmitterUpdate") || Out.Group == TEXT("ParticleSpawn")
		|| Out.Group == TEXT("ParticleUpdate") || Out.Group == TEXT("EventHandler") || Out.Group == TEXT("SimulationStage");
	if ((bSystem && !bSystemGroup) || (!bSystem && !bEmitterGroup))
	{
		return Op.Fail(TEXT("Niagara.UnknownStackGroup"), FString::Printf(TEXT(
			"'%s' is not a module stack of %s (system: SystemSpawn, SystemUpdate; emitter: EmitterSpawn, EmitterUpdate, ParticleSpawn, "
			"ParticleUpdate, EventHandler, SimulationStage)."), *Out.Group, *Out.Where), TEXT("group"));
	}
	FVersionedNiagaraEmitterData* Data = Out.Handle.IsValid() ? Out.Handle->GetEmitterViewModel()->GetEmitter().GetEmitterData() : nullptr;
	if (Out.Group == TEXT("EventHandler"))
	{
		int64 Index = 0;
		if (!Op.Int(TEXT("event"), Index))
		{
			return false;
		}
		const TArray<FNiagaraEventScriptProperties>& Handlers = Data->GetEventHandlers();
		if (Index < 0 || Index >= Handlers.Num() || Handlers[static_cast<int32>(Index)].Script == nullptr)
		{
			return Op.Fail(TEXT("Niagara.EventHandlerNotFound"), FString::Printf(TEXT("%s has no event handler %lld (it has %d)."), *Out.Where,
				Index, Handlers.Num()), TEXT("event"));
		}
		Out.UsageId = Handlers[static_cast<int32>(Index)].Script->GetUsageId();
		Out.bHasUsageId = true;
	}
	else if (Out.Group == TEXT("SimulationStage"))
	{
		FString StageName;
		if (!Op.String(TEXT("stage"), StageName))
		{
			return false;
		}
		for (const UNiagaraSimulationStageBase* Stage : Data->GetSimulationStages())
		{
			if (Stage != nullptr && Stage->Script != nullptr && Stage->SimulationStageName.ToString() == StageName)
			{
				Out.UsageId = Stage->Script->GetUsageId();
				Out.bHasUsageId = true;
			}
		}
		if (!Out.bHasUsageId)
		{
			return Op.Fail(TEXT("Niagara.StageNotFound"), FString::Printf(TEXT("%s has no simulation stage '%s'."), *Out.Where, *StageName), TEXT("stage"));
		}
	}
	return true;
}

TArray<UNiagaraStackScriptItemGroup*> ScriptGroups(UNiagaraStackEntry& Root)
{
	TArray<UNiagaraStackEntry*> Children;
	Root.GetUnfilteredChildren(Children);
	TArray<UNiagaraStackScriptItemGroup*> Groups;
	for (UNiagaraStackEntry* Child : Children)
	{
		UNiagaraStackScriptItemGroup* Group = Cast<UNiagaraStackScriptItemGroup>(Child);
		if (Group != nullptr && Group->GetScriptOutputNode() != nullptr)
		{
			Groups.Add(Group);
		}
	}
	return Groups;
}

UNiagaraStackScriptItemGroup* FindGroup(const FLocation& Location)
{
	for (UNiagaraStackScriptItemGroup* Group : ScriptGroups(*Location.Root))
	{
		const UNiagaraNodeOutput* Output = Group->GetScriptOutputNode();
		const TCHAR* Key = GroupKey(Output->GetUsage());
		if (Key != nullptr && Location.Group == Key && (!Location.bHasUsageId || Output->GetUsageId() == Location.UsageId))
		{
			return Group;
		}
	}
	return nullptr;
}

bool RequireGroup(FReader& Op, const FLocation& Location, UNiagaraStackScriptItemGroup*& OutGroup)
{
	OutGroup = FindGroup(Location);
	if (OutGroup == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ViewModelUnavailable"), FString::Printf(TEXT("%s has no %s stack group."), *Location.Where, *Location.Group), TEXT("group"));
	}
	return true;
}

TArray<UNiagaraStackModuleItem*> Modules(UNiagaraStackEntry& Group)
{
	TArray<UNiagaraStackEntry*> Children;
	Group.GetUnfilteredChildren(Children);
	TArray<UNiagaraStackModuleItem*> Out;
	for (UNiagaraStackEntry* Child : Children)
	{
		if (UNiagaraStackModuleItem* Module = Cast<UNiagaraStackModuleItem>(Child))
		{
			Out.Add(Module);
		}
	}
	return Out;
}

UNiagaraStackModuleItem* FindModule(UNiagaraStackEntry& Group, const FString& Name)
{
	for (UNiagaraStackModuleItem* Module : Modules(Group))
	{
		if (Module->GetModuleNode().GetFunctionName() == Name)
		{
			return Module;
		}
	}
	return nullptr;
}

// A module anywhere in the location's graph (every group of its stack).
UNiagaraStackModuleItem* FindModuleInGraph(const FLocation& Location, const FString& Name)
{
	for (UNiagaraStackScriptItemGroup* Group : ScriptGroups(*Location.Root))
	{
		if (UNiagaraStackModuleItem* Module = FindModule(*Group, Name))
		{
			return Module;
		}
	}
	return nullptr;
}

bool RequireModule(FReader& Op, const FLocation& Location, UNiagaraStackScriptItemGroup& Group, const TCHAR* Field, UNiagaraStackModuleItem*& OutModule)
{
	FString Name;
	if (!Op.String(Field, Name))
	{
		return false;
	}
	OutModule = FindModule(Group, Name);
	if (OutModule == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ModuleNotFound"), FString::Printf(TEXT("The %s stack of %s has no module '%s'."), *Location.Group,
			*Location.Where, *Name), Field);
	}
	return true;
}

bool IsScratchPadModule(const UNiagaraStackModuleItem& Module)
{
	const UNiagaraScript* Script = Module.GetModuleNode().FunctionScript;
	return Module.IsScratchModule() || (Script != nullptr && !Script->IsAsset() && !Cast<UNiagaraNodeAssignment>(&Module.GetModuleNode()));
}

// Scratch pad scripts are not versioned: the version menu does not exist for their modules.
bool RefuseScratchPadVersion(FReader& Op, const UNiagaraStackModuleItem& Module, const TCHAR* Field)
{
	if (!IsScratchPadModule(Module))
	{
		return true;
	}
	return Op.Fail(TEXT("Niagara.ScriptVersionNotFound"), FString::Printf(TEXT(
		"'%s' runs a scratch pad script, which has no versions."), *Module.GetModuleNode().GetFunctionName()), Field);
}

// The insertion index after the module named by "after" (omitted or null: first).
bool InsertIndex(FReader& Op, const FLocation& Location, UNiagaraStackScriptItemGroup& Group, int32& OutIndex, UNiagaraStackModuleItem** OutAnchor = nullptr)
{
	OutIndex = 0;
	const FValue* After = Op.Find(TEXT("after"));
	if (After == nullptr || After->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		return true;
	}
	if (After->Kind != EAssetDocumentRuntimeValueKind::String)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("after is a module name or null."), TEXT("after"));
	}
	UNiagaraStackModuleItem* Anchor = FindModule(Group, After->StringValue);
	if (Anchor == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ModuleNotFound"), FString::Printf(TEXT("The %s stack of %s has no module '%s'."), *Location.Group,
			*Location.Where, *After->StringValue), TEXT("after"));
	}
	OutIndex = Anchor->GetModuleIndex() + 1;
	if (OutAnchor != nullptr)
	{
		*OutAnchor = Anchor;
	}
	return true;
}

// The add action's initialization of a new module (FNiagaraStackGraphUtilities::InitializeStackFunctionInputs:
// every resettable direct input of the module's input hierarchy is reset, which creates its data interface and
// default dynamic input; then the data object change notification).
void InitializeNewModule(UNiagaraStackModuleItem& Module)
{
	Module.RefreshChildren();
	TArray<UNiagaraStackEntry*> Children;
	Module.GetUnfilteredChildren(Children);
	for (UNiagaraStackEntry* Child : Children)
	{
		if (UNiagaraStackScriptHierarchyRoot* Hierarchy = Cast<UNiagaraStackScriptHierarchyRoot>(Child))
		{
			TArray<UNiagaraStackFunctionInput*> Inputs;
			Hierarchy->GetUnfilteredChildrenOfType(Inputs);
			for (UNiagaraStackFunctionInput* Input : Inputs)
			{
				if (Input != nullptr && Input->CanReset())
				{
					Input->Reset();
				}
			}
		}
	}
	Module.OnDataObjectModified().Broadcast(TArray<UObject*>(), ENiagaraDataObjectChange::Unknown);
}

UNiagaraScript* LoadScript(const FString& Path)
{
	return LoadObject<UNiagaraScript>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

// A module or dynamic input script by its document path: an asset path, or "ScratchPad:<name>": a scratch pad of the
// system (the system editor's Scratch Pad panel), else one of the emitter whose stack uses it.
UNiagaraScript* ResolveScript(FNiagaraSystemViewModel& ViewModel, const FVersionedNiagaraEmitterData* EmitterData, const FString& Path)
{
	FString Name = Path;
	if (!Name.RemoveFromStart(ScratchPadPrefix))
	{
		return LoadScript(Path);
	}
	for (UNiagaraScript* Script : ViewModel.GetSystem().ScratchPadScripts)
	{
		if (Script != nullptr && Script->GetName() == Name)
		{
			return Script;
		}
	}
	if (EmitterData != nullptr && EmitterData->ScratchPads != nullptr)
	{
		for (UNiagaraScript* Script : EmitterData->ScratchPads->Scripts)
		{
			if (Script != nullptr && Script->GetName() == Name)
			{
				return Script;
			}
		}
	}
	return nullptr;
}

bool ParseVersion(FReader& Op, const TCHAR* Field, const UNiagaraScript& Script, FGuid& OutVersion)
{
	OutVersion = FGuid();
	if (!Op.Has(Field))
	{
		return true;
	}
	FString Text;
	if (!Op.String(Field, Text))
	{
		return false;
	}
	if (Text.IsEmpty())
	{
		return true;
	}
	if (!FGuid::Parse(Text, OutVersion) || !Script.IsVersioningEnabled() || Script.GetScriptData(OutVersion) == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ScriptVersionNotFound"), FString::Printf(TEXT("%s has no version '%s'."), *Script.GetPathName(), *Text), Field);
	}
	return true;
}

bool AddModule(FReader& Op, FStackEdit& Edit)
{
	FLocation Location;
	UNiagaraStackScriptItemGroup* Group = nullptr;
	FString Name;
	FString ScriptPath;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("group"), TEXT("event"), TEXT("stage"), TEXT("name"), TEXT("script"), TEXT("version"), TEXT("after")})
		|| !ResolveLocation(Op, Edit, Location) || !RequireGroup(Op, Location, Group) || !Op.String(TEXT("name"), Name)
		|| !Op.String(TEXT("script"), ScriptPath))
	{
		return false;
	}
	const FNiagaraEmitterHandle* EmitterHandle = Location.Handle.IsValid() ? Location.Handle->GetEmitterHandle() : nullptr;
	UNiagaraScript* Script = ResolveScript(Edit.ViewModel, EmitterHandle != nullptr ? EmitterHandle->GetEmitterData() : nullptr, ScriptPath);
	if (Script == nullptr || Script->GetUsage() != ENiagaraScriptUsage::Module)
	{
		return Op.Fail(TEXT("Niagara.ModuleScriptNotFound"), FString::Printf(TEXT("'%s' is not a Niagara module script asset or scratch pad."), *ScriptPath),
			TEXT("script"));
	}
	FGuid Version;
	if (!ParseVersion(Op, TEXT("version"), *Script, Version))
	{
		return false;
	}
	const FVersionedNiagaraScriptData* ScriptData = Script->GetScriptData(Version.IsValid() ? Version : Script->GetExposedVersion().VersionGuid);
	const ENiagaraScriptUsage Usage = Group->GetScriptOutputNode()->GetUsage();
	if (ScriptData == nullptr || !ScriptData->GetSupportedUsageContexts().ContainsByPredicate([Usage](ENiagaraScriptUsage Supported)
		{
			return UNiagaraScript::IsEquivalentUsage(Usage, Supported);
		}))
	{
		return Op.Fail(TEXT("Niagara.ModuleUsage"), FString::Printf(TEXT(
			"%s cannot run in the %s stack (the stack's \"+\" menu does not offer it there)."), *Script->GetName(), *Location.Group), TEXT("script"));
	}
	if (FindModuleInGraph(Location, Name) != nullptr)
	{
		return Op.Fail(TEXT("Niagara.ModuleExists"), FString::Printf(TEXT("%s already has a module '%s' (module names are unique in a graph)."),
			*Location.Where, *Name), TEXT("name"));
	}
	const FString ScriptName = Script->GetName();
	if (Name != ScriptName && !(Name.StartsWith(ScriptName, ESearchCase::CaseSensitive) && Name.RightChop(ScriptName.Len()).IsNumeric()))
	{
		return Op.Fail(TEXT("Niagara.InvalidModuleName"), FString::Printf(TEXT(
			"A new module is named after its script: '%s' or '%s' followed by digits (e.g. %s001); got '%s'."), *ScriptName, *ScriptName,
			*ScriptName, *Name), TEXT("name"));
	}
	int32 Index = 0;
	if (!InsertIndex(Op, Location, *Group, Index))
	{
		return false;
	}
	UNiagaraNodeFunctionCall* Node = nullptr;
	{
		const FScopedTransaction Transaction(LOCTEXT("InsertNewModule", "Insert new module"));
		Node = FNiagaraStackGraphUtilities::AddScriptModuleToStack(Script, *Group->GetScriptOutputNode(), Index, Name, Version);
		Edit.bChanged = true;
		if (Node == nullptr)
		{
			return Op.Fail(TEXT("Niagara.AddModuleFailed"), FString::Printf(TEXT("%s could not be added."), *ScriptName), TEXT("script"));
		}
		Group->RefreshChildren();
		UNiagaraStackModuleItem* Module = FindModule(*Group, Node->GetFunctionName());
		if (Module == nullptr)
		{
			return Op.Fail(TEXT("Niagara.AddModuleFailed"), FString::Printf(TEXT("%s was added but the stack does not list it."), *ScriptName), TEXT("script"));
		}
		InitializeNewModule(*Module);
	}
	if (Node->GetFunctionName() != Name)
	{
		return Op.Fail(TEXT("Niagara.InvalidModuleName"), FString::Printf(TEXT(
			"The editor names the new module '%s', not '%s' (the name is taken by another function call of the graph)."),
			*Node->GetFunctionName(), *Name), TEXT("name"));
	}
	return true;
}

bool ParseTargets(FReader& Op, const TCHAR* Field, TArray<FNiagaraVariable>& OutTargets)
{
	const FValue* List = nullptr;
	if (!Op.Has(Field))
	{
		return true;
	}
	if (!Op.Array(Field, List))
	{
		return false;
	}
	for (int32 Index = 0; Index < List->Elements.Num(); ++Index)
	{
		const FString Where = FString::Printf(TEXT("%s/%d"), Field, Index);
		FReader Target = Op.Child(Where, List->Elements[Index]);
		FString Name;
		FString TypeName;
		if (!Target.IsObject() || !Target.OnlyFields({TEXT("name"), TEXT("type")}) || !Target.String(TEXT("name"), Name) || !Target.String(TEXT("type"), TypeName))
		{
			return Target.IsObject() ? false : Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A target is {name, type}."), *Where);
		}
		FNiagaraTypeDefinition Type;
		if (!TypeFromToken(TypeName, Type))
		{
			return Target.Fail(TEXT("Niagara.UnknownParameterType"), FString::Printf(TEXT("'%s' is not a Niagara type."), *TypeName), TEXT("type"));
		}
		OutTargets.Emplace(Type, FName(*Name));
	}
	return true;
}

bool AddTargets(FReader& Op, UNiagaraStackModuleItem& Module, TConstArrayView<FNiagaraVariable> Targets, const TCHAR* Field)
{
	for (const FNiagaraVariable& Target : Targets)
	{
		if (!Module.CanAddInput(Target))
		{
			return Op.Fail(TEXT("Niagara.InvalidAssignmentTarget"), FString::Printf(TEXT(
				"'%s' cannot be set by this Set Parameters module (already set by it, or not writable from its stack)."),
				*Target.GetName().ToString()), Field);
		}
		Module.AddInput(Target);
		Module.RefreshChildren();
	}
	return true;
}

bool AddAssignment(FReader& Op, FStackEdit& Edit)
{
	FLocation Location;
	UNiagaraStackScriptItemGroup* Group = nullptr;
	TArray<FNiagaraVariable> Targets;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("group"), TEXT("event"), TEXT("stage"), TEXT("after"), TEXT("targets")})
		|| !ResolveLocation(Op, Edit, Location) || !RequireGroup(Op, Location, Group) || !ParseTargets(Op, TEXT("targets"), Targets))
	{
		return false;
	}
	int32 Index = 0;
	if (!InsertIndex(Op, Location, *Group, Index))
	{
		return false;
	}
	const FScopedTransaction Transaction(LOCTEXT("InsertNewAssignment", "Insert new module"));
	UNiagaraNodeAssignment* Node = FNiagaraStackGraphUtilities::AddParameterModuleToStack(TArray<FNiagaraVariable>(), *Group->GetScriptOutputNode(), Index,
		TArray<FString>());
	Edit.bChanged = true;
	if (Node == nullptr)
	{
		return Op.Fail(TEXT("Niagara.AddModuleFailed"), TEXT("The Set Parameters module could not be added."), TEXT("targets"));
	}
	Group->RefreshChildren();
	UNiagaraStackModuleItem* Module = FindModule(*Group, Node->GetFunctionName());
	if (Module == nullptr)
	{
		return Op.Fail(TEXT("Niagara.AddModuleFailed"), TEXT("The Set Parameters module was added but the stack does not list it."), TEXT("targets"));
	}
	InitializeNewModule(*Module);
	return AddTargets(Op, *Module, Targets, TEXT("targets"));
}

bool DeleteModule(FReader& Op, FStackEdit& Edit)
{
	FLocation Location;
	UNiagaraStackScriptItemGroup* Group = nullptr;
	UNiagaraStackModuleItem* Module = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("group"), TEXT("event"), TEXT("stage"), TEXT("name")}) || !ResolveLocation(Op, Edit, Location)
		|| !RequireGroup(Op, Location, Group) || !RequireModule(Op, Location, *Group, TEXT("name"), Module))
	{
		return false;
	}
	FText Message;
	if (!Module->TestCanDeleteWithMessage(Message))
	{
		return Op.Fail(Module->GetIsInherited() ? TEXT("Niagara.InheritedModule") : TEXT("Niagara.ModuleNotDeletable"), FString::Printf(
			TEXT("The module '%s' cannot be deleted: %s"), *Module->GetModuleNode().GetFunctionName(), *Message.ToString()), TEXT("name"));
	}
	const FScopedTransaction Transaction(Module->GetDeleteTransactionText());
	Module->Delete();
	Edit.bChanged = true;
	return true;
}

bool MoveModule(FReader& Op, FStackEdit& Edit)
{
	FLocation Location;
	UNiagaraStackScriptItemGroup* Group = nullptr;
	FString Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("group"), TEXT("event"), TEXT("stage"), TEXT("name"), TEXT("after")}) || !ResolveLocation(Op, Edit, Location)
		|| !RequireGroup(Op, Location, Group) || !Op.String(TEXT("name"), Name))
	{
		return false;
	}
	UNiagaraStackModuleItem* Module = FindModuleInGraph(Location, Name);
	if (Module == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ModuleNotFound"), FString::Printf(TEXT("%s has no module '%s'."), *Location.Where, *Name), TEXT("name"));
	}
	int32 Index = 0;
	UNiagaraStackModuleItem* Anchor = nullptr;
	if (!InsertIndex(Op, Location, *Group, Index, &Anchor))
	{
		return false;
	}
	if (Anchor == Module)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A module cannot be moved after itself."), TEXT("after"));
	}
	// Already in place: nothing to drop.
	const TArray<UNiagaraStackModuleItem*> Current = Modules(*Group);
	const int32 At = Current.IndexOfByKey(Module);
	const int32 AnchorAt = Anchor != nullptr ? Current.IndexOfByKey(Anchor) : INDEX_NONE;
	if (At != INDEX_NONE && At == AnchorAt + 1)
	{
		return true;
	}
	TArray<UNiagaraStackEntry*> Dragged;
	Dragged.Add(Module);
	const TSharedRef<FNiagaraStackEntryDragDropOp> DragDrop = MakeShared<FNiagaraStackEntryDragDropOp>(Dragged);
	UNiagaraStackEntry* Target = Anchor != nullptr ? static_cast<UNiagaraStackEntry*>(Anchor) : static_cast<UNiagaraStackEntry*>(Group);
	const UNiagaraStackEntry::FDropRequest Request(DragDrop, Anchor != nullptr ? EItemDropZone::BelowItem : EItemDropZone::OntoItem,
		UNiagaraStackEntry::EDragOptions::None, UNiagaraStackEntry::EDropOptions::Overview);
	const TOptional<UNiagaraStackEntry::FDropRequestResponse> Can = Target->CanDrop(Request);
	if (!Can.IsSet() || !Can->DropZone.IsSet())
	{
		const FString Why = Can.IsSet() && !Can->DropMessage.IsEmpty() ? Can->DropMessage.ToString() : FString(TEXT("the stack does not accept the drop"));
		return Op.Fail(Module->GetIsInherited() ? TEXT("Niagara.InheritedModule") : TEXT("Niagara.ModuleNotMovable"), FString::Printf(
			TEXT("The module '%s' cannot move to the %s stack of %s: %s."), *Name, *Location.Group, *Location.Where, *Why.Replace(TEXT("\n"), TEXT(" "))), TEXT("name"));
	}
	Target->Drop(Request);
	Edit.bChanged = true;
	Location.Root->RefreshChildren();
	UNiagaraStackScriptItemGroup* After = FindGroup(Location);
	const TArray<UNiagaraStackModuleItem*> Result = After != nullptr ? Modules(*After) : TArray<UNiagaraStackModuleItem*>();
	const int32 Placed = Result.IndexOfByPredicate([&Name](const UNiagaraStackModuleItem* Item) { return Item->GetModuleNode().GetFunctionName() == Name; });
	const FValue* AfterName = Op.Find(TEXT("after"));
	const bool bFirst = AfterName == nullptr || AfterName->Kind != EAssetDocumentRuntimeValueKind::String;
	const bool bPlaced = Placed != INDEX_NONE && (bFirst ? Placed == 0
		: Placed > 0 && Result[Placed - 1]->GetModuleNode().GetFunctionName() == AfterName->StringValue);
	if (!bPlaced)
	{
		return Op.Fail(TEXT("Niagara.MoveModuleFailed"), FString::Printf(TEXT("The drop did not place '%s' where the document wants it."), *Name), TEXT("name"));
	}
	return true;
}

bool EditModule(FReader& Op, FStackEdit& Edit)
{
	FLocation Location;
	UNiagaraStackScriptItemGroup* Group = nullptr;
	UNiagaraStackModuleItem* Module = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("group"), TEXT("event"), TEXT("stage"), TEXT("name"), TEXT("enabled"), TEXT("version"),
		TEXT("display_name"), TEXT("note")}) || !ResolveLocation(Op, Edit, Location) || !RequireGroup(Op, Location, Group)
		|| !RequireModule(Op, Location, *Group, TEXT("name"), Module))
	{
		return false;
	}
	if (Op.Has(TEXT("version")))
	{
		if (!RefuseScratchPadVersion(Op, *Module, TEXT("version")))
		{
			return false;
		}
		UNiagaraScript* Script = Module->GetModuleNode().FunctionScript;
		FGuid Version;
		if (Script == nullptr || Cast<UNiagaraNodeAssignment>(&Module->GetModuleNode()) != nullptr || !ParseVersion(Op, TEXT("version"), *Script, Version))
		{
			return Script == nullptr || Cast<UNiagaraNodeAssignment>(&Module->GetModuleNode()) != nullptr
				? Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("This module has no script versions."), TEXT("version")) : false;
		}
		if (!Version.IsValid() && Script->IsVersioningEnabled())
		{
			Version = Script->GetExposedVersion().VersionGuid;
		}
		if (Script->IsVersioningEnabled() && Module->GetModuleNode().SelectedScriptVersion != Version)
		{
			Module->ChangeScriptVersion(Version);
			Edit.bChanged = true;
			Group->RefreshChildren();
			Module = FindModule(*Group, Module->GetModuleNode().GetFunctionName());
			if (Module == nullptr)
			{
				return Op.Fail(TEXT("Niagara.ModuleNotFound"), TEXT("The module is gone after its version change."), TEXT("version"));
			}
		}
	}
	if (Op.Has(TEXT("enabled")))
	{
		bool bEnabled = true;
		if (!Op.Bool(TEXT("enabled"), bEnabled, true))
		{
			return false;
		}
		if (Module->GetIsEnabled() != bEnabled)
		{
			Module->SetIsEnabled(bEnabled);
			Edit.bChanged = true;
		}
	}
	if (Op.Has(TEXT("display_name")))
	{
		FString DisplayName;
		if (!Op.String(TEXT("display_name"), DisplayName, true))
		{
			return false;
		}
		const TOptional<FText> Current = Module->GetAlternateDisplayName();
		const FString Have = Current.IsSet() ? Current->ToString() : FString();
		if (Have != DisplayName)
		{
			Module->OnRenamed(FText::FromString(DisplayName));
			Edit.bChanged = true;
			const TOptional<FText> After = Module->GetAlternateDisplayName();
			if ((After.IsSet() ? After->ToString() : FString()) != DisplayName)
			{
				return Op.Fail(TEXT("Niagara.InvalidDisplayName"), FString::Printf(TEXT(
					"The stack does not keep '%s' as a display name (a name equal to the module's own display name is no rename)."), *DisplayName),
					TEXT("display_name"));
			}
		}
	}
	if (const FValue* Note = Op.Find(TEXT("note")))
	{
		const FNiagaraStackNoteData Have = Module->GetStackNoteData();
		if (Note->Kind == EAssetDocumentRuntimeValueKind::Null)
		{
			if (Have.IsValid())
			{
				Module->DeleteStackNoteData();
				Edit.bChanged = true;
			}
		}
		else
		{
			FReader NoteReader = Op.Child(TEXT("note"), *Note);
			FString Header;
			FString Body;
			if (!NoteReader.IsObject() || !NoteReader.OnlyFields({TEXT("Header"), TEXT("Body")}) || !NoteReader.String(TEXT("Header"), Header, true)
				|| !NoteReader.String(TEXT("Body"), Body, true))
			{
				return NoteReader.IsObject() ? false : Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("note is {Header, Body} or null."), TEXT("note"));
			}
			if (Have.MessageHeader.ToString() != Header || Have.Message.ToString() != Body)
			{
				FNiagaraStackNoteData Want = Have;
				Want.MessageHeader = FText::FromString(Header);
				Want.Message = FText::FromString(Body);
				const FScopedTransaction Transaction(LOCTEXT("EditStackNote", "Edit Stack Note"));
				Module->GetStackEditorData().Modify();
				Module->SetStackNoteData(Want);
				Edit.bChanged = true;
			}
		}
	}
	return true;
}

bool EditAssignment(FReader& Op, FStackEdit& Edit)
{
	FLocation Location;
	UNiagaraStackScriptItemGroup* Group = nullptr;
	UNiagaraStackModuleItem* Module = nullptr;
	TArray<FNiagaraVariable> Add;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("group"), TEXT("event"), TEXT("stage"), TEXT("name"), TEXT("remove"), TEXT("add")})
		|| !ResolveLocation(Op, Edit, Location) || !RequireGroup(Op, Location, Group) || !RequireModule(Op, Location, *Group, TEXT("name"), Module)
		|| !ParseTargets(Op, TEXT("add"), Add))
	{
		return false;
	}
	if (Cast<UNiagaraNodeAssignment>(&Module->GetModuleNode()) == nullptr)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("'%s' is not a Set Parameters module."),
			*Module->GetModuleNode().GetFunctionName()), TEXT("name"));
	}
	const FValue* Remove = nullptr;
	if (Op.Has(TEXT("remove")) && !Op.Array(TEXT("remove"), Remove))
	{
		return false;
	}
	for (int32 Index = 0; Remove != nullptr && Index < Remove->Elements.Num(); ++Index)
	{
		const FString Where = FString::Printf(TEXT("remove/%d"), Index);
		const FValue& Target = Remove->Elements[Index];
		if (Target.Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("remove lists target names."), *Where);
		}
		TArray<UNiagaraStackFunctionInput*> Inputs;
		Module->GetParameterInputs(Inputs);
		UNiagaraStackFunctionInput** Found = Inputs.FindByPredicate([&Target](const UNiagaraStackFunctionInput* Input)
		{
			return Input->GetInputParameterHandle().GetName().ToString() == Target.StringValue;
		});
		if (Found == nullptr || !(*Found)->CanDeleteInput())
		{
			return Op.Fail(TEXT("Niagara.InputNotFound"), FString::Printf(TEXT("'%s' sets no '%s'."), *Module->GetModuleNode().GetFunctionName(),
				*Target.StringValue), *Where);
		}
		(*Found)->DeleteInput();
		Edit.bChanged = true;
		Module->RefreshChildren();
	}
	if (!Add.IsEmpty())
	{
		const FScopedTransaction Transaction(LOCTEXT("AddAssignmentTargets", "Add parameters to set"));
		if (!AddTargets(Op, *Module, Add, TEXT("add")))
		{
			return false;
		}
		Edit.bChanged = true;
	}
	return true;
}

// ---- inputs ---------------------------------------------------------------------------------------------------

FString InputName(const UNiagaraStackFunctionInput& Input)
{
	return Input.GetInputParameterHandle().GetName().ToString();
}

UNiagaraStackFunctionInput* FindInput(TConstArrayView<UNiagaraStackFunctionInput*> Inputs, const FString& Name)
{
	for (UNiagaraStackFunctionInput* Input : Inputs)
	{
		if (Input != nullptr && InputName(*Input) == Name)
		{
			return Input;
		}
	}
	return nullptr;
}

// An input addressed by its path of names from the module (through dynamic inputs), resolved fresh.
UNiagaraStackFunctionInput* ResolveInput(UNiagaraStackModuleItem& Module, TConstArrayView<FString> Path)
{
	Module.RefreshChildren();
	TArray<UNiagaraStackFunctionInput*> Inputs = ModuleInputs(Module);
	UNiagaraStackFunctionInput* Input = nullptr;
	for (int32 Index = 0; Index < Path.Num(); ++Index)
	{
		Input = FindInput(Inputs, Path[Index]);
		if (Input == nullptr)
		{
			return nullptr;
		}
		if (Index + 1 < Path.Num())
		{
			Inputs = DynamicInputInputs(*Input);
		}
	}
	return Input;
}

FString JoinPath(TConstArrayView<FString> Path)
{
	return FString::Join(Path, TEXT("/"));
}

FString AvailableInputs(UNiagaraStackModuleItem& Module, TConstArrayView<FString> Parent)
{
	TArray<UNiagaraStackFunctionInput*> Inputs;
	if (Parent.IsEmpty())
	{
		Inputs = ModuleInputs(Module);
	}
	else if (UNiagaraStackFunctionInput* Owner = ResolveInput(Module, Parent))
	{
		Inputs = DynamicInputInputs(*Owner);
	}
	TArray<FString> Names;
	for (const UNiagaraStackFunctionInput* Input : Inputs)
	{
		if (Input != nullptr)
		{
			Names.Add(InputName(*Input));
		}
	}
	return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT(", "));
}

struct FInputSetter
{
	FReader& Op;
	FStackEdit& Edit;
	UNiagaraStackModuleItem& Module;

	bool Fail(const TCHAR* Code, const FString& Message, const FString& Where)
	{
		return Op.Fail(Code, Message, *Where);
	}

	bool SetLocal(UNiagaraStackFunctionInput& Input, const FValue& Value, const FString& Where)
	{
		const FNiagaraTypeDefinition Type = Input.GetInputType();
		const UScriptStruct* Struct = Type.GetScriptStruct();
		if (Struct == nullptr || Type.IsDataInterface() || Type.IsUObject())
		{
			return Fail(TEXT("Niagara.InvalidInputValue"), FString::Printf(TEXT("'%s' (%s) takes no literal value."), *InputName(Input),
				*TypeToken(Type)), Where);
		}
		const TSharedRef<FStructOnScope> Local = MakeShared<FStructOnScope>(Struct);
		const TSharedPtr<const FStructOnScope> Current = Input.GetLocalValueStruct();
		if (Input.GetValueMode() == EValueMode::Local && Current.IsValid() && Current->GetStruct() == Struct)
		{
			Struct->CopyScriptStruct(Local->GetStructMemory(), Current->GetStructMemory());
		}
		FString Message;
		if (!DecodeValue(Type, Value, Local->GetStructMemory(), Message))
		{
			return Fail(TEXT("Niagara.InvalidInputValue"), FString::Printf(TEXT("The value does not fit '%s' (%s): %s"), *InputName(Input),
				*TypeToken(Type), *Message), Where);
		}
		if (Input.GetValueMode() == EValueMode::Local && Current.IsValid() && Current->GetStruct() == Struct
			&& Struct->CompareScriptStruct(Local->GetStructMemory(), Current->GetStructMemory(), PPF_None))
		{
			return true;
		}
		Input.SetLocalValue(Local);
		Edit.bChanged = true;
		return true;
	}

	bool SetLink(UNiagaraStackFunctionInput& Input, FReader& Form, const FString& Where)
	{
		FString Name;
		if (!Form.OnlyFields({TEXT("Link"), TEXT("Type")}) || !Form.String(TEXT("Link"), Name))
		{
			return false;
		}
		FNiagaraTypeDefinition Type = Input.GetInputType();
		if (Form.Has(TEXT("Type")))
		{
			FString TypeName;
			if (!Form.String(TEXT("Type"), TypeName))
			{
				return false;
			}
			if (!TypeFromToken(TypeName, Type))
			{
				return Form.Fail(TEXT("Niagara.UnknownParameterType"), FString::Printf(TEXT("'%s' is not a Niagara type."), *TypeName), TEXT("Type"));
			}
		}
		if (Name.IsEmpty())
		{
			return Fail(TEXT("Niagara.InvalidInputValue"), TEXT("A link names a parameter."), Where);
		}
		const FNiagaraVariableBase Parameter(Type, FName(*Name));
		if (Input.GetValueMode() == EValueMode::Linked && Input.GetLinkedParameterValue() == Parameter)
		{
			return true;
		}
		// A link to a user parameter of another type would make the editor add a second user parameter of the
		// input's type under the same name.
		if (Name.StartsWith(TEXT("User.")))
		{
			for (const FNiagaraVariableWithOffset& Existing : Module.GetSystemViewModel()->GetSystem().GetExposedParameters().ReadParameterVariables())
			{
				if (Existing.GetName() == Parameter.GetName() && Existing.GetType() != Type)
				{
					return Fail(TEXT("Niagara.LinkTypeMismatch"), FString::Printf(TEXT(
						"'%s' is a %s user parameter; '%s' takes %s. Link a %s parameter, or give the link's Type to link through a conversion."),
						*Name, *TypeToken(Existing.GetType()), *InputName(Input), *TypeToken(Type), *TypeToken(Input.GetInputType())), Where);
				}
			}
		}
		Input.SetLinkedParameterValue(Parameter);
		Edit.bChanged = true;
		return true;
	}

	bool SetExpression(UNiagaraStackFunctionInput& Input, FReader& Form, const FString& Where)
	{
		FString Expression;
		if (!Form.OnlyFields({TEXT("Expression")}) || !Form.String(TEXT("Expression"), Expression, true))
		{
			return false;
		}
		if (Input.GetValueMode() == EValueMode::Expression && Input.GetCustomExpressionText().ToString() == Expression)
		{
			return true;
		}
		if (!Input.SupportsCustomExpressions())
		{
			return Fail(TEXT("Niagara.InvalidInputValue"), FString::Printf(TEXT("'%s' does not take an expression."), *InputName(Input)), Where);
		}
		Input.SetCustomExpression(Expression);
		Edit.bChanged = true;
		return true;
	}

	bool SetObject(UNiagaraStackFunctionInput& Input, FReader& Form, const FString& Where)
	{
		FString Path;
		if (!Form.OnlyFields({TEXT("Object")}) || !Form.String(TEXT("Object"), Path, true))
		{
			return false;
		}
		if (!Input.GetInputType().IsUObject())
		{
			return Fail(TEXT("Niagara.InvalidInputValue"), FString::Printf(TEXT("'%s' (%s) takes no object."), *InputName(Input),
				*TypeToken(Input.GetInputType())), Where);
		}
		UObject* Object = Path.IsEmpty() ? nullptr : LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Path.IsEmpty() && Object == nullptr)
		{
			return Fail(TEXT("Niagara.ObjectNotFound"), FString::Printf(TEXT("'%s' does not load."), *Path), Where);
		}
		const UClass* Class = Input.GetInputType().GetClass();
		if (Object != nullptr && Class != nullptr && !Object->IsA(Class))
		{
			return Fail(TEXT("Niagara.InvalidInputValue"), FString::Printf(TEXT("'%s' is not a %s."), *Path, *Class->GetName()), Where);
		}
		if (Input.GetValueMode() == EValueMode::ObjectAsset && Input.GetObjectAssetValue() == Object)
		{
			return true;
		}
		Input.SetObjectAssetValue(Object);
		Edit.bChanged = true;
		return true;
	}

	bool SetDataInterface(UNiagaraStackFunctionInput& Input, FReader& Form, const FString& Where)
	{
		const FValue* Spec = nullptr;
		if (!Form.OnlyFields({TEXT("DataInterface")}) || !Form.Object(TEXT("DataInterface"), Spec))
		{
			return false;
		}
		FReader Interface = Form.Child(TEXT("DataInterface"), *Spec);
		if (!Interface.OnlyFields({TEXT("Class"), TEXT("Properties")}))
		{
			return false;
		}
		const FNiagaraTypeDefinition Type = Input.GetInputType();
		if (!Type.IsDataInterface())
		{
			return Fail(TEXT("Niagara.InvalidInputValue"), FString::Printf(TEXT("'%s' (%s) takes no data interface."), *InputName(Input),
				*TypeToken(Type)), Where);
		}
		UClass* Class = Type.GetClass();
		if (Interface.Has(TEXT("Class")))
		{
			FString Label;
			if (!Interface.String(TEXT("Class"), Label))
			{
				return false;
			}
			const FString Path = Label.StartsWith(TEXT("/")) ? Label : FString::Printf(TEXT("/Script/Niagara.NiagaraDataInterface%s"), *Label);
			UClass* Wanted = FindObject<UClass>(nullptr, *Path);
			if (Wanted == nullptr || Class == nullptr || !Wanted->IsChildOf(Class) || Wanted->HasAnyClassFlags(CLASS_Abstract))
			{
				return Interface.Fail(TEXT("Niagara.UnknownDataInterfaceClass"), FString::Printf(TEXT("'%s' is not a data interface class for '%s' (%s)."),
					*Label, *InputName(Input), *TypeToken(Type)), TEXT("Class"));
			}
			Class = Wanted;
		}
		UNiagaraDataInterface* Current = Input.GetValueMode() == EValueMode::Data ? Input.GetDataValueObject() : nullptr;
		const UPackage* Package = Module.GetModuleNode().GetOutermost();
		if (Current == nullptr || Current->GetClass() != Class || Current->GetOutermost() != Package)
		{
			Input.SetDataInterfaceValue(Class);
			Edit.bChanged = true;
			Current = Input.GetDataValueObject();
		}
		if (Current == nullptr || Current->GetOutermost() != Package)
		{
			return Fail(TEXT("Niagara.InvalidInputValue"), FString::Printf(TEXT("'%s' holds no editable data interface."), *InputName(Input)), Where);
		}
		// Properties are complete: listed values are written, the others go back to the class defaults.
		const FValue* Properties = Interface.Find(TEXT("Properties"));
		const UObject* Defaults = Current->GetClass()->GetDefaultObject();
		UNiagaraStackObject* DataObject = Input.GetChildDataObject();
		for (TFieldIterator<FProperty> It(Current->GetClass()); It; ++It)
		{
			FProperty& Property = **It;
			if (!Property.HasAnyPropertyFlags(CPF_Edit) || Property.HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
			{
				continue;
			}
			const FString Key = Property.GetAuthoredName();
			const int32 Index = Properties != nullptr && Properties->Kind == EAssetDocumentRuntimeValueKind::Object ? Properties->ObjectKeys.IndexOfByKey(Key) : INDEX_NONE;
			void* ValuePtr = Property.ContainerPtrToValuePtr<void>(Current);
			const void* DefaultPtr = Property.ContainerPtrToValuePtr<void>(Defaults);
			auto Write = [&](auto&& Apply)
			{
				Current->Modify();
				Current->PreEditChange(&Property);
				Apply();
				FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
				Current->PostEditChangeProperty(Event);
				if (DataObject != nullptr)
				{
					DataObject->NotifyPostChange(Event, &Property);
				}
				Edit.bChanged = true;
			};
			if (Index == INDEX_NONE)
			{
				if (!PropertyMatchesDefault(Property, ValuePtr, DefaultPtr) && !Property.ContainsInstancedObjectProperty())
				{
					Write([&]() { Property.CopyCompleteValue(ValuePtr, DefaultPtr); });
				}
				continue;
			}
			bool bSame = false;
			FString Code;
			if (!DecodePropertyIsSame(Property, Properties->Elements[Index], ValuePtr, Current, bSame, Code, DefaultPtr))
			{
				return Interface.Fail(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT("'%s' does not fit the property (%s)."), *Key, *Code),
					*(FString(TEXT("Properties/")) + Key));
			}
			if (!bSame)
			{
				Write([&]() { DecodeProperty(Property, Properties->Elements[Index], ValuePtr, Current, Code, DefaultPtr); });
			}
		}
		if (Properties != nullptr && Properties->Kind == EAssetDocumentRuntimeValueKind::Object)
		{
			for (const FString& Key : Properties->ObjectKeys)
			{
				if (FindEditableProperty(*Current->GetClass(), Key) == nullptr)
				{
					return Interface.Fail(TEXT("Niagara.UnknownProperty"), FString::Printf(TEXT("%s has no editable property '%s'."),
						*Current->GetClass()->GetName(), *Key), *(FString(TEXT("Properties/")) + Key));
				}
			}
		}
		return true;
	}

	bool SetDynamic(TArray<FString> Path, FReader& Form, const FString& Where, int32 Depth)
	{
		const FValue* Spec = nullptr;
		if (!Form.OnlyFields({TEXT("Dynamic")}) || !Form.Object(TEXT("Dynamic"), Spec))
		{
			return false;
		}
		FReader Dynamic = Form.Child(TEXT("Dynamic"), *Spec);
		FString ScriptPath;
		if (!Dynamic.OnlyFields({TEXT("Script"), TEXT("Version"), TEXT("Inputs")}) || !Dynamic.String(TEXT("Script"), ScriptPath))
		{
			return false;
		}
		const TSharedPtr<FNiagaraEmitterViewModel> EmitterViewModel = Module.GetEmitterViewModel();
		UNiagaraScript* Script = ResolveScript(Edit.ViewModel, EmitterViewModel.IsValid() ? EmitterViewModel->GetEmitter().GetEmitterData() : nullptr,
			ScriptPath);
		if (Script == nullptr || Script->GetUsage() != ENiagaraScriptUsage::DynamicInput)
		{
			return Dynamic.Fail(TEXT("Niagara.DynamicInputNotFound"), FString::Printf(TEXT("'%s' is not a Niagara dynamic input script asset or scratch pad."),
				*ScriptPath), TEXT("Script"));
		}
		FGuid Version;
		if (!ParseVersion(Dynamic, TEXT("Version"), *Script, Version))
		{
			return false;
		}
		if (!Version.IsValid() && Script->IsVersioningEnabled())
		{
			Version = Script->GetExposedVersion().VersionGuid;
		}
		UNiagaraStackFunctionInput* Input = ResolveInput(Module, Path);
		if (Input == nullptr)
		{
			return Fail(TEXT("Niagara.InputNotFound"), FString::Printf(TEXT("'%s' has no input '%s'."), *Module.GetModuleNode().GetFunctionName(), *JoinPath(Path)), Where);
		}
		const UNiagaraNodeFunctionCall* Node = Input->GetValueMode() == EValueMode::Dynamic ? Input->GetDynamicInputNode() : nullptr;
		if (Node != nullptr && Node->FunctionScript == Script)
		{
			if (Script->IsVersioningEnabled() && Node->SelectedScriptVersion != Version)
			{
				Input->ChangeScriptVersion(Version);
				Edit.bChanged = true;
			}
		}
		else
		{
			TArray<UNiagaraScript*> Available;
			Input->GetAvailableDynamicInputs(Available, true);
			// The menu leaves deprecated scripts out, which existing content still uses: the menu's own type rule
			// (the script's single output assignable to the input's type) admits them.
			if (!Available.Contains(Script) && !OutputMatches(*Script, Input->GetInputType()))
			{
				return Dynamic.Fail(TEXT("Niagara.DynamicInputType"), FString::Printf(TEXT(
					"%s does not output a value '%s' (%s) takes (the input's dynamic input menu does not offer it)."), *Script->GetName(),
					*InputName(*Input), *TypeToken(Input->GetInputType())), TEXT("Script"));
			}
			Input->SetDynamicInput(Script, FString(), Version);
			Edit.bChanged = true;
		}
		// The nested inputs are complete: listed ones set, overridden unlisted ones reset.
		const FValue* Inputs = Dynamic.Find(TEXT("Inputs"));
		if (Inputs != nullptr && Inputs->Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			return Dynamic.Fail(TEXT("Graph.InvalidArgument"), TEXT("Inputs is an object."), TEXT("Inputs"));
		}
		const FValue NoInputs = Graph::MakeEmptyObject();
		FReader Nested = Dynamic.Child(TEXT("Inputs"), Inputs != nullptr ? *Inputs : NoInputs);
		TArray<FString> Reset;
		if (UNiagaraStackFunctionInput* Owner = ResolveInput(Module, Path))
		{
			for (const UNiagaraStackFunctionInput* Child : DynamicInputInputs(*Owner))
			{
				if (Child != nullptr && Child->CanReset() && (Inputs == nullptr || !Inputs->ObjectKeys.Contains(InputName(*Child))))
				{
					Reset.Add(InputName(*Child));
				}
			}
		}
		return Apply(Nested, Path, Inputs, Reset, Where + TEXT("/Dynamic/Inputs"), Depth + 1);
	}

	bool SetValue(const TArray<FString>& Path, FReader& Holder, const FString& Key, const FValue& Value, const FString& Where, int32 Depth)
	{
		UNiagaraStackFunctionInput* Input = ResolveInput(Module, Path);
		if (Input == nullptr)
		{
			return Fail(TEXT("Niagara.InputNotFound"), FString::Printf(TEXT("'%s' has no input '%s'."), *Module.GetModuleNode().GetFunctionName(), *JoinPath(Path)), Where);
		}
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Object && Value.ObjectKeys.Num() >= 1)
		{
			FReader Form = Holder.Child(Key, Value);
			const FString& Tag = Value.ObjectKeys[0];
			if (Tag == TEXT("Link"))
			{
				return SetLink(*Input, Form, Where);
			}
			if (Tag == TEXT("Dynamic"))
			{
				return SetDynamic(Path, Form, Where, Depth);
			}
			if (Tag == TEXT("Expression"))
			{
				return SetExpression(*Input, Form, Where);
			}
			if (Tag == TEXT("DataInterface"))
			{
				return SetDataInterface(*Input, Form, Where);
			}
			if (Tag == TEXT("Object"))
			{
				return SetObject(*Input, Form, Where);
			}
			if (Tag == TEXT("Unsupported"))
			{
				return Fail(TEXT("Niagara.UnsupportedInputValue"), FString::Printf(TEXT(
					"'%s' holds a value the document cannot write (%s); leave the input as the extract lists it or give it a value."),
					*InputName(*Input), Value.Elements[0].Kind == EAssetDocumentRuntimeValueKind::String ? *Value.Elements[0].StringValue : TEXT("?")), Where);
			}
		}
		return SetLocal(*Input, Value, Where);
	}

	// Resets, then sets in rounds (an input revealed by a static switch set in an earlier round is found later).
	bool Apply(FReader& Holder, const TArray<FString>& Parent, const FValue* Set, TConstArrayView<FString> Reset, const FString& Where, int32 Depth)
	{
		if (Depth > 16)
		{
			return Fail(TEXT("Graph.InvalidArgument"), TEXT("Dynamic inputs nest more than 16 levels."), Where);
		}
		for (const FString& Name : Reset)
		{
			TArray<FString> Path = Parent;
			Path.Add(Name);
			UNiagaraStackFunctionInput* Input = ResolveInput(Module, Path);
			if (Input != nullptr && Input->CanReset())
			{
				Input->Reset();
				Edit.bChanged = true;
			}
		}
		if (Set == nullptr)
		{
			return true;
		}
		TArray<int32> Pending;
		for (int32 Index = 0; Index < Set->ObjectKeys.Num(); ++Index)
		{
			Pending.Add(Index);
		}
		while (!Pending.IsEmpty())
		{
			TArray<int32> Missing;
			for (const int32 Index : Pending)
			{
				TArray<FString> Path = Parent;
				Path.Add(Set->ObjectKeys[Index]);
				if (ResolveInput(Module, Path) == nullptr)
				{
					Missing.Add(Index);
					continue;
				}
				if (!SetValue(Path, Holder, Set->ObjectKeys[Index], Set->Elements[Index], Where + TEXT("/") + Set->ObjectKeys[Index], Depth))
				{
					return false;
				}
			}
			if (Missing.Num() == Pending.Num())
			{
				const FString& Name = Set->ObjectKeys[Missing[0]];
				return Fail(TEXT("Niagara.InputNotFound"), FString::Printf(TEXT("'%s' has no input '%s'%s. Inputs: %s."),
					*Module.GetModuleNode().GetFunctionName(), *Name, Parent.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" under '%s'"), *JoinPath(Parent)),
					*AvailableInputs(Module, Parent)), Where + TEXT("/") + Name);
			}
			Pending = MoveTemp(Missing);
		}
		// A value equal to the input's default leaves the input not overridden (the stack shows no reset arrow), so the
		// extract does not list it: the document says so instead of failing the apply's verification.
		for (const FString& Name : Set->ObjectKeys)
		{
			TArray<FString> Path = Parent;
			Path.Add(Name);
			const UNiagaraStackFunctionInput* Input = ResolveInput(Module, Path);
			if (Input == nullptr || Input->CanReset())
			{
				continue;
			}
			const FString Label = FString::Printf(TEXT("'%s' of '%s'"), *JoinPath(Path), *Module.GetModuleNode().GetFunctionName());
			if (FNiagaraDefaultInputReport* Report = FNiagaraDefaultInputReport::Active())
			{
				Report->Add(Op.FieldPath(Where + TEXT("/") + Name), Label);
				continue;
			}
			return Fail(TEXT("Niagara.InputIsDefault"), FString::Printf(TEXT(
				"%s is set to its default value: leave it out (Inputs lists overridden inputs only; the default is what an omitted input has)."),
				*Label), Where + TEXT("/") + Name);
		}
		return true;
	}
};

bool EditInputs(FReader& Op, FStackEdit& Edit)
{
	FLocation Location;
	UNiagaraStackScriptItemGroup* Group = nullptr;
	UNiagaraStackModuleItem* Module = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("group"), TEXT("event"), TEXT("stage"), TEXT("module"), TEXT("set"), TEXT("reset")})
		|| !ResolveLocation(Op, Edit, Location) || !RequireGroup(Op, Location, Group) || !RequireModule(Op, Location, *Group, TEXT("module"), Module))
	{
		return false;
	}
	const FValue* Set = nullptr;
	if (Op.Has(TEXT("set")) && !Op.Object(TEXT("set"), Set))
	{
		return false;
	}
	const FValue* ResetList = nullptr;
	if (Op.Has(TEXT("reset")) && !Op.Array(TEXT("reset"), ResetList))
	{
		return false;
	}
	TArray<FString> Reset;
	for (int32 Index = 0; ResetList != nullptr && Index < ResetList->Elements.Num(); ++Index)
	{
		if (ResetList->Elements[Index].Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("reset lists input names."), *FString::Printf(TEXT("reset/%d"), Index));
		}
		Reset.Add(ResetList->Elements[Index].StringValue);
	}
	FReader Holder = Set != nullptr ? Op.Child(TEXT("set"), *Set) : Op;
	FInputSetter Setter{Op, Edit, *Module};
	return Setter.Apply(Holder, TArray<FString>(), Set, Reset, TEXT("set"), 0);
}

// ---- event handlers and simulation stages ---------------------------------------------------------------------

UNiagaraStackEmitterPropertiesGroup* PropertiesGroup(UNiagaraStackEntry& Root)
{
	TArray<UNiagaraStackEntry*> Children;
	Root.GetUnfilteredChildren(Children);
	for (UNiagaraStackEntry* Child : Children)
	{
		if (UNiagaraStackEmitterPropertiesGroup* Group = Cast<UNiagaraStackEmitterPropertiesGroup>(Child))
		{
			return Group;
		}
	}
	return nullptr;
}

bool EmitterRoot(FReader& Op, FStackEdit& Edit, TSharedPtr<FNiagaraEmitterHandleViewModel>& OutHandle, UNiagaraStackEntry*& OutRoot)
{
	if (!RequireHandle(Op, Edit, TEXT("emitter"), OutHandle))
	{
		return false;
	}
	UNiagaraStackViewModel* Stack = OutHandle->GetEmitterStackViewModel();
	OutRoot = Stack != nullptr ? Stack->GetRootEntry() : nullptr;
	if (OutRoot == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ViewModelUnavailable"), TEXT("The emitter stack could not be built."), TEXT("emitter"));
	}
	OutRoot->RefreshChildren();
	return true;
}

// The "+ Stage" action: "Event Handler" (category Events) or a simulation stage class (category Simulation Stages,
// keywords = the class name). OutOffered collects the offered stage classes.
TSharedPtr<INiagaraStackItemGroupAddAction> StageAction(INiagaraStackItemGroupAddUtilities& AddUtilities, bool bEvent, const UClass* StageClass,
	TArray<FString>* OutOffered)
{
	TArray<TSharedRef<INiagaraStackItemGroupAddAction>> Actions;
	AddUtilities.GenerateAddActions(Actions, FNiagaraStackItemGroupAddOptions());
	TSharedPtr<INiagaraStackItemGroupAddAction> Chosen;
	for (const TSharedRef<INiagaraStackItemGroupAddAction>& Action : Actions)
	{
		const TArray<FString> Categories = Action->GetCategories();
		if (bEvent)
		{
			if (Categories.Contains(TEXT("Events")) && !Chosen.IsValid())
			{
				Chosen = Action.ToSharedPtr();
			}
			continue;
		}
		if (!Categories.Contains(TEXT("Simulation Stages")))
		{
			continue;
		}
		const FString ClassName = Action->GetKeywords().ToString();
		if (OutOffered != nullptr)
		{
			OutOffered->Add(ClassName.StartsWith(TEXT("NiagaraSimulationStage")) ? ClassName.RightChop(22) : ClassName);
		}
		if (!Chosen.IsValid() && StageClass != nullptr && ClassName == StageClass->GetName())
		{
			Chosen = Action.ToSharedPtr();
		}
	}
	return Chosen;
}

bool AddEventHandler(FReader& Op, FStackEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	UNiagaraStackEntry* Root = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter")}) || !EmitterRoot(Op, Edit, Handle, Root))
	{
		return false;
	}
	UNiagaraStackEmitterPropertiesGroup* Group = PropertiesGroup(*Root);
	INiagaraStackItemGroupAddUtilities* AddUtilities = Group != nullptr ? Group->GetAddUtilities() : nullptr;
	const TSharedPtr<INiagaraStackItemGroupAddAction> Action = AddUtilities != nullptr ? StageAction(*AddUtilities, true, nullptr, nullptr) : nullptr;
	if (!Action.IsValid())
	{
		return Op.Fail(TEXT("Niagara.ViewModelUnavailable"), TEXT("The emitter's \"+ Stage\" menu offers no event handler."), TEXT("emitter"));
	}
	AddUtilities->ExecuteAddAction(Action.ToSharedRef(), INDEX_NONE);
	Edit.bChanged = true;
	return true;
}

UNiagaraStackEventScriptItemGroup* FindEventGroup(UNiagaraStackEntry& Root, const FGuid& UsageId)
{
	TArray<UNiagaraStackEntry*> Children;
	Root.GetUnfilteredChildren(Children);
	for (UNiagaraStackEntry* Child : Children)
	{
		UNiagaraStackEventScriptItemGroup* Group = Cast<UNiagaraStackEventScriptItemGroup>(Child);
		if (Group != nullptr && Group->GetScriptOutputNode() != nullptr && Group->GetScriptOutputNode()->GetUsageId() == UsageId)
		{
			return Group;
		}
	}
	return nullptr;
}

bool RequireEvent(FReader& Op, FNiagaraEmitterHandleViewModel& Handle, int32& OutIndex, FNiagaraEventScriptProperties*& OutProperties)
{
	int64 Index = 0;
	if (!Op.Int(TEXT("event"), Index))
	{
		return false;
	}
	FVersionedNiagaraEmitter Emitter = Handle.GetEmitterViewModel()->GetEmitter();
	const TArray<FNiagaraEventScriptProperties>& Handlers = Emitter.GetEmitterData()->GetEventHandlers();
	if (Index < 0 || Index >= Handlers.Num() || Handlers[static_cast<int32>(Index)].Script == nullptr)
	{
		return Op.Fail(TEXT("Niagara.EventHandlerNotFound"), FString::Printf(TEXT("'%s' has no event handler %lld (it has %d)."),
			*Handle.GetName().ToString(), Index, Handlers.Num()), TEXT("event"));
	}
	OutIndex = static_cast<int32>(Index);
	OutProperties = Emitter.GetEmitterData()->GetEventHandlerByIdUnsafe(Handlers[OutIndex].Script->GetUsageId());
	return OutProperties != nullptr;
}

bool DeleteEventHandler(FReader& Op, FStackEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	UNiagaraStackEntry* Root = nullptr;
	int32 Index = 0;
	FNiagaraEventScriptProperties* Properties = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("event")}) || !EmitterRoot(Op, Edit, Handle, Root) || !RequireEvent(Op, *Handle, Index, Properties))
	{
		return false;
	}
	UNiagaraStackEventScriptItemGroup* Group = FindEventGroup(*Root, Properties->Script->GetUsageId());
	if (Group == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ViewModelUnavailable"), TEXT("The stack has no group for the event handler."), TEXT("event"));
	}
	FText Message;
	if (!Group->TestCanDeleteWithMessage(Message))
	{
		return Op.Fail(Group->GetIsInherited() ? TEXT("Niagara.InheritedEventHandler") : TEXT("Niagara.EventHandlerNotDeletable"), FString::Printf(
			TEXT("Event handler %d cannot be deleted: %s"), Index, *Message.ToString()), TEXT("event"));
	}
	Group->Delete();
	Edit.bChanged = true;
	return true;
}

// The emitter's handle name of an event source id ("" = the emitter itself) and back.
FString SourceEmitterName(FNiagaraSystemViewModel& ViewModel, const FGuid& Id)
{
	if (!Id.IsValid())
	{
		return FString();
	}
	for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle : ViewModel.GetEmitterHandleViewModels())
	{
		if (Handle->GetEmitterHandle() != nullptr && Handle->GetId() == Id)
		{
			return Handle->GetName().ToString();
		}
	}
	return Id.ToString(EGuidFormats::DigitsWithHyphensLower);
}

bool EditEventHandler(FReader& Op, FStackEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	UNiagaraStackEntry* Root = nullptr;
	int32 Index = 0;
	FNiagaraEventScriptProperties* Properties = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("event"), TEXT("properties"), TEXT("reset")}) || !EmitterRoot(Op, Edit, Handle, Root)
		|| !RequireEvent(Op, *Handle, Index, Properties))
	{
		return false;
	}
	const FVersionedNiagaraEmitter Emitter = Handle->GetEmitterViewModel()->GetEmitter();
	UNiagaraEmitter* Object = Emitter.Emitter;
	const FGuid Version = Emitter.Version;
	const UScriptStruct* Struct = FNiagaraEventScriptProperties::StaticStruct();
	FProperty* SourceProperty = Struct->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FNiagaraEventScriptProperties, SourceEmitterID));
	auto Changed = [Object, Version](FProperty& Property)
	{
		FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
		Object->PostEditChangeVersionedProperty(Event, Version);
	};
	// SourceEmitter: the source emitter's dropdown (the handle id of the named emitter; "" = this emitter).
	const FValue* AuthoredProperties = Op.Find(TEXT("properties"));
	if (AuthoredProperties != nullptr && AuthoredProperties->Kind == EAssetDocumentRuntimeValueKind::Object)
	{
		const int32 SourceIndex = AuthoredProperties->ObjectKeys.IndexOfByKey(TEXT("SourceEmitter"));
		if (SourceIndex != INDEX_NONE)
		{
			const FValue& Source = AuthoredProperties->Elements[SourceIndex];
			if (Source.Kind != EAssetDocumentRuntimeValueKind::String)
			{
				return Op.Fail(TEXT("Niagara.InvalidPropertyValue"), TEXT("SourceEmitter is an emitter name (\"\" = this emitter)."), TEXT("properties/SourceEmitter"));
			}
			FGuid Id;
			if (!Source.StringValue.IsEmpty())
			{
				const TSharedPtr<FNiagaraEmitterHandleViewModel> SourceHandle = FindHandle(Edit.ViewModel, Source.StringValue);
				if (!SourceHandle.IsValid())
				{
					return Op.Fail(TEXT("Niagara.EmitterNotFound"), FString::Printf(TEXT("The system has no emitter '%s'."), *Source.StringValue),
						TEXT("properties/SourceEmitter"));
				}
				Id = SourceHandle->GetId();
			}
			if (Properties->SourceEmitterID != Id)
			{
				Object->Modify();
				Properties->SourceEmitterID = Id;
				Changed(*SourceProperty);
				Edit.bChanged = true;
			}
		}
	}
	const FValue* ResetList = Op.Find(TEXT("reset"));
	if (ResetList != nullptr && ResetList->Kind == EAssetDocumentRuntimeValueKind::Array)
	{
		for (const FValue& Name : ResetList->Elements)
		{
			if (Name.Kind == EAssetDocumentRuntimeValueKind::String && Name.StringValue == TEXT("SourceEmitter") && Properties->SourceEmitterID.IsValid())
			{
				Object->Modify();
				Properties->SourceEmitterID.Invalidate();
				Changed(*SourceProperty);
				Edit.bChanged = true;
			}
		}
	}
	const FNiagaraEventScriptProperties Defaults;
	const TArray<FName> Refused = {GET_MEMBER_NAME_CHECKED(FNiagaraEventScriptProperties, Script), GET_MEMBER_NAME_CHECKED(FNiagaraEventScriptProperties, SourceEmitterID)};
	const TArray<FString> Skip = {TEXT("SourceEmitter")};
	return WriteProperties(Op, *Struct, Properties, &Defaults, Object, Refused, Edit.bChanged,
		[Object](FProperty&) { Object->Modify(); }, Changed, Skip);
}

UNiagaraStackSimulationStageGroup* FindStageGroup(UNiagaraStackEntry& Root, const FString& Name)
{
	TArray<UNiagaraStackEntry*> Children;
	Root.GetUnfilteredChildren(Children);
	for (UNiagaraStackEntry* Child : Children)
	{
		UNiagaraStackSimulationStageGroup* Group = Cast<UNiagaraStackSimulationStageGroup>(Child);
		if (Group != nullptr && Group->GetSimulationStage() != nullptr && Group->GetSimulationStage()->SimulationStageName.ToString() == Name)
		{
			return Group;
		}
	}
	return nullptr;
}

bool RequireStage(FReader& Op, UNiagaraStackEntry& Root, const FNiagaraEmitterHandleViewModel& Handle, const TCHAR* Field, UNiagaraStackSimulationStageGroup*& OutGroup)
{
	FString Name;
	if (!Op.String(Field, Name))
	{
		return false;
	}
	OutGroup = FindStageGroup(Root, Name);
	if (OutGroup == nullptr)
	{
		return Op.Fail(TEXT("Niagara.StageNotFound"), FString::Printf(TEXT("'%s' has no simulation stage '%s'."), *Handle.GetName().ToString(), *Name), Field);
	}
	return true;
}

UClass* StageClass(const FString& Label)
{
	const FString Path = Label.StartsWith(TEXT("/")) ? Label : FString::Printf(TEXT("/Script/Niagara.NiagaraSimulationStage%s"), *Label);
	return FindObject<UClass>(nullptr, *Path);
}

bool RenameStage(FReader& Op, FStackEdit& Edit, UNiagaraSimulationStageBase& Stage, const FString& To, const TCHAR* Field)
{
	if (Stage.SimulationStageName.ToString() == To)
	{
		return true;
	}
	if (To.IsEmpty())
	{
		return Op.Fail(TEXT("Niagara.InvalidStageName"), TEXT("A simulation stage has a name."), Field);
	}
	FProperty* Property = Stage.GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageBase, SimulationStageName));
	Stage.Modify();
	Stage.PreEditChange(Property);
	Stage.SimulationStageName = FName(*To);
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Stage.PostEditChangeProperty(Event);
	Edit.bChanged = true;
	return true;
}

bool AddStage(FReader& Op, FStackEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	UNiagaraStackEntry* Root = nullptr;
	FString Name;
	FString ClassName;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("name"), TEXT("class"), TEXT("after")}) || !EmitterRoot(Op, Edit, Handle, Root)
		|| !Op.String(TEXT("name"), Name) || !Op.String(TEXT("class"), ClassName))
	{
		return false;
	}
	if (FindStageGroup(*Root, Name) != nullptr)
	{
		return Op.Fail(TEXT("Niagara.StageExists"), FString::Printf(TEXT("'%s' already has a simulation stage '%s'."), *Handle->GetName().ToString(), *Name),
			TEXT("name"));
	}
	FVersionedNiagaraEmitterData* Data = Handle->GetEmitterViewModel()->GetEmitter().GetEmitterData();
	int32 Index = 0;
	const FValue* After = Op.Find(TEXT("after"));
	if (After != nullptr && After->Kind == EAssetDocumentRuntimeValueKind::String)
	{
		UNiagaraStackSimulationStageGroup* Anchor = FindStageGroup(*Root, After->StringValue);
		if (Anchor == nullptr)
		{
			return Op.Fail(TEXT("Niagara.StageNotFound"), FString::Printf(TEXT("'%s' has no simulation stage '%s'."), *Handle->GetName().ToString(),
				*After->StringValue), TEXT("after"));
		}
		Index = Data->GetSimulationStages().IndexOfByKey(Anchor->GetSimulationStage()) + 1;
	}
	UNiagaraStackEmitterPropertiesGroup* Group = PropertiesGroup(*Root);
	INiagaraStackItemGroupAddUtilities* AddUtilities = Group != nullptr ? Group->GetAddUtilities() : nullptr;
	UClass* Class = StageClass(ClassName);
	TArray<FString> Offered;
	const TSharedPtr<INiagaraStackItemGroupAddAction> Action = AddUtilities != nullptr ? StageAction(*AddUtilities, false, Class, &Offered) : nullptr;
	if (!Action.IsValid())
	{
		Offered.Sort();
		return Op.Fail(TEXT("Niagara.UnknownStageClass"), FString::Printf(TEXT("'%s' is not a simulation stage the \"+ Stage\" menu offers. Classes: %s."),
			*ClassName, *FString::Join(Offered, TEXT(", "))), TEXT("class"));
	}
	const TArray<UNiagaraSimulationStageBase*> Before = Data->GetSimulationStages();
	AddUtilities->ExecuteAddAction(Action.ToSharedRef(), Index);
	Edit.bChanged = true;
	Data = Handle->GetEmitterViewModel()->GetEmitter().GetEmitterData();
	UNiagaraSimulationStageBase* Added = nullptr;
	for (UNiagaraSimulationStageBase* Stage : Data->GetSimulationStages())
	{
		if (Stage != nullptr && !Before.Contains(Stage))
		{
			Added = Stage;
		}
	}
	if (Added == nullptr)
	{
		return Op.Fail(TEXT("Niagara.AddStageFailed"), FString::Printf(TEXT("The %s simulation stage could not be added."), *ClassName), TEXT("class"));
	}
	return RenameStage(Op, Edit, *Added, Name, TEXT("name"));
}

bool DeleteStage(FReader& Op, FStackEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	UNiagaraStackEntry* Root = nullptr;
	UNiagaraStackSimulationStageGroup* Group = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("stage")}) || !EmitterRoot(Op, Edit, Handle, Root) || !RequireStage(Op, *Root, *Handle, TEXT("stage"), Group))
	{
		return false;
	}
	FText Message;
	if (!Group->TestCanDeleteWithMessage(Message))
	{
		return Op.Fail(Group->GetIsInherited() ? TEXT("Niagara.InheritedStage") : TEXT("Niagara.StageNotDeletable"), FString::Printf(
			TEXT("The simulation stage cannot be deleted: %s"), *Message.ToString()), TEXT("stage"));
	}
	Group->Delete();
	Edit.bChanged = true;
	return true;
}

bool MoveStage(FReader& Op, FStackEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	UNiagaraStackEntry* Root = nullptr;
	UNiagaraStackSimulationStageGroup* Group = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("stage"), TEXT("after")}) || !EmitterRoot(Op, Edit, Handle, Root)
		|| !RequireStage(Op, *Root, *Handle, TEXT("stage"), Group))
	{
		return false;
	}
	const TArray<UNiagaraSimulationStageBase*>& Stages = Handle->GetEmitterViewModel()->GetEmitter().GetEmitterData()->GetSimulationStages();
	const int32 At = Stages.IndexOfByKey(Group->GetSimulationStage());
	UNiagaraStackSimulationStageGroup* Target = nullptr;
	EItemDropZone Zone = EItemDropZone::AboveItem;
	const FValue* After = Op.Find(TEXT("after"));
	if (After != nullptr && After->Kind == EAssetDocumentRuntimeValueKind::String)
	{
		Target = FindStageGroup(*Root, After->StringValue);
		if (Target == nullptr)
		{
			return Op.Fail(TEXT("Niagara.StageNotFound"), FString::Printf(TEXT("'%s' has no simulation stage '%s'."), *Handle->GetName().ToString(),
				*After->StringValue), TEXT("after"));
		}
		if (Stages.IndexOfByKey(Target->GetSimulationStage()) + 1 == At)
		{
			return true;
		}
		Zone = EItemDropZone::BelowItem;
	}
	else
	{
		if (At == 0)
		{
			return true;
		}
		Target = Stages.Num() > 0 ? FindStageGroup(*Root, Stages[0]->SimulationStageName.ToString()) : nullptr;
	}
	if (Target == nullptr || Target == Group)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A simulation stage cannot be moved relative to itself."), TEXT("after"));
	}
	TArray<UNiagaraStackEntry*> Dragged;
	Dragged.Add(Group);
	const UNiagaraStackEntry::FDropRequest Request(MakeShared<FNiagaraStackEntryDragDropOp>(Dragged), Zone, UNiagaraStackEntry::EDragOptions::None,
		UNiagaraStackEntry::EDropOptions::Overview);
	const TOptional<UNiagaraStackEntry::FDropRequestResponse> Can = Target->CanDrop(Request);
	if (!Can.IsSet() || !Can->DropZone.IsSet())
	{
		return Op.Fail(Group->GetIsInherited() ? TEXT("Niagara.InheritedStage") : TEXT("Niagara.StageNotMovable"), FString::Printf(
			TEXT("The simulation stage cannot move there: %s"), Can.IsSet() ? *Can->DropMessage.ToString().Replace(TEXT("\n"), TEXT(" "))
			: TEXT("the stack does not accept the drop")), TEXT("stage"));
	}
	Target->Drop(Request);
	Edit.bChanged = true;
	return true;
}

bool EditStage(FReader& Op, FStackEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	UNiagaraStackEntry* Root = nullptr;
	UNiagaraStackSimulationStageGroup* Group = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("stage"), TEXT("enabled"), TEXT("name"), TEXT("properties"), TEXT("reset")})
		|| !EmitterRoot(Op, Edit, Handle, Root) || !RequireStage(Op, *Root, *Handle, TEXT("stage"), Group))
	{
		return false;
	}
	UNiagaraSimulationStageBase* Stage = Group->GetSimulationStage();
	if (Op.Has(TEXT("enabled")))
	{
		bool bEnabled = true;
		if (!Op.Bool(TEXT("enabled"), bEnabled, true))
		{
			return false;
		}
		if ((Stage->bEnabled != 0) != bEnabled)
		{
			Group->SetIsEnabled(bEnabled);
			Edit.bChanged = true;
		}
	}
	if (Op.Has(TEXT("properties")) || Op.Has(TEXT("reset")))
	{
		const TArray<FName> Refused = {GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageBase, SimulationStageName), FName(TEXT("bEnabled"))};
		if (!WriteProperties(Op, *Stage->GetClass(), Stage, Stage->GetClass()->GetDefaultObject(), Stage, Refused, Edit.bChanged,
			[Stage](FProperty& Property)
			{
				Stage->Modify();
				Stage->PreEditChange(&Property);
			},
			[Stage](FProperty& Property)
			{
				FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
				Stage->PostEditChangeProperty(Event);
			}))
		{
			return false;
		}
	}
	if (Op.Has(TEXT("name")))
	{
		FString To;
		if (!Op.String(TEXT("name"), To))
		{
			return false;
		}
		if (FindStageGroup(*Root, To) != nullptr && Stage->SimulationStageName.ToString() != To)
		{
			return Op.Fail(TEXT("Niagara.StageExists"), FString::Printf(TEXT("'%s' already has a simulation stage '%s'."), *Handle->GetName().ToString(), *To),
				TEXT("name"));
		}
		return RenameStage(Op, Edit, *Stage, To, TEXT("name"));
	}
	return true;
}
} // namespace

namespace
{
FNiagaraDefaultInputReport* GNiagaraActiveDefaultInputReport = nullptr;
}

FNiagaraDefaultInputReport::FNiagaraDefaultInputReport()
	: Previous(GNiagaraActiveDefaultInputReport)
{
	GNiagaraActiveDefaultInputReport = this;
}

FNiagaraDefaultInputReport::~FNiagaraDefaultInputReport()
{
	GNiagaraActiveDefaultInputReport = Previous;
}

FNiagaraDefaultInputReport* FNiagaraDefaultInputReport::Active()
{
	return GNiagaraActiveDefaultInputReport;
}

void FNiagaraDefaultInputReport::Add(const FString& Path, const FString& Label)
{
	Inputs.Emplace(Path, Label);
}

bool FNiagaraDefaultInputReport::Finish(FReader& Args) const
{
	if (Inputs.IsEmpty())
	{
		return true;
	}
	TArray<FString> Labels;
	for (const TPair<FString, FString>& Input : Inputs)
	{
		Labels.Add(Input.Value);
	}
	// The first input's path relative to Args (Args.FieldPath(Key) is "<args path>/<Key>").
	const FString Prefix = Args.FieldPath(TEXT("_")).LeftChop(1);
	const FString& First = Inputs[0].Key;
	const FString Key = First.StartsWith(Prefix) ? First.RightChop(Prefix.Len()) : FString();
	const FString Message = Inputs.Num() == 1
		? FString::Printf(TEXT("%s is set to its default value: leave it out (Inputs lists overridden inputs only; the default is what "
			"an omitted input has)."), *Labels[0])
		: FString::Printf(TEXT("%d inputs are set to their default value: %s. Leave them out (Inputs lists overridden inputs only; the "
			"default is what an omitted input has)."), Inputs.Num(), *FString::Join(Labels, TEXT(", ")));
	return Args.Fail(TEXT("Niagara.InputIsDefault"), Message, Key);
}

bool IsStackOp(const FString& Kind)
{
	static const TSet<FString> Kinds = {TEXT("add_module"), TEXT("add_assignment"), TEXT("delete_module"), TEXT("move_module"), TEXT("module"),
		TEXT("assignment"), TEXT("inputs"), TEXT("add_event_handler"), TEXT("delete_event_handler"), TEXT("event_handler"), TEXT("add_stage"),
		TEXT("delete_stage"), TEXT("move_stage"), TEXT("stage")};
	return Kinds.Contains(Kind);
}

bool ApplyStackOp(Graph::FReader& Op, const FString& Kind, FNiagaraSystemViewModel& ViewModel, bool& bOutChanged)
{
	FStackEdit Edit{ViewModel, bOutChanged};
	if (Kind == TEXT("add_module")) return AddModule(Op, Edit);
	if (Kind == TEXT("add_assignment")) return AddAssignment(Op, Edit);
	if (Kind == TEXT("delete_module")) return DeleteModule(Op, Edit);
	if (Kind == TEXT("move_module")) return MoveModule(Op, Edit);
	if (Kind == TEXT("module")) return EditModule(Op, Edit);
	if (Kind == TEXT("assignment")) return EditAssignment(Op, Edit);
	if (Kind == TEXT("inputs")) return EditInputs(Op, Edit);
	if (Kind == TEXT("add_event_handler")) return AddEventHandler(Op, Edit);
	if (Kind == TEXT("delete_event_handler")) return DeleteEventHandler(Op, Edit);
	if (Kind == TEXT("event_handler")) return EditEventHandler(Op, Edit);
	if (Kind == TEXT("add_stage")) return AddStage(Op, Edit);
	if (Kind == TEXT("delete_stage")) return DeleteStage(Op, Edit);
	if (Kind == TEXT("move_stage")) return MoveStage(Op, Edit);
	if (Kind == TEXT("stage")) return EditStage(Op, Edit);
	return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown stack op '%s'."), *Kind), TEXT("op"));
}

FString EventSourceEmitterName(FNiagaraSystemViewModel& ViewModel, const FGuid& Id)
{
	return SourceEmitterName(ViewModel, Id);
}
} // namespace UE::AssetDocument::Niagara

#undef LOCTEXT_NAMESPACE
