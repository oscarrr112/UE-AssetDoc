// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeGraphBundleInternal.h"

#include "AssetDocumentRollbackAction.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "AIGraphNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace
{
const FName GraphPayloadType(TEXT("AssetDocument.AI.BehaviorTree.Graph.v1"));
const FName LiveGraphPayloadType(TEXT("AssetDocument.AI.BehaviorTree.LiveGraph.v1"));
const FName NodePayloadType(TEXT("AssetDocument.AI.BehaviorTree.Node.v1"));
constexpr TCHAR ScratchRollbackComponent[] =
	TEXT("assetdocument.ai.behavior_tree.scratch_bundle.v1");

class FScratchBundleCleanupAction final : public IAssetDocumentRollbackAction
{
public:
	explicit FScratchBundleCleanupAction(
		TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> InState)
		: State(MoveTemp(InState)) {}
	virtual FStringView GetRollbackComponentId() const override
	{
		return ScratchRollbackComponent;
	}
	virtual FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		State->bConsumed = true;
		State->Graph.Reset();
		State->Tree.Reset();
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}
private:
	TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> State;
};

class FGraphPayload final : public IAssetDocumentNativeHandlePayload
{
public:
	FGraphPayload(TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> InState,
		FString InLivePath) : State(MoveTemp(InState)), LivePath(MoveTemp(InLivePath)) {}
	virtual FName GetPayloadTypeId() const override { return GraphPayloadType; }
	virtual bool IsAlive() const override
	{
		const UBehaviorTree* Tree = State->Tree.Get();
		const UBehaviorTreeGraph* Graph = State->Graph.Get();
		return !State->bConsumed && Tree != nullptr && Graph != nullptr
			&& Tree->GetClass() == UBehaviorTree::StaticClass()
			&& Graph->GetClass() == UBehaviorTreeGraph::StaticClass()
			&& Graph->GraphGuid.IsValid()
			&& Graph->GetOuter() == Tree && Tree->GetOutermost() == GetTransientPackage()
			&& Tree->HasAllFlags(RF_Transient | RF_Transactional)
			&& Graph->HasAnyFlags(RF_Transient) && !Tree->IsAsset()
			&& !Graph->IsAsset() && Tree->BTGraph == nullptr && !LivePath.IsEmpty();
	}
	TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> GetState() const { return State; }
	UBehaviorTreeGraph* GetGraph() const { return IsAlive() ? State->Graph.Get() : nullptr; }
	const FString& GetLivePath() const { return LivePath; }
private:
	TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> State;
	FString LivePath;
};

class FNodePayload final : public IAssetDocumentNativeHandlePayload
{
public:
	FNodePayload(TWeakPtr<FAssetDocumentAIBehaviorTreeScratchBundleState> InState,
		UEdGraphNode& InNode, FString InLivePath)
		: State(MoveTemp(InState)), Node(&InNode), LivePath(MoveTemp(InLivePath)) {}
	virtual FName GetPayloadTypeId() const override { return NodePayloadType; }
	virtual bool IsAlive() const override
	{
		const TSharedPtr<FAssetDocumentAIBehaviorTreeScratchBundleState> Pinned = State.Pin();
		const UBehaviorTreeGraph* Graph = Pinned.IsValid() ? Pinned->Graph.Get() : nullptr;
		const UEdGraphNode* GraphNode = Node.Get();
		const UAIGraphNode* AINode = Cast<UAIGraphNode>(GraphNode);
		const bool bMain = Graph != nullptr && Graph->Nodes.Contains(GraphNode);
		const bool bSubnode = AINode != nullptr && AINode->ParentNode != nullptr
			&& AINode->ParentNode->GetGraph() == Graph
			&& AINode->ParentNode->SubNodes.Contains(AINode);
		return Pinned.IsValid() && !Pinned->bConsumed && Graph != nullptr && GraphNode != nullptr && GraphNode->GetOuter() == Graph
			&& GraphNode->NodeGuid.IsValid() && (bMain || bSubnode) && !LivePath.IsEmpty();
	}
	UEdGraphNode* GetNode() const { return IsAlive() ? Node.Get() : nullptr; }
private:
	TWeakPtr<FAssetDocumentAIBehaviorTreeScratchBundleState> State;
	TWeakObjectPtr<UEdGraphNode> Node;
	FString LivePath;
};

class FLiveGraphPayload final : public IAssetDocumentNativeHandlePayload
{
public:
	explicit FLiveGraphPayload(TSharedRef<FAssetDocumentAIBehaviorTreeLiveGraphState> InState) : State(MoveTemp(InState)) {}
	FName GetPayloadTypeId() const override { return LiveGraphPayloadType; }
	bool IsAlive() const override
	{
		const auto* Tree = State->Tree.Get();
		const auto* Graph = State->Graph.Get();
		return State->bAlive && Tree && Graph && Tree->GetClass() == UBehaviorTree::StaticClass()
			&& Graph->GetClass() == UBehaviorTreeGraph::StaticClass() && Tree->BTGraph == Graph
			&& Graph->GetOuter() == Tree && Graph->GraphGuid == State->GraphGuid && State->GraphGuid.IsValid();
	}
	TSharedRef<FAssetDocumentAIBehaviorTreeLiveGraphState> State;
};

const FAssetDocumentPrimitiveHandleArgument* FindHandleArgument(
	const FAssetDocumentPrimitiveInvocation& Invocation, FStringView Name)
{
	const FAssetDocumentPrimitiveHandleArgument* Found = nullptr;
	for (const FAssetDocumentPrimitiveHandleArgument& Argument : Invocation.HandleArguments)
	{
		if (Argument.Name == Name)
		{
			if (Found != nullptr) return nullptr;
			Found = &Argument;
		}
	}
	return Found;
}

TUniquePtr<IAssetDocumentVerifiedHandleLease> AcquireLease(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, FStringView Name,
	EAssetDocumentHandleKind Kind,
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	const FAssetDocumentPrimitiveHandleArgument* Argument =
		FindHandleArgument(Invocation, Name);
	if (Argument == nullptr) return nullptr;
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("ai");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = Kind;
	Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	FAssetDocumentVerifiedLeaseResult Result = Handles.AcquireVerifiedLease(Argument->Handle, Context);
	if (!Result.Lease) Diagnostics.Append(Result.Diagnostics);
	return MoveTemp(Result.Lease);
}
} // namespace

FAssetDocumentPrimitiveOutcome MakeAssetDocumentAIBehaviorTreeGraphFailure(
	const TCHAR* Code, const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	return MakeAssetDocumentAIPrimitiveFailure(
		EAssetDocumentPrimitiveOutcomeStatus::Failed, Code, Message, Diagnostics);
}

FAssetDocumentRuntimeValueDto MakeAssetDocumentAIBehaviorTreeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeAssetDocumentAIBehaviorTreeIntegerValue(int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeAssetDocumentAIBehaviorTreeObjectValue(
	TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

const FAssetDocumentRuntimeValueDto* FindAssetDocumentAIBehaviorTreeField(
	const FAssetDocumentRuntimeValueDto& Object, const TCHAR* Name)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num()) return nullptr;
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Name) return &Object.Elements[Index];
	}
	return nullptr;
}

bool HasAssetDocumentAIBehaviorTreeExactFields(
	const FAssetDocumentRuntimeValueDto& Object,
	std::initializer_list<const TCHAR*> Names)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num()
		|| Object.ObjectKeys.Num() != static_cast<int32>(Names.size())) return false;
	TSet<FString> Seen;
	for (const FString& Key : Object.ObjectKeys)
	{
		if (Key.IsEmpty() || Seen.Contains(Key)) return false;
		Seen.Add(Key);
	}
	for (const TCHAR* Name : Names)
	{
		if (FindAssetDocumentAIBehaviorTreeField(Object, Name) == nullptr) return false;
	}
	return true;
}

bool IsAssetDocumentAIBehaviorTreeCanonicalObjectPath(
	const FAssetDocumentRuntimeValueDto* Value)
{
	if (Value == nullptr || Value->Kind != EAssetDocumentRuntimeValueKind::String
		|| Value->StringValue.IsEmpty()) return false;
	const FSoftObjectPath Path(Value->StringValue);
	return Path.IsValid() && Path.GetSubPathString().IsEmpty()
		&& Path.ToString() == Value->StringValue;
}

bool ReadAssetDocumentAIBehaviorTreeFiniteCoordinate(
	const FAssetDocumentRuntimeValueDto* Value, double& OutValue)
{
	if (Value == nullptr) return false;
	if (Value->Kind == EAssetDocumentRuntimeValueKind::Integer)
	{
		OutValue = static_cast<double>(Value->IntegerValue);
		return true;
	}
	if (Value->Kind != EAssetDocumentRuntimeValueKind::Number
		|| !FMath::IsFinite(Value->NumberValue)) return false;
	OutValue = Value->NumberValue;
	return true;
}

bool IsAssetDocumentAIBehaviorTreeScratchContext()
{
	return IsInGameThread() && GIsEditor && !IsRunningCommandlet()
		&& !IsGarbageCollecting();
}

UBehaviorTree* ResolveAssetDocumentAIBehaviorTreeLiveTree(const FString& Path)
{
	UBehaviorTree* Tree = Cast<UBehaviorTree>(FSoftObjectPath(Path).ResolveObject());
	return Tree != nullptr && Tree->GetPathName() == Path ? Tree : nullptr;
}

bool EnlistAssetDocumentAIBehaviorTreeScratchBundle(
	IAssetDocumentAtomicTransaction& Transaction,
	TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> State,
	const FGuid& RequestId,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	FAssetDocumentContractValidationResult Result = Transaction.EnlistRollbackAction(
		MakeShared<FScratchBundleCleanupAction>(State));
	if (!Result.bValid)
	{
		OutDiagnostics.Append(Result.Diagnostics);
		return false;
	}
	FAssetDocumentMutationFootprint Footprint;
	Footprint.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
	Footprint.StableId = FString::Printf(TEXT("scratch:ai.behavior_tree:request:%s"),
		*RequestId.ToString(EGuidFormats::Digits));
	Footprint.RollbackComponentId = ScratchRollbackComponent;
	Result = Transaction.EnlistFootprint(MakeArrayView(&Footprint, 1));
	OutDiagnostics.Append(Result.Diagnostics);
	return Result.bValid;
}

bool AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, FStringView Name,
	FAssetDocumentAIBehaviorTreeGraphLease& OutLease,
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	OutLease.Lease = AcquireLease(Invocation, Handles, Name,
		EAssetDocumentHandleKind::Graph, Diagnostics);
	if (!OutLease.Lease) return false;
	IAssetDocumentNativeHandlePayload& Native = OutLease.Lease->GetPayloadChecked();
	if (Native.GetPayloadTypeId() != GraphPayloadType || !Native.IsAlive()) return false;
	FGraphPayload* Payload = static_cast<FGraphPayload*>(&Native);
	OutLease.State = Payload->GetState();
	OutLease.Graph = Payload->GetGraph();
	OutLease.LiveBehaviorTreePath = Payload->GetLivePath();
	return OutLease.State.IsValid() && OutLease.Graph != nullptr
		&& !OutLease.LiveBehaviorTreePath.IsEmpty();
}

bool AcquireAssetDocumentAIBehaviorTreeGraphLease(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, FStringView ArgumentName,
	FAssetDocumentAIBehaviorTreeGraphLeaseView& OutLease,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	// Live and scratch payloads have distinct type ids and closed casts. Scratch
	// action handlers continue to use the scratch-only Internal acquisition.
	auto Lease = AcquireLease(Invocation, Handles, ArgumentName, EAssetDocumentHandleKind::Graph, OutDiagnostics);
	if (!Lease) return false;
	auto& Native = Lease->GetPayloadChecked();
	if (Native.GetPayloadTypeId() == LiveGraphPayloadType)
	{
		if (!Native.IsAlive()) return false;
		const auto& Payload = static_cast<FLiveGraphPayload&>(Native);
		OutLease.Graph = Payload.State->Graph.Get();
		OutLease.LiveBehaviorTreePath = Payload.State->Tree->GetPathName();
		OutLease.Lease = MoveTemp(Lease);
		return true;
	}
	Lease.Reset();
	FAssetDocumentAIBehaviorTreeGraphLease InternalLease;
	if (!AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(
		Invocation, Handles, ArgumentName, InternalLease, OutDiagnostics)) return false;
	OutLease.Lease = MoveTemp(InternalLease.Lease);
	OutLease.Graph = InternalLease.Graph;
	OutLease.LiveBehaviorTreePath = MoveTemp(InternalLease.LiveBehaviorTreePath);
	return OutLease.Lease.IsValid() && OutLease.Graph != nullptr
		&& !OutLease.LiveBehaviorTreePath.IsEmpty();
}

bool AcquireAssetDocumentAIBehaviorTreeNodeLeaseInternal(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, FStringView Name,
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	FAssetDocumentAIBehaviorTreeNodeLease& OutLease,
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	OutLease.Lease = AcquireLease(Invocation, Handles, Name,
		EAssetDocumentHandleKind::Node, Diagnostics);
	if (!OutLease.Lease) return false;
	IAssetDocumentNativeHandlePayload& Native = OutLease.Lease->GetPayloadChecked();
	if (Native.GetPayloadTypeId() != NodePayloadType || !Native.IsAlive()) return false;
	FNodePayload* Payload = static_cast<FNodePayload*>(&Native);
	OutLease.Node = Payload->GetNode();
	return OutLease.Node != nullptr && OutLease.Node->GetGraph() == GraphLease.Graph;
}

bool IssueAssetDocumentAIBehaviorTreeGraphHandle(
	IAssetDocumentHandleResolver& Handles,
	TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> State,
	const FString& LivePath, FAssetDocumentPrimitiveOutcome& Outcome)
{
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FGraphPayload>(MoveTemp(State), LivePath));
	if (!Issued.Handle.IsSet())
	{
		Outcome.Diagnostics.Append(Issued.Diagnostics);
		return false;
	}
	FAssetDocumentPrimitiveHandleResult& Result = Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = TEXT("graph");
	Result.Handles.Add(Issued.Handle.GetValue());
	return true;
}

bool IssueAssetDocumentAIBehaviorTreeLiveGraphHandle(
	IAssetDocumentHandleResolver& Handles,
	TSharedRef<FAssetDocumentAIBehaviorTreeLiveGraphState> State,
	FAssetDocumentPrimitiveOutcome& Outcome)
{
	auto Issued = Handles.Issue(EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FLiveGraphPayload>(MoveTemp(State)));
	if (!Issued.Handle.IsSet()) { Outcome.Diagnostics.Append(Issued.Diagnostics); return false; }
	auto& Result = Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = TEXT("live_graph"); Result.Handles.Add(Issued.Handle.GetValue());
	return true;
}

bool IssueAssetDocumentAIBehaviorTreeNodeHandle(
	IAssetDocumentHandleResolver& Handles, FStringView ResultName,
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease, UEdGraphNode& Node,
	FAssetDocumentPrimitiveOutcome& Outcome)
{
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FNodePayload>(GraphLease.State.ToSharedRef(), Node,
			GraphLease.LiveBehaviorTreePath));
	if (!Issued.Handle.IsSet())
	{
		Outcome.Diagnostics.Append(Issued.Diagnostics);
		return false;
	}
	FAssetDocumentPrimitiveHandleResult& Result = Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = FString(ResultName);
	Result.Handles.Add(Issued.Handle.GetValue());
	return true;
}

FAssetDocumentChangedRecord MakeAssetDocumentAIBehaviorTreeScratchChange(
	const FString& LivePath, const UBehaviorTreeGraph& Graph,
	const UEdGraphNode* Node, FString Kind)
{
	FAssetDocumentChangedRecord Result;
	Result.AuthorityId = TEXT("assetdocument.ai.behavior_tree.scratch_graph.v1");
	Result.StableObjectId = LivePath;
	Result.Path = FString::Printf(TEXT("asset:%s/ScratchGraph[guid=%s,name=%s]"),
		*LivePath,
		*Graph.GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
		*Graph.GetName());
	if (Node != nullptr)
	{
		Result.Path += FString::Printf(TEXT("/Node[guid=%s]"),
			*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	}
	Result.ChangeKind = MoveTemp(Kind);
	return Result;
}

FAssetDocumentChangedRecord MakeAssetDocumentAIBehaviorTreeScratchConnectionChange(
	const FString& LivePath, const UBehaviorTreeGraph& Graph,
	const UEdGraphNode& FromNode, const UEdGraphNode& ToNode)
{
	FAssetDocumentChangedRecord Result = MakeAssetDocumentAIBehaviorTreeScratchChange(
		LivePath, Graph, &FromNode, TEXT("connected"));
	Result.Path += FString::Printf(TEXT("/Node[guid=%s]"),
		*ToNode.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	return Result;
}

FAssetDocumentPrimitiveOutcome MakeAssetDocumentAIBehaviorTreeNodeActionSuccess(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	const FString& ClassPath, UEdGraphNode& Node)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MakeAssetDocumentAIBehaviorTreeObjectValue(
		{TEXT("node_class_path"), TEXT("position"), TEXT("stable_node_id")},
		{MakeAssetDocumentAIBehaviorTreeStringValue(ClassPath),
			MakeAssetDocumentAIBehaviorTreeObjectValue({TEXT("x"), TEXT("y")},
				{MakeAssetDocumentAIBehaviorTreeIntegerValue(Node.NodePosX),
					MakeAssetDocumentAIBehaviorTreeIntegerValue(Node.NodePosY)}),
			MakeAssetDocumentAIBehaviorTreeStringValue(
				Node.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower))});
	if (!IssueAssetDocumentAIBehaviorTreeNodeHandle(
		Handles, TEXTVIEW("node"), GraphLease, Node, Outcome))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.BehaviorTree.ActionInvocationFailed"),
			TEXT("The BehaviorTree action node handle could not be issued."));
	}
	Outcome.ChangedSet.Add(MakeAssetDocumentAIBehaviorTreeScratchChange(
		GraphLease.LiveBehaviorTreePath, *GraphLease.Graph, &Node,
		TEXT("node_created")));
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeAssetDocumentAIBehaviorTreeSubnodeActionSuccess(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	const FString& ClassPath, const UAIGraphNode& Parent,
	UAIGraphNode& Child, bool bDecorator)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MakeAssetDocumentAIBehaviorTreeObjectValue(
		{TEXT("node_class_path"), TEXT("parent_stable_node_id"),
			TEXT("stable_node_id"), TEXT("subnode_kind")},
		{MakeAssetDocumentAIBehaviorTreeStringValue(ClassPath),
			MakeAssetDocumentAIBehaviorTreeStringValue(
				Parent.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)),
			MakeAssetDocumentAIBehaviorTreeStringValue(
				Child.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)),
			MakeAssetDocumentAIBehaviorTreeStringValue(
				bDecorator ? TEXT("decorator") : TEXT("service"))});
	if (!IssueAssetDocumentAIBehaviorTreeNodeHandle(
		Handles, TEXTVIEW("node"), GraphLease, Child, Outcome))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.BehaviorTree.ActionInvocationFailed"),
			TEXT("The BehaviorTree subnode handle could not be issued."));
	}
	Outcome.ChangedSet.Add(MakeAssetDocumentAIBehaviorTreeScratchChange(
		GraphLease.LiveBehaviorTreePath, *GraphLease.Graph, &Child,
		TEXT("subnode_created")));
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeAssetDocumentAIBehaviorTreeCommentActionSuccess(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	UEdGraphNode_Comment& Comment)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MakeAssetDocumentAIBehaviorTreeObjectValue(
		{TEXT("bounds"), TEXT("stable_node_id"), TEXT("text")},
		{MakeAssetDocumentAIBehaviorTreeObjectValue(
			{TEXT("h"), TEXT("w"), TEXT("x"), TEXT("y")},
			{MakeAssetDocumentAIBehaviorTreeIntegerValue(Comment.NodeHeight),
				MakeAssetDocumentAIBehaviorTreeIntegerValue(Comment.NodeWidth),
				MakeAssetDocumentAIBehaviorTreeIntegerValue(Comment.NodePosX),
				MakeAssetDocumentAIBehaviorTreeIntegerValue(Comment.NodePosY)}),
			MakeAssetDocumentAIBehaviorTreeStringValue(
				Comment.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)),
			MakeAssetDocumentAIBehaviorTreeStringValue(Comment.NodeComment)});
	if (!IssueAssetDocumentAIBehaviorTreeNodeHandle(
		Handles, TEXTVIEW("node"), GraphLease, Comment, Outcome))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.BehaviorTree.CommentActionFailed"),
			TEXT("The comment node handle could not be issued."));
	}
	Outcome.ChangedSet.Add(MakeAssetDocumentAIBehaviorTreeScratchChange(
		GraphLease.LiveBehaviorTreePath, *GraphLease.Graph, &Comment,
		TEXT("comment_created")));
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeAssetDocumentAIBehaviorTreeConnectionSuccess(
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	const FAssetDocumentAIBehaviorTreeNodeLease& FromLease,
	const FAssetDocumentAIBehaviorTreeNodeLease& ToLease)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MakeAssetDocumentAIBehaviorTreeObjectValue(
		{TEXT("connection_kind"), TEXT("from_stable_node_id"),
			TEXT("to_stable_node_id")},
		{MakeAssetDocumentAIBehaviorTreeStringValue(TEXT("schema")),
			MakeAssetDocumentAIBehaviorTreeStringValue(
				FromLease.Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)),
			MakeAssetDocumentAIBehaviorTreeStringValue(
				ToLease.Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower))});
	Outcome.ChangedSet.Add(
		MakeAssetDocumentAIBehaviorTreeScratchConnectionChange(
			GraphLease.LiveBehaviorTreePath, *GraphLease.Graph,
			*FromLease.Node, *ToLease.Node));
	return Outcome;
}
