// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentRollbackAction.h"

#include "AIGraphTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentAICacheRefreshTests
{
constexpr const TCHAR* SelectorResolveId = TEXT("ai.behavior_tree.selector.resolve.v1");
constexpr const TCHAR* RuntimeMirrorId = TEXT("ai.behavior_tree.runtime_mirror.rebuild.v1");
constexpr const TCHAR* BlackboardSetId = TEXT("ai.behavior_tree.blackboard.set.v1");
constexpr const TCHAR* GraphCreateId = TEXT("ai.behavior_tree.graph.create.v1");

FAssetDocumentContractValidationResult Valid()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentRuntimeValueDto StringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto IntegerValue(const int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto NullValue()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

bool HasDiagnostic(TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics, const TCHAR* Code)
{
	for (const FAssetDocumentRuntimeDiagnostic& Item : Diagnostics)
	{
		if (Item.Code == Code) return true;
	}
	return false;
}

bool SameHandle(const FAssetDocumentOpaqueHandle& Left, const FAssetDocumentOpaqueHandle& Right)
{
	return Left.RequestId == Right.RequestId && Left.TransactionId == Right.TransactionId
		&& Left.RuntimeRevision == Right.RuntimeRevision && Left.ProviderId == Right.ProviderId
		&& Left.Slot == Right.Slot && Left.Generation == Right.Generation && Left.Kind == Right.Kind
		&& Left.Access == Right.Access;
}

class FCacheRefreshTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	FCacheRefreshTransaction()
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
		return Valid();
	}
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer) override
	{
		Order.Add(TEXT("finalizer"));
		Finalizers.Add(MoveTemp(Finalizer));
		return Valid();
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
		return Valid();
	}
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Valid(); }
	virtual FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return Valid(); }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView Code) override
	{
		FAssetDocumentContractValidationResult Result = Valid();
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

// Stores the payload issued by the real Editor primitive.  Aliasing is test
// setup only: the public request context below is the observed handoff seam.
class FCacheRefreshResolver final : public IAssetDocumentHandleResolver
{
public:
	struct FEntry
	{
		EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Object;
		EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::ReadOnly;
		FString ProviderId;
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
		IAssetDocumentNativeHandlePayload* Payload;
	};

	explicit FCacheRefreshResolver(const FAssetDocumentMutationToken& InToken) : Token(InToken) {}
	virtual FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access, TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		FAssetDocumentIssuedHandleResult Result;
		if (!Payload || !Payload->IsAlive()) return Result;
		FEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Kind = Kind;
		Entry.Access = Access;
		Entry.ProviderId = Kind == EAssetDocumentHandleKind::Asset
			|| Kind == EAssetDocumentHandleKind::Package
			? TEXT("editor") : TEXT("ai");
		Entry.Payload = MakeShareable(Payload.Release());
		Result.Handle = MakeHandle(Entry, Entries.Num());
		return Result;
	}
	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle, const FAssetDocumentHandleValidationContext& Context) override
	{
		FAssetDocumentVerifiedLeaseResult Result;
		const FAssetDocumentContractValidationResult Metadata = ValidateAssetDocumentHandleMetadata(Handle, Context);
		if (!Metadata.bValid) { Result.Diagnostics = Metadata.Diagnostics; return Result; }
		const int32 Index = static_cast<int32>(Handle.Slot) - 1;
		if (!Entries.IsValidIndex(Index)) return Result;
		FEntry& Entry = Entries[Index];
		if (!Entry.bInvalidated && Entry.Kind == Handle.Kind && Entry.Access == Handle.Access
			&& Entry.ProviderId == Handle.ProviderId && Entry.Payload.IsValid() && Entry.Payload->IsAlive())
		{
			Result.Lease = MakeUnique<FLease>(Handle, *Entry.Payload);
		}
		return Result;
	}
	virtual FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation& Event) override
	{
		FAssetDocumentHandleInvalidationResult Result;
		if (Event.RequestId != Token.RequestId || Event.TransactionId != Token.TransactionId
			|| Event.RuntimeRevision != Token.RuntimeRevision) return Result;
		for (FEntry& Entry : Entries)
		{
			if (!Entry.bInvalidated && (Event.ProviderId.IsEmpty() || Entry.ProviderId == Event.ProviderId))
			{
				Entry.bInvalidated = true;
				++Result.InvalidatedHandleCount;
			}
		}
		if (Result.InvalidatedHandleCount > 0) ++InvalidationCount;
		return Result;
	}
	TOptional<FAssetDocumentOpaqueHandle> AliasForAI(const FAssetDocumentOpaqueHandle& Source)
	{
		const int32 Index = static_cast<int32>(Source.Slot) - 1;
		if (!Entries.IsValidIndex(Index) || Entries[Index].ProviderId != TEXT("editor")) return {};
		FEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry = Entries[Index];
		Entry.ProviderId = TEXT("ai");
		return MakeHandle(Entry, Entries.Num());
	}
	int32 GetEntryCount() const { return Entries.Num(); }
	int32 GetInvalidationCount() const { return InvalidationCount; }

private:
	FAssetDocumentOpaqueHandle MakeHandle(const FEntry& Entry, const int32 Slot) const
	{
		FAssetDocumentOpaqueHandle Handle;
		Handle.RequestId = Token.RequestId;
		Handle.TransactionId = Token.TransactionId;
		Handle.RuntimeRevision = Token.RuntimeRevision;
		Handle.ProviderId = Entry.ProviderId;
		Handle.Slot = Slot;
		Handle.Generation = 1;
		Handle.Kind = Entry.Kind;
		Handle.Access = Entry.Access;
		return Handle;
	}
	FAssetDocumentMutationToken Token;
	TArray<FEntry> Entries;
	int32 InvalidationCount = 0;
};

class FCacheRefreshHandoffContext final : public IAssetDocumentNativeRequestContext
{
public:
	FCacheRefreshHandoffContext(FAssetDocumentOpaqueHandle InSource, FAssetDocumentOpaqueHandle InTarget,
		const FAssetDocumentMutationToken& Token) : Source(MoveTemp(InSource)), Target(MoveTemp(InTarget))
	{
		Metadata.RequestId = Token.RequestId;
		Metadata.PinnedRuntimeRevision = Token.RuntimeRevision;
		Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	}
	virtual const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
	virtual FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation&) override { return {}; }
	virtual FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return {}; }
	virtual FAssetDocumentIssuedHandleResult Handoff(const FAssetDocumentOpaqueHandle& Requested,
		FStringView Provider, EAssetDocumentHandleAccess Access) override
	{
		FAssetDocumentIssuedHandleResult Result;
		if (SameHandle(Requested, Source) && Provider == TEXTVIEW("ai") && Access == Target.Access) Result.Handle = Target;
		return Result;
	}
	private:
	FAssetDocumentOpaqueHandle Source;
	FAssetDocumentOpaqueHandle Target;
	FAssetDocumentRequestContextMetadata Metadata;
};

FAssetDocumentPrimitiveInvocation Invocation(const TCHAR* Id, EAssetDocumentRuntimeRequestPhase Phase,
	const FAssetDocumentMutationToken& Token, FAssetDocumentRuntimeValueDto Value,
	TArray<FAssetDocumentPrimitiveHandleArgument> Handles = {})
{
	FAssetDocumentPrimitiveInvocation Result;
	Result.PrimitiveId = Id;
	Result.Version = 1;
	Result.RequestId = Token.RequestId;
	Result.TransactionId = Token.TransactionId;
	Result.RuntimeRevision = Token.RuntimeRevision;
	Result.Phase = Phase;
	Result.ValueArguments = MoveTemp(Value);
	Result.HandleArguments = MoveTemp(Handles);
	return Result;
}

TOptional<FAssetDocumentOpaqueHandle> OutcomeHandle(const FAssetDocumentPrimitiveOutcome& Outcome, FStringView Name)
{
	for (const FAssetDocumentPrimitiveHandleResult& Result : Outcome.HandleResults)
	{
		if (Result.Name == Name && Result.Handles.Num() == 1) return Result.Handles[0];
	}
	return {};
}

FAssetDocumentHandleValidationContext Context(const FAssetDocumentMutationToken& Token, FStringView Provider,
	EAssetDocumentHandleKind Kind)
{
	FAssetDocumentHandleValidationContext Result;
	Result.RequestId = Token.RequestId;
	Result.TransactionId = Token.TransactionId;
	Result.RuntimeRevision = Token.RuntimeRevision;
	Result.ProviderId = Provider;
	Result.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Result.RequiredKind = Kind;
	Result.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Result;
}

struct FFixture
{
	struct FRegisteredAsset
	{
		explicit FRegisteredAsset(UObject& InAsset) : Asset(&InAsset)
		{
			FAssetRegistryModule::AssetCreated(&InAsset);
		}
		~FRegisteredAsset()
		{
			if (UObject* Existing = Asset.Get())
			{
				FAssetRegistryModule::AssetDeleted(Existing);
			}
		}
		TWeakObjectPtr<UObject> Asset;
	};

	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UBehaviorTree> Tree;
	TStrongObjectPtr<UBehaviorTreeGraph> Graph;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Root> Root;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Composite> CompositeGraph;
	TStrongObjectPtr<UBTComposite_Sequence> Composite;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Task> TaskGraph;
	TStrongObjectPtr<UBTTaskNode> Task;
	TStrongObjectPtr<UBlackboardData> BlackboardA;
	TStrongObjectPtr<UBlackboardData> BlackboardB;
	TStrongObjectPtr<UBehaviorTree> ForeignTree;
	TUniquePtr<FRegisteredAsset> Registration;
};

UEdGraphPin* Pin(const UEdGraphNode& Node, const EEdGraphPinDirection Direction)
{
	for (UEdGraphPin* Candidate : Node.Pins)
	{
		if (Candidate != nullptr && Candidate->Direction == Direction) return Candidate;
	}
	return nullptr;
}

TOptional<FFixture> MakeFixture(FAutomationTestBase& Test)
{
	FFixture Fixture;
	const FString AssetName = FString::Printf(TEXT("BT_CacheRefresh_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Fixture.Package.Reset(CreatePackage(*FString::Printf(
		TEXT("/Game/AssetDocumentTests/P_AI/%s"), *AssetName)));
	Fixture.Tree.Reset(NewObject<UBehaviorTree>(Fixture.Package.Get(), *AssetName,
		RF_Public | RF_Standalone | RF_Transactional));
	Fixture.ForeignTree.Reset(NewObject<UBehaviorTree>(Fixture.Package.Get(), TEXT("BT_Foreign"),
		RF_Public | RF_Standalone | RF_Transactional));
	Fixture.Graph.Reset(Cast<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(Fixture.Tree.Get(),
		TEXT("BehaviorTreeGraph"), UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass())));
	Fixture.BlackboardA.Reset(NewObject<UBlackboardData>(Fixture.Package.Get(), TEXT("BB_A"), RF_Public | RF_Standalone));
	Fixture.BlackboardB.Reset(NewObject<UBlackboardData>(Fixture.Package.Get(), TEXT("BB_B"), RF_Public | RF_Standalone));
	if (!Test.TestNotNull(TEXT("live tree"), Fixture.Tree.Get())
		|| !Test.TestNotNull(TEXT("foreign path tree"), Fixture.ForeignTree.Get())
		|| !Test.TestNotNull(TEXT("installed exact graph"), Fixture.Graph.Get())
		|| !Test.TestNotNull(TEXT("blackboard A"), Fixture.BlackboardA.Get()) || !Test.TestNotNull(TEXT("blackboard B"), Fixture.BlackboardB.Get())) return {};
	Fixture.Registration = MakeUnique<FFixture::FRegisteredAsset>(*Fixture.Tree);
	UBlackboardKeyType_Object* ParentKey =
		Fixture.BlackboardA->UpdatePersistentKey<UBlackboardKeyType_Object>(TEXT("ParentKey"));
	UBlackboardKeyType_Object* ReplacementParentKey =
		Fixture.BlackboardB->UpdatePersistentKey<UBlackboardKeyType_Object>(TEXT("ParentKey"));
	Fixture.BlackboardB->UpdatePersistentKey<UBlackboardKeyType_Object>(TEXT("OtherKey"));
	if (!Test.TestNotNull(TEXT("Blackboard A object key type"), ParentKey)
		|| !Test.TestNotNull(TEXT("Blackboard B object key type"),
			ReplacementParentKey)) return {};
	ParentKey->BaseClass = AActor::StaticClass();
	ReplacementParentKey->BaseClass = AActor::StaticClass();
	Fixture.Tree->BTGraph = Fixture.Graph.Get();
	Fixture.Tree->BlackboardAsset = Fixture.BlackboardA.Get();
	Fixture.Graph->GetSchema()->CreateDefaultNodesForGraph(*Fixture.Graph);
	Fixture.Graph->OnCreated();
	Fixture.Graph->Initialize();
	Fixture.Root.Reset(Fixture.Graph->Nodes.Num() > 0 ? Cast<UBehaviorTreeGraphNode_Root>(Fixture.Graph->Nodes[0]) : nullptr);
	if (!Test.TestNotNull(TEXT("unique stock root"), Fixture.Root.Get())) return {};
	Fixture.Root->BlackboardAsset = Fixture.BlackboardA.Get();
	Fixture.Root->UpdateBlackboard();
	UClass* TaskClass = LoadClass<UBTTaskNode>(nullptr, TEXT("/Script/AssetDocumentTests.AssetDocumentSelectorTaskTestNode"));
	if (!Test.TestNotNull(TEXT("dynamic test task class"), TaskClass)) return {};
	FAISchemaAction_NewNode CompositeAction;
	CompositeAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Composite>();
	CompositeAction.NodeTemplate->NodeInstance =
		NewObject<UBTComposite_Sequence>(CompositeAction.NodeTemplate);
	UEdGraphPin* RootOut = Pin(*Fixture.Root, EGPD_Output);
	Fixture.CompositeGraph.Reset(Cast<UBehaviorTreeGraphNode_Composite>(
		CompositeAction.PerformAction(Fixture.Graph.Get(), RootOut,
			FVector2f(160.0f, 0.0f), false)));
	Fixture.Composite.Reset(Fixture.CompositeGraph.IsValid()
		? Cast<UBTComposite_Sequence>(Fixture.CompositeGraph->NodeInstance) : nullptr);
	FAISchemaAction_NewNode TaskAction;
	TaskAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Task>();
	TaskAction.NodeTemplate->NodeInstance = NewObject<UBTTaskNode>(TaskAction.NodeTemplate, TaskClass);
	UEdGraphPin* CompositeOut = Fixture.CompositeGraph.IsValid()
		? Pin(*Fixture.CompositeGraph, EGPD_Output) : nullptr;
	Fixture.TaskGraph.Reset(Cast<UBehaviorTreeGraphNode_Task>(
		TaskAction.PerformAction(Fixture.Graph.Get(), CompositeOut,
			FVector2f(320.0f, 0.0f), false)));
	Fixture.Task.Reset(Fixture.TaskGraph.IsValid()
		? Cast<UBTTaskNode>(Fixture.TaskGraph->NodeInstance) : nullptr);
	if (!Test.TestNotNull(TEXT("stock action creates the composite graph node"),
		Fixture.CompositeGraph.Get())
		|| !Test.TestNotNull(TEXT("stock action creates the Sequence instance"),
			Fixture.Composite.Get())
		|| !Test.TestNotNull(TEXT("stock action creates the task graph node"),
			Fixture.TaskGraph.Get())
		|| !Test.TestNotNull(TEXT("stock action creates the project task instance"),
			Fixture.Task.Get())) return {};
	FStructProperty* DirectSelector = FindFProperty<FStructProperty>(TaskClass, TEXT("DirectSelector"));
	FBlackboardKeySelector* Selector = DirectSelector != nullptr
		? DirectSelector->ContainerPtrToValuePtr<FBlackboardKeySelector>(Fixture.Task.Get()) : nullptr;
	if (!Test.TestNotNull(TEXT("top-level DirectSelector exists"), Selector)) return {};
	if (!Test.TestTrue(TEXT("stock actions connect root, composite and task"),
		RootOut != nullptr && RootOut->LinkedTo.Num() == 1
		&& RootOut->LinkedTo[0]->GetOwningNode() == Fixture.CompositeGraph.Get()
		&& CompositeOut != nullptr && CompositeOut->LinkedTo.Num() == 1
		&& CompositeOut->LinkedTo[0]->GetOwningNode() == Fixture.TaskGraph.Get())) return {};
	Fixture.Tree->BlackboardAsset = nullptr;
	Fixture.Root->UpdateBlackboard();
	Selector->SelectedKeyName = TEXT("ParentKey");
	Selector->InvalidateResolvedKey();
	if (!Test.TestTrue(TEXT("native setup initializes the exact live task closure"),
		Fixture.Tree->BlackboardAsset == Fixture.BlackboardA.Get()
		&& Fixture.Task->GetTreeAsset() == Fixture.Tree.Get()
		&& Fixture.Task->GetBlackboardAsset() == Fixture.BlackboardA.Get())) return {};
	return Fixture;
}

FBlackboardKeySelector* GetDirectSelector(UBTTaskNode& Task)
{
	const FStructProperty* Property = FindFProperty<FStructProperty>(Task.GetClass(), TEXT("DirectSelector"));
	return Property != nullptr ? Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(&Task) : nullptr;
}

TOptional<FAssetDocumentOpaqueHandle> ResolveEditorAsset(FAutomationTestBase& Test, UBehaviorTree& Tree,
	FCacheRefreshTransaction& Transaction, FCacheRefreshResolver& Resolver)
{
	const FAssetDocumentPrimitiveOutcome Outcome = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(
		Invocation(TEXT("editor.asset.resolve_existing.v1"), EAssetDocumentRuntimeRequestPhase::Apply,
			Transaction.GetMutationToken(), ObjectValue({TEXT("expected_class"), TEXT("path")},
				{StringValue(Tree.GetClass()->GetPathName()), StringValue(Tree.GetPathName())})), Resolver, nullptr);
	Test.TestEqual(TEXT("real editor.asset.resolve_existing.v1 succeeds"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	return OutcomeHandle(Outcome, TEXTVIEW("asset_mutable"));
}
} // namespace AssetDocumentAICacheRefreshTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAICacheRefreshTest,
	"UEAssetDoc.AIProvider.CacheRefresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAICacheRefreshTest::RunTest(const FString&)
{
	using namespace AssetDocumentAICacheRefreshTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentAIPrimitiveProvider();
	TOptional<FFixture> Fixture = MakeFixture(*this);
	if (!Fixture.IsSet()) return false;
	FFixture& F = Fixture.GetValue();
	FCacheRefreshTransaction Transaction;
	FCacheRefreshResolver Resolver(Transaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> EditorHandle = ResolveEditorAsset(*this, *F.Tree, Transaction, Resolver);
	if (!TestTrue(TEXT("Editor issued a mutable Asset handle"), EditorHandle.IsSet())) return false;
	const FBlackboardKeySelector* SelectorBefore = GetDirectSelector(*F.Task);
	if (!TestNotNull(TEXT("selector preimage is observable"), SelectorBefore)) return false;
	const FName SelectorNameBefore = SelectorBefore->SelectedKeyName;
	const FBlackboard::FKey SelectorIdBefore = SelectorBefore->GetSelectedKeyID();
	const UClass* SelectorTypeBefore = SelectorBefore->SelectedKeyType.Get();
	const bool bSelectorPackageDirtyBefore = F.Package->IsDirty();
	const FAssetDocumentPrimitiveOutcome DirectEditor = Provider->Invoke(Invocation(SelectorResolveId,
		EAssetDocumentRuntimeRequestPhase::Repair, Transaction.GetMutationToken(), ObjectValue(
			{TEXT("behavior_tree_path"), TEXT("blackboard_path"), TEXT("property_path"), TEXT("selected_key_name"), TEXT("stable_node_id")},
			{StringValue(F.Tree->GetPathName()), StringValue(F.BlackboardA->GetPathName()), StringValue(TEXT("DirectSelector")),
				StringValue(TEXT("ParentKey")), StringValue(F.TaskGraph->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower))}),
		{{TEXT("asset"), EditorHandle.GetValue()}}), Resolver, &Transaction);
	bool Passed = TestEqual(TEXT("direct Editor handle fails at the P-AI provider boundary"),
		DirectEditor.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Passed &= TestTrue(TEXT("direct Editor handle preserves ProviderMismatch and zero enlist"),
		HasDiagnostic(DirectEditor.Diagnostics, TEXT("HandleMetadata.ProviderMismatch"))
		&& DirectEditor.ChangedSet.IsEmpty() && Transaction.Order.IsEmpty());
	const FAssetDocumentVerifiedLeaseResult Foreign = Resolver.AcquireVerifiedLease(EditorHandle.GetValue(),
		Context(Transaction.GetMutationToken(), TEXTVIEW("ai"), EAssetDocumentHandleKind::Asset));
	Passed &= TestNull(TEXT("direct Editor handle cannot enlist as AI"), Foreign.Lease.Get());
	Passed &= TestTrue(TEXT("foreign direct handle is ProviderMismatch with zero enlist"),
		HasDiagnostic(Foreign.Diagnostics, TEXT("HandleMetadata.ProviderMismatch"))
		&& Transaction.Order.IsEmpty());
	const TOptional<FAssetDocumentOpaqueHandle> Alias = Resolver.AliasForAI(EditorHandle.GetValue());
	if (!TestTrue(TEXT("test setup creates AI alias over real Editor payload"), Alias.IsSet())) return false;
	FCacheRefreshHandoffContext Handoff(EditorHandle.GetValue(), Alias.GetValue(), Transaction.GetMutationToken());
	const FAssetDocumentIssuedHandleResult HandedOff = Handoff.Handoff(EditorHandle.GetValue(), TEXTVIEW("ai"), EAssetDocumentHandleAccess::Mutable);
	if (!TestTrue(TEXT("public handoff returns AI alias"), HandedOff.Handle.IsSet())) return false;
	const FAssetDocumentOpaqueHandle AIHandle = HandedOff.Handle.GetValue();
	Passed &= TestEqual(TEXT("handoff provider is ai"), AIHandle.ProviderId, FString(TEXT("ai")));

	const FAssetDocumentPrimitiveOutcome Selector = Provider->Invoke(Invocation(SelectorResolveId,
		EAssetDocumentRuntimeRequestPhase::Repair, Transaction.GetMutationToken(), ObjectValue(
			{TEXT("behavior_tree_path"), TEXT("blackboard_path"), TEXT("property_path"), TEXT("selected_key_name"), TEXT("stable_node_id")},
			{StringValue(F.Tree->GetPathName()), StringValue(F.BlackboardA->GetPathName()), StringValue(TEXT("DirectSelector")),
				StringValue(TEXT("ParentKey")), StringValue(F.TaskGraph->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower))}),
		{{TEXT("asset"), AIHandle}}), Resolver, &Transaction);
	if (Selector.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Selector.Diagnostics)
		{
			AddInfo(FString::Printf(TEXT("valid selector diagnostic=%s message=%s"),
				*Diagnostic.Code, *Diagnostic.Message));
		}
	}
	Passed &= TestEqual(TEXT("selector resolve succeeds"), Selector.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Passed &= TestTrue(TEXT("selector action then footprint"), Transaction.Order.Num() >= 2
		&& Transaction.Order[0] == TEXT("action") && Transaction.Order[1] == TEXT("footprint"));
	Passed &= TestEqual(TEXT("selector has one ChangedSet"), Selector.ChangedSet.Num(), 1);
	const FBlackboardKeySelector* ResolvedSelector = GetDirectSelector(*F.Task);
	const FBlackboard::FKey SelectorId = ResolvedSelector != nullptr ? ResolvedSelector->GetSelectedKeyID()
		: FBlackboard::InvalidKey;
	Passed &= TestNotEqual(TEXT("selector has resolved key id"), SelectorId, FBlackboard::InvalidKey);
	Passed &= TestTrue(TEXT("selector rollback reverses real typed preimage"), Transaction.Rollback(TEXT("Test.SelectorRollback")).bValid);
	const FBlackboardKeySelector* RestoredSelector = GetDirectSelector(*F.Task);
	if (!TestNotNull(TEXT("DirectSelector remains present after rollback"), RestoredSelector)) return false;
	Passed &= TestEqual(TEXT("rollback restores selector name"), RestoredSelector->SelectedKeyName, SelectorNameBefore);
	Passed &= TestEqual(TEXT("rollback restores selector cache id"), RestoredSelector->GetSelectedKeyID(), SelectorIdBefore);
	Passed &= TestEqual(TEXT("rollback restores selector cache type"),
		static_cast<const void*>(RestoredSelector->SelectedKeyType.Get()), static_cast<const void*>(SelectorTypeBefore));
	Passed &= TestEqual(TEXT("rollback restores package dirty preimage"),
		F.Package->IsDirty(), bSelectorPackageDirtyBefore);
	// The missing-key terminal is rejected before row generation, typed capture,
	// footprint admission, or authored/cache mutation.
	FCacheRefreshTransaction MissingTransaction;
	FCacheRefreshResolver MissingResolver(MissingTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> MissingEditor = ResolveEditorAsset(*this, *F.Tree, MissingTransaction, MissingResolver);
	const TOptional<FAssetDocumentOpaqueHandle> MissingAI = MissingEditor.IsSet()
		? MissingResolver.AliasForAI(MissingEditor.GetValue()) : TOptional<FAssetDocumentOpaqueHandle>();
	if (!MissingAI.IsSet()) return false;
	UBTTaskNode* MissingGraphTask = Cast<UBTTaskNode>(F.TaskGraph->NodeInstance);
	FBlackboardKeySelector* MissingSelector = MissingGraphTask != nullptr
		? GetDirectSelector(*MissingGraphTask) : nullptr;
	Passed &= TestEqual(TEXT("missing selector uses the exact graph-node instance"),
		static_cast<const void*>(MissingGraphTask), static_cast<const void*>(F.Task.Get()));
	if (!TestNotNull(TEXT("missing selector preflight is observable"), MissingSelector)) return false;
	MissingSelector->SelectedKeyName = TEXT("MissingKey");
	Passed &= TestEqual(TEXT("missing selector setup reaches exact authored storage"),
		MissingSelector->SelectedKeyName, FName(TEXT("MissingKey")));
	const FName MissingNameBefore = MissingSelector->SelectedKeyName;
	const FBlackboard::FKey MissingIdBefore = MissingSelector->GetSelectedKeyID();
	const UClass* MissingTypeBefore = MissingSelector->SelectedKeyType.Get();
	const bool bMissingPackageDirtyBefore = F.Package->IsDirty();
	const FAssetDocumentPrimitiveOutcome Missing = Provider->Invoke(Invocation(SelectorResolveId,
		EAssetDocumentRuntimeRequestPhase::Repair, MissingTransaction.GetMutationToken(), ObjectValue(
			{TEXT("behavior_tree_path"), TEXT("blackboard_path"), TEXT("property_path"), TEXT("selected_key_name"), TEXT("stable_node_id")},
			{StringValue(F.Tree->GetPathName()), StringValue(F.BlackboardA->GetPathName()), StringValue(TEXT("DirectSelector")),
				StringValue(TEXT("MissingKey")), StringValue(F.TaskGraph->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower))}),
		{{TEXT("asset"), MissingAI.GetValue()}}), MissingResolver, &MissingTransaction);
	Passed &= TestEqual(TEXT("missing selector key fails"), Missing.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	const FBlackboardKeySelector* MissingAfter = GetDirectSelector(*MissingGraphTask);
	Passed &= TestNotNull(TEXT("missing selector remains observable after failure"), MissingAfter);
	if (MissingAfter != nullptr)
	{
		Passed &= TestEqual(TEXT("missing selector failure preserves the authored key"),
			MissingAfter->SelectedKeyName, MissingNameBefore);
		Passed &= TestEqual(TEXT("missing selector failure preserves the cache id"),
			MissingAfter->GetSelectedKeyID(), MissingIdBefore);
		Passed &= TestEqual(TEXT("missing selector failure preserves the cache type"),
			static_cast<const void*>(MissingAfter->SelectedKeyType.Get()),
			static_cast<const void*>(MissingTypeBefore));
	}
	Passed &= TestEqual(TEXT("missing selector failure preserves package dirty state"),
		F.Package->IsDirty(), bMissingPackageDirtyBefore);
	Passed &= TestTrue(TEXT("missing selector fails before row generation and mutation coverage"),
		HasDiagnostic(Missing.Diagnostics, TEXT("AIProvider.BehaviorTree.SelectorResolveFailed"))
		&& Missing.ChangedSet.IsEmpty() && MissingTransaction.Order.IsEmpty());
	Passed &= TestTrue(TEXT("missing selector rollback succeeds"), MissingTransaction.Rollback(TEXT("Test.MissingSelector")).bValid);
	GetDirectSelector(*F.Task)->SelectedKeyName = TEXT("ParentKey");

	// A fresh transaction proves graph provenance, runtime invalidation/finalizer, and a malformed DTO's no-write boundary.
	FCacheRefreshTransaction RuntimeTransaction;
	FCacheRefreshResolver RuntimeResolver(RuntimeTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> RuntimeEditor = ResolveEditorAsset(*this, *F.Tree, RuntimeTransaction, RuntimeResolver);
	if (!RuntimeEditor.IsSet()) return false;
	const TOptional<FAssetDocumentOpaqueHandle> RuntimeAI = RuntimeResolver.AliasForAI(RuntimeEditor.GetValue());
	if (!RuntimeAI.IsSet()) return false;
	const FAssetDocumentPrimitiveOutcome GraphCreate = Provider->Invoke(Invocation(GraphCreateId,
		EAssetDocumentRuntimeRequestPhase::Apply, RuntimeTransaction.GetMutationToken(), ObjectValue(
			{TEXT("behavior_tree_path"), TEXT("graph_name")}, {StringValue(F.Tree->GetPathName()), StringValue(TEXT("CacheRefreshProvenance"))})),
		RuntimeResolver, &RuntimeTransaction);
	const TOptional<FAssetDocumentOpaqueHandle> GraphHandle = OutcomeHandle(GraphCreate, TEXTVIEW("graph"));
	if (!TestEqual(TEXT("provider graph.create supplies provenance"), GraphCreate.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		|| !TestTrue(TEXT("provider graph.create issues HG"), GraphHandle.IsSet())) return false;
	const int32 EntriesBeforeBadDto = RuntimeResolver.GetEntryCount();
	const int32 EnlistsBeforeBadDto = RuntimeTransaction.Order.Num();
	const int32 InvalidationsBeforeBadDto = RuntimeResolver.GetInvalidationCount();
	const FAssetDocumentPrimitiveOutcome BadRuntime = Provider->Invoke(Invocation(RuntimeMirrorId,
		EAssetDocumentRuntimeRequestPhase::Repair, RuntimeTransaction.GetMutationToken(), ObjectValue(
			{TEXT("behavior_tree_path"), TEXT("update_flags")}, {StringValue(F.Tree->GetPathName()), IntegerValue(1)}),
		{{TEXT("asset"), RuntimeAI.GetValue()}, {TEXT("graph"), GraphHandle.GetValue()}}), RuntimeResolver, &RuntimeTransaction);
	Passed &= TestEqual(TEXT("mismatched runtime DTO fails"), BadRuntime.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Passed &= TestTrue(TEXT("mismatched runtime DTO has zero write/enlist/invalidation"), BadRuntime.ChangedSet.IsEmpty()
		&& RuntimeResolver.GetEntryCount() == EntriesBeforeBadDto
		&& RuntimeResolver.GetInvalidationCount() == InvalidationsBeforeBadDto
		&& RuntimeTransaction.Order.Num() == EnlistsBeforeBadDto);
	Passed &= TestNotNull(TEXT("mismatched DTO keeps HG alive"), RuntimeResolver.AcquireVerifiedLease(GraphHandle.GetValue(),
		Context(RuntimeTransaction.GetMutationToken(), TEXTVIEW("ai"), EAssetDocumentHandleKind::Graph)).Lease.Get());
	const FAssetDocumentPrimitiveOutcome PathMismatch = Provider->Invoke(Invocation(RuntimeMirrorId,
		EAssetDocumentRuntimeRequestPhase::Repair, RuntimeTransaction.GetMutationToken(), ObjectValue(
			{TEXT("behavior_tree_path"), TEXT("update_flags")},
			{StringValue(F.ForeignTree->GetPathName()), IntegerValue(UBehaviorTreeGraph::KeepRebuildCounter)}),
		{{TEXT("asset"), RuntimeAI.GetValue()}, {TEXT("graph"), GraphHandle.GetValue()}}),
		RuntimeResolver, &RuntimeTransaction);
	Passed &= TestEqual(TEXT("HA/DTO/HG live path mismatch fails closed"),
		PathMismatch.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Passed &= TestTrue(TEXT("live path mismatch has zero enlist/write/invalidation"),
		PathMismatch.ChangedSet.IsEmpty()
		&& RuntimeTransaction.Order.Num() == EnlistsBeforeBadDto
		&& RuntimeResolver.GetInvalidationCount() == InvalidationsBeforeBadDto);
	Passed &= TestNotNull(TEXT("live path mismatch preserves owning HG"),
		RuntimeResolver.AcquireVerifiedLease(GraphHandle.GetValue(),
			Context(RuntimeTransaction.GetMutationToken(), TEXTVIEW("ai"),
				EAssetDocumentHandleKind::Graph)).Lease.Get());
	F.Graph->LockUpdates();
	const FAssetDocumentPrimitiveOutcome Locked = Provider->Invoke(Invocation(RuntimeMirrorId,
		EAssetDocumentRuntimeRequestPhase::Repair, RuntimeTransaction.GetMutationToken(), ObjectValue(
			{TEXT("behavior_tree_path"), TEXT("update_flags")}, {StringValue(F.Tree->GetPathName()), IntegerValue(UBehaviorTreeGraph::KeepRebuildCounter)}),
		{{TEXT("asset"), RuntimeAI.GetValue()}, {TEXT("graph"), GraphHandle.GetValue()}}), RuntimeResolver, &RuntimeTransaction);
	F.Graph->UnlockUpdates();
	Passed &= TestEqual(TEXT("locked graph is negative and test unlocks it"), Locked.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Passed &= TestTrue(TEXT("locked graph has zero enlist/write/invalidation and preserves HG"),
		Locked.ChangedSet.IsEmpty() && RuntimeTransaction.Order.Num() == EnlistsBeforeBadDto
		&& RuntimeResolver.GetInvalidationCount() == InvalidationsBeforeBadDto
		&& RuntimeResolver.AcquireVerifiedLease(GraphHandle.GetValue(),
			Context(RuntimeTransaction.GetMutationToken(), TEXTVIEW("ai"),
				EAssetDocumentHandleKind::Graph)).Lease.IsValid());
	const UBTCompositeNode* RootBefore = F.Tree->RootNode;
	const UBlackboardData* BlackboardBefore = F.Tree->BlackboardAsset;
	const FAssetDocumentPrimitiveOutcome Runtime = Provider->Invoke(Invocation(RuntimeMirrorId,
		EAssetDocumentRuntimeRequestPhase::Repair, RuntimeTransaction.GetMutationToken(), ObjectValue(
			{TEXT("behavior_tree_path"), TEXT("update_flags")}, {StringValue(F.Tree->GetPathName()), IntegerValue(UBehaviorTreeGraph::KeepRebuildCounter)}),
		{{TEXT("asset"), RuntimeAI.GetValue()}, {TEXT("graph"), GraphHandle.GetValue()}}), RuntimeResolver, &RuntimeTransaction);
	Passed &= TestEqual(TEXT("runtime mirror rebuild succeeds"), Runtime.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Passed &= TestEqual(TEXT("runtime mirror has one ChangedSet"), Runtime.ChangedSet.Num(), 1);
	Passed &= TestEqual(TEXT("runtime mirror returns only the frozen three fields"),
		Runtime.Value.ObjectKeys,
		TArray<FString>({TEXT("execution_order_hash"), TEXT("node_count"),
			TEXT("root_node_class_path")}));
	Passed &= TestTrue(TEXT("runtime finalizer precedes its footprint"), RuntimeTransaction.Order.Contains(TEXT("finalizer"))
		&& RuntimeTransaction.Order.Find(TEXT("finalizer")) < RuntimeTransaction.Order.FindLast(TEXT("footprint")));
	Passed &= TestNull(TEXT("runtime invalidates AI handle"), RuntimeResolver.AcquireVerifiedLease(RuntimeAI.GetValue(),
		Context(RuntimeTransaction.GetMutationToken(), TEXTVIEW("ai"), EAssetDocumentHandleKind::Asset)).Lease.Get());
	Passed &= TestNotNull(TEXT("runtime preserves Editor source handle"), RuntimeResolver.AcquireVerifiedLease(RuntimeEditor.GetValue(),
		Context(RuntimeTransaction.GetMutationToken(), TEXTVIEW("editor"), EAssetDocumentHandleKind::Asset)).Lease.Get());
	Passed &= TestTrue(TEXT("runtime success emits one provider-scoped invalidation"),
		RuntimeResolver.GetInvalidationCount() == InvalidationsBeforeBadDto + 1);
	Passed &= TestTrue(TEXT("runtime rollback finalizer succeeds"), RuntimeTransaction.Rollback(TEXT("Test.RuntimeRollback")).bValid);
	Passed &= TestTrue(TEXT("runtime rollback semantic root equality"), F.Tree->RootNode == RootBefore);
	Passed &= TestTrue(TEXT("runtime rollback semantic Blackboard equality"), F.Tree->BlackboardAsset == BlackboardBefore);

	FCacheRefreshTransaction MissingNodeTransaction;
	FCacheRefreshResolver MissingNodeResolver(MissingNodeTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> MissingNodeEditor =
		ResolveEditorAsset(*this, *F.Tree, MissingNodeTransaction, MissingNodeResolver);
	const TOptional<FAssetDocumentOpaqueHandle> MissingNodeAI = MissingNodeEditor.IsSet()
		? MissingNodeResolver.AliasForAI(MissingNodeEditor.GetValue())
		: TOptional<FAssetDocumentOpaqueHandle>();
	if (!MissingNodeAI.IsSet()) return false;
	UObject* TaskInstanceBefore = F.TaskGraph->NodeInstance;
	const bool bMissingNodeDirtyBefore = F.Package->IsDirty();
	const UBlackboardData* MissingNodeRootBefore = F.Root->BlackboardAsset;
	const UBlackboardData* MissingNodeTreeBefore = F.Tree->BlackboardAsset;
	F.TaskGraph->NodeInstance = nullptr;
	const FAssetDocumentPrimitiveOutcome MissingNode = Provider->Invoke(Invocation(BlackboardSetId,
		EAssetDocumentRuntimeRequestPhase::Apply, MissingNodeTransaction.GetMutationToken(), ObjectValue(
			{TEXT("behavior_tree_path"), TEXT("blackboard_path_or_null")},
			{StringValue(F.Tree->GetPathName()), StringValue(F.BlackboardB->GetPathName())}),
		{{TEXT("asset"), MissingNodeAI.GetValue()}}), MissingNodeResolver, &MissingNodeTransaction);
	Passed &= TestEqual(TEXT("missing NodeInstance fails closed"), MissingNode.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Passed &= TestTrue(TEXT("missing NodeInstance has zero mutation delta"),
		MissingNode.ChangedSet.IsEmpty() && MissingNodeTransaction.Order.IsEmpty()
		&& F.Root->BlackboardAsset == MissingNodeRootBefore
		&& F.Tree->BlackboardAsset == MissingNodeTreeBefore
		&& F.Package->IsDirty() == bMissingNodeDirtyBefore);
	F.TaskGraph->NodeInstance = TaskInstanceBefore;

	FCacheRefreshTransaction BlackboardTransaction;
	FCacheRefreshResolver BlackboardResolver(BlackboardTransaction.GetMutationToken());
	const TOptional<FAssetDocumentOpaqueHandle> BlackboardEditor = ResolveEditorAsset(*this, *F.Tree, BlackboardTransaction, BlackboardResolver);
	const TOptional<FAssetDocumentOpaqueHandle> BlackboardAI = BlackboardEditor.IsSet() ? BlackboardResolver.AliasForAI(BlackboardEditor.GetValue()) : TOptional<FAssetDocumentOpaqueHandle>();
	if (!BlackboardAI.IsSet()) return false;
	const bool bBlackboardPackageDirtyBefore = F.Package->IsDirty();
	const FString TaskErrorBefore = F.TaskGraph->ErrorMessage;
	const UBlackboardData* RootBlackboardBefore = F.Root->BlackboardAsset;
	const UBlackboardData* TreeBlackboardBefore = F.Tree->BlackboardAsset;
	const FAssetDocumentPrimitiveOutcome Blackboard = Provider->Invoke(Invocation(BlackboardSetId,
		EAssetDocumentRuntimeRequestPhase::Apply, BlackboardTransaction.GetMutationToken(), ObjectValue(
			{TEXT("behavior_tree_path"), TEXT("blackboard_path_or_null")}, {StringValue(F.Tree->GetPathName()), StringValue(F.BlackboardB->GetPathName())}),
		{{TEXT("asset"), BlackboardAI.GetValue()}}), BlackboardResolver, &BlackboardTransaction);
	Passed &= TestEqual(TEXT("blackboard typed set succeeds"), Blackboard.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Passed &= TestEqual(TEXT("blackboard set one ChangedSet"), Blackboard.ChangedSet.Num(), 1);
	Passed &= TestTrue(TEXT("Root to tree to task closure is native"), F.Root->BlackboardAsset == F.BlackboardB.Get()
		&& F.Tree->BlackboardAsset == F.BlackboardB.Get() && F.Task->GetTreeAsset() == F.Tree.Get()
		&& F.Task->GetBlackboardAsset() == F.BlackboardB.Get()
		&& F.TaskGraph->ErrorMessage == F.Task->GetErrorMessage());
	const FAssetDocumentPrimitiveOutcome ClearBlackboard = Provider->Invoke(Invocation(BlackboardSetId,
		EAssetDocumentRuntimeRequestPhase::Apply, BlackboardTransaction.GetMutationToken(), ObjectValue(
			{TEXT("behavior_tree_path"), TEXT("blackboard_path_or_null")}, {StringValue(F.Tree->GetPathName()), NullValue()}),
		{{TEXT("asset"), BlackboardAI.GetValue()}}), BlackboardResolver, &BlackboardTransaction);
	Passed &= TestEqual(TEXT("blackboard typed clear succeeds"), ClearBlackboard.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Passed &= TestEqual(TEXT("blackboard clear one ChangedSet"), ClearBlackboard.ChangedSet.Num(), 1);
	Passed &= TestTrue(TEXT("clear propagates through Root/tree/task"), F.Root->BlackboardAsset == nullptr
		&& F.Tree->BlackboardAsset == nullptr && F.Task->GetBlackboardAsset() == nullptr);
	Passed &= TestTrue(TEXT("blackboard rollback succeeds"), BlackboardTransaction.Rollback(TEXT("Test.BlackboardRollback")).bValid);
	Passed &= TestTrue(TEXT("blackboard rollback restores Root/tree/task closure"),
		F.Root->BlackboardAsset == RootBlackboardBefore
		&& F.Tree->BlackboardAsset == TreeBlackboardBefore
		&& F.Task->GetTreeAsset() == F.Tree.Get()
		&& F.Task->GetBlackboardAsset() == TreeBlackboardBefore
		&& F.TaskGraph->ErrorMessage == TaskErrorBefore
		&& F.Package->IsDirty() == bBlackboardPackageDirtyBefore);
	return Passed && !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
