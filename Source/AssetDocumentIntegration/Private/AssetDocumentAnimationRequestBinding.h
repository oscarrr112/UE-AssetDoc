// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AssetDocumentAnimationMutationPreparation.h"

// The registry freezes this catalog facade. Its mutable implementation exists
// only for one Service dispatch, shared by preparation and primitive execution.
class FAssetDocumentAnimationRequestBinding final : public IAssetDocumentPrimitiveProvider
{
public:
	using FFactory = TFunction<TSharedRef<IAssetDocumentAnimationRequestProvider>()>;
	explicit FAssetDocumentAnimationRequestBinding(FFactory InFactory = CreateAssetDocumentAnimationRequestProvider);
	FString GetProviderId() const override;
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override;
	FAssetDocumentProviderAvailability InspectAvailability() const override;
	FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction) override;
	FAssetDocumentContractValidationResult BindRequest(const FAssetDocumentRuntimeRequestDto& Request);
	void CloseRequest();
	FAssetDocumentContractValidationResult Prepare(const FAssetDocumentRuntimeRequestDto& Request,
		TConstArrayView<FAssetDocumentMutationIntent> Intents, FAssetDocumentPreparedMutationBatch& Batch);
	FAssetDocumentContractValidationResult PrepareSequenceCompressionDocument(const FAssetDocumentRuntimeRequestDto& Request,
		const FAssetDocumentRuntimeValueDto& DocumentInputs, int32 StepIndex, EAssetDocumentRuntimeRequestPhase Phase,
		TConstArrayView<FAssetDocumentMutationIntent> OtherIntents,
		TArray<FAssetDocumentMutationIntent>& ActualIntents, FAssetDocumentPreparedMutationBatch& Batch);
	FAssetDocumentContractValidationResult PrepareSequenceReferencesDocument(const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& ReadonlyContext, const FGuid& ReadonlyTransaction,
		const FAssetDocumentRuntimeValueDto& DocumentInputs, int32 NameStepIndex, int32 AssetStepIndex,
		EAssetDocumentRuntimeRequestPhase Phase, TConstArrayView<FAssetDocumentMutationIntent> OtherIntents,
		TArray<FAssetDocumentMutationIntent>& ActualIntents, FAssetDocumentPreparedMutationBatch& Batch);
	TUniquePtr<FAssetDocumentAnimationStagedReadLease> BindStagedRead(UAnimMontage& Target,
		const FString& Path, uint64 FreshRevision, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics);
private:
	FFactory Factory;
	FString ProviderId;
	TArray<FAssetDocumentPrimitiveDescriptor> Descriptors;
	FAssetDocumentProviderAvailability Availability;
	TSharedPtr<IAssetDocumentAnimationRequestProvider> Active;
	FGuid RequestId;
	uint64 Revision = 0;
	uint64 StagedRevision = 0;
	FString BoundTargetPath;
	FString BoundAssetClassPath;
};
