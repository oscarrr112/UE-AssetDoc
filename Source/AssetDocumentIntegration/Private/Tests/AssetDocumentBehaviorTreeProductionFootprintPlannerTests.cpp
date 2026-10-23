// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentBehaviorTreeProductionFootprintPlanner.h"
#include "AssetDocumentGenericMutationPreparation.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBehaviorTreeTargetPreparationTests
{
FAssetDocumentRuntimeValueDto MakeDocument(FString Action, FAssetDocumentRuntimeValueDto Body, TOptional<FAssetDocumentRuntimeValueDto> Properties);
FAssetDocumentRuntimeValueDto MakeFullBody();
FAssetDocumentRuntimeValueDto MakeSparseBody();
TOptional<FAssetDocumentRuntimeValueDto> MakeProperties(int32 Mode);
FAssetDocumentRuntimeRequestDto MakeRequest(const FString& Target, FAssetDocumentRuntimeValueDto Document);
FAssetDocumentGenericMutationPreparationResult Prepare(const FAssetDocumentRuntimeRequestDto& Request, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics);
UObject* CreateExistingBehaviorTreeTarget(const FString& Target);
}
namespace BehaviorTreeProductionFootprintPlannerTestsLocal
{
bool SameRows(FAutomationTestBase& Test, TConstArrayView<FAssetDocumentMutationFootprint> A,
 TConstArrayView<FAssetDocumentMutationFootprint> B)
{
 if (!Test.TestEqual(TEXT("receipt and persistence have identical row counts"), A.Num(), B.Num())) return false;
 bool Equal = true;
 for (int32 Index = 0; Index < A.Num(); ++Index)
 {
  Equal &= Test.TestEqual(TEXT("identical row kind"), A[Index].Kind, B[Index].Kind);
  Equal &= Test.TestEqual(TEXT("identical row identity and order"), A[Index].StableId, B[Index].StableId);
  Equal &= Test.TestEqual(TEXT("identical row rollback owner"), A[Index].RollbackComponentId, B[Index].RollbackComponentId);
 }
 return Equal;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBTProductionFootprintMatrix,
 "UEAssetDoc.Integration.BehaviorTree.ProductionFootprint.ReceiptPersistenceMatrix",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBTProductionFootprintMatrix::RunTest(const FString&)
{
 using namespace BehaviorTreeProductionFootprintPlannerTestsLocal;
 using namespace AssetDocumentBehaviorTreeTargetPreparationTests;
 const FString Root = TEXT("/Game/AssetDocumentTests/D5_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 const FString Existing = Root + TEXT("/Existing.Existing"), Missing = Root + TEXT("/Missing.Missing");
 if (!TestNotNull(TEXT("real existing target"), CreateExistingBehaviorTreeTarget(Existing))) return false;
 for (bool Full : {false, true}) for (bool Factory : {false, true}) for (int32 PropertyMode : {0, 1, 2})
 {
  if (!Full && Factory) continue;
  auto Request = MakeRequest(Factory ? Missing : Existing, MakeDocument(Factory ? TEXT("Create") : TEXT("Update"),
   Full ? MakeFullBody() : MakeSparseBody(), MakeProperties(PropertyMode)));
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  auto Prepared = Prepare(Request, Diagnostics);
  for (const auto& D : Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  if (!TestTrue(TEXT("production preparation succeeds"), Prepared.Mutation.IsValid())) return false;
  const auto& Rows = Prepared.Mutation->PreflightReceipt.CompleteFootprint;
  SameRows(*this, Rows, Prepared.Mutation->PersistencePlanTemplate.Footprint);
  TMap<FString, FString> BT;
  for (int32 Index = 0; Index < Rows.Num(); ++Index)
  {
   if (Index) TestTrue(TEXT("complete row identities strictly sorted and unique"), Rows[Index - 1].StableId < Rows[Index].StableId);
   if (Rows[Index].RollbackComponentId.StartsWith(TEXT("assetdocument.ai.behavior_tree.")))
   {
    TestEqual(TEXT("BT rows are UObject state"), Rows[Index].Kind, EAssetDocumentMutationFootprintKind::UObjectState);
    BT.Add(Rows[Index].StableId, Rows[Index].RollbackComponentId);
   }
  }
  const FString Asset = TEXT("asset:") + Request.TargetAssetPath;
  TMap<FString,FString> Expected{{Asset + TEXT("/SelectorCache"), TEXT("assetdocument.ai.behavior_tree.selector_cache.v1")},
   {Asset + TEXT("/DerivedCompletion"), TEXT("assetdocument.ai.behavior_tree.derived_completion.v1")}};
  if (Full)
  {
   Expected.Add(Asset + TEXT("/GraphInstall"), TEXT("assetdocument.ai.behavior_tree.graph_install.v1"));
   Expected.Add(TEXT("scratch:ai.behavior_tree:request:") + Request.RequestId.ToString(EGuidFormats::Digits), TEXT("assetdocument.ai.behavior_tree.scratch_bundle.v1"));
  }
  else Expected.Add(Asset + TEXT("/BlackboardAsset"), TEXT("assetdocument.ai.behavior_tree.blackboard_asset.v1"));
  TestEqual(TEXT("exact BT subset without leaf/cardinality or speculative rows"), BT.Num(), Expected.Num());
  for (const auto& Row : Expected) TestEqual(TEXT("actual released row identity/component"), BT.FindRef(Row.Key), Row.Value);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBTProductionFootprintRejects,
 "UEAssetDoc.Integration.BehaviorTree.ProductionFootprint.ExactRowsAndRejections",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBTProductionFootprintRejects::RunTest(const FString&)
{
 using namespace AssetDocumentBehaviorTreeTargetPreparationTests;
 auto Body = MakeFullBody();
 auto BB = MakeSparseBody(); Body.ObjectKeys.Insert(BB.ObjectKeys[0], 0); Body.Elements.Insert(BB.Elements[0], 0);
 auto Request = MakeRequest(TEXT("/Game/AI/BT_Test.BT_Test"), MakeDocument(TEXT("CreateOrUpdate"), Body, MakeProperties(0)));
 auto Classification = ClassifyAssetDocumentBehaviorTreeOperation(Request.RequestedAssetClassPath, Request.CanonicalDocument);
 TArray<FAssetDocumentMutationFootprint> Editor{{EAssetDocumentMutationFootprintKind::File, TEXT("z-editor-row"), TEXT("editor.test")},
  {EAssetDocumentMutationFootprintKind::Package, TEXT("a-editor-row"), TEXT("editor.test")}};
 TArray<FAssetDocumentMutationFootprint> Rows;
 if (!TestTrue(TEXT("full Tree with explicit Blackboard has five BT rows"), FAssetDocumentBehaviorTreeProductionFootprintPlanner::Plan(Request, Classification, Editor, Rows).bValid)) return false;
 TestEqual(TEXT("Editor values plus exactly five BT rows"), Rows.Num(), 7);
 for (const auto& Expected : Editor)
  TestTrue(TEXT("Editor input retained verbatim"), Rows.ContainsByPredicate([&](const auto& Actual)
   { return Actual.Kind == Expected.Kind && Actual.StableId == Expected.StableId && Actual.RollbackComponentId == Expected.RollbackComponentId; }));
 Editor.Add(Rows[1]);
 TestFalse(TEXT("duplicate declared identity rejected before receipt"), FAssetDocumentBehaviorTreeProductionFootprintPlanner::Plan(Request, Classification, Editor, Rows).bValid);
 TestTrue(TEXT("no partial footprint on failure"), Rows.IsEmpty());
 Request.RequestId.Invalidate();
 TestFalse(TEXT("missing request identity cannot guess a transaction"), FAssetDocumentBehaviorTreeProductionFootprintPlanner::Plan(Request, Classification, {}, Rows).bValid);
 TestTrue(TEXT("rejected request leaves no rows"), Rows.IsEmpty());
 return !HasAnyErrors();
}
#endif
