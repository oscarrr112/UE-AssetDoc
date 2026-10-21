// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// System and emitter asset reads through the Niagara editor's own stack view models.
//
// A view model is built headlessly for each read, the way the engine builds one for merges and Python
// upgrade scripts (NiagaraScriptMergeManager.cpp): bIsForDataProcessingOnly, no auto compile, no
// simulation, no timeline edits. Systems are read in place (SystemAsset mode); an emitter asset is read
// through a transient placeholder system holding the editor's working copy of it (EmitterAsset mode, as
// the emitter editor opens it, FNiagaraSystemToolkit::InitializeWithEmitter). Module lists come from the
// stack groups in execution order and inputs from UNiagaraStackFunctionInput, so value modes and
// "overridden" are exactly what the stack shows (the Reset arrow), whatever the storage (rapid iteration
// parameter or override pin).
//
// Rows (fx.niagara.read.v1, "rows"; every row has "kind"):
//   system           {}                                             (header)
//   compile          {compile: {Status, Messages}}                  (inspect only)
//   user_parameter   {name, type, value?, data_interface?, object?, description?, category?}
//   emitter          {name, enabled, mode, parent?, properties, stateless?}
//   module           {emitter, group, stage?, event?, name, script?, assign?, version?, enabled, display_name?,
//                     note?, inherited?, inputs: [input]}
//   renderer         {emitter, name, class, enabled, properties}
//   event_handler    {emitter, index, properties}                    (properties.SourceEmitter: a handle name)
//   declared_parameter {name, type, description?}                    (the Parameters panel's declared parameters)
//   simulation_stage {emitter, name, class, enabled, properties}
//   library          {group, scripts}                                (inspect only)
//   script, parameter, node, link, pin_default  (scratch pads, with "scope"; AssetDocumentNiagaraScriptRead.cpp)
// input: {name, type, mode, overridden, value? | link? (+ link_type when it differs from type) | dynamic? {script, version?, inputs} | expression? |
//         data_interface? | object?, + inspect: static, visible, inline_edit_condition, edit_condition?}
// "document" reads list only overridden inputs; "inspect" reads list every input.

#include "AssetDocumentNiagaraPrimitives.h"

#include "AssetDocumentEditorNonInteractiveScope.h"

#include "AssetRegistry/AssetData.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterBase.h"
#include "NiagaraEmitterEditorData.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMessages.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraScript.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraStackEditorData.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

namespace UE::AssetDocument::Niagara
{
namespace
{
using EValueMode = UNiagaraStackFunctionInput::EValueMode;

const TCHAR* ModeName(EValueMode Mode)
{
	switch (Mode)
	{
	case EValueMode::Local: return TEXT("Local");
	case EValueMode::Linked: return TEXT("Linked");
	case EValueMode::Dynamic: return TEXT("Dynamic");
	case EValueMode::Data: return TEXT("Data");
	case EValueMode::ObjectAsset: return TEXT("ObjectAsset");
	case EValueMode::Expression: return TEXT("Expression");
	case EValueMode::DefaultFunction: return TEXT("DefaultFunction");
	case EValueMode::InvalidOverride: return TEXT("InvalidOverride");
	case EValueMode::UnsupportedDefault: return TEXT("UnsupportedDefault");
	default: return TEXT("None");
	}
}

const TCHAR* UsageGroup(ENiagaraScriptUsage Usage)
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

// Module script reference: the asset path, or "ScratchPad:<name>" for a scratch pad script (a subobject of
// the system or of the emitter's scratch pad container).
FString ScriptReference(const UNiagaraScript* Script)
{
	if (Script == nullptr)
	{
		return FString();
	}
	return Script->IsAsset() ? Script->GetPathName() : FString(TEXT("ScratchPad:")) + Script->GetName();
}

// The selected script version, when it is not the version the script exposes (omitted = the exposed one).
FString NonDefaultVersion(const UNiagaraScript* Script, const FGuid& Selected)
{
	if (Script == nullptr || !Script->IsVersioningEnabled() || !Selected.IsValid()
		|| Selected == Script->GetExposedVersion().VersionGuid)
	{
		return FString();
	}
	return Selected.ToString(EGuidFormats::DigitsWithHyphensLower);
}

// Where a module sits in the document, for compile message attribution.
struct FModuleLocation
{
	FString Emitter;
	FString Group;
	FString Name;
};

class FStackReader
{
public:
	FStackReader(const FReadOptions& InOptions, TArray<FValue>& InRows)
		: Options(InOptions), Rows(InRows)
	{
	}

	void ReadStack(UNiagaraStackViewModel* Stack, const FString& Emitter, const FVersionedNiagaraEmitterData* EmitterData)
	{
		if (Stack == nullptr || Stack->GetRootEntry() == nullptr)
		{
			return;
		}
		TArray<UNiagaraStackEntry*> Groups;
		Stack->GetRootEntry()->GetUnfilteredChildren(Groups);
		for (UNiagaraStackEntry* Entry : Groups)
		{
			UNiagaraStackScriptItemGroup* Group = Cast<UNiagaraStackScriptItemGroup>(Entry);
			const UNiagaraNodeOutput* Output = Group != nullptr ? Group->GetScriptOutputNode() : nullptr;
			const TCHAR* Key = Output != nullptr ? UsageGroup(Output->GetUsage()) : nullptr;
			if (Key == nullptr)
			{
				continue;
			}
			FObjectBuilder Where;
			Where.AddString(TEXT("emitter"), Emitter);
			Where.AddString(TEXT("group"), Key);
			if (!AddGroupIdentity(Output->GetUsage(), Output->GetUsageId(), EmitterData, Where))
			{
				continue;
			}
			const FValue GroupFields = Where.Build();
			TArray<UNiagaraStackEntry*> Items;
			Group->GetUnfilteredChildren(Items);
			for (UNiagaraStackEntry* Item : Items)
			{
				if (UNiagaraStackModuleItem* Module = Cast<UNiagaraStackModuleItem>(Item))
				{
					Rows.Add(ReadModule(*Module, GroupFields, Emitter, Key));
				}
			}
			if (Options.bInspect)
			{
				AddLibrary(Key, Output->GetUsage());
			}
		}
	}

	FValue ReadModule(UNiagaraStackModuleItem& Module, const FValue& GroupFields, const FString& Emitter, const TCHAR* Group)
	{
		const UNiagaraNodeFunctionCall& Node = Module.GetModuleNode();
		FObjectBuilder Row;
		Row.AddString(TEXT("kind"), TEXT("module"));
		for (int32 Index = 0; Index < GroupFields.ObjectKeys.Num(); ++Index)
		{
			Row.Add(GroupFields.ObjectKeys[Index], GroupFields.Elements[Index]);
		}
		Row.AddString(TEXT("name"), Node.GetFunctionName());
		if (const UNiagaraNodeAssignment* Assignment = Cast<UNiagaraNodeAssignment>(&Node))
		{
			TArray<FValue> Targets;
			for (const FNiagaraVariable& Target : Assignment->GetAssignmentTargets())
			{
				FObjectBuilder TargetRow;
				TargetRow.AddString(TEXT("Target"), Target.GetName().ToString());
				TargetRow.AddString(TEXT("Type"), TypeToken(Target.GetType()));
				Targets.Add(TargetRow.Build());
			}
			Row.Add(TEXT("assign"), Graph::MakeArray(MoveTemp(Targets)));
		}
		else
		{
			Row.AddString(TEXT("script"), ScriptReference(Node.FunctionScript));
			const FString Version = NonDefaultVersion(Node.FunctionScript, Node.SelectedScriptVersion);
			if (!Version.IsEmpty())
			{
				Row.AddString(TEXT("version"), Version);
			}
		}
		Row.AddBool(TEXT("enabled"), Module.GetIsEnabled());
		const TOptional<FText> DisplayName = Module.GetAlternateDisplayName();
		if (DisplayName.IsSet() && !DisplayName->IsEmpty())
		{
			Row.AddString(TEXT("display_name"), DisplayName->ToString());
		}
		const TOptional<FNiagaraStackNoteData> Note = Module.GetStackEditorData().GetStackNote(Module.GetStackEditorDataKey());
		if (Note.IsSet())
		{
			FObjectBuilder NoteRow;
			NoteRow.AddString(TEXT("Header"), Note->MessageHeader.ToString());
			NoteRow.AddString(TEXT("Body"), Note->Message.ToString());
			Row.Add(TEXT("note"), NoteRow.Build());
		}
		if (Options.bInspect)
		{
			Row.AddBool(TEXT("inherited"), Module.GetIsInherited());
		}
		Row.Add(TEXT("inputs"), ReadInputs(ModuleInputs(Module)));
		ModuleLocations.Add(Node.NodeGuid, {Emitter, Group, Node.GetFunctionName()});
		return Row.Build();
	}

	FValue ReadInputs(TConstArrayView<UNiagaraStackFunctionInput*> Inputs)
	{
		TArray<FValue> Out;
		for (UNiagaraStackFunctionInput* Input : Inputs)
		{
			if (Input != nullptr && (Options.bInspect || Input->CanReset()))
			{
				Out.Add(ReadInput(*Input));
			}
		}
		return Graph::MakeArray(MoveTemp(Out));
	}

	FValue ReadInput(UNiagaraStackFunctionInput& Input)
	{
		const EValueMode Mode = Input.GetValueMode();
		FObjectBuilder Row;
		Row.AddString(TEXT("name"), Input.GetInputParameterHandle().GetName().ToString());
		Row.AddString(TEXT("type"), TypeToken(Input.GetInputType()));
		Row.AddString(TEXT("mode"), ModeName(Mode));
		Row.AddBool(TEXT("overridden"), Input.CanReset());
		switch (Mode)
		{
		case EValueMode::Local:
		{
			const TSharedPtr<const FStructOnScope> Local = Input.GetLocalValueStruct();
			FValue Value;
			if (Local.IsValid() && EncodeValue(Input.GetInputType(), Local->GetStructMemory(), Value))
			{
				Row.Add(TEXT("value"), MoveTemp(Value));
			}
			break;
		}
		case EValueMode::Linked:
		{
			const FNiagaraVariableBase& Linked = Input.GetLinkedParameterValue();
			Row.AddString(TEXT("link"), Linked.GetName().ToString());
			// A link to a parameter of another type (the stack converts, e.g. a Position read as a Vector).
			if (Linked.GetType() != Input.GetInputType())
			{
				Row.AddString(TEXT("link_type"), TypeToken(Linked.GetType()));
			}
			break;
		}
		case EValueMode::Dynamic:
		{
			const UNiagaraNodeFunctionCall* Node = Input.GetDynamicInputNode();
			FObjectBuilder Dynamic;
			Dynamic.AddString(TEXT("script"), Node != nullptr ? ScriptReference(Node->FunctionScript) : FString());
			if (Node != nullptr)
			{
				const FString Version = NonDefaultVersion(Node->FunctionScript, Node->SelectedScriptVersion);
				if (!Version.IsEmpty())
				{
					Dynamic.AddString(TEXT("version"), Version);
				}
			}
			Dynamic.Add(TEXT("inputs"), ReadInputs(DynamicInputInputs(Input)));
			Row.Add(TEXT("dynamic"), Dynamic.Build());
			break;
		}
		case EValueMode::Expression:
			Row.AddString(TEXT("expression"), Input.GetCustomExpressionText().ToString());
			break;
		case EValueMode::Data:
			if (const UNiagaraDataInterface* DataInterface = Input.GetDataValueObject())
			{
				Row.Add(TEXT("data_interface"), DescribeDataInterface(*DataInterface));
			}
			break;
		case EValueMode::ObjectAsset:
			Row.AddString(TEXT("object"), ObjectPathOrEmpty(Input.GetObjectAssetValue()));
			break;
		default:
			break;
		}
		if (Options.bInspect)
		{
			Row.AddBool(TEXT("static"), Input.IsStaticParameter());
			Row.AddBool(TEXT("visible"), !Input.GetHasVisibleCondition() || Input.GetVisibleConditionEnabled());
			Row.AddBool(TEXT("inline_edit_condition"), Input.GetIsInlineEditConditionToggle());
			if (Input.GetHasEditCondition())
			{
				Row.AddBool(TEXT("edit_condition"), Input.GetEditConditionEnabled());
			}
		}
		return Row.Build();
	}

	// Event handlers are identified by position, simulation stages by name (design §3.3).
	static bool AddGroupIdentity(ENiagaraScriptUsage Usage, const FGuid& UsageId, const FVersionedNiagaraEmitterData* EmitterData,
		FObjectBuilder& Where)
	{
		if (Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			const TArray<FNiagaraEventScriptProperties>& Handlers = EmitterData != nullptr ? EmitterData->GetEventHandlers()
				: TArray<FNiagaraEventScriptProperties>();
			for (int32 Index = 0; Index < Handlers.Num(); ++Index)
			{
				if (Handlers[Index].Script != nullptr && Handlers[Index].Script->GetUsageId() == UsageId)
				{
					Where.AddInt(TEXT("event"), Index);
					return true;
				}
			}
			return false;
		}
		if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			if (EmitterData != nullptr)
			{
				for (const UNiagaraSimulationStageBase* Stage : EmitterData->GetSimulationStages())
				{
					if (Stage != nullptr && Stage->Script != nullptr && Stage->Script->GetUsageId() == UsageId)
					{
						Where.AddString(TEXT("stage"), Stage->SimulationStageName.ToString());
						return true;
					}
				}
			}
			return false;
		}
		return true;
	}

	// The modules the stack's "+" menu offers for a group (library scripts whose usage matches).
	void AddLibrary(const TCHAR* Group, ENiagaraScriptUsage Usage)
	{
		if (LibraryGroups.Contains(Group))
		{
			return;
		}
		LibraryGroups.Add(Group);
		FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions Filter;
		Filter.ScriptUsageToInclude = ENiagaraScriptUsage::Module;
		Filter.TargetUsageToMatch = Usage;
		TArray<FAssetData> Assets;
		FNiagaraEditorUtilities::GetFilteredScriptAssets(Filter, Assets);
		TArray<FString> Paths;
		for (const FAssetData& Asset : Assets)
		{
			Paths.Add(Asset.GetObjectPathString());
		}
		Paths.Sort();
		FObjectBuilder Row;
		Row.AddString(TEXT("kind"), TEXT("library"));
		Row.AddString(TEXT("group"), Group);
		Row.Add(TEXT("scripts"), Graph::MakeStringArray(Paths));
		Rows.Add(Row.Build());
	}

	// The emitter body outside the stacks: handle, properties, renderers, event handlers, stages.
	void ReadEmitter(const FNiagaraEmitterHandle& Handle, const FString& Name)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("kind"), TEXT("emitter"));
		Row.AddString(TEXT("name"), Name);
		Row.AddBool(TEXT("enabled"), Handle.GetIsEnabled());
		const bool bStateless = Handle.GetEmitterMode() == ENiagaraEmitterMode::Stateless;
		Row.AddString(TEXT("mode"), bStateless ? TEXT("Stateless") : TEXT("Standard"));
		if (bStateless)
		{
			if (const UObject* Stateless = Handle.GetEmitterBase())
			{
				Row.Add(TEXT("stateless"), ReadStateless(*Stateless));
			}
			Rows.Add(Row.Build());
			return;
		}
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (Data == nullptr)
		{
			Rows.Add(Row.Build());
			return;
		}
		const FVersionedNiagaraEmitter Parent = Data->GetParent();
		if (Parent.Emitter != nullptr)
		{
			FObjectBuilder ParentRow;
			ParentRow.AddString(TEXT("Emitter"), Parent.Emitter->GetPathName());
			if (Parent.Emitter->IsVersioningEnabled() && Parent.Version.IsValid()
				&& Parent.Version != Parent.Emitter->GetExposedVersion().VersionGuid)
			{
				ParentRow.AddString(TEXT("Version"), Parent.Version.ToString(EGuidFormats::DigitsWithHyphensLower));
			}
			Row.Add(TEXT("parent"), ParentRow.Build());
		}
		const FVersionedNiagaraEmitterData Defaults;
		Row.Add(TEXT("properties"), SparseProperties(*FVersionedNiagaraEmitterData::StaticStruct(), Data, &Defaults));
		Rows.Add(Row.Build());

		for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
		{
			if (Renderer != nullptr)
			{
				Rows.Add(ReadRenderer(*Renderer, Name));
			}
		}
		const FNiagaraEventScriptProperties EventDefaults;
		const TArray<FNiagaraEventScriptProperties>& Handlers = Data->GetEventHandlers();
		for (int32 Index = 0; Index < Handlers.Num(); ++Index)
		{
			FObjectBuilder Handler;
			Handler.AddString(TEXT("kind"), TEXT("event_handler"));
			Handler.AddString(TEXT("emitter"), Name);
			Handler.AddInt(TEXT("index"), Index);
			FValue Properties = SparseProperties(*FNiagaraEventScriptProperties::StaticStruct(), &Handlers[Index], &EventDefaults,
				{TEXT("Script"), TEXT("SourceEmitterID")});
			// The source emitter's dropdown shows a handle name ("" = this emitter): its id is not authored.
			if (Handlers[Index].SourceEmitterID.IsValid())
			{
				const FString* Source = HandleNames.Find(Handlers[Index].SourceEmitterID);
				Properties.ObjectKeys.Add(TEXT("SourceEmitter"));
				Properties.Elements.Add(Graph::MakeString(Source != nullptr ? *Source
					: Handlers[Index].SourceEmitterID.ToString(EGuidFormats::DigitsWithHyphensLower)));
			}
			Handler.Add(TEXT("properties"), MoveTemp(Properties));
			Rows.Add(Handler.Build());
		}
		for (const UNiagaraSimulationStageBase* Stage : Data->GetSimulationStages())
		{
			if (Stage == nullptr)
			{
				continue;
			}
			FObjectBuilder StageRow;
			StageRow.AddString(TEXT("kind"), TEXT("simulation_stage"));
			StageRow.AddString(TEXT("emitter"), Name);
			StageRow.AddString(TEXT("name"), Stage->SimulationStageName.ToString());
			StageRow.AddString(TEXT("class"), ClassLabel(Stage->GetClass(), TEXT("NiagaraSimulationStage"), TEXT("")));
			StageRow.AddBool(TEXT("enabled"), Stage->bEnabled != 0);
			StageRow.Add(TEXT("properties"), SparseObjectProperties(*Stage, {TEXT("SimulationStageName"), TEXT("bEnabled")}));
			Rows.Add(StageRow.Build());
		}
		if (Data->ScratchPads != nullptr)
		{
			for (const UNiagaraScript* Script : Data->ScratchPads->Scripts)
			{
				if (Script != nullptr)
				{
					ReadScriptRows(*Script, Name + TEXT("/") + Script->GetName(), Options, Rows);
				}
			}
		}
	}

	FValue ReadRenderer(const UNiagaraRendererProperties& Renderer, const FString& Emitter) const
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("kind"), TEXT("renderer"));
		Row.AddString(TEXT("emitter"), Emitter);
		Row.AddString(TEXT("name"), Renderer.GetName());
		Row.AddString(TEXT("class"), ClassLabel(Renderer.GetClass(), TEXT("Niagara"), TEXT("Properties")));
		Row.AddBool(TEXT("enabled"), Renderer.GetIsEnabled());
		Row.Add(TEXT("properties"), SparseObjectProperties(Renderer, {TEXT("bIsEnabled")}));
		return Row.Build();
	}

	// A lightweight emitter's reflected shape (UNiagaraStatelessEmitter is engine-internal: read through
	// reflection only): its edit-visible properties, spawn infos, modules and renderers.
	FValue ReadStateless(const UObject& Emitter) const
	{
		FObjectBuilder Out;
		const TArray<FName> Lists = {TEXT("Modules"), TEXT("RendererProperties")};
		Out.Add(TEXT("Properties"), SparseObjectProperties(Emitter, Lists));
		auto ReadObjects = [&Emitter](const TCHAR* PropertyName, const TCHAR* Prefix, const TCHAR* Suffix, bool bNamed)
		{
			TArray<FValue> Items;
			const FArrayProperty* Array = CastField<FArrayProperty>(Emitter.GetClass()->FindPropertyByName(PropertyName));
			const FObjectPropertyBase* Inner = Array != nullptr ? CastField<FObjectPropertyBase>(Array->Inner) : nullptr;
			if (Inner == nullptr)
			{
				return Items;
			}
			FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(&Emitter));
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				const UObject* Item = Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index));
				if (Item == nullptr)
				{
					continue;
				}
				FObjectBuilder Row;
				if (bNamed)
				{
					Row.AddString(TEXT("Name"), Item->GetName());
				}
				Row.AddString(TEXT("Class"), ClassLabel(Item->GetClass(), Prefix, Suffix));
				Row.Add(TEXT("Properties"), SparseObjectProperties(*Item));
				Items.Add(Row.Build());
			}
			return Items;
		};
		Out.Add(TEXT("Modules"), Graph::MakeArray(ReadObjects(TEXT("Modules"), TEXT("NiagaraStatelessModule_"), TEXT(""), false)));
		Out.Add(TEXT("Renderers"), Graph::MakeArray(ReadObjects(TEXT("RendererProperties"), TEXT("Niagara"), TEXT("Properties"), true)));
		return Out.Build();
	}

	void ReadUserParameters(const UNiagaraSystem& System)
	{
		const FNiagaraUserRedirectionParameterStore& Store = System.GetExposedParameters();
		TMap<FName, FString> Descriptions;
		if (const UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System.GetEditorData()))
		{
			// The user parameter panel's metadata (description); read through reflection (the list is not public).
			const FArrayProperty* MetaData = CastField<FArrayProperty>(UNiagaraSystemEditorData::StaticClass()->FindPropertyByName(TEXT("UserParameterMetaData")));
			const FObjectPropertyBase* Inner = MetaData != nullptr ? CastField<FObjectPropertyBase>(MetaData->Inner) : nullptr;
			if (Inner != nullptr)
			{
				FScriptArrayHelper Helper(MetaData, MetaData->ContainerPtrToValuePtr<void>(EditorData));
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					if (const UNiagaraScriptVariable* Variable = Cast<UNiagaraScriptVariable>(Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index))))
					{
						if (!Variable->Metadata.Description.IsEmpty())
						{
							Descriptions.Add(Variable->Variable.GetName(), Variable->Metadata.Description.ToString());
						}
					}
				}
			}
		}
		const TMap<FName, FString> Categories = ReadUserParameterCategories(System);
		TArray<FNiagaraVariableWithOffset> Parameters(Store.ReadParameterVariables());
		Parameters.Sort([](const FNiagaraVariableWithOffset& A, const FNiagaraVariableWithOffset& B)
		{
			return A.GetName().LexicalLess(B.GetName());
		});
		for (const FNiagaraVariableWithOffset& Parameter : Parameters)
		{
			if (!Parameter.GetName().ToString().StartsWith(TEXT("User.")))
			{
				continue;
			}
			FObjectBuilder Row;
			Row.AddString(TEXT("kind"), TEXT("user_parameter"));
			Row.AddString(TEXT("name"), Parameter.GetName().ToString());
			Row.AddString(TEXT("type"), TypeToken(Parameter.GetType()));
			if (Parameter.GetType().IsDataInterface())
			{
				if (const UNiagaraDataInterface* DataInterface = Store.GetDataInterface(Parameter.Offset))
				{
					Row.Add(TEXT("data_interface"), DescribeDataInterface(*DataInterface));
				}
			}
			else if (Parameter.GetType().IsUObject())
			{
				Row.AddString(TEXT("object"), ObjectPathOrEmpty(Store.GetUObject(Parameter.Offset).Get()));
			}
			else
			{
				FValue Value;
				if (EncodeValue(Parameter.GetType(), Store.GetParameterData(Parameter), Value))
				{
					Row.Add(TEXT("value"), MoveTemp(Value));
				}
			}
			if (const FString* Description = Descriptions.Find(Parameter.GetName()))
			{
				Row.AddString(TEXT("description"), *Description);
			}
			if (const FString* Category = Categories.Find(Parameter.GetName()))
			{
				Row.AddString(TEXT("category"), *Category);
			}
			Rows.Add(Row.Build());
		}
	}

	// The system Parameters panel's declared parameters (by name).
	void ReadDeclared(UNiagaraSystem& System)
	{
		TArray<const UNiagaraScriptVariable*> Declared = ReadDeclaredParameters(System);
		Declared.Sort([](const UNiagaraScriptVariable& A, const UNiagaraScriptVariable& B)
		{
			return A.Variable.GetName().LexicalLess(B.Variable.GetName());
		});
		for (const UNiagaraScriptVariable* Variable : Declared)
		{
			FObjectBuilder Row;
			Row.AddString(TEXT("kind"), TEXT("declared_parameter"));
			Row.AddString(TEXT("name"), Variable->Variable.GetName().ToString());
			Row.AddString(TEXT("type"), TypeToken(Variable->Variable.GetType()));
			if (!Variable->Metadata.Description.IsEmpty())
			{
				Row.AddString(TEXT("description"), Variable->Metadata.Description.ToString());
			}
			Rows.Add(Row.Build());
		}
	}

	// The toolbar's compile status and the last compile's warnings and errors, attributed to the module whose
	// call they come from (the stack's own mapping: the call node guid in the event's stack guids).
	void ReadCompile(const UNiagaraSystem& System)
	{
		TArray<FCompileMessage> Events;
		const FString Status = ReadSettledCompileStatus(System, Events);
		TArray<FValue> Messages;
		for (const FCompileMessage& Event : Events)
		{
			FObjectBuilder Message;
			Message.AddString(TEXT("Severity"), Event.Severity);
			Message.AddString(TEXT("Message"), Event.Message);
			for (const FGuid& Guid : Event.StackGuids)
			{
				if (const FModuleLocation* Location = ModuleLocations.Find(Guid))
				{
					Message.AddString(TEXT("Emitter"), Location->Emitter);
					Message.AddString(TEXT("Stack"), Location->Group);
					Message.AddString(TEXT("Module"), Location->Name);
					break;
				}
			}
			Messages.Add(Message.Build());
		}
		FObjectBuilder Compile;
		Compile.AddString(TEXT("Status"), Status);
		Compile.Add(TEXT("Messages"), Graph::MakeArray(MoveTemp(Messages)));
		FObjectBuilder Row;
		Row.AddString(TEXT("kind"), TEXT("compile"));
		Row.Add(TEXT("compile"), Compile.Build());
		Rows.Add(Row.Build());
	}

	const FReadOptions& Options;
	TArray<FValue>& Rows;
	TMap<FGuid, FModuleLocation> ModuleLocations;
	// Emitter handle id -> name (event handler sources).
	TMap<FGuid, FString> HandleNames;
	TSet<FString> LibraryGroups;
};

void ReplaceInStrings(FValue& Value, const FString& From, const FString& To)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::String && Value.StringValue.StartsWith(From, ESearchCase::CaseSensitive))
	{
		const FString Rest = Value.StringValue.Mid(From.Len());
		if (Rest.IsEmpty() || Rest[0] == TEXT('.') || Rest[0] == TEXT(':'))
		{
			Value.StringValue = To + (Rest.IsEmpty() ? Rest : FString(TEXT(":")) + Rest.Mid(1));
		}
	}
	for (FValue& Element : Value.Elements)
	{
		ReplaceInStrings(Element, From, To);
	}
}

FNiagaraSystemViewModelOptions HeadlessOptions(const UNiagaraSystem& System, ENiagaraSystemViewModelEditMode Mode)
{
	FNiagaraSystemViewModelOptions Options;
	Options.bCanModifyEmittersFromTimeline = false;
	Options.bCanSimulate = false;
	Options.bCanAutoCompile = false;
	Options.bIsForDataProcessingOnly = true;
	Options.EditMode = Mode;
	Options.MessageLogGuid = System.GetAssetGuid();
	return Options;
}

bool RefuseInteraction(FCall& Call, const FAssetDocumentEditorNonInteractiveScope& Scope)
{
	if (!Scope.WasInteractionRefused())
	{
		return true;
	}
	return Fail(Call, TEXT("Niagara.InteractiveReadRefused"), FString::Printf(
		TEXT("Reading %s tried to open an editor dialog, which an MCP request cannot answer; the read was refused."),
		*Call.Asset.GetPathName()));
}
} // namespace

namespace
{
// Owner's descendant inputs that belong to Node: the parameter hierarchy nests inputs under other inputs of the
// same function call, a dynamic input's inputs belong to the dynamic input's node.
void AddOwnInputs(const UNiagaraStackFunctionInput& Owner, const UNiagaraNodeFunctionCall* Node, TArray<UNiagaraStackFunctionInput*>& Out)
{
	for (UNiagaraStackFunctionInput* Child : Owner.GetChildInputs())
	{
		if (Child != nullptr && Node != nullptr && &Child->GetInputFunctionCallNode() == Node)
		{
			Out.AddUnique(Child);
		}
	}
}
} // namespace

TArray<UNiagaraStackFunctionInput*> ModuleInputs(UNiagaraStackModuleItem& Module)
{
	TArray<UNiagaraStackFunctionInput*> Top;
	Module.GetParameterInputs(Top);
	TArray<UNiagaraStackFunctionInput*> Out;
	for (UNiagaraStackFunctionInput* Input : Top)
	{
		if (Input != nullptr)
		{
			Out.AddUnique(Input);
			AddOwnInputs(*Input, &Module.GetModuleNode(), Out);
		}
	}
	return Out;
}

TArray<UNiagaraStackFunctionInput*> DynamicInputInputs(const UNiagaraStackFunctionInput& Input)
{
	TArray<UNiagaraStackFunctionInput*> Out;
	AddOwnInputs(Input, Input.GetDynamicInputNode(), Out);
	return Out;
}

FBoolProperty* SummaryViewFlag()
{
	static FBoolProperty* const Flag = FindFProperty<FBoolProperty>(UNiagaraEmitterEditorData::StaticClass(), TEXT("bShowSummaryView"));
	return Flag;
}

void FFullStackView::Cover(UNiagaraSystem& System)
{
	FBoolProperty* const Flag = SummaryViewFlag();
	if (Flag == nullptr)
	{
		return;
	}
	for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
	{
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		UNiagaraEmitterEditorData* EditorData = Data != nullptr ? Cast<UNiagaraEmitterEditorData>(Data->GetEditorData()) : nullptr;
		if (EditorData != nullptr && EditorData->ShouldShowSummaryView())
		{
			Flag->SetPropertyValue_InContainer(EditorData, false);
			Cleared.Add(EditorData);
			EditorData->OnSummaryViewStateChanged().Broadcast();
		}
	}
}

void FFullStackView::Restore()
{
	FBoolProperty* const Flag = SummaryViewFlag();
	for (const TWeakObjectPtr<UObject>& Entry : Cleared)
	{
		if (UObject* EditorData = Entry.Get(); EditorData != nullptr && Flag != nullptr)
		{
			Flag->SetPropertyValue_InContainer(EditorData, true);
		}
	}
	Cleared.Reset();
}

bool ReadSystemRows(FCall& Call, UNiagaraSystem& System, const FReadOptions& Options, TArray<FValue>& OutRows)
{
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	FStackReader Reader(Options, OutRows);
	{
		FFullStackView FullStack;
		FullStack.Cover(System);
		const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();
		{
			FScratchPadsSetAside SetAside(&System.ScratchPadScripts);
			ViewModel->Initialize(System, HeadlessOptions(System, ENiagaraSystemViewModelEditMode::SystemAsset));
		}
		FObjectBuilder Header;
		Header.AddString(TEXT("kind"), TEXT("system"));
		OutRows.Add(Header.Build());
		Reader.ReadUserParameters(System);
		Reader.ReadDeclared(System);
		for (const FNiagaraEmitterHandle& EmitterHandle : System.GetEmitterHandles())
		{
			Reader.HandleNames.Add(EmitterHandle.GetId(), EmitterHandle.GetName().ToString());
		}
		Reader.ReadStack(ViewModel->GetSystemStackViewModel(), FString(), nullptr);
		for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle : ViewModel->GetEmitterHandleViewModels())
		{
			const FNiagaraEmitterHandle* EmitterHandle = Handle->GetEmitterHandle();
			if (EmitterHandle == nullptr)
			{
				continue;
			}
			const FString Name = EmitterHandle->GetName().ToString();
			Reader.ReadEmitter(*EmitterHandle, Name);
			Reader.ReadStack(Handle->GetEmitterStackViewModel(), Name, EmitterHandle->GetEmitterData());
		}
		for (const UNiagaraScript* Script : System.ScratchPadScripts)
		{
			if (Script != nullptr)
			{
				ReadScriptRows(*Script, FString(TEXT("/")) + Script->GetName(), Options, OutRows);
			}
		}
		if (Options.bInspect)
		{
			Reader.ReadCompile(System);
		}
	}
	return RefuseInteraction(Call, NonInteractive);
}

bool ReadEmitterAssetRows(FCall& Call, UNiagaraEmitter& Emitter, const FReadOptions& Options, TArray<FValue>& OutRows)
{
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	FStackReader Reader(Options, OutRows);
	{
		// The emitter editor's working copy lives in a transient placeholder system (MakeEmitterPlaceholder, as
		// FNiagaraSystemToolkit::InitializeWithEmitter makes it). The view model is built in the merge manager's
		// data-processing mode, so nothing compiles and the editor's recently-used emitter list is not touched.
		TStrongObjectPtr<UNiagaraSystem> Placeholder(MakeEmitterPlaceholder(Emitter));
		FFullStackView FullStack;
		FullStack.Cover(*Placeholder);
		const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();
		{
			FVersionedNiagaraEmitterData* CopyData = Placeholder->GetEmitterHandles().Num() == 1 ? Placeholder->GetEmitterHandles()[0].GetEmitterData() : nullptr;
			FScratchPadsSetAside SetAside(CopyData != nullptr && CopyData->ScratchPads != nullptr ? &CopyData->ScratchPads->Scripts : nullptr);
			ViewModel->Initialize(*Placeholder, HeadlessOptions(*Placeholder, ENiagaraSystemViewModelEditMode::EmitterDuringMerge));
		}
		const TArray<TSharedRef<FNiagaraEmitterHandleViewModel>>& Handles = ViewModel->GetEmitterHandleViewModels();
		const TSharedPtr<FNiagaraEmitterHandleViewModel> Handle = Handles.Num() == 1 ? Handles[0].ToSharedPtr() : nullptr;
		const FNiagaraEmitterHandle* EmitterHandle = Handle.IsValid() ? Handle->GetEmitterHandle() : nullptr;
		if (EmitterHandle == nullptr)
		{
			return Fail(Call, TEXT("Niagara.ViewModelUnavailable"), FString::Printf(
				TEXT("The Niagara emitter editor's view model could not be built for %s."), *Emitter.GetPathName()));
		}
		FObjectBuilder Header;
		Header.AddString(TEXT("kind"), TEXT("system"));
		OutRows.Add(Header.Build());
		// The working copy's handle name (the emitter's unique emitter name) names the emitter in the rows and in the
		// edit ops; "new" tells a package this apply created (made from another emitter only then); "versions" is
		// the version menu of a versioned emitter (the rows describe the exposed version).
		const FString Name = EmitterHandle->GetName().ToString();
		FObjectBuilder Asset;
		Asset.AddString(TEXT("kind"), TEXT("emitter_asset"));
		Asset.AddString(TEXT("name"), Name);
		Asset.AddBool(TEXT("new"), Emitter.GetOutermost()->HasAnyPackageFlags(PKG_NewlyCreated));
		if (Emitter.IsVersioningEnabled())
		{
			Asset.Add(TEXT("versions"), ReadVersionList(Emitter));
		}
		OutRows.Add(Asset.Build());
		const int32 FirstRow = OutRows.Num();
		Reader.HandleNames.Add(EmitterHandle->GetId(), Name);
		Reader.ReadEmitter(*EmitterHandle, Name);
		Reader.ReadStack(Handle->GetEmitterStackViewModel(), Name, EmitterHandle->GetEmitterData());
		if (Options.bInspect)
		{
			Reader.ReadCompile(*Placeholder);
		}
		// References into the working copy (a data interface naming one of the emitter's renderers) are
		// reported as references into the emitter asset itself.
		const UNiagaraEmitter* WorkingCopy = EmitterHandle->GetInstance().Emitter;
		if (WorkingCopy != nullptr && WorkingCopy != &Emitter)
		{
			for (int32 Index = FirstRow; Index < OutRows.Num(); ++Index)
			{
				ReplaceInStrings(OutRows[Index], WorkingCopy->GetPathName(), Emitter.GetPathName());
			}
		}
	}
	return RefuseInteraction(Call, NonInteractive);
}
} // namespace UE::AssetDocument::Niagara
