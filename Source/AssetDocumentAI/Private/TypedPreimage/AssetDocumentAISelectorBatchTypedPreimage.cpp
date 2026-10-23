// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "TypedPreimage/AssetDocumentAISelectorTypedPreimageInternal.h"
#include "PropertyHandle.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::AssetDocument::AI::TypedPreimage::Selector
{
UObject* ResolveExactSelectorOwner(UBehaviorTreeGraph& Graph,
 FStringView StableNodeId, UEdGraphNode** OutNode)
{
 if (OutNode != nullptr) *OutNode = nullptr;
 UBehaviorTree* Tree = Cast<UBehaviorTree>(Graph.GetOuter());
 if (!IsAdmittedObject(Tree) || Tree->BTGraph != &Graph || !Graph.GraphGuid.IsValid()) return nullptr;
 FGuid Guid;
 if (!FGuid::ParseExact(FString(StableNodeId), EGuidFormats::Digits, Guid)
  || !Guid.IsValid() || Guid.ToString(EGuidFormats::Digits) != StableNodeId) return nullptr;
 TSet<UEdGraphNode*> Seen;
 TArray<UEdGraphNode*> Pending;
 for (UEdGraphNode* Node : Graph.Nodes) Pending.Add(Node);
 UEdGraphNode* Match = nullptr;
 UObject* Owner = nullptr;
 while (!Pending.IsEmpty())
 {
  UEdGraphNode* Node = Pending.Pop(EAllowShrinking::No);
  if (Node == nullptr || Seen.Contains(Node)) continue;
  Seen.Add(Node);
  if (!IsAdmittedObject(Node) || !Node->IsIn(Tree)) return nullptr;
  UObject* Instance = nullptr;
  if (UBehaviorTreeGraphNode* BTNode = Cast<UBehaviorTreeGraphNode>(Node))
  {
   Instance = BTNode->NodeInstance.Get();
   for (UAIGraphNode* Child : BTNode->SubNodes) Pending.Add(Child);
   for (UBehaviorTreeGraphNode* Child : BTNode->Decorators) Pending.Add(Child);
   for (UBehaviorTreeGraphNode* Child : BTNode->Services) Pending.Add(Child);
   if (UBehaviorTreeGraphNode_CompositeDecorator* Composite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(BTNode))
    if (Composite->BoundGraph != nullptr)
     for (UEdGraphNode* Child : Composite->BoundGraph->Nodes) Pending.Add(Child);
  }
  else if (UBehaviorTreeDecoratorGraphNode_Decorator* Decorator = Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(Node))
   Instance = Decorator->NodeInstance.Get();
  if (Node->NodeGuid != Guid) continue;
  if (Match != nullptr || !IsAdmittedObject(Instance) || !Instance->IsIn(Tree)) return nullptr;
  Match = Node;
  Owner = Instance;
 }
 if (OutNode != nullptr) *OutNode = Match;
 return Owner;
}

namespace
{
constexpr const TCHAR* SelectorComponent =
 TEXT("assetdocument.ai.behavior_tree.selector_cache.v1");

struct FCapturedSelector
{
 FString StableNodeId;
 FString PropertyPath;
 TSharedRef<FAliasRollbackState> State;
 TStrongObjectPtr<UEdGraphNode> Node;
 TStrongObjectPtr<UObject> Owner;
 TStrongObjectPtr<UEdGraph> NodeGraph;
 FGuid NodeGuid;
 FGuid NodeGraphGuid;
 UObject* NodeOuter;
 UObject* OwnerOuter;
 TFunction<TSharedPtr<IPropertyHandle>()> NestedRebind;
 TFunction<bool(IPropertyHandle&)> NestedBinding;
};

class FSelectorBatchRollbackState final : public FAliasRollbackState
{
public:
 FSelectorBatchRollbackState(const FAssetDocumentEditorAssetIdentityView& Identity,
  TArray<FCapturedSelector> InSelectors)
  : Asset(&Identity.GetObject()), Package(Identity.GetObject().GetOutermost()),
    AssetPath(Identity.GetCanonicalObjectPath()), PackageName(Package->GetName()),
    AssetRevision(Identity.GetTypeRevision()), PackageId(Package->GetUniqueID()),
    bPackageDirty(Package->IsDirty()),
    Graph(Cast<UBehaviorTreeGraph>(CastChecked<UBehaviorTree>(Asset.Get())->BTGraph)),
    GraphGuid(Graph->GraphGuid), Selectors(MoveTemp(InSelectors))
 {
  for (const FCapturedSelector& Selector : Selectors) Selector.State->Arm();
 }

protected:
 FAssetDocumentContractValidationResult RestoreOnce() override
 {
  if (!IsEditorGameThread())
   return MakeValidationFailure(SelectorPath,
    TEXT("AITypedPreimage.InvalidExecutionContext"),
    TEXT("BehaviorTree selector rollback requires the Editor game thread."));
  if (!IsAdmittedObject(Asset.Get()) || !Package.IsValid()
   || Asset->GetPathName() != AssetPath || Asset->GetOutermost() != Package.Get()
   || static_cast<uint64>(Asset->GetUniqueID()) != AssetRevision
   || Package->GetName() != PackageName || Package->GetUniqueID() != PackageId)
   return MakeValidationFailure(SelectorPath,
    TEXT("AITypedPreimage.BlackboardSelectorRollbackIdentityMismatch"),
    TEXT("The selector Asset, owner, or package identity changed."));
  UBehaviorTree* Tree = Cast<UBehaviorTree>(Asset.Get());
  if (Tree == nullptr || !IsAdmittedObject(Graph.Get()) || Tree->BTGraph != Graph.Get()
   || Graph->GetOuter() != Tree || Graph->GraphGuid != GraphGuid)
   return MakeValidationFailure(SelectorPath,TEXT("AITypedPreimage.BlackboardSelectorRollbackIdentityMismatch"),TEXT("The selector root graph pointer, outer, or GUID changed."));
  // Validate the complete immutable binding inventory before any selector writes.
  for (const FCapturedSelector& Selector : Selectors)
  {
   UEdGraphNode* CurrentNode = nullptr;
   UObject* CurrentOwner = ResolveExactSelectorOwner(*Graph.Get(), Selector.StableNodeId, &CurrentNode);
   if (CurrentOwner == nullptr || CurrentOwner != Selector.Owner.Get()
    || CurrentNode != Selector.Node.Get() || CurrentNode->NodeGuid != Selector.NodeGuid
    || CurrentNode->GetOuter() != Selector.NodeOuter || CurrentOwner->GetOuter() != Selector.OwnerOuter
    || CurrentNode->GetGraph() != Selector.NodeGraph.Get()
    || !Selector.NodeGraph.IsValid() || Selector.NodeGraph->GraphGuid != Selector.NodeGraphGuid)
    return MakeValidationFailure(SelectorPath,TEXT("AITypedPreimage.BlackboardSelectorRollbackIdentityMismatch"),*FString::Printf(TEXT("Selector node/owner binding changed: %s/%s."),*Selector.StableNodeId,*Selector.PropertyPath));
   if (Selector.NestedRebind)
   {
    const auto Current=Selector.NestedRebind();
    if (!Current.IsValid() || !Selector.NestedBinding || !Selector.NestedBinding(*Current)) return MakeValidationFailure(SelectorPath,TEXT("AITypedPreimage.BlackboardSelectorRollbackIdentityMismatch"),*FString::Printf(TEXT("Selector native property binding changed: %s/%s (rebind=%d)."),*Selector.StableNodeId,*Selector.PropertyPath,Current.IsValid()));
   }
  }
  FAssetDocumentContractValidationResult Result = MakeValidationSuccess();
  // The immutable inventory was captured in canonical order before enlistment.
  for (int32 Index = Selectors.Num() - 1; Index >= 0; --Index)
  {
   const FAssetDocumentContractValidationResult Restored = Selectors[Index].State->Restore();
   Result.bValid &= Restored.bValid;
   Result.Diagnostics.Append(Restored.Diagnostics);
  }
  if (!Result.bValid) return Result;
  Package->SetDirtyFlag(bPackageDirty);
  if (Package->IsDirty() != bPackageDirty)
   return MakeValidationFailure(SelectorPath,
    TEXT("AITypedPreimage.BlackboardSelectorRollbackReadbackMismatch"),
    TEXT("The selector package dirty preimage did not restore."));
  // The kernel still owns this action while earlier factory inverses run.
  // Only a fully restored aggregate may release its target and nested states.
  Selectors.Reset();
  Graph.Reset();
  Asset.Reset();
  Package.Reset();
  return Result;
 }

private:
 static FAssetDocumentContractValidationResult BindingFailure()
 {
  return MakeValidationFailure(SelectorPath,
   TEXT("AITypedPreimage.BlackboardSelectorRollbackIdentityMismatch"),
   TEXT("The selector graph, node or owner binding changed."));
 }
 TStrongObjectPtr<UObject> Asset;
 TStrongObjectPtr<UPackage> Package;
 FString AssetPath;
 FString PackageName;
 uint64 AssetRevision;
 int32 PackageId;
 bool bPackageDirty;
 TStrongObjectPtr<UBehaviorTreeGraph> Graph;
 FGuid GraphGuid;
 TArray<FCapturedSelector> Selectors;
};

FAssetDocumentEditorTypedPreimageResult CaptureInventory(
 const FAssetDocumentEditorAssetIdentityView& Identity,
 TConstArrayView<FAssetDocumentAISelectorBatchPreimageEntry> Entries,
 TArray<FCapturedSelector>& OutSelectors)
{
 UBehaviorTree* Tree = Cast<UBehaviorTree>(&Identity.GetObject());
 UBehaviorTreeGraph* Graph = Tree != nullptr ? Cast<UBehaviorTreeGraph>(Tree->BTGraph) : nullptr;
 if (!IsAdmittedObject(Graph) || Graph->GetOuter() != Tree || !Graph->GraphGuid.IsValid())
  return MakePreimageFailure(SelectorPath, TEXT("AITypedPreimage.BlackboardSelectorRequired"),
   TEXT("An exact BehaviorTree graph identity is required."));
 for (int32 Index = 0; Index < Entries.Num(); ++Index)
 {
  const FAssetDocumentAISelectorBatchPreimageEntry& Entry = Entries[Index];
  const bool bOrdered = Index == 0
   || Entries[Index - 1].StableNodeId.Compare(Entry.StableNodeId, ESearchCase::CaseSensitive) < 0
   || (Entries[Index - 1].StableNodeId == Entry.StableNodeId
    && Entries[Index - 1].PropertyPath.Compare(Entry.PropertyPath, ESearchCase::CaseSensitive) < 0);
  if (!bOrdered || Entry.StableNodeId.IsEmpty() || Entry.PropertyPath.IsEmpty()
   || Entry.ExactOwner == nullptr
   || (Entry.NestedRebind ? FString(Entry.SelectorPropertyHandle->GetPropertyPath())
    : Entry.SelectorPropertyHandle->GeneratePathToProperty()) != Entry.PropertyPath)
   return MakePreimageFailure(SelectorPath,
    TEXT("AITypedPreimage.BlackboardSelectorRequired"),
    TEXT("The selector batch requires exact canonical owner/property locators."));
  UEdGraphNode* Node = nullptr;
  if (ResolveExactSelectorOwner(*Graph, Entry.StableNodeId, &Node) != Entry.ExactOwner
   || Node == nullptr || Node->GetGraph() == nullptr || !Node->GetGraph()->GraphGuid.IsValid())
   return MakePreimageFailure(SelectorPath, TEXT("AITypedPreimage.BlackboardSelectorRequired"),
    TEXT("The selector owner must match its exact graph node binding."));
  TSharedPtr<FAliasRollbackState> State;
  const FAssetDocumentEditorTypedPreimageResult Captured = CaptureSelectorState(
   Identity, *Entry.ExactOwner, Entry.PropertyRows, Entry.SelectorPropertyHandle, State, Entry.NestedRebind, Entry.NestedBinding);
  if (!Captured.bSucceeded) return Captured;
  OutSelectors.Add({Entry.StableNodeId, Entry.PropertyPath, State.ToSharedRef(),
   TStrongObjectPtr<UEdGraphNode>(Node), TStrongObjectPtr<UObject>(Entry.ExactOwner),
   TStrongObjectPtr<UEdGraph>(Node->GetGraph()), Node->NodeGuid, Node->GetGraph()->GraphGuid,
   Node->GetOuter(), Entry.ExactOwner->GetOuter(), Entry.NestedRebind, Entry.NestedBinding});
 }
 return MakePreimageSuccess();
}
} // namespace
} // namespace UE::AssetDocument::AI::TypedPreimage::Selector

FAssetDocumentEditorTypedPreimageResult
CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorBatchPreimage(
 const IAssetDocumentVerifiedHandleLease& AssetLease,
 TConstArrayView<FAssetDocumentAISelectorBatchPreimageEntry> Entries,
 IAssetDocumentAtomicTransaction& Transaction)
{
 using namespace UE::AssetDocument::AI::TypedPreimage::Selector;
 FAssetDocumentEditorTypedPreimageResult Result;
 const TArray<FString> Components = {SelectorComponent};
 const auto Failure = [&Result]()
 {
  FAssetDocumentContractValidationResult Validation;
  Validation.Diagnostics = Result.Diagnostics;
  return Validation;
 };
 const FAssetDocumentContractValidationResult IdentityResult = WithAssetDocumentEditorAssetIdentity(
  AssetLease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
  {
   Result = ValidateCommonCapture(SelectorPath, AssetLease, Identity, Transaction, Components);
   if (!Result.bSucceeded) return Failure();
   if (Identity.GetObject().GetOutermost() == nullptr)
    return MakeValidationFailure(SelectorPath,
     TEXT("AITypedPreimage.BlackboardSelectorRequired"),
     TEXT("A live same-package selector owner and exact rows are required."));
   TArray<FCapturedSelector> Captured;
   Result = CaptureInventory(Identity, Entries, Captured);
   if (!Result.bSucceeded) return Failure();
   const TSharedRef<FSelectorBatchRollbackState> State =
    MakeShared<FSelectorBatchRollbackState>(Identity, MoveTemp(Captured));
   // One fixed component excludes both a second batch and a scalar in this transaction.
   Result = EnlistAliasActions(SelectorPath, Components, State, Transaction);
   return Result.bSucceeded ? MakeValidationSuccess()
    : Failure();
  });
 if (!IdentityResult.bValid && Result.Diagnostics.IsEmpty()) Result = IdentityFailure(IdentityResult);
 return Result;
}
