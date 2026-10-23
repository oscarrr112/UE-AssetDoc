// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeScratchAuthoring.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDecoratorInput.h"
#include "AIGraphNode.h"
#include "AIGraphSchema.h"
#include "AIGraphTypes.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTreeEditorModule.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_SimpleParallel.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "EdGraphSchema_BehaviorTreeDecorator.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "ScopedTransaction.h"
#include "UObject/GarbageCollection.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AssetDocumentAI::BehaviorTreeScratchAuthoring
{
namespace
{
using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;
namespace Readback = BehaviorTreeAuthoredReadback;
const FValue* Field(const FValue& Object, const TCHAR* Name) { return FindAssetDocumentAIBehaviorTreeField(Object, Name); }
bool String(const FValue* Value, FString& Out)
{ if (!Value || Value->Kind != EKind::String) return false; Out = Value->StringValue; return true; }
bool Integer(const FValue* Value, int32& Out)
{
 if (!Value || Value->Kind != EKind::Integer || Value->IntegerValue < MIN_int32 || Value->IntegerValue > MAX_int32) return false;
 Out = static_cast<int32>(Value->IntegerValue); return true;
}
bool Guid(const FValue* Value, FGuid& Out)
{
 return Value && Value->Kind == EKind::String && FGuid::ParseExact(Value->StringValue, EGuidFormats::Digits, Out)
  && Out.IsValid() && Out.ToString(EGuidFormats::Digits) == Value->StringValue;
}
bool Array(const FValue* Value) { return Value && Value->Kind == EKind::Array; }

struct FBuilder
{
 const FAssetDocumentAIBehaviorTreeGraphLease& Lease;
 IAssetDocumentAtomicTransaction& Transaction;
 FString Error;
 TSet<FGuid> Ids;
 int32 Budget = 4096;

 bool Reject(const TCHAR* Message) { Error = Message; return false; }
 bool SetGuid(UObject& Object, FName Name, const FValue* Value)
 {
  FGuid Id;
  const bool bNode = Cast<UEdGraphNode>(&Object) != nullptr;
  if (!Guid(Value, Id) || (bNode && Ids.Contains(Id))) return Reject(TEXT("Invalid or duplicate scratch authored GUID"));
  // Use FGuid's native 32-digit import form. The generic "(A=..,B=..)" struct text path
  // parses each uint32 member with a platform `long`, which clamps values above INT32_MAX
  // to 0x7FFFFFFF on Windows (LLP64) and corrupted any GUID with a high-bit B or C word.
  const FString Formatted = Id.ToString(EGuidFormats::Digits);
  if (!WriteExactHiddenField(Object, Name, Formatted, Error)) return false;
  const auto* Graph = Cast<UEdGraph>(&Object);
  const auto* Node = Cast<UEdGraphNode>(&Object);
  if ((Graph && Graph->GraphGuid != Id) || (Node && Node->NodeGuid != Id)) return Reject(TEXT("Scratch GUID readback mismatch"));
  if (bNode) Ids.Add(Id); return true;
 }
 bool Editor(UEdGraphNode& Node, const FValue* Value)
 {
  if (!Value || !HasAssetDocumentAIBehaviorTreeExactFields(*Value,
   {TEXT("comment_bubble_pinned"), TEXT("comment_bubble_visible"), TEXT("node_comment"), TEXT("position")})) return Reject(TEXT("Invalid complete editor DTO"));
  const auto* Position = Field(*Value, TEXT("position"));
  int32 X = 0, Y = 0;
  FString Comment;
  const auto* Pinned = Field(*Value, TEXT("comment_bubble_pinned"));
  const auto* Visible = Field(*Value, TEXT("comment_bubble_visible"));
  if (!Position || !HasAssetDocumentAIBehaviorTreeExactFields(*Position, {TEXT("x"), TEXT("y")})
   || !Integer(Field(*Position, TEXT("x")), X) || !Integer(Field(*Position, TEXT("y")), Y)
   || !String(Field(*Value, TEXT("node_comment")), Comment) || !Pinned || Pinned->Kind != EKind::Boolean
   || !Visible || Visible->Kind != EKind::Boolean) return Reject(TEXT("Invalid editor scalar"));
  // Exact hidden handles avoid float-position rounding and retain authored int32.
  if (!WriteExactHiddenField(Node, TEXT("NodePosX"), LexToString(X), Error)
   || !WriteExactHiddenField(Node, TEXT("NodePosY"), LexToString(Y), Error)
   || !WriteExactHiddenField(Node, TEXT("NodeComment"), Comment, Error)
   || !WriteExactHiddenField(Node, TEXT("bCommentBubblePinned"), Pinned->BooleanValue ? TEXT("True") : TEXT("False"), Error)
   || !WriteExactHiddenField(Node, TEXT("bCommentBubbleVisible"), Visible->BooleanValue ? TEXT("True") : TEXT("False"), Error)) return false;
  return Node.NodePosX == X && Node.NodePosY == Y && Node.NodeComment == Comment
   && Node.bCommentBubblePinned == Pinned->BooleanValue && Node.bCommentBubbleVisible == Visible->BooleanValue;
 }
 bool Initialize(UEdGraphNode& Node, UObject* Instance, const FValue& Value)
 {
  const auto* Properties = Field(Value, TEXT("authored_properties"));
  if (!Properties || Properties->Kind != EKind::Object || !SetGuid(Node, TEXT("NodeGuid"), Field(Value, TEXT("stable_node_id")))) return false;
  if (!Editor(Node, Field(Value, TEXT("editor")))) return false;
  if (Instance) return WriteProperties(*Instance, *Properties, Error);
  return Properties->ObjectKeys.IsEmpty() || Reject(TEXT("Non-instance node has authored properties"));
 }
 bool Connect(UEdGraph& Graph, UEdGraphNode& From, UEdGraphNode& To, int32 InputIndex = 0, FName OutputName = NAME_None)
 {
  UEdGraphPin* Output = nullptr;
  UEdGraphPin* Input = nullptr;
  int32 Index = 0;
  for (auto* Pin : From.Pins) if (Pin && Pin->Direction == EGPD_Output
   && (OutputName.IsNone() || Pin->PinName == OutputName))
  { if (Output) return Reject(TEXT("Ambiguous output pin")); Output = Pin; }
  for (auto* Pin : To.Pins) if (Pin && Pin->Direction == EGPD_Input)
  { if (Index++ == InputIndex) Input = Pin; }
  return Output && Input && From.GetGraph() == &Graph && To.GetGraph() == &Graph
   && Graph.GetSchema() && Graph.GetSchema()->TryCreateConnection(Output, Input);
 }
 bool Comments(UEdGraph& Graph, const FValue* Values)
 {
  if (!Array(Values)) return Reject(TEXT("comments must be an array"));
  const auto* Schema = Cast<UAIGraphSchema>(Lease.Graph->GetSchema());
  if (!Schema) return false;
  for (const auto& Value : Values->Elements)
  {
   if (--Budget < 0 || !HasAssetDocumentAIBehaviorTreeExactFields(Value,
    {TEXT("color"), TEXT("color_comment_bubble"), TEXT("comment_bubble_pinned"), TEXT("comment_bubble_visible"),
     TEXT("comment_bubble_visible_in_details"), TEXT("comment_depth"), TEXT("font_size"), TEXT("move_mode"),
     TEXT("node_details"), TEXT("position"), TEXT("size"), TEXT("stable_node_id"), TEXT("text")})) return Reject(TEXT("Invalid comment DTO"));
   const auto Action = Schema->GetCreateCommentAction();
   auto* Node = Action.IsValid() ? Cast<UEdGraphNode_Comment>(Action->PerformAction(&Graph, nullptr, FVector2f::ZeroVector, false)) : nullptr;
   if (!Node || Node->GetOuter() != &Graph || !Graph.Nodes.Contains(Node)
    || !SetGuid(*Node, TEXT("NodeGuid"), Field(Value, TEXT("stable_node_id")))) return Reject(TEXT("Stock comment action/identity failed"));
   FValue EditorDto = Readback::ObjectValue({TEXT("comment_bubble_pinned"), TEXT("comment_bubble_visible"), TEXT("node_comment"), TEXT("position")},
    {*Field(Value, TEXT("comment_bubble_pinned")), *Field(Value, TEXT("comment_bubble_visible")), *Field(Value, TEXT("text")), *Field(Value, TEXT("position"))});
   const auto* Size = Field(Value, TEXT("size"));
   int32 Width = 0, Height = 0, Font = 0, Depth = 0;
   if (!Size || !HasAssetDocumentAIBehaviorTreeExactFields(*Size, {TEXT("height"), TEXT("width")})
    || !Integer(Field(*Size, TEXT("width")), Width) || !Integer(Field(*Size, TEXT("height")), Height)
    || Width <= 0 || Height <= 0 || !Integer(Field(Value, TEXT("font_size")), Font) || Font <= 0
    || !Integer(Field(Value, TEXT("comment_depth")), Depth)) return Reject(TEXT("Invalid comment size/font/depth"));
   for (const auto& Pair : TArray<TPair<FName, int32>>{{TEXT("NodeWidth"), Width}, {TEXT("NodeHeight"), Height}, {TEXT("FontSize"), Font}, {TEXT("CommentDepth"), Depth}})
    if (!WriteExactHiddenField(*Node, Pair.Key, LexToString(Pair.Value), Error)) return false;
   FString Mode, Details;
   if (!String(Field(Value, TEXT("move_mode")), Mode) || (Mode != TEXT("GroupMovement") && Mode != TEXT("NoGroupMovement"))
    || !String(Field(Value, TEXT("node_details")), Details)
    || !WriteExactHiddenField(*Node, TEXT("MoveMode"), Mode, Error) || !WriteExactHiddenField(*Node, TEXT("NodeDetails"), Details, Error)) return false;
   // Stock Color Bubble is edit-conditioned on the details visibility flag.
   // Open that condition through its ordinary handle, then publish both final values.
   if (!WriteExactHiddenField(*Node, TEXT("bCommentBubbleVisible_InDetailsPanel"), TEXT("True"), Error)) return false;
   for (const auto& Pair : TArray<TPair<const TCHAR*, FName>>{{TEXT("color_comment_bubble"), TEXT("bColorCommentBubble")}, {TEXT("comment_bubble_visible_in_details"), TEXT("bCommentBubbleVisible_InDetailsPanel")}})
   {
    const auto* Boolean = Field(Value, Pair.Key);
    if (!Boolean || Boolean->Kind != EKind::Boolean
     || !WriteExactHiddenField(*Node, Pair.Value, Boolean->BooleanValue ? TEXT("True") : TEXT("False"), Error)) return false;
   }
   // The details flag's stock PostEditChangeProperty synchronizes pinned/visible.
   // Apply their independently authored values after that notification.
   if (!Editor(*Node, &EditorDto)) return false;
   const auto* Color = Field(Value, TEXT("color"));
   double R = 0, G = 0, B = 0, A = 0;
   if (!Color || !HasAssetDocumentAIBehaviorTreeExactFields(*Color, {TEXT("a"), TEXT("b"), TEXT("g"), TEXT("r")})
    || !ReadAssetDocumentAIBehaviorTreeFiniteCoordinate(Field(*Color, TEXT("r")), R)
    || !ReadAssetDocumentAIBehaviorTreeFiniteCoordinate(Field(*Color, TEXT("g")), G)
    || !ReadAssetDocumentAIBehaviorTreeFiniteCoordinate(Field(*Color, TEXT("b")), B)
    || !ReadAssetDocumentAIBehaviorTreeFiniteCoordinate(Field(*Color, TEXT("a")), A)) return Reject(TEXT("Invalid comment color"));
   if (!WriteExactHiddenField(*Node, TEXT("CommentColor"), FLinearColor(R, G, B, A).ToString(), Error)) return false;
  }
  return true;
 }
 UEdGraphNode* BoundAction(UEdGraph& Graph, const FString& Kind, const FString& ClassPath)
 {
  const auto* Schema = Cast<UEdGraphSchema_BehaviorTreeDecorator>(Graph.GetSchema());
  auto* Module = FModuleManager::GetModulePtr<FBehaviorTreeEditorModule>(TEXT("BehaviorTreeEditor"));
  if (!Schema || !Module || !Module->GetClassCache().IsValid()) return nullptr;
  FGCScopeGuard Guard;
  TArray<FGraphNodeClassData> Classes;
  Module->GetClassCache()->GatherClasses(UBTDecorator::StaticClass(), Classes);
  int32 ActionIndex = INDEX_NONE;
  if (Kind == TEXT("test"))
  {
   UClass* Class = FSoftClassPath(ClassPath).ResolveClass();
   for (int32 Index = 0; Class && Index < Classes.Num(); ++Index)
    if (Classes[Index].GetClass(true) == Class) { if (ActionIndex != INDEX_NONE) return nullptr; ActionIndex = Index; }
  }
  else if (Kind == TEXT("and")) ActionIndex = Classes.Num();
  else if (Kind == TEXT("or")) ActionIndex = Classes.Num() + 1;
  else if (Kind == TEXT("not")) ActionIndex = Classes.Num() + 2;
  if (ActionIndex == INDEX_NONE) return nullptr;
  FGraphContextMenuBuilder Builder(&Graph);
  Schema->GetGraphContextActions(Builder);
  if (Builder.GetNumActions() != Classes.Num() + 3) return nullptr;
  const auto Action = Builder.GetSchemaAction(ActionIndex);
  const int32 Before = Graph.Nodes.Num();
  auto* Node = Action.IsValid() ? Action->PerformAction(&Graph, nullptr, FVector2f::ZeroVector, false) : nullptr;
  return Node && Node->GetOuter() == &Graph && Graph.Nodes.Contains(Node) && Graph.Nodes.Num() == Before + 1 ? Node : nullptr;
 }
 bool Bound(UBehaviorTreeGraphNode_CompositeDecorator& Composite, const FValue* Value)
 {
  UEdGraph* Graph = Composite.BoundGraph;
  if (!Graph || !Value || !HasAssetDocumentAIBehaviorTreeExactFields(*Value,
   {TEXT("comments"), TEXT("graph_guid"), TEXT("links"), TEXT("nodes")})
   || !SetGuid(*Graph, TEXT("GraphGuid"), Field(*Value, TEXT("graph_guid")))) return Reject(TEXT("Invalid BoundGraph DTO"));
  const auto* Nodes = Field(*Value, TEXT("nodes"));
  const auto* Links = Field(*Value, TEXT("links"));
  if (!Array(Nodes) || !Array(Links) || Graph->Nodes.Num() != 1) return Reject(TEXT("BoundGraph stock default sink missing"));
  auto* Sink = Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Graph->Nodes[0]);
  if (!Sink || Sink->LogicMode != EDecoratorLogicMode::Sink) return false;
  TMap<FString, UEdGraphNode*> Created;
  bool bSawSink = false;
  for (const auto& NodeValue : Nodes->Elements)
  {
   if (--Budget < 0 || !HasAssetDocumentAIBehaviorTreeExactFields(NodeValue, {TEXT("authored_properties"), TEXT("editor"), TEXT("input_count"), TEXT("kind"), TEXT("node_class_path_or_null"), TEXT("stable_node_id")})) return Reject(TEXT("Invalid BoundGraph node DTO"));
   FString Kind, ClassPath;
   int32 Inputs = 0;
   if (!String(Field(NodeValue, TEXT("kind")), Kind) || !Integer(Field(NodeValue, TEXT("input_count")), Inputs)) return false;
   const auto* Class = Field(NodeValue, TEXT("node_class_path_or_null"));
   if ((Kind == TEXT("test") && !String(Class, ClassPath)) || (Kind != TEXT("test") && Class->Kind != EKind::Null)) return false;
   UEdGraphNode* Node = Kind == TEXT("sink") ? Sink : BoundAction(*Graph, Kind, ClassPath);
   if (!Node || (Kind == TEXT("sink") && bSawSink)) return Reject(TEXT("Stock decorator graph action failed"));
   bSawSink |= Kind == TEXT("sink");
   auto* Test = Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(Node);
   if ((Kind == TEXT("test")) != (Test != nullptr) || (Test && (!Test->NodeInstance || Test->NodeInstance->GetClass()->GetPathName() != ClassPath))) return false;
   if (!Initialize(*Node, Test ? Test->NodeInstance.Get() : nullptr, NodeValue)) return false;
   if (Kind == TEXT("and") || Kind == TEXT("or"))
   {
    const FGuid Id = Node->NodeGuid;
    const auto Result = BehaviorTreeDecoratorInput::EnsureInputCount(Lease, Composite.NodeGuid, Graph->GraphGuid, Node->NodeGuid, Kind, Inputs, Transaction);
    if (Result.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) return Reject(TEXT("Admitted decorator input completion failed"));
    // A2 may replace the exact logic wrapper; reacquire by persistent identity.
    Graph = Composite.BoundGraph;
    if (!Graph) return false;
    Node = nullptr;
    for (UEdGraphNode* Candidate : Graph->Nodes) if (Candidate && Candidate->NodeGuid == Id) Node = Candidate;
   }
   else if (Inputs != (Kind == TEXT("test") ? 0 : 1)) return Reject(TEXT("Invalid fixed decorator input count"));
   if (!Node) return false;
   Created.Add(Field(NodeValue, TEXT("stable_node_id"))->StringValue, Node);
  }
  if (!bSawSink) return Reject(TEXT("Exactly one sink is required"));
  // The stock decorator schema permits a pair of opposite-direction pins;
  // reject cycles before triggering any native description/graph traversal.
  TMap<FString, TArray<FString>> Edges;
  for (const auto& Link : Links->Elements)
  {
   FString From, To;
   if (!String(Field(Link, TEXT("from_stable_node_id")), From) || !String(Field(Link, TEXT("to_stable_node_id")), To)
    || !Created.Contains(From) || !Created.Contains(To)) return Reject(TEXT("Invalid BoundGraph endpoint"));
   Edges.FindOrAdd(From).Add(To);
  }
  TSet<FString> Active, Complete;
  TFunction<bool(const FString&, int32)> Visit = [&](const FString& Id, int32 Depth)
  {
   if (Active.Contains(Id) || Depth > 64) return false;
   if (Complete.Contains(Id)) return true;
   Active.Add(Id);
   if (const auto* Next = Edges.Find(Id)) for (const auto& Child : *Next) if (!Visit(Child, Depth + 1)) return false;
   Active.Remove(Id); Complete.Add(Id); return true;
  };
  for (const auto& Entry : Created) if (!Visit(Entry.Key, 0)) return Reject(TEXT("Cyclic BoundGraph is not admissible"));
  for (const auto& Link : Links->Elements)
  {
   FString From, To; int32 Input = 0;
   if (!HasAssetDocumentAIBehaviorTreeExactFields(Link, {TEXT("from_stable_node_id"), TEXT("to_input_index"), TEXT("to_stable_node_id")})
    || !String(Field(Link, TEXT("from_stable_node_id")), From) || !String(Field(Link, TEXT("to_stable_node_id")), To)
    || !Integer(Field(Link, TEXT("to_input_index")), Input) || !Created.Contains(From) || !Created.Contains(To)
    || !Connect(*Graph, *Created[From], *Created[To], Input)) return Reject(TEXT("BoundGraph schema connection rejected"));
  }
  return Comments(*Graph, Field(*Value, TEXT("comments")));
 }
 bool Main(const FValue& Value, UEdGraphNode& Parent, int32 Depth, int32 ChildIndex = 0)
 {
  if (--Budget < 0 || Depth > 64 || !HasAssetDocumentAIBehaviorTreeExactFields(Value,
   {TEXT("authored_properties"), TEXT("children"), TEXT("decorators"), TEXT("editor"), TEXT("node_class_path"), TEXT("services"), TEXT("stable_node_id")})) return Reject(TEXT("Invalid main-node DTO"));
  FString ClassPath;
  const auto* Children = Field(Value, TEXT("children"));
  const auto* Decorators = Field(Value, TEXT("decorators"));
  const auto* Services = Field(Value, TEXT("services"));
  if (!String(Field(Value, TEXT("node_class_path")), ClassPath) || !Array(Children) || !Array(Decorators) || !Array(Services)) return false;
  auto* Node = Cast<UBehaviorTreeGraphNode>(CreateMain(*Lease.Graph, ClassPath, 0, 0));
  if (!Node || !Initialize(*Node, Node->NodeInstance, Value)) return Reject(Error.IsEmpty() ? TEXT("Stock main-node action failed") : *Error);
  for (const auto& Decorator : Decorators->Elements)
  {
   FString Kind;
   if (!HasAssetDocumentAIBehaviorTreeExactFields(Decorator, {TEXT("authored_properties"), TEXT("bound_graph"), TEXT("editor"), TEXT("kind"), TEXT("node_class_path"), TEXT("stable_node_id")})
    || !String(Field(Decorator, TEXT("kind")), Kind)) return false;
   if (Kind == TEXT("composite"))
   {
    if (Field(Decorator, TEXT("node_class_path"))->Kind != EKind::Null) return false;
    auto* Composite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(CreateComposite(*Lease.Graph, *Node));
    if (!Composite || !Initialize(*Composite, Composite, Decorator) || !Bound(*Composite, Field(Decorator, TEXT("bound_graph")))) return false;
   }
   else if (Kind == TEXT("simple"))
   {
    FString Path;
    if (!String(Field(Decorator, TEXT("node_class_path")), Path) || Field(Decorator, TEXT("bound_graph"))->Kind != EKind::Null) return false;
    auto* Sub = Cast<UBehaviorTreeGraphNode>(CreateSubnode(*Lease.Graph, *Node, Path, true));
    if (!Sub || !Initialize(*Sub, Sub->NodeInstance, Decorator)) return false;
   }
   else return Reject(TEXT("Unknown decorator kind"));
  }
  for (const auto& Service : Services->Elements)
  {
   FString Path;
   if (!HasAssetDocumentAIBehaviorTreeExactFields(Service, {TEXT("authored_properties"), TEXT("editor"), TEXT("node_class_path"), TEXT("stable_node_id")})
    || !String(Field(Service, TEXT("node_class_path")), Path)) return false;
   auto* Sub = Cast<UBehaviorTreeGraphNode>(CreateSubnode(*Lease.Graph, *Node, Path, false));
   if (!Sub || !Initialize(*Sub, Sub->NodeInstance, Service)) return false;
  }
  FName OutputName = NAME_None;
  if (Parent.IsA<UBehaviorTreeGraphNode_SimpleParallel>())
  {
   if (ChildIndex < 0 || ChildIndex > 1) return Reject(TEXT("SimpleParallel requires its Task and Out child slots"));
   OutputName = ChildIndex == 0 ? TEXT("Task") : TEXT("Out");
  }
  if (!Connect(*Lease.Graph, Parent, *Node, 0, OutputName)) return Reject(TEXT("Main graph schema connection rejected"));
  for (int32 Index = 0; Index < Children->Elements.Num(); ++Index)
   if (!Main(Children->Elements[Index], *Node, Depth + 1, Index)) return false;
  return true;
 }
 bool Run(const FValue& Tree)
 {
  if (!HasAssetDocumentAIBehaviorTreeExactFields(Tree, {TEXT("comments"), TEXT("graph_guid"), TEXT("graph_state"), TEXT("root")})
   || !Field(Tree, TEXT("graph_state")) || Field(Tree, TEXT("graph_state"))->StringValue != TEXT("present")) return Reject(TEXT("Invalid complete lowered tree DTO"));
  UBehaviorTreeGraphNode_Root* Root = nullptr;
  if (Lease.Graph->Nodes.IsEmpty()) Lease.Graph->GetSchema()->CreateDefaultNodesForGraph(*Lease.Graph);
  if (Lease.Graph->Nodes.Num() == 1) Root = Cast<UBehaviorTreeGraphNode_Root>(Lease.Graph->Nodes[0]);
  if (!Root || !Root->NodeGuid.IsValid()) return Reject(TEXT("Whole authored entry requires an empty/default-only scratch graph"));
  for (auto* Pin : Root->Pins) if (!Pin || !Pin->LinkedTo.IsEmpty()) return Reject(TEXT("Scratch default root is already connected"));
  auto* ScratchTree = Lease.State.IsValid() ? Lease.State->Tree.Get() : nullptr;
  if (!ScratchTree || Lease.Graph->GetOuter() != ScratchTree || Root->GetGraph() != Lease.Graph
   || !ScratchTree->HasAnyFlags(RF_Transient)) return Reject(TEXT("Whole authored root must belong to the transient scratch tree"));
  // Stock Root placement selects the first loaded Blackboard. Clear that
  // unrelated choice before creating instances; the admitted document's
  // Blackboard is assigned separately after graph installation.
  if (Root->BlackboardAsset || ScratchTree->BlackboardAsset)
  {
   if (!WriteProperties(*Root, Readback::ObjectValue({TEXT("BlackboardAsset")}, {Readback::NullValue()}), Error)) return false;
   if (Root->BlackboardAsset || ScratchTree->BlackboardAsset) return Reject(TEXT("Scratch Root did not clear its implicit Blackboard through the typed property"));
  }
  const auto* GraphGuid = Field(Tree, TEXT("graph_guid"));
  if (GraphGuid->Kind == EKind::Null)
  {
   if (!Lease.Graph->GraphGuid.IsValid()) return Reject(TEXT("Stock scratch graph has no generated GUID"));
  }
  else if (!SetGuid(*Lease.Graph, TEXT("GraphGuid"), GraphGuid)) return false;
  const auto* DesiredRoot = Field(Tree, TEXT("root"));
  if (DesiredRoot->Kind != EKind::Null && !Main(*DesiredRoot, *Root, 0)) return false;
  return Comments(*Lease.Graph, Field(Tree, TEXT("comments")));
 }
};
}
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeAuthorScratch(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction)
{
 using namespace UE::AssetDocumentAI::BehaviorTreeScratchAuthoring;
 using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
 if (Invocation.Version != 1 || Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Apply)
  return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidValueShape"), TEXT("Whole authored scratch requires version 1 and Apply phase."));
 if (!IsAssetDocumentAITransactionValid(Invocation, Transaction))
  return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidValueShape"), TEXT("Whole authored scratch requires the actual matching Atomic transaction."));
 if (!IsAssetDocumentAIBehaviorTreeScratchContext())
  return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidValueShape"), TEXT("Whole authored scratch requires the Editor scratch context."));
 if (!EnsureAssetDocumentAIBehaviorTreeEditorAuthority())
  return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidValueShape"), TEXT("Whole authored scratch could not establish stock Editor authority."));
 if (!IsAssetDocumentAICanonicalRuntimeValue(Invocation.ValueArguments))
  return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidValueShape"), TEXT("Whole authored scratch value arguments are not canonical RuntimeValue."));
 if (!HasAssetDocumentAIBehaviorTreeExactFields(Invocation.ValueArguments, {TEXT("tree")}))
  return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidValueShape"), TEXT("Whole authored scratch requires exactly the tree argument."));
 FAssetDocumentAIBehaviorTreeGraphLease Lease;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 if (Invocation.HandleArguments.Num() != 1 || !AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(Invocation, Handles, TEXTVIEW("graph"), Lease, Diagnostics))
  return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidHandle"), TEXT("One live same-operation scratch graph is required."), Diagnostics);
 // Stock actions can record editor undo even when node selection is disabled.
 // Only an undo scope created here is cancelled; Atomic still owns the whole
 // scratch inverse, and an existing user's transaction is never discarded.
 TUniquePtr<FScopedTransaction> ScratchUndo;
 if (GEditor && !GEditor->IsTransactionActive()) ScratchUndo = MakeUnique<FScopedTransaction>(
  NSLOCTEXT("AssetDocumentAI", "ScratchAuthoring", "Build authored BehaviorTree scratch"));
 ON_SCOPE_EXIT { if (ScratchUndo) ScratchUndo->Cancel(); };
 FBuilder Builder{Lease, *Transaction};
 if (!Builder.Run(*FindAssetDocumentAIBehaviorTreeField(Invocation.ValueArguments, TEXT("tree"))))
 {
  // The aggregate rollback owns all native allocations. Failure consumes the
  // capability immediately so a caller cannot INSTALL a partially built graph.
  Lease.State->bConsumed = true;
  return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.ScratchAuthoringRejected"),
   Builder.Error.IsEmpty() ? TEXT("The stock authored action/property operation failed.") : *Builder.Error);
 }
 FAssetDocumentPrimitiveOutcome Outcome;
 bool Overflow = false;
 if (!BuildAuthoredGraphProjection(*Lease.State->Tree.Get(), *Lease.Graph, Outcome.Value, Overflow, EAuthoredGraphReadbackVersion::V2))
 {
  Lease.State->bConsumed = true;
  return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.ScratchReadbackRejected"), TEXT("The completed scratch graph has no complete V2 authored readback."));
 }
 Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 Outcome.ChangedSet.Add(MakeAssetDocumentAIBehaviorTreeScratchChange(Lease.LiveBehaviorTreePath, *Lease.Graph, nullptr, TEXT("authored")));
 return Outcome;
}
