// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentAIPrimitiveProvider.h"
#include "Tests/AssetDocumentAIBehaviorTreeProviderTestSupport.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIHeaderReadbackTest,
 "UEAssetDoc.AIProvider.BehaviorTree.HeaderReadback.LiveNullAndBlackboard",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIHeaderReadbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeProviderTestSupport;
 for(bool HasBlackboard:{false,true}) for(auto Phase:{EAssetDocumentRuntimeRequestPhase::Apply,EAssetDocumentRuntimeRequestPhase::Repair})
 {
  TStrongObjectPtr<UPackage> Package(CreatePackage(*(TEXT("/Temp/AssetDocumentAI/Header_")+FGuid::NewGuid().ToString(EGuidFormats::Digits))));
  TStrongObjectPtr<UBehaviorTree> Tree(NewObject<UBehaviorTree>(Package.Get(),TEXT("Tree"),RF_Public|RF_Standalone));
  UBlackboardData* Blackboard=HasBlackboard?NewObject<UBlackboardData>(Package.Get(),TEXT("BB"),RF_Public):nullptr;
  Tree->BlackboardAsset=Blackboard;Package->SetDirtyFlag(false);
  FResolveExistingHandleResolver Resolver;
  FAssetDocumentRollbackActionComposition Composition(Resolver,{},[this](EAssetDocumentRuntimeRequestPhase,const FAssetDocumentRuntimeDiagnostic& D){AddInfo(D.Code+TEXT(": ")+D.Message);});
  FAssetDocumentPreflightReceipt Receipt;Receipt.RequestId=FGuid::NewGuid();Receipt.RuntimeRevision=1;
  Receipt.ReceiptHash=TEXT("sha256:header-before-authored-mutation");Receipt.bSucceeded=true;
  // Observe the header before a planned authored mutation. The read itself
  // must not consume that mutation's receipt row or register rollback work.
  Receipt.CompleteFootprint.Add({EAssetDocumentMutationFootprintKind::UObjectState,
   Tree->GetPathName(),TEXT("Test.HeaderReadback.PendingAuthoredMutation")});
  if(!TestTrue(TEXT("nonempty planned receipt is admissible"),ValidateAssetDocumentPreflightReceipt(Receipt).bValid))return false;
  FNoCancellation Cancellation;auto Atomic=Composition.BeginLiveAfterPreflight(Receipt,Cancellation);
  if(!TestNotNull(TEXT("real Atomic for mutation-phase readback"),Atomic.Get()))return false;
  Resolver.SetMutationToken(Atomic->GetMutationToken());
  auto Lease=ResolveExistingMutableAssetLease(*this,*Tree,*Atomic,Resolver);if(!Lease)return false;
  auto Alias=Resolver.RegisterProviderScopedAlias(Lease->GetHandle(),TEXTVIEW("ai"),EAssetDocumentHandleAccess::ReadOnly);
  if(!TestTrue(TEXT("exact read-only alias"),Alias.IsSet()))return false;
  const auto& Token=Atomic->GetMutationToken();FAssetDocumentPrimitiveInvocation I;
  I.PrimitiveId=TEXT("ai.behavior_tree.blackboard.readback.v1");I.Version=1;
  I.RequestId=Token.RequestId;I.TransactionId=Token.TransactionId;I.RuntimeRevision=Token.RuntimeRevision;I.Phase=Phase;
  I.HandleArguments={{TEXT("asset"),Alias.GetValue()}};
  I.ValueArguments.Kind=EAssetDocumentRuntimeValueKind::Object;I.ValueArguments.ObjectKeys={TEXT("behavior_tree_path")};
  FAssetDocumentRuntimeValueDto Path;Path.Kind=EAssetDocumentRuntimeValueKind::String;Path.StringValue=Tree->GetPathName();I.ValueArguments.Elements={Path};
  FInstrumentedTransaction Observed(*Atomic);
  const auto R=CreateAssetDocumentAIPrimitiveProvider()->Invoke(I,Resolver,&Observed);
  for(const auto& D:R.Diagnostics)AddInfo(D.Code+TEXT(": ")+D.Message);
  if(!TestEqual(TEXT("header reads absent graph"),R.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded))return false;
  TestEqual(TEXT("exact header output"),R.Value.ObjectKeys,TArray<FString>{TEXT("blackboard_path_or_null")});
  if(!TestEqual(TEXT("one header field"),R.Value.Elements.Num(),1))return false;
  TestEqual(TEXT("actual nullable header kind"),R.Value.Elements[0].Kind,HasBlackboard?EAssetDocumentRuntimeValueKind::String:EAssetDocumentRuntimeValueKind::Null);
  if(HasBlackboard)TestEqual(TEXT("actual current Blackboard path"),R.Value.Elements[0].StringValue,Blackboard->GetPathName());
  TestTrue(TEXT("no handles or changes"),R.HandleResults.IsEmpty()&&R.ChangedSet.IsEmpty());
  TestTrue(TEXT("no atomic registration"),Observed.Calls.IsEmpty());TestEqual(TEXT("no actions"),Observed.ActionCalls,0);
  TestNull(TEXT("no graph materialized"),Tree->BTGraph);TestFalse(TEXT("readback remains clean"),Package->IsDirty());
  auto Wrong=I;Wrong.ValueArguments.Elements[0].StringValue+=TEXT("_Other");
  TestNotEqual(TEXT("wrong target rejected"),CreateAssetDocumentAIPrimitiveProvider()->Invoke(Wrong,Resolver,&Observed).Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  TestNotEqual(TEXT("mutation-phase read requires actual transaction"),CreateAssetDocumentAIPrimitiveProvider()->Invoke(I,Resolver,nullptr).Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  TestFalse(TEXT("header read cannot complete the unperformed authored mutation"),Atomic->MarkReadyToCommit().bValid);
  TestTrue(TEXT("real Atomic rollback remains empty"),Atomic->Rollback(TEXTVIEW("Test.HeaderReadback")).bValid);
  Tree->ClearFlags(RF_Standalone);Package->SetDirtyFlag(false);
 }
 return !HasAnyErrors();
}
#endif
