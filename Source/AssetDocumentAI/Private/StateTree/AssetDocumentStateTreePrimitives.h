// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphPrimitives.h"

class FProperty;
class UObject;
class UStateTree;
class UStateTreeEditorData;
class UStateTreeState;
class UStruct;
class UScriptStruct;
struct FStateTreeEditorNode;
struct FStateTreeTransition;

// StateTree primitives (ai.state_tree.*, owner AI). The AI provider ("ai") serves them through the
// table-driven provider built here (CreateProvider), so they share the AI owner / provider prefix
// rule. Same conventions as the data and graph table providers: one "asset" handle (an exact UStateTree), value DTOs with
// schema_version 1, whole-package footprint, no typed inverse (Core restores the package snapshot
// on rollback, the StateTree rebuilder relinks the compiled tree). Each edit does what the
// StateTree editor does for the same change (state view model, Details panel of a state / node /
// transition, binding menu, compile button). DTOs: AssetDocumentStateTreeCatalog.cpp.
namespace UE::AssetDocument::Graph::StateTree
{
inline const TCHAR* ProviderId = TEXT("ai");
inline const TCHAR* PrimitivePrefix = TEXT("ai.state_tree.");

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors();
TConstArrayView<FBinding> GetBindings();
// The table-driven provider the AI provider delegates ai.state_tree.* invocations to.
TSharedRef<IAssetDocumentPrimitiveProvider> CreateProvider();

// ---- shared by the StateTree primitive files -----------------------------------------------------

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/"));

// The tree and its editor data; fails StateTree.UnsupportedAsset / StateTree.MissingEditorData.
bool GetTree(FCall& Call, UStateTree*& OutTree, UStateTreeEditorData*& OutEditorData);

FString GuidString(const FGuid& Guid);
FString PathOf(const UObject* Object);
// Shortest decimal that reads back as the same float32.
double ShortestFloat(float Value);

// Enum <-> name through reflection (the names the editor's enum pickers show, e.g. "TryEnterState").
FString EnumToName(const UEnum& Enum, int64 Value);
bool ReadEnum(FReader& Reader, FStringView Key, const UEnum& Enum, int64& OutValue, bool bRequired = true);

// The node lists a document addresses.
enum class ENodeList : uint8
{
	Evaluators,
	GlobalTasks,
	EnterConditions,
	Tasks,
	SingleTask,
	Considerations,
	TransitionConditions,
};
const TCHAR* NodeListName(ENodeList List);
bool ParseNodeList(FStringView Name, ENodeList& OutList);
// The FStateTreeNodeBase family struct / Blueprint base class a list accepts (Details meta BaseStruct / BaseClass).
const UScriptStruct* GetNodeBaseStruct(ENodeList List);
const UClass* GetNodeBaseClass(ENodeList List);

// Owner object (Outer of Blueprint node instances, the object Details edits) and the array of a list;
// null array for a missing state / transition.
struct FNodeListRef
{
	ENodeList List = ENodeList::Tasks;
	UObject* Owner = nullptr;
	UStateTreeState* State = nullptr;
	FStateTreeTransition* Transition = nullptr;
	TArray<FStateTreeEditorNode>* Nodes = nullptr;
	FStateTreeEditorNode* Single = nullptr;
	FName MemberName;
};
bool ResolveNodeList(UStateTreeEditorData& EditorData, ENodeList List, const FGuid& StateId, const FGuid& TransitionId, FNodeListRef& Out);

UStateTreeState* FindState(const UStateTreeEditorData& EditorData, const FGuid& StateId);
FStateTreeTransition* FindTransition(UStateTreeState& State, const FGuid& TransitionId);

// The Details panel's edit of one member property of Owner: Modify, PreEditChange(member chain),
// Write, PostEditChangeChainProperty(ValueSet). The owner's own change handlers run (state type
// changes reset tasks / links, schema changes broadcast, parameter edits update linked states).
void EditMember(UObject& Owner, FName MemberName, TFunctionRef<void()> Write);

// ---- reflected values (the generic route's typed value codec) -------------------------------------

// Which properties a document authors on a node struct, an instance data struct or a Blueprint
// node instance: editable, not const / transient / deprecated, not FStateTreeNodeBase::Name, not
// Output / Context usage (outputs are written by the node, context data is bound by the compiler).
enum class EValueOwner : uint8
{
	NodeStruct,
	InstanceStruct,
	InstanceObject,
	Schema,
	PropertyBag,
};
bool IsAuthoredProperty(const FProperty& Property, EValueOwner Owner);

// {name: value} of every authored, codec-supported property; names equal to the type's default
// in OutDefaults, codec-unsupported names in OutUnsupported.
FValue EncodeValues(const UStruct& Type, const void* Memory, const void* DefaultMemory, EValueOwner Owner,
	TArray<FString>& OutDefaults, TArray<FString>& OutUnsupported);
// {name: default value} of the authored, codec-supported properties whose value differs from the default (what
// an omitted value is reset to).
FValue EncodeChangedDefaults(const UStruct& Type, const void* Memory, const void* DefaultMemory, EValueOwner Owner);
// Decodes Values ({name: value}, partial: omitted names keep their value) into Memory; Reader is
// positioned at the object holding the values field. Fails StateTree.UnknownProperty /
// StateTree.UnsupportedProperty / StateTree.InvalidValue with the property's path.
bool DecodeValues(FReader& Reader, FStringView Key, const UStruct& Type, void* Memory, UObject* Owner, EValueOwner ValueOwner,
	TArray<FProperty*>* OutWritten = nullptr);

// Struct / class by object path, or by short name among FStateTreeNodeBase structs / UObject classes
// ("StateTreeDelayTask", "FStateTreeDelayTask").
const UStruct* ResolveType(const FString& Text, const UScriptStruct* BaseStruct, const UClass* BaseClass);

// ---- handlers ---------------------------------------------------------------------------------------

bool HandleRead(FCall& Call, FObjectBuilder& Result);
bool HandleEdit(FCall& Call, FObjectBuilder& Result);
bool HandleCompile(FCall& Call, FObjectBuilder& Result);

// Read records (AssetDocumentStateTreeRead.cpp).
void AppendNodeRecord(TArray<FValue>& Out, const FStateTreeEditorNode& Node, ENodeList List, int32 Index,
	const UStateTreeState* State, const FStateTreeTransition* Transition);

// Node list edit (AssetDocumentStateTreeNodes.cpp): replaces a list as the Details panel's node
// picker / array editor does; a node whose id and type match keeps its memory.
bool EditNodes(FCall& Call, FReader& Op, UStateTreeEditorData& EditorData);
// Creates the node of Type (struct or Blueprint class) with ID, as the node picker does.
ASSETDOCUMENTAI_API bool InitializeNode(FStateTreeEditorNode& Node, const UStruct& Type, const FGuid& Id, UObject& Outer);
// Node fields the editor exposes outside the instance data: Name, Enabled (tasks),
// ConsideredForCompletion (tasks), EvaluationMode (conditions), indent / operand.
FString GetNodeTypePath(const FStateTreeEditorNode& Node);
bool IsTaskEnabled(const FStateTreeEditorNode& Node, bool& bOutEnabled);
bool IsTaskConsideredForCompletion(const FStateTreeEditorNode& Node, bool& bOutConsidered, bool& bOutEditable);
bool GetConditionEvaluationMode(const FStateTreeEditorNode& Node, int64& OutMode);

// Bindings (AssetDocumentStateTreeBindings.cpp).
void AppendBindingRecords(TArray<FValue>& Out, const UStateTreeEditorData& EditorData);
bool EditBindings(FCall& Call, FReader& Op, UStateTreeEditorData& EditorData);

// Parameters (AssetDocumentStateTreeEdit.cpp).
FValue MakeParametersRecord(const UStateTreeEditorData& EditorData, const UStateTreeState* State);
} // namespace UE::AssetDocument::Graph::StateTree

// Registers the StateTree derived-state rebuilder (snapshot rollback recompiles the restored tree),
// the StateTree compile status describer and the type discovery hooks; called by the AssetDocumentAI
// module at startup.
void RegisterAssetDocumentStateTreeEditorHooks();
void UnregisterAssetDocumentStateTreeEditorHooks();
// Node struct inspect and list_types schema filter (AssetDocumentStateTreeDiscovery.cpp).
void RegisterAssetDocumentStateTreeDiscoveryHooks();
void UnregisterAssetDocumentStateTreeDiscoveryHooks();
