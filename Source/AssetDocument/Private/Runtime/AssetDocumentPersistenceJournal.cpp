// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentPersistenceJournal.h"

#include "AssetDocumentAtomicFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Runtime/AssetDocumentSidecarByteIo.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Templates/Atomic.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include "Containers/StringConv.h"
#include <sys/stat.h>
#endif

namespace
{
constexpr uint32 JournalWireVersion = 1;
constexpr int32 MaximumJournalArtifacts = 64;
const FString JournalMagic = TEXT("ASSETDOC_ATOMIC_JOURNAL");

#if WITH_DEV_AUTOMATION_TESTS
TAtomic<bool> GFailNextJournalCleanup(false);
#endif

FAssetDocumentContractValidationResult MakeJournalResult(
	bool bValid,
	const TCHAR* Code = TEXT(""),
	const TCHAR* Message = TEXT(""))
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!bValid)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/PersistenceJournal");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

FAssetDocumentContractValidationResult MakeRecoveredJournalResult()
{
	FAssetDocumentContractValidationResult Result = MakeJournalResult(true);
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/PersistenceJournal");
	Diagnostic.Code = TEXT("Persistence.Recovered");
	Diagnostic.Message = TEXT("Incomplete persistence recovery completed.");
	return Result;
}

void SerializeBool(FArchive& Archive, bool& Value)
{
	uint8 WireValue = Value ? 1 : 0;
	Archive << WireValue;
	Value = WireValue == 1;
	if (Archive.IsLoading() && WireValue > 1)
	{
		Archive.SetError();
	}
}

void SerializeArtifact(FArchive& Archive, FAssetDocumentJournalArtifact& Artifact)
{
	Archive << Artifact.TargetPath << Artifact.BackupPath;
	Archive << Artifact.PreimageContentDigest << Artifact.InstalledContentDigest;
	SerializeBool(Archive, Artifact.bTargetExisted);
	SerializeBool(Archive, Artifact.bInstalled);
}

bool IsWireHeaderValid(const FString& Magic, uint32 Version, int32 ArtifactCount)
{
	return Magic == JournalMagic && Version == JournalWireVersion
		&& ArtifactCount >= 0 && ArtifactCount <= MaximumJournalArtifacts;
}

bool SerializeRecord(FArchive& Archive, FAssetDocumentPersistenceJournalRecord& Record)
{
	FString Magic = JournalMagic;
	uint32 Version = JournalWireVersion;
	uint8 State = static_cast<uint8>(Record.State);
	Archive << Magic << Version << Record.RequestId << Record.TransactionId << Record.JournalId << State;
	int32 ArtifactCount = Record.Artifacts.Num();
	Archive << ArtifactCount;
	if (!IsWireHeaderValid(Magic, Version, ArtifactCount))
	{
		return false;
	}
	if (Archive.IsLoading())
	{
		Record.Artifacts.SetNum(ArtifactCount);
	}
	for (FAssetDocumentJournalArtifact& Artifact : Record.Artifacts)
	{
		SerializeArtifact(Archive, Artifact);
	}
	Archive << Record.SyncPreimageBytes << Record.SyncPreimageContentDigest;
	SerializeBool(Archive, Record.bSyncBaselineExisted);
	SerializeBool(Archive, Record.bSyncInstalled);
	Record.State = static_cast<EAssetDocumentJournalState>(State);
	if (Archive.IsError())
	{
		return false;
	}
	return State <= static_cast<uint8>(EAssetDocumentJournalState::Poisoned);
}

bool IsSafeJournalCharacter(TCHAR Character)
{
	return FChar::IsAlnum(Character) || Character == TEXT('-') || Character == TEXT('_');
}

bool IsSafeJournalId(FStringView JournalId)
{
	if (JournalId.IsEmpty())
	{
		return false;
	}
	if (JournalId.Contains(TEXT("..")))
	{
		return false;
	}
	for (TCHAR Character : JournalId)
	{
		if (!IsSafeJournalCharacter(Character))
		{
			return false;
		}
	}
	return true;
}

bool IsActualSymlink(IPlatformFile& PlatformFile, const FString& Path)
{
#if PLATFORM_MAC || PLATFORM_LINUX
	struct stat FileInfo;
	FTCHARToUTF8 NativePath(*Path);
	return lstat(NativePath.Get(), &FileInfo) == 0 && S_ISLNK(FileInfo.st_mode);
#else
	return PlatformFile.IsSymlink(*Path) == ESymlinkResult::Symlink;
#endif
}

bool IsExistingPath(IPlatformFile& PlatformFile, const FString& Path)
{
	if (PlatformFile.FileExists(*Path))
	{
		return true;
	}
	return PlatformFile.DirectoryExists(*Path);
}

bool IsExistingSymlink(IPlatformFile& PlatformFile, const FString& Path)
{
	if (!IsExistingPath(PlatformFile, Path))
	{
		return false;
	}
	return IsActualSymlink(PlatformFile, Path);
}

bool HasSymlinkComponent(const FString& AbsolutePath)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FString Cursor = AbsolutePath;
	while (!Cursor.IsEmpty() && Cursor != FPaths::GetPath(Cursor))
	{
		if (IsExistingSymlink(PlatformFile, Cursor))
		{
			return true;
		}
		Cursor = FPaths::GetPath(Cursor);
	}
	return false;
}

bool IsCanonicalAbsolutePath(const FString& Path)
{
	if (FPaths::IsRelative(Path))
	{
		return false;
	}
	if (Path.Contains(TEXT("..")))
	{
		return false;
	}
	FString Normalized = Path;
	FPaths::NormalizeFilename(Normalized);
	if (Normalized != Path)
	{
		return false;
	}
	return !HasSymlinkComponent(Path);
}

bool IsLowerHex(TStringView<TCHAR> Hex)
{
	for (TCHAR Character : Hex)
	{
		if (!FChar::IsHexDigit(Character) || FChar::IsUpper(Character))
		{
			return false;
		}
	}
	return true;
}

bool IsDigest(const FString& Digest)
{
	return Digest.Len() == 71 && Digest.StartsWith(TEXT("sha256:"))
		&& IsLowerHex(FStringView(Digest).RightChop(7));
}

bool HasValidArtifactPaths(const FAssetDocumentJournalArtifact& Artifact, const FString& JournalRoot)
{
	return IsCanonicalAbsolutePath(Artifact.TargetPath) && IsCanonicalAbsolutePath(Artifact.BackupPath)
		&& FPaths::IsUnderDirectory(Artifact.BackupPath, JournalRoot);
}

bool HasValidArtifactDigests(const FAssetDocumentJournalArtifact& Artifact)
{
	if (!IsDigest(Artifact.InstalledContentDigest))
	{
		return false;
	}
	return !Artifact.bTargetExisted || IsDigest(Artifact.PreimageContentDigest);
}

bool IsValidArtifact(const FAssetDocumentJournalArtifact& Artifact, const FString& JournalRoot)
{
	return HasValidArtifactPaths(Artifact, JournalRoot) && HasValidArtifactDigests(Artifact);
}

bool FileMatchesDigest(const FString& Path, const FString& Digest)
{
	TArray64<uint8> Bytes;
	return FFileHelper::LoadFileToArray(Bytes, *Path)
		&& ComputeAssetDocumentContentDigest(Bytes) == Digest;
}

bool IsPrepareIdentityValid(const FAssetDocumentPersistenceJournalRecord& Record)
{
	return Record.RequestId.IsValid() && Record.TransactionId.IsValid()
		&& IsSafeJournalId(Record.JournalId);
}

bool IsCleanupState(EAssetDocumentJournalState State)
{
	return State == EAssetDocumentJournalState::DurableComplete
		|| State == EAssetDocumentJournalState::Recovered;
}

bool DeleteJournalBackups(const TArray<FAssetDocumentJournalArtifact>& Artifacts)
{
	for (const FAssetDocumentJournalArtifact& Artifact : Artifacts)
	{
		if (IFileManager::Get().FileExists(*Artifact.BackupPath)
			&& !IFileManager::Get().Delete(*Artifact.BackupPath, false, true, true))
		{
			return false;
		}
	}
	return true;
}

bool HasValidCurrentIdentity(const FAssetDocumentPersistenceJournalRecord& Record)
{
	return Record.RequestId.IsValid() && Record.TransactionId.IsValid()
		&& IsSafeJournalId(Record.JournalId) && !Record.Artifacts.IsEmpty();
}

bool IsSyncPreimageValid(const FAssetDocumentPersistenceJournalRecord& Record)
{
	if (!Record.bSyncBaselineExisted)
	{
		return Record.SyncPreimageBytes.IsEmpty() && Record.SyncPreimageContentDigest.IsEmpty();
	}
	if (!IsDigest(Record.SyncPreimageContentDigest))
	{
		return false;
	}
	return ComputeAssetDocumentContentDigest(Record.SyncPreimageBytes) == Record.SyncPreimageContentDigest;
}

bool IsLoadedRecordValid(
	FMemoryReader& Reader,
	FAssetDocumentPersistenceJournalRecord& Record,
	FStringView JournalId)
{
	if (!SerializeRecord(Reader, Record))
	{
		return false;
	}
	if (Record.JournalId != JournalId)
	{
		return false;
	}
	return Reader.Tell() == Reader.TotalSize();
}

#if WITH_DEV_AUTOMATION_TESTS
bool ConsumeCleanupFailure()
{
	bool bExpected = true;
	return GFailNextJournalCleanup.CompareExchange(bExpected, false);
}
#endif
}

FAssetDocumentPersistenceJournal::FAssetDocumentPersistenceJournal(
	FString InJournalRoot,
	FAssetDocumentSyncPreimageRestore InSyncRestore,
	FAssetDocumentInstalledSyncValidate InSyncValidate)
	: JournalRoot(FPaths::ConvertRelativePathToFull(MoveTemp(InJournalRoot)))
	, SyncRestore(MoveTemp(InSyncRestore))
	, SyncValidate(MoveTemp(InSyncValidate))
{
	FPaths::NormalizeFilename(JournalRoot);
}

FAssetDocumentJournalRecoveryResult FAssetDocumentPersistenceJournal::RecoverIncomplete(FStringView JournalId)
{
	FAssetDocumentJournalRecoveryResult Result;
	if (!IsSafeJournalId(JournalId))
	{
		Result.Outcome = EAssetDocumentJournalRecoveryOutcome::Poisoned;
		Result.Validation = MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Journal identity is unsafe."));
		Result.bMayStartNewPersistence = false;
		return Result;
	}
	if (!IFileManager::Get().FileExists(*GetJournalPath(JournalId)))
	{
		Result.Validation = MakeJournalResult(true);
		return Result;
	}
	Result.Validation = Load(JournalId);
	if (!Result.Validation.bValid)
	{
		Result.Outcome = EAssetDocumentJournalRecoveryOutcome::Poisoned;
		Result.bMayStartNewPersistence = false;
		return Result;
	}
	return RecoverLoaded();
}

FAssetDocumentJournalRecoveryResult FAssetDocumentPersistenceJournal::RecoverLoaded()
{
	if (Current->State == EAssetDocumentJournalState::DurableComplete)
	{
		return RecoverCommitted();
	}
	if (Current->State == EAssetDocumentJournalState::Recovered)
	{
		return RecoverRecovered();
	}
	return RecoverPreCommit();
}

FAssetDocumentJournalRecoveryResult FAssetDocumentPersistenceJournal::RecoverCommitted()
{
	FAssetDocumentJournalRecoveryResult Result;
	Result.Validation = ValidateCommitted();
	if (!Result.Validation.bValid)
	{
		Result.Outcome = EAssetDocumentJournalRecoveryOutcome::Poisoned;
		Result.bMayStartNewPersistence = false;
		return Result;
	}
	if (!Cleanup().bValid)
	{
		Result.Outcome = EAssetDocumentJournalRecoveryOutcome::CommittedCleanupDeferred;
		Result.Validation = MakeJournalResult(false, TEXT("Persistence.CleanupDeferred"), TEXT("Committed journal cleanup is deferred."));
		Result.bMayStartNewPersistence = false;
		return Result;
	}
	Result.Outcome = EAssetDocumentJournalRecoveryOutcome::CleanedCommitted;
	Result.Validation = MakeRecoveredJournalResult();
	return Result;
}

FAssetDocumentJournalRecoveryResult FAssetDocumentPersistenceJournal::RecoverRecovered()
{
	FAssetDocumentJournalRecoveryResult Result;
	Result.Validation = Cleanup();
	if (!Result.Validation.bValid)
	{
		Result.Outcome = EAssetDocumentJournalRecoveryOutcome::Poisoned;
		Result.bMayStartNewPersistence = false;
		return Result;
	}
	Result.Outcome = EAssetDocumentJournalRecoveryOutcome::RecoveredPreCommit;
	Result.Validation = MakeRecoveredJournalResult();
	return Result;
}

FAssetDocumentJournalRecoveryResult FAssetDocumentPersistenceJournal::RecoverPreCommit()
{
	FAssetDocumentJournalRecoveryResult Result;
	Result.Validation = RestorePreCommit();
	if (!Result.Validation.bValid)
	{
		Result.Outcome = EAssetDocumentJournalRecoveryOutcome::Poisoned;
		Result.bMayStartNewPersistence = false;
		return Result;
	}
	Result.Outcome = EAssetDocumentJournalRecoveryOutcome::RecoveredPreCommit;
	Result.Validation = MakeRecoveredJournalResult();
	return Result;
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::Prepare(
	const FAssetDocumentPersistenceJournalRecord& Record)
{
	const FAssetDocumentContractValidationResult Admission = ValidatePrepareAdmission(Record);
	if (!Admission.bValid)
	{
		return Admission;
	}
	return InstallPreparedRecord(Record);
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::ValidatePrepareAdmission(
	const FAssetDocumentPersistenceJournalRecord& Record) const
{
	if (Current.IsSet())
	{
		return MakeJournalResult(false, TEXT("Persistence.PrepareJournalFailed"), TEXT("Journal identity or lifecycle is invalid."));
	}
	if (!IsPrepareIdentityValid(Record))
	{
		return MakeJournalResult(false, TEXT("Persistence.PrepareJournalFailed"), TEXT("Journal identity or lifecycle is invalid."));
	}
	if (!IFileManager::Get().MakeDirectory(*JournalRoot, true))
	{
		return MakeJournalResult(false, TEXT("Persistence.PrepareJournalFailed"), TEXT("Journal root could not be created."));
	}
	return MakeJournalResult(true);
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::InstallPreparedRecord(
	const FAssetDocumentPersistenceJournalRecord& Record)
{
	Current = Record;
	Current->State = EAssetDocumentJournalState::Prepared;
	const FAssetDocumentContractValidationResult Validation = ValidateCurrent();
	if (!Validation.bValid)
	{
		Current.Reset();
		return Validation;
	}
	const FAssetDocumentContractValidationResult Written = WriteCurrent();
	if (!Written.bValid)
	{
		Current.Reset();
	}
	return Written;
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::MarkArtifactInstalled(int32 ArtifactIndex)
{
	if (!Current.IsSet() || !Current->Artifacts.IsValidIndex(ArtifactIndex)
		|| Current->State == EAssetDocumentJournalState::DurableComplete)
	{
		return MakeJournalResult(false, TEXT("Persistence.AtomicInstallFailed"), TEXT("Journal artifact marker is invalid."));
	}
	Current->State = EAssetDocumentJournalState::Installing;
	Current->Artifacts[ArtifactIndex].bInstalled = true;
	const bool bAllInstalled = Current->Artifacts.ContainsByPredicate(
		[](const FAssetDocumentJournalArtifact& Artifact) { return !Artifact.bInstalled; }) == false;
	Current->State = bAllInstalled ? EAssetDocumentJournalState::ArtifactsInstalled : Current->State;
	return WriteCurrent();
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::MarkSyncInstalled()
{
	if (!Current.IsSet() || Current->State != EAssetDocumentJournalState::ArtifactsInstalled)
	{
		return MakeJournalResult(false, TEXT("Persistence.PrepareJournalFailed"), TEXT("Sync marker requires installed artifacts."));
	}
	Current->bSyncInstalled = true;
	Current->State = EAssetDocumentJournalState::SyncInstalled;
	return WriteCurrent();
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::MarkDurableComplete()
{
	if (!Current.IsSet() || (Current->State != EAssetDocumentJournalState::ArtifactsInstalled
		&& Current->State != EAssetDocumentJournalState::SyncInstalled))
	{
		return MakeJournalResult(false, TEXT("Persistence.PrepareJournalFailed"), TEXT("Durable marker requires installed state."));
	}
	Current->State = EAssetDocumentJournalState::DurableComplete;
	return WriteCurrent();
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::Cleanup()
{
	const FAssetDocumentContractValidationResult Admission = ValidateCleanupAdmission();
	if (!Admission.bValid)
	{
		return Admission;
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeCleanupFailure())
	{
		return MakeJournalResult(false, TEXT("Persistence.CleanupDeferred"), TEXT("Injected journal cleanup deferral."));
	}
#endif
	if (!DeleteJournalBackups(Current->Artifacts))
	{
		return MakeJournalResult(false, TEXT("Persistence.CleanupDeferred"), TEXT("Journal backup cleanup is deferred."));
	}
	return DeleteCurrentJournal();
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::ValidateCleanupAdmission() const
{
	if (!Current.IsSet())
	{
		return MakeJournalResult(false, TEXT("Persistence.CleanupDeferred"), TEXT("Journal is not in a cleanup state."));
	}
	if (!IsCleanupState(Current->State))
	{
		return MakeJournalResult(false, TEXT("Persistence.CleanupDeferred"), TEXT("Journal is not in a cleanup state."));
	}
	return MakeJournalResult(true);
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::DeleteCurrentJournal()
{
	const FString JournalPath = GetJournalPath(Current->JournalId);
	if (!IFileManager::Get().Delete(*JournalPath, false, true, true))
	{
		return MakeJournalResult(false, TEXT("Persistence.CleanupDeferred"), TEXT("Journal record cleanup is deferred."));
	}
	FString Error;
	if (!FAssetDocumentAtomicFile::FlushParentDirectory(JournalPath, Error))
	{
		return MakeJournalResult(false, TEXT("Persistence.CleanupDeferred"), TEXT("Journal directory cleanup flush is deferred."));
	}
	Current.Reset();
	return MakeJournalResult(true);
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::Load(FStringView JournalId)
{
	if (!IsSafeJournalId(JournalId))
	{
		return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Journal identity is unsafe."));
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *GetJournalPath(JournalId)))
	{
		return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Journal bytes could not be read."));
	}
	FMemoryReader Reader(Bytes, true);
	FAssetDocumentPersistenceJournalRecord Record;
	if (!IsLoadedRecordValid(Reader, Record, JournalId))
	{
		return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Journal wire record is corrupt."));
	}
	Current = MoveTemp(Record);
	return ValidateCurrent();
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::ValidateCurrent() const
{
	if (!Current.IsSet())
	{
		return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Journal record identity is invalid."));
	}
	if (!HasValidCurrentIdentity(Current.GetValue()))
	{
		return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Journal record identity is invalid."));
	}
	if (!IsSyncPreimageValid(Current.GetValue()))
	{
		return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Journal sync preimage is invalid."));
	}
	return ValidateArtifacts();
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::ValidateArtifacts() const
{
	for (const FAssetDocumentJournalArtifact& Artifact : Current->Artifacts)
	{
		if (!IsValidArtifact(Artifact, JournalRoot))
		{
			return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Journal artifact authority is invalid."));
		}
	}
	return MakeJournalResult(true);
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::RestorePreCommit()
{
	const FAssetDocumentContractValidationResult SyncResult = RestoreSyncPreimage();
	if (!SyncResult.bValid)
	{
		return PoisonRecovery(TEXT("Journal sync preimage recovery failed."));
	}
	const FAssetDocumentContractValidationResult ArtifactResult = RestoreArtifacts();
	if (!ArtifactResult.bValid)
	{
		return PoisonRecovery(TEXT("Journal artifact recovery failed."));
	}
	return CompletePreCommitRecovery();
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::RestoreArtifacts()
{
	FAssetDocumentSidecarByteIo ByteIo;
	for (int32 Index = Current->Artifacts.Num() - 1; Index >= 0; --Index)
	{
		const FAssetDocumentJournalArtifact& Artifact = Current->Artifacts[Index];
		if (Artifact.bInstalled && !ByteIo.Restore(Artifact).bValid)
		{
			return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Journal artifact recovery failed."));
		}
	}
	return MakeJournalResult(true);
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::CompletePreCommitRecovery()
{
	Current->State = EAssetDocumentJournalState::Recovered;
	const FAssetDocumentContractValidationResult Written = WriteCurrent();
	if (!Written.bValid)
	{
		return Written;
	}
	const FAssetDocumentContractValidationResult Cleaned = Cleanup();
	if (!Cleaned.bValid)
	{
		return Cleaned;
	}
	return MakeJournalResult(true);
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::RestoreSyncPreimage()
{
	if (!Current->bSyncInstalled)
	{
		return MakeJournalResult(true);
	}
	if (!SyncRestore)
	{
		return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Sync recovery authority is unavailable."));
	}
	return SyncRestore(Current->SyncPreimageBytes, Current->bSyncBaselineExisted);
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::PoisonRecovery(const TCHAR* Message)
{
	Current->State = EAssetDocumentJournalState::Poisoned;
	WriteCurrent();
	return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), Message);
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::ValidateCommitted() const
{
	for (const FAssetDocumentJournalArtifact& Artifact : Current->Artifacts)
	{
		if (!Artifact.bInstalled || !FileMatchesDigest(Artifact.TargetPath, Artifact.InstalledContentDigest))
		{
			return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Committed installed digest does not match."));
		}
	}
	return ValidateCommittedSync();
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::ValidateCommittedSync() const
{
	if (!Current->bSyncInstalled)
	{
		return MakeJournalResult(true);
	}
	if (!SyncValidate)
	{
		return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Committed sync validation authority is unavailable."));
	}
	if (!SyncValidate(Current->RequestId, Current->TransactionId, Current->JournalId).bValid)
	{
		return MakeJournalResult(false, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Committed sync identity does not match."));
	}
	return MakeJournalResult(true);
}

FAssetDocumentContractValidationResult FAssetDocumentPersistenceJournal::WriteCurrent()
{
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes, true);
	if (!SerializeRecord(Writer, Current.GetValue()))
	{
		return MakeJournalResult(false, TEXT("Persistence.PrepareJournalFailed"), TEXT("Journal record could not be serialized."));
	}
	Writer.Flush();
	FString Error;
	if (!FAssetDocumentAtomicFile::WriteBytesAtomically(GetJournalPath(Current->JournalId), Bytes, Error))
	{
		return MakeJournalResult(false, TEXT("Persistence.PrepareJournalFailed"), TEXT("Journal bytes could not be written durably."));
	}
	return MakeJournalResult(true);
}

FString FAssetDocumentPersistenceJournal::GetJournalPath(FStringView JournalId) const
{
	return IsSafeJournalId(JournalId)
		? FPaths::Combine(JournalRoot, FString(JournalId) + TEXT(".bin"))
		: FString();
}

#if WITH_DEV_AUTOMATION_TESTS
TOptional<EAssetDocumentJournalState> FAssetDocumentPersistenceJournal::GetCurrentStateForTest() const
{
	return Current.IsSet() ? TOptional<EAssetDocumentJournalState>(Current->State) : TOptional<EAssetDocumentJournalState>();
}

void FAssetDocumentPersistenceJournal::FailNextCleanupForTest()
{
	GFailNextJournalCleanup.Store(true);
}
#endif
