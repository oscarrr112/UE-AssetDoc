// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once

#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "AssetDocumentAnimationMutationPreparation.h"

namespace AssetDocumentMontage
{
struct FPreparedNotifyStaging;
struct FRequestReadBinding;

// Request-local provider and preparation must be retained as the same instance.
// Exact private descriptors are projected with the corresponding formal authority records.
FAssetDocumentPrimitiveDescriptor MakeMontageNotifyReplacementDescriptor(bool bStates);

class ASSETDOCUMENTANIMATION_API FMontageNotifyRequestOwner final : public IAssetDocumentAnimationRequestProvider
{
public:
 FMontageNotifyRequestOwner();
 FAssetDocumentContractValidationResult BindRequest(const FAssetDocumentRuntimeRequestDto& Request) override;
 void CloseRequest() override;
 TUniquePtr<FAssetDocumentAnimationStagedReadLease> BindStagedRead(UAnimMontage& Target,
  const FString& StagedPackagePath, uint64 RuntimeRevision, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics) override;
 FString GetProviderId() const override;
 TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override;
 FAssetDocumentProviderAvailability InspectAvailability() const override;
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& Invocation,
  IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction) override;
 FAssetDocumentContractValidationResult Prepare(const FAssetDocumentRuntimeRequestDto& Request,
  TConstArrayView<FAssetDocumentMutationIntent> Intents, FAssetDocumentPreparedMutationBatch& OutBatch) override;

private:
 const TSharedRef<IAssetDocumentPrimitiveProvider> Inner;
 TSharedRef<FRequestReadBinding> ReadBinding;
 TArray<FAssetDocumentPrimitiveDescriptor> Descriptors;
 TSharedPtr<FPreparedNotifyStaging> Prepared;
 FGuid RequestId;
 uint64 Revision = 0;
 FString TargetPath;
 FString Primitive;
 EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 bool bPreparationAttempted = false;
 bool bInvocationAttempted = false;
 // Create: the Montage does not exist while the request is prepared. Each
 // family is then prepared just in time on the created target inside its own
 // invocation; the created-asset rollback covers every step.
 bool bDeferredCreate = false;
 TArray<FAssetDocumentMutationIntent> DeferredIntents;
 int32 DeferredNext = 0;
 FAssetDocumentContractValidationResult PrepareDeferredCreate(TConstArrayView<FAssetDocumentMutationIntent> Intents,
  FAssetDocumentPreparedMutationBatch& OutBatch);
 FAssetDocumentPrimitiveOutcome InvokeDeferred(const FAssetDocumentPrimitiveInvocation& Invocation,
  IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction);
};

// Bind {Owner, [Owner](Request, Intents, Batch) { return Owner->Prepare(...); }}.
// Integration pairs the original provider instance by address before wrapping it.
ASSETDOCUMENTANIMATION_API TSharedRef<FMontageNotifyRequestOwner> CreateMontageNotifyRequestOwner();
}
