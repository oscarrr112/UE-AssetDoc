// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDerivedCompletion.h"
#include "AssetDocumentRollbackAction.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeValueOrKeyInventory.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeGraphInstall.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardValueOrKeyCacheInvalidation.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredGraphValidation.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_NativeEnum.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Misc/ScopeExit.h"

using namespace UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private;
using EProjectionVersion = UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion;
namespace AIBehaviorTreeDerivedCompletionLocal
{
constexpr TCHAR DerivedComponent[] = TEXT("assetdocument.ai.behavior_tree.derived_completion.v1");
constexpr TCHAR MirrorComponent[] = TEXT("assetdocument.ai.behavior_tree.runtime_mirror.v1");
using FResult = FAssetDocumentAIBehaviorTreeDerivedCompletionResult;
using FValidation = FAssetDocumentContractValidationResult;
#if WITH_DEV_AUTOMATION_TESTS
TArray<FString> NotificationTrace;
bool FailAfterNotification = false;
EValueOrCompletionFailure ValueOrFailure = EValueOrCompletionFailure::None;
TFunction<void()> BeforeValueOrSecondObservation;
#endif
FValidation Fail(const TCHAR* Code, const TCHAR* Message)
{
 FValidation Out;
 FAssetDocumentRuntimeDiagnostic D;
 D.Code = Code; D.Message = Message; D.Path = TEXT("/BehaviorTreeDerivedCompletion");
 Out.Diagnostics.Add(MoveTemp(D)); return Out;
}
FValidation Valid() { FValidation Out; Out.bValid = true; return Out; }
void AddText(FString& Out, const FString& Text) { Out += FString::Printf(TEXT("%d:"), Text.Len()) + Text; }
void AddValue(FString& Out, const FAssetDocumentRuntimeValueDto& Value)
{
 Out += FString::Printf(TEXT("%d["), static_cast<int32>(Value.Kind));
 switch (Value.Kind)
 {
 case EAssetDocumentRuntimeValueKind::Boolean: Out += Value.BooleanValue ? TEXT("1") : TEXT("0"); break;
 case EAssetDocumentRuntimeValueKind::Integer: Out += LexToString(Value.IntegerValue); break;
 case EAssetDocumentRuntimeValueKind::Number: Out += FString::Printf(TEXT("%.17g"), Value.NumberValue); break;
 case EAssetDocumentRuntimeValueKind::String: AddText(Out, Value.StringValue); break;
 case EAssetDocumentRuntimeValueKind::Bytes:
  for (uint8 Byte : Value.BytesValue) Out += FString::Printf(TEXT("%02x"), Byte); break;
 case EAssetDocumentRuntimeValueKind::Object:
  for (int32 I = 0; I < Value.Elements.Num(); ++I) { AddText(Out, Value.ObjectKeys[I]); AddValue(Out, Value.Elements[I]); } break;
 case EAssetDocumentRuntimeValueKind::Array:
  for (const auto& Element : Value.Elements) AddValue(Out, Element); break;
 default: break;
 }
 Out += TEXT("]");
}
struct FDecorator
{
 UBTDecorator_Blackboard* Instance = nullptr;
 UEdGraphNode* Node = nullptr;
 UBehaviorTreeGraphNode_CompositeDecorator* Wrapper = nullptr;
 FString Locator;
 FString Operation;
 bool bEnum = false;
};
struct FInventory
{
 TArray<FDecorator> Decorators;
 TArray<UBehaviorTreeGraphNode_CompositeDecorator*> Wrappers;
 FString Identity;
 FString Topology;
 FString Authored;
 FString Derived;
};
bool ExactGraph(const UBehaviorTree& Tree, const UBehaviorTreeGraph& Graph)
{
 return GIsEditor && IsInGameThread() && !IsRunningCommandlet() && !IsGarbageCollecting()
  && IsValid(&Tree) && !Tree.IsTemplate() && IsValid(&Graph) && !Graph.IsTemplate()
  && Graph.GetClass() == UBehaviorTreeGraph::StaticClass() && Tree.BTGraph.Get() == &Graph
  && Graph.GetOuter() == &Tree && Graph.GetOutermost() == Tree.GetOutermost()
  && Graph.GraphGuid.IsValid() && !Graph.IsLocked();
}
// Invocation-local observation only: no derived fields or dirty ownership.
void ObserveTopology(const UEdGraphNode& Node, FString& Out)
{
 AddText(Out, Node.NodeGuid.ToString());
 Out += FString::Printf(TEXT("%d:%d;"), Node.NodePosX, Node.NodePosY);
 AddText(Out, Node.GetClass()->GetPathName());
 if (const auto* AI = Cast<UAIGraphNode>(&Node)) AddText(Out, AI->NodeInstance ? AI->NodeInstance->GetClass()->GetPathName() : FString());
 for (const UEdGraphPin* Pin : Node.Pins)
 {
  AddText(Out, Pin->PinName.ToString()); Out += FString::Printf(TEXT("%d;"), static_cast<int32>(Pin->Direction));
  for (const UEdGraphPin* Link : Pin->LinkedTo) { AddText(Out, Link->GetOwningNode()->NodeGuid.ToString()); AddText(Out, Link->PinName.ToString()); Out += LexToString(static_cast<int32>(Link->Direction)); }
 }
 if (const auto* BT = Cast<UBehaviorTreeGraphNode>(&Node))
  for (const UAIGraphNode* Sub : BT->SubNodes) ObserveTopology(*Sub, Out);
 if (const auto* Wrapper = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(&Node))
 {
  AddText(Out, Wrapper->BoundGraph->GraphGuid.ToString());
  for (const UEdGraphNode* Inner : Wrapper->BoundGraph->Nodes) ObserveTopology(*Inner, Out);
 }
}
FValidation Gather(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph, FInventory& Out, EProjectionVersion Version = EProjectionVersion::V1)
{
 FString ValidationDigest, Diagnostic;
 if (!ExactGraph(Tree, Graph) || !UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::CaptureGraphIdentity(Tree, ValidationDigest, Diagnostic, Version))
  return Fail(TEXT("AIProvider.BehaviorTree.DecoratorInventoryRejected"), TEXT("The exact live authored graph inventory is invalid."));
 for (const UEdGraphNode* Node : Graph.Nodes) ObserveTopology(*Node, Out.Topology);
 auto Add = [&](UEdGraphNode& Node, UObject* RawInstance, UBehaviorTreeGraphNode_CompositeDecorator* Wrapper)
 {
  UBTDecorator_Blackboard* Instance = Cast<UBTDecorator_Blackboard>(RawInstance);
  if (!Instance) return;
  FDecorator Row; Row.Node = &Node; Row.Instance = Instance; Row.Wrapper = Wrapper;
  Row.Locator = Tree.GetPathName() + TEXT("/") + Graph.GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
   + TEXT("/") + (Wrapper ? Wrapper->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
    + TEXT("/") + Wrapper->BoundGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower) : TEXT("direct"))
   + TEXT("/") + Node.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
  Out.Decorators.Add(MoveTemp(Row));
 };
 for (UEdGraphNode* Node : Graph.Nodes)
 {
  UBehaviorTreeGraphNode* Main = Cast<UBehaviorTreeGraphNode>(Node);
  if (!Main) continue;
  for (UBehaviorTreeGraphNode* Decorator : Main->Decorators)
  {
   if (Decorator->bInjectedNode) continue;
   if (auto* Wrapper = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Decorator))
   {
    Out.Wrappers.Add(Wrapper);
    for (UEdGraphNode* Inner : Wrapper->BoundGraph->Nodes)
     if (auto* Test = Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(Inner)) Add(*Test, Test->NodeInstance, Wrapper);
   }
   else Add(*Decorator, Decorator->NodeInstance, nullptr);
  }
 }
 Out.Decorators.Sort([](const FDecorator& A, const FDecorator& B) { return A.Locator < B.Locator; });
 Out.Wrappers.Sort([](const auto& A, const auto& B) { return A.NodeGuid < B.NodeGuid; });
 AddText(Out.Authored, Tree.GetPathName()); AddText(Out.Authored, Graph.GraphGuid.ToString());
 for (FDecorator& Row : Out.Decorators)
 {
  AddText(Out.Identity, Row.Locator); AddText(Out.Identity, Row.Instance->GetClass()->GetPathName());
  const FStructProperty* SelectorProperty = FindFProperty<FStructProperty>(UBTDecorator_BlackboardBase::StaticClass(), TEXT("BlackboardKey"));
  const FBlackboardKeySelector* Selector = SelectorProperty && SelectorProperty->Struct == FBlackboardKeySelector::StaticStruct()
   ? SelectorProperty->ContainerPtrToValuePtr<FBlackboardKeySelector>(Row.Instance) : nullptr;
  const UBlackboardData* Blackboard = Tree.GetBlackboardAsset();
  const FBlackboardEntry* Entry = Selector && Blackboard ? Blackboard->GetKey(Blackboard->GetKeyID(Selector->SelectedKeyName)) : nullptr;
  if (!Selector || !Entry || !Entry->KeyType || Selector->GetSelectedKeyID() != Blackboard->GetKeyID(Selector->SelectedKeyName)
   || Selector->SelectedKeyType != Entry->KeyType->GetClass())
   return Fail(TEXT("AIProvider.BehaviorTree.DecoratorSelectorUnresolved"), TEXT("Every decorator selector must already resolve the exact effective Blackboard key."));
  switch (Entry->KeyType->GetTestOperation())
  {
  case EBlackboardKeyOperation::Basic: Row.Operation = TEXT("BasicOperation"); break;
  case EBlackboardKeyOperation::Arithmetic: Row.Operation = TEXT("ArithmeticOperation"); break;
  case EBlackboardKeyOperation::Text: Row.Operation = TEXT("TextOperation"); break;
  default: return Fail(TEXT("AIProvider.BehaviorTree.DecoratorOperationPropertyRejected"), TEXT("The selected key has no admitted operation family."));
  }
  Row.bEnum = Entry->KeyType->GetClass() == UBlackboardKeyType_Enum::StaticClass()
   || Entry->KeyType->GetClass() == UBlackboardKeyType_NativeEnum::StaticClass();
  FAssetDocumentRuntimeValueDto Properties;
  if (!UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::BuildAuthoredProperties(*Row.Instance, Properties))
   return Fail(TEXT("AIProvider.BehaviorTree.DecoratorOperationPropertyRejected"), TEXT("Authored decorator properties are unavailable."));
  // Enum IntValue is derived, even though the generic property projection exposes CPF_Edit.
  if (Row.bEnum)
  {
   const int32 Index = Properties.ObjectKeys.IndexOfByKey(TEXT("IntValue"));
   if (Index != INDEX_NONE) { Properties.ObjectKeys.RemoveAt(Index); Properties.Elements.RemoveAt(Index); }
  }
  AddText(Out.Authored, Row.Locator); AddText(Out.Authored, Row.Instance->GetClass()->GetPathName()); AddValue(Out.Authored, Properties);
  AddText(Out.Derived, Row.Locator); AddText(Out.Derived, Row.Instance->GetStaticDescription());
  const FByteProperty* Op = FindFProperty<FByteProperty>(UBTDecorator_Blackboard::StaticClass(), TEXT("OperationType"));
  const FIntProperty* Int = FindFProperty<FIntProperty>(UBTDecorator_Blackboard::StaticClass(), TEXT("IntValue"));
  if (!Op || !Int) return Fail(TEXT("AIProvider.BehaviorTree.DecoratorCompletionMismatch"), TEXT("Native derived observations are unavailable."));
  Out.Derived += FString::Printf(TEXT("%u:%d;"), Op->GetPropertyValue_InContainer(Row.Instance), Row.bEnum ? Int->GetPropertyValue_InContainer(Row.Instance) : 0);
 }
 for (const auto* Wrapper : Out.Wrappers)
 {
  AddText(Out.Identity, Wrapper->NodeGuid.ToString()); AddText(Out.Identity, Wrapper->BoundGraph->GraphGuid.ToString());
  AddText(Out.Derived, Wrapper->NodeGuid.ToString()); AddText(Out.Derived, Wrapper->GetDescription().ToString());
  Out.Derived += Wrapper->bCanAbortFlow ? TEXT("1") : TEXT("0");
 }
 return Valid();
}
FValidation Notify(FDecorator& Row, const FString& Name)
{
 FPropertyEditorModule* Module = FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
 if (!Module) return Fail(TEXT("AIProvider.BehaviorTree.DecoratorNotificationFailed"), TEXT("PropertyEditor is unavailable."));
 auto Rows = Module->CreatePropertyRowGenerator(FPropertyRowGeneratorArgs());
 Rows->SetPropertyGenerationAllowListPaths({Name}); Rows->SetObjects({Row.Instance});
 TSharedPtr<IPropertyHandle> Found;
 TFunction<void(const TArray<TSharedRef<IDetailTreeNode>>&)> Find = [&](const auto& Nodes)
 {
  for (const auto& Node : Nodes)
  {
   auto Handle = Node->CreatePropertyHandle();
   if (Handle && Handle->GetProperty() && Handle->GetProperty()->GetName() == Name)
   {
    TArray<UObject*> Outers; Handle->GetOuterObjects(Outers);
    if (Outers.Num() == 1 && Outers[0] == Row.Instance) Found = Handle;
   }
   TArray<TSharedRef<IDetailTreeNode>> Children; Node->GetChildren(Children, true); Find(Children);
  }
 };
 Find(Rows->GetRootTreeNodes());
 const FProperty* Exact = FindFProperty<FProperty>(UBTDecorator_Blackboard::StaticClass(), *Name);
 if (!Found || !Found->IsValidHandle() || Found->GetProperty() != Exact
  || (Name == TEXT("StringValue") ? !CastField<FStrProperty>(Exact) : !CastField<FByteProperty>(Exact)))
  return Fail(TEXT("AIProvider.BehaviorTree.DecoratorOperationPropertyRejected"), TEXT("The exact typed decorator operation handle is unavailable."));
 Found->NotifyPreChange(); Found->NotifyPostChange(EPropertyChangeType::ValueSet); Found->NotifyFinishedChangingProperties();
#if WITH_DEV_AUTOMATION_TESTS
 NotificationTrace.Add(Row.Locator + TEXT("/") + Name);
 if (FailAfterNotification)
 {
  FailAfterNotification = false;
  return Fail(TEXT("AIProvider.BehaviorTree.DecoratorNotificationFailed"), TEXT("Injected failure after the first notification."));
 }
#endif
 return Valid();
}
FValidation VerifyProjection(const FInventory& Inventory, const UBehaviorTree& Tree)
{
 for (const auto& Row : Inventory.Decorators)
 {
  const auto* Active = FindFProperty<FByteProperty>(UBTDecorator_Blackboard::StaticClass(), *Row.Operation);
  const auto* Derived = FindFProperty<FByteProperty>(UBTDecorator_Blackboard::StaticClass(), TEXT("OperationType"));
  if (!Active || !Derived || Active->GetPropertyValue_InContainer(Row.Instance) != Derived->GetPropertyValue_InContainer(Row.Instance))
   return Fail(TEXT("AIProvider.BehaviorTree.DecoratorCompletionMismatch"), TEXT("Native operation projection disagrees with the active authored operation."));
  if (Row.bEnum)
  {
   const auto* Entry = Tree.GetBlackboardAsset()->GetKey(Tree.GetBlackboardAsset()->GetKeyID(Row.Instance->GetSelectedBlackboardKey()));
   const UEnum* Enum = Entry->KeyType->GetClass() == UBlackboardKeyType_Enum::StaticClass()
    ? CastChecked<UBlackboardKeyType_Enum>(Entry->KeyType)->EnumType.Get()
    : CastChecked<UBlackboardKeyType_NativeEnum>(Entry->KeyType)->EnumType.Get();
   const auto* String = FindFProperty<FStrProperty>(UBTDecorator_Blackboard::StaticClass(), TEXT("StringValue"));
   const auto* Int = FindFProperty<FIntProperty>(UBTDecorator_Blackboard::StaticClass(), TEXT("IntValue"));
   if (!Enum || !String || !Int || Enum->GetValueByNameString(String->GetPropertyValue_InContainer(Row.Instance))
    != Int->GetPropertyValue_InContainer(Row.Instance))
    return Fail(TEXT("AIProvider.BehaviorTree.DecoratorCompletionMismatch"), TEXT("Native enum projection disagrees with authored StringValue."));
  }
 }
 return Valid();
}
FValidation RecomputeDecorators(FInventory& Inventory)
{
 for (FDecorator& Row : Inventory.Decorators)
 {
  if (Row.bEnum) { const auto R = Notify(Row, TEXT("StringValue")); if (!R.bValid) return R; }
  const auto R = Notify(Row, Row.Operation); if (!R.bValid) return R;
 }
 for (auto* Wrapper : Inventory.Wrappers) Wrapper->OnInnerGraphChanged();
 return Valid();
}
FResult Evidence(const FInventory& Inventory)
{
 FResult Out; Out.bSucceeded = true; Out.DecoratorCount = Inventory.Decorators.Num();
 for (const auto& Row : Inventory.Decorators) if (Row.Wrapper) ++Out.BoundDecoratorCount;
 Out.AuthoredDecoratorDigest = HashEvidence(Inventory.Authored); Out.DerivedSemanticDigest = HashEvidence(Inventory.Derived); return Out;
}
namespace ValueOr = UE::AssetDocumentAI::BehaviorTreeValueOrKey;
FValidation CaptureValueOr(UBehaviorTree& Tree, ValueOr::FInventory& Out, EProjectionVersion Version)
{
 FString Diagnostic;
 if (!ValueOr::Capture(Tree, Out, Diagnostic, Version)) return Fail(*Diagnostic, TEXT("The complete CurrentGraph ValueOr inventory was rejected."));
 return Valid();
}
// The sole approved BT sink. Only the immutable top mode invokes this operation.
FValidation CompleteExactCurrentGraphValueOrInventory(UBehaviorTree& Tree, const ValueOr::FInventory& Inventory)
{
 using namespace AssetDocumentAIBlackboardValueOrKeyCacheInvalidation;
 FString Diagnostic;
 const bool Completed = ValueOr::VisitExactLeaves(Tree, Inventory,
  [](ValueOr::FExactLeafReference& Reference, const ValueOr::FLeaf&)
  {
   return Visit([](auto* Leaf)
   {
    using FExact = typename TRemovePointer<decltype(Leaf)>::Type;
    FResolvedExactValueOrLeaf Resolved(*Leaf, *FExact::StaticStruct());
    return InvalidateExactValueOrBlackboardKeyCache(Resolved) == EExactValueOrKeyCacheInvalidationResult::Success;
   }, Reference);
  }, Diagnostic);
 return Completed ? Valid() : Fail(TEXT("AIProvider.BehaviorTree.ValueOrInvalidationFailed"), TEXT("The exact CurrentGraph leaf could not be invalidated."));
}
FValidation VerifyValueOr(const ValueOr::FInventory& Before, const ValueOr::FInventory& After)
{
 if (Before.ProjectionVersion != After.ProjectionVersion || Before.SemanticAuthoredDigest != After.SemanticAuthoredDigest || Before.TypedDigest != After.TypedDigest
  || Before.MappingDigest != After.MappingDigest || Before.GraphIdentity != After.GraphIdentity
  || Before.FamilyCounts != After.FamilyCounts || Before.Leaves.Num() != After.Leaves.Num())
  return Fail(TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"), TEXT("Completion changed the authored inventory, mapping, or visible graph semantics."));
 return Valid();
}
class FAssetDocumentAIBehaviorTreeDerivedCompletion final : public IAssetDocumentPostRollbackFinalizer
{
 TStrongObjectPtr<UBehaviorTree> Tree;
 TStrongObjectPtr<UBehaviorTreeGraph> Graph;
 const EAssetDocumentAIBehaviorTreeCompletionMode Mode;
 const EProjectionVersion ProjectionVersion;
 const bool NullableBlackboard;
 const FGuid GraphGuid;
 const FString InventoryIdentity;
 const FString ValueOrIdentity;
 const FString TreePath, GraphPath, PackagePath;
 const bool DirtyAtCapture;
 bool IntroducedDirty = false;
 TFunction<FValidation()> MirrorRollback;
public:
 FAssetDocumentAIBehaviorTreeDerivedCompletion(UBehaviorTree& T, UBehaviorTreeGraph& G,
  EAssetDocumentAIBehaviorTreeCompletionMode M, FString Identity, FString InValueOrIdentity, EProjectionVersion Version, bool Nullable = false,
  bool InitializeFromAsset = true, bool AllowEmptyAuthoredGraph = false)
  : Tree(&T), Graph(&G), Mode(M), ProjectionVersion(Version), NullableBlackboard(Nullable), GraphGuid(G.GraphGuid), InventoryIdentity(MoveTemp(Identity)),
  ValueOrIdentity(MoveTemp(InValueOrIdentity)), TreePath(T.GetPathName()), GraphPath(G.GetPathName()),
  PackagePath(T.GetOutermost()->GetPathName()), DirtyAtCapture(T.GetOutermost()->IsDirty()), MirrorRollback(CaptureMirrorRollback(T,G,InitializeFromAsset,AllowEmptyAuthoredGraph)) {}
 void ObserveForwardDirty() { IntroducedDirty |= !DirtyAtCapture && Tree->GetOutermost()->IsDirty(); }
 FStringView GetRollbackComponentId() const override { return Mode == EAssetDocumentAIBehaviorTreeCompletionMode::MirrorOnly ? MirrorComponent : DerivedComponent; }
 FValidation FinalizePostRollback(FStringView) override
 {
  if (!Tree.Get() || !Graph.Get() || !ExactGraph(*Tree, *Graph) || Graph->GraphGuid != GraphGuid
   || Tree->GetPathName() != TreePath || Graph->GetPathName() != GraphPath || Tree->GetOutermost()->GetPathName() != PackagePath)
   return Fail(TEXT("AIProvider.BehaviorTree.DecoratorCompletionMismatch"), TEXT("The CurrentGraph rollback identity changed."));
  const bool Top = Mode == EAssetDocumentAIBehaviorTreeCompletionMode::ValueOrDecoratorMirror;
  // Top-mode forward restores its own derived-only dirtiness before returning;
  // the ordinary actions' current dirty flag is authoritative at rollback.
  const bool Dirty = Tree->GetOutermost()->IsDirty() && (Top || !IntroducedDirty);
  const bool Decorators = Mode != EAssetDocumentAIBehaviorTreeCompletionMode::MirrorOnly;
  FValidation Result = Valid();
  FInventory Before;
  ValueOr::FInventory ValueBefore;
  // Capture both inventories before beginning any completion write. The restored
  // Blackboard is authoritative; the forward expected path is deliberately absent.
  if (Top)
  {
   Result = CaptureValueOr(*Tree, ValueBefore, ProjectionVersion);
   if (Result.bValid && NullableBlackboard && !Tree->BlackboardAsset
    && ValueBefore.Leaves.ContainsByPredicate([](const auto& Leaf){ return !Leaf.Key.IsNone(); }))
    Result = Fail(TEXT("AIProvider.BehaviorTree.ValueOrBlackboardMismatch"), TEXT("The restored key-bound ValueOr leaf requires a Blackboard."));
   if (Result.bValid && ValueBefore.TypedDigest != ValueOrIdentity)
    Result = Fail(TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected"), TEXT("The restored semantic leaf identities or authored schemas changed."));
  }
  if (Result.bValid && Decorators)
  {
   Result = Gather(*Tree, *Graph, Before, ProjectionVersion);
   if (Result.bValid && Before.Identity != InventoryIdentity)
    Result = Fail(TEXT("AIProvider.BehaviorTree.DecoratorInventoryRejected"), TEXT("The restored decorator inventory changed identity."));
  }
  if (Result.bValid && Top) Result = CompleteExactCurrentGraphValueOrInventory(*Tree, ValueBefore);
  if (Result.bValid && Decorators) Result = RecomputeDecorators(Before);
  if (Result.bValid) Result = MirrorRollback();
  if (Result.bValid && Decorators)
  {
   FInventory After; Result = Gather(*Tree, *Graph, After, ProjectionVersion);
   if (Result.bValid) Result = VerifyProjection(After, *Tree);
   if (Result.bValid && (Before.Authored != After.Authored || Before.Identity != After.Identity || Before.Topology != After.Topology))
    Result = Fail(TEXT("AIProvider.BehaviorTree.DecoratorCompletionMismatch"), TEXT("Rollback completion changed authored decorators or visible topology."));
  }
  if (Result.bValid && Top)
  {
   ValueOr::FInventory After; Result = CaptureValueOr(*Tree, After, ProjectionVersion);
   if (Result.bValid) Result = VerifyValueOr(ValueBefore, After);
  }
  Tree->GetOutermost()->SetDirtyFlag(Dirty);
  return Result;
 }
};

bool IsEmptyInstalledOldGraph(UBehaviorTreeGraph& Graph)
{
 return IsExactEmptyAuthoredGraph(Graph);
}
bool CaptureEmptyInstalledOldGraph(UBehaviorTree& Tree, FString& Digest)
{
 FString Identity, Diagnostic;
 FAssetDocumentRuntimeValueDto Projection; bool Overflow = false;
 return ValidateInstalledGraphOwnership(Tree).bValid
  && UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::CaptureGraphIdentity(Tree, Identity, Diagnostic, EProjectionVersion::V2)
  && UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::BuildAuthoredGraphProjection(Tree, Projection, Overflow, EProjectionVersion::V2)
  && !Overflow
  && ValueOr::HashAuthoredGraphValue(Tree.GetPathName(), Projection, Digest, EProjectionVersion::V2);
}
class FInstalledReplacementFinalizer final : public IAssetDocumentPostRollbackFinalizer
{
 const EProjectionVersion ProjectionVersion = EProjectionVersion::V2;
 TWeakObjectPtr<UBehaviorTree> Tree;
 FString TreePath, GraphPath, PackagePath, InventoryIdentity, ValueOrIdentity, AuthoredDigest;
 FGuid GraphGuid;
 bool OldNull, Empty;
public:
 FInstalledReplacementFinalizer(UBehaviorTree& T, UBehaviorTreeGraph* G, bool IsEmpty,
  FString Identity, FString TypedIdentity, FString SemanticDigest)
  : Tree(&T), TreePath(T.GetPathName()), GraphPath(G ? G->GetPathName() : FString()),
  PackagePath(T.GetPackage()->GetPathName()), InventoryIdentity(MoveTemp(Identity)),
  ValueOrIdentity(MoveTemp(TypedIdentity)), AuthoredDigest(MoveTemp(SemanticDigest)),
  GraphGuid(G ? G->GraphGuid : FGuid()), OldNull(!G), Empty(IsEmpty) {}
 FStringView GetRollbackComponentId() const override { return DerivedComponent; }
 FValidation FinalizePostRollback(FStringView Reason) override
 {
  // Creation/installation ordinary actions own null/absence restoration. This
  // branch must not retain or dereference the newly-created target.
  if (OldNull) return Valid();
  UBehaviorTree* T = Tree.Get();
  auto* G = T ? Cast<UBehaviorTreeGraph>(T->BTGraph) : nullptr;
  if (!T || !G || !ExactGraph(*T,*G) || T->GetPathName()!=TreePath
   || T->GetPackage()->GetPathName()!=PackagePath || G->GetPathName()!=GraphPath || G->GraphGuid!=GraphGuid)
   return Fail(TEXT("AIProvider.BehaviorTree.GraphInstallRollbackFailed"), TEXT("The restored installed-replacement target or graph is not the captured old identity."));
  if (Empty)
  {
   FString Before, After;
   if (!IsEmptyInstalledOldGraph(*G) || !CaptureEmptyInstalledOldGraph(*T,Before) || Before!=AuthoredDigest)
    return Fail(TEXT("AIProvider.BehaviorTree.GraphInstallRollbackFailed"), TEXT("The restored empty old graph changed authored content."));
   auto Result=RecompleteGraphInstallInverse(*T);
   if (Result.bValid && (!CaptureEmptyInstalledOldGraph(*T,After) || After!=Before))
    return Fail(TEXT("AIProvider.BehaviorTree.GraphInstallRollbackFailed"), TEXT("Empty old graph recompletion changed authored content."));
   return Result;
  }
  // INSTALL inverse verified its complete D2 preimage before earlier ordinary
  // actions ran. Their restored Blackboard is now authoritative, including enum
  // mappings. The existing completion validates identity and its own before/after
  // semantics without comparing derived values against the installation-time BB.
  // The existing completion class rebinds current restored objects and uses the
  // original sole ValueOr sink. No new invalidator caller or registration.
  FAssetDocumentAIBehaviorTreeDerivedCompletion Completion(*T,*G,
   EAssetDocumentAIBehaviorTreeCompletionMode::ValueOrDecoratorMirror,InventoryIdentity,ValueOrIdentity,ProjectionVersion);
  return Completion.FinalizePostRollback(Reason);
 }
};
}
FAssetDocumentContractValidationResult RecompleteGraphInstallInverse(UBehaviorTree& Tree)
{
 using namespace AIBehaviorTreeDerivedCompletionLocal;
 auto* Graph=Cast<UBehaviorTreeGraph>(Tree.BTGraph);
 if (!Graph || !ExactGraph(Tree,*Graph))
  return Fail(TEXT("AIProvider.BehaviorTree.GraphInstallRollbackFailed"),TEXT("The restored graph is not owned by the target."));
 const bool Dirty=Tree.GetPackage()->IsDirty();
 ON_SCOPE_EXIT { Tree.GetPackage()->SetDirtyFlag(Dirty); };
 if (IsEmptyInstalledOldGraph(*Graph))
 {
  FString Before, After;
  if (!CaptureEmptyInstalledOldGraph(Tree,Before))
   return Fail(TEXT("AIProvider.BehaviorTree.GraphInstallRollbackFailed"),TEXT("Cannot read the empty restored graph."));
  FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult Out;
  auto Result=CompleteMirror(Tree,*Graph,Out,true,true);
  if (Result.bValid && (!CaptureEmptyInstalledOldGraph(Tree,After) || After!=Before))
   return Fail(TEXT("AIProvider.BehaviorTree.GraphInstallRollbackFailed"),TEXT("Empty graph recompletion changed authored content."));
  return Result;
 }
 FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult Out;
 return CompleteMirror(Tree,*Graph,Out);
}
FAssetDocumentContractValidationResult CaptureAndRegisterInstalledReplacementCompletion(UBehaviorTree& Tree, IAssetDocumentAtomicTransaction& Transaction)
{
 using namespace AIBehaviorTreeDerivedCompletionLocal;
 auto* Graph=Cast<UBehaviorTreeGraph>(Tree.BTGraph);
 if (Tree.BTGraph && (!Graph || !ExactGraph(Tree,*Graph)))
  return Fail(TEXT("AIProvider.BehaviorTree.GraphInstallPreimageMismatch"),TEXT("The old graph is not the exact live graph."));
 if (!Graph && (Tree.RootNode || !Tree.RootDecorators.IsEmpty() || !Tree.RootDecoratorOps.IsEmpty()))
  return Fail(TEXT("AIProvider.BehaviorTree.GraphInstallPreimageMismatch"),TEXT("An old-null graph must have an empty runtime mirror."));
 FInventory Decorators; ValueOr::FInventory Values; FString Digest;
 const bool Empty=Graph && IsEmptyInstalledOldGraph(*Graph);
 if (Empty)
 {
  if (!CaptureEmptyInstalledOldGraph(Tree,Digest))
   return Fail(TEXT("AIProvider.BehaviorTree.GraphInstallPreimageMismatch"),TEXT("Cannot read the empty old graph."));
 }
 else if (Graph)
 {
  auto R=CaptureValueOr(Tree,Values,EProjectionVersion::V2); if (!R.bValid) return R;
  R=Gather(Tree,*Graph,Decorators,EProjectionVersion::V2); if (!R.bValid) return R;
  R=ValidateMirror(Tree,*Graph); if (!R.bValid) return R;
  if (!Tree.BlackboardAsset && (Values.Leaves.ContainsByPredicate([](const auto& Leaf){return !Leaf.Key.IsNone();}) || !Decorators.Decorators.IsEmpty()))
   return Fail(TEXT("AIProvider.BehaviorTree.GraphInstallPreimageMismatch"),TEXT("The old graph requires a Blackboard for recompletion."));
  Digest=Values.SemanticAuthoredDigest;
 }
 return Transaction.EnlistPostRollbackFinalizer(MakeShared<FInstalledReplacementFinalizer>(Tree,Graph,Empty,
  Decorators.Identity,Values.TypedDigest,MoveTemp(Digest)));
}
FAssetDocumentAIBehaviorTreeDerivedCompletionResult CaptureAssetDocumentBehaviorTreeDecoratorEvidence(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph)
{
 using namespace AIBehaviorTreeDerivedCompletionLocal;
 FInventory Inventory; const auto R = Gather(Tree,Graph,Inventory);
 if (R.bValid) return Evidence(Inventory);
 FResult Out; Out.Diagnostics = R.Diagnostics; return Out;
}
FAssetDocumentAIBehaviorTreeDerivedCompletionResult RunAssetDocumentBehaviorTreeDerivedCompletion(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph,
 EAssetDocumentAIBehaviorTreeCompletionMode Mode, const FAssetDocumentMutationFootprint& Footprint,
 IAssetDocumentAtomicTransaction& Transaction, const FString& ExpectedBlackboardPath, const FString& ExpectedAuthoredDecoratorDigest,
 EAssetDocumentAIBehaviorTreeCompletionLifecycle Lifecycle, EProjectionVersion ProjectionVersion, bool AllowNullBlackboard)
{
 using namespace AIBehaviorTreeDerivedCompletionLocal;
 FResult Out;
 auto Reject = [&](FValidation R) { FResult Failed; Failed.Diagnostics = MoveTemp(R.Diagnostics); return Failed; };
 const bool Top = Mode == EAssetDocumentAIBehaviorTreeCompletionMode::ValueOrDecoratorMirror;
 const bool Installed=Lifecycle==EAssetDocumentAIBehaviorTreeCompletionLifecycle::InstalledReplacement;
 const bool InitializeFromAsset=Installed || ProjectionVersion==EProjectionVersion::V1;
 const bool AllowEmptyAuthoredGraph=Top && ProjectionVersion==EProjectionVersion::V2;
 if ((Installed && !Top) || (!Installed && Lifecycle!=EAssetDocumentAIBehaviorTreeCompletionLifecycle::CurrentGraph))
  return Reject(Fail(TEXT("AIProvider.BehaviorTree.ValueOrRegistrationRejected"),TEXT("The completion lifecycle is not admitted for this mode.")));
 if (!ExactGraph(Tree,Graph) || (!Top && Mode != EAssetDocumentAIBehaviorTreeCompletionMode::MirrorOnly && Mode != EAssetDocumentAIBehaviorTreeCompletionMode::DecoratorAndMirror))
  return Reject(Fail(Top ? TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected") : TEXT("AIProvider.BehaviorTree.DecoratorInventoryRejected"), TEXT("CurrentGraph completion requires the exact live Asset graph and a closed mode.")));
 const bool Decorators = Mode != EAssetDocumentAIBehaviorTreeCompletionMode::MirrorOnly;
 const FString StableId = TEXT("asset:") + Tree.GetPathName() + (Decorators ? TEXT("/DerivedCompletion") : TEXT("/RuntimeMirror"));
 if (Footprint.Kind != EAssetDocumentMutationFootprintKind::UObjectState || Footprint.StableId != StableId
  || Footprint.RollbackComponentId != (Decorators ? DerivedComponent : MirrorComponent))
  return Reject(Fail(TEXT("AIProvider.OperationRejected"), TEXT("The terminal footprint does not match the selected descriptor and exact target.")));
 FInventory Before;
 ValueOr::FInventory ValueBefore;
 // External V1 admission stays V1. INSTALL additionally observes complete V2
 // content; this never reinterprets or substitutes the caller's expected digest.
 ValueOr::FInventory InstalledFullBefore;
 if (Decorators)
 {
  const bool ExactNull = Top && AllowNullBlackboard && ExpectedBlackboardPath.IsEmpty() && !Tree.GetBlackboardAsset();
  if (!ExactNull && (!Tree.GetBlackboardAsset() || Tree.GetBlackboardAsset()->GetPathName() != ExpectedBlackboardPath))
   return Reject(Fail(Top ? TEXT("AIProvider.BehaviorTree.ValueOrBlackboardMismatch") : TEXT("AIProvider.BehaviorTree.DecoratorSelectorUnresolved"), TEXT("The effective Blackboard does not match the request.")));
  if (Top)
  {
   auto R = CaptureValueOr(Tree, ValueBefore, ProjectionVersion); if (!R.bValid) return Reject(R);
   if (ExactNull && ValueBefore.Leaves.ContainsByPredicate([](const auto& Leaf){ return !Leaf.Key.IsNone(); }))
    return Reject(Fail(TEXT("AIProvider.BehaviorTree.ValueOrBlackboardMismatch"), TEXT("A key-bound ValueOr leaf requires a Blackboard.")));
   if (ValueBefore.AuthoredDigest != ExpectedAuthoredDecoratorDigest)
    return Reject(Fail(TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected"), TEXT("The complete authored graph digest does not match the request.")));
  }
  auto R = Gather(Tree, Graph, Before, ProjectionVersion); if (!R.bValid) return Reject(R);
  if (!Top && HashEvidence(Before.Authored) != ExpectedAuthoredDecoratorDigest)
   return Reject(Fail(TEXT("AIProvider.BehaviorTree.DecoratorInventoryRejected"), TEXT("The authored decorator digest does not match the request.")));
 }
 if (Top)
 {
  if (Installed && ProjectionVersion == EProjectionVersion::V1)
  {
   auto FullAdmission = CaptureValueOr(Tree, InstalledFullBefore, EProjectionVersion::V2);
   if (!FullAdmission.bValid) return Reject(FullAdmission);
  }
  auto MirrorAdmission = ValidateMirror(Tree, Graph, AllowEmptyAuthoredGraph);
  if (!MirrorAdmission.bValid) return Reject(Fail(TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected"), TEXT("The current graph cannot complete its runtime mirror.")));
 }
 TSharedPtr<FAssetDocumentAIBehaviorTreeDerivedCompletion> Finalizer;
 auto R = Valid();
 if (!Installed)
 {
  Finalizer=MakeShared<FAssetDocumentAIBehaviorTreeDerivedCompletion>(Tree, Graph, Mode, Before.Identity, ValueBefore.TypedDigest, ProjectionVersion, AllowNullBlackboard, InitializeFromAsset, AllowEmptyAuthoredGraph);
  R=Transaction.EnlistPostRollbackFinalizer(Finalizer.ToSharedRef());
 }
 auto RegistrationFailure = [&](FValidation Failure)
 {
  auto Combined = Fail(TEXT("AIProvider.OperationRejected"), TEXT("Atomic rejected the terminal completion registration."));
  Combined.Diagnostics.Append(Failure.Diagnostics); return Reject(Combined);
 };
 if (!R.bValid) return Top ? RegistrationFailure(R) : Reject(R);
 R = Transaction.EnlistFootprint(MakeArrayView(&Footprint, 1)); if (!R.bValid) return Top ? RegistrationFailure(R) : Reject(R);
 const bool DirtyBeforeCompletion = Tree.GetOutermost()->IsDirty();
 ON_SCOPE_EXIT { if (Top) Tree.GetOutermost()->SetDirtyFlag(DirtyBeforeCompletion); };
 if (Installed && Top)
 {
  // INSTALL may bind the graph while Create still has a null BB header. After
  // the admitted late header/selector assignment, initialize only stock Wait's
  // predicted hidden Key transition. No project lifecycle precedes the baseline.
  const auto& FullBefore = ProjectionVersion == EProjectionVersion::V1 ? InstalledFullBefore : ValueBefore;
  ValueOr::FInventory Stable; FString Diagnostic;
  if (!ValueOr::InitializeInstalledStockWaitParameters(Tree, FullBefore, Stable, Diagnostic))
   return Reject(Fail(*Diagnostic, TEXT("The exact installed stock Wait initialization did not match its complete predicted evidence.")));
  if (ProjectionVersion == EProjectionVersion::V1)
  {
   InstalledFullBefore = MoveTemp(Stable);
   R = CaptureValueOr(Tree, ValueBefore, ProjectionVersion); if (!R.bValid) return Reject(R);
   if (ValueBefore.AuthoredDigest != ExpectedAuthoredDecoratorDigest)
    return Reject(Fail(TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"), TEXT("Stock initialization changed the admitted V1 authored projection.")));
  }
  else ValueBefore = MoveTemp(Stable);
 }
 if (Top) { R = CompleteExactCurrentGraphValueOrInventory(Tree, ValueBefore); if (Finalizer) Finalizer->ObserveForwardDirty(); if (!R.bValid) return Reject(R); }
 if (Decorators) { R = RecomputeDecorators(Before); if (Finalizer) Finalizer->ObserveForwardDirty(); if (!R.bValid) return Reject(R); }
#if WITH_DEV_AUTOMATION_TESTS
 if (Top && ValueOrFailure == EValueOrCompletionFailure::BeforeMirror)
 {
  ValueOrFailure = EValueOrCompletionFailure::None;
  return Reject(Fail(TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"), TEXT("Injected failure before mirror completion.")));
 }
#endif
 R = CompleteMirror(Tree,Graph,Out.Mirror,InitializeFromAsset,AllowEmptyAuthoredGraph); if (Finalizer) Finalizer->ObserveForwardDirty();
 if (!R.bValid) return Top ? Reject(Fail(TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"), TEXT("The runtime mirror could not complete."))) : Reject(R);
 if (Decorators)
 {
  FInventory After; R = Gather(Tree,Graph,After, ProjectionVersion); if (!R.bValid) return Reject(R);
  R = VerifyProjection(After, Tree); if (!R.bValid) return Reject(R);
  if (Before.Identity != After.Identity || Before.Authored != After.Authored || Before.Topology != After.Topology)
   return Reject(Fail(TEXT("AIProvider.BehaviorTree.DecoratorCompletionMismatch"), TEXT("Completion changed authored decorators or visible topology.")));
  auto Snapshot = Evidence(After); Snapshot.Mirror = Out.Mirror; Out = MoveTemp(Snapshot);
 }
 if (Top)
 {
#if WITH_DEV_AUTOMATION_TESTS
  if (BeforeValueOrSecondObservation)
  {
   auto Hook = MoveTemp(BeforeValueOrSecondObservation);
   BeforeValueOrSecondObservation = {};
   Hook();
  }
  if (ValueOrFailure == EValueOrCompletionFailure::BeforeSecondObservation)
  {
   ValueOrFailure = EValueOrCompletionFailure::None;
   return Reject(Fail(TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"), TEXT("Injected failure before final observation.")));
  }
#endif
  ValueOr::FInventory After; R = CaptureValueOr(Tree, After, ProjectionVersion);
  if (!R.bValid) return Reject(Fail(TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"), TEXT("The second complete inventory observation failed.")));
  FInventory FinalDecorators;
  R = Gather(Tree, Graph, FinalDecorators, ProjectionVersion); if (!R.bValid) return Reject(R);
  R = VerifyProjection(FinalDecorators, Tree); if (!R.bValid) return Reject(R);
  R = VerifyValueOr(ValueBefore, After); if (!R.bValid) return Reject(R);
  if (Installed && ProjectionVersion == EProjectionVersion::V1)
  {
   ValueOr::FInventory InstalledFullAfter;
   R = CaptureValueOr(Tree, InstalledFullAfter, EProjectionVersion::V2); if (!R.bValid) return Reject(R);
   R = VerifyValueOr(InstalledFullBefore, InstalledFullAfter); if (!R.bValid) return Reject(R);
  }
  Out.ValueOrLeafCount = After.Leaves.Num();
  for (int32 I = 0; I < After.FamilyCounts.Num(); ++I)
   Out.ValueOrFamilyCounts.Add(ValueOr::FamilyName(static_cast<ValueOr::EFamily>(I)), After.FamilyCounts[I]);
  Out.AuthoredInventoryDigest = After.AuthoredDigest; Out.ResolvedMappingDigest = After.MappingDigest;
 }
 Out.bSucceeded = true; return Out;
}
namespace UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private
{
bool IsExactEmptyAuthoredGraph(const UBehaviorTreeGraph& Graph)
{
 if (!IsValid(&Graph) || Graph.GetClass()!=UBehaviorTreeGraph::StaticClass()
  || !Graph.GraphGuid.IsValid() || Graph.Nodes.IsEmpty() || Graph.Nodes.Num()>4096) return false;
 const UBehaviorTreeGraphNode_Root* Root=nullptr;
 TSet<FGuid> Guids; Guids.Add(Graph.GraphGuid);
 for (const UEdGraphNode* Node : Graph.Nodes)
 {
  if (!IsValid(Node) || Node->GetOuter()!=&Graph || Node->GetGraph()!=&Graph
   || !Node->NodeGuid.IsValid() || Guids.Contains(Node->NodeGuid)) return false;
  Guids.Add(Node->NodeGuid);
  if (const auto* Candidate=Cast<UBehaviorTreeGraphNode_Root>(Node))
  {
   if (Root || Candidate->GetClass()!=UBehaviorTreeGraphNode_Root::StaticClass()
    || Candidate->ParentNode || Candidate->NodeInstance || !Candidate->SubNodes.IsEmpty()
    || !Candidate->Decorators.IsEmpty() || !Candidate->Services.IsEmpty()
    || Candidate->Pins.Num()!=1) return false;
   const UEdGraphPin* Pin=Candidate->Pins[0];
   if (!Pin || !Pin->PinId.IsValid() || Pin->GetOwningNode()!=Candidate
    || Pin->Direction!=EGPD_Output || !Pin->LinkedTo.IsEmpty()) return false;
   Root=Candidate;
  }
  else if (!Node->IsA<UEdGraphNode_Comment>() || !Node->Pins.IsEmpty()) return false;
 }
 return Root!=nullptr;
}
#if WITH_DEV_AUTOMATION_TESTS
void SetValueOrCompletionFailureForTest(EValueOrCompletionFailure Point) { using namespace AIBehaviorTreeDerivedCompletionLocal; ValueOrFailure = Point; }
void SetBeforeValueOrSecondObservationForTest(TFunction<void()> Hook) { using namespace AIBehaviorTreeDerivedCompletionLocal; BeforeValueOrSecondObservation = MoveTemp(Hook); }
void ResetNotificationTraceForTest() { using namespace AIBehaviorTreeDerivedCompletionLocal; NotificationTrace.Reset(); }
void SetFailAfterFirstNotificationForTest(bool Fail) { using namespace AIBehaviorTreeDerivedCompletionLocal; FailAfterNotification = Fail; }
TArray<FString> GetNotificationTraceForTest() { using namespace AIBehaviorTreeDerivedCompletionLocal; return NotificationTrace; }
#endif
}
