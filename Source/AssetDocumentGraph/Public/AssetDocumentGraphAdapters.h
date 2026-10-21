// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

// Public adapter surface of the generic graph provider (provider id "graph").
// Contract: docs/architecture/asset-document-graph-primitives.md §5.
//
// Schema adapters are matched by UEdGraphSchema class (deepest registered
// ancestor wins); host adapters are matched by asset class (deepest wins).
// Modules register their adapters in StartupModule and unregister them in
// ShutdownModule. Adapters never write rollback code: Core restores the whole
// package snapshot.

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

class FProperty;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphSchema;
class UObject;

// A stable, AI-readable failure. Code is one of the Graph.* codes of the
// contract (§7); Context carries machine-readable extras (suggestions, reason).
struct ASSETDOCUMENTGRAPH_API FAssetDocumentGraphError
{
	FString Code;
	FString Message;
	FString Path;
	TArray<FAssetDocumentRuntimeDiagnosticContext> Context;

	void Set(FStringView InCode, FStringView InMessage, FStringView InPath = TEXTVIEW("/"));
	void AddContext(FStringView Key, FAssetDocumentRuntimeValueDto Value);
	void AddContextString(FStringView Key, FStringView Value);
};

struct FAssetDocumentGraphActionRecord
{
	FString ActionKey;
	FString DisplayName;
	FString Category;
	FString Tooltip;
	FString NodeClassPath;
	bool bRequiresContext = false;
	// false only for fallback keys that are not guaranteed to survive an editor restart.
	bool bStable = true;
};

struct FAssetDocumentGraphActionQuery
{
	UObject* Asset = nullptr;
	UEdGraph* Graph = nullptr;
	UEdGraphPin* FromPin = nullptr;
	UEdGraphNode* ParentNode = nullptr;
	// Optional pre-filters; adapters may ignore them (the provider filters again).
	FString Text;
	FString KeyPrefix;
	// When set, the caller only needs these exact keys (fast path allowed).
	TArray<FString> ExactKeys;
	// When true the adapter must fill DisplayName/Category/Tooltip.
	bool bWantUiText = true;
	// Classes whose members the actions may target besides the graph's own class, as the editor's menu off a
	// pin of such a class lists them (its interfaces' functions included).
	TArray<UClass*> TargetClasses;
	// Also the function calls the editor's menu lists with Context Sensitive off (functions of other classes,
	// called on an object linked into their Target pin), with bRequiresContext set; when Text is set, only those
	// whose key or display name holds each of its words. Adapters without such a mode ignore it.
	bool bContextFreeCalls = false;
};

struct FAssetDocumentGraphSpawnRequest
{
	UObject* Asset = nullptr;
	UEdGraph* Graph = nullptr;
	FString ActionKey;
	TOptional<FVector2D> Position;
	UEdGraphPin* FromPin = nullptr;
	UEdGraphNode* ParentNode = nullptr;
};

struct FAssetDocumentGraphSpawnResult
{
	UEdGraphNode* Node = nullptr;
	// true when an existing node (e.g. a ghost or already placed event) was reused.
	bool bReused = false;
};

struct FAssetDocumentGraphNodeDescription
{
	// Empty when the key cannot be derived.
	FString ActionKey;
	// "entry" / "result" / "root" / "sm_entry"...; empty = none.
	FString ImplicitRole;
	// Object value (schema-specific facts); Kind Null/empty = {}.
	FAssetDocumentRuntimeValueDto Extras;
	TOptional<int32> SubnodeIndex;
	UEdGraphNode* ParentNode = nullptr;
};

struct FAssetDocumentGraphUserPin
{
	FName Name;
	EEdGraphPinDirection Direction = EGPD_Input;
	FEdGraphPinType Type;
	TOptional<FString> DefaultValue;
};

// The node the editor inserts when a dragged link needs a conversion
// (CanCreateConnection answers CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE;
// UEdGraphSchema_K2::CreateAutomaticConversionNodeAndConnections).
struct FAssetDocumentGraphConversion
{
	// The node's action key; empty when the node has none.
	FString ActionKey;
	FString NodeClassPath;
	// The conversion node's pins the link goes through; NAME_None when not known.
	FName InputPin;
	FName OutputPin;
};

class ASSETDOCUMENTGRAPH_API IAssetDocumentGraphSchemaAdapter
{
public:
	virtual ~IAssetDocumentGraphSchemaAdapter() = default;

	// Schema class this adapter handles (and its subclasses).
	virtual UClass* GetSchemaClass() const = 0;
	// Short tag used as action-key prefix and GraphSnapshot.schema_tag ("K2", "BT", ...).
	virtual FString Tag(const UEdGraph& Graph) const = 0;

	virtual bool EnumerateActions(
		const FAssetDocumentGraphActionQuery& Query,
		TArray<FAssetDocumentGraphActionRecord>& OutActions,
		FAssetDocumentGraphError& OutError) const = 0;

	// Resolves exactly one action for the canonical key and spawns it. Unknown
	// keys return Graph.ActionNotFound (the provider adds suggestions).
	virtual bool ResolveAndSpawn(
		const FAssetDocumentGraphSpawnRequest& Request,
		FAssetDocumentGraphSpawnResult& OutResult,
		FAssetDocumentGraphError& OutError) const = 0;

	virtual void DescribeNode(
		const UEdGraphNode& Node,
		FAssetDocumentGraphNodeDescription& OutDescription) const = 0;

	// "node" -> the node; other containers (e.g. "template") are adapter-specific.
	virtual UObject* ResolvePropertyContainer(UEdGraphNode& Node, FStringView Container) const;

	virtual bool RemoveNode(
		UObject& Asset,
		UEdGraphNode& Node,
		FAssetDocumentGraphError& OutError) const = 0;

	// Role of a graph owned by a node (composite, state machine...). Default "other".
	virtual FString ClassifySubgraph(const UEdGraph& Subgraph, const UEdGraphNode& OwnerNode) const;
	// Graphs graph.list reports as owned by Node. Default: Node.GetSubGraphs(). An adapter may
	// report graphs an editor-internal container graph holds instead of the container itself
	// (the sample graphs of a blend space node); their path then also names every graph
	// between the owner node and the reported graph.
	virtual void GetOwnedSubgraphs(const UEdGraphNode& Node, TArray<UEdGraph*>& OutGraphs) const;

	virtual bool SetUserPins(
		UEdGraphNode& Node,
		TConstArrayView<FAssetDocumentGraphUserPin> Pins,
		FAssetDocumentGraphError& OutError) const;
	virtual bool CanAddDynamicPin(const UEdGraphNode& Node) const;
	virtual bool AddDynamicPin(UEdGraphNode& Node, FAssetDocumentGraphError& OutError) const;
	virtual bool RemoveDynamicPin(
		UEdGraphNode& Node,
		UEdGraphPin* Pin,
		FAssetDocumentGraphError& OutError) const;
	// graph.pin.type.set.v1: the type the user picks on the pin itself (K2: the Select node's
	// Index type selector). Pins listed in DescribeNode extras "typed_pins". The pin may be
	// destroyed (node reconstruction). Default: Graph.PinTypeNotEditable.
	virtual bool SetPinType(
		UEdGraphNode& Node,
		UEdGraphPin& Pin,
		const FEdGraphPinType& Type,
		FAssetDocumentGraphError& OutError) const;

	// graph.pin.connect.v1 makes the link as the editor's drag-drop does (UEdGraphSchema::TryCreateConnection).
	// KeepTypesA / KeepTypesB (optional): the pin types the caller states for that end's node. A node whose
	// pin types each connection re-evaluates takes them back wherever its links fit them (K2: a promotable
	// operator after its Pin Conversions). Default: TryCreateConnection.
	virtual bool ConnectPins(
		UEdGraphPin& A,
		UEdGraphPin& B,
		const TMap<FName, FEdGraphPinType>* KeepTypesA,
		const TMap<FName, FEdGraphPinType>* KeepTypesB) const;

	// The conversion node the editor would insert between Output and Input (a link the schema makes only
	// through one). Default: false (not known).
	virtual bool DescribeConversion(
		const UEdGraphPin& Output,
		const UEdGraphPin& Input,
		FAssetDocumentGraphConversion& OutConversion) const;

	// graph.link.check.v1: the palette's template node of each key (K2: UBlueprintNodeSpawner::GetTemplateNode),
	// which is in no graph of the asset. Keys without exactly one action are left out. Default: none.
	virtual void GetTemplateNodes(
		UObject& Asset,
		UEdGraph& Graph,
		TConstArrayView<FString> Keys,
		TMap<FString, UEdGraphNode*>& OutNodes) const;

	// graph.link.check.v1 describe: false (with the error ResolveAndSpawn would report, e.g. Graph.ActionNotFound /
	// Graph.ActionAmbiguous) when spawning Key in Graph could not find exactly one action; nothing is spawned.
	// Default: true (not known before spawning).
	virtual bool CheckActionKey(
		UObject& Asset,
		UEdGraph& Graph,
		const FString& Key,
		FAssetDocumentGraphError& OutError) const;

	// graph.function.flags.set.v1: Flags is a partial object of
	// {pure, const, call_in_editor, access, category, tooltip, keywords};
	// OutFlags receives the complete resulting flags object.
	virtual bool SetFunctionFlags(
		UObject& Asset,
		UEdGraph& Graph,
		const FAssetDocumentRuntimeValueDto& Flags,
		FAssetDocumentRuntimeValueDto& OutFlags,
		FAssetDocumentGraphError& OutError) const;

	// graph.node.property.set.v1 (container "node") asks the adapter first. A node
	// setting that is not a plain reflected property (e.g. the states a state alias
	// points at) is applied here with bOutHandled = true; leaving bOutHandled false
	// falls back to the reflected property path. Default: not handled.
	virtual bool SetNodeSetting(
		UObject& Asset,
		UEdGraphNode& Node,
		FStringView Path,
		const FAssetDocumentRuntimeValueDto& Value,
		bool& bOutHandled,
		FAssetDocumentGraphError& OutError) const;

	// graph.node.property.set.v1 (container "node"), before SetNodeSetting: a setting that the node spells another way
	// (a property other engine versions name or shape differently) is rewritten in place to the node's own path and
	// value. False (with OutError) when the value has no equivalent on this node. Default: unchanged.
	virtual bool MapNodeSetting(
		const UEdGraphNode& Node,
		FString& InOutPath,
		FAssetDocumentRuntimeValueDto& InOutValue,
		FAssetDocumentGraphError& OutError) const;

	// graph.pin.default.set.v1 (kinds value / text / object): why the value cannot be the pin's default, "" when it
	// can. Default: the schema's IsPinDefaultValid. A schema that does not implement it (Niagara: its pin widgets write
	// the default without asking) needs an adapter override.
	virtual FString ValidatePinDefault(
		const UEdGraphPin& Pin,
		const FString& Value,
		UObject* Object,
		const FText& Text) const;

	// A pin address whose name no pin of Node has (in that direction): the pin the adapter reports under that name
	// (DescribeNode extras say which), e.g. Niagara's Map Get default value pins, which have no name of their own and
	// are reported as their output pin's name (direction input). Default: none.
	virtual UEdGraphPin* ResolvePinAlias(
		UEdGraphNode& Node,
		FName Name,
		EEdGraphPinDirection Direction) const;

	// graph.node.rename.v1 asks the adapter first: a schema whose node identity is not the node's editable title
	// (SoundCue: the sound node object name, renamed to the document key) renames it here with bOutHandled = true;
	// leaving bOutHandled false falls back to the editor's title rename (GetCanRenameNode / OnRenameNode).
	// Default: not handled.
	virtual bool RenameNode(
		UObject& Asset,
		UEdGraphNode& Node,
		const FString& Name,
		bool& bOutHandled,
		FAssetDocumentGraphError& OutError) const;
};

struct FAssetDocumentGraphInfo
{
	UEdGraph* Graph = nullptr;
	FString Path;
	FString Role;
	UEdGraph* ParentGraph = nullptr;
	UEdGraphNode* OwnerNode = nullptr;
	bool bCanSpawn = true;
	bool bCanRename = false;
	bool bCanRemove = false;
};

struct FAssetDocumentGraphFinalizeMessage
{
	FString Severity; // "error" | "warning" | "info"
	FString Message;
	FGuid NodeGuid;
	FString PinName;
	bool bOrphanedPin = false; // The message's pin is an orphan kept by node reconstruction (UEdGraphPin::bOrphanedPin).
	FString Identifier; // The compiler's message identifier (FTokenizedMessage::GetIdentifier), e.g. PrunedExecInUse; may be empty.
};

struct FAssetDocumentGraphFinalizeResult
{
	FString Status = TEXT("ok"); // "ok" | "warnings" | "errors"
	TArray<FAssetDocumentGraphFinalizeMessage> Messages;
};

class ASSETDOCUMENTGRAPH_API IAssetDocumentGraphHostAdapter
{
public:
	virtual ~IAssetDocumentGraphHostAdapter() = default;

	virtual UClass* GetAssetClass() const = 0;

	// Top-level graphs only; the provider appends node-owned subgraphs.
	virtual void EnumerateGraphs(UObject& Asset, TArray<FAssetDocumentGraphInfo>& OutGraphs) const = 0;

	virtual bool CreateGraph(
		UObject& Asset,
		FStringView Role,
		FStringView Name,
		UEdGraph* ParentGraph,
		UEdGraph*& OutGraph,
		FAssetDocumentGraphError& OutError) const;
	virtual bool RemoveGraph(UObject& Asset, UEdGraph& Graph, FAssetDocumentGraphError& OutError) const;
	virtual bool RenameGraph(
		UObject& Asset,
		UEdGraph& Graph,
		FStringView NewName,
		FAssetDocumentGraphError& OutError) const;

	// Rebuilds derived state (Blueprint compile, BT UpdateAsset...). Also the
	// Core derived-state rebuild hook once Core exposes one.
	virtual bool Finalize(
		UObject& Asset,
		FAssetDocumentGraphFinalizeResult& OutResult,
		FAssetDocumentGraphError& OutError) const = 0;

	// Called after any graph edit. bStructural = nodes/graphs/pins added or removed.
	virtual void NotifyGraphChanged(UObject& Asset, UEdGraph* Graph, bool bStructural) const;
};

// Class-specific Blueprint settings that are not editable reflected properties
// (e.g. an Animation Blueprint's preview mesh, set through UAnimBlueprint::SetPreviewMesh).
// blueprint.member.query.v1 reports QuerySettings as "settings";
// blueprint.member.setting.set.v1 {name, value} calls SetSetting. Matched by
// Blueprint class (deepest registered ancestor wins).
class ASSETDOCUMENTGRAPH_API IAssetDocumentBlueprintSettingsAdapter
{
public:
	virtual ~IAssetDocumentBlueprintSettingsAdapter() = default;

	virtual UClass* GetBlueprintClass() const = 0;
	// Object value: setting name -> current value.
	virtual FAssetDocumentRuntimeValueDto QuerySettings(UBlueprint& Blueprint) const = 0;
	virtual bool SetSetting(
		UBlueprint& Blueprint,
		FStringView Name,
		const FAssetDocumentRuntimeValueDto& Value,
		FAssetDocumentGraphError& OutError) const = 0;
};

// Blueprint classes whose package is restored by the graph module's derived-state
// rebuilder (reconstruct nodes, recompile, restore class defaults) in addition to the
// exact UBlueprint (e.g. UWidgetBlueprint, registered by AssetDocumentWidget). Core
// restores the authored objects of the package from its snapshot; the rebuilder
// regenerates the derived ones. Matching is by Blueprint class (IsA).
class ASSETDOCUMENTGRAPH_API IAssetDocumentBlueprintRebuildAdapter
{
public:
	virtual ~IAssetDocumentBlueprintRebuildAdapter() = default;

	virtual UClass* GetBlueprintClass() const = 0;
	// True for a package object that a compile of this Blueprint class regenerates from
	// authored state (so it must stay out of the snapshot's authored set), e.g. the
	// widget-tree archetype under a widget generated class. Asked for every object of a
	// restored Blueprint package, whatever the Blueprint class: answer true only for
	// objects that belong to GetBlueprintClass. Transient objects and the engine's
	// default derived classes (UStruct, CDOs) are always derived.
	virtual bool IsDerivedObject(const UObject& Object) const = 0;
};

// Object properties of a Blueprint's generated class that get component-style bound
// events (the Details panel's "On Clicked [+]"): the action database registers
// bound-event spawners for UActorComponent delegates only, so a Blueprint class whose
// member objects are not components (Widget Blueprints: UWidget) registers its
// object class here and the K2 adapter enumerates and spawns the same
// UBlueprintBoundEventNodeSpawner for each assignable delegate of those objects.
class ASSETDOCUMENTGRAPH_API IAssetDocumentBoundEventSourceAdapter
{
public:
	virtual ~IAssetDocumentBoundEventSourceAdapter() = default;

	// Blueprint classes (and subclasses) the source applies to.
	virtual UClass* GetBlueprintClass() const = 0;
	// Object property class (and subclasses) whose BlueprintAssignable delegates are bindable.
	virtual UClass* GetBoundObjectClass() const = 0;
};

ASSETDOCUMENTGRAPH_API void RegisterAssetDocumentBlueprintRebuildAdapter(
	TSharedRef<IAssetDocumentBlueprintRebuildAdapter> Adapter);
ASSETDOCUMENTGRAPH_API void UnregisterAssetDocumentBlueprintRebuildAdapter(
	const TSharedRef<IAssetDocumentBlueprintRebuildAdapter>& Adapter);
ASSETDOCUMENTGRAPH_API void RegisterAssetDocumentBoundEventSourceAdapter(
	TSharedRef<IAssetDocumentBoundEventSourceAdapter> Adapter);
ASSETDOCUMENTGRAPH_API void UnregisterAssetDocumentBoundEventSourceAdapter(
	const TSharedRef<IAssetDocumentBoundEventSourceAdapter>& Adapter);

// Registered adapters whose Blueprint class the given Blueprint is (IsA).
ASSETDOCUMENTGRAPH_API TArray<TSharedRef<IAssetDocumentBlueprintRebuildAdapter>>
FindAssetDocumentBlueprintRebuildAdapters(const UBlueprint* Blueprint);
ASSETDOCUMENTGRAPH_API TArray<TSharedRef<IAssetDocumentBlueprintRebuildAdapter>>
GetAssetDocumentBlueprintRebuildAdapters();
ASSETDOCUMENTGRAPH_API TArray<TSharedRef<IAssetDocumentBoundEventSourceAdapter>>
FindAssetDocumentBoundEventSourceAdapters(const UBlueprint* Blueprint);

ASSETDOCUMENTGRAPH_API void RegisterAssetDocumentGraphSchemaAdapter(
	TSharedRef<IAssetDocumentGraphSchemaAdapter> Adapter);
ASSETDOCUMENTGRAPH_API void UnregisterAssetDocumentGraphSchemaAdapter(
	const TSharedRef<IAssetDocumentGraphSchemaAdapter>& Adapter);
ASSETDOCUMENTGRAPH_API void RegisterAssetDocumentGraphHostAdapter(
	TSharedRef<IAssetDocumentGraphHostAdapter> Adapter);
ASSETDOCUMENTGRAPH_API void UnregisterAssetDocumentGraphHostAdapter(
	const TSharedRef<IAssetDocumentGraphHostAdapter>& Adapter);

ASSETDOCUMENTGRAPH_API void RegisterAssetDocumentBlueprintSettingsAdapter(
	TSharedRef<IAssetDocumentBlueprintSettingsAdapter> Adapter);
ASSETDOCUMENTGRAPH_API void UnregisterAssetDocumentBlueprintSettingsAdapter(
	const TSharedRef<IAssetDocumentBlueprintSettingsAdapter>& Adapter);

ASSETDOCUMENTGRAPH_API TSharedPtr<IAssetDocumentGraphSchemaAdapter>
FindAssetDocumentGraphSchemaAdapter(const UEdGraphSchema* Schema);
ASSETDOCUMENTGRAPH_API TSharedPtr<IAssetDocumentGraphHostAdapter>
FindAssetDocumentGraphHostAdapter(const UObject* Asset);

ASSETDOCUMENTGRAPH_API TSharedPtr<IAssetDocumentBlueprintSettingsAdapter>
FindAssetDocumentBlueprintSettingsAdapter(const UBlueprint* Blueprint);

// Canonical GUID text: 32 upper-case hex digits, no hyphens.
ASSETDOCUMENTGRAPH_API FString FormatAssetDocumentGraphGuid(const FGuid& Guid);

// The typed JSON property codec of graph.node.property.* (contract §5), for adapters
// that report or apply reflected values themselves.
ASSETDOCUMENTGRAPH_API FAssetDocumentRuntimeValueDto EncodeAssetDocumentGraphPropertyValue(
	const FProperty& Property, const void* ValuePtr);
ASSETDOCUMENTGRAPH_API bool DecodeAssetDocumentGraphPropertyValue(
	const FProperty& Property, void* ValuePtr, const FAssetDocumentRuntimeValueDto& Value, FString& OutError);
