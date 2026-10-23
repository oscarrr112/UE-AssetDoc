// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentAIBehaviorTreeProviderTestSupport.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDecoratorInput.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeGraphBundleInternal.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Decorators/BTDecorator_Cooldown.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ScopeExit.h"

namespace AssetDocumentDecoratorInputTests
{
using namespace AssetDocumentAIBehaviorTreeProviderTestSupport;
using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
// Focused callback recorder; the separate INSTALL integration uses the real
// Atomic kernel. This fixture exercises the actual provider and native leases.
class FRecordedAtomic final : public IAssetDocumentAtomicTransaction
{
public:
 FAssetDocumentMutationToken Token{FGuid::NewGuid(), FGuid::NewGuid(), 1};
 TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
 int32 Rows = 0;
 FGuid GetTransactionId() const override { return Token.TransactionId; }
 EAssetDocumentAtomicState GetState() const override { return EAssetDocumentAtomicState::Live; }
 const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
 static FAssetDocumentContractValidationResult Ok() { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
 FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Value) override { Rows += Value.Num(); return Ok(); }
 FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction> Action) override { Actions.Add(Action); return Ok(); }
 FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override { return {}; }
 FAssetDocumentContractValidationResult MarkReadyToCommit() override { return {}; }
 FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return {}; }
 FAssetDocumentContractValidationResult Rollback(FStringView Reason) override
 {
  for (int32 Index = Actions.Num() - 1; Index >= 0; --Index) { auto R = Actions[Index]->Rollback(Reason); if (!R.bValid) return R; }
  return Ok();
 }
};
UEdGraphPin* Pin(UEdGraphNode& Node, EEdGraphPinDirection Direction, int32 Index = 0)
{
 for (UEdGraphPin* P : Node.Pins) if (P && P->Direction == Direction && Index-- == 0) return P;
 return nullptr;
}
struct FFixture
{
 TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> State = MakeShared<FAssetDocumentAIBehaviorTreeScratchBundleState>();
 FRecordedAtomic Transaction;
 FResolveExistingHandleResolver Resolver;
 FAssetDocumentPrimitiveInvocation Invocation;
 UBehaviorTreeGraphNode_CompositeDecorator* Wrapper = nullptr;
 UBehaviorTreeDecoratorGraphNode_Decorator* Leaf = nullptr;
 UBTDecorator_Cooldown* Instance = nullptr;
 FGuid LogicId;
 FAutomationTestBase* OwnerTest = nullptr;
 bool Create(FAutomationTestBase& Test, EDecoratorLogicMode::Type Mode)
 {
  OwnerTest = &Test;
  State->Tree.Reset(NewObject<UBehaviorTree>(GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional));
  State->Graph.Reset(Cast<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(State->Tree.Get(), TEXT("Graph"),
   UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass())));
  if (!Test.TestNotNull(TEXT("stock scratch main graph created"), State->Graph.Get())) return false;
  State->Graph->SetFlags(RF_Transient);
  FGraphNodeCreator<UBehaviorTreeGraphNode_CompositeDecorator> WrapperCreator(*State->Graph);
  Wrapper = WrapperCreator.CreateNode(false); WrapperCreator.Finalize();
  auto* Bound = Cast<UBehaviorTreeDecoratorGraph>(Wrapper->BoundGraph);
  if (!Test.TestNotNull(TEXT("stock wrapper creates its bound graph"), Bound)) return false;
  FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Logic> LogicCreator(*Bound);
  auto* Logic = LogicCreator.CreateNode(false); Logic->LogicMode = Mode; LogicCreator.Finalize();
  LogicId = Logic->NodeGuid; Logic->NodePosX = 87; Logic->NodePosY = -139;
  Logic->NodeComment = TEXT("retain target layout and bubble"); Logic->bCommentBubblePinned = true;
  FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Decorator> LeafCreator(*Bound);
  Leaf = LeafCreator.CreateNode(false); LeafCreator.Finalize();
  Instance = NewObject<UBTDecorator_Cooldown>(Leaf); Instance->CoolDownTime = FValueOrBBKey_Float(6.75f); Leaf->NodeInstance = Instance;
  auto* Output = Pin(*Leaf, EGPD_Output); auto* Input = Pin(*Logic, EGPD_Input);
  if (!Test.TestTrue(TEXT("stock schema connects the original first input"), Output && Input
   && Bound->GetSchema()->TryCreateConnection(Output, Input))) return false;
  FGraphNodeCreator<UEdGraphNode_Comment> CommentCreator(*Bound);
  auto* Comment = CommentCreator.CreateNode(false); CommentCreator.Finalize();
  Comment->NodeComment = TEXT("disconnected user comment"); Comment->NodeWidth = 421; Comment->NodeHeight = 239;
  Comment->NodePosX = -91; Comment->NodePosY = 301; Comment->FontSize = 27;
  TArray<FAssetDocumentRuntimeDiagnostic> CleanupDiagnostics;
  if (!Test.TestTrue(TEXT("existing scratch cleanup is enlisted"),
   EnlistAssetDocumentAIBehaviorTreeScratchBundle(Transaction, State, Transaction.Token.RequestId, CleanupDiagnostics))) return false;
  Resolver.SetMutationToken(Transaction.Token); Resolver.SetIssuingProviderId(TEXT("ai"));
  FAssetDocumentPrimitiveOutcome Issued;
  if (!Test.TestTrue(TEXT("scratch payload accepts exact transient tree and graph flags"),
   IssueAssetDocumentAIBehaviorTreeGraphHandle(Resolver, State, TEXT("/Temp/DecoratorInput.Target"), Issued))) return false;
  Invocation.PrimitiveId = TEXT("ai.behavior_tree.decorator.ensure_input_count.v1"); Invocation.Version = 1;
  Invocation.RequestId = Transaction.Token.RequestId; Invocation.TransactionId = Transaction.Token.TransactionId;
  Invocation.RuntimeRevision = Transaction.Token.RuntimeRevision; Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  if (!Test.TestTrue(TEXT("real scratch payload issued"), Issued.HandleResults.Num() == 1)) return false;
  Invocation.HandleArguments = {{TEXT("graph"), Issued.HandleResults[0].Handles[0]}};
  Invocation.ValueArguments = ObjectValue({TEXT("bound_graph_guid"), TEXT("composite_stable_node_id"), TEXT("exact_input_count"), TEXT("logic_kind"), TEXT("logic_stable_node_id")},
   {StringValue(Bound->GraphGuid.ToString(EGuidFormats::Digits)), StringValue(Wrapper->NodeGuid.ToString(EGuidFormats::Digits)), IntegerValue(2),
    StringValue(Mode == EDecoratorLogicMode::And ? TEXT("And") : TEXT("Or")), StringValue(LogicId.ToString(EGuidFormats::Digits))});
  return true;
 }
 FAssetDocumentPrimitiveOutcome Invoke(int32 Count)
 {
  Invocation.ValueArguments.Elements[2].IntegerValue = Count;
  return CreateAssetDocumentAIPrimitiveProvider()->Invoke(Invocation, Resolver, &Transaction);
 }
 bool Snapshot(TArray<uint8>& Out)
 {
  FAssetDocumentRuntimeValueDto Value; bool Overflow = false;
  return OwnerTest->TestTrue(TEXT("complete bound V2 snapshot encodes"),
   BuildAuthoredBoundGraphProjection(*Wrapper->BoundGraph, Value, Overflow, EAuthoredGraphReadbackVersion::V2)
   && !Overflow && EncodeAuthoredCanonicalValue(Value, Out));
 }
 UBehaviorTreeDecoratorGraphNode_Logic* Logic() const
 {
  for (UEdGraphNode* Node : Wrapper->BoundGraph->Nodes) if (Node->NodeGuid == LogicId) return Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Node);
  return nullptr;
 }
};
bool Succeeded(FAutomationTestBase& Test, const FAssetDocumentPrimitiveOutcome& O)
{
 for (const auto& D : O.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 return Test.TestEqual(TEXT("actual provider cardinality operation succeeds"), O.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDecoratorInputLifecycle,
 "UEAssetDoc.AIProvider.BehaviorTree.DecoratorInput.ScopedContinuousAndCleanup",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDecoratorInputLifecycle::RunTest(const FString&)
{
 using namespace AssetDocumentDecoratorInputTests;
 for (const auto Mode : {EDecoratorLogicMode::And, EDecoratorLogicMode::Or})
 {
  FFixture F; if (!F.Create(*this, Mode)) return false;
  TArray<uint8> Before; if (!F.Snapshot(Before)) return false;
  auto* Main = F.State->Graph.Get(); auto* Wrapper = F.Wrapper;
  if (!Succeeded(*this, F.Invoke(5)) || !Succeeded(*this, F.Invoke(3))) return false;
  TestTrue(TEXT("main graph and wrapper capabilities preserve their identity"), Main == F.State->Graph.Get() && Wrapper == F.Wrapper);
  TestTrue(TEXT("arbitrary native instance and ownership subtree retained"), F.Leaf->NodeInstance == F.Instance
   && F.Instance->GetOuter() == F.Leaf && F.Leaf->GetOuter() == F.Wrapper->BoundGraph);
  TestEqual(TEXT("runtime property preserved"), F.Instance->CoolDownTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr)), 6.75f);
  const int32 Actions = F.Transaction.Actions.Num();
  const auto NoOp = F.Invoke(3); if (!Succeeded(*this, NoOp)) return false;
  TestTrue(TEXT("exact no-op does not enlist or report a change"), F.Transaction.Actions.Num() == Actions && NoOp.ChangedSet.IsEmpty());
  TestEqual(TEXT("only existing scratch bundle owns terminal cleanup"), F.Transaction.Actions.Num(), 1);
  TestTrue(TEXT("existing scratch cleanup succeeds"), F.Transaction.Rollback(TEXTVIEW("test_abort")).bValid);
  TestTrue(TEXT("cleanup discards rebuilt scratch scope"), F.State->bConsumed && !F.State->Tree.IsValid() && !F.State->Graph.IsValid());
  const auto Stale = F.Invoke(5);
  TestTrue(TEXT("consumed scratch capability is rejected"), Stale.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDecoratorInputFailures,
 "UEAssetDoc.AIProvider.BehaviorTree.DecoratorInput.ConnectedTailAndLocalFailures",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDecoratorInputFailures::RunTest(const FString&)
{
 using namespace AssetDocumentDecoratorInputTests;
 using namespace UE::AssetDocumentAI::BehaviorTreeDecoratorInput;
 ON_SCOPE_EXIT { SetFailurePointForTest(EFailurePoint::None); };
 for (const auto Point : {EFailurePoint::AfterNodeMigration, EFailurePoint::AfterPublication})
 {
  FFixture F; if (!F.Create(*this, EDecoratorLogicMode::And)) return false;
  TArray<uint8> Before; if (!F.Snapshot(Before)) return false;
  SetFailurePointForTest(Point);
  const auto Outcome = F.Invoke(5); SetFailurePointForTest(EFailurePoint::None);
  TestTrue(TEXT("injected failure returns closed build diagnostic"), Outcome.Diagnostics.ContainsByPredicate(
   [](const auto& D) { return D.Code == TEXT("AIProvider.BehaviorTree.DecoratorInputBuildFailed"); }));
  TestTrue(TEXT("failure returns no partial result"), Outcome.Value.Elements.IsEmpty() && Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty());
  TArray<uint8> After; if (!F.Snapshot(After)) return false;
  TestEqual(TEXT("local failure restores full V2 before returning"), After, Before);
  TestTrue(TEXT("later existing scratch cleanup succeeds"), F.Transaction.Rollback(TEXTVIEW("test_abort")).bValid);
 }
 FFixture F; if (!F.Create(*this, EDecoratorLogicMode::Or) || !Succeeded(*this, F.Invoke(5))) return false;
 auto* Graph = F.Wrapper->BoundGraph.Get(); Graph->GetSchema()->BreakNodeLinks(*F.Leaf);
 if (!TestTrue(TEXT("stock schema connects the occupied trailing slot"),
  Graph->GetSchema()->TryCreateConnection(Pin(*F.Leaf, EGPD_Output), Pin(*F.Logic(), EGPD_Input, 4)))) return false;
 TArray<uint8> Before; if (!F.Snapshot(Before)) return false;
 const int32 Actions = F.Transaction.Actions.Num();
 const auto Rejected = F.Invoke(3);
 TestTrue(TEXT("occupied trailing input rejects before enlistment"), Rejected.Diagnostics.ContainsByPredicate(
  [](const auto& D) { return D.Code == TEXT("AIProvider.BehaviorTree.DecoratorInputRejected"); }) && Actions == F.Transaction.Actions.Num());
 TArray<uint8> After; if (!F.Snapshot(After)) return false;
 TestEqual(TEXT("rejection retains every existing slot and link"), After, Before);
 return true;
}
#endif
