// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentRollbackAction.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"
#include "Tests/Fixtures/AssetDocumentAITestFixture.h"

#include "AIGraphNode.h"
#include "AIGraphSchema.h"
#include "AIGraphTypes.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Decorators/BTDecorator_ForceSuccess.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTreeEditorModule.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Editor.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "PropertyHandle.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UObjectIterator.h"

namespace AssetDocumentAIBehaviorTreeActionSchemaTests
{
constexpr uint32 PrimitiveVersion = 1;
constexpr const TCHAR* UnknownPrimitiveCode = TEXT("GenericProvider.UnknownPrimitive");
constexpr const TCHAR* InvalidValueShapeCode = TEXT("AIProvider.InvalidValueShape");

struct FExpectedPrimitive
{
	const TCHAR* Id;
	EAssetDocumentPrimitiveAccess Access;
	EAssetDocumentRuntimeRequestPhase Phase;
	bool bStableBlocker;
	const TCHAR* StableDiagnostic;
	TArray<FAssetDocumentPrimitiveHandleArgumentSchema> HandleArguments;
	TArray<FAssetDocumentPrimitiveHandleResultSchema> HandleResults;
};

FAssetDocumentPrimitiveHandleArgumentSchema RequiredHandle(
	const TCHAR* Name,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access)
{
	FAssetDocumentPrimitiveHandleArgumentSchema Schema;
	Schema.Name = Name;
	Schema.Kind = Kind;
	Schema.RequiredAccess = Access;
	Schema.bRequired = true;
	return Schema;
}

FAssetDocumentPrimitiveHandleResultSchema ResultHandle(
	const TCHAR* Name,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access)
{
	FAssetDocumentPrimitiveHandleResultSchema Schema;
	Schema.Name = Name;
	Schema.Kind = Kind;
	Schema.GrantedAccess = Access;
	Schema.Cardinality = EAssetDocumentHandleResultCardinality::ExactlyOne;
	return Schema;
}

const TArray<FExpectedPrimitive>& ExpectedPrimitives()
{
	static const TArray<FExpectedPrimitive> Expected = {
		{TEXT("ai.behavior_tree.graph.create.v1"), EAssetDocumentPrimitiveAccess::Scratch,
			EAssetDocumentRuntimeRequestPhase::Apply, false, TEXT(""), {},
			{ResultHandle(TEXT("graph"), EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::Mutable)}},
		{TEXT("ai.behavior_tree.graph.create_defaults.v1"), EAssetDocumentPrimitiveAccess::Scratch,
			EAssetDocumentRuntimeRequestPhase::Apply, false, TEXT(""),
			{RequiredHandle(TEXT("graph"), EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::Mutable)},
			{ResultHandle(TEXT("root_node"), EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable)}},
		{TEXT("ai.behavior_tree.node.create_action.v1"), EAssetDocumentPrimitiveAccess::Scratch,
			EAssetDocumentRuntimeRequestPhase::Apply, false, TEXT(""),
			{RequiredHandle(TEXT("graph"), EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::Mutable)},
			{ResultHandle(TEXT("node"), EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable)}},
		{TEXT("ai.behavior_tree.subnode.create_action.v1"), EAssetDocumentPrimitiveAccess::Scratch,
			EAssetDocumentRuntimeRequestPhase::Apply, false, TEXT(""),
			{RequiredHandle(TEXT("graph"), EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::Mutable),
			 RequiredHandle(TEXT("parent_node"), EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable)},
			{ResultHandle(TEXT("node"), EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable)}},
		{TEXT("ai.behavior_tree.comment.create_action.v1"), EAssetDocumentPrimitiveAccess::Scratch,
			EAssetDocumentRuntimeRequestPhase::Apply, false, TEXT(""),
			{RequiredHandle(TEXT("graph"), EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::Mutable)},
			{ResultHandle(TEXT("node"), EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable)}},
		{TEXT("ai.behavior_tree.graph.connect.v1"), EAssetDocumentPrimitiveAccess::Scratch,
			EAssetDocumentRuntimeRequestPhase::Apply, false, TEXT(""),
			{RequiredHandle(TEXT("graph"), EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::Mutable),
			 RequiredHandle(TEXT("from_node"), EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable),
			 RequiredHandle(TEXT("to_node"), EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable)}, {}},
		{TEXT("ai.behavior_tree.runtime_mirror.rebuild.v1"), EAssetDocumentPrimitiveAccess::Mutating,
			EAssetDocumentRuntimeRequestPhase::Repair, false, TEXT(""),
			{RequiredHandle(TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable),
			 RequiredHandle(TEXT("graph"), EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::Mutable)}, {}},
		{TEXT("ai.behavior_tree.graph.install.v1"), EAssetDocumentPrimitiveAccess::Mutating,
			EAssetDocumentRuntimeRequestPhase::Apply, false, TEXT(""),
			{RequiredHandle(TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable),
			 RequiredHandle(TEXT("graph"), EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::Mutable)},
			{ResultHandle(TEXT("live_graph"), EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::Mutable)}},
	};
	return Expected;
}

class FCountingHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	struct FEntry
	{
		EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Object;
		EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::ReadOnly;
		uint32 Generation = 1;
		bool bInvalidated = false;
		TSharedPtr<IAssetDocumentNativeHandlePayload> Payload;
	};

	class FLease final : public IAssetDocumentVerifiedHandleLease
	{
	public:
		FLease(FAssetDocumentOpaqueHandle InHandle, IAssetDocumentNativeHandlePayload& InPayload)
			: Handle(MoveTemp(InHandle)), Payload(&InPayload) {}
		virtual const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
		virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return *Payload; }
	private:
		FAssetDocumentOpaqueHandle Handle;
		IAssetDocumentNativeHandlePayload* Payload = nullptr;
	};

	explicit FCountingHandleResolver(const FAssetDocumentMutationToken& InToken)
		: Token(InToken) {}

	int32 IssueCalls = 0;
	int32 LeaseCalls = 0;
	int32 InvalidateCalls = 0;
	bool bRejectNextIssue = false;

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		++IssueCalls;
		FAssetDocumentIssuedHandleResult Result;
		if (bRejectNextIssue)
		{
			bRejectNextIssue = false;
			FAssetDocumentRuntimeDiagnostic& Diagnostic =
				Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Code = TEXT("Test.RejectHandleIssue");
			return Result;
		}
		if (!Payload || !Payload->IsAlive()) return Result;
		FEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Kind = Kind;
		Entry.Access = Access;
		Entry.Payload = MakeShareable(Payload.Release());
		FAssetDocumentOpaqueHandle Handle;
		Handle.RequestId = Token.RequestId;
		Handle.TransactionId = Token.TransactionId;
		Handle.RuntimeRevision = Token.RuntimeRevision;
		Handle.ProviderId = TEXT("ai");
		Handle.Slot = Entries.Num();
		Handle.Generation = Entry.Generation;
		Handle.Kind = Kind;
		Handle.Access = Access;
		Result.Handle = MoveTemp(Handle);
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) override
	{
		++LeaseCalls;
		FAssetDocumentVerifiedLeaseResult Result;
		const FAssetDocumentContractValidationResult Metadata =
			ValidateAssetDocumentHandleMetadata(Handle, Context);
		if (!Metadata.bValid)
		{
			Result.Diagnostics = Metadata.Diagnostics;
			return Result;
		}
		const int32 Index = static_cast<int32>(Handle.Slot) - 1;
		if (!Entries.IsValidIndex(Index)) return Result;
		FEntry& Entry = Entries[Index];
		if (!Entry.bInvalidated && Entry.Generation == Handle.Generation
			&& Entry.Kind == Handle.Kind && Entry.Access == Handle.Access
			&& Entry.Payload.IsValid() && Entry.Payload->IsAlive())
		{
			Result.Lease = MakeUnique<FLease>(Handle, *Entry.Payload);
		}
		return Result;
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation&) override
	{
		++InvalidateCalls;
		FAssetDocumentHandleInvalidationResult Result;
		for (FEntry& Entry : Entries)
		{
			if (!Entry.bInvalidated)
			{
				Entry.bInvalidated = true;
				++Result.InvalidatedHandleCount;
			}
		}
		return Result;
	}

	bool IsAlive(const FAssetDocumentOpaqueHandle& Handle,
		EAssetDocumentRuntimeRequestPhase Phase)
	{
		FAssetDocumentHandleValidationContext Context;
		Context.RequestId = Token.RequestId;
		Context.TransactionId = Token.TransactionId;
		Context.RuntimeRevision = Token.RuntimeRevision;
		Context.ProviderId = TEXT("ai");
		Context.Phase = Phase;
		Context.RequiredKind = Handle.Kind;
		Context.RequiredAccess = Handle.Access;
		return AcquireVerifiedLease(Handle, Context).Lease.IsValid();
	}

private:
	FAssetDocumentMutationToken Token;
	TArray<FEntry> Entries;
};

class FCountingTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	FCountingTransaction()
	{
		Token.RequestId = FGuid::NewGuid();
		Token.TransactionId = FGuid::NewGuid();
		Token.RuntimeRevision = 1;
	}

	int32 FootprintCalls = 0;
	int32 RollbackActionCalls = 0;
	int32 FinalizerCalls = 0;
	TArray<FString> CallOrder;

	virtual FGuid GetTransactionId() const override { return Token.TransactionId; }
	virtual EAssetDocumentAtomicState GetState() const override { return State; }
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override
	{
		++FootprintCalls;
		if (bRejectNextFootprint)
		{
			bRejectNextFootprint = false;
			CallOrder.Add(TEXT("footprint-rejected"));
			return Rejected(TEXT("Test.RejectFootprint"));
		}
		for (const FAssetDocumentMutationFootprint& Item : Footprint)
		{
			const int32 ActionIndex = Actions.IndexOfByPredicate(
				[&Item](const TSharedRef<IAssetDocumentRollbackAction>& Action)
				{
					return Action->GetRollbackComponentId() == Item.RollbackComponentId;
				});
			if (ActionIndex == INDEX_NONE)
			{
				return Rejected(TEXT("Test.MissingRollbackAction"));
			}
			RollbackTargets.AddUnique(ActionIndex);
		}
		CallOrder.Add(TEXT("footprint"));
		State = EAssetDocumentAtomicState::Mutating;
		return Valid();
	}
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		State = EAssetDocumentAtomicState::ReadyToCommit;
		return Valid();
	}
	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt&) override
	{
		State = EAssetDocumentAtomicState::Committed;
		Actions.Reset();
		RollbackTargets.Reset();
		return Valid();
	}
	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) override
	{
		FAssetDocumentContractValidationResult Result = Valid();
		for (int32 Index = RollbackTargets.Num() - 1; Index >= 0; --Index)
		{
			const FAssetDocumentContractValidationResult ActionResult =
				Actions[RollbackTargets[Index]]->Rollback(StableFailureCode);
			if (!ActionResult.bValid) Result = ActionResult;
		}
		Actions.Reset();
		RollbackTargets.Reset();
		State = Result.bValid ? EAssetDocumentAtomicState::RolledBack
			: EAssetDocumentAtomicState::Poisoned;
		return Result;
	}
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		++RollbackActionCalls;
		if (bRejectNextAction)
		{
			bRejectNextAction = false;
			CallOrder.Add(TEXT("rollback_action-rejected"));
			return Rejected(TEXT("Test.RejectRollbackAction"));
		}
		CallOrder.Add(TEXT("rollback_action"));
		Actions.Add(MoveTemp(Action));
		return Valid();
	}
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		++FinalizerCalls;
		CallOrder.Add(TEXT("post_rollback_finalizer"));
		return Valid();
	}

	bool bRejectNextAction = false;
	bool bRejectNextFootprint = false;

private:
	static FAssetDocumentContractValidationResult Valid()
	{
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}
	static FAssetDocumentContractValidationResult Rejected(const TCHAR* Code)
	{
		FAssetDocumentContractValidationResult Result;
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/BehaviorTreeActionSchemaLifecycle");
		Diagnostic.Code = Code;
		Diagnostic.Message = TEXT("The focused test deliberately rejected this enlistment.");
		return Result;
	}

	FAssetDocumentMutationToken Token;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Live;
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
	TArray<int32> RollbackTargets;
};

FAssetDocumentRuntimeValueDto StringValue(const TCHAR* Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentRuntimeValueDto NumberValue(const double Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Number;
	Result.NumberValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto IntegerValue(const int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentPrimitiveInvocation MakeBehaviorInvocation(
	const TCHAR* PrimitiveId,
	EAssetDocumentRuntimeRequestPhase Phase,
	const FAssetDocumentMutationToken& Token,
	FAssetDocumentRuntimeValueDto Value,
	TArray<FAssetDocumentPrimitiveHandleArgument> HandleArguments = {})
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = PrimitiveId;
	Invocation.Version = PrimitiveVersion;
	Invocation.RequestId = Token.RequestId;
	Invocation.TransactionId = Token.TransactionId;
	Invocation.RuntimeRevision = Token.RuntimeRevision;
	Invocation.Phase = Phase;
	Invocation.ValueArguments = MoveTemp(Value);
	Invocation.HandleArguments = MoveTemp(HandleArguments);
	return Invocation;
}

TOptional<FAssetDocumentOpaqueHandle> FindOutcomeHandle(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Name)
{
	for (const FAssetDocumentPrimitiveHandleResult& Result : Outcome.HandleResults)
	{
		if (Result.Name == Name && Result.Handles.Num() == 1)
		{
			return Result.Handles[0];
		}
	}
	return {};
}

bool RequireSuccess(
	FAutomationTestBase& Test,
	const TCHAR* Label,
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Outcome.Diagnostics)
		{
			Test.AddError(FString::Printf(TEXT("%s diagnostic %s: %s"),
				Label, *Diagnostic.Code, *Diagnostic.Message));
		}
	}
	return Test.TestEqual(Label, Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		&& Test.TestTrue(TEXT("successful primitive has no diagnostics"),
			Outcome.Diagnostics.IsEmpty())
		&& Test.TestEqual(TEXT("successful primitive has one semantic ChangedSet record"),
			Outcome.ChangedSet.Num(), 1);
}

const FAssetDocumentRuntimeValueDto* FindField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num()) return nullptr;
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Name) return &Object.Elements[Index];
	}
	return nullptr;
}

bool RequireScratchChangedSet(
	FAutomationTestBase& Test,
	const TCHAR* Label,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const FString& LiveTreePath,
	const UBehaviorTreeGraph& Graph,
	TConstArrayView<const UEdGraphNode*> Nodes)
{
	if (!Test.TestEqual(*FString::Printf(TEXT("%s has one ChangedSet record"), Label),
		Outcome.ChangedSet.Num(), 1)) return false;
	if (!Test.TestTrue(*FString::Printf(TEXT("%s graph has a valid actual GUID"), Label),
		Graph.GraphGuid.IsValid())) return false;
	const FAssetDocumentChangedRecord& Change = Outcome.ChangedSet[0];
	bool bValid = Test.TestEqual(*FString::Printf(TEXT("%s binds stable live provenance"), Label),
		Change.StableObjectId, LiveTreePath);
	FString Expected = FString::Printf(TEXT("asset:%s/ScratchGraph[guid=%s,name=%s]"),
		*LiveTreePath, *Graph.GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower), *Graph.GetName());
	for (const UEdGraphNode* Node : Nodes)
	{
		if (!Test.TestTrue(*FString::Printf(TEXT("%s changed node has a valid actual GUID"), Label),
			Node != nullptr && Node->NodeGuid.IsValid())) return false;
		Expected += FString::Printf(TEXT("/Node[guid=%s]"),
			*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	}
	return bValid & Test.TestEqual(*FString::Printf(TEXT("%s has the exact scratch path"), Label),
		Change.Path, Expected);
}

UBehaviorTreeGraph* FindUniqueScratchGraph(
	FAutomationTestBase& Test,
	const FString& GraphName)
{
	UBehaviorTreeGraph* Found = nullptr;
	for (TObjectIterator<UBehaviorTreeGraph> It; It; ++It)
	{
		if (It->GetName() != GraphName || It->GetOutermost() != GetTransientPackage()) continue;
		if (Found != nullptr)
		{
			Test.AddError(FString::Printf(TEXT("scratch graph name is not unique: %s"), *GraphName));
			return nullptr;
		}
		Found = *It;
	}
	Test.TestNotNull(TEXT("scratch graph is discoverable for exact postcondition evidence"), Found);
	return Found;
}

UBehaviorTreeGraphNode* FindMainBehaviorNode(
	UBehaviorTreeGraph& Graph,
	const FString& StableNodeId)
{
	FGuid NodeGuid;
	if (!FGuid::ParseExact(StableNodeId,
		EGuidFormats::DigitsWithHyphensLower, NodeGuid)) return nullptr;
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		if (UBehaviorTreeGraphNode* BehaviorNode = Cast<UBehaviorTreeGraphNode>(Node);
			BehaviorNode != nullptr && BehaviorNode->NodeGuid == NodeGuid)
		{
			return BehaviorNode;
		}
	}
	return nullptr;
}

UEdGraphNode* FindGraphNode(UBehaviorTreeGraph& Graph, const FString& StableNodeId)
{
	FGuid NodeGuid;
	if (!FGuid::ParseExact(StableNodeId,
		EGuidFormats::DigitsWithHyphensLower, NodeGuid)) return nullptr;
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr && Node->NodeGuid == NodeGuid) return Node;
	}
	return nullptr;
}

UAIGraphNode* FindSubnode(
	const UAIGraphNode& Parent,
	const FString& StableNodeId)
{
	FGuid NodeGuid;
	if (!FGuid::ParseExact(StableNodeId,
		EGuidFormats::DigitsWithHyphensLower, NodeGuid)) return nullptr;
	for (UAIGraphNode* Child : Parent.SubNodes)
	{
		if (Child != nullptr && Child->NodeGuid == NodeGuid) return Child;
	}
	return nullptr;
}

TArray<FGuid> CaptureSubnodeOrder(const UAIGraphNode& Parent)
{
	TArray<FGuid> Result;
	for (const UAIGraphNode* Node : Parent.SubNodes)
		Result.Add(Node != nullptr ? Node->NodeGuid : FGuid());
	return Result;
}

struct FScratchGraphActionState
{
	TArray<UEdGraphNode*> Nodes;
	TArray<FGuid> NodeGuids;
	TArray<int32> NodePositionsX;
	TArray<int32> NodePositionsY;
	bool bPackageDirty = false;
};

FScratchGraphActionState CaptureScratchGraphActionState(const UBehaviorTreeGraph& Graph)
{
	FScratchGraphActionState State;
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		State.Nodes.Add(Node);
		State.NodeGuids.Add(Node != nullptr ? Node->NodeGuid : FGuid());
		State.NodePositionsX.Add(Node != nullptr ? Node->NodePosX : 0);
		State.NodePositionsY.Add(Node != nullptr ? Node->NodePosY : 0);
	}
	State.bPackageDirty = Graph.GetOutermost()->IsDirty();
	return State;
}

bool RequireUnchangedScratchGraphActionState(
	FAutomationTestBase& Test, const TCHAR* Label,
	const FScratchGraphActionState& Before, const UBehaviorTreeGraph& Graph)
{
	const FScratchGraphActionState After = CaptureScratchGraphActionState(Graph);
	return Test.TestEqual(*FString::Printf(TEXT("%s does not dispatch a schema action"), Label),
		After.Nodes, Before.Nodes)
		&& Test.TestEqual(*FString::Printf(TEXT("%s preserves scratch node GUID order"), Label),
			After.NodeGuids, Before.NodeGuids)
		&& Test.TestEqual(*FString::Printf(TEXT("%s preserves scratch node X positions"), Label),
			After.NodePositionsX, Before.NodePositionsX)
		&& Test.TestEqual(*FString::Printf(TEXT("%s preserves scratch node Y positions"), Label),
			After.NodePositionsY, Before.NodePositionsY)
		&& Test.TestEqual(*FString::Printf(TEXT("%s preserves scratch package dirty state"), Label),
			After.bPackageDirty, Before.bPackageDirty);
}

bool RequireOneAppendedSubnode(
	FAutomationTestBase& Test, const TCHAR* Label, const UAIGraphNode& Parent,
	TConstArrayView<FGuid> Before, const UAIGraphNode* ExpectedChild)
{
	bool bValid = Test.TestTrue(*FString::Printf(TEXT("%s appends exactly one subnode"), Label),
		ExpectedChild != nullptr && Parent.SubNodes.Num() == Before.Num() + 1
		&& Parent.SubNodes.Last() == ExpectedChild);
	for (int32 Index = 0; Index < Before.Num(); ++Index)
		bValid &= Test.TestTrue(*FString::Printf(TEXT("%s preserves subnode preimage order %d"), Label, Index),
			Parent.SubNodes[Index] != nullptr && Parent.SubNodes[Index]->NodeGuid == Before[Index]);
	return bValid;
}

UBehaviorTreeGraphNode* CreateFixtureSequenceNode(
	FAutomationTestBase& Test,
	UBehaviorTreeGraph& Graph)
{
	FBehaviorTreeEditorModule* Module =
		FModuleManager::GetModulePtr<FBehaviorTreeEditorModule>(TEXT("BehaviorTreeEditor"));
	if (!Test.TestNotNull(TEXT("fixture sees the ready public BehaviorTreeEditor module"), Module)
		|| !Module->GetClassCache().IsValid()) return nullptr;

	TArray<FGraphNodeClassData> CompositeClasses;
	Module->GetClassCache()->GatherClasses(
		UBTCompositeNode::StaticClass(), CompositeClasses);
	int32 SequenceIndex = INDEX_NONE;
	for (int32 Index = 0; Index < CompositeClasses.Num(); ++Index)
	{
		if (CompositeClasses[Index].GetClass(true)
			== UBTComposite_Sequence::StaticClass())
		{
			SequenceIndex = Index;
			break;
		}
	}
	if (!Test.TestTrue(TEXT("fixture public cache contains the native Sequence class"),
		SequenceIndex != INDEX_NONE)) return nullptr;

	const UEdGraphSchema_BehaviorTree* Schema =
		Cast<UEdGraphSchema_BehaviorTree>(Graph.GetSchema());
	if (!Test.TestNotNull(TEXT("fixture graph exposes the stock BehaviorTree schema"), Schema))
	{
		return nullptr;
	}
	FGraphContextMenuBuilder Builder(&Graph);
	Builder.OwnerOfTemporaries = &Graph;
	Schema->GetGraphContextActions(Builder);
	if (!Test.TestTrue(TEXT("fixture schema preserves the public cache/action ordinal"),
		SequenceIndex >= 0 && SequenceIndex < Builder.GetNumActions())) return nullptr;
	TSharedPtr<FEdGraphSchemaAction>& Action = Builder.GetSchemaAction(SequenceIndex);
	UEdGraphNode* Created = Action.IsValid()
		? Action->PerformAction(&Graph, nullptr, FVector2f(320.0f, 80.0f), false)
		: nullptr;
	UBehaviorTreeGraphNode* Node = Cast<UBehaviorTreeGraphNode>(Created);
	if (!Test.TestNotNull(TEXT("fixture stock action creates one BehaviorTree graph node"), Node)
		|| !Test.TestNotNull(TEXT("fixture action creates a node instance"),
			Node != nullptr ? Node->NodeInstance.Get() : nullptr)) return nullptr;
	Test.TestTrue(TEXT("fixture action creates the exact Sequence instance"),
		Node->NodeInstance->GetClass() == UBTComposite_Sequence::StaticClass());
	return Node;
}

FAssetDocumentOpaqueHandle MakeHandle(
	const FAssetDocumentMutationToken& Token,
	EAssetDocumentHandleKind Kind,
	uint32 Slot)
{
	FAssetDocumentOpaqueHandle Handle;
	Handle.RequestId = Token.RequestId;
	Handle.TransactionId = Token.TransactionId;
	Handle.RuntimeRevision = Token.RuntimeRevision;
	Handle.ProviderId = TEXT("ai");
	Handle.Slot = Slot;
	Handle.Generation = 1;
	Handle.Kind = Kind;
	Handle.Access = EAssetDocumentHandleAccess::Mutable;
	return Handle;
}

FAssetDocumentPrimitiveInvocation MakeInvocation(
	const FExpectedPrimitive& Expected,
	const FAssetDocumentMutationToken& Token,
	const FString& LiveTreePath)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = Expected.Id;
	Invocation.Version = PrimitiveVersion;
	Invocation.RequestId = Token.RequestId;
	Invocation.TransactionId = Token.TransactionId;
	Invocation.RuntimeRevision = Token.RuntimeRevision;
	Invocation.Phase = Expected.Phase;
	Invocation.ValueArguments = StringValue(TEXT("deliberately-malformed"));
	for (int32 Index = 0; Index < Expected.HandleArguments.Num(); ++Index)
	{
		FAssetDocumentPrimitiveHandleArgument& Argument = Invocation.HandleArguments.AddDefaulted_GetRef();
		Argument.Name = Expected.HandleArguments[Index].Name;
		Argument.Handle = MakeHandle(Token, Expected.HandleArguments[Index].Kind, Index + 1);
	}
	return Invocation;
}

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const IAssetDocumentPrimitiveProvider& Provider,
	const TCHAR* PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider.DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == PrimitiveId && Descriptor.Version == PrimitiveVersion)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

bool HasDiagnostic(const FAssetDocumentPrimitiveOutcome& Outcome, const TCHAR* Code)
{
	return Outcome.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code;
	});
}

bool HasOnlyDiagnostic(const FAssetDocumentPrimitiveOutcome& Outcome, const TCHAR* Code)
{
	return Outcome.Diagnostics.Num() == 1 && Outcome.Diagnostics[0].Code == Code;
}

struct FSideEffectCounts
{
	int32 IssueCalls = 0;
	int32 LeaseCalls = 0;
	int32 InvalidateCalls = 0;
	int32 FootprintCalls = 0;
	int32 RollbackActionCalls = 0;
	int32 FinalizerCalls = 0;
	TArray<FString> TransactionOrder;
};

FSideEffectCounts CaptureSideEffectCounts(
	const FCountingHandleResolver& Handles,
	const FCountingTransaction& Transaction)
{
	FSideEffectCounts Counts;
	Counts.IssueCalls = Handles.IssueCalls;
	Counts.LeaseCalls = Handles.LeaseCalls;
	Counts.InvalidateCalls = Handles.InvalidateCalls;
	Counts.FootprintCalls = Transaction.FootprintCalls;
	Counts.RollbackActionCalls = Transaction.RollbackActionCalls;
	Counts.FinalizerCalls = Transaction.FinalizerCalls;
	Counts.TransactionOrder = Transaction.CallOrder;
	return Counts;
}

bool RequireNoSideEffectDelta(
	FAutomationTestBase& Test,
	const TCHAR* Row,
	const FSideEffectCounts& Before,
	const FCountingHandleResolver& Handles,
	const FCountingTransaction& Transaction)
{
	const FSideEffectCounts After = CaptureSideEffectCounts(Handles, Transaction);
	return Test.TestEqual(*FString::Printf(TEXT("%s issues no handles before its admission terminal"), Row),
		After.IssueCalls, Before.IssueCalls)
		&& Test.TestEqual(*FString::Printf(TEXT("%s acquires no leases before its admission terminal"), Row),
			After.LeaseCalls, Before.LeaseCalls)
		&& Test.TestEqual(*FString::Printf(TEXT("%s invalidates no handles before its admission terminal"), Row),
			After.InvalidateCalls, Before.InvalidateCalls)
		&& Test.TestEqual(*FString::Printf(TEXT("%s enlists no footprint before rollback coverage"), Row),
			After.FootprintCalls, Before.FootprintCalls)
		&& Test.TestEqual(*FString::Printf(TEXT("%s enlists no rollback action before admission"), Row),
			After.RollbackActionCalls, Before.RollbackActionCalls)
		&& Test.TestEqual(*FString::Printf(TEXT("%s enlists no runtime-mirror finalizer before admission"), Row),
			After.FinalizerCalls, Before.FinalizerCalls)
		&& Test.TestEqual(*FString::Printf(TEXT("%s preserves transaction action/footprint ordering"), Row),
			After.TransactionOrder, Before.TransactionOrder);
}

bool MatchesHandleSchemas(
	const TArray<FAssetDocumentPrimitiveHandleArgumentSchema>& Actual,
	const TArray<FAssetDocumentPrimitiveHandleArgumentSchema>& Expected)
{
	if (Actual.Num() != Expected.Num()) return false;
	for (int32 Index = 0; Index < Expected.Num(); ++Index)
	{
		if (Actual[Index].Name != Expected[Index].Name || Actual[Index].Kind != Expected[Index].Kind
			|| Actual[Index].RequiredAccess != Expected[Index].RequiredAccess
			|| Actual[Index].bRequired != Expected[Index].bRequired) return false;
	}
	return true;
}

bool MatchesResultSchemas(
	const TArray<FAssetDocumentPrimitiveHandleResultSchema>& Actual,
	const TArray<FAssetDocumentPrimitiveHandleResultSchema>& Expected)
{
	if (Actual.Num() != Expected.Num()) return false;
	for (int32 Index = 0; Index < Expected.Num(); ++Index)
	{
		if (Actual[Index].Name != Expected[Index].Name || Actual[Index].Kind != Expected[Index].Kind
			|| Actual[Index].GrantedAccess != Expected[Index].GrantedAccess
			|| Actual[Index].Cardinality != Expected[Index].Cardinality) return false;
	}
	return true;
}

struct FFixtureState
{
	UBehaviorTree* Tree = nullptr;
	UBehaviorTreeGraph* Graph = nullptr;
	UBehaviorTreeGraphNode_Root* Root = nullptr;
	UEdGraph* InstalledGraph = nullptr;
	UObject* GraphOuter = nullptr;
	UObject* RootGraph = nullptr;
	UBlackboardData* TreeBlackboard = nullptr;
	UBlackboardData* RootBlackboard = nullptr;
	UBlackboardData* BlackboardParent = nullptr;
	TArray<FName> BlackboardKeyNames;
	TArray<UClass*> BlackboardKeyTypes;
	TArray<FGuid> NodeGuids;
	TArray<UClass*> NodeClasses;
	TArray<UObject*> NodeInstances;
	bool bTreePackageDirty = false;
	bool bGraphPackageDirty = false;
	bool bBlackboardPackageDirty = false;
	int32 NodeCount = 0;
	int32 OpenEditorCount = INDEX_NONE;
	int32 EditedAssetCount = INDEX_NONE;
};

FFixtureState CaptureFixtureState(const FAssetDocumentAITestFixture& Fixture)
{
	FFixtureState State;
	State.Tree = Fixture.GetScratchTree();
	State.Graph = Fixture.GetScratchGraph();
	State.Root = Fixture.GetRootNode();
	State.InstalledGraph = State.Tree ? State.Tree->BTGraph : nullptr;
	State.GraphOuter = State.Graph ? State.Graph->GetOuter() : nullptr;
	State.RootGraph = State.Root ? State.Root->GetGraph() : nullptr;
	State.TreeBlackboard = State.Tree ? State.Tree->BlackboardAsset.Get() : nullptr;
	State.RootBlackboard = State.Root ? State.Root->BlackboardAsset.Get() : nullptr;
	State.BlackboardParent = State.TreeBlackboard ? State.TreeBlackboard->Parent.Get() : nullptr;
	if (State.TreeBlackboard)
	{
		for (const FBlackboardEntry& Entry : State.TreeBlackboard->Keys)
		{
			State.BlackboardKeyNames.Add(Entry.EntryName);
			State.BlackboardKeyTypes.Add(Entry.KeyType ? Entry.KeyType->GetClass() : nullptr);
		}
	}
	if (State.Graph)
	{
		for (UEdGraphNode* Node : State.Graph->Nodes)
		{
			State.NodeGuids.Add(Node ? Node->NodeGuid : FGuid());
			State.NodeClasses.Add(Node ? Node->GetClass() : nullptr);
			const UAIGraphNode* AINode = Cast<UAIGraphNode>(Node);
			State.NodeInstances.Add(AINode ? AINode->NodeInstance.Get() : nullptr);
		}
	}
	State.bTreePackageDirty = State.Tree && State.Tree->GetOutermost()->IsDirty();
	State.bGraphPackageDirty = State.Graph && State.Graph->GetOutermost()->IsDirty();
	State.bBlackboardPackageDirty = State.TreeBlackboard
		&& State.TreeBlackboard->GetOutermost()->IsDirty();
	State.NodeCount = State.Graph ? State.Graph->Nodes.Num() : 0;
	if (GEditor)
	{
		if (UAssetEditorSubsystem* Subsystem =
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			State.OpenEditorCount = Subsystem->GetAllOpenEditors().Num();
			State.EditedAssetCount = Subsystem->GetAllEditedAssets().Num();
		}
	}
	return State;
}

bool RequireUnchangedFixtureState(
	FAutomationTestBase& Test,
	const FFixtureState& Before,
	const FAssetDocumentAITestFixture& Fixture)
{
	const FFixtureState After = CaptureFixtureState(Fixture);
	return Test.TestEqual(TEXT("lookup RED preserves fixture scratch tree"), After.Tree, Before.Tree)
		&& Test.TestEqual(TEXT("lookup RED preserves fixture scratch graph"), After.Graph, Before.Graph)
		&& Test.TestEqual(TEXT("lookup RED preserves fixture root"), After.Root, Before.Root)
		&& Test.TestEqual(TEXT("lookup RED preserves installed graph pointer"), After.InstalledGraph, Before.InstalledGraph)
		&& Test.TestEqual(TEXT("lookup RED preserves scratch graph outer"), After.GraphOuter, Before.GraphOuter)
		&& Test.TestEqual(TEXT("lookup RED preserves root graph membership"), After.RootGraph, Before.RootGraph)
		&& Test.TestEqual(TEXT("scratch operations preserve the live Tree Blackboard mirror"), After.TreeBlackboard, Before.TreeBlackboard)
		&& Test.TestEqual(TEXT("scratch operations preserve the live Root Blackboard source"), After.RootBlackboard, Before.RootBlackboard)
		&& Test.TestEqual(TEXT("scratch operations preserve the live Blackboard parent"), After.BlackboardParent, Before.BlackboardParent)
		&& Test.TestEqual(TEXT("scratch operations preserve live Blackboard authored key names"), After.BlackboardKeyNames, Before.BlackboardKeyNames)
		&& Test.TestEqual(TEXT("scratch operations preserve live Blackboard key types"), After.BlackboardKeyTypes, Before.BlackboardKeyTypes)
		&& Test.TestEqual(TEXT("scratch operations preserve live graph node GUID order"), After.NodeGuids, Before.NodeGuids)
		&& Test.TestEqual(TEXT("scratch operations preserve live graph node classes"), After.NodeClasses, Before.NodeClasses)
		&& Test.TestEqual(TEXT("scratch operations preserve live graph node instances"), After.NodeInstances, Before.NodeInstances)
		&& Test.TestEqual(TEXT("lookup RED preserves scratch tree package dirty state"), After.bTreePackageDirty, Before.bTreePackageDirty)
		&& Test.TestEqual(TEXT("lookup RED preserves scratch graph package dirty state"), After.bGraphPackageDirty, Before.bGraphPackageDirty)
		&& Test.TestEqual(TEXT("scratch operations preserve live Blackboard package dirty state"), After.bBlackboardPackageDirty, Before.bBlackboardPackageDirty)
		&& Test.TestEqual(TEXT("lookup RED preserves scratch graph node count"), After.NodeCount, Before.NodeCount)
		&& Test.TestEqual(TEXT("scratch operations leave no public toolkit window"), After.OpenEditorCount, Before.OpenEditorCount)
		&& Test.TestEqual(TEXT("scratch operations leave no edited-asset registration"), After.EditedAssetCount, Before.EditedAssetCount);
}

void AssertDescriptorShape(
	FAutomationTestBase& Test,
	const FExpectedPrimitive& Expected,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	Test.TestEqual(TEXT("future descriptor retains exact primitive id"), Descriptor.PrimitiveId, FString(Expected.Id));
	Test.TestEqual(TEXT("future descriptor retains exact version"), Descriptor.Version, PrimitiveVersion);
	Test.TestEqual(TEXT("future descriptor is owned by AI"), Descriptor.Owner, EAssetDocumentPrimitiveOwner::AI);
	Test.TestEqual(TEXT("future descriptor retains access classification"), Descriptor.Access, Expected.Access);
	Test.TestTrue(TEXT("future descriptor admits its frozen phase"), Descriptor.AllowedPhases.Contains(Expected.Phase));
	Test.TestEqual(TEXT("future descriptor retains HG/HN argument schema"),
		MatchesHandleSchemas(Descriptor.HandleArgumentSchemas, Expected.HandleArguments), true);
	Test.TestEqual(TEXT("future descriptor retains HG/HN result schema"),
		MatchesResultSchemas(Descriptor.HandleResultSchemas, Expected.HandleResults), true);
	if (Expected.bStableBlocker)
	{
		Test.TestEqual(TEXT("graph install remains a stable blocker"), Descriptor.Admission.Disposition,
			EAssetDocumentPrimitiveDisposition::StableBlocker);
		Test.TestTrue(TEXT("graph install retains its frozen diagnostic"),
			Descriptor.StableDiagnosticCodes.Contains(Expected.StableDiagnostic));
		Test.TestTrue(TEXT("graph install exposes no callable handles"),
			Descriptor.HandleArgumentSchemas.IsEmpty() && Descriptor.HandleResultSchemas.IsEmpty());
	}
	else
	{
		Test.TestEqual(TEXT("ordinary scratch and runtime rows require an active transaction"),
			Descriptor.bRequiresActiveTransaction, true);
	}
}
} // namespace AssetDocumentAIBehaviorTreeActionSchemaTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBehaviorTreeActionSchemaTest,
	"UEAssetDoc.AIProvider.BehaviorTreeActionSchemaLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBehaviorTreeActionSchemaTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeActionSchemaTests;
	UAssetEditorSubsystem* AssetEditors = GEditor
		? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("normal Editor exposes AssetEditorSubsystem"), AssetEditors))
	{
		return false;
	}
	const int32 OpenEditorsBeforeReadiness = AssetEditors->GetAllOpenEditors().Num();
	const int32 EditedAssetsBeforeReadiness = AssetEditors->GetAllEditedAssets().Num();
	TestTrue(TEXT("provider-owned readiness admits the normal Editor authority"),
		EnsureAssetDocumentAIBehaviorTreeEditorAuthority());
	FBehaviorTreeEditorModule* ReadyModule =
		FModuleManager::GetModulePtr<FBehaviorTreeEditorModule>(TEXT("BehaviorTreeEditor"));
	TestTrue(TEXT("provider readiness leaves the public BehaviorTree class cache ready"),
		ReadyModule != nullptr && ReadyModule->GetClassCache().IsValid());
	TestEqual(TEXT("provider readiness closes its public toolkit window"),
		AssetEditors->GetAllOpenEditors().Num(), OpenEditorsBeforeReadiness);
	TestEqual(TEXT("provider readiness releases its temporary edited asset"),
		AssetEditors->GetAllEditedAssets().Num(), EditedAssetsBeforeReadiness);

	FAssetDocumentAITestFixture Fixture;
	if (!Fixture.CreateFactoryOwnedScratchBundle(*this)
		|| !Fixture.RequireUniqueRootPropertyHandle(*this))
	{
		return false;
	}
	UBlackboardData* LiveBlackboard = NewObject<UBlackboardData>(
		Fixture.GetScratchTree(), TEXT("AssetDocumentAIT4LiveBlackboard"), RF_Transactional);
	if (!TestNotNull(TEXT("test-owned live Blackboard exists"), LiveBlackboard))
	{
		return false;
	}
	LiveBlackboard->UpdatePersistentKey<UBlackboardKeyType_Bool>(TEXT("Ready"));
	if (!TestEqual(TEXT("typed Root fixture write initializes the live Blackboard closure"),
		Fixture.GetBlackboardAssetHandle()->SetValue(
			LiveBlackboard, EPropertyValueSetFlags::NotTransactable),
		FPropertyAccess::Success)) return false;
	UBehaviorTreeGraphNode* ExistingSequence =
		CreateFixtureSequenceNode(*this, *Fixture.GetScratchGraph());
	if (ExistingSequence == nullptr)
	{
		return false;
	}
	// This is test-fixture setup for the separate existing-instance read-only row;
	// provider graph.install remains frozen and is never invoked successfully.
	Fixture.GetScratchTree()->BTGraph = Fixture.GetScratchGraph();
	const FFixtureState Before = CaptureFixtureState(Fixture);
	TestNotNull(TEXT("fixture supplies the live provenance tree"), Before.Tree);
	TestNotNull(TEXT("fixture supplies its existing live graph"), Before.Graph);
	TestEqual(TEXT("fixture installs only its test-owned existing graph"),
		Before.InstalledGraph, static_cast<UEdGraph*>(Before.Graph));

	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	FCountingTransaction Transaction;
	FCountingHandleResolver Handles(Transaction.GetMutationToken());

	for (const FExpectedPrimitive& Expected : ExpectedPrimitives())
	{
		const FAssetDocumentPrimitiveInvocation Invocation =
			MakeInvocation(Expected, Transaction.GetMutationToken(), Before.Tree->GetPathName());
		const FSideEffectCounts CountsBeforeInvocation = CaptureSideEffectCounts(Handles, Transaction);
		const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(*Provider, Expected.Id);
		if (!Descriptor)
		{
			AddError(*FString::Printf(TEXT("required descriptor is absent: %s"), Expected.Id));
			const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(Invocation, Handles, &Transaction);
			TestEqual(TEXT("missing descriptor terminates as a failed lookup RED"), Outcome.Status,
				EAssetDocumentPrimitiveOutcomeStatus::Failed);
			TestTrue(TEXT("missing descriptor returns GenericProvider.UnknownPrimitive"),
				HasDiagnostic(Outcome, UnknownPrimitiveCode));
			TestTrue(TEXT("lookup RED returns no value handles"), Outcome.HandleResults.IsEmpty());
			TestTrue(TEXT("lookup RED returns no changed set"), Outcome.ChangedSet.IsEmpty());
			RequireNoSideEffectDelta(*this, Expected.Id, CountsBeforeInvocation, Handles, Transaction);
			continue;
		}
		AssertDescriptorShape(*this, Expected, *Descriptor);
		if (Expected.bStableBlocker)
		{
			const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(Invocation, Handles, &Transaction);
			TestEqual(TEXT("graph install returns the stable unavailable terminal"), Outcome.Status,
				EAssetDocumentPrimitiveOutcomeStatus::Unavailable);
			TestTrue(TEXT("graph install returns only its frozen authority diagnostic"),
				HasOnlyDiagnostic(Outcome, Expected.StableDiagnostic));
			TestTrue(TEXT("graph install exposes no value handles"), Outcome.HandleResults.IsEmpty());
			TestTrue(TEXT("graph install exposes no changed set"), Outcome.ChangedSet.IsEmpty());
			RequireNoSideEffectDelta(*this, Expected.Id, CountsBeforeInvocation, Handles, Transaction);
			continue;
		}

		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(Invocation, Handles, &Transaction);
		TestEqual(TEXT("ordinary descriptor reaches a stable admission terminal"), Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed);
		TestTrue(TEXT("ordinary descriptor no longer reports an unknown primitive"),
			!HasDiagnostic(Outcome, UnknownPrimitiveCode));
		TestTrue(TEXT("ordinary descriptor reaches a stable provider-local admission diagnostic"),
			!Outcome.Diagnostics.IsEmpty());
		TestTrue(TEXT("ordinary admission failure exposes no value handles"), Outcome.HandleResults.IsEmpty());
		TestTrue(TEXT("ordinary admission failure exposes no changed set"), Outcome.ChangedSet.IsEmpty());
		RequireNoSideEffectDelta(*this, Expected.Id, CountsBeforeInvocation, Handles, Transaction);
	}

	TestEqual(TEXT("malformed descriptor probes issue zero HG/HN handles"), Handles.IssueCalls, 0);
	TestEqual(TEXT("malformed descriptor probes acquire zero HG/HN leases"), Handles.LeaseCalls, 0);
	TestEqual(TEXT("malformed descriptor probes enlist zero footprints"), Transaction.FootprintCalls, 0);
	TestEqual(TEXT("malformed descriptor probes enlist zero rollback actions"), Transaction.RollbackActionCalls, 0);

	const FString LivePath = Before.Tree->GetPathName();
	const FAssetDocumentPrimitiveOutcome ClassInspect = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.node_class.inspect.v1"),
			EAssetDocumentRuntimeRequestPhase::Validate, Transaction.GetMutationToken(),
			ObjectValue({TEXT("expected_base_class_path"), TEXT("node_class_path")},
				{StringValue(*UBTCompositeNode::StaticClass()->GetPathName()),
					StringValue(*UBTComposite_Sequence::StaticClass()->GetPathName())})),
		Handles, nullptr);
	TestEqual(TEXT("node class inspection succeeds"), ClassInspect.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestTrue(TEXT("node class inspection is read-only"),
		ClassInspect.HandleResults.IsEmpty() && ClassInspect.ChangedSet.IsEmpty()
		&& ClassInspect.Diagnostics.IsEmpty());
	const FAssetDocumentRuntimeValueDto* ExistingProperties =
		FindField(ClassInspect.Value, TEXT("canonical_properties"));
	TestNotNull(TEXT("node class inspection returns canonical properties"), ExistingProperties);
	if (ExistingProperties != nullptr)
	{
		const FAssetDocumentPrimitiveOutcome InstanceValidation = Provider->Invoke(
			MakeBehaviorInvocation(TEXT("ai.behavior_tree.instance.validate_properties.v1"),
				EAssetDocumentRuntimeRequestPhase::Validate, Transaction.GetMutationToken(),
				ObjectValue({TEXT("behavior_tree_path"), TEXT("canonical_properties"),
					TEXT("expected_class_path"), TEXT("stable_node_id")},
					{StringValue(*LivePath), *ExistingProperties,
						StringValue(*UBTComposite_Sequence::StaticClass()->GetPathName()),
						StringValue(*ExistingSequence->NodeGuid.ToString(
							EGuidFormats::DigitsWithHyphensLower))})),
			Handles, nullptr);
		TestEqual(TEXT("existing action-created instance validation succeeds"),
			InstanceValidation.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestTrue(TEXT("existing instance validation is read-only"),
			InstanceValidation.HandleResults.IsEmpty()
			&& InstanceValidation.ChangedSet.IsEmpty()
			&& InstanceValidation.Diagnostics.IsEmpty());
	}

	const FSideEffectCounts BeforeInvalidGraph =
		CaptureSideEffectCounts(Handles, Transaction);
	const FAssetDocumentPrimitiveOutcome PreflightGraph = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.graph.create.v1"),
			EAssetDocumentRuntimeRequestPhase::Preflight, Transaction.GetMutationToken(),
			ObjectValue({TEXT("behavior_tree_path"), TEXT("graph_name")},
				{StringValue(*LivePath), StringValue(TEXT("MustNotCreateInPreflight"))})),
		Handles, &Transaction);
	TestEqual(TEXT("graph creation rejects Preflight even with a live transaction"),
		PreflightGraph.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	RequireNoSideEffectDelta(*this, TEXT("Preflight graph creation"), BeforeInvalidGraph,
		Handles, Transaction);
	const FAssetDocumentPrimitiveOutcome InvalidGraph = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.graph.create.v1"),
			EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
			ObjectValue({TEXT("behavior_tree_path"), TEXT("graph_name")},
				{StringValue(*LivePath), StringValue(TEXT(""))})),
		Handles, &Transaction);
	TestEqual(TEXT("empty graph name fails before factory coverage"), InvalidGraph.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("empty graph name returns InvalidValueShape"),
		HasDiagnostic(InvalidGraph, InvalidValueShapeCode));
	RequireNoSideEffectDelta(*this, TEXT("empty graph name"), BeforeInvalidGraph,
		Handles, Transaction);

	FCountingTransaction RejectActionTransaction;
	FCountingHandleResolver RejectActionHandles(
		RejectActionTransaction.GetMutationToken());
	RejectActionTransaction.bRejectNextAction = true;
	const FAssetDocumentPrimitiveOutcome ActionRejected = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.graph.create.v1"),
			EAssetDocumentRuntimeRequestPhase::Apply,
			RejectActionTransaction.GetMutationToken(),
			ObjectValue({TEXT("behavior_tree_path"), TEXT("graph_name")},
				{StringValue(*LivePath), StringValue(TEXT("ActionRejectedGraph"))})),
		RejectActionHandles, &RejectActionTransaction);
	TestEqual(TEXT("rollback-action rejection fails graph creation"),
		ActionRejected.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestEqual(TEXT("rollback-action rejection issues no HG"),
		RejectActionHandles.IssueCalls, 0);
	TestEqual(TEXT("rollback-action rejection enlists no footprint"),
		RejectActionTransaction.FootprintCalls, 0);

	FCountingTransaction RejectFootprintTransaction;
	FCountingHandleResolver RejectFootprintHandles(
		RejectFootprintTransaction.GetMutationToken());
	RejectFootprintTransaction.bRejectNextFootprint = true;
	const FAssetDocumentPrimitiveOutcome FootprintRejected = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.graph.create.v1"),
			EAssetDocumentRuntimeRequestPhase::Apply,
			RejectFootprintTransaction.GetMutationToken(),
			ObjectValue({TEXT("behavior_tree_path"), TEXT("graph_name")},
				{StringValue(*LivePath), StringValue(TEXT("FootprintRejectedGraph"))})),
		RejectFootprintHandles, &RejectFootprintTransaction);
	TestEqual(TEXT("footprint rejection fails graph creation"),
		FootprintRejected.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestEqual(TEXT("footprint rejection issues no HG"),
		RejectFootprintHandles.IssueCalls, 0);
	TestEqual(TEXT("footprint rejection records action before footprint"),
		RejectFootprintTransaction.CallOrder,
		TArray<FString>({TEXT("rollback_action"), TEXT("footprint-rejected")}));
	RejectFootprintTransaction.Rollback(TEXTVIEW("Test.FootprintRejected"));

	const FAssetDocumentPrimitiveOutcome GraphCreate = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.graph.create.v1"),
			EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
			ObjectValue({TEXT("behavior_tree_path"), TEXT("graph_name")},
				{StringValue(*LivePath), StringValue(TEXT("AssetDocumentAIT4ScratchGraph"))})),
		Handles, &Transaction);
	RequireSuccess(*this, TEXT("factory-owned scratch graph creation succeeds"), GraphCreate);
	UBehaviorTreeGraph* ScratchGraph = FindUniqueScratchGraph(
		*this, TEXT("AssetDocumentAIT4ScratchGraph"));
	if (ScratchGraph != nullptr)
		RequireScratchChangedSet(*this, TEXT("graph create"), GraphCreate, LivePath,
			*ScratchGraph, {});
	if (ScratchGraph != nullptr)
	{
		FBehaviorTreeEditorModule* Module =
			FModuleManager::GetModulePtr<FBehaviorTreeEditorModule>(TEXT("BehaviorTreeEditor"));
		const UEdGraphSchema_BehaviorTree* Schema =
			Cast<UEdGraphSchema_BehaviorTree>(ScratchGraph->GetSchema());
		if (Module != nullptr && Module->GetClassCache().IsValid() && Schema != nullptr)
		{
			TArray<FGraphNodeClassData> Composites;
			TArray<FGraphNodeClassData> Tasks;
			Module->GetClassCache()->GatherClasses(UBTCompositeNode::StaticClass(), Composites);
			Module->GetClassCache()->GatherClasses(UBTTaskNode::StaticClass(), Tasks);
			FGraphContextMenuBuilder Builder(ScratchGraph);
			Builder.OwnerOfTemporaries = ScratchGraph;
			Schema->GetGraphContextActions(Builder);
			TestEqual(TEXT("stock main-node builder cardinality is composites plus tasks plus two"),
				Builder.GetNumActions(), Composites.Num() + Tasks.Num() + 2);
		}
	}
	const TOptional<FAssetDocumentOpaqueHandle> GraphHandle =
		FindOutcomeHandle(GraphCreate, TEXTVIEW("graph"));
	TestTrue(TEXT("graph creation returns one HG"), GraphHandle.IsSet());
	TestEqual(TEXT("scratch graph enlists cleanup action before complete footprint"),
		Transaction.CallOrder,
		TArray<FString>({TEXT("rollback_action"), TEXT("footprint")}));
	if (!GraphHandle.IsSet()) return false;
	if (ScratchGraph == nullptr) return false;

	auto RequireInvalidNodeActionPosition = [&](
		const TCHAR* Label, FAssetDocumentRuntimeValueDto X,
		FAssetDocumentRuntimeValueDto Y)
	{
		const FSideEffectCounts CountsBefore = CaptureSideEffectCounts(Handles, Transaction);
		const FScratchGraphActionState ScratchBefore =
			CaptureScratchGraphActionState(*ScratchGraph);
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			MakeBehaviorInvocation(TEXT("ai.behavior_tree.node.create_action.v1"),
				EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
				ObjectValue({TEXT("node_class_path"), TEXT("position")},
					{StringValue(*UBTComposite_Sequence::StaticClass()->GetPathName()),
						ObjectValue({TEXT("x"), TEXT("y")},
							{MoveTemp(X), MoveTemp(Y)})}),
				{{TEXT("graph"), GraphHandle.GetValue()}}),
			Handles, &Transaction);
		TestEqual(*FString::Printf(TEXT("%s fails before graph admission"), Label),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
		TestTrue(*FString::Printf(TEXT("%s uses only InvalidValueShape"), Label),
			HasOnlyDiagnostic(Outcome, InvalidValueShapeCode));
		TestTrue(*FString::Printf(TEXT("%s returns no value payload, HN or ChangedSet"), Label),
			Outcome.Value.Kind == EAssetDocumentRuntimeValueKind::Object
			&& Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty()
			&& Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty());
		RequireNoSideEffectDelta(*this, Label, CountsBefore, Handles, Transaction);
		RequireUnchangedScratchGraphActionState(*this, Label, ScratchBefore, *ScratchGraph);
		RequireUnchangedFixtureState(*this, Before, Fixture);
	};

	RequireInvalidNodeActionPosition(TEXT("fractional node X position"),
		NumberValue(320.5), NumberValue(80.0));
	RequireInvalidNodeActionPosition(TEXT("over-int32 number node X position"),
		NumberValue(static_cast<double>(MAX_int32) + 1.0), NumberValue(80.0));
	RequireInvalidNodeActionPosition(TEXT("over-int32 integer node X position"),
		IntegerValue(static_cast<int64>(MAX_int32) + 1), IntegerValue(80));
	RequireInvalidNodeActionPosition(TEXT("fractional node Y position"),
		NumberValue(320.0), NumberValue(80.5));
	RequireInvalidNodeActionPosition(TEXT("under-int32 integer node Y position"),
		IntegerValue(320), IntegerValue(static_cast<int64>(MIN_int32) - 1));

	const FSideEffectCounts BeforeOversizedComment =
		CaptureSideEffectCounts(Handles, Transaction);
	const FAssetDocumentPrimitiveOutcome OversizedComment = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.comment.create_action.v1"),
			EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
			ObjectValue({TEXT("bounds"), TEXT("text")},
				{ObjectValue({TEXT("h"), TEXT("w"), TEXT("x"), TEXT("y")},
					{NumberValue(120.0), NumberValue(240.0),
						NumberValue(static_cast<double>(MAX_int32)), NumberValue(160.0)}),
					StringValue(TEXT("out-of-range comment"))}),
			{{TEXT("graph"), GraphHandle.GetValue()}}),
		Handles, &Transaction);
	TestEqual(TEXT("out-of-range comment bounds fail before the public action"),
		OversizedComment.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("out-of-range comment uses InvalidValueShape"),
		HasDiagnostic(OversizedComment, InvalidValueShapeCode));
	RequireNoSideEffectDelta(*this, TEXT("out-of-range comment"),
		BeforeOversizedComment, Handles, Transaction);
	const FSideEffectCounts BeforeUnderflowComment =
		CaptureSideEffectCounts(Handles, Transaction);
	const FAssetDocumentPrimitiveOutcome UnderflowComment = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.comment.create_action.v1"),
			EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
			ObjectValue({TEXT("bounds"), TEXT("text")},
				{ObjectValue({TEXT("h"), TEXT("w"), TEXT("x"), TEXT("y")},
					{NumberValue(120.0), NumberValue(240.0),
						NumberValue(static_cast<double>(MIN_int32) - 1.0),
						NumberValue(160.0)}),
					StringValue(TEXT("underflow comment"))}),
			{{TEXT("graph"), GraphHandle.GetValue()}}),
		Handles, &Transaction);
	TestEqual(TEXT("underflow comment bounds fail before the public action"),
		UnderflowComment.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("underflow comment uses InvalidValueShape"),
		HasDiagnostic(UnderflowComment, InvalidValueShapeCode));
	RequireNoSideEffectDelta(*this, TEXT("underflow comment"),
		BeforeUnderflowComment, Handles, Transaction);

	FCountingTransaction RejectCommentIssueTransaction;
	FCountingHandleResolver RejectCommentIssueHandles(
		RejectCommentIssueTransaction.GetMutationToken());
	const FAssetDocumentPrimitiveOutcome RejectCommentGraph = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.graph.create.v1"),
			EAssetDocumentRuntimeRequestPhase::Apply,
			RejectCommentIssueTransaction.GetMutationToken(),
			ObjectValue({TEXT("behavior_tree_path"), TEXT("graph_name")},
				{StringValue(*LivePath),
					StringValue(TEXT("RejectCommentIssueGraph"))})),
		RejectCommentIssueHandles, &RejectCommentIssueTransaction);
	const TOptional<FAssetDocumentOpaqueHandle> RejectCommentGraphHandle =
		FindOutcomeHandle(RejectCommentGraph, TEXTVIEW("graph"));
	TestTrue(TEXT("comment issue-failure fixture creates one HG"),
		RejectCommentGraphHandle.IsSet());
	if (RejectCommentGraphHandle.IsSet())
	{
		RejectCommentIssueHandles.bRejectNextIssue = true;
		const FAssetDocumentPrimitiveOutcome RejectCommentIssue = Provider->Invoke(
			MakeBehaviorInvocation(TEXT("ai.behavior_tree.comment.create_action.v1"),
				EAssetDocumentRuntimeRequestPhase::Apply,
				RejectCommentIssueTransaction.GetMutationToken(),
				ObjectValue({TEXT("bounds"), TEXT("text")},
					{ObjectValue({TEXT("h"), TEXT("w"), TEXT("x"), TEXT("y")},
						{NumberValue(120.0), NumberValue(240.0),
							NumberValue(640.0), NumberValue(160.0)}),
						StringValue(TEXT("issue failure comment"))}),
				{{TEXT("graph"), RejectCommentGraphHandle.GetValue()}}),
			RejectCommentIssueHandles, &RejectCommentIssueTransaction);
		TestEqual(TEXT("comment HN issue failure reaches a stable terminal"),
			RejectCommentIssue.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
		TestTrue(TEXT("comment HN issue failure exposes no result or ChangedSet"),
			RejectCommentIssue.HandleResults.IsEmpty()
			&& RejectCommentIssue.ChangedSet.IsEmpty()
			&& HasDiagnostic(RejectCommentIssue,
				TEXT("AIProvider.BehaviorTree.CommentActionFailed")));
		TestTrue(TEXT("comment HN issue failure preserves the live closure"),
			RequireUnchangedFixtureState(*this, Before, Fixture));
		TestTrue(TEXT("comment HN issue failure rolls back its owned scratch bundle"),
			RejectCommentIssueTransaction.Rollback(
				TEXTVIEW("Test.CommentIssueFailure")).bValid);
		TestFalse(TEXT("comment HN issue failure releases its HG after rollback"),
			RejectCommentIssueHandles.IsAlive(RejectCommentGraphHandle.GetValue(),
				EAssetDocumentRuntimeRequestPhase::Apply));
		TestTrue(TEXT("comment HN issue rollback preserves the live closure"),
			RequireUnchangedFixtureState(*this, Before, Fixture));
	}

	const FAssetDocumentPrimitiveOutcome Defaults = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.graph.create_defaults.v1"),
			EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
			ObjectValue({}, {}), {{TEXT("graph"), GraphHandle.GetValue()}}),
		Handles, &Transaction);
	RequireSuccess(*this, TEXT("schema default creation succeeds"), Defaults);
	if (const FAssetDocumentRuntimeValueDto* RootId =
		FindField(Defaults.Value, TEXTVIEW("root_stable_node_id")))
	{
		if (ScratchGraph != nullptr)
			RequireScratchChangedSet(*this, TEXT("default root"), Defaults, LivePath,
				*ScratchGraph, {FindGraphNode(*ScratchGraph, RootId->StringValue)});
	}
	const TOptional<FAssetDocumentOpaqueHandle> RootHandle =
		FindOutcomeHandle(Defaults, TEXTVIEW("root_node"));
	TestTrue(TEXT("default creation returns one root HN"), RootHandle.IsSet());

	const FAssetDocumentPrimitiveOutcome WrongNodeClass = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.node.create_action.v1"),
			EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
			ObjectValue({TEXT("node_class_path"), TEXT("position")},
				{StringValue(*UBTDecorator_ForceSuccess::StaticClass()->GetPathName()),
					ObjectValue({TEXT("x"), TEXT("y")},
						{NumberValue(0.0), NumberValue(0.0)})}),
			{{TEXT("graph"), GraphHandle.GetValue()}}),
		Handles, &Transaction);
	TestEqual(TEXT("decorator cannot enter the main-node action family"),
		WrongNodeClass.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("wrong node family has the stable class mismatch diagnostic"),
		HasDiagnostic(WrongNodeClass,
			TEXT("AIProvider.BehaviorTree.ActionClassMismatch")));

	const FAssetDocumentPrimitiveOutcome NodeCreate = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.node.create_action.v1"),
			EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
			ObjectValue({TEXT("node_class_path"), TEXT("position")},
				{StringValue(*UBTComposite_Sequence::StaticClass()->GetPathName()),
					ObjectValue({TEXT("x"), TEXT("y")},
						{NumberValue(320.0), NumberValue(80.0)})}),
			{{TEXT("graph"), GraphHandle.GetValue()}}),
		Handles, &Transaction);
	RequireSuccess(*this, TEXT("public Sequence node action succeeds"), NodeCreate);
	UBehaviorTreeGraphNode* CreatedMainNode = nullptr;
	if (const FAssetDocumentRuntimeValueDto* NodeId =
		FindField(NodeCreate.Value, TEXTVIEW("stable_node_id")))
	{
		UBehaviorTreeGraphNode* ActualNode = ScratchGraph != nullptr
			? FindMainBehaviorNode(*ScratchGraph, NodeId->StringValue) : nullptr;
		CreatedMainNode = ActualNode;
		if (ScratchGraph != nullptr)
			RequireScratchChangedSet(*this, TEXT("main node"), NodeCreate, LivePath,
				*ScratchGraph, {ActualNode});
		TestTrue(TEXT("main action creates the exact requested instance class"),
			ActualNode != nullptr && ActualNode->NodeInstance != nullptr
			&& ActualNode->NodeInstance->GetClass() == UBTComposite_Sequence::StaticClass()
			&& ActualNode->NodePosX == 320 && ActualNode->NodePosY == 80);
	}
	const TOptional<FAssetDocumentOpaqueHandle> NodeHandle =
		FindOutcomeHandle(NodeCreate, TEXTVIEW("node"));
	TestTrue(TEXT("node action returns one HN"), NodeHandle.IsSet());

	if (NodeHandle.IsSet())
	{
		const TArray<FGuid> DecoratorPreimage = CreatedMainNode != nullptr
			? CaptureSubnodeOrder(*CreatedMainNode) : TArray<FGuid>();
		const FAssetDocumentPrimitiveOutcome DecoratorCreate = Provider->Invoke(
			MakeBehaviorInvocation(TEXT("ai.behavior_tree.subnode.create_action.v1"),
				EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
				ObjectValue({TEXT("node_class_path"), TEXT("subnode_kind")},
					{StringValue(*UBTDecorator_ForceSuccess::StaticClass()->GetPathName()),
						StringValue(TEXT("decorator"))}),
				{{TEXT("graph"), GraphHandle.GetValue()},
					{TEXT("parent_node"), NodeHandle.GetValue()}}),
			Handles, &Transaction);
		RequireSuccess(*this, TEXT("public decorator subnode action succeeds"),
			DecoratorCreate);
		if (const FAssetDocumentRuntimeValueDto* NodeId =
			FindField(DecoratorCreate.Value, TEXTVIEW("stable_node_id")))
		{
			const FAssetDocumentRuntimeValueDto* ParentId =
				FindField(DecoratorCreate.Value, TEXTVIEW("parent_stable_node_id"));
			UBehaviorTreeGraphNode* Parent = ScratchGraph != nullptr && ParentId != nullptr
				? FindMainBehaviorNode(*ScratchGraph, ParentId->StringValue) : nullptr;
			UAIGraphNode* Child = Parent != nullptr
				? FindSubnode(*Parent, NodeId->StringValue) : nullptr;
			if (ScratchGraph != nullptr)
				RequireScratchChangedSet(*this, TEXT("decorator subnode"),
					DecoratorCreate, LivePath, *ScratchGraph, {Child});
			if (Parent != nullptr)
				RequireOneAppendedSubnode(*this, TEXT("decorator subnode"), *Parent,
					DecoratorPreimage, Child);
			TestTrue(TEXT("decorator action creates the exact requested instance class"),
				Child != nullptr && Child->NodeInstance != nullptr
				&& Child->NodeInstance->GetClass() == UBTDecorator_ForceSuccess::StaticClass());
		}

		const TArray<FGuid> ServicePreimage = CreatedMainNode != nullptr
			? CaptureSubnodeOrder(*CreatedMainNode) : TArray<FGuid>();
		const FAssetDocumentPrimitiveOutcome ServiceCreate = Provider->Invoke(
			MakeBehaviorInvocation(TEXT("ai.behavior_tree.subnode.create_action.v1"),
				EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
				ObjectValue({TEXT("node_class_path"), TEXT("subnode_kind")},
					{StringValue(*UBTService_DefaultFocus::StaticClass()->GetPathName()),
						StringValue(TEXT("service"))}),
				{{TEXT("graph"), GraphHandle.GetValue()},
					{TEXT("parent_node"), NodeHandle.GetValue()}}),
			Handles, &Transaction);
		RequireSuccess(*this, TEXT("public service subnode action succeeds"),
			ServiceCreate);
		if (const FAssetDocumentRuntimeValueDto* NodeId =
			FindField(ServiceCreate.Value, TEXTVIEW("stable_node_id")))
		{
			const FAssetDocumentRuntimeValueDto* ParentId =
				FindField(ServiceCreate.Value, TEXTVIEW("parent_stable_node_id"));
			UBehaviorTreeGraphNode* Parent = ScratchGraph != nullptr && ParentId != nullptr
				? FindMainBehaviorNode(*ScratchGraph, ParentId->StringValue) : nullptr;
			UAIGraphNode* Child = Parent != nullptr
				? FindSubnode(*Parent, NodeId->StringValue) : nullptr;
			if (ScratchGraph != nullptr)
				RequireScratchChangedSet(*this, TEXT("service subnode"),
					ServiceCreate, LivePath, *ScratchGraph, {Child});
			if (Parent != nullptr)
				RequireOneAppendedSubnode(*this, TEXT("service subnode"), *Parent,
					ServicePreimage, Child);
			TestTrue(TEXT("service action creates the exact requested instance class"),
				Child != nullptr && Child->NodeInstance != nullptr
				&& Child->NodeInstance->GetClass() == UBTService_DefaultFocus::StaticClass());
		}
	}

	const FAssetDocumentPrimitiveOutcome CommentCreate = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.comment.create_action.v1"),
			EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
			ObjectValue({TEXT("bounds"), TEXT("text")},
				{ObjectValue({TEXT("h"), TEXT("w"), TEXT("x"), TEXT("y")},
					{NumberValue(120.0), NumberValue(240.0),
						NumberValue(640.0), NumberValue(160.0)}),
					StringValue(TEXT("P-AI scratch comment"))}),
			{{TEXT("graph"), GraphHandle.GetValue()}}),
		Handles, &Transaction);
	RequireSuccess(*this, TEXT("public comment action succeeds"), CommentCreate);
	if (const FAssetDocumentRuntimeValueDto* NodeId =
		FindField(CommentCreate.Value, TEXTVIEW("stable_node_id")))
	{
		if (ScratchGraph != nullptr)
			RequireScratchChangedSet(*this, TEXT("comment"), CommentCreate, LivePath,
				*ScratchGraph, {FindGraphNode(*ScratchGraph, NodeId->StringValue)});
	}

	if (RootHandle.IsSet() && NodeHandle.IsSet())
	{
		const FAssetDocumentPrimitiveOutcome MissingPin = Provider->Invoke(
			MakeBehaviorInvocation(TEXT("ai.behavior_tree.graph.connect.v1"),
				EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
				ObjectValue({TEXT("from_pin_name"), TEXT("to_pin_name")},
					{StringValue(TEXT("Missing")), StringValue(TEXT("In"))}),
				{{TEXT("graph"), GraphHandle.GetValue()},
					{TEXT("from_node"), RootHandle.GetValue()},
					{TEXT("to_node"), NodeHandle.GetValue()}}),
			Handles, &Transaction);
		TestEqual(TEXT("missing exact pin fails closed"), MissingPin.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed);
		TestTrue(TEXT("missing exact pin returns the frozen diagnostic"),
			HasDiagnostic(MissingPin, TEXT("AIProvider.BehaviorTree.PinMissing")));

		const FAssetDocumentPrimitiveOutcome Connect = Provider->Invoke(
			MakeBehaviorInvocation(TEXT("ai.behavior_tree.graph.connect.v1"),
				EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
				ObjectValue({TEXT("from_pin_name"), TEXT("to_pin_name")},
					{StringValue(TEXT("In")), StringValue(TEXT("In"))}),
				{{TEXT("graph"), GraphHandle.GetValue()},
					{TEXT("from_node"), RootHandle.GetValue()},
					{TEXT("to_node"), NodeHandle.GetValue()}}),
			Handles, &Transaction);
		RequireSuccess(*this, TEXT("stock schema exact-pin connection succeeds"),
			Connect);
		const FAssetDocumentRuntimeValueDto* FromId =
			FindField(Connect.Value, TEXTVIEW("from_stable_node_id"));
		const FAssetDocumentRuntimeValueDto* ToId =
			FindField(Connect.Value, TEXTVIEW("to_stable_node_id"));
		if (FromId != nullptr && ToId != nullptr)
		{
			if (ScratchGraph != nullptr)
				RequireScratchChangedSet(*this, TEXT("connection"), Connect, LivePath,
					*ScratchGraph, {FindGraphNode(*ScratchGraph, FromId->StringValue),
						FindGraphNode(*ScratchGraph, ToId->StringValue)});
		}
	}

	const FAssetDocumentPrimitiveOutcome InstallBlocked = Provider->Invoke(
		MakeBehaviorInvocation(TEXT("ai.behavior_tree.graph.install.v1"),
			EAssetDocumentRuntimeRequestPhase::Apply, Transaction.GetMutationToken(),
			ObjectValue({}, {})), Handles, &Transaction);
	TestEqual(TEXT("graph install rejects malformed input without installing"), InstallBlocked.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("graph install returns its closed shape diagnostic"),
		HasOnlyDiagnostic(InstallBlocked,
			TEXT("AIProvider.InvalidValueShape")));

	TestTrue(TEXT("all scratch operations preserve the live fixture closure"),
		RequireUnchangedFixtureState(*this, Before, Fixture));
	const FAssetDocumentContractValidationResult RollbackResult =
		Transaction.Rollback(TEXTVIEW("Test.ScratchRollback"));
	TestTrue(TEXT("scratch cleanup rollback succeeds"), RollbackResult.bValid);
	TestFalse(TEXT("scratch HG becomes invalid after rollback cleanup"),
		Handles.IsAlive(GraphHandle.GetValue(), EAssetDocumentRuntimeRequestPhase::Apply));
	if (RootHandle.IsSet())
	{
		TestFalse(TEXT("scratch root HN becomes invalid after rollback cleanup"),
			Handles.IsAlive(RootHandle.GetValue(),
				EAssetDocumentRuntimeRequestPhase::Apply));
	}
	TestTrue(TEXT("rollback still preserves the live fixture closure"),
		RequireUnchangedFixtureState(*this, Before, Fixture));
	return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
