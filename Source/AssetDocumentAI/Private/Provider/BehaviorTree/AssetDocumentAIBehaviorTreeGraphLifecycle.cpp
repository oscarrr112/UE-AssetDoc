// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeGraphBundleInternal.h"

#include "Provider/AssetDocumentAIProviderContracts.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeFactory.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "UObject/UObjectGlobals.h"

namespace AIBehaviorTreeGraphLifecycleLocal
{
constexpr TCHAR ScratchRollbackComponent[] =
	TEXT("assetdocument.ai.behavior_tree.scratch_bundle.v1");

bool IsScratchTreeValid(const UBehaviorTree* Tree)
{
	return Tree != nullptr && Tree->GetClass() == UBehaviorTree::StaticClass()
		&& Tree->GetOuter() == GetTransientPackage()
		&& Tree->HasAllFlags(RF_Transient | RF_Transactional)
		&& !Tree->HasAnyFlags(RF_Public | RF_Standalone) && !Tree->IsAsset()
		&& Tree->BTGraph == nullptr;
}

bool IsRequestedCreateValue(const FAssetDocumentRuntimeValueDto& Value,
	const FAssetDocumentRuntimeValueDto*& TreePath,
	const FAssetDocumentRuntimeValueDto*& GraphName)
{
	if (!IsAssetDocumentAIBehaviorTreeScratchContext()
		|| !IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| !HasAssetDocumentAIBehaviorTreeExactFields(Value,
			{TEXT("behavior_tree_path"), TEXT("graph_name")})) return false;
	TreePath = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("behavior_tree_path"));
	GraphName = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("graph_name"));
	return IsAssetDocumentAIBehaviorTreeCanonicalObjectPath(TreePath)
		&& GraphName != nullptr
		&& GraphName->Kind == EAssetDocumentRuntimeValueKind::String
		&& !GraphName->StringValue.IsEmpty();
}

bool CreateScratchGraph(
	FAssetDocumentAIBehaviorTreeScratchBundleState& State, const FString& GraphName)
{
	UBehaviorTreeFactory* Factory = UBehaviorTreeFactory::StaticClass()
		->GetDefaultObject<UBehaviorTreeFactory>();
	if (Factory == nullptr) return false;
	const FName ScratchName = MakeUniqueObjectName(GetTransientPackage(),
		UBehaviorTree::StaticClass(), TEXT("AssetDocumentAIScratchBehaviorTree"));
	State.Tree.Reset(Cast<UBehaviorTree>(Factory->FactoryCreateNew(
		UBehaviorTree::StaticClass(), GetTransientPackage(), ScratchName,
		RF_Transient | RF_Transactional, nullptr, nullptr)));
	if (!IsScratchTreeValid(State.Tree.Get())) return false;
	State.Graph.Reset(Cast<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(
		State.Tree.Get(), FName(*GraphName), UBehaviorTreeGraph::StaticClass(),
		UEdGraphSchema_BehaviorTree::StaticClass())));
	UBehaviorTreeGraph* Graph = State.Graph.Get();
	if (Graph == nullptr || Graph->GetClass() != UBehaviorTreeGraph::StaticClass()
		|| Graph->GetOuter() != State.Tree.Get()) return false;
	Graph->SetFlags(RF_Transient);
	// The current graph version, as the Behavior Tree editor's new graph has (UAIGraph::OnCreated -> MarkVersion);
	// otherwise opening the installed graph in the editor upgrades it (UpdateVersion) and dirties the package.
	Graph->MarkVersion();
	return Graph->GraphGuid.IsValid() && Graph->HasAnyFlags(RF_Transient) && !Graph->IsAsset()
		&& State.Tree->BTGraph == nullptr;
}

UBehaviorTreeGraphNode_Root* FindUniqueRoot(const UBehaviorTreeGraph& Graph)
{
	UBehaviorTreeGraphNode_Root* Root = nullptr;
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		UBehaviorTreeGraphNode_Root* Candidate = Cast<UBehaviorTreeGraphNode_Root>(Node);
		if (Candidate == nullptr) continue;
		if (Root != nullptr) return nullptr;
		Root = Candidate;
	}
	return Root;
}

enum class EScratchGraphCreateResult
{
	EnlistRejected,
	CreationFailed,
	Succeeded,
};

EScratchGraphCreateResult CreateEnlistedScratchGraph(
	IAssetDocumentAtomicTransaction& Transaction,
	TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> State,
	const FGuid& RequestId, const FString& GraphName,
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	if (!EnlistAssetDocumentAIBehaviorTreeScratchBundle(
		Transaction, State, RequestId, Diagnostics)) return EScratchGraphCreateResult::EnlistRejected;
	return CreateScratchGraph(*State, GraphName)
		? EScratchGraphCreateResult::Succeeded : EScratchGraphCreateResult::CreationFailed;
}

FAssetDocumentPrimitiveOutcome MakeScratchGraphCreateFailure(
	EScratchGraphCreateResult Result,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	return Result == EScratchGraphCreateResult::EnlistRejected
		? MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.OperationRejected"),
			TEXT("The scratch bundle could not be covered by rollback."), Diagnostics)
		: MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.GraphCreateFailed"),
			TEXT("The public factories did not create the exact scratch graph."));
}

FAssetDocumentPrimitiveOutcome MakeGraphCreateSuccess(
	IAssetDocumentHandleResolver& Handles,
	TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> State,
	const FString& LivePath)
{
	UBehaviorTreeGraph* Graph = State->Graph.Get();
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MakeAssetDocumentAIBehaviorTreeObjectValue(
		{TEXT("graph_name"), TEXT("schema_class_path")},
		{MakeAssetDocumentAIBehaviorTreeStringValue(Graph->GetName()),
			MakeAssetDocumentAIBehaviorTreeStringValue(
				Graph->GetSchema()->GetClass()->GetPathName())});
	if (!IssueAssetDocumentAIBehaviorTreeGraphHandle(Handles, State, LivePath, Outcome))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.BehaviorTree.GraphCreateFailed"),
			TEXT("The scratch graph handle could not be issued."));
	}
	Outcome.ChangedSet.Add(MakeAssetDocumentAIBehaviorTreeScratchChange(
		LivePath, *Graph, nullptr, TEXT("created")));
	return Outcome;
}

bool IsDefaultsRequest(const FAssetDocumentPrimitiveInvocation& Invocation)
{
	return IsAssetDocumentAIBehaviorTreeScratchContext()
		&& EnsureAssetDocumentAIBehaviorTreeEditorAuthority()
		&& IsAssetDocumentAICanonicalRuntimeValue(Invocation.ValueArguments)
		&& HasAssetDocumentAIBehaviorTreeExactFields(Invocation.ValueArguments, {});
}

UBehaviorTreeGraphNode_Root* CreateExactDefaultRoot(UBehaviorTreeGraph& Graph)
{
	const UEdGraphSchema_BehaviorTree* Schema =
		Cast<UEdGraphSchema_BehaviorTree>(Graph.GetSchema());
	if (Schema == nullptr) return nullptr;
	const int32 Before = Graph.Nodes.Num();
	Schema->CreateDefaultNodesForGraph(Graph);
	UBehaviorTreeGraphNode_Root* Root = FindUniqueRoot(Graph);
	return Root != nullptr && Root->NodeGuid.IsValid()
		&& Root->GetOuter() == &Graph && Graph.Nodes.Num() == Before + 1 ? Root : nullptr;
}

FAssetDocumentPrimitiveOutcome MakeDefaultsSuccess(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	UBehaviorTreeGraphNode_Root& Root)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MakeAssetDocumentAIBehaviorTreeObjectValue(
		{TEXT("node_count"), TEXT("root_stable_node_id")},
		{MakeAssetDocumentAIBehaviorTreeIntegerValue(GraphLease.Graph->Nodes.Num()),
			MakeAssetDocumentAIBehaviorTreeStringValue(
				Root.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower))});
	if (!IssueAssetDocumentAIBehaviorTreeNodeHandle(
		Handles, TEXTVIEW("root_node"), GraphLease, Root, Outcome))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.BehaviorTree.DefaultNodesRejected"),
			TEXT("The scratch root handle could not be issued."));
	}
	Outcome.ChangedSet.Add(MakeAssetDocumentAIBehaviorTreeScratchChange(
		GraphLease.LiveBehaviorTreePath, *GraphLease.Graph, &Root,
		TEXT("defaults_created")));
	return Outcome;
}
} // namespace

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeGraphCreate(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBehaviorTreeGraphLifecycleLocal;
	const FAssetDocumentRuntimeValueDto* TreePath = nullptr;
	const FAssetDocumentRuntimeValueDto* GraphName = nullptr;
	if (!IsRequestedCreateValue(Invocation.ValueArguments, TreePath, GraphName))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.InvalidValueShape"),
			TEXT("The scratch graph-create request is not canonical."));
	}
	if (ResolveAssetDocumentAIBehaviorTreeLiveTree(TreePath->StringValue) == nullptr)
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.BehaviorTree.GraphCreateFailed"),
			TEXT("The frozen live BehaviorTree provenance is not loaded."));
	}
	if (ActiveTransaction == nullptr)
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.InvalidTransaction"),
			TEXT("Scratch graph creation requires an active transaction."));
	}
	TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> State = MakeShared<
		FAssetDocumentAIBehaviorTreeScratchBundleState>();
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	const EScratchGraphCreateResult CreateResult = CreateEnlistedScratchGraph(
		*ActiveTransaction, State, Invocation.RequestId, GraphName->StringValue, Diagnostics);
	if (CreateResult != EScratchGraphCreateResult::Succeeded)
		return MakeScratchGraphCreateFailure(CreateResult, Diagnostics);
	return MakeGraphCreateSuccess(Handles, State, TreePath->StringValue);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeGraphCreateDefaults(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction*)
{
	using namespace AIBehaviorTreeGraphLifecycleLocal;
	if (!IsDefaultsRequest(Invocation))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("BehaviorTree default-node creation requires ready Editor authority and an empty request."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FAssetDocumentAIBehaviorTreeGraphLease GraphLease;
	if (!AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(
		Invocation, Handles, TEXTVIEW("graph"), GraphLease, Diagnostics))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The graph handle is not a live BehaviorTree scratch bundle."), Diagnostics);
	}
	UBehaviorTreeGraphNode_Root* Root = CreateExactDefaultRoot(*GraphLease.Graph);
	if (Root == nullptr)
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.BehaviorTree.RootInvariantFailed"),
			TEXT("The schema did not create exactly one scratch root."));
	}
	return MakeDefaultsSuccess(Handles, GraphLease, *Root);
}
