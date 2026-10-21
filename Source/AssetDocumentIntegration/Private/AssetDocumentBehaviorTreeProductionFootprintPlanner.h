// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "AssetDocumentBehaviorTreeOperationClassifier.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

// Value-only declaration before Atomic creates a TransactionId. Editor rows
// are successful preparation output and retain their exact values.
class FAssetDocumentBehaviorTreeProductionFootprintPlanner
{
public:
 static FAssetDocumentContractValidationResult Plan(
  const FAssetDocumentRuntimeRequestDto& Request,
  const FAssetDocumentBehaviorTreeOperationClassification& ClassifiedOperation,
  TConstArrayView<FAssetDocumentMutationFootprint> EditorFootprint,
  TArray<FAssetDocumentMutationFootprint>& OutCompleteFootprint);
};
