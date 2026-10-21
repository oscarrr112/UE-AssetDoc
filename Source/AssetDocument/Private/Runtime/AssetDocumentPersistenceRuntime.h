// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPackageStaging.h"
#include "Runtime/AssetDocumentPersistenceCoordinator.h"
#include "Runtime/AssetDocumentPersistenceJournal.h"
#include "Runtime/AssetDocumentSidecarByteIo.h"
#include "Runtime/AssetDocumentTypedSyncRuntime.h"

class IAssetDocumentHandleResolver;

using FAssetDocumentPersistenceEventSink = TFunction<void(FStringView)>;
using FAssetDocumentPersistenceDiagnosticSink =
	TFunction<void(EAssetDocumentPersistenceStep, const FAssetDocumentRuntimeDiagnostic&)>;

struct FAssetDocumentPersistenceFailure
{
	EAssetDocumentPersistenceStep Step = EAssetDocumentPersistenceStep::StagePackage;
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	TArray<FAssetDocumentRuntimeDiagnostic> VerifierDiagnostics;
};

struct FAssetDocumentPersistenceWork;

struct FAssetDocumentPersistenceDependencies
{
	FAssetDocumentPackageStager& PackageStager;
	FAssetDocumentSidecarByteIo& SidecarByteIo;
	FAssetDocumentPersistenceJournal& Journal;
	IAssetDocumentTypedSyncStateStore& TypedStore;
	IAssetDocumentSyncCoordinator& Sync;
	IAssetDocumentHandleResolver& Handles;
	FAssetDocumentSyncStateKey SyncKey;
	FAssetDocumentSyncEvidencePair InstalledEvidence;
	FString JournalBackupRoot;
	uint64 FreshHandleRevision = 0;
	FGuid ExpectedRequestId;
	FGuid ExpectedTransactionId;
	FString ExpectedJournalId;
	FString ExpectedStagingRoot;
	FString ExpectedTargetPackagePath;
	FString ExpectedTargetSidecarPath;
	FAssetDocumentPersistenceEventSink Events;
	FAssetDocumentPersistenceDiagnosticSink Diagnostics;
};

FAssetDocumentDurableCompletionReceipt MakeInvalidDurableReceipt();

class FAssetDocumentPersistenceCoordinatorRuntime final : public IAssetDocumentPersistenceCoordinator
{
public:
	explicit FAssetDocumentPersistenceCoordinatorRuntime(FAssetDocumentPersistenceDependencies InDependencies);
	virtual FAssetDocumentDurableCompletionReceipt Persist(
		const FAssetDocumentPersistencePlan& Plan,
		IAssetDocumentStagedArtifactVerifier& Verifier) override;

private:
	bool BuildVerifiedStaging(
		const FAssetDocumentPersistencePlan& Plan,
		IAssetDocumentStagedArtifactVerifier& Verifier,
		FAssetDocumentPersistenceWork& Work,
		FAssetDocumentPersistenceFailure& OutFailure) const;
	bool PrepareRecoveryState(
		const FAssetDocumentPersistencePlan& Plan,
		FAssetDocumentPersistenceWork& Work,
		FAssetDocumentPersistenceFailure& OutFailure) const;
	bool InstallArtifacts(
		FAssetDocumentPersistenceWork& Work,
		FAssetDocumentPersistenceFailure& OutFailure) const;
	bool ValidateAndRecover(
		const FAssetDocumentPersistencePlan& Plan,
		FAssetDocumentPersistenceFailure& OutFailure) const;
	bool StageArtifacts(
		const FAssetDocumentPersistencePlan& Plan,
		FAssetDocumentPersistenceWork& Work,
		FAssetDocumentPersistenceFailure& OutFailure) const;
	bool VerifyStaged(
		const FAssetDocumentPersistencePlan& Plan,
		IAssetDocumentStagedArtifactVerifier& Verifier,
		FAssetDocumentPersistenceWork& Work,
		FAssetDocumentPersistenceFailure& OutFailure) const;
	bool CaptureBaselines(
		FAssetDocumentPersistenceWork& Work,
		FAssetDocumentPersistenceFailure& OutFailure) const;
	bool PrepareJournal(
		const FAssetDocumentPersistencePlan& Plan,
		FAssetDocumentPersistenceWork& Work,
		FAssetDocumentPersistenceFailure& OutFailure) const;
	bool InstallPackage(
		FAssetDocumentPersistenceWork& Work,
		FAssetDocumentPersistenceFailure& OutFailure) const;
	bool InstallPackageArtifact(
		int32 ArtifactIndex,
		FAssetDocumentPersistenceWork& Work,
		FAssetDocumentPersistenceFailure& OutFailure) const;
	bool InstallPackageCompanions(
		int32 HeaderIndex,
		FAssetDocumentPersistenceWork& Work,
		FAssetDocumentPersistenceFailure& OutFailure) const;
	bool InstallSidecar(
		FAssetDocumentPersistenceWork& Work,
		FAssetDocumentPersistenceFailure& OutFailure) const;
	FAssetDocumentDurableCompletionReceipt AdvanceAndCommit(
		const FAssetDocumentPersistencePlan& Plan,
		FAssetDocumentPersistenceWork& Work) const;
	FAssetDocumentDurableCompletionReceipt Fail(const FAssetDocumentPersistenceFailure& Failure) const;
	void RecordEvent(FStringView Event) const;
	FAssetDocumentDurableCompletionReceipt Fail(
		EAssetDocumentPersistenceStep Step,
		const FAssetDocumentContractValidationResult& Validation,
		const TCHAR* FallbackCode,
		const TCHAR* FallbackMessage) const;
	FAssetDocumentDurableCompletionReceipt FailAndRecover(
		EAssetDocumentPersistenceStep Step,
		const FAssetDocumentContractValidationResult& Validation,
		const FAssetDocumentPersistencePlan& Plan) const;
	void RecordRecoveryDiagnostic(const FAssetDocumentJournalRecoveryResult& Recovery) const;
	FAssetDocumentPersistenceDependencies Dependencies;
};
