// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentAIPrimitiveProvider.h"
#include "Tests/AssetDocumentAIBehaviorTreeProviderTestSupport.h"
#include "Tests/Fixtures/AssetDocumentAISelectorInventoryTestTypes.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/BTTaskNode.h"
#include "AIGraphTypes.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "StructUtils/InstancedStruct.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
namespace AssetDocumentAISelectorInventoryTests
{
UEdGraphPin* Output(UEdGraphNode& Node){for(auto* P:Node.Pins)if(P&&P->Direction==EGPD_Output)return P;return nullptr;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAISelectorInventoryTest,
 "UEAssetDoc.AIProvider.BehaviorTree.SelectorInventory.NestedV2AndStructuralDrift",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAISelectorInventoryTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeProviderTestSupport;
 using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
 using namespace AssetDocumentAISelectorInventoryTests;
 for(bool Drift:{false,true})
 {
  TStrongObjectPtr<UPackage> Package(CreatePackage(*(TEXT("/Temp/AssetDocumentAI/SelectorInventory_")+FGuid::NewGuid().ToString(EGuidFormats::Digits))));
  TStrongObjectPtr<UBehaviorTree> Tree(NewObject<UBehaviorTree>(Package.Get(),TEXT("Tree"),RF_Public|RF_Standalone));
  auto* BB=NewObject<UBlackboardData>(Package.Get(),TEXT("BB"),RF_Public);BB->UpdatePersistentKey<UBlackboardKeyType_Object>(TEXT("Target"));Tree->BlackboardAsset=BB;
  FScopedTransaction Fixture(FText::FromString(TEXT("Selector inventory fixture")));
  auto* Graph=CastChecked<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(Tree.Get(),TEXT("Graph"),UBehaviorTreeGraph::StaticClass(),UEdGraphSchema_BehaviorTree::StaticClass()));
  Graph->GraphGuid=FGuid::NewGuid();Tree->BTGraph=Graph;Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);Graph->OnCreated();Graph->Initialize();
  auto* Root=CastChecked<UBehaviorTreeGraphNode_Root>(Graph->Nodes[0]);Root->BlackboardAsset=BB;Root->UpdateBlackboard();
  FAISchemaAction_NewNode C;C.NodeTemplate=NewObject<UBehaviorTreeGraphNode_Composite>();C.NodeTemplate->NodeInstance=NewObject<UBTComposite_Sequence>(C.NodeTemplate);
  auto* Composite=C.PerformAction(Graph,Output(*Root),FVector2f(0,100),false);
  UClass* Class=LoadClass<UBTTaskNode>(nullptr,TEXT("/Script/AssetDocumentTests.AssetDocumentSelectorTaskTestNode"));
  if(!TestNotNull(TEXT("existing nested selector fixture class"),Class)||!Composite)return false;
  FAISchemaAction_NewNode N;N.NodeTemplate=NewObject<UBehaviorTreeGraphNode_Task>();N.NodeTemplate->NodeInstance=NewObject<UBTTaskNode>(N.NodeTemplate,Class);
  auto* Node=CastChecked<UBehaviorTreeGraphNode_Task>(N.PerformAction(Graph,Output(*Composite),FVector2f(0,200),false));UObject* Instance=Node->NodeInstance;
  FAISchemaAction_NewNode FixedAction;FixedAction.NodeTemplate=NewObject<UBehaviorTreeGraphNode_Task>();FixedAction.NodeTemplate->NodeInstance=NewObject<UAssetDocumentAISelectorFixedArrayTask>(FixedAction.NodeTemplate);
  auto* FixedNode=CastChecked<UBehaviorTreeGraphNode_Task>(FixedAction.PerformAction(Graph,Output(*Composite),FVector2f(200,200),false));
  auto* FixedInstance=CastChecked<UAssetDocumentAISelectorFixedArrayTask>(FixedNode->NodeInstance);
  FixedInstance->First=NewObject<UAssetDocumentAISelectorInlineObject>(FixedInstance,TEXT("First"));
  FixedInstance->Second=NewObject<UAssetDocumentAISelectorInlineObject>(FixedInstance,TEXT("Second"));
  auto* ArrayProp=FindFProperty<FArrayProperty>(Class,TEXT("ArrayValues"));FScriptArrayHelper Array(ArrayProp,ArrayProp->ContainerPtrToValuePtr<void>(Instance));Array.AddValues(2);
  auto* MapProp=FindFProperty<FMapProperty>(Class,TEXT("MapValues"));FScriptMapHelper Map(MapProp,MapProp->ContainerPtrToValuePtr<void>(Instance));
  auto* SetProp=FindFProperty<FSetProperty>(Class,TEXT("SetValues"));FScriptSetHelper Set(SetProp,SetProp->ContainerPtrToValuePtr<void>(Instance));
  UScriptStruct* ElementType=CastFieldChecked<FStructProperty>(SetProp->ElementProp)->Struct;auto* Identity=FindFProperty<FNameProperty>(ElementType,TEXT("Identity"));
  for(const FName Key:{FName(TEXT("Zulu")),FName(TEXT("Alpha"))})
  {
   const int32 MI=Map.AddDefaultValue_Invalid_NeedsRehash();CastFieldChecked<FNameProperty>(MapProp->KeyProp)->SetPropertyValue(Map.GetKeyPtr(MI),Key);Identity->SetPropertyValue_InContainer(Map.GetValuePtr(MI),Key);
   const int32 SI=Set.AddDefaultValue_Invalid_NeedsRehash();Identity->SetPropertyValue_InContainer(Set.GetElementPtr(SI),Key);
  }
  Map.Rehash();Set.Rehash(); // Fixture construction only; production uses stock handles.
  auto HashLookup=[&]()
  {
   for(const FName Key:{FName(TEXT("Zulu")),FName(TEXT("Alpha"))})TestNotNull(TEXT("native map hash lookup retains authored key"),Map.FindValueFromHash(&Key));
   for(int32 SI=0;SI<2;++SI)TestEqual(TEXT("native set hash lookup finds its exact element"),Set.FindElementIndexFromHash(Set.GetElementPtr(SI)),SI);
  };
  HashLookup();
  TArray<FBlackboardKeySelector*> Selectors;
  for(auto& Selector:FixedInstance->Fixed)Selectors.Add(&Selector);
  Selectors.Add(&FixedInstance->First->Selector);Selectors.Add(&FixedInstance->Second->Selector);
  auto Add=[&](void* Data,const UStruct* Type){auto* P=FindFProperty<FStructProperty>(Type,TEXT("Selector"));Selectors.Add(P->ContainerPtrToValuePtr<FBlackboardKeySelector>(Data));};
  for(const TCHAR* Name:{TEXT("DirectSelector"),TEXT("OptionalSelector")})Selectors.Add(FindFProperty<FStructProperty>(Class,Name)->ContainerPtrToValuePtr<FBlackboardKeySelector>(Instance));
  auto* Nested=FindFProperty<FStructProperty>(Class,TEXT("NestedValue"));Add(Nested->ContainerPtrToValuePtr<void>(Instance),Nested->Struct);
  auto* Dynamic=FindFProperty<FStructProperty>(Class,TEXT("DynamicValue"))->ContainerPtrToValuePtr<FInstancedStruct>(Instance);Add(Dynamic->GetMutableMemory(),Dynamic->GetScriptStruct());
  for(int32 I=0;I<2;++I){Add(Array.GetRawPtr(I),ElementType);Add(Map.GetValuePtr(I),ElementType);Add(Set.GetElementPtr(I),ElementType);}
  for(auto* S:Selectors){S->SelectedKeyName=TEXT("Target");S->ResolveSelectedKey(*BB);S->SelectedKeyName=NAME_None;S->AllowNoneAsValue(true);}
  Fixture.Cancel();Package->SetDirtyFlag(false);
  FResolveExistingHandleResolver Resolver;FAssetDocumentRollbackActionComposition Composition(Resolver,{},[](EAssetDocumentRuntimeRequestPhase,const FAssetDocumentRuntimeDiagnostic&){});
  FAssetDocumentPreflightReceipt Receipt;Receipt.RequestId=FGuid::NewGuid();Receipt.RuntimeRevision=1;Receipt.ReceiptHash=TEXT("sha256:nested-selector-inventory");Receipt.bSucceeded=true;
  Receipt.CompleteFootprint={{EAssetDocumentMutationFootprintKind::UObjectState,TEXT("asset:")+Tree->GetPathName()+TEXT("/SelectorCache"),TEXT("assetdocument.ai.behavior_tree.selector_cache.v1")}};
  FNoCancellation Cancellation;auto Atomic=Composition.BeginLiveAfterPreflight(Receipt,Cancellation);if(!Atomic)return false;Resolver.SetMutationToken(Atomic->GetMutationToken());
  auto Lease=ResolveExistingMutableAssetLease(*this,*Tree,*Atomic,Resolver);if(!Lease)return false;
  auto ReadAlias=Resolver.RegisterProviderScopedAlias(Lease->GetHandle(),TEXTVIEW("ai"),EAssetDocumentHandleAccess::ReadOnly);
  auto WriteAlias=Resolver.RegisterProviderScopedAlias(Lease->GetHandle(),TEXTVIEW("ai"),EAssetDocumentHandleAccess::Mutable);if(!ReadAlias||!WriteAlias)return false;
  FAssetDocumentPrimitiveInvocation I;I.PrimitiveId=TEXT("ai.behavior_tree.selector.inventory.v1");I.Version=1;I.Phase=EAssetDocumentRuntimeRequestPhase::Repair;
  const auto& Token=Atomic->GetMutationToken();I.RequestId=Token.RequestId;I.TransactionId=Token.TransactionId;I.RuntimeRevision=Token.RuntimeRevision;
  I.HandleArguments={{TEXT("asset"),ReadAlias.GetValue()}};I.ValueArguments=ObjectValue({TEXT("behavior_tree_path")},{StringValue(Tree->GetPathName())});
  auto Provider=CreateAssetDocumentAIPrimitiveProvider();const auto Inventory=Provider->Invoke(I,Resolver,Atomic.Get());
  for(const auto& D:Inventory.Diagnostics)AddInfo(D.Code+TEXT(": ")+D.Message);
  if(!TestEqual(TEXT("native inventory succeeds"),Inventory.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded))return false;
  TSet<FString> ActualPaths;for(const auto& Selector:Inventory.Value.Elements[0].Elements)ActualPaths.Add(Selector.Elements[0].StringValue);
  for(const TCHAR* Expected:{TEXT("DirectSelector"),TEXT("OptionalSelector"),TEXT("First->Selector"),TEXT("Second->Selector"),TEXT("Fixed->Fixed[0]"),TEXT("Fixed->Fixed[1]"),TEXT("ArrayValues[0]->Selector"),TEXT("ArrayValues[1]->Selector"),TEXT("DynamicValue->Selector"),TEXT("NestedValue->Selector"),TEXT("MapValues->MapValues[0]->Selector"),TEXT("MapValues->MapValues[1]->Selector"),TEXT("SetValues->SetValues[0]->Selector"),TEXT("SetValues->SetValues[1]->Selector")})
   TestTrue(FString(TEXT("real inventory includes "))+Expected,ActualPaths.Contains(Expected));
  TestEqual(TEXT("all direct/struct/dynamic/array/map/set/fixed/inline selectors returned"),Inventory.Value.Elements[0].Elements.Num(),14);
  I.PrimitiveId=TEXT("ai.behavior_tree.selector.resolve_batch.v2");I.Version=2;I.HandleArguments={{TEXT("asset"),WriteAlias.GetValue()}};
  I.ValueArguments=ObjectValue({TEXT("behavior_tree_path"),TEXT("blackboard_path"),TEXT("selectors")},{StringValue(Tree->GetPathName()),StringValue(BB->GetPathName()),Inventory.Value.Elements[0]});
  auto Legacy=I;Legacy.PrimitiveId=TEXT("ai.behavior_tree.selector.resolve_batch.v1");Legacy.Version=1;
  TestNotEqual(TEXT("old v1 nested rejection preserved"),Provider->Invoke(Legacy,Resolver,Atomic.Get()).Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  const auto Resolved=Provider->Invoke(I,Resolver,Atomic.Get());for(const auto& D:Resolved.Diagnostics)AddInfo(D.Code+TEXT(": ")+D.Message);
  if(!TestEqual(TEXT("native inventory feeds v2 without guessed indices"),Resolved.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded))return false;
  HashLookup();
  TestTrue(TEXT("single aggregate receipt is fully enlisted"),Atomic->MarkReadyToCommit().bValid);
  if(Drift)Array.AddValue();
  const auto Rolled=Atomic->Rollback(TEXTVIEW("Test.NestedLateFailure"));
  for(const auto& Diagnostic:Rolled.Diagnostics)AddInfo(Diagnostic.Code+TEXT(": ")+Diagnostic.Message);
  TestEqual(TEXT("structural drift rejects inverse; stable late failure restores"),Rolled.bValid,!Drift);
  if(!Drift)for(auto* S:Selectors)TestEqual(TEXT("exact original stale ID restored"),S->GetSelectedKeyID(),BB->GetKeyID(TEXT("Target")));
  if(!Drift)HashLookup();
  Tree->ClearFlags(RF_Standalone);Package->SetDirtyFlag(false);
 }
 return !HasAnyErrors();
}
#endif
