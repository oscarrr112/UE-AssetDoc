// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Runtime/AssetDocumentPersistenceRuntime.h"

#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentRuntimeTestHooks.h"

#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace
{
FAssetDocumentContractValidationResult ValidResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentContractValidationResult InvalidResult(const TCHAR* Code, const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Persistence");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

FAssetDocumentByteArtifact MakeByteArtifact(const FAssetDocumentPackageArtifact& Artifact)
{
	FAssetDocumentByteArtifact Result;
	Result.StagedPath = Artifact.StagedPath;
	Result.TargetPath = Artifact.TargetPath;
	Result.ContentDigest = Artifact.ContentDigest;
	return Result;
}

bool IsPackageHeader(const FAssetDocumentPackageArtifact& Artifact)
{
	const FString Extension = FPaths::GetExtension(Artifact.TargetPath, true);
	return Extension.Equals(TEXT(".uasset"), ESearchCase::IgnoreCase)
		|| Extension.Equals(TEXT(".umap"), ESearchCase::IgnoreCase);
}

int32 FindPackageHeader(const FAssetDocumentPackageArtifactSet& Package)
{
	return Package.Artifacts.IndexOfByPredicate(IsPackageHeader);
}

FAssetDocumentFreshVerificationInput MakeVerificationInput(
	const FAssetDocumentPersistencePlan& Plan,
	const FAssetDocumentPackageArtifactSet& Package,
	const FAssetDocumentByteArtifact& Sidecar,
	uint64 FreshHandleRevision)
{
	FAssetDocumentFreshVerificationInput Input;
	const int32 HeaderIndex = FindPackageHeader(Package);
	Input.StagedPackagePath = Package.Artifacts.IsValidIndex(HeaderIndex)
		? Package.Artifacts[HeaderIndex].StagedPath : FString();
	Input.StagedSidecarPath = Sidecar.StagedPath;
	Input.FreshHandleRevision = FreshHandleRevision;
	Input.ExpectedSemanticHash = Plan.Sidecar.SemanticHash;
	Input.ExpectedCodecVersion = Plan.Sidecar.CodecVersion;
	return Input;
}

FAssetDocumentContractValidationResult ValidateFreshResult(
	const FAssetDocumentFreshVerificationResult& Result,
	const FAssetDocumentPersistencePlan& Plan)
{
	if (!Result.bFreshLoadProven)
	{
		return InvalidResult(TEXT("Persistence.FreshLoadFailed"), TEXT("Staged package was not verified through a fresh load."));
	}
	if (!Result.bCanonicalMatch)
	{
		return InvalidResult(TEXT("Persistence.CanonicalMismatch"), TEXT("Freshly loaded staged package did not match canonical document semantics."));
	}
	if (Result.ActualSemanticHash != Plan.Sidecar.SemanticHash)
	{
		return InvalidResult(TEXT("Persistence.CanonicalMismatch"), TEXT("Fresh verification returned a different semantic hash."));
	}
	if (Result.ActualCodecVersion != Plan.Sidecar.CodecVersion)
	{
		return InvalidResult(TEXT("Persistence.CanonicalMismatch"), TEXT("Fresh verification returned a different codec version."));
	}
	return ValidResult();
}

FAssetDocumentDurableCompletionReceipt MakeReceipt(
	const FAssetDocumentPersistencePlan& Plan,
	const FAssetDocumentPackageArtifactSet& Package,
	const FAssetDocumentByteArtifact& Sidecar)
{
	FAssetDocumentDurableCompletionReceipt Receipt;
	Receipt.RequestId = Plan.RequestId;
	Receipt.TransactionId = Plan.TransactionId;
	Receipt.JournalId = Plan.JournalId;
	Receipt.PackageContentDigest = Package.AggregateContentDigest;
	Receipt.SidecarContentDigest = Sidecar.ContentDigest;
	return Receipt;
}

bool SetFailure(
	FAssetDocumentPersistenceFailure& OutFailure,
	EAssetDocumentPersistenceStep Step,
	const FAssetDocumentContractValidationResult& Validation,
	const TCHAR* FallbackCode,
	const TCHAR* FallbackMessage)
{
	OutFailure.Step = Step;
	if (!Validation.Diagnostics.IsEmpty())
	{
		OutFailure.Diagnostic = Validation.Diagnostics[0];
	}
	else
	{
		OutFailure.Diagnostic.Path = TEXT("/Persistence");
		OutFailure.Diagnostic.Code = FallbackCode;
		OutFailure.Diagnostic.Message = FallbackMessage;
	}
	return false;
}

FString CanonicalPersistencePath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(MoveTemp(Path));
	FPaths::NormalizeFilename(Path);
	return Path;
}

bool HasPersistenceContext(const FAssetDocumentPersistenceDependencies& Dependencies)
{
	return !Dependencies.JournalBackupRoot.IsEmpty() && Dependencies.FreshHandleRevision != 0;
}

bool HasMatchingRequestAuthority(
	const FAssetDocumentPersistencePlan& Plan,
	const FAssetDocumentPersistenceDependencies& Dependencies)
{
	return Plan.RequestId == Dependencies.ExpectedRequestId
		&& Plan.TransactionId == Dependencies.ExpectedTransactionId
		&& Plan.JournalId == Dependencies.ExpectedJournalId;
}

bool HasMatchingTargetAuthority(
	const FAssetDocumentPersistencePlan& Plan,
	const FAssetDocumentPersistenceDependencies& Dependencies)
{
	return CanonicalPersistencePath(Plan.StagingRoot) == CanonicalPersistencePath(Dependencies.ExpectedStagingRoot)
		&& Plan.TargetPackagePath == Dependencies.ExpectedTargetPackagePath
		&& CanonicalPersistencePath(Plan.TargetSidecarPath)
			== CanonicalPersistencePath(Dependencies.ExpectedTargetSidecarPath);
}

FAssetDocumentContractValidationResult ValidatePersistenceAdmission(
	const FAssetDocumentPersistencePlan& Plan,
	const FAssetDocumentPersistenceDependencies& Dependencies)
{
	const FAssetDocumentContractValidationResult PlanValidation = ValidateAssetDocumentPersistencePlan(Plan);
	if (!PlanValidation.bValid)
	{
		return PlanValidation;
	}
	if (!HasPersistenceContext(Dependencies))
	{
		return InvalidResult(TEXT("Persistence.InvalidRequestContext"),
			TEXT("Persistence requires a backup root and fresh handle revision."));
	}
	if (!HasMatchingRequestAuthority(Plan, Dependencies))
	{
		return InvalidResult(TEXT("Persistence.InvalidRequestContext"),
			TEXT("Persistence request identity does not match its bound authority."));
	}
	if (!HasMatchingTargetAuthority(Plan, Dependencies))
	{
		return InvalidResult(TEXT("Persistence.InvalidRequestContext"),
			TEXT("Persistence targets do not match their bound authority."));
	}
	return ValidResult();
}
}

struct FAssetDocumentPersistenceWork
{
	FAssetDocumentPackageArtifactSet Package;
	FAssetDocumentByteArtifact Sidecar;
	FAssetDocumentPersistenceJournalRecord Journal;
};

FAssetDocumentDurableCompletionReceipt MakeInvalidDurableReceipt()
{
	return {};
}

FAssetDocumentPersistenceCoordinatorRuntime::FAssetDocumentPersistenceCoordinatorRuntime(
	FAssetDocumentPersistenceDependencies InDependencies)
	: Dependencies(MoveTemp(InDependencies))
{
}

FAssetDocumentDurableCompletionReceipt FAssetDocumentPersistenceCoordinatorRuntime::Persist(
	const FAssetDocumentPersistencePlan& Plan,
	IAssetDocumentStagedArtifactVerifier& Verifier)
{
	FAssetDocumentPersistenceFailure Failure;
	FAssetDocumentPersistenceWork Work;
	ON_SCOPE_EXIT
	{
		Dependencies.PackageStager.CleanupStaged(Work.Package, Plan.StagingRoot);
		Dependencies.SidecarByteIo.CleanupStaged(Work.Sidecar, Plan.StagingRoot);
	};
	if (!BuildVerifiedStaging(Plan, Verifier, Work, Failure))
	{
		return Fail(Failure);
	}
	if (!PrepareRecoveryState(Plan, Work, Failure))
	{
		return Fail(Failure);
	}
	if (!InstallArtifacts(Work, Failure))
	{
		return FailAndRecover(Failure.Step, InvalidResult(
			*Failure.Diagnostic.Code, *Failure.Diagnostic.Message), Plan);
	}
	return AdvanceAndCommit(Plan, Work);
}

bool FAssetDocumentPersistenceCoordinatorRuntime::BuildVerifiedStaging(
	const FAssetDocumentPersistencePlan& Plan,
	IAssetDocumentStagedArtifactVerifier& Verifier,
	FAssetDocumentPersistenceWork& Work,
	FAssetDocumentPersistenceFailure& OutFailure) const
{
	if (!ValidateAndRecover(Plan, OutFailure))
	{
		return false;
	}
	if (!StageArtifacts(Plan, Work, OutFailure))
	{
		return false;
	}
	return VerifyStaged(Plan, Verifier, Work, OutFailure);
}

bool FAssetDocumentPersistenceCoordinatorRuntime::PrepareRecoveryState(
	const FAssetDocumentPersistencePlan& Plan,
	FAssetDocumentPersistenceWork& Work,
	FAssetDocumentPersistenceFailure& OutFailure) const
{
	if (!CaptureBaselines(Work, OutFailure))
	{
		return false;
	}
	return PrepareJournal(Plan, Work, OutFailure);
}

bool FAssetDocumentPersistenceCoordinatorRuntime::InstallArtifacts(
	FAssetDocumentPersistenceWork& Work,
	FAssetDocumentPersistenceFailure& OutFailure) const
{
	if (!InstallPackage(Work, OutFailure))
	{
		return false;
	}
	return InstallSidecar(Work, OutFailure);
}

bool FAssetDocumentPersistenceCoordinatorRuntime::ValidateAndRecover(
	const FAssetDocumentPersistencePlan& Plan,
	FAssetDocumentPersistenceFailure& OutFailure) const
{
	const FAssetDocumentContractValidationResult Admission = ValidatePersistenceAdmission(Plan, Dependencies);
	if (!Admission.bValid)
	{
		return SetFailure(OutFailure, EAssetDocumentPersistenceStep::StagePackage, Admission,
			TEXT("Persistence.InvalidPlan"), TEXT("Persistence plan validation failed."));
	}
	RecordEvent(TEXT("RecoverIncomplete"));
	const FAssetDocumentJournalRecoveryResult Recovery = Dependencies.Journal.RecoverIncomplete(Plan.JournalId);
	if (!Recovery.bMayStartNewPersistence)
	{
		return SetFailure(OutFailure, EAssetDocumentPersistenceStep::Recover,
			Recovery.Validation, TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Incomplete persistence could not be recovered."));
	}
	RecordRecoveryDiagnostic(Recovery);
	return true;
}

bool FAssetDocumentPersistenceCoordinatorRuntime::StageArtifacts(
	const FAssetDocumentPersistencePlan& Plan,
	FAssetDocumentPersistenceWork& Work,
	FAssetDocumentPersistenceFailure& OutFailure) const
{
	RecordEvent(TEXT("StagePackage"));
	FAssetDocumentContractValidationResult Result = Dependencies.PackageStager.Stage(Plan, Work.Package);
	if (!Result.bValid)
	{
		return SetFailure(OutFailure, EAssetDocumentPersistenceStep::StagePackage, Result,
			TEXT("Persistence.StagePackageFailed"), TEXT("Package staging failed."));
	}
	RecordEvent(TEXT("StageSidecar"));
	Result = Dependencies.SidecarByteIo.Stage(Plan, Work.Sidecar);
	return Result.bValid || SetFailure(OutFailure, EAssetDocumentPersistenceStep::StageSidecar, Result,
		TEXT("Persistence.StageSidecarFailed"), TEXT("Sidecar staging failed."));
}

bool FAssetDocumentPersistenceCoordinatorRuntime::VerifyStaged(
	const FAssetDocumentPersistencePlan& Plan,
	IAssetDocumentStagedArtifactVerifier& Verifier,
	FAssetDocumentPersistenceWork& Work,
	FAssetDocumentPersistenceFailure& OutFailure) const
{
	RecordEvent(TEXT("InvalidatePackageReload"));
	FAssetDocumentHandleInvalidation Invalidation;
	Invalidation.Reason = EAssetDocumentHandleInvalidationReason::PackageReload;
	Invalidation.RequestId = Plan.RequestId;
	Invalidation.TransactionId = Plan.TransactionId;
	Invalidation.RuntimeRevision = Dependencies.FreshHandleRevision;
	Dependencies.Handles.Invalidate(Invalidation);
	RecordEvent(TEXT("VerifyFresh"));
	const FAssetDocumentFreshVerificationResult Verification = Verifier.VerifyFresh(
		MakeVerificationInput(Plan, Work.Package, Work.Sidecar, Dependencies.FreshHandleRevision));
	RecordEvent(TEXT("VerifyCanonical"));
	const FAssetDocumentContractValidationResult Result = ValidateFreshResult(Verification, Plan);
	if (!Result.bValid)
	{
		SetFailure(OutFailure, EAssetDocumentPersistenceStep::FreshLoadStagedPackage, Result,
			TEXT("Persistence.FreshVerificationFailed"), TEXT("Fresh verification failed."));
		// Keep verifier details separate from the stable primary failure diagnostic.
		OutFailure.VerifierDiagnostics = Verification.Diagnostics;
		return false;
	}
	if (Dependencies.Diagnostics)
	{
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Verification.Diagnostics)
		{
			Dependencies.Diagnostics(EAssetDocumentPersistenceStep::FreshLoadStagedPackage, Diagnostic);
		}
	}
	return true;
}

bool FAssetDocumentPersistenceCoordinatorRuntime::CaptureBaselines(
	FAssetDocumentPersistenceWork& Work,
	FAssetDocumentPersistenceFailure& OutFailure) const
{
	RecordEvent(TEXT("CapturePackageInstalledBaseline"));
	FAssetDocumentContractValidationResult Result = Dependencies.PackageStager.CaptureInstalledBaseline(
		Work.Package, Dependencies.JournalBackupRoot, Work.Journal.Artifacts);
	if (!Result.bValid)
	{
		return SetFailure(OutFailure, EAssetDocumentPersistenceStep::PrepareJournal, Result,
			TEXT("Persistence.PrepareJournalFailed"), TEXT("Package baseline capture failed."));
	}
	FAssetDocumentJournalArtifact SidecarJournal;
	RecordEvent(TEXT("CaptureSidecarInstalledBaseline"));
	Result = Dependencies.SidecarByteIo.CaptureInstalledBaseline(Work.Sidecar,
		FPaths::Combine(Dependencies.JournalBackupRoot, TEXT("sidecar.bin")), SidecarJournal);
	if (Result.bValid)
	{
		Work.Journal.Artifacts.Add(MoveTemp(SidecarJournal));
	}
	return Result.bValid || SetFailure(OutFailure, EAssetDocumentPersistenceStep::PrepareJournal, Result,
		TEXT("Persistence.PrepareJournalFailed"), TEXT("Sidecar baseline capture failed."));
}

bool FAssetDocumentPersistenceCoordinatorRuntime::PrepareJournal(
	const FAssetDocumentPersistencePlan& Plan,
	FAssetDocumentPersistenceWork& Work,
	FAssetDocumentPersistenceFailure& OutFailure) const
{
	TOptional<FAssetDocumentTypedSyncStateRecord> SyncBaseline;
	RecordEvent(TEXT("LoadTypedSyncBaseline"));
	FAssetDocumentContractValidationResult Result = Dependencies.TypedStore.Load(Dependencies.SyncKey, SyncBaseline,
		Work.Journal.SyncPreimageBytes, Work.Journal.bSyncBaselineExisted);
	if (!Result.bValid)
	{
		return SetFailure(OutFailure, EAssetDocumentPersistenceStep::PrepareJournal, Result,
			TEXT("Persistence.PrepareJournalFailed"), TEXT("Typed sync baseline capture failed."));
	}
	Work.Journal.RequestId = Plan.RequestId;
	Work.Journal.TransactionId = Plan.TransactionId;
	Work.Journal.JournalId = Plan.JournalId;
	for (FAssetDocumentJournalArtifact& Artifact : Work.Journal.Artifacts)
	{
		Artifact.TargetPath = FPaths::ConvertRelativePathToFull(Artifact.TargetPath);
		Artifact.BackupPath = FPaths::ConvertRelativePathToFull(Artifact.BackupPath);
		FPaths::NormalizeFilename(Artifact.TargetPath);
		FPaths::NormalizeFilename(Artifact.BackupPath);
	}
	if (Work.Journal.bSyncBaselineExisted)
	{
		Work.Journal.SyncPreimageContentDigest = ComputeAssetDocumentContentDigest(Work.Journal.SyncPreimageBytes);
	}
	RecordEvent(TEXT("PrepareJournal"));
	Result = Dependencies.Journal.Prepare(Work.Journal);
	return Result.bValid || SetFailure(OutFailure, EAssetDocumentPersistenceStep::PrepareJournal, Result,
		TEXT("Persistence.PrepareJournalFailed"), TEXT("Persistence journal preparation failed."));
}

bool FAssetDocumentPersistenceCoordinatorRuntime::InstallPackage(
	FAssetDocumentPersistenceWork& Work,
	FAssetDocumentPersistenceFailure& OutFailure) const
{
	const int32 HeaderIndex = FindPackageHeader(Work.Package);
	if (!Work.Package.Artifacts.IsValidIndex(HeaderIndex))
	{
		return SetFailure(OutFailure, EAssetDocumentPersistenceStep::AtomicInstall,
			InvalidResult(TEXT("Persistence.MissingPackageHeader"), TEXT("Staged package did not include a package header.")),
			TEXT("Persistence.MissingPackageHeader"), TEXT("Staged package header is missing."));
	}
	RecordEvent(TEXT("InstallPackageHeader"));
	const bool bHeaderInstalled = InstallPackageArtifact(HeaderIndex, Work, OutFailure);
	RecordEvent(TEXT("InstallPackageCompanions"));
	if (!bHeaderInstalled)
	{
		return false;
	}
	return InstallPackageCompanions(HeaderIndex, Work, OutFailure);
}

bool FAssetDocumentPersistenceCoordinatorRuntime::InstallPackageArtifact(
	int32 ArtifactIndex,
	FAssetDocumentPersistenceWork& Work,
	FAssetDocumentPersistenceFailure& OutFailure) const
{
	FAssetDocumentContractValidationResult Result = Dependencies.Journal.MarkArtifactInstalled(ArtifactIndex);
	if (Result.bValid)
	{
		Result = Dependencies.SidecarByteIo.Install(MakeByteArtifact(Work.Package.Artifacts[ArtifactIndex]));
	}
	if (Result.bValid)
	{
		return true;
	}
	return SetFailure(OutFailure, EAssetDocumentPersistenceStep::AtomicInstall,
		InvalidResult(TEXT("Persistence.AtomicInstallFailed"), TEXT("Package artifact installation failed.")),
		TEXT("Persistence.AtomicInstallFailed"), TEXT("Package artifact installation failed."));
}

bool FAssetDocumentPersistenceCoordinatorRuntime::InstallPackageCompanions(
	int32 HeaderIndex,
	FAssetDocumentPersistenceWork& Work,
	FAssetDocumentPersistenceFailure& OutFailure) const
{
	for (int32 Index = 0; Index < Work.Package.Artifacts.Num(); ++Index)
	{
		if (Index == HeaderIndex)
		{
			continue;
		}
		if (!InstallPackageArtifact(Index, Work, OutFailure))
		{
			return false;
		}
	}
	return true;
}

bool FAssetDocumentPersistenceCoordinatorRuntime::InstallSidecar(
	FAssetDocumentPersistenceWork& Work,
	FAssetDocumentPersistenceFailure& OutFailure) const
{
	RecordEvent(TEXT("InstallSidecar"));
	FAssetDocumentContractValidationResult Result = Dependencies.Journal.MarkArtifactInstalled(Work.Package.Artifacts.Num());
	if (Result.bValid)
	{
		Result = Dependencies.SidecarByteIo.Install(Work.Sidecar);
	}
	if (Result.bValid)
	{
		return true;
	}
	return SetFailure(OutFailure, EAssetDocumentPersistenceStep::AtomicInstall,
		InvalidResult(TEXT("Persistence.AtomicInstallFailed"), TEXT("Sidecar installation failed.")),
		TEXT("Persistence.AtomicInstallFailed"), TEXT("Sidecar installation failed."));
}

FAssetDocumentDurableCompletionReceipt FAssetDocumentPersistenceCoordinatorRuntime::AdvanceAndCommit(
	const FAssetDocumentPersistencePlan& Plan,
	FAssetDocumentPersistenceWork& Work) const
{
	FAssetDocumentDurableCompletionReceipt Receipt = MakeReceipt(Plan, Work.Package, Work.Sidecar);
	FAssetDocumentContractValidationResult Result = Dependencies.Journal.MarkSyncInstalled();
	if (Result.bValid)
	{
		RecordEvent(TEXT("AdvanceSync"));
		Result = Dependencies.Sync.AdvanceAfterDurableCompletion(Dependencies.InstalledEvidence, Receipt);
	}
	if (!Result.bValid)
	{
		return FailAndRecover(EAssetDocumentPersistenceStep::AtomicInstall, Result, Plan);
	}
	Receipt.bSyncStateAdvanced = true;
	RecordEvent(TEXT("MarkDurableComplete"));
	Result = Dependencies.Journal.MarkDurableComplete();
	if (!Result.bValid)
	{
		return FailAndRecover(EAssetDocumentPersistenceStep::DurableComplete, Result, Plan);
	}
	RecordEvent(TEXT("Cleanup"));
	Result = Dependencies.Journal.Cleanup();
	if (!Result.bValid)
	{
		Fail(EAssetDocumentPersistenceStep::DurableComplete, Result,
			TEXT("Persistence.CleanupDeferred"), TEXT("Durable persistence cleanup was deferred."));
	}
	return Receipt;
}

void FAssetDocumentPersistenceCoordinatorRuntime::RecordEvent(FStringView Event) const
{
	FAssetDocumentRuntimeTestHooks::ObservePersistenceEvent(Event);
	if (Dependencies.Events)
	{
		Dependencies.Events(Event);
	}
}

FAssetDocumentDurableCompletionReceipt FAssetDocumentPersistenceCoordinatorRuntime::Fail(
	const FAssetDocumentPersistenceFailure& Failure) const
{
	if (Dependencies.Diagnostics)
	{
		Dependencies.Diagnostics(Failure.Step, Failure.Diagnostic);
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Failure.VerifierDiagnostics)
		{
			Dependencies.Diagnostics(Failure.Step, Diagnostic);
		}
	}
	return MakeInvalidDurableReceipt();
}

FAssetDocumentDurableCompletionReceipt FAssetDocumentPersistenceCoordinatorRuntime::Fail(
	EAssetDocumentPersistenceStep Step,
	const FAssetDocumentContractValidationResult& Validation,
	const TCHAR* FallbackCode,
	const TCHAR* FallbackMessage) const
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	if (!Validation.Diagnostics.IsEmpty())
	{
		Diagnostic = Validation.Diagnostics[0];
	}
	else
	{
		Diagnostic.Path = TEXT("/Persistence");
		Diagnostic.Code = FallbackCode;
		Diagnostic.Message = FallbackMessage;
	}
	if (Dependencies.Diagnostics)
	{
		Dependencies.Diagnostics(Step, Diagnostic);
	}
	return MakeInvalidDurableReceipt();
}

FAssetDocumentDurableCompletionReceipt FAssetDocumentPersistenceCoordinatorRuntime::FailAndRecover(
	EAssetDocumentPersistenceStep Step,
	const FAssetDocumentContractValidationResult& Validation,
	const FAssetDocumentPersistencePlan& Plan) const
{
	const FAssetDocumentDurableCompletionReceipt Invalid = Fail(Step, Validation,
		TEXT("Persistence.AtomicInstallFailed"), TEXT("Atomic persistence failed before the durable commit point."));
	const FAssetDocumentJournalRecoveryResult Recovery = Dependencies.Journal.RecoverIncomplete(Plan.JournalId);
	if (Recovery.bMayStartNewPersistence)
	{
		RecordRecoveryDiagnostic(Recovery);
	}
	else
	{
		Fail(EAssetDocumentPersistenceStep::Recover, Recovery.Validation,
			TEXT("Atomic.Poisoned.RecoveryFailed"), TEXT("Persistence rollback failed and poisoned the transaction."));
	}
	return Invalid;
}

void FAssetDocumentPersistenceCoordinatorRuntime::RecordRecoveryDiagnostic(
	const FAssetDocumentJournalRecoveryResult& Recovery) const
{
	if (!Recovery.Validation.Diagnostics.IsEmpty() && Dependencies.Diagnostics)
	{
		Dependencies.Diagnostics(EAssetDocumentPersistenceStep::Recover, Recovery.Validation.Diagnostics[0]);
	}
}
