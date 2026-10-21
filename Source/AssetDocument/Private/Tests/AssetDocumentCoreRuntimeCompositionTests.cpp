// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentCoreRuntimeComposition.h"
#include "Runtime/AssetDocumentTypedSyncRuntime.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentCoreRuntimeCompositionTests
{
FString CoreTestPhaseName(EAssetDocumentRuntimeRequestPhase Phase)
{
	switch (Phase)
	{
	case EAssetDocumentRuntimeRequestPhase::Validate:
		return TEXT("Validate");
	case EAssetDocumentRuntimeRequestPhase::Preflight:
		return TEXT("Preflight");
	case EAssetDocumentRuntimeRequestPhase::Apply:
		return TEXT("Apply");
	case EAssetDocumentRuntimeRequestPhase::Repair:
		return TEXT("Repair");
	default:
		return TEXT("Other");
	}
}

class FRequestContext final : public IAssetDocumentNativeRequestContext
{
public:
	FRequestContext(
		FAssetDocumentRequestContextMetadata InMetadata,
		IAssetDocumentAtomicTransaction* InTransaction = nullptr,
		TArray<FAssetDocumentMutationFootprint> InFootprint = {},
		int32* InEnlistCount = nullptr)
		: Metadata(MoveTemp(InMetadata))
		, Transaction(InTransaction)
		, Footprint(MoveTemp(InFootprint))
		, EnlistCount(InEnlistCount)
	{
	}

	virtual const FAssetDocumentRequestContextMetadata& GetMetadata() const override
	{
		return Metadata;
	}

	virtual FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation) override
	{
		return {};
	}

	virtual FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override
	{
		return {};
	}

	FAssetDocumentContractValidationResult EnlistMutationFootprint()
	{
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
			if (Transaction == nullptr)
			{
				return Result;
			}
			if (EnlistCount)
			{
				++*EnlistCount;
			}
			return Transaction->EnlistFootprint(Footprint);
	}

	IAssetDocumentAtomicTransaction* GetTransaction() const
	{
		return Transaction;
	}

private:
	FAssetDocumentRequestContextMetadata Metadata;
	IAssetDocumentAtomicTransaction* Transaction = nullptr;
	TArray<FAssetDocumentMutationFootprint> Footprint;
	int32* EnlistCount = nullptr;
};

class FHost final : public IAssetDocumentPythonRuntimeHost
{
public:
	explicit FHost(
		TArray<FAssetDocumentMutationFootprint> InFootprint = {},
		bool* InCaptureOpen = nullptr,
		TArray<FString>* InTrace = nullptr,
		FGuid* InApplyTransactionId = nullptr,
		FGuid* InRepairTransactionId = nullptr)
		: Footprint(MoveTemp(InFootprint))
		, CaptureOpen(InCaptureOpen)
		, Trace(InTrace)
		, ApplyTransactionId(InApplyTransactionId)
		, RepairTransactionId(InRepairTransactionId)
	{
	}

	virtual FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) override
	{
		++DispatchCount;
		FAssetDocumentRuntimeResultDto Result;
		Result.RequestId = Request.RequestId;
		Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded;
		Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
			Result.RuntimeRevision = Context.GetMetadata().PinnedRuntimeRevision;
			Phases.Add(Context.GetMetadata().Phase);
			if (FailPhase.IsSet()
				&& FailPhase.GetValue() == Context.GetMetadata().Phase)
			{
				return MakeInjectedFailure(Request);
			}
			AppendChangedRecord(Result, Context.GetMetadata().Phase);
			if (Context.GetMetadata().Phase
			== EAssetDocumentRuntimeRequestPhase::Apply)
		{
			HandleApply(Context, Result);
		}
		else if (Context.GetMetadata().Phase
			== EAssetDocumentRuntimeRequestPhase::Repair)
		{
			HandleRepair(Context);
		}
		return Result;
	}

private:
	static void AppendChangedRecord(
		FAssetDocumentRuntimeResultDto& Result,
		EAssetDocumentRuntimeRequestPhase Phase)
	{
		const FString PhaseName = CoreTestPhaseName(Phase);
		FAssetDocumentChangedRecord& Changed =
			Result.ChangedSet.AddDefaulted_GetRef();
		Changed.AuthorityId = TEXT("host.runtime");
		Changed.StableObjectId = TEXT("phase:") + PhaseName;
		Changed.Path = TEXT("/ChangedSet/") + PhaseName;
		Changed.ChangeKind = PhaseName;
	}

	void HandleApply(
		IAssetDocumentNativeRequestContext& Context,
		FAssetDocumentRuntimeResultDto& Result)
	{
		++ApplyCount;
		if (Trace)
		{
			Trace->Add(TEXT("Host.Apply"));
		}
		bCaptureOpenDuringApply = CaptureOpen && *CaptureOpen;
		if (OnApply)
		{
			OnApply();
		}
		FRequestContext& RequestContext =
			static_cast<FRequestContext&>(Context);
		if (ApplyTransactionId)
		{
			*ApplyTransactionId =
				RequestContext.GetTransaction()->GetTransactionId();
		}
		const FAssetDocumentContractValidationResult Enlisted =
			RequestContext.EnlistMutationFootprint();
		if (!Enlisted.bValid)
		{
			Result.TerminalStatus =
				EAssetDocumentRuntimeTerminalStatus::Failed;
			Result.Diagnostics = Enlisted.Diagnostics;
		}
		else if (bFailAfterApplyEnlist)
		{
			Result.TerminalStatus =
				EAssetDocumentRuntimeTerminalStatus::Failed;
			Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
			Result.ChangedSet.Reset();
			FAssetDocumentRuntimeDiagnostic& Diagnostic =
				Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/InjectedHost/ApplyAfterEnlist");
			Diagnostic.Code = TEXT("Host.InjectedApplyFailureAfterEnlist");
			Diagnostic.Message =
				TEXT("Injected Apply failure after footprint enlistment.");
			Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
		}
	}

		void HandleRepair(IAssetDocumentNativeRequestContext& Context)
	{
		if (Trace)
		{
			Trace->Add(TEXT("Host.Repair"));
		}
		bCaptureOpenDuringRepair = CaptureOpen && *CaptureOpen;
		if (RepairTransactionId)
		{
			*RepairTransactionId =
				static_cast<FRequestContext&>(Context)
					.GetTransaction()->GetTransactionId();
			}
		}

		FAssetDocumentRuntimeResultDto MakeInjectedFailure(
			const FAssetDocumentRuntimeRequestDto& Request) const
		{
			FAssetDocumentRuntimeResultDto Result;
			Result.RequestId = Request.RequestId;
			Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
			Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
			Result.RuntimeRevision = Request.ExpectedRuntimeRevision;
			FAssetDocumentRuntimeDiagnostic& Diagnostic =
				Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/InjectedHost");
			Diagnostic.Code = TEXT("Host.InjectedFailure");
			Diagnostic.Message = TEXT("Injected Host phase failure.");
			Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
			return Result;
		}

public:

	int32 DispatchCount = 0;
	int32 ApplyCount = 0;
	bool bCaptureOpenDuringApply = false;
	bool bCaptureOpenDuringRepair = false;
	TArray<EAssetDocumentRuntimeRequestPhase> Phases;
	TArray<FAssetDocumentMutationFootprint> Footprint;
	TOptional<EAssetDocumentRuntimeRequestPhase> FailPhase;
	bool bFailAfterApplyEnlist = false;
	TUniqueFunction<void()> OnApply;

private:
	bool* CaptureOpen = nullptr;
	TArray<FString>* Trace = nullptr;
	FGuid* ApplyTransactionId = nullptr;
	FGuid* RepairTransactionId = nullptr;
};

class FPrimitiveRegistry final : public IAssetDocumentPrimitiveRegistry
{
public:
	virtual FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const override
	{
		return {};
	}

	virtual FAssetDocumentPrimitiveResolution Resolve(
		const FAssetDocumentPrimitiveRequirement& Requirement) const override
	{
		return {};
	}
};

class FCancellation final : public IAssetDocumentCancellationView
{
public:
	virtual bool IsCancellationRequested() const override
	{
		return false;
	}
};

class FObserver final : public IAssetDocumentRequestObserver
{
public:
	explicit FObserver(
		bool* InCaptureOpen = nullptr,
		TArray<FString>* InTrace = nullptr)
		: CaptureOpen(InCaptureOpen)
		, Trace(InTrace)
	{
	}

	virtual void OnPhaseChanged(
		FGuid RequestId,
		EAssetDocumentRuntimeRequestPhase Phase) override
	{
		Phases.Add(Phase);
		RecordTrace(Phase);
		if (Phase == EAssetDocumentRuntimeRequestPhase::Persist)
		{
			bCaptureOpenAtPersist = CaptureOpen && *CaptureOpen;
		}
	}

	bool bCaptureOpenAtPersist = false;
	TArray<EAssetDocumentRuntimeRequestPhase> Phases;

private:
	void RecordTrace(EAssetDocumentRuntimeRequestPhase Phase)
	{
		if (!Trace)
		{
			return;
		}
		switch (Phase)
		{
		case EAssetDocumentRuntimeRequestPhase::Persist:
			Trace->Add(TEXT("Observer.Persist"));
			break;
		case EAssetDocumentRuntimeRequestPhase::Complete:
			Trace->Add(TEXT("Observer.Complete"));
			break;
		default:
			break;
		}
	}

	bool* CaptureOpen = nullptr;
	TArray<FString>* Trace = nullptr;
};

class FRollbackAction final : public IAssetDocumentRollbackAction
{
public:
	explicit FRollbackAction(
		FString InComponentId = TEXT("rollback.package"),
		bool* InCaptureOpen = nullptr,
		TArray<FString>* InTrace = nullptr)
		: ComponentId(MoveTemp(InComponentId))
		, CaptureOpen(InCaptureOpen)
		, Trace(InTrace)
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return ComponentId;
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		++RollbackCount;
		bCaptureOpenAtRollback = CaptureOpen && *CaptureOpen;
		if (Trace)
		{
			Trace->Add(TEXT("RollbackAction.") + ComponentId);
		}
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}

	bool bCaptureOpenAtRollback = false;
	int32 RollbackCount = 0;

private:
	FString ComponentId;
	bool* CaptureOpen = nullptr;
	TArray<FString>* Trace = nullptr;
};

class FVerifier final : public IAssetDocumentStagedArtifactVerifier
{
public:
	virtual FAssetDocumentFreshVerificationResult VerifyFresh(
		const FAssetDocumentFreshVerificationInput& Input) override
	{
		++CallCount;
		LastInput = Input;
		bStagedPackageExists =
			IFileManager::Get().FileExists(*Input.StagedPackagePath);
		bStagedSidecarExists =
			IFileManager::Get().FileExists(*Input.StagedSidecarPath);
		FAssetDocumentFreshVerificationResult Result;
		if (bFail)
		{
			return Result;
		}
		Result.bFreshLoadProven = true;
		Result.bCanonicalMatch = true;
		Result.ActualSemanticHash = Input.ExpectedSemanticHash;
		Result.ActualCodecVersion = Input.ExpectedCodecVersion;
		return Result;
	}

	bool bFail = false;
	int32 CallCount = 0;
	bool bStagedPackageExists = false;
	bool bStagedSidecarExists = false;
	FAssetDocumentFreshVerificationInput LastInput;
};

FAssetDocumentRuntimeRequestDto MakeRequest(
	EAssetDocumentRuntimeOperation Operation =
		EAssetDocumentRuntimeOperation::Inspect,
	FString TargetAssetPath =
		TEXT("/Game/AssetDocument/T_CoreComposition.T_CoreComposition"))
{
	FAssetDocumentRuntimeRequestDto Request;
	Request.RequestId = FGuid::NewGuid();
	Request.Operation = Operation;
	Request.TargetAssetPath = MoveTemp(TargetAssetPath);
	Request.ExpectedRuntimeRevision = 7;
	return Request;
}

FAssetDocumentRegionEvidence MakeEvidence(const TCHAR* Hash)
{
	FAssetDocumentRegionEvidence Evidence;
	Evidence.Kind = EAssetDocumentEvidenceKind::Present;
	Evidence.SemanticHash = Hash;
	Evidence.CodecVersion = 1;
	return Evidence;
}

FAssetDocumentPersistencePlan MakePersistencePlan(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FString& PackageName,
	const FString& Root,
	const FAssetDocumentMutationFootprint& Footprint)
{
	FAssetDocumentPersistencePlan Plan;
	Plan.RequestId = Request.RequestId;
	Plan.TargetPackagePath = PackageName;
	Plan.TargetSidecarPath =
		FPaths::Combine(Root, TEXT("Installed/Asset.assetdoc"));
	Plan.StagingRoot = FPaths::Combine(Root, TEXT("Staging"));
	Plan.JournalId = TEXT("core-composition");
	Plan.Sidecar.Utf8Bytes = {0x7b, 0x7d};
	Plan.Sidecar.SemanticHash = TEXT("semantic:v1");
	Plan.Sidecar.CodecVersion = 1;
	Plan.Footprint.Add(Footprint);
	return Plan;
}

FAssetDocumentPreflightReceipt MakePreflight(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentMutationFootprint& Footprint)
{
	FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = Request.RequestId;
	Receipt.RuntimeRevision = Request.ExpectedRuntimeRevision;
	Receipt.CompleteFootprint.Add(Footprint);
	Receipt.ReceiptHash = TEXT("sha256:core-composition");
	Receipt.bSucceeded = true;
	return Receipt;
}

FAssetDocumentCoreRuntimeDependencies MakeCoreDependencies(
	FHost& Host,
	FPrimitiveRegistry& Registry,
	FCancellation& Cancellation,
	FObserver& Observer,
	int32* ContextCount = nullptr,
	int32* InvalidationCount = nullptr)
{
	return {
		Host,
		Registry,
		Cancellation,
		Observer,
		[ContextCount](
			const FAssetDocumentRequestContextMetadata& Metadata,
			const FGuid& TransactionId,
			IAssetDocumentAtomicTransaction* ActiveTransaction,
			TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
		{
			if (ContextCount)
			{
				++*ContextCount;
			}
			return MakeUnique<FRequestContext>(Metadata);
		},
		[InvalidationCount](const FAssetDocumentHandleInvalidation& Event)
		{
			if (InvalidationCount)
			{
				++*InvalidationCount;
			}
			return FAssetDocumentHandleInvalidationResult{};
		}};
}

FAssetDocumentCoreMutationInputs MakeOperationAdmissionMutation(
	FVerifier& Verifier)
{
	return {
		FAssetDocumentPreflightReceipt{},
		FAssetDocumentPersistencePlan{},
		Verifier,
		{},
		{},
		{},
		{},
		{},
		{},
		0,
		{},
		{}};
}

void TestReadOnlyComposition(FAutomationTestBase& Test)
{
	FHost Host;
	FPrimitiveRegistry Registry;
	FCancellation Cancellation;
	FObserver Observer;
	FAssetDocumentCoreRuntimeComposition Composition(
		MakeCoreDependencies(Host, Registry, Cancellation, Observer));
	const FAssetDocumentRuntimeResultDto Result =
		Composition.ExecuteReadOnly(MakeRequest());
	Test.TestEqual(
		TEXT("core composition executes through the existing framework owners"),
		Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Succeeded);
}

void DeletePackageSegments(const FString& PackageName)
{
	const FString HeaderPath = FPackageName::LongPackageNameToFilename(
		PackageName,
		FPackageName::GetAssetPackageExtension());
	const FString BasePath = HeaderPath.LeftChop(
		FCString::Strlen(TEXT(".uasset")));
	const TArray<FString> SegmentPaths{
		HeaderPath,
		BasePath + TEXT(".uexp"),
		BasePath + TEXT(".ubulk"),
		BasePath + TEXT(".uptnl"),
		BasePath + TEXT(".m.ubulk"),
		BasePath + TEXT(".upayload")};
	for (const FString& SegmentPath : SegmentPaths)
	{
		IFileManager::Get().Delete(*SegmentPath, false, true);
	}
}

void ClearPackageObjects(UPackage* Package, UObject* Asset)
{
	if (Package)
	{
		FAssetRegistryModule::GetRegistry().PackageDeleted(Package);
	}
	if (Asset)
	{
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->MarkAsGarbage();
	}
	if (Package)
	{
		Package->SetDirtyFlag(false);
	}
}

void CleanupPackageFixture(
	const FString& PackageName,
	UPackage* Package,
	UObject* Asset)
{
	if (Package)
	{
		ResetLoaders(Package);
	}
	DeletePackageSegments(PackageName);
	ClearPackageObjects(Package, Asset);
}

enum class EMutationCaseVariant : uint8
{
	Default,
	EmptyRollbackActions,
	DuplicateRollbackActions,
	PreflightRequestMismatch,
	PersistenceRequestMismatch,
	PreflightRevisionMismatch,
	CallerTransactionId,
	OrderedFootprintMismatch,
	PackageTargetMismatch,
	AssetOuterMismatch,
	EmptyJournalRoot,
	EmptyJournalBackupRoot,
	EmptySyncStateRoot,
	EmptySyncRegionId,
	ZeroFreshRevision,
	MissingCaptureOpener,
	MissingPersistenceTargetBinder,
	EmptyPersistenceTargetBinding,
	DiagnosticAndEmptyPersistenceTargetBinding,
	PersistenceTargetRequestMismatch,
	PersistenceTargetTransactionMismatch,
	PersistenceTargetRevisionMismatch,
	NullPersistenceTargetAsset,
	NullPersistenceTargetPackage,
	PersistenceTargetObjectPathMismatch,
	PersistenceTargetClassMismatch,
};

struct FMutationCaseOptions
{
	EMutationCaseVariant Variant = EMutationCaseVariant::Default;
	bool bPreExistingTarget = false;
	bool bFailVerification = false;
	TOptional<EAssetDocumentRuntimeRequestPhase> FailContextPhase;
	TOptional<EAssetDocumentRuntimeRequestPhase> FailHostPhase;
	bool bFailCaptureOpen = false;
	bool bFailAfterApplyEnlist = false;
	bool bUseUndeclaredProviderFootprint = false;
	bool bIncompleteProviderCoverage = false;
	bool bUseTwoRollbackActions = false;
};

struct FMutationCaseResult
{
	FAssetDocumentRuntimeRequestDto Request;
	FAssetDocumentRuntimeResultDto Result;
	TArray<FAssetDocumentHandleInvalidation> Invalidations;
	TArray<EAssetDocumentRuntimeRequestPhase> ContextAttempts;
	TArray<EAssetDocumentRuntimeRequestPhase> ObserverPhases;
	TArray<FString> Trace;
	FGuid TransactionId;
	FGuid ApplyTransactionId;
	FGuid RepairTransactionId;
	EAssetDocumentAtomicState StateAtCaptureClose =
		EAssetDocumentAtomicState::Poisoned;
	int32 OpenCount = 0;
	int32 CloseCount = 0;
	int32 RollbackCount = 0;
	int32 EnlistCount = 0;
	int32 HostDispatchCount = 0;
	int32 HostApplyCount = 0;
	int32 BinderCount = 0;
	int32 PersistencePhaseCount = 0;
	int32 CommitCount = 0;
	int32 VerifierCallCount = 0;
	FGuid BinderRequestId;
	FGuid BinderTransactionId;
	uint64 BinderRuntimeRevision = 0;
	FString BinderCanonicalObjectPath;
	FString BinderExactAssetClassPath;
	FString BinderTargetPackagePath;
	TArray<FAssetDocumentChangedRecord> BinderChangedSet;
	bool bCaptureOpen = false;
	bool bTargetAbsentAtCaptureOpen = false;
	bool bCaptureOpenAtBind = false;
	bool bTargetExistsAtBind = false;
	bool bCaptureOpenDuringApply = false;
	bool bCaptureOpenDuringRepair = false;
	bool bCaptureOpenAtPersist = false;
	bool bCaptureOpenAtCommit = false;
	bool bPackageDirtyAtCommit = false;
	bool bPackageDirtyAfterExecute = false;
	bool bCaptureOpenAtRollbackInvalidation = false;
	bool bCaptureOpenAtRollbackAction = false;
	bool bStagedPackageExistsAtVerify = false;
	bool bStagedSidecarExistsAtVerify = false;
	FAssetDocumentFreshVerificationInput VerificationInput;
	bool bInstalledPackageExists = false;
	int64 InstalledPackageSize = -1;
	bool bInstalledSidecarExists = false;
	TArray<uint8> InstalledSidecarBytes;
	bool bSyncLoadValid = false;
	bool bSyncExisted = false;
	TOptional<FAssetDocumentTypedSyncStateRecord> SyncRecord;
	TArray<uint8> EncodedSyncBytes;
	bool bStagingHasFiles = false;
};

struct FMovedMutationInputReuseResult
{
	FMutationCaseResult First;
	FAssetDocumentRuntimeResultDto Second;
	int32 HostApplyDelta = 0;
	int32 BinderDelta = 0;
	int32 PersistenceDelta = 0;
	int32 VerifierDelta = 0;
	int32 CommitDelta = 0;
	int32 RollbackDelta = 0;
};

struct FMutationFixturePaths
{
	FString Root;
	FString PackageRoot;
	FString InstalledRoot;
	FString PackageName;
};

FMutationFixturePaths MakeMutationFixturePaths()
{
	const FString FixtureId = FGuid::NewGuid().ToString();
	FMutationFixturePaths Paths;
	Paths.Root = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AssetDocumentCoreRuntimeCompositionTests"),
		FixtureId);
	Paths.PackageRoot = FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("AssetDocumentCoreRuntimeCompositionTests"),
		FixtureId);
	Paths.InstalledRoot = FPaths::Combine(Paths.Root, TEXT("Installed"));
	Paths.PackageName = FString::Printf(
		TEXT("/Game/AssetDocumentCoreRuntimeCompositionTests/%s/Asset"),
		*FixtureId);
	return Paths;
}

class FMutationCaseHarness final
{
public:
	FMutationCaseHarness(
		FAutomationTestBase& InTest,
		FMutationCaseOptions InOptions)
		: Test(InTest)
		, Options(InOptions)
		, Paths(MakeMutationFixturePaths())
		, Footprint{
			EAssetDocumentMutationFootprintKind::Package,
			Paths.PackageName,
			TEXT("rollback.package")}
		, OtherFootprint{
			EAssetDocumentMutationFootprintKind::File,
			FPaths::Combine(Paths.Root, TEXT("other.bin")),
			TEXT("rollback.other")}
		, Host(
			{Footprint},
			&Outcome.bCaptureOpen,
			&Outcome.Trace,
			&Outcome.ApplyTransactionId,
			&Outcome.RepairTransactionId)
		, Observer(&Outcome.bCaptureOpen, &Outcome.Trace)
		, Action(MakeShared<FRollbackAction>(
			TEXT("rollback.package"),
			&Outcome.bCaptureOpen,
			&Outcome.Trace))
	{
		Outcome.Request = MakeRequest(
			EAssetDocumentRuntimeOperation::Apply,
			Paths.PackageName + TEXT(".Asset"));
		Outcome.Request.RequestedAssetClassPath =
			UEnum::StaticClass()->GetPathName();
		Preflight = MakePreflight(Outcome.Request, Footprint);
		Plan = MakePersistencePlan(
			Outcome.Request,
			Paths.PackageName,
			Paths.Root,
			Footprint);
		Verifier.bFail = Options.bFailVerification;
		Host.FailPhase = Options.FailHostPhase;
		Host.bFailAfterApplyEnlist = Options.bFailAfterApplyEnlist;
		Host.OnApply = [this]()
		{
			if (!Options.bPreExistingTarget)
			{
				CreateTarget();
			}
			Package->SetDirtyFlag(true);
		};
		ApplyVariant();
	}

	~FMutationCaseHarness()
	{
		Cleanup();
	}

	FMutationCaseResult Execute()
	{
		if (!Setup())
		{
			return MoveTemp(Outcome);
		}
		FAssetDocumentCoreRuntimeComposition Composition(BuildDependencies());
		FAssetDocumentCoreMutationInputs Mutation = BuildMutation();
		Outcome.Result = Composition.ExecuteMutation(
			Outcome.Request,
			MoveTemp(Mutation));
		CollectOutcome();
		return MoveTemp(Outcome);
	}

	FMovedMutationInputReuseResult ExecuteMovedInputTwice()
	{
		if (!Setup())
		{
			return {MoveTemp(Outcome)};
		}
		FAssetDocumentCoreRuntimeComposition Composition(BuildDependencies());
		FAssetDocumentCoreMutationInputs Mutation = BuildMutation();
		Outcome.Result = Composition.ExecuteMutation(
			Outcome.Request,
			MoveTemp(Mutation));
		CollectOutcome();
		FMovedMutationInputReuseResult Reuse;
		Reuse.First = Outcome;
		Reuse.Second = Composition.ExecuteMutation(
			Outcome.Request,
			MoveTemp(Mutation));
		Reuse.HostApplyDelta = Host.ApplyCount - Reuse.First.HostApplyCount;
		Reuse.BinderDelta = Outcome.BinderCount - Reuse.First.BinderCount;
		Reuse.PersistenceDelta =
			CountPersistencePhases() - Reuse.First.PersistencePhaseCount;
		Reuse.VerifierDelta =
			Verifier.CallCount - Reuse.First.VerifierCallCount;
		Reuse.CommitDelta = Outcome.CommitCount - Reuse.First.CommitCount;
		Reuse.RollbackDelta = CountRollbacks() - Reuse.First.RollbackCount;
		return Reuse;
	}

private:
	bool Setup()
	{
		if (!Test.TestTrue(
				TEXT("creates isolated package and sidecar fixture roots"),
				IFileManager::Get().MakeDirectory(*Paths.PackageRoot, true)
					&& IFileManager::Get().MakeDirectory(
						*Paths.InstalledRoot,
						true)))
		{
			return false;
		}
		if (!Test.TestNull(
			TEXT("target package is absent before Atomic begin"),
			FindPackage(nullptr, *Paths.PackageName)))
		{
			return false;
		}
		if (Options.bPreExistingTarget)
		{
			CreateTarget();
		}
		return true;
	}

	void CreateTarget()
	{
		Package = CreatePackage(*Paths.PackageName);
		Asset = NewObject<UEnum>(
			Package, TEXT("Asset"), RF_Public | RF_Standalone);
		if (Options.Variant
			== EMutationCaseVariant::PersistenceTargetObjectPathMismatch)
		{
			MismatchedAsset = NewObject<UEnum>(
				Package, TEXT("OtherAsset"), RF_Public | RF_Standalone);
		}
		if (Options.Variant == EMutationCaseVariant::AssetOuterMismatch)
		{
			MismatchedAsset = NewObject<UEnum>(
				GetTransientPackage(), NAME_None, RF_Transient);
		}
	}

		void ApplyVariant()
		{
			ApplyIdentityVariant();
			if (Options.Variant == EMutationCaseVariant::PackageTargetMismatch)
			{
				Plan.TargetPackagePath =
					TEXT("/Game/AssetDocumentCoreRuntimeCompositionTests/Other");
			}
			if (Options.Variant
				== EMutationCaseVariant::PersistenceTargetClassMismatch)
			{
				Outcome.Request.RequestedAssetClassPath =
					UObject::StaticClass()->GetPathName();
			}
			if (Options.Variant == EMutationCaseVariant::OrderedFootprintMismatch)
			{
				Preflight.CompleteFootprint.Add(OtherFootprint);
				Plan.Footprint.Insert(OtherFootprint, 0);
			}
			ApplyCrossOwnerOptions();
		}

		void ApplyCrossOwnerOptions()
		{
			if (Options.bIncompleteProviderCoverage
				|| Options.bUseTwoRollbackActions)
			{
				Preflight.CompleteFootprint.Add(OtherFootprint);
				Plan.Footprint.Add(OtherFootprint);
			}
			if (Options.bUseTwoRollbackActions)
			{
				Host.Footprint.Add(OtherFootprint);
			}
			if (Options.bUseUndeclaredProviderFootprint)
			{
				Host.Footprint[0].StableId += TEXT(".undeclared");
			}
		}

	void ApplyIdentityVariant()
	{
		switch (Options.Variant)
		{
		case EMutationCaseVariant::PreflightRequestMismatch:
			Preflight.RequestId = FGuid::NewGuid();
			break;
		case EMutationCaseVariant::PersistenceRequestMismatch:
			Plan.RequestId = FGuid::NewGuid();
			break;
		case EMutationCaseVariant::PreflightRevisionMismatch:
			++Preflight.RuntimeRevision;
			break;
		case EMutationCaseVariant::CallerTransactionId:
			Plan.TransactionId = FGuid::NewGuid();
			break;
		default:
			break;
		}
	}

	FAssetDocumentCoreRuntimeDependencies BuildDependencies()
	{
		return {
			Host,
			Registry,
			Cancellation,
			Observer,
				[this](
					const FAssetDocumentRequestContextMetadata& Metadata,
					const FGuid& TransactionId,
					IAssetDocumentAtomicTransaction* InActiveTransaction,
					TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
					-> TUniquePtr<IAssetDocumentNativeRequestContext>
				{
					Outcome.ContextAttempts.Add(Metadata.Phase);
					if (Options.FailContextPhase.IsSet()
						&& Options.FailContextPhase.GetValue()
							== Metadata.Phase)
					{
						FAssetDocumentRuntimeDiagnostic& Diagnostic =
							OutDiagnostics.AddDefaulted_GetRef();
						Diagnostic.Path = TEXT("/InjectedContext");
						Diagnostic.Code = TEXT("Context.InjectedFailure");
						Diagnostic.Message =
							TEXT("Injected context creation failure.");
						Diagnostic.Severity =
							EAssetDocumentDiagnosticSeverity::Error;
						return TUniquePtr<IAssetDocumentNativeRequestContext>();
					}
					if (InActiveTransaction)
					{
						ActiveTransaction = InActiveTransaction;
					}
					return MakeUnique<FRequestContext>(
						Metadata,
						InActiveTransaction,
						Host.Footprint,
						&Outcome.EnlistCount);
				},
			[this](const FAssetDocumentHandleInvalidation& Event)
			{
				RecordInvalidation(Event);
				return FAssetDocumentHandleInvalidationResult{};
			}};
	}

	void RecordInvalidation(const FAssetDocumentHandleInvalidation& Event)
	{
		Outcome.Invalidations.Add(Event);
		switch (Event.Reason)
		{
		case EAssetDocumentHandleInvalidationReason::Commit:
			++Outcome.CommitCount;
			Outcome.bCaptureOpenAtCommit = Outcome.bCaptureOpen;
			Outcome.bPackageDirtyAtCommit = Package && Package->IsDirty();
			Outcome.Trace.Add(TEXT("Invalidation.Commit"));
			break;
		case EAssetDocumentHandleInvalidationReason::Rollback:
			Outcome.bCaptureOpenAtRollbackInvalidation =
				Outcome.bCaptureOpen;
			Outcome.Trace.Add(TEXT("Invalidation.Rollback"));
			break;
		default:
			Outcome.Trace.Add(TEXT("Invalidation.PackageReload"));
			break;
		}
	}

	TArray<TSharedRef<IAssetDocumentRollbackAction>> BuildRollbackActions()
	{
		TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
		if (Options.Variant != EMutationCaseVariant::EmptyRollbackActions)
		{
			Actions.Add(Action);
		}
			if (Options.Variant == EMutationCaseVariant::DuplicateRollbackActions)
			{
			Actions.Add(MakeShared<FRollbackAction>(
				TEXT("rollback.package"),
				&Outcome.bCaptureOpen,
					&Outcome.Trace));
			}
			if (Options.bIncompleteProviderCoverage
				|| Options.bUseTwoRollbackActions)
			{
				SecondaryAction = MakeShared<FRollbackAction>(
					TEXT("rollback.other"),
					&Outcome.bCaptureOpen,
					&Outcome.Trace);
				Actions.Add(SecondaryAction.ToSharedRef());
			}
			return Actions;
	}

	FAssetDocumentCoreMutationCaptureOpener BuildCaptureOpener()
	{
		if (Options.Variant == EMutationCaseVariant::MissingCaptureOpener)
		{
			return {};
		}
		return [this](
			const FGuid& TransactionId,
			TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
			{
				++Outcome.OpenCount;
				Outcome.TransactionId = TransactionId;
				Outcome.bTargetAbsentAtCaptureOpen =
					Package == nullptr && Asset == nullptr;
				Outcome.bCaptureOpen = true;
				Outcome.Trace.Add(TEXT("Capture.Open"));
				if (Options.bFailCaptureOpen)
				{
					FAssetDocumentRuntimeDiagnostic& Diagnostic =
						OutDiagnostics.AddDefaulted_GetRef();
					Diagnostic.Path = TEXT("/InjectedCapture");
					Diagnostic.Code = TEXT("Capture.InjectedFailure");
					Diagnostic.Message =
						TEXT("Injected capture opener failure.");
					Diagnostic.Severity =
						EAssetDocumentDiagnosticSeverity::Error;
					Outcome.bCaptureOpen = false;
					return TUniquePtr<
						FAssetDocumentCoreMutationCaptureLease>();
				}
				return MakeUnique<FAssetDocumentCoreMutationCaptureLease>(
				[this]()
				{
					CloseCapture();
				});
			};
		}

		FAssetDocumentCorePersistenceTargetBinder BuildPersistenceTargetBinder()
		{
			if (Options.Variant
				== EMutationCaseVariant::MissingPersistenceTargetBinder)
			{
				return {};
			}
			return [this](
				const FAssetDocumentCorePersistenceTargetRequest& Request,
				TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
				-> TOptional<FAssetDocumentCorePersistenceTargetBinding>
			{
				++Outcome.BinderCount;
				Outcome.Trace.Add(TEXT("PersistenceTarget.Bind"));
				Outcome.bCaptureOpenAtBind = Outcome.bCaptureOpen;
				Outcome.bTargetExistsAtBind =
					Package != nullptr && Asset != nullptr;
				Outcome.BinderRequestId = Request.RequestId;
				Outcome.BinderTransactionId = Request.TransactionId;
				Outcome.BinderRuntimeRevision = Request.RuntimeRevision;
				Outcome.BinderCanonicalObjectPath =
					FString(Request.CanonicalObjectPath);
				Outcome.BinderExactAssetClassPath =
					FString(Request.ExactAssetClassPath);
				Outcome.BinderTargetPackagePath =
					FString(Request.TargetPackagePath);
				Outcome.BinderChangedSet.Append(Request.ChangedSet);
				if (Options.Variant
					== EMutationCaseVariant::
						DiagnosticAndEmptyPersistenceTargetBinding)
				{
					FAssetDocumentRuntimeDiagnostic& Diagnostic =
						OutDiagnostics.AddDefaulted_GetRef();
					Diagnostic.Path =
						TEXT("/CoreRuntimeComposition/PersistenceTarget");
					Diagnostic.Code =
						TEXT("Test.PersistenceTargetBinderRejected");
					Diagnostic.Message =
						TEXT("Injected persistence target binder rejection.");
					Diagnostic.Severity =
						EAssetDocumentDiagnosticSeverity::Error;
					return {};
				}
				if (Options.Variant
					== EMutationCaseVariant::EmptyPersistenceTargetBinding)
				{
					return {};
				}
				FAssetDocumentCorePersistenceTargetBinding Binding;
				Binding.RequestId = Request.RequestId;
				Binding.TransactionId = Request.TransactionId;
				Binding.RuntimeRevision = Request.RuntimeRevision;
				Binding.Asset = GetMutationAsset();
				Binding.Package = Package;
				ApplyPersistenceTargetBindingVariant(Binding);
				return Binding;
			};
		}

		void ApplyPersistenceTargetBindingVariant(
			FAssetDocumentCorePersistenceTargetBinding& Binding) const
		{
			switch (Options.Variant)
			{
			case EMutationCaseVariant::PersistenceTargetRequestMismatch:
				Binding.RequestId = FGuid::NewGuid();
				break;
			case EMutationCaseVariant::PersistenceTargetTransactionMismatch:
				Binding.TransactionId = FGuid::NewGuid();
				break;
			case EMutationCaseVariant::PersistenceTargetRevisionMismatch:
				++Binding.RuntimeRevision;
				break;
			case EMutationCaseVariant::NullPersistenceTargetAsset:
				Binding.Asset = nullptr;
				break;
			case EMutationCaseVariant::NullPersistenceTargetPackage:
				Binding.Package = nullptr;
				break;
			default:
				break;
			}
		}

		void CloseCapture()
		{
		++Outcome.CloseCount;
		if (ActiveTransaction)
		{
			Outcome.StateAtCaptureClose = ActiveTransaction->GetState();
		}
		Outcome.bCaptureOpen = false;
			Outcome.Trace.Add(TEXT("Capture.Close"));
		}

		FString MakeJournalRoot() const
		{
			return Options.Variant == EMutationCaseVariant::EmptyJournalRoot
				? FString()
				: FPaths::Combine(Paths.Root, TEXT("Journal"));
		}

		FString MakeJournalBackupRoot() const
		{
			return Options.Variant
				== EMutationCaseVariant::EmptyJournalBackupRoot
				? FString()
				: FPaths::Combine(Paths.Root, TEXT("Journal/Backups"));
		}

		FString MakeSyncStateRoot() const
		{
			return Options.Variant == EMutationCaseVariant::EmptySyncStateRoot
				? FString()
				: FPaths::Combine(Paths.Root, TEXT("Sync"));
		}

		FString MakeSyncRegionId() const
		{
			return Options.Variant == EMutationCaseVariant::EmptySyncRegionId
				? FString()
				: FString(TEXT("Body"));
		}

		UObject* GetMutationAsset() const
		{
			return Options.Variant == EMutationCaseVariant::AssetOuterMismatch
					|| Options.Variant
						== EMutationCaseVariant::
							PersistenceTargetObjectPathMismatch
				? MismatchedAsset
				: Asset;
		}

		FAssetDocumentCoreMutationInputs BuildMutation()
		{
			return {
				Preflight,
				Plan,
				Verifier,
				BuildPersistenceTargetBinder(),
				{MakeEvidence(TEXT("asset:v1")),
				MakeEvidence(TEXT("semantic:v1"))},
				MakeJournalRoot(),
				MakeJournalBackupRoot(),
				MakeSyncStateRoot(),
				MakeSyncRegionId(),
			Options.Variant == EMutationCaseVariant::ZeroFreshRevision ? uint64(0) : uint64(7),
			BuildRollbackActions(),
			BuildCaptureOpener()};
	}

		void CollectOutcome()
		{
			Outcome.RollbackCount = CountRollbacks();
			Outcome.bPackageDirtyAfterExecute = Package && Package->IsDirty();
		Outcome.bCaptureOpenAtRollbackAction =
			Action->bCaptureOpenAtRollback;
		Outcome.HostDispatchCount = Host.DispatchCount;
		Outcome.HostApplyCount = Host.ApplyCount;
		Outcome.bCaptureOpenDuringApply = Host.bCaptureOpenDuringApply;
		Outcome.bCaptureOpenDuringRepair = Host.bCaptureOpenDuringRepair;
		Outcome.bCaptureOpenAtPersist = Observer.bCaptureOpenAtPersist;
			Outcome.ObserverPhases = Observer.Phases;
			Outcome.PersistencePhaseCount = CountPersistencePhases();
		Outcome.VerifierCallCount = Verifier.CallCount;
		Outcome.bStagedPackageExistsAtVerify =
			Verifier.bStagedPackageExists;
		Outcome.bStagedSidecarExistsAtVerify =
			Verifier.bStagedSidecarExists;
		Outcome.VerificationInput = Verifier.LastInput;
		CollectArtifactOutcome();
			CollectSyncOutcome();
		}

		int32 CountPersistencePhases() const
		{
			int32 Count = 0;
			for (EAssetDocumentRuntimeRequestPhase Phase : Observer.Phases)
			{
				if (Phase == EAssetDocumentRuntimeRequestPhase::Persist)
				{
					++Count;
				}
			}
			return Count;
		}

		int32 CountRollbacks() const
		{
			return Action->RollbackCount
				+ (SecondaryAction.IsValid()
					? SecondaryAction->RollbackCount
					: 0);
		}

	void CollectArtifactOutcome()
	{
		const FString InstalledPackagePath =
			FPackageName::LongPackageNameToFilename(
				Paths.PackageName,
				FPackageName::GetAssetPackageExtension());
		Outcome.bInstalledPackageExists =
			IFileManager::Get().FileExists(*InstalledPackagePath);
		Outcome.InstalledPackageSize =
			IFileManager::Get().FileSize(*InstalledPackagePath);
		const FString InstalledSidecarPath =
			FPaths::Combine(Paths.InstalledRoot, TEXT("Asset.assetdoc"));
		Outcome.bInstalledSidecarExists =
			IFileManager::Get().FileExists(*InstalledSidecarPath);
		if (Outcome.bInstalledSidecarExists)
		{
			FFileHelper::LoadFileToArray(
				Outcome.InstalledSidecarBytes,
				*InstalledSidecarPath);
		}
	}

	void CollectSyncOutcome()
	{
		FAssetDocumentJournaledTypedSyncStateStore SyncStore(
			FPaths::Combine(Paths.Root, TEXT("Sync")));
		Outcome.bSyncLoadValid = SyncStore.Load(
			{Paths.PackageName, TEXT("Body")},
			Outcome.SyncRecord,
			Outcome.EncodedSyncBytes,
			Outcome.bSyncExisted).bValid;
		TArray<FString> StagedFiles;
		IFileManager::Get().FindFilesRecursive(
			StagedFiles,
			*FPaths::Combine(Paths.Root, TEXT("Staging")),
			TEXT("*"),
			true,
			false);
		Outcome.bStagingHasFiles = !StagedFiles.IsEmpty();
	}

		void Cleanup()
		{
			CleanupPackageFixture(Paths.PackageName, Package, Asset);
			if (MismatchedAsset)
			{
				MismatchedAsset->ClearFlags(RF_Public | RF_Standalone);
				MismatchedAsset->MarkAsGarbage();
			}
			IFileManager::Get().DeleteDirectory(*Paths.Root, false, true);
		IFileManager::Get().DeleteDirectory(*Paths.PackageRoot, false, true);
	}

	FAutomationTestBase& Test;
	FMutationCaseOptions Options;
	FMutationCaseResult Outcome;
	FMutationFixturePaths Paths;
		UPackage* Package = nullptr;
		UObject* Asset = nullptr;
		UObject* MismatchedAsset = nullptr;
	FAssetDocumentMutationFootprint Footprint;
	FAssetDocumentMutationFootprint OtherFootprint;
	FAssetDocumentPreflightReceipt Preflight;
	FAssetDocumentPersistencePlan Plan;
	FHost Host;
	FPrimitiveRegistry Registry;
	FCancellation Cancellation;
	FObserver Observer;
	FVerifier Verifier;
		TSharedRef<FRollbackAction> Action;
		TSharedPtr<FRollbackAction> SecondaryAction;
	IAssetDocumentAtomicTransaction* ActiveTransaction = nullptr;
};

FMutationCaseResult RunMutationCase(
	FAutomationTestBase& Test,
	FMutationCaseOptions Options = {})
{
	FMutationCaseHarness Harness(Test, Options);
	return Harness.Execute();
}

FMovedMutationInputReuseResult RunMovedMutationInputReuseCase(
	FAutomationTestBase& Test)
{
	FMutationCaseHarness Harness(Test, {});
	return Harness.ExecuteMovedInputTwice();
}

int32 CountDiagnosticCode(
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics,
	FStringView Code)
{
	int32 Count = 0;
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Diagnostics)
	{
		if (Diagnostic.Code == Code)
		{
			++Count;
		}
	}
	return Count;
}

int32 CountDiagnosticCode(
	const FAssetDocumentRuntimeResultDto& Result,
	FStringView Code)
{
	return CountDiagnosticCode(Result.Diagnostics, Code);
}

void TestOperationAdmissionFailure(
	FAutomationTestBase& Test,
	const FAssetDocumentRuntimeResultDto& Result,
	FStringView ExpectedCode)
{
	Test.TestEqual(
		TEXT("operation admission fails closed"),
		Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(
		TEXT("operation admission emits one diagnostic"),
		Result.Diagnostics.Num(),
		1);
	Test.TestEqual(
		TEXT("operation admission diagnostic code is exact"),
		CountDiagnosticCode(Result, ExpectedCode),
		1);
	if (!Result.Diagnostics.IsEmpty())
	{
		Test.TestEqual(
			TEXT("operation admission diagnostic path is exact"),
			Result.Diagnostics[0].Path,
			FString(TEXT("/CoreRuntimeComposition/Operation")));
	}
}

void TestReadOnlyRejectsMutationOperations(FAutomationTestBase& Test)
{
	FHost Host;
	FPrimitiveRegistry Registry;
	FCancellation Cancellation;
	FObserver Observer;
	int32 ContextCount = 0;
	int32 InvalidationCount = 0;
	FAssetDocumentCoreRuntimeComposition Composition(MakeCoreDependencies(
		Host,
		Registry,
		Cancellation,
		Observer,
		&ContextCount,
		&InvalidationCount));
	const EAssetDocumentRuntimeOperation MutationOperations[]{
		EAssetDocumentRuntimeOperation::Preflight,
		EAssetDocumentRuntimeOperation::Apply,
		EAssetDocumentRuntimeOperation::RegenerateSidecar,
		EAssetDocumentRuntimeOperation::Sync};
	for (const EAssetDocumentRuntimeOperation Operation : MutationOperations)
	{
		TestOperationAdmissionFailure(
			Test,
			Composition.ExecuteReadOnly(MakeRequest(Operation)),
			TEXTVIEW("CoreRuntimeComposition.MutationInputsRequired"));
	}
	Test.TestEqual(TEXT("read-only admission does not dispatch Host"), Host.DispatchCount, 0);
	Test.TestEqual(TEXT("read-only admission creates no context"), ContextCount, 0);
	Test.TestEqual(TEXT("read-only admission invalidates no handles"), InvalidationCount, 0);
}

void TestMutationRejectsPreflightOperation(FAutomationTestBase& Test)
{
	FHost Host;
	FPrimitiveRegistry Registry;
	FCancellation Cancellation;
	FObserver Observer;
	FVerifier Verifier;
	int32 ContextCount = 0;
	int32 InvalidationCount = 0;
	FAssetDocumentCoreRuntimeComposition Composition(MakeCoreDependencies(
		Host,
		Registry,
		Cancellation,
		Observer,
		&ContextCount,
		&InvalidationCount));
	TestOperationAdmissionFailure(
		Test,
		Composition.ExecuteMutation(
			MakeRequest(EAssetDocumentRuntimeOperation::Preflight),
			MakeOperationAdmissionMutation(Verifier)),
		TEXTVIEW("CoreRuntimeComposition.PreflightRouteNotActive"));
	Test.TestEqual(TEXT("preflight admission does not dispatch Host"), Host.DispatchCount, 0);
	Test.TestEqual(TEXT("preflight admission creates no context"), ContextCount, 0);
	Test.TestEqual(TEXT("preflight admission invalidates no handles"), InvalidationCount, 0);
}

int32 CountInvalidationReason(
	const FMutationCaseResult& Run,
	EAssetDocumentHandleInvalidationReason Reason)
{
	int32 Count = 0;
	for (const FAssetDocumentHandleInvalidation& Invalidation :
		Run.Invalidations)
	{
		if (Invalidation.Reason == Reason)
		{
			++Count;
		}
	}
	return Count;
}

void TestInvalidationIdentity(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Run,
	int32 Index,
	EAssetDocumentHandleInvalidationReason ExpectedReason)
{
	if (!Run.Invalidations.IsValidIndex(Index))
	{
		Test.AddError(TEXT("missing expected handle invalidation"));
		return;
	}
	const FAssetDocumentHandleInvalidation& Event = Run.Invalidations[Index];
	Test.TestEqual(TEXT("invalidation reason is exact"), Event.Reason, ExpectedReason);
	Test.TestEqual(TEXT("invalidation request identity is exact"), Event.RequestId, Run.Request.RequestId);
	Test.TestEqual(TEXT("invalidation transaction identity is exact"), Event.TransactionId, Run.TransactionId);
	Test.TestEqual(TEXT("invalidation runtime revision is exact"), Event.RuntimeRevision, uint64{7});
}

int32 CountOccurrences(const FString& Text, FStringView Needle)
{
	int32 Count = 0;
	int32 Offset = 0;
	while ((Offset = Text.Find(
		FString(Needle),
		ESearchCase::CaseSensitive,
		ESearchDir::FromStart,
		Offset)) != INDEX_NONE)
	{
		++Count;
		Offset += Needle.Len();
	}
	return Count;
}

FString ExtractSourceRange(
	const FString& Source,
	FStringView Begin,
	FStringView End)
{
	const int32 BeginIndex = Source.Find(FString(Begin));
	if (BeginIndex == INDEX_NONE)
	{
		return {};
	}
	const int32 EndIndex = Source.Find(
		FString(End),
		ESearchCase::CaseSensitive,
		ESearchDir::FromStart,
		BeginIndex);
	return EndIndex > BeginIndex
		? Source.Mid(BeginIndex, EndIndex - BeginIndex)
		: FString();
}

bool LoadPrivateHandleAdapterSources(
	FAutomationTestBase& Test,
	FString& OutHeader,
	FString& OutSource)
{
	const FString SourceRoot = FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocument"));
	const bool bLoaded = FFileHelper::LoadFileToString(
			OutHeader,
			*FPaths::Combine(
				SourceRoot,
				TEXT("Public/AssetDocumentCoreRuntimeComposition.h")))
		&& FFileHelper::LoadFileToString(
			OutSource,
			*FPaths::Combine(
				SourceRoot,
				TEXT("Private/Runtime/AssetDocumentCoreRuntimeComposition.cpp")));
	Test.TestTrue(TEXT("private handle adapter sources load"), bLoaded);
	return bLoaded;
}

void TestPrivateHandleIssueBoundary(
	FAutomationTestBase& Test,
	const FString& Source,
	const FString& IssueBody)
{
	Test.TestEqual(
		TEXT("composition owns exactly one private invalidation adapter"),
			CountOccurrences(
				Source,
				TEXTVIEW("class FCoreInvalidationOnlyHandleResolver final")),
		1);
	Test.TestTrue(
		TEXT("private Issue fails closed with the stable diagnostic"),
		IssueBody.Contains(TEXT("CoreRuntimeComposition.HandleIssueUnavailable"))
			&& IssueBody.Contains(TEXT("return Result;")));
	Test.TestFalse(
		TEXT("Issue failure emits no external invalidation"),
		IssueBody.Contains(TEXT("Invalidator(")));
}

void TestPrivateHandleAcquireBoundary(
	FAutomationTestBase& Test,
	const FString& Header,
	const FString& AcquireBody)
{
	Test.TestTrue(
		TEXT("private Acquire fails closed with the stable diagnostic"),
		AcquireBody.Contains(TEXT("CoreRuntimeComposition.HandleLeaseUnavailable"))
			&& AcquireBody.Contains(TEXT("return Result;")));
	Test.TestFalse(
		TEXT("Acquire failure emits no external invalidation"),
		AcquireBody.Contains(TEXT("Invalidator(")));
	Test.TestFalse(
		TEXT("public facade exposes no resolver"),
		Header.Contains(TEXT("IAssetDocumentHandleResolver"))
				|| Header.Contains(TEXT("AcquireVerifiedLease("))
					|| Header.Contains(TEXT(" Issue(")));
}

void TestPrivateHandleAdapterBoundary(FAutomationTestBase& Test)
{
	FString Header;
	FString Source;
	if (!LoadPrivateHandleAdapterSources(Test, Header, Source))
	{
		return;
	}
	const FString IssueBody = ExtractSourceRange(
		Source,
		TEXTVIEW("virtual FAssetDocumentIssuedHandleResult Issue("),
		TEXTVIEW("virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease("));
	const FString AcquireBody = ExtractSourceRange(
		Source,
		TEXTVIEW("virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease("),
		TEXTVIEW("virtual FAssetDocumentHandleInvalidationResult Invalidate("));
	TestPrivateHandleIssueBoundary(Test, Source, IssueBody);
	TestPrivateHandleAcquireBoundary(Test, Header, AcquireBody);
}

void TestPersistenceTemplateValidationBoundary(FAutomationTestBase& Test)
{
	FString Header;
	FString Source;
	if (!LoadPrivateHandleAdapterSources(Test, Header, Source))
	{
		return;
	}
	const FString TemplateValidation = ExtractSourceRange(
		Source,
		TEXTVIEW("ValidatePersistenceTemplateLocations("),
		TEXTVIEW("FAssetDocumentContractValidationResult ValidateMutationInputs("));
	Test.TestFalse(
		TEXT("template validation does not invent a transaction identity"),
		TemplateValidation.Contains(TEXT("FGuid::NewGuid()")));
	Test.TestFalse(
		TEXT("template validation does not call the complete plan validator"),
		TemplateValidation.Contains(
			TEXT("ValidateAssetDocumentPersistencePlan")));
	Test.TestTrue(
		TEXT("template validation forwards target validation"),
		TemplateValidation.Contains(TEXT("ValidatePlanTargets")));
	Test.TestTrue(
		TEXT("template validation forwards staging-root validation"),
		TemplateValidation.Contains(TEXT("ValidatePlanStagingRoot")));
	Test.TestTrue(
		TEXT("template validation forwards staging-isolation validation"),
		TemplateValidation.Contains(TEXT("ValidateStagingIsolation")));
	Test.TestTrue(
		TEXT("template validation forwards journal validation"),
		TemplateValidation.Contains(TEXT("ValidatePlanJournal")));
	Test.TestTrue(
		TEXT("template validation forwards sidecar validation"),
		TemplateValidation.Contains(TEXT("ValidatePlanSidecar")));
	Test.TestTrue(
		TEXT("template validation forwards footprint validation"),
		TemplateValidation.Contains(TEXT("ValidatePersistenceFootprint")));
}

void TestInvalidMutationCase(
	FAutomationTestBase& Test,
	EMutationCaseVariant Variant,
	FStringView ExpectedCode,
	int32 ExpectedHostDispatchCount = 0)
{
	FMutationCaseOptions Options;
	Options.Variant = Variant;
	const FMutationCaseResult Run = RunMutationCase(Test, Options);
	Test.TestEqual(
		TEXT("invalid composition input fails closed"),
		Run.Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(
		TEXT("invalid composition input emits its stable diagnostic once"),
		CountDiagnosticCode(Run.Result, ExpectedCode),
		1);
	Test.TestEqual(
		TEXT("invalid composition input performs only its expected Host dispatches"),
		Run.HostDispatchCount,
		ExpectedHostDispatchCount);
	Test.TestEqual(
		TEXT("invalid composition input opens no mutation capture"),
		Run.OpenCount,
		0);
	Test.TestEqual(
		TEXT("invalid composition input emits no invalidation"),
		Run.Invalidations.Num(),
		0);
	Test.TestEqual(
		TEXT("invalid composition input invokes no verifier"),
		Run.VerifierCallCount,
		0);
	Test.TestEqual(
		TEXT("invalid composition input enlists no footprint"),
		Run.EnlistCount,
		0);
	Test.TestFalse(
		TEXT("invalid composition input writes no package"),
		Run.bInstalledPackageExists);
	Test.TestFalse(
		TEXT("invalid composition input writes no sidecar"),
		Run.bInstalledSidecarExists);
	Test.TestFalse(
		TEXT("invalid composition input writes no sync record"),
		Run.bSyncExisted);
}

void TestMissingContextDependency(
	FAutomationTestBase& Test,
	FHost& Host,
	FPrimitiveRegistry& Registry,
	FCancellation& Cancellation,
	FObserver& Observer)
{
	FAssetDocumentCoreRuntimeDependencies MissingContext{
		Host,
		Registry,
		Cancellation,
		Observer,
		{},
		[](const FAssetDocumentHandleInvalidation& Event)
		{
			return FAssetDocumentHandleInvalidationResult{};
		}};
	FAssetDocumentCoreRuntimeComposition MissingContextComposition(
		MoveTemp(MissingContext));
	Test.TestFalse(
		TEXT("missing context factory invalidates the facade"),
		MissingContextComposition.IsValid());
	Test.TestEqual(
		TEXT("missing context factory diagnostic is stable and singular"),
		CountDiagnosticCode(
			MissingContextComposition.GetValidation().Diagnostics,
			TEXT("CoreRuntimeComposition.MissingContextFactory")),
		1);
}

void TestMissingInvalidatorDependency(
	FAutomationTestBase& Test,
	FHost& Host,
	FPrimitiveRegistry& Registry,
	FCancellation& Cancellation,
	FObserver& Observer)
{
	FAssetDocumentCoreRuntimeDependencies MissingInvalidator{
		Host,
		Registry,
		Cancellation,
		Observer,
		[](const FAssetDocumentRequestContextMetadata& Metadata,
			const FGuid& TransactionId,
			IAssetDocumentAtomicTransaction* ActiveTransaction,
			TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
		{
			return MakeUnique<FRequestContext>(Metadata);
		},
		{}};
	FAssetDocumentCoreRuntimeComposition MissingInvalidatorComposition(
		MoveTemp(MissingInvalidator));
	Test.TestFalse(
		TEXT("missing invalidator invalidates the facade"),
		MissingInvalidatorComposition.IsValid());
	Test.TestEqual(
		TEXT("missing invalidator diagnostic is stable and singular"),
		CountDiagnosticCode(
			MissingInvalidatorComposition.GetValidation().Diagnostics,
			TEXT("CoreRuntimeComposition.MissingHandleInvalidator")),
		1);
}

void TestConstructorDependencyValidation(FAutomationTestBase& Test)
{
	FHost Host;
	FPrimitiveRegistry Registry;
	FCancellation Cancellation;
	FObserver Observer;
	TestMissingContextDependency(
		Test, Host, Registry, Cancellation, Observer);
	TestMissingInvalidatorDependency(
		Test, Host, Registry, Cancellation, Observer);
	Test.TestEqual(
		TEXT("constructor rejection performs no Host dispatch"),
		Host.DispatchCount,
		0);
}

int32 FindTraceEvent(const FMutationCaseResult& Run, FStringView Event)
{
	return Run.Trace.IndexOfByPredicate(
		[Event](const FString& Candidate)
		{
			return Candidate == Event;
			});
}

void TestTraceBefore(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Run,
	FStringView Earlier,
	FStringView Later)
{
	const int32 EarlierIndex = FindTraceEvent(Run, Earlier);
	const int32 LaterIndex = FindTraceEvent(Run, Later);
	Test.TestTrue(
		*FString::Printf(
			TEXT("%.*s precedes %.*s"),
			Earlier.Len(),
			Earlier.GetData(),
			Later.Len(),
			Later.GetData()),
		EarlierIndex != INDEX_NONE && EarlierIndex < LaterIndex);
}

void TestCrossOwnerSuccessFlow(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Run)
{
	const TArray<EAssetDocumentRuntimeRequestPhase> ExpectedPhases{
		EAssetDocumentRuntimeRequestPhase::Validate,
		EAssetDocumentRuntimeRequestPhase::Preflight,
		EAssetDocumentRuntimeRequestPhase::Apply,
		EAssetDocumentRuntimeRequestPhase::Repair,
		EAssetDocumentRuntimeRequestPhase::Persist,
		EAssetDocumentRuntimeRequestPhase::Verify,
		EAssetDocumentRuntimeRequestPhase::Complete};
	Test.TestEqual(
		TEXT("public facade observes the exact success phase flow"),
		Run.ObserverPhases,
		ExpectedPhases);
	Test.TestEqual(
		TEXT("provider enlists the admitted footprint exactly once"),
		Run.EnlistCount,
		1);
	TestTraceBefore(Test, Run, TEXTVIEW("Capture.Open"), TEXTVIEW("Host.Apply"));
	TestTraceBefore(Test, Run, TEXTVIEW("Host.Apply"), TEXTVIEW("Host.Repair"));
	TestTraceBefore(Test, Run, TEXTVIEW("Host.Repair"), TEXTVIEW("Capture.Close"));
	TestTraceBefore(Test, Run, TEXTVIEW("Capture.Close"), TEXTVIEW("Observer.Persist"));
	TestTraceBefore(
		Test,
		Run,
		TEXTVIEW("Observer.Persist"),
		TEXTVIEW("Invalidation.PackageReload"));
	TestTraceBefore(
		Test,
		Run,
		TEXTVIEW("Invalidation.PackageReload"),
		TEXTVIEW("Invalidation.Commit"));
	TestTraceBefore(
		Test,
		Run,
		TEXTVIEW("Invalidation.Commit"),
		TEXTVIEW("Observer.Complete"));
}

void TestCaptureBoundary(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Success,
	const FMutationCaseResult& Failure)
{
	Test.TestEqual(
		TEXT("capture opener receives the generated Apply transaction"),
		Success.TransactionId,
		Success.ApplyTransactionId);
	Test.TestEqual(
		TEXT("Apply and Repair share the generated transaction"),
		Success.ApplyTransactionId,
		Success.RepairTransactionId);
	Test.TestTrue(
		TEXT("capture is open during Apply"),
		Success.bCaptureOpenDuringApply);
	Test.TestTrue(
		TEXT("capture is open during Repair"),
		Success.bCaptureOpenDuringRepair);
	Test.TestEqual(
		TEXT("capture closes while Atomic remains Mutating"),
		Success.StateAtCaptureClose,
		EAssetDocumentAtomicState::Mutating);
	Test.TestFalse(
		TEXT("capture is closed before Persist"),
		Success.bCaptureOpenAtPersist);
	Test.TestFalse(
		TEXT("capture is closed before Commit invalidation"),
		Success.bCaptureOpenAtCommit);
	Test.TestFalse(
		TEXT("capture is closed before Rollback invalidation"),
		Failure.bCaptureOpenAtRollbackInvalidation);
	Test.TestFalse(
		TEXT("capture is closed before rollback action"),
		Failure.bCaptureOpenAtRollbackAction);
	const int32 CloseIndex = FindTraceEvent(Failure, TEXTVIEW("Capture.Close"));
	const int32 PersistIndex =
		FindTraceEvent(Failure, TEXTVIEW("Observer.Persist"));
	const int32 InvalidationIndex =
		FindTraceEvent(Failure, TEXTVIEW("Invalidation.Rollback"));
	const int32 RollbackIndex =
		FindTraceEvent(
			Failure,
			TEXTVIEW("RollbackAction.rollback.package"));
	Test.TestTrue(
		TEXT("failure closes capture before Persist"),
		CloseIndex != INDEX_NONE && CloseIndex < PersistIndex);
	Test.TestTrue(
		TEXT("Persist precedes Rollback invalidation"),
		PersistIndex != INDEX_NONE && PersistIndex < InvalidationIndex);
	Test.TestTrue(
		TEXT("Rollback invalidation precedes rollback action"),
		InvalidationIndex != INDEX_NONE
			&& InvalidationIndex < RollbackIndex);
}

void TestDurableArtifactAssertions(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Run)
{
	Test.TestEqual(
		TEXT("fresh verifier runs once"),
		Run.VerifierCallCount,
		1);
	Test.TestTrue(
		TEXT("staged package exists during fresh verification"),
		Run.bStagedPackageExistsAtVerify);
	Test.TestTrue(
		TEXT("staged sidecar exists during fresh verification"),
		Run.bStagedSidecarExistsAtVerify);
	Test.TestEqual(
		TEXT("fresh verification receives exact revision"),
		Run.VerificationInput.FreshHandleRevision,
		uint64{7});
	Test.TestEqual(
		TEXT("fresh verification receives exact semantic hash"),
		Run.VerificationInput.ExpectedSemanticHash,
		FString(TEXT("semantic:v1")));
	Test.TestEqual(
		TEXT("fresh verification receives exact codec"),
		Run.VerificationInput.ExpectedCodecVersion,
		uint32{1});
	Test.TestTrue(
		TEXT("durable package exists"),
		Run.bInstalledPackageExists && Run.InstalledPackageSize > 0);
	Test.TestTrue(
		TEXT("durable sidecar exists"),
		Run.bInstalledSidecarExists);
	Test.TestEqual(
		TEXT("durable sidecar bytes are exact"),
		Run.InstalledSidecarBytes,
		TArray<uint8>({0x7b, 0x7d}));
}

void TestDurableSyncAssertions(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Run)
{
	Test.TestTrue(
		TEXT("typed sync state loads"),
		Run.bSyncLoadValid);
	Test.TestTrue(
		TEXT("typed sync state exists with encoded bytes"),
		Run.bSyncExisted
			&& Run.SyncRecord.IsSet()
			&& !Run.EncodedSyncBytes.IsEmpty());
	if (Run.SyncRecord.IsSet())
	{
		Test.TestEqual(
			TEXT("typed sync state preserves request identity"),
			Run.SyncRecord->RequestId,
			Run.Request.RequestId);
		Test.TestEqual(
			TEXT("typed sync state preserves transaction identity"),
			Run.SyncRecord->TransactionId,
			Run.TransactionId);
		Test.TestEqual(
			TEXT("typed sync state preserves journal identity"),
			Run.SyncRecord->JournalId,
			FString(TEXT("core-composition")));
		Test.TestEqual(
			TEXT("typed sync state preserves asset evidence"),
			Run.SyncRecord->LastDurable.Asset.SemanticHash,
			FString(TEXT("asset:v1")));
		Test.TestEqual(
			TEXT("typed sync state preserves sidecar evidence"),
			Run.SyncRecord->LastDurable.Sidecar.SemanticHash,
			FString(TEXT("semantic:v1")));
	}
}

void TestDurableSuccess(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Run)
{
	Test.TestEqual(
		TEXT("successful durable path reaches Complete"),
		Run.Result.FinalPhase,
		EAssetDocumentRuntimeRequestPhase::Complete);
	TestDurableArtifactAssertions(Test, Run);
	TestDurableSyncAssertions(Test, Run);
}

void TestPersistenceFailureHasNoPartialSuccess(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Run)
{
	const TArray<EAssetDocumentRuntimeRequestPhase> ExpectedPhases{
		EAssetDocumentRuntimeRequestPhase::Validate,
		EAssetDocumentRuntimeRequestPhase::Preflight,
		EAssetDocumentRuntimeRequestPhase::Apply,
		EAssetDocumentRuntimeRequestPhase::Repair,
		EAssetDocumentRuntimeRequestPhase::Persist,
		EAssetDocumentRuntimeRequestPhase::Failed};
	Test.TestEqual(
		TEXT("persistence failure reaches Failed"),
		Run.Result.FinalPhase,
		EAssetDocumentRuntimeRequestPhase::Failed);
	Test.TestEqual(
		TEXT("persistence failure stops at exact observer phases"),
		Run.ObserverPhases,
		ExpectedPhases);
	Test.TestEqual(
		TEXT("failing fresh verifier runs once"),
		Run.VerifierCallCount,
		1);
	Test.TestFalse(
		TEXT("persistence failure leaves package absent"),
		Run.bInstalledPackageExists);
	Test.TestFalse(
		TEXT("persistence failure leaves sidecar absent"),
		Run.bInstalledSidecarExists);
	Test.TestTrue(
		TEXT("persistence failure leaves valid empty sync preimage"),
		Run.bSyncLoadValid
			&& !Run.bSyncExisted
			&& !Run.SyncRecord.IsSet()
			&& Run.EncodedSyncBytes.IsEmpty());
	Test.TestFalse(
		TEXT("persistence failure leaves no staged files"),
		Run.bStagingHasFiles);
}

TArray<EAssetDocumentRuntimeRequestPhase> ExpectedFailurePhases(
	EAssetDocumentRuntimeRequestPhase FailedPhase)
{
	TArray<EAssetDocumentRuntimeRequestPhase> Phases{
		EAssetDocumentRuntimeRequestPhase::Validate};
	if (FailedPhase != EAssetDocumentRuntimeRequestPhase::Validate)
	{
		Phases.Add(EAssetDocumentRuntimeRequestPhase::Preflight);
	}
	if (FailedPhase == EAssetDocumentRuntimeRequestPhase::Apply
		|| FailedPhase == EAssetDocumentRuntimeRequestPhase::Repair)
	{
		Phases.Add(EAssetDocumentRuntimeRequestPhase::Apply);
	}
	if (FailedPhase == EAssetDocumentRuntimeRequestPhase::Repair)
	{
		Phases.Add(EAssetDocumentRuntimeRequestPhase::Repair);
	}
	Phases.Add(EAssetDocumentRuntimeRequestPhase::Failed);
	return Phases;
}

void TestLiveFailureTrace(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Run,
	bool bExpectedLiveTransaction)
{
	if (!bExpectedLiveTransaction)
	{
		return;
	}
	TestTraceBefore(
		Test,
		Run,
		TEXTVIEW("Capture.Close"),
		TEXTVIEW("Invalidation.Rollback"));
}

void TestInjectedPhaseFailure(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Run,
	EAssetDocumentRuntimeRequestPhase FailedPhase,
	FStringView DiagnosticCode,
	int32 ExpectedContextAttempts,
	int32 ExpectedHostCalls,
	bool bExpectedLiveTransaction)
{
	Test.TestEqual(
		TEXT("injected phase failure returns Failed"),
		Run.Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(
		TEXT("injected phase failure is emitted once"),
		CountDiagnosticCode(Run.Result, DiagnosticCode),
		1);
	Test.TestEqual(
		TEXT("phase context is attempted without retry"),
		Run.ContextAttempts.Num(),
		ExpectedContextAttempts);
	Test.TestEqual(
		TEXT("Host dispatch stops without retry"),
		Run.HostDispatchCount,
		ExpectedHostCalls);
	Test.TestEqual(
		TEXT("no later observer phase runs"),
		Run.ObserverPhases,
		ExpectedFailurePhases(FailedPhase));
	const int32 ExpectedLiveCount = bExpectedLiveTransaction ? 1 : 0;
	const int32 ExpectedRollbackActions =
		FailedPhase == EAssetDocumentRuntimeRequestPhase::Repair ? 1 : 0;
	Test.TestEqual(
		TEXT("capture opens only after Atomic begin"),
		Run.OpenCount,
		ExpectedLiveCount);
	Test.TestEqual(
		TEXT("live failure closes capture once"),
		Run.CloseCount,
		ExpectedLiveCount);
	Test.TestEqual(
		TEXT("only successfully enlisted rollback actions run"),
		Run.RollbackCount,
		ExpectedRollbackActions);
	Test.TestEqual(
		TEXT("live transaction chooses one rollback terminal"),
		CountInvalidationReason(
			Run,
			EAssetDocumentHandleInvalidationReason::Rollback),
		ExpectedLiveCount);
	Test.TestEqual(
		TEXT("phase failure never reaches persistence"),
		Run.VerifierCallCount,
		0);
	TestLiveFailureTrace(Test, Run, bExpectedLiveTransaction);
}

void TestContextCreationFailuresThroughFacade(FAutomationTestBase& Test)
{
	struct FCase
	{
		EAssetDocumentRuntimeRequestPhase Phase;
		int32 HostCalls;
		bool bLive;
	};
	const FCase Cases[]{
		{EAssetDocumentRuntimeRequestPhase::Validate, 0, false},
		{EAssetDocumentRuntimeRequestPhase::Preflight, 1, false},
		{EAssetDocumentRuntimeRequestPhase::Apply, 2, true},
		{EAssetDocumentRuntimeRequestPhase::Repair, 3, true}};
	for (const FCase& Case : Cases)
	{
		FMutationCaseOptions Options;
		Options.FailContextPhase = Case.Phase;
		const FMutationCaseResult Run = RunMutationCase(Test, Options);
		TestInjectedPhaseFailure(
			Test,
			Run,
			Case.Phase,
			TEXTVIEW("Context.InjectedFailure"),
			Case.HostCalls + 1,
			Case.HostCalls,
			Case.bLive);
	}
}

void TestHostFailuresThroughFacade(FAutomationTestBase& Test)
{
	struct FCase
	{
		EAssetDocumentRuntimeRequestPhase Phase;
		int32 HostCalls;
		bool bLive;
	};
	const FCase Cases[]{
		{EAssetDocumentRuntimeRequestPhase::Validate, 1, false},
		{EAssetDocumentRuntimeRequestPhase::Preflight, 2, false},
		{EAssetDocumentRuntimeRequestPhase::Apply, 3, true},
		{EAssetDocumentRuntimeRequestPhase::Repair, 4, true}};
	for (const FCase& Case : Cases)
	{
		FMutationCaseOptions Options;
		Options.FailHostPhase = Case.Phase;
		const FMutationCaseResult Run = RunMutationCase(Test, Options);
		TestInjectedPhaseFailure(
			Test,
			Run,
			Case.Phase,
			TEXTVIEW("Host.InjectedFailure"),
			Case.HostCalls,
			Case.HostCalls,
			Case.bLive);
	}
}

void TestFailedCaptureOpenerThroughFacade(FAutomationTestBase& Test)
{
	FMutationCaseOptions Options;
	Options.bFailCaptureOpen = true;
	const FMutationCaseResult Run = RunMutationCase(Test, Options);
	Test.TestEqual(
		TEXT("failed opener returns Failed"),
		Run.Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(
		TEXT("failed opener is called once"),
		Run.OpenCount,
		1);
	Test.TestTrue(
		TEXT("failed opener receives a generated transaction"),
		Run.TransactionId.IsValid());
	Test.TestEqual(
		TEXT("failed opener creates no lease to close"),
		Run.CloseCount,
		0);
	Test.TestEqual(
		TEXT("failed opener has no enlisted rollback action"),
		Run.RollbackCount,
		0);
	Test.TestEqual(
		TEXT("failed opener chooses one rollback terminal"),
		CountInvalidationReason(
			Run,
			EAssetDocumentHandleInvalidationReason::Rollback),
		1);
	Test.TestEqual(
		TEXT("failed opener diagnostic is exact"),
		CountDiagnosticCode(Run.Result, TEXTVIEW("Capture.InjectedFailure")),
		1);
	Test.TestEqual(
		TEXT("failed opener stops after Validate and Preflight"),
		Run.HostDispatchCount,
		2);
}

void TestUndeclaredProviderFootprintThroughFacade(FAutomationTestBase& Test)
{
	FMutationCaseOptions UndeclaredOptions;
	UndeclaredOptions.bUseUndeclaredProviderFootprint = true;
	const FMutationCaseResult Undeclared =
		RunMutationCase(Test, UndeclaredOptions);
	Test.TestEqual(
		TEXT("undeclared provider footprint fails at enlist"),
		CountDiagnosticCode(
			Undeclared.Result,
			TEXTVIEW("Atomic.FootprintNotDeclared")),
		1);
	Test.TestEqual(
		TEXT("undeclared provider footprint enlists once"),
		Undeclared.EnlistCount,
		1);
	Test.TestEqual(
		TEXT("undeclared footprint enlists no rollback action"),
		Undeclared.RollbackCount,
		0);
	Test.TestEqual(
		TEXT("undeclared footprint chooses one rollback terminal"),
		CountInvalidationReason(
			Undeclared,
			EAssetDocumentHandleInvalidationReason::Rollback),
		1);
	Test.TestEqual(
		TEXT("undeclared provider footprint never persists"),
		Undeclared.VerifierCallCount,
		0);
}

void TestIncompleteProviderCoverageThroughFacade(FAutomationTestBase& Test)
{
	FMutationCaseOptions IncompleteOptions;
	IncompleteOptions.bIncompleteProviderCoverage = true;
	const FMutationCaseResult Incomplete =
		RunMutationCase(Test, IncompleteOptions);
	Test.TestEqual(
		TEXT("incomplete provider coverage fails only at MarkReady"),
		CountDiagnosticCode(
			Incomplete.Result,
			TEXTVIEW("Atomic.IncompleteFootprint")),
		1);
	Test.TestEqual(
		TEXT("incomplete provider coverage runs all Host phases"),
		Incomplete.HostDispatchCount,
		4);
	Test.TestEqual(
		TEXT("incomplete provider coverage enlists once"),
		Incomplete.EnlistCount,
		1);
	Test.TestEqual(
		TEXT("incomplete coverage rolls back only its enlisted action"),
		Incomplete.RollbackCount,
		1);
	Test.TestEqual(
		TEXT("incomplete coverage chooses one rollback terminal"),
		CountInvalidationReason(
			Incomplete,
			EAssetDocumentHandleInvalidationReason::Rollback),
		1);
	Test.TestEqual(
		TEXT("incomplete provider coverage never persists"),
		Incomplete.VerifierCallCount,
		0);
}

void TestProviderEnlistFailuresThroughFacade(FAutomationTestBase& Test)
{
	TestUndeclaredProviderFootprintThroughFacade(Test);
	TestIncompleteProviderCoverageThroughFacade(Test);
}

void TestReverseRollbackActionsThroughFacade(FAutomationTestBase& Test)
{
	FMutationCaseOptions Options;
	Options.bFailVerification = true;
	Options.bUseTwoRollbackActions = true;
	const FMutationCaseResult Run = RunMutationCase(Test, Options);
	Test.TestEqual(
		TEXT("two admitted rollback actions each run once"),
		Run.RollbackCount,
		2);
	TestTraceBefore(
		Test,
		Run,
		TEXTVIEW("Invalidation.Rollback"),
		TEXTVIEW("RollbackAction.rollback.other"));
	TestTraceBefore(
		Test,
		Run,
		TEXTVIEW("RollbackAction.rollback.other"),
		TEXTVIEW("RollbackAction.rollback.package"));
}

void TestPreRegisteredActionRollsBackAfterApplyFailure(
	FAutomationTestBase& Test)
{
	FMutationCaseOptions Options;
	Options.bFailAfterApplyEnlist = true;
	const FMutationCaseResult Run = RunMutationCase(Test, Options);
	Test.TestEqual(
		TEXT("post-enlist Apply failure is projected once"),
		CountDiagnosticCode(
			Run.Result,
			TEXTVIEW("Host.InjectedApplyFailureAfterEnlist")),
		1);
	Test.TestEqual(
		TEXT("post-enlist Apply failure enlisted footprint once"),
		Run.EnlistCount,
		1);
	Test.TestEqual(
		TEXT("pre-registered action rolls back after Apply failure"),
		Run.RollbackCount,
		1);
	Test.TestEqual(
		TEXT("post-enlist Apply failure chooses one rollback terminal"),
		CountInvalidationReason(
			Run,
			EAssetDocumentHandleInvalidationReason::Rollback),
		1);
	Test.TestEqual(
		TEXT("post-enlist Apply failure never reaches Repair"),
		Run.HostDispatchCount,
		3);
	TestTraceBefore(
		Test,
		Run,
		TEXTVIEW("Invalidation.Rollback"),
		TEXTVIEW("RollbackAction.rollback.package"));
}

void TestCrossOwnerFailureGroup(FAutomationTestBase& Test)
{
	TestContextCreationFailuresThroughFacade(Test);
	TestHostFailuresThroughFacade(Test);
	TestFailedCaptureOpenerThroughFacade(Test);
	TestProviderEnlistFailuresThroughFacade(Test);
	TestPreRegisteredActionRollsBackAfterApplyFailure(Test);
	TestReverseRollbackActionsThroughFacade(Test);
}

void TestRollbackAndFootprintInputs(FAutomationTestBase& Test)
{
	FMutationCaseOptions EmptyOptions;
	EmptyOptions.Variant = EMutationCaseVariant::EmptyRollbackActions;
	const FMutationCaseResult Empty = RunMutationCase(Test, EmptyOptions);
	Test.TestEqual(
		TEXT("empty rollback actions fail at footprint coverage"),
		CountDiagnosticCode(
			Empty.Result,
			TEXTVIEW("Atomic.IncompleteRollbackCoverage")),
		1);
	Test.TestEqual(
		TEXT("empty rollback actions reach Apply once"),
		Empty.HostDispatchCount,
		3);
	Test.TestEqual(
		TEXT("empty rollback actions open one live capture"),
		Empty.OpenCount,
		1);
	Test.TestEqual(
		TEXT("empty rollback actions attempt one footprint enlistment"),
		Empty.EnlistCount,
		1);
	Test.TestEqual(
		TEXT("empty rollback actions execute no rollback target"),
		Empty.RollbackCount,
		0);
	Test.TestEqual(
		TEXT("empty rollback actions choose one rollback terminal"),
		CountInvalidationReason(
			Empty,
			EAssetDocumentHandleInvalidationReason::Rollback),
		1);
	Test.TestEqual(
		TEXT("empty rollback actions never reach persistence"),
		Empty.VerifierCallCount,
		0);
	TestInvalidMutationCase(
		Test,
		EMutationCaseVariant::DuplicateRollbackActions,
		TEXTVIEW("RollbackActionComposition.DuplicateComponentId"));
	TestInvalidMutationCase(
		Test,
		EMutationCaseVariant::OrderedFootprintMismatch,
		TEXTVIEW("CoreRuntimeComposition.FootprintMismatch"));
}

void TestLateBoundIdentityMismatch(
	FAutomationTestBase& Test,
	EMutationCaseVariant Variant)
{
	FMutationCaseOptions Options;
	Options.Variant = Variant;
	const FMutationCaseResult Run = RunMutationCase(Test, Options);
	Test.TestEqual(
		TEXT("late-bound identity mismatch fails closed"),
		Run.Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(
		TEXT("late-bound identity mismatch diagnostic is stable"),
		CountDiagnosticCode(
			Run.Result,
			TEXTVIEW(
				"CoreRuntimeComposition.PersistenceTargetIdentityMismatch")),
		1);
	Test.TestEqual(
		TEXT("identity is checked after all Host mutation phases"),
		Run.HostDispatchCount,
		4);
	Test.TestEqual(
		TEXT("late-bound mismatch opens one capture"),
		Run.OpenCount,
		1);
	Test.TestEqual(
		TEXT("late-bound mismatch closes one capture"),
		Run.CloseCount,
		1);
	Test.TestEqual(
		TEXT("late-bound mismatch invokes its binder once"),
		Run.BinderCount,
		1);
	Test.TestEqual(
		TEXT("late-bound mismatch rolls back once"),
		Run.RollbackCount,
		1);
	Test.TestEqual(
		TEXT("late-bound mismatch never observes Persist"),
		Run.PersistencePhaseCount,
		0);
	Test.TestEqual(
		TEXT("late-bound mismatch never invokes fresh verification"),
		Run.VerifierCallCount,
		0);
	Test.TestEqual(
		TEXT("late-bound mismatch selects only rollback invalidation"),
		CountInvalidationReason(
			Run,
			EAssetDocumentHandleInvalidationReason::Rollback),
		1);
}

void TestIdentityInputs(FAutomationTestBase& Test)
{
	TestInvalidMutationCase(
		Test,
		EMutationCaseVariant::PreflightRequestMismatch,
		TEXTVIEW("CoreRuntimeComposition.IdentityMismatch"));
	TestInvalidMutationCase(
		Test,
		EMutationCaseVariant::PersistenceRequestMismatch,
		TEXTVIEW("CoreRuntimeComposition.IdentityMismatch"));
	TestInvalidMutationCase(
		Test,
		EMutationCaseVariant::PreflightRevisionMismatch,
		TEXTVIEW("CoreRuntimeComposition.IdentityMismatch"));
	TestInvalidMutationCase(
		Test,
		EMutationCaseVariant::CallerTransactionId,
		TEXTVIEW("CoreRuntimeComposition.IdentityMismatch"));
	TestLateBoundIdentityMismatch(
		Test,
		EMutationCaseVariant::PackageTargetMismatch);
	TestLateBoundIdentityMismatch(
		Test,
		EMutationCaseVariant::AssetOuterMismatch);
}

void TestNoBindingResidue(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Run)
{
	Test.TestFalse(
		TEXT("binding failure leaves package uninstalled"),
		Run.bInstalledPackageExists);
	Test.TestFalse(
		TEXT("binding failure leaves sidecar uninstalled"),
		Run.bInstalledSidecarExists);
	Test.TestTrue(
		TEXT("binding failure leaves an empty sync preimage"),
		Run.bSyncLoadValid
			&& !Run.bSyncExisted
			&& !Run.SyncRecord.IsSet()
			&& Run.EncodedSyncBytes.IsEmpty());
	Test.TestFalse(
		TEXT("binding failure leaves no staged files"),
		Run.bStagingHasFiles);
}

void TestPersistenceTargetFailureCase(
	FAutomationTestBase& Test,
	EMutationCaseVariant Variant,
	FStringView ExpectedCode,
	bool bPreBegin,
	bool bExpectBinderDiagnostic)
{
	FMutationCaseOptions Options;
	Options.Variant = Variant;
	const FMutationCaseResult Run = RunMutationCase(Test, Options);
	Test.TestEqual(
		TEXT("binding failure selects Failed terminal"),
		Run.Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(
		TEXT("binding failure selects Failed phase"),
		Run.Result.FinalPhase,
		EAssetDocumentRuntimeRequestPhase::Failed);
	Test.TestEqual(
		TEXT("binding failure diagnostic code is stable and singular"),
		CountDiagnosticCode(Run.Result, ExpectedCode),
		1);
	const FAssetDocumentRuntimeDiagnostic* Diagnostic =
		Run.Result.Diagnostics.FindByPredicate(
			[ExpectedCode](const FAssetDocumentRuntimeDiagnostic& Candidate)
			{
				return Candidate.Code == ExpectedCode;
			});
	Test.TestTrue(
		TEXT("binding failure diagnostic uses the frozen path"),
		Diagnostic != nullptr
			&& Diagnostic->Path
				== TEXT("/CoreRuntimeComposition/PersistenceTarget"));
	Test.TestEqual(
		TEXT("binding failure Host dispatch count is exact"),
		Run.HostDispatchCount,
		bPreBegin ? 0 : 4);
	Test.TestEqual(
		TEXT("binding failure Host Apply count is exact"),
		Run.HostApplyCount,
		bPreBegin ? 0 : 1);
	Test.TestEqual(
		TEXT("binding failure binder count is exact"),
		Run.BinderCount,
		bPreBegin ? 0 : 1);
	Test.TestEqual(
		TEXT("binding failure persistence count is zero"),
		Run.PersistencePhaseCount,
		0);
	Test.TestEqual(
		TEXT("binding failure verifier count is zero"),
		Run.VerifierCallCount,
		0);
	Test.TestEqual(
		TEXT("binding failure commit count is zero"),
		Run.CommitCount,
		0);
	Test.TestEqual(
		TEXT("binding failure rollback count is exact"),
		Run.RollbackCount,
		bPreBegin ? 0 : 1);
	Test.TestEqual(
		TEXT("binding failure capture open count is exact"),
		Run.OpenCount,
		bPreBegin ? 0 : 1);
	Test.TestEqual(
		TEXT("binding failure capture close count is exact"),
		Run.CloseCount,
		bPreBegin ? 0 : 1);
	if (!bPreBegin)
	{
		TestTraceBefore(
			Test,
			Run,
			TEXTVIEW("Capture.Close"),
			TEXTVIEW("PersistenceTarget.Bind"));
		TestTraceBefore(
			Test,
			Run,
			TEXTVIEW("PersistenceTarget.Bind"),
			TEXTVIEW("Invalidation.Rollback"));
		TestTraceBefore(
			Test,
			Run,
			TEXTVIEW("Invalidation.Rollback"),
			TEXTVIEW("RollbackAction.rollback.package"));
	}
	Test.TestEqual(
		TEXT("binder diagnostic preservation is exact"),
		CountDiagnosticCode(
			Run.Result,
			TEXTVIEW("Test.PersistenceTargetBinderRejected")),
		bExpectBinderDiagnostic ? 1 : 0);
	TestNoBindingResidue(Test, Run);
}

void TestPersistenceTargetFailureMatrix(FAutomationTestBase& Test)
{
	struct FCase
	{
		EMutationCaseVariant Variant;
		const TCHAR* ExpectedCode;
		bool bPreBegin;
		bool bExpectBinderDiagnostic;
	};
	const FCase Cases[]{
		{EMutationCaseVariant::MissingPersistenceTargetBinder,
			TEXT("CoreRuntimeComposition.PersistenceTargetBinderMissing"),
			true,
			false},
		{EMutationCaseVariant::EmptyPersistenceTargetBinding,
			TEXT("CoreRuntimeComposition.PersistenceTargetBindingFailed"),
			false,
			false},
		{EMutationCaseVariant::DiagnosticAndEmptyPersistenceTargetBinding,
			TEXT("CoreRuntimeComposition.PersistenceTargetBindingFailed"),
			false,
			true},
		{EMutationCaseVariant::PersistenceTargetRequestMismatch,
			TEXT("CoreRuntimeComposition.PersistenceTargetIdentityMismatch"),
			false,
			false},
		{EMutationCaseVariant::PersistenceTargetTransactionMismatch,
			TEXT("CoreRuntimeComposition.PersistenceTargetIdentityMismatch"),
			false,
			false},
		{EMutationCaseVariant::PersistenceTargetRevisionMismatch,
			TEXT("CoreRuntimeComposition.PersistenceTargetIdentityMismatch"),
			false,
			false},
		{EMutationCaseVariant::NullPersistenceTargetAsset,
			TEXT("CoreRuntimeComposition.PersistenceTargetIdentityMismatch"),
			false,
			false},
		{EMutationCaseVariant::NullPersistenceTargetPackage,
			TEXT("CoreRuntimeComposition.PersistenceTargetIdentityMismatch"),
			false,
			false},
		{EMutationCaseVariant::PersistenceTargetObjectPathMismatch,
			TEXT("CoreRuntimeComposition.PersistenceTargetIdentityMismatch"),
			false,
			false},
		{EMutationCaseVariant::PersistenceTargetClassMismatch,
			TEXT("CoreRuntimeComposition.PersistenceTargetIdentityMismatch"),
			false,
			false},
		{EMutationCaseVariant::AssetOuterMismatch,
			TEXT("CoreRuntimeComposition.PersistenceTargetIdentityMismatch"),
			false,
			false},
		{EMutationCaseVariant::PackageTargetMismatch,
			TEXT("CoreRuntimeComposition.PersistenceTargetIdentityMismatch"),
			false,
			false}};
	for (const FCase& Case : Cases)
	{
		TestPersistenceTargetFailureCase(
			Test,
			Case.Variant,
			Case.ExpectedCode,
			Case.bPreBegin,
			Case.bExpectBinderDiagnostic);
	}
}

void TestMovedMutationInputFailsBeforeBegin(FAutomationTestBase& Test)
{
	const FMovedMutationInputReuseResult Reuse =
		RunMovedMutationInputReuseCase(Test);
	Test.TestEqual(
		TEXT("first moved-input execution succeeds"),
		Reuse.First.Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Succeeded);
	Test.TestEqual(
		TEXT("first moved-input execution binds once"),
		Reuse.First.BinderCount,
		1);
	Test.TestEqual(
		TEXT("first moved-input execution persists once"),
		Reuse.First.PersistencePhaseCount,
		1);
	Test.TestEqual(
		TEXT("moved mutation input reuse fails before Begin"),
		Reuse.Second.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(
		TEXT("moved mutation input reuse selects Failed phase"),
		Reuse.Second.FinalPhase,
		EAssetDocumentRuntimeRequestPhase::Failed);
	Test.TestEqual(
		TEXT("moved mutation input reuse reports missing binder"),
		CountDiagnosticCode(
			Reuse.Second,
			TEXTVIEW(
				"CoreRuntimeComposition.PersistenceTargetBinderMissing")),
		1);
	Test.TestEqual(
		TEXT("moved mutation input reuse performs no Host Apply"),
		Reuse.HostApplyDelta,
		0);
	Test.TestEqual(
		TEXT("moved mutation input reuse performs no binding"),
		Reuse.BinderDelta,
		0);
	Test.TestEqual(
		TEXT("moved mutation input reuse performs no persistence"),
		Reuse.PersistenceDelta,
		0);
	Test.TestEqual(
		TEXT("moved mutation input reuse performs no verification"),
		Reuse.VerifierDelta,
		0);
	Test.TestEqual(
		TEXT("moved mutation input reuse performs no commit"),
		Reuse.CommitDelta,
		0);
	Test.TestEqual(
		TEXT("moved mutation input reuse performs no rollback"),
		Reuse.RollbackDelta,
		0);
}

void TestStorageInputs(FAutomationTestBase& Test)
{
	TestInvalidMutationCase(
		Test,
		EMutationCaseVariant::EmptyJournalRoot,
		TEXTVIEW("CoreRuntimeComposition.InvalidStorageRoot"));
	TestInvalidMutationCase(
		Test,
		EMutationCaseVariant::EmptyJournalBackupRoot,
		TEXTVIEW("CoreRuntimeComposition.InvalidStorageRoot"));
	TestInvalidMutationCase(
		Test,
		EMutationCaseVariant::EmptySyncStateRoot,
		TEXTVIEW("CoreRuntimeComposition.InvalidStorageRoot"));
	TestInvalidMutationCase(
		Test,
		EMutationCaseVariant::EmptySyncRegionId,
		TEXTVIEW("CoreRuntimeComposition.InvalidStorageRoot"));
	TestInvalidMutationCase(
		Test,
		EMutationCaseVariant::ZeroFreshRevision,
		TEXTVIEW("CoreRuntimeComposition.InvalidPersistence"));
	TestInvalidMutationCase(
		Test,
		EMutationCaseVariant::MissingCaptureOpener,
		TEXTVIEW("CoreRuntimeComposition.MissingMutationCaptureOpener"));
}

void TestInvalidInputGroup(FAutomationTestBase& Test)
{
	TestRollbackAndFootprintInputs(Test);
	TestIdentityInputs(Test);
	TestStorageInputs(Test);
}

void TestBoundChangedSet(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Success)
{
	const FString ExpectedPhases[]{
		TEXT("Validate"),
		TEXT("Preflight"),
		TEXT("Apply"),
		TEXT("Repair")};
	Test.TestEqual(
		TEXT("binder receives the complete validated Host changed-set"),
		Success.BinderChangedSet.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(ExpectedPhases)));
	for (int32 Index = 0;
		Index < static_cast<int32>(UE_ARRAY_COUNT(ExpectedPhases))
			&& Success.BinderChangedSet.IsValidIndex(Index);
		++Index)
	{
		const FAssetDocumentChangedRecord& Changed =
			Success.BinderChangedSet[Index];
		const FString& Phase = ExpectedPhases[Index];
		Test.TestEqual(
			TEXT("binder changed-set authority is exact"),
			Changed.AuthorityId,
			FString(TEXT("host.runtime")));
		Test.TestEqual(
			TEXT("binder changed-set stable identity is phase ordered"),
			Changed.StableObjectId,
			TEXT("phase:") + Phase);
		Test.TestEqual(
			TEXT("binder changed-set path is phase ordered"),
			Changed.Path,
			TEXT("/ChangedSet/") + Phase);
		Test.TestEqual(
			TEXT("binder changed-set kind is phase ordered"),
			Changed.ChangeKind,
			Phase);
	}
}

void TestBoundPersistenceTargetRequest(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Success)
{
	Test.TestEqual(
		TEXT("binder receives the exact request identity"),
		Success.BinderRequestId,
		Success.Request.RequestId);
	Test.TestEqual(
		TEXT("binder receives the live transaction identity"),
		Success.BinderTransactionId,
		Success.TransactionId);
	Test.TestEqual(
		TEXT("binder receives the pinned runtime revision"),
		Success.BinderRuntimeRevision,
		Success.Request.ExpectedRuntimeRevision);
	Test.TestEqual(
		TEXT("binder receives the canonical object path"),
		Success.BinderCanonicalObjectPath,
		Success.Request.TargetAssetPath);
	Test.TestEqual(
		TEXT("binder receives the exact requested asset class"),
		Success.BinderExactAssetClassPath,
		Success.Request.RequestedAssetClassPath);
	Test.TestEqual(
		TEXT("binder receives the admitted target package"),
		Success.BinderTargetPackagePath,
		FPackageName::ObjectPathToPackageName(
			Success.Request.TargetAssetPath));
	TestBoundChangedSet(Test, Success);
}

void TestSuccessfulMutationResult(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Success,
	bool bExpectedTargetAbsentAtCaptureOpen)
{
	Test.TestEqual(
		TEXT("target entry state matches Create or Update"),
		Success.bTargetAbsentAtCaptureOpen,
		bExpectedTargetAbsentAtCaptureOpen);
	Test.TestTrue(
		TEXT("Apply executes while mutation capture is open"),
		Success.bCaptureOpenDuringApply);
	Test.TestEqual(
		TEXT("mutation executes Apply exactly once"),
		Success.HostApplyCount,
		1);
	Test.TestEqual(
		TEXT("mutation invokes the binder exactly once"),
		Success.BinderCount,
		1);
	Test.TestFalse(
		TEXT("binder runs only after mutation capture closes"),
		Success.bCaptureOpenAtBind);
	Test.TestTrue(
		TEXT("binder observes the admitted live target"),
		Success.bTargetExistsAtBind);
	Test.TestEqual(
		TEXT("validated binding reaches Persist exactly once"),
		Success.PersistencePhaseCount,
		1);
	Test.TestEqual(
		TEXT("validated binding reaches fresh verification once"),
		Success.VerifierCallCount,
		1);
	Test.TestEqual(
		TEXT("validated binding reaches commit once"),
		Success.CommitCount,
		1);
	Test.TestEqual(
		TEXT("validated binding does not roll back"),
		Success.RollbackCount,
		0);
	Test.TestEqual(
		TEXT("validated binding emits no binding-failed diagnostic"),
		CountDiagnosticCode(
			Success.Result,
			TEXTVIEW(
				"CoreRuntimeComposition.PersistenceTargetBindingFailed")),
		0);
	TestBoundPersistenceTargetRequest(Test, Success);
	Test.TestTrue(
		TEXT("staging preserves live dirty state through Commit invalidation"),
		Success.bPackageDirtyAtCommit);
	Test.TestFalse(
		TEXT("durable success completes dirty state of the saved live package"),
		Success.bPackageDirtyAfterExecute);
	Test.TestEqual(
		TEXT("valid composition reaches durable success through existing owners"),
		Success.Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Succeeded);
	Test.TestEqual(
		TEXT("successful composition opens capture once"),
		Success.OpenCount,
		1);
	Test.TestEqual(
		TEXT("successful composition closes capture once"),
		Success.CloseCount,
		1);
	Test.TestEqual(
		TEXT("successful composition does not roll back"),
		Success.RollbackCount,
		0);
	Test.TestEqual(
		TEXT("successful composition forwards two invalidations"),
		Success.Invalidations.Num(),
		2);
	TestInvalidationIdentity(
		Test,
		Success,
		0,
		EAssetDocumentHandleInvalidationReason::PackageReload);
	TestInvalidationIdentity(
		Test,
		Success,
		1,
		EAssetDocumentHandleInvalidationReason::Commit);
}

void TestFailedMutationResult(
	FAutomationTestBase& Test,
	const FMutationCaseResult& Failure)
{
	// This harness action records rollback only; completion must not clean
	// the package on failure or substitute for a provider's dirty restore.
	Test.TestTrue(
		TEXT("failed durable verification does not run success dirty completion"),
		Failure.bPackageDirtyAfterExecute);
	Test.TestEqual(
		TEXT("fresh verification failure selects failed terminal"),
		Failure.Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(
		TEXT("failed composition opens capture once"),
		Failure.OpenCount,
		1);
	Test.TestEqual(
		TEXT("failed composition closes capture once"),
		Failure.CloseCount,
		1);
	Test.TestEqual(
		TEXT("failed composition rolls back once"),
		Failure.RollbackCount,
		1);
	Test.TestEqual(
		TEXT("failed persistence enlists once before rollback"),
		Failure.EnlistCount,
		1);
	Test.TestEqual(
		TEXT("failed composition forwards two invalidations"),
		Failure.Invalidations.Num(),
		2);
	TestInvalidationIdentity(
		Test,
		Failure,
		0,
		EAssetDocumentHandleInvalidationReason::PackageReload);
	TestInvalidationIdentity(
		Test,
		Failure,
		1,
		EAssetDocumentHandleInvalidationReason::Rollback);
	Test.TestEqual(
		TEXT("existing persistence failure is preserved once"),
		CountDiagnosticCode(
			Failure.Result,
			TEXT("Persistence.FreshLoadFailed")),
		1);
	Test.TestEqual(
		TEXT("coordinator terminal diagnostic is emitted once"),
		CountDiagnosticCode(
			Failure.Result,
			TEXT("RequestCoordinator.InvalidDurableReceipt")),
		1);
}

void TestSuccessFailureGroup(FAutomationTestBase& Test)
{
	const FMutationCaseResult Success = RunMutationCase(Test);
	TestSuccessfulMutationResult(Test, Success, true);

	FMutationCaseOptions UpdateOptions;
	UpdateOptions.bPreExistingTarget = true;
	const FMutationCaseResult Update =
		RunMutationCase(Test, UpdateOptions);
	TestSuccessfulMutationResult(Test, Update, false);
	TestDurableSuccess(Test, Update);

	FMutationCaseOptions FailureOptions;
	FailureOptions.bFailVerification = true;
	const FMutationCaseResult Failure =
		RunMutationCase(Test, FailureOptions);
	TestFailedMutationResult(Test, Failure);
	TestCrossOwnerSuccessFlow(Test, Success);
	TestCaptureBoundary(Test, Success, Failure);
	TestDurableSuccess(Test, Success);
	TestPersistenceFailureHasNoPartialSuccess(Test, Failure);
}
} // namespace AssetDocumentCoreRuntimeCompositionTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCoreRuntimeCompositionTest,
	"UEAssetDoc.AtomicFramework.CoreRuntimeComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCoreRuntimeCompositionTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentCoreRuntimeCompositionTests;

	TestReadOnlyComposition(*this);
	TestReadOnlyRejectsMutationOperations(*this);
	TestMutationRejectsPreflightOperation(*this);
	TestPrivateHandleAdapterBoundary(*this);
	TestPersistenceTemplateValidationBoundary(*this);
	TestConstructorDependencyValidation(*this);
	TestInvalidInputGroup(*this);
	TestPersistenceTargetFailureMatrix(*this);
	TestMovedMutationInputFailsBeforeBegin(*this);
	TestSuccessFailureGroup(*this);
	TestCrossOwnerFailureGroup(*this);
	return true;
}

#endif
