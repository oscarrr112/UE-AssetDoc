// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentAtomicFile.h"
#include "Runtime/AssetDocumentPackageStaging.h"
#include "Runtime/AssetDocumentPersistenceJournal.h"
#include "Runtime/AssetDocumentSidecarByteIo.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
FAssetDocumentJournalArtifact MakeArtifact(
	const FString& Target,
	const FString& Backup,
	TArrayView64<const uint8> Original,
	TArrayView64<const uint8> Installed,
	bool bExisted)
{
	FAssetDocumentJournalArtifact Artifact;
	Artifact.TargetPath = Target;
	Artifact.BackupPath = Backup;
	Artifact.PreimageContentDigest = bExisted ? ComputeAssetDocumentContentDigest(Original) : FString();
	Artifact.InstalledContentDigest = ComputeAssetDocumentContentDigest(Installed);
	Artifact.bTargetExisted = bExisted;
	return Artifact;
}

FAssetDocumentPersistenceJournalRecord MakeRecord(FString JournalId)
{
	FAssetDocumentPersistenceJournalRecord Record;
	Record.RequestId = FGuid::NewGuid();
	Record.TransactionId = FGuid::NewGuid();
	Record.JournalId = MoveTemp(JournalId);
	return Record;
}

bool SaveBytes(const FString& Path, TArrayView64<const uint8> Bytes)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

bool LoadMatches(const FString& Path, TArrayView64<const uint8> Expected)
{
	TArray64<uint8> Actual;
	return FFileHelper::LoadFileToArray(Actual, *Path) && Actual == Expected;
}

class FPackageSaveBackendStub final : public IAssetDocumentPackageSaveBackend
{
public:
	virtual FAssetDocumentContractValidationResult SaveToStaging(
		const FString& StagedHeaderPath,
		const FString& TargetHeaderPath,
		FAssetDocumentPackageArtifactSet& OutArtifacts) override
	{
		FAssetDocumentPackageArtifact& Second = OutArtifacts.Artifacts.AddDefaulted_GetRef();
		Second.StagedPath = FPaths::ChangeExtension(StagedHeaderPath, TEXT("uexp"));
		Second.TargetPath = FPaths::ChangeExtension(TargetHeaderPath, TEXT("uexp"));
		Second.ContentDigest = ComputeAssetDocumentContentDigest(MakeArrayView(reinterpret_cast<const uint8*>("b"), 1));
		FAssetDocumentPackageArtifact& First = OutArtifacts.Artifacts.AddDefaulted_GetRef();
		First.StagedPath = StagedHeaderPath;
		First.TargetPath = TargetHeaderPath;
		First.ContentDigest = ComputeAssetDocumentContentDigest(MakeArrayView(reinterpret_cast<const uint8*>("a"), 1));
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPackageSidecarInstallRecoveryTest,
	"UEAssetDoc.AtomicFramework.PackageSidecarInstallRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPackageSidecarInstallRecoveryTest::RunTest(const FString& Parameters)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("AtomicJournal_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString JournalRoot = FPaths::Combine(Root, TEXT("journal"));
	const FString BackupRoot = FPaths::Combine(JournalRoot, TEXT("backups"));
	IFileManager::Get().MakeDirectory(*BackupRoot, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const FString MountPoint = TEXT("/Game/Atomic/");
	const FString MountRoot = FPaths::Combine(Root, TEXT("installed")) + TEXT("/");
	FPackageName::RegisterMountPoint(MountPoint, MountRoot);
	ON_SCOPE_EXIT { FPackageName::UnRegisterMountPoint(MountPoint, MountRoot); };

	const TArray64<uint8> OriginalA = {0x01, 0x02, 0x03};
	const TArray64<uint8> OriginalC = {0x04, 0x05};
	const TArray64<uint8> Installed = {0x09, 0x08, 0x07};
	const FString TargetA = FPaths::Combine(Root, TEXT("a.bin"));
	const FString TargetB = FPaths::Combine(Root, TEXT("b.bin"));
	const FString TargetC = FPaths::Combine(Root, TEXT("c.bin"));
	const FString BackupA = FPaths::Combine(BackupRoot, TEXT("a.bin"));
	const FString BackupB = FPaths::Combine(BackupRoot, TEXT("b.bin"));
	const FString BackupC = FPaths::Combine(BackupRoot, TEXT("c.bin"));
	TestTrue(TEXT("writes exact baselines"), SaveBytes(BackupA, OriginalA) && SaveBytes(BackupC, OriginalC));
	TestTrue(TEXT("writes installed fixtures"), SaveBytes(TargetA, Installed) && SaveBytes(TargetB, Installed) && SaveBytes(TargetC, Installed));

	FAssetDocumentPersistenceJournalRecord Record = MakeRecord(TEXT("legal-installing"));
	Record.Artifacts.Add(MakeArtifact(TargetA, BackupA, OriginalA, Installed, true));
	Record.Artifacts.Add(MakeArtifact(TargetB, BackupB, {}, Installed, false));
	Record.Artifacts.Add(MakeArtifact(TargetC, BackupC, OriginalC, Installed, true));
	FAssetDocumentPersistenceJournal Journal(JournalRoot);
	TestTrue(TEXT("prepares legal journal baseline"), Journal.Prepare(Record).bValid);
	TestTrue(TEXT("marks all installed artifacts"), Journal.MarkArtifactInstalled(0).bValid
		&& Journal.MarkArtifactInstalled(1).bValid && Journal.MarkArtifactInstalled(2).bValid);
	const FAssetDocumentJournalRecoveryResult Recovery = Journal.RecoverIncomplete(Record.JournalId);
	TestEqual(TEXT("legal journal baseline completes owned recovery"), Recovery.Outcome,
		EAssetDocumentJournalRecoveryOutcome::RecoveredPreCommit);
	TestTrue(TEXT("recovery restores exact byte and absence baselines"), LoadMatches(TargetA, OriginalA)
		&& !IFileManager::Get().FileExists(*TargetB) && LoadMatches(TargetC, OriginalC));
	TestTrue(TEXT("successful precommit recovery carries frozen recovered diagnostic"),
		Recovery.Validation.bValid && Recovery.Validation.Diagnostics.Num() == 1
		&& Recovery.Validation.Diagnostics[0].Code == TEXT("Persistence.Recovered")
		&& !Recovery.Validation.Diagnostics[0].Path.IsEmpty()
		&& !Recovery.Validation.Diagnostics[0].Message.IsEmpty());
	const FAssetDocumentJournalRecoveryResult NoJournal = Journal.RecoverIncomplete(Record.JournalId);
	TestEqual(TEXT("recovery is idempotent after journal cleanup"), NoJournal.Outcome,
		EAssetDocumentJournalRecoveryOutcome::NoJournal);
	TestTrue(TEXT("no journal does not claim successful recovery"),
		NoJournal.Validation.bValid && NoJournal.Validation.Diagnostics.IsEmpty());

	TestEqual(TEXT("sha256 is stable lowercase wire form"), ComputeAssetDocumentContentDigest(MakeArrayView(
		reinterpret_cast<const uint8*>("abc"), 3)),
		FString(TEXT("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	FPackageSaveBackendStub PackageBackend;
	FAssetDocumentPackageStager PackageStager(PackageBackend);
	FAssetDocumentPersistencePlan PackagePlan;
	PackagePlan.TargetPackagePath = TEXT("/Game/Atomic/StagedAsset");
	PackagePlan.StagingRoot = FPaths::Combine(Root, TEXT("staging"));
	FAssetDocumentPackageArtifactSet PackageArtifacts;
	TestTrue(TEXT("package stager accepts authoritative artifact set"), PackageStager.Stage(PackagePlan, PackageArtifacts).bValid);
	TestTrue(TEXT("package artifacts are canonical sorted with aggregate digest"), PackageArtifacts.Artifacts.Num() == 2
		&& PackageArtifacts.Artifacts[0].TargetPath < PackageArtifacts.Artifacts[1].TargetPath
		&& PackageArtifacts.AggregateContentDigest.StartsWith(TEXT("sha256:")));

	UPackage* BoundPackage = CreatePackage(TEXT("/Game/Atomic/Bound"));
	UObject* BoundAsset = NewObject<UEnum>(BoundPackage, TEXT("BoundAsset"), RF_Public | RF_Standalone);
	FAssetDocumentPublicPackageSaveBackend PublicBackend(*BoundPackage, BoundAsset);
	const FString MismatchStage = FPaths::Combine(Root, TEXT("public-mismatch/Other.uasset"));
	const FString MismatchTarget = FPackageName::LongPackageNameToFilename(
		TEXT("/Game/Atomic/Other"), FPackageName::GetAssetPackageExtension());
	FAssetDocumentPackageArtifactSet MismatchArtifacts;
	const FAssetDocumentContractValidationResult PackageMismatch =
		PublicBackend.SaveToStaging(MismatchStage, MismatchTarget, MismatchArtifacts);
	TestTrue(TEXT("public package backend rejects mismatched bound package before filesystem use"),
		!PackageMismatch.bValid && !IFileManager::Get().DirectoryExists(*FPaths::GetPath(MismatchStage)));

	FAssetDocumentPersistencePlan SidecarPlan;
	SidecarPlan.StagingRoot = FPaths::Combine(Root, TEXT("sidecar-staging"));
	SidecarPlan.TargetSidecarPath = FPaths::Combine(Root, TEXT("opaque.sidecar"));
	SidecarPlan.Sidecar.Utf8Bytes.Append(Installed.GetData(), Installed.Num());
	FAssetDocumentByteArtifact SidecarArtifact;
	FAssetDocumentSidecarByteIo SidecarIo;
	TestTrue(TEXT("opaque sidecar bytes stage"), SidecarIo.Stage(SidecarPlan, SidecarArtifact).bValid);
	FAssetDocumentJournalArtifact SidecarBaseline;
	TestTrue(TEXT("absent sidecar baseline captures"), SidecarIo.CaptureInstalledBaseline(
		SidecarArtifact, FPaths::Combine(BackupRoot, TEXT("sidecar.bin")), SidecarBaseline).bValid);
	TestTrue(TEXT("opaque sidecar bytes install exactly"), SidecarIo.Install(SidecarArtifact).bValid
		&& LoadMatches(SidecarArtifact.TargetPath, Installed));
	TestTrue(TEXT("absent sidecar baseline restores exactly"), SidecarIo.Restore(SidecarBaseline).bValid
		&& !IFileManager::Get().FileExists(*SidecarArtifact.TargetPath));

	const FString DurableTarget = FPaths::Combine(Root, TEXT("durable.bin"));
	const FString DurableBackup = FPaths::Combine(BackupRoot, TEXT("durable.bin"));
	TestTrue(TEXT("writes durable fixtures"), SaveBytes(DurableTarget, Installed) && SaveBytes(DurableBackup, OriginalA));
	FAssetDocumentPersistenceJournalRecord DurableRecord = MakeRecord(TEXT("durable"));
	DurableRecord.Artifacts.Add(MakeArtifact(DurableTarget, DurableBackup, OriginalA, Installed, true));
	FAssetDocumentPersistenceJournal DurableJournal(JournalRoot);
	TestTrue(TEXT("marks durable record"), DurableJournal.Prepare(DurableRecord).bValid
		&& DurableJournal.MarkArtifactInstalled(0).bValid && DurableJournal.MarkDurableComplete().bValid);
	const FAssetDocumentJournalRecoveryResult DurableRecovery = DurableJournal.RecoverIncomplete(DurableRecord.JournalId);
	TestEqual(TEXT("durable recovery cleans without restoring preimage"), DurableRecovery.Outcome,
		EAssetDocumentJournalRecoveryOutcome::CleanedCommitted);
	TestTrue(TEXT("committed cleanup recovery carries frozen recovered diagnostic"),
		DurableRecovery.Validation.bValid && DurableRecovery.Validation.Diagnostics.Num() == 1
		&& DurableRecovery.Validation.Diagnostics[0].Code == TEXT("Persistence.Recovered"));
	TestTrue(TEXT("durable recovery retains installed bytes"), LoadMatches(DurableTarget, Installed));

	const FString DeferredTarget = FPaths::Combine(Root, TEXT("deferred.bin"));
	const FString DeferredBackup = FPaths::Combine(BackupRoot, TEXT("deferred.bin"));
	TestTrue(TEXT("writes deferred-cleanup fixtures"), SaveBytes(DeferredTarget, Installed) && SaveBytes(DeferredBackup, OriginalA));
	FAssetDocumentPersistenceJournalRecord DeferredRecord = MakeRecord(TEXT("deferred"));
	DeferredRecord.Artifacts.Add(MakeArtifact(DeferredTarget, DeferredBackup, OriginalA, Installed, true));
	FAssetDocumentPersistenceJournal DeferredJournal(JournalRoot);
	TestTrue(TEXT("prepares deferred-cleanup durable record"), DeferredJournal.Prepare(DeferredRecord).bValid
		&& DeferredJournal.MarkArtifactInstalled(0).bValid && DeferredJournal.MarkDurableComplete().bValid);
	FAssetDocumentPersistenceJournal::FailNextCleanupForTest();
	const FAssetDocumentJournalRecoveryResult DeferredRecovery = DeferredJournal.RecoverIncomplete(DeferredRecord.JournalId);
	TestEqual(TEXT("post-mark cleanup failure is deferred"), DeferredRecovery.Outcome,
		EAssetDocumentJournalRecoveryOutcome::CommittedCleanupDeferred);
	TestTrue(TEXT("deferred cleanup does not claim successful recovery"),
		!DeferredRecovery.Validation.bValid && DeferredRecovery.Validation.Diagnostics.Num() == 1
		&& DeferredRecovery.Validation.Diagnostics[0].Code == TEXT("Persistence.CleanupDeferred"));
	TestTrue(TEXT("deferred cleanup retains installed bytes"), LoadMatches(DeferredTarget, Installed));
	FAssetDocumentPersistenceJournal DeferredRetryJournal(JournalRoot);
	const FAssetDocumentJournalRecoveryResult DeferredRetry = DeferredRetryJournal.RecoverIncomplete(DeferredRecord.JournalId);
	TestEqual(TEXT("next recovery retries committed cleanup"), DeferredRetry.Outcome,
		EAssetDocumentJournalRecoveryOutcome::CleanedCommitted);
	TestTrue(TEXT("deferred committed cleanup reports recovery exactly once"),
		DeferredRetry.Validation.Diagnostics.Num() == 1
		&& DeferredRetry.Validation.Diagnostics[0].Code == TEXT("Persistence.Recovered"));
	TestTrue(TEXT("committed cleanup never restores preimage"), LoadMatches(DeferredTarget, Installed));

	const FString FailedTarget = FPaths::Combine(Root, TEXT("failed-mark.bin"));
	const FString FailedBackup = FPaths::Combine(BackupRoot, TEXT("failed-mark.bin"));
	TestTrue(TEXT("writes failed-mark fixtures"), SaveBytes(FailedTarget, Installed) && SaveBytes(FailedBackup, OriginalA));
	FAssetDocumentPersistenceJournalRecord FailedRecord = MakeRecord(TEXT("failed-mark"));
	FailedRecord.Artifacts.Add(MakeArtifact(FailedTarget, FailedBackup, OriginalA, Installed, true));
	FAssetDocumentPersistenceJournal FailedJournal(JournalRoot);
	TestTrue(TEXT("prepares failed durable marker"), FailedJournal.Prepare(FailedRecord).bValid
		&& FailedJournal.MarkArtifactInstalled(0).bValid);
	FAssetDocumentAtomicFile::FailNextWriteAtForTest(EAssetDocumentAtomicFileFailurePoint::Flush);
	TestFalse(TEXT("durable state write failure remains pre-commit"), FailedJournal.MarkDurableComplete().bValid);
	FAssetDocumentAtomicFile::ResetFailureForTest();
	FAssetDocumentPersistenceJournal FailedRecoveryJournal(JournalRoot);
	const FAssetDocumentJournalRecoveryResult FailedRecovery = FailedRecoveryJournal.RecoverIncomplete(FailedRecord.JournalId);
	TestEqual(TEXT("failed durable marker restores preimage"), FailedRecovery.Outcome,
		EAssetDocumentJournalRecoveryOutcome::RecoveredPreCommit);
	TestTrue(TEXT("failed durable marker recovery carries recovered diagnostic"),
		FailedRecovery.Validation.Diagnostics.Num() == 1
		&& FailedRecovery.Validation.Diagnostics[0].Code == TEXT("Persistence.Recovered"));
	TestTrue(TEXT("failed durable marker restores exact bytes"), LoadMatches(FailedTarget, OriginalA));

	bool bSyncRestoreCalled = false;
	TArray64<uint8> RestoredSyncBytes;
	FAssetDocumentPersistenceJournal SyncJournal(
		JournalRoot,
		[&](TArrayView64<const uint8> Bytes, bool bExisted)
		{
			bSyncRestoreCalled = bExisted;
			RestoredSyncBytes.Append(Bytes.GetData(), Bytes.Num());
			FAssetDocumentContractValidationResult Result;
			Result.bValid = true;
			return Result;
		});
	const FString SyncTarget = FPaths::Combine(Root, TEXT("sync-artifact.bin"));
	const FString SyncBackup = FPaths::Combine(BackupRoot, TEXT("sync-artifact.bin"));
	TestTrue(TEXT("writes sync recovery fixtures"), SaveBytes(SyncTarget, Installed) && SaveBytes(SyncBackup, OriginalA));
	FAssetDocumentPersistenceJournalRecord SyncRecord = MakeRecord(TEXT("sync-preimage"));
	SyncRecord.Artifacts.Add(MakeArtifact(SyncTarget, SyncBackup, OriginalA, Installed, true));
	SyncRecord.SyncPreimageBytes.Append(OriginalC.GetData(), OriginalC.Num());
	SyncRecord.SyncPreimageContentDigest = ComputeAssetDocumentContentDigest(OriginalC);
	SyncRecord.bSyncBaselineExisted = true;
	TestTrue(TEXT("prepares sync recovery journal"), SyncJournal.Prepare(SyncRecord).bValid
		&& SyncJournal.MarkArtifactInstalled(0).bValid && SyncJournal.MarkSyncInstalled().bValid);
	const FAssetDocumentJournalRecoveryResult SyncRecovery = SyncJournal.RecoverIncomplete(SyncRecord.JournalId);
	TestEqual(TEXT("sync-installed journal completes precommit recovery"), SyncRecovery.Outcome,
		EAssetDocumentJournalRecoveryOutcome::RecoveredPreCommit);
	TestTrue(TEXT("sync-installed recovery carries frozen recovered diagnostic"),
		SyncRecovery.Validation.Diagnostics.Num() == 1
		&& SyncRecovery.Validation.Diagnostics[0].Code == TEXT("Persistence.Recovered"));
	TestTrue(TEXT("journal restores opaque sync preimage"), bSyncRestoreCalled && RestoredSyncBytes == OriginalC);

	FAssetDocumentPersistenceJournalRecord UnsafeRecord = MakeRecord(TEXT("unsafe"));
	UnsafeRecord.Artifacts.Add(MakeArtifact(TargetA, FPaths::Combine(Root, TEXT("outside.bin")), OriginalA, Installed, true));
	FAssetDocumentPersistenceJournal UnsafeJournal(JournalRoot);
	TestFalse(TEXT("backup path outside journal authority is rejected"), UnsafeJournal.Prepare(UnsafeRecord).bValid);
	TestEqual(TEXT("journal id traversal is poisoned"), UnsafeJournal.RecoverIncomplete(TEXT("../unsafe")).Outcome,
		EAssetDocumentJournalRecoveryOutcome::Poisoned);
	FAssetDocumentPersistenceJournalRecord SymlinkRecord = MakeRecord(TEXT("symlink"));
	const FString SymlinkTarget = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		GetAssetDocumentPluginBaseDir(), TEXT("symlink-escape.bin")));
	SymlinkRecord.Artifacts.Add(MakeArtifact(SymlinkTarget, FPaths::Combine(BackupRoot, TEXT("symlink.bin")), {}, Installed, false));
	FAssetDocumentPersistenceJournal SymlinkJournal(JournalRoot);
	TestFalse(TEXT("symlink escape target is rejected"), SymlinkJournal.Prepare(SymlinkRecord).bValid);

	const FString CorruptTarget = FPaths::Combine(Root, TEXT("corrupt.bin"));
	const FString CorruptBackup = FPaths::Combine(BackupRoot, TEXT("corrupt.bin"));
	TestTrue(TEXT("writes corruption fixtures"), SaveBytes(CorruptTarget, Installed) && SaveBytes(CorruptBackup, OriginalA));
	FAssetDocumentPersistenceJournalRecord CorruptRecord = MakeRecord(TEXT("corrupt-digest"));
	CorruptRecord.Artifacts.Add(MakeArtifact(CorruptTarget, CorruptBackup, OriginalA, Installed, true));
	FAssetDocumentPersistenceJournal CorruptJournal(JournalRoot);
	TestTrue(TEXT("prepares corruption journal"), CorruptJournal.Prepare(CorruptRecord).bValid
		&& CorruptJournal.MarkArtifactInstalled(0).bValid);
	const TArray64<uint8> CorruptBytes = {0xff};
	TestTrue(TEXT("corrupts recovery backup"), SaveBytes(CorruptBackup, CorruptBytes));
	FAssetDocumentPersistenceJournal CorruptRecoveryJournal(JournalRoot);
	const FAssetDocumentJournalRecoveryResult CorruptRecovery = CorruptRecoveryJournal.RecoverIncomplete(CorruptRecord.JournalId);
	TestEqual(TEXT("corrupt preimage digest poisons recovery"), CorruptRecovery.Outcome,
		EAssetDocumentJournalRecoveryOutcome::Poisoned);
	TestTrue(TEXT("poisoned recovery does not claim successful recovery"),
		!CorruptRecovery.Validation.bValid && CorruptRecovery.Validation.Diagnostics.Num() == 1
		&& CorruptRecovery.Validation.Diagnostics[0].Code == TEXT("Atomic.Poisoned.RecoveryFailed"));

	const FString MagicJournalId = TEXT("corrupt-magic");
	const FString MagicJournalPath = FPaths::Combine(JournalRoot, MagicJournalId + TEXT(".bin"));
	TestTrue(TEXT("writes corrupt journal magic"), SaveBytes(MagicJournalPath, CorruptBytes));
	FAssetDocumentPersistenceJournal MagicJournal(JournalRoot);
	TestEqual(TEXT("corrupt journal wire record poisons recovery"), MagicJournal.RecoverIncomplete(MagicJournalId).Outcome,
		EAssetDocumentJournalRecoveryOutcome::Poisoned);
	return true;
}

#endif
