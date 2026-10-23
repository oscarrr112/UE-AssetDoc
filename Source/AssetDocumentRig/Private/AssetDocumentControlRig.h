// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentRigPrimitives.h"

#include "Misc/PackageName.h"
#include "UObject/SoftObjectPtr.h"

class FJsonObject;
class IAssetDocumentDerivedStateRebuilder;
class UControlRigBlueprint;
class UObject;
class UScriptStruct;
class URigVMController;
class URigVMFunctionReferenceNode;
class URigVMGraph;
class URigVMBlueprint;
class URigVMNode;
struct FRigElementKey;
struct FRigModuleReference;
struct FRigVMMemoryStorageStruct;

// Control Rig documents (docs/architecture/asset-document-controlrig-design.md) on the "rig" provider. Reads:
// rig.controlrig.read.v1 (AssetDocumentControlRigRead.cpp, AssetDocumentControlRigGraphRead.cpp),
// rig.controlrig.catalog.v1 and the list_types kinds rigvm_node / rigvm_type / controlrig_module
// (AssetDocumentControlRigCatalog.cpp), the compile status describer (AssetDocumentControlRigStatus.cpp) and the
// shape library facts of Inspect; they read the stored, load-patched state as the Control Rig editor shows it and
// never compile, initialize a VM, open an editor or dirty a package. Edits: rig.controlrig.edit.v1
// (AssetDocumentControlRigEdit.cpp, graph ops in AssetDocumentControlRigGraphEdit.cpp, function ops in
// AssetDocumentControlRigFunctionEdit.cpp) and rig.controlrig.compile.v1 plus the rollback rebuilder
// (AssetDocumentControlRigCompile.cpp).
namespace UE::AssetDocument::Graph::Rig::ControlRig
{
inline const TCHAR* BlueprintClassPath = TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint");

// ---- values (AssetDocumentControlRigValues.cpp) ------------------------------------------------------------------

// The document name of a RigVM type (design §3.2 Variables): the CPP type for types without a type object
// ("float", "FName", "TArray<int32>"), else the CPP type with its innermost type token replaced by the type
// object's path ("/Script/CoreUObject.Vector", "TArray</Script/ControlRig.RigElementKey>",
// "TObjectPtr</Script/Engine.StaticMesh>").
FString TypeName(const FString& CPPType, const UObject* CPPTypeObject);

// True for types that carry no value in a document: execute contexts and wildcards.
bool IsValuelessType(const FString& CPPType, const UObject* CPPTypeObject);

// One property value as document JSON: the Editor typed value codec, then RigVM's short forms: an FRigElementKey is
// its ToString form ("Bone(spine_01)"), an FRigElementKeyCollection a list of those, float32 numbers are their
// shortest decimal, -0 reads 0 and an FTransform is {Translation?, Rotation?, Scale3D?} without identity parts.
bool EncodeProperty(const FProperty& Property, const void* ValuePtr, FValue& OutValue, FString& OutError);

// FTransform as {Translation?: {X,Y,Z}, Rotation?: {X,Y,Z,W}, Scale3D?: {X,Y,Z}}: parts equal to identity are
// omitted (an identity transform is {}).
FValue EncodeTransform(const FTransform& Transform);

// Pin / variable values held as RigVM export text, typed through a property of the value's type (one property bag
// per type, cached for the lifetime of the encoder).
class FTextValues
{
public:
	FTextValues();
	~FTextValues();
	FTextValues(const FTextValues&) = delete;
	FTextValues& operator=(const FTextValues&) = delete;

	// Text imported into a value of the type and encoded. False with OutError when the type cannot hold a value or
	// the text does not import.
	bool Encode(const FString& CPPType, UObject* CPPTypeObject, const FString& Text, FValue& OutValue, FString& OutError);
	// True when both texts import to identical values of the type (semantic compare: "1" equals "1.000000"). Texts
	// that do not import compare as strings.
	bool Same(const FString& CPPType, UObject* CPPTypeObject, const FString& A, const FString& B);
	// A document value as export text of the type (the inverse of Encode): Current (the pin's present text) is
	// imported first, so an object value that leaves fields out keeps their present values; a string the typed codec
	// does not take is imported as export text (the read keeps such values as text). False with OutError.
	bool Decode(const FString& CPPType, UObject* CPPTypeObject, const FString& Current, const FValue& Value, FString& OutText,
		FString& OutError);

private:
	struct FSlot;
	FSlot* Find(const FString& CPPType, UObject* CPPTypeObject);
	TMap<FString, TUniquePtr<FSlot>> Slots;
};

// A document value written into a property value (the inverse of EncodeProperty): RigVM's short forms (element keys
// "Bone(spine_01)", key collections, {Translation?, Rotation?, Scale3D?} transforms), structs field by field (fields
// the value omits keep their current value), arrays resized to the value, everything else through the Editor typed
// value codec. False with OutError naming the field; ValuePtr may be partly written then.
bool DecodeProperty(const FProperty& Property, void* ValuePtr, const FValue& Value, FString& OutError);

// The fields of Object written over a struct instance (sparse: omitted fields keep their value); Skip names fields
// that are not authored there (refused as unknown).
bool DecodeStructFields(const UScriptStruct& Struct, void* Instance, const FValue& Object, TFunctionRef<bool(const FProperty&)> Skip,
	FString& OutError);

// {Translation?, Rotation?, Scale3D?} (identity for omitted parts). bFloat rounds every part to float32, the precision
// hierarchy transforms are saved at, so the in-memory value equals the one a reload reads.
bool DecodeTransform(const FValue& Value, FTransform& OutTransform, FString& OutError, bool bFloat = false);

// "Type(Name)" (FRigElementKey::ToString) as a key; "Type()" a key of that type without a name, "None(Name)" a name
// without a type; "" the empty key (FRigElementKey()).
bool ParseElementKey(const FString& Text, FRigElementKey& OutKey);
// The value form ParseElementKey reads.
FString ElementKeyText(const FRigElementKey& Key);

// ---- names (AssetDocumentControlRigValues.cpp) -------------------------------------------------------------------

// Document name of a unit struct (design §3.3): the struct's short name when exactly one loaded FRigVMStruct has it,
// else its path.
FString UnitName(const UScriptStruct& Struct);
// Every loaded FRigVMStruct (units and events) a graph can hold, sorted by path.
TArray<UScriptStruct*> GetUnitStructs();

// A RigVM type from its document name (the inverse of TypeName): the CPP type and its type object ("float" ->
// {"float", null}; "TArray</Script/CoreUObject.Vector>" -> {"TArray<FVector>", Vector}). False for a type path that
// does not load.
bool ParseTypeName(const FString& Name, FString& OutCPPType, UObject*& OutCPPTypeObject);

// Document name of a graph model: "Rig" for the default model, else the model name without the "RigVMModel " prefix
// the editor's "+" gives it (design §3.2 Graphs).
FString GraphName(const URigVMGraph& Model, const URigVMGraph* DefaultModel);

// ---- read (AssetDocumentControlRigRead.cpp / AssetDocumentControlRigGraphRead.cpp) -------------------------------

// A soft reference loaded only when its package exists (a rig copied without its mesh or shape library reads without
// LoadPackage warnings; the read then degrades as the editor does: no imported-bone omission, no shape names).
template <typename T>
T* LoadIfPresent(const TSoftObjectPtr<T>& Pointer)
{
	if (T* Loaded = Pointer.Get())
	{
		return Loaded;
	}
	const FString Package = Pointer.ToSoftObjectPath().GetLongPackageName();
	if (Package.IsEmpty() || !FPackageName::IsValidLongPackageName(Package) || !FPackageName::DoesPackageExist(Package))
	{
		return nullptr;
	}
	return Pointer.LoadSynchronous();
}

// Reference pose of SourceHierarchyImport (the skeleton the editor's Import / Refresh used), by bone name: what an
// imported bone without Transform has (design §3.4).
struct FSourcePose
{
	static constexpr double Tolerance = 1.0e-4;
	bool bValid = false;
	TMap<FName, FTransform> Local;

	explicit FSourcePose(UControlRigBlueprint& Blueprint);
	bool Matches(FName Bone, const FTransform& Transform) const;
};

// Fields of FRigControlSettings owned by other element keys or never authored (design §3.4): limits (Minimum /
// Maximum), spaces (Customization), transient and deprecated fields.
bool IsSkippedControlField(const FProperty& Property);

// "Rig", "Module" or "Modular" (design §3.2 RigType).
FString GetRigType(const UControlRigBlueprint& Blueprint);

// The records of rig.controlrig.read.v1 for one asset (unpaged).
bool BuildRecords(UControlRigBlueprint& Blueprint, bool bInspect, FCall& Call, TArray<FValue>& Records);
// Graph models, the function library and their nodes (AssetDocumentControlRigGraphRead.cpp).
bool BuildGraphRecords(UControlRigBlueprint& Blueprint, bool bInspect, FTextValues& Values, FCall& Call, TArray<FValue>& Records);

bool HandleRead(FCall& Call, FObjectBuilder& Result);
bool HandleCatalog(FCall& Call, FObjectBuilder& Result);

// ---- edit / compile (AssetDocumentControlRigEdit.cpp, AssetDocumentControlRigCompile.cpp) --------------------------

bool HandleEdit(FCall& Call, FObjectBuilder& Result);
bool HandleCompile(FCall& Call, FObjectBuilder& Result);

// URigVMBlueprint's own member-variable change handlers, which it binds to the object-modified and blueprint-changed
// delegates in PostLoad: pre remembers the member variables, post moves variable nodes and pin bindings along with
// the renames, retypes and removals since then.
void RunPreVariableChange(URigVMBlueprint& Blueprint);
void RunPostVariableChange(URigVMBlueprint& Blueprint);

// Points every member-variable node at the member variable it names. A Blueprint duplicate gives its variables new
// guids but keeps its variable nodes' ones; on engines that follow member variables by guid, renames, retypes and
// removals would otherwise pass those nodes by.
void PointVariableNodesAtVariables(URigVMBlueprint& Blueprint);

// The function node's Details "Variables" remapping: maps the inner variable Inner of a function of another rig to the
// host's member variable Outer, or unmaps it (Outer None). Returns false when the node does not remap Inner or the
// controller refuses.
bool SetFunctionVariableMapping(URigVMController& Controller, URigVMFunctionReferenceNode& Reference, FName Inner, FName Outer,
	bool bSetupUndoRedo);

// Graph ops of rig.controlrig.edit.v1 (AssetDocumentControlRigGraphEdit.cpp): models, nodes of every
// graph, links, pin values, bindings, pin types, aggregate pins, traits and function variable maps through
// URigVMController with undo and Python print off.
struct FGraphEditState
{
	explicit FGraphEditState(UControlRigBlueprint& InBlueprint);
	~FGraphEditState();
	FGraphEditState(const FGraphEditState&) = delete;
	FGraphEditState& operator=(const FGraphEditState&) = delete;

	UControlRigBlueprint& Blueprint;
	// Controller errors (ReportError) of the op being run.
	TArray<FString>* Errors = nullptr;
	bool bChanged = false;
	// Warnings for the caller: {op, code, message}.
	TArray<FValue> Notes;
	TUniquePtr<FTextValues> Values;
};
bool IsGraphOp(const FString& Kind);
bool RunGraphOp(FGraphEditState& State, FReader& Op, const FString& Kind, int32 OpIndex);

// The graph an op edits (its "graph" field): a model by document name ("Rig"), or a scope as the read records name it:
// ["Graphs", <model>] or ["Functions", <function>], then ["Nodes", <collapse node>, "Collapse"] per nesting level (a
// function body or a collapse node's graph; aggregate nodes have no editable graph).
struct FGraphTarget
{
	URigVMGraph* Graph = nullptr;
	URigVMController* Controller = nullptr;
	// For messages: the model name or the scope joined with '/'.
	FString GraphName;
};
bool ReadGraphField(FGraphEditState& State, FReader& Op, FGraphTarget& Out);
// The controller of the local function library (its functions' definitions), null when the rig has none.
URigVMController* GetLibraryController(UControlRigBlueprint& Blueprint);
// Fails the op when the controller reported errors during it (with Code unless the message names a known refusal).
bool CheckReported(FGraphEditState& State, FReader& Op, const FString& What, const TCHAR* Code = TEXT("ControlRig.EditRefused"),
	const TCHAR* Field = TEXT(""));

// Controller reporting on for an op (refusals arrive as ReportError), restored after.
class FReportingScope
{
public:
	explicit FReportingScope(URigVMController& InController);
	~FReportingScope();
	FReportingScope(const FReportingScope&) = delete;
	FReportingScope& operator=(const FReportingScope&) = delete;

private:
	URigVMController& Controller;
	bool bPrevious;
};

// Function ops of rig.controlrig.edit.v1 (AssetDocumentControlRigFunctionEdit.cpp): the local function
// library (functions, access, colour), the exposed pins and header of function and collapse graphs, function local
// variables, function reference variable remaps and the dependents report of public functions.
bool IsFunctionOp(const FString& Kind);
bool RunFunctionOp(FGraphEditState& State, FReader& Op, const FString& Kind, int32 OpIndex);

// What the editor's Compile button and its Details callbacks do (design §5.1): auto VM recompile back on, editor
// graphs of a headless-made rig bound to the asset (as opening the editor does, so the compiler can report on
// nodes), RecompileModularRig for modular rigs, the full Blueprint compile, then the hierarchy / draw-container
// propagation to instances. Returns the errors the RigVM compiler reported during the compile as compile messages
// ({Severity, Message, Graph, Node, Path}; KI-10: they are in neither the Blueprint status nor its logs).
TArray<TSharedPtr<FJsonValue>> Finalize(UControlRigBlueprint& Blueprint, bool bBindEditorGraphs = true);

// The Core package-snapshot rebuilder of Control Rigs (design §5.3): what editor Undo does after a restore.
TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeRebuilder();

// ---- modular ops (AssetDocumentControlRigModularEdit.cpp) --------------------------------------------------------

// module.add / remove / rename / reparent / reorder / swap / config / bind, connect, disconnect, modular.settings and
// hierarchy.import ops of rig.controlrig.edit.v1 (design §3.7).
bool IsModularOp(const FString& Kind);
const TArray<FString>* ModularOpFields(const FString& Kind);
bool RunModularOp(UControlRigBlueprint& Blueprint, FReader& Op, const FString& Kind, bool& bChanged, TArray<FValue>& Notes, int32 OpIndex);
// Constructs a modular rig's debugged instance (what the editor's live preview does every tick), so its hierarchy
// holds the elements module construction makes; nothing for other rigs.
void ConstructModularPreview(UControlRigBlueprint& Blueprint);
// Inspect facts of a modular rig: {auto_connect{connector: key}, candidates{connector: [keys]}, unresolved_primary[]}.
FValue AutoConnectFacts(UControlRigBlueprint& Blueprint);
// The Control Rig class a module instantiates: its path, the class when it is loaded, the class loaded on demand.
FSoftObjectPath GetModuleClassPath(const FRigModuleReference& Module);
UClass* FindModuleClass(const FRigModuleReference& Module);
UClass* LoadModuleClass(const FRigModuleReference& Module);

// ---- validate (AssetDocumentControlRigTrial.cpp) ----------------------------------------------------------------

// Validate's trial target of Control Rigs (a transient Duplicate of the rig, or a new transient rig from the factory),
// and the duplicate fixup of Control Rigs (AssetDocumentDuplicateFixups.h: the copy's model graphs post-load as on a
// load from disk), registered for the ControlRigBlueprint class path (unregistering never touches the class).
void RegisterTrialTargetMaker(const FTopLevelAssetPath& ControlRigBlueprintPath);
void UnregisterTrialTargetMaker(const FTopLevelAssetPath& ControlRigBlueprintPath);
// A transient copy of the rig (the trial target's Duplicate: its own generated class, neither Public nor Standalone).
UControlRigBlueprint* MakeTransientCopy(UControlRigBlueprint& Source, FString& OutMessage);

// ---- status (AssetDocumentControlRigStatus.cpp) -----------------------------------------------------------------

// Compile status without compiling (design §5.2): {Status, Messages[{Severity, Message, Graph?, Node?, Path?}],
// VMHash?, Instructions?}, Status from UBlueprint::Status; messages are the compiler's per-node messages and the
// compile log of the last compile. Null for other assets.
TSharedPtr<FJsonObject> DescribeStatus(const UObject& Asset);
// Document path of a model node (/Body/Graphs/<Graph>/Nodes/<Node>, /Body/Functions/<F>/Nodes/<Node>, collapsed graphs
// nested); OutGraph is the top-level graph or function.
FString DocumentNodePath(const UControlRigBlueprint& Blueprint, const URigVMNode& Node, FString& OutGraph);

// Inspect facts of a Control Rig Shape Library (generic route): {DefaultShape, Shapes[{Name, Mesh, Transform?}]}.
// Null for other assets.
TSharedPtr<FJsonObject> DescribeShapeLibrary(const UObject& Asset);

// ---- list_types (AssetDocumentControlRigCatalog.cpp) ------------------------------------------------------------

bool ListNodeTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);
bool ListValueTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);
bool ListModules(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);

// Pins of a unit struct as a new node of it has them (URigVMController's pins for a struct): {Name, Type,
// Direction, Default?, Expand?}, in property order.
TArray<TSharedPtr<FJsonValue>> DescribeUnitPins(UScriptStruct& Struct);

// "Input", "Output", "IO", "Visible", "Hidden" (ERigVMPinDirection).
const TCHAR* DirectionName(uint8 Direction);
} // namespace UE::AssetDocument::Graph::Rig::ControlRig
