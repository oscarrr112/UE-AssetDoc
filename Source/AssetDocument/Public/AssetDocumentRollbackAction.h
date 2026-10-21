// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

class ASSETDOCUMENT_API IAssetDocumentRollbackAction
{
public:
	virtual ~IAssetDocumentRollbackAction() = default;
	virtual FStringView GetRollbackComponentId() const = 0;
	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) = 0;
};

class ASSETDOCUMENT_API IAssetDocumentPostRollbackFinalizer
{
public:
	virtual ~IAssetDocumentPostRollbackFinalizer() = default;
	virtual FStringView GetRollbackComponentId() const = 0;
	virtual FAssetDocumentContractValidationResult FinalizePostRollback(
		FStringView StableFailureCode) = 0;
};

class ASSETDOCUMENT_API FAssetDocumentRollbackActionComposition final
	: public IAssetDocumentAtomicTransactionFactory
{
public:
	FAssetDocumentRollbackActionComposition(
		IAssetDocumentHandleResolver& InHandleResolver,
		TArray<TSharedRef<IAssetDocumentRollbackAction>> InActions,
		TFunction<void(
			EAssetDocumentRuntimeRequestPhase,
			const FAssetDocumentRuntimeDiagnostic&)> InDiagnosticSink);
	~FAssetDocumentRollbackActionComposition();

	FAssetDocumentRollbackActionComposition(
		FAssetDocumentRollbackActionComposition&& Other);
	FAssetDocumentRollbackActionComposition& operator=(
		FAssetDocumentRollbackActionComposition&& Other);

	FAssetDocumentRollbackActionComposition(
		const FAssetDocumentRollbackActionComposition&) = delete;
	FAssetDocumentRollbackActionComposition& operator=(
		const FAssetDocumentRollbackActionComposition&) = delete;

	bool IsValid() const;
	const FAssetDocumentContractValidationResult& GetValidation() const;
	virtual TUniquePtr<IAssetDocumentAtomicTransaction>
	BeginLiveAfterPreflight(
		const FAssetDocumentPreflightReceipt& Receipt,
		IAssetDocumentCancellationView& Cancellation) override;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
