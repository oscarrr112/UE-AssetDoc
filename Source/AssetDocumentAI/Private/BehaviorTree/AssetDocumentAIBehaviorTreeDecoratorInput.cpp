// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDecoratorInput.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeGraphBundleInternal.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/Decorators/BTDecorator_ForceSuccess.h"
#include "BehaviorTreeGraph.h"
#include "AIGraphNode.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "EdGraphSchema_BehaviorTreeDecorator.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace UE::AssetDocumentAI::BehaviorTreeDecoratorInput
{
namespace AIBehaviorTreeDecoratorInputLocal
{
constexpr ERenameFlags RenameFlags = REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional;
#if WITH_DEV_AUTOMATION_TESTS
EFailurePoint FailurePoint = EFailurePoint::None;
bool Fault(EFailurePoint Point) { return FailurePoint == Point; }
#else
enum class EFailurePoint : uint8 { None, AfterNodeMigration, AfterPublication };
bool Fault(EFailurePoint) { return false; }
#endif
FAssetDocumentContractValidationResult Failure(const TCHAR* Code, const TCHAR* Message)
{
 FAssetDocumentContractValidationResult R; auto& D = R.Diagnostics.AddDefaulted_GetRef();
 D.Code = FString(TEXT("AIProvider.BehaviorTree.")) + Code; D.Path = TEXT("/DecoratorInput"); D.Message = Message; return R;
}
FAssetDocumentContractValidationResult Success() { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
FAssetDocumentPrimitiveOutcome FailOutcome(const FAssetDocumentContractValidationResult& R)
{
 return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.DecoratorInputBuildFailed"),
  TEXT("Decorator cardinality could not complete."), R.Diagnostics);
}
bool EncodeBound(UBehaviorTreeDecoratorGraph& Graph, TArray<uint8>& Bytes, FAssetDocumentRuntimeValueDto* Value = nullptr)
{
 using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
 FAssetDocumentRuntimeValueDto Projection; bool Overflow = false;
 if (!BuildAuthoredBoundGraphProjection(Graph, Projection, Overflow, EAuthoredGraphReadbackVersion::V2)
  || Overflow || !EncodeAuthoredCanonicalValue(Projection, Bytes)) return false;
 if (Value) *Value = MoveTemp(Projection);
 return true;
}
UEdGraphPin* Pin(UEdGraphNode& Node, EEdGraphPinDirection Direction, int32 At = 0)
{
 for (UEdGraphPin* P : Node.Pins) if (P && P->Direction == Direction && At-- == 0) return P;
 return nullptr;
}
int32 Inputs(const UEdGraphNode& Node)
{
 int32 Count = 0; for (const UEdGraphPin* P : Node.Pins) if (P && P->Direction == EGPD_Input) ++Count; return Count;
}
FString Kind(EDecoratorLogicMode::Type Mode)
{
 switch (Mode) { case EDecoratorLogicMode::And: return TEXT("And"); case EDecoratorLogicMode::Or: return TEXT("Or");
 case EDecoratorLogicMode::Not: return TEXT("Not"); case EDecoratorLogicMode::Sink: return TEXT("Sink"); default: return {}; }
}
bool Arity(EDecoratorLogicMode::Type Mode, int32 Count)
{
 return Count <= 256 && !Kind(Mode).IsEmpty()
  && ((Mode == EDecoratorLogicMode::Sink || Mode == EDecoratorLogicMode::Not) ? Count == 1 : Count >= 2);
}
bool Prefix(const TArray<UBTDecorator*>& Instances, const TArray<FBTDecoratorLogic>& Ops)
{
 if (Ops.IsEmpty() || Ops.Num() > 4096 || Instances.Num() > 256) return false;
 int32 At = 0;
 TFunction<bool(int32)> Read = [&](int32 Depth)
 {
  if (Depth > 128 || !Ops.IsValidIndex(At)) return false;
  const auto Op = Ops[At++];
  if (Op.Operation == EBTDecoratorLogic::Test) return Instances.IsValidIndex(Op.Number) && IsValid(Instances[Op.Number]);
  if (Op.Operation != EBTDecoratorLogic::And && Op.Operation != EBTDecoratorLogic::Or && Op.Operation != EBTDecoratorLogic::Not) return false;
  if (Op.Number > 256 || (Op.Operation == EBTDecoratorLogic::Not ? Op.Number != 1 : Op.Number < 2)) return false;
  for (int32 Child = 0; Child < Op.Number; ++Child) if (!Read(Depth + 1)) return false;
  return true;
 };
 return Read(0) && At == Ops.Num();
}
struct FTemporary
{
 TStrongObjectPtr<UBehaviorTreeGraph> Main;
 TStrongObjectPtr<UBehaviorTreeGraphNode_CompositeDecorator> Wrapper;
 bool Create()
 {
  const FName Name = MakeUniqueObjectName(GetTransientPackage(), UBehaviorTreeGraph::StaticClass(), TEXT("AssetDocumentDecoratorTemporary"));
  Main.Reset(Cast<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(GetTransientPackage(), Name,
   UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass())));
  if (!Main.IsValid()) return false;
  FGraphNodeCreator<UBehaviorTreeGraphNode_CompositeDecorator> Creator(*Main);
  Wrapper.Reset(Creator.CreateNode(false)); Creator.Finalize();
  return Wrapper.IsValid() && Cast<UBehaviorTreeDecoratorGraph>(Wrapper->BoundGraph);
 }
 UBehaviorTreeDecoratorGraph* Graph() const { return Cast<UBehaviorTreeDecoratorGraph>(Wrapper->BoundGraph); }
};
struct FLink { UEdGraphNode* From; UEdGraphNode* To; int32 Input; };
struct FNode { TStrongObjectPtr<UEdGraphNode> Object; FName Name; };

class FRebuild final
{
public:
 FRebuild(const FAssetDocumentAIBehaviorTreeGraphLease& Lease) : State(Lease.State), Main(Lease.Graph) {}
 FAssetDocumentContractValidationResult Reject() const
 { return Failure(TEXT("DecoratorInputRejected"), TEXT("The exact scoped graph, topology, kind or requested cardinality is not admissible.")); }
 FAssetDocumentContractValidationResult BuildFailure() const
 { return Failure(TEXT("DecoratorInputBuildFailed"), TEXT("The candidate graph could not preserve the captured authored content.")); }
 FAssetDocumentContractValidationResult Capture(FGuid Composite, FGuid Bound, FGuid LogicGuid, const FString& RequestedKind, int32 InCount)
 {
  Count = InCount;
  if (!State || State->bConsumed || !State->Tree.IsValid() || State->Graph.Get() != Main.Get()
   || State->Tree->BTGraph || Main->GetOuter() != State->Tree.Get() || Main->IsLocked()) return Reject();
  TSet<UEdGraphNode*> Seen; TArray<UBehaviorTreeGraphNode*> Pending;
  for (UEdGraphNode* Node : Main->Nodes) if (auto* BT = Cast<UBehaviorTreeGraphNode>(Node)) Pending.Add(BT);
  while (!Pending.IsEmpty())
  {
   auto* Node = Pending.Pop(EAllowShrinking::No);
   if (Seen.Contains(Node)) continue;
   if (Seen.Num() >= 4096 || Node->GetOuter() != Main.Get()) return Reject();
   Seen.Add(Node);
   if (Node->NodeGuid == Composite)
   {
    if (Wrapper.IsValid()) return Reject();
    Wrapper.Reset(Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Node));
    if (!Wrapper.IsValid()) return Reject();
   }
   for (UAIGraphNode* Child : Node->SubNodes) { auto* BT = Cast<UBehaviorTreeGraphNode>(Child); if (!BT) return Reject(); Pending.Add(BT); }
   for (UBehaviorTreeGraphNode* Child : Node->Decorators) { if (!Child) return Reject(); Pending.Add(Child); }
   for (UBehaviorTreeGraphNode* Child : Node->Services) { if (!Child) return Reject(); Pending.Add(Child); }
  }
  Old.Reset(Wrapper.IsValid() ? Cast<UBehaviorTreeDecoratorGraph>(Wrapper->BoundGraph) : nullptr);
  if (!Old.IsValid() || Old->GetClass() != UBehaviorTreeDecoratorGraph::StaticClass() || Old->GraphGuid != Bound
   || Old->GetOuter() != Wrapper.Get() || Old->Schema != UEdGraphSchema_BehaviorTreeDecorator::StaticClass()
   || !Old->SubGraphs.IsEmpty() || Old->Nodes.IsEmpty() || Old->Nodes.Num() > 4096) return Reject();
  OldName = Old->GetFName(); OldGuid = Old->GraphGuid; SubGraphs = Main->SubGraphs;
  int32 Matches = 0; for (UEdGraph* Graph : Main->SubGraphs) if (Graph == Old.Get()) ++Matches;
  if (Matches != 1) return Reject();
  TSet<FGuid> Guids; TSet<UObject*> Allowed; TSet<UBTDecorator*> Instances; int32 Sinks = 0;
  for (UEdGraphNode* Node : Old->Nodes)
  {
   if (!Node || Node->GetOuter() != Old.Get() || !Node->NodeGuid.IsValid() || Guids.Contains(Node->NodeGuid)) return Reject();
   Guids.Add(Node->NodeGuid); Allowed.Add(Node); Nodes.Add({TStrongObjectPtr<UEdGraphNode>(Node), Node->GetFName()});
   if (auto* Logic = Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Node))
   {
    if (Logic->GetClass() != UBehaviorTreeDecoratorGraphNode_Logic::StaticClass() || !Arity(Logic->LogicMode, Inputs(*Logic))) return Reject();
    if (Logic->LogicMode == EDecoratorLogicMode::Sink) ++Sinks;
    if (Logic->NodeGuid == LogicGuid) Target.Reset(Logic);
   }
   else if (auto* Test = Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(Node))
   {
    auto* Instance = Cast<UBTDecorator>(Test->NodeInstance);
    if (Test->GetClass() != UBehaviorTreeDecoratorGraphNode_Decorator::StaticClass() || !Instance
     || !Instance->IsIn(State->Tree.Get()) || Instances.Contains(Instance)) return Reject();
    Instances.Add(Instance); Allowed.Add(Instance);
    TArray<UObject*> Nested; GetObjectsWithOuter(Instance, Nested); for (UObject* Object : Nested) Allowed.Add(Object);
   }
   else if (Node->GetClass() != UEdGraphNode_Comment::StaticClass()) return Reject();
  }
  if (Sinks != 1 || !Target.IsValid() || Kind(Target->LogicMode) != RequestedKind || !Arity(Target->LogicMode, Count)) return Reject();
  TArray<UObject*> Nested; GetObjectsWithOuter(Old.Get(), Nested);
  for (UObject* Object : Nested) if (!Allowed.Contains(Object)) return Reject();
  for (const auto& N : Nodes)
  {
   auto* Node = N.Object.Get(); int32 InputIndex = 0;
   for (const UEdGraphPin* P : Node->Pins)
   {
    if (!P || P->GetOwningNode() != Node || P->LinkedTo.Num() > 1) return Reject();
    for (const UEdGraphPin* Other : P->LinkedTo)
     if (!Other || Other->Direction == P->Direction || !Guids.Contains(Other->GetOwningNode()->NodeGuid)
      || Other->GetOwningNode()->GetOuter() != Old.Get() || !Other->LinkedTo.Contains(P)) return Reject();
    if (P->Direction != EGPD_Input) continue;
    if (Node == Target.Get() && InputIndex >= Count && !P->LinkedTo.IsEmpty()) return Reject();
    for (const auto* Other : P->LinkedTo) Links.Add({Other->GetOwningNode(), Node, InputIndex});
    ++InputIndex;
   }
  }
  // Reject cycles even in disconnected user components before any stock traversal.
  TSet<UEdGraphNode*> Active, Done;
  TFunction<bool(UEdGraphNode*, int32)> Visit = [&](UEdGraphNode* Node, int32 Depth)
  {
   if (Active.Contains(Node) || Depth > 128) return false;
   if (Done.Contains(Node)) return true;
   Active.Add(Node);
   for (const auto& Link : Links) if (Link.To == Node && !Visit(Link.From, Depth + 1)) return false;
   Active.Remove(Node); Done.Add(Node); return true;
  };
  for (const auto& N : Nodes) if (!Visit(N.Object.Get(), 0)) return Reject();
  FAssetDocumentRuntimeValueDto Expected;
  if (!EncodeBound(*Old, BeforeBytes, &Expected)) return Reject();
  const int32 NodeIndex = Expected.ObjectKeys.IndexOfByKey(TEXT("nodes"));
  if (!Expected.Elements.IsValidIndex(NodeIndex)) return Reject();
  int32 ChangedRows = 0;
  for (auto& Node : Expected.Elements[NodeIndex].Elements)
  {
   const int32 Id = Node.ObjectKeys.IndexOfByKey(TEXT("stable_node_id"));
   const int32 Input = Node.ObjectKeys.IndexOfByKey(TEXT("input_count"));
   if (!Node.Elements.IsValidIndex(Id) || !Node.Elements.IsValidIndex(Input)) return Reject();
   if (Node.Elements[Id].StringValue == LogicGuid.ToString(EGuidFormats::Digits))
   { Node.Elements[Input].IntegerValue = Count; ++ChangedRows; }
  }
  if (ChangedRows != 1 || !UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EncodeAuthoredCanonicalValue(Expected, ExpectedBytes)) return Reject();
  bDirty = State->Tree->GetPackage()->IsDirty(); return Success();
 }
 bool Prepare()
 {
  if (Inputs(*Target) == Count) return true;
  FTemporary SeedOwner;
  if (!Temporary.Create() || !SeedOwner.Create()) return false;
  Candidate.Reset(Temporary.Graph()); auto* Seed = SeedOwner.Graph();
  // The candidate retains a real stock wrapper while schema callbacks run.
  // Its default sink is also seed-only; the original sink will be migrated.
  TArray<UEdGraphNode*> Defaults; for (UEdGraphNode* Node : Candidate->Nodes) Defaults.Add(Node);
  for (UEdGraphNode* Node : Defaults)
  {
   Candidate->GetSchema()->BreakNodeLinks(*Node); if (!Candidate->RemoveNode(Node)) return false;
   if (!Node->Rename(nullptr, Seed, RenameFlags)) return false;
  }
  Candidate->GraphGuid = OldGuid;
  Candidate->InterfaceGuid = Old->InterfaceGuid;
  Candidate->bEditable = Old->bEditable; Candidate->bAllowDeletion = Old->bAllowDeletion;
  Candidate->bAllowRenaming = Old->bAllowRenaming;
  // Stock SpawnMissingNodes only reads this existing instance; all placeholder
  // nodes stay with Seed. No placeholder is installed or attached to the tree.
  TArray<UBTDecorator*> Instances{UBTDecorator_ForceSuccess::StaticClass()->GetDefaultObject<UBTDecorator>()};
  const uint8 Op = Target->LogicMode == EDecoratorLogicMode::And ? EBTDecoratorLogic::And : EBTDecoratorLogic::Or;
  TArray<FBTDecoratorLogic> Operations{FBTDecoratorLogic(Op, static_cast<uint16>(Count))};
  for (int32 Index = 0; Index < Count; ++Index) Operations.Emplace(EBTDecoratorLogic::Test, 0);
  if (!Prefix(Instances, Operations) || Seed->SpawnMissingNodes(Instances, Operations, 0) != Operations.Num()) return false;
  TArray<UEdGraphNode*> SeedNodes; for (UEdGraphNode* Node : Seed->Nodes) SeedNodes.Add(Node);
  for (UEdGraphNode* Node : SeedNodes)
  {
   if (auto* Logic = Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Node))
    if (Logic->LogicMode == Target->LogicMode) Replacement.Reset(Logic);
   Seed->GetSchema()->BreakNodeLinks(*Node);
   if (!Seed->RemoveNode(Node)) return false;
  }
  if (!Replacement.IsValid() || Inputs(*Replacement) != Count
   || !Replacement->Rename(*Target->GetName(), Candidate.Get(), RenameFlags)) return false;
  Replacement->NodeGuid = Target->NodeGuid; Replacement->NodePosX = Target->NodePosX; Replacement->NodePosY = Target->NodePosY;
  Replacement->NodeWidth = Target->NodeWidth; Replacement->NodeHeight = Target->NodeHeight;
  Replacement->NodeComment = Target->NodeComment;
  Replacement->bCommentBubblePinned = Target->bCommentBubblePinned; Replacement->bCommentBubbleVisible = Target->bCommentBubbleVisible;
  Replacement->bCommentBubbleMakeVisible = Target->bCommentBubbleMakeVisible;
  Replacement->bCanResizeNode = Target->bCanResizeNode; Replacement->bCanRenameNode = Target->bCanRenameNode;
  Replacement->bAllowModifingInputs = Target->bAllowModifingInputs;
  return true;
 }
 UEdGraphNode* Mapped(UEdGraphNode* Node, bool Forward) const { return Forward && Node == Target.Get() ? Replacement.Get() : Node; }
 bool Reconnect(UBehaviorTreeDecoratorGraph& Graph, bool Forward)
 {
  for (const auto& Link : Links)
  {
   auto* From = Mapped(Link.From, Forward); auto* To = Mapped(Link.To, Forward);
   auto* Output = Pin(*From, EGPD_Output); auto* Input = Pin(*To, EGPD_Input, Link.Input);
   if (!Output || !Input || !Graph.GetSchema()->TryCreateConnection(Output, Input)) return false;
  }
  return true;
 }
 bool Transfer()
 {
  bStarted = true;
  for (const auto& N : Nodes) Old->GetSchema()->BreakNodeLinks(*N.Object);
  for (const auto& N : Nodes)
  {
   auto* Original = N.Object.Get();
   if (!Old->RemoveNode(Original)) return false;
   auto* Node = Mapped(Original, true);
   if (Node != Replacement.Get() && !Node->Rename(*N.Name.ToString(), Candidate.Get(), RenameFlags)) return false;
   Candidate->AddNode(Node, false, false);
  }
  if (Fault(EFailurePoint::AfterNodeMigration) || !Reconnect(*Candidate, true)) return false;
  const FName Isolated = MakeUniqueObjectName(Temporary.Wrapper.Get(), Old->GetClass(), TEXT("AssetDocumentOldDecoratorGraph"));
  if (!Old->Rename(*Isolated.ToString(), Temporary.Wrapper.Get(), RenameFlags)
   || !Candidate->Rename(*OldName.ToString(), Wrapper.Get(), RenameFlags)) return false;
  Temporary.Wrapper->BoundGraph = Old.Get();
  Wrapper->BoundGraph = Candidate.Get();
  for (auto& Graph : Main->SubGraphs) if (Graph == Old.Get()) Graph = Candidate.Get();
  bPublished = true;
  Wrapper->OnInnerGraphChanged();
  State->Tree->GetPackage()->SetDirtyFlag(bDirty);
  return !Fault(EFailurePoint::AfterPublication) && Validate(true);
 }
 bool Validate(bool Forward) const
 {
  auto* Graph = Forward ? Candidate.Get() : Old.Get();
  if (Graph->GraphGuid != OldGuid || Graph->GetOuter() != Wrapper.Get() || Graph->GetFName() != OldName
   || Wrapper->BoundGraph != Graph || Graph->Nodes.Num() != Nodes.Num()) return false;
  for (int32 Index = 0; Index < Nodes.Num(); ++Index)
  {
   auto* Node = Mapped(Nodes[Index].Object.Get(), Forward);
   if (Graph->Nodes[Index] != Node || Node->GetOuter() != Graph) return false;
   for (int32 Input = 0; Input < Inputs(*Node); ++Input)
   {
    const auto* P = Pin(*Node, EGPD_Input, Input); UEdGraphNode* Expected = nullptr;
    for (const auto& Link : Links) if (Mapped(Link.To, Forward) == Node && Link.Input == Input) Expected = Mapped(Link.From, Forward);
    if (!P || P->LinkedTo.Num() != (Expected ? 1 : 0)
     || (Expected && P->LinkedTo[0]->GetOwningNode() != Expected)) return false;
   }
  }
  if (Main->SubGraphs.Num() != SubGraphs.Num()) return false;
  for (int32 Index = 0; Index < SubGraphs.Num(); ++Index)
   if (Main->SubGraphs[Index] != (Forward && SubGraphs[Index] == Old.Get() ? Candidate.Get() : SubGraphs[Index].Get())) return false;
  TArray<uint8> Actual;
  return EncodeBound(*Graph, Actual) && Actual == (Forward ? ExpectedBytes : BeforeBytes);
 }
 FAssetDocumentContractValidationResult Rollback()
 {
  if (!bStarted || bRestored) return Success();
  auto Bad = [&]() { return Failure(TEXT("DecoratorInputRollbackFailed"), TEXT("The captured scratch topology could not be restored.")); };
  if (!State->Tree.IsValid() || !Main.IsValid() || !Old.IsValid() || !Candidate.IsValid() || !Wrapper.IsValid()) return Bad();
  TArray<UEdGraphNode*> Current; for (UEdGraphNode* Node : Candidate->Nodes) Current.Add(Node);
  for (UEdGraphNode* Node : Current) { Candidate->GetSchema()->BreakNodeLinks(*Node); if (!Candidate->RemoveNode(Node)) return Bad(); }
  for (const auto& N : Nodes)
  {
   auto* Node = N.Object.Get(); Old->GetSchema()->BreakNodeLinks(*Node);
   if (Old->Nodes.Contains(Node) && !Old->RemoveNode(Node)) return Bad();
   if (Node->GetOuter() != Old.Get() && !Node->Rename(*N.Name.ToString(), Old.Get(), RenameFlags)) return Bad();
   Old->AddNode(Node, false, false);
  }
  if (Candidate->GetOuter() == Wrapper.Get())
  {
   const FName Name = MakeUniqueObjectName(Temporary.Wrapper.Get(), Candidate->GetClass(), TEXT("AssetDocumentDiscardedDecoratorGraph"));
   if (!Candidate->Rename(*Name.ToString(), Temporary.Wrapper.Get(), RenameFlags)) return Bad();
  }
  if ((Old->GetOuter() != Wrapper.Get() || Old->GetFName() != OldName)
   && !Old->Rename(*OldName.ToString(), Wrapper.Get(), RenameFlags)) return Bad();
  Wrapper->BoundGraph = Old.Get(); Main->SubGraphs = SubGraphs;
  Temporary.Wrapper->BoundGraph = Candidate.Get();
  if (!Reconnect(*Old, false) || !Validate(false)) return Bad();
  Wrapper->OnInnerGraphChanged();
  State->Tree->GetPackage()->SetDirtyFlag(bDirty); bRestored = true; return Success();
 }
 TSharedPtr<FAssetDocumentAIBehaviorTreeScratchBundleState> State;
 TStrongObjectPtr<UBehaviorTreeGraph> Main;
 TStrongObjectPtr<UBehaviorTreeGraphNode_CompositeDecorator> Wrapper;
 TStrongObjectPtr<UBehaviorTreeDecoratorGraph> Old, Candidate;
 TStrongObjectPtr<UBehaviorTreeDecoratorGraphNode_Logic> Target, Replacement;
 FTemporary Temporary;
 TArray<FNode> Nodes; TArray<FLink> Links; TArray<TObjectPtr<UEdGraph>> SubGraphs;
 TArray<uint8> BeforeBytes, ExpectedBytes;
 FName OldName; FGuid OldGuid; int32 Count = 0;
 bool bDirty = false, bStarted = false, bPublished = false, bRestored = false;
};
}
FAssetDocumentPrimitiveOutcome EnsureInputCount(const FAssetDocumentAIBehaviorTreeGraphLease& Lease,
 const FGuid& Composite, const FGuid& BoundGraph, const FGuid& Logic,
 const FString& RequestedKind, int32 Count, IAssetDocumentAtomicTransaction&)
{
 using namespace AIBehaviorTreeDecoratorInputLocal;
 const auto Action = MakeShared<FRebuild>(Lease);
 auto R = Action->Capture(Composite, BoundGraph, Logic, RequestedKind, Count); if (!R.bValid) return FailOutcome(R);
 const bool Changed = Inputs(*Action->Target) != Count;
 if (Changed)
 {
  if (!Action->Prepare()) return FailOutcome(Action->BuildFailure());
  // The pre-existing scratch bundle owns terminal cleanup. This operation
  // adds no kernel row: its local inverse restores any failed unpublished edit.
  if (!Action->Transfer())
  {
   const auto Restored = Action->Rollback();
   return FailOutcome(Restored.bValid ? Action->BuildFailure() : Restored);
  }
 }
 FAssetDocumentPrimitiveOutcome Out; Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 Out.Value = MakeAssetDocumentAIBehaviorTreeObjectValue({TEXT("exact_input_count"), TEXT("logic_stable_node_id")},
  {MakeAssetDocumentAIBehaviorTreeIntegerValue(Count), MakeAssetDocumentAIBehaviorTreeStringValue(Logic.ToString(EGuidFormats::Digits))});
 if (Changed) Out.ChangedSet.Add(MakeAssetDocumentAIBehaviorTreeScratchChange(Lease.LiveBehaviorTreePath,
  *Lease.Graph, Action->Wrapper.Get(), TEXT("Rebuild")));
 return Out;
}
#if WITH_DEV_AUTOMATION_TESTS
void SetFailurePointForTest(EFailurePoint Point) { using namespace AIBehaviorTreeDecoratorInputLocal; FailurePoint = Point; }
#endif
}
