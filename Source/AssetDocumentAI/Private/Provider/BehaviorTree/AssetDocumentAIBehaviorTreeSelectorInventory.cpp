// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredGraphValidation.h"
#include "TypedPreimage/AssetDocumentAISelectorTypedPreimageInternal.h"
#include "AssetDocumentEditorNativeState.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "IPropertyTypeCustomization.h"
#include "IDetailChildrenBuilder.h"
#include "DetailWidgetRow.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentAISelectorInventory
{
using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
using UE::AssetDocument::AI::TypedPreimage::Selector::ResolveExactSelectorOwner;
class FPassiveSelector final:public IPropertyTypeCustomization
{
 void CustomizeHeader(TSharedRef<IPropertyHandle> Handle,FDetailWidgetRow& Row,IPropertyTypeCustomizationUtils&) override
 {Row.NameContent()[Handle->CreatePropertyNameWidget()].ValueContent()[Handle->CreatePropertyValueWidget()];}
 void CustomizeChildren(TSharedRef<IPropertyHandle>,IDetailChildrenBuilder&,IPropertyTypeCustomizationUtils&) override {}
};
struct FInventory
{
 int32 Remaining=4096;
 TMap<FString,FAssetDocumentRuntimeValueDto> Entries;
 bool Visit(IPropertyHandle& H,UObject& NodeInstance,const FString& Id,int32 Depth)
 {
  if(Depth>64||--Remaining<0||!H.IsValidHandle())return false;
  const FProperty* P=H.GetProperty();
  if(const auto Fixed=H.AsArray();Fixed.IsValid()&&P&&P->ArrayDim>1)
  {
   uint32 Count=0;if(Fixed->GetNumElements(Count)!=FPropertyAccess::Success||Count!=uint32(P->ArrayDim))return false;
   for(uint32 I=0;I<Count;++I){const auto Element=Fixed->GetElement(I);if(!Visit(*Element,NodeInstance,Id,Depth+1))return false;}
   return true;
  }
  if(P&&CastField<FStructProperty>(P)&&CastFieldChecked<FStructProperty>(P)->Struct==FBlackboardKeySelector::StaticStruct())
  {
   const auto Key=H.GetChildHandle(TEXT("SelectedKeyName"),false);FName Name;
   // The native full path retains owning inline-object properties; GeneratePathToProperty stops at that object.
   const FString Path(H.GetPropertyPath());
   if(Path.IsEmpty()||!Key.IsValid()||Key->GetValue(Name)!=FPropertyAccess::Success)return false;
   const FString Locator=Id+TEXT("/")+Path;if(Entries.Contains(Locator))return false;
   Entries.Add(Locator,ObjectValue({TEXT("property_path"),TEXT("selected_key_name"),TEXT("stable_node_id")},
    {StringValue(Path),StringValue(Name.ToString()),StringValue(Id)}));return true;
  }
  if(P&&CastField<FObjectPropertyBase>(P))
  {
   if(!P->HasAnyPropertyFlags(CPF_InstancedReference))return true;
   const UObject* Object=nullptr;if(H.GetValue(Object)!=FPropertyAccess::Success)return false;
   if(!Object)return true;
   if(!IsValid(Object)||!Object->IsIn(&NodeInstance))return false;
  }
  const auto A=H.AsArray();const auto M=H.AsMap();const auto S=H.AsSet();
  uint32 Count=0;
  if(A.IsValid()||M.IsValid()||S.IsValid())
  {
   const auto Read=A.IsValid()?A->GetNumElements(Count):M.IsValid()?M->GetNumElements(Count):S->GetNumElements(Count);
   if(Read!=FPropertyAccess::Success||Count>4096)return false;
   for(uint32 I=0;I<Count;++I)
   {
    const auto Element=A.IsValid()?A->GetElement(I):M.IsValid()?M->GetElement(I):S->GetElement(I);
    if(!Visit(*Element,NodeInstance,Id,Depth+1))return false;
   }
   return true;
  }
  if(H.GetNumChildren(Count)!=FPropertyAccess::Success||Count>4096)return false;
  for(uint32 I=0;I<Count;++I)
  {
   const auto Child=H.GetChildHandle(I);if(!Child.IsValid())return false;
   if(Child->GetProperty()&&!IsAuthoredProperty(*Child))continue;
   if(!Visit(*Child,NodeInstance,Id,Depth+1))return false;
  }
  return true;
 }
 bool Object(UObject& Instance,const FString& Id)
 {
  auto* Module=FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));if(!Module)return false;
  auto Rows=Module->CreatePropertyRowGenerator(FPropertyRowGeneratorArgs());ConfigureReadOnlyValueOrRows(*Rows);
  Rows->RegisterInstancedCustomPropertyTypeLayout(FBlackboardKeySelector::StaticStruct()->GetFName(),
   FOnGetPropertyTypeCustomizationInstance::CreateLambda([]()->TSharedRef<IPropertyTypeCustomization>{return MakeShared<FPassiveSelector>();}));
  Rows->SetObjects({&Instance});TMap<FString,TSharedRef<IPropertyHandle>> Roots;
  TFunction<bool(const TArray<TSharedRef<IDetailTreeNode>>&)> Gather;
  Gather=[&](const auto& Nodes)
  {
   for(const auto& Node:Nodes)
   {
    if(--Remaining<0)return false;
    auto H=Node->CreatePropertyHandle();
    for(int32 D=0;H.IsValid()&&D<64;++D,H=H->GetParentHandle())
    {
     const FProperty* P=H->GetProperty();
     if(P&&P->GetOwnerProperty()==P&&P->GetOwnerClass()&&Instance.IsA(P->GetOwnerClass())&&IsAuthoredProperty(*H))
      Roots.Add(P->GetName(),H.ToSharedRef());
    }
    TArray<TSharedRef<IDetailTreeNode>> Children;Node->GetChildren(Children,true);if(!Gather(Children))return false;
   }
   return true;
  };
  if(!Gather(Rows->GetRootTreeNodes()))return false;
  for(const auto& Root:Roots)if(!Visit(*Root.Value,Instance,Id,0))return false;
  return true;
 }
 bool Graph(UBehaviorTree& Tree)
 {
  auto* G=Cast<UBehaviorTreeGraph>(Tree.BTGraph);if(!G)return !Tree.BTGraph;
  TArray<UEdGraphNode*> Pending;for(UEdGraphNode* Node:G->Nodes)Pending.Add(Node);TSet<UEdGraphNode*> Seen;
  while(!Pending.IsEmpty())
  {
   UEdGraphNode* Node=Pending.Pop(EAllowShrinking::No);if(!Node||Seen.Contains(Node))continue;Seen.Add(Node);
   if(--Remaining<0)return false;UObject* Instance=nullptr;
   if(auto* Main=Cast<UBehaviorTreeGraphNode>(Node))
   {
    if(Main->bInjectedNode)continue;Instance=Main->NodeInstance;
    for(UAIGraphNode* Child:Main->SubNodes)Pending.Add(Child);
    for(UBehaviorTreeGraphNode* Child:Main->Decorators)Pending.Add(Child);
    for(UBehaviorTreeGraphNode* Child:Main->Services)Pending.Add(Child);
    if(auto* Wrapper=Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Main))
     if(Wrapper->BoundGraph)for(UEdGraphNode* Child:Wrapper->BoundGraph->Nodes)Pending.Add(Child);
   }
   else if(auto* Leaf=Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(Node))Instance=Leaf->NodeInstance;
   if(!Instance)continue;
   const FString Id=Node->NodeGuid.ToString(EGuidFormats::Digits);
   if(ResolveExactSelectorOwner(*G,Id)!=Instance||!Object(*Instance,Id))return false;
  }
  return true;
 }
};
}
FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeSelectorInventory(
 const FAssetDocumentPrimitiveInvocation& I,IAssetDocumentHandleResolver& Handles,IAssetDocumentAtomicTransaction* Tx)
{
 using namespace AssetDocumentAISelectorInventory;
 auto Fail=[](const TCHAR* C,const TCHAR* M){return MakeAssetDocumentAIPrimitiveFailure(EAssetDocumentPrimitiveOutcomeStatus::Failed,C,M);};
 const bool Mutation=I.Phase==EAssetDocumentRuntimeRequestPhase::Apply||I.Phase==EAssetDocumentRuntimeRequestPhase::Repair;
 if(!IsInGameThread()||!GIsEditor||IsRunningCommandlet()||IsGarbageCollecting()
  ||(!Mutation&&I.Phase!=EAssetDocumentRuntimeRequestPhase::Preflight&&I.Phase!=EAssetDocumentRuntimeRequestPhase::Verify))
  return Fail(TEXT("AIProvider.InvalidContext"),TEXT("Selector inventory requires its admitted Editor phase."));
 const auto& V=I.ValueArguments;
 if(I.Version!=1||I.PrimitiveId!=TEXT("ai.behavior_tree.selector.inventory.v1")||!IsAssetDocumentAICanonicalRuntimeValue(V)
  ||V.Kind!=EAssetDocumentRuntimeValueKind::Object||V.ObjectKeys!=TArray<FString>{TEXT("behavior_tree_path")}
  ||V.Elements.Num()!=1||V.Elements[0].Kind!=EAssetDocumentRuntimeValueKind::String)
  return Fail(TEXT("AIProvider.InvalidValueShape"),TEXT("Selector inventory requires behavior_tree_path."));
 const FString& Path=V.Elements[0].StringValue;const FSoftObjectPath Target(Path);
 if(!Target.IsValid()||!Target.GetSubPathString().IsEmpty()||Target.ToString()!=Path)
  return Fail(TEXT("AIProvider.InvalidValueShape"),TEXT("The target path is not canonical."));
 if((Mutation&&!IsAssetDocumentAITransactionValid(I,Tx))||(!Mutation&&Tx))
  return Fail(TEXT("AIProvider.InvalidTransaction"),TEXT("Inventory transaction state must match its phase."));
 if(I.HandleArguments.Num()!=1||I.HandleArguments[0].Name!=TEXT("asset"))return Fail(TEXT("AIProvider.InvalidHandle"),TEXT("One read-only Asset is required."));
 FAssetDocumentHandleValidationContext C;C.RequestId=I.RequestId;C.TransactionId=I.TransactionId;C.RuntimeRevision=I.RuntimeRevision;
 C.ProviderId=TEXT("ai");C.Phase=I.Phase;C.RequiredKind=EAssetDocumentHandleKind::Asset;C.RequiredAccess=EAssetDocumentHandleAccess::ReadOnly;
 auto Lease=Handles.AcquireVerifiedLease(I.HandleArguments[0].Handle,C);if(!Lease.Lease)return Fail(TEXT("AIProvider.InvalidHandle"),TEXT("The read-only Asset lease is invalid."));
 FAssetDocumentPrimitiveOutcome Out;
 const auto Identity=WithAssetDocumentEditorAssetIdentity(*Lease.Lease,[&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  FAssetDocumentContractValidationResult Result;auto* Tree=Cast<UBehaviorTree>(&View.GetObject());
  if(!Tree||Tree->GetClass()!=UBehaviorTree::StaticClass()||View.GetCanonicalObjectPath()!=Path)
  {Out=Fail(TEXT("AIProvider.InvalidHandle"),TEXT("The lease is not the exact requested BehaviorTree."));return Result;}
  FString Before,After,Diagnostic;FInventory Inventory;
  if(!CaptureGraphIdentity(*Tree,Before,Diagnostic,EAuthoredGraphReadbackVersion::V2)||!Inventory.Graph(*Tree)
   ||!CaptureGraphIdentity(*Tree,After,Diagnostic,EAuthoredGraphReadbackVersion::V2)||Before!=After)
  {Out=Fail(TEXT("AIProvider.BehaviorTree.SelectorBatchInvalidInventory"),TEXT("The complete authored selector inventory is unavailable or changed."));return Result;}
  TArray<FString> Keys;Inventory.Entries.GetKeys(Keys);Keys.Sort();TArray<FAssetDocumentRuntimeValueDto> Entries;
  for(const auto& Key:Keys)Entries.Add(MoveTemp(Inventory.Entries.FindChecked(Key)));
  Out.Value=ObjectValue({TEXT("selectors")},{ArrayValue(MoveTemp(Entries))});
  if(!IsAssetDocumentAICanonicalRuntimeValue(Out.Value)){Out=Fail(TEXT("AIProvider.BehaviorTree.SelectorBatchInvalidInventory"),TEXT("The selector inventory exceeds its canonical budget."));return Result;}
  Out.Status=EAssetDocumentPrimitiveOutcomeStatus::Succeeded;Result.bValid=true;return Result;
 });
 return Identity.bValid||!Out.Diagnostics.IsEmpty()?Out:Fail(TEXT("AIProvider.InvalidHandle"),TEXT("The Asset lease could not enter Editor identity."));
}
