// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentBlackboardFootprintProjection.h"

class FAssetDocumentBlackboardAtomicTransactionAdapter final : public IAssetDocumentAtomicTransaction
{
public:
	FAssetDocumentBlackboardAtomicTransactionAdapter(
		IAssetDocumentAtomicTransaction& InUnderlying,
		FAssetDocumentBlackboardFootprintProjection InProjection,
		TArray<FAssetDocumentMutationFootprint> InOptionalCreatePrefix);

	virtual FGuid GetTransactionId() const override;
	virtual EAssetDocumentAtomicState GetState() const override;
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override;
	virtual FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override;
	FAssetDocumentContractValidationResult ValidateCompleteEnlistment() const;
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override;
	virtual FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt& Receipt) override;
	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) override;
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction> Action) override;
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer) override;

private:
	FAssetDocumentContractValidationResult Reject(const TCHAR* Code, const TCHAR* Message);
	const FAssetDocumentBlackboardFootprintProjectionStep* GetNextStep() const;
	const FAssetDocumentMutationFootprint* FindQualifiedComponent(FStringView RawComponentId) const;
	bool IsExactPrefix(TConstArrayView<FAssetDocumentMutationFootprint> Footprint) const;

	IAssetDocumentAtomicTransaction* Underlying = nullptr;
	FAssetDocumentBlackboardFootprintProjection Projection;
	TArray<FAssetDocumentMutationFootprint> OptionalCreatePrefix;
	int32 NextStepIndex = 0;
	bool bPrefixConsumed = false;
	bool bRejected = false;
	TSet<FString> WrappedQualifiedComponents;
};
