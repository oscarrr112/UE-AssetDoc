// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentRollbackAction.h"
#include "AIGraphTypes.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "ScopedTransaction.h"
#include "Misc/ScopeExit.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
namespace AssetDocumentAISelectorBatchTests
{
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

class FNoCancellation final : public IAssetDocumentCancellationView
{
public:
 virtual bool IsCancellationRequested() const override { return false; }
};
// Instrumentation delegates all admission and rollback decisions to the real kernel.
class FObservedTransaction final : public IAssetDocumentAtomicTransaction
{
public:
 explicit FObservedTransaction(IAssetDocumentAtomicTransaction& In) : Inner(In) {}
 IAssetDocumentAtomicTransaction& Inner;
 TArray<FString> Actions;
 TArray<TSharedRef<IAssetDocumentRollbackAction>> RetainedActions;
 TArray<FAssetDocumentRuntimeDiagnostic> ActionDiagnostics;
 TArray<FAssetDocumentMutationFootprint> Rows;
 virtual FGuid GetTransactionId() const override { return Inner.GetTransactionId(); }
 virtual EAssetDocumentAtomicState GetState() const override { return Inner.GetState(); }
 virtual const FAssetDocumentMutationToken& GetMutationToken() const override { return Inner.GetMutationToken(); }
 virtual FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Value) override
 { Rows.Append(Value.GetData(), Value.Num()); return Inner.EnlistFootprint(Value); }
 virtual FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction> Value) override
 { Actions.Add(FString(Value->GetRollbackComponentId())); auto R=Inner.EnlistRollbackAction(Value); if(R.bValid) RetainedActions.Add(Value); ActionDiagnostics.Append(R.Diagnostics); return R; }
 virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer> Value) override { return Inner.EnlistPostRollbackFinalizer(Value); }
 virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Inner.MarkReadyToCommit(); }
 virtual FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt& Value) override { return Inner.Commit(Value); }
 virtual FAssetDocumentContractValidationResult Rollback(FStringView Value) override { return Inner.Rollback(Value); }
};
FAssetDocumentRuntimeValueDto String(FString Text)
{ FAssetDocumentRuntimeValueDto V; V.Kind=EAssetDocumentRuntimeValueKind::String; V.StringValue=MoveTemp(Text); return V; }
FAssetDocumentRuntimeValueDto Object(TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values)
{ FAssetDocumentRuntimeValueDto V; V.Kind=EAssetDocumentRuntimeValueKind::Object; V.ObjectKeys=MoveTemp(Keys); V.Elements=MoveTemp(Values); return V; }
UEdGraphPin* Pin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{ if(Node) for(auto* P : Node->Pins) if(P && P->Direction==Direction) return P; return nullptr; }
struct FScenario
{
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UBehaviorTree> Tree;
 UBehaviorTreeGraph* Graph=nullptr;
 UBlackboardData* Blackboard=nullptr;
 TArray<UBehaviorTreeGraphNode_Task*> Nodes;
 FResolveExistingHandleResolver Resolver;
 TUniquePtr<FAssetDocumentRollbackActionComposition> Composition;
 TUniquePtr<IAssetDocumentAtomicTransaction> Atomic;
 TUniquePtr<FObservedTransaction> Transaction;
 FAssetDocumentOpaqueHandle Handle;
 TArray<FAssetDocumentRuntimeDiagnostic> KernelDiagnostics;
 ~FScenario() { if(Tree.IsValid()) Tree->ClearFlags(RF_Standalone); if(Blackboard) Blackboard->ClearFlags(RF_Standalone); if(Package.IsValid()) Package->SetDirtyFlag(false); }
 FBlackboardKeySelector& Selector(int32 I)
 { return *FindFProperty<FStructProperty>(Nodes[I]->NodeInstance->GetClass(), TEXT("DirectSelector"))->ContainerPtrToValuePtr<FBlackboardKeySelector>(Nodes[I]->NodeInstance); }
 FString Id(int32 I) const { return Nodes[I]->NodeGuid.ToString(EGuidFormats::Digits); }
 FString ScalarId(int32 I) const { return Nodes[I]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower); }
 bool SetUp(FAutomationTestBase& Test, int32 Count, bool IncludeScalarRows=false, bool WrongAggregateRow=false)
 {
  Package.Reset(CreatePackage(*(TEXT("/Temp/AssetDocumentAI/SelectorBatch_")+FGuid::NewGuid().ToString(EGuidFormats::Digits))));
  Tree.Reset(NewObject<UBehaviorTree>(Package.Get(),TEXT("Tree"),RF_Public|RF_Standalone));
  Blackboard=NewObject<UBlackboardData>(Package.Get(),TEXT("Blackboard"),RF_Public|RF_Standalone);
  Blackboard->UpdatePersistentKey<UBlackboardKeyType_Object>(TEXT("Target"));
  Tree->BlackboardAsset=Blackboard;
  if(!Test.TestTrue(TEXT("fixture owns an independent Editor transaction"),GEditor&&GEditor->Trans&&!GEditor->Trans->IsActive())) return false;
  FScopedTransaction FixtureTransaction(FText::FromString(TEXT("SelectorBatch fixture construction")));
  ON_SCOPE_EXIT { FixtureTransaction.Cancel(); };
  Graph=CastChecked<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(Tree.Get(),TEXT("BehaviorTreeGraph"),UBehaviorTreeGraph::StaticClass(),UEdGraphSchema_BehaviorTree::StaticClass()));
  Tree->BTGraph=Graph; Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph); Graph->OnCreated(); Graph->Initialize();
  auto* Root=CastChecked<UBehaviorTreeGraphNode_Root>(Graph->Nodes[0]); Root->BlackboardAsset=Blackboard; Root->UpdateBlackboard();
  FAISchemaAction_NewNode C; C.NodeTemplate=NewObject<UBehaviorTreeGraphNode_Composite>(); C.NodeTemplate->NodeInstance=NewObject<UBTComposite_Sequence>(C.NodeTemplate);
  auto* Composite=C.PerformAction(Graph,Pin(Root,EGPD_Output),FVector2f(100,0),false);
  UClass* TaskClass=LoadClass<UBTTaskNode>(nullptr,TEXT("/Script/AssetDocumentTests.AssetDocumentSelectorTaskTestNode"));
  if(!Test.TestNotNull(TEXT("selector fixture task class"),TaskClass)) return false;
  for(int32 I=0;I<Count;++I)
  {
   FAISchemaAction_NewNode A; A.NodeTemplate=NewObject<UBehaviorTreeGraphNode_Task>(); A.NodeTemplate->NodeInstance=NewObject<UBTTaskNode>(A.NodeTemplate,TaskClass);
   auto* Node=Cast<UBehaviorTreeGraphNode_Task>(A.PerformAction(Graph,Pin(Composite,EGPD_Output),FVector2f(200,I*100),false));
   if(!Test.TestNotNull(TEXT("stock selector task exists"),Node)) return false;
   Node->NodeGuid=FGuid(0,0,0,I+1); Nodes.Add(Node);
  }
  Tree->BlackboardAsset=nullptr; Root->UpdateBlackboard();
  for(int32 I=0;I<Count;++I) { Selector(I).SelectedKeyName=TEXT("Target"); Selector(I).InvalidateResolvedKey(); Selector(I).SelectedKeyType=nullptr; }
  Package->SetDirtyFlag(false);
  // Stock node actions transact even when bSelectNewNode is false. Discard only
  // this fixture's undo record before the real Atomic transaction starts.
  FixtureTransaction.Cancel();
  Composition=MakeUnique<FAssetDocumentRollbackActionComposition>(Resolver,TArray<TSharedRef<IAssetDocumentRollbackAction>>{},[this](EAssetDocumentRuntimeRequestPhase,const FAssetDocumentRuntimeDiagnostic& D){KernelDiagnostics.Add(D);});
  FAssetDocumentPreflightReceipt R; R.RequestId=FGuid::NewGuid(); R.RuntimeRevision=1; R.ReceiptHash=TEXT("sha256:selector-batch-local-exact"); R.bSucceeded=true;
  R.CompleteFootprint={{EAssetDocumentMutationFootprintKind::UObjectState,TEXT("asset:")+Tree->GetPathName()+TEXT("/SelectorCache"),TEXT("assetdocument.ai.behavior_tree.selector_cache.v1")}};
  // Scalar characterization needs its historical per-selector rows as well.
  if(IncludeScalarRows) for(int32 I=0;I<Count;++I) R.CompleteFootprint.Add({EAssetDocumentMutationFootprintKind::UObjectState,TEXT("asset:")+Tree->GetPathName()+TEXT("/Nodes[guid=")+ScalarId(I)+TEXT("].DirectSelector"),TEXT("assetdocument.ai.behavior_tree.selector_cache.v1")});
  if(WrongAggregateRow) R.CompleteFootprint[0].StableId+=TEXT("_Undeclared");
  static FNoCancellation Cancellation; Atomic=Composition->BeginLiveAfterPreflight(R,Cancellation);
  if(!Test.TestNotNull(TEXT("real Atomic live"),Atomic.Get())) return false;
  Transaction=MakeUnique<FObservedTransaction>(*Atomic); Resolver.SetMutationToken(Atomic->GetMutationToken());
  auto Lease=ResolveExistingMutableAssetLease(Test,*Tree,*Atomic,Resolver); if(!Lease) return false;
  auto Alias=Resolver.RegisterProviderScopedAlias(Lease->GetHandle(),TEXTVIEW("ai"),EAssetDocumentHandleAccess::Mutable); if(!Alias.IsSet()) return false; Handle=Alias.GetValue(); return true;
 }
 FAssetDocumentPrimitiveInvocation Invocation(bool Scalar=false,int32 Index=0)
 {
  FAssetDocumentPrimitiveInvocation I; I.PrimitiveId=Scalar?TEXT("ai.behavior_tree.selector.resolve.v1"):TEXT("ai.behavior_tree.selector.resolve_batch.v1"); I.Version=1;
  const auto& T=Atomic->GetMutationToken(); I.RequestId=T.RequestId; I.TransactionId=T.TransactionId; I.RuntimeRevision=T.RuntimeRevision; I.Phase=EAssetDocumentRuntimeRequestPhase::Repair; I.HandleArguments={{TEXT("asset"),Handle}};
  if(Scalar) I.ValueArguments=Object({TEXT("behavior_tree_path"),TEXT("blackboard_path"),TEXT("property_path"),TEXT("selected_key_name"),TEXT("stable_node_id")},{String(Tree->GetPathName()),String(Blackboard->GetPathName()),String(TEXT("DirectSelector")),String(TEXT("Target")),String(ScalarId(Index))});
  else {
   FAssetDocumentRuntimeValueDto Entries; Entries.Kind=EAssetDocumentRuntimeValueKind::Array;
   for(int32 N=0;N<Nodes.Num();++N) Entries.Elements.Add(Object({TEXT("property_path"),TEXT("selected_key_name"),TEXT("stable_node_id")},{String(TEXT("DirectSelector")),String(TEXT("Target")),String(Id(N))}));
   I.ValueArguments=Object({TEXT("behavior_tree_path"),TEXT("blackboard_path"),TEXT("selectors")},{String(Tree->GetPathName()),String(Blackboard->GetPathName()),MoveTemp(Entries)});
  } return I;
 }
 FAssetDocumentPrimitiveOutcome Invoke(FAssetDocumentPrimitiveInvocation I)
 { return CreateAssetDocumentAIPrimitiveProvider()->Invoke(I,Resolver,Transaction.Get()); }
 void CheckRestored(FAutomationTestBase& Test, FName SecondName=TEXT("Target"))
 {
  for(int32 I=0;I<Nodes.Num();++I) {
   Test.TestEqual(TEXT("authored selector name restored"),Selector(I).SelectedKeyName,I==1?SecondName:FName(TEXT("Target")));
   Test.TestEqual(TEXT("derived key ID restored"),Selector(I).GetSelectedKeyID(),FBlackboard::InvalidKey);
   Test.TestNull(TEXT("derived type restored"),Selector(I).SelectedKeyType.Get());
  } Test.TestFalse(TEXT("dirty ownership restored"),Package->IsDirty());
 }
};
bool Succeeded(FAutomationTestBase& Test,const FAssetDocumentPrimitiveOutcome& O)
{ for(const auto& D:O.Diagnostics) Test.AddInfo(D.Code+TEXT(": ")+D.Message); return Test.TestEqual(TEXT("batch succeeds"),O.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded); }
} // namespace
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSelectorBatchNullableTest,"UEAssetDoc.AI.SelectorBatch.NullableNoneAndRollback",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FSelectorBatchNullableTest::RunTest(const FString&)
{
 using namespace AssetDocumentAISelectorBatchTests;
 for (bool NullBlackboard : {false,true}) for (int32 Count : {0,1,2})
 {
  FScenario S; if(!S.SetUp(*this,Count)) return false;
  auto I=S.Invocation(); I.PrimitiveId=TEXT("ai.behavior_tree.selector.resolve_batch.v2"); I.Version=2;
  if(Count)
  {
   S.Selector(0).ResolveSelectedKey(*S.Blackboard);
   S.Selector(0).SelectedKeyName=NAME_None; S.Selector(0).AllowNoneAsValue(true);
   I.ValueArguments.Elements[2].Elements[0].Elements[1]=String(TEXT("None"));
  }
  if(Count==2)
  {
   S.Selector(1).SelectedKeyName=NAME_None; S.Selector(1).AllowNoneAsValue(true);
   I.ValueArguments.Elements[2].Elements[1].Elements[1]=String(TEXT("None"));
  }
  const auto OldId=Count?S.Selector(0).GetSelectedKeyID():FBlackboard::InvalidKey;
  if(NullBlackboard)
  {
   S.Tree->BlackboardAsset=nullptr;
   I.ValueArguments.Elements[1]=FAssetDocumentRuntimeValueDto();
   I.ValueArguments.Elements[1].Kind=EAssetDocumentRuntimeValueKind::Null;
   auto Legacy=I; Legacy.PrimitiveId=TEXT("ai.behavior_tree.selector.resolve_batch.v1"); Legacy.Version=1;
   TestNotEqual(TEXT("old batch rejects null schema"),S.Invoke(Legacy).Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
   TestTrue(TEXT("old nullable rejection enlists no row"),S.Transaction->Rows.IsEmpty());
  }
  const auto O=S.Invoke(I); if(!Succeeded(*this,O)) return false;
  TestEqual(TEXT("nullable batch retains one aggregate row"),S.Transaction->Rows.Num(),1);
  if(Count)
  {
   TestTrue(TEXT("native None policy and invalid ID observed"),S.Selector(0).IsNone());
   TestEqual(TEXT("None wire ID is invalid sentinel"),O.Value.Elements[1].Elements[0].Elements[1].IntegerValue,int64(-1));
   TestEqual(TEXT("stock retained type is reported truthfully"),O.Value.Elements[1].Elements[0].Elements[3].StringValue,S.Selector(0).SelectedKeyType->GetPathName());
  }
  if(Count==2)TestEqual(TEXT("native absent type is reported null"),O.Value.Elements[1].Elements[1].Elements[3].Kind,EAssetDocumentRuntimeValueKind::Null);
  TestTrue(TEXT("nullable aggregate receipt complete"),S.Transaction->MarkReadyToCommit().bValid);
  TestTrue(TEXT("nullable real Atomic inverse"),S.Transaction->Rollback(TEXTVIEW("Test.NullableNone")).bValid);
  if(Count)
  {
   TestEqual(TEXT("exact stale preimage ID restored"),S.Selector(0).GetSelectedKeyID(),OldId);
   TestTrue(TEXT("authored None restored"),S.Selector(0).SelectedKeyName.IsNone());
  }
 }
 for(bool ForbiddenNone : {false,true})
 {
  FScenario S; if(!S.SetUp(*this,1)) return false;
  S.Tree->BlackboardAsset=nullptr;
  auto I=S.Invocation(); I.PrimitiveId=TEXT("ai.behavior_tree.selector.resolve_batch.v2"); I.Version=2;
  I.ValueArguments.Elements[1]=FAssetDocumentRuntimeValueDto(); I.ValueArguments.Elements[1].Kind=EAssetDocumentRuntimeValueKind::Null;
  if(ForbiddenNone) { S.Selector(0).SelectedKeyName=NAME_None; S.Selector(0).AllowNoneAsValue(false); I.ValueArguments.Elements[2].Elements[0].Elements[1]=String(TEXT("None")); }
  TestNotEqual(TEXT("real dependency or forbidden None rejects"),S.Invoke(I).Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  TestTrue(TEXT("rejection captures no action"),S.Transaction->Actions.IsEmpty());
  TestTrue(TEXT("rejection enlists no row"),S.Transaction->Rows.IsEmpty());
  TestTrue(TEXT("rejected Atomic rollback"),S.Transaction->Rollback(TEXTVIEW("Test.NullableReject")).bValid);
 }
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSelectorBatchAtomicCountsTest,"UEAssetDoc.AI.SelectorBatch.AtomicCountsAndReverseRollback",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FSelectorBatchAtomicCountsTest::RunTest(const FString&)
{
 using namespace AssetDocumentAISelectorBatchTests;
 for(int32 Count:{0,1,2}) {
  FScenario S; if(!S.SetUp(*this,Count)) return false;
  const auto O=S.Invoke(S.Invocation()); if(!Succeeded(*this,O)) return false;
  TestEqual(TEXT("exactly one action"),S.Transaction->Actions,TArray<FString>{TEXT("assetdocument.ai.behavior_tree.selector_cache.v1")});
  TestEqual(TEXT("exactly one enlisted row"),S.Transaction->Rows.Num(),1);
  TestEqual(TEXT("exactly one ChangedSet"),O.ChangedSet.Num(),1);
  if(O.ChangedSet.Num()==1) TestEqual(TEXT("ChangedSet aggregate path"),O.ChangedSet[0].Path,TEXT("asset:")+S.Tree->GetPathName()+TEXT("/SelectorCache"));
  TestTrue(TEXT("no handles"),O.HandleResults.IsEmpty());
  if(S.Transaction->Rows.Num()==1) TestEqual(TEXT("aggregate row canonical path"),S.Transaction->Rows[0].StableId,TEXT("asset:")+S.Tree->GetPathName()+TEXT("/SelectorCache"));
  TestEqual(TEXT("output exact keys"),O.Value.ObjectKeys,TArray<FString>{TEXT("selector_count"),TEXT("selectors")});
  if(O.Value.Elements.Num()==2) {
   TestEqual(TEXT("output exact count"),O.Value.Elements[0].IntegerValue,int64(Count)); TestEqual(TEXT("output inventory count"),O.Value.Elements[1].Elements.Num(),Count);
   for(int32 I=0;I<O.Value.Elements[1].Elements.Num();++I) {
    const auto& V=O.Value.Elements[1].Elements[I];
    TestEqual(TEXT("selector output exact fields"),V.ObjectKeys,TArray<FString>{TEXT("property_path"),TEXT("selected_key_id"),TEXT("selected_key_name"),TEXT("selected_key_type_path"),TEXT("stable_node_id")});
    if(V.Elements.Num()==5) { TestEqual(TEXT("canonical node order"),V.Elements[4].StringValue,S.Id(I)); TestEqual(TEXT("batch output node ID is canonical 32 hex"),V.Elements[4].StringValue.Len(),32); TestEqual(TEXT("exact property path"),V.Elements[0].StringValue,FString(TEXT("DirectSelector"))); TestEqual(TEXT("authored name"),V.Elements[2].StringValue,FString(TEXT("Target"))); TestEqual(TEXT("public ID agrees"),V.Elements[1].IntegerValue,int64(static_cast<int32>(S.Selector(I).GetSelectedKeyID()))); TestEqual(TEXT("public type agrees"),V.Elements[3].StringValue,S.Selector(I).SelectedKeyType->GetPathName()); }
   }
  } else AddError(TEXT("output must contain count and array"));
  TestTrue(TEXT("aggregate-only receipt is fully enlisted and ready"),S.Transaction->MarkReadyToCommit().bValid);
  TArray<int32> Order;
  const FDelegateHandle Listener=FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([&](UObject* Owner,FPropertyChangedEvent&){for(int32 I=0;I<S.Nodes.Num();++I) if(Owner==S.Nodes[I]->NodeInstance && (Order.IsEmpty()||Order.Last()!=I)) Order.Add(I);});
  TestTrue(TEXT("real Atomic rollback succeeds"),S.Transaction->Rollback(TEXTVIEW("Test.BatchReverse")).bValid);
  FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Listener);
  TArray<int32> Expected; for(int32 I=Count-1;I>=0;--I) Expected.Add(I);
  TestEqual(TEXT("rollback notifications follow reverse canonical owner order"),Order,Expected); S.CheckRestored(*this);
  TWeakObjectPtr<UBehaviorTree> WeakTree(S.Tree.Get());
  TWeakObjectPtr<UPackage> WeakPackage(S.Package.Get());
  TWeakObjectPtr<UBehaviorTreeGraph> WeakGraph(S.Graph);
  S.Tree->ClearFlags(RF_Public|RF_Standalone); S.Blackboard->ClearFlags(RF_Public|RF_Standalone);
  S.Tree.Reset(); S.Package.Reset(); S.Blackboard=nullptr; S.Graph=nullptr; S.Nodes.Reset();
  CollectGarbage(RF_NoFlags);
  TestFalse(TEXT("successful aggregate releases tree while action remains alive"),WeakTree.IsValid());
  TestFalse(TEXT("successful aggregate releases graph"),WeakGraph.IsValid());
  TestFalse(TEXT("successful aggregate releases package"),WeakPackage.IsValid());
  for(const auto& Action:S.Transaction->RetainedActions)
   TestTrue(TEXT("successful action caches result after objects are collected"),Action->Rollback(TEXTVIEW("Test.BatchRepeat")).bValid);
 } return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSelectorBatchRestoreFailureLifetimeTest,"UEAssetDoc.AI.SelectorBatch.RestoreFailureRetainsPreimage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FSelectorBatchRestoreFailureLifetimeTest::RunTest(const FString&)
{
 using namespace AssetDocumentAISelectorBatchTests;
 FScenario S; if(!S.SetUp(*this,1)||!Succeeded(*this,S.Invoke(S.Invocation()))) return false;
 S.Tree->BTGraph=nullptr;
 TestFalse(TEXT("changed graph binding rejects real Atomic rollback"),S.Transaction->Rollback(TEXTVIEW("Test.BatchWrongGraph")).bValid);
 TWeakObjectPtr<UBehaviorTree> WeakTree(S.Tree.Get());
 TWeakObjectPtr<UPackage> WeakPackage(S.Package.Get());
 TWeakObjectPtr<UBehaviorTreeGraph> WeakGraph(S.Graph);
 TWeakObjectPtr<UObject> WeakOwner(S.Nodes[0]->NodeInstance);
 S.Tree->ClearFlags(RF_Public|RF_Standalone); S.Blackboard->ClearFlags(RF_Public|RF_Standalone);
 S.Package->SetDirtyFlag(false); S.Tree.Reset(); S.Package.Reset(); S.Blackboard=nullptr; S.Graph=nullptr; S.Nodes.Reset();
 CollectGarbage(RF_NoFlags);
 TestTrue(TEXT("failed aggregate retains tree graph owner and package"),WeakTree.IsValid()&&WeakGraph.IsValid()&&WeakOwner.IsValid()&&WeakPackage.IsValid());
 // Repairing the binding cannot retry an already consumed failed preimage.
 if(WeakTree.IsValid()) WeakTree->BTGraph=WeakGraph.Get();
 for(const auto& Action:S.Transaction->RetainedActions)
 {
  const auto Repeated=Action->Rollback(TEXTVIEW("Test.BatchRepeatFailure"));
  TestFalse(TEXT("failed action remains failed after binding repair"),Repeated.bValid);
  TestTrue(TEXT("cached failure preserves binding diagnostic"),Repeated.Diagnostics.ContainsByPredicate([](const auto& D){return D.Code==TEXT("AITypedPreimage.BlackboardSelectorRollbackIdentityMismatch");}));
 }
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSelectorBatchPartialFailureTest,"UEAssetDoc.AI.SelectorBatch.PartialFailureZeroResidual",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FSelectorBatchPartialFailureTest::RunTest(const FString&)
{
 using namespace AssetDocumentAISelectorBatchTests; FScenario S; if(!S.SetUp(*this,2)) return false;
 S.Selector(1).SelectedKeyName=TEXT("MissingKey");
 S.Selector(1).AllowedTypes.Reset(); auto* Filter=NewObject<UBlackboardKeyType_Int>(S.Nodes[1]->NodeInstance); S.Selector(1).AllowedTypes.Add(Filter);
 auto I=S.Invocation(); I.ValueArguments.Elements[2].Elements[1].Elements[1]=String(TEXT("MissingKey"));
 AddExpectedError(TEXT("Failed to find key 'MissingKey'"),EAutomationExpectedErrorFlags::Contains,1);
 const auto O=S.Invoke(I); TestNotEqual(TEXT("second selector missing key fails"),O.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 TestEqual(TEXT("complete inventory enlists one action before resolving"),S.Transaction->Actions.Num(),1); TestEqual(TEXT("aggregate row enlisted once"),S.Transaction->Rows.Num(),1);
 TestNotEqual(TEXT("first selector actually resolved before second failed"),S.Selector(0).GetSelectedKeyID(),FBlackboard::InvalidKey);
 TestTrue(TEXT("failed call returns no success ChangedSet"),O.ChangedSet.IsEmpty());
 TestTrue(TEXT("failure rollback succeeds"),S.Transaction->Rollback(TEXTVIEW("Test.BatchPartialFailure")).bValid); S.CheckRestored(*this,TEXT("MissingKey"));
 TestEqual(TEXT("filter identity survives"),S.Selector(1).AllowedTypes.Num(),1); if(S.Selector(1).AllowedTypes.Num()==1) TestTrue(TEXT("same filter object"),S.Selector(1).AllowedTypes[0].Get()==Filter); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSelectorBatchRejectedInventoryTest,"UEAssetDoc.AI.SelectorBatch.RejectedInventoryZeroResidual",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FSelectorBatchRejectedInventoryTest::RunTest(const FString&)
{
 using namespace AssetDocumentAISelectorBatchTests;
 for(int32 Case=0;Case<8;++Case) {
  FScenario S; if(!S.SetUp(*this,2)) return false; auto I=S.Invocation(); auto& Entries=I.ValueArguments.Elements[2].Elements;
  if(Case==0) Entries[0].Elements[2]=String(TEXT("not-a-guid"));
  if(Case==1) Entries[1]=Entries[0];
  if(Case==2) Swap(Entries[0],Entries[1]);
  if(Case==3) Entries[1].Elements[1]=String(TEXT("AuthoredDrift"));
  if(Case==4) I.ValueArguments.Elements[1]=String(S.Tree->GetPathName());
  if(Case==5) Entries[1].Elements[0]=String(TEXT("MissingProperty"));
  if(Case==6) { auto* Foreign=NewObject<UBehaviorTree>(S.Package.Get()); S.Nodes[1]->NodeInstance=NewObject<UBTTaskNode>(Foreign,S.Nodes[0]->NodeInstance->GetClass()); }
  if(Case==7) Entries[0].ObjectKeys.Add(TEXT("unexpected")), Entries[0].Elements.Add(String(TEXT("extra")));
  const auto O=S.Invoke(I); TestNotEqual(*FString::Printf(TEXT("invalid inventory case %d rejected"),Case),O.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  TestTrue(TEXT("invalid inventory enlists no action"),S.Transaction->Actions.IsEmpty()); TestTrue(TEXT("invalid inventory enlists no row"),S.Transaction->Rows.IsEmpty()); TestTrue(TEXT("invalid inventory has no ChangedSet"),O.ChangedSet.IsEmpty()); TestTrue(TEXT("invalid inventory has no handles"),O.HandleResults.IsEmpty());
  if(Case!=6) S.CheckRestored(*this);
  else {TestEqual(TEXT("first cache untouched on owner rebound"),S.Selector(0).GetSelectedKeyID(),FBlackboard::InvalidKey); TestFalse(TEXT("owner rebound adds no package dirty"),S.Package->IsDirty());}
  TestTrue(TEXT("empty failed transaction rollback succeeds"),S.Transaction->Rollback(TEXTVIEW("Test.BatchReject")).bValid);
 } return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSelectorBatchScalarExclusionTest,"UEAssetDoc.AI.SelectorBatch.ScalarLateEnlistmentAndMutualExclusion",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FSelectorBatchScalarExclusionTest::RunTest(const FString&)
{
 using namespace AssetDocumentAISelectorBatchTests;
 for(int32 Mode=0;Mode<3;++Mode) {
  FScenario S; if(!S.SetUp(*this,2,true)) return false;
  const auto First=S.Invoke(S.Invocation(Mode!=2,0)); if(!Succeeded(*this,First)) return false;
  const auto Second=S.Invoke(S.Invocation(Mode!=1,1));
  TestNotEqual(TEXT("second owner of fixed selector component rejected"),Second.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  bool Duplicate=false; for(const auto& D:S.Transaction->ActionDiagnostics) if(D.Code==TEXT("Atomic.RollbackActionLateEnlistment")) Duplicate=true;
  TestTrue(TEXT("real kernel characterizes exact late enlistment"),Duplicate);
  TestEqual(TEXT("second rejected before additional footprint"),S.Transaction->Rows.Num(),1);
  TestTrue(TEXT("mixed/scalar rollback succeeds"),S.Transaction->Rollback(TEXTVIEW("Test.SelectorExclusion")).bValid); S.CheckRestored(*this);
 } return true;
}
// Initial false DuplicateComponentId expectation remains preserved in commit 3a15674e
// and initial-runtime-01. The real kernel checks LateEnlistment first after footprint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSelectorScalarLateEnlistmentCharacterizationTest,"UEAssetDoc.AI.SelectorBatch.ScalarLateEnlistmentCharacterization",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FSelectorScalarLateEnlistmentCharacterizationTest::RunTest(const FString&)
{
 using namespace AssetDocumentAISelectorBatchTests;
 FScenario S; if(!S.SetUp(*this,2,true)) return false;
 const auto First=S.Invoke(S.Invocation(true,0)); if(!Succeeded(*this,First)) return false;
 const auto Second=S.Invoke(S.Invocation(true,1));
 TestNotEqual(TEXT("scalar selector two rejected"),Second.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 bool Duplicate=false; for(const auto& D:S.Transaction->ActionDiagnostics) if(D.Code==TEXT("Atomic.RollbackActionLateEnlistment")) Duplicate=true;
 TestTrue(TEXT("real kernel exact late-enlistment terminal"),Duplicate);
 TestEqual(TEXT("selector one alone enlisted a footprint"),S.Transaction->Rows.Num(),1);
 TestTrue(TEXT("scalar duplicate rollback succeeds"),S.Transaction->Rollback(TEXTVIEW("Test.ScalarDuplicate")).bValid);
 S.CheckRestored(*this); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSelectorBatchDescriptorTest,"UEAssetDoc.AI.SelectorBatch.ExactDescriptor",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FSelectorBatchDescriptorTest::RunTest(const FString&)
{
 const auto Provider=CreateAssetDocumentAIPrimitiveProvider();
 const FAssetDocumentPrimitiveDescriptor* Batch=nullptr; int32 Count=0;
 for(const auto& D:Provider->DescribePrimitives()) if(D.PrimitiveId==TEXT("ai.behavior_tree.selector.resolve_batch.v1")) { Batch=&D; ++Count; }
 TestEqual(TEXT("one independent batch descriptor"),Count,1); if(!Batch) return false;
 TestEqual(TEXT("version one"),Batch->Version,uint32(1)); TestEqual(TEXT("provider AI"),Batch->ProviderId,FString(TEXT("ai")));
 TestEqual(TEXT("mutating access"),Batch->Access,EAssetDocumentPrimitiveAccess::Mutating);
 TestEqual(TEXT("Repair only"),Batch->AllowedPhases,TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Repair});
 TestEqual(TEXT("unchanged consumer"),Batch->RequiredConsumerIds,TArray<FString>{TEXT("ai_blackboard_binding_repair")});
 TestTrue(TEXT("active transaction required"),Batch->bRequiresActiveTransaction);
 TestEqual(TEXT("fixed rollback schema"),Batch->MutationFootprintSchemaId,FString(TEXT("assetdocument.ai.behavior_tree.selector_cache.v1")));
 TestTrue(TEXT("no result handles"),Batch->HandleResultSchemas.IsEmpty());
 TestEqual(TEXT("one asset input handle"),Batch->HandleArgumentSchemas.Num(),1);
 if(Batch->HandleArgumentSchemas.Num()==1) { const auto& A=Batch->HandleArgumentSchemas[0]; TestEqual(TEXT("asset name"),A.Name,FString(TEXT("asset"))); TestEqual(TEXT("Asset kind"),A.Kind,EAssetDocumentHandleKind::Asset); TestEqual(TEXT("mutable asset"),A.RequiredAccess,EAssetDocumentHandleAccess::Mutable); TestTrue(TEXT("asset required"),A.bRequired); }
 const TArray<FString> Codes={TEXT("AIProvider.InvalidValueShape"),TEXT("AIProvider.InvalidContext"),TEXT("AIProvider.InvalidTransaction"),TEXT("AIProvider.InvalidHandle"),TEXT("AIProvider.AuthorityUnavailable"),TEXT("AIProvider.OperationRejected"),TEXT("AIProvider.BehaviorTree.SelectorBatchInvalidInventory"),TEXT("AIProvider.BehaviorTree.SelectorBatchDuplicateLocator"),TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"),TEXT("AIProvider.BehaviorTree.BlackboardMismatch"),TEXT("AIProvider.BehaviorTree.SelectorResolveFailed"),TEXT("AIProvider.BehaviorTree.SelectorBatchCompletionMismatch")};
 TestEqual(TEXT("complete closed diagnostic array"),Batch->StableDiagnosticCodes,Codes); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSelectorBatchContextTest,"UEAssetDoc.AI.SelectorBatch.InvalidContextZeroResidual",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FSelectorBatchContextTest::RunTest(const FString&)
{
 using namespace AssetDocumentAISelectorBatchTests;
 const TArray<EAssetDocumentRuntimeRequestPhase> Phases={EAssetDocumentRuntimeRequestPhase::Validate,EAssetDocumentRuntimeRequestPhase::Preflight,EAssetDocumentRuntimeRequestPhase::Apply,EAssetDocumentRuntimeRequestPhase::Verify,EAssetDocumentRuntimeRequestPhase::Persist,EAssetDocumentRuntimeRequestPhase::Complete,EAssetDocumentRuntimeRequestPhase::Failed};
 for(int32 Case=0;Case<Phases.Num()+6;++Case) {
  FScenario S; if(!S.SetUp(*this,2)) return false; auto I=S.Invocation(); auto* Tx=S.Transaction.Get();
  if(Case<Phases.Num()) I.Phase=Phases[Case];
  else switch(Case-Phases.Num()) {
   case 0: Tx=nullptr; break;
   case 1: I.TransactionId=FGuid::NewGuid(); break;
   case 2: I.HandleArguments.Reset(); break;
   case 3: I.HandleArguments[0].Handle.Access=EAssetDocumentHandleAccess::ReadOnly; break;
   case 4: ++I.HandleArguments[0].Handle.Generation; break;
   case 5: I.HandleArguments[0].Handle.Kind=EAssetDocumentHandleKind::Graph; break;
  }
  const auto O=CreateAssetDocumentAIPrimitiveProvider()->Invoke(I,S.Resolver,Tx);
  TestNotEqual(*FString::Printf(TEXT("context case %d rejected"),Case),O.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  TestFalse(TEXT("context rejection gives diagnostic"),O.Diagnostics.IsEmpty());
  TestTrue(TEXT("bad context no action"),S.Transaction->Actions.IsEmpty()); TestTrue(TEXT("bad context no footprint"),S.Transaction->Rows.IsEmpty()); TestTrue(TEXT("bad context no changed rows"),O.ChangedSet.IsEmpty()); TestTrue(TEXT("bad context no result handles"),O.HandleResults.IsEmpty()); S.CheckRestored(*this);
  TestTrue(TEXT("bad context empty rollback"),S.Transaction->Rollback(TEXTVIEW("Test.BatchContext")).bValid);
 } return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSelectorBatchCaptureAndFootprintRejectionTest,"UEAssetDoc.AI.SelectorBatch.CaptureAndFootprintRejectionZeroResidual",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FSelectorBatchCaptureAndFootprintRejectionTest::RunTest(const FString&)
{
 using namespace AssetDocumentAISelectorBatchTests;
 for(bool MissingFootprint:{false,true}) {
  FScenario S; if(!S.SetUp(*this,2,false,MissingFootprint)) return false;
  if(!MissingFootprint) S.Selector(1).AllowedTypes.Add(nullptr);
  const auto O=S.Invoke(S.Invocation()); TestNotEqual(TEXT("capture/footprint rejects"),O.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  TestEqual(TEXT("capture/footprint top-level exact rejection"),O.Diagnostics.Num(),1);
  if(O.Diagnostics.Num()==1) TestEqual(TEXT("operation rejected exact"),O.Diagnostics[0].Code,FString(TEXT("AIProvider.OperationRejected")));
  TestEqual(TEXT("only footprint case reached action enlist"),S.Transaction->Actions.Num(),MissingFootprint?1:0);
  TestEqual(TEXT("only footprint case attempted row enlist"),S.Transaction->Rows.Num(),MissingFootprint?1:0);
  TestTrue(TEXT("rejection no changed rows"),O.ChangedSet.IsEmpty()); TestTrue(TEXT("rejection no handle results"),O.HandleResults.IsEmpty()); S.CheckRestored(*this);
  TestTrue(TEXT("capture/footprint real kernel rollback succeeds"),S.Transaction->Rollback(TEXTVIEW("Test.BatchCaptureFootprint")).bValid); S.CheckRestored(*this);
  if(!MissingFootprint) { TestTrue(TEXT("invalid filter retained without writes"),S.Selector(1).AllowedTypes.Last()==nullptr); }
 } return true;
}
#endif
