// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphPrimitives.h"
#include "Dom/JsonObject.h"

struct FGraphNodeClassData;
class UAIGraph;
class UAIGraphNode;
class UClass;
class UEdGraph;
class UEdGraphNode;
class UEdGraphNode_Comment;
class UEdGraphPin;
class UEnvQuery;
class UEnvQueryOption;
class UObject;

// Environment Query primitives (ai.env_query.*, owner AI), served by the AI provider ("ai") through the
// table-driven provider built here, like ai.state_tree.*. One "asset" handle (an exact UEnvQuery), value DTOs
// with schema_version 1, whole-package footprint, no typed inverse (Core restores the package snapshot; the
// EnvQuery rebuilder notifies the EQS manager). Each edit does what the Environment Query editor does
// (docs/architecture/asset-document-eqs-design.md §3). The editor's graph classes are not exported: they are
// found by path and driven through the exported AIGraph bases (UAIGraph, UAIGraphNode, UAIGraphSchema).
namespace UE::AssetDocument::Graph::EnvQuery
{
inline const TCHAR* ProviderId = TEXT("ai");
inline const TCHAR* PrimitivePrefix = TEXT("ai.env_query.");

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors();
TConstArrayView<FBinding> GetBindings();
TSharedRef<IAssetDocumentPrimitiveProvider> CreateProvider();

bool HandleRead(FCall& Call, FObjectBuilder& Result);
bool HandleEdit(FCall& Call, FObjectBuilder& Result);
bool HandleFinalize(FCall& Call, FObjectBuilder& Result);

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/"));

// ---- the editor's classes (EnvironmentQueryEditor, found by path) -------------------------------------------------

enum class ENodeKind : uint8
{
	Generator,
	Test,
};

// Loads the Environment Query editor module; false when it is not available (plugin disabled).
bool EnsureEditorModule();
UClass* GraphClass();
UClass* OptionNodeClass();
UClass* TestNodeClass();
UClass* RootNodeClass();

UEnvQuery* GetQuery(FCall& Call);
UAIGraph* GetGraph(const UEnvQuery& Query);
// The graph the editor's graph tab creates for a query without one (new graph, Root, OnCreated, Initialize).
UAIGraph* EnsureGraph(UEnvQuery& Query);
UEdGraphNode* FindRoot(const UEdGraph& Graph);
UEdGraphPin* RootOutput(const UEdGraph& Graph);
TArray<UAIGraphNode*> GetOptionNodes(const UEdGraph& Graph);
UEnvQueryOption* GetOption(const UAIGraphNode& OptionNode);
bool IsConnected(const UAIGraphNode& OptionNode);
// The document key of an option / test node: its graph node's name once a document key was given to it, otherwise its
// instance's object name. The editor's Undo / Redo renames instances (UAIGraphNode::PostEditUndo -> ResetNodeOwner ->
// Rename(nullptr)) but restores graph node names, so a given key survives them.
bool HasKeyName(const UEdGraphNode& Node);
FString NodeKey(const UAIGraphNode& Node);
// The key of Option (its graph node's, or its object name when the query has no graph).
FString OptionKey(const UEnvQuery& Query, const UEnvQueryOption& Option);
UAIGraphNode* FindOptionNode(const UEdGraph& Graph, FStringView Name);
UAIGraphNode* FindTestNode(const UEdGraph& Graph, FStringView Name);
UEdGraphNode_Comment* FindComment(const UEdGraph& Graph, FStringView Name);
bool IsTestEnabled(const UAIGraphNode& TestNode);
float GetTestWeight(const UAIGraphNode& TestNode);
bool IsOpenInEditor(const UObject& Asset);

// ---- classes: the palette (generators) and the Add Test menu (tests) ----------------------------------------------

struct FPaletteClass
{
	UClass* Class = nullptr;
	FString Label;
	FString DisplayName;
	FString Category;
	FString Tooltip;
	FString Deprecated;
	bool bBlueprint = false;
};
TArray<FPaletteClass> GetPalette(ENodeKind Kind);
// The palette / Add Test class data (FGraphNodeClassData) of Class; false when the menu does not offer it. Built as
// FEnvironmentQueryEditorModule's class cache (which exists only once an Environment Query editor has opened):
// FGraphNodeClassHelper over UEnvQueryNode, observing Blueprint generators.
bool FindClassData(const UClass& Class, ENodeKind Kind, FGraphNodeClassData& OutData);
void ReleaseClassCache();
// Engine generators / tests by short name ("SimpleGrid", "Distance"), others by class path.
FString ClassLabel(const UClass& Class, ENodeKind Kind);
// A palette class by label or path; null with up to five close candidates otherwise.
UClass* ResolveClass(const FString& Text, ENodeKind Kind, TArray<FString>& OutCandidates);

// ---- values ----------------------------------------------------------------------------------------------------

// The object a new palette node of Class gets (NewObject + UpdateNodeVersion), in the transient package.
UObject* NewDefaultsObject(UClass& Class);
// {name: value} of the Details values of Object: every admitted field (template semantics), sparse over
// Reference when given (fields with the same document value left out). Codec failures go to OutUnsupported.
// FEnvTraceData values (ProjectionData, TraceData) carry only what FEnvTraceDataCustomization shows.
FValue EncodeProperties(UObject& Object, UObject* Reference, TArray<FString>& OutUnsupported);
// A member of an FEnvTraceData the Details panel does not show (VersionNum, SerializedChannel, bCan*) in Value
// (written to Owner's Property); OutPath points at it inside Value.
bool FindHiddenMember(UObject& Owner, const FString& Property, const FValue& Value, FString& OutPath);
// FEnvTraceDataCustomization::OnTraceChannelChanged for every trace data of Object and its instanced objects whose
// TraceChannel was changed: SerializedChannel follows (the channel a loaded node keeps).
void SyncTraceChannels(UObject& Object);
// The comment box Details the document carries (CommentColor, FontSize, ...).
FValue EncodeCommentProperties(UEdGraphNode_Comment& Comment);
extern const TCHAR* const CommentPropertyNames[];
extern const int32 NumCommentPropertyNames;

// ---- validation ------------------------------------------------------------------------------------------------

struct FMessage
{
	FString Severity; // error, warning, info
	FString Message;
	FString Option;
	FString Test;
};
void Validate(const UEnvQuery& Query, TArray<FMessage>& OutMessages);
FValue MessagesValue(TConstArrayView<FMessage> Messages);
// The query's named params (CollectQueryParams): [{name, type}].
FValue ParamsValue(const UEnvQuery& Query);

TSharedPtr<FJsonObject> DescribeCompileStatus(const UObject& Asset);
} // namespace UE::AssetDocument::Graph::EnvQuery

// The EnvQuery derived-state rebuilder, compile status describer and list_types kinds env_query_generator /
// env_query_test; called by the AssetDocumentAI module at startup.
void RegisterAssetDocumentEnvQueryHooks();
void UnregisterAssetDocumentEnvQueryHooks();
