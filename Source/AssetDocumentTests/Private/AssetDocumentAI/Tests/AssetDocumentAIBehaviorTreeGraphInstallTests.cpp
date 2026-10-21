// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentAIBehaviorTreeProviderTestSupport.h"
#include "Fixtures/AssetDocumentAIReadbackTestTypes.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeGraphInstall.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeValueOrKeyInventory.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "EdGraphNode_Comment.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Tasks/BTTask_WaitBlackboardTime.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Decorators/BTDecorator_ForceSuccess.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "AIGraphSchema.h"
#include "Settings/EditorStyleSettings.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Editor.h"
#include "Engine/World.h"
#include "UObject/SavePackage.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "Utils/AssetDocumentEngineCompat.h"

namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests
{
FString ProducerDigest(const FString& Path,const FAssetDocumentRuntimeValueDto& Value);
}

namespace AssetDocumentAIBehaviorTreeAuthoredGraphReadbackTests
{
UBehaviorTreeDecoratorGraphNode_Logic* AddEmptyReadbackLogic(UBehaviorTreeDecoratorGraph& Target, int32 Count);
}

namespace AssetDocumentAIBehaviorTreeGraphInstallTests
{
using namespace AssetDocumentAIBehaviorTreeProviderTestSupport;
FAssetDocumentRuntimeValueDto String(FString Text)
{
 FAssetDocumentRuntimeValueDto Value;
 Value.Kind=EAssetDocumentRuntimeValueKind::String;
 Value.StringValue=MoveTemp(Text);
 return Value;
}
FAssetDocumentRuntimeValueDto Number(double Input)
{
 FAssetDocumentRuntimeValueDto Value;
 Value.Kind=EAssetDocumentRuntimeValueKind::Number;
 Value.NumberValue=Input;
 return Value;
}
FAssetDocumentRuntimeValueDto Object(TArray<FString> Keys,TArray<FAssetDocumentRuntimeValueDto> Values)
{
 FAssetDocumentRuntimeValueDto Value;
 Value.Kind=EAssetDocumentRuntimeValueKind::Object;
 Value.ObjectKeys=MoveTemp(Keys);
 Value.Elements=MoveTemp(Values);
 return Value;
}
const FAssetDocumentRuntimeValueDto* Field(const FAssetDocumentRuntimeValueDto& Value,const TCHAR* Name)
{
 const int32 Index=Value.ObjectKeys.IndexOfByKey(Name);
 return Value.Elements.IsValidIndex(Index)?&Value.Elements[Index]:nullptr;
}
bool StockWaitAuthoredBytes(UBTTask_WaitBlackboardTime& Wait,TArray<uint8>& Bytes)
{
 using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
 FAssetDocumentRuntimeValueDto Visible;Bytes.Reset();
 if(!BuildAuthoredProperties(Wait,Visible,nullptr,EAuthoredGraphReadbackVersion::V2))return false;
 return EncodeAuthoredCanonicalValue(ArrayValue({MoveTemp(Visible),
  ArrayValue({StringValue(Wait.WaitTime.GetKey().ToString()),NumberValue(Wait.WaitTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr)))}),
  ArrayValue({StringValue(Wait.RandomDeviation.GetKey().ToString()),NumberValue(Wait.RandomDeviation.GetValue(static_cast<const UBlackboardComponent*>(nullptr)))})}),Bytes);
}
bool Success(FAutomationTestBase& Test,const FAssetDocumentPrimitiveOutcome& Outcome,const TCHAR* Label)
{
 for(const auto& Diagnostic:Outcome.Diagnostics)Test.AddInfo(FString(Label)+TEXT(": ")+Diagnostic.Code+TEXT(": ")+Diagnostic.Message);
 return Test.TestEqual(Label,Outcome.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
}
bool Failure(FAutomationTestBase& Test,const FAssetDocumentPrimitiveOutcome& Outcome,const TCHAR* Code)
{
 for(const auto& Diagnostic:Outcome.Diagnostics)
 {
  Test.AddInfo(Diagnostic.Code+TEXT(": ")+Diagnostic.Message);
  for(const auto& Context:Diagnostic.Context)
   if(Context.Key==TEXT("nested_diagnostics")&&Context.Value.Kind==EAssetDocumentRuntimeValueKind::Array)
    for(const auto& Nested:Context.Value.Elements)
     if(Nested.Kind==EAssetDocumentRuntimeValueKind::String)Test.AddInfo(Nested.StringValue);
 }
 bool Result=Test.TestTrue(TEXT("closed provider diagnostic"),Outcome.Diagnostics.ContainsByPredicate([&](const auto& D){return D.Code==Code;}));
 Result&=Test.TestTrue(TEXT("failure has no partial value, handles or authored changes"),Outcome.Value.Kind==EAssetDocumentRuntimeValueKind::Object&&Outcome.Value.ObjectKeys.IsEmpty()&&Outcome.Value.Elements.IsEmpty()&&Outcome.HandleResults.IsEmpty()&&Outcome.ChangedSet.IsEmpty());
 return Result;
}
TOptional<FAssetDocumentOpaqueHandle> ResultHandle(const FAssetDocumentPrimitiveOutcome& Outcome,const TCHAR* Name)
{
 for(const auto& Result:Outcome.HandleResults)if(Result.Name==Name&&Result.Handles.Num()==1)return Result.Handles[0];
 return {};
}
bool ReadRestoredPublicD2(FAutomationTestBase& Test,UBehaviorTree& Tree,FAssetDocumentRuntimeValueDto& Value, bool V2=false)
{
 // A terminal Atomic invalidates its handles. The separate Verify dispatch
 // acquires a fresh native Editor payload and uses no mutation transaction.
 FResolveExistingHandleResolver Resolver;
 FAssetDocumentMutationToken Scope;Scope.RequestId=FGuid::NewGuid();Scope.TransactionId=FGuid::NewGuid();Scope.RuntimeRevision=1;
 Resolver.SetMutationToken(Scope);
 FAssetDocumentPrimitiveInvocation Resolve;Resolve.PrimitiveId=TEXT("editor.asset.resolve_existing.v1");Resolve.Version=1;
 Resolve.RequestId=Scope.RequestId;Resolve.TransactionId=Scope.TransactionId;Resolve.RuntimeRevision=Scope.RuntimeRevision;Resolve.Phase=EAssetDocumentRuntimeRequestPhase::Verify;
 Resolve.ValueArguments=Object({TEXT("expected_class"),TEXT("path")},{String(Tree.GetClass()->GetPathName()),String(Tree.GetPathName())});
 const auto Resolved=CreateAssetDocumentEditorPrimitiveProvider()->Invoke(Resolve,Resolver,nullptr);
 const auto Read=ResultHandle(Resolved,TEXT("asset_read"));if(!Success(Test,Resolved,TEXT("fresh Verify native Editor lease"))||!Read.IsSet())return false;
 const auto Alias=Resolver.RegisterProviderScopedAlias(Read.GetValue(),TEXTVIEW("ai"),EAssetDocumentHandleAccess::ReadOnly);if(!Alias.IsSet())return false;
 auto I=Resolve;I.PrimitiveId=V2?TEXT("ai.behavior_tree.authored_graph.readback.v2"):TEXT("ai.behavior_tree.authored_graph.readback.v1");I.Version=V2?2:1;I.ValueArguments=Object({TEXT("behavior_tree_path")},{String(Tree.GetPathName())});I.HandleArguments={{TEXT("asset"),Alias.GetValue()}};
 const auto Outcome=CreateAssetDocumentAIPrimitiveProvider()->Invoke(I,Resolver,nullptr);
 if(!Success(Test,Outcome,TEXT("public terminal D2 observation")))return false;Value=Outcome.Value;return true;
}
UEdGraphPin* OutputPin(UEdGraphNode& Node)
{
 for(UEdGraphPin* Pin:Node.Pins)if(Pin&&Pin->Direction==EGPD_Output)return Pin;
 return nullptr;
}
enum class EOldGraph { Absent,RootOnly,Complete };

struct FTarget
{
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UBehaviorTree> Tree;
 UBlackboardData* Blackboard=nullptr;
 UBehaviorTreeGraph* OldGraph=nullptr;
 FString OldGraphName;
 TArray<FGuid> OldGuids;
 TArray<UObject*> OldOwned;
 TArray<FEditedDocumentInfo> OldDocuments;
 FSoftObjectPath OldBoundDocumentPath;
 ~FTarget()
 {
  if(Tree.IsValid())Tree->ClearFlags(RF_Standalone);
  if(Blackboard)Blackboard->ClearFlags(RF_Standalone);
  if(Package.IsValid())Package->SetDirtyFlag(false);
 }
 bool Create(FAutomationTestBase& Test,EOldGraph Mode,FString PackagePath={},bool WithBlackboard=true,bool WithBoundDocument=false)
 {
  if(PackagePath.IsEmpty())PackagePath=TEXT("/Temp/AssetDocumentAI/Install_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
  Package.Reset(CreatePackage(*PackagePath));
  Tree.Reset(NewObject<UBehaviorTree>(Package.Get(),TEXT("Tree"),RF_Public|RF_Standalone));
  if(WithBlackboard)
  {
   Blackboard=NewObject<UBlackboardData>(Package.Get(),TEXT("Blackboard"),RF_Public|RF_Standalone);
   Tree->BlackboardAsset=Blackboard;
  }
  if(Mode!=EOldGraph::Absent)
  {
   OldGraph=Cast<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(Tree.Get(),TEXT("BehaviorTreeGraph"),UBehaviorTreeGraph::StaticClass(),UEdGraphSchema_BehaviorTree::StaticClass()));
   Tree->BTGraph=OldGraph;
   OldGraph->GetSchema()->CreateDefaultNodesForGraph(*OldGraph);
   OldGraph->OnCreated();OldGraph->Initialize();
   auto* Root=Cast<UBehaviorTreeGraphNode_Root>(OldGraph->Nodes[0]);
   if(!Test.TestNotNull(TEXT("old root created by stock schema"),Root))return false;
   Root->BlackboardAsset=Blackboard;Root->UpdateBlackboard();
   if(Mode==EOldGraph::Complete)
   {
    FAISchemaAction_NewNode Composite;
    Composite.NodeTemplate=NewObject<UBehaviorTreeGraphNode_Composite>();
    Composite.NodeTemplate->NodeInstance=NewObject<UBTComposite_Sequence>(Composite.NodeTemplate);
    auto* Parent=Composite.PerformAction(OldGraph,OutputPin(*Root),FVector2f(0,100),false);
    if(!Parent)return false;
    FAISchemaAction_NewNode Task;
    Task.NodeTemplate=NewObject<UBehaviorTreeGraphNode_Task>();
    Task.NodeTemplate->NodeInstance=NewObject<UAssetDocumentAIValueOrEmptyTask>(Task.NodeTemplate);
    auto* TaskNode=Cast<UBehaviorTreeGraphNode_Task>(Task.PerformAction(OldGraph,OutputPin(*Parent),FVector2f(0,200),false));
    if(!TaskNode)return false;
    if(WithBoundDocument)
    {
     OldGraph->LockUpdates();
     FAISchemaAction_NewSubNode Bound;Bound.ParentNode=TaskNode;Bound.NodeTemplate=NewObject<UBehaviorTreeGraphNode_CompositeDecorator>();
     Bound.PerformAction(OldGraph,nullptr,FVector2f::ZeroVector,false);
     auto* Wrapper=Cast<UBehaviorTreeGraphNode_CompositeDecorator>(TaskNode->Decorators.Last());
     if(!Wrapper||!Wrapper->BoundGraph){OldGraph->UnlockUpdates();return false;}
     UEdGraph* BoundGraph=Wrapper->BoundGraph;
     FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Decorator> Creator(*BoundGraph);
     auto* Leaf=Creator.CreateNode(false);Leaf->ClassData=FGraphNodeClassData(UBTDecorator_ForceSuccess::StaticClass(),FString());Creator.Finalize();
     UEdGraphPin* Sink=nullptr;for(UEdGraphPin* Pin:BoundGraph->Nodes[0]->Pins)if(Pin&&Pin->Direction==EGPD_Input)Sink=Pin;
     const bool Connected=Sink&&OutputPin(*Leaf)&&BoundGraph->GetSchema()->TryCreateConnection(OutputPin(*Leaf),Sink);
     OldGraph->UnlockUpdates();if(!Connected)return false;
     OldBoundDocumentPath=FSoftObjectPath(BoundGraph);
     Tree->LastEditedDocuments.Add(FEditedDocumentInfo(BoundGraph,FVector2D(71,83),0.5f));
     FEditedDocumentInfo Unrelated;Unrelated.EditedObjectPath=FSoftObjectPath(OldBoundDocumentPath.ToString()+TEXT("_Unrelated"));Unrelated.SavedZoomAmount=2.0f;
     Tree->LastEditedDocuments.Add(Unrelated);
    }
   }
   OldGraph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
   OldGraphName=OldGraph->GetName();
   for(UEdGraphNode* Node:OldGraph->Nodes)OldGuids.Add(Node->NodeGuid);
   Tree->LastEditedDocuments.Add(FEditedDocumentInfo(OldGraph,FVector2D(123,456),0.75f));
  }
  Tree->LastEditedDocuments.Add(FEditedDocumentInfo(Tree.Get(),FVector2D(17,29),1.25f));
  OldDocuments=Tree->LastEditedDocuments;
  GetObjectsWithOuter(Tree.Get(),OldOwned,AssetDocumentObjects::DirectOnly);
  Package->SetDirtyFlag(false);
  return true;
 }
 bool CheckRestored(FAutomationTestBase& Test,EOldGraph Mode)
 {
  bool Result=Test.TestFalse(TEXT("inverse restores original clean package"),Package->IsDirty());
  Result&=Test.TestTrue(TEXT("inverse restores document paths and exact view parameters"),Tree->LastEditedDocuments==OldDocuments);
  TArray<UObject*> Now;GetObjectsWithOuter(Tree.Get(),Now,AssetDocumentObjects::DirectOnly);
  Result&=Test.TestEqual(TEXT("inverse leaves no incoming or generated target-owned residue"),Now.Num(),OldOwned.Num());
  if(Mode==EOldGraph::Absent)
  {
   Result&=Test.TestNull(TEXT("old-null restores BTGraph absence"),Tree->BTGraph.Get());
   Result&=Test.TestNull(TEXT("old-null publicly clears root runtime"),Tree->RootNode.Get());
   Result&=Test.TestTrue(TEXT("old-null has no root decorators or operations"),Tree->RootDecorators.IsEmpty()&&Tree->RootDecoratorOps.IsEmpty());
  }
  else
  {
   auto* Restored=Cast<UBehaviorTreeGraph>(Tree->BTGraph);
   if(!Test.TestNotNull(TEXT("old graph restored"),Restored))return false;
   Result&=Test.TestEqual(TEXT("graph name restored"),Restored->GetName(),OldGraphName);
   Result&=Test.TestTrue(TEXT("restored graph owned by exact target"),Restored->GetOuter()==Tree.Get());
   TArray<FGuid> Guids;for(UEdGraphNode* Node:Restored->Nodes)Guids.Add(Node->NodeGuid);
   Result&=Test.TestEqual(TEXT("visible persistent node locators restored"),Guids,OldGuids);
   Result&=Test.TestEqual(TEXT("runtime availability matches old graph semantics"),Tree->RootNode!=nullptr,Mode==EOldGraph::Complete);
  }
  return Result;
 }
};

struct FInstallScenario
{
 FTarget Target;
 FResolveExistingHandleResolver Resolver;
 TUniquePtr<FAssetDocumentRollbackActionComposition> Composition;
 TUniquePtr<IAssetDocumentAtomicTransaction> Kernel;
 FAssetDocumentOpaqueHandle AssetHandle,ScratchHandle,RootHandle,CompositeHandle,TaskHandle,LiveHandle;
 TStrongObjectPtr<UBehaviorTreeGraph> Incoming;
 TStrongObjectPtr<UObject> OriginalScratchOwner;
 FString OriginalScratchName;
 bool OriginalScratchTransient=false;
 FAssetDocumentRuntimeValueDto BeforeValue;
 TArray<FAssetDocumentRuntimeDiagnostic> KernelDiagnostics;
 TArray<FString> OrdinaryOrder;
 UClass* RequestedTaskClass=nullptr;
 bool bPlanEarlierBlackboardChange=false;
 bool bV2=false;
 TFunction<bool(FTarget&)> ConfigureTargetBeforeBegin;
 ~FInstallScenario()
 {
  if(Kernel&&(Kernel->GetState()==EAssetDocumentAtomicState::Live||Kernel->GetState()==EAssetDocumentAtomicState::Mutating||Kernel->GetState()==EAssetDocumentAtomicState::ReadyToCommit))Kernel->Rollback(TEXTVIEW("Test.InstallCleanup"));
 }
 FAssetDocumentPrimitiveInvocation Invocation(const TCHAR* Id,FAssetDocumentRuntimeValueDto Value,TArray<FAssetDocumentPrimitiveHandleArgument> Handles={},EAssetDocumentRuntimeRequestPhase Phase=EAssetDocumentRuntimeRequestPhase::Apply)
 {
  FAssetDocumentPrimitiveInvocation I;I.PrimitiveId=Id;I.Version=(I.PrimitiveId==TEXT("ai.behavior_tree.authored_graph.readback.v2")||I.PrimitiveId==TEXT("ai.behavior_tree.value_or_key.refresh.v2"))?2:1;
  const auto& Token=Kernel->GetMutationToken();I.RequestId=Token.RequestId;I.TransactionId=Token.TransactionId;I.RuntimeRevision=Token.RuntimeRevision;
  I.Phase=Phase;I.ValueArguments=MoveTemp(Value);I.HandleArguments=MoveTemp(Handles);return I;
 }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& I,IAssetDocumentAtomicTransaction* Through=nullptr)
 {
  Resolver.SetIssuingProviderId(TEXT("ai"));
  return CreateAssetDocumentAIPrimitiveProvider()->Invoke(I,Resolver,Through?Through:Kernel.Get());
 }
 bool ReissueAsset(FAutomationTestBase& Test)
 {
  Resolver.SetIssuingProviderId(TEXT("editor"));
  auto Lease=ResolveExistingMutableAssetLease(Test,*Target.Tree,*Kernel,Resolver);if(!Lease)return false;
  auto Alias=Resolver.RegisterProviderScopedAlias(Lease->GetHandle(),TEXTVIEW("ai"),EAssetDocumentHandleAccess::Mutable);
  if(!Alias.IsSet())return false;AssetHandle=Alias.GetValue();return true;
 }
 bool Readback(FAutomationTestBase& Test,FAssetDocumentRuntimeValueDto& Value)
 {
  auto Alias=Resolver.RegisterProviderScopedAlias(AssetHandle,TEXTVIEW("ai"),EAssetDocumentHandleAccess::ReadOnly);
  if(!Alias.IsSet())return false;
  const auto R=Invoke(Invocation(bV2?TEXT("ai.behavior_tree.authored_graph.readback.v2"):TEXT("ai.behavior_tree.authored_graph.readback.v1"),Object({TEXT("behavior_tree_path")},{String(Target.Tree->GetPathName())}),{{TEXT("asset"),Alias.GetValue()}}));
  if(!Success(Test,R,TEXT("public D2 readback")))return false;Value=R.Value;return true;
 }
 bool Begin(FAutomationTestBase& Test)
 {
  TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
  Actions.Add(MakeShared<FOrderedAuthoredAction>(TEXT("test.install.ordinary"),OrdinaryOrder,TEXT("ordinary")));
  Composition=MakeUnique<FAssetDocumentRollbackActionComposition>(Resolver,MoveTemp(Actions),[this](EAssetDocumentRuntimeRequestPhase,const FAssetDocumentRuntimeDiagnostic& D){KernelDiagnostics.Add(D);});
  FAssetDocumentPreflightReceipt Receipt;Receipt.RequestId=FGuid::NewGuid();Receipt.RuntimeRevision=1;Receipt.ReceiptHash=TEXT("sha256:install-provider-real-atomic");Receipt.bSucceeded=true;
  const FString Prefix=TEXT("asset:")+Target.Tree->GetPathName();
  Receipt.CompleteFootprint={{EAssetDocumentMutationFootprintKind::UObjectState,Prefix+TEXT("/TestOrdinary"),TEXT("test.install.ordinary")},{EAssetDocumentMutationFootprintKind::UObjectState,Prefix+TEXT("/GraphInstall"),TEXT("assetdocument.ai.behavior_tree.graph_install.v1")},{EAssetDocumentMutationFootprintKind::UObjectState,Prefix+TEXT("/DerivedCompletion"),TEXT("assetdocument.ai.behavior_tree.derived_completion.v1")}};
  Receipt.CompleteFootprint.Add({EAssetDocumentMutationFootprintKind::UObjectState,
   TEXT("scratch:ai.behavior_tree:request:")+Receipt.RequestId.ToString(EGuidFormats::Digits),
   TEXT("assetdocument.ai.behavior_tree.scratch_bundle.v1")});
  if(bPlanEarlierBlackboardChange)Receipt.CompleteFootprint.Add({EAssetDocumentMutationFootprintKind::UObjectState,Prefix+TEXT("/BlackboardAsset"),TEXT("assetdocument.ai.behavior_tree.blackboard_asset.v1")});
  if(RequestedTaskClass==UBTTask_WaitBlackboardTime::StaticClass())Receipt.CompleteFootprint.Add({EAssetDocumentMutationFootprintKind::UObjectState,Prefix+TEXT("/SelectorCache"),TEXT("assetdocument.ai.behavior_tree.selector_cache.v1")});
  static FNoCancellation Cancellation;Kernel=Composition->BeginLiveAfterPreflight(Receipt,Cancellation);if(!Kernel)return false;
  if(!Kernel->EnlistFootprint(MakeArrayView(Receipt.CompleteFootprint.GetData(),1)).bValid)return false;
  Resolver.SetMutationToken(Kernel->GetMutationToken());
  return ReissueAsset(Test);
 }
 bool CreateNode(FAutomationTestBase& Test,UClass& Class,double Y,FAssetDocumentOpaqueHandle& Handle)
 {
  const auto R=Invoke(Invocation(TEXT("ai.behavior_tree.node.create_action.v1"),Object({TEXT("node_class_path"),TEXT("position")},{String(Class.GetPathName()),Object({TEXT("x"),TEXT("y")},{Number(0),Number(Y)})}),{{TEXT("graph"),ScratchHandle}}));
  const FString Context=FString::Printf(TEXT("actual scratch node action %s at (0, %.0f)"),*Class.GetPathName(),Y);
  const auto H=ResultHandle(R,TEXT("node"));if(!Success(Test,R,*Context)||!H.IsSet())return false;Handle=H.GetValue();return true;
 }
 bool Connect(FAutomationTestBase& Test,const FAssetDocumentOpaqueHandle& From,const FAssetDocumentOpaqueHandle& To,const FString& OutName)
 {
  return Success(Test,Invoke(Invocation(TEXT("ai.behavior_tree.graph.connect.v1"),Object({TEXT("from_pin_name"),TEXT("to_pin_name")},{String(OutName),String(TEXT("In"))}),{{TEXT("graph"),ScratchHandle},{TEXT("from_node"),From},{TEXT("to_node"),To}})),TEXT("actual scratch graph connection"));
 }
 bool SetUp(FAutomationTestBase& Test,EOldGraph Mode,bool Families=false,FString PackagePath={},bool WithBlackboard=true,bool WithBoundDocument=false)
 {
  if(!Target.Create(Test,Mode,PackagePath,WithBlackboard,WithBoundDocument))return false;
  if(ConfigureTargetBeforeBegin&&!ConfigureTargetBeforeBegin(Target))return false;
  if(!Begin(Test)||!Readback(Test,BeforeValue))return false;
  const auto Created=Invoke(Invocation(TEXT("ai.behavior_tree.graph.create.v1"),Object({TEXT("behavior_tree_path"),TEXT("graph_name")},{String(Target.Tree->GetPathName()),String(TEXT("BehaviorTreeGraph"))})));
  auto Graph=ResultHandle(Created,TEXT("graph"));if(!Success(Test,Created,TEXT("actual provider scratch graph create"))||!Graph.IsSet())return false;ScratchHandle=Graph.GetValue();
  const auto Defaults=Invoke(Invocation(TEXT("ai.behavior_tree.graph.create_defaults.v1"),Object({},{}),{{TEXT("graph"),ScratchHandle}}));
  auto Root=ResultHandle(Defaults,TEXT("root_node"));if(!Success(Test,Defaults,TEXT("actual schema default root"))||!Root.IsSet())return false;RootHandle=Root.GetValue();
  FAssetDocumentAIBehaviorTreeGraphLeaseView View;TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  if(!AcquireAssetDocumentAIBehaviorTreeGraphLease(Invocation(TEXT("ai.behavior_tree.graph.create_defaults.v1"),Object({},{}),{{TEXT("graph"),ScratchHandle}}),Resolver,TEXTVIEW("graph"),View,Diagnostics))return false;
  Incoming.Reset(View.Graph);OriginalScratchOwner.Reset(Incoming->GetOuter());OriginalScratchName=Incoming->GetName();OriginalScratchTransient=Incoming->HasAnyFlags(RF_Transient);
  UClass* TaskClass=RequestedTaskClass?RequestedTaskClass:Families?UAssetDocumentAIInstallFamiliesTask::StaticClass():UAssetDocumentAIValueOrEmptyTask::StaticClass();
  // The public whole-scratch writer clears stock Root's implicit loaded BB
  // before creating instances. Preserve that ordering for this exact fixture.
  if(TaskClass==UBTTask_WaitBlackboardTime::StaticClass())
   for(UEdGraphNode* Node:Incoming->Nodes)if(auto* ScratchRoot=Cast<UBehaviorTreeGraphNode_Root>(Node))
   {ScratchRoot->BlackboardAsset=nullptr;ScratchRoot->UpdateBlackboard();}
  // Stock public actions snap coordinates; request exact positions on the configured editor grid.
  const int32 GridSize=GetDefault<UEditorStyleSettings>()->GridSnapSize;
  if(!CreateNode(Test,*UBTComposite_Sequence::StaticClass(),6*GridSize,CompositeHandle)||!CreateNode(Test,*TaskClass,12*GridSize,TaskHandle))return false;
  FString RootOut,CompositeOut;
  for(UEdGraphNode* Node:Incoming->Nodes)
  {
   if(auto* ScratchRoot=Cast<UBehaviorTreeGraphNode_Root>(Node))
   {
    // The scratch provider creates an empty tree; Blackboard selection is authored fixture data.
    ScratchRoot->BlackboardAsset=Target.Tree->BlackboardAsset;ScratchRoot->UpdateBlackboard();
    if(OutputPin(*Node))RootOut=OutputPin(*Node)->PinName.ToString();
   }
   if(Cast<UBehaviorTreeGraphNode_Composite>(Node)&&OutputPin(*Node))CompositeOut=OutputPin(*Node)->PinName.ToString();
  }
  return Connect(Test,RootHandle,CompositeHandle,RootOut)&&Connect(Test,CompositeHandle,TaskHandle,CompositeOut);
 }
 FAssetDocumentPrimitiveInvocation InstallInvocation()
 {
  const auto* Guid=Field(BeforeValue,TEXT("graph_guid"));
  return Invocation(TEXT("ai.behavior_tree.graph.install.v1"),Object({TEXT("behavior_tree_path"),TEXT("expected_current_graph_guid_or_null"),TEXT("rollback_completion_mode")},{String(Target.Tree->GetPathName()),Guid?*Guid:FAssetDocumentRuntimeValueDto(),String(TEXT("ValueOrDecoratorMirror"))}),{{TEXT("asset"),AssetHandle},{TEXT("graph"),ScratchHandle}});
 }
 FAssetDocumentPrimitiveOutcome Repair(FAutomationTestBase& Test)
 {
  FAssetDocumentRuntimeValueDto D2;if(!Readback(Test,D2))return {};
  FString Digest;
  UE::AssetDocumentAI::BehaviorTreeValueOrKey::HashAuthoredGraphValue(Target.Tree->GetPathName(),D2,Digest, bV2?UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion::V2:UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion::V1);
  return Invoke(Invocation(bV2?TEXT("ai.behavior_tree.value_or_key.refresh.v2"):TEXT("ai.behavior_tree.value_or_key.refresh.v1"),Object({TEXT("behavior_tree_path"),TEXT("completion_registration"),TEXT("expected_authored_inventory_digest"),TEXT("expected_blackboard_path")},{String(Target.Tree->GetPathName()),String(TEXT("installed_replacement")),String(Digest),String(Target.Tree->BlackboardAsset?Target.Tree->BlackboardAsset->GetPathName():FString())}),{{TEXT("asset"),AssetHandle}},EAssetDocumentRuntimeRequestPhase::Repair));
 }
 bool HandleAlive(const FAssetDocumentOpaqueHandle& Handle)
 {
  FAssetDocumentHandleValidationContext Context;const auto& Token=Kernel->GetMutationToken();Context.RequestId=Token.RequestId;Context.TransactionId=Token.TransactionId;Context.RuntimeRevision=Token.RuntimeRevision;Context.ProviderId=Handle.ProviderId;Context.Phase=EAssetDocumentRuntimeRequestPhase::Apply;Context.RequiredKind=Handle.Kind;Context.RequiredAccess=Handle.Access;
  return Resolver.AcquireVerifiedLease(Handle,Context).Lease.IsValid();
 }
 bool CheckRestoredD2(FAutomationTestBase& Test)
 {
  FAssetDocumentRuntimeValueDto After;if(!ReadRestoredPublicD2(Test,*Target.Tree,After,bV2))return false;
  return Test.TestEqual(TEXT("terminal public D2 restores complete authored properties, topology and references"),AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests::ProducerDigest(Target.Tree->GetPathName(),After),AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests::ProducerDigest(Target.Tree->GetPathName(),BeforeValue));
 }
};

// Reflection is limited to authored fixture fields and observation of the native enum discriminator.
template<typename T> T& InstallFixtureProperty(UObject& Object,const TCHAR* Name)
{
 FProperty* Property=Object.GetClass()->FindPropertyByName(Name);check(Property);
 return *Property->ContainerPtrToValuePtr<T>(&Object);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallEarlierBlackboardEnumTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.EarlierBlackboardEnumRollback",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallEarlierBlackboardEnumTest::RunTest(const FString&)
{
 FInstallScenario S;S.bPlanEarlierBlackboardChange=true;
 UBlackboardData* BlackboardB=nullptr;UBehaviorTreeGraphNode_Decorator* DecoratorWrapper=nullptr;
 S.ConfigureTargetBeforeBegin=[&](FTarget& Target)
 {
  auto MakeEnum=[&](const TCHAR* Name,int64 Second)
  {
   // Scoped (EnumClass) entries so the two fixture enums do not collide in UE's global enum-name table.
   const FString EnumName=FString(Name)+TEXT("_")+FPackageName::GetShortName(Target.Package->GetName());
   UEnum* Enum=NewObject<UEnum>(Target.Package.Get(),FName(*EnumName),RF_Public);
   TArray<TPair<FName,int64>> Names={{FName(*(EnumName+TEXT("::First"))),0},{FName(*(EnumName+TEXT("::Second"))),Second}};
   AssetDocumentEnums::SetEnums(*Enum, Names,UEnum::ECppForm::EnumClass);
   return Enum;
  };
  Target.Blackboard->UpdatePersistentKey<UBlackboardKeyType_Enum>(TEXT("Enum"))->EnumType=MakeEnum(TEXT("EnumA"),7);
  BlackboardB=NewObject<UBlackboardData>(Target.Package.Get(),TEXT("BlackboardB"),RF_Public);
  BlackboardB->UpdatePersistentKey<UBlackboardKeyType_Enum>(TEXT("Enum"))->EnumType=MakeEnum(TEXT("EnumB"),9);
  UBehaviorTreeGraphNode_Task* Task=nullptr;
  for(UEdGraphNode* Node:Target.OldGraph->Nodes)if(auto* Candidate=Cast<UBehaviorTreeGraphNode_Task>(Node))Task=Candidate;
  if(!Task)return false;
  Target.OldGraph->LockUpdates();
  FAISchemaAction_NewSubNode Action;Action.ParentNode=Task;
  Action.NodeTemplate=NewObject<UBehaviorTreeGraphNode_Decorator>();
  Action.NodeTemplate->NodeInstance=NewObject<UBTDecorator_Blackboard>(Action.NodeTemplate);
  Action.PerformAction(Target.OldGraph,nullptr,FVector2f::ZeroVector,false);
  DecoratorWrapper=Cast<UBehaviorTreeGraphNode_Decorator>(Task->Decorators.Last());
  auto* Decorator=Cast<UBTDecorator_Blackboard>(DecoratorWrapper->NodeInstance);
  auto& Selector=InstallFixtureProperty<FBlackboardKeySelector>(*Decorator,TEXT("BlackboardKey"));
  Selector.SelectedKeyName=TEXT("Enum");Selector.ResolveSelectedKey(*Target.Blackboard);
  InstallFixtureProperty<FString>(*Decorator,TEXT("StringValue"))=TEXT("Second");
  InstallFixtureProperty<uint8>(*Decorator,TEXT("ArithmeticOperation"))=uint8(EArithmeticKeyOperation::Equal);
  Target.OldGraph->UnlockUpdates();Target.OldGraph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
  Decorator=Cast<UBTDecorator_Blackboard>(DecoratorWrapper->NodeInstance);
  if(!TestEqual(TEXT("original A enum discriminator"),InstallFixtureProperty<int32>(*Decorator,TEXT("IntValue")),7))return false;
  Target.OldOwned.Reset();GetObjectsWithOuter(Target.Tree.Get(),Target.OldOwned,AssetDocumentObjects::DirectOnly);
  Target.Package->SetDirtyFlag(false);return true;
 };
 if(!S.SetUp(*this,EOldGraph::Complete))return false;
 const auto Changed=S.Invoke(S.Invocation(TEXT("ai.behavior_tree.blackboard.set.v1"),Object({TEXT("behavior_tree_path"),TEXT("blackboard_path_or_null")},{String(S.Target.Tree->GetPathName()),String(BlackboardB->GetPathName())}),{{TEXT("asset"),S.AssetHandle}}));
 if(!Success(*this,Changed,TEXT("real earlier Blackboard A to B mutation")))return false;
 TestTrue(TEXT("earlier provider selected B"),S.Target.Tree->BlackboardAsset==BlackboardB);
 // blackboard.set writes Tree before Root so the stock Root notification does not re-initialize
 // nodes; derived caches are re-derived by the refresh primitives and Repair, not by the assignment.
 TestEqual(TEXT("Blackboard assignment leaves the derived A discriminator for Repair"),InstallFixtureProperty<int32>(*DecoratorWrapper->NodeInstance,TEXT("IntValue")),7);
 // Configure the incoming authored root for the Blackboard selected by the earlier provider action.
 for(UEdGraphNode* Node:S.Incoming->Nodes)if(auto* Root=Cast<UBehaviorTreeGraphNode_Root>(Node)){Root->BlackboardAsset=BlackboardB;Root->UpdateBlackboard();}
 if(!Success(*this,S.Invoke(S.InstallInvocation()),TEXT("install after real Blackboard retarget")))return false;
 if(!Success(*this,S.Repair(*this),TEXT("installed Repair under B")))return false;
 TestTrue(TEXT("late rollback completes through real Atomic"),S.Kernel->Rollback(TEXTVIEW("Test.EarlierBlackboardLateFailure")).bValid);
 TestTrue(TEXT("earlier typed preimage restores Blackboard A"),S.Target.Tree->BlackboardAsset==S.Target.Blackboard);
 auto* RestoredDecorator=Cast<UBTDecorator_Blackboard>(DecoratorWrapper->NodeInstance);
 if(!TestNotNull(TEXT("old decorator restored at persistent wrapper"),RestoredDecorator))return false;
 TestEqual(TEXT("terminal completion derives restored A discriminator"),InstallFixtureProperty<int32>(*RestoredDecorator,TEXT("IntValue")),7);
 TestEqual(TEXT("authored enum choice remains Second"),InstallFixtureProperty<FString>(*RestoredDecorator,TEXT("StringValue")),FString(TEXT("Second")));
 S.Target.CheckRestored(*this,EOldGraph::Complete);S.CheckRestoredD2(*this);return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallInverseTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.IndependentInverseAndInstalledRepair",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallInverseTest::RunTest(const FString&)
{
 for(EOldGraph Mode:{EOldGraph::Absent,EOldGraph::RootOnly,EOldGraph::Complete})
 for(bool Complete:{false,true})
 {
  FInstallScenario S;if(!S.SetUp(*this,Mode))return false;
  FInstrumentedTransaction Proxy(*S.Kernel);
  const auto Installed=S.Invoke(S.InstallInvocation(),&Proxy);
  if(!Success(*this,Installed,TEXT("actual INSTALL descriptor")))return false;
  const auto Live=ResultHandle(Installed,TEXT("live_graph"));
  if(!TestTrue(TEXT("install returns one live graph capability"),Live.IsSet()))return false;
  S.LiveHandle=Live.GetValue();
  TestEqual(TEXT("single InstalledReplacement finalizer registered"),Proxy.FinalizerCalls,1);
  TestEqual(TEXT("one independent install action registered"),Proxy.ActionCalls,1);
  TestEqual(TEXT("only install row enlisted before Repair"),Proxy.FootprintCalls,1);
  TestTrue(TEXT("incoming graph is exact current target graph"),S.Target.Tree->BTGraph==S.Incoming.Get());
  TestTrue(TEXT("incoming graph now owned by target"),S.Incoming->GetOuter()==S.Target.Tree.Get());
  TestFalse(TEXT("installed graph is persistent"),S.Incoming->HasAnyFlags(RF_Transient));
  TestTrue(TEXT("root and unrelated document records remain mapped after same-name installation"),S.Target.Tree->LastEditedDocuments==S.Target.OldDocuments);
  TestFalse(TEXT("consumed scratch graph capability invalidated"),S.HandleAlive(S.ScratchHandle));
  TestFalse(TEXT("consumed scratch node capability invalidated"),S.HandleAlive(S.TaskHandle));
  TestTrue(TEXT("new live graph capability is usable before Repair"),S.HandleAlive(S.LiveHandle));
  TestEqual(TEXT("install publishes one authored ChangedSet entry"),Installed.ChangedSet.Num(),1);
  if(Installed.ChangedSet.Num()==1)
  {
   TestEqual(TEXT("exact install authority"),Installed.ChangedSet[0].AuthorityId,FString(TEXT("AD.AI.BT.GRAPH_INSTALL.v1")));
   TestEqual(TEXT("exact aggregate changed identity"),Installed.ChangedSet[0].StableObjectId,TEXT("asset:")+S.Target.Tree->GetPathName()+TEXT("/GraphInstall"));
  }
  if(Complete&&!Success(*this,S.Repair(*this),TEXT("Repair reuses pre-registered InstalledReplacement completion")))return false;
  if(Complete)TestNotNull(TEXT("installed Repair produces usable runtime root"),S.Target.Tree->RootNode.Get());
  if(!TestTrue(TEXT("real Atomic inverse completes with or without terminal row"),S.Kernel->Rollback(TEXTVIEW("Test.InstallLateFailure")).bValid))return false;
  TestFalse(TEXT("live graph capability invalid after inverse"),S.HandleAlive(S.LiveHandle));
  TestFalse(TEXT("scratch capability cannot revive after rollback"),S.HandleAlive(S.ScratchHandle));
  TestEqual(TEXT("real kernel closes scratch cleanup with its transaction"),S.Kernel->GetState(),EAssetDocumentAtomicState::RolledBack);
  TestEqual(TEXT("ordinary kernel action ran"),S.OrdinaryOrder,TArray<FString>{TEXT("ordinary")});
  TestTrue(TEXT("incoming ownership returns to original scratch tree"),S.Incoming->GetOuter()==S.OriginalScratchOwner.Get());
  TestEqual(TEXT("incoming original name restored"),S.Incoming->GetName(),S.OriginalScratchName);
  TestEqual(TEXT("incoming original transient bit restored"),S.Incoming->HasAnyFlags(RF_Transient),S.OriginalScratchTransient);
  if(!S.Target.CheckRestored(*this,Mode)||!S.CheckRestoredD2(*this))return false;
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallNoBlackboardTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.EmptyOldGraphWithoutBlackboard",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallNoBlackboardTest::RunTest(const FString&)
{
 for(EOldGraph Mode:{EOldGraph::Absent,EOldGraph::RootOnly})
 {
  FInstallScenario S;if(!S.SetUp(*this,Mode,false,{},false))return false;
  if(!Success(*this,S.Invoke(S.InstallInvocation()),TEXT("old empty/no-BB admission")))return false;
  TestTrue(TEXT("independent inverse requires no Blackboard for empty old graph"),S.Kernel->Rollback(TEXTVIEW("Test.NoBlackboardInverse")).bValid);
  if(!S.Target.CheckRestored(*this,Mode))return false;
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallFaultTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.PartialTransferAndHandleIssueRollback",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallFaultTest::RunTest(const FString&)
{
 using namespace UE::AssetDocumentAI::BehaviorTreeGraphInstall;
 for(auto Point:{EFailurePoint::AfterOldGraphIsolation,EFailurePoint::AfterOldRootsIsolation,EFailurePoint::AfterIncomingGraphTransfer,EFailurePoint::AfterIncomingRootsTransfer,EFailurePoint::AfterFlags,EFailurePoint::AfterGraphAssignment,EFailurePoint::BeforeHandleIssue})
 {
  FInstallScenario S;if(!S.SetUp(*this,EOldGraph::Complete))return false;
  AddInfo(FString::Printf(TEXT("transfer failure point %d"),int32(Point)));
  SetFailurePointForTest(Point);
  const auto Outcome=S.Invoke(S.InstallInvocation());
  SetFailurePointForTest(EFailurePoint::None);
  if(!Failure(*this,Outcome,Point==EFailurePoint::BeforeHandleIssue?TEXT("AIProvider.BehaviorTree.GraphInstallHandleIssueFailed"):TEXT("AIProvider.BehaviorTree.GraphInstallTransferFailed")))return false;
  if(!TestTrue(TEXT("real install inverse reverses every completed transfer prefix"),S.Kernel->Rollback(TEXTVIEW("Test.TransferPrefix")).bValid))return false;
  if(!S.Target.CheckRestored(*this,EOldGraph::Complete))return false;
 }
 FInstallScenario S;if(!S.SetUp(*this,EOldGraph::Complete))return false;
 S.Resolver.bRejectNextIssue=true;
 const auto Outcome=S.Invoke(S.InstallInvocation());
 if(!Failure(*this,Outcome,TEXT("AIProvider.BehaviorTree.GraphInstallHandleIssueFailed")))return false;
 TestTrue(TEXT("actual resolver Issue failure reverses install"),S.Kernel->Rollback(TEXTVIEW("Test.ActualIssueFailure")).bValid);
 return S.Target.CheckRestored(*this,EOldGraph::Complete)&&!HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallAdmissionTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.AdmissionAndRegistrationZeroTransfer",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallAdmissionTest::RunTest(const FString&)
{
 for(int32 Case=0;Case<4;++Case)
 {
  FInstallScenario S;if(!S.SetUp(*this,EOldGraph::Complete))return false;
  auto I=S.InstallInvocation();FInstrumentedTransaction Proxy(*S.Kernel);
  const TCHAR* Code=TEXT("AIProvider.BehaviorTree.GraphInstallPreimageMismatch");
  if(Case==0)I.ValueArguments.Elements[1]=String(FGuid::NewGuid().ToString(EGuidFormats::Digits));
  if(Case==1){Proxy.bFailFinalizer=true;Code=TEXT("AIProvider.BehaviorTree.GraphInstallRegistrationRejected");}
  if(Case==2){Proxy.bFailAction=true;Code=TEXT("AIProvider.BehaviorTree.GraphInstallRegistrationRejected");}
  if(Case==3){Proxy.bFailFootprint=true;Code=TEXT("AIProvider.BehaviorTree.GraphInstallFootprintRejected");}
  if(!Failure(*this,S.Invoke(I,&Proxy),Code))return false;
  TestTrue(TEXT("rejected install keeps old target graph"),S.Target.Tree->BTGraph==S.Target.OldGraph);
  TestTrue(TEXT("rejected install keeps incoming scratch ownership"),S.Incoming->GetOuter()==S.OriginalScratchOwner.Get());
  TestFalse(TEXT("rejected install leaves original package clean"),S.Target.Package->IsDirty());
  TestTrue(TEXT("rejected setup can roll back ordinary/scratch actions"),S.Kernel->Rollback(TEXTVIEW("Test.InstallAdmission")).bValid);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallOwnedFamilyTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.ElevenFamiliesAndNestedOwnership",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallOwnedFamilyTest::RunTest(const FString&)
{
 FInstallScenario S;if(!S.SetUp(*this,EOldGraph::Complete,true))return false;
 UAssetDocumentAIInstallFamiliesTask* Task=nullptr;
 for(UEdGraphNode* Node:S.Incoming->Nodes)if(auto* Wrapper=Cast<UBehaviorTreeGraphNode_Task>(Node))Task=Cast<UAssetDocumentAIInstallFamiliesTask>(Wrapper->NodeInstance);
 if(!TestNotNull(TEXT("actual node.create_action owns test project class"),Task)||!TestNotNull(TEXT("constructor-owned instanced child"),Task->Nested.Get()))return false;
 const FString NestedName=Task->Nested->GetName();
 if(!Success(*this,S.Invoke(S.InstallInvocation()),TEXT("install project task ownership closure")))return false;
 TestTrue(TEXT("task runtime ownership transferred to target"),Task->GetOuter()==S.Target.Tree.Get());
 TestTrue(TEXT("nested child remains owned by its task"),Task->Nested->GetOuter()==Task);
 const auto Completed=S.Repair(*this);
 if(!Success(*this,Completed,TEXT("installed eleven-family completion")))return false;
 const auto* Count=Field(Completed.Value,TEXT("leaf_count"));if(!TestNotNull(TEXT("leaf count output"),Count))return false;
 TestEqual(TEXT("all eleven families on installed project task are completed"),Count->IntegerValue,int64(11));
 TestEqual(TEXT("nested authored content retained"),Task->Nested->AuthoredValue,73);
 TestTrue(TEXT("actual installed Int32 default usable"),Task->Int32Value.GetValue(static_cast<const UBlackboardComponent*>(nullptr))==0);
 TestTrue(TEXT("family install rollback"),S.Kernel->Rollback(TEXTVIEW("Test.NestedOwnedRollback")).bValid);
 TestTrue(TEXT("nested relative ownership retained through inverse"),Task->Nested->GetOuter()==Task);
 TestEqual(TEXT("nested name retained"),Task->Nested->GetName(),NestedName);
 return S.Target.CheckRestored(*this,EOldGraph::Complete)&&!HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallPoisonTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.InverseFailurePoisonsRealAtomic",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallPoisonTest::RunTest(const FString&)
{
 using namespace UE::AssetDocumentAI::BehaviorTreeGraphInstall;
 FInstallScenario S;if(!S.SetUp(*this,EOldGraph::Complete))return false;
 if(!Success(*this,S.Invoke(S.InstallInvocation()),TEXT("install before inverse failure")))return false;
 SetFailurePointForTest(EFailurePoint::InverseTransfer);
 const auto Rollback=S.Kernel->Rollback(TEXTVIEW("Test.InverseTransferFailure"));
 SetFailurePointForTest(EFailurePoint::None);
 TestFalse(TEXT("failed inverse is not reported restored"),Rollback.bValid);
 TestEqual(TEXT("actual kernel reaches Poisoned"),S.Kernel->GetState(),EAssetDocumentAtomicState::Poisoned);
 TestTrue(TEXT("real kernel reports component restoration failure"),S.KernelDiagnostics.ContainsByPredicate([](const auto& D){return D.Code==TEXT("Atomic.Poisoned.ComponentFailed");}));
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallBoundDocumentsTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.BoundDocumentRemovalAndExactInverse",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallBoundDocumentsTest::RunTest(const FString&)
{
 FInstallScenario S;if(!S.SetUp(*this,EOldGraph::Complete,false,{},true,true))return false;
 TArray<FEditedDocumentInfo> Expected=S.Target.OldDocuments;
 Expected.RemoveAll([&](const FEditedDocumentInfo& Document){return Document.EditedObjectPath==S.Target.OldBoundDocumentPath;});
 if(!TestEqual(TEXT("fixture contains exactly one old bound document"),S.Target.OldDocuments.Num()-Expected.Num(),1))return false;
 if(!Success(*this,S.Invoke(S.InstallInvocation()),TEXT("replace old bound graph")))return false;
 TestTrue(TEXT("only exact removed bound graph document is dropped; prefix sibling and view data retained"),S.Target.Tree->LastEditedDocuments==Expected);
 if(!Success(*this,S.Repair(*this),TEXT("Repair after old bound replacement")))return false;
 if(!TestTrue(TEXT("bound replacement rolls back"),S.Kernel->Rollback(TEXTVIEW("Test.BoundDocuments")).bValid))return false;
 return S.Target.CheckRestored(*this,EOldGraph::Complete)&&S.CheckRestoredD2(*this)&&!HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallForeignResidueTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.ForeignOwnedResidueRejectedWithoutWrites",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallForeignResidueTest::RunTest(const FString&)
{
 FInstallScenario S;if(!S.SetUp(*this,EOldGraph::Complete))return false;
 TStrongObjectPtr<UObject> Foreign(NewObject<UAssetDocumentAIInstallOwnedChild>(S.Target.Tree.Get(),TEXT("UnrelatedTargetOwnedObject")));
 const EObjectFlags OriginalFlags=Foreign->GetFlags();
 FInstrumentedTransaction Proxy(*S.Kernel);
 if(!Failure(*this,S.Invoke(S.InstallInvocation(),&Proxy),TEXT("AIProvider.BehaviorTree.GraphInstallOwnershipRejected")))return false;
 TestEqual(TEXT("foreign residue rejected before finalizer registration"),Proxy.FinalizerCalls,0);
 TestEqual(TEXT("foreign residue rejected before install row"),Proxy.FootprintCalls,0);
 TestTrue(TEXT("foreign object remains under its original owner"),Foreign->GetOuter()==S.Target.Tree.Get());
 TestTrue(TEXT("foreign object flags unchanged"),Foreign->GetFlags()==OriginalFlags);
 TestTrue(TEXT("old graph remains installed"),S.Target.Tree->BTGraph==S.Target.OldGraph);
 TestTrue(TEXT("incoming remains in scratch"),S.Incoming->GetOuter()==S.OriginalScratchOwner.Get());
 TestFalse(TEXT("foreign rejection does not dirty target"),S.Target.Package->IsDirty());
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallNestedPromotionTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.NestedInstancePromotionRestoresBeforeSameNameOldRoot",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallNestedPromotionTest::RunTest(const FString&)
{
 FInstallScenario S;if(!S.SetUp(*this,EOldGraph::Complete))return false;
 UBehaviorTreeGraphNode_Task* IncomingWrapper=nullptr;UObject* OldTask=nullptr;
 for(UEdGraphNode* Node:S.Incoming->Nodes)if(auto* Wrapper=Cast<UBehaviorTreeGraphNode_Task>(Node))IncomingWrapper=Wrapper;
 for(UEdGraphNode* Node:S.Target.OldGraph->Nodes)if(auto* Wrapper=Cast<UBehaviorTreeGraphNode_Task>(Node))OldTask=Wrapper->NodeInstance;
 if(!TestNotNull(TEXT("incoming wrapper"),IncomingWrapper)||!TestNotNull(TEXT("old runtime root with colliding name"),OldTask))return false;
 UObject* IncomingInstance=IncomingWrapper->NodeInstance;
 const FString Name=OldTask->GetName();
 // Stock's copy lifecycle temporarily puts its native instance under the
 // graph wrapper. The fixture uses that legal nesting with an old-root name.
 IncomingWrapper->PrepareForCopying();
 if(!TestTrue(TEXT("fixture preserves colliding name inside its wrapper"),IncomingInstance->Rename(*Name,IncomingWrapper,REN_DoNotDirty|REN_DontCreateRedirectors|REN_NonTransactional)))return false;
 TestTrue(TEXT("incoming instance starts nested"),IncomingInstance->GetOuter()==IncomingWrapper);
 if(!Success(*this,S.Invoke(S.InstallInvocation()),TEXT("install nested incoming instance"))||!Success(*this,S.Repair(*this),TEXT("stock Repair promotes incoming runtime instance")))return false;
 TestTrue(TEXT("stock Repair made the instance target-owned"),IncomingInstance->GetOuter()==S.Target.Tree.Get());
 if(!TestTrue(TEXT("inverse frees colliding target name before restoring old runtime root"),S.Kernel->Rollback(TEXTVIEW("Test.NestedNameCollision")).bValid))return false;
 TestTrue(TEXT("inverse restores original nested ownership"),IncomingInstance->GetOuter()==IncomingWrapper);
 TestEqual(TEXT("inverse restores incoming name"),IncomingInstance->GetName(),Name);
 return S.Target.CheckRestored(*this,EOldGraph::Complete)&&S.CheckRestoredD2(*this)&&!HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallBlueprintTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.CompiledBlueprintWithInstancedChild",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallBlueprintTest::RunTest(const FString&)
{
 const FString PackagePath=TEXT("/Temp/AssetDocumentAI/InstallBlueprint_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<UPackage> Package(CreatePackage(*PackagePath));
 TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(UAssetDocumentAIInstallFamiliesTask::StaticClass(),Package.Get(),TEXT("InstallTaskBP"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass()));
 ON_SCOPE_EXIT{if(Blueprint.IsValid())Blueprint->ClearFlags(RF_Standalone);Package->SetDirtyFlag(false);};
 if(!TestNotNull(TEXT("stock creates test-owned task Blueprint"),Blueprint.Get()))return false;
 FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
 if(!TestNotNull(TEXT("stock compiler produces loaded project class"),Blueprint->GeneratedClass.Get()))return false;
 FInstallScenario S;S.RequestedTaskClass=Blueprint->GeneratedClass;
 if(!S.SetUp(*this,EOldGraph::Complete,true))return false;
 UAssetDocumentAIInstallFamiliesTask* Task=nullptr;
 for(UEdGraphNode* Node:S.Incoming->Nodes)if(auto* Wrapper=Cast<UBehaviorTreeGraphNode_Task>(Node))Task=Cast<UAssetDocumentAIInstallFamiliesTask>(Wrapper->NodeInstance);
 if(!TestNotNull(TEXT("public node.create_action materializes Blueprint task"),Task))return false;
 TestTrue(TEXT("actual runtime instance uses generated class"),Task->GetClass()==Blueprint->GeneratedClass);
 if(!TestNotNull(TEXT("Blueprint instance has inherited instanced child"),Task->Nested.Get()))return false;
 if(!Success(*this,S.Invoke(S.InstallInvocation()),TEXT("install Blueprint ownership closure"))||!Success(*this,S.Repair(*this),TEXT("complete Blueprint-installed graph")))return false;
 TestTrue(TEXT("installed Blueprint child retains task-relative ownership"),Task->Nested->GetOuter()==Task);
 TestEqual(TEXT("installed Blueprint child retains authored content"),Task->Nested->AuthoredValue,73);
 if(!TestTrue(TEXT("Blueprint inverse succeeds"),S.Kernel->Rollback(TEXTVIEW("Test.BlueprintInstall")).bValid))return false;
 return S.Target.CheckRestored(*this,EOldGraph::Complete)&&S.CheckRestoredD2(*this)&&!HasAnyErrors();
}

UBehaviorTreeDecoratorGraphNode_Logic* AddV2InstallContent(UBehaviorTreeGraph& Graph)
{
 UBehaviorTreeGraphNode_Task* Task=nullptr;
 for(UEdGraphNode* Node:Graph.Nodes)if(auto* Candidate=Cast<UBehaviorTreeGraphNode_Task>(Node))Task=Candidate;
 if(!Task)return nullptr;
 const bool Locked=Graph.IsLocked();Graph.LockUpdates();
 ON_SCOPE_EXIT { if(!Locked)Graph.UnlockUpdates(); };
 FAISchemaAction_NewSubNode Action;Action.ParentNode=Task;
 Action.NodeTemplate=NewObject<UBehaviorTreeGraphNode_CompositeDecorator>();
 Action.PerformAction(&Graph,nullptr,FVector2f::ZeroVector,false);
 auto* Wrapper=Task->Decorators.IsEmpty()?nullptr:Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Task->Decorators.Last());
 auto* Bound=Wrapper?Cast<UBehaviorTreeDecoratorGraph>(Wrapper->BoundGraph):nullptr;
 if(!Bound)return nullptr;
 FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Decorator> LeafCreator(*Bound);
 auto* Leaf=LeafCreator.CreateNode(false);Leaf->ClassData=FGraphNodeClassData(UBTDecorator_ForceSuccess::StaticClass(),FString());LeafCreator.Finalize();
 UEdGraphPin* Sink=nullptr;
 for(UEdGraphNode* Node:Bound->Nodes)if(auto* Logic=Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Node))
  if(Logic->LogicMode==EDecoratorLogicMode::Sink)for(UEdGraphPin* Pin:Logic->Pins)if(Pin->Direction==EGPD_Input)Sink=Pin;
 if(!Sink||!OutputPin(*Leaf)||!Bound->GetSchema()->TryCreateConnection(OutputPin(*Leaf),Sink))return nullptr;
 auto* Logic=AssetDocumentAIBehaviorTreeAuthoredGraphReadbackTests::AddEmptyReadbackLogic(*Bound,2);
 if(!Logic)return nullptr;
 Logic->NodePosX=352;Logic->NodePosY=-176;Logic->NodeComment=TEXT("disconnected empty inputs remain authored");
 FGraphNodeCreator<UEdGraphNode_Comment> CommentCreator(*Bound);auto* Comment=CommentCreator.CreateNode(false);CommentCreator.Finalize();
 Comment->NodeComment=TEXT("A2 persistent bound comment");Comment->NodePosX=-384;Comment->NodePosY=224;
 Comment->NodeWidth=432;Comment->NodeHeight=256;Comment->CommentColor=FLinearColor(0.2f,0.4f,0.6f,0.8f);
 Comment->FontSize=21;Comment->CommentDepth=-2;Comment->bCommentBubbleVisible=true;
 return Logic;
}
bool EnsureV2InstallInputs(FAutomationTestBase& Test,FInstallScenario& S)
{
 auto* Logic=AddV2InstallContent(*S.Incoming);if(!Test.TestNotNull(TEXT("stock bound content fixture"),Logic))return false;
 auto* Bound=Cast<UBehaviorTreeDecoratorGraph>(Logic->GetGraph());auto* Wrapper=Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Bound->GetOuter());
 FAssetDocumentRuntimeValueDto Count;Count.Kind=EAssetDocumentRuntimeValueKind::Integer;Count.IntegerValue=5;
 const auto Request=Object({TEXT("bound_graph_guid"),TEXT("composite_stable_node_id"),TEXT("exact_input_count"),TEXT("logic_kind"),TEXT("logic_stable_node_id")},
  {String(Bound->GraphGuid.ToString(EGuidFormats::Digits)),String(Wrapper->NodeGuid.ToString(EGuidFormats::Digits)),Count,String(TEXT("And")),String(Logic->NodeGuid.ToString(EGuidFormats::Digits))});
 return Success(Test,S.Invoke(S.Invocation(TEXT("ai.behavior_tree.decorator.ensure_input_count.v1"),Request,{{TEXT("graph"),S.ScratchHandle}})),TEXT("actual A2 preserves bound content and expands empty inputs"));
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallV2RollbackTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstallV2.BoundContentRollback",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallV2RollbackTest::RunTest(const FString&)
{
 FInstallScenario S;S.bV2=true;
 S.ConfigureTargetBeforeBegin=[](FTarget& Target)
 {
  if(!AddV2InstallContent(*Target.OldGraph))return false;
  Target.OldGraph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);return true;
 };
 if(!S.SetUp(*this,EOldGraph::Complete)||!EnsureV2InstallInputs(*this,S))return false;
 if(!Success(*this,S.Invoke(S.InstallInvocation()),TEXT("A2 graph installs over V2 old content"))
  ||!Success(*this,S.Repair(*this),TEXT("installed V2 Repair")))return false;
 const auto RolledBack=S.Kernel->Rollback(TEXTVIEW("Test.A2V2Rollback"));
 TestTrue(TEXT("real Atomic inverse and V2 finalizer succeed"),RolledBack.bValid);
 return S.CheckRestoredD2(*this)&&!HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphCreateProductionFootprintTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphCreate.RequestStableRealAtomic",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphCreateProductionFootprintTest::RunTest(const FString&)
{
 FTarget Target;if(!Target.Create(*this,EOldGraph::Absent,{},false))return false;
 const FGuid RequestId=FGuid::NewGuid();
 const FString PlannedRow=TEXT("scratch:ai.behavior_tree:request:")+RequestId.ToString(EGuidFormats::Digits);
 TArray<FGuid> ActualTransactions;
 // A stable request row is planned before either independently generated transaction.
 for(int32 Run=0;Run<2;++Run)
 {
  FResolveExistingHandleResolver Resolver;
  TArray<FAssetDocumentRuntimeDiagnostic> KernelDiagnostics;
  FAssetDocumentRollbackActionComposition Composition(Resolver,{},
   [&](EAssetDocumentRuntimeRequestPhase,const FAssetDocumentRuntimeDiagnostic& D){KernelDiagnostics.Add(D);});
  FAssetDocumentPreflightReceipt Receipt;Receipt.RequestId=RequestId;Receipt.RuntimeRevision=1;
  Receipt.ReceiptHash=TEXT("sha256:request-stable-scratch");Receipt.bSucceeded=true;
  Receipt.CompleteFootprint={{EAssetDocumentMutationFootprintKind::UObjectState,PlannedRow,
   TEXT("assetdocument.ai.behavior_tree.scratch_bundle.v1")}};
  FNoCancellation Cancellation;
  auto Kernel=Composition.BeginLiveAfterPreflight(Receipt,Cancellation);
  if(!TestTrue(TEXT("real Atomic begins with request-only planned row"),Kernel.IsValid()))return false;
  const auto& Token=Kernel->GetMutationToken();ActualTransactions.Add(Token.TransactionId);
  Resolver.SetMutationToken(Token);Resolver.SetIssuingProviderId(TEXT("ai"));
  FAssetDocumentPrimitiveInvocation I;I.PrimitiveId=TEXT("ai.behavior_tree.graph.create.v1");I.Version=1;
  I.RequestId=RequestId;I.TransactionId=Token.TransactionId;I.RuntimeRevision=Token.RuntimeRevision;
  I.Phase=EAssetDocumentRuntimeRequestPhase::Preflight;
  I.ValueArguments=Object({TEXT("behavior_tree_path"),TEXT("graph_name")},
   {String(Target.Tree->GetPathName()),String(TEXT("RequestStableScratch"))});
  auto Provider=CreateAssetDocumentAIPrimitiveProvider();
  const auto Preflight=Provider->Invoke(I,Resolver,Kernel.Get());
  TestTrue(TEXT("Preflight cannot create a scratch graph"),Preflight.Status==EAssetDocumentPrimitiveOutcomeStatus::Failed
   &&Preflight.HandleResults.IsEmpty()&&Preflight.ChangedSet.IsEmpty());
  I.Phase=EAssetDocumentRuntimeRequestPhase::Apply;
  const FGuid TransactionId=I.TransactionId;I.TransactionId=FGuid::NewGuid();
  const auto WrongTransaction=Provider->Invoke(I,Resolver,Kernel.Get());
  TestTrue(TEXT("request-stable row does not weaken actual transaction binding"),WrongTransaction.Status==EAssetDocumentPrimitiveOutcomeStatus::Failed
   &&WrongTransaction.HandleResults.IsEmpty());
  I.TransactionId=TransactionId;
  const auto Created=Provider->Invoke(I,Resolver,Kernel.Get());
  if(!Success(*this,Created,TEXT("Apply enlists the preplanned scratch row through real Atomic")))return false;
  const auto Graph=ResultHandle(Created,TEXT("graph"));if(!Graph.IsSet())return false;
  TestEqual(TEXT("issued scratch capability retains generated transaction identity"),Graph->TransactionId,TransactionId);
  TestTrue(TEXT("all and only declared rows enlisted before ready-to-commit"),Kernel->MarkReadyToCommit().bValid);
  TestTrue(TEXT("real kernel owns scratch rollback"),Kernel->Rollback(TEXTVIEW("Test.D5ScratchCleanup")).bValid);
  TestEqual(TEXT("rollback invalidates scratch handles"),Resolver.GetInvalidationCount(),1);
  TestTrue(TEXT("real scratch lifecycle emits no kernel diagnostics"),KernelDiagnostics.IsEmpty());
 }
 TestTrue(TEXT("same planned request row supports distinct actual transaction IDs"),
  ActualTransactions.Num()==2&&ActualTransactions[0]!=ActualTransactions[1]);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallInitializationTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.TargetBlackboardInitialization",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallInitializationTest::RunTest(const FString&)
{
 for(const bool HasCurrentBlackboard:{false,true})
 {
  FInstallScenario S;S.bPlanEarlierBlackboardChange=true;
  if(!S.SetUp(*this,EOldGraph::Absent,false,{},HasCurrentBlackboard))return false;
  TStrongObjectPtr<UBlackboardData> ScratchOnly(NewObject<UBlackboardData>(S.Target.Package.Get(),TEXT("ScratchOnly"),RF_Public));
  TStrongObjectPtr<UBlackboardData> NextBlackboard(NewObject<UBlackboardData>(S.Target.Package.Get(),TEXT("NextBlackboard"),RF_Public));
  UBehaviorTreeGraphNode_Root* Root=nullptr;
  for(UEdGraphNode* Node:S.Incoming->Nodes)if(auto* Candidate=Cast<UBehaviorTreeGraphNode_Root>(Node))Root=Candidate;
  if(!TestNotNull(TEXT("scratch root fixture"),Root))return false;
  Root->BlackboardAsset=ScratchOnly.Get();Root->UpdateBlackboard();
  auto* ScratchTree=Cast<UBehaviorTree>(S.OriginalScratchOwner.Get());
  if(!TestTrue(TEXT("fixture simulates stock selection of an unrelated loaded Blackboard"),ScratchTree
   &&ScratchTree->BlackboardAsset==ScratchOnly.Get()&&S.Target.Tree->BlackboardAsset!=ScratchOnly.Get()))return false;
  UBlackboardData* const TargetBefore=S.Target.Tree->BlackboardAsset;
  if(!Success(*this,S.Invoke(S.InstallInvocation()),TEXT("INSTALL initializes against current target header")))return false;
  TestTrue(TEXT("INSTALL preserves target header rather than publishing scratch selection"),
   S.Target.Tree->BlackboardAsset==TargetBefore&&Root->BlackboardAsset==TargetBefore);
  for(UEdGraphNode* Node:S.Incoming->Nodes)
   if(auto* Wrapper=Cast<UBehaviorTreeGraphNode>(Node))if(auto* Instance=Cast<UBTNode>(Wrapper->NodeInstance))
    TestTrue(TEXT("installed instance binds exact target and current Blackboard"),
     Instance->GetTreeAsset()==S.Target.Tree.Get()&&Instance->GetBlackboardAsset()==TargetBefore);
  const auto Assignment=S.Invoke(S.Invocation(TEXT("ai.behavior_tree.blackboard.set.v1"),
   Object({TEXT("behavior_tree_path"),TEXT("blackboard_path_or_null")},
    {String(S.Target.Tree->GetPathName()),String(NextBlackboard->GetPathName())}),{{TEXT("asset"),S.AssetHandle}}));
  if(!Success(*this,Assignment,TEXT("existing strict Blackboard assignment admits freshly installed graph")))return false;
  TestTrue(TEXT("subsequent Blackboard assignment chooses requested target"),S.Target.Tree->BlackboardAsset==NextBlackboard.Get());
  if(!TestTrue(TEXT("real inverse restores target and scratch initialization"),S.Kernel->Rollback(TEXTVIEW("Test.InstallInitialization")).bValid))return false;
  TestTrue(TEXT("target header restored after inverse"),S.Target.Tree->BlackboardAsset==TargetBefore);
  TestTrue(TEXT("scratch root header restores original provenance"),Root->BlackboardAsset==ScratchOnly.Get()
   &&ScratchTree->BlackboardAsset==ScratchOnly.Get());
  for(UEdGraphNode* Node:S.Incoming->Nodes)
   if(auto* Wrapper=Cast<UBehaviorTreeGraphNode>(Node))if(auto* Instance=Cast<UBTNode>(Wrapper->NodeInstance))
    TestTrue(TEXT("restored scratch instances retain no target TreeAsset reference"),
     Instance->GetTreeAsset()==ScratchTree&&Instance->GetBlackboardAsset()==ScratchOnly.Get());
 }
 // Public Create installs against its initially null target BB, then assigns
 // the header and resolves selectors. The hidden stock Wait key is still None
 // until installed completion; test that exact sequence through real providers.
 enum class EWaitCase { Initialize, ConflictingKey, WrongType };
 for(const bool V2:{false,true})
 for(const EWaitCase Case:{EWaitCase::ConflictingKey,EWaitCase::WrongType,EWaitCase::Initialize})
 {
  FInstallScenario S;S.RequestedTaskClass=UBTTask_WaitBlackboardTime::StaticClass();
  S.bPlanEarlierBlackboardChange=true;S.bV2=V2;
  if(!S.SetUp(*this,EOldGraph::Absent,false,{},false))return false;
  UBTTask_WaitBlackboardTime* Wait=nullptr;UBehaviorTreeGraphNode_Root* Root=nullptr;
  for(UEdGraphNode* Node:S.Incoming->Nodes)
  {
   if(auto* Candidate=Cast<UBehaviorTreeGraphNode_Root>(Node))Root=Candidate;
   if(auto* Wrapper=Cast<UBehaviorTreeGraphNode_Task>(Node))Wait=Cast<UBTTask_WaitBlackboardTime>(Wrapper->NodeInstance);
  }
  if(!TestNotNull(TEXT("late BB fixture has actual stock Wait"),Wait)||!TestNotNull(TEXT("late BB fixture has its root"),Root))return false;
  const auto* SelectorProperty=FindFProperty<FStructProperty>(Wait->GetClass(),TEXT("BlackboardKey"));
  if(!TestTrue(TEXT("late BB fixture uses the stock selector declaration"),SelectorProperty
   &&SelectorProperty->GetOwnerStruct()==UBTTask_WaitBlackboardTime::StaticClass()
   &&SelectorProperty->Struct==FBlackboardKeySelector::StaticStruct()))return false;
  auto* Selector=SelectorProperty->ContainerPtrToValuePtr<FBlackboardKeySelector>(Wait);
  Selector->SelectedKeyName=TEXT("WaitDuration");Selector->InvalidateResolvedKey();Selector->SelectedKeyType=nullptr;
  Wait->RandomDeviation=FValueOrBBKey_Float(0.375f);
  auto* CDO=CastChecked<UBTTask_WaitBlackboardTime>(UBTTask_WaitBlackboardTime::StaticClass()->GetDefaultObject());
  TArray<uint8> CDOBefore,CDOAfter;
  if(!TestTrue(TEXT("stock Wait CDO authored bytes readable"),StockWaitAuthoredBytes(*CDO,CDOBefore)))return false;
  const bool CDODirty=CDO->GetPackage()->IsDirty();
  if(!Success(*this,S.Invoke(S.InstallInvocation()),TEXT("actual Wait INSTALL with null target BB")))return false;
  TestNull(TEXT("INSTALL keeps the null target BB header"),S.Target.Tree->BlackboardAsset.Get());
  TestTrue(TEXT("null BB stock initialization clears only WaitTime value mode"),Wait->WaitTime.GetKey().IsNone());
  const float Default=Wait->WaitTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr));
  TestEqual(TEXT("null BB initialization has its actual stock default"),Default,0.f);
  TStrongObjectPtr<UBlackboardData> LateBB(NewObject<UBlackboardData>(S.Target.Package.Get(),TEXT("LateBB"),RF_Public));
  LateBB->UpdatePersistentKey<UBlackboardKeyType_Float>(TEXT("WaitDuration"));
  LateBB->UpdatePersistentKey<UBlackboardKeyType_Float>(TEXT("OtherDuration"));
  LateBB->UpdatePersistentKey<UBlackboardKeyType_String>(TEXT("WrongType"));
  LateBB->UpdateParentKeys();
  if(!Success(*this,S.Invoke(S.Invocation(TEXT("ai.behavior_tree.blackboard.set.v1"),
   Object({TEXT("behavior_tree_path"),TEXT("blackboard_path_or_null")},
    {String(S.Target.Tree->GetPathName()),String(LateBB->GetPathName())}),{{TEXT("asset"),S.AssetHandle}})),
   TEXT("actual late BB assignment after null-header INSTALL")))return false;
  TestTrue(TEXT("late header does not perform project or Wait initialization"),Wait->WaitTime.GetKey().IsNone());
  const auto ReadAlias=S.Resolver.RegisterProviderScopedAlias(S.AssetHandle,TEXTVIEW("ai"),EAssetDocumentHandleAccess::ReadOnly);
  if(!ReadAlias.IsSet())return false;
  const auto Inventory=S.Invoke(S.Invocation(TEXT("ai.behavior_tree.selector.inventory.v1"),
   Object({TEXT("behavior_tree_path")},{String(S.Target.Tree->GetPathName())}),{{TEXT("asset"),ReadAlias.GetValue()}},EAssetDocumentRuntimeRequestPhase::Repair));
  if(!Success(*this,Inventory,TEXT("actual late Wait selector inventory")))return false;
  const auto* Selectors=Field(Inventory.Value,TEXT("selectors"));
  if(!TestTrue(TEXT("actual selector inventory supplies its typed array"),Selectors&&Selectors->Kind==EAssetDocumentRuntimeValueKind::Array))return false;
  auto Resolve=S.Invocation(TEXT("ai.behavior_tree.selector.resolve_batch.v2"),
   Object({TEXT("behavior_tree_path"),TEXT("blackboard_path"),TEXT("selectors")},
    {String(S.Target.Tree->GetPathName()),String(LateBB->GetPathName()),*Selectors}),{{TEXT("asset"),S.AssetHandle}},EAssetDocumentRuntimeRequestPhase::Repair);
  Resolve.Version=2;
  if(!Success(*this,S.Invoke(Resolve),TEXT("actual late Wait selector batch resolution")))return false;
  TestEqual(TEXT("actual selector resolves late BB ID"),Selector->GetSelectedKeyID(),LateBB->GetKeyID(TEXT("WaitDuration")));
  TestTrue(TEXT("selector repair leaves hidden Wait key unauthored"),Wait->WaitTime.GetKey().IsNone());
  S.Target.Package->SetDirtyFlag(false);
  FAssetDocumentRuntimeValueDto D2;
  if(!S.Readback(*this,D2))return false;
  FString Digest;
  if(!UE::AssetDocumentAI::BehaviorTreeValueOrKey::HashAuthoredGraphValue(S.Target.Tree->GetPathName(),D2,Digest,
   V2?UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion::V2
    :UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion::V1))return false;
  auto Complete=S.Invocation(V2?TEXT("ai.behavior_tree.value_or_key.refresh.v2"):TEXT("ai.behavior_tree.value_or_key.refresh.v1"),
   Object({TEXT("behavior_tree_path"),TEXT("completion_registration"),TEXT("expected_authored_inventory_digest"),TEXT("expected_blackboard_path")},
    {String(S.Target.Tree->GetPathName()),String(TEXT("installed_replacement")),String(Digest),String(LateBB->GetPathName())}),
   {{TEXT("asset"),S.AssetHandle}},EAssetDocumentRuntimeRequestPhase::Repair);
  TArray<uint8> Unwritten,AfterDenied;
  if(!StockWaitAuthoredBytes(*Wait,Unwritten))return false;
  FInstrumentedTransaction Denied(*S.Kernel);Denied.bFailFootprint=true;
  if(!Failure(*this,S.Invoke(Complete,&Denied),TEXT("AIProvider.OperationRejected")))return false;
  TestEqual(TEXT("installed footprint rejection occurs before stock initialization"),Denied.FootprintCalls,1);
  if(!StockWaitAuthoredBytes(*Wait,AfterDenied))return false;
  TestEqual(TEXT("denied footprint leaves all Wait authored bytes unchanged"),AfterDenied,Unwritten);
  TestFalse(TEXT("denied footprint preserves package cleanliness"),S.Target.Package->IsDirty());
  auto BadDigest=Complete;BadDigest.ValueArguments.Elements[2]=String(FString::ChrN(64,TCHAR('0')));
  FInstrumentedTransaction Admission(*S.Kernel);
  if(!Failure(*this,S.Invoke(BadDigest,&Admission),TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected")))return false;
  TestEqual(TEXT("hidden stock initialization cannot precede digest admission"),Admission.FootprintCalls,0);
  if(!StockWaitAuthoredBytes(*Wait,AfterDenied))return false;
  TestEqual(TEXT("bad digest retains exact hidden/visible authored bytes"),AfterDenied,Unwritten);
  // Completion enlists its terminal footprint even when its later typed
  // admission fails. Each negative uses its own real Atomic, so a subsequent
  // branch reaches the actual typed check rather than FootprintAlreadyEnlisted.
  if(Case==EWaitCase::ConflictingKey)
  {
   Wait->WaitTime.SetKey(TEXT("OtherDuration"));
   if(!StockWaitAuthoredBytes(*Wait,Unwritten))return false;
   if(!Failure(*this,S.Repair(*this),TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected")))return false;
   if(!StockWaitAuthoredBytes(*Wait,AfterDenied))return false;
   TestEqual(TEXT("non-None conflicting hidden key fails without changing authored bytes"),AfterDenied,Unwritten);
   Wait->WaitTime.SetKey(NAME_None);
  }
  else if(Case==EWaitCase::WrongType)
  {
   Selector->SelectedKeyName=TEXT("WrongType");Selector->ResolveSelectedKey(*LateBB);
   if(!StockWaitAuthoredBytes(*Wait,Unwritten))return false;
   if(!Failure(*this,S.Repair(*this),TEXT("AIProvider.BehaviorTree.ValueOrKeyTypeMismatch")))return false;
   if(!StockWaitAuthoredBytes(*Wait,AfterDenied))return false;
   TestEqual(TEXT("wrong selected key type leaves actual stock authored bytes unchanged"),AfterDenied,Unwritten);
   Selector->SelectedKeyName=TEXT("WaitDuration");Selector->ResolveSelectedKey(*LateBB);
  }
  else
  {
   const auto Completed=S.Invoke(Complete);
   if(!Success(*this,Completed,TEXT("installed completion proves the finite late Wait initialization")))return false;
   TestEqual(TEXT("installed Wait binds exactly its authored selector"),Wait->WaitTime.GetKey(),FName(TEXT("WaitDuration")));
   TestEqual(TEXT("finite initialization retains actual Wait default"),Wait->WaitTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr)),Default);
   TestEqual(TEXT("finite initialization retains RandomDeviation default"),Wait->RandomDeviation.GetValue(static_cast<const UBlackboardComponent*>(nullptr)),0.375f);
   TestTrue(TEXT("finite initialization retains RandomDeviation literal mode"),Wait->RandomDeviation.GetKey().IsNone());
   auto* World=GEditor?GEditor->GetEditorWorldContext().World():nullptr;
   if(!TestNotNull(TEXT("late Wait runtime observation world"),World))return false;
   TStrongObjectPtr<UBlackboardComponent> Component(NewObject<UBlackboardComponent>(World));
   if(!TestTrue(TEXT("late Wait component initializes"),Component->InitializeBlackboard(*LateBB)))return false;
   Component->SetValueAsFloat(TEXT("WaitDuration"),23.5f);
   TestEqual(TEXT("late Wait resolves actual target key value"),Wait->WaitTime.GetValue(Component.Get()),23.5f);
   TestFalse(TEXT("installed completion preserves clean target package"),S.Target.Package->IsDirty());
   if(!S.ReissueAsset(*this))return false;
   FAssetDocumentRuntimeValueDto AfterD2;
   if(!S.Readback(*this,AfterD2))return false;
   TArray<uint8> BeforeBytes,AfterBytes;
   if(!UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EncodeAuthoredCanonicalValue(D2,BeforeBytes)
    ||!UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EncodeAuthoredCanonicalValue(AfterD2,AfterBytes))return false;
   TestEqual(TEXT("finite initialization preserves complete public authored bytes"),AfterBytes,BeforeBytes);
  }
  if(!TestTrue(TEXT("late initialized Wait participates in actual Atomic inverse"),S.Kernel->Rollback(TEXTVIEW("Test.LateWaitInitialization")).bValid))return false;
  if(!S.Target.CheckRestored(*this,EOldGraph::Absent))return false;
  TestNull(TEXT("late Wait inverse restores target BB absence"),S.Target.Tree->BlackboardAsset.Get());
  TestTrue(TEXT("late Wait inverse restores exact scratch graph ownership"),S.Incoming->GetOuter()==S.OriginalScratchOwner.Get());
  auto* ScratchTree=Cast<UBehaviorTree>(S.OriginalScratchOwner.Get());
  TestTrue(TEXT("late Wait inverse retains no target initialization reference"),Wait->GetTreeAsset()==ScratchTree&&Wait->GetBlackboardAsset()==nullptr);
  TestTrue(TEXT("scratch Wait returns to original None mode"),Wait->WaitTime.GetKey().IsNone());
  TestEqual(TEXT("scratch Wait returns to original stock literal"),Wait->WaitTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr)),Default);
  TestEqual(TEXT("inverse retains unrelated hidden RandomDeviation"),Wait->RandomDeviation.GetValue(static_cast<const UBlackboardComponent*>(nullptr)),0.375f);
  if(!StockWaitAuthoredBytes(*CDO,CDOAfter))return false;
  TestEqual(TEXT("finite stock initialization leaves complete CDO authored bytes unchanged"),CDOAfter,CDOBefore);
  TestEqual(TEXT("finite stock initialization preserves CDO package state"),CDO->GetPackage()->IsDirty(),CDODirty);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIGraphInstallEnumInitializationTest,
 "UEAssetDoc.AIProvider.BehaviorTree.GraphInstall.EnumInitializationPreservesLabel",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIGraphInstallEnumInitializationTest::RunTest(const FString&)
{
 for(const bool ValidTargetLabel:{true,false})
 {
  FInstallScenario S;
  S.ConfigureTargetBeforeBegin=[&](FTarget& Target)
  {
   const FString EnumName=TEXT("TargetEnum_")+FPackageName::GetShortName(Target.Package->GetName());
   UEnum* Enum=NewObject<UEnum>(Target.Package.Get(),FName(*EnumName),RF_Public);
   TArray<TPair<FName,int64>> Names={{FName(*(EnumName+TEXT("::First"))),0},{FName(*(EnumName+(ValidTargetLabel?TEXT("::Second"):TEXT("::Other")))),9}};
   AssetDocumentEnums::SetEnums(*Enum, Names,UEnum::ECppForm::EnumClass);
   Target.Blackboard->UpdatePersistentKey<UBlackboardKeyType_Enum>(TEXT("Enum"))->EnumType=Enum;
   return true;
  };
  if(!S.SetUp(*this,EOldGraph::Absent))return false;
  TStrongObjectPtr<UBlackboardData> ScratchBB(NewObject<UBlackboardData>(S.Target.Package.Get(),TEXT("ScratchEnumBB"),RF_Public));
  const FString SourceEnumName=TEXT("SourceEnum_")+FPackageName::GetShortName(S.Target.Package->GetName());
  UEnum* SourceEnum=NewObject<UEnum>(S.Target.Package.Get(),FName(*SourceEnumName),RF_Public);
  TArray<TPair<FName,int64>> SourceNames={{FName(*(SourceEnumName+TEXT("::First"))),0},{FName(*(SourceEnumName+TEXT("::Second"))),7}};
  AssetDocumentEnums::SetEnums(*SourceEnum, SourceNames,UEnum::ECppForm::EnumClass);
  ScratchBB->UpdatePersistentKey<UBlackboardKeyType_Enum>(TEXT("Enum"))->EnumType=SourceEnum;
  UBehaviorTreeGraphNode_Task* Task=nullptr;
  for(UEdGraphNode* Node:S.Incoming->Nodes)
  {
   if(auto* Root=Cast<UBehaviorTreeGraphNode_Root>(Node)){Root->BlackboardAsset=ScratchBB.Get();Root->UpdateBlackboard();}
   if(auto* Candidate=Cast<UBehaviorTreeGraphNode_Task>(Node))Task=Candidate;
  }
  if(!TestNotNull(TEXT("incoming task for enum decorator"),Task))return false;
  S.Incoming->LockUpdates();
  FAISchemaAction_NewSubNode Action;Action.ParentNode=Task;
  Action.NodeTemplate=NewObject<UBehaviorTreeGraphNode_Decorator>();
  Action.NodeTemplate->NodeInstance=NewObject<UBTDecorator_Blackboard>(Action.NodeTemplate);
  Action.PerformAction(S.Incoming.Get(),nullptr,FVector2f::ZeroVector,false);
  auto* Decorator=Cast<UBTDecorator_Blackboard>(Task->Decorators.Last()->NodeInstance);
  if(!Decorator){S.Incoming->UnlockUpdates();return false;}
  auto& Selector=InstallFixtureProperty<FBlackboardKeySelector>(*Decorator,TEXT("BlackboardKey"));
  Selector.SelectedKeyName=TEXT("Enum");
  InstallFixtureProperty<FString>(*Decorator,TEXT("StringValue"))=TEXT("Second");
  InstallFixtureProperty<uint8>(*Decorator,TEXT("ArithmeticOperation"))=uint8(EArithmeticKeyOperation::Equal);
  auto* ScratchTree=CastChecked<UBehaviorTree>(S.OriginalScratchOwner.Get());
  static_cast<UBTNode*>(Decorator)->InitializeFromAsset(*ScratchTree);S.Incoming->UnlockUpdates();
  TestEqual(TEXT("source native enum mapping is seven"),InstallFixtureProperty<int32>(*Decorator,TEXT("IntValue")),7);
  using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
  FAssetDocumentRuntimeValueDto Before;bool Overflow=false;TArray<uint8> BeforeBytes;
  if(!TestTrue(TEXT("complete original scratch D2 captured"),BuildAuthoredGraphProjection(*ScratchTree,*S.Incoming,Before,Overflow,EAuthoredGraphReadbackVersion::V2)
   &&!Overflow&&EncodeAuthoredCanonicalValue(Before,BeforeBytes)))return false;
  const auto Installed=S.Invoke(S.InstallInvocation());
  if(ValidTargetLabel)
  {
   if(!Success(*this,Installed,TEXT("same authored enum label installs across numeric mappings")))return false;
   TestEqual(TEXT("target derived enum mapping is nine"),InstallFixtureProperty<int32>(*Decorator,TEXT("IntValue")),9);
   TestEqual(TEXT("authored label stays exact"),InstallFixtureProperty<FString>(*Decorator,TEXT("StringValue")),FString(TEXT("Second")));
  }
  else if(!Failure(*this,Installed,TEXT("AIProvider.BehaviorTree.GraphInstallTransferFailed")))return false;
  if(!TestTrue(TEXT("real inverse restores source enum mapping"),S.Kernel->Rollback(TEXTVIEW("Test.EnumInitialization")).bValid))return false;
  TestEqual(TEXT("inverse native enum mapping is seven"),InstallFixtureProperty<int32>(*Decorator,TEXT("IntValue")),7);
  FAssetDocumentRuntimeValueDto After;TArray<uint8> AfterBytes;Overflow=false;
  if(!TestTrue(TEXT("restored complete scratch D2 captured"),BuildAuthoredGraphProjection(*ScratchTree,*S.Incoming,After,Overflow,EAuthoredGraphReadbackVersion::V2)
   &&!Overflow&&EncodeAuthoredCanonicalValue(After,AfterBytes)))return false;
  TestEqual(TEXT("inverse restores every original D2 field"),AfterBytes,BeforeBytes);
 }
 return !HasAnyErrors();
}
}
#endif
