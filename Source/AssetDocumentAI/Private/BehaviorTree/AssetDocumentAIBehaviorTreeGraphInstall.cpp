// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "BehaviorTree/AssetDocumentAIBehaviorTreeGraphInstall.h"
#include "AssetDocumentRollbackAction.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDerivedCompletion.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeGraphBundleInternal.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredGraphValidation.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_NativeEnum.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "UObject/UObjectHash.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Utils/AssetDocumentEngineCompat.h"

namespace UE::AssetDocumentAI::BehaviorTreeGraphInstall
{
namespace
{
constexpr TCHAR Component[] = TEXT("assetdocument.ai.behavior_tree.graph_install.v1");
constexpr ERenameFlags RenameFlags = REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional;
FAssetDocumentContractValidationResult Failure(const TCHAR* Suffix, const TCHAR* Message)
{
 FAssetDocumentContractValidationResult R;
 FAssetDocumentRuntimeDiagnostic D; D.Code = FString(TEXT("AIProvider.BehaviorTree.GraphInstall")) + Suffix;
 D.Path = TEXT("/GraphInstall"); D.Message = Message; R.Diagnostics.Add(MoveTemp(D)); return R;
}
FAssetDocumentContractValidationResult Success() { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
FAssetDocumentPrimitiveOutcome OutcomeFailure(const FAssetDocumentContractValidationResult& R)
{
 return MakeAssetDocumentAIPrimitiveFailure(EAssetDocumentPrimitiveOutcomeStatus::Failed,
  TEXT("AIProvider.OperationRejected"), TEXT("Graph installation failed."), R.Diagnostics);
}
FAssetDocumentContractValidationResult NestedFailure(const TCHAR* Suffix,
 const FAssetDocumentContractValidationResult& Nested)
{
 auto R = Failure(Suffix, TEXT("The graph installation lifecycle was rejected."));
 auto& C = R.Diagnostics[0].Context.AddDefaulted_GetRef(); C.Key = TEXT("nested_diagnostics");
 C.Value.Kind = EAssetDocumentRuntimeValueKind::Array;
 for (const auto& D : Nested.Diagnostics)
  C.Value.Elements.Add(MakeAssetDocumentAIBehaviorTreeStringValue(D.Code + TEXT(": ") + D.Message));
 return R;
}
#if WITH_DEV_AUTOMATION_TESTS
EFailurePoint FailurePoint = EFailurePoint::None;
bool Fault(EFailurePoint Point) { return FailurePoint == Point; }
#else
enum class EFailurePoint : uint8 { None, AfterOldGraphIsolation, AfterOldRootsIsolation,
 AfterIncomingGraphTransfer, AfterIncomingRootsTransfer, AfterFlags, AfterGraphAssignment,
 BeforeHandleIssue, InverseTransfer };
bool Fault(EFailurePoint) { return false; }
#endif

struct FClosure
{
 TSet<UObject*> Domain;
 TSet<UBTNode*> Instances;
 TArray<UObject*> Roots;
 TArray<TStrongObjectPtr<UObject>> Objects;
 TArray<FSoftObjectPath> BoundPaths;
};

bool VisitGraph(UEdGraph& Graph, UBehaviorTree& Tree, FClosure& Out);
bool VisitNode(UEdGraphNode& Node, UBehaviorTree& Tree, FClosure& Out)
{
 if (Out.Domain.Contains(&Node)) return true;
 if (!Node.IsIn(&Tree) || Out.Domain.Num() >= 4096) return false;
 Out.Domain.Add(&Node);
 UBTNode* Instance = nullptr;
 if (auto* BT = Cast<UBehaviorTreeGraphNode>(&Node))
 {
  Instance = Cast<UBTNode>(BT->NodeInstance);
  if (BT->NodeInstance && !Instance) return false;
  for (UEdGraphNode* Child : BT->SubNodes) if (!Child || !VisitNode(*Child, Tree, Out)) return false;
  for (UEdGraphNode* Child : BT->Decorators) if (!Child || !VisitNode(*Child, Tree, Out)) return false;
  for (UEdGraphNode* Child : BT->Services) if (!Child || !VisitNode(*Child, Tree, Out)) return false;
  if (auto* Composite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(BT))
  {
   if (!Composite->BoundGraph || Composite->BoundGraph->GetOuter() != Composite
    || !VisitGraph(*Composite->BoundGraph, Tree, Out)) return false;
   Out.BoundPaths.Add(FSoftObjectPath(Composite->BoundGraph));
  }
 }
 else if (auto* Test = Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(&Node))
 {
  Instance = Cast<UBTNode>(Test->NodeInstance);
  if (!Instance) return false;
 }
 if (Instance)
 {
  if (!Instance->IsIn(&Tree) || Out.Instances.Contains(Instance)) return false;
  Out.Instances.Add(Instance); Out.Domain.Add(Instance);
 }
 return true;
}
bool VisitGraph(UEdGraph& Graph, UBehaviorTree& Tree, FClosure& Out)
{
 if (Out.Domain.Contains(&Graph) || !Graph.IsIn(&Tree)) return false;
 Out.Domain.Add(&Graph);
 for (UEdGraphNode* Node : Graph.Nodes) if (!Node || !VisitNode(*Node, Tree, Out)) return false;
 return true;
}

FAssetDocumentContractValidationResult CaptureClosure(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph, FClosure& Out)
{
 if (Graph.GetOuter() != &Tree || !VisitGraph(Graph, Tree, Out))
  return Failure(TEXT("OwnershipRejected"), TEXT("Graph domain references have foreign or shared ownership."));
 TSet<UObject*> All = Out.Domain;
 for (UBTNode* Instance : Out.Instances)
 {
  TArray<UObject*> Nested; GetObjectsWithOuter(Instance, Nested);
  for (UObject* Object : Nested) All.Add(Object);
  if (Instance->GetOuter() == &Tree) Out.Roots.Add(Instance);
  else if (!Instance->IsIn(&Graph))
  {
   bool OwnedByInstance = false;
   for (const UBTNode* Other : Out.Instances) if (Other != Instance && Instance->IsIn(Other)) OwnedByInstance = true;
   if (!OwnedByInstance) return Failure(TEXT("OwnershipRejected"), TEXT("Runtime instance has an unrelated ownership root."));
  }
 }
 TArray<UObject*> GraphNested; GetObjectsWithOuter(&Graph, GraphNested);
 for (UObject* Object : GraphNested)
  if (!All.Contains(Object))
  {
   auto R = Failure(TEXT("OwnershipRejected"), TEXT("Graph contains an unreferenced owned object."));
   R.Diagnostics[0].Message += FString::Printf(TEXT(" Object=%s Class=%s Outer=%s Graph=%s"),
    *Object->GetPathName(), *Object->GetClass()->GetPathName(), *GetPathNameSafe(Object->GetOuter()), *Graph.GetPathName());
   for (const auto& Field : {TPair<FString, FString>(TEXT("object_path"), Object->GetPathName()),
    TPair<FString, FString>(TEXT("object_class"), Object->GetClass()->GetPathName()),
    TPair<FString, FString>(TEXT("outer_path"), GetPathNameSafe(Object->GetOuter()))})
   {
    auto& Context = R.Diagnostics[0].Context.AddDefaulted_GetRef();
    Context.Key = Field.Key; Context.Value = MakeAssetDocumentAIBehaviorTreeStringValue(Field.Value);
   }
   return R;
  }
 if (All.Num() > 4096) return Failure(TEXT("OwnershipRejected"), TEXT("Owned graph closure exceeds the bounded inventory."));
 for (UObject* Object : All)
 {
  if (!IsValid(Object) || Object->IsTemplate() || Object->HasAnyFlags(RF_Public | RF_Standalone | RF_BeginDestroyed | RF_FinishDestroyed))
   return Failure(TEXT("OwnershipRejected"), TEXT("Owned graph closure has unsupported object flags."));
  Out.Objects.Emplace(Object);
 }
 TArray<UObject*> Direct; GetObjectsWithOuter(&Tree, Direct, AssetDocumentObjects::DirectOnly);
 for (UObject* Object : Direct)
 {
  // These public editor objects are exempt from stock orphan cleanup. No
  // unrelated runtime/object root may be silently relocated by UpdateAsset.
  if (!All.Contains(Object) && !Object->IsA<UEdGraph>() && !Object->IsA<UEdGraphNode>() && !Object->IsA<UEdGraphSchema>())
   return Failure(TEXT("OwnershipRejected"), TEXT("Tree owns a foreign object that stock completion could orphan."));
 }
 return Success();
}

bool EncodeProjection(UBehaviorTree& Tree, UBehaviorTreeGraph* ScratchGraph, TArray<uint8>& Out,
 FAssetDocumentRuntimeValueDto* CapturedValue = nullptr)
{
 using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
 FString Identity, Diagnostic;
 if (!(ScratchGraph ? CaptureGraphIdentity(Tree, *ScratchGraph, Identity, Diagnostic, EAuthoredGraphReadbackVersion::V2)
  : CaptureGraphIdentity(Tree, Identity, Diagnostic, EAuthoredGraphReadbackVersion::V2))) return false;
 FAssetDocumentRuntimeValueDto Value; bool Overflow = false;
 if (!(ScratchGraph ? BuildAuthoredGraphProjection(Tree, *ScratchGraph, Value, Overflow, EAuthoredGraphReadbackVersion::V2)
  : BuildAuthoredGraphProjection(Tree, Value, Overflow, EAuthoredGraphReadbackVersion::V2))) return false;
 if (Overflow || !EncodeAuthoredCanonicalValue(Value, Out)) return false;
 if (CapturedValue) *CapturedValue = MoveTemp(Value);
 return true;
}

void RemapOwnedReferences(FAssetDocumentRuntimeValueDto& Value, const TMap<FString, FString>& Paths)
{
 // Only the D2 reference envelope can change under an admitted ownership move.
 // Authored strings, external paths, class paths, and all other DTO fields stay exact.
 if (Value.Kind == EAssetDocumentRuntimeValueKind::Object
  && Value.ObjectKeys == TArray<FString>{TEXT("Kind"), TEXT("Path")} && Value.Elements.Num() == 2
  && Value.Elements[0].Kind == EAssetDocumentRuntimeValueKind::String
  && Value.Elements[1].Kind == EAssetDocumentRuntimeValueKind::String
  && (Value.Elements[0].StringValue == TEXT("AssetRef") || Value.Elements[0].StringValue == TEXT("ClassRef")))
 {
  if (const FString* Path = Paths.Find(Value.Elements[1].StringValue)) Value.Elements[1].StringValue = *Path;
  return;
 }
 for (auto& Element : Value.Elements) RemapOwnedReferences(Element, Paths);
}

struct FMove
{
 TStrongObjectPtr<UObject> Object;
 TStrongObjectPtr<UObject> Outer;
 FName Name;
};
struct FFlag
{
 TStrongObjectPtr<UObject> Object;
 bool bTransient;
};

bool SetRootBlackboard(UBehaviorTreeGraphNode_Root& Root, UBlackboardData* Blackboard)
{
 auto* Module = FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
 if (!Module) return false;
 FPropertyRowGeneratorArgs Args; Args.bAllowEditingClassDefaultObjects = false;
 const TSharedRef<IPropertyRowGenerator> Rows = Module->CreatePropertyRowGenerator(Args);
 Rows->SetPropertyGenerationAllowListPaths({GET_MEMBER_NAME_CHECKED(UBehaviorTreeGraphNode_Root, BlackboardAsset).ToString()});
 Rows->SetObjects({&Root});
 TSharedPtr<IPropertyHandle> Handle; int32 Matches = 0;
 TFunction<void(const TArray<TSharedRef<IDetailTreeNode>>&)> Find = [&](const TArray<TSharedRef<IDetailTreeNode>>& Nodes)
 {
  for (const auto& Node : Nodes)
  {
   const auto Candidate = Node->CreatePropertyHandle();
   const auto* Property = Candidate.IsValid() ? CastField<FObjectPropertyBase>(Candidate->GetProperty()) : nullptr;
   TArray<UObject*> Outers; if (Candidate.IsValid()) Candidate->GetOuterObjects(Outers);
   if (Property && Property->GetFName() == GET_MEMBER_NAME_CHECKED(UBehaviorTreeGraphNode_Root, BlackboardAsset)
    && Property->GetOwnerClass() == UBehaviorTreeGraphNode_Root::StaticClass()
    && Property->PropertyClass == UBlackboardData::StaticClass()
    && Candidate->GeneratePathToProperty() == TEXT("BlackboardAsset") && Candidate->IsValidHandle() && Candidate->IsEditable()
    && Outers.Num() == 1 && Outers[0] == &Root)
   { Handle = Candidate; ++Matches; }
   TArray<TSharedRef<IDetailTreeNode>> Children; Node->GetChildren(Children, true); Find(Children);
  }
 };
 Find(Rows->GetRootTreeNodes());
 return Matches == 1 && Handle->SetValue(Blackboard, EPropertyValueSetFlags::NotTransactable) == FPropertyAccess::Success
  && Root.BlackboardAsset == Blackboard;
}

bool InitializeInstalledGraph(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph,
 UBehaviorTreeGraphNode_Root& Root, const FClosure& Closure)
{
 if (Graph.GetOuter() != &Tree || Root.GetGraph() != &Graph) return false;
 UBlackboardData* const Blackboard = Tree.BlackboardAsset;
 // The target header is authoritative. The scratch root may have selected an
 // unrelated loaded Blackboard in stock PostPlacedNewNode; never publish it.
 if (!SetRootBlackboard(Root, Blackboard) || Tree.BlackboardAsset != Blackboard) return false;
 // SetValue of the same header can skip the native notification. This exported
 // lifecycle initializes all main, service and bound-decorator instances.
 Graph.UpdateBlackboardChange();
 for (const UBTNode* Instance : Closure.Instances)
  if (Instance->GetTreeAsset() != &Tree || Instance->GetBlackboardAsset() != Blackboard) return false;
 return Root.BlackboardAsset == Blackboard && Tree.BlackboardAsset == Blackboard;
}

const UEnum* SelectedEnum(const UBlackboardData* Blackboard, FName Key)
{
 const FBlackboardEntry* Entry = Blackboard ? Blackboard->GetKey(Blackboard->GetKeyID(Key)) : nullptr;
 if (!Entry || !Entry->KeyType) return nullptr;
 if (Entry->KeyType->GetClass() == UBlackboardKeyType_Enum::StaticClass())
  return CastChecked<UBlackboardKeyType_Enum>(Entry->KeyType)->EnumType;
 if (Entry->KeyType->GetClass() == UBlackboardKeyType_NativeEnum::StaticClass())
  return CastChecked<UBlackboardKeyType_NativeEnum>(Entry->KeyType)->EnumType;
 return nullptr;
}
struct FEnumTransition
{
 UBTDecorator_Blackboard* Instance = nullptr;
 FString Label;
 int32 Before = 0, After = 0;
};
FAssetDocumentRuntimeValueDto* Field(FAssetDocumentRuntimeValueDto& Object, const TCHAR* Name)
{
 const int32 Index = Object.ObjectKeys.IndexOfByKey(Name);
 return Object.Kind == EAssetDocumentRuntimeValueKind::Object && Object.Elements.IsValidIndex(Index) ? &Object.Elements[Index] : nullptr;
}
bool ApplyEnumExpectation(FAssetDocumentRuntimeValueDto& Node, const TMap<FString, FEnumTransition>& Transitions, TSet<FString>& Seen)
{
 if (Node.Kind == EAssetDocumentRuntimeValueKind::Null) return true;
 auto* Guid = Field(Node, TEXT("stable_node_id"));
 if (!Guid || Guid->Kind != EAssetDocumentRuntimeValueKind::String) return false;
 if (const auto* Transition = Transitions.Find(Guid->StringValue))
 {
  auto* Properties = Field(Node, TEXT("authored_properties"));
  auto* Int = Properties ? Field(*Properties, TEXT("IntValue")) : nullptr;
  auto* Label = Properties ? Field(*Properties, TEXT("StringValue")) : nullptr;
  if (Seen.Contains(Guid->StringValue) || !Int || Int->Kind != EAssetDocumentRuntimeValueKind::Integer
   || Int->IntegerValue != Transition->Before || !Label || Label->Kind != EAssetDocumentRuntimeValueKind::String
   || Label->StringValue != Transition->Label) return false;
  Seen.Add(Guid->StringValue); Int->IntegerValue = Transition->After;
 }
 // Only D2's node envelopes are traversed, never arbitrary authored maps.
 for (const TCHAR* Name : {TEXT("children"), TEXT("decorators"), TEXT("services")})
  if (auto* Values = Field(Node, Name))
  {
   if (Values->Kind != EAssetDocumentRuntimeValueKind::Array) return false;
   for (auto& Child : Values->Elements) if (!ApplyEnumExpectation(Child, Transitions, Seen)) return false;
  }
 if (auto* Bound = Field(Node, TEXT("bound_graph")); Bound && Bound->Kind != EAssetDocumentRuntimeValueKind::Null)
 {
  auto* Nodes = Field(*Bound, TEXT("nodes")); if (!Nodes || Nodes->Kind != EAssetDocumentRuntimeValueKind::Array) return false;
  for (auto& Child : Nodes->Elements) if (!ApplyEnumExpectation(Child, Transitions, Seen)) return false;
 }
 return true;
}

class FInstallAction final : public IAssetDocumentRollbackAction
{
public:
 FInstallAction(UBehaviorTree& InTree, TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> InScratch)
  : Tree(&InTree), Scratch(MoveTemp(InScratch)), Incoming(Scratch->Graph.Get()),
    Old(Cast<UBehaviorTreeGraph>(InTree.BTGraph)), Live(MakeShared<FAssetDocumentAIBehaviorTreeLiveGraphState>()),
    Documents(InTree.LastEditedDocuments), bDirty(InTree.GetPackage()->IsDirty()) {}
 FStringView GetRollbackComponentId() const override { return Component; }

 FAssetDocumentContractValidationResult Capture()
 {
  if (!Incoming.IsValid() || !Scratch->Tree.IsValid() || Scratch->bConsumed || Scratch->Tree.Get() == Tree.Get()
   || Scratch->Tree->BTGraph || Incoming->GetOuter() != Scratch->Tree.Get())
   return Failure(TEXT("OwnershipRejected"), TEXT("Installation requires an unconsumed exact scratch bundle."));
  if (!Old && (Tree->RootNode || !Tree->RootDecorators.IsEmpty() || !Tree->RootDecoratorOps.IsEmpty()))
   return Failure(TEXT("PreimageMismatch"), TEXT("An absent old graph must have an empty runtime mirror."));
  if (!EncodeProjection(*Tree, nullptr, OldProjection)
   || !EncodeProjection(*Scratch->Tree, Incoming.Get(), IncomingProjection, &IncomingValue))
   return Failure(TEXT("PreimageMismatch"), TEXT("Both graph projections must have complete canonical authored semantics."));
  // Root=null is a present empty authored tree. Its only additional nodes
  // may be owned comments; the normal tree retains its single composite root.
  UBehaviorTreeGraphNode_Root* Root = nullptr;
  for (UEdGraphNode* Node : Incoming->Nodes) if (auto* Candidate = Cast<UBehaviorTreeGraphNode_Root>(Node)) Root = Candidate;
  int32 RootLinks = 0;
  UBehaviorTreeGraphNode* RootChild = nullptr;
  if (Root) for (const auto* Pin : Root->Pins) if (Pin && Pin->Direction == EGPD_Output)
  {
   RootLinks += Pin->LinkedTo.Num();
   for (const auto* Linked : Pin->LinkedTo)
    if (Linked) RootChild = Cast<UBehaviorTreeGraphNode>(Linked->GetOwningNode());
  }
  bIncomingEmpty=Root && RootLinks==0
   && UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::IsExactEmptyAuthoredGraph(*Incoming);
  if (!Root || (!bIncomingEmpty && (RootLinks != 1 || !RootChild || !Cast<UBTCompositeNode>(RootChild->NodeInstance))))
   return Failure(TEXT("PreimageMismatch"), TEXT("Incoming graph requires its complete single composite-rooted tree."));
  if (Root->BlackboardAsset != Scratch->Tree->BlackboardAsset)
   return Failure(TEXT("PreimageMismatch"), TEXT("Incoming root must match its own scratch Blackboard header."));
  IncomingRoot = Root; ScratchBlackboard = Root->BlackboardAsset;
  auto R = CaptureClosure(*Scratch->Tree, *Incoming, IncomingClosure); if (!R.bValid) return R;
  const auto* Int = FindFProperty<FIntProperty>(UBTDecorator_Blackboard::StaticClass(), TEXT("IntValue"));
  const auto* Label = FindFProperty<FStrProperty>(UBTDecorator_Blackboard::StaticClass(), TEXT("StringValue"));
  if (!Int || !Label) return Failure(TEXT("PreimageMismatch"), TEXT("The stock decorator enum fields are unavailable."));
  for (UObject* Object : IncomingClosure.Domain)
  {
   UBTDecorator_Blackboard* Instance = nullptr;
   auto* Node = Cast<UEdGraphNode>(Object);
   if (auto* MainNode = Cast<UBehaviorTreeGraphNode>(Node)) Instance = Cast<UBTDecorator_Blackboard>(MainNode->NodeInstance);
   else if (auto* BoundNode = Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(Node)) Instance = Cast<UBTDecorator_Blackboard>(BoundNode->NodeInstance);
   if (!Instance) continue;
   const UEnum* BeforeEnum = SelectedEnum(Scratch->Tree->BlackboardAsset, Instance->GetSelectedBlackboardKey());
   const UEnum* AfterEnum = SelectedEnum(Tree->BlackboardAsset, Instance->GetSelectedBlackboardKey());
   const FString Name = Label->GetPropertyValue_InContainer(Instance);
   // Unknown/non-enum sources and legacy empty labels receive no exception.
   if (!BeforeEnum || !AfterEnum || Name.IsEmpty()
    || BeforeEnum->GetIndexByNameString(Name) == INDEX_NONE || AfterEnum->GetIndexByNameString(Name) == INDEX_NONE) continue;
   const int64 Before = BeforeEnum->GetValueByNameString(Name), After = AfterEnum->GetValueByNameString(Name);
   if (Before < MIN_int32 || Before > MAX_int32 || After < MIN_int32 || After > MAX_int32
    || Int->GetPropertyValue_InContainer(Instance) != Before) continue;
   const FString Guid = Node->NodeGuid.ToString(EGuidFormats::Digits);
   if (EnumTransitions.Contains(Guid)) return Failure(TEXT("PreimageMismatch"), TEXT("Enum transition node identity is not unique."));
   EnumTransitions.Add(Guid, {Instance, Name, static_cast<int32>(Before), static_cast<int32>(After)});
  }
  for (const auto& Object : IncomingClosure.Objects) IncomingPaths.Add(Object.Get(), Object->GetPathName());
  if (Old) { R = CaptureClosure(*Tree, *Old, OldClosure); if (!R.bValid) return R; }
  else
  {
   TArray<UObject*> Direct; GetObjectsWithOuter(Tree.Get(), Direct, AssetDocumentObjects::DirectOnly);
   for (UObject* Object : Direct)
    if (!Object->IsA<UEdGraph>() && !Object->IsA<UEdGraphNode>() && !Object->IsA<UEdGraphSchema>())
     return Failure(TEXT("OwnershipRejected"), TEXT("Absent old graph has an unrelated owned runtime object."));
  }
  for (const auto* Closure : {&OldClosure, &IncomingClosure})
   for (const auto& Object : Closure->Objects)
   {
    Flags.Add({TStrongObjectPtr<UObject>(Object.Get()), Object->HasAnyFlags(RF_Transient)});
    OriginalOwnership.Add({TStrongObjectPtr<UObject>(Object.Get()), TStrongObjectPtr<UObject>(Object->GetOuter()), Object->GetFName()});
   }
  DesiredGraphName = Old ? Old->GetFName() : Incoming->GetFName();
  if (UObject* Conflict = StaticFindObjectFast(UObject::StaticClass(), Tree.Get(), DesiredGraphName))
   if (Conflict != Old.Get()) return Failure(TEXT("OwnershipRejected"), TEXT("Incoming graph name conflicts with a foreign object."));
  for (UObject* Object : IncomingClosure.Roots)
   if (UObject* Conflict = StaticFindObjectFast(UObject::StaticClass(), Tree.Get(), Object->GetFName()))
    if (!OldClosure.Roots.Contains(Conflict)) return Failure(TEXT("OwnershipRejected"), TEXT("Incoming runtime name conflicts with a foreign object."));
  return Success();
 }

 bool Move(UObject& Object, UObject& Outer, FName Name)
 {
  if (Object.GetOuter() == &Outer && Object.GetFName() == Name) return true;
  if (!Object.Rename(*Name.ToString(), &Outer, RenameFlags | REN_Test)) return false;
  FMove Entry{TStrongObjectPtr<UObject>(&Object), TStrongObjectPtr<UObject>(Object.GetOuter()), Object.GetFName()};
  const bool Renamed = Object.Rename(*Name.ToString(), &Outer, RenameFlags);
  // A failed native return can still have changed ownership: retain its inverse.
  if (Object.GetOuter() != Entry.Outer.Get() || Object.GetFName() != Entry.Name) Moves.Add(MoveTemp(Entry));
  return Renamed && Object.GetOuter() == &Outer && Object.GetFName() == Name;
 }

 FAssetDocumentContractValidationResult Transfer()
 {
  Scratch->bConsumed = true; bStarted = true;
  if (Old)
  {
   const FName Name = MakeUniqueObjectName(GetTransientPackage(), Old->GetClass(), TEXT("AssetDocumentOldBTGraph"));
   if (!Move(*Old, *GetTransientPackage(), Name) || Fault(EFailurePoint::AfterOldGraphIsolation)) return TransferFailure();
   for (UObject* Object : OldClosure.Roots) if (!Move(*Object, *Old, Object->GetFName())) return TransferFailure();
  }
  if (Fault(EFailurePoint::AfterOldRootsIsolation)) return TransferFailure();
  if (!Move(*Incoming, *Tree, DesiredGraphName) || Fault(EFailurePoint::AfterIncomingGraphTransfer)) return TransferFailure();
  for (UObject* Object : IncomingClosure.Roots) if (!Move(*Object, *Tree, Object->GetFName())) return TransferFailure();
  if (Fault(EFailurePoint::AfterIncomingRootsTransfer)) return TransferFailure();
  for (const auto& Object : OldClosure.Objects) Object->SetFlags(RF_Transient);
  for (const auto& Object : IncomingClosure.Objects) Object->ClearFlags(RF_Transient);
  if (Fault(EFailurePoint::AfterFlags)) return TransferFailure();
  Tree->BTGraph = Incoming.Get(); bAssigned = true;
  if (!InitializeInstalledGraph(*Tree, *Incoming, *IncomingRoot, IncomingClosure)) return TransferFailure();
  if (bIncomingEmpty)
  {
   if (!UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::IsExactEmptyAuthoredGraph(*Incoming)) return TransferFailure();
   Incoming->CreateBTFromGraph(nullptr);
   if (Tree->RootNode || !Tree->RootDecorators.IsEmpty() || !Tree->RootDecoratorOps.IsEmpty()) return TransferFailure();
  }
  FClosure Current; const auto R = CaptureClosure(*Tree, *Incoming, Current); if (!R.bValid) return R;
  TMap<FString, FString> ReferencePaths;
  for (const auto& Entry : IncomingPaths) ReferencePaths.Add(Entry.Value, Entry.Key->GetPathName());
  FAssetDocumentRuntimeValueDto ExpectedValue = IncomingValue;
  RemapOwnedReferences(ExpectedValue, ReferencePaths);
  auto* ExpectedRoot = Field(ExpectedValue, TEXT("root")); TSet<FString> SeenEnums;
  if (!ExpectedRoot || !ApplyEnumExpectation(*ExpectedRoot, EnumTransitions, SeenEnums)
   || SeenEnums.Num() != EnumTransitions.Num()) return TransferFailure();
  const auto* Int = FindFProperty<FIntProperty>(UBTDecorator_Blackboard::StaticClass(), TEXT("IntValue"));
  for (const auto& Entry : EnumTransitions)
   if (!Int || Int->GetPropertyValue_InContainer(Entry.Value.Instance) != Entry.Value.After) return TransferFailure();
  TArray<uint8> ExpectedProjection, ActualProjection;
  if (!UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EncodeAuthoredCanonicalValue(ExpectedValue, ExpectedProjection)
   || !EncodeProjection(*Tree, nullptr, ActualProjection) || ExpectedProjection != ActualProjection)
   return TransferFailure();
  Tree->LastEditedDocuments.RemoveAll([&](const FEditedDocumentInfo& Document)
  { return OldClosure.BoundPaths.Contains(Document.EditedObjectPath) && !Current.BoundPaths.Contains(Document.EditedObjectPath); });
  Tree->GetPackage()->SetDirtyFlag(true);
  if (Fault(EFailurePoint::AfterGraphAssignment)) return TransferFailure();
  Live->Tree = Tree.Get(); Live->Graph = Incoming.Get(); Live->GraphGuid = Incoming->GraphGuid;
  return Success();
 }

 FAssetDocumentContractValidationResult Rollback(FStringView) override
 {
  Live->bAlive = false;
  if (bRestored) return Success();
  if (!bStarted) { ReleaseSuccessfulRollback(); return Success(); }
  if (!Tree.IsValid() || !Incoming.IsValid() || Fault(EFailurePoint::InverseTransfer)) return RollbackFailure();
  // Discover only current graph-proven effects, never arbitrary new target
  // objects. Original preimage/ownership records are immutable.
  if (Incoming->GetOuter() == Tree.Get())
  {
   if (bAssigned)
   {
    FClosure Current; const auto R = CaptureClosure(*Tree, *Incoming, Current); if (!R.bValid) return RollbackFailure();
    for (UObject* Object : Current.Roots)
    {
     if (!IncomingClosure.Domain.Contains(Object))
     {
      Object->SetFlags(RF_Transient);
      const FName Name = MakeUniqueObjectName(Incoming.Get(), Object->GetClass(), Object->GetFName());
      if (!Object->Rename(*Name.ToString(), Incoming.Get(), RenameFlags)) return RollbackFailure();
     }
    }
   }
   Incoming->CreateBTFromGraph(nullptr);
   if (Tree->RootNode || !Tree->RootDecorators.IsEmpty() || !Tree->RootDecoratorOps.IsEmpty()) return RollbackFailure();
  }
  // Stock completion can promote an originally nested incoming instance to
  // a target-owned root. Release every captured incoming target root before
  // restoring old roots: otherwise their original names may still collide.
  for (const auto& Entry : OriginalOwnership)
  {
   if (!IncomingPaths.Contains(Entry.Object.Get()) || Entry.Object.Get() == Incoming.Get()) continue;
   if (!Entry.Object.IsValid() || !Entry.Outer.IsValid()) return RollbackFailure();
   if (Entry.Object->GetOuter() != Tree.Get()) continue;
   if (!Entry.Object->Rename(*Entry.Name.ToString(), Entry.Outer.Get(), RenameFlags | REN_Test)
    || !Entry.Object->Rename(*Entry.Name.ToString(), Entry.Outer.Get(), RenameFlags)
    || Entry.Object->GetOuter() != Entry.Outer.Get() || Entry.Object->GetFName() != Entry.Name) return RollbackFailure();
  }
  for (int32 Index = Moves.Num() - 1; Index >= 0; --Index)
  {
   auto& Entry = Moves[Index];
   if (!Entry.Object.IsValid() || !Entry.Outer.IsValid()) return RollbackFailure();
   if (Entry.Object->GetOuter() == Entry.Outer.Get() && Entry.Object->GetFName() == Entry.Name) continue;
   if (!Entry.Object->Rename(*Entry.Name.ToString(), Entry.Outer.Get(), RenameFlags | REN_Test)
    || !Entry.Object->Rename(*Entry.Name.ToString(), Entry.Outer.Get(), RenameFlags)
    || Entry.Object->GetOuter() != Entry.Outer.Get() || Entry.Object->GetFName() != Entry.Name) return RollbackFailure();
  }
  // Public completion can reouter a referenced runtime instance that was
  // originally nested in its graph. Restore recorded ownership, not runtime
  // bytes or the address of a newly materialized replacement instance.
  for (const auto& Entry : OriginalOwnership)
  {
   if (!Entry.Object.IsValid() || !Entry.Outer.IsValid()) return RollbackFailure();
   if (Entry.Object->GetOuter() == Entry.Outer.Get() && Entry.Object->GetFName() == Entry.Name) continue;
   if (!Entry.Object->Rename(*Entry.Name.ToString(), Entry.Outer.Get(), RenameFlags | REN_Test)
    || !Entry.Object->Rename(*Entry.Name.ToString(), Entry.Outer.Get(), RenameFlags)
    || Entry.Object->GetOuter() != Entry.Outer.Get() || Entry.Object->GetFName() != Entry.Name) return RollbackFailure();
  }
  for (const auto& Flag : Flags)
  {
   if (!Flag.Object.IsValid()) return RollbackFailure();
   if (Flag.bTransient) Flag.Object->SetFlags(RF_Transient); else Flag.Object->ClearFlags(RF_Transient);
  }
  Tree->BTGraph = Old.Get();
  Tree->LastEditedDocuments = Documents;
  TArray<uint8> Restored;
  if (!EncodeProjection(*Tree, nullptr, Restored) || Restored != OldProjection) return RollbackFailure();
  if (Old)
  {
   const auto R = RecompleteGraphInstallInverse(*Tree); if (!R.bValid) return NestedFailure(TEXT("RollbackFailed"), R);
   const auto Ownership = ValidateInstalledGraphOwnership(*Tree); if (!Ownership.bValid) return RollbackFailure();
  }
  else if (Tree->RootNode || !Tree->RootDecorators.IsEmpty() || !Tree->RootDecoratorOps.IsEmpty()) return RollbackFailure();
  if (Incoming->GetOuter() != Scratch->Tree.Get()) return RollbackFailure();
  if (!IncomingRoot || Scratch->Tree->BlackboardAsset != ScratchBlackboard.Get()
   || !InitializeInstalledGraph(*Scratch->Tree, *Incoming, *IncomingRoot, IncomingClosure)) return RollbackFailure();
  TArray<uint8> RestoredIncoming;
  if (!EncodeProjection(*Scratch->Tree, Incoming.Get(), RestoredIncoming) || RestoredIncoming != IncomingProjection) return RollbackFailure();
  FClosure RestoredScratchClosure;
  if (!CaptureClosure(*Scratch->Tree, *Incoming, RestoredScratchClosure).bValid) return RollbackFailure();
  for (const auto& Object : IncomingClosure.Objects) if (Object->IsIn(Tree.Get())) return RollbackFailure();
  Tree->GetPackage()->SetDirtyFlag(bDirty);
  ReleaseSuccessfulRollback();
  return Success();
 }
 void ReleaseSuccessfulRollback()
 {
  // Atomic retains completed actions while earlier factory inverses run.
  // Only a fully verified successful inverse may release its target keepalive.
  bRestored = true;
  Moves.Reset(); OriginalOwnership.Reset(); Flags.Reset();
  IncomingClosure = FClosure{}; OldClosure = FClosure{};
  IncomingPaths.Reset(); EnumTransitions.Reset(); Incoming.Reset(); Old.Reset(); Tree.Reset();
  IncomingRoot = nullptr; ScratchBlackboard.Reset();
 }
 FAssetDocumentContractValidationResult TransferFailure() const { return Failure(TEXT("TransferFailed"), TEXT("An ownership transfer did not complete; Atomic inverse is required.")); }
 FAssetDocumentContractValidationResult RollbackFailure() const { return Failure(TEXT("RollbackFailed"), TEXT("Graph installation inverse could not restore its exact owned aggregate.")); }

 TStrongObjectPtr<UBehaviorTree> Tree;
 TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> Scratch;
 TStrongObjectPtr<UBehaviorTreeGraph> Incoming, Old;
 TSharedRef<FAssetDocumentAIBehaviorTreeLiveGraphState> Live;
 FClosure IncomingClosure, OldClosure;
 TArray<FMove> Moves;
 TArray<FMove> OriginalOwnership;
 TArray<FFlag> Flags;
 TArray<FEditedDocumentInfo> Documents;
 TArray<uint8> OldProjection, IncomingProjection;
 FAssetDocumentRuntimeValueDto IncomingValue;
 TMap<UObject*, FString> IncomingPaths;
 TMap<FString, FEnumTransition> EnumTransitions;
 UBehaviorTreeGraphNode_Root* IncomingRoot = nullptr;
 bool bIncomingEmpty = false;
 TWeakObjectPtr<UBlackboardData> ScratchBlackboard;
 FName DesiredGraphName;
 bool bDirty = false, bStarted = false, bAssigned = false, bRestored = false;
};
}

FAssetDocumentPrimitiveOutcome InstallExactGraph(UBehaviorTree& Tree,
 TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> Scratch,
 IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction& Transaction)
{
 const auto Action = MakeShared<FInstallAction>(Tree, MoveTemp(Scratch));
 auto R = Action->Capture(); if (!R.bValid) return OutcomeFailure(R);
 R = CaptureAndRegisterInstalledReplacementCompletion(Tree, Transaction);
 if (!R.bValid) return OutcomeFailure(NestedFailure(TEXT("RegistrationRejected"), R));
 R = Transaction.EnlistRollbackAction(Action);
 if (!R.bValid) return OutcomeFailure(NestedFailure(TEXT("RegistrationRejected"), R));
 FAssetDocumentMutationFootprint Row;
 Row.Kind = EAssetDocumentMutationFootprintKind::UObjectState; Row.RollbackComponentId = Component;
 Row.StableId = TEXT("asset:") + Tree.GetPathName() + TEXT("/GraphInstall");
 R = Transaction.EnlistFootprint(MakeArrayView(&Row, 1));
 if (!R.bValid) return OutcomeFailure(NestedFailure(TEXT("FootprintRejected"), R));
 R = Action->Transfer(); if (!R.bValid) return OutcomeFailure(R);
 FAssetDocumentPrimitiveOutcome Out; Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 FAssetDocumentRuntimeValueDto Replaced;
 if (Action->Old) Replaced = MakeAssetDocumentAIBehaviorTreeStringValue(Action->Old->GraphGuid.ToString(EGuidFormats::Digits));
 Out.Value = MakeAssetDocumentAIBehaviorTreeObjectValue({TEXT("installed_graph_guid"), TEXT("replaced_graph_guid_or_null")},
  {MakeAssetDocumentAIBehaviorTreeStringValue(Action->Incoming->GraphGuid.ToString(EGuidFormats::Digits)), MoveTemp(Replaced)});
 if (Fault(EFailurePoint::BeforeHandleIssue) || !IssueAssetDocumentAIBehaviorTreeLiveGraphHandle(Handles, Action->Live, Out))
 {
  auto FailureResult = Failure(TEXT("HandleIssueFailed"), TEXT("Installed live graph handle could not be issued."));
  if (!Out.Diagnostics.IsEmpty()) { FAssetDocumentContractValidationResult Native; Native.Diagnostics = Out.Diagnostics; FailureResult = NestedFailure(TEXT("HandleIssueFailed"), Native); }
  return OutcomeFailure(FailureResult);
 }
 auto& Change = Out.ChangedSet.AddDefaulted_GetRef(); Change.AuthorityId = TEXT("AD.AI.BT.GRAPH_INSTALL.v1");
 Change.StableObjectId = Row.StableId; Change.Path = Tree.GetPathName(); Change.ChangeKind = TEXT("Rebuild");
 return Out;
}
#if WITH_DEV_AUTOMATION_TESTS
void SetFailurePointForTest(EFailurePoint Point) { FailurePoint = Point; }
#endif
}

FAssetDocumentContractValidationResult ValidateInstalledGraphOwnership(UBehaviorTree& Tree)
{
 using namespace UE::AssetDocumentAI::BehaviorTreeGraphInstall;
 auto* Graph = Cast<UBehaviorTreeGraph>(Tree.BTGraph);
 if (!Graph || Graph->GetClass() != UBehaviorTreeGraph::StaticClass())
  return Failure(TEXT("OwnershipRejected"), TEXT("Installed ownership requires its exact current graph."));
 FClosure Closure; return CaptureClosure(Tree, *Graph, Closure);
}
