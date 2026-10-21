// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentMutationPreparation.h"

ASSETDOCUMENTANIMATION_API FAssetDocumentContractValidationResult
PrepareAssetDocumentAnimationMutationBatch(
 const FAssetDocumentRuntimeRequestDto& Request,
 TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch);

#include "Runtime/AssetDocumentPrimitiveProvider.h"

class UAnimMontage;
class ASSETDOCUMENTANIMATION_API FAssetDocumentAnimationStagedReadLease final
{
public:
 explicit FAssetDocumentAnimationStagedReadLease(TUniqueFunction<void()>&& InClose) : OnClose(MoveTemp(InClose)) {}
 ~FAssetDocumentAnimationStagedReadLease() { Close(); }
 void Close() { if (OnClose) { auto Action = MoveTemp(OnClose); Action(); } }
private:
 TUniqueFunction<void()> OnClose;
};

// One request-local instance is used both as the provider and by the preparation binding.
class ASSETDOCUMENTANIMATION_API IAssetDocumentAnimationRequestProvider : public IAssetDocumentPrimitiveProvider
{
public:
 virtual FAssetDocumentContractValidationResult BindRequest(const FAssetDocumentRuntimeRequestDto& Request) = 0;
 virtual void CloseRequest() = 0;
 // RuntimeRevision pins the fresh verification handle revision; StagedPackagePath is the actual loaded file path.
 // Verify supplies a readonly handle to that staged object. Mutation remains bound to the original request.
 virtual TUniquePtr<FAssetDocumentAnimationStagedReadLease> BindStagedRead(UAnimMontage& Target,
  const FString& StagedPackagePath, uint64 RuntimeRevision, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics) = 0;
 virtual FAssetDocumentContractValidationResult Prepare(const FAssetDocumentRuntimeRequestDto& Request,
  TConstArrayView<FAssetDocumentMutationIntent> Intents, FAssetDocumentPreparedMutationBatch& OutBatch) = 0;
};
ASSETDOCUMENTANIMATION_API TSharedRef<IAssetDocumentAnimationRequestProvider>
CreateAssetDocumentAnimationRequestProvider();

// Native preparation only: consumes the frozen handle-free Compression inputs
// from the private candidate, resolves them before one already-bound owner
// Prepare, and returns those actual Intents with that original prepared batch.
// Existing Prepare/virtual ABI and public document ingress are unchanged.
ASSETDOCUMENTANIMATION_API FAssetDocumentContractValidationResult
PrepareAssetDocumentAnimationSequenceCompressionDocumentBatch(
 const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentAnimationRequestProvider& Owner,
 const FAssetDocumentRuntimeValueDto& DocumentInputs, int32 StepIndex, EAssetDocumentRuntimeRequestPhase Phase,
 TConstArrayView<FAssetDocumentMutationIntent> OtherIntents,
 TArray<FAssetDocumentMutationIntent>& OutActualIntents, FAssetDocumentPreparedMutationBatch& OutBatch);

class IAssetDocumentNativeRequestContext;
// Private References composition uses the original Preflight context for the
// existing readonly resolvers, then prepares once through the bound owner.
// No new virtual method, primitive, document route or final Intent schema.
ASSETDOCUMENTANIMATION_API FAssetDocumentContractValidationResult
PrepareAssetDocumentAnimationSequenceReferencesDocumentBatch(
 const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentAnimationRequestProvider& Owner,
 IAssetDocumentNativeRequestContext& ReadonlyContext, const FGuid& ReadonlyTransaction,
 const FAssetDocumentRuntimeValueDto& DocumentInputs, int32 NameStepIndex, int32 AssetStepIndex,
 EAssetDocumentRuntimeRequestPhase Phase, TConstArrayView<FAssetDocumentMutationIntent> OtherIntents,
 TArray<FAssetDocumentMutationIntent>& OutActualIntents, FAssetDocumentPreparedMutationBatch& OutBatch);

#if WITH_DEV_AUTOMATION_TESTS
// Test-only failure after an actual exact float-curve patch on the named target.
// An occupied hook slot leaves this scope unarmed; the first matching patch consumes it.
class ASSETDOCUMENTANIMATION_API FAssetDocumentAnimationFloatCurveFailureScope final
{
public:
 explicit FAssetDocumentAnimationFloatCurveFailureScope(const FString& ExactTargetPath);
 ~FAssetDocumentAnimationFloatCurveFailureScope();
 bool IsArmed() const;
 bool WasTriggered() const;
 FAssetDocumentAnimationFloatCurveFailureScope(const FAssetDocumentAnimationFloatCurveFailureScope&) = delete;
 FAssetDocumentAnimationFloatCurveFailureScope& operator=(const FAssetDocumentAnimationFloatCurveFailureScope&) = delete;
private:
 struct FState;
 TUniquePtr<FState> State;
};
#endif
