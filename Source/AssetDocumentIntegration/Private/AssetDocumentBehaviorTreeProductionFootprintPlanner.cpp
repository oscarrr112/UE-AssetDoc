// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentBehaviorTreeProductionFootprintPlanner.h"
#include "UObject/SoftObjectPath.h"

namespace BehaviorTreeProductionFootprintPlannerLocal
{
const FAssetDocumentRuntimeValueDto* Field(const FAssetDocumentRuntimeValueDto& Value, const TCHAR* Key)
{
 if (Value.Kind != EAssetDocumentRuntimeValueKind::Object) return nullptr;
 const int32 Index = Value.ObjectKeys.IndexOfByKey(Key);
 return Value.Elements.IsValidIndex(Index) ? &Value.Elements[Index] : nullptr;
}
FAssetDocumentContractValidationResult Invalid()
{
 FAssetDocumentContractValidationResult Result;
 auto& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
 Diagnostic.Path = TEXT("/BehaviorTree");
 Diagnostic.Code = TEXT("GenericRuntime.BehaviorTreeFootprint.InvalidCanonicalPlan");
 Diagnostic.Message = TEXT("The classified BehaviorTree operation cannot declare an exact complete footprint.");
 return Result;
}
}

FAssetDocumentContractValidationResult FAssetDocumentBehaviorTreeProductionFootprintPlanner::Plan(
 const FAssetDocumentRuntimeRequestDto& Request,
 const FAssetDocumentBehaviorTreeOperationClassification& ClassifiedOperation,
 TConstArrayView<FAssetDocumentMutationFootprint> EditorFootprint,
 TArray<FAssetDocumentMutationFootprint>& OutCompleteFootprint)
{
 using namespace BehaviorTreeProductionFootprintPlannerLocal;
 OutCompleteFootprint.Reset();
 const bool Full = ClassifiedOperation.Shape == EAssetDocumentBehaviorTreeOperationShape::FullTreeLifecycle;
 const bool Overlay = ClassifiedOperation.Shape == EAssetDocumentBehaviorTreeOperationShape::RetainedTreeOverlay;
 const auto* Action = Field(Request.CanonicalDocument, TEXT("Action"));
 const FSoftObjectPath Target(Request.TargetAssetPath);
 if (!Request.RequestId.IsValid() || Request.RequestedAssetClassPath != TEXT("/Script/AIModule.BehaviorTree")
  || Request.Operation != EAssetDocumentRuntimeOperation::Apply || (!Full && !Overlay)
  || !Target.IsValid() || !Target.GetSubPathString().IsEmpty() || Target.ToString() != Request.TargetAssetPath
  || !Action || Action->Kind != EAssetDocumentRuntimeValueKind::String
  || (Action->StringValue != TEXT("Create") && Action->StringValue != TEXT("Update") && Action->StringValue != TEXT("CreateOrUpdate"))
  || (Overlay && Action->StringValue == TEXT("Create"))) return Invalid();

 TArray<FAssetDocumentMutationFootprint> Complete;
 Complete.Append(EditorFootprint);
 auto Add = [&](FString StableId, const TCHAR* Component)
 {
  FAssetDocumentMutationFootprint Row;
  Row.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
  Row.StableId = MoveTemp(StableId); Row.RollbackComponentId = Component;
  Complete.Add(MoveTemp(Row));
 };
 const FString Asset = TEXT("asset:") + Request.TargetAssetPath;
 if (Full)
 {
  Add(TEXT("scratch:ai.behavior_tree:request:") + Request.RequestId.ToString(EGuidFormats::Digits),
   TEXT("assetdocument.ai.behavior_tree.scratch_bundle.v1"));
  Add(Asset + TEXT("/GraphInstall"), TEXT("assetdocument.ai.behavior_tree.graph_install.v1"));
 }
 const auto* Body = Field(Request.CanonicalDocument, TEXT("Body"));
 // Omitted BlackboardAsset is not a write. A validated overlay always carries
 // it; full Tree documents may retain the existing Blackboard assignment.
 if (Body && Field(*Body, TEXT("BlackboardAsset")))
  Add(Asset + TEXT("/BlackboardAsset"), TEXT("assetdocument.ai.behavior_tree.blackboard_asset.v1"));
 Add(Asset + TEXT("/SelectorCache"), TEXT("assetdocument.ai.behavior_tree.selector_cache.v1"));
 Add(Asset + TEXT("/DerivedCompletion"), TEXT("assetdocument.ai.behavior_tree.derived_completion.v1"));
 Complete.Sort([](const auto& A, const auto& B) { return A.StableId < B.StableId; });
 for (int32 Index = 0; Index < Complete.Num(); ++Index)
 {
  const auto& Row = Complete[Index];
  if (static_cast<uint8>(Row.Kind) > static_cast<uint8>(EAssetDocumentMutationFootprintKind::Handle)
   || Row.StableId.IsEmpty() || Row.RollbackComponentId.IsEmpty()
   || (Index && Complete[Index - 1].StableId == Row.StableId)) return Invalid();
 }
 OutCompleteFootprint = MoveTemp(Complete);
 FAssetDocumentContractValidationResult Result; Result.bValid = true; return Result;
}
