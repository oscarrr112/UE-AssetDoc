// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentEditorStagedBehaviorTreeRead.h"
#include "AssetDocumentRollbackAction.h"
#include "BehaviorTree/BlackboardData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTreeFactory.h"
#include "UObject/Package.h"

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/ObjectWriter.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "AIGraphTypes.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "BehaviorTree/Decorators/BTDecorator_Cooldown.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/GarbageCollection.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "Editor.h"
#include "ScopedTransaction.h"

namespace AssetDocumentAIBehaviorTreeAuthoredGraphReadbackTests
{
constexpr TCHAR PrimitiveId[] = TEXT("ai.behavior_tree.authored_graph.readback.v1");

// A registered, unsaved test asset lets the real Editor resolver issue its own
// native payload. No Editor-private identity interface is copied into the test.
struct FRegisteredReadbackTree
{
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UBehaviorTree> Tree;
	FRegisteredReadbackTree()
	{
		Package.Reset(CreatePackage(*(TEXT("/Game/AssetDocumentTests/Readback_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		Tree.Reset(Cast<UBehaviorTree>(GetMutableDefault<UBehaviorTreeFactory>()->FactoryCreateNew(
			UBehaviorTree::StaticClass(), Package.Get(), TEXT("Tree"),
			RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn)));
		if (Tree.IsValid()) FAssetRegistryModule::AssetCreated(Tree.Get());
	}
	~FRegisteredReadbackTree()
	{
		if (Tree.IsValid())
		{
			FAssetRegistryModule::AssetDeleted(Tree.Get());
			Tree->ClearFlags(RF_Public | RF_Standalone);
		}
		if (Package.IsValid()) Package->SetDirtyFlag(false);
	}
};

UEdGraphPin* OutputPin(UEdGraphNode& Node)
{
	for (UEdGraphPin* Pin : Node.Pins)
		if (Pin != nullptr && Pin->Direction == EGPD_Output) return Pin;
	return nullptr;
}

UBehaviorTreeGraphNode_Task* InstallTestGraph(UBehaviorTree& Tree, bool bIncludeBoundExpression = true)
{
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(
		&Tree, NAME_None, UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass()));
	if (Graph == nullptr) return nullptr;
	// Fixture setup only: this does not exercise or admit the blocked INSTALL primitive.
	Tree.BTGraph = Graph;
	Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
	Graph->OnCreated();
	Graph->Initialize();
	UBehaviorTreeGraphNode_Root* Root = Graph->Nodes.Num() == 1
		? Cast<UBehaviorTreeGraphNode_Root>(Graph->Nodes[0]) : nullptr;
	if (Root == nullptr || OutputPin(*Root) == nullptr) return nullptr;
	FAISchemaAction_NewNode CompositeAction;
	CompositeAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Composite>();
	CompositeAction.NodeTemplate->NodeInstance = NewObject<UBTComposite_Sequence>(CompositeAction.NodeTemplate);
	UEdGraphNode* Composite = CompositeAction.PerformAction(Graph, OutputPin(*Root), FVector2f(0, 100), false);
	if (Composite == nullptr || OutputPin(*Composite) == nullptr) return nullptr;
	FAISchemaAction_NewNode TaskAction;
	TaskAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Task>();
	TaskAction.NodeTemplate->NodeInstance = NewObject<UBTTask_Wait>(TaskAction.NodeTemplate);
	UBehaviorTreeGraphNode_Task* Task = Cast<UBehaviorTreeGraphNode_Task>(TaskAction.PerformAction(
		Graph, OutputPin(*Composite), FVector2f(0, 200), false));
	if (Task == nullptr) return nullptr;
	FAISchemaAction_NewSubNode DecoratorAction;
	DecoratorAction.ParentNode = Task;
	DecoratorAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Decorator>();
	DecoratorAction.NodeTemplate->NodeInstance = NewObject<UBTDecorator_Cooldown>(DecoratorAction.NodeTemplate);
	DecoratorAction.PerformAction(Graph, nullptr, FVector2f::ZeroVector, false);
	FAISchemaAction_NewSubNode ServiceAction;
	ServiceAction.ParentNode = Task;
	ServiceAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Service>();
	ServiceAction.NodeTemplate->NodeInstance = NewObject<UBTService_DefaultFocus>(ServiceAction.NodeTemplate);
	ServiceAction.PerformAction(Graph, nullptr, FVector2f::ZeroVector, false);
	if (bIncludeBoundExpression)
	{
		// Keep the intermediate empty expression out of runtime rebuilding. Complete
		// the stock bound graph with one test feeding its default sink, then rebuild.
		Graph->LockUpdates();
		const bool bBoundReady = [&]()
		{
			FAISchemaAction_NewSubNode BoundAction;
			BoundAction.ParentNode = Task;
			UBehaviorTreeGraphNode_CompositeDecorator* BoundOwner = NewObject<UBehaviorTreeGraphNode_CompositeDecorator>();
			BoundAction.NodeTemplate = BoundOwner;
			BoundAction.PerformAction(Graph, nullptr, FVector2f::ZeroVector, false);
			UEdGraph* BoundGraph = BoundOwner->BoundGraph;
			if (BoundGraph == nullptr || BoundGraph->Nodes.Num() != 1) return false;
			UEdGraphPin* SinkInput = nullptr;
			for (UEdGraphPin* Pin : BoundGraph->Nodes[0]->Pins)
				if (Pin != nullptr && Pin->Direction == EGPD_Input) SinkInput = Pin;
			if (SinkInput == nullptr) return false;
			FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Decorator> Creator(*BoundGraph);
			UBehaviorTreeDecoratorGraphNode_Decorator* TestNode = Creator.CreateNode(false);
			TestNode->ClassData = FGraphNodeClassData(UBTDecorator_Cooldown::StaticClass(), FString());
			Creator.Finalize();
			UEdGraphPin* TestOutput = OutputPin(*TestNode);
			return TestNode->NodeInstance != nullptr && TestOutput != nullptr
				&& BoundGraph->GetSchema()->TryCreateConnection(TestOutput, SinkInput);
		}();
		Graph->UnlockUpdates();
		if (!bBoundReady) return nullptr;
	}

	FAISchemaAction_AddComment CommentAction;
	UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(CommentAction.PerformAction(Graph, nullptr, FVector2f(300, 400), false));
	if (Comment == nullptr) return nullptr;
	Comment->NodeComment = TEXT("authored readback comment");
	Comment->FontSize = 21;
	return Task;
}

class FRejectingHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind,
		EAssetDocumentHandleAccess,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&&) override
	{
		return {};
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle&,
		const FAssetDocumentHandleValidationContext&) override
	{
		return {};
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation&) override
	{
		return {};
	}
};

class FReadbackHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	class FLease final : public IAssetDocumentVerifiedHandleLease
	{
	public:
		FLease(FAssetDocumentOpaqueHandle InHandle, IAssetDocumentNativeHandlePayload& InPayload)
			: Handle(MoveTemp(InHandle)), Payload(&InPayload) {}
		virtual const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
		virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return *Payload; }
	private:
		FAssetDocumentOpaqueHandle Handle;
		IAssetDocumentNativeHandlePayload* Payload;
	};

	FReadbackHandleResolver(const FAssetDocumentMutationToken& InToken, EAssetDocumentRuntimeRequestPhase InPhase)
		: Token(InToken), Phase(InPhase) {}
	int32 IssueCalls = 0;
	int32 LeaseCalls = 0;
	int32 PhaseMismatchCalls = 0;
	virtual FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access, TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		++IssueCalls;
		FAssetDocumentIssuedHandleResult Result;
		if (!Payload || !Payload->IsAlive()) return Result;
		FEntry Entry;
		Entry.Payload = MakeShareable(Payload.Release());
		Entry.Handle.RequestId = Token.RequestId;
		Entry.Handle.TransactionId = Token.TransactionId;
		Entry.Handle.RuntimeRevision = Token.RuntimeRevision;
		Entry.Handle.ProviderId = TEXT("editor");
		Entry.Handle.Slot = Entries.Num() + 1;
		Entry.Handle.Generation = 1;
		Entry.Handle.Kind = Kind;
		Entry.Handle.Access = Access;
		Result.Handle = Entry.Handle;
		Entries.Add(MoveTemp(Entry));
		return Result;
	}

	// Test setup only, including Verify: this is not the production handoff.
	TOptional<FAssetDocumentOpaqueHandle> AliasForAI(const FAssetDocumentOpaqueHandle& Source,
		EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::ReadOnly)
	{
		const int32 Index = static_cast<int32>(Source.Slot) - 1;
		if (!Entries.IsValidIndex(Index) || Entries[Index].Handle.ProviderId != TEXT("editor")
			|| Source.Kind != EAssetDocumentHandleKind::Asset
			|| Source.Access != Access) return {};
		FEntry Alias = Entries[Index];
		Alias.Handle.ProviderId = TEXT("ai");
		Alias.Handle.Slot = Entries.Num() + 1;
		const FAssetDocumentOpaqueHandle Result = Alias.Handle;
		Entries.Add(MoveTemp(Alias));
		return Result;
	}
	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& InHandle,
		const FAssetDocumentHandleValidationContext& Context) override
	{
		++LeaseCalls;
		FAssetDocumentVerifiedLeaseResult Result;
		if (Context.Phase != Phase)
		{
			++PhaseMismatchCalls;
			return Result;
		}
		const FAssetDocumentContractValidationResult Metadata = ValidateAssetDocumentHandleMetadata(InHandle, Context);
		if (!Metadata.bValid) { Result.Diagnostics = Metadata.Diagnostics; return Result; }
		const int32 Index = static_cast<int32>(InHandle.Slot) - 1;
		if (!Entries.IsValidIndex(Index)) return Result;
		FEntry& Entry = Entries[Index];
		if (InHandle.RequestId == Entry.Handle.RequestId && InHandle.TransactionId == Entry.Handle.TransactionId
			&& InHandle.RuntimeRevision == Entry.Handle.RuntimeRevision && InHandle.Generation == Entry.Handle.Generation
			&& InHandle.Kind == Entry.Handle.Kind && InHandle.Access == Entry.Handle.Access
			&& InHandle.ProviderId == Entry.Handle.ProviderId && Entry.Payload.IsValid() && Entry.Payload->IsAlive())
			Result.Lease = MakeUnique<FLease>(InHandle, *Entry.Payload);
		return Result;
	}
	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation&) override { return {}; }
private:
	struct FEntry
	{
		FAssetDocumentOpaqueHandle Handle;
		TSharedPtr<IAssetDocumentNativeHandlePayload> Payload;
	};
	FAssetDocumentMutationToken Token;
	EAssetDocumentRuntimeRequestPhase Phase;
	TArray<FEntry> Entries;
};

class FReadbackTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	FReadbackTransaction()
	{
		Token.RequestId = FGuid::NewGuid();
		Token.TransactionId = FGuid::NewGuid();
		Token.RuntimeRevision = 1;
	}
	int32 SideEffectCalls = 0;
	virtual FGuid GetTransactionId() const override { return Token.TransactionId; }
	virtual EAssetDocumentAtomicState GetState() const override { return EAssetDocumentAtomicState::Live; }
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
	virtual FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint>) override { ++SideEffectCalls; return {}; }
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override { ++SideEffectCalls; return {}; }
	virtual FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { ++SideEffectCalls; return {}; }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView) override { ++SideEffectCalls; return {}; }
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction>) override { ++SideEffectCalls; return {}; }
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override { ++SideEffectCalls; return {}; }
private:
	FAssetDocumentMutationToken Token;
};

FAssetDocumentContractValidationResult SetupValid()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

class FReadbackSetupTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	FReadbackSetupTransaction()
	{
		Token.RequestId = FGuid::NewGuid();
		Token.TransactionId = FGuid::NewGuid();
		Token.RuntimeRevision = 1;
	}
	virtual FGuid GetTransactionId() const override { return Token.TransactionId; }
	virtual EAssetDocumentAtomicState GetState() const override { return State; }
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		Order.Add(TEXT("action"));
		Actions.Add(MoveTemp(Action));
		return SetupValid();
	}
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer) override
	{
		Order.Add(TEXT("finalizer"));
		Finalizers.Add(MoveTemp(Finalizer));
		return SetupValid();
	}
	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override
	{
		for (const FAssetDocumentMutationFootprint& Item : Footprint)
		{
			const bool bRuntime = Item.RollbackComponentId.Contains(TEXT("runtime_mirror"));
			const bool bCovered = bRuntime
				? Finalizers.ContainsByPredicate([&Item](const TSharedRef<IAssetDocumentPostRollbackFinalizer>& Value)
					{ return Value->GetRollbackComponentId() == Item.RollbackComponentId; })
				: Actions.ContainsByPredicate([&Item](const TSharedRef<IAssetDocumentRollbackAction>& Value)
					{ return Value->GetRollbackComponentId() == Item.RollbackComponentId; });
			if (!bCovered)
			{
				FAssetDocumentContractValidationResult Result;
				FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
				Diagnostic.Code = TEXT("Test.FootprintBeforeRollbackCoverage");
				return Result;
			}
		}
		Order.Add(TEXT("footprint"));
		Footprints.Append(Footprint.GetData(), Footprint.Num());
		State = EAssetDocumentAtomicState::Mutating;
		return SetupValid();
	}
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override { return SetupValid(); }
	virtual FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return SetupValid(); }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView Code) override
	{
		FAssetDocumentContractValidationResult Result = SetupValid();
		for (int32 Index = Actions.Num() - 1; Index >= 0; --Index)
		{
			Order.Add(TEXT("rollback-action"));
			if (!Actions[Index]->Rollback(Code).bValid) Result.bValid = false;
		}
		for (int32 Index = 0; Index < Finalizers.Num(); ++Index)
		{
			Order.Add(TEXT("rollback-finalizer"));
			if (!Finalizers[Index]->FinalizePostRollback(Code).bValid) Result.bValid = false;
		}
		State = Result.bValid ? EAssetDocumentAtomicState::RolledBack : EAssetDocumentAtomicState::Poisoned;
		return Result;
	}

	TArray<FString> Order;
	TArray<FAssetDocumentMutationFootprint> Footprints;

private:
	FAssetDocumentMutationToken Token;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Live;
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
	TArray<TSharedRef<IAssetDocumentPostRollbackFinalizer>> Finalizers;
};


FAssetDocumentRuntimeValueDto StringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto ReadbackRequest(const UBehaviorTree& Tree)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = {TEXT("behavior_tree_path")};
	Result.Elements = {StringValue(Tree.GetPathName())};
	return Result;
}

FAssetDocumentPrimitiveInvocation ReadbackInvocation(
	EAssetDocumentRuntimeRequestPhase Phase, const FAssetDocumentMutationToken& Token,
	const UBehaviorTree& Tree, const FAssetDocumentOpaqueHandle& Handle)
{
	FAssetDocumentPrimitiveInvocation Result;
	Result.PrimitiveId = PrimitiveId;
	Result.Version = 1;
	Result.RequestId = Token.RequestId;
	Result.TransactionId = Token.TransactionId;
	Result.RuntimeRevision = Token.RuntimeRevision;
	Result.Phase = Phase;
	Result.ValueArguments = ReadbackRequest(Tree);
	Result.HandleArguments = {{TEXT("asset"), Handle}};
	return Result;
}

const FAssetDocumentRuntimeValueDto* FindField(const FAssetDocumentRuntimeValueDto& Object, FStringView Name)
{
	for (int32 Index = 0; Index < Object.ObjectKeys.Num() && Index < Object.Elements.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Name) return &Object.Elements[Index];
	}
	return nullptr;
}

void CheckApplyPreimage(FAutomationTestBase& Test, const UBehaviorTree& Tree,
	const FAssetDocumentRuntimeValueDto& Readback)
{
	// Same-dispatch projection into the frozen INSTALL input schema. This
	// transient DTO never invokes INSTALL or survives the current observation.
	const auto* Guid = FindField(Readback, TEXT("graph_guid"));
	if (!Test.TestNotNull(TEXT("Apply has a complete graph preimage"), Guid)) return;
	FAssetDocumentRuntimeValueDto Preimage;
	Preimage.Kind = EAssetDocumentRuntimeValueKind::Object;
	Preimage.ObjectKeys = {TEXT("behavior_tree_path"), TEXT("expected_current_graph_guid_or_null"), TEXT("rollback_completion_mode")};
	Preimage.Elements = {StringValue(Tree.GetPathName()), *Guid, StringValue(TEXT("ValueOrDecoratorMirror"))};
	Test.TestTrue(TEXT("same-Apply install preimage is a canonical RuntimeValue"), ValidateAssetDocumentRuntimeValue(Preimage).bValid);
	Test.TestTrue(TEXT("same-Apply preimage matches the current graph GUID/null"), Tree.BTGraph == nullptr
		? Preimage.Elements[1].Kind == EAssetDocumentRuntimeValueKind::Null
		: Preimage.Elements[1].StringValue == Tree.BTGraph->GraphGuid.ToString(EGuidFormats::Digits));
}

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const IAssetDocumentPrimitiveProvider& Provider)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider.DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == PrimitiveId && Descriptor.Version == 1)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

bool HasDiagnostic(const FAssetDocumentPrimitiveOutcome& Outcome, FStringView Code)
{
	return Outcome.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code;
	});
}
TOptional<FAssetDocumentOpaqueHandle> ResolveTestAlias(FAutomationTestBase& Test, UBehaviorTree& Tree,
	const FAssetDocumentMutationToken& Token, EAssetDocumentRuntimeRequestPhase Phase,
	FReadbackHandleResolver& Handles, EAssetDocumentHandleAccess Access)
{
	FAssetDocumentPrimitiveInvocation Resolve;
	Resolve.PrimitiveId = TEXT("editor.asset.resolve_existing.v1");
	Resolve.Version = 1;
	Resolve.RequestId = Token.RequestId;
	Resolve.TransactionId = Token.TransactionId;
	Resolve.RuntimeRevision = Token.RuntimeRevision;
	Resolve.Phase = Phase;
	Resolve.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	Resolve.ValueArguments.ObjectKeys = {TEXT("expected_class"), TEXT("path")};
	Resolve.ValueArguments.Elements = {StringValue(Tree.GetClass()->GetPathName()), StringValue(Tree.GetPathName())};
	const auto Resolved = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(Resolve, Handles, nullptr);
	if (!Test.TestEqual(TEXT("phase-local Editor setup resolves exact asset"), Resolved.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return {};
	const FString GroupName = Access == EAssetDocumentHandleAccess::Mutable ? TEXT("asset_mutable") : TEXT("asset_read");
	for (const auto& Group : Resolved.HandleResults)
		if (Group.Name == GroupName && Group.Handles.Num() == 1) return Handles.AliasForAI(Group.Handles[0], Access);
	return {};
}

// Declared before the fixture so all phase leases, captured rollback actions,
// row generators and strong asset references have been released before cleanup.
// Stock Root placement scans every loaded Blackboard, including unsaved assets.
class FReadbackBlackboardCleanup
{
public:
	explicit FReadbackBlackboardCleanup(FAutomationTestBase& InTest)
		: Test(InTest), EditorTransaction(TEXT("AssetDocumentReadbackFixture"),
			FText::FromString(TEXT("Readback test fixture setup")), nullptr) {}
	void Track(UBlackboardData* Blackboard) { Blackboards.Add(Blackboard); }
	~FReadbackBlackboardCleanup()
	{
		// The helper admits only a top-level transaction. Stock graph actions
		// nest into this one record; Cancel drops it and restores prior history.
		EditorTransaction.Cancel();
		Test.TestFalse(TEXT("fixture Editor transaction is no longer active"), GEditor->IsTransactionActive());
		for (const TWeakObjectPtr<UBlackboardData>& Weak : Blackboards)
		{
			if (UBlackboardData* Blackboard = Weak.Get())
			{
				Blackboard->ClearFlags(RF_Public | RF_Standalone);
			}
		}
		CollectGarbage(RF_NoFlags, true);
		for (const TWeakObjectPtr<UBlackboardData>& Weak : Blackboards)
		{
			UBlackboardData* Remaining = Weak.GetEvenIfUnreachable();
			if (Remaining != nullptr)
			{
				Test.AddInfo(FString::Printf(TEXT("Readback cleanup retained %s flags=%u internal=%u rooted=%d garbage-elimination=%d"),
					*Remaining->GetPathName(), static_cast<uint32>(Remaining->GetFlags()),
					static_cast<uint32>(Remaining->GetInternalFlags()), Remaining->IsRooted(), UObject::IsGarbageEliminationEnabled()));
			}
			Test.TestTrue(TEXT("test-owned Blackboard is reclaimed after all fixture references release"), Remaining == nullptr);
		}
	}
private:
	FAutomationTestBase& Test;
	FScopedTransaction EditorTransaction;
	TArray<TWeakObjectPtr<UBlackboardData>> Blackboards;
};

bool TestRepairAfterBlackboardApply(FAutomationTestBase& Test, IAssetDocumentPrimitiveProvider& Provider)
{
	// UTransBuffer cannot cancel a partial nested transaction. Never place this
	// test's cancelable scope inside an external Editor transaction.
	if (!Test.TestTrue(TEXT("fixture can own an isolated Editor transaction"),
		GEditor != nullptr && GEditor->CanTransact() && !GEditor->IsTransactionActive())) return false;
	FReadbackBlackboardCleanup Cleanup(Test);
	FRegisteredReadbackTree Fixture;
	UBehaviorTree* Tree = Fixture.Tree.Get();
	if (!Test.TestNotNull(TEXT("Repair setup owns a factory tree"), Tree)) return false;
	// Existing blackboard.set.v1 admits ordinary instantiated subnodes only.
	// The four-phase readback cases separately retain composite-bound coverage.
	UBehaviorTreeGraphNode_Task* Task = InstallTestGraph(*Tree, false);
	if (!Test.TestNotNull(TEXT("Repair setup owns a complete authored tree"), Task)) return false;
	TStrongObjectPtr<UBlackboardData> Blackboard(NewObject<UBlackboardData>(Fixture.Package.Get(), TEXT("RepairBlackboard"), RF_Public | RF_Transactional));
	Cleanup.Track(Blackboard.Get());
	UBehaviorTreeGraphNode_Root* Root = Cast<UBehaviorTreeGraphNode_Root>(Tree->BTGraph->Nodes[0]);
	if (!Test.TestNotNull(TEXT("Repair setup has its native Root"), Root)) return false;
	TStrongObjectPtr<UBlackboardData> InitialBlackboard(NewObject<UBlackboardData>(Fixture.Package.Get(), TEXT("InitialBlackboard"), RF_Public | RF_Transactional));
	Cleanup.Track(InitialBlackboard.Get());
	Root->BlackboardAsset = InitialBlackboard.Get();
	Root->UpdateBlackboard();
	Test.TestTrue(TEXT("Root initializes the exact pre-Apply Blackboard closure"), Tree->BlackboardAsset == InitialBlackboard.Get());
	const UBlackboardData* OriginalBlackboard = Tree->BlackboardAsset;
	FReadbackSetupTransaction Transaction;
	const auto& Token = Transaction.GetMutationToken();
	bool bApplied = false;
	{
		FReadbackHandleResolver ApplyHandles(Token, EAssetDocumentRuntimeRequestPhase::Apply);
		const auto ApplyAsset = ResolveTestAlias(Test, *Tree, Token, EAssetDocumentRuntimeRequestPhase::Apply,
			ApplyHandles, EAssetDocumentHandleAccess::Mutable);
		if (!Test.TestTrue(TEXT("Apply owns a test mutable alias"), ApplyAsset.IsSet())) return false;
		auto Apply = ReadbackInvocation(EAssetDocumentRuntimeRequestPhase::Apply, Token, *Tree, ApplyAsset.GetValue());
		Apply.PrimitiveId = TEXT("ai.behavior_tree.blackboard.set.v1");
		Apply.ValueArguments.ObjectKeys = {TEXT("behavior_tree_path"), TEXT("blackboard_path_or_null")};
		Apply.ValueArguments.Elements = {StringValue(Tree->GetPathName()), StringValue(Blackboard->GetPathName())};
		const auto Applied = Provider.Invoke(Apply, ApplyHandles, &Transaction);
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Applied.Diagnostics)
			Test.AddInfo(FString::Printf(TEXT("Blackboard Apply diagnostic: %s | %s | %s"),
				*Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
		bApplied = Test.TestEqual(TEXT("real Blackboard Apply rewrite succeeds"), Applied.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		Test.TestEqual(TEXT("Blackboard Apply reports its one authored change"), Applied.ChangedSet.Num(), 1);
	}
	// Apply registry and values are gone; Repair signs a new local payload.
	if (bApplied)
	{
		Test.TestTrue(TEXT("Apply completed native Root/tree Blackboard closure"), Root->BlackboardAsset == Blackboard.Get() && Tree->BlackboardAsset == Blackboard.Get());
		FReadbackHandleResolver RepairHandles(Token, EAssetDocumentRuntimeRequestPhase::Repair);
		const auto RepairAsset = ResolveTestAlias(Test, *Tree, Token, EAssetDocumentRuntimeRequestPhase::Repair,
			RepairHandles, EAssetDocumentHandleAccess::ReadOnly);
		if (Test.TestTrue(TEXT("Repair owns a newly issued read-only alias"), RepairAsset.IsSet()))
		{
			const int32 BeforeActions = Transaction.Order.Num();
			const int32 BeforeFootprints = Transaction.Footprints.Num();
			const int32 BeforeIssues = RepairHandles.IssueCalls;
			const bool bDirty = Fixture.Package->IsDirty();
			const auto Repair = Provider.Invoke(ReadbackInvocation(EAssetDocumentRuntimeRequestPhase::Repair,
				Token, *Tree, RepairAsset.GetValue()), RepairHandles, &Transaction);
			Test.TestEqual(TEXT("Repair observes the tree after real Blackboard Apply"), Repair.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
			const auto* State = FindField(Repair.Value, TEXT("graph_state"));
			const auto* ProjectionRoot = FindField(Repair.Value, TEXT("root"));
			const auto* Children = ProjectionRoot ? FindField(*ProjectionRoot, TEXT("children")) : nullptr;
			Test.TestTrue(TEXT("post-Apply Repair projection remains complete"), State && State->StringValue == TEXT("present") && Children && Children->Elements.Num() == 1);
			Test.TestTrue(TEXT("Repair itself has no mutation results"), Repair.HandleResults.IsEmpty() && Repair.ChangedSet.IsEmpty());
			Test.TestEqual(TEXT("Repair adds no Atomic action/finalizer"), Transaction.Order.Num(), BeforeActions);
			Test.TestEqual(TEXT("Repair adds no footprint"), Transaction.Footprints.Num(), BeforeFootprints);
			Test.TestEqual(TEXT("Repair issues no handles"), RepairHandles.IssueCalls, BeforeIssues);
			Test.TestEqual(TEXT("Repair preserves dirty state"), Fixture.Package->IsDirty(), bDirty);
		}
	}
	Test.TestTrue(TEXT("setup mutation rolls back through captured typed actions"), Transaction.Rollback(TEXT("ReadbackTest.Cleanup")).bValid);
	Test.TestTrue(TEXT("rollback restores exact original Blackboard"), Tree->BlackboardAsset == OriginalBlackboard && Root->BlackboardAsset == OriginalBlackboard);
	return bApplied;
}

} // namespace AssetDocumentAIBehaviorTreeAuthoredGraphReadbackTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIBehaviorTreeAuthoredGraphReadbackDescriptorTest,
	"UEAssetDoc.AIProvider.BehaviorTree.AuthoredGraphReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBehaviorTreeAuthoredGraphReadbackDescriptorTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeAuthoredGraphReadbackTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(*Provider);
	if (!TestNotNull(TEXT("D2 registers the authored-graph readback descriptor"), Descriptor))
	{
		FAssetDocumentPrimitiveInvocation MissingInvocation;
		MissingInvocation.PrimitiveId = PrimitiveId;
		MissingInvocation.Version = 1;
		MissingInvocation.RequestId = FGuid::NewGuid();
		MissingInvocation.RuntimeRevision = 1;
		MissingInvocation.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
		FRejectingHandleResolver Handles;
		const FAssetDocumentPrimitiveOutcome Missing = Provider->Invoke(
			MissingInvocation, Handles, nullptr);
		TestTrue(TEXT("a missing primitive is the exact missing-provider terminal"),
			HasDiagnostic(Missing, TEXT("GenericProvider.UnknownPrimitive")));
		TestTrue(TEXT("a missing primitive has no value, handle, or ChangedSet lanes"),
			Missing.HandleResults.IsEmpty() && Missing.ChangedSet.IsEmpty());
		return false;
	}

	TestEqual(TEXT("readback is version one"), Descriptor->Version, 1u);
	TestEqual(TEXT("readback is read-only"), Descriptor->Access,
		EAssetDocumentPrimitiveAccess::ReadOnly);
	TestEqual(TEXT("readback admits the four frozen phases"), Descriptor->AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>{
			EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply,
			EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify});
	TestFalse(TEXT("readback never requires an active transaction"),
		Descriptor->bRequiresActiveTransaction);
	TestEqual(TEXT("readback requires exactly one AI-owned read-only Asset"),
		Descriptor->HandleArgumentSchemas.Num(), 1);
	if (Descriptor->HandleArgumentSchemas.Num() == 1)
	{
		const FAssetDocumentPrimitiveHandleArgumentSchema& Asset =
			Descriptor->HandleArgumentSchemas[0];
		TestEqual(TEXT("readback asset argument name"), Asset.Name, FString(TEXT("asset")));
		TestEqual(TEXT("readback asset argument kind"), Asset.Kind,
			EAssetDocumentHandleKind::Asset);
		TestEqual(TEXT("readback asset argument access"), Asset.RequiredAccess,
			EAssetDocumentHandleAccess::ReadOnly);
	}
	TestTrue(TEXT("readback never returns handles or a mutation footprint"),
		Descriptor->HandleResultSchemas.IsEmpty()
		&& Descriptor->MutationFootprintSchemaId.IsEmpty());

	FReadbackTransaction Transaction;
	for (const EAssetDocumentRuntimeRequestPhase Phase : {
		EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Apply,
		EAssetDocumentRuntimeRequestPhase::Repair, EAssetDocumentRuntimeRequestPhase::Verify})
	{
		// A new factory asset per dispatch prevents Initialize from reconstructing
		// a previous phase's runtime mirror into a replacement authored graph.
		FRegisteredReadbackTree Fixture;
		UBehaviorTree* const Tree = Fixture.Tree.Get();
		if (!TestNotNull(TEXT("registered factory tree exists"), Tree)) return false;
		TestNull(TEXT("factory tree starts with absent authored graph"), Tree->BTGraph);
		TestNull(TEXT("factory tree has no previous-phase runtime root"), Tree->RootNode);
		const bool bMutationPhase = Phase == EAssetDocumentRuntimeRequestPhase::Apply
			|| Phase == EAssetDocumentRuntimeRequestPhase::Repair;
		FAssetDocumentMutationToken Token = Transaction.GetMutationToken();
		if (!bMutationPhase)
		{
			// Native bootstrap allocates a valid HandleScopeId for a no-Atomic
			// dispatch; it is independent of the active mutation transaction.
			Token.TransactionId = FGuid::NewGuid();
		}
		// Each dispatch owns an independent registry and newly issued native payload.
		FReadbackHandleResolver Handles(Token, Phase);
		FAssetDocumentPrimitiveInvocation Resolve;
		Resolve.PrimitiveId = TEXT("editor.asset.resolve_existing.v1");
		Resolve.Version = 1;
		Resolve.RequestId = Token.RequestId;
		Resolve.TransactionId = Token.TransactionId;
		Resolve.RuntimeRevision = Token.RuntimeRevision;
		Resolve.Phase = Phase;
		Resolve.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
		Resolve.ValueArguments.ObjectKeys = {TEXT("expected_class"), TEXT("path")};
		Resolve.ValueArguments.Elements = {StringValue(Tree->GetClass()->GetPathName()), StringValue(Tree->GetPathName())};
		const FAssetDocumentPrimitiveOutcome Resolved = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(Resolve, Handles, nullptr);
		if (!TestEqual(TEXT("real Editor resolver succeeds"), Resolved.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
		TOptional<FAssetDocumentOpaqueHandle> Handle;
		for (const FAssetDocumentPrimitiveHandleResult& Group : Resolved.HandleResults)
			if (Group.Name == TEXT("asset_read") && Group.Handles.Num() == 1) Handle = Handles.AliasForAI(Group.Handles[0]);
		if (!TestTrue(TEXT("test-only AI alias retains real Editor-issued read-only payload"), Handle.IsSet())) return false;
		const int32 SetupIssueCalls = Handles.IssueCalls;
		const FAssetDocumentPrimitiveInvocation Invocation = ReadbackInvocation(Phase, Token, *Tree, Handle.GetValue());
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(Invocation, Handles, bMutationPhase ? &Transaction : nullptr);
		TestEqual(TEXT("each frozen phase reaches real authored readback"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		const FAssetDocumentRuntimeValueDto* State = FindField(Outcome.Value, TEXT("graph_state"));
		const FAssetDocumentRuntimeValueDto* Root = FindField(Outcome.Value, TEXT("root"));
		const FAssetDocumentRuntimeValueDto* AbsentGuid = FindField(Outcome.Value, TEXT("graph_guid"));
		const FAssetDocumentRuntimeValueDto* AbsentComments = FindField(Outcome.Value, TEXT("comments"));
		TestTrue(TEXT("an absent graph is a complete readback observation"), State != nullptr
			&& State->Kind == EAssetDocumentRuntimeValueKind::String && State->StringValue == TEXT("absent")
			&& Root != nullptr && Root->Kind == EAssetDocumentRuntimeValueKind::Null
			&& AbsentGuid != nullptr && AbsentGuid->Kind == EAssetDocumentRuntimeValueKind::Null
			&& AbsentComments != nullptr && AbsentComments->Kind == EAssetDocumentRuntimeValueKind::Array
			&& AbsentComments->Elements.IsEmpty());
		TestTrue(TEXT("readback has no handle or ChangedSet result lane"), Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty());

		if (Phase == EAssetDocumentRuntimeRequestPhase::Apply) CheckApplyPreimage(*this, *Tree, Outcome.Value);
		UBehaviorTreeGraphNode_Task* Task = InstallTestGraph(*Tree);
		if (!TestNotNull(TEXT("stock schema/action creates a present graph with a Wait task"), Task)) return false;
		const bool bDirtyBefore = Fixture.Package->IsDirty();
		const FAssetDocumentPrimitiveOutcome Present = Provider->Invoke(Invocation, Handles, bMutationPhase ? &Transaction : nullptr);
		TestEqual(TEXT("present authored graph succeeds in each phase"), Present.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		const FAssetDocumentRuntimeValueDto* PresentState = FindField(Present.Value, TEXT("graph_state"));
		const FAssetDocumentRuntimeValueDto* PresentRoot = FindField(Present.Value, TEXT("root"));
		const FAssetDocumentRuntimeValueDto* GraphGuid = FindField(Present.Value, TEXT("graph_guid"));
		TestTrue(TEXT("present graph carries its exact persistent GUID"), PresentState != nullptr
			&& PresentState->StringValue == TEXT("present") && GraphGuid != nullptr
			&& GraphGuid->StringValue == Tree->BTGraph->GraphGuid.ToString(EGuidFormats::Digits));
		const FAssetDocumentRuntimeValueDto* Children = PresentRoot != nullptr ? FindField(*PresentRoot, TEXT("children")) : nullptr;
		const FAssetDocumentRuntimeValueDto* TaskId = Children != nullptr && Children->Elements.Num() == 1
			? FindField(Children->Elements[0], TEXT("stable_node_id")) : nullptr;
		TestTrue(TEXT("present projection retains the authored Wait task identity"), TaskId != nullptr
			&& TaskId->StringValue == Task->NodeGuid.ToString(EGuidFormats::Digits));
		const FAssetDocumentRuntimeValueDto* Decorators = Children != nullptr && Children->Elements.Num() == 1
			? FindField(Children->Elements[0], TEXT("decorators")) : nullptr;
		const FAssetDocumentRuntimeValueDto* Services = Children != nullptr && Children->Elements.Num() == 1
			? FindField(Children->Elements[0], TEXT("services")) : nullptr;
		TestTrue(TEXT("stock simple and composite decorators are both projected"), Decorators != nullptr && Decorators->Elements.Num() == 2);
		TestTrue(TEXT("stock service is projected"), Services != nullptr && Services->Elements.Num() == 1);
		if (Decorators != nullptr && Decorators->Elements.Num() == 2)
		{
			const FAssetDocumentRuntimeValueDto* Bound = FindField(Decorators->Elements[1], TEXT("bound_graph"));
			TestTrue(TEXT("stock composite decorator retains its bound graph"), Bound != nullptr && Bound->Kind == EAssetDocumentRuntimeValueKind::Object);
			const FAssetDocumentRuntimeValueDto* BoundNodes = Bound != nullptr ? FindField(*Bound, TEXT("nodes")) : nullptr;
			const FAssetDocumentRuntimeValueDto* BoundLinks = Bound != nullptr ? FindField(*Bound, TEXT("links")) : nullptr;
			TestTrue(TEXT("bound expression has one test feeding one sink"), BoundNodes != nullptr
				&& BoundNodes->Elements.Num() == 2 && BoundLinks != nullptr && BoundLinks->Elements.Num() == 1);
		}
		const FAssetDocumentRuntimeValueDto* Comments = FindField(Present.Value, TEXT("comments"));
		const FAssetDocumentRuntimeValueDto* CommentText = Comments != nullptr && Comments->Elements.Num() == 1
			? FindField(Comments->Elements[0], TEXT("text")) : nullptr;
		TestTrue(TEXT("authored comment text survives projection"), CommentText != nullptr && CommentText->StringValue == TEXT("authored readback comment"));

		TestEqual(TEXT("present readback preserves package dirty state"), Fixture.Package->IsDirty(), bDirtyBefore);
		TestTrue(TEXT("present readback has empty mutation lanes"), Present.HandleResults.IsEmpty() && Present.ChangedSet.IsEmpty());

		if (Phase == EAssetDocumentRuntimeRequestPhase::Apply) CheckApplyPreimage(*this, *Tree, Present.Value);

		auto ExpectRejected = [&](const FAssetDocumentPrimitiveInvocation& RejectedInvocation,
			IAssetDocumentAtomicTransaction* Active, const TCHAR* Code)
		{
			const FAssetDocumentPrimitiveOutcome Rejected = Provider->Invoke(RejectedInvocation, Handles, Active);
			TestEqual(TEXT("invalid phase/context fails"), Rejected.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
			TestTrue(TEXT("invalid phase/context has exact diagnostic"), HasDiagnostic(Rejected, Code));
			TestTrue(TEXT("rejection has empty result lanes"), Rejected.Value.Kind == EAssetDocumentRuntimeValueKind::Object
				&& Rejected.Value.ObjectKeys.IsEmpty() && Rejected.Value.Elements.IsEmpty()
				&& Rejected.HandleResults.IsEmpty() && Rejected.ChangedSet.IsEmpty());
		};
		// Corrupt only test-owned authored state, assert failure with empty lanes,
		// then restore the exact pointers/order before observing the next case.
		auto ExpectBadGraph = [&](const TCHAR* Label, const TCHAR* Code)
		{
			AddInfo(Label);
			ExpectRejected(Invocation, bMutationPhase ? &Transaction : nullptr, Code);
		};
		constexpr TCHAR TopologyRejected[] = TEXT("AIProvider.BehaviorTree.GraphReadbackTopologyRejected");
		const FGuid SavedTaskGuid = Task->NodeGuid;
		Task->NodeGuid = Tree->BTGraph->Nodes[0]->NodeGuid;
		ExpectBadGraph(TEXT("duplicate authored node GUID"), TopologyRejected);
		Task->NodeGuid = SavedTaskGuid;
		UObject* SavedInstance = Task->NodeInstance;
		Task->NodeInstance = nullptr;
		ExpectBadGraph(TEXT("missing task instance"), TEXT("AIProvider.BehaviorTree.GraphReadbackUnsupportedNode"));
		Task->NodeInstance = SavedInstance;
		UEdGraphPin* TaskInput = nullptr;
		for (UEdGraphPin* Pin : Task->Pins) if (Pin->Direction == EGPD_Input) TaskInput = Pin;
		if (!TestNotNull(TEXT("stock task input exists"), TaskInput) || TaskInput->LinkedTo.Num() != 1) return false;
		UEdGraphPin* ParentOutput = TaskInput->LinkedTo[0];
		const auto SavedInputLinks = TaskInput->LinkedTo;
		TaskInput->LinkedTo.Reset();
		ExpectBadGraph(TEXT("nonreciprocal authored link"), TopologyRejected);
		TaskInput->LinkedTo = SavedInputLinks;
		const auto SavedNodes = Tree->BTGraph->Nodes;
		Tree->BTGraph->Nodes.Remove(Task);
		ExpectBadGraph(TEXT("link to a node outside graph membership"), TopologyRejected);
		Tree->BTGraph->Nodes = SavedNodes;
		UEdGraphPin* RootOutput = OutputPin(*Tree->BTGraph->Nodes[0]);
		if (!TestNotNull(TEXT("stock root output exists"), RootOutput)) return false;
		const auto SavedRootLinks = RootOutput->LinkedTo;
		RootOutput->LinkedTo.Add(TaskInput);
		TaskInput->LinkedTo.Add(RootOutput);
		ExpectBadGraph(TEXT("task with two authored parents"), TopologyRejected);
		RootOutput->LinkedTo = SavedRootLinks;
		TaskInput->LinkedTo = SavedInputLinks;
		UEdGraphPin* ParentInput = nullptr;
		for (UEdGraphPin* Pin : ParentOutput->GetOwningNode()->Pins)
			if (Pin->Direction == EGPD_Input) ParentInput = Pin;
		if (!TestNotNull(TEXT("stock composite input exists"), ParentInput)) return false;
		const auto SavedParentInputLinks = ParentInput->LinkedTo;
		const auto SavedParentOutputLinks = ParentOutput->LinkedTo;
		ParentOutput->LinkedTo.Add(ParentInput);
		ParentInput->LinkedTo.Add(ParentOutput);
		ExpectBadGraph(TEXT("authored composite self-cycle"), TopologyRejected);
		ParentInput->LinkedTo = SavedParentInputLinks;
		ParentOutput->LinkedTo = SavedParentOutputLinks;
		const FAssetDocumentPrimitiveOutcome Restored = Provider->Invoke(Invocation, Handles, bMutationPhase ? &Transaction : nullptr);
		TestEqual(TEXT("exact fixture restoration returns to success"), Restored.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestTrue(TEXT("fixture restoration preserves persistent task identity"), Task->NodeGuid == SavedTaskGuid && Task->NodeInstance == SavedInstance);
		TestEqual(TEXT("negative readbacks preserve package dirty state"), Fixture.Package->IsDirty(), bDirtyBefore);

		FAssetDocumentPrimitiveInvocation OtherPhase = Invocation;
		OtherPhase.Phase = Phase == EAssetDocumentRuntimeRequestPhase::Apply ? EAssetDocumentRuntimeRequestPhase::Repair
			: Phase == EAssetDocumentRuntimeRequestPhase::Repair ? EAssetDocumentRuntimeRequestPhase::Apply
			: Phase == EAssetDocumentRuntimeRequestPhase::Preflight ? EAssetDocumentRuntimeRequestPhase::Verify
			: EAssetDocumentRuntimeRequestPhase::Preflight;
		const int32 BeforePhaseMismatches = Handles.PhaseMismatchCalls;
		const FAssetDocumentPrimitiveOutcome PhaseLeak = Provider->Invoke(OtherPhase, Handles, bMutationPhase ? &Transaction : nullptr);
		TestTrue(TEXT("a valid-phase dispatch cannot consume another phase's issued lease"),
			PhaseLeak.Status == EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& HasDiagnostic(PhaseLeak, TEXT("AIProvider.InvalidHandle"))
			&& Handles.PhaseMismatchCalls == BeforePhaseMismatches + 1
			&& PhaseLeak.Value.Kind == EAssetDocumentRuntimeValueKind::Object
			&& PhaseLeak.Value.ObjectKeys.IsEmpty() && PhaseLeak.Value.Elements.IsEmpty()
			&& PhaseLeak.HandleResults.IsEmpty() && PhaseLeak.ChangedSet.IsEmpty());
		if (bMutationPhase)
		{
			ExpectRejected(Invocation, nullptr, TEXT("AIProvider.InvalidTransaction"));
			FReadbackTransaction OtherTransaction;
			ExpectRejected(Invocation, &OtherTransaction, TEXT("AIProvider.InvalidTransaction"));
			TestEqual(TEXT("wrong transaction remains untouched"), OtherTransaction.SideEffectCalls, 0);
			FAssetDocumentPrimitiveInvocation WrongRequest = Invocation;
			WrongRequest.RequestId = FGuid::NewGuid();
			ExpectRejected(WrongRequest, &Transaction, TEXT("AIProvider.InvalidTransaction"));
			FAssetDocumentPrimitiveInvocation WrongRevision = Invocation;
			++WrongRevision.RuntimeRevision;
			ExpectRejected(WrongRevision, &Transaction, TEXT("AIProvider.InvalidTransaction"));
		}
		else ExpectRejected(Invocation, &Transaction, TEXT("AIProvider.InvalidTransaction"));
		for (const EAssetDocumentRuntimeRequestPhase RejectedPhase : {
			EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Persist,
			EAssetDocumentRuntimeRequestPhase::Complete, EAssetDocumentRuntimeRequestPhase::Failed})
		{
			FAssetDocumentPrimitiveInvocation Rejected = Invocation;
			Rejected.Phase = RejectedPhase;
			ExpectRejected(Rejected, bMutationPhase ? &Transaction : nullptr, TEXT("AIProvider.InvalidContext"));
		}
		TestEqual(TEXT("readback does not issue native handles after setup"), Handles.IssueCalls, SetupIssueCalls);
	}
	TestEqual(TEXT("readback does not enlist any Atomic side effect"), Transaction.SideEffectCalls, 0);
	TestRepairAfterBlackboardApply(*this, *Provider);
	return true;
}

namespace AssetDocumentAIBehaviorTreeAuthoredGraphReadbackTests
{
const FAssetDocumentRuntimeValueDto* FirstBoundValue(const FAssetDocumentRuntimeValueDto& Value)
{
 if (const auto* Bound = FindField(Value, TEXT("bound_graph")))
  if (Bound->Kind == EAssetDocumentRuntimeValueKind::Object) return Bound;
 for (const auto& Child : Value.Elements) if (const auto* Found = FirstBoundValue(Child)) return Found;
 return nullptr;
}
void RemoveV2BoundFields(FAssetDocumentRuntimeValueDto& Value, const FAssetDocumentRuntimeValueDto* V1)
{
 if (Value.Kind == EAssetDocumentRuntimeValueKind::Object)
 {
  const auto* Kind = FindField(Value, TEXT("kind"));
  const auto* Class = FindField(Value, TEXT("node_class_path"));
  const auto* BoundGraph = FindField(Value, TEXT("bound_graph"));
  if (Kind && Kind->Kind == EAssetDocumentRuntimeValueKind::String && Kind->StringValue == TEXT("composite")
   && Class && Class->Kind == EAssetDocumentRuntimeValueKind::Null
   && BoundGraph && BoundGraph->Kind == EAssetDocumentRuntimeValueKind::Object)
  {
   const int32 PropertiesIndex = Value.ObjectKeys.IndexOfByKey(TEXT("authored_properties"));
   if (Value.Elements.IsValidIndex(PropertiesIndex))
   {
    auto& Properties = Value.Elements[PropertiesIndex];
    if (Properties.Kind == EAssetDocumentRuntimeValueKind::Object && Properties.ObjectKeys.Num() == Properties.Elements.Num())
     for (int32 Index = Properties.ObjectKeys.Num() - 1; Index >= 0; --Index)
      if (Properties.ObjectKeys[Index] == TEXT("bShowOperations") || Properties.ObjectKeys[Index] == TEXT("CompositeName"))
      { Properties.ObjectKeys.RemoveAt(Index); Properties.Elements.RemoveAt(Index); }
   }
  }
  if (Class && Class->Kind == EAssetDocumentRuntimeValueKind::String
   && Class->StringValue == TEXT("/Script/AIModule.BTService_DefaultFocus")
   && Value.ObjectKeys == TArray<FString>{TEXT("authored_properties"), TEXT("editor"), TEXT("node_class_path"), TEXT("stable_node_id")}
   && Value.Elements.Num() == 4)
  {
   // Some candidates may already be visible in V1. Only remove actual V2
   // additions from this same service, never a retained V1 field or value.
   const auto* V1Class = V1 ? FindField(*V1, TEXT("node_class_path")) : nullptr;
   const auto* Identity = FindField(Value, TEXT("stable_node_id"));
   const auto* V1Identity = V1 ? FindField(*V1, TEXT("stable_node_id")) : nullptr;
   const auto* V1Properties = V1 ? FindField(*V1, TEXT("authored_properties")) : nullptr;
   const bool bSameService = V1Class && V1Class->Kind == EAssetDocumentRuntimeValueKind::String
    && V1Class->StringValue == Class->StringValue && Identity && V1Identity
    && Identity->Kind == EAssetDocumentRuntimeValueKind::String && V1Identity->Kind == EAssetDocumentRuntimeValueKind::String
    && Identity->StringValue == V1Identity->StringValue && V1Properties && V1Properties->Kind == EAssetDocumentRuntimeValueKind::Object;
   auto& Properties = Value.Elements[0];
   if (bSameService && Properties.Kind == EAssetDocumentRuntimeValueKind::Object && Properties.ObjectKeys.Num() == Properties.Elements.Num())
    for (int32 Index = Properties.ObjectKeys.Num() - 1; Index >= 0; --Index)
     if (!V1Properties->ObjectKeys.Contains(Properties.ObjectKeys[Index])
      && (Properties.ObjectKeys[Index] == TEXT("Interval") || Properties.ObjectKeys[Index] == TEXT("RandomDeviation")
       || Properties.ObjectKeys[Index] == TEXT("bRestartTimerOnEachActivation") || Properties.ObjectKeys[Index] == TEXT("bCallTickOnSearchStart")))
     { Properties.ObjectKeys.RemoveAt(Index); Properties.Elements.RemoveAt(Index); }
  }
  const bool bSimpleCooldown = Kind && Kind->Kind == EAssetDocumentRuntimeValueKind::String && Kind->StringValue == TEXT("simple")
   && Class && Class->Kind == EAssetDocumentRuntimeValueKind::String && Class->StringValue == TEXT("/Script/AIModule.BTDecorator_Cooldown")
   && BoundGraph && BoundGraph->Kind == EAssetDocumentRuntimeValueKind::Null
   && Value.ObjectKeys == TArray<FString>{TEXT("authored_properties"), TEXT("bound_graph"), TEXT("editor"), TEXT("kind"), TEXT("node_class_path"), TEXT("stable_node_id")}
   && Value.Elements.Num() == 6;
  const auto* BoundClass = FindField(Value, TEXT("node_class_path_or_null"));
  const bool bBoundCooldown = Kind && Kind->Kind == EAssetDocumentRuntimeValueKind::String && Kind->StringValue == TEXT("test")
   && BoundClass && BoundClass->Kind == EAssetDocumentRuntimeValueKind::String && BoundClass->StringValue == TEXT("/Script/AIModule.BTDecorator_Cooldown")
   && Value.ObjectKeys == TArray<FString>{TEXT("authored_properties"), TEXT("editor"), TEXT("input_count"), TEXT("kind"), TEXT("node_class_path_or_null"), TEXT("stable_node_id")}
   && Value.Elements.Num() == 6;
  if (bSimpleCooldown || bBoundCooldown)
  {
   // Cooldown hides Condition. V2 reads its actual typed bool, while V1 keeps
   // the visible-row projection. Remove only this addition from the same node.
   const TCHAR* ClassField = bSimpleCooldown ? TEXT("node_class_path") : TEXT("node_class_path_or_null");
   const auto* CooldownClass = bSimpleCooldown ? Class : BoundClass;
   const auto* V1Class = V1 ? FindField(*V1, ClassField) : nullptr;
   const auto* V1Kind = V1 ? FindField(*V1, TEXT("kind")) : nullptr;
   const auto* Identity = FindField(Value, TEXT("stable_node_id"));
   const auto* V1Identity = V1 ? FindField(*V1, TEXT("stable_node_id")) : nullptr;
   const auto* V1Properties = V1 ? FindField(*V1, TEXT("authored_properties")) : nullptr;
   const bool bSameCooldown = V1Class && V1Class->Kind == EAssetDocumentRuntimeValueKind::String && V1Class->StringValue == CooldownClass->StringValue
    && V1Kind && V1Kind->Kind == EAssetDocumentRuntimeValueKind::String && V1Kind->StringValue == Kind->StringValue
    && Identity && Identity->Kind == EAssetDocumentRuntimeValueKind::String && V1Identity && V1Identity->Kind == EAssetDocumentRuntimeValueKind::String
    && Identity->StringValue == V1Identity->StringValue && V1Properties && V1Properties->Kind == EAssetDocumentRuntimeValueKind::Object
    && !V1Properties->ObjectKeys.Contains(TEXT("bInverseCondition"));
   auto& Properties = Value.Elements[0];
   const int32 InverseIndex = Properties.ObjectKeys.IndexOfByKey(TEXT("bInverseCondition"));
   if (bSameCooldown && Properties.Kind == EAssetDocumentRuntimeValueKind::Object
    && Properties.ObjectKeys.Num() == Properties.Elements.Num() && Properties.Elements.IsValidIndex(InverseIndex)
    && Properties.Elements[InverseIndex].Kind == EAssetDocumentRuntimeValueKind::Boolean)
   { Properties.ObjectKeys.RemoveAt(InverseIndex); Properties.Elements.RemoveAt(InverseIndex); }
  }
  const bool bBound = FindField(Value, TEXT("nodes")) && FindField(Value, TEXT("links"));
  for (int32 Index = Value.ObjectKeys.Num() - 1; Index >= 0; --Index)
   if (Value.ObjectKeys[Index] == TEXT("input_count") || (bBound && Value.ObjectKeys[Index] == TEXT("comments")))
   { Value.ObjectKeys.RemoveAt(Index); Value.Elements.RemoveAt(Index); }
 }
 for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
 {
  const auto* V1Child = V1 && Value.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(*V1, Value.ObjectKeys[Index])
   : (V1 && V1->Kind == EAssetDocumentRuntimeValueKind::Array && V1->Elements.IsValidIndex(Index) ? &V1->Elements[Index] : nullptr);
  RemoveV2BoundFields(Value.Elements[Index], V1Child);
 }
}
FString FirstReadbackDifference(const FAssetDocumentRuntimeValueDto& V1, const FAssetDocumentRuntimeValueDto& V2, const FString& Path)
{
 if (V1.Kind != V2.Kind) return FString::Printf(TEXT("%s kind V1=%d V2=%d"), *Path, static_cast<int32>(V1.Kind), static_cast<int32>(V2.Kind));
 if (V1.Kind == EAssetDocumentRuntimeValueKind::Object && V1.ObjectKeys != V2.ObjectKeys)
  return FString::Printf(TEXT("%s keys V1=[%s] V2=[%s]"), *Path, *FString::Join(V1.ObjectKeys, TEXT(",")), *FString::Join(V2.ObjectKeys, TEXT(",")));
 if (V1.Elements.Num() != V2.Elements.Num())
  return FString::Printf(TEXT("%s element count V1=%d V2=%d"), *Path, V1.Elements.Num(), V2.Elements.Num());
 if (V1.Kind == EAssetDocumentRuntimeValueKind::Object || V1.Kind == EAssetDocumentRuntimeValueKind::Array)
 {
  for (int32 Index = 0; Index < V1.Elements.Num(); ++Index)
  {
   const FString Segment = V1.Kind == EAssetDocumentRuntimeValueKind::Object ? V1.ObjectKeys[Index] : LexToString(Index);
   const FString Difference = FirstReadbackDifference(V1.Elements[Index], V2.Elements[Index], Path + TEXT("/") + Segment.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
   if (!Difference.IsEmpty()) return Difference;
  }
  return {};
 }
 TArray<uint8> V1Bytes, V2Bytes;
 using UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EncodeAuthoredCanonicalValue;
 if (EncodeAuthoredCanonicalValue(V1, V1Bytes) && EncodeAuthoredCanonicalValue(V2, V2Bytes) && V1Bytes == V2Bytes) return {};
 return FString::Printf(TEXT("%s scalar kind=%d V1=(bool:%d int:%s number:%.17g string:%s) V2=(bool:%d int:%s number:%.17g string:%s)"),
  *Path, static_cast<int32>(V1.Kind), V1.BooleanValue ? 1 : 0, *LexToString(V1.IntegerValue), V1.NumberValue, *V1.StringValue.Left(128),
  V2.BooleanValue ? 1 : 0, *LexToString(V2.IntegerValue), V2.NumberValue, *V2.StringValue.Left(128));
}
UBehaviorTreeDecoratorGraphNode_Logic* AddEmptyReadbackLogic(UBehaviorTreeDecoratorGraph& Target, int32 Count)
{
 // Only these two fixed, bounded valid prefix expressions reach the stock constructor.
 if (Count != 2 && Count != 5) return nullptr;
 TStrongObjectPtr<UBehaviorTreeGraphNode_CompositeDecorator> Seed(NewObject<UBehaviorTreeGraphNode_CompositeDecorator>());
 auto* Graph = NewObject<UBehaviorTreeDecoratorGraph>(Seed.Get());
 Seed->BoundGraph = Graph; Graph->GraphGuid = FGuid::NewGuid();
 Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
 TArray<UBTDecorator*> Instances{NewObject<UBTDecorator_Cooldown>(Seed.Get())};
 TArray<FBTDecoratorLogic> Ops{FBTDecoratorLogic(EBTDecoratorLogic::And, static_cast<uint16>(Count))};
 for (int32 Index = 0; Index < Count; ++Index) Ops.Emplace(EBTDecoratorLogic::Test, 0);
 if (Seed->SpawnMissingNodes(Instances, Ops, 0) != Ops.Num()) return nullptr;
 UBehaviorTreeDecoratorGraphNode_Logic* Result = nullptr;
 TArray<UEdGraphNode*> Nodes; for (UEdGraphNode* Node : Graph->Nodes) Nodes.Add(Node);
 for (UEdGraphNode* Node : Nodes)
 {
  if (auto* Logic = Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Node)) if (Logic->LogicMode == EDecoratorLogicMode::And) Result = Logic;
  Graph->GetSchema()->BreakNodeLinks(*Node); if (!Graph->RemoveNode(Node)) return nullptr;
 }
 if (!Result || !Result->Rename(nullptr, &Target, REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional)) return nullptr;
 Target.AddNode(Result, false, false); return Result;
}

TArray<uint8> ReadbackObjectBytes(UBehaviorTree& Tree)
{
 TArray<UObject*> Objects{&Tree};
 ForEachObjectWithOuter(&Tree, [&Objects](UObject* Object) { Objects.Add(Object); });
 Objects.Sort([](const UObject& Left, const UObject& Right) { return Left.GetPathName() < Right.GetPathName(); });
 TArray<uint8> Result;
 for (UObject* Object : Objects)
 {
  TArray<uint8> Bytes;
  // Avoid EnsurePackageNamespace: the byte observation must not write metadata.
  FObjectWriter Writer(Object, Bytes, false, false, false, PPF_DuplicateVerbatim);
  Result.Append(Bytes);
 }
 return Result;
}

bool TestFreshSubnodeReadback(FAutomationTestBase& Test, const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider)
{
 FReadbackBlackboardCleanup Cleanup(Test);
 FRegisteredReadbackTree Fixture;
 auto* LiveTask = InstallTestGraph(*Fixture.Tree, false);
 if (!Test.TestNotNull(TEXT("save/load fixture has stock decorator and service"), LiveTask)) return false;
 const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
 const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/BTReadbackFresh") / Id);
 const FString Filename = Directory / TEXT("Tree.uasset");
 const bool bOwnsDirectory = !IFileManager::Get().DirectoryExists(*Directory);
 TStrongObjectPtr<UPackage> FreshPackage;
 TStrongObjectPtr<UBehaviorTree> FreshTree;
 ON_SCOPE_EXIT
 {
  if (FreshTree.IsValid()) FreshTree->ClearFlags(RF_Public | RF_Standalone);
  if (FreshPackage.IsValid()) FreshPackage->SetDirtyFlag(false);
  UPackage::WaitForAsyncFileWrites();
  if (bOwnsDirectory) IFileManager::Get().DeleteDirectory(*Directory, false, true);
 };
 if (!Test.TestTrue(TEXT("fresh readback does not reuse existing files"), bOwnsDirectory)) return false;
 if (!Test.TestTrue(TEXT("isolated fresh readback directory created"), IFileManager::Get().MakeDirectory(*Directory, true))) return false;
 FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
 if (!Test.TestTrue(TEXT("stock authored graph saves actual package bytes"), UPackage::SavePackage(Fixture.Package.Get(), Fixture.Tree.Get(), *Filename, Args))) return false;
 UPackage::WaitForAsyncFileWrites();
 FreshPackage.Reset(CreatePackage(*(TEXT("/Temp/BTReadbackFresh_") + Id)));
 UPackage* Loaded = LoadPackage(FreshPackage.Get(), FPackagePath::FromLocalPath(Filename), LOAD_ForDiff);
 if (!Test.TestTrue(TEXT("distinct stock package loads for diff"), Loaded == FreshPackage.Get())) return false;
 FreshTree.Reset(FindObject<UBehaviorTree>(Loaded, TEXT("Tree")));
 if (!Test.TestTrue(TEXT("fresh tree is the distinct saved asset"), FreshTree.IsValid() && FreshTree.Get() != Fixture.Tree.Get())) return false;
 auto* Graph = Cast<UBehaviorTreeGraph>(FreshTree->BTGraph);
 UBehaviorTreeGraphNode_Task* Task = nullptr;
 UBehaviorTreeGraphNode_Composite* Composite = nullptr;
 if (!Test.TestNotNull(TEXT("fresh package retained graph"), Graph)) return false;
 for (UEdGraphNode* Node : Graph->Nodes)
 {
  if (auto* Candidate = Cast<UBehaviorTreeGraphNode_Task>(Node)) Task = Candidate;
  if (auto* Candidate = Cast<UBehaviorTreeGraphNode_Composite>(Node)) Composite = Candidate;
 }
 if (!Test.TestTrue(TEXT("fresh graph retained typed attachment membership"), Task && Composite && Task->Decorators.Num() == 1 && Task->Services.Num() == 1 && Task->SubNodes.Num() == 2)) return false;
 UBehaviorTreeGraphNode* Decorator = Task->Decorators[0];
 UBehaviorTreeGraphNode* Service = Task->Services[0];
 if (!Test.TestTrue(TEXT("PostLoad leaves stock transient attachment parents absent"), Decorator && Service && !Decorator->ParentNode && !Service->ParentNode)) return false;
 Fixture.Package->SetDirtyFlag(false); FreshPackage->SetDirtyFlag(false);
 FReadbackTransaction Transaction; const auto Token = Transaction.GetMutationToken();
 FReadbackHandleResolver Handles(Token, EAssetDocumentRuntimeRequestPhase::Verify);
 FAssetDocumentRuntimeRequestDto Request;
 Request.RequestId = Token.RequestId; Request.Operation = EAssetDocumentRuntimeOperation::Apply;
 Request.ExpectedRuntimeRevision = Token.RuntimeRevision; Request.TargetAssetPath = Fixture.Tree->GetPathName();
 Request.RequestedAssetClassPath = TEXT("/Script/AIModule.BehaviorTree");
 FAssetDocumentHandleValidationContext Context;
 Context.RequestId = Token.RequestId; Context.TransactionId = Token.TransactionId; Context.RuntimeRevision = Token.RuntimeRevision;
 Context.ProviderId = TEXT("editor"); Context.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
 Context.RequiredKind = EAssetDocumentHandleKind::Asset; Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 auto Lease = ResolveAssetDocumentEditorStagedBehaviorTreeRead(Request, Context, FreshTree->GetPathName(), Filename, Token.RuntimeRevision, Handles, Diagnostics);
 for (const auto& D : Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("real staged resolver issues exact readonly lease"), Lease.IsValid())) return false;
 const auto Alias = Handles.AliasForAI(Lease->GetAssetReadHandle(), EAssetDocumentHandleAccess::ReadOnly);
 if (!Test.TestTrue(TEXT("fresh readback retains actual staged payload"), Alias.IsSet())) return false;
 auto Invocation = ReadbackInvocation(EAssetDocumentRuntimeRequestPhase::Verify, Token, *FreshTree, Alias.GetValue());
 Invocation.PrimitiveId = TEXT("ai.behavior_tree.authored_graph.readback.v2"); Invocation.Version = 2;
 const auto LiveBytes = ReadbackObjectBytes(*Fixture.Tree);
 auto Observe = [&](const TCHAR* Label, bool bExpectedSuccess)
 {
  const auto Before = ReadbackObjectBytes(*FreshTree);
  const int32 IssueCalls = Handles.IssueCalls;
  const auto Outcome = Provider->Invoke(Invocation, Handles, nullptr);
  for (const auto& D : Outcome.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
  Test.TestEqual(Label, Outcome.Status, bExpectedSuccess ? EAssetDocumentPrimitiveOutcomeStatus::Succeeded : EAssetDocumentPrimitiveOutcomeStatus::Failed);
  if (!bExpectedSuccess) Test.TestTrue(TEXT("malformed attachment has topology diagnostic"), HasDiagnostic(Outcome, TEXT("AIProvider.BehaviorTree.GraphReadbackTopologyRejected")));
  Test.TestTrue(TEXT("fresh readback preserves complete saved object memory"), ReadbackObjectBytes(*FreshTree) == Before);
  Test.TestTrue(TEXT("fresh readback preserves complete original live memory"), ReadbackObjectBytes(*Fixture.Tree) == LiveBytes);
  Test.TestFalse(TEXT("fresh readback leaves staged package clean"), FreshPackage->IsDirty());
  Test.TestFalse(TEXT("fresh readback leaves live package clean"), Fixture.Package->IsDirty());
  Test.TestEqual(TEXT("readback issues no handles"), Handles.IssueCalls, IssueCalls);
  Test.TestTrue(TEXT("fresh readback has empty mutation lanes"), Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty());
  if (!bExpectedSuccess) Test.TestTrue(TEXT("topology failure has empty value lane"), Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty());
  return Outcome;
 };
 const auto Fresh = Observe(TEXT("saved decorator/service graph reads without transient parents"), true);
 Test.TestTrue(TEXT("readonly readback does not reconstruct transient parent cache"), !Decorator->ParentNode && !Service->ParentNode);
 const auto* Root = FindField(Fresh.Value, TEXT("root"));
 const auto* Children = Root ? FindField(*Root, TEXT("children")) : nullptr;
 const auto* Decorators = Children && Children->Elements.Num() == 1 ? FindField(Children->Elements[0], TEXT("decorators")) : nullptr;
 const auto* Services = Children && Children->Elements.Num() == 1 ? FindField(Children->Elements[0], TEXT("services")) : nullptr;
 Test.TestTrue(TEXT("fresh projection retains saved decorator and service"), Decorators && Services && Decorators->Elements.Num() == 1 && Services->Elements.Num() == 1);
 Decorator->ParentNode = Composite;
 Observe(TEXT("nonnull wrong decorator cache still rejects"), false);
 Test.TestTrue(TEXT("rejection does not repair wrong decorator parent"), Decorator->ParentNode == Composite);
 Decorator->ParentNode = nullptr;
 Service->ParentNode = Composite;
 Observe(TEXT("nonnull wrong service cache still rejects"), false);
 Test.TestTrue(TEXT("rejection does not repair wrong service parent"), Service->ParentNode == Composite);
 Service->ParentNode = nullptr;
 Composite->Decorators.Add(Decorator); Composite->SubNodes.Add(Decorator);
 Observe(TEXT("null transient cache cannot hide two persistent owners"), false);
 Composite->Decorators.Remove(Decorator); Composite->SubNodes.Remove(Decorator);
 Task->SubNodes.Remove(Service);
 Observe(TEXT("null transient cache cannot hide missing persisted membership"), false);
 Task->SubNodes.Add(Service);
 Observe(TEXT("exact persistent attachment restoration reads successfully"), true);
 Test.TestTrue(TEXT("all reads preserve missing transient caches"), !Decorator->ParentNode && !Service->ParentNode);
 Test.TestTrue(TEXT("fresh lease remains current after success and rejection"), Lease->IsCurrent());
 return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeReadbackV2Test,
 "UEAssetDoc.AIProvider.BehaviorTree.AuthoredGraphReadbackV2.BoundCommentsAndEmptyInputs",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeReadbackV2Test::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeAuthoredGraphReadbackTests;
 using UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EncodeAuthoredCanonicalValue;
 const auto Provider = CreateAssetDocumentAIPrimitiveProvider();
 for (const int32 Count : {2, 5})
 {
  FReadbackBlackboardCleanup Cleanup(*this); FRegisteredReadbackTree Fixture;
  auto* Task = InstallTestGraph(*Fixture.Tree);
  if (!TestNotNull(TEXT("complete real authored fixture"), Task)) return false;
  UBehaviorTreeGraphNode_CompositeDecorator* Wrapper = nullptr;
  for (UBehaviorTreeGraphNode* Node : Task->Decorators) if (auto* Candidate = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Node)) Wrapper = Candidate;
  if (!Wrapper) return false;
  auto* Bound = Cast<UBehaviorTreeDecoratorGraph>(Wrapper->BoundGraph);
  auto* EmptyLogic = AddEmptyReadbackLogic(*Bound, Count);
  if (!TestNotNull(TEXT("stock constructed disconnected logic with empty slots"), EmptyLogic)) return false;
  Fixture.Package->SetDirtyFlag(false);
  FReadbackTransaction Transaction; const auto Token = Transaction.GetMutationToken();
  FReadbackHandleResolver Handles(Token, EAssetDocumentRuntimeRequestPhase::Verify);
  auto Alias = ResolveTestAlias(*this, *Fixture.Tree, Token, EAssetDocumentRuntimeRequestPhase::Verify, Handles, EAssetDocumentHandleAccess::ReadOnly);
  if (!Alias.IsSet()) return false;
  auto V1 = ReadbackInvocation(EAssetDocumentRuntimeRequestPhase::Verify, Token, *Fixture.Tree, Alias.GetValue());
  auto V2 = V1; V2.PrimitiveId = TEXT("ai.behavior_tree.authored_graph.readback.v2"); V2.Version = 2;
  const auto Old = Provider->Invoke(V1, Handles, nullptr); const auto Complete = Provider->Invoke(V2, Handles, nullptr);
  if (!TestEqual(TEXT("V1 original valid domain succeeds"), Old.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
   || !TestEqual(TEXT("V2 same graph succeeds"), Complete.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
  const auto* BoundValue = FirstBoundValue(Complete.Value); if (!BoundValue) return false;
  const auto* Nodes = FindField(*BoundValue, TEXT("nodes")); if (!Nodes) return false;
  UBehaviorTreeDecoratorGraphNode_Decorator* BoundCooldownNode = nullptr;
  for (UEdGraphNode* Node : Bound->Nodes)
   if (auto* Candidate = Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(Node); Candidate && Candidate->NodeInstance
    && Candidate->NodeInstance->GetClass() == UBTDecorator_Cooldown::StaticClass()) BoundCooldownNode = Candidate;
  const auto* BoundCooldown = BoundCooldownNode ? Cast<UBTDecorator_Cooldown>(BoundCooldownNode->NodeInstance) : nullptr;
  if (!TestNotNull(TEXT("actual stock bound Cooldown fixture"), BoundCooldown)) return false;
  bool Found = false;
  bool FoundBoundCooldown = false;
  for (const auto& Node : Nodes->Elements)
  {
   const auto* Kind = FindField(Node, TEXT("kind"));
   if (Kind->StringValue == TEXT("sink")) TestEqual(TEXT("sink has its one actual input"), FindField(Node, TEXT("input_count"))->IntegerValue, int64(1));
   if (Kind->StringValue == TEXT("test")) TestEqual(TEXT("test node has no inputs"), FindField(Node, TEXT("input_count"))->IntegerValue, int64(0));
   if (FindField(Node, TEXT("stable_node_id"))->StringValue == EmptyLogic->NodeGuid.ToString(EGuidFormats::Digits))
   { Found = true; TestEqual(TEXT("input count includes every unconnected slot"), FindField(Node, TEXT("input_count"))->IntegerValue, int64(Count)); }
   if (FindField(Node, TEXT("stable_node_id"))->StringValue == BoundCooldownNode->NodeGuid.ToString(EGuidFormats::Digits))
   {
    FoundBoundCooldown = true;
    const auto* Properties = FindField(Node, TEXT("authored_properties"));
    const auto* Inverse = Properties ? FindField(*Properties, TEXT("bInverseCondition")) : nullptr;
    TestTrue(TEXT("V2 bound Cooldown inverse condition matches the actual stock decorator"), Inverse
     && Inverse->Kind == EAssetDocumentRuntimeValueKind::Boolean && Inverse->BooleanValue == BoundCooldown->IsInversed());
   }
  }
  TestTrue(TEXT("disconnected logic is retained"), Found);
  TestTrue(TEXT("V2 contains the actual bound Cooldown evidence"), FoundBoundCooldown);
  const auto* RootValue = FindField(Complete.Value, TEXT("root"));
  const auto* Children = RootValue ? FindField(*RootValue, TEXT("children")) : nullptr;
  const auto* Decorators = Children && Children->Elements.Num() == 1 ? FindField(Children->Elements[0], TEXT("decorators")) : nullptr;
  const FAssetDocumentRuntimeValueDto* WrapperProperties = nullptr;
  if (Decorators)
   for (const auto& Decorator : Decorators->Elements)
    if (const auto* Identity = FindField(Decorator, TEXT("stable_node_id")); Identity && Identity->StringValue == Wrapper->NodeGuid.ToString(EGuidFormats::Digits))
     WrapperProperties = FindField(Decorator, TEXT("authored_properties"));
  const auto* ShowOperations = WrapperProperties ? FindField(*WrapperProperties, TEXT("bShowOperations")) : nullptr;
  const auto* CompositeName = WrapperProperties ? FindField(*WrapperProperties, TEXT("CompositeName")) : nullptr;
  TestTrue(TEXT("V2 wrapper show-operations matches the actual stock wrapper"), ShowOperations && ShowOperations->Kind == EAssetDocumentRuntimeValueKind::Boolean && ShowOperations->BooleanValue == (Wrapper->bShowOperations != 0));
  TestTrue(TEXT("V2 wrapper composite name matches the actual stock wrapper"), CompositeName && CompositeName->Kind == EAssetDocumentRuntimeValueKind::String && CompositeName->StringValue == Wrapper->CompositeName);
  UBehaviorTreeGraphNode* SimpleCooldownNode = nullptr;
  for (UBehaviorTreeGraphNode* Node : Task->Decorators)
   if (Node && Node->NodeInstance && Node->NodeInstance->GetClass() == UBTDecorator_Cooldown::StaticClass()) SimpleCooldownNode = Node;
  const auto* SimpleCooldown = SimpleCooldownNode ? Cast<UBTDecorator_Cooldown>(SimpleCooldownNode->NodeInstance) : nullptr;
  if (!TestNotNull(TEXT("actual stock simple Cooldown fixture"), SimpleCooldown)) return false;
  const FAssetDocumentRuntimeValueDto* SimpleCooldownProperties = nullptr;
  if (Decorators)
   for (const auto& Decorator : Decorators->Elements)
    if (const auto* Identity = FindField(Decorator, TEXT("stable_node_id")); Identity && Identity->Kind == EAssetDocumentRuntimeValueKind::String
     && Identity->StringValue == SimpleCooldownNode->NodeGuid.ToString(EGuidFormats::Digits)) SimpleCooldownProperties = FindField(Decorator, TEXT("authored_properties"));
  const auto* SimpleInverse = SimpleCooldownProperties ? FindField(*SimpleCooldownProperties, TEXT("bInverseCondition")) : nullptr;
  TestTrue(TEXT("V2 simple Cooldown inverse condition matches the actual stock decorator"), SimpleInverse
   && SimpleInverse->Kind == EAssetDocumentRuntimeValueKind::Boolean && SimpleInverse->BooleanValue == SimpleCooldown->IsInversed());
  auto* FocusServiceNode = Task->Services.Num() == 1 ? Task->Services[0].Get() : nullptr;
  auto* FocusService = FocusServiceNode ? Cast<UBTService_DefaultFocus>(FocusServiceNode->NodeInstance) : nullptr;
  if (!TestNotNull(TEXT("actual stock DefaultFocus service fixture"), FocusService)) return false;
  const auto* Services = Children && Children->Elements.Num() == 1 ? FindField(Children->Elements[0], TEXT("services")) : nullptr;
  const FAssetDocumentRuntimeValueDto* FocusProperties = nullptr;
  if (Services && Services->Kind == EAssetDocumentRuntimeValueKind::Array)
   for (const auto& Service : Services->Elements)
    if (const auto* Identity = FindField(Service, TEXT("stable_node_id")); Identity && Identity->Kind == EAssetDocumentRuntimeValueKind::String
     && Identity->StringValue == FocusServiceNode->NodeGuid.ToString(EGuidFormats::Digits))
     FocusProperties = FindField(Service, TEXT("authored_properties"));
  const auto* IntervalProperty = FindFProperty<FFloatProperty>(FocusService->GetClass(), TEXT("Interval"));
  const auto* DeviationProperty = FindFProperty<FFloatProperty>(FocusService->GetClass(), TEXT("RandomDeviation"));
  const auto* RestartProperty = FindFProperty<FBoolProperty>(FocusService->GetClass(), TEXT("bRestartTimerOnEachActivation"));
  const auto* CallTickProperty = FindFProperty<FBoolProperty>(FocusService->GetClass(), TEXT("bCallTickOnSearchStart"));
  const auto* Interval = FocusProperties ? FindField(*FocusProperties, TEXT("Interval")) : nullptr;
  const auto* Deviation = FocusProperties ? FindField(*FocusProperties, TEXT("RandomDeviation")) : nullptr;
  const auto* Restart = FocusProperties ? FindField(*FocusProperties, TEXT("bRestartTimerOnEachActivation")) : nullptr;
  const auto* CallTick = FocusProperties ? FindField(*FocusProperties, TEXT("bCallTickOnSearchStart")) : nullptr;
  TestTrue(TEXT("V2 hidden service interval matches the actual stock property"), IntervalProperty && Interval && Interval->Kind == EAssetDocumentRuntimeValueKind::Number
   && Interval->NumberValue == static_cast<double>(IntervalProperty->GetPropertyValue_InContainer(FocusService)));
  TestTrue(TEXT("V2 hidden service random deviation matches the actual stock property"), DeviationProperty && Deviation && Deviation->Kind == EAssetDocumentRuntimeValueKind::Number
   && Deviation->NumberValue == static_cast<double>(DeviationProperty->GetPropertyValue_InContainer(FocusService)));
  TestTrue(TEXT("V2 hidden service restart flag matches the actual stock property"), RestartProperty && Restart && Restart->Kind == EAssetDocumentRuntimeValueKind::Boolean
   && Restart->BooleanValue == RestartProperty->GetPropertyValue_InContainer(FocusService));
  TestTrue(TEXT("V2 hidden service call-tick flag matches the actual stock property"), CallTickProperty && CallTick && CallTick->Kind == EAssetDocumentRuntimeValueKind::Boolean
   && CallTick->BooleanValue == CallTickProperty->GetPropertyValue_InContainer(FocusService));
  auto Legacy = Complete.Value; RemoveV2BoundFields(Legacy, &Old.Value); TArray<uint8> OldBytes, LegacyBytes;
  TestTrue(TEXT("V1 bytes equal shared V2 projection with only added fields removed"), EncodeAuthoredCanonicalValue(Old.Value, OldBytes) && EncodeAuthoredCanonicalValue(Legacy, LegacyBytes) && OldBytes == LegacyBytes);
  if (OldBytes != LegacyBytes)
   UE_LOG(LogTemp, Display, TEXT("V1/normalized V2 first difference (Count=%d): %s"), Count, *FirstReadbackDifference(Old.Value, Legacy, TEXT("")));
  FAISchemaAction_AddComment Action;
  auto* Comment = Cast<UEdGraphNode_Comment>(Action.PerformAction(Bound, nullptr, FVector2f(320, 400), false));
  if (!Comment) return false;
  Comment->NodeComment = TEXT("bound comment 文本"); Comment->NodeDetails = FText::FromString(TEXT("details"));
  Comment->NodeWidth = 512; Comment->NodeHeight = 240; Comment->FontSize = 23; Comment->CommentDepth = 4;
  Comment->CommentColor = FLinearColor(0.125f, 0.25f, 0.5f, 1.0f); Comment->MoveMode = ECommentBoxMode::NoGroupMovement;
  Comment->bColorCommentBubble = true; Comment->bCommentBubblePinned = true;
  Comment->bCommentBubbleVisible = true; Comment->bCommentBubbleVisible_InDetailsPanel = true;
  Fixture.Package->SetDirtyFlag(false);
  const auto Rejected = Provider->Invoke(V1, Handles, nullptr);
  TestEqual(TEXT("V1 retains original bound comment rejection"), Rejected.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  TestTrue(TEXT("V1 retains precise unsupported node diagnostic"), HasDiagnostic(Rejected, TEXT("AIProvider.BehaviorTree.GraphReadbackUnsupportedNode")));
  const auto WithComment = Provider->Invoke(V2, Handles, nullptr);
  if (!TestEqual(TEXT("V2 reads independent bound comment"), WithComment.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
  const auto* Comments = FindField(*FirstBoundValue(WithComment.Value), TEXT("comments"));
  if (!Comments || !TestEqual(TEXT("one independent bound comment"), Comments->Elements.Num(), 1)) return false;
  const auto& C = Comments->Elements[0];
  TestEqual(TEXT("full comment codec field count"), C.ObjectKeys.Num(), 13);
  TestEqual(TEXT("comment text"), FindField(C, TEXT("text"))->StringValue, Comment->NodeComment);
  TestEqual(TEXT("comment details"), FindField(C, TEXT("node_details"))->StringValue, Comment->NodeDetails.ToString());
  TestEqual(TEXT("comment width"), FindField(*FindField(C, TEXT("size")), TEXT("width"))->IntegerValue, int64(512));
  TestEqual(TEXT("comment height"), FindField(*FindField(C, TEXT("size")), TEXT("height"))->IntegerValue, int64(240));
  TestEqual(TEXT("comment red"), FindField(*FindField(C, TEXT("color")), TEXT("r"))->NumberValue, double(0.125));
  TestEqual(TEXT("comment green"), FindField(*FindField(C, TEXT("color")), TEXT("g"))->NumberValue, double(0.25));
  TestEqual(TEXT("comment blue"), FindField(*FindField(C, TEXT("color")), TEXT("b"))->NumberValue, double(0.5));
  TestEqual(TEXT("comment alpha"), FindField(*FindField(C, TEXT("color")), TEXT("a"))->NumberValue, double(1));
  TestEqual(TEXT("comment x"), FindField(*FindField(C, TEXT("position")), TEXT("x"))->IntegerValue, int64(Comment->NodePosX));
  TestEqual(TEXT("comment y"), FindField(*FindField(C, TEXT("position")), TEXT("y"))->IntegerValue, int64(Comment->NodePosY));
  TestEqual(TEXT("comment font size"), FindField(C, TEXT("font_size"))->IntegerValue, int64(23));
  TestEqual(TEXT("comment depth"), FindField(C, TEXT("comment_depth"))->IntegerValue, int64(4));
  TestEqual(TEXT("comment movement"), FindField(C, TEXT("move_mode"))->StringValue, FString(TEXT("NoGroupMovement")));
  TestEqual(TEXT("comment stable GUID"), FindField(C, TEXT("stable_node_id"))->StringValue, Comment->NodeGuid.ToString(EGuidFormats::Digits));
  for (const TCHAR* Key : {TEXT("color_comment_bubble"), TEXT("comment_bubble_pinned"), TEXT("comment_bubble_visible"), TEXT("comment_bubble_visible_in_details")})
   TestTrue(Key, FindField(C, Key)->BooleanValue);
  TestFalse(TEXT("both versions remain read-only"), Fixture.Package->IsDirty());
 }
 return TestFreshSubnodeReadback(*this, Provider);
}

#endif
