// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentRollbackAction.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"

class UObject;
class UPackage;

using FAssetDocumentCorePhaseContextFactory =
	TFunction<TUniquePtr<IAssetDocumentNativeRequestContext>(
		const FAssetDocumentRequestContextMetadata& Metadata,
		const FGuid& TransactionId,
		IAssetDocumentAtomicTransaction* ActiveTransaction,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)>;

using FAssetDocumentCoreHandleInvalidator =
	TFunction<FAssetDocumentHandleInvalidationResult(
		const FAssetDocumentHandleInvalidation&)>;

class ASSETDOCUMENT_API FAssetDocumentCoreMutationCaptureLease final
{
public:
	explicit FAssetDocumentCoreMutationCaptureLease(
		TUniqueFunction<void()>&& InClose);
	~FAssetDocumentCoreMutationCaptureLease();

	FAssetDocumentCoreMutationCaptureLease(
		FAssetDocumentCoreMutationCaptureLease&& Other);
	FAssetDocumentCoreMutationCaptureLease& operator=(
		FAssetDocumentCoreMutationCaptureLease&& Other);

	FAssetDocumentCoreMutationCaptureLease(
		const FAssetDocumentCoreMutationCaptureLease&) = delete;
	FAssetDocumentCoreMutationCaptureLease& operator=(
		const FAssetDocumentCoreMutationCaptureLease&) = delete;

	bool IsValid() const;
	void Close();

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};

using FAssetDocumentCoreMutationCaptureOpener =
	TUniqueFunction<TUniquePtr<FAssetDocumentCoreMutationCaptureLease>(
		const FGuid& TransactionId,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)>;

struct ASSETDOCUMENT_API FAssetDocumentCorePersistenceTargetRequest
{
	FGuid RequestId;
	FGuid TransactionId;
	uint64 RuntimeRevision = 0;
	FStringView CanonicalObjectPath;
	FStringView ExactAssetClassPath;
	FStringView TargetPackagePath;
	TConstArrayView<FAssetDocumentChangedRecord> ChangedSet;
};

struct ASSETDOCUMENT_API FAssetDocumentCorePersistenceTargetBinding
{
	FGuid RequestId;
	FGuid TransactionId;
	uint64 RuntimeRevision = 0;
	UObject* Asset = nullptr;
	UPackage* Package = nullptr;
};

using FAssetDocumentCorePersistenceTargetBinder =
	TUniqueFunction<TOptional<FAssetDocumentCorePersistenceTargetBinding>(
		const FAssetDocumentCorePersistenceTargetRequest& Request,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)>;

struct ASSETDOCUMENT_API FAssetDocumentCoreRuntimeDependencies
{
	IAssetDocumentPythonRuntimeHost& Host;
	IAssetDocumentPrimitiveRegistry& PrimitiveRegistry;
	IAssetDocumentCancellationView& Cancellation;
	IAssetDocumentRequestObserver& Observer;
	FAssetDocumentCorePhaseContextFactory CreatePhaseContext;
	FAssetDocumentCoreHandleInvalidator InvalidateHandles;
};

struct ASSETDOCUMENT_API FAssetDocumentCoreMutationInputs
{
	FAssetDocumentPreflightReceipt PreflightReceipt;
	FAssetDocumentPersistencePlan PersistencePlanTemplate;
	IAssetDocumentStagedArtifactVerifier& Verifier;
	FAssetDocumentCorePersistenceTargetBinder BindPersistenceTarget;
	FAssetDocumentSyncEvidencePair InstalledEvidence;
	FString JournalRoot;
	FString JournalBackupRoot;
	FString SyncStateRoot;
	FString SyncRegionId;
	uint64 FreshHandleRevision = 0;
	TArray<TSharedRef<IAssetDocumentRollbackAction>> RollbackActions;
	FAssetDocumentCoreMutationCaptureOpener OpenMutationCapture;
	// Loaded packages captured as whole-package snapshots at BeginLive and
	// restored by Core (core.package_snapshot.v1) after every other rollback
	// target. Empty keeps the previous behavior.
	TArray<FString> SnapshotPackageNames;
};

class ASSETDOCUMENT_API FAssetDocumentCoreRuntimeComposition final
{
public:
	explicit FAssetDocumentCoreRuntimeComposition(
		FAssetDocumentCoreRuntimeDependencies InDependencies);
	~FAssetDocumentCoreRuntimeComposition();

	FAssetDocumentCoreRuntimeComposition(
		FAssetDocumentCoreRuntimeComposition&& Other);
	FAssetDocumentCoreRuntimeComposition& operator=(
		FAssetDocumentCoreRuntimeComposition&& Other);

	FAssetDocumentCoreRuntimeComposition(
		const FAssetDocumentCoreRuntimeComposition&) = delete;
	FAssetDocumentCoreRuntimeComposition& operator=(
		const FAssetDocumentCoreRuntimeComposition&) = delete;

	bool IsValid() const;
	const FAssetDocumentContractValidationResult& GetValidation() const;

	FAssetDocumentRuntimeResultDto ExecuteReadOnly(
		const FAssetDocumentRuntimeRequestDto& Request);
	FAssetDocumentRuntimeResultDto ExecuteMutation(
		const FAssetDocumentRuntimeRequestDto& Request,
		FAssetDocumentCoreMutationInputs&& Mutation);

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
