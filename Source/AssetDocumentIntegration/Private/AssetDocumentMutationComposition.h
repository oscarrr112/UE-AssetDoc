// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once

#include "Runtime/AssetDocumentMutationPreparation.h"

// The sole request-local consumer of provider-owned frozen contributions.
class FAssetDocumentMutationComposition final : public TSharedFromThis<FAssetDocumentMutationComposition>
{
public:
 struct FProviderBatch
 {
  FString ProviderId;
  FAssetDocumentPreparedMutationBatch Batch;
  TSharedPtr<IAssetDocumentPrimitiveProvider> ActualProvider;
 };
 static TSharedPtr<FAssetDocumentMutationComposition> Freeze(
  const FAssetDocumentRuntimeRequestDto& Request,
  TArray<FAssetDocumentMutationIntent> Intents,
  TArray<FProviderBatch> Batches,
  TArray<FAssetDocumentMutationFootprint> FactoryFootprint,
  TArray<TSharedRef<IAssetDocumentRollbackAction>> FactoryActions,
  FAssetDocumentCoreMutationCaptureOpener FactoryOpener,
  TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics,
  bool bPackageSnapshot = false);
 // No declared steps: every mutating invocation of the request is forwarded and
 // its own enlistments are absorbed, because an existing target is covered by
 // Core's package snapshot (bPackageSnapshot) and a created target by the
 // factory rollback (FactoryFootprint/Actions/Opener). Only resources of the
 // target package are accepted.
 static TSharedPtr<FAssetDocumentMutationComposition> FreezeSnapshotCovered(
  const FAssetDocumentRuntimeRequestDto& Request,
  TArray<FAssetDocumentMutationFootprint> FactoryFootprint,
  TArray<TSharedRef<IAssetDocumentRollbackAction>> FactoryActions,
  FAssetDocumentCoreMutationCaptureOpener FactoryOpener,
  bool bPackageSnapshot,
  TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics);
 ~FAssetDocumentMutationComposition();
 TConstArrayView<FAssetDocumentMutationFootprint> GetFootprint() const;
 TArray<TSharedRef<IAssetDocumentRollbackAction>> MakeInitialActions();
 FAssetDocumentCoreMutationCaptureOpener MakeCaptureOpener();
 IAssetDocumentAtomicTransaction* BindTransaction(IAssetDocumentAtomicTransaction& Transaction,
  const FAssetDocumentRequestContextMetadata& Metadata, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics);
 FAssetDocumentPrimitiveOutcome Invoke(const FString& ProviderId,
  const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
  IAssetDocumentAtomicTransaction* Transaction, TFunctionRef<FAssetDocumentPrimitiveOutcome()> Forward);
 FAssetDocumentContractValidationResult ValidateComplete() const;
private:
 struct FState;
 explicit FAssetDocumentMutationComposition(TSharedRef<FState> InState);
 TSharedRef<FState> State;
 TUniquePtr<IAssetDocumentAtomicTransaction> Adapter;
};
