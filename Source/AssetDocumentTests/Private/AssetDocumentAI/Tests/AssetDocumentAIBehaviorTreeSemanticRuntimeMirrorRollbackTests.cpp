// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestTypes.h"

#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AIGraphSchema.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Decorators/BTDecorator_ForceSuccess.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_SimpleParallel.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"
#include "AssetDocumentRollbackAction.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if __has_include("BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackPrivate.h")
#include "BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackPrivate.h"
#define ASSETDOCUMENT_AI_BT_SEMANTIC_ROLLBACK_PRIVATE_TEST_HOOK_PRESENT 1
#else
#define ASSETDOCUMENT_AI_BT_SEMANTIC_ROLLBACK_PRIVATE_TEST_HOOK_PRESENT 0
#endif

// Node B intentionally starts red before its public owner exists.  This keeps
// the Automation rows discoverable and turns the missing owner into a contract
// assertion, rather than a missing-symbol, UHT, or skipped-test failure.
#if __has_include("BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollback.h")
#include "BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollback.h"
#include "AssetDocumentPluginPaths.h"
#define ASSETDOCUMENT_AI_BT_SEMANTIC_ROLLBACK_HELPER_PRESENT 1
#else
#define ASSETDOCUMENT_AI_BT_SEMANTIC_ROLLBACK_HELPER_PRESENT 0
#endif

namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests
{
constexpr const TCHAR* ComponentId =
	TEXT("assetdocument.ai.behavior_tree.runtime_mirror.v1");

struct FTestSemanticResult
{
	bool bSucceeded = false;
	FString RootNodeClassPath;
	int32 NodeCount = 0;
	FString ExecutionOrderHash;
	FString BlackboardEvidenceHash;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

bool HasDiagnostic(const FTestSemanticResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentRuntimeDiagnostic& Item)
	{
		return Item.Code == Code;
	});
}

bool IsSha256(const FString& Value)
{
	if (Value.Len() != 64) return false;
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsHexDigit(Character)) return false;
	}
	return true;
}

struct FPublicDebuggerEvidence
{
	uint32 Breakpoint; uint32 BreakpointEnabled; uint32 Current; uint32 Previous; uint32 Flash;
	uint32 Succeeded; uint32 Failed; uint32 Trigger; uint32 FailedTrigger; uint32 Reverse; uint32 BreakpointTrigger;
	int32 SearchIndex; int32 SearchSize; int32 UpdateCounter; FString Description;
	explicit FPublicDebuggerEvidence(const UBehaviorTreeGraphNode& Node)
		: Breakpoint(Node.bHasBreakpoint), BreakpointEnabled(Node.bIsBreakpointEnabled), Current(Node.bDebuggerMarkCurrentlyActive), Previous(Node.bDebuggerMarkPreviouslyActive), Flash(Node.bDebuggerMarkFlashActive)
		, Succeeded(Node.bDebuggerMarkSearchSucceeded), Failed(Node.bDebuggerMarkSearchFailed), Trigger(Node.bDebuggerMarkSearchTrigger), FailedTrigger(Node.bDebuggerMarkSearchFailedTrigger), Reverse(Node.bDebuggerMarkSearchReverseConnection), BreakpointTrigger(Node.bDebuggerMarkBreakpointTrigger)
		, SearchIndex(Node.DebuggerSearchPathIndex), SearchSize(Node.DebuggerSearchPathSize), UpdateCounter(Node.DebuggerUpdateCounter), Description(Node.DebuggerRuntimeDescription) {}
	bool Equals(const UBehaviorTreeGraphNode& Node) const
	{
		return Breakpoint == Node.bHasBreakpoint && BreakpointEnabled == Node.bIsBreakpointEnabled && Current == Node.bDebuggerMarkCurrentlyActive && Previous == Node.bDebuggerMarkPreviouslyActive && Flash == Node.bDebuggerMarkFlashActive
			&& Succeeded == Node.bDebuggerMarkSearchSucceeded && Failed == Node.bDebuggerMarkSearchFailed && Trigger == Node.bDebuggerMarkSearchTrigger && FailedTrigger == Node.bDebuggerMarkSearchFailedTrigger && Reverse == Node.bDebuggerMarkSearchReverseConnection && BreakpointTrigger == Node.bDebuggerMarkBreakpointTrigger
			&& SearchIndex == Node.DebuggerSearchPathIndex && SearchSize == Node.DebuggerSearchPathSize && UpdateCounter == Node.DebuggerUpdateCounter && Description == Node.DebuggerRuntimeDescription;
	}
};


class FNoCancellation final : public IAssetDocumentCancellationView
{
public:
	virtual bool IsCancellationRequested() const override { return false; }
};

class FOrderedAuthoredAction final : public IAssetDocumentRollbackAction
{
public:
	FOrderedAuthoredAction(
		FString InComponentId,
		TArray<FString>& InOrder,
		FString InName,
		TFunction<void()> InRestore = {})
		: ComponentId(MoveTemp(InComponentId)), Order(InOrder), Name(MoveTemp(InName)), Restore(MoveTemp(InRestore)) {}
	virtual FStringView GetRollbackComponentId() const override { return ComponentId; }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		Order.Add(Name);
		if (Restore)
		{
			Restore();
		}
		FAssetDocumentContractValidationResult Result; Result.bValid = true; return Result;
	}
private:
	FString ComponentId;
	TArray<FString>& Order;
	FString Name;
	TFunction<void()> Restore;
};

FAssetDocumentMutationFootprint MakeRuntimeMirrorFootprint(const UBehaviorTree& Tree)
{
	FAssetDocumentMutationFootprint Footprint;
	Footprint.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
	Footprint.StableId = TEXT("asset:") + Tree.GetPathName() + TEXT("/RuntimeMirror");
	Footprint.RollbackComponentId = ComponentId;
	return Footprint;
}

// This resolver is test-local storage for the real opaque payload issued by
// editor.asset.resolve_existing.v1.  It does not construct an Editor payload.
class FResolveExistingHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	struct FEntry
	{
		EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Object;
		EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::ReadOnly;
		FString ProviderId;
		uint32 Generation = 1;
		TSharedPtr<IAssetDocumentNativeHandlePayload> Payload;
	};

	class FLease final : public IAssetDocumentVerifiedHandleLease
	{
	public:
		FLease(FAssetDocumentOpaqueHandle InHandle,
			IAssetDocumentNativeHandlePayload& InPayload)
			: Handle(MoveTemp(InHandle))
			, Payload(&InPayload)
		{
		}
		virtual const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
		virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override
		{
			check(Payload != nullptr);
			return *Payload;
		}
	private:
		FAssetDocumentOpaqueHandle Handle;
		IAssetDocumentNativeHandlePayload* Payload = nullptr;
	};

	void SetMutationToken(const FAssetDocumentMutationToken& InToken) { Token = InToken; }

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		FAssetDocumentIssuedHandleResult Result;
		if (!Payload || !Payload->IsAlive())
		{
			return Result;
		}
		FEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Kind = Kind;
		Entry.Access = Access;
		Entry.ProviderId = TEXT("editor");
		Entry.Payload = MakeShareable(Payload.Release());
		FAssetDocumentOpaqueHandle Handle;
		Handle.RequestId = Token.RequestId;
		Handle.TransactionId = Token.TransactionId;
		Handle.RuntimeRevision = Token.RuntimeRevision;
		Handle.ProviderId = Entry.ProviderId;
		Handle.Slot = Entries.Num();
		Handle.Generation = Entry.Generation;
		Handle.Kind = Kind;
		Handle.Access = Access;
		Result.Handle = Handle;
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) override
	{
		FAssetDocumentVerifiedLeaseResult Result;
		const FAssetDocumentContractValidationResult Metadata =
			ValidateAssetDocumentHandleMetadata(Handle, Context);
		if (!Metadata.bValid)
		{
			Result.Diagnostics = Metadata.Diagnostics;
			return Result;
		}
		const int32 Index = static_cast<int32>(Handle.Slot) - 1;
		if (!Entries.IsValidIndex(Index))
		{
			return Result;
		}
		FEntry& Entry = Entries[Index];
		IAssetDocumentNativeHandlePayload* Payload = Entry.Payload.Get();
		if (Entry.Generation == Handle.Generation && Entry.Kind == Handle.Kind
			&& Entry.Access == Handle.Access && Entry.ProviderId == Handle.ProviderId
			&& Payload != nullptr && Payload->IsAlive())
		{
			Result.Lease = MakeUnique<FLease>(Handle, *Payload);
		}
		return Result;
	}

	TOptional<FAssetDocumentOpaqueHandle> RegisterProviderScopedAlias(
		const FAssetDocumentOpaqueHandle& SourceHandle,
		FStringView TargetProviderId,
		EAssetDocumentHandleAccess RequestedAccess)
	{
		const int32 SourceIndex = static_cast<int32>(SourceHandle.Slot) - 1;
		if (!Entries.IsValidIndex(SourceIndex) || TargetProviderId.IsEmpty())
		{
			return {};
		}
		FEntry& SourceEntry = Entries[SourceIndex];
		const TSharedPtr<IAssetDocumentNativeHandlePayload> SharedPayload = SourceEntry.Payload;
		if (SourceEntry.Generation != SourceHandle.Generation
			|| SourceEntry.ProviderId != SourceHandle.ProviderId
			|| !SharedPayload.IsValid() || !SharedPayload->IsAlive())
		{
			return {};
		}

		FEntry& TargetEntry = Entries.AddDefaulted_GetRef();
		TargetEntry.Kind = SourceHandle.Kind;
		TargetEntry.Access = RequestedAccess;
		TargetEntry.ProviderId = FString(TargetProviderId);
		TargetEntry.Payload = SharedPayload;
		FAssetDocumentOpaqueHandle TargetHandle = SourceHandle;
		TargetHandle.ProviderId = TargetEntry.ProviderId;
		TargetHandle.Slot = Entries.Num();
		TargetHandle.Generation = TargetEntry.Generation;
		TargetHandle.Access = RequestedAccess;
		return TargetHandle;
	}
	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		if (Event.RequestId == Token.RequestId && Event.TransactionId == Token.TransactionId
			&& Event.RuntimeRevision == Token.RuntimeRevision
			&& (Event.ProviderId.IsEmpty()
				|| Entries.ContainsByPredicate([&Event](const FEntry& Entry)
				{
					return Entry.ProviderId == Event.ProviderId;
				})))
		{
			const uint32 Count = static_cast<uint32>(Entries.Num());
			Entries.Reset();
			++InvalidationCount;
			return {Count};
		}
		return {};
	}
	int32 GetInvalidationCount() const { return InvalidationCount; }

private:
	FAssetDocumentMutationToken Token;
	TArray<FEntry> Entries;
	int32 InvalidationCount = 0;
};

class FPublicProviderHandoffContext final : public IAssetDocumentNativeRequestContext
{
public:
	FPublicProviderHandoffContext(
		FAssetDocumentOpaqueHandle InSourceHandle,
		FAssetDocumentOpaqueHandle InTargetHandle,
		const FAssetDocumentMutationToken& Token)
		: SourceHandle(MoveTemp(InSourceHandle))
		, TargetHandle(MoveTemp(InTargetHandle))
	{
		Metadata.RequestId = Token.RequestId;
		Metadata.PinnedRuntimeRevision = Token.RuntimeRevision;
		Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	}

	virtual const FAssetDocumentRequestContextMetadata& GetMetadata() const override
	{
		return Metadata;
	}

	virtual FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation&) override
	{
		return {};
	}

	virtual FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override
	{
		return {};
	}

	virtual FAssetDocumentIssuedHandleResult Handoff(
		const FAssetDocumentOpaqueHandle& RequestedSourceHandle,
		FStringView TargetProviderId,
		EAssetDocumentHandleAccess RequestedAccess) override
	{
		FAssetDocumentIssuedHandleResult Result;
		if (RequestedSourceHandle.RequestId == SourceHandle.RequestId
			&& RequestedSourceHandle.TransactionId == SourceHandle.TransactionId
			&& RequestedSourceHandle.RuntimeRevision == SourceHandle.RuntimeRevision
			&& RequestedSourceHandle.ProviderId == SourceHandle.ProviderId
			&& RequestedSourceHandle.Slot == SourceHandle.Slot
			&& RequestedSourceHandle.Generation == SourceHandle.Generation
			&& RequestedSourceHandle.Kind == SourceHandle.Kind
			&& RequestedSourceHandle.Access == SourceHandle.Access
			&& TargetProviderId == TargetHandle.ProviderId
			&& RequestedAccess == TargetHandle.Access)
		{
			Result.Handle = TargetHandle;
		}
		return Result;
	}

private:
	FAssetDocumentOpaqueHandle SourceHandle;
	FAssetDocumentOpaqueHandle TargetHandle;
	FAssetDocumentRequestContextMetadata Metadata;
};

bool RunStaticFallbackGuard(FAutomationTestBase& Test)
{
	const FString SourcePath = FPaths::Combine(
		FPaths::ConvertRelativePathToFull(GetAssetDocumentPluginBaseDir()),
		TEXT("Source/AssetDocumentAI/Private/BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollback.cpp"));
	FString Source;
	if (!Test.TestTrue(TEXT("Node B implementation source exists for static fallback guard"),
		FFileHelper::LoadFileToString(Source, *SourcePath)))
	{
		return false;
	}
	const TCHAR* Forbidden[] = {
		TEXT("Private/Provider"), TEXT("BehaviorTreeEditor/Private"), TEXT("AIModule/Private"),
		TEXT("Engine/Source"), TEXT("Serialize"), TEXT("FArchive"), TEXT("FindProperty"),
		TEXT("ProcessEvent"), TEXT("DuplicateObject"), TEXT("LoadObject"), TEXT("GEditor->UndoTransaction"),
		TEXT("UpdateAsset()"), TEXT("RootNode ="), TEXT("FObjectWriter"), TEXT("FObjectReader"),
		TEXT("StaticConstructObject"), TEXT("ReloadPackage"), TEXT("LoadPackage"), TEXT("Children.Add"),
		TEXT("Services.Add"), TEXT("Decorators.Add"), TEXT("RootDecorators.Add"),
	};
	bool bPassed = true;
	for (const TCHAR* Token : Forbidden)
	{
		bPassed &= Test.TestFalse(*FString::Printf(TEXT("static guard excludes %s"), Token),
			Source.Contains(Token));
	}
	bPassed &= Test.TestFalse(TEXT("semantic hash/readback excludes fixture initialize-counter state"),
		Source.Contains(TEXT("GetInitializeInvocationCount()")));
	return bPassed;
}

TUniquePtr<IAssetDocumentVerifiedHandleLease> ResolveExistingMutableAssetLease(
	FAutomationTestBase& Test,
	UObject& Object,
	IAssetDocumentAtomicTransaction& Transaction,
	FResolveExistingHandleResolver& Resolver)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = TEXT("editor.asset.resolve_existing.v1");
	Invocation.Version = 1;
	Invocation.RequestId = Transaction.GetMutationToken().RequestId;
	Invocation.TransactionId = Transaction.GetMutationToken().TransactionId;
	Invocation.RuntimeRevision = Transaction.GetMutationToken().RuntimeRevision;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	Invocation.ValueArguments.ObjectKeys = {TEXT("path"), TEXT("expected_class")};
	FAssetDocumentRuntimeValueDto Path;
	Path.Kind = EAssetDocumentRuntimeValueKind::String;
	Path.StringValue = Object.GetPathName();
	FAssetDocumentRuntimeValueDto Class;
	Class.Kind = EAssetDocumentRuntimeValueKind::String;
	Class.StringValue = Object.GetClass()->GetPathName();
	Invocation.ValueArguments.Elements = {MoveTemp(Path), MoveTemp(Class)};
	const FAssetDocumentPrimitiveOutcome Outcome =
		CreateAssetDocumentEditorPrimitiveProvider()->Invoke(Invocation, Resolver, nullptr);
	if (!Test.TestEqual(TEXT("public resolve-existing provider succeeds for exact test tree"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return nullptr;
	}
	for (const FAssetDocumentPrimitiveHandleResult& Result : Outcome.HandleResults)
	{
		if (Result.Name == TEXT("asset_mutable") && Result.Handles.Num() == 1)
		{
			FAssetDocumentHandleValidationContext Context;
			Context.RequestId = Transaction.GetMutationToken().RequestId;
			Context.TransactionId = Transaction.GetMutationToken().TransactionId;
			Context.RuntimeRevision = Transaction.GetMutationToken().RuntimeRevision;
			Context.ProviderId = TEXT("editor");
			Context.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
			Context.RequiredKind = EAssetDocumentHandleKind::Asset;
			Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
			return Resolver.AcquireVerifiedLease(Result.Handles[0], Context).Lease;
		}
	}
	Test.AddError(TEXT("resolve-existing returned no exact mutable Asset lease"));
	return nullptr;
}

FTestSemanticResult InvokeHelperAdapter(
	const IAssetDocumentVerifiedHandleLease& Lease,
	UBehaviorTreeGraph& Graph,
	int32 UpdateFlags,
	const FAssetDocumentMutationFootprint& Footprint,
	IAssetDocumentAtomicTransaction& Transaction)
{
	FTestSemanticResult Result;
#if ASSETDOCUMENT_AI_BT_SEMANTIC_ROLLBACK_HELPER_PRESENT
	const FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult Production =
		RebuildAssetDocumentBehaviorTreeRuntimeMirror(
			Lease, Graph, UpdateFlags, Footprint, Transaction);
	Result.bSucceeded = Production.bSucceeded;
	Result.RootNodeClassPath = Production.RootNodeClassPath;
	Result.NodeCount = Production.NodeCount;
	Result.ExecutionOrderHash = Production.ExecutionOrderHash;
	Result.BlackboardEvidenceHash = Production.BlackboardEvidenceHash;
	Result.Diagnostics = Production.Diagnostics;
#else
	(void)Lease; (void)Graph; (void)UpdateFlags; (void)Footprint; (void)Transaction;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/BehaviorTreeSemanticRollback");
	Diagnostic.Code = TEXT("Test.NodeBMissing");
	Diagnostic.Message = TEXT("Node B exported helper is absent; this is the intentional failure terminal.");
#endif
	return Result;
}

struct FFixture
{
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UBehaviorTree> Tree;
	TStrongObjectPtr<UBehaviorTreeGraph> Graph;
	TStrongObjectPtr<UBlackboardData> Blackboard;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Root> Root;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Task> TaskGraphNode;
	TStrongObjectPtr<UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestTask> Task;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Composite> Composite;
	TStrongObjectPtr<UBehaviorTreeGraphNode_CompositeDecorator> RootDecorator;
	TStrongObjectPtr<UBehaviorTreeGraphNode_CompositeDecorator> Decorator;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Service> Service;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Service> TaskService;
};

UEdGraphPin* FindPinByDirection(const UEdGraphNode& Node, EEdGraphPinDirection Direction)
{
	for (UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin && Pin->Direction == Direction) return Pin;
	}
	return nullptr;
}

UBehaviorTreeGraphNode* FindRuntimeRootGraphNode(UBehaviorTreeGraphNode_Root& Root)
{
	UEdGraphPin* RootOutput = FindPinByDirection(Root, EGPD_Output);
	return RootOutput != nullptr && RootOutput->LinkedTo.Num() > 0
		? Cast<UBehaviorTreeGraphNode>(RootOutput->LinkedTo[0]->GetOwningNode())
		: nullptr;
}

int32 CountReachableRuntimeNodeClosure(const UBTNode& Node, TSet<const UBTNode*>& Visited)
{
	if (Visited.Contains(&Node))
	{
		return 0;
	}
	Visited.Add(&Node);
	int32 Count = 1;
	if (const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(&Node))
	{
		for (const UBTService* Service : Composite->Services)
		{
			if (Service != nullptr) Count += CountReachableRuntimeNodeClosure(*Service, Visited);
		}
		for (const FBTCompositeChild& Child : Composite->Children)
		{
			for (const UBTDecorator* Decorator : Child.Decorators)
			{
				if (Decorator != nullptr) Count += CountReachableRuntimeNodeClosure(*Decorator, Visited);
			}
			if (Child.ChildComposite != nullptr) Count += CountReachableRuntimeNodeClosure(*Child.ChildComposite, Visited);
			else if (Child.ChildTask != nullptr) Count += CountReachableRuntimeNodeClosure(*Child.ChildTask, Visited);
		}
	}
	else if (const UBTTaskNode* Task = Cast<UBTTaskNode>(&Node))
	{
		for (const UBTService* Service : Task->Services)
		{
			if (Service != nullptr) Count += CountReachableRuntimeNodeClosure(*Service, Visited);
		}
	}
	return Count;
}

int32 CountCanonicalRuntimeNodeClosure(const UBehaviorTree& Tree)
{
	TSet<const UBTNode*> Visited;
	int32 Count = 0;
	for (const UBTDecorator* RootDecorator : Tree.RootDecorators)
	{
		if (RootDecorator != nullptr) Count += CountReachableRuntimeNodeClosure(*RootDecorator, Visited);
	}
	if (Tree.RootNode != nullptr) Count += CountReachableRuntimeNodeClosure(*Tree.RootNode, Visited);
	return Count;
}

enum class EDecoratorWrapperKind : uint8
{
	Ordinary,
	Composite,
};

bool MatchesDecoratorIdentities(
	const TArray<TObjectPtr<UBTDecorator>>& Actual,
	TConstArrayView<UBTDecorator*> Expected)
{
	if (Actual.Num() != Expected.Num()) return false;
	for (int32 Index = 0; Index < Actual.Num(); ++Index)
	{
		if (Actual[Index] != Expected[Index]) return false;
	}
	return true;
}

UBehaviorTreeGraphNode_Decorator* AddOrdinaryForceSuccessDecorator(
	FAutomationTestBase& Test,
	UBehaviorTreeGraph& Graph,
	UBehaviorTreeGraphNode& Parent)
{
	FAISchemaAction_NewSubNode Action;
	Action.ParentNode = &Parent;
	Action.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Decorator>();
	Action.NodeTemplate->NodeInstance = NewObject<UBTDecorator_ForceSuccess>(Action.NodeTemplate);
	const int32 DecoratorCountBeforeAction = Parent.Decorators.Num();
	Action.PerformAction(&Graph, nullptr, FVector2f::ZeroVector, false);
	UBehaviorTreeGraphNode_Decorator* Added = Parent.Decorators.IsValidIndex(DecoratorCountBeforeAction)
		? Cast<UBehaviorTreeGraphNode_Decorator>(Parent.Decorators[DecoratorCountBeforeAction]) : nullptr;
	if (!Test.TestNotNull(TEXT("stock schema action attaches an ordinary decorator"), Added)) return nullptr;
	return Added;
}

UBehaviorTreeGraphNode_CompositeDecorator* AddNestedCompositeForceSuccessDecorator(
	FAutomationTestBase& Test,
	UBehaviorTreeGraph& Graph,
	UBehaviorTreeGraphNode& Parent)
{
	FAISchemaAction_NewSubNode Action;
	Action.ParentNode = &Parent;
	Action.NodeTemplate = NewObject<UBehaviorTreeGraphNode_CompositeDecorator>();
	const int32 DecoratorCountBeforeAction = Parent.Decorators.Num();
	Action.PerformAction(&Graph, nullptr, FVector2f::ZeroVector, false);
	UBehaviorTreeGraphNode_CompositeDecorator* Composite =
		Parent.Decorators.IsValidIndex(DecoratorCountBeforeAction)
			? Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Parent.Decorators[DecoratorCountBeforeAction]) : nullptr;
	if (!Test.TestNotNull(TEXT("stock schema action attaches a composite decorator"), Composite)) return nullptr;
	UBehaviorTreeDecoratorGraph* BoundGraph = Cast<UBehaviorTreeDecoratorGraph>(Composite->BoundGraph);
	if (!Test.TestNotNull(TEXT("composite wrapper owns a stock decorator graph"), BoundGraph)) return nullptr;

	UBehaviorTreeDecoratorGraphNode_Logic* Sink = nullptr;
	for (UEdGraphNode* Node : BoundGraph->Nodes)
	{
		UBehaviorTreeDecoratorGraphNode_Logic* Logic = Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Node);
		if (Logic && Logic->LogicMode == EDecoratorLogicMode::Sink)
		{
			Sink = Logic;
			break;
		}
	}
	if (!Test.TestNotNull(TEXT("stock decorator graph provides a sink"), Sink)) return nullptr;

	auto AddLeaf = [&]()
	{
		FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Decorator> Creator(*BoundGraph);
		UBehaviorTreeDecoratorGraphNode_Decorator* Leaf = Creator.CreateNode(false);
		Leaf->ClassData = FGraphNodeClassData(UBTDecorator_ForceSuccess::StaticClass(), FString());
		Creator.Finalize();
		return Leaf;
	};
	auto AddLogic = [&](EDecoratorLogicMode::Type Mode)
	{
		FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Logic> Creator(*BoundGraph);
		UBehaviorTreeDecoratorGraphNode_Logic* Logic = Creator.CreateNode(false);
		Logic->LogicMode = Mode;
		Creator.Finalize();
		return Logic;
	};
	auto FindPinAt = [](UEdGraphNode& Node, EEdGraphPinDirection Direction, int32 DesiredIndex)
	{
		int32 Index = 0;
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == Direction && Index++ == DesiredIndex) return Pin;
		}
		return static_cast<UEdGraphPin*>(nullptr);
	};
	auto Connect = [&](UEdGraphNode& From, UEdGraphNode& To, int32 ToInput, const TCHAR* What)
	{
		UEdGraphPin* Output = FindPinAt(From, EGPD_Output, 0);
		UEdGraphPin* Input = FindPinAt(To, EGPD_Input, ToInput);
		return Test.TestTrue(What, Output != nullptr && Input != nullptr
			&& BoundGraph->GetSchema()->TryCreateConnection(Output, Input));
	};

	UBehaviorTreeDecoratorGraphNode_Decorator* FirstLeaf = AddLeaf();
	UBehaviorTreeDecoratorGraphNode_Decorator* SecondLeaf = AddLeaf();
	UBehaviorTreeDecoratorGraphNode_Logic* And = AddLogic(EDecoratorLogicMode::And);
	UBehaviorTreeDecoratorGraphNode_Logic* Not = AddLogic(EDecoratorLogicMode::Not);
	if (!Test.TestTrue(TEXT("nested composite uses two stock decorator leaves"), FirstLeaf && SecondLeaf)
		|| !Test.TestTrue(TEXT("nested composite uses stock And and Not logic nodes"), And && Not)
		|| !Connect(*FirstLeaf, *And, 0, TEXT("first leaf connects to nested And logic"))
		|| !Connect(*SecondLeaf, *And, 1, TEXT("second leaf connects to nested And logic"))
		|| !Connect(*And, *Not, 0, TEXT("And logic connects to nested Not logic"))
		|| !Connect(*Not, *Sink, 0, TEXT("nested Not logic connects to stock sink"))) return nullptr;
	BoundGraph->NotifyGraphChanged();
	Composite->OnInnerGraphChanged();
	TArray<UBTDecorator*> Decorators;
	TArray<FBTDecoratorLogic> Operations;
	Composite->CollectDecoratorData(Decorators, Operations);
	if (!Test.TestEqual(TEXT("nested composite public readback exposes both leaf identities"), Decorators.Num(), 2)
		|| !Test.TestTrue(TEXT("nested composite public readback exposes nested logic operations"), Operations.Num() >= 4)) return nullptr;
	return Composite;
}

bool CollectAuthoredDecoratorData(
	FAutomationTestBase& Test,
	const UBehaviorTreeGraphNode& Parent,
	TArray<UBTDecorator*>& OutDecorators,
	TArray<FBTDecoratorLogic>& OutOperations)
{
	OutDecorators.Reset();
	OutOperations.Reset();
	TArray<FBTDecoratorLogic> RawOperations;
	int32 WrapperCount = 0;
	int32 CompositeWrapperCount = 0;
	for (UBehaviorTreeGraphNode* Wrapper : Parent.Decorators)
	{
		if (Wrapper != nullptr && Wrapper->bInjectedNode)
		{
			continue;
		}
		if (const UBehaviorTreeGraphNode_Decorator* Ordinary = Cast<UBehaviorTreeGraphNode_Decorator>(Wrapper))
		{
			Ordinary->CollectDecoratorData(OutDecorators, RawOperations);
		}
		else if (const UBehaviorTreeGraphNode_CompositeDecorator* Composite =
			Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Wrapper))
		{
			++CompositeWrapperCount;
			Composite->CollectDecoratorData(OutDecorators, RawOperations);
		}
		else
		{
			Test.AddError(TEXT("decorator composition contains an unsupported authored wrapper"));
			return false;
		}
		++WrapperCount;
	}
	if (CompositeWrapperCount == 0)
	{
		return true;
	}
	if (WrapperCount > 1)
	{
		OutOperations.Emplace(static_cast<uint8>(EBTDecoratorLogic::And),
			static_cast<uint16>(WrapperCount));
	}
	OutOperations.Append(MoveTemp(RawOperations));
	return true;
}

bool ConfigureDecoratorWrapperPair(
	FAutomationTestBase& Test,
	UBehaviorTreeGraph& Graph,
	UBehaviorTreeGraphNode& Parent,
	EDecoratorWrapperKind First,
	EDecoratorWrapperKind Second)
{
	const TArray<TObjectPtr<UBehaviorTreeGraphNode>> ExistingDecorators = Parent.Decorators;
	const int32 ServiceCountBeforeDecoratorRemoval = Parent.Services.Num();
	for (UBehaviorTreeGraphNode* ExistingDecorator : ExistingDecorators)
	{
		Parent.RemoveSubNode(ExistingDecorator);
	}
	if (!Test.TestEqual(TEXT("stock parent clears only its pre-existing decorator composition"), Parent.Decorators.Num(), 0)
		|| !Test.TestEqual(TEXT("stock parent retains its service composition while decorators change"),
			Parent.Services.Num(), ServiceCountBeforeDecoratorRemoval)) return false;
	for (const EDecoratorWrapperKind Kind : {First, Second})
	{
		UBehaviorTreeGraphNode* Added = Kind == EDecoratorWrapperKind::Ordinary
			? static_cast<UBehaviorTreeGraphNode*>(AddOrdinaryForceSuccessDecorator(Test, Graph, Parent))
			: static_cast<UBehaviorTreeGraphNode*>(AddNestedCompositeForceSuccessDecorator(Test, Graph, Parent));
		if (Added == nullptr) return false;
	}
	return Test.TestEqual(TEXT("stock schema actions preserve the authored two-wrapper order"), Parent.Decorators.Num(), 2);
}

UBehaviorTreeGraphNode_CompositeDecorator* AddSingleTestCompositeDecorator(
	FAutomationTestBase& Test,
	UBehaviorTreeGraph& Graph,
	UBehaviorTreeGraphNode& Parent)
{
	UBehaviorTreeGraphNode_CompositeDecorator* Composite =
		NewObject<UBehaviorTreeGraphNode_CompositeDecorator>(&Graph);
	FAISchemaAction_NewSubNode Action;
	Action.ParentNode = &Parent;
	Action.NodeTemplate = Composite;
	Action.PerformAction(&Graph, nullptr, FVector2f::ZeroVector, false);
	UBehaviorTreeDecoratorGraph* BoundGraph = Cast<UBehaviorTreeDecoratorGraph>(Composite->BoundGraph);
	if (!Test.TestNotNull(TEXT("composite decorator owns its stock bound graph"), BoundGraph)) return nullptr;
	UBehaviorTreeDecoratorGraphNode_Logic* Sink = nullptr;
	for (UEdGraphNode* Node : BoundGraph->Nodes)
	{
		UBehaviorTreeDecoratorGraphNode_Logic* Logic = Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Node);
		if (Logic && Logic->LogicMode == EDecoratorLogicMode::Sink) { Sink = Logic; break; }
	}
	if (!Test.TestNotNull(TEXT("composite decorator bound graph owns its stock sink"), Sink)) return nullptr;
	FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Decorator> NodeCreator(*BoundGraph);
	UBehaviorTreeDecoratorGraphNode_Decorator* TestNode = NodeCreator.CreateNode(false);
	TestNode->ClassData = FGraphNodeClassData(UBTDecorator_ForceSuccess::StaticClass(), FString());
	NodeCreator.Finalize();
	UEdGraphPin* Output = FindPinByDirection(*TestNode, EGPD_Output);
	UEdGraphPin* Input = FindPinByDirection(*Sink, EGPD_Input);
	if (!Test.TestTrue(TEXT("composite decorator test connects to its sink"),
		Output && Input && BoundGraph->GetSchema()->TryCreateConnection(Output, Input))) return nullptr;
	BoundGraph->NotifyGraphChanged();
	Composite->OnInnerGraphChanged();
	return Composite;
}

struct FGraphDebuggerEvidence
{
	FGuid NodeGuid;
	FString ClassPath;
	FPublicDebuggerEvidence Evidence;
	FGraphDebuggerEvidence(UBehaviorTreeGraphNode& InNode)
		: NodeGuid(InNode.NodeGuid), ClassPath(InNode.GetClass()->GetPathName()), Evidence(InNode) {}
};

TArray<FGraphDebuggerEvidence> CaptureAllPublicDebuggerEvidence(const FFixture& Fixture)
{
	TArray<FGraphDebuggerEvidence> Captured;
	auto Capture = [&Captured](UBehaviorTreeGraphNode* Node)
	{
		if (Node && !Captured.ContainsByPredicate([Node](const FGraphDebuggerEvidence& Entry) { return Entry.NodeGuid == Node->NodeGuid; })) Captured.Emplace(*Node);
	};
	for (UEdGraphNode* Node : Fixture.Graph->Nodes) Capture(Cast<UBehaviorTreeGraphNode>(Node));
	Capture(Fixture.RootDecorator.Get()); Capture(Fixture.Decorator.Get());
	Capture(Fixture.Service.Get()); Capture(Fixture.TaskService.Get());
	return Captured;
}

bool DoesAllPublicDebuggerEvidenceMatch(const TArray<FGraphDebuggerEvidence>& Captured, const FFixture& Fixture)
{
	const auto Current = CaptureAllPublicDebuggerEvidence(Fixture);
	for (const FGraphDebuggerEvidence& Entry : Captured)
	{
		const auto* Match = Current.FindByPredicate([&Entry](const FGraphDebuggerEvidence& Item)
		{
			return Item.NodeGuid == Entry.NodeGuid && Item.ClassPath == Entry.ClassPath;
		});
		if (Match == nullptr) return false;
		// Compare public fields independently of the graph wrapper's address.
		const auto& A = Entry.Evidence;
		const auto& B = Match->Evidence;
		if (A.Breakpoint != B.Breakpoint || A.BreakpointEnabled != B.BreakpointEnabled
			|| A.Current != B.Current || A.Previous != B.Previous || A.Flash != B.Flash
			|| A.Succeeded != B.Succeeded || A.Failed != B.Failed || A.Trigger != B.Trigger
			|| A.FailedTrigger != B.FailedTrigger || A.Reverse != B.Reverse || A.BreakpointTrigger != B.BreakpointTrigger
			|| A.SearchIndex != B.SearchIndex || A.SearchSize != B.SearchSize || A.UpdateCounter != B.UpdateCounter
			|| A.Description != B.Description) return false;
	}
	return true;
}

bool MatchesDecoratorOperations(TConstArrayView<FBTDecoratorLogic> Actual, TConstArrayView<FBTDecoratorLogic> Expected)
{
	if (Actual.Num() != Expected.Num()) return false;
	for (int32 Index = 0; Index < Actual.Num(); ++Index)
	{
		if (Actual[Index].Operation != Expected[Index].Operation || Actual[Index].Number != Expected[Index].Number) return false;
	}
	return true;
}

TOptional<FFixture> MakeFixture(FAutomationTestBase& Test)
{
	FFixture Fixture;
	Fixture.Package.Reset(CreatePackage(*FString::Printf(
		TEXT("/Temp/AssetDocumentAI/BehaviorTreeSemanticRollback_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	Fixture.Tree.Reset(Fixture.Package.Get() != nullptr ? NewObject<UBehaviorTree>(Fixture.Package.Get(),
		FName(TEXT("BT_SemanticRollback")), RF_Public | RF_Standalone) : nullptr);
	if (!Test.TestNotNull(TEXT("test-owned live BehaviorTree exists"), Fixture.Tree.Get()))
	{
		return {};
	}
	Fixture.Graph.Reset(Cast<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(
		Fixture.Tree.Get(), TEXT("BehaviorTreeGraph"), UBehaviorTreeGraph::StaticClass(),
		UEdGraphSchema_BehaviorTree::StaticClass())));
	if (!Test.TestNotNull(TEXT("stock public CreateNewGraph creates exact BehaviorTree graph"), Fixture.Graph.Get()))
	{
		return {};
	}
	Fixture.Tree->BTGraph = Fixture.Graph.Get();
	Fixture.Blackboard.Reset(NewObject<UBlackboardData>(Fixture.Package.Get(), TEXT("BB_SemanticRollback"), RF_Public | RF_Standalone));
	Fixture.Blackboard->UpdatePersistentKey<UBlackboardKeyType_Object>(TEXT("Target"));
	Fixture.Tree->BlackboardAsset = Fixture.Blackboard.Get();
	Fixture.Graph->GetSchema()->CreateDefaultNodesForGraph(*Fixture.Graph);
	Fixture.Graph->OnCreated();
	Fixture.Graph->Initialize();
	Fixture.Root.Reset(Fixture.Graph->Nodes.Num() > 0
		? Cast<UBehaviorTreeGraphNode_Root>(Fixture.Graph->Nodes[0]) : nullptr);
	if (!Test.TestNotNull(TEXT("stock BehaviorTree schema creates root node"), Fixture.Root.Get()))
	{
		return {};
	}
	Fixture.Root->BlackboardAsset = Fixture.Blackboard.Get();
	Fixture.Root->UpdateBlackboard();
	if (!Test.TestTrue(TEXT("fixture root selection deterministically owns the tree Blackboard"),
		Fixture.Root->BlackboardAsset == Fixture.Blackboard.Get()
		&& Fixture.Tree->BlackboardAsset == Fixture.Blackboard.Get()))
	{
		return {};
	}
	FAISchemaAction_NewNode CompositeAction;
	CompositeAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Composite>();
	CompositeAction.NodeTemplate->NodeInstance = NewObject<UBTComposite_Sequence>(CompositeAction.NodeTemplate);
	UEdGraphPin* RootOutput = FindPinByDirection(*Fixture.Root, EGPD_Output);
	if (!Test.TestNotNull(TEXT("root exposes an output pin"), RootOutput)) return {};
	Fixture.Composite.Reset(Cast<UBehaviorTreeGraphNode_Composite>(CompositeAction.PerformAction(
		Fixture.Graph.Get(), RootOutput, FVector2f(160.0f, 0.0f), false)));
	if (!Test.TestNotNull(TEXT("schema action creates composite"), Fixture.Composite.Get())) return {};
	Fixture.RootDecorator.Reset(AddSingleTestCompositeDecorator(Test, *Fixture.Graph, *Fixture.Composite));
	if (!Test.TestNotNull(TEXT("runtime-root decorator subnode is retained"), Fixture.RootDecorator.Get())) return {};
	FAISchemaAction_NewNode TaskAction;
	TaskAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Task>();
	UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestTask* TaskTemplate =
		NewObject<UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestTask>(TaskAction.NodeTemplate);
	TaskTemplate->ConfigureSelectedKey(TEXT("Target"), UBlackboardKeyType_Object::StaticClass());
	TaskTemplate->SetExplicitAuthoredValue(17);
	TaskAction.NodeTemplate->NodeInstance = TaskTemplate;
	UEdGraphPin* CompositeOutput = Fixture.Composite.Get() ? FindPinByDirection(*Fixture.Composite, EGPD_Output) : nullptr;
	if (!Test.TestNotNull(TEXT("composite exposes an output pin"), CompositeOutput)) return {};
	UBehaviorTreeGraphNode_Task* TaskGraphNode = Cast<UBehaviorTreeGraphNode_Task>(TaskAction.PerformAction(
		Fixture.Graph.Get(), CompositeOutput,
		FVector2f(320.0f, 0.0f), false));
	Fixture.TaskGraphNode.Reset(TaskGraphNode);
	Fixture.Task.Reset(TaskGraphNode
		? Cast<UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestTask>(TaskGraphNode->NodeInstance)
		: nullptr);
	if (!Test.TestNotNull(TEXT("schema action retains the deterministic Blackboard task instance"), Fixture.Task.Get()))
	{
		return {};
	}
	Fixture.Decorator.Reset(AddSingleTestCompositeDecorator(Test, *Fixture.Graph, *Fixture.TaskGraphNode));
	FAISchemaAction_NewSubNode ServiceAction;
	ServiceAction.ParentNode = Fixture.Composite.Get();
	ServiceAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Service>();
	ServiceAction.NodeTemplate->NodeInstance = NewObject<UBTService_DefaultFocus>(ServiceAction.NodeTemplate);
	ServiceAction.PerformAction(Fixture.Graph.Get(), nullptr, FVector2f::ZeroVector, false);
	Fixture.Service.Reset(Fixture.Composite->Services.Num() > 0 ? Cast<UBehaviorTreeGraphNode_Service>(Fixture.Composite->Services[0]) : nullptr);
	FAISchemaAction_NewSubNode TaskServiceAction;
	TaskServiceAction.ParentNode = Fixture.TaskGraphNode.Get();
	TaskServiceAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Service>();
	TaskServiceAction.NodeTemplate->NodeInstance = NewObject<UBTService_DefaultFocus>(TaskServiceAction.NodeTemplate);
	TaskServiceAction.PerformAction(Fixture.Graph.Get(), nullptr, FVector2f::ZeroVector, false);
	Fixture.TaskService.Reset(Fixture.TaskGraphNode->Services.Num() > 0
		? Cast<UBehaviorTreeGraphNode_Service>(Fixture.TaskGraphNode->Services[0]) : nullptr);
	if (!Test.TestNotNull(TEXT("decorator subnode is retained by the task child"), Fixture.Decorator.Get())
		|| !Test.TestNotNull(TEXT("service subnode is retained by the composite"), Fixture.Service.Get())
		|| !Test.TestNotNull(TEXT("stock service subnode is retained by the task child"), Fixture.TaskService.Get())) return {};
	return Fixture;
}

struct FScenario
{
	TOptional<FFixture> Fixture;
	FResolveExistingHandleResolver Resolver;
	TUniquePtr<FAssetDocumentRollbackActionComposition> Composition;
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction;
	TUniquePtr<IAssetDocumentVerifiedHandleLease> Lease;
	TArray<FString> RollbackOrder;
	~FScenario()
	{
		Lease.Reset();
		Transaction.Reset();
		Composition.Reset();
	}
};

bool SetUpScenario(
	FAutomationTestBase& Test,
	FScenario& Out,
	TFunction<void(FFixture&)> ForwardMutation = {},
	TFunction<void(FFixture&)> RestoreAuthoredState = {})
{
	Out.Fixture = MakeFixture(Test);
	if (!Out.Fixture.IsSet()) return false;
	FFixture& Fixture = Out.Fixture.GetValue();
	if (ForwardMutation) ForwardMutation(Fixture);
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
	Actions.Add(MakeShared<FOrderedAuthoredAction>(TEXT("test.authored.a"), Out.RollbackOrder, TEXT("authored.a"),
		[&Fixture, RestoreAuthoredState]() { if (RestoreAuthoredState) RestoreAuthoredState(Fixture); }));
	Actions.Add(MakeShared<FOrderedAuthoredAction>(TEXT("test.authored.b"), Out.RollbackOrder, TEXT("authored.b")));
	Out.Composition = MakeUnique<FAssetDocumentRollbackActionComposition>(Out.Resolver, MoveTemp(Actions),
		[](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic&) {});
	FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = FGuid::NewGuid(); Receipt.RuntimeRevision = 1; Receipt.ReceiptHash = TEXT("sha256:node-b-test"); Receipt.bSucceeded = true;
	Receipt.CompleteFootprint = {
		{EAssetDocumentMutationFootprintKind::UObjectState, TEXT("asset:/authored/a"), TEXT("test.authored.a")},
		{EAssetDocumentMutationFootprintKind::UObjectState, TEXT("asset:/authored/b"), TEXT("test.authored.b")},
		MakeRuntimeMirrorFootprint(*Fixture.Tree)};
	static FNoCancellation Cancellation;
	Out.Transaction = Out.Composition->BeginLiveAfterPreflight(Receipt, Cancellation);
	if (!Test.TestNotNull(TEXT("real Node A composition creates a live transaction"), Out.Transaction.Get())) return false;
	if (!Test.TestTrue(TEXT("representative authored actions own their declared footprints before Node B"),
		Out.Transaction->EnlistFootprint(MakeArrayView(Receipt.CompleteFootprint.GetData(), 2)).bValid)) return false;
	Out.Resolver.SetMutationToken(Out.Transaction->GetMutationToken());
	Out.Lease = ResolveExistingMutableAssetLease(Test, *Fixture.Tree, *Out.Transaction, Out.Resolver);
	return Test.TestNotNull(TEXT("real editor.asset.resolve_existing.v1 issued a mutable Asset lease"), Out.Lease.Get());
}

bool RequireSuccess(FAutomationTestBase& Test, const FTestSemanticResult& Result, const TCHAR* What)
{
	// Test.NodeBMissing is intentionally an Automation failure: discovery and UHT
	// remain valid, but no frozen behavior has been supplied yet.
	return Test.TestTrue(What, Result.bSucceeded)
		&& Test.TestFalse(TEXT("missing helper terminal never satisfies a Node B contract"),
			HasDiagnostic(Result, TEXT("Test.NodeBMissing")));
}

bool RequireExactReject(FAutomationTestBase& Test, const FTestSemanticResult& Result, const TCHAR* Code)
{
	return Test.TestFalse(TEXT("rejected call has no success result"), Result.bSucceeded)
		&& Test.TestTrue(*FString::Printf(TEXT("rejected call has exact %s diagnostic"), Code), HasDiagnostic(Result, Code))
		&& Test.TestFalse(TEXT("missing-helper pseudo terminal is not accepted"), HasDiagnostic(Result, TEXT("Test.NodeBMissing")));
}

bool RequireEditorNativeIdentityReject(FAutomationTestBase& Test, const FTestSemanticResult& Result)
{
	return Test.TestFalse(TEXT("native identity rejection has no success result"), Result.bSucceeded)
		&& Test.TestTrue(TEXT("native identity rejection preserves EditorNativeState.AssetObjectInvalid"),
			HasDiagnostic(Result, TEXT("EditorNativeState.AssetObjectInvalid")))
		&& Test.TestFalse(TEXT("missing-helper pseudo terminal is not accepted"), HasDiagnostic(Result, TEXT("Test.NodeBMissing")));
}

class FInstrumentedTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	explicit FInstrumentedTransaction(IAssetDocumentAtomicTransaction& InInner) : Inner(InInner) {}
	bool bFailFinalizer = false;
	bool bFailFootprint = false;
	int32 FinalizerCalls = 0;
	int32 FootprintCalls = 0;
	TArray<FString> Calls;
	virtual FGuid GetTransactionId() const override { return Inner.GetTransactionId(); }
	virtual EAssetDocumentAtomicState GetState() const override { return Inner.GetState(); }
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override { return Inner.GetMutationToken(); }
	virtual FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Value) override
	{
		++FootprintCalls; Calls.Add(TEXT("footprint"));
		return bFailFootprint ? Failure(TEXT("Test.InjectedFootprintFailure")) : Inner.EnlistFootprint(Value);
	}
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Inner.MarkReadyToCommit(); }
	virtual FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt& Value) override { return Inner.Commit(Value); }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView Value) override { return Inner.Rollback(Value); }
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction> Value) override { return Inner.EnlistRollbackAction(Value); }
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer> Value) override
	{
		++FinalizerCalls; Calls.Add(TEXT("finalizer"));
		return bFailFinalizer ? Failure(TEXT("Test.InjectedFinalizerFailure")) : Inner.EnlistPostRollbackFinalizer(Value);
	}
private:
	static FAssetDocumentContractValidationResult Failure(const TCHAR* Code)
	{
		FAssetDocumentContractValidationResult Result; Result.bValid = false;
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/NodeBTest"); Diagnostic.Code = Code; Diagnostic.Message = TEXT("Injected enlistment failure.");
		return Result;
	}
	IAssetDocumentAtomicTransaction& Inner;
};

class FMutatedLeaseFacade final : public IAssetDocumentVerifiedHandleLease
{
public:
	FMutatedLeaseFacade(FAssetDocumentOpaqueHandle InHandle, IAssetDocumentNativeHandlePayload& InPayload)
		: Handle(MoveTemp(InHandle)), Payload(InPayload) {}
	virtual const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
	virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return Payload; }
private:
	FAssetDocumentOpaqueHandle Handle;
	IAssetDocumentNativeHandlePayload& Payload;
};

class FScopedObjectFlags final
{
public:
	FScopedObjectFlags(UObject& InObject, EObjectFlags InMask)
		: Object(InObject), Mask(InMask), Original(Object.GetFlags() & Mask) {}
	~FScopedObjectFlags()
	{
		Object.ClearFlags(Mask);
		Object.SetFlags(Original);
	}
	void Set(EObjectFlags Flags) { Object.SetFlags(Flags); }
private:
	UObject& Object;
	EObjectFlags Mask;
	EObjectFlags Original;
};

class FScopedObjectRename final
{
public:
	FScopedObjectRename(UObject& InObject, FName TemporaryName)
		: Object(InObject), OriginalName(Object.GetFName()), OriginalOuter(Object.GetOuter())
	{
		bRenamed = Object.Rename(*TemporaryName.ToString(), OriginalOuter, REN_DontCreateRedirectors | REN_NonTransactional);
	}
	~FScopedObjectRename()
	{
		if (bRenamed) Object.Rename(*OriginalName.ToString(), OriginalOuter, REN_DontCreateRedirectors | REN_NonTransactional);
	}
	bool WasRenamed() const { return bRenamed; }
private:
	UObject& Object;
	FName OriginalName;
	UObject* OriginalOuter = nullptr;
	bool bRenamed = false;
};
} // namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests

void UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestTask::ConfigureSelectedKey(
	FName InKeyName, TSubclassOf<UBlackboardKeyType> InKeyType)
{
	BlackboardKey.SelectedKeyName = InKeyName;
	BlackboardKey.SelectedKeyType = InKeyType;
	BlackboardKey.InvalidateResolvedKey();
}

void UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestTask::InitializeFromAsset(
	UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);
	++InitializeInvocationCount;
	ObservedBlackboardAssetSignature = Asset.GetBlackboardAsset()
		? Asset.GetBlackboardAsset()->GetPathName()
		: FString();
}

void UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestService::InitializeFromAsset(
	UBehaviorTree& Asset)
{
	Super::InitializeFromAsset(Asset);
	++InitializeInvocationCount;
	if (bCorruptParent)
	{
		InitializeNode(nullptr, GetExecutionIndex(), GetMemoryOffset(), GetTreeDepth());
	}
	if (bCorruptChildIndex)
	{
		InitializeParentLink(127);
	}
}

#if ASSETDOCUMENT_AI_BT_SEMANTIC_ROLLBACK_PRIVATE_TEST_HOOK_PRESENT
namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests
{
// The runtime-mirror evidence also records what the fixture task observed in InitializeFromAsset, for as
// long as this module is loaded.
struct FFixtureEvidenceRegistration
{
	FFixtureEvidenceRegistration()
	{
		UE::AssetDocumentAI::BehaviorTreeSemanticRuntimeMirrorRollback::Private::SetBlackboardEvidenceExtensionForTest(
			[](FString& Lines, const FString& RolePath, const UBTNode& Node)
			{
				const UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestTask* Fixture =
					Cast<UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestTask>(&Node);
				if (Fixture == nullptr)
				{
					return;
				}
				const UClass* KeyType = Fixture->GetObservedSelectedKeyType();
				Lines += FString::Printf(TEXT("fixture|%s|%d|%s|%s|%s" LINE_TERMINATOR), *RolePath,
					static_cast<int32>(Fixture->GetObservedSelectedKeyId()),
					KeyType != nullptr ? *KeyType->GetPathName() : TEXT("<none>"),
					*Fixture->GetObservedSelectedKeyName().ToString(), *Fixture->GetObservedBlackboardAssetSignature());
			});
	}
	~FFixtureEvidenceRegistration()
	{
		UE::AssetDocumentAI::BehaviorTreeSemanticRuntimeMirrorRollback::Private::SetBlackboardEvidenceExtensionForTest(nullptr);
	}
};
static FFixtureEvidenceRegistration GFixtureEvidenceRegistration;
} // namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests
#endif


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackAuthoredPreimageTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.AuthoredPreimage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackAuthoredPreimageTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	FScenario Scenario; FGuid OriginalGuid; int32 OriginalValue = 0; int32 ForwardModCounter = 0; UEdGraphNode* Speculative = nullptr; TWeakObjectPtr<UEdGraphNode> SpeculativeWeak; UBlackboardData* OriginalBlackboard = nullptr;
	UEdGraphPin* CompositeOutput = nullptr; UEdGraphPin* TaskInput = nullptr; FString OriginalTaskClass;
	if (!SetUpScenario(*this, Scenario,
		[&](FFixture& F) { OriginalGuid = F.TaskGraphNode->NodeGuid; OriginalValue = F.Task->GetExplicitAuthoredValue(); OriginalBlackboard = F.Root->BlackboardAsset.Get();
			CompositeOutput = FindPinByDirection(*F.Composite, EGPD_Output);
			TaskInput = FindPinByDirection(*F.TaskGraphNode, EGPD_Input);
			OriginalTaskClass = F.TaskGraphNode->NodeInstance->GetClass()->GetPathName();
			if (CompositeOutput && TaskInput) CompositeOutput->BreakLinkTo(TaskInput);
			F.TaskGraphNode->NodeGuid = FGuid::NewGuid(); F.Task->SetExplicitAuthoredValue(99);
			UBlackboardData* SpeculativeBlackboard = NewObject<UBlackboardData>(F.Tree.Get(), TEXT("BB_Speculative")); SpeculativeBlackboard->UpdatePersistentKey<UBlackboardKeyType_Object>(TEXT("Target")); F.Root->BlackboardAsset = SpeculativeBlackboard; F.Tree->BlackboardAsset = SpeculativeBlackboard;
			FAISchemaAction_NewNode SpeculativeAction; SpeculativeAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Composite>(); SpeculativeAction.NodeTemplate->NodeInstance = NewObject<UBTComposite_Sequence>(SpeculativeAction.NodeTemplate);
			Speculative = SpeculativeAction.PerformAction(F.Graph.Get(), CompositeOutput, FVector2f(240.0f, 160.0f), false); SpeculativeWeak = Speculative; if (Speculative != nullptr) { if (UEdGraphPin* SpeculativeOutput = FindPinByDirection(*Speculative, EGPD_Output)) { if (TaskInput) SpeculativeOutput->MakeLinkTo(TaskInput); } } ForwardModCounter = F.Graph->ModCounter; },
		[&](FFixture& F) { F.TaskGraphNode->NodeGuid = OriginalGuid; F.Task->SetExplicitAuthoredValue(OriginalValue); F.Root->BlackboardAsset = OriginalBlackboard; F.Tree->BlackboardAsset = OriginalBlackboard; if (Speculative) { F.Graph->RemoveNode(Speculative); Speculative->MarkAsGarbage(); Speculative = nullptr; } if (CompositeOutput && TaskInput) CompositeOutput->MakeLinkTo(TaskInput); F.Graph->ModCounter = ForwardModCounter; })) return false;
	FFixture& F = Scenario.Fixture.GetValue();
	bool Passed = TestTrue(TEXT("speculative authored node is attached before the forward runtime-mirror rebuild"),
		SpeculativeWeak.IsValid() && F.Graph->Nodes.Contains(SpeculativeWeak.Get()));
	const int32 CounterBeforeHelper = F.Graph->ModCounter;
	TArray<FGraphDebuggerEvidence> StableDebuggerEvidence = CaptureAllPublicDebuggerEvidence(F);
	StableDebuggerEvidence.RemoveAll([Speculative](const FGraphDebuggerEvidence& Entry) { return Speculative && Entry.NodeGuid == Speculative->NodeGuid; });
	for (auto& Entry : StableDebuggerEvidence)
	{
		if (Entry.NodeGuid == F.TaskGraphNode->NodeGuid) Entry.NodeGuid = OriginalGuid;
	}
	const FTestSemanticResult Result = InvokeHelperAdapter(*Scenario.Lease, *F.Graph, UBehaviorTreeGraph::KeepRebuildCounter, MakeRuntimeMirrorFootprint(*F.Tree), *Scenario.Transaction);
	Passed &= RequireSuccess(*this, Result, TEXT("forward rebuild succeeds before authored rollback"));
	Scenario.Transaction->Rollback(TEXT("Test.AuthoredPreimage"));
	Passed &= TestEqual(TEXT("NodeGuid restored by ordinary authored action"), F.TaskGraphNode->NodeGuid, OriginalGuid);
	Passed &= TestEqual(TEXT("explicit authored task property restored"), F.Task->GetExplicitAuthoredValue(), OriginalValue);
	Passed &= TestEqual(TEXT("tree keeps the original graph identity"), F.Tree->BTGraph.Get(), static_cast<UEdGraph*>(F.Graph.Get()));
	Passed &= TestEqual(TEXT("root blackboard authored truth restored"), F.Root->BlackboardAsset.Get(), F.Blackboard.Get());
	Passed &= TestEqual(TEXT("tree blackboard authored truth restored"), F.Tree->BlackboardAsset.Get(), F.Blackboard.Get());
	UEdGraphPin* RestoredOutput = FindPinByDirection(*F.Composite, EGPD_Output);
	UEdGraphPin* RestoredInput = FindPinByDirection(*F.TaskGraphNode, EGPD_Input);
	Passed &= TestTrue(TEXT("authored composite-to-task connection is restored"), RestoredOutput && RestoredInput && RestoredOutput->LinkedTo.Contains(RestoredInput));
	Passed &= TestEqual(TEXT("restored authored node keeps its runtime class"), F.TaskGraphNode->NodeInstance->GetClass()->GetPathName(), OriginalTaskClass);
	Passed &= TestFalse(TEXT("speculative authored node is legally destroyed after ordinary rollback detaches it"), SpeculativeWeak.IsValid());
	Passed &= TestEqual(TEXT("rollback rebuild preserves ModCounter after speculative cleanup"), F.Graph->ModCounter, CounterBeforeHelper);
	Passed &= TestTrue(TEXT("rollback rebuild preserves debugger evidence for every surviving authored graph node"),
		DoesAllPublicDebuggerEvidenceMatch(StableDebuggerEvidence, F));
	UBehaviorTreeGraphNode* RuntimeRootGraphNode = FindRuntimeRootGraphNode(*F.Root);
	Passed &= TestEqual(TEXT("rollback runtime root remains the exact authored graph NodeInstance"),
		RuntimeRootGraphNode != nullptr ? static_cast<UObject*>(RuntimeRootGraphNode->NodeInstance.Get()) : nullptr,
		static_cast<UObject*>(F.Tree->RootNode.Get()));
	Passed &= TestEqual(TEXT("finalizer ignores detached speculative debugger evidence and completes rollback"),
		Scenario.Transaction->GetState(), EAssetDocumentAtomicState::RolledBack);
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackCanonicalDerivedReadbackTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.CanonicalDerivedReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackCanonicalDerivedReadbackTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	FScenario Scenario; if (!SetUpScenario(*this, Scenario)) return false; FFixture& F = Scenario.Fixture.GetValue();
	const FTestSemanticResult Result = InvokeHelperAdapter(*Scenario.Lease, *F.Graph, UBehaviorTreeGraph::KeepRebuildCounter, MakeRuntimeMirrorFootprint(*F.Tree), *Scenario.Transaction);
	bool Passed = RequireSuccess(*this, Result, TEXT("canonical derived readback succeeds"));
	UBehaviorTreeGraphNode* RuntimeRootGraphNode = FindRuntimeRootGraphNode(*F.Root);
	Passed &= TestNotNull(TEXT("graph root links to the authored runtime-root graph node"), RuntimeRootGraphNode);
	Passed &= TestNotNull(TEXT("public runtime root was projected"), F.Tree->RootNode.Get());
	Passed &= TestEqual(TEXT("graph-root-linked NodeInstance is the exact Tree.RootNode identity"),
		RuntimeRootGraphNode != nullptr ? static_cast<UObject*>(RuntimeRootGraphNode->NodeInstance.Get()) : nullptr,
		static_cast<UObject*>(F.Tree->RootNode.Get()));
	Passed &= TestEqual(TEXT("reported root class is the public runtime root class"), Result.RootNodeClassPath, F.Tree->RootNode->GetClass()->GetPathName());
	Passed &= TestEqual(TEXT("reported node count projects the full public runtime closure including task-attached services"),
		Result.NodeCount, CountCanonicalRuntimeNodeClosure(*F.Tree));
	Passed &= TestTrue(TEXT("execution order is a non-empty SHA-256 hex value"), IsSha256(Result.ExecutionOrderHash));
	Passed &= TestTrue(TEXT("Blackboard evidence is a non-empty SHA-256 hex value"), IsSha256(Result.BlackboardEvidenceHash));
	Passed &= TestTrue(TEXT("runtime root exposes its child closure"), F.Tree->RootNode && F.Tree->RootNode->Children.Num() > 0);
	Passed &= TestEqual(TEXT("authored task retains its stock task-attached service"), F.TaskGraphNode->Services.Num(), 1);
	Passed &= TestTrue(TEXT("authored task-attached service uses the selected stock service class"),
		F.TaskService->NodeInstance != nullptr && F.TaskService->NodeInstance->IsA(UBTService_DefaultFocus::StaticClass()));
	TArray<UBTDecorator*> ExpectedRootDecorators; TArray<FBTDecoratorLogic> ExpectedRootOps;
	F.RootDecorator->CollectDecoratorData(ExpectedRootDecorators, ExpectedRootOps);
	Passed &= TestEqual(TEXT("root decorator array is projected from authored root decorator"), F.Tree->RootDecorators.Num(), ExpectedRootDecorators.Num());
	Passed &= TestTrue(TEXT("every root decorator Operation/Number is canonical"), MatchesDecoratorOperations(F.Tree->RootDecoratorOps, ExpectedRootOps));
	if (F.Tree->RootNode && F.Tree->RootNode->Children.Num() > 0)
	{
		const FBTCompositeChild& Child = F.Tree->RootNode->Children[0];
		TArray<UBTDecorator*> ExpectedChildDecorators; TArray<FBTDecoratorLogic> ExpectedChildOps;
		F.Decorator->CollectDecoratorData(ExpectedChildDecorators, ExpectedChildOps);
		Passed &= TestTrue(TEXT("child task/composite runtime pointer is public and non-null"), Child.ChildComposite != nullptr || Child.ChildTask != nullptr);
		Passed &= TestEqual(TEXT("child decorator array is projected from authored decorator"), Child.Decorators.Num(), ExpectedChildDecorators.Num());
		Passed &= TestTrue(TEXT("every child decorator Operation/Number is canonical"), MatchesDecoratorOperations(Child.DecoratorOps, ExpectedChildOps));
		Passed &= TestTrue(TEXT("owning composite services array is public canonical closure"), F.Tree->RootNode->Services.Num() > 0);
		const FBTCompositeChild& TaskChild = Child;
		Passed &= TestNotNull(TEXT("runtime root exposes the authored task child"), TaskChild.ChildTask.Get());
		Passed &= TestEqual(TEXT("runtime task projects exactly one task-attached stock service"),
			TaskChild.ChildTask != nullptr ? TaskChild.ChildTask->Services.Num() : 0, 1);
		Passed &= TestEqual(TEXT("runtime task-attached service preserves the authored graph NodeInstance identity"),
			TaskChild.ChildTask != nullptr && TaskChild.ChildTask->Services.Num() == 1
				? TaskChild.ChildTask->Services[0].Get() : nullptr,
			Cast<UBTService>(F.TaskService->NodeInstance.Get()));
	}
	struct FDecoratorCompositionRow
	{
		const TCHAR* Name;
		bool bRootAttachment;
		EDecoratorWrapperKind First;
		EDecoratorWrapperKind Second;
	};
	const FDecoratorCompositionRow DecoratorRows[] = {
		{TEXT("root ordinary+ordinary"), true, EDecoratorWrapperKind::Ordinary, EDecoratorWrapperKind::Ordinary},
		{TEXT("root ordinary+composite"), true, EDecoratorWrapperKind::Ordinary, EDecoratorWrapperKind::Composite},
		{TEXT("root composite+ordinary"), true, EDecoratorWrapperKind::Composite, EDecoratorWrapperKind::Ordinary},
		{TEXT("root composite+composite"), true, EDecoratorWrapperKind::Composite, EDecoratorWrapperKind::Composite},
		{TEXT("child ordinary+ordinary"), false, EDecoratorWrapperKind::Ordinary, EDecoratorWrapperKind::Ordinary},
		{TEXT("child ordinary+composite"), false, EDecoratorWrapperKind::Ordinary, EDecoratorWrapperKind::Composite},
		{TEXT("child composite+ordinary"), false, EDecoratorWrapperKind::Composite, EDecoratorWrapperKind::Ordinary},
		{TEXT("child composite+composite"), false, EDecoratorWrapperKind::Composite, EDecoratorWrapperKind::Composite},
	};
	for (const FDecoratorCompositionRow& Row : DecoratorRows)
	{
		FScenario MatrixScenario;
		if (!SetUpScenario(*this, MatrixScenario)) return false;
		FFixture& Matrix = MatrixScenario.Fixture.GetValue();
		UBehaviorTreeGraphNode& Parent = Row.bRootAttachment
			? static_cast<UBehaviorTreeGraphNode&>(*Matrix.Composite)
			: static_cast<UBehaviorTreeGraphNode&>(*Matrix.TaskGraphNode);
		if (!ConfigureDecoratorWrapperPair(*this, *Matrix.Graph, Parent, Row.First, Row.Second)) return false;
		auto IsExpectedWrapper = [](const UBehaviorTreeGraphNode* Wrapper, EDecoratorWrapperKind Kind)
		{
			return Kind == EDecoratorWrapperKind::Ordinary
				? Cast<UBehaviorTreeGraphNode_Decorator>(Wrapper) != nullptr
				: Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Wrapper) != nullptr;
		};
		bool RowPassed = TestTrue(*FString::Printf(TEXT("%s authored wrapper order preserves first public wrapper kind"), Row.Name),
			Parent.Decorators.IsValidIndex(0) && IsExpectedWrapper(Parent.Decorators[0], Row.First));
		RowPassed &= TestTrue(*FString::Printf(TEXT("%s authored wrapper order preserves second public wrapper kind"), Row.Name),
			Parent.Decorators.IsValidIndex(1) && IsExpectedWrapper(Parent.Decorators[1], Row.Second));
		TArray<UBTDecorator*> ExpectedDecorators;
		TArray<FBTDecoratorLogic> ExpectedOperations;
		if (!CollectAuthoredDecoratorData(*this, Parent, ExpectedDecorators, ExpectedOperations)) return false;
		const FTestSemanticResult MatrixResult = InvokeHelperAdapter(*MatrixScenario.Lease, *Matrix.Graph,
			UBehaviorTreeGraph::KeepRebuildCounter, MakeRuntimeMirrorFootprint(*Matrix.Tree), *MatrixScenario.Transaction);
		RowPassed &= RequireSuccess(*this, MatrixResult,
			*FString::Printf(TEXT("%s multi-wrapper public composition rebuild succeeds"), Row.Name));
		RowPassed &= TestEqual(*FString::Printf(TEXT("%s node count is read from the stock runtime closure"), Row.Name),
			MatrixResult.NodeCount, CountCanonicalRuntimeNodeClosure(*Matrix.Tree));
		auto VerifyRuntimeProjection = [&](const TCHAR* Phase)
		{
			bool ProjectionPassed = true;
			if (Row.bRootAttachment)
			{
				ProjectionPassed &= TestTrue(*FString::Printf(TEXT("%s %s root runtime decorator identities preserve authored order"), Row.Name, Phase),
					MatchesDecoratorIdentities(Matrix.Tree->RootDecorators, ExpectedDecorators));
				ProjectionPassed &= TestTrue(*FString::Printf(TEXT("%s %s root runtime decorator Operation/Number values are canonical"), Row.Name, Phase),
					MatchesDecoratorOperations(Matrix.Tree->RootDecoratorOps, ExpectedOperations));
			}
			else
			{
				const UBTCompositeNode* RuntimeRoot = Matrix.Tree->RootNode.Get();
				ProjectionPassed &= TestTrue(*FString::Printf(TEXT("%s %s runtime root exposes its child attachment"), Row.Name, Phase),
					RuntimeRoot != nullptr && RuntimeRoot->Children.IsValidIndex(0));
				if (RuntimeRoot != nullptr && RuntimeRoot->Children.IsValidIndex(0))
				{
					const FBTCompositeChild& RuntimeChild = RuntimeRoot->Children[0];
					ProjectionPassed &= TestTrue(*FString::Printf(TEXT("%s %s child runtime decorator identities preserve authored order"), Row.Name, Phase),
						MatchesDecoratorIdentities(RuntimeChild.Decorators, ExpectedDecorators));
					ProjectionPassed &= TestTrue(*FString::Printf(TEXT("%s %s child runtime decorator Operation/Number values are canonical"), Row.Name, Phase),
						MatchesDecoratorOperations(RuntimeChild.DecoratorOps, ExpectedOperations));
				}
			}
			return ProjectionPassed;
		};
		RowPassed &= VerifyRuntimeProjection(TEXT("forward"));
		MatrixScenario.Transaction->Rollback(TEXT("Test.CanonicalDerivedReadback.DecoratorMatrix"));
		RowPassed &= TestEqual(*FString::Printf(TEXT("%s multi-wrapper rollback completes"), Row.Name),
			MatrixScenario.Transaction->GetState(), EAssetDocumentAtomicState::RolledBack);
		RowPassed &= VerifyRuntimeProjection(TEXT("rollback"));
		Passed &= RowPassed;
	}
	struct FRuntimeLinkCorruptionRow
	{
		const TCHAR* Name;
		bool bCorruptParent;
		bool bCorruptChildIndex;
	};
	for (const FRuntimeLinkCorruptionRow& Row : {
		FRuntimeLinkCorruptionRow{TEXT("service parent"), true, false},
		FRuntimeLinkCorruptionRow{TEXT("auxiliary child index"), false, true}})
	{
		FScenario CorruptionScenario;
		if (!SetUpScenario(*this, CorruptionScenario)) return false;
		FFixture& Corruption = CorruptionScenario.Fixture.GetValue();
		UBehaviorTreeGraphNode_Service* ServiceGraphNode = Row.bCorruptParent
			? Corruption.Service.Get() : Corruption.TaskService.Get();
		UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestService* CorruptingService =
			NewObject<UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestService>(ServiceGraphNode);
		ServiceGraphNode->NodeInstance = CorruptingService;
		CorruptingService->ConfigureRuntimeLinkCorruption(Row.bCorruptParent, Row.bCorruptChildIndex);
		const FTestSemanticResult CorruptionResult = InvokeHelperAdapter(*CorruptionScenario.Lease,
			*Corruption.Graph, UBehaviorTreeGraph::KeepRebuildCounter, MakeRuntimeMirrorFootprint(*Corruption.Tree),
			*CorruptionScenario.Transaction);
		Passed &= RequireExactReject(*this, CorruptionResult,
			TEXT("BehaviorTreeSemanticRollback.SemanticReadbackFailed"));
		Passed &= TestEqual(*FString::Printf(TEXT("%s corruption runs after the real stock UpdateAsset initialization"), Row.Name),
			CorruptingService->GetInitializeInvocationCount(), 1);
		if (Row.bCorruptParent)
		{
			Passed &= TestNull(TEXT("configured service corrupts its own public runtime parent"),
				CorruptingService->GetParentNode());
		}
		else
		{
			Passed &= TestEqual(TEXT("configured service corrupts its own public auxiliary child index"),
				CorruptingService->GetChildIndex(), static_cast<uint8>(127));
		}
		CorruptingService->ConfigureRuntimeLinkCorruption(false, false);
		CorruptionScenario.Transaction->Rollback(TEXT("Test.CanonicalDerivedReadback.RuntimeLinkCorruption"));
		Passed &= TestEqual(*FString::Printf(TEXT("%s corruption scenario cleans up through the real rollback finalizer"), Row.Name),
			CorruptionScenario.Transaction->GetState(), EAssetDocumentAtomicState::RolledBack);
	}

	FScenario PinOrderScenario;
	if (!SetUpScenario(*this, PinOrderScenario)) return false;
	FFixture& PinOrder = PinOrderScenario.Fixture.GetValue();
	PinOrder.Graph->RemoveNode(PinOrder.TaskGraphNode.Get());
	PinOrder.Graph->RemoveNode(PinOrder.Composite.Get());
	FAISchemaAction_NewNode ParallelAction;
	ParallelAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_SimpleParallel>();
	ParallelAction.NodeTemplate->NodeInstance = NewObject<UBTComposite_SimpleParallel>(ParallelAction.NodeTemplate);
	UBehaviorTreeGraphNode_SimpleParallel* Parallel = Cast<UBehaviorTreeGraphNode_SimpleParallel>(
		ParallelAction.PerformAction(PinOrder.Graph.Get(), FindPinByDirection(*PinOrder.Root, EGPD_Output),
			FVector2f(240.0f, 0.0f), false));
	if (!TestNotNull(TEXT("stock schema action creates a SimpleParallel graph node"), Parallel)) return false;
	TArray<UEdGraphPin*> ParallelOutputs;
	for (UEdGraphPin* Pin : Parallel->Pins)
	{
		if (Pin != nullptr && Pin->Direction == EGPD_Output) ParallelOutputs.Add(Pin);
	}
	if (!TestEqual(TEXT("stock SimpleParallel exposes two declared output pins"), ParallelOutputs.Num(), 2)) return false;
	auto AddParallelTask = [&](UEdGraphPin* OutputPin, FVector2f Position)
	{
		FAISchemaAction_NewNode TaskAction;
		TaskAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Task>();
		TaskAction.NodeTemplate->NodeInstance = NewObject<UBTTask_Wait>(TaskAction.NodeTemplate);
		return Cast<UBehaviorTreeGraphNode_Task>(TaskAction.PerformAction(
			PinOrder.Graph.Get(), OutputPin, Position, false));
	};
	UBehaviorTreeGraphNode_Task* MainTask = AddParallelTask(ParallelOutputs[0], FVector2f(640.0f, -80.0f));
	UBehaviorTreeGraphNode_Task* BackgroundTask = AddParallelTask(ParallelOutputs[1], FVector2f(400.0f, 120.0f));
	if (!TestNotNull(TEXT("stock first SimpleParallel output owns the main task"), MainTask)
		|| !TestNotNull(TEXT("stock second SimpleParallel output owns the background node"), BackgroundTask)) return false;
	Passed &= TestTrue(TEXT("SimpleParallel fixture reverses global X order relative to pin declaration order"),
		MainTask->NodePosX > BackgroundTask->NodePosX);
	const FTestSemanticResult PinOrderResult = InvokeHelperAdapter(*PinOrderScenario.Lease,
		*PinOrder.Graph, UBehaviorTreeGraph::KeepRebuildCounter, MakeRuntimeMirrorFootprint(*PinOrder.Tree),
		*PinOrderScenario.Transaction);
	Passed &= RequireSuccess(*this, PinOrderResult,
		TEXT("SimpleParallel canonical readback follows stock pin declaration order"));
	auto VerifyParallelPinOrder = [&](const TCHAR* Phase)
	{
		const UBTCompositeNode* RuntimeParallel = PinOrder.Tree->RootNode.Get();
		bool PinOrderPassed = TestTrue(*FString::Printf(TEXT("%s SimpleParallel runtime root preserves exact authored identity"), Phase),
			RuntimeParallel == Cast<UBTCompositeNode>(Parallel->NodeInstance.Get()));
		PinOrderPassed &= TestTrue(*FString::Printf(TEXT("%s SimpleParallel runtime exposes both declared-pin children"), Phase),
			RuntimeParallel != nullptr && RuntimeParallel->Children.Num() == 2);
		if (RuntimeParallel != nullptr && RuntimeParallel->Children.Num() == 2)
		{
			PinOrderPassed &= TestTrue(*FString::Printf(TEXT("%s first declared output remains the runtime main child"), Phase),
				RuntimeParallel->Children[0].ChildTask.Get() == Cast<UBTTaskNode>(MainTask->NodeInstance.Get()));
			PinOrderPassed &= TestTrue(*FString::Printf(TEXT("%s second declared output remains the runtime background child"), Phase),
				RuntimeParallel->Children[1].ChildTask.Get() == Cast<UBTTaskNode>(BackgroundTask->NodeInstance.Get()));
		}
		return PinOrderPassed;
	};
	Passed &= VerifyParallelPinOrder(TEXT("forward"));
	PinOrderScenario.Transaction->Rollback(TEXT("Test.CanonicalDerivedReadback.SimpleParallelPinOrder"));
	Passed &= TestEqual(TEXT("SimpleParallel pin-order scenario completes rollback"),
		PinOrderScenario.Transaction->GetState(), EAssetDocumentAtomicState::RolledBack);
	Passed &= VerifyParallelPinOrder(TEXT("rollback"));

	FScenario InjectedDecoratorScenario;
	if (!SetUpScenario(*this, InjectedDecoratorScenario)) return false;
	FFixture& Injected = InjectedDecoratorScenario.Fixture.GetValue();
	if (!ConfigureDecoratorWrapperPair(*this, *Injected.Graph, *Injected.TaskGraphNode,
		EDecoratorWrapperKind::Ordinary, EDecoratorWrapperKind::Ordinary)) return false;
	UBehaviorTreeGraphNode_Decorator* InjectedWrapper =
		Cast<UBehaviorTreeGraphNode_Decorator>(Injected.TaskGraphNode->Decorators[0]);
	UBehaviorTreeGraphNode_Decorator* NormalWrapper =
		Cast<UBehaviorTreeGraphNode_Decorator>(Injected.TaskGraphNode->Decorators[1]);
	if (!TestNotNull(TEXT("injected fixture owns an ordinary injected wrapper"), InjectedWrapper)
		|| !TestNotNull(TEXT("injected fixture owns an ordinary normal sibling"), NormalWrapper)) return false;
	InjectedWrapper->bInjectedNode = true;
	TArray<UBTDecorator*> NonInjectedDecorators;
	TArray<FBTDecoratorLogic> NonInjectedOperations;
	if (!CollectAuthoredDecoratorData(*this, *Injected.TaskGraphNode,
		NonInjectedDecorators, NonInjectedOperations)) return false;
	UBTDecorator* NormalDecorator = Cast<UBTDecorator>(NormalWrapper->NodeInstance.Get());
	Passed &= TestTrue(TEXT("test-side stock projection excludes the injected wrapper identity"),
		NonInjectedDecorators.Num() == 1 && NonInjectedDecorators[0] == NormalDecorator);
	Passed &= TestEqual(TEXT("ordinary non-injected sibling has no composite decorator ops"),
		NonInjectedOperations.Num(), 0);
	const FTestSemanticResult InjectedResult = InvokeHelperAdapter(*InjectedDecoratorScenario.Lease,
		*Injected.Graph, UBehaviorTreeGraph::KeepRebuildCounter, MakeRuntimeMirrorFootprint(*Injected.Tree),
		*InjectedDecoratorScenario.Transaction);
	Passed &= RequireSuccess(*this, InjectedResult,
		TEXT("canonical readback follows stock injected-decorator filtering"));
	auto VerifyInjectedProjection = [&](const TCHAR* Phase)
	{
		const UBTCompositeNode* RuntimeRoot = Injected.Tree->RootNode.Get();
		bool InjectedPassed = TestTrue(*FString::Printf(TEXT("%s injected-decorator scenario exposes its task child"), Phase),
			RuntimeRoot != nullptr && RuntimeRoot->Children.IsValidIndex(0));
		if (RuntimeRoot != nullptr && RuntimeRoot->Children.IsValidIndex(0))
		{
			const FBTCompositeChild& Child = RuntimeRoot->Children[0];
			InjectedPassed &= TestTrue(*FString::Printf(TEXT("%s runtime decorator projection contains only the normal sibling identity"), Phase),
				Child.Decorators.Num() == 1 && Child.Decorators[0].Get() == NormalDecorator);
			InjectedPassed &= TestEqual(*FString::Printf(TEXT("%s filtered ordinary decorator projection has no ops"), Phase),
				Child.DecoratorOps.Num(), 0);
		}
		return InjectedPassed;
	};
	Passed &= VerifyInjectedProjection(TEXT("forward"));
	InjectedDecoratorScenario.Transaction->Rollback(TEXT("Test.CanonicalDerivedReadback.InjectedDecorator"));
	Passed &= TestEqual(TEXT("injected-decorator scenario completes rollback"),
		InjectedDecoratorScenario.Transaction->GetState(), EAssetDocumentAtomicState::RolledBack);
	Passed &= VerifyInjectedProjection(TEXT("rollback"));
	Scenario.Transaction->Rollback(TEXT("Test.CanonicalDerivedReadback"));
	Passed &= TestNotNull(TEXT("rollback finalizer restores public runtime root"), F.Tree->RootNode.Get());
	RuntimeRootGraphNode = FindRuntimeRootGraphNode(*F.Root);
	Passed &= TestEqual(TEXT("rollback preserves graph-root-linked NodeInstance and Tree.RootNode identity"),
		RuntimeRootGraphNode != nullptr ? static_cast<UObject*>(RuntimeRootGraphNode->NodeInstance.Get()) : nullptr,
		static_cast<UObject*>(F.Tree->RootNode.Get()));
	Passed &= TestEqual(TEXT("rollback preserves the full canonical public runtime closure"),
		Result.NodeCount, CountCanonicalRuntimeNodeClosure(*F.Tree));
	Passed &= TestTrue(TEXT("rollback finalizer restores root decorator-op values"), MatchesDecoratorOperations(F.Tree->RootDecoratorOps, ExpectedRootOps));
	if (F.Tree->RootNode && F.Tree->RootNode->Children.Num() > 0)
	{
		TArray<UBTDecorator*> IgnoredDecorators; TArray<FBTDecoratorLogic> ExpectedChildOps;
		F.Decorator->CollectDecoratorData(IgnoredDecorators, ExpectedChildOps);
		Passed &= TestTrue(TEXT("rollback finalizer restores child decorator-op values"), MatchesDecoratorOperations(F.Tree->RootNode->Children[0].DecoratorOps, ExpectedChildOps));
	}
	FScenario IncoherentScenario;
	TWeakObjectPtr<UBehaviorTreeGraphNode_Task> OrphanedAuthoredNode;
	if (!SetUpScenario(*this, IncoherentScenario, {}, [&](FFixture& IncoherentFixture)
		{
			UBehaviorTreeGraphNode_Task* Orphan = NewObject<UBehaviorTreeGraphNode_Task>(IncoherentFixture.Graph.Get());
			Orphan->NodeInstance = NewObject<UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestTask>(Orphan);
			IncoherentFixture.Graph->AddNode(Orphan, false, false);
			OrphanedAuthoredNode = Orphan;
		})) return false;
	FFixture& IncoherentFixture = IncoherentScenario.Fixture.GetValue();
	Passed &= RequireSuccess(*this,
		InvokeHelperAdapter(*IncoherentScenario.Lease, *IncoherentFixture.Graph, 2,
			MakeRuntimeMirrorFootprint(*IncoherentFixture.Tree), *IncoherentScenario.Transaction),
		TEXT("forward rebuild succeeds before an authored rollback owner restores incoherent graph state"));
	const int32 InitializeAfterForward = IncoherentFixture.Task->GetInitializeInvocationCount();
	IncoherentScenario.Transaction->Rollback(TEXT("Test.CanonicalDerivedReadback.IncoherentAuthoredGraph"));
	Passed &= TestEqual(TEXT("incoherent restored authored closure poisons instead of accepting a runtime-only projection"),
		IncoherentScenario.Transaction->GetState(), EAssetDocumentAtomicState::Poisoned);
	Passed &= TestEqual(TEXT("incoherent restored authored closure is rejected before rollback UpdateAsset"),
		IncoherentFixture.Task->GetInitializeInvocationCount(), InitializeAfterForward);
	Passed &= TestTrue(TEXT("semantic verification never repairs or removes the incoherent authored node"),
		OrphanedAuthoredNode.IsValid() && IncoherentFixture.Graph->Nodes.Contains(OrphanedAuthoredNode.Get()));
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackUpdateFlagsStrictTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.UpdateFlagsStrict",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackUpdateFlagsStrictTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	bool Passed = true;
	for (const int32 Flags : {0, 1, 3, -1, 4}) { FScenario S; if (!SetUpScenario(*this, S)) return false; FFixture& F = S.Fixture.GetValue();
		const int32 InitBefore = F.Task->GetInitializeInvocationCount(); const bool DirtyBefore = F.Package->IsDirty();
		const FTestSemanticResult R = InvokeHelperAdapter(*S.Lease, *F.Graph, Flags, MakeRuntimeMirrorFootprint(*F.Tree), *S.Transaction);
		Passed &= RequireExactReject(*this, R, TEXT("BehaviorTreeSemanticRollback.UpdateFlagsRejected"));
		Passed &= TestEqual(TEXT("unsafe flags do not initialize the fixture"), F.Task->GetInitializeInvocationCount(), InitBefore);
		Passed &= TestEqual(TEXT("unsafe flags do not dirty the package"), F.Package->IsDirty(), DirtyBefore); }
	FScenario Valid; if (!SetUpScenario(*this, Valid)) return false; FFixture& F = Valid.Fixture.GetValue();
	Passed &= RequireSuccess(*this, InvokeHelperAdapter(*Valid.Lease, *F.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), *Valid.Transaction), TEXT("only literal KeepRebuildCounter is accepted"));
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackCounterDebuggerDirtyTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.CounterDebuggerDirty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackCounterDebuggerDirtyTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	bool Passed = true; for (const bool Dirty : {false, true}) { FScenario S; if (!SetUpScenario(*this, S)) return false; FFixture& F = S.Fixture.GetValue();
		F.Package->SetDirtyFlag(Dirty); const int32 Counter = F.Graph->ModCounter; const TArray<FGraphDebuggerEvidence> Debugger = CaptureAllPublicDebuggerEvidence(F);
		Passed &= RequireSuccess(*this, InvokeHelperAdapter(*S.Lease, *F.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), *S.Transaction), TEXT("counter/debugger forward rebuild succeeds"));
		Passed &= TestTrue(TEXT("forward rebuild preserves every public debugger marker/counter/description"), DoesAllPublicDebuggerEvidenceMatch(Debugger, F));
		S.Transaction->Rollback(TEXT("Test.CounterDebuggerDirty"));
		Passed &= TestEqual(TEXT("ModCounter stays bit-exact"), F.Graph->ModCounter, Counter);
		Passed &= TestTrue(TEXT("every graph node and direct decorator/service public debugger marker/counter/description stays exact"), DoesAllPublicDebuggerEvidenceMatch(Debugger, F));
		Passed &= TestEqual(TEXT("dirty state is restored as final finalizer write"), F.Package->IsDirty(), Dirty); }
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackRestoredEditorStateTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.RestoredEditorState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackRestoredEditorStateTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	FScenario Scenario;
	if (!SetUpScenario(*this, Scenario,
		[](FFixture& F)
		{
			F.TaskGraphNode->bHasBreakpoint = false;
			F.TaskGraphNode->DebuggerRuntimeDescription = TEXT("forward");
			F.Graph->ModCounter = 21;
		},
		[](FFixture& F)
		{
			F.TaskGraphNode->bHasBreakpoint = true;
			F.TaskGraphNode->DebuggerRuntimeDescription = TEXT("restored");
			F.Graph->ModCounter = 20;
			F.Package->SetDirtyFlag(true);
		})) return false;
	FFixture& F = Scenario.Fixture.GetValue();
	bool Passed = RequireSuccess(*this, InvokeHelperAdapter(*Scenario.Lease, *F.Graph,
		UBehaviorTreeGraph::KeepRebuildCounter, MakeRuntimeMirrorFootprint(*F.Tree), *Scenario.Transaction),
		TEXT("forward mirror accepts the current editor state"));
	Scenario.Transaction->Rollback(TEXT("Test.RestoredEditorState"));
	Passed &= TestEqual(TEXT("finalizer accepts ordinary rollback editor state"),
		Scenario.Transaction->GetState(), EAssetDocumentAtomicState::RolledBack);
	Passed &= TestTrue(TEXT("restored breakpoint remains visible"), F.TaskGraphNode->bHasBreakpoint);
	Passed &= TestEqual(TEXT("restored debugger description remains visible"),
		F.TaskGraphNode->DebuggerRuntimeDescription, FString(TEXT("restored")));
	Passed &= TestEqual(TEXT("rebuild preserves the restored modification counter"), F.Graph->ModCounter, 20);
	Passed &= TestTrue(TEXT("rebuild preserves ordinary rollback dirty ownership"), F.Package->IsDirty());
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackDeterministicInitializeFixtureTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.DeterministicInitializeFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackDeterministicInitializeFixtureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	FScenario S; if (!SetUpScenario(*this, S)) return false; FFixture& F = S.Fixture.GetValue();
	const int32 InitializeBefore = F.Task->GetInitializeInvocationCount();
	bool Passed = RequireSuccess(*this, InvokeHelperAdapter(*S.Lease, *F.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), *S.Transaction), TEXT("fixture forward rebuild succeeds"));
	Passed &= TestEqual(TEXT("InitializeFromAsset runs once on forward rebuild"), F.Task->GetInitializeInvocationCount(), InitializeBefore + 1);
	S.Transaction->Rollback(TEXT("Test.DeterministicInitializeFixture"));
	Passed &= TestEqual(TEXT("InitializeFromAsset runs once more in rollback finalizer"), F.Task->GetInitializeInvocationCount(), InitializeBefore + 2);
	Passed &= TestEqual(TEXT("typed selected key name is canonical"), F.Task->GetObservedSelectedKeyName(), FName(TEXT("Target")));
	Passed &= TestNotEqual(TEXT("typed selected key id is resolved"), F.Task->GetObservedSelectedKeyId(), FBlackboard::InvalidKey);
	Passed &= TestEqual(TEXT("typed selected key type is canonical"), F.Task->GetObservedSelectedKeyType(), UBlackboardKeyType_Object::StaticClass());
	Passed &= TestEqual(TEXT("fixture observes the tree Blackboard signature"), F.Task->GetObservedBlackboardAssetSignature(), F.Blackboard->GetPathName());
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackProviderScopedHandoffLeaseTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.ProviderScopedHandoffLease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackProviderScopedHandoffLeaseTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	FScenario Scenario;
	if (!SetUpScenario(*this, Scenario)) return false;
	FFixture& Fixture = Scenario.Fixture.GetValue();
	const FAssetDocumentOpaqueHandle EditorHandle = Scenario.Lease->GetHandle();

	FAssetDocumentHandleValidationContext AIContext;
	AIContext.RequestId = Scenario.Transaction->GetMutationToken().RequestId;
	AIContext.TransactionId = Scenario.Transaction->GetMutationToken().TransactionId;
	AIContext.RuntimeRevision = Scenario.Transaction->GetMutationToken().RuntimeRevision;
	AIContext.ProviderId = TEXT("ai");
	AIContext.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	AIContext.RequiredKind = EAssetDocumentHandleKind::Asset;
	AIContext.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	const FAssetDocumentVerifiedLeaseResult DirectForeign =
		Scenario.Resolver.AcquireVerifiedLease(EditorHandle, AIContext);
	bool Passed = TestNull(TEXT("the original Editor handle is not an AI-scoped lease"), DirectForeign.Lease.Get());
	Passed &= TestTrue(TEXT("direct foreign-provider validation preserves ProviderMismatch"),
		DirectForeign.Diagnostics.ContainsByPredicate([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == TEXT("HandleMetadata.ProviderMismatch");
		}));

	const TOptional<FAssetDocumentOpaqueHandle> RegisteredAIHandle =
		Scenario.Resolver.RegisterProviderScopedAlias(
			EditorHandle, TEXTVIEW("ai"), EAssetDocumentHandleAccess::Mutable);
	if (!TestTrue(TEXT("test setup registers a fresh AI alias over the real Editor payload"),
		RegisteredAIHandle.IsSet()))
	{
		return false;
	}
	FPublicProviderHandoffContext HandoffContext(
		EditorHandle, RegisteredAIHandle.GetValue(),
		Scenario.Transaction->GetMutationToken());
	const FAssetDocumentIssuedHandleResult Handoff = HandoffContext.Handoff(
		EditorHandle, TEXTVIEW("ai"), EAssetDocumentHandleAccess::Mutable);
	if (!TestTrue(TEXT("public handoff re-signs the mutable Editor Asset for AI"), Handoff.Handle.IsSet()))
	{
		return false;
	}
	const FAssetDocumentOpaqueHandle AIHandle = Handoff.Handle.GetValue();
	Passed &= TestEqual(TEXT("handoff provider identity is AI"), AIHandle.ProviderId, FString(TEXT("ai")));
	Passed &= TestNotEqual(TEXT("handoff allocates a fresh slot"), AIHandle.Slot, EditorHandle.Slot);
	Passed &= TestEqual(TEXT("handoff preserves request identity"), AIHandle.RequestId, EditorHandle.RequestId);
	Passed &= TestEqual(TEXT("handoff preserves transaction identity"), AIHandle.TransactionId, EditorHandle.TransactionId);
	Passed &= TestEqual(TEXT("handoff preserves runtime revision"), AIHandle.RuntimeRevision, EditorHandle.RuntimeRevision);

	FAssetDocumentVerifiedLeaseResult AILease =
		Scenario.Resolver.AcquireVerifiedLease(AIHandle, AIContext);
	if (!TestNotNull(TEXT("resolver verifies the handed-off AI-scoped mutable Asset lease"), AILease.Lease.Get()))
	{
		return false;
	}
	const FTestSemanticResult Result = InvokeHelperAdapter(
		*AILease.Lease, *Fixture.Graph, UBehaviorTreeGraph::KeepRebuildCounter,
		MakeRuntimeMirrorFootprint(*Fixture.Tree), *Scenario.Transaction);
	Passed &= RequireSuccess(*this, Result,
		TEXT("shared runtime-mirror helper accepts the caller-validated AI lease"));
	if (Result.bSucceeded)
	{
		Passed &= TestTrue(TEXT("AI-scoped helper activation rolls back through the shared finalizer"),
			Scenario.Transaction->Rollback(TEXTVIEW("Test.ProviderScopedHandoffLease")).bValid);
		const FAssetDocumentVerifiedLeaseResult Invalidated =
			Scenario.Resolver.AcquireVerifiedLease(AIHandle, AIContext);
		Passed &= TestNull(TEXT("rollback invalidation prevents reacquiring the handed-off AI lease"),
			Invalidated.Lease.Get());
	}
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackRejectsGraphIdentityTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.RejectsGraphIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackRejectsGraphIdentityTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	FScenario S; if (!SetUpScenario(*this, S)) return false; FFixture& F = S.Fixture.GetValue(); bool Passed = true;
	struct FGraphIdentityCase { const TCHAR* Name; UBehaviorTreeGraph* Graph; };
	UBehaviorTreeGraph* WrongOuter = NewObject<UBehaviorTreeGraph>(GetTransientPackage());
	UBehaviorTreeGraph* WrongPackage = NewObject<UBehaviorTreeGraph>(NewObject<UBehaviorTree>(GetTransientPackage()));
	for (const FGraphIdentityCase& Row : TArray<FGraphIdentityCase>{{TEXT("tree graph pointer/outer"), WrongOuter}, {TEXT("package identity"), WrongPackage}})
	{
		const int32 Init = F.Task->GetInitializeInvocationCount(); const bool Dirty = F.Package->IsDirty();
		Passed &= RequireExactReject(*this, InvokeHelperAdapter(*S.Lease, *Row.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), *S.Transaction), TEXT("BehaviorTreeSemanticRollback.GraphIdentityMismatch"));
		Passed &= TestEqual(*FString::Printf(TEXT("%s performs no InitializeFromAsset"), Row.Name), F.Task->GetInitializeInvocationCount(), Init);
		Passed &= TestEqual(*FString::Printf(TEXT("%s performs no dirty write"), Row.Name), F.Package->IsDirty(), Dirty);
	}
	UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestGraph* DerivedGraph =
		NewObject<UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestGraph>(F.Tree.Get());
	UEdGraph* OriginalGraph = F.Tree->BTGraph.Get();
	F.Tree->BTGraph = DerivedGraph;
	Passed &= RequireExactReject(*this,
		InvokeHelperAdapter(*S.Lease, *DerivedGraph, 2, MakeRuntimeMirrorFootprint(*F.Tree), *S.Transaction),
		TEXT("BehaviorTreeSemanticRollback.GraphIdentityMismatch"));
	F.Tree->BTGraph = OriginalGraph;
	TUniquePtr<IAssetDocumentVerifiedHandleLease> WrongAssetLease = ResolveExistingMutableAssetLease(*this, *F.Blackboard, *S.Transaction, S.Resolver);
	if (!TestNotNull(TEXT("real resolver can issue a mutable non-BehaviorTree asset lease"), WrongAssetLease.Get())) return false;
	Passed &= RequireExactReject(*this, InvokeHelperAdapter(*WrongAssetLease, *F.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), *S.Transaction), TEXT("BehaviorTreeSemanticRollback.BehaviorTreeRequired"));
	// The public resolver signs a normal, admitted tree.  Flipping only public
	// object flags after issuance makes the real payload facade reject before the
	// BehaviorTree-specific row; no private payload is fabricated for this test.
	{
		FScopedObjectFlags Flags(*F.Tree, RF_ArchetypeObject | RF_ClassDefaultObject);
		Flags.Set(RF_ArchetypeObject);
		Passed &= RequireEditorNativeIdentityReject(*this, InvokeHelperAdapter(*S.Lease, *F.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), *S.Transaction));
		F.Tree->ClearFlags(RF_ArchetypeObject);
		Flags.Set(RF_ClassDefaultObject);
		Passed &= RequireEditorNativeIdentityReject(*this, InvokeHelperAdapter(*S.Lease, *F.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), *S.Transaction));
	}
	{
		FScopedObjectRename Rename(*F.Tree, MakeUniqueObjectName(F.Package.Get(), UBehaviorTree::StaticClass(), TEXT("BT_PathStale")));
		if (!TestTrue(TEXT("public rename makes the resolved asset payload path stale"), Rename.WasRenamed())) return false;
		const FTestSemanticResult PathRejection = InvokeHelperAdapter(*S.Lease, *F.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), *S.Transaction);
		Passed &= TestFalse(TEXT("stale public payload has no success result"), PathRejection.bSucceeded);
		Passed &= TestTrue(TEXT("stale public payload preserves exact liveness/path diagnostic"), HasDiagnostic(PathRejection, TEXT("EditorNativeState.AssetPayloadNotAlive")));
	}
	F.Graph->LockUpdates();
	Passed &= RequireExactReject(*this, InvokeHelperAdapter(*S.Lease, *F.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), *S.Transaction), TEXT("BehaviorTreeSemanticRollback.GraphLocked"));
	const int32 InitBeforeUnlockCleanup = F.Task->GetInitializeInvocationCount();
	F.Graph->UnlockUpdates();
	(void)InitBeforeUnlockCleanup; // UnlockUpdates may rebuild; it is cleanup after the rejection observation.
	// CDO/template, exact class, Tree->BTGraph and package checks are frozen on the
	// production path; the table above keeps the callable rows all live objects.
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackRejectsLeaseRevisionLifetimeTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.RejectsLeaseRevisionLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackRejectsLeaseRevisionLifetimeTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	FScenario S; if (!SetUpScenario(*this, S)) return false; FFixture& F = S.Fixture.GetValue(); bool Passed = true;
	const FAssetDocumentOpaqueHandle Handle = S.Lease->GetHandle();
	FAssetDocumentHandleValidationContext Context; Context.RequestId = S.Transaction->GetMutationToken().RequestId; Context.TransactionId = S.Transaction->GetMutationToken().TransactionId;
	Context.RuntimeRevision = S.Transaction->GetMutationToken().RuntimeRevision; Context.ProviderId = TEXT("editor"); Context.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Context.RequiredKind = EAssetDocumentHandleKind::Asset; Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	for (const TCHAR* Name : {TEXT("request"), TEXT("transaction"), TEXT("revision"), TEXT("empty_provider"), TEXT("kind"), TEXT("access"), TEXT("slot"), TEXT("generation")})
	{
		FAssetDocumentOpaqueHandle Bad = Handle;
		if (FCString::Strcmp(Name, TEXT("request")) == 0) Bad.RequestId = FGuid::NewGuid();
		else if (FCString::Strcmp(Name, TEXT("transaction")) == 0) Bad.TransactionId = FGuid::NewGuid();
		else if (FCString::Strcmp(Name, TEXT("revision")) == 0) ++Bad.RuntimeRevision;
		else if (FCString::Strcmp(Name, TEXT("empty_provider")) == 0) Bad.ProviderId.Reset();
		else if (FCString::Strcmp(Name, TEXT("kind")) == 0) Bad.Kind = EAssetDocumentHandleKind::Object;
		else if (FCString::Strcmp(Name, TEXT("access")) == 0) Bad.Access = EAssetDocumentHandleAccess::ReadOnly;
		else if (FCString::Strcmp(Name, TEXT("slot")) == 0) Bad.Slot = 0;
		else Bad.Generation = 0;
		FMutatedLeaseFacade BadLease(Bad, S.Lease->GetPayloadChecked());
		FInstrumentedTransaction Proxy(*S.Transaction); const int32 Init = F.Task->GetInitializeInvocationCount(); const bool Dirty = F.Package->IsDirty();
		const FTestSemanticResult Rejection = InvokeHelperAdapter(BadLease, *F.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), Proxy);
		Passed &= RequireExactReject(*this, Rejection, TEXT("BehaviorTreeSemanticRollback.TransactionBindingMismatch"));
		Passed &= TestEqual(*FString::Printf(TEXT("%s mismatch enlists no finalizer"), Name), Proxy.FinalizerCalls, 0);
		Passed &= TestEqual(*FString::Printf(TEXT("%s mismatch enlists no footprint"), Name), Proxy.FootprintCalls, 0);
		Passed &= TestEqual(*FString::Printf(TEXT("%s mismatch performs no initialization"), Name), F.Task->GetInitializeInvocationCount(), Init);
		Passed &= TestEqual(*FString::Printf(TEXT("%s mismatch performs no dirty write"), Name), F.Package->IsDirty(), Dirty);
	}
	const int32 InitBefore = F.Task->GetInitializeInvocationCount(); const bool DirtyBefore = F.Package->IsDirty();
	FAssetDocumentHandleValidationContext WrongRevision = Context; ++WrongRevision.RuntimeRevision;
	Passed &= TestFalse(TEXT("resolver refuses stale lease before helper invocation"), S.Resolver.AcquireVerifiedLease(Handle, WrongRevision).Lease.Get() != nullptr);
	Passed &= TestEqual(TEXT("lease failures leave fixture uninitialized"), F.Task->GetInitializeInvocationCount(), InitBefore);
	Passed &= TestEqual(TEXT("lease failures leave package clean state unchanged"), F.Package->IsDirty(), DirtyBefore);
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackEnlistBeforeFootprintTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.EnlistBeforeFootprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackEnlistBeforeFootprintTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	bool Passed = true; for (const bool FailFinalizer : {true, false}) { FScenario S; if (!SetUpScenario(*this, S)) return false; FFixture& F = S.Fixture.GetValue();
		FInstrumentedTransaction Proxy(*S.Transaction); Proxy.bFailFinalizer = FailFinalizer; Proxy.bFailFootprint = !FailFinalizer;
		const int32 Init = F.Task->GetInitializeInvocationCount(); const bool Dirty = F.Package->IsDirty();
		const FTestSemanticResult R = InvokeHelperAdapter(*S.Lease, *F.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), Proxy);
		Passed &= TestFalse(TEXT("enlistment injection never reaches a successful UE mutation"), R.bSucceeded);
		Passed &= TestEqual(TEXT("finalizer is attempted first"), Proxy.FinalizerCalls, 1);
		Passed &= TestEqual(TEXT("footprint follows only an accepted finalizer"), Proxy.FootprintCalls, FailFinalizer ? 0 : 1);
		Passed &= TestEqual(TEXT("failed enlistment causes no InitializeFromAsset"), F.Task->GetInitializeInvocationCount(), Init);
		Passed &= TestEqual(TEXT("failed enlistment causes no dirty write"), F.Package->IsDirty(), Dirty); }
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackPostRollbackOrderingTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.PostRollbackOrdering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackPostRollbackOrderingTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	FScenario S; int32 InitObservedByAuthoredAction = -1; int32 InvalidationsObservedByAuthoredAction = -1;
	if (!SetUpScenario(*this, S, {}, [&](FFixture& F) { InitObservedByAuthoredAction = F.Task->GetInitializeInvocationCount(); InvalidationsObservedByAuthoredAction = S.Resolver.GetInvalidationCount(); })) return false;
	FFixture& F = S.Fixture.GetValue();
	const int32 InitializeBefore = F.Task->GetInitializeInvocationCount();
	bool Passed = RequireSuccess(*this, InvokeHelperAdapter(*S.Lease, *F.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), *S.Transaction), TEXT("real helper arms a Node A finalizer"));
	S.Transaction->Rollback(TEXT("Test.PostRollbackOrdering"));
	Passed &= TestEqual(TEXT("ordinary authored actions retain reverse first-footprint order"), S.RollbackOrder, TArray<FString>({TEXT("authored.b"), TEXT("authored.a")}));
	Passed &= TestEqual(TEXT("ordinary rollback observes no finalizer rebuild yet"), InitObservedByAuthoredAction, InitializeBefore + 1);
	Passed &= TestEqual(TEXT("ordinary rollback observes lease invalidation before finalizer"), InvalidationsObservedByAuthoredAction, 1);
	Passed &= TestEqual(TEXT("lease invalidation occurs before no-retry finalizer completion"), S.Resolver.GetInvalidationCount(), 1);
	Passed &= TestEqual(TEXT("finalizer rebuild is the second fixture initialization"), F.Task->GetInitializeInvocationCount(), InitializeBefore + 2);
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackFinalizerFailureNoRetryTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.FinalizerFailureNoRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackFinalizerFailureNoRetryTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	FScenario S; if (!SetUpScenario(*this, S)) return false; FFixture& F = S.Fixture.GetValue();
	const int32 InitializeBefore = F.Task->GetInitializeInvocationCount();
	const bool DirtyBefore = F.Package->IsDirty();
	bool Passed = RequireSuccess(*this, InvokeHelperAdapter(*S.Lease, *F.Graph, 2, MakeRuntimeMirrorFootprint(*F.Tree), *S.Transaction), TEXT("forward call reaches the real finalizer path"));
#if ASSETDOCUMENT_AI_BT_SEMANTIC_ROLLBACK_PRIVATE_TEST_HOOK_PRESENT
	UE::AssetDocumentAI::BehaviorTreeSemanticRuntimeMirrorRollback::Private::SetForceSemanticReadbackFailureForTest(true);
	S.Transaction->Rollback(TEXT("Test.FinalizerFailureNoRetry"));
	UE::AssetDocumentAI::BehaviorTreeSemanticRuntimeMirrorRollback::Private::SetForceSemanticReadbackFailureForTest(false);
	Passed &= TestEqual(TEXT("post-call readback failure poisons once"), S.Transaction->GetState(), EAssetDocumentAtomicState::Poisoned);
	Passed &= TestEqual(TEXT("finalizer never retries UpdateAsset"), F.Task->GetInitializeInvocationCount(), InitializeBefore + 2);
	Passed &= TestEqual(TEXT("dirty restoration remains final even when readback fails"), F.Package->IsDirty(), DirtyBefore);
#else
	Passed &= TestTrue(TEXT("private deterministic post-UpdateAsset readback hook is required"), false);
#endif
	return Passed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackNoProviderOrPrivateFallbackTest,
	"UEAssetDoc.AI.BehaviorTreeSemanticRuntimeRollback.NoProviderOrPrivateFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeSemanticRuntimeRollbackNoProviderOrPrivateFallbackTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTests;
	return RunStaticFallbackGuard(*this);
}

#endif // WITH_DEV_AUTOMATION_TESTS
