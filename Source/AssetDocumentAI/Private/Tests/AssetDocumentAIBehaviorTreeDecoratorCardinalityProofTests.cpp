// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/Decorators/BTDecorator_Cooldown.h"
#include "BehaviorTree/Decorators/BTDecorator_ForceSuccess.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "Utils/AssetDocumentEngineCompat.h"

namespace AssetDocumentDecoratorCardinalityProof
{
UEdGraphPin* Pin(UEdGraphNode& Node, EEdGraphPinDirection Direction, int32 At = 0)
{
 for (UEdGraphPin* Value : Node.Pins)
  if (Value && Value->Direction == Direction && At-- == 0) return Value;
 return nullptr;
}
int32 Inputs(const UEdGraphNode& Node)
{
 int32 Count = 0;
 for (const UEdGraphPin* Value : Node.Pins) if (Value && Value->Direction == EGPD_Input) ++Count;
 return Count;
}
bool Connect(UEdGraphNode& From, UEdGraphNode& To, int32 At)
{
 auto* Output = Pin(From, EGPD_Output); auto* Input = Pin(To, EGPD_Input, At);
 return Output && Input && To.GetGraph()->GetSchema()->TryCreateConnection(Output, Input);
}
bool Prefix(const TArray<UBTDecorator*>& Instances, const TArray<FBTDecoratorLogic>& Ops)
{
 if (Ops.IsEmpty() || Ops.Num() > 256 || Instances.Num() > 16) return false;
 int32 At = 0;
 TFunction<bool(int32)> Read = [&](int32 Depth)
 {
  if (Depth > 32 || !Ops.IsValidIndex(At)) return false;
  const auto Op = Ops[At++];
  if (Op.Operation == EBTDecoratorLogic::Test)
   return Instances.IsValidIndex(Op.Number) && IsValid(Instances[Op.Number]);
  if (Op.Operation != EBTDecoratorLogic::And && Op.Operation != EBTDecoratorLogic::Or
   && Op.Operation != EBTDecoratorLogic::Not) return false;
  if (Op.Number > 16 || (Op.Operation == EBTDecoratorLogic::Not ? Op.Number != 1 : Op.Number < 2)) return false;
  for (int32 Child = 0; Child < Op.Number; ++Child) if (!Read(Depth + 1)) return false;
  return true;
 };
 return Read(0) && At == Ops.Num();
}
struct FGraph
{
 TStrongObjectPtr<UBehaviorTreeGraphNode_CompositeDecorator> Wrapper;
 UBehaviorTreeDecoratorGraph* Graph = nullptr;
 FGraph() : Wrapper(NewObject<UBehaviorTreeGraphNode_CompositeDecorator>())
 {
  Graph = NewObject<UBehaviorTreeDecoratorGraph>(Wrapper.Get());
  Wrapper->BoundGraph = Graph;
  Graph->GraphGuid = FGuid::NewGuid();
  Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
 }
 UBehaviorTreeDecoratorGraphNode_Logic* Sink() const
 {
  for (UEdGraphNode* Node : Graph->Nodes)
   if (auto* Logic = Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Node))
    if (Logic->LogicMode == EDecoratorLogicMode::Sink) return Logic;
  return nullptr;
 }
};

UBehaviorTreeDecoratorGraphNode_Logic* SeedLogic(FGraph& Target, EDecoratorLogicMode::Type Mode, int32 Count)
{
 if (Mode == EDecoratorLogicMode::Sink) return Count == 1 ? Target.Sink() : nullptr;
 if (Count > 16 || (Mode == EDecoratorLogicMode::Not ? Count != 1 : Count < 2)
  || (Mode != EDecoratorLogicMode::And && Mode != EDecoratorLogicMode::Or && Mode != EDecoratorLogicMode::Not)) return nullptr;
 FGraph Seed;
 TArray<UBTDecorator*> Instances{NewObject<UBTDecorator_ForceSuccess>(Seed.Wrapper.Get())};
 const uint8 Operation = Mode == EDecoratorLogicMode::And ? EBTDecoratorLogic::And
  : Mode == EDecoratorLogicMode::Or ? EBTDecoratorLogic::Or : EBTDecoratorLogic::Not;
 TArray<FBTDecoratorLogic> Ops{FBTDecoratorLogic(Operation, static_cast<uint16>(Count))};
 for (int32 Index = 0; Index < Count; ++Index) Ops.Emplace(EBTDecoratorLogic::Test, 0);
 if (!Prefix(Instances, Ops) || Seed.Wrapper->SpawnMissingNodes(Instances, Ops, 0) != Ops.Num()) return nullptr;
 UBehaviorTreeDecoratorGraphNode_Logic* Result = nullptr;
 TArray<UEdGraphNode*> SeedNodes;
 for (UEdGraphNode* Node : Seed.Graph->Nodes)
 {
  SeedNodes.Add(Node);
  if (auto* Logic = Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Node))
   if (Logic->LogicMode == Mode) Result = Logic;
 }
 if (!Result || Inputs(*Result) != Count) return nullptr;
 // All placeholders remain under the isolated seed owner after removal.
 // The candidate receives only the disconnected requested logic node.
 for (UEdGraphNode* Node : SeedNodes)
 {
  Seed.Graph->GetSchema()->BreakNodeLinks(*Node);
  if (!Seed.Graph->RemoveNode(Node)) return nullptr;
 }
 if (!Result->Rename(nullptr, Target.Graph, REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional)) return nullptr;
 Target.Graph->AddNode(Result, false, false);
 return Result;
}
UBehaviorTreeDecoratorGraphNode_Decorator* Leaf(FGraph& Target, float Cooldown)
{
 FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Decorator> Creator(*Target.Graph);
 auto* Node = Creator.CreateNode(false); Creator.Finalize();
 auto* Instance = NewObject<UBTDecorator_Cooldown>(Node);
 Instance->CoolDownTime = FValueOrBBKey_Float(Cooldown);
 Node->NodeInstance = Instance;
 return Node;
}
struct FNode
{
 FGuid Guid;
 int32 X = 0, Y = 0, Count = 0;
 FString Comment;
 int32 Kind = 0; // logic, typed cooldown leaf, comment
 EDecoratorLogicMode::Type Mode = EDecoratorLogicMode::Sink;
 float Cooldown = 0;
};
struct FLink { FGuid From, To; int32 Input = 0; };
struct FSnapshot { FGuid GraphGuid; TArray<FNode> Nodes; TArray<FLink> Links; };
bool Capture(FGraph& Source, FSnapshot& Out)
{
 Out.GraphGuid = Source.Graph->GraphGuid;
 for (UEdGraphNode* Node : Source.Graph->Nodes)
 {
  FNode N; N.Guid = Node->NodeGuid; N.X = Node->NodePosX; N.Y = Node->NodePosY;
  N.Comment = Node->NodeComment; N.Count = Inputs(*Node);
  if (auto* Logic = Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Node)) N.Mode = Logic->LogicMode;
  else if (auto* Test = Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(Node))
  {
   const auto* Instance = Cast<UBTDecorator_Cooldown>(Test->NodeInstance);
   if (!Instance) return false;
   N.Kind = 1; N.Cooldown = Instance->CoolDownTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr));
  }
  else if (Cast<UEdGraphNode_Comment>(Node)) N.Kind = 2;
  else return false;
  Out.Nodes.Add(N);
  for (int32 Index = 0; Index < N.Count; ++Index)
  {
   auto* Input = Pin(*Node, EGPD_Input, Index);
   if (!Input || Input->LinkedTo.Num() > 1) return false;
   for (const auto* Link : Input->LinkedTo)
   {
    if (!Link || Link->Direction != EGPD_Output) return false;
    Out.Links.Add({Link->GetOwningNode()->NodeGuid, N.Guid, Index});
   }
  }
 }
 return true;
}
bool Rebuild(const FSnapshot& Before, FGuid Changed, int32 Count, TUniquePtr<FGraph>& Out)
{
 const FNode* Requested = Before.Nodes.FindByPredicate([&](const FNode& N) { return N.Guid == Changed; });
 if (!Requested || Requested->Kind != 0 || Count > 16
  || (Requested->Mode == EDecoratorLogicMode::Sink || Requested->Mode == EDecoratorLogicMode::Not ? Count != 1 : Count < 2)) return false;
 for (const auto& Link : Before.Links) if (Link.To == Changed && Link.Input >= Count) return false;
 auto Candidate = MakeUnique<FGraph>(); Candidate->Graph->GraphGuid = Before.GraphGuid;
 TMap<FGuid, UEdGraphNode*> Nodes;
 for (const auto& N : Before.Nodes)
 {
  UEdGraphNode* Node = nullptr;
  if (N.Kind == 0) Node = SeedLogic(*Candidate, N.Mode, N.Guid == Changed ? Count : N.Count);
  else if (N.Kind == 1) Node = Leaf(*Candidate, N.Cooldown);
  else
  {
   FGraphNodeCreator<UEdGraphNode_Comment> Creator(*Candidate->Graph);
   Node = Creator.CreateNode(false); Creator.Finalize();
  }
  if (!Node) return false;
  Node->NodeGuid = N.Guid; Node->NodePosX = N.X; Node->NodePosY = N.Y; Node->NodeComment = N.Comment;
  Nodes.Add(N.Guid, Node);
 }
 for (const auto& Link : Before.Links)
 {
  UEdGraphNode** From = Nodes.Find(Link.From); UEdGraphNode** To = Nodes.Find(Link.To);
  if (!From || !To || !Connect(**From, **To, Link.Input)) return false;
 }
 Out = MoveTemp(Candidate); return true;
}
bool Equivalent(FAutomationTestBase& Test, const FSnapshot& Before, FGraph& After, FGuid Changed, int32 Count)
{
 FSnapshot Actual;
 if (!Capture(After, Actual)) return false;
 bool Ok = Test.TestEqual(TEXT("graph GUID preserved"), Actual.GraphGuid, Before.GraphGuid)
  & Test.TestEqual(TEXT("all disconnected and connected nodes preserved"), Actual.Nodes.Num(), Before.Nodes.Num())
  & Test.TestEqual(TEXT("all ordered connections preserved"), Actual.Links.Num(), Before.Links.Num());
 for (const auto& N : Before.Nodes)
 {
  const FNode* A = Actual.Nodes.FindByPredicate([&](const FNode& Value) { return Value.Guid == N.Guid; });
  Ok &= Test.TestTrue(TEXT("stable node GUID, mode, layout, comment and property preserved"), A
   && A->Kind == N.Kind && A->Mode == N.Mode && A->X == N.X && A->Y == N.Y
   && A->Comment == N.Comment && A->Cooldown == N.Cooldown && A->Count == (N.Guid == Changed ? Count : N.Count));
 }
 for (const auto& L : Before.Links)
  Ok &= Test.TestTrue(TEXT("source and ordered target input unchanged"), Actual.Links.ContainsByPredicate(
   [&](const FLink& A) { return A.From == L.From && A.To == L.To && A.Input == L.Input; }));
 TArray<UObject*> Owned; GetObjectsWithOuter(After.Graph, Owned, AssetDocumentObjects::DirectOnly);
 for (UObject* Object : Owned)
  Ok &= Test.TestTrue(TEXT("no seed or removed placeholder remains in candidate closure"), After.Graph->Nodes.Contains(Cast<UEdGraphNode>(Object)));
 return Ok;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDecoratorCardinalityProof,
 "UEAssetDoc.AIProvider.BehaviorTree.DecoratorCardinalityProof.StockSeedRebuild",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDecoratorCardinalityProof::RunTest(const FString&)
{
 using namespace AssetDocumentDecoratorCardinalityProof;
 auto Current = MakeUnique<FGraph>();
 auto* And = SeedLogic(*Current, EDecoratorLogicMode::And, 2);
 auto* Or = SeedLogic(*Current, EDecoratorLogicMode::Or, 2);
 auto* Not = SeedLogic(*Current, EDecoratorLogicMode::Not, 1);
 auto* A = Leaf(*Current, 3.25f); auto* B = Leaf(*Current, 7.5f);
 auto* C = Leaf(*Current, 13.0f); Leaf(*Current, 29.0f); // disconnected user content
 FGraphNodeCreator<UEdGraphNode_Comment> Creator(*Current->Graph);
 auto* Comment = Creator.CreateNode(false); Creator.Finalize(); Comment->NodeComment = TEXT("unconnected comment preserved");
 if (!TestNotNull(TEXT("stock five-class seed construction"), And) || !Or || !Not
  || !Connect(*A, *And, 0) || !Connect(*B, *And, 1) || !Connect(*And, *Not, 0)
  || !Connect(*Not, *Current->Sink(), 0) || !Connect(*C, *Or, 0)) return false;
 int32 Position = 0;
 for (UEdGraphNode* Node : Current->Graph->Nodes) { Node->NodePosX = ++Position * 37; Node->NodePosY = Position * -53; }
 const FGuid AndGuid = And->NodeGuid, OrGuid = Or->NodeGuid;
 for (const FGuid Changed : {AndGuid, OrGuid})
  for (const int32 Count : {5, 3})
  {
   FSnapshot Before; if (!Capture(*Current, Before)) return false;
   TUniquePtr<FGraph> Candidate;
   if (!TestTrue(TEXT("stock seed supports 2 to 5 to 3 without dropping empty slots"), Rebuild(Before, Changed, Count, Candidate))) return false;
   if (!Equivalent(*this, Before, *Candidate, Changed, Count)) return false;
   Current = MoveTemp(Candidate);
  }
 FSnapshot Final; if (!Capture(*Current, Final)) return false;
 TUniquePtr<FGraph> Rejected;
 TestFalse(TEXT("Sink remains exactly one input"), Rebuild(Final, Current->Sink()->NodeGuid, 2, Rejected));
 const auto* NotState = Final.Nodes.FindByPredicate([](const FNode& N) { return N.Kind == 0 && N.Mode == EDecoratorLogicMode::Not; });
 TestTrue(TEXT("Not remains exactly one input"), NotState && !Rebuild(Final, NotState->Guid, 2, Rejected));
 UEdGraphNode* CurrentOr = nullptr; UEdGraphNode* Loose = nullptr;
 for (UEdGraphNode* Node : Current->Graph->Nodes)
 {
  if (Node->NodeGuid == OrGuid) CurrentOr = Node;
  if (auto* Test = Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(Node))
   if (Cast<UBTDecorator_Cooldown>(Test->NodeInstance)->CoolDownTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr)) == 29.0f) Loose = Node;
 }
 if (!CurrentOr || !Loose || !Connect(*Loose, *CurrentOr, 2)) return false;
 FSnapshot Connected; if (!Capture(*Current, Connected)) return false;
 TestFalse(TEXT("shrinking an occupied trailing slot is rejected"), Rebuild(Connected, OrGuid, 2, Rejected));
 Equivalent(*this, Connected, *Current, OrGuid, 3);
 TArray<UBTDecorator*> BeforeTests; TArray<FBTDecoratorLogic> BeforeOps;
 Current->Wrapper->CollectDecoratorData(BeforeTests, BeforeOps);
 TestEqual(TEXT("connected runtime expression retains two leaves"), BeforeTests.Num(), 2);
 TestTrue(TEXT("connected runtime expression retains Not then And order"), BeforeOps.Num() == 4
  && BeforeOps[0].Operation == EBTDecoratorLogic::Not && BeforeOps[1].Operation == EBTDecoratorLogic::And
  && BeforeOps[1].Number == 2 && BeforeOps[2].Operation == EBTDecoratorLogic::Test
  && BeforeOps[3].Operation == EBTDecoratorLogic::Test);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDecoratorCardinalityPrefixProof,
 "UEAssetDoc.AIProvider.BehaviorTree.DecoratorCardinalityProof.RejectUnsafeSeedPrefix",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDecoratorCardinalityPrefixProof::RunTest(const FString&)
{
 using namespace AssetDocumentDecoratorCardinalityProof;
 TStrongObjectPtr<UBTDecorator> LeafInstance(NewObject<UBTDecorator_ForceSuccess>());
 TArray<UBTDecorator*> Instances{LeafInstance.Get()};
 TestFalse(TEXT("empty prefix rejected before stock indexing"), Prefix(Instances, {}));
 TestFalse(TEXT("unknown opcode rejected"), Prefix(Instances, {FBTDecoratorLogic(EBTDecoratorLogic::Invalid, 0)}));
 TestFalse(TEXT("invalid Test index rejected"), Prefix(Instances, {FBTDecoratorLogic(EBTDecoratorLogic::Test, 1)}));
 TestFalse(TEXT("truncated children rejected"), Prefix(Instances, {FBTDecoratorLogic(EBTDecoratorLogic::And, 2), FBTDecoratorLogic(EBTDecoratorLogic::Test, 0)}));
 TestFalse(TEXT("invalid Not arity rejected"), Prefix(Instances, {FBTDecoratorLogic(EBTDecoratorLogic::Not, 2), FBTDecoratorLogic(EBTDecoratorLogic::Test, 0), FBTDecoratorLogic(EBTDecoratorLogic::Test, 0)}));
 TArray<FBTDecoratorLogic> Deep;
 for (int32 Index = 0; Index < 34; ++Index) Deep.Emplace(EBTDecoratorLogic::Not, 1);
 Deep.Emplace(EBTDecoratorLogic::Test, 0);
 TestFalse(TEXT("depth bound rejected"), Prefix(Instances, Deep));
 TArray<FBTDecoratorLogic> Large; Large.Init(FBTDecoratorLogic(EBTDecoratorLogic::Test, 0), 257);
 TestFalse(TEXT("array bound rejected"), Prefix(Instances, Large));
 return true;
}
#endif
