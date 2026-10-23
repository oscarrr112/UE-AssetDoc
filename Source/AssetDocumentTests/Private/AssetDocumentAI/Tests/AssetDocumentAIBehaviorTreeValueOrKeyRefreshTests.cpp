// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentAIBehaviorTreeProviderTestSupport.h"
#include "Fixtures/AssetDocumentAIReadbackTestTypes.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDerivedCompletion.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Tasks/BTTask_WaitBlackboardTime.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeValueOrKeyInventory.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "BehaviorTree/Decorators/BTDecorator_Cooldown.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "AIGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Templates/UnrealTemplate.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardValueOrKeyCacheInvalidation.h"
#include "UObject/SavePackage.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
namespace AssetDocumentAIBehaviorTreeAuthoredGraphReadbackTests
{
UBehaviorTreeDecoratorGraphNode_Logic* AddEmptyReadbackLogic(UBehaviorTreeDecoratorGraph& Target, int32 Count);
}
namespace AssetDocumentAIBehaviorTreeGraphInstallTests
{
bool ReadRestoredPublicD2(FAutomationTestBase& Test, UBehaviorTree& Tree, FAssetDocumentRuntimeValueDto& Value, bool V2);
}
namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests
{
using namespace AssetDocumentAIBehaviorTreeProviderTestSupport;
FAssetDocumentRuntimeValueDto String(FString S)
{ FAssetDocumentRuntimeValueDto V; V.Kind=EAssetDocumentRuntimeValueKind::String; V.StringValue=MoveTemp(S); return V; }
bool ExpectedCode(FAutomationTestBase& Test,const FAssetDocumentPrimitiveOutcome& Outcome,const TCHAR* Code,const FString& Case)
{
 const bool Found=Outcome.Diagnostics.ContainsByPredicate([&](const auto& D){return D.Code==Code;});
 if(!Found)
 {
  Test.AddInfo(Case+TEXT(" expected ")+Code+FString::Printf(TEXT("; actual status=%d diagnostics=%d"),int32(Outcome.Status),Outcome.Diagnostics.Num()));
  for(const auto& D:Outcome.Diagnostics)Test.AddInfo(D.Code+TEXT(": ")+D.Message);
 }
 return Found;
}
// Independent consumer of public D2 Value. No reflected schema, leaf inventory,
// private inventory digest helper or production encoder supplies expected bytes.
void ProducerWord(TArray<uint8>& B, uint64 W)
{ for (int32 I=7; I>=0; --I) B.Add(uint8(W >> (I*8))); }
bool ProducerEncode(const FAssetDocumentRuntimeValueDto& V, TArray<uint8>& B)
{
 B.Add(uint8(V.Kind));
 switch(V.Kind)
 {
 case EAssetDocumentRuntimeValueKind::Null: return true;
 case EAssetDocumentRuntimeValueKind::Boolean: B.Add(V.BooleanValue?1:0); return true;
 case EAssetDocumentRuntimeValueKind::Integer: ProducerWord(B,uint64(V.IntegerValue)); return true;
 case EAssetDocumentRuntimeValueKind::Number: ProducerWord(B,BitCast<uint64>(V.NumberValue==0?0.0:V.NumberValue)); return FMath::IsFinite(V.NumberValue);
 case EAssetDocumentRuntimeValueKind::String:
 { FTCHARToUTF8 U(*V.StringValue,V.StringValue.Len()); ProducerWord(B,U.Length()); B.Append(reinterpret_cast<const uint8*>(U.Get()),U.Length()); return true; }
 case EAssetDocumentRuntimeValueKind::Array:
 case EAssetDocumentRuntimeValueKind::Object:
  ProducerWord(B,V.Elements.Num());
  for(int32 I=0;I<V.Elements.Num();++I)
  { if(V.Kind==EAssetDocumentRuntimeValueKind::Object && (!V.ObjectKeys.IsValidIndex(I)||!ProducerEncode(String(V.ObjectKeys[I]),B))) return false;
    if(!ProducerEncode(V.Elements[I],B)) return false; }
  return true;
 default:return false;
 }
}
FString ProducerDigest(const FString& Path,const FAssetDocumentRuntimeValueDto& Value)
{
 FAssetDocumentRuntimeValueDto Envelope; Envelope.Kind=EAssetDocumentRuntimeValueKind::Array;
 Envelope.Elements={String(TEXT("assetdocument.ai.bt.value_or.authored.v1")),String(Path),Value};
 TArray<uint8> Bytes; if(!ProducerEncode(Envelope,Bytes)) return {};
 return UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::HashEvidence(BytesToHex(Bytes.GetData(),Bytes.Num()).ToLower());
}
FString ProducerDigestV2(const FString& Path,const FAssetDocumentRuntimeValueDto& Value)
{
 FAssetDocumentRuntimeValueDto Envelope; Envelope.Kind=EAssetDocumentRuntimeValueKind::Array;
 Envelope.Elements={String(TEXT("assetdocument.ai.bt.value_or.authored.v2")),String(Path),Value};
 TArray<uint8> Bytes;if(!ProducerEncode(Envelope,Bytes))return {};
 return UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::HashEvidence(BytesToHex(Bytes.GetData(),Bytes.Num()).ToLower());
}
UEdGraphPin* Out(UEdGraphNode& N)
{ for(auto* P:N.Pins) if(P&&P->Direction==EGPD_Output) return P; return nullptr; }
struct FScenario
{
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UBehaviorTree> Tree;
 UBlackboardData* A=nullptr; UBlackboardData* B=nullptr;
 UBehaviorTreeGraph* Graph=nullptr; UBehaviorTreeGraphNode_Root* Root=nullptr;
 UAssetDocumentAIValueOrTask* Task=nullptr;
 FResolveExistingHandleResolver Resolver;
 TUniquePtr<FAssetDocumentRollbackActionComposition> Composition;
 TUniquePtr<IAssetDocumentAtomicTransaction> Transaction;
 FAssetDocumentOpaqueHandle Handle; TArray<FString> Order;
 TArray<FAssetDocumentRuntimeDiagnostic> KernelDiagnostics;
 TFunction<void()> AfterOrdinaryRestore;
 ~FScenario(){ if(Tree.IsValid()) Tree->ClearFlags(RF_Standalone); if(A) A->ClearFlags(RF_Standalone); if(B) B->ClearFlags(RF_Standalone); if(Package.IsValid()) Package->SetDirtyFlag(false); }
 void Configure(UBlackboardData& BB,bool Shift)
 {
  if(Shift) BB.UpdatePersistentKey<UBlackboardKeyType_Int>(TEXT("Padding"));
  BB.UpdatePersistentKey<UBlackboardKeyType_Bool>(TEXT("Bool"));
  BB.UpdatePersistentKey<UBlackboardKeyType_Class>(TEXT("Class"))->BaseClass=UObject::StaticClass();
  auto* E=BB.UpdatePersistentKey<UBlackboardKeyType_Enum>(TEXT("Enum")); E->EnumType=StaticEnum<EAssetDocumentAIReadbackEnum>(); E->EnumName=E->EnumType->GetPathName();
  BB.UpdatePersistentKey<UBlackboardKeyType_Float>(TEXT("Float"));
  BB.UpdatePersistentKey<UBlackboardKeyType_Int>(TEXT("Int32"));
  BB.UpdatePersistentKey<UBlackboardKeyType_Name>(TEXT("Name"));
  BB.UpdatePersistentKey<UBlackboardKeyType_Object>(TEXT("Object"))->BaseClass=UObject::StaticClass();
  BB.UpdatePersistentKey<UBlackboardKeyType_Rotator>(TEXT("Rotator"));
  BB.UpdatePersistentKey<UBlackboardKeyType_String>(TEXT("String"));
  auto* S=BB.UpdatePersistentKey<UBlackboardKeyType_Struct>(TEXT("Struct")); S->DefaultValue.InitializeAs<FVector>(FVector::ZeroVector); S->PostLoad();
  BB.UpdatePersistentKey<UBlackboardKeyType_Vector>(TEXT("Vector"));
 }
 void Retarget(UBlackboardData* BB){ Tree->BlackboardAsset=BB; Root->BlackboardAsset=BB; Root->UpdateBlackboard(); }
 TArray<FName> AuthoredKeys() const
 {
  return {Task->BoolValue.GetKey(),Task->ClassValue.GetKey(),Task->EnumValue.GetKey(),Task->FloatValue.GetKey(),Task->Int32Value.GetKey(),Task->NameValue.GetKey(),Task->ObjectValue.GetKey(),Task->RotatorValue.GetKey(),Task->StringValue.GetKey(),Task->StructValue.GetKey(),Task->VectorValue.GetKey()};
 }
 bool Create(FAutomationTestBase& T,bool Containers=false,FString PackagePath={})
 {
  if(PackagePath.IsEmpty())PackagePath=TEXT("/Temp/AssetDocumentAI/ValueOr_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
  Package.Reset(CreatePackage(*PackagePath));
  Tree.Reset(NewObject<UBehaviorTree>(Package.Get(),TEXT("Tree"),RF_Public|RF_Standalone));
  A=NewObject<UBlackboardData>(Package.Get(),TEXT("A"),RF_Public|RF_Standalone); B=NewObject<UBlackboardData>(Package.Get(),TEXT("B"),RF_Public|RF_Standalone); Configure(*A,false); Configure(*B,true); Tree->BlackboardAsset=A;
  Graph=Cast<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(Tree.Get(),TEXT("Graph"),UBehaviorTreeGraph::StaticClass(),UEdGraphSchema_BehaviorTree::StaticClass())); Tree->BTGraph=Graph;
  Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph); Graph->OnCreated(); Graph->Initialize(); Root=Cast<UBehaviorTreeGraphNode_Root>(Graph->Nodes[0]); if(!Root) return false; Retarget(A);
  FAISchemaAction_NewNode C; C.NodeTemplate=NewObject<UBehaviorTreeGraphNode_Composite>(); C.NodeTemplate->NodeInstance=NewObject<UBTComposite_Sequence>(C.NodeTemplate);
  auto* Composite=C.PerformAction(Graph,Out(*Root),FVector2f(0,100),false); if(!Composite) return false;
  FAISchemaAction_NewNode N; N.NodeTemplate=NewObject<UBehaviorTreeGraphNode_Task>(); N.NodeTemplate->NodeInstance=NewObject<UAssetDocumentAIValueOrTask>(N.NodeTemplate);
  auto* Wrapper=Cast<UBehaviorTreeGraphNode_Task>(N.PerformAction(Graph,Out(*Composite),FVector2f(0,200),false)); if(!Wrapper) return false; Task=Cast<UAssetDocumentAIValueOrTask>(Wrapper->NodeInstance); if(!Task) return false;
  Task->BoolValue=FValueOrBBKey_Bool(false); Task->ClassValue.SetBaseClass(UObject::StaticClass()); Task->EnumValue.SetEnumType(StaticEnum<EAssetDocumentAIReadbackEnum>());
  Task->FloatValue=FValueOrBBKey_Float(1.25f); Task->Int32Value=FValueOrBBKey_Int32(37); Task->NameValue=FValueOrBBKey_Name(TEXT("DefaultName")); Task->ObjectValue.SetBaseClass(UObject::StaticClass());
  Task->RotatorValue=FValueOrBBKey_Rotator(FRotator(1,2,3)); Task->StringValue=FValueOrBBKey_String(TEXT("DefaultString")); Task->StructValue=FValueOrBBKey_Struct(FVector(2,3,5)); Task->StructValue.SetStructType(TBaseStructure<FVector>::Get()); Task->VectorValue=FValueOrBBKey_Vector(FVector(5,8,13));
  Task->BoolValue.SetKey(TEXT("Bool"));
  Task->ClassValue.SetKey(TEXT("Class"));
  Task->EnumValue.SetKey(TEXT("Enum"));
  Task->FloatValue.SetKey(TEXT("Float"));
  Task->Int32Value.SetKey(TEXT("Int32"));
  Task->NameValue.SetKey(TEXT("Name"));
  Task->ObjectValue.SetKey(TEXT("Object"));
  Task->RotatorValue.SetKey(TEXT("Rotator"));
  Task->StringValue.SetKey(TEXT("String"));
  Task->StructValue.SetKey(TEXT("Struct"));
  Task->VectorValue.SetKey(TEXT("Vector"));
  if(Containers)
  {
   Task->Containers.InitializeAs<FAssetDocumentAIValueOrContainers>(); auto& V=Task->Containers.GetMutable<FAssetDocumentAIValueOrContainers>();
   V.Nested.Value.SetKey(TEXT("Int32"));
   for(int32 I=0;I<2;++I){V.StaticValues[I].Identity=I;V.StaticValues[I].Value.SetKey(TEXT("Int32")); V.Array.Add(V.StaticValues[I]);}
   V.Map.Add(TEXT("Left"),V.StaticValues[0]); V.Map.Add(TEXT("Right"),V.StaticValues[1]);
   V.Dynamic.InitializeAs<FAssetDocumentAIValueOrContainer>(); V.Dynamic.GetMutable<FAssetDocumentAIValueOrContainer>().Value.SetKey(TEXT("Int32"));
  }
  Package->SetDirtyFlag(false);
  return Begin(T);
 }
 bool Begin(FAutomationTestBase& T)
 {
  TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
  Actions.Add(MakeShared<FOrderedAuthoredAction>(TEXT("test.value_or.authored"),Order,TEXT("authored"),[this](){Retarget(A);Package->SetDirtyFlag(false);if(AfterOrdinaryRestore)AfterOrdinaryRestore();}));
  Composition=MakeUnique<FAssetDocumentRollbackActionComposition>(Resolver,MoveTemp(Actions),[this](EAssetDocumentRuntimeRequestPhase,const FAssetDocumentRuntimeDiagnostic& D){KernelDiagnostics.Add(D);});
  FAssetDocumentPreflightReceipt Receipt; Receipt.RequestId=FGuid::NewGuid(); Receipt.RuntimeRevision=1; Receipt.ReceiptHash=TEXT("sha256:value-or-fixture"); Receipt.bSucceeded=true;
  Receipt.CompleteFootprint={{EAssetDocumentMutationFootprintKind::UObjectState,TEXT("asset:")+Tree->GetPathName()+TEXT("/TestAuthored"),TEXT("test.value_or.authored")},{EAssetDocumentMutationFootprintKind::UObjectState,TEXT("asset:")+Tree->GetPathName()+TEXT("/DerivedCompletion"),TEXT("assetdocument.ai.behavior_tree.derived_completion.v1")}};
  static FNoCancellation Cancellation; Transaction=Composition->BeginLiveAfterPreflight(Receipt,Cancellation); if(!Transaction) return false;
  if(!Transaction->EnlistFootprint(MakeArrayView(Receipt.CompleteFootprint.GetData(),1)).bValid) return false;
  Resolver.SetMutationToken(Transaction->GetMutationToken()); return Reissue(T);
 }
 bool Reissue(FAutomationTestBase& T)
 {
  auto Lease=ResolveExistingMutableAssetLease(T,*Tree,*Transaction,Resolver); if(!Lease) return false;
  auto Alias=Resolver.RegisterProviderScopedAlias(Lease->GetHandle(),TEXTVIEW("ai"),EAssetDocumentHandleAccess::Mutable); if(!Alias.IsSet()) return false; Handle=Alias.GetValue();return true;
 }
 FAssetDocumentPrimitiveInvocation Base(const TCHAR* Id)
 {
  FAssetDocumentPrimitiveInvocation I; I.PrimitiveId=Id; I.Version=1; const auto& Token=Transaction->GetMutationToken(); I.RequestId=Token.RequestId; I.TransactionId=Token.TransactionId; I.RuntimeRevision=Token.RuntimeRevision; I.Phase=EAssetDocumentRuntimeRequestPhase::Repair; I.HandleArguments={{TEXT("asset"),Handle}}; I.ValueArguments.Kind=EAssetDocumentRuntimeValueKind::Object; return I;
 }
 FString Expected(FAutomationTestBase& T,int32 Version=1)
 {
  auto I=Base(TEXT("ai.behavior_tree.authored_graph.readback.v1")); I.ValueArguments.ObjectKeys={TEXT("behavior_tree_path")}; I.ValueArguments.Elements={String(Tree->GetPathName())};
  if(Version==2){I.PrimitiveId=TEXT("ai.behavior_tree.authored_graph.readback.v2");I.Version=2;}
  const auto ReadOnly=Resolver.RegisterProviderScopedAlias(Handle,TEXTVIEW("ai"),EAssetDocumentHandleAccess::ReadOnly);
  if(!T.TestTrue(TEXT("D2 uses its own exact read-only Asset alias"),ReadOnly.IsSet()))return {};
  I.HandleArguments={{TEXT("asset"),ReadOnly.GetValue()}};
  const auto R=CreateAssetDocumentAIPrimitiveProvider()->Invoke(I,Resolver,Transaction.Get());
  for(const auto& D:R.Diagnostics) T.AddInfo(D.Code+TEXT(": ")+D.Message);
  // Direct public type checks localize a malformed fixture separately from
  // the inventory's literal/property-handle observation checks.
  if(Task&&Tree->BlackboardAsset)
  {
   const TArray<FValueOrBlackboardKeyBase*> Leaves={&Task->BoolValue,&Task->ClassValue,&Task->EnumValue,&Task->FloatValue,&Task->Int32Value,&Task->NameValue,&Task->ObjectValue,&Task->RotatorValue,&Task->StringValue,&Task->StructValue,&Task->VectorValue};
   for(const auto* Leaf:Leaves)
   {
    const auto ID=Tree->BlackboardAsset->GetKeyID(Leaf->GetKey());
    const auto* Entry=Tree->BlackboardAsset->GetKey(ID);
    if(Entry&&!Leaf->IsCompatibleType(Entry->KeyType))T.AddInfo(TEXT("Fixture public IsCompatibleType failed for key ")+Leaf->GetKey().ToString()+TEXT(" / ")+GetNameSafe(Entry->KeyType));
   }
  }
  if(!T.TestEqual(TEXT("public D2 readback produces expected digest input"),R.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return {};
  return Version==2?ProducerDigestV2(Tree->GetPathName(),R.Value):ProducerDigest(Tree->GetPathName(),R.Value);
 }
 FAssetDocumentPrimitiveInvocation Invocation(const FString& Digest,int32 Version=1)
 {
  auto I=Base(TEXT("ai.behavior_tree.value_or_key.refresh.v1"));
  if(Version==2){I.PrimitiveId=TEXT("ai.behavior_tree.value_or_key.refresh.v2");I.Version=2;}
  I.ValueArguments.ObjectKeys={TEXT("behavior_tree_path"),TEXT("completion_registration"),TEXT("expected_authored_inventory_digest"),TEXT("expected_blackboard_path")};
  I.ValueArguments.Elements={String(Tree->GetPathName()),String(TEXT("current_graph")),String(Digest),String(Tree->BlackboardAsset->GetPathName())}; return I;
 }
};
bool Success(FAutomationTestBase& T,const FAssetDocumentPrimitiveOutcome& R)
{
 for(const auto& D:R.Diagnostics) T.AddInfo(D.Code+TEXT(": ")+D.Message);
 return T.TestEqual(TEXT("real ValueOr provider succeeds"),R.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
}
TStrongObjectPtr<UBlackboardComponent> MakeValueOrBlackboardComponent(FAutomationTestBase& T,UBlackboardData& BB)
{
 auto* W=GEditor?GEditor->GetEditorWorldContext().World():nullptr; if(!T.TestNotNull(TEXT("Editor world"),W)) return {};
 TStrongObjectPtr<UBlackboardComponent> C(NewObject<UBlackboardComponent>(W)); if(!T.TestTrue(TEXT("public Blackboard initialization"),C->InitializeBlackboard(BB))) return {}; return C;
}
bool Values(FAutomationTestBase& T,UAssetDocumentAIValueOrTask& V,UBlackboardComponent& C)
{
 C.SetValueAsBool(TEXT("Bool"),true); C.SetValueAsClass(TEXT("Class"),AActor::StaticClass()); C.SetValueAsEnum(TEXT("Enum"),7);
 C.SetValueAsFloat(TEXT("Float"),42.5f); C.SetValueAsInt(TEXT("Int32"),4242); C.SetValueAsName(TEXT("Name"),TEXT("RuntimeName")); C.SetValueAsObject(TEXT("Object"),&C);
 C.SetValueAsRotator(TEXT("Rotator"),FRotator(9,8,7)); C.SetValueAsString(TEXT("String"),TEXT("RuntimeString")); C.SetValueAsVector(TEXT("Vector"),FVector(89,144,233));
 FInstancedStruct S; S.InitializeAs<FVector>(FVector(21,34,55)); C.SetValue<UBlackboardKeyType_Struct>(TEXT("Struct"),FConstStructView(S));
 bool OK=T.TestTrue(TEXT("Bool resolves"),V.BoolValue.GetValue(&C));
 OK&=T.TestEqual(TEXT("Class resolves"),V.ClassValue.GetValue(&C),AActor::StaticClass()); OK&=T.TestEqual(TEXT("Enum resolves"),V.EnumValue.GetValue(&C),uint8(7));
 OK&=T.TestEqual(TEXT("Float resolves"),V.FloatValue.GetValue(&C),42.5f); OK&=T.TestEqual(TEXT("Int32 resolves"),V.Int32Value.GetValue(&C),4242);
 OK&=T.TestEqual(TEXT("Name resolves"),V.NameValue.GetValue(&C),FName(TEXT("RuntimeName"))); OK&=T.TestEqual(TEXT("Object resolves"),V.ObjectValue.GetValue(&C),static_cast<UObject*>(&C));
 OK&=T.TestEqual(TEXT("Rotator resolves"),V.RotatorValue.GetValue(&C),FRotator(9,8,7)); OK&=T.TestEqual(TEXT("String resolves"),V.StringValue.GetValue(&C),FString(TEXT("RuntimeString"))); OK&=T.TestEqual(TEXT("Vector resolves"),V.VectorValue.GetValue(&C),FVector(89,144,233));
 const FConstStructView Actual=V.StructValue.GetValue(&C); OK&=T.TestTrue(TEXT("Struct resolves"),Actual.GetScriptStruct()==TBaseStructure<FVector>::Get()&&Actual.GetMemory()&&*reinterpret_cast<const FVector*>(Actual.GetMemory())==FVector(21,34,55));
 if(V.Containers.GetScriptStruct()==FAssetDocumentAIValueOrContainers::StaticStruct())
 {
  auto& X=V.Containers.GetMutable<FAssetDocumentAIValueOrContainers>();
  auto Check=[&](FAssetDocumentAIValueOrContainer& L){OK&=T.TestEqual(TEXT("container leaf resolves by name"),L.Value.GetValue(&C),4242);};
  Check(X.Nested);for(auto& L:X.StaticValues)Check(L);for(auto& L:X.Array)Check(L);for(auto& L:X.Map)Check(L.Value);Check(X.Dynamic.GetMutable<FAssetDocumentAIValueOrContainer>());
 }
 return OK;
}
UBTTask_WaitBlackboardTime* AddHiddenWait(FAutomationTestBase& T,FScenario& S,FBlackboardKeySelector*& Selector)
{
 UBehaviorTreeGraphNode_Composite* Composite=nullptr;
 for(UEdGraphNode* Node:S.Graph->Nodes)if(auto* C=Cast<UBehaviorTreeGraphNode_Composite>(Node))Composite=C;
 if(!T.TestNotNull(TEXT("hidden Wait fixture has its stock composite"),Composite))return nullptr;
 FAISchemaAction_NewNode Action;Action.NodeTemplate=NewObject<UBehaviorTreeGraphNode_Task>();
 Action.NodeTemplate->NodeInstance=NewObject<UBTTask_WaitBlackboardTime>(Action.NodeTemplate);
 auto* Wrapper=Cast<UBehaviorTreeGraphNode_Task>(Action.PerformAction(S.Graph,Out(*Composite),FVector2f(200,200),false));
 auto* Wait=Wrapper?Cast<UBTTask_WaitBlackboardTime>(Wrapper->NodeInstance):nullptr;
 if(!T.TestNotNull(TEXT("stock action creates the hidden Wait task"),Wait))return nullptr;
 const auto* Property=FindFProperty<FStructProperty>(Wait->GetClass(),TEXT("BlackboardKey"));
 if(!T.TestTrue(TEXT("fixture uses the actual stock selector declaration"),Property
  &&Property->GetOwnerStruct()==UBTTask_WaitBlackboardTime::StaticClass()
  &&Property->Struct==FBlackboardKeySelector::StaticStruct()))return nullptr;
 Selector=Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(Wait);
 Selector->SelectedKeyName=TEXT("Float");Wait->InitializeFromAsset(*S.Tree);
 Wait->WaitTime=FValueOrBBKey_Float(17.25f);Wait->WaitTime.SetKey(TEXT("Float"));
 Wait->RandomDeviation=FValueOrBBKey_Float(0.375f);
 return Wait;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrRetargetTest,"UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh.RetargetAllFamiliesAndLateRollback",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrRetargetTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests;
 for(bool Containers:{false,true})
 {
  FScenario S;if(!S.Create(*this,Containers))return false;
  auto Old=MakeValueOrBlackboardComponent(*this,*S.A);if(!Old.IsValid()||!Values(*this,*S.Task,*Old))return false;
  S.Retarget(S.B);const FString Expected=S.Expected(*this);if(Expected.IsEmpty())return false;
  FInstrumentedTransaction Proxy(*S.Transaction);const auto R=CreateAssetDocumentAIPrimitiveProvider()->Invoke(S.Invocation(Expected),S.Resolver,&Proxy);if(!Success(*this,R))return false;
  TestEqual(TEXT("one finalizer followed by one row"),Proxy.Calls,TArray<FString>{TEXT("finalizer"),TEXT("footprint")});TestTrue(TEXT("derived completion has no ChangedSet"),R.ChangedSet.IsEmpty());TestTrue(TEXT("derived completion returns no handles"),R.HandleResults.IsEmpty());
  const int32 CountIndex=R.Value.ObjectKeys.IndexOfByKey(TEXT("leaf_count"));if(!TestTrue(TEXT("success leaf_count exists"),CountIndex!=INDEX_NONE))return false;
  TestEqual(TEXT("eleven families plus eight container leaves"),R.Value.Elements[CountIndex].IntegerValue,int64(Containers?19:11));
  auto New=MakeValueOrBlackboardComponent(*this,*S.B);if(!New.IsValid()||!Values(*this,*S.Task,*New))return false;
  if(!S.Reissue(*this))return false;TestEqual(TEXT("public authored value unchanged across derived completion"),S.Expected(*this),Expected);
  TestTrue(TEXT("late Atomic rollback restores A and recompletes"),S.Transaction->Rollback(TEXTVIEW("Test.LateValueOrFailure")).bValid);
  TestEqual(TEXT("ordinary restoration executed"),S.Order,TArray<FString>{TEXT("authored")});TestTrue(TEXT("reference restored to A"),S.Tree->BlackboardAsset==S.A);TestFalse(TEXT("dirty state restored"),S.Package->IsDirty());
  if(!Values(*this,*S.Task,*Old))return false;
 }
 // Actual stock hidden-category leaves must participate in the same complete
 // typed inventory. Sparse V2 also preserves a retained literal/empty selector.
 for(const int32 Case:{0,1,2})
 {
  const int32 Version=Case==0?1:2;const bool Literal=Case==2;
  FScenario S;if(!S.Create(*this))return false;
  FBlackboardKeySelector* Selector=nullptr;auto* Wait=AddHiddenWait(*this,S,Selector);if(!Wait)return false;
  const auto* CDO=GetDefault<UBTTask_WaitBlackboardTime>();
  const auto CDOAuthored=[CDO](){return TArray<FString>{CDO->GetSelectedBlackboardKey().ToString(),
   CDO->WaitTime.GetKey().ToString(),FString::SanitizeFloat(CDO->WaitTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr))),
   CDO->RandomDeviation.GetKey().ToString(),FString::SanitizeFloat(CDO->RandomDeviation.GetValue(static_cast<const UBlackboardComponent*>(nullptr)))};};
  const auto CDOBefore=CDOAuthored();const bool CDODirty=CDO->GetPackage()->IsDirty();
  if(Literal)
  {
   Selector->SelectedKeyName=NAME_None;Selector->InvalidateResolvedKey();Selector->SelectedKeyType=nullptr;
   Wait->WaitTime=FValueOrBBKey_Float(17.25f);
   S.AfterOrdinaryRestore=[Wait,Selector](){Selector->SelectedKeyName=NAME_None;Selector->InvalidateResolvedKey();
    Selector->SelectedKeyType=nullptr;Wait->WaitTime=FValueOrBBKey_Float(17.25f);};
  }
  const FName Key=Wait->WaitTime.GetKey();
  using namespace UE::AssetDocumentAI::BehaviorTreeValueOrKey;
  const auto ProjectionVersion=Version==1?UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion::V1
   :UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion::V2;
  S.Package->SetDirtyFlag(false);FInventory Captured;FString Diagnostic;
  if(!TestTrue(TEXT("complete inventory includes the real hidden Wait fields"),Capture(*S.Tree,Captured,Diagnostic,ProjectionVersion)))
  {AddInfo(Diagnostic);return false;}
  TestEqual(TEXT("eleven fixture leaves plus both hidden Wait floats"),Captured.Leaves.Num(),13);
  TestEqual(TEXT("capture leaves hidden authored mode intact"),Wait->WaitTime.GetKey(),Key);
  TestEqual(TEXT("capture leaves hidden default intact"),Wait->WaitTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr)),17.25f);
  TestFalse(TEXT("hidden data-node generation does not dirty the live package"),S.Package->IsDirty());
  // A hidden authored change must reject the saved inventory before even the
  // first typed cache visitor. This would be invisible to the public projection.
  Wait->WaitTime=FValueOrBBKey_Float(18.25f);Wait->WaitTime.SetKey(Key);int32 Visits=0;
  TestFalse(TEXT("hidden default change rejects the old complete inventory"),VisitExactLeaves(*S.Tree,Captured,
   [&Visits](FExactLeafReference&,const UE::AssetDocumentAI::BehaviorTreeValueOrKey::FLeaf&){++Visits;return true;},Diagnostic));
  TestEqual(TEXT("hidden inventory mismatch is closed"),Diagnostic,FString(TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected")));
  TestEqual(TEXT("hidden mismatch invokes no cache sink"),Visits,0);
  Wait->WaitTime=FValueOrBBKey_Float(17.25f);Wait->WaitTime.SetKey(TEXT("String"));
  FInventory RejectedInventory;
  TestFalse(TEXT("hidden Float rejects an actual String blackboard key"),Capture(*S.Tree,RejectedInventory,Diagnostic,ProjectionVersion));
  TestEqual(TEXT("hidden wrong key type has its exact diagnostic"),Diagnostic,FString(TEXT("AIProvider.BehaviorTree.ValueOrKeyTypeMismatch")));
  TestEqual(TEXT("rejected observation retains the authored incompatible key"),Wait->WaitTime.GetKey(),FName(TEXT("String")));
  Wait->WaitTime.SetKey(Key);
  auto Old=MakeValueOrBlackboardComponent(*this,*S.A);if(!Old.IsValid())return false;
  Old->SetValueAsFloat(TEXT("Float"),42.5f);
  TestEqual(TEXT("hidden Wait primes its real old key cache"),Wait->WaitTime.GetValue(Old.Get()),Literal?17.25f:42.5f);
  if(Literal){S.Tree->BlackboardAsset=S.B;S.Root->BlackboardAsset=S.B;}
  else S.Retarget(S.B);
  TestNotEqual(TEXT("fixture retarget actually reorders the Float slot"),S.A->GetKeyID(TEXT("Float")),S.B->GetKeyID(TEXT("Float")));
  S.Package->SetDirtyFlag(false);const FString Expected=S.Expected(*this,Version);if(Expected.IsEmpty())return false;
  FInstrumentedTransaction Proxy(*S.Transaction);
  const auto Result=CreateAssetDocumentAIPrimitiveProvider()->Invoke(S.Invocation(Expected,Version),S.Resolver,&Proxy);
  if(!Success(*this,Result))return false;
  const int32 Count=Result.Value.ObjectKeys.IndexOfByKey(TEXT("leaf_count"));
  if(!TestTrue(TEXT("hidden refresh reports its complete leaf count"),Count!=INDEX_NONE))return false;
  TestEqual(TEXT("refresh covers thirteen actual leaves"),Result.Value.Elements[Count].IntegerValue,int64(13));
  TestEqual(TEXT("hidden completion registers exactly the top finalizer and row"),Proxy.Calls,TArray<FString>{TEXT("finalizer"),TEXT("footprint")});
  auto New=MakeValueOrBlackboardComponent(*this,*S.B);if(!New.IsValid())return false;New->SetValueAsFloat(TEXT("Float"),73.25f);
  TestEqual(TEXT("hidden Wait resolves the retargeted slot or retained literal"),Wait->WaitTime.GetValue(New.Get()),Literal?17.25f:73.25f);
  TestEqual(TEXT("hidden RandomDeviation literal is preserved"),Wait->RandomDeviation.GetValue(New.Get()),0.375f);
  TestEqual(TEXT("hidden Wait mode remains authored"),Wait->WaitTime.GetKey(),Key);
  if(Literal)
  {
   TestTrue(TEXT("sparse V2 does not synthesize the retained selector"),Selector->SelectedKeyName.IsNone());
   TestEqual(TEXT("retained None selector cache remains invalid"),Selector->GetSelectedKeyID(),FBlackboard::InvalidKey);
   TestNull(TEXT("retained None selector type remains empty"),Selector->SelectedKeyType.Get());
  }
  TestFalse(TEXT("derived hidden completion preserves package cleanliness"),S.Package->IsDirty());
  if(!S.Reissue(*this))return false;TestEqual(TEXT("hidden completion keeps public authored bytes"),S.Expected(*this,Version),Expected);
  TestTrue(TEXT("hidden leaves join real late Atomic recompletion"),S.Transaction->Rollback(TEXTVIEW("Test.HiddenWaitRollback")).bValid);
  TestEqual(TEXT("hidden rollback restores Wait key mode"),Wait->WaitTime.GetKey(),Key);
  TestEqual(TEXT("hidden rollback restores the old resolved slot or literal"),Wait->WaitTime.GetValue(Old.Get()),Literal?17.25f:42.5f);
  TestEqual(TEXT("hidden inventory and cache completion leave CDO authored values unchanged"),CDOAuthored(),CDOBefore);
  TestEqual(TEXT("hidden inventory preserves CDO package state"),CDO->GetPackage()->IsDirty(),CDODirty);
  TestFalse(TEXT("hidden rollback restores package cleanliness"),S.Package->IsDirty());
 }
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrAdmissionTest,"UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh.AdmissionZeroWrite",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrAdmissionTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests;
 const TCHAR* Codes[]={TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected"),TEXT("AIProvider.OperationRejected"),TEXT("AIProvider.BehaviorTree.ValueOrBlackboardMismatch"),TEXT("AIProvider.InvalidValueShape"),TEXT("AIProvider.InvalidContext"),TEXT("AIProvider.InvalidTransaction"),TEXT("AIProvider.InvalidHandle")};
 for(int32 Case=0;Case<7;++Case)
 {
  FScenario S;if(!S.Create(*this))return false;const FString Digest=S.Expected(*this);if(Digest.IsEmpty())return false;
  auto I=S.Invocation(Digest);if(Case==0)I.ValueArguments.Elements[2]=String(FString::ChrN(64,'0'));if(Case==1)I.ValueArguments.Elements[1]=String(TEXT("installed_replacement"));if(Case==2)I.ValueArguments.Elements[3]=String(S.B->GetPathName());if(Case==3)I.ValueArguments.Elements[1]=String(TEXT("unknown"));if(Case==4)I.Phase=EAssetDocumentRuntimeRequestPhase::Apply;if(Case==5)I.TransactionId=FGuid::NewGuid();if(Case==6)++I.HandleArguments[0].Handle.Generation;
  FInstrumentedTransaction Proxy(*S.Transaction);auto R=CreateAssetDocumentAIPrimitiveProvider()->Invoke(I,S.Resolver,&Proxy);
  TestTrue(TEXT("exact admission diagnostic"),ExpectedCode(*this,R,Codes[Case],FString::Printf(TEXT("Admission case %d"),Case)));TestEqual(TEXT("no fallback finalizer registration"),Proxy.FinalizerCalls,0);TestEqual(TEXT("only installed mode asks Atomic to validate its missing coverage"),Proxy.FootprintCalls,Case==1?1:0);TestTrue(TEXT("failure Value is an empty Object"),R.Value.Kind==EAssetDocumentRuntimeValueKind::Object&&R.Value.ObjectKeys.IsEmpty()&&R.Value.Elements.IsEmpty());TestTrue(TEXT("failure handles/changes empty"),R.HandleResults.IsEmpty()&&R.ChangedSet.IsEmpty());
 }
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrFaultTest,"UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh.EveryFamilyFaultAtomicRollback",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrFaultTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests;
 using namespace AssetDocumentAIBlackboardValueOrKeyCacheInvalidation;
 // Enum values are the existing immutable invalidator's eleven before/after pairs.
 for(uint8 Point=uint8(EValueOrKeyCacheInvalidationTestFailurePoint::BeforeBool);Point<=uint8(EValueOrKeyCacheInvalidationTestFailurePoint::AfterVector);++Point)
 {
  FScenario S;if(!S.Create(*this))return false;
  auto Old=MakeValueOrBlackboardComponent(*this,*S.A);if(!Old.IsValid()||!Values(*this,*S.Task,*Old))return false;
  S.Retarget(S.B);const FString Digest=S.Expected(*this);if(Digest.IsEmpty())return false;
  SetValueOrKeyCacheInvalidationTestFailurePoint(EValueOrKeyCacheInvalidationTestFailurePoint(Point));
  FInstrumentedTransaction Proxy(*S.Transaction);
  const auto R=CreateAssetDocumentAIPrimitiveProvider()->Invoke(S.Invocation(Digest),S.Resolver,&Proxy);
  SetValueOrKeyCacheInvalidationTestFailurePoint(EValueOrKeyCacheInvalidationTestFailurePoint::None);
  TestTrue(TEXT("each exact family failure has closed invalidation diagnostic"),ExpectedCode(*this,R,TEXT("AIProvider.BehaviorTree.ValueOrInvalidationFailed"),FString::Printf(TEXT("Invalidator failure point %d"),Point)));
  TestTrue(TEXT("failure Value is an empty Object"),R.Value.Kind==EAssetDocumentRuntimeValueKind::Object&&R.Value.ObjectKeys.IsEmpty()&&R.Value.Elements.IsEmpty());
  TestTrue(TEXT("failure has no handles or changes"),R.HandleResults.IsEmpty()&&R.ChangedSet.IsEmpty());
  TestEqual(TEXT("failure registered only the combined finalizer"),Proxy.FinalizerCalls,1);
  TestTrue(TEXT("ordinary actions then terminal recompletion recover from partial leaves"),S.Transaction->Rollback(TEXTVIEW("Test.InjectedValueOrFailure")).bValid);
  TestTrue(TEXT("A restored after partial failure"),S.Tree->BlackboardAsset==S.A);
  if(!Values(*this,*S.Task,*Old))return false;
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrDeletedTest,"UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh.DeletedSameTypeSlotReturnsDefaults",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrDeletedTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests;
 FScenario S;if(!S.Create(*this))return false;
 auto Old=MakeValueOrBlackboardComponent(*this,*S.A);if(!Old.IsValid()||!Values(*this,*S.Task,*Old))return false;
 // Keep every old numeric slot occupied by the same key type, with another name.
 S.B->Keys=S.A->Keys;
 for(auto& K:S.B->Keys)K.EntryName=FName(*(TEXT("Decoy_")+K.EntryName.ToString()));
 S.Retarget(S.B);
 const TArray<FName> DeletedAuthoredKeys=S.AuthoredKeys();const bool DirtyBeforeRead=S.Package->IsDirty();
 const FString Digest=S.Expected(*this);
 TestEqual(TEXT("public D2 preserves all eleven missing authored names"),S.AuthoredKeys(),DeletedAuthoredKeys);
 TestEqual(TEXT("public D2 cannot dirty a missing-name graph"),S.Package->IsDirty(),DirtyBeforeRead);
 if(Digest.IsEmpty())return false;
 const auto Refreshed=CreateAssetDocumentAIPrimitiveProvider()->Invoke(S.Invocation(Digest),S.Resolver,S.Transaction.Get());
 TestEqual(TEXT("derived refresh preserves all eleven missing authored names"),S.AuthoredKeys(),DeletedAuthoredKeys);
 TestEqual(TEXT("derived refresh preserves original dirty state"),S.Package->IsDirty(),DirtyBeforeRead);
 if(!Success(*this,Refreshed))return false;
 auto New=MakeValueOrBlackboardComponent(*this,*S.B);if(!New.IsValid())return false;
 New->SetValueAsBool(TEXT("Decoy_Bool"),true);New->SetValueAsClass(TEXT("Decoy_Class"),AActor::StaticClass());New->SetValueAsEnum(TEXT("Decoy_Enum"),7);New->SetValueAsFloat(TEXT("Decoy_Float"),99);New->SetValueAsInt(TEXT("Decoy_Int32"),99);New->SetValueAsName(TEXT("Decoy_Name"),TEXT("Wrong"));New->SetValueAsObject(TEXT("Decoy_Object"),New.Get());New->SetValueAsRotator(TEXT("Decoy_Rotator"),FRotator(9,9,9));New->SetValueAsString(TEXT("Decoy_String"),TEXT("Wrong"));New->SetValueAsVector(TEXT("Decoy_Vector"),FVector(9,9,9));
 FInstancedStruct Decoy;Decoy.InitializeAs<FVector>(FVector(9,9,9));New->SetValue<UBlackboardKeyType_Struct>(TEXT("Decoy_Struct"),FConstStructView(Decoy));
 auto& V=*S.Task;
 TestFalse(TEXT("deleted Bool uses literal"),V.BoolValue.GetValue(New.Get()));TestNull(TEXT("deleted Class uses literal"),V.ClassValue.GetValue(New.Get()));TestEqual(TEXT("deleted Enum uses literal"),V.EnumValue.GetValue(New.Get()),uint8(EAssetDocumentAIReadbackEnum::First));TestEqual(TEXT("deleted Float uses literal"),V.FloatValue.GetValue(New.Get()),1.25f);TestEqual(TEXT("deleted Int32 uses literal"),V.Int32Value.GetValue(New.Get()),37);TestEqual(TEXT("deleted Name uses literal"),V.NameValue.GetValue(New.Get()),FName(TEXT("DefaultName")));TestNull(TEXT("deleted Object uses literal"),V.ObjectValue.GetValue(New.Get()));TestEqual(TEXT("deleted Rotator uses literal"),V.RotatorValue.GetValue(New.Get()),FRotator(1,2,3));TestEqual(TEXT("deleted String uses literal"),V.StringValue.GetValue(New.Get()),FString(TEXT("DefaultString")));TestEqual(TEXT("deleted Vector uses literal"),V.VectorValue.GetValue(New.Get()),FVector(5,8,13));
 const auto Actual=V.StructValue.GetValue(New.Get());TestTrue(TEXT("deleted Struct uses literal"),Actual.GetScriptStruct()==TBaseStructure<FVector>::Get()&&Actual.GetMemory()&&*reinterpret_cast<const FVector*>(Actual.GetMemory())==FVector(2,3,5));
 TestTrue(TEXT("rollback still recovers original mapping"),S.Transaction->Rollback(TEXTVIEW("Test.DeletedKeyRollback")).bValid);
 return Values(*this,*S.Task,*Old)&&!HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrGoldenTest,"UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh.PublicProducerGolden",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrGoldenTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests;
 // Constants independently generated from the documented byte grammar with
 // Python struct.pack('>Q' / '>d') and hashlib.sha256(hex_ascii), never either C++ encoder.
 const FString Path=TEXT("/Game/Tests/Tree.Tree");
 FAssetDocumentRuntimeValueDto EmptyArray;EmptyArray.Kind=EAssetDocumentRuntimeValueKind::Array;
 FAssetDocumentRuntimeValueDto D2Absent;D2Absent.Kind=EAssetDocumentRuntimeValueKind::Object;
 D2Absent.ObjectKeys={TEXT("comments"),TEXT("graph_guid"),TEXT("graph_state"),TEXT("root")};
 D2Absent.Elements={EmptyArray,FAssetDocumentRuntimeValueDto(),String(TEXT("absent")),FAssetDocumentRuntimeValueDto()};
 const FString AbsentGolden=TEXT("1af504e7713643b160eb6b3ac9262a142ba9276d065e31ebab3bb60e822c0da9");
 TestEqual(TEXT("independent producer hashes the frozen public D2 absent graph Value"),ProducerDigest(Path,D2Absent),AbsentGolden);
 FString Actual;TestTrue(TEXT("provider hashes the same public Value"),UE::AssetDocumentAI::BehaviorTreeValueOrKey::HashAuthoredGraphValue(Path,D2Absent,Actual));TestEqual(TEXT("public Value alone produces exact provider wire digest"),Actual,AbsentGolden);
 FAssetDocumentRuntimeValueDto Zero;Zero.Kind=EAssetDocumentRuntimeValueKind::Number;Zero.NumberValue=-0.0;
 FAssetDocumentRuntimeValueDto Unicode;Unicode.Kind=EAssetDocumentRuntimeValueKind::Array;Unicode.Elements={String(TEXT("显示_Name_7")),Zero};
 const FString UnicodeGolden=TEXT("346751651fcacfe4184b30a930b6dde87f07054869b53b06dc39e93e9a8eebf9");
 TestEqual(TEXT("producer UTF8 and negative zero golden"),ProducerDigest(Path,Unicode),UnicodeGolden);
 TestTrue(TEXT("provider encodes UTF8 and zero"),UE::AssetDocumentAI::BehaviorTreeValueOrKey::HashAuthoredGraphValue(Path,Unicode,Actual));TestEqual(TEXT("provider UTF8 negative zero golden"),Actual,UnicodeGolden);
 Unicode.Elements[1].NumberValue=0.0;TestEqual(TEXT("negative zero canonicalizes to positive zero"),ProducerDigest(Path,Unicode),UnicodeGolden);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrNullableTest,"UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh.NullableV3CompletionAndRollback",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrNullableTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests;
 for(int32 Case=0;Case<6;++Case)
 {
  FScenario S;if(!S.Create(*this))return false;
  const TArray<FValueOrBlackboardKeyBase*> Leaves={&S.Task->BoolValue,&S.Task->ClassValue,&S.Task->EnumValue,&S.Task->FloatValue,&S.Task->Int32Value,&S.Task->NameValue,&S.Task->ObjectValue,&S.Task->RotatorValue,&S.Task->StringValue,&S.Task->StructValue,&S.Task->VectorValue};
  for(auto* Leaf:Leaves)Leaf->SetKey(NAME_None);
  if(Case==0)
   for(UEdGraphNode* N:S.Graph->Nodes)if(auto* TaskNode=Cast<UBehaviorTreeGraphNode_Task>(N))TaskNode->NodeInstance=NewObject<UAssetDocumentAIValueOrEmptyTask>(S.Tree.Get());
  if(Case==2)S.Task->Int32Value.SetKey(TEXT("Int32"));
  // Build the request before making the actual live header null: the legacy
  // fixture helper deliberately only supports the old non-null contract.
  auto I=S.Invocation(TEXT("unused"));
  I.PrimitiveId=Case==3?TEXT("ai.behavior_tree.value_or_key.refresh.v1"):
   Case==4?TEXT("ai.behavior_tree.value_or_key.refresh.v2"):TEXT("ai.behavior_tree.value_or_key.refresh.v3");
  I.Version=Case==3?1:Case==4?2:3;
  S.Retarget(Case==5?S.B:nullptr);S.Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);S.Package->SetDirtyFlag(false);
  if(Case==5)S.AfterOrdinaryRestore=[&S](){S.Retarget(nullptr);};
  const FString Digest=S.Expected(*this,I.Version==1?1:2);if(Digest.IsEmpty())return false;
  I.ValueArguments.Elements[2]=String(Digest);
  I.ValueArguments.Elements[3]=FAssetDocumentRuntimeValueDto();I.ValueArguments.Elements[3].Kind=EAssetDocumentRuntimeValueKind::Null;
  if(Case==5)I.ValueArguments.Elements[3]=String(S.B->GetPathName());
  FInstrumentedTransaction Proxy(*S.Transaction);
  const auto R=CreateAssetDocumentAIPrimitiveProvider()->Invoke(I,S.Resolver,&Proxy);
  if(Case<2 || Case==5)
  {
   if(!Success(*this,R))return false;
   TestEqual(TEXT("null still registers full finalizer and row"),Proxy.Calls,TArray<FString>{TEXT("finalizer"),TEXT("footprint")});
   TestTrue(TEXT("null still completes runtime mirror"),S.Tree->RootNode!=nullptr);
   TestEqual(TEXT("forward retains exact requested Blackboard"),S.Tree->BlackboardAsset.Get(),Case==5?S.B:nullptr);
   TestTrue(TEXT("null completion fully enlists receipt"),S.Transaction->MarkReadyToCommit().bValid);
  }
  else
  {
   TestNotEqual(TEXT("key dependency or old nullable schema rejected"),R.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
   TestTrue(TEXT("rejection precedes finalizer/row"),Proxy.Calls.IsEmpty());
  }
  TestTrue(TEXT("real Atomic rollback after nullable completion/rejection"),S.Transaction->Rollback(TEXTVIEW("Test.NullableV3")).bValid);
  TestEqual(TEXT("ordinary Blackboard restoration preserved"),S.Tree->BlackboardAsset.Get(),Case==5?nullptr:S.A);
  TestFalse(TEXT("nullable terminal dirty ownership"),S.Package->IsDirty());
 }
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrEmptyTest,"UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh.EmptyInventoryStillTopMode",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrEmptyTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests;
 FScenario S;if(!S.Create(*this))return false;
 for(UEdGraphNode* N:S.Graph->Nodes)if(auto* T=Cast<UBehaviorTreeGraphNode_Task>(N))T->NodeInstance=NewObject<UAssetDocumentAIValueOrEmptyTask>(S.Tree.Get());
 S.Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);S.Package->SetDirtyFlag(false);
 const FString Digest=S.Expected(*this);if(Digest.IsEmpty())return false;
 FInstrumentedTransaction Proxy(*S.Transaction);const auto R=CreateAssetDocumentAIPrimitiveProvider()->Invoke(S.Invocation(Digest),S.Resolver,&Proxy);if(!Success(*this,R))return false;
 TestEqual(TEXT("empty inventory still registers the top-mode finalizer and row"),Proxy.Calls,TArray<FString>{TEXT("finalizer"),TEXT("footprint")});
 const int32 Count=R.Value.ObjectKeys.IndexOfByKey(TEXT("leaf_count"));const int32 Families=R.Value.ObjectKeys.IndexOfByKey(TEXT("family_counts"));
 if(!TestTrue(TEXT("counts present"),Count!=INDEX_NONE&&Families!=INDEX_NONE))return false;
 TestEqual(TEXT("empty leaf count"),R.Value.Elements[Count].IntegerValue,int64(0));TestEqual(TEXT("all eleven zero family counts are present"),R.Value.Elements[Families].Elements.Num(),11);
 for(const auto& V:R.Value.Elements[Families].Elements)TestEqual(TEXT("zero family count"),V.IntegerValue,int64(0));
 TestTrue(TEXT("empty inventory still rebuilds runtime mirror"),S.Tree->RootNode!=nullptr);
 return S.Transaction->Rollback(TEXTVIEW("Test.EmptyRollback")).bValid&&!HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrTypedRejectionTest,"UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh.TypedContainerAndKeyTypeRejections",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrTypedRejectionTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests;
 for(int32 Case=0;Case<3;++Case)
 {
  FScenario S;if(!S.Create(*this,true))return false;
  FString Digest=S.Expected(*this);if(Digest.IsEmpty())return false;
  const TCHAR* Code=TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected");
  if(Case==0){auto& A=S.Task->Containers.GetMutable<FAssetDocumentAIValueOrContainers>().Array;A[1]=A[0];Digest=S.Expected(*this);}
  if(Case==1){for(UEdGraphNode* N:S.Graph->Nodes)if(Cast<UBehaviorTreeGraphNode_Task>(N))N->NodeGuid.Invalidate();}
  FName WrongTypeAuthoredKey;bool WrongTypeDirty=false;
  if(Case==2)
  {
   S.B->Keys[S.B->GetKeyID(TEXT("Int32"))].KeyType=NewObject<UBlackboardKeyType_String>(S.B);S.Retarget(S.B);
   WrongTypeAuthoredKey=S.Task->Int32Value.GetKey();WrongTypeDirty=S.Package->IsDirty();
   Digest=S.Expected(*this);
   TestEqual(TEXT("public D2 preserves incompatible authored key name"),S.Task->Int32Value.GetKey(),WrongTypeAuthoredKey);
   TestEqual(TEXT("public D2 preserves dirty state for incompatible type"),S.Package->IsDirty(),WrongTypeDirty);
   Code=TEXT("AIProvider.BehaviorTree.ValueOrKeyTypeMismatch");
  }
  if(Digest.IsEmpty())return false;
  FInstrumentedTransaction Proxy(*S.Transaction);const auto R=CreateAssetDocumentAIPrimitiveProvider()->Invoke(S.Invocation(Digest),S.Resolver,&Proxy);
  if(Case==2)
  {
   TestEqual(TEXT("rejected refresh preserves incompatible authored key name"),S.Task->Int32Value.GetKey(),WrongTypeAuthoredKey);
   TestEqual(TEXT("rejected refresh preserves dirty state"),S.Package->IsDirty(),WrongTypeDirty);
  }
  TestTrue(TEXT("typed admission has exact rejection"),ExpectedCode(*this,R,Code,FString::Printf(TEXT("Typed case %d"),Case)));TestEqual(TEXT("typed rejection precedes registration"),Proxy.FinalizerCalls,0);TestTrue(TEXT("typed rejection has empty results"),R.Value.Kind==EAssetDocumentRuntimeValueKind::Object&&R.Value.ObjectKeys.IsEmpty()&&R.Value.Elements.IsEmpty()&&R.HandleResults.IsEmpty()&&R.ChangedSet.IsEmpty());
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrCompletionFaultTest,"UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh.MirrorAndObservationFaults",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrCompletionFaultTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests;
 using namespace UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private;
 for(auto Point:{EValueOrCompletionFailure::BeforeMirror,EValueOrCompletionFailure::BeforeSecondObservation})
 {
  FScenario S;if(!S.Create(*this))return false;auto Old=MakeValueOrBlackboardComponent(*this,*S.A);if(!Old.IsValid()||!Values(*this,*S.Task,*Old))return false;
  S.Retarget(S.B);const FString Digest=S.Expected(*this);if(Digest.IsEmpty())return false;
  SetValueOrCompletionFailureForTest(Point);const auto R=CreateAssetDocumentAIPrimitiveProvider()->Invoke(S.Invocation(Digest),S.Resolver,S.Transaction.Get());SetValueOrCompletionFailureForTest(EValueOrCompletionFailure::None);
  TestTrue(TEXT("mirror or observation fault returns completion mismatch"),ExpectedCode(*this,R,TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"),FString::Printf(TEXT("Completion failure point %d"),int32(Point))));TestTrue(TEXT("completion failure empty results"),R.Value.Kind==EAssetDocumentRuntimeValueKind::Object&&R.Value.ObjectKeys.IsEmpty()&&R.Value.Elements.IsEmpty()&&R.HandleResults.IsEmpty()&&R.ChangedSet.IsEmpty());
  TestTrue(TEXT("terminal rollback recovers after late derived failure"),S.Transaction->Rollback(TEXTVIEW("Test.CompletionFailure")).bValid);if(!Values(*this,*S.Task,*Old))return false;
 }
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrPoisonTest,"UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh.WrongRestoredLeafPoisonsAtomic",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrPoisonTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests;
 FScenario S;if(!S.Create(*this,true))return false;
 S.Retarget(S.B);const FString Digest=S.Expected(*this);if(Digest.IsEmpty())return false;
 FInstrumentedTransaction Proxy(*S.Transaction);if(!Success(*this,CreateAssetDocumentAIPrimitiveProvider()->Invoke(S.Invocation(Digest),S.Resolver,&Proxy)))return false;
 S.AfterOrdinaryRestore=[&S](){S.Task->Containers.GetMutable<FAssetDocumentAIValueOrContainers>().Array.RemoveAt(0);};
 const auto Result=S.Transaction->Rollback(TEXTVIEW("Test.WrongRestoredLeaf"));
 TestFalse(TEXT("wrong restored typed inventory fails real Atomic rollback"),Result.bValid);
 TestTrue(TEXT("kernel reports terminal finalizer poison"),S.KernelDiagnostics.ContainsByPredicate([](const auto& D){return D.Code==TEXT("Atomic.Poisoned.PostRollbackFinalizerFailed");}));
 TestEqual(TEXT("ordinary restoration ran once before the terminal poison"),S.Order,TArray<FString>{TEXT("authored")});
 TestEqual(TEXT("no replacement finalizer registered after poison"),Proxy.FinalizerCalls,1);
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrVersionDomainTest,
 "UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefreshV2.DigestDomainsRejectCrossVersion",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrVersionDomainTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests;
 for(const int32 Version:{1,2})
 {
  FScenario S;if(!S.Create(*this))return false;
  const FString V1=S.Expected(*this,1),V2=S.Expected(*this,2);if(V1.IsEmpty()||V2.IsEmpty())return false;
  TestNotEqual(TEXT("same public content has explicitly distinct version domains"),V1,V2);
  const auto Keys=S.AuthoredKeys();const bool Dirty=S.Package->IsDirty();
  FInstrumentedTransaction Proxy(*S.Transaction);
  const auto Wrong=CreateAssetDocumentAIPrimitiveProvider()->Invoke(S.Invocation(Version==1?V2:V1,Version),S.Resolver,&Proxy);
  TestTrue(TEXT("opposite version digest is rejected"),ExpectedCode(*this,Wrong,TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected"),TEXT("Cross-version")));
  TestEqual(TEXT("cross-version rejection registers no finalizer"),Proxy.FinalizerCalls,0);
  TestEqual(TEXT("cross-version rejection enlists no row"),Proxy.FootprintCalls,0);
  TestEqual(TEXT("cross-version rejection preserves authored keys"),S.AuthoredKeys(),Keys);
  TestEqual(TEXT("cross-version rejection preserves dirty state"),S.Package->IsDirty(),Dirty);
  TestTrue(TEXT("cross-version failure results remain empty"),Wrong.Value.Kind==EAssetDocumentRuntimeValueKind::Object&&Wrong.Value.ObjectKeys.IsEmpty()&&Wrong.Value.Elements.IsEmpty()&&Wrong.HandleResults.IsEmpty()&&Wrong.ChangedSet.IsEmpty());
  if(!Success(*this,CreateAssetDocumentAIPrimitiveProvider()->Invoke(S.Invocation(Version==1?V1:V2,Version),S.Resolver,&Proxy)))return false;
  TestEqual(TEXT("correct version registers exactly one finalizer"),Proxy.FinalizerCalls,1);
  TestTrue(TEXT("correct version completes actual Atomic rollback"),S.Transaction->Rollback(TEXTVIEW("Test.VersionDomainRollback")).bValid);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrV2BoundRollbackTest,
 "UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefreshV2.BoundCommentAndEmptyInputsRollback",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrV2BoundRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests;
 FScenario S;if(!S.Create(*this))return false;
 UBehaviorTreeGraphNode_Task* Parent=nullptr;
 for(UEdGraphNode* N:S.Graph->Nodes)if(auto* T=Cast<UBehaviorTreeGraphNode_Task>(N))if(T->NodeInstance==S.Task)Parent=T;
 if(!Parent)return false;
 S.Graph->LockUpdates();
 FAISchemaAction_NewSubNode Action;Action.ParentNode=Parent;Action.NodeTemplate=NewObject<UBehaviorTreeGraphNode_CompositeDecorator>();
 Action.PerformAction(S.Graph,nullptr,FVector2f::ZeroVector,false);
 auto* Wrapper=Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Parent->Decorators.Last());
 auto* Bound=Wrapper?Cast<UBehaviorTreeDecoratorGraph>(Wrapper->BoundGraph):nullptr;
 if(!Bound){S.Graph->UnlockUpdates();return false;}
 FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Decorator> Creator(*Bound);
 auto* Leaf=Creator.CreateNode(false);Leaf->ClassData=FGraphNodeClassData(UBTDecorator_Cooldown::StaticClass(),FString());Creator.Finalize();
 UEdGraphPin* Sink=nullptr;for(auto* P:Bound->Nodes[0]->Pins)if(P&&P->Direction==EGPD_Input)Sink=P;
 if(!Sink||!Out(*Leaf)||!Bound->GetSchema()->TryCreateConnection(Out(*Leaf),Sink)){S.Graph->UnlockUpdates();return false;}
 auto* Logic=AssetDocumentAIBehaviorTreeAuthoredGraphReadbackTests::AddEmptyReadbackLogic(*Bound,5);
 FAISchemaAction_AddComment CommentAction;auto* Comment=Cast<UEdGraphNode_Comment>(CommentAction.PerformAction(Bound,nullptr,FVector2f(320,400),false));
 if(!Logic||!Comment){S.Graph->UnlockUpdates();return false;}
 Comment->NodeComment=TEXT("V2 retained bound comment");Comment->NodeWidth=480;Comment->NodeHeight=208;Comment->CommentColor=FLinearColor(0.25f,0.5f,0.75f,1);
 const FGuid LogicId=Logic->NodeGuid,CommentId=Comment->NodeGuid;
 S.Graph->UnlockUpdates();S.Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);S.Package->SetDirtyFlag(false);
 const FString Original=S.Expected(*this,2);if(Original.IsEmpty())return false;
 auto Old=MakeValueOrBlackboardComponent(*this,*S.A);if(!Old.IsValid()||!Values(*this,*S.Task,*Old))return false;
 S.Retarget(S.B);const FString Changed=S.Expected(*this,2);if(Changed.IsEmpty())return false;
 FInstrumentedTransaction Proxy(*S.Transaction);
 if(!Success(*this,CreateAssetDocumentAIPrimitiveProvider()->Invoke(S.Invocation(Changed,2),S.Resolver,&Proxy)))return false;
 if(!S.Reissue(*this))return false;
 TestEqual(TEXT("V2 Repair preserves full public bound content"),S.Expected(*this,2),Changed);
 TestTrue(TEXT("V2 bound content completes actual Atomic rollback"),S.Transaction->Rollback(TEXTVIEW("Test.V2BoundLateFailure")).bValid);
 TestTrue(TEXT("rollback restores Blackboard A"),S.Tree->BlackboardAsset==S.A);
 if(!Values(*this,*S.Task,*Old))return false;
 int32 Inputs=0;for(auto* P:Logic->Pins)if(P&&P->Direction==EGPD_Input){++Inputs;TestTrue(TEXT("all retained slots remain empty"),P->LinkedTo.IsEmpty());}
 TestEqual(TEXT("all five empty slots survive completion and rollback"),Inputs,5);
 TestEqual(TEXT("logic persistent locator retained"),Logic->NodeGuid,LogicId);
 TestEqual(TEXT("comment persistent locator retained"),Comment->NodeGuid,CommentId);
 TestEqual(TEXT("comment content retained"),Comment->NodeComment,FString(TEXT("V2 retained bound comment")));
 FAssetDocumentRuntimeValueDto Final;
 if(!AssetDocumentAIBehaviorTreeGraphInstallTests::ReadRestoredPublicD2(*this,*S.Tree,Final,true))return false;
 TestEqual(TEXT("terminal V2 full authored value matches original"),ProducerDigestV2(S.Tree->GetPathName(),Final),Original);
 TestFalse(TEXT("terminal readback remains clean"),S.Package->IsDirty());
 return !HasAnyErrors();
}
#endif
