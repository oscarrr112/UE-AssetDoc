// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphPrimitives.h"
#include "UObject/UnrealType.h"
#include "UObject/ObjectPtr.h"

class FNiagaraSystemViewModel;
class FNiagaraVersionedObject;
class UNiagaraStackFunctionInput;
class UNiagaraStackModuleItem;
class FProperty;
class IAssetDocumentDerivedStateRebuilder;
class UClass;
class UPackage;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UNiagaraDataInterface;
class UNiagaraParameterCollection;
class UNiagaraScript;
class UNiagaraScratchPadViewModel;
class UNiagaraScriptVariable;
class UNiagaraSystem;
class UNiagaraEmitter;
class UNiagaraGraph;
class UObject;
class UStruct;
struct FNiagaraTypeDefinition;
struct FNiagaraVariable;

// Niagara primitives (canonical provider "fx", owner FX). The read primitive, fx.niagara.read.v1, serves
// the four asset classes as a paged list of rows (row contracts: AssetDocumentNiagaraStackRead.cpp and
// AssetDocumentNiagaraScriptRead.cpp):
//   systems and emitter assets  -> read through the Niagara editor's stack view models, built headlessly
//                                  (FNiagaraSystemViewModel with bIsForDataProcessingOnly, as the engine's
//                                  merge and upgrade paths build them): what the stack shows, including
//                                  each input's value mode and whether it is overridden;
//   scripts and scratch pads    -> the script graph (nodes, links, pin defaults), its parameters and settings;
//   parameter collections       -> the parameters with their default values.
// Reads never compile, never dirty a package and never open UI.
// System-level authoring: fx.niagara.edit.v1 runs an op list over one headless system view model
// (emitters, emitter properties, renderers: AssetDocumentNiagaraEdit.cpp; user parameters:
// AssetDocumentNiagaraUserParameters.cpp) and fx.niagara.compile.v1 is the toolbar's Compile
// (AssetDocumentNiagaraCompile.cpp).
namespace UE::AssetDocument::Niagara
{
using Graph::FCall;
using Graph::FObjectBuilder;
using Graph::FValue;

inline const TCHAR* ProviderId = TEXT("fx");

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors();
TConstArrayView<Graph::FBinding> GetBindings();

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/"));

// What a read returns: the document's authored state ("document") or, for Inspect, everything the stack
// lists (every input with its value mode, the module library per group, compile status).
struct FReadOptions
{
	bool bInspect = false;
	// Script graph node rows also carry property_defaults: the values of a new node of that kind (the template the
	// sparse properties are read against), for Diff / Verify of authored values the sparse read leaves out.
	bool bNodeDefaults = false;
};

// ---- values (AssetDocumentNiagaraValues.cpp) ---------------------------------------------------------

// Document type token of a Niagara type: float, int32, bool, Vector2, Vector, Vector4, LinearColor, Quat,
// Position, Matrix, ID, an enum / struct path, a data interface class path, "Object:<class path>".
FString TypeToken(const FNiagaraTypeDefinition& Type);

// The value of a plain-data Niagara type stored at Data: float32 as the shortest decimal, the scalar
// wrapper structs (FNiagaraFloat / Int32 / Bool) as JSON scalars, enums by authored name, structs as
// objects through the Editor typed value codec. False when the type is not plain data or not representable.
bool EncodeValue(const FNiagaraTypeDefinition& Type, const uint8* Data, FValue& OutValue);

// Short label of a class under a prefix / suffix convention ("NiagaraSpriteRendererProperties" ->
// "SpriteRenderer"); classes outside the convention (or outside /Script/Niagara*) by class path.
FString ClassLabel(const UClass* Class, const TCHAR* Prefix, const TCHAR* Suffix);

// Edit-visible (CPF_Edit) properties of Struct whose value in Container differs from Defaults, encoded with
// the Editor typed value codec (floats shortest). Skip names are never listed; properties the codec
// cannot represent are appended to OutUnsupported (when given).
FValue SparseProperties(const UStruct& Struct, const void* Container, const void* Defaults,
	TConstArrayView<FName> Skip = {}, TArray<FString>* OutUnsupported = nullptr);

// True when the property's document form (SparseProperties) equals the default's: reset skips it.
bool PropertyMatchesDefault(FProperty& Property, const void* ValuePtr, const void* DefaultPtr);

// SparseProperties of an object against its class default object.
FValue SparseObjectProperties(const UObject& Object, TConstArrayView<FName> Skip = {}, TArray<FString>* OutUnsupported = nullptr);

// {"Class": <label>, "Properties": {...}} of a data interface value.
FValue DescribeDataInterface(const UNiagaraDataInterface& DataInterface);

// The registered Niagara type whose TypeToken is Token (false when none is).
bool TypeFromToken(const FString& Token, FNiagaraTypeDefinition& OutType);

// The inverse of EncodeValue: Value written over the plain-data value at Data (struct members the value
// omits keep their current value). False with OutMessage when the value does not fit the type.
bool DecodeValue(const FNiagaraTypeDefinition& Type, const FValue& Value, uint8* Data, FString& OutMessage);
// An enum value's name without a trailing parenthesized explanation ("Kill (Emitter and Particles Die Immediately)" ->
// "Kill"); DecodeValue accepts it when exactly one value has it.
FString EnumShortName(const FString& Name);

// The inverse of SparseProperties for one property: Value (with type tokens where the read wrote type
// handles) decoded by the Editor typed value codec into ValuePtr. Owner is the object holding the value.
// BasePtr: the value Value edits (struct members it omits keep theirs; default: the value at ValuePtr). A data
// interface's Properties edit its class default's values: they are complete at every depth, as the read is
// sparse over the class default (DescribeDataInterface).
bool DecodeProperty(FProperty& Property, const FValue& Value, void* ValuePtr, UObject* Owner, FString& OutCode,
	const void* BasePtr = nullptr);

// Would DecodeProperty change the value at ValuePtr? (Decodes into a copy.) False with OutCode on a decode
// failure, OutSame telling whether the decoded value is identical to the current one.
bool DecodePropertyIsSame(FProperty& Property, const FValue& Value, const void* ValuePtr, UObject* Owner, bool& OutSame, FString& OutCode,
	const void* BasePtr = nullptr);

// The edit-visible (CPF_Edit, not deprecated / transient) property of Struct whose authored name is Name.
FProperty* FindEditableProperty(const UStruct& Struct, const FString& Name);

// Details edit of a reflected container: "properties" writes each listed property (skipped when the decoded
// value is identical), "reset" ([names]) sets each listed property back to its default (Defaults: the same
// container type holding the defaults; the extract leaves properties at their defaults out). Before / After
// bracket each write (the owner's Modify / PreEditChange and PostEditChange calls). Skip: keys the caller
// handles itself (neither written nor refused).
template <typename FBefore, typename FAfter>
inline bool WriteProperties(Graph::FReader& Op, const UStruct& Struct, void* Container, const void* Defaults, UObject* Owner,
	TConstArrayView<FName> Refused, bool& bOutChanged, FBefore&& Before, FAfter&& After,
	TConstArrayView<FString> Skip = TConstArrayView<FString>())
{
	if (const FValue* Properties = Op.Find(TEXT("properties")))
	{
		if (Properties->Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("properties is an object."), TEXT("properties"));
		}
		for (int32 Index = 0; Index < Properties->ObjectKeys.Num(); ++Index)
		{
			const FString& Key = Properties->ObjectKeys[Index];
			if (Skip.Contains(Key))
			{
				continue;
			}
			const FString Where = FString(TEXT("properties/")) + Key;
			FProperty* Property = FindEditableProperty(Struct, Key);
			if (Property == nullptr || Refused.Contains(Property->GetFName()))
			{
				return Op.Fail(TEXT("Niagara.UnknownProperty"), FString::Printf(TEXT("%s has no editable property '%s'."), *Struct.GetName(), *Key), Where);
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
			bool bSame = false;
			FString Code;
			if (!DecodePropertyIsSame(*Property, Properties->Elements[Index], ValuePtr, Owner, bSame, Code))
			{
				return Op.Fail(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT("'%s' does not fit the property (%s)."), *Key, *Code), Where);
			}
			if (bSame)
			{
				continue;
			}
			Before(*Property);
			DecodeProperty(*Property, Properties->Elements[Index], ValuePtr, Owner, Code);
			After(*Property);
			bOutChanged = true;
		}
	}
	const FValue* Reset = nullptr;
	if (Op.Has(TEXT("reset")) && !Op.Array(TEXT("reset"), Reset))
	{
		return false;
	}
	for (int32 Index = 0; Reset != nullptr && Index < Reset->Elements.Num(); ++Index)
	{
		const FValue& Name = Reset->Elements[Index];
		const FString Where = FString::Printf(TEXT("reset/%d"), Index);
		if (Name.Kind == EAssetDocumentRuntimeValueKind::String && Skip.Contains(Name.StringValue))
		{
			continue;
		}
		FProperty* Property = Name.Kind == EAssetDocumentRuntimeValueKind::String ? FindEditableProperty(Struct, Name.StringValue) : nullptr;
		if (Property == nullptr || Refused.Contains(Property->GetFName()))
		{
			return Op.Fail(TEXT("Niagara.UnknownProperty"), FString::Printf(TEXT("%s has no editable property '%s'."), *Struct.GetName(),
				*Name.StringValue), Where);
		}
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
		const void* DefaultPtr = Property->ContainerPtrToValuePtr<void>(Defaults);
		if (PropertyMatchesDefault(*Property, ValuePtr, DefaultPtr))
		{
			continue;
		}
		if (Property->ContainsInstancedObjectProperty())
		{
			return Op.Fail(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT(
				"'%s' holds instanced objects; author its value instead of leaving it out."), *Name.StringValue), Where);
		}
		Before(*Property);
		Property->CopyCompleteValue(ValuePtr, DefaultPtr);
		After(*Property);
		bOutChanged = true;
	}
	return true;
}

// ---- module stacks (AssetDocumentNiagaraStackEdit.cpp) -------------------------------------------------

// The module stack ops of fx.niagara.edit.v1 (modules, their inputs, event handlers, simulation stages), made
// through the stack view models as the stack widgets make them. False with the op's error when refused.
bool IsStackOp(const FString& Kind);
bool ApplyStackOp(Graph::FReader& Op, const FString& Kind, FNiagaraSystemViewModel& ViewModel, bool& bOutChanged);
// The inputs of one function call as the stack lists them, in stack order: a module's (or a dynamic input's) own
// inputs, including those its parameter hierarchy nests under another input (InitializeParticle's "Lifetime Min"
// under "Lifetime Mode"), but not the inputs of a dynamic input set on one of them.
TArray<UNiagaraStackFunctionInput*> ModuleInputs(UNiagaraStackModuleItem& Module);
TArray<UNiagaraStackFunctionInput*> DynamicInputInputs(const UNiagaraStackFunctionInput& Input);
// The emitter handle name of an event handler's source emitter id ("" = none / the emitter itself).
FString EventSourceEmitterName(FNiagaraSystemViewModel& ViewModel, const FGuid& Id);

// Niagara.InputIsDefault over one edit call. While a report is active, an input set to its default value is
// recorded and the edit goes on; the call then fails once, naming every such input, so that one apply reports
// them all (an input at its default is not overridden, so the extract cannot list it). Without an active report
// the first one fails its op.
class FNiagaraDefaultInputReport
{
public:
	FNiagaraDefaultInputReport();
	~FNiagaraDefaultInputReport();
	FNiagaraDefaultInputReport(const FNiagaraDefaultInputReport&) = delete;
	FNiagaraDefaultInputReport& operator=(const FNiagaraDefaultInputReport&) = delete;

	static FNiagaraDefaultInputReport* Active();
	// Path: the input's full primitive path; Label: "'<input>' of '<module>'".
	void Add(const FString& Path, const FString& Label);
	// Fails Args with Niagara.InputIsDefault at the first recorded input; true when none was recorded.
	bool Finish(Graph::FReader& Args) const;

private:
	FNiagaraDefaultInputReport* Previous = nullptr;
	TArray<TPair<FString, FString>> Inputs;
};

// ---- readers ------------------------------------------------------------------------------------------

// System / emitter asset rows (AssetDocumentNiagaraStackRead.cpp).
bool ReadSystemRows(FCall& Call, UNiagaraSystem& System, const FReadOptions& Options, TArray<FValue>& OutRows);
bool ReadEmitterAssetRows(FCall& Call, UNiagaraEmitter& Emitter, const FReadOptions& Options, TArray<FValue>& OutRows);

// Script graph rows (AssetDocumentNiagaraScriptRead.cpp). Scope is "" for the asset's own script and
// "<emitter>/<scratch pad>" ("/<scratch pad>" at system level) for scratch pads.
void ReadScriptRows(const UNiagaraScript& Script, const FString& Scope, const FReadOptions& Options, TArray<FValue>& OutRows);
// Script graph node facts shared by the read rows and the graph schema adapter (AssetDocumentNiagaraGraphAdapters.cpp).
FString NodeActionKey(const UEdGraphNode& Node);
// Node properties never listed in a node's "properties" (layout, identity and the facts other fields carry).
const TArray<FName>& NodePropertySkip();
bool IsDynamicAddPin(const UEdGraphPin& Pin);
bool HasDynamicAddPin(const UEdGraphNode& Node);
// A node's Details as a document lists them: its sparse editable properties, plus a static switch's parameter name
// ("InputParameterName", the name box of the switch's Details) when it is not the new node's.
FValue NodeDetails(const UEdGraphNode& Node);
// The Details of a new node of Node's kind (the template NodeDetails compares against), every editable property.
FValue NodeDetailDefaults(const UEdGraphNode& Node);
// Script Settings properties the document does not list in Settings (the version fields Versions carries).
TArray<FName> ScriptSettingsSkip();
// An op or function call whose Numeric pins the user typed (the pin's "Convert Numeric To..." menu): a pin its
// signature (the op's, the called function's inputs) makes Numeric holds another type.
bool HasConvertedNumericPins(const UEdGraphNode& Node);
// The pins of an op or function call whose signature type is Numeric (name, direction).
TArray<TPair<FName, EEdGraphPinDirection>> NumericSignaturePins(const UEdGraphNode& Node);
// The node lists its "pins": an Add pin, or converted numeric pins.
inline bool HasPinsSetting(const UEdGraphNode& Node)
{
	return HasDynamicAddPin(Node) || HasConvertedNumericPins(Node);
}
// {Inputs, Outputs} of {Name, Type} of the pins the user adds on a node with an Add pin (or types, on an op).
FValue DynamicPins(const UEdGraphNode& Node);
// The function call's selected script version when it is not the exposed one ("" otherwise).
FString NodeVersion(const UEdGraphNode& Node);
void ReadParameterCollectionRows(const UNiagaraParameterCollection& Collection, TArray<FValue>& OutRows);

// An object value as documents write it: its path, "" for none (GetPathNameSafe says "None").
FString ObjectPathOrEmpty(const UObject* Object);

// ---- compile status (AssetDocumentNiagaraCompile.cpp) -----------------------------------------------

struct FCompileMessage
{
	FString Severity;
	FString Message;
	TArray<FGuid> StackGuids;
};

// The toolbar's compile status of a system (UpToDate, UpToDateWithWarnings, Dirty, Error) and the
// warnings / errors of its scripts' last compile. Never compiles.
FString ReadCompileStatus(const UNiagaraSystem& System, TArray<FCompileMessage>& OutMessages);

// ReadCompileStatus, except that a Dirty system with a compile request outstanding (a compile in flight, e.g.
// the one an editor requests when it opens) is first waited for (SettleCompilation): the outcome of that
// compile, or Compiling when it is still running past the budget. Starts no compile.
FString ReadSettledCompileStatus(const UNiagaraSystem& System, TArray<FCompileMessage>& OutMessages);

// Wait (no progress UI, GPU shaders not awaited) until the system has no compile request outstanding or the
// time budget runs out, then let the Niagara compiling manager drop the finished tasks: a finished task keeps
// its digested graphs, whose data interface duplicates reference the system, until the manager's next tick,
// and a rollback that deletes or restores the package must not find them. True when nothing is outstanding.
bool SettleCompilation(UNiagaraSystem& System);

// An emitter's copy of its parent, kept for merging, has no inheritance of its own: the engine removes it when the copy
// is loaded or saved (UNiagaraEmitter::PreSave). A copy made by duplicating an emitter (adding an emitter to a system,
// creating or merging an inheriting emitter) still references its own parent's copy until then. The copies in Package
// drop it now, as their save would, so that what the save writes is known before it runs (the dropped parent copy is
// not written).
void SettleMergeCopies(UPackage& Package);

// Open-editor apply of systems, emitter assets, scripts and parameter collections (AssetDocumentNiagaraOpenEditor.cpp).
void RegisterNiagaraOpenEditorHandler();
void UnregisterNiagaraOpenEditorHandler();

// The "Compile" block of Extract / Apply results for systems (AssetDocumentCompileStatus.h).
void RegisterNiagaraCompileStatusDescriber();
void UnregisterNiagaraCompileStatusDescriber();

// ---- user parameters (AssetDocumentNiagaraUserParameters.cpp) ----------------------------------------

// The hierarchy category path ("Look", "Look|Fire") of each categorized user parameter, by name.
TMap<FName, FString> ReadUserParameterCategories(const UNiagaraSystem& System);

// The system Parameters panel's declared (editor-only) parameters.
TArray<const UNiagaraScriptVariable*> ReadDeclaredParameters(UNiagaraSystem& System);

// Objects of an object-array property, reached through reflection (engine-internal classes).
TArray<UObject*> ReflectedObjectList(const UObject* Owner, const TCHAR* PropertyName);

// The user parameter ops of fx.niagara.edit.v1 (add_user_parameter, delete_user_parameter,
// rename_user_parameter, user_parameter), as the system's User Parameters panel and its hierarchy editor make
// them. False with the op's error when the op is refused.
bool ApplyUserParameterOp(Graph::FReader& Op, const FString& Kind, FNiagaraSystemViewModel& ViewModel, bool& bOutChanged);

// ---- scripts and scratch pads (AssetDocumentNiagaraScriptEdit.cpp, AssetDocumentNiagaraScriptVariableRules.cpp) -----

// The graph a script is edited at: its exposed version's (the latest when it is not versioned), as the script editor
// opens it.
UNiagaraGraph* ScriptEditGraph(const UNiagaraScript& Script);

// The rule a load applies to a module / function / dynamic input graph: its script variables
// are exactly those of the parameters the graph references. True when it changed anything.
bool ManageScriptVariables(UNiagaraGraph& Graph, TArray<FString>* OutRemoved, TArray<FString>* OutCreated);

// The script-level ops of fx.niagara.edit.v1 that also apply to a system's scratch pads (script_settings,
// script_parameter, purge_parameters, refresh_scratch_pad) and the system's scratch pad ops (add_scratch_pad,
// delete_scratch_pad, rename_scratch_pad, scratch_pad_guids). ViewModel: the system's (null for a script asset).
bool IsScriptOp(const FString& Kind);
bool ApplyScriptOp(Graph::FReader& Op, const FString& Kind, UObject& Asset, FNiagaraSystemViewModel* ViewModel, bool& bOutChanged);
// fx.niagara.edit.v1 on a module / function / dynamic input asset.
bool HandleScriptEdit(FCall& Call, UNiagaraScript& Script, const FValue& Ops, FObjectBuilder& Result);

struct FScriptCompileMessage
{
	FString Severity;
	FString Message;
	FString Node; // the graph node's object name, when the event names one
};
// The script editor's compile status of a standalone script (never compiles) and its last compile's messages.
FString ReadScriptCompileStatus(const UNiagaraScript& Script, TArray<FScriptCompileMessage>& OutMessages);
// {status, errors, warnings, messages: [{severity, message, node?}]}.
void AddScriptCompileResult(const UNiagaraScript& Script, FObjectBuilder& Result);
// fx.niagara.compile.v1 on a script asset: the script editor's Compile (synchronous).
bool CompileScript(FCall& Call, UNiagaraScript& Script, FObjectBuilder& Result);

// The version menu (SNiagaraVersionWidget) of a versioned script or emitter asset: {op: "versions", versions: [...]}
// (AssetDocumentNiagaraScriptEdit.cpp). Asset is the object that Versioned is (Modify before the Details edits).
bool ApplyVersions(Graph::FReader& Op, UObject& Asset, FNiagaraVersionedObject& Versioned, bool& bOutChanged);
// The version menu's list as reads report it: [{Number, Guid, Exposed?, ChangeDescription?, Deprecated?,
// DeprecationMessage?, Visible?}] (AssetDocumentNiagaraScriptRead.cpp).
FValue ReadVersionList(FNiagaraVersionedObject& Versioned);

// ---- emitter assets (AssetDocumentNiagaraEmitterAsset.cpp) -----------------------------------------------

// The emitter editor's working copy (FNiagaraSystemToolkit::InitializeWithEmitter): a transient placeholder system
// whose only emitter handle holds a copy of the exposed version of Emitter, named by its unique emitter name.
UNiagaraSystem* MakeEmitterPlaceholder(UNiagaraEmitter& Emitter);
// The scratch pad list of the placeholder's emitter copy (null when it has none).
TArray<TObjectPtr<UNiagaraScript>>* PlaceholderPads(UNiagaraSystem& Placeholder);
// fx.niagara.edit.v1 / fx.niagara.compile.v1 / the "compile" read on an emitter asset.
bool HandleEmitterEdit(FCall& Call, UNiagaraEmitter& Emitter, const FValue& Ops, FObjectBuilder& Result);
bool CompileEmitter(FCall& Call, UNiagaraEmitter& Emitter, FObjectBuilder& Result);
bool ReadEmitterCompile(FCall& Call, UNiagaraEmitter& Emitter, FObjectBuilder& Result);
// Core package-snapshot rebuilder of emitter assets: loaded child emitters re-merge the restored emitter and the
// systems that use them recompile.
TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeEmitterRebuilder();
// Compile output a package snapshot does not restore (data interface copies cached by scripts, Set Parameters
// function scripts regenerated from their node): the rebuilders' Classify (AssetDocumentNiagaraCompile.cpp).
bool IsDerivedNiagaraObject(const UObject& Object);

// One system-level op of fx.niagara.edit.v1 (emitters, renderers, user parameters, stacks, scratch pads) on a
// headless system view model (AssetDocumentNiagaraEdit.cpp). OutCreated collects the emitters it adds.
bool ApplySystemEditOp(Graph::FReader& Op, const FString& Kind, UNiagaraSystem& System, const TSharedRef<FNiagaraSystemViewModel>& ViewModel,
	bool& bOutChanged, TArray<FString>& OutCreated);

// ---- parameter collections (AssetDocumentNiagaraCollectionEdit.cpp) -------------------------------------

// fx.niagara.edit.v1 on a parameter collection: the collection editor's parameter list and default values.
bool HandleCollectionEdit(FCall& Call, UNiagaraParameterCollection& Collection, const FValue& Ops, FObjectBuilder& Result);

// The Niagara adapters of the shared graph primitives (AssetDocumentNiagaraGraphAdapters.cpp).
void RegisterNiagaraGraphAdapters();
void UnregisterNiagaraGraphAdapters();

// The Scratch Pad panel makes a parameter panel view model per scratch pad (FNiagaraScriptToolkitParameterPanelViewModel)
// that registers with a static map keyed by the pad's transient edit copy, and nothing unregisters it (only the script
// and system toolkits clean up their own panels). Every system view model with scratch pads therefore leaves entries
// keyed by freed objects, and a later panel view model allocated at a freed address, for an edit copy allocated at a
// freed address, fails the map's check (TNiagaraViewModelManager.h: FoundItem->Contains(ViewModel) == false). Reads do
// not use the Scratch Pad panel (they read the scratch pad scripts themselves): their view model is built with the
// scratch pad list set aside, so it makes no panel view models, and the list is put back right after. Edits build
// theirs the same way and make the panel's view models when an op first uses the Scratch Pad panel
// (EnsureScratchPadViewModels).
class FScratchPadsSetAside
{
public:
	explicit FScratchPadsSetAside(TArray<TObjectPtr<UNiagaraScript>>* InScripts)
		: Scripts(InScripts)
	{
		if (Scripts != nullptr)
		{
			Saved = MoveTemp(*Scripts);
			Scripts->Reset();
		}
	}
	~FScratchPadsSetAside()
	{
		Restore();
	}
	FScratchPadsSetAside(const FScratchPadsSetAside&) = delete;
	FScratchPadsSetAside& operator=(const FScratchPadsSetAside&) = delete;
	void Restore()
	{
		if (Scripts != nullptr)
		{
			*Scripts = MoveTemp(Saved);
			Scripts = nullptr;
		}
	}

private:
	TArray<TObjectPtr<UNiagaraScript>>* Scripts = nullptr;
	TArray<TObjectPtr<UNiagaraScript>> Saved;
};

// The stack of an emitter whose editor data shows the Summary View (the emitter header's "Show Summary View" toggle,
// which the New Emitter dialog sets on emitters whose AddEmitterDefaultViewState is Summary) has only the Emitter
// Summary group: its module stacks, simulation stages, event handlers and renderers are not entries of the stack until
// the summary is turned off. Reads and edits work on the full stack, as a user does who turns the summary off: Cover
// clears the toggle on the emitters of System that show the summary (by reflection: SetShowSummaryView would also
// change the emitter's change id) and refreshes their stacks (OnSummaryViewStateChanged); the scope puts the toggles
// back (Restore, or when it ends), so the toggle, editor state of the asset, is unchanged. Cover again after an op that
// can add an emitter.
class FFullStackView
{
public:
	FFullStackView() = default;
	~FFullStackView() { Restore(); }
	FFullStackView(const FFullStackView&) = delete;
	FFullStackView& operator=(const FFullStackView&) = delete;
	void Cover(UNiagaraSystem& System);
	void Restore();

private:
	TArray<TWeakObjectPtr<UObject>> Cleared;
};

// The Scratch Pad panel of a view model built with FScratchPadsSetAside: its script view models, made as
// UNiagaraScratchPadViewModel::Initialize makes them (the first one active). Null when the view model has none.
UNiagaraScratchPadViewModel* EnsureScratchPadViewModels(FNiagaraSystemViewModel& ViewModel);

// ---- handlers -----------------------------------------------------------------------------------------

bool HandleRead(FCall& Call, FObjectBuilder& Result);
bool HandleEdit(FCall& Call, FObjectBuilder& Result);
bool HandleCompile(FCall& Call, FObjectBuilder& Result);

// Core package-snapshot rebuilder of systems: a restored system is recompiled (compile output is not
// transacted).
TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeSystemRebuilder();
// Core package-snapshot rebuilder of module / function / dynamic input assets: a restored script is recompiled.
TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeScriptRebuilder();
} // namespace UE::AssetDocument::Niagara
