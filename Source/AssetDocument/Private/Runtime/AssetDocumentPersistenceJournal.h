// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

enum class EAssetDocumentJournalState : uint8
{
	Prepared = 0,
	Installing = 1,
	ArtifactsInstalled = 2,
	SyncInstalled = 3,
	DurableComplete = 4,
	Recovered = 5,
	Poisoned = 6,
};

struct FAssetDocumentJournalArtifact
{
	FString TargetPath;
	FString BackupPath;
	FString PreimageContentDigest;
	FString InstalledContentDigest;
	bool bTargetExisted = false;
	bool bInstalled = false;
};

struct FAssetDocumentPersistenceJournalRecord
{
	FGuid RequestId;
	FGuid TransactionId;
	FString JournalId;
	EAssetDocumentJournalState State = EAssetDocumentJournalState::Prepared;
	TArray<FAssetDocumentJournalArtifact> Artifacts;
	TArray<uint8> SyncPreimageBytes;
	FString SyncPreimageContentDigest;
	bool bSyncBaselineExisted = false;
	bool bSyncInstalled = false;
};

enum class EAssetDocumentJournalRecoveryOutcome : uint8
{
	NoJournal = 0,
	RecoveredPreCommit = 1,
	CleanedCommitted = 2,
	CommittedCleanupDeferred = 3,
	Poisoned = 4,
};

struct FAssetDocumentJournalRecoveryResult
{
	EAssetDocumentJournalRecoveryOutcome Outcome = EAssetDocumentJournalRecoveryOutcome::NoJournal;
	FAssetDocumentContractValidationResult Validation;
	bool bMayStartNewPersistence = true;
};

using FAssetDocumentSyncPreimageRestore =
	TFunction<FAssetDocumentContractValidationResult(TArrayView64<const uint8>, bool)>;
using FAssetDocumentInstalledSyncValidate =
	TFunction<FAssetDocumentContractValidationResult(const FGuid&, const FGuid&, FStringView)>;

class FAssetDocumentPersistenceJournal
{
public:
	explicit FAssetDocumentPersistenceJournal(
		FString InJournalRoot,
		FAssetDocumentSyncPreimageRestore InSyncRestore = {},
		FAssetDocumentInstalledSyncValidate InSyncValidate = {});
	FAssetDocumentJournalRecoveryResult RecoverIncomplete(FStringView JournalId);
	FAssetDocumentContractValidationResult Prepare(const FAssetDocumentPersistenceJournalRecord& Record);
	FAssetDocumentContractValidationResult MarkArtifactInstalled(int32 ArtifactIndex);
	FAssetDocumentContractValidationResult MarkSyncInstalled();
	FAssetDocumentContractValidationResult MarkDurableComplete();
	FAssetDocumentContractValidationResult Cleanup();

#if WITH_DEV_AUTOMATION_TESTS
	TOptional<EAssetDocumentJournalState> GetCurrentStateForTest() const;
	static void FailNextCleanupForTest();
#endif

private:
	FAssetDocumentContractValidationResult Load(FStringView JournalId);
	FAssetDocumentContractValidationResult ValidatePrepareAdmission(
		const FAssetDocumentPersistenceJournalRecord& Record) const;
	FAssetDocumentContractValidationResult InstallPreparedRecord(
		const FAssetDocumentPersistenceJournalRecord& Record);
	FAssetDocumentContractValidationResult ValidateCleanupAdmission() const;
	FAssetDocumentContractValidationResult ValidateCurrent() const;
	FAssetDocumentContractValidationResult ValidateArtifacts() const;
	FAssetDocumentContractValidationResult RestorePreCommit();
	FAssetDocumentContractValidationResult RestoreSyncPreimage();
	FAssetDocumentContractValidationResult RestoreArtifacts();
	FAssetDocumentContractValidationResult CompletePreCommitRecovery();
	FAssetDocumentContractValidationResult ValidateCommitted() const;
	FAssetDocumentContractValidationResult ValidateCommittedSync() const;
	FAssetDocumentContractValidationResult PoisonRecovery(const TCHAR* Message);
	FAssetDocumentJournalRecoveryResult RecoverLoaded();
	FAssetDocumentJournalRecoveryResult RecoverCommitted();
	FAssetDocumentJournalRecoveryResult RecoverRecovered();
	FAssetDocumentJournalRecoveryResult RecoverPreCommit();
	FAssetDocumentContractValidationResult DeleteCurrentJournal();
	FAssetDocumentContractValidationResult WriteCurrent();
	FString GetJournalPath(FStringView JournalId) const;

	FString JournalRoot;
	FAssetDocumentSyncPreimageRestore SyncRestore;
	FAssetDocumentInstalledSyncValidate SyncValidate;
	TOptional<FAssetDocumentPersistenceJournalRecord> Current;
};
