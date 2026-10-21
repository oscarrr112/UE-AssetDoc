// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentAtomicFile.h"
#include "Tests/AssetDocumentAtomicTestMount.h"
#include "Runtime/AssetDocumentAtomicTransactionRuntime.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentPersistenceRuntime.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include "Containers/StringConv.h"
#include <unistd.h>
#endif

#if WITH_DEV_AUTOMATION_TESTS

namespace FreshVerificationBeforeCommitTestsLocal
{
FAssetDocumentContractValidationResult ValidResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentContractValidationResult InvalidResult(const TCHAR* Code)
{
	FAssetDocumentContractValidationResult Result;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/TestDouble");
	Diagnostic.Code = Code;
	Diagnostic.Message = TEXT("Injected persistence failure.");
	return Result;
}

bool WriteBytes(const FString& Path, TConstArrayView<uint8> Bytes)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

class FPackageBackendRecorder final : public IAssetDocumentPackageSaveBackend
{
public:
	explicit FPackageBackendRecorder(FString InRoot, int32 InViolation)
		: Root(MoveTemp(InRoot)), Violation(InViolation) {}

	virtual FAssetDocumentContractValidationResult SaveToStaging(
		const FString& StagedHeaderPath,
		const FString& TargetHeaderPath,
		FAssetDocumentPackageArtifactSet& OutArtifacts) override
	{
		++SaveCount;
		static const TArray<uint8> PackageBytes{0x50, 0x4b, 0x47};
		if (!WriteBytes(StagedHeaderPath, PackageBytes))
		{
			return InvalidResult(TEXT("Test.StageWriteFailed"));
		}
		FAssetDocumentPackageArtifact& Artifact = OutArtifacts.Artifacts.AddDefaulted_GetRef();
		Artifact.StagedPath = Violation == 3
			? FPaths::Combine(Root, TEXT("Outside/Asset.uasset")) : StagedHeaderPath;
		Artifact.TargetPath = Violation == 1
			? FPaths::Combine(Root, TEXT("Installed/Other.uasset")) : TargetHeaderPath;
		Artifact.ContentDigest = ComputeAssetDocumentContentDigest(PackageBytes);
		if (Violation == 2)
		{
			FAssetDocumentPackageArtifact& Companion = OutArtifacts.Artifacts.AddDefaulted_GetRef();
			Companion.StagedPath = FPaths::Combine(FPaths::GetPath(StagedHeaderPath), TEXT("Asset.uexp"));
			Companion.TargetPath = FPaths::Combine(Root, TEXT("Installed/Other.uexp"));
			Companion.ContentDigest = Artifact.ContentDigest;
			WriteBytes(Companion.StagedPath, PackageBytes);
		}
		return ValidResult();
	}

	int32 SaveCount = 0;

private:
	FString Root;
	int32 Violation = 0;
};

class FTypedStoreRecorder final : public IAssetDocumentTypedSyncStateStore
{
public:
	bool bLoadSucceeds = true;
	int32 RestoreCount = 0;

	virtual FAssetDocumentContractValidationResult Load(
		const FAssetDocumentSyncStateKey& Key,
		TOptional<FAssetDocumentTypedSyncStateRecord>& OutRecord,
		TArray<uint8>& OutEncodedBytes,
		bool& bOutExisted) override
	{
		OutRecord.Reset();
		OutEncodedBytes.Reset();
		bOutExisted = false;
		return bLoadSucceeds ? ValidResult() : InvalidResult(TEXT("Persistence.PrepareJournalFailed"));
	}

	virtual FAssetDocumentContractValidationResult Store(
		const FAssetDocumentSyncStateKey& Key,
		const FAssetDocumentTypedSyncStateRecord& Record) override
	{
		return ValidResult();
	}

	virtual FAssetDocumentContractValidationResult Restore(
		const FAssetDocumentSyncStateKey& Key,
		TArrayView64<const uint8> EncodedPreimage,
		bool bExisted) override
	{
		++RestoreCount;
		return ValidResult();
	}
};

class FSyncRecorder final : public IAssetDocumentSyncCoordinator
{
public:
	bool bAdvanceSucceeds = true;
	bool bSawProvisionalReceipt = false;

	virtual FAssetDocumentSyncDecision Compare(const FAssetDocumentSyncComparisonInput& Input) const override
	{
		return {};
	}

	virtual FAssetDocumentContractValidationResult AdvanceAfterDurableCompletion(
		const FAssetDocumentSyncEvidencePair& Installed,
		const FAssetDocumentDurableCompletionReceipt& Receipt) override
	{
		bSawProvisionalReceipt = Receipt.RequestId.IsValid() && !Receipt.bSyncStateAdvanced;
		return bAdvanceSucceeds ? ValidResult() : InvalidResult(TEXT("Test.SyncAdvanceFailed"));
	}
};

class FRollbackParticipant final : public IAssetDocumentRollbackParticipant
{
public:
	virtual FStringView GetRollbackComponentId() const override { return TEXT("package"); }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) override
	{
		++RollbackCount;
		return ValidResult();
	}
	int32 RollbackCount = 0;
};

class FParticipantRegistry final : public IAssetDocumentRollbackParticipantRegistry
{
public:
	explicit FParticipantRegistry(FRollbackParticipant& InParticipant) : Participant(InParticipant) {}
	virtual IAssetDocumentRollbackParticipant* Resolve(FStringView RollbackComponentId) override
	{
		return RollbackComponentId == Participant.GetRollbackComponentId() ? &Participant : nullptr;
	}
private:
	FRollbackParticipant& Participant;
};

class FHandleRecorder final : public IAssetDocumentHandleResolver
{
public:
	TArray<FAssetDocumentHandleInvalidation> Invalidations;

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override { return {}; }
	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) override { return {}; }
	virtual FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation& Event) override
	{
		Invalidations.Add(Event);
		return {};
	}
};

enum class EVerificationFailure : uint8
{
	None,
	Fresh,
	Canonical,
	Hash,
	Codec,
};

class FVerifierRecorder final : public IAssetDocumentStagedArtifactVerifier
{
public:
	EVerificationFailure Failure = EVerificationFailure::None;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;

	virtual FAssetDocumentFreshVerificationResult VerifyFresh(
		const FAssetDocumentFreshVerificationInput& Input) override
	{
		LastInput = Input;
		FAssetDocumentFreshVerificationResult Result;
		Result.bFreshLoadProven = Failure != EVerificationFailure::Fresh;
		Result.bCanonicalMatch = Failure != EVerificationFailure::Canonical;
		Result.ActualSemanticHash = Failure == EVerificationFailure::Hash ? TEXT("wrong") : Input.ExpectedSemanticHash;
		Result.ActualCodecVersion = Failure == EVerificationFailure::Codec ? 99 : Input.ExpectedCodecVersion;
		Result.Diagnostics = Diagnostics;
		return Result;
	}

	FAssetDocumentFreshVerificationInput LastInput;
};

struct FRunResult
{
	FAssetDocumentDurableCompletionReceipt Receipt;
	TArray<FString> Calls;
	TArray<FAssetDocumentPersistenceFailure> Failures;
	TArray<FAssetDocumentHandleInvalidation> Invalidations;
	bool bSawProvisionalReceipt = false;
	int32 BackendSaveCount = 0;
};

enum class EPersistenceInjection : uint8
{
	None,
	DurableMark,
	Cleanup,
	RecoveryCleanup,
};

FAssetDocumentRegionEvidence MakeEvidence(const FString& Hash, uint32 CodecVersion)
{
	FAssetDocumentRegionEvidence Evidence;
	Evidence.Kind = EAssetDocumentEvidenceKind::Present;
	Evidence.SemanticHash = Hash;
	Evidence.CodecVersion = CodecVersion;
	return Evidence;
}

FAssetDocumentPersistencePlan MakePlan(const FString& Root)
{
	FAssetDocumentPersistencePlan Plan;
	Plan.RequestId = FGuid::NewGuid();
	Plan.TransactionId = FGuid::NewGuid();
	// Under the test's one content mount (AssetDocumentAtomicTestMount.h: no registry scan races
	// the placeholder packages).
	Plan.TargetPackagePath = FAssetDocumentAtomicTestMount::PackageNameFor(Root);
	Plan.TargetSidecarPath = FPaths::Combine(Root, TEXT("Installed/Asset.assetdoc"));
	Plan.StagingRoot = FPaths::Combine(Root, TEXT("Staging"));
	Plan.JournalId = TEXT("fresh-before-commit");
	Plan.Sidecar.Utf8Bytes = {0x01, 0x02};
	Plan.Sidecar.SemanticHash = TEXT("semantic:v1");
	Plan.Sidecar.CodecVersion = 3;
	FAssetDocumentMutationFootprint& Item = Plan.Footprint.AddDefaulted_GetRef();
	Item.Kind = EAssetDocumentMutationFootprintKind::Package;
	Item.StableId = TEXT("/Game/Atomic/Asset");
	Item.RollbackComponentId = TEXT("package");
	return Plan;
}

FRunResult RunPersistenceCase(
	const FString& Root,
	FVerifierRecorder& Verifier,
	FTypedStoreRecorder& TypedStore,
	bool bBlockPackageBaseline = false,
	bool bBlockSidecarBaseline = false,
	EPersistenceInjection Injection = EPersistenceInjection::None,
	TOptional<FAssetDocumentPersistencePlan> PlanOverride = {},
	TOptional<FAssetDocumentPersistencePlan> AuthorityOverride = {},
	int32 PackageViolation = 0)
{
	FRunResult Out;
	const FString JournalRoot = FPaths::Combine(Root, TEXT("Journal"));
	const FString BackupRoot = FPaths::Combine(JournalRoot, TEXT("Backups"));
	IFileManager::Get().MakeDirectory(*JournalRoot, true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(Root, TEXT("Installed")), true);
	if (bBlockPackageBaseline)
	{
		WriteBytes(BackupRoot, TArray<uint8>{0x42});
	}
	if (bBlockSidecarBaseline)
	{
		IFileManager::Get().MakeDirectory(*FPaths::Combine(BackupRoot, TEXT("sidecar.bin")), true);
		WriteBytes(FPaths::Combine(Root, TEXT("Installed/Asset.assetdoc")), TArray<uint8>{0x10});
	}

	FPackageBackendRecorder Backend(Root, PackageViolation);
	FAssetDocumentPackageStager PackageStager(Backend);
	FAssetDocumentSidecarByteIo Sidecar;
	const FAssetDocumentSyncStateKey SyncKey{TEXT("/Game/Atomic/Asset"), TEXT("Body")};
	FAssetDocumentPersistenceJournal Journal(
		JournalRoot,
		[&TypedStore, SyncKey](TArrayView64<const uint8> Bytes, bool bExisted)
		{
			return TypedStore.Restore(SyncKey, Bytes, bExisted);
		},
		[](const FGuid& RequestId, const FGuid& TransactionId, FStringView JournalId)
		{
			return ValidResult();
		});
	FSyncRecorder Sync;
	FHandleRecorder Handles;
	FAssetDocumentSyncEvidencePair Installed;
	Installed.Asset = MakeEvidence(TEXT("asset:v1"), 3);
	Installed.Sidecar = MakeEvidence(TEXT("semantic:v1"), 3);
	const FAssetDocumentPersistencePlan Plan = PlanOverride.IsSet() ? PlanOverride.GetValue() : MakePlan(Root);
	const FAssetDocumentPersistencePlan Authority = AuthorityOverride.IsSet() ? AuthorityOverride.GetValue() : Plan;
	FAssetDocumentPersistenceDependencies Dependencies{
		PackageStager, Sidecar, Journal, TypedStore, Sync, Handles, SyncKey, Installed, BackupRoot, 7,
		Authority.RequestId, Authority.TransactionId, Authority.JournalId, Authority.StagingRoot,
		Authority.TargetPackagePath, Authority.TargetSidecarPath,
		[&Out, Injection](FStringView Event)
		{
			Out.Calls.Add(FString(Event));
			if (Injection == EPersistenceInjection::DurableMark && Event == TEXT("MarkDurableComplete"))
			{
				FAssetDocumentAtomicFile::FailNextWriteAtForTest(EAssetDocumentAtomicFileFailurePoint::Flush);
			}
			if (Injection == EPersistenceInjection::Cleanup && Event == TEXT("Cleanup"))
			{
				FAssetDocumentPersistenceJournal::FailNextCleanupForTest();
			}
		},
		[&Out](EAssetDocumentPersistenceStep Step, const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			Out.Failures.Add({Step, Diagnostic});
		}};
	FAssetDocumentPersistenceCoordinatorRuntime Runtime(MoveTemp(Dependencies));
	if (Injection == EPersistenceInjection::RecoveryCleanup)
	{
		FAssetDocumentPersistenceJournal::FailNextCleanupForTest();
	}
	Out.Receipt = Runtime.Persist(Plan, Verifier);
	Out.Invalidations = Handles.Invalidations;
	Out.bSawProvisionalReceipt = Sync.bSawProvisionalReceipt;
	Out.BackendSaveCount = Backend.SaveCount;
	return Out;
}

FAssetDocumentPreflightReceipt MakePreflight(const FAssetDocumentPersistencePlan& Plan)
{
	FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = Plan.RequestId;
	Receipt.RuntimeRevision = 7;
	Receipt.CompleteFootprint = Plan.Footprint;
	Receipt.ReceiptHash = TEXT("sha256:preflight");
	Receipt.bSucceeded = true;
	return Receipt;
}

bool StopsBeforeBaselineCapture(const FRunResult& Result)
{
	return Result.Calls.Contains(TEXT("VerifyCanonical"))
		&& !Result.Calls.Contains(TEXT("CapturePackageInstalledBaseline"))
		&& !Result.Receipt.RequestId.IsValid();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentFreshVerificationBeforeCommitTest,
	"UEAssetDoc.AtomicFramework.FreshVerificationBeforeCommit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFreshVerificationBeforeCommitTest::RunTest(const FString& Parameters)
{
	using namespace FreshVerificationBeforeCommitTestsLocal;
	// Whether the corrupt-package probes reach the linker (and log these) depends on loader state left by
	// earlier tests in the same process, so suppress them without requiring an occurrence (-1).
	AddExpectedMessage(TEXT("Failed to read package file summary"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedError(TEXT("Package is unloadable"), EAutomationExpectedErrorFlags::Contains, -1);
	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentAtomicTests"), FGuid::NewGuid().ToString());
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const FAssetDocumentAtomicTestMount Mount(Root);
	FVerifierRecorder Verifier;
	FTypedStoreRecorder TypedStore;
	const FRunResult Success = RunPersistenceCase(FPaths::Combine(Root, TEXT("Success")), Verifier, TypedStore);
	const TArray<FString> ExpectedCalls = {
		TEXT("RecoverIncomplete"), TEXT("StagePackage"), TEXT("StageSidecar"), TEXT("InvalidatePackageReload"),
		TEXT("VerifyFresh"), TEXT("VerifyCanonical"), TEXT("CapturePackageInstalledBaseline"),
		TEXT("CaptureSidecarInstalledBaseline"), TEXT("LoadTypedSyncBaseline"), TEXT("PrepareJournal"),
		TEXT("InstallPackageHeader"), TEXT("InstallPackageCompanions"), TEXT("InstallSidecar"), TEXT("AdvanceSync"),
		TEXT("MarkDurableComplete"), TEXT("Cleanup")};
	TestTrue(TEXT("success persistence returns durable receipt and exact call order"),
		Success.Receipt.RequestId.IsValid() && Success.Receipt.TransactionId.IsValid()
		&& Success.Receipt.bSyncStateAdvanced && Success.Calls == ExpectedCalls
		&& Success.Failures.IsEmpty() && Success.bSawProvisionalReceipt
		&& Success.Invalidations.Num() == 1
		&& Success.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::PackageReload
		&& Verifier.LastInput.FreshHandleRevision == 7);

	const auto CheckAuthorityMismatch = [this, &Root](
		const TCHAR* Label,
		TFunction<void(FAssetDocumentPersistencePlan&)> Mutate)
	{
		const FString CaseRoot = FPaths::Combine(Root, Label);
		const FAssetDocumentPersistencePlan Authority = MakePlan(CaseRoot);
		FAssetDocumentPersistencePlan Requested = Authority;
		Mutate(Requested);
		FVerifierRecorder CaseVerifier;
		FTypedStoreRecorder CaseStore;
		const FRunResult Result = RunPersistenceCase(
			CaseRoot, CaseVerifier, CaseStore, false, false, EPersistenceInjection::None, Requested, Authority);
		TestTrue(*FString::Printf(TEXT("authority mismatch is rejected before recovery: %s"), Label),
			!Result.Receipt.RequestId.IsValid() && Result.Calls.IsEmpty() && Result.Failures.Num() == 1);
	};
	CheckAuthorityMismatch(TEXT("RequestMismatch"), [](FAssetDocumentPersistencePlan& Plan) { Plan.RequestId = FGuid::NewGuid(); });
	CheckAuthorityMismatch(TEXT("TransactionMismatch"), [](FAssetDocumentPersistencePlan& Plan) { Plan.TransactionId = FGuid::NewGuid(); });
	CheckAuthorityMismatch(TEXT("JournalMismatch"), [](FAssetDocumentPersistencePlan& Plan) { Plan.JournalId = TEXT("other-journal"); });
	CheckAuthorityMismatch(TEXT("StagingRootMismatch"), [](FAssetDocumentPersistencePlan& Plan) { Plan.StagingRoot += TEXT("-other"); });
	CheckAuthorityMismatch(TEXT("PackageContextMismatch"), [](FAssetDocumentPersistencePlan& Plan) { Plan.TargetPackagePath = TEXT("/Game/Atomic/Other"); });
	CheckAuthorityMismatch(TEXT("SidecarTargetMismatch"), [](FAssetDocumentPersistencePlan& Plan) { Plan.TargetSidecarPath += TEXT(".other"); });

#if PLATFORM_MAC || PLATFORM_LINUX
	{
		const FString CaseRoot = FPaths::Combine(Root, TEXT("StagingRootSymlinkEscape"));
		const FString RedirectedRoot = FPaths::Combine(CaseRoot, TEXT("Redirected"));
		const FString SymlinkRoot = FPaths::Combine(CaseRoot, TEXT("Staging"));
		IFileManager::Get().MakeDirectory(*RedirectedRoot, true);
		FTCHARToUTF8 RedirectedUtf8(*RedirectedRoot);
		FTCHARToUTF8 SymlinkUtf8(*SymlinkRoot);
		TestEqual(TEXT("staging-root escape fixture creates a symlink"), symlink(RedirectedUtf8.Get(), SymlinkUtf8.Get()), 0);
		ON_SCOPE_EXIT { unlink(SymlinkUtf8.Get()); };
		FAssetDocumentPersistencePlan EscapingPlan = MakePlan(CaseRoot);
		EscapingPlan.StagingRoot = SymlinkRoot;
		FVerifierRecorder CaseVerifier;
		FTypedStoreRecorder CaseStore;
		const FRunResult Result = RunPersistenceCase(
			CaseRoot, CaseVerifier, CaseStore, false, false, EPersistenceInjection::None,
			EscapingPlan, EscapingPlan);
		TestTrue(TEXT("staging-root symlink escape is rejected before backend write"),
			!Result.Receipt.RequestId.IsValid()
			&& Result.BackendSaveCount == 0
			&& !IFileManager::Get().FileExists(*FPaths::Combine(RedirectedRoot, TEXT("Asset.uasset"))));
	}
#endif

	for (int32 Violation : {1, 2, 3})
	{
		const FString CaseRoot = FPaths::Combine(Root, FString::Printf(TEXT("ArtifactAuthority-%d"), Violation));
		FVerifierRecorder CaseVerifier;
		FTypedStoreRecorder CaseStore;
		const FRunResult Result = RunPersistenceCase(
			CaseRoot, CaseVerifier, CaseStore, false, false, EPersistenceInjection::None, {}, {}, Violation);
		TestTrue(TEXT("backend artifact authority violation is rejected during package staging"),
			!Result.Receipt.RequestId.IsValid()
			&& Result.Calls == TArray<FString>{TEXT("RecoverIncomplete"), TEXT("StagePackage")}
			&& Result.Failures.Num() == 1
			&& Result.Failures[0].Diagnostic.Code == TEXT("Persistence.StagePackageFailed"));
	}

	for (EVerificationFailure Failure : {EVerificationFailure::Fresh, EVerificationFailure::Canonical,
		EVerificationFailure::Hash, EVerificationFailure::Codec})
	{
		FVerifierRecorder FailedVerifier;
		FailedVerifier.Failure = Failure;
		FTypedStoreRecorder Store;
		const FRunResult Result = RunPersistenceCase(
			FPaths::Combine(Root, FString::Printf(TEXT("Verify-%d"), static_cast<uint8>(Failure))), FailedVerifier, Store);
		const TCHAR* ExpectedCode = Failure == EVerificationFailure::Fresh
			? TEXT("Persistence.FreshLoadFailed") : TEXT("Persistence.CanonicalMismatch");
		TestTrue(TEXT("fresh and canonical verification failures stop before baseline capture"),
			StopsBeforeBaselineCapture(Result) && Result.Failures.Num() == 1
			&& Result.Failures[0].Step == EAssetDocumentPersistenceStep::FreshLoadStagedPackage
			&& Result.Failures[0].Diagnostic.Code == ExpectedCode
			&& !Result.Failures[0].Diagnostic.Path.IsEmpty()
			&& !Result.Failures[0].Diagnostic.Message.IsEmpty());
	}

	FVerifierRecorder PackageVerifier;
	FTypedStoreRecorder PackageStore;
	const FRunResult PackageCaptureFailure = RunPersistenceCase(
		FPaths::Combine(Root, TEXT("PackageCaptureFailure")), PackageVerifier, PackageStore, true, false);
	TestTrue(TEXT("package baseline failure stops before journal preparation"),
		PackageCaptureFailure.Calls.Last() == TEXT("CapturePackageInstalledBaseline")
		&& !PackageCaptureFailure.Receipt.RequestId.IsValid());

	FVerifierRecorder SidecarVerifier;
	FTypedStoreRecorder SidecarStore;
	const FRunResult SidecarCaptureFailure = RunPersistenceCase(
		FPaths::Combine(Root, TEXT("SidecarCaptureFailure")), SidecarVerifier, SidecarStore, false, true);
	TestTrue(TEXT("sidecar baseline failure stops before journal preparation"),
		SidecarCaptureFailure.Calls.Last() == TEXT("CaptureSidecarInstalledBaseline")
		&& !SidecarCaptureFailure.Receipt.RequestId.IsValid());

	FVerifierRecorder TypedVerifier;
	FTypedStoreRecorder FailedTypedStore;
	FailedTypedStore.bLoadSucceeds = false;
	const FRunResult TypedCaptureFailure = RunPersistenceCase(
		FPaths::Combine(Root, TEXT("TypedCaptureFailure")), TypedVerifier, FailedTypedStore);
	TestTrue(TEXT("typed baseline failure stops before journal preparation"),
		TypedCaptureFailure.Calls.Last() == TEXT("LoadTypedSyncBaseline")
		&& !TypedCaptureFailure.Receipt.RequestId.IsValid()
		&& TypedCaptureFailure.Failures[0].Diagnostic.Code == TEXT("Persistence.PrepareJournalFailed"));

	{
		const FString CaseRoot = FPaths::Combine(Root, TEXT("DurableMarkFailure"));
		FAssetDocumentPersistencePlan Plan = MakePlan(CaseRoot);
		FHandleRecorder TransactionHandles;
		FRollbackParticipant Participant;
		FParticipantRegistry Registry(Participant);
		FAssetDocumentAtomicTransactionRuntime Transaction(MakePreflight(Plan), TransactionHandles, Registry, {});
		Plan.TransactionId = Transaction.GetTransactionId();
		Transaction.EnlistFootprint(Plan.Footprint);
		Transaction.MarkReadyToCommit();
		FVerifierRecorder TerminalVerifier;
		FTypedStoreRecorder TerminalStore;
		const FRunResult Failure = RunPersistenceCase(
			CaseRoot, TerminalVerifier, TerminalStore, false, false, EPersistenceInjection::DurableMark, Plan);
		const FAssetDocumentContractValidationResult RolledBack = Transaction.Rollback(TEXT("Persistence.DurableCompleteFailed"));
		TestTrue(TEXT("durable mark failure returns invalid receipt and current live transaction rolls back"),
			!Failure.Receipt.RequestId.IsValid() && RolledBack.bValid
			&& Transaction.GetState() == EAssetDocumentAtomicState::RolledBack && Participant.RollbackCount == 1);
	}

	{
		const FString CaseRoot = FPaths::Combine(Root, TEXT("CleanupRetry"));
		FAssetDocumentPersistencePlan Plan = MakePlan(CaseRoot);
		FHandleRecorder TransactionHandles;
		FRollbackParticipant Participant;
		FParticipantRegistry Registry(Participant);
		FAssetDocumentAtomicTransactionRuntime Transaction(MakePreflight(Plan), TransactionHandles, Registry, {});
		Plan.TransactionId = Transaction.GetTransactionId();
		Transaction.EnlistFootprint(Plan.Footprint);
		Transaction.MarkReadyToCommit();
		FVerifierRecorder FirstVerifier;
		FTypedStoreRecorder Store;
		const FRunResult Deferred = RunPersistenceCase(
			CaseRoot, FirstVerifier, Store, false, false, EPersistenceInjection::Cleanup, Plan);
		const FAssetDocumentContractValidationResult Committed = Transaction.Commit(Deferred.Receipt);
		FVerifierRecorder RetryVerifier;
		const FRunResult Retry = RunPersistenceCase(CaseRoot, RetryVerifier, Store);
		TestTrue(TEXT("post-mark cleanup defer keeps valid receipt and next request proceeds after cleanup"),
			Deferred.Receipt.bSyncStateAdvanced && Deferred.Failures.Num() == 1
			&& Deferred.Failures[0].Diagnostic.Code == TEXT("Persistence.CleanupDeferred")
			&& Committed.bValid && Transaction.GetState() == EAssetDocumentAtomicState::Committed
			&& Retry.Receipt.bSyncStateAdvanced && Retry.Calls.Num() > 1
			&& Retry.Calls[0] == TEXT("RecoverIncomplete") && Retry.Calls[1] == TEXT("StagePackage"));
	}

	{
		const FString CaseRoot = FPaths::Combine(Root, TEXT("CleanupStillDeferred"));
		FVerifierRecorder FirstVerifier;
		FTypedStoreRecorder Store;
		const FRunResult Prior = RunPersistenceCase(
			CaseRoot, FirstVerifier, Store, false, false, EPersistenceInjection::Cleanup);
		FVerifierRecorder CurrentVerifier;
		const FRunResult Current = RunPersistenceCase(
			CaseRoot, CurrentVerifier, Store, false, false, EPersistenceInjection::RecoveryCleanup);
		TestTrue(TEXT("repeated committed cleanup defer blocks current persistence without invalidating prior receipt"),
			Prior.Receipt.bSyncStateAdvanced && !Current.Receipt.RequestId.IsValid()
			&& Current.Calls == TArray<FString>{TEXT("RecoverIncomplete")});
	}

	{
		const FString CaseRoot = FPaths::Combine(Root, TEXT("CommittedDigestMismatch"));
		FVerifierRecorder FirstVerifier;
		FTypedStoreRecorder Store;
		const FRunResult Prior = RunPersistenceCase(
			CaseRoot, FirstVerifier, Store, false, false, EPersistenceInjection::Cleanup);
		WriteBytes(FPaths::Combine(CaseRoot, TEXT("Installed/Asset.uasset")), TArray<uint8>{0xff});
		FVerifierRecorder CurrentVerifier;
		const FRunResult Current = RunPersistenceCase(CaseRoot, CurrentVerifier, Store);
		TestTrue(TEXT("committed digest mismatch blocks current persistence and preserves prior receipt identity"),
			Prior.Receipt.bSyncStateAdvanced && !Current.Receipt.RequestId.IsValid()
			&& Current.Calls == TArray<FString>{TEXT("RecoverIncomplete")}
			&& Current.Failures[0].Diagnostic.Code == TEXT("Atomic.Poisoned.RecoveryFailed"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentFreshVerifierDiagnosticsTest,
	"UEAssetDoc.AtomicFramework.FreshVerifierDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFreshVerifierDiagnosticsTest::RunTest(const FString& Parameters)
{
	using namespace FreshVerificationBeforeCommitTestsLocal;
	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentAtomicTests"), FGuid::NewGuid().ToString());
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const FAssetDocumentAtomicTestMount Mount(Root);
	for (EVerificationFailure Failure : {EVerificationFailure::None, EVerificationFailure::Fresh,
		EVerificationFailure::Canonical, EVerificationFailure::Hash, EVerificationFailure::Codec})
	{
		FVerifierRecorder Verifier;
		Verifier.Failure = Failure;
		Verifier.Diagnostics = {
			{TEXT("/Verifier/Fields/Parent"), TEXT("TestVerifier.FieldDetail"), TEXT("Original verifier field detail."),
				EAssetDocumentDiagnosticSeverity::Warning},
			{TEXT("/Verifier/Load"), TEXT("TestVerifier.LoadInfo"), TEXT("Original verifier load information."),
				EAssetDocumentDiagnosticSeverity::Info}};
		if (Failure != EVerificationFailure::None)
		{
			Verifier.Diagnostics[0].Severity = EAssetDocumentDiagnosticSeverity::Error;
		}
		auto& Context = Verifier.Diagnostics[0].Context.AddDefaulted_GetRef();
		Context.Key = TEXT("property");
		Context.Value.Kind = EAssetDocumentRuntimeValueKind::String;
		Context.Value.StringValue = TEXT("ParentClass");
		// Identical original entries must remain distinct, not be filtered as duplicates.
		const FAssetDocumentRuntimeDiagnostic Repeated = Verifier.Diagnostics[0];
		Verifier.Diagnostics.Add(Repeated);
		FTypedStoreRecorder Store;
		const FRunResult Result = RunPersistenceCase(
			FPaths::Combine(Root, FString::Printf(TEXT("Details-%d"), static_cast<uint8>(Failure))), Verifier, Store);
		const int32 DetailOffset = Failure == EVerificationFailure::None ? 0 : 1;
		TestEqual(TEXT("sink emits one entry per original detail plus only the failed aggregate"),
			Result.Failures.Num(), DetailOffset + 3);
		if (Failure == EVerificationFailure::None)
		{
			TestTrue(TEXT("successful verifier warnings and info do not change durable acceptance"),
				Result.Receipt.RequestId.IsValid() && Result.Receipt.bSyncStateAdvanced
				&& Result.Calls.Contains(TEXT("MarkDurableComplete")) && Result.Calls.Last() == TEXT("Cleanup"));
		}
		else
		{
			TestTrue(TEXT("verifier failures still stop before baseline capture and install"), StopsBeforeBaselineCapture(Result));
			if (!Result.Failures.IsEmpty())
			{
				const TCHAR* ExpectedCode = Failure == EVerificationFailure::Fresh
					? TEXT("Persistence.FreshLoadFailed") : TEXT("Persistence.CanonicalMismatch");
				TestEqual(TEXT("existing stable failure remains first"), Result.Failures[0].Diagnostic.Code, FString(ExpectedCode));
				TestEqual(TEXT("aggregate retains persistence path"), Result.Failures[0].Diagnostic.Path, FString(TEXT("/Persistence")));
			}
		}
		if (Result.Failures.Num() != DetailOffset + 3) continue;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const auto& Emitted = Result.Failures[DetailOffset + Index];
			const auto& Expected = Verifier.Diagnostics[Index];
			TestTrue(TEXT("sink preserves original diagnostic phase, path, code, message and severity in order"),
				Emitted.Step == EAssetDocumentPersistenceStep::FreshLoadStagedPackage
				&& Emitted.Diagnostic.Path == Expected.Path && Emitted.Diagnostic.Code == Expected.Code
				&& Emitted.Diagnostic.Message == Expected.Message && Emitted.Diagnostic.Severity == Expected.Severity
				&& Emitted.Diagnostic.Context.Num() == Expected.Context.Num());
			if (Index != 1)
			{
				const auto& ActualContext = Emitted.Diagnostic.Context;
				TestTrue(TEXT("typed context survives on each repeated original entry"),
					ActualContext.Num() == 1 && ActualContext[0].Key == TEXT("property")
					&& ActualContext[0].Value.Kind == EAssetDocumentRuntimeValueKind::String
					&& ActualContext[0].Value.StringValue == TEXT("ParentClass"));
			}
		}
	}
	return true;
}

#endif
