// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentCoreRuntimeComposition.h"

using FAssetDocumentRequestPersistenceTargetBindingCallback =
	TUniqueFunction<bool(
		const FAssetDocumentRuntimeRequestDto& Request,
		const FGuid& TransactionId,
		TConstArrayView<FAssetDocumentChangedRecord> ChangedSet,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)>;

struct FAssetDocumentRequestCoordinatorRuntimeBindings
{
	FAssetDocumentCorePhaseContextFactory CreatePhaseContext;
	FAssetDocumentCoreMutationCaptureOpener OpenMutationCapture;
	FAssetDocumentRequestPersistenceTargetBindingCallback BindPersistenceTarget;
	const FAssetDocumentPreflightReceipt* PreflightReceipt = nullptr;
	const FAssetDocumentPersistencePlan* PersistencePlanTemplate = nullptr;
	IAssetDocumentStagedArtifactVerifier* Verifier = nullptr;
	TFunction<void(const FAssetDocumentRuntimeDiagnostic&)> DiagnosticSink;
};

struct FAssetDocumentRequestCoordinatorReadOnlyDependencies
{
	IAssetDocumentPythonRuntimeHost& Host;
	IAssetDocumentCancellationView& Cancellation;
	IAssetDocumentRequestObserver& Observer;
};

// Private no-write execution input. It carries no mutation receipt, footprint,
// transaction or durable-completion receipt. Semantic decisions remain in Python;
// the callbacks prove the exact native owner and independent installed artifacts.
struct FAssetDocumentCoreEqualDeltaInputs
{
	FGuid RequestId;
	uint64 RuntimeRevision = 0;
	FString TargetAssetPath;
	FString ExactAssetClassPath;
	FString SemanticHash;
	uint32 CodecVersion = 0;
	TUniqueFunction<FAssetDocumentContractValidationResult()> ValidateLive;
	TUniqueFunction<FAssetDocumentFreshVerificationResult()> VerifyDurable;
};

ASSETDOCUMENT_API FAssetDocumentRuntimeResultDto ExecuteAssetDocumentCoreEqualDelta(
	FAssetDocumentCoreRuntimeDependencies& Dependencies,
	const FAssetDocumentRuntimeRequestDto& Request,
	FAssetDocumentCoreEqualDeltaInputs&& Inputs);

class FAssetDocumentRequestCoordinatorRuntime final
	: public IAssetDocumentRequestCoordinator
{
public:
	explicit FAssetDocumentRequestCoordinatorRuntime(
		FAssetDocumentRequestCoordinatorRuntimeBindings InBindings);

	FAssetDocumentRuntimeResultDto ExecuteReadOnly(
		const FAssetDocumentRuntimeRequestDto& Request,
		FAssetDocumentRequestCoordinatorReadOnlyDependencies& Dependencies);

	FAssetDocumentRuntimeResultDto ExecuteEqualDelta(
		const FAssetDocumentRuntimeRequestDto& Request,
		FAssetDocumentRequestCoordinatorReadOnlyDependencies& Dependencies,
		FAssetDocumentCoreEqualDeltaInputs&& Inputs);

	virtual FAssetDocumentRuntimeResultDto Execute(
		const FAssetDocumentRuntimeRequestDto& Request,
		FAssetDocumentRequestCoordinatorDependencies& Dependencies) override;

private:
	FAssetDocumentRequestCoordinatorRuntimeBindings Bindings;
};
