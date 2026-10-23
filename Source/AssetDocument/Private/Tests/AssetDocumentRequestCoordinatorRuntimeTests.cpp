// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentRequestCoordinatorRuntime.h"

#include "AssetDocumentRollbackAction.h"
#include "Misc/AutomationTest.h"
#include "Runtime/AssetDocumentAtomicTransactionRuntime.h"
#include "Tests/AssetDocumentAtomicFrameworkTestDoubles.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentRequestCoordinatorRuntimeTests
{
class FRequestContext final : public IAssetDocumentNativeRequestContext
{
public:
	explicit FRequestContext(FAssetDocumentRequestContextMetadata InMetadata)
		: Metadata(MoveTemp(InMetadata))
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

private:
	FAssetDocumentRequestContextMetadata Metadata;
};

class FHost final : public IAssetDocumentPythonRuntimeHost
{
public:
	virtual FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) override
	{
		++DispatchCount;
		ObservedMetadata.Add(Context.GetMetadata());
		FAssetDocumentRuntimeResultDto Result;
		Result.RequestId = Request.RequestId;
		Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded;
		Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
		Result.RuntimeRevision = Context.GetMetadata().PinnedRuntimeRevision;
		return Result;
	}

	int32 DispatchCount = 0;
	TArray<FAssetDocumentRequestContextMetadata> ObservedMetadata;
};

class FInvalidHost final : public IAssetDocumentPythonRuntimeHost
{
public:
	virtual FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) override
	{
		++DispatchCount;
		return {};
	}

	int32 DispatchCount = 0;
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
	virtual void OnPhaseChanged(
		FGuid RequestId,
		EAssetDocumentRuntimeRequestPhase Phase) override
	{
		ObservedPhases.Add(Phase);
	}

	TArray<EAssetDocumentRuntimeRequestPhase> ObservedPhases;
};

FAssetDocumentRuntimeRequestDto MakeRequest()
{
	FAssetDocumentRuntimeRequestDto Request;
	Request.RequestId = FGuid::NewGuid();
	Request.Operation = EAssetDocumentRuntimeOperation::Inspect;
	Request.TargetAssetPath = TEXT("/Game/AssetDocument/T_CoreRuntime.T_CoreRuntime");
	Request.ExpectedRuntimeRevision = 7;
	return Request;
}

struct FMutationContextRecord
{
	int32 InstanceId = 0;
	FAssetDocumentRequestContextMetadata Metadata;
	FGuid TransactionId;
	IAssetDocumentAtomicTransaction* ActiveTransaction = nullptr;
};

class FMutationRequestContext final : public IAssetDocumentNativeRequestContext
{
public:
	explicit FMutationRequestContext(FMutationContextRecord InRecord)
		: Record(MoveTemp(InRecord))
	{
	}

	virtual const FAssetDocumentRequestContextMetadata& GetMetadata() const override
	{
		return Record.Metadata;
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

	const FMutationContextRecord& GetRecord() const
	{
		return Record;
	}

private:
	FMutationContextRecord Record;
};

FString PhaseName(EAssetDocumentRuntimeRequestPhase Phase)
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

FAssetDocumentContractValidationResult ValidContractResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

bool ConfiguredPhaseMatches(
	const TOptional<EAssetDocumentRuntimeRequestPhase>& Configured,
	EAssetDocumentRuntimeRequestPhase Actual)
{
	return Configured.IsSet() && Configured.GetValue() == Actual;
}

class FCoordinatorPostRollbackFinalizer final : public IAssetDocumentPostRollbackFinalizer
{
public:
	explicit FCoordinatorPostRollbackFinalizer(TArray<FString>& InCalls)
		: Calls(InCalls)
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return TEXTVIEW("rollback.finalizer");
	}

	virtual FAssetDocumentContractValidationResult FinalizePostRollback(FStringView) override
	{
		++CallCount;
		Calls.Add(TEXT("PostRollbackFinalizer.rollback.finalizer"));
		FAssetDocumentContractValidationResult Result;
		Result.bValid = !bFail;
		Result.Diagnostics = Diagnostics;
		return Result;
	}

	int32 CallCount = 0;
	bool bFail = false;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;

private:
	TArray<FString>& Calls;
};

class FMutationHost final : public IAssetDocumentPythonRuntimeHost
{
public:
	virtual FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) override
	{
		const FMutationRequestContext& MutationContext =
			static_cast<const FMutationRequestContext&>(Context);
		const FMutationContextRecord& Record = MutationContext.GetRecord();
		ObservedContexts.Add(Record);
		Calls.Add(FString::Printf(
			TEXT("Host.%s"),
			*PhaseName(Record.Metadata.Phase)));
		RecordLifecycleTrace(Record.Metadata.Phase);
		RecordMutationCaptureState(Record.Metadata.Phase);
		if (ConfiguredPhaseMatches(
			InvalidResultPhase,
			Record.Metadata.Phase))
		{
			return {};
		}
		if (ConfiguredPhaseMatches(FailPhase, Record.Metadata.Phase))
		{
			return MakeInjectedFailure(Request);
		}
		const FAssetDocumentContractValidationResult Enlisted =
			EnlistIfNeeded(Record);
		if (!Enlisted.bValid)
		{
			return MakeFailure(Request, Enlisted.Diagnostics);
		}
		ApplyConfiguredCancellation(Record.Metadata.Phase);
		return MakeSuccess(Request, Record.Metadata.Phase);
	}

	void RecordLifecycleTrace(EAssetDocumentRuntimeRequestPhase Phase)
	{
		if (LifecycleTrace != nullptr && IsMutationPhase(Phase))
		{
			LifecycleTrace->Add(PhaseName(Phase));
		}
	}

	void RecordMutationCaptureState(
		EAssetDocumentRuntimeRequestPhase Phase)
	{
		if (!IsMutationPhase(Phase))
		{
			return;
		}
		const bool bWasOpen = bCaptureOpen != nullptr && *bCaptureOpen;
		bCaptureWasOpenDuringMutation.Add(bWasOpen);
	}

	FAssetDocumentRuntimeResultDto MakeInjectedFailure(
		const FAssetDocumentRuntimeRequestDto& Request) const
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/InjectedHost");
		Diagnostic.Code = TEXT("Host.InjectedFailure");
		Diagnostic.Message = TEXT("Injected Host phase failure.");
		Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
		return MakeFailure(Request, MoveTemp(Diagnostics));
	}

	FAssetDocumentContractValidationResult EnlistIfNeeded(
		const FMutationContextRecord& Record)
	{
		if (Record.Metadata.Phase != EAssetDocumentRuntimeRequestPhase::Apply
			|| !bEnlistOnApply)
		{
			return ValidContractResult();
		}
		check(Record.ActiveTransaction);
		if (bEnlistPostRollbackFinalizerOnApply)
		{
			check(PostRollbackFinalizer.IsValid());
			const FAssetDocumentContractValidationResult FinalizerResult =
				Record.ActiveTransaction->EnlistPostRollbackFinalizer(
					PostRollbackFinalizer.ToSharedRef());
			if (!FinalizerResult.bValid)
			{
				return FinalizerResult;
			}
		}
		return Record.ActiveTransaction->EnlistFootprint(Footprint);
	}

	void ApplyConfiguredCancellation(
		EAssetDocumentRuntimeRequestPhase Phase)
	{
		if (bCancellationRequested != nullptr
			&& ConfiguredPhaseMatches(CancelAfterPhase, Phase))
		{
			*bCancellationRequested = true;
		}
	}

	FAssetDocumentRuntimeResultDto MakeSuccess(
		const FAssetDocumentRuntimeRequestDto& Request,
		EAssetDocumentRuntimeRequestPhase Phase) const
	{
		FAssetDocumentRuntimeResultDto Result;
		Result.RequestId = Request.RequestId;
		Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded;
		Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
		Result.RuntimeRevision = Request.ExpectedRuntimeRevision;
		FAssetDocumentChangedRecord& Changed =
			Result.ChangedSet.AddDefaulted_GetRef();
		Changed.AuthorityId = TEXT("host.runtime");
		Changed.StableObjectId =
			FString::Printf(TEXT("phase:%s"), *PhaseName(Phase));
		Changed.Path =
			FString::Printf(TEXT("/ChangedSet/%s"), *PhaseName(Phase));
		Changed.ChangeKind = PhaseName(Phase);
		return Result;
	}

	FAssetDocumentRuntimeResultDto MakeFailure(
		const FAssetDocumentRuntimeRequestDto& Request,
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics) const
	{
		FAssetDocumentRuntimeResultDto Result;
		Result.RequestId = Request.RequestId;
		Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
		Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
		Result.RuntimeRevision = Request.ExpectedRuntimeRevision;
		Result.Diagnostics = MoveTemp(Diagnostics);
		return Result;
	}

	TArray<FMutationContextRecord> ObservedContexts;
	TArray<FAssetDocumentMutationFootprint> Footprint;
	TArray<FString> Calls;
	TArray<bool> bCaptureWasOpenDuringMutation;
	TOptional<EAssetDocumentRuntimeRequestPhase> FailPhase;
	TOptional<EAssetDocumentRuntimeRequestPhase> InvalidResultPhase;
	TOptional<EAssetDocumentRuntimeRequestPhase> CancelAfterPhase;
	bool* bCaptureOpen = nullptr;
	bool* bCancellationRequested = nullptr;
	TArray<FString>* LifecycleTrace = nullptr;
	bool bEnlistOnApply = true;
	bool bEnlistPostRollbackFinalizerOnApply = false;
	TSharedPtr<IAssetDocumentPostRollbackFinalizer> PostRollbackFinalizer;

private:
	static bool IsMutationPhase(EAssetDocumentRuntimeRequestPhase Phase)
	{
		return Phase == EAssetDocumentRuntimeRequestPhase::Apply
			|| Phase == EAssetDocumentRuntimeRequestPhase::Repair;
	}
};

class FMutationObserver final : public IAssetDocumentRequestObserver
{
public:
	virtual void OnPhaseChanged(
		FGuid RequestId,
		EAssetDocumentRuntimeRequestPhase Phase) override
	{
		Phases.Add(Phase);
		if (LifecycleTrace == nullptr)
		{
			return;
		}
		if (Phase == EAssetDocumentRuntimeRequestPhase::Persist)
		{
			LifecycleTrace->Add(TEXT("Persist"));
		}
		else if (Phase == EAssetDocumentRuntimeRequestPhase::Verify)
		{
			LifecycleTrace->Add(TEXT("Verify"));
		}
		else if (Phase == EAssetDocumentRuntimeRequestPhase::Complete)
		{
			LifecycleTrace->Add(TEXT("Complete"));
		}
	}

	TArray<EAssetDocumentRuntimeRequestPhase> Phases;
	TArray<FString>* LifecycleTrace = nullptr;
};

class FMutationPrimitiveRegistry final : public IAssetDocumentPrimitiveRegistry
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

class FMutationVerifier final : public IAssetDocumentStagedArtifactVerifier
{
public:
	virtual FAssetDocumentFreshVerificationResult VerifyFresh(
		const FAssetDocumentFreshVerificationInput& Input) override
	{
		return {};
	}
};

class FMutationPersistence final : public IAssetDocumentPersistenceCoordinator
{
public:
	virtual FAssetDocumentDurableCompletionReceipt Persist(
		const FAssetDocumentPersistencePlan& Plan,
		IAssetDocumentStagedArtifactVerifier& Verifier) override
	{
		Calls.Add(TEXT("Persistence.Persist"));
		if (SharedCalls != nullptr)
		{
			SharedCalls->Add(TEXT("Persistence.Persist"));
		}
		ObservedPlan = Plan;
		FAssetDocumentDurableCompletionReceipt Receipt;
		Receipt.RequestId = Plan.RequestId;
		Receipt.TransactionId = Plan.TransactionId;
		Receipt.JournalId = Plan.JournalId;
		Receipt.PackageContentDigest = TEXT("sha256:package");
		Receipt.SidecarContentDigest = TEXT("sha256:sidecar");
		Receipt.bSyncStateAdvanced = true;
		if (bReturnInvalidReceipt)
		{
			Receipt.TransactionId.Invalidate();
		}
		return Receipt;
	}

	TArray<FString> Calls;
	TArray<FString>* SharedCalls = nullptr;
	TOptional<FAssetDocumentPersistencePlan> ObservedPlan;
	bool bReturnInvalidReceipt = false;
};

class FMutationSync final : public IAssetDocumentSyncCoordinator
{
public:
	virtual FAssetDocumentSyncDecision Compare(
		const FAssetDocumentSyncComparisonInput& Input) const override
	{
		return {};
	}

	virtual FAssetDocumentContractValidationResult AdvanceAfterDurableCompletion(
		const FAssetDocumentSyncEvidencePair& Installed,
		const FAssetDocumentDurableCompletionReceipt& Receipt) override
	{
		return ValidContractResult();
	}
};

class FRecordingTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	FRecordingTransaction(
		TUniquePtr<IAssetDocumentAtomicTransaction>&& InInner,
		TArray<FString>& InCalls,
		int32& InEnlistCount,
		int32& InMarkReadyCount,
		int32& InCommitCount,
		int32& InRollbackCount,
		TArray<FString>& InLifecycleTrace,
		bool& bInFailCommit)
		: Inner(MoveTemp(InInner))
		, Calls(InCalls)
		, EnlistCount(InEnlistCount)
		, MarkReadyCount(InMarkReadyCount)
		, CommitCount(InCommitCount)
		, RollbackCount(InRollbackCount)
		, LifecycleTrace(InLifecycleTrace)
		, bFailCommit(bInFailCommit)
	{
	}

	virtual FGuid GetTransactionId() const override
	{
		return Inner->GetTransactionId();
	}

	virtual EAssetDocumentAtomicState GetState() const override
	{
		return Inner->GetState();
	}

	virtual const FAssetDocumentMutationToken& GetMutationToken() const override
	{
		return Inner->GetMutationToken();
	}

	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override
	{
		Calls.Add(TEXT("Atomic.Enlist"));
		++EnlistCount;
		return Inner->EnlistFootprint(Footprint);
	}

	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		Calls.Add(TEXT("Atomic.MarkReady"));
		LifecycleTrace.Add(TEXT("transaction.ready"));
		++MarkReadyCount;
		return Inner->MarkReadyToCommit();
	}

	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt& Receipt) override
	{
		Calls.Add(TEXT("Atomic.Commit"));
		LifecycleTrace.Add(TEXT("Commit"));
		++CommitCount;
		if (bFailCommit)
		{
			FAssetDocumentContractValidationResult Result;
			Result.bValid = false;
			FAssetDocumentRuntimeDiagnostic& Diagnostic =
				Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/InjectedAtomic");
			Diagnostic.Code = TEXT("Atomic.InjectedCommitFailure");
			Diagnostic.Message = TEXT("Injected atomic commit failure.");
			Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
			return Result;
		}
		return Inner->Commit(Receipt);
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		Calls.Add(TEXT("Atomic.Rollback"));
		++RollbackCount;
		return Inner->Rollback(StableFailureCode);
	}

	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		return Inner->EnlistRollbackAction(MoveTemp(Action));
	}

	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer) override
	{
		return Inner->EnlistPostRollbackFinalizer(MoveTemp(Finalizer));
	}

	TUniquePtr<IAssetDocumentAtomicTransaction> Inner;
	TArray<FString>& Calls;
	int32& EnlistCount;
	int32& MarkReadyCount;
	int32& CommitCount;
	int32& RollbackCount;
	TArray<FString>& LifecycleTrace;
	bool& bFailCommit;
};

class FRecordingTransactionFactory final
	: public IAssetDocumentAtomicTransactionFactory
{
public:
	FRecordingTransactionFactory(
		IAssetDocumentAtomicTransactionFactory& InInner,
		TArray<FString>& InCalls,
		TArray<FString>& InLifecycleTrace)
		: Inner(InInner)
		, Calls(InCalls)
		, LifecycleTrace(InLifecycleTrace)
	{
	}

	virtual TUniquePtr<IAssetDocumentAtomicTransaction>
	BeginLiveAfterPreflight(
		const FAssetDocumentPreflightReceipt& Receipt,
		IAssetDocumentCancellationView& Cancellation) override
	{
		Calls.Add(TEXT("Atomic.Begin"));
		++BeginCount;
		if (bFailBegin)
		{
			return nullptr;
		}
		TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
			Inner.BeginLiveAfterPreflight(Receipt, Cancellation);
		if (!Transaction)
		{
			return nullptr;
		}
		GeneratedTransactionId = Transaction->GetTransactionId();
		return MakeUnique<FRecordingTransaction>(
			MoveTemp(Transaction),
			Calls,
			EnlistCount,
			MarkReadyCount,
			CommitCount,
			RollbackCount,
			LifecycleTrace,
			bFailCommit);
	}

	IAssetDocumentAtomicTransactionFactory& Inner;
	TArray<FString>& Calls;
	TArray<FString>& LifecycleTrace;
	FGuid GeneratedTransactionId;
	bool bFailBegin = false;
	bool bFailCommit = false;
	int32 BeginCount = 0;
	int32 EnlistCount = 0;
	int32 MarkReadyCount = 0;
	int32 CommitCount = 0;
	int32 RollbackCount = 0;
};

class FMutationFixture
{
public:
	FMutationFixture()
		: Handles(&AtomicRecord)
		, Participant(TEXT("rollback.asset"), &AtomicRecord)
		, AtomicFactory(
			Handles,
			Participants,
			[this](
				EAssetDocumentRuntimeRequestPhase Phase,
				const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{
				AtomicDiagnostics.Emplace(Phase, Diagnostic);
			})
		, RecordingFactory(
			AtomicFactory,
			AtomicRecord.Calls,
			LifecycleTrace)
	{
		Participants.Add(Participant);
		Request = MakeRequest();
		Request.Operation = EAssetDocumentRuntimeOperation::Apply;
		Footprint.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
		Footprint.StableId = TEXT("asset:/Game/AssetDocument/T_CoreRuntime");
		Footprint.RollbackComponentId = TEXT("rollback.asset");
		Preflight.RequestId = Request.RequestId;
		Preflight.RuntimeRevision = Request.ExpectedRuntimeRevision;
		Preflight.CompleteFootprint.Add(Footprint);
		Preflight.ReceiptHash = TEXT("sha256:preflight");
		Preflight.bSucceeded = true;
		PersistencePlan.RequestId = Request.RequestId;
		PersistencePlan.TargetPackagePath =
			TEXT("/Game/AssetDocument/T_CoreRuntime");
		PersistencePlan.TargetSidecarPath =
			TEXT("/tmp/asset-document/T_CoreRuntime.assetdoc.json");
		PersistencePlan.StagingRoot =
			TEXT("/tmp/asset-document/staging/core-runtime");
		PersistencePlan.JournalId = TEXT("journal:core-runtime");
		PersistencePlan.Sidecar.Utf8Bytes = {0x7b, 0x7d};
		PersistencePlan.Sidecar.SemanticHash = TEXT("sha256:sidecar");
		PersistencePlan.Sidecar.CodecVersion = 1;
		PersistencePlan.Footprint.Add(Footprint);
		Host.Footprint.Add(Footprint);
		Host.bCaptureOpen = &bCaptureOpen;
		Host.bCancellationRequested = &Cancellation.bCancelled;
		Host.LifecycleTrace = &LifecycleTrace;
		Observer.LifecycleTrace = &LifecycleTrace;
		Persistence.SharedCalls = &AtomicRecord.Calls;
	}

	TUniquePtr<IAssetDocumentNativeRequestContext> CreateContext(
		const FAssetDocumentRequestContextMetadata& Metadata,
		const FGuid& TransactionId,
		IAssetDocumentAtomicTransaction* ActiveTransaction,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
	{
		if (FailContextPhase.IsSet()
			&& FailContextPhase.GetValue() == Metadata.Phase)
		{
			++ContextCount;
			FAssetDocumentRuntimeDiagnostic Diagnostic;
			Diagnostic.Path = TEXT("/InjectedContext");
			Diagnostic.Code = TEXT("Context.InjectedFailure");
			Diagnostic.Message = TEXT("Injected context creation failure.");
			Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
			OutDiagnostics.Add(MoveTemp(Diagnostic));
			return nullptr;
		}
		FMutationContextRecord Record;
		Record.InstanceId = ++ContextCount;
		Record.Metadata = Metadata;
		Record.TransactionId = TransactionId;
		Record.ActiveTransaction = ActiveTransaction;
		return MakeUnique<FMutationRequestContext>(MoveTemp(Record));
	}

	TUniquePtr<FAssetDocumentCoreMutationCaptureLease> OpenCapture(
		const FGuid& TransactionId,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
	{
		AtomicRecord.Calls.Add(TEXT("Capture.Open"));
		++CaptureOpenCount;
		CapturedTransactionId = TransactionId;
		if (bFailCapture)
		{
			FAssetDocumentRuntimeDiagnostic Diagnostic;
			Diagnostic.Path = TEXT("/InjectedCapture");
			Diagnostic.Code = TEXT("Capture.InjectedFailure");
			Diagnostic.Message = TEXT("Injected capture open failure.");
			Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
			OutDiagnostics.Add(MoveTemp(Diagnostic));
			return nullptr;
		}
		bCaptureOpen = true;
		return MakeUnique<FAssetDocumentCoreMutationCaptureLease>(
			TUniqueFunction<void()>([this]()
			{
				AtomicRecord.Calls.Add(TEXT("Capture.Close"));
				LifecycleTrace.Add(TEXT("capture.close"));
				++CaptureCloseCount;
				bCaptureOpen = false;
			}));
	}

	FAssetDocumentRuntimeResultDto Execute()
	{
		FAssetDocumentRequestCoordinatorRuntimeBindings Bindings;
		Bindings.DiagnosticSink = [this](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			CoordinatorDiagnostics.Add(Diagnostic);
		};
		Bindings.CreatePhaseContext =
			[this](const FAssetDocumentRequestContextMetadata& Metadata,
				const FGuid& TransactionId,
				IAssetDocumentAtomicTransaction* ActiveTransaction,
				TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
			{
				return CreateContext(
					Metadata,
					TransactionId,
					ActiveTransaction,
					OutDiagnostics);
			};
		if (!bMissingCaptureOpener)
		{
			Bindings.OpenMutationCapture =
				[this](const FGuid& TransactionId,
					TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
				{
					return OpenCapture(TransactionId, OutDiagnostics);
				};
		}
		Bindings.PreflightReceipt = &Preflight;
		Bindings.PersistencePlanTemplate = &PersistencePlan;
		Bindings.Verifier = &Verifier;
		Bindings.BindPersistenceTarget =
			[this](
				const FAssetDocumentRuntimeRequestDto& BoundRequest,
				const FGuid& TransactionId,
				TConstArrayView<FAssetDocumentChangedRecord> ChangedSet,
				TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
			{
				AtomicRecord.Calls.Add(TEXT("PersistenceTarget.Bind"));
				LifecycleTrace.Add(TEXT("persistence_target.bind"));
				++PersistenceTargetBindCount;
				bCaptureOpenAtPersistenceTargetBind = bCaptureOpen;
				bApplyAndRepairCompleteAtPersistenceTargetBind =
					Host.ObservedContexts.Num() == 4;
				BoundPersistenceTargetRequestId = BoundRequest.RequestId;
				BoundPersistenceTargetTransactionId = TransactionId;
				BoundPersistenceTargetChangedSet.Append(ChangedSet);
				if (bFailPersistenceTargetBinding)
				{
					FAssetDocumentRuntimeDiagnostic Diagnostic;
					Diagnostic.Path =
						TEXT("/CoreRuntimeComposition/PersistenceTarget");
					Diagnostic.Code = TEXT(
						"CoreRuntimeComposition.PersistenceTargetBindingFailed");
					Diagnostic.Message =
						TEXT("Injected persistence-target binding failure.");
					Diagnostic.Severity =
						EAssetDocumentDiagnosticSeverity::Error;
					OutDiagnostics.Add(MoveTemp(Diagnostic));
					return false;
				}
				return true;
			};
		FAssetDocumentRequestCoordinatorRuntime Coordinator(MoveTemp(Bindings));
		FAssetDocumentRequestCoordinatorDependencies Dependencies{
			Host,
			Registry,
			Handles,
			RecordingFactory,
			Persistence,
			Sync,
			Cancellation,
			Observer};
		return Coordinator.Execute(Request, Dependencies);
	}

	AssetDocumentAtomicFrameworkTestDoubles::FCallRecord AtomicRecord;
	TArray<FString> LifecycleTrace;
	FMutationHost Host;
	FMutationObserver Observer;
	FMutationPrimitiveRegistry Registry;
	AssetDocumentAtomicFrameworkTestDoubles::FCancellationView Cancellation;
	AssetDocumentAtomicFrameworkTestDoubles::FHandleResolver Handles;
	AssetDocumentAtomicFrameworkTestDoubles::FRollbackParticipant Participant;
	AssetDocumentAtomicFrameworkTestDoubles::FParticipantRegistry Participants;
	TArray<TPair<
		EAssetDocumentRuntimeRequestPhase,
		FAssetDocumentRuntimeDiagnostic>> AtomicDiagnostics;
	TArray<FAssetDocumentRuntimeDiagnostic> CoordinatorDiagnostics;
	FAssetDocumentAtomicTransactionFactoryRuntime AtomicFactory;
	FRecordingTransactionFactory RecordingFactory;
	FMutationPersistence Persistence;
	FMutationSync Sync;
	FMutationVerifier Verifier;
	FAssetDocumentRuntimeRequestDto Request;
	FAssetDocumentMutationFootprint Footprint;
	FAssetDocumentPreflightReceipt Preflight;
	FAssetDocumentPersistencePlan PersistencePlan;
	FGuid CapturedTransactionId;
	FGuid BoundPersistenceTargetRequestId;
	FGuid BoundPersistenceTargetTransactionId;
	TArray<FAssetDocumentChangedRecord> BoundPersistenceTargetChangedSet;
	TOptional<EAssetDocumentRuntimeRequestPhase> FailContextPhase;
	bool bMissingCaptureOpener = false;
	bool bFailCapture = false;
	bool bFailPersistenceTargetBinding = false;
	bool bCaptureOpenAtPersistenceTargetBind = false;
	bool bApplyAndRepairCompleteAtPersistenceTargetBind = false;
	int32 ContextCount = 0;
	int32 CaptureOpenCount = 0;
	int32 CaptureCloseCount = 0;
	int32 PersistenceTargetBindCount = 0;
	bool bCaptureOpen = false;
};

bool AreObserverEdgesValid(
	TConstArrayView<EAssetDocumentRuntimeRequestPhase> Phases)
{
	for (int32 Index = 1; Index < Phases.Num(); ++Index)
	{
		if (!IsAssetDocumentRequestPhaseTransitionAllowed(
			Phases[Index - 1],
			Phases[Index]))
		{
			return false;
		}
	}
	return true;
}

void CheckPhases(
	FAutomationTestBase& Test,
	const TCHAR* ExactLabel,
	TConstArrayView<EAssetDocumentRuntimeRequestPhase> Actual,
	const TArray<EAssetDocumentRuntimeRequestPhase>& Expected)
{
	Test.TestTrue(ExactLabel, Actual == Expected);
	Test.TestTrue(
		TEXT("every observed phase edge uses the frozen predicate"),
		AreObserverEdgesValid(Actual));
}

void CheckSingleDiagnostic(
	FAutomationTestBase& Test,
	const FAssetDocumentRuntimeResultDto& Result,
	const TCHAR* ExpectedCode)
{
	Test.TestEqual(
		TEXT("failure returns exactly one diagnostic"),
		Result.Diagnostics.Num(),
		1);
	if (Result.Diagnostics.Num() == 1)
	{
		Test.TestEqual(
			TEXT("failure preserves the expected diagnostic"),
			Result.Diagnostics[0].Code,
			FString(ExpectedCode));
	}
}

void CheckSingleAtomicDiagnostic(
	FAutomationTestBase& Test,
	const TArray<TPair<
		EAssetDocumentRuntimeRequestPhase,
		FAssetDocumentRuntimeDiagnostic>>& Diagnostics,
	EAssetDocumentRuntimeRequestPhase ExpectedPhase,
	const TCHAR* ExpectedCode)
{
	Test.TestEqual(
		TEXT("Atomic emits exactly one diagnostic"),
		Diagnostics.Num(),
		1);
	if (Diagnostics.Num() == 1)
	{
		Test.TestEqual(
			TEXT("Atomic diagnostic uses the expected phase"),
			Diagnostics[0].Key,
			ExpectedPhase);
		Test.TestEqual(
			TEXT("Atomic diagnostic preserves the expected code"),
			Diagnostics[0].Value.Code,
			FString(ExpectedCode));
	}
}

void CheckCallBefore(
	FAutomationTestBase& Test,
	const TArray<FString>& Calls,
	const TCHAR* Before,
	const TCHAR* After)
{
	const int32 BeforeIndex = Calls.IndexOfByKey(FString(Before));
	const int32 AfterIndex = Calls.IndexOfByKey(FString(After));
	Test.TestTrue(TEXT("earlier call is recorded"), BeforeIndex != INDEX_NONE);
	Test.TestTrue(TEXT("later call is recorded"), AfterIndex != INDEX_NONE);
	Test.TestTrue(TEXT("calls preserve frozen order"), BeforeIndex < AfterIndex);
}

void TestMutationSuccess(FAutomationTestBase& Test)
{
	FMutationFixture Fixture;
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	Test.TestEqual(
		TEXT("mutation returns a committed success terminal"),
		Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Succeeded);
	CheckPhases(
		Test,
		TEXT("mutation observes the exact legal phase sequence"),
		Fixture.Observer.Phases,
		{EAssetDocumentRuntimeRequestPhase::Validate,
		 EAssetDocumentRuntimeRequestPhase::Preflight,
		 EAssetDocumentRuntimeRequestPhase::Apply,
		 EAssetDocumentRuntimeRequestPhase::Repair,
		 EAssetDocumentRuntimeRequestPhase::Persist,
		 EAssetDocumentRuntimeRequestPhase::Verify,
		 EAssetDocumentRuntimeRequestPhase::Complete});
	Test.TestEqual(TEXT("four phase contexts are created"),
		Fixture.Host.ObservedContexts.Num(), 4);
	for (int32 Index = 0; Index < Fixture.Host.ObservedContexts.Num(); ++Index)
	{
		Test.TestEqual(TEXT("every context has a distinct instance identity"),
			Fixture.Host.ObservedContexts[Index].InstanceId, Index + 1);
	}
	Test.TestNull(TEXT("Validate context has no active transaction"),
		Fixture.Host.ObservedContexts[0].ActiveTransaction);
	Test.TestNull(TEXT("Preflight context has no active transaction"),
		Fixture.Host.ObservedContexts[1].ActiveTransaction);
	Test.TestNotNull(TEXT("Apply context has the active transaction"),
		Fixture.Host.ObservedContexts[2].ActiveTransaction);
	Test.TestTrue(TEXT("Repair reuses only the active transaction identity"),
		Fixture.Host.ObservedContexts[3].ActiveTransaction
			== Fixture.Host.ObservedContexts[2].ActiveTransaction);
	Test.TestEqual(TEXT("capture opens once"), Fixture.CaptureOpenCount, 1);
	Test.TestEqual(TEXT("capture closes once"), Fixture.CaptureCloseCount, 1);
	Test.TestFalse(TEXT("capture is closed at terminal"), Fixture.bCaptureOpen);
	Test.TestEqual(TEXT("persistence target binds exactly once"),
		Fixture.PersistenceTargetBindCount, 1);
	Test.TestFalse(TEXT("persistence target binds after capture closes"),
		Fixture.bCaptureOpenAtPersistenceTargetBind);
	Test.TestTrue(TEXT("persistence target binds after Apply and Repair"),
		Fixture.bApplyAndRepairCompleteAtPersistenceTargetBind);
	Test.TestTrue(TEXT("persistence target binding keeps request identity"),
		Fixture.BoundPersistenceTargetRequestId == Fixture.Request.RequestId);
	Test.TestTrue(TEXT("persistence target binding receives live transaction"),
		Fixture.BoundPersistenceTargetTransactionId
			== Fixture.RecordingFactory.GeneratedTransactionId);
	Test.TestTrue(TEXT("capture receives the generated transaction identity"),
		Fixture.CapturedTransactionId
			== Fixture.RecordingFactory.GeneratedTransactionId);
	Test.TestTrue(TEXT("capture remains open through Apply and Repair"),
		Fixture.Host.bCaptureWasOpenDuringMutation
			== TArray<bool>{true, true});
	Test.TestEqual(TEXT("provider enlists once"),
		Fixture.RecordingFactory.EnlistCount, 1);
	Test.TestEqual(TEXT("ready is marked once"),
		Fixture.RecordingFactory.MarkReadyCount, 1);
	Test.TestEqual(TEXT("commit occurs once"),
		Fixture.RecordingFactory.CommitCount, 1);
	Test.TestEqual(TEXT("success never rolls back"),
		Fixture.RecordingFactory.RollbackCount, 0);
	Test.TestTrue(TEXT("success preserves the frozen lifecycle trace"),
		Fixture.LifecycleTrace
			== TArray<FString>{
				TEXT("Apply"),
				TEXT("Repair"),
				TEXT("capture.close"),
				TEXT("persistence_target.bind"),
				TEXT("transaction.ready"),
				TEXT("Persist"),
				TEXT("Verify"),
				TEXT("Commit"),
				TEXT("Complete")});
	TArray<FString> ChangedKinds;
	for (const FAssetDocumentChangedRecord& Changed
		: Fixture.BoundPersistenceTargetChangedSet)
	{
		ChangedKinds.Add(Changed.ChangeKind);
	}
	Test.TestTrue(TEXT("binding receives each successful phase change once"),
		ChangedKinds
			== TArray<FString>{
				TEXT("Validate"),
				TEXT("Preflight"),
				TEXT("Apply"),
				TEXT("Repair")});
	Test.TestEqual(TEXT("transaction begins exactly once"),
		Fixture.RecordingFactory.BeginCount, 1);
	Test.TestEqual(TEXT("persistence runs exactly once"),
		Fixture.Persistence.Calls.Num(), 1);
}

void TestPersistenceTargetBindingFailure(FAutomationTestBase& Test)
{
	FMutationFixture Fixture;
	Fixture.bFailPersistenceTargetBinding = true;
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	Test.TestEqual(TEXT("binding failure returns failed"),
		Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("binding failure invokes binder once"),
		Fixture.PersistenceTargetBindCount, 1);
	Test.TestFalse(TEXT("binding failure occurs after capture closes"),
		Fixture.bCaptureOpenAtPersistenceTargetBind);
	Test.TestTrue(TEXT("binding failure occurs after Apply and Repair"),
		Fixture.bApplyAndRepairCompleteAtPersistenceTargetBind);
	Test.TestEqual(TEXT("binding failure never marks ready"),
		Fixture.RecordingFactory.MarkReadyCount, 0);
	Test.TestTrue(TEXT("binding failure never persists"),
		Fixture.Persistence.Calls.IsEmpty());
	Test.TestEqual(TEXT("binding failure never commits"),
		Fixture.RecordingFactory.CommitCount, 0);
	Test.TestEqual(TEXT("binding failure rolls back once"),
		Fixture.RecordingFactory.RollbackCount, 1);
	CheckSingleDiagnostic(
		Test,
		Result,
		TEXT("CoreRuntimeComposition.PersistenceTargetBindingFailed"));
	CheckCallBefore(Test, Fixture.AtomicRecord.Calls,
		TEXT("Capture.Close"), TEXT("PersistenceTarget.Bind"));
	CheckCallBefore(Test, Fixture.AtomicRecord.Calls,
		TEXT("PersistenceTarget.Bind"), TEXT("Atomic.Rollback"));
}

void TestInvalidReadOnlyRequest(FAutomationTestBase& Test)
{
	FAssetDocumentRuntimeRequestDto InvalidRequest = MakeRequest();
	InvalidRequest.RequestId.Invalidate();
	int32 ContextCount = 0;
	FAssetDocumentRequestCoordinatorRuntimeBindings InvalidBindings;
	InvalidBindings.CreatePhaseContext =
		[&](const FAssetDocumentRequestContextMetadata& Metadata,
			const FGuid& TransactionId,
			IAssetDocumentAtomicTransaction* ActiveTransaction,
			TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
		{
			++ContextCount;
			return MakeUnique<FRequestContext>(Metadata);
		};
	FAssetDocumentRequestCoordinatorRuntime InvalidCoordinator(
		MoveTemp(InvalidBindings));
	FHost UnusedHost;
	FCancellation Cancellation;
	FObserver InvalidObserver;
	FAssetDocumentRequestCoordinatorReadOnlyDependencies InvalidDependencies{
		UnusedHost,
		Cancellation,
		InvalidObserver};
	const FAssetDocumentRuntimeResultDto InvalidRequestResult =
		InvalidCoordinator.ExecuteReadOnly(
			InvalidRequest,
			InvalidDependencies);
	Test.TestEqual(TEXT("invalid request fails"),
		InvalidRequestResult.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("invalid request creates no context"), ContextCount, 0);
	Test.TestEqual(TEXT("invalid request makes no Host call"),
		UnusedHost.DispatchCount, 0);
	Test.TestTrue(TEXT("invalid request observes no phase"),
		InvalidObserver.ObservedPhases.IsEmpty());
}

void TestInvalidReadOnlyHost(FAutomationTestBase& Test)
{
	TArray<FAssetDocumentRuntimeDiagnostic> EmittedDiagnostics;
	FAssetDocumentRequestCoordinatorRuntimeBindings HostBindings;
	HostBindings.CreatePhaseContext =
		[](const FAssetDocumentRequestContextMetadata& Metadata,
			const FGuid& TransactionId,
			IAssetDocumentAtomicTransaction* ActiveTransaction,
			TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
		{
			return MakeUnique<FRequestContext>(Metadata);
		};
	HostBindings.DiagnosticSink =
		[&](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			EmittedDiagnostics.Add(Diagnostic);
		};
	FAssetDocumentRequestCoordinatorRuntime HostCoordinator(
		MoveTemp(HostBindings));
	FInvalidHost InvalidHost;
	FCancellation Cancellation;
	FObserver HostObserver;
	FAssetDocumentRequestCoordinatorReadOnlyDependencies HostDependencies{
		InvalidHost,
		Cancellation,
		HostObserver};
	const FAssetDocumentRuntimeResultDto InvalidHostResult =
		HostCoordinator.ExecuteReadOnly(MakeRequest(), HostDependencies);
	Test.TestEqual(TEXT("invalid Host result fails"),
		InvalidHostResult.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("invalid Host result is not retried"),
		InvalidHost.DispatchCount, 1);
	CheckPhases(
		Test,
		TEXT("invalid Host result fails on the legal Validate to Failed edge"),
		HostObserver.ObservedPhases,
		{EAssetDocumentRuntimeRequestPhase::Validate,
		 EAssetDocumentRuntimeRequestPhase::Failed});
	Test.TestEqual(TEXT("one validator diagnostic is emitted"),
		EmittedDiagnostics.Num(), 1);
	if (EmittedDiagnostics.Num() == 1)
	{
		Test.TestEqual(TEXT("validator diagnostic is preserved"),
			EmittedDiagnostics[0].Code,
			FString(TEXT("RuntimeResult.InvalidIdentity")));
	}
}

void TestValidationFailures(FAutomationTestBase& Test)
{
	TestInvalidReadOnlyRequest(Test);
	TestInvalidReadOnlyHost(Test);
}

TArray<EAssetDocumentRuntimeRequestPhase> ExpectedContextFailurePhases(
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

void CheckContextCreationFailure(
	FAutomationTestBase& Test,
	EAssetDocumentRuntimeRequestPhase FailedPhase,
	int32 ExpectedHostCalls,
	bool bExpectedLiveTransaction)
{
	FMutationFixture Fixture;
	Fixture.FailContextPhase = FailedPhase;
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	const TArray<EAssetDocumentRuntimeRequestPhase> ExpectedPhases =
		ExpectedContextFailurePhases(FailedPhase);
	const int32 ExpectedLiveCount = bExpectedLiveTransaction ? 1 : 0;
	Test.TestEqual(
		*FString::Printf(
			TEXT("%s context creation returns failed"),
			*PhaseName(FailedPhase)),
		Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("failed context creation is not retried"),
		Fixture.ContextCount, ExpectedHostCalls + 1);
	Test.TestEqual(TEXT("Host stops before the missing context"),
		Fixture.Host.ObservedContexts.Num(), ExpectedHostCalls);
	CheckPhases(Test, TEXT("context failure observes exact phases"),
		Fixture.Observer.Phases, ExpectedPhases);
	Test.TestEqual(TEXT("live transaction begin count is exact"),
		Fixture.RecordingFactory.BeginCount, ExpectedLiveCount);
	Test.TestEqual(TEXT("capture open count is exact"),
		Fixture.CaptureOpenCount, ExpectedLiveCount);
	Test.TestEqual(TEXT("capture close count is exact"),
		Fixture.CaptureCloseCount, ExpectedLiveCount);
	Test.TestEqual(TEXT("rollback count is exact"),
		Fixture.RecordingFactory.RollbackCount, ExpectedLiveCount);
	CheckSingleDiagnostic(Test, Result, TEXT("Context.InjectedFailure"));
}

void TestContextCreationFailures(FAutomationTestBase& Test)
{
	CheckContextCreationFailure(
		Test,
		EAssetDocumentRuntimeRequestPhase::Validate,
		0,
		false);
	CheckContextCreationFailure(
		Test,
		EAssetDocumentRuntimeRequestPhase::Preflight,
		1,
		false);
	CheckContextCreationFailure(
		Test,
		EAssetDocumentRuntimeRequestPhase::Apply,
		2,
		true);
	CheckContextCreationFailure(
		Test,
		EAssetDocumentRuntimeRequestPhase::Repair,
		3,
		true);
}

void TestBeginFailure(FAutomationTestBase& Test)
{
	FMutationFixture Begin;
	Begin.RecordingFactory.bFailBegin = true;
	const FAssetDocumentRuntimeResultDto BeginResult = Begin.Execute();
	Test.TestEqual(TEXT("failed begin returns failed"),
		BeginResult.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("begin is not retried"),
		Begin.RecordingFactory.BeginCount, 1);
	Test.TestEqual(TEXT("failed begin never opens capture"),
		Begin.CaptureOpenCount, 0);
	Test.TestEqual(TEXT("failed begin stops before mutation Host phases"),
		Begin.Host.ObservedContexts.Num(), 2);
	Test.TestEqual(TEXT("failed begin has no live rollback"),
		Begin.RecordingFactory.RollbackCount, 0);
}

void TestMissingCaptureOpener(FAutomationTestBase& Test)
{
	FMutationFixture Missing;
	Missing.bMissingCaptureOpener = true;
	const FAssetDocumentRuntimeResultDto MissingResult = Missing.Execute();
	Test.TestEqual(TEXT("missing opener returns failed"),
		MissingResult.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("missing opener begins once"),
		Missing.RecordingFactory.BeginCount, 1);
	Test.TestEqual(TEXT("missing opener is not called"),
		Missing.CaptureOpenCount, 0);
	Test.TestEqual(TEXT("missing opener stops before mutation"),
		Missing.Host.ObservedContexts.Num(), 2);
	Test.TestEqual(TEXT("missing opener rolls back once"),
		Missing.RecordingFactory.RollbackCount, 1);
	CheckSingleDiagnostic(
		Test,
		MissingResult,
		TEXT("RequestCoordinator.MutationCaptureOpenFailed"));
}

void TestFailedCaptureOpener(FAutomationTestBase& Test)
{
	FMutationFixture Failed;
	Failed.bFailCapture = true;
	const FAssetDocumentRuntimeResultDto FailedResult = Failed.Execute();
	Test.TestEqual(TEXT("failed opener returns failed"),
		FailedResult.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("failed opener is called once"),
		Failed.CaptureOpenCount, 1);
	Test.TestTrue(TEXT("failed opener receives exact transaction identity"),
		Failed.CapturedTransactionId
			== Failed.RecordingFactory.GeneratedTransactionId);
	Test.TestEqual(TEXT("invalid lease is never closed"),
		Failed.CaptureCloseCount, 0);
	Test.TestEqual(TEXT("failed opener stops before mutation"),
		Failed.Host.ObservedContexts.Num(), 2);
	Test.TestEqual(TEXT("failed opener rolls back once"),
		Failed.RecordingFactory.RollbackCount, 1);
	CheckSingleDiagnostic(Test, FailedResult, TEXT("Capture.InjectedFailure"));
}

void TestBeginAndCaptureFailures(FAutomationTestBase& Test)
{
	TestBeginFailure(Test);
	TestMissingCaptureOpener(Test);
	TestFailedCaptureOpener(Test);
}

void TestProviderEnlistFailure(FAutomationTestBase& Test)
{
	FMutationFixture Fixture;
	Fixture.Host.Footprint[0].StableId =
		TEXT("asset:/Game/AssetDocument/Unknown");
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	Test.TestEqual(TEXT("provider enlist failure returns failed"),
		Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("provider enlist is attempted once"),
		Fixture.RecordingFactory.EnlistCount, 1);
	Test.TestEqual(TEXT("provider failure never marks ready"),
		Fixture.RecordingFactory.MarkReadyCount, 0);
	Test.TestEqual(TEXT("provider failure never commits"),
		Fixture.RecordingFactory.CommitCount, 0);
	Test.TestEqual(TEXT("provider failure rolls back once"),
		Fixture.RecordingFactory.RollbackCount, 1);
	Test.TestEqual(TEXT("provider failure closes capture once"),
		Fixture.CaptureCloseCount, 1);
	CheckSingleDiagnostic(Test, Result, TEXT("Atomic.FootprintNotDeclared"));
	CheckCallBefore(Test, Fixture.AtomicRecord.Calls,
		TEXT("Capture.Close"), TEXT("Atomic.Rollback"));
}

void TestApplyFailure(FAutomationTestBase& Test)
{
	FMutationFixture Fixture;
	Fixture.Host.FailPhase = EAssetDocumentRuntimeRequestPhase::Apply;
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	Test.TestEqual(TEXT("Apply failure returns failed"), Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("Apply failure is not retried"),
		Fixture.Host.ObservedContexts.Num(), 3);
	CheckPhases(Test, TEXT("Apply failure observes exact phases"),
		Fixture.Observer.Phases,
		{EAssetDocumentRuntimeRequestPhase::Validate,
		 EAssetDocumentRuntimeRequestPhase::Preflight,
		 EAssetDocumentRuntimeRequestPhase::Apply,
		 EAssetDocumentRuntimeRequestPhase::Failed});
	Test.TestTrue(TEXT("capture is open during Apply"),
		Fixture.Host.bCaptureWasOpenDuringMutation == TArray<bool>{true});
	Test.TestEqual(TEXT("Apply failure closes capture once"),
		Fixture.CaptureCloseCount, 1);
	Test.TestEqual(TEXT("failed Apply enlists nothing"),
		Fixture.RecordingFactory.EnlistCount, 0);
	Test.TestEqual(TEXT("failed Apply never binds the persistence target"),
		Fixture.PersistenceTargetBindCount, 0);
	Test.TestEqual(TEXT("failed Apply never marks ready"),
		Fixture.RecordingFactory.MarkReadyCount, 0);
	Test.TestEqual(TEXT("failed Apply never commits"),
		Fixture.RecordingFactory.CommitCount, 0);
	Test.TestEqual(TEXT("failed Apply rolls back once"),
		Fixture.RecordingFactory.RollbackCount, 1);
	CheckSingleDiagnostic(Test, Result, TEXT("Host.InjectedFailure"));
	CheckCallBefore(Test, Fixture.AtomicRecord.Calls,
		TEXT("Capture.Close"), TEXT("Atomic.Rollback"));
}

void TestRepairFailure(FAutomationTestBase& Test)
{
	FMutationFixture Fixture;
	Fixture.Host.FailPhase = EAssetDocumentRuntimeRequestPhase::Repair;
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	Test.TestEqual(TEXT("Repair failure returns failed"), Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("Repair failure is not retried"),
		Fixture.Host.ObservedContexts.Num(), 4);
	CheckPhases(Test, TEXT("Repair failure observes exact phases"),
		Fixture.Observer.Phases,
		{EAssetDocumentRuntimeRequestPhase::Validate,
		 EAssetDocumentRuntimeRequestPhase::Preflight,
		 EAssetDocumentRuntimeRequestPhase::Apply,
		 EAssetDocumentRuntimeRequestPhase::Repair,
		 EAssetDocumentRuntimeRequestPhase::Failed});
	Test.TestTrue(TEXT("capture is open during Apply and Repair"),
		Fixture.Host.bCaptureWasOpenDuringMutation
			== TArray<bool>{true, true});
	Test.TestEqual(TEXT("Repair failure closes capture once"),
		Fixture.CaptureCloseCount, 1);
	Test.TestEqual(TEXT("Apply provider enlists once before Repair"),
		Fixture.RecordingFactory.EnlistCount, 1);
	Test.TestEqual(TEXT("failed Repair never binds the persistence target"),
		Fixture.PersistenceTargetBindCount, 0);
	Test.TestEqual(TEXT("failed Repair never marks ready"),
		Fixture.RecordingFactory.MarkReadyCount, 0);
	Test.TestEqual(TEXT("failed Repair never commits"),
		Fixture.RecordingFactory.CommitCount, 0);
	Test.TestEqual(TEXT("failed Repair rolls back once"),
		Fixture.RecordingFactory.RollbackCount, 1);
	CheckSingleDiagnostic(Test, Result, TEXT("Host.InjectedFailure"));
	CheckCallBefore(Test, Fixture.AtomicRecord.Calls,
		TEXT("Capture.Close"), TEXT("Atomic.Rollback"));
}

void TestPostRollbackFinalizerOnCoordinatorFailure(FAutomationTestBase& Test, bool bFailFinalizer)
{
	FMutationFixture Fixture;
	FAssetDocumentMutationFootprint FinalizerFootprint;
	FinalizerFootprint.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
	FinalizerFootprint.StableId = TEXT("asset:/Game/AssetDocument/T_CoreRuntime.Finalizer");
	FinalizerFootprint.RollbackComponentId = TEXT("rollback.finalizer");
	Fixture.Preflight.CompleteFootprint.Add(FinalizerFootprint);
	Fixture.PersistencePlan.Footprint.Add(FinalizerFootprint);
	Fixture.Host.Footprint.Add(FinalizerFootprint);
	TSharedPtr<FCoordinatorPostRollbackFinalizer> Finalizer =
		MakeShared<FCoordinatorPostRollbackFinalizer>(Fixture.AtomicRecord.Calls);
	Finalizer->bFail = bFailFinalizer;
	Finalizer->Diagnostics = {
		{TEXT("/Test/Finalizer"), TEXT("TestFinalizer.Detail"), TEXT("Original finalizer detail."),
			bFailFinalizer ? EAssetDocumentDiagnosticSeverity::Error : EAssetDocumentDiagnosticSeverity::Warning},
		{TEXT("/Test/Finalizer"), TEXT("TestFinalizer.Info"), TEXT("Original finalizer information."),
			EAssetDocumentDiagnosticSeverity::Info}};
	// Identical entries are legitimate; forwarding must not silently deduplicate them.
	const FAssetDocumentRuntimeDiagnostic RepeatedDetail = Finalizer->Diagnostics[0];
	Finalizer->Diagnostics.Add(RepeatedDetail);
	Fixture.Host.PostRollbackFinalizer = Finalizer;
	Fixture.Host.bEnlistPostRollbackFinalizerOnApply = true;
	Fixture.Host.FailPhase = EAssetDocumentRuntimeRequestPhase::Repair;
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	Test.TestEqual(TEXT("coordinator repair failure still returns failed"),
		Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("coordinator rolls back once after active finalizer footprint"),
		Fixture.RecordingFactory.RollbackCount, 1);
	Test.TestEqual(TEXT("coordinator failure invokes the active finalizer exactly once"),
		Finalizer->CallCount, 1);
	CheckCallBefore(Test, Fixture.AtomicRecord.Calls,
		TEXT("Capture.Close"), TEXT("Atomic.Rollback"));
	CheckCallBefore(Test, Fixture.AtomicRecord.Calls,
		TEXT("Invalidate.Rollback"), TEXT("Rollback.rollback.asset"));
	CheckCallBefore(Test, Fixture.AtomicRecord.Calls,
		TEXT("Rollback.rollback.asset"), TEXT("PostRollbackFinalizer.rollback.finalizer"));
	Test.TestEqual(TEXT("coordinator failure leaves finalizer as the last rollback callback"),
		Fixture.AtomicRecord.Calls.Last(), FString(TEXT("PostRollbackFinalizer.rollback.finalizer")));
	const int32 DetailOffset = bFailFinalizer ? 2 : 1;
	Test.TestEqual(TEXT("coordinator returns host failure plus each original rollback diagnostic once"),
		Result.Diagnostics.Num(), DetailOffset + 3);
	Test.TestEqual(TEXT("coordinator sink receives each returned diagnostic once"),
		Fixture.CoordinatorDiagnostics.Num(), DetailOffset + 3);
	if (Result.Diagnostics.Num() == DetailOffset + 3
		&& Fixture.CoordinatorDiagnostics.Num() == DetailOffset + 3)
	{
		if (bFailFinalizer)
		{
			Test.TestEqual(TEXT("rollback aggregate precedes all original failure details"),
				Result.Diagnostics[1].Code, FString(TEXT("Atomic.Poisoned.RollbackFailed")));
		}
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const auto& Expected = Finalizer->Diagnostics[Index];
			const auto& Returned = Result.Diagnostics[DetailOffset + Index];
			const auto& Emitted = Fixture.CoordinatorDiagnostics[DetailOffset + Index];
			Test.TestTrue(TEXT("return and sink preserve ordered error or warning/info entries, including repeats"),
				Returned.Path == Expected.Path && Returned.Code == Expected.Code
				&& Returned.Message == Expected.Message && Returned.Severity == Expected.Severity
				&& Emitted.Path == Expected.Path && Emitted.Code == Expected.Code
				&& Emitted.Message == Expected.Message && Emitted.Severity == Expected.Severity);
		}
	}
}

void TestIncompleteProviderCoverage(FAutomationTestBase& Test)
{
	FMutationFixture Fixture;
	FAssetDocumentMutationFootprint MissingFootprint = Fixture.Footprint;
	MissingFootprint.StableId =
		TEXT("asset:/Game/AssetDocument/T_CoreRuntime.Second");
	Fixture.Preflight.CompleteFootprint.Add(MissingFootprint);
	Fixture.PersistencePlan.Footprint.Add(MissingFootprint);
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	Test.TestEqual(TEXT("incomplete coverage returns failed"),
		Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("all Host mutation phases run once"),
		Fixture.Host.ObservedContexts.Num(), 4);
	Test.TestEqual(TEXT("provider enlists its actual footprint once"),
		Fixture.RecordingFactory.EnlistCount, 1);
	Test.TestEqual(TEXT("only MarkReady evaluates complete coverage"),
		Fixture.RecordingFactory.MarkReadyCount, 1);
	Test.TestTrue(TEXT("incomplete coverage never persists"),
		Fixture.Persistence.Calls.IsEmpty());
	Test.TestEqual(TEXT("incomplete coverage never commits"),
		Fixture.RecordingFactory.CommitCount, 0);
	Test.TestEqual(TEXT("incomplete coverage rolls back once"),
		Fixture.RecordingFactory.RollbackCount, 1);
	CheckSingleDiagnostic(Test, Result, TEXT("Atomic.IncompleteFootprint"));
	CheckCallBefore(Test, Fixture.AtomicRecord.Calls,
		TEXT("Capture.Close"), TEXT("Atomic.MarkReady"));
	CheckCallBefore(Test, Fixture.AtomicRecord.Calls,
		TEXT("Atomic.MarkReady"), TEXT("Atomic.Rollback"));
	CheckPhases(Test, TEXT("incomplete coverage observes exact phases"),
		Fixture.Observer.Phases,
		{EAssetDocumentRuntimeRequestPhase::Validate,
		 EAssetDocumentRuntimeRequestPhase::Preflight,
		 EAssetDocumentRuntimeRequestPhase::Apply,
		 EAssetDocumentRuntimeRequestPhase::Repair,
		 EAssetDocumentRuntimeRequestPhase::Failed});
}

void TestInvalidDurableReceipt(FAutomationTestBase& Test)
{
	FMutationFixture Fixture;
	Fixture.Persistence.bReturnInvalidReceipt = true;
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	const TArray<FString>& Calls = Fixture.AtomicRecord.Calls;
	Test.TestEqual(TEXT("invalid receipt returns failed"), Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("capture is already closed"),
		Fixture.CaptureCloseCount, 1);
	Test.TestEqual(TEXT("ready is marked before persistence"),
		Fixture.RecordingFactory.MarkReadyCount, 1);
	Test.TestEqual(TEXT("persistence runs once"),
		Fixture.Persistence.Calls.Num(), 1);
	Test.TestEqual(TEXT("invalid receipt never commits"),
		Fixture.RecordingFactory.CommitCount, 0);
	Test.TestEqual(TEXT("invalid receipt rolls back once"),
		Fixture.RecordingFactory.RollbackCount, 1);
	CheckSingleDiagnostic(
		Test,
		Result,
		TEXT("RequestCoordinator.InvalidDurableReceipt"));
	CheckCallBefore(Test, Calls,
		TEXT("Capture.Close"), TEXT("Atomic.MarkReady"));
	CheckCallBefore(Test, Calls,
		TEXT("Atomic.MarkReady"), TEXT("Persistence.Persist"));
	CheckCallBefore(Test, Calls,
		TEXT("Persistence.Persist"), TEXT("Atomic.Rollback"));
	CheckPhases(Test, TEXT("invalid receipt observes exact phases"),
		Fixture.Observer.Phases,
		{EAssetDocumentRuntimeRequestPhase::Validate,
		 EAssetDocumentRuntimeRequestPhase::Preflight,
		 EAssetDocumentRuntimeRequestPhase::Apply,
		 EAssetDocumentRuntimeRequestPhase::Repair,
		 EAssetDocumentRuntimeRequestPhase::Persist,
		 EAssetDocumentRuntimeRequestPhase::Failed});
}

void TestCommitFailure(FAutomationTestBase& Test)
{
	FMutationFixture Fixture;
	Fixture.RecordingFactory.bFailCommit = true;
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	const TArray<FString>& Calls = Fixture.AtomicRecord.Calls;
	Test.TestEqual(TEXT("commit failure returns failed"), Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("capture is already closed"),
		Fixture.CaptureCloseCount, 1);
	Test.TestEqual(TEXT("ready is marked once"),
		Fixture.RecordingFactory.MarkReadyCount, 1);
	Test.TestEqual(TEXT("persistence runs once"),
		Fixture.Persistence.Calls.Num(), 1);
	Test.TestEqual(TEXT("commit is attempted once"),
		Fixture.RecordingFactory.CommitCount, 1);
	Test.TestEqual(TEXT("commit failure rolls back once"),
		Fixture.RecordingFactory.RollbackCount, 1);
	CheckSingleDiagnostic(Test, Result, TEXT("Atomic.InjectedCommitFailure"));
	CheckCallBefore(Test, Calls,
		TEXT("Capture.Close"), TEXT("Atomic.MarkReady"));
	CheckCallBefore(Test, Calls,
		TEXT("Atomic.MarkReady"), TEXT("Persistence.Persist"));
	CheckCallBefore(Test, Calls,
		TEXT("Persistence.Persist"), TEXT("Atomic.Commit"));
	CheckCallBefore(Test, Calls,
		TEXT("Atomic.Commit"), TEXT("Atomic.Rollback"));
	CheckPhases(Test, TEXT("commit failure observes exact phases"),
		Fixture.Observer.Phases,
		{EAssetDocumentRuntimeRequestPhase::Validate,
		 EAssetDocumentRuntimeRequestPhase::Preflight,
		 EAssetDocumentRuntimeRequestPhase::Apply,
		 EAssetDocumentRuntimeRequestPhase::Repair,
		 EAssetDocumentRuntimeRequestPhase::Persist,
		 EAssetDocumentRuntimeRequestPhase::Verify,
		 EAssetDocumentRuntimeRequestPhase::Failed});
}

void TestCancellationBetweenApplyAndRepair(FAutomationTestBase& Test)
{
	FMutationFixture Fixture;
	Fixture.Host.CancelAfterPhase = EAssetDocumentRuntimeRequestPhase::Apply;
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	Test.TestEqual(TEXT("Apply-boundary cancellation returns failed"),
		Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("Repair is never dispatched"),
		Fixture.Host.ObservedContexts.Num(), 3);
	Test.TestTrue(TEXT("capture was open during Apply"),
		Fixture.Host.bCaptureWasOpenDuringMutation == TArray<bool>{true});
	Test.TestEqual(TEXT("capture closes once"), Fixture.CaptureCloseCount, 1);
	Test.TestEqual(TEXT("provider enlists once"),
		Fixture.RecordingFactory.EnlistCount, 1);
	Test.TestEqual(TEXT("cancellation never binds the persistence target"),
		Fixture.PersistenceTargetBindCount, 0);
	Test.TestEqual(TEXT("cancellation never marks ready"),
		Fixture.RecordingFactory.MarkReadyCount, 0);
	Test.TestTrue(TEXT("cancellation never persists"),
		Fixture.Persistence.Calls.IsEmpty());
	Test.TestEqual(TEXT("cancellation never commits"),
		Fixture.RecordingFactory.CommitCount, 0);
	Test.TestEqual(TEXT("transaction rolls back once"),
		Fixture.RecordingFactory.RollbackCount, 1);
	Test.TestEqual(TEXT("participant rolls back once"),
		Fixture.Participant.RollbackCount, 1);
	CheckSingleDiagnostic(
		Test,
		Result,
		TEXT("RequestCoordinator.InvalidPhaseResult"));
	CheckSingleAtomicDiagnostic(
		Test,
		Fixture.AtomicDiagnostics,
		EAssetDocumentRuntimeRequestPhase::Apply,
		TEXT("Atomic.CancelledAtBoundary"));
	CheckCallBefore(Test, Fixture.AtomicRecord.Calls,
		TEXT("Capture.Close"), TEXT("Atomic.Rollback"));
	CheckPhases(Test, TEXT("Apply-boundary cancellation observes exact phases"),
		Fixture.Observer.Phases,
		{EAssetDocumentRuntimeRequestPhase::Validate,
		 EAssetDocumentRuntimeRequestPhase::Preflight,
		 EAssetDocumentRuntimeRequestPhase::Apply,
		 EAssetDocumentRuntimeRequestPhase::Failed});
}

void TestCancellationBeforeAtomicBegin(FAutomationTestBase& Test)
{
	FMutationFixture Fixture;
	Fixture.Host.CancelAfterPhase = EAssetDocumentRuntimeRequestPhase::Preflight;
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	Test.TestEqual(TEXT("pre-begin cancellation returns failed"),
		Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("only read phases dispatch"),
		Fixture.Host.ObservedContexts.Num(), 2);
	Test.TestEqual(TEXT("factory observes cancellation once"),
		Fixture.RecordingFactory.BeginCount, 1);
	Test.TestEqual(TEXT("capture never opens"), Fixture.CaptureOpenCount, 0);
	Test.TestEqual(TEXT("provider never enlists"),
		Fixture.RecordingFactory.EnlistCount, 0);
	Test.TestEqual(TEXT("ready is never marked"),
		Fixture.RecordingFactory.MarkReadyCount, 0);
	Test.TestTrue(TEXT("persistence never runs"),
		Fixture.Persistence.Calls.IsEmpty());
	Test.TestEqual(TEXT("commit never runs"),
		Fixture.RecordingFactory.CommitCount, 0);
	Test.TestEqual(TEXT("no live transaction rolls back"),
		Fixture.RecordingFactory.RollbackCount, 0);
	CheckSingleDiagnostic(
		Test,
		Result,
		TEXT("RequestCoordinator.InvalidPhaseResult"));
	CheckSingleAtomicDiagnostic(
		Test,
		Fixture.AtomicDiagnostics,
		EAssetDocumentRuntimeRequestPhase::Preflight,
		TEXT("Atomic.CancelledBeforeBegin"));
	CheckPhases(Test, TEXT("pre-begin cancellation observes exact phases"),
		Fixture.Observer.Phases,
		{EAssetDocumentRuntimeRequestPhase::Validate,
		 EAssetDocumentRuntimeRequestPhase::Preflight,
		 EAssetDocumentRuntimeRequestPhase::Failed});
}

void TestInvalidMutationHostResult(FAutomationTestBase& Test)
{
	FMutationFixture Fixture;
	Fixture.Host.InvalidResultPhase =
		EAssetDocumentRuntimeRequestPhase::Validate;
	const FAssetDocumentRuntimeResultDto Result = Fixture.Execute();
	Test.TestEqual(TEXT("invalid mutation Host result returns failed"),
		Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Failed);
	Test.TestEqual(TEXT("Validate context is created once"),
		Fixture.ContextCount, 1);
	Test.TestEqual(TEXT("invalid Host result is not retried"),
		Fixture.Host.ObservedContexts.Num(), 1);
	Test.TestEqual(TEXT("invalid Host result never begins"),
		Fixture.RecordingFactory.BeginCount, 0);
	Test.TestEqual(TEXT("invalid Host result never opens capture"),
		Fixture.CaptureOpenCount, 0);
	Test.TestEqual(TEXT("invalid Host result has no live rollback"),
		Fixture.RecordingFactory.RollbackCount, 0);
	CheckSingleDiagnostic(Test, Result, TEXT("RuntimeResult.InvalidIdentity"));
	CheckPhases(Test, TEXT("invalid Host result observes exact phases"),
		Fixture.Observer.Phases,
		{EAssetDocumentRuntimeRequestPhase::Validate,
		 EAssetDocumentRuntimeRequestPhase::Failed});
}

struct FReadOnlyContextObservation
{
	int32 ContextCount = 0;
	int32 PersistenceTargetBindCount = 0;
	bool bSawInvalidTransactionId = false;
	bool bSawNullActiveTransaction = false;
};

FAssetDocumentRequestCoordinatorRuntimeBindings MakeReadOnlyBindings(
	FReadOnlyContextObservation& Observation)
{
	FAssetDocumentRequestCoordinatorRuntimeBindings Bindings;
	Bindings.CreatePhaseContext = [&Observation](
		const FAssetDocumentRequestContextMetadata& Metadata,
		const FGuid& TransactionId,
		IAssetDocumentAtomicTransaction* ActiveTransaction,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
	{
		++Observation.ContextCount;
		Observation.bSawInvalidTransactionId = !TransactionId.IsValid();
		Observation.bSawNullActiveTransaction = ActiveTransaction == nullptr;
		return MakeUnique<FRequestContext>(Metadata);
	};
	Bindings.BindPersistenceTarget =
		[&Observation](
			const FAssetDocumentRuntimeRequestDto& BoundRequest,
			const FGuid& TransactionId,
			TConstArrayView<FAssetDocumentChangedRecord> ChangedSet,
			TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
		{
			++Observation.PersistenceTargetBindCount;
			return true;
		};
	return Bindings;
}

void TestReadOnlySuccess(FAutomationTestBase& Test)
{
	FReadOnlyContextObservation Observation;
	FAssetDocumentRequestCoordinatorRuntime Coordinator(
		MakeReadOnlyBindings(Observation));
	FHost Host;
	FCancellation Cancellation;
	FObserver Observer;
	FAssetDocumentRequestCoordinatorReadOnlyDependencies Dependencies{
		Host,
		Cancellation,
		Observer};
	const FAssetDocumentRuntimeResultDto Result =
		Coordinator.ExecuteReadOnly(MakeRequest(), Dependencies);
	Test.TestEqual(TEXT("read-only returns the validated Host terminal"),
		Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Succeeded);
	Test.TestEqual(TEXT("read-only creates one fresh context"),
		Observation.ContextCount, 1);
	Test.TestTrue(TEXT("read-only context has no transaction ID"),
		Observation.bSawInvalidTransactionId);
	Test.TestTrue(TEXT("read-only context has no active transaction"),
		Observation.bSawNullActiveTransaction);
	Test.TestEqual(TEXT("read-only dispatches Host once"), Host.DispatchCount, 1);
	Test.TestEqual(TEXT("read-only never binds a persistence target"),
		Observation.PersistenceTargetBindCount, 0);
	Test.TestEqual(TEXT("Host sees one context"), Host.ObservedMetadata.Num(), 1);
	Test.TestTrue(TEXT("Host context keeps request identity"),
		Host.ObservedMetadata[0].RequestId == Result.RequestId);
	Test.TestEqual(TEXT("Host context keeps pinned revision"),
		Host.ObservedMetadata[0].PinnedRuntimeRevision, uint64(7));
	Test.TestEqual(TEXT("Host context is Validate"),
		Host.ObservedMetadata[0].Phase,
		EAssetDocumentRuntimeRequestPhase::Validate);
	CheckPhases(Test, TEXT("read-only success observes only Validate"),
		Observer.ObservedPhases,
		{EAssetDocumentRuntimeRequestPhase::Validate});
}

class FEqualDeltaHost final : public IAssetDocumentPythonRuntimeHost
{
public:
	int32 Mode = 0;
	AssetDocumentAtomicFrameworkTestDoubles::FCancellationView* Cancellation = nullptr;
	TArray<EAssetDocumentRuntimeRequestPhase> Phases;
	FAssetDocumentRuntimeResultDto Dispatch(const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) override
	{
		const auto Phase = Context.GetMetadata().Phase; Phases.Add(Phase);
		FAssetDocumentRuntimeResultDto Result;
		Result.RequestId=Request.RequestId; Result.RuntimeRevision=Request.ExpectedRuntimeRevision;
		Result.TerminalStatus=EAssetDocumentRuntimeTerminalStatus::Succeeded;
		Result.FinalPhase=EAssetDocumentRuntimeRequestPhase::Complete;
		Result.SemanticHash=TEXT("equal:v1"); Result.CodecVersion=1; Result.CanonicalBytes={0x7b,0x7d};
		if(Mode==2 && Phase==EAssetDocumentRuntimeRequestPhase::Apply)
			Result.ChangedSet.Add({TEXT("test"),TEXT("asset"),TEXT("/Body"),TEXT("Update")});
		if(Mode==3 && Phase==EAssetDocumentRuntimeRequestPhase::Verify) Result.SemanticHash=TEXT("different:v1");
		if(Mode==6 && Phase==EAssetDocumentRuntimeRequestPhase::Apply)
		{
			Result.TerminalStatus=EAssetDocumentRuntimeTerminalStatus::Failed;Result.FinalPhase=EAssetDocumentRuntimeRequestPhase::Failed;
		}
		if(Mode==7 && Phase==EAssetDocumentRuntimeRequestPhase::Apply) Cancellation->bCancelled=true;
		return Result;
	}
};

void TestEqualDeltaExecution(FAutomationTestBase& Test)
{
	for(int32 Mode=0;Mode!=10;++Mode)
	{
		FEqualDeltaHost Host;Host.Mode=Mode;
		AssetDocumentAtomicFrameworkTestDoubles::FCancellationView Cancellation;Host.Cancellation=&Cancellation;
		FObserver Observer;
		FAssetDocumentRequestCoordinatorReadOnlyDependencies Dependencies{Host,Cancellation,Observer};
		auto Request=MakeRequest();Request.Operation=EAssetDocumentRuntimeOperation::Apply;Request.RequestedAssetClassPath=TEXT("/Script/Engine.DataAsset");
		int32 Contexts=0,FreshChecks=0,CaptureOpens=0;
		bool bTransactionSeen=false;
		FAssetDocumentRequestCoordinatorRuntimeBindings Bindings;
		Bindings.CreatePhaseContext=[&](const auto& Metadata,const FGuid& Id,IAssetDocumentAtomicTransaction* Transaction,TArray<FAssetDocumentRuntimeDiagnostic>&)
			->TUniquePtr<IAssetDocumentNativeRequestContext>
		{
			++Contexts;bTransactionSeen|=Id.IsValid() || Transaction!=nullptr;
			if(Mode==8 && Metadata.Phase==EAssetDocumentRuntimeRequestPhase::Verify) return nullptr;
			return MakeUnique<FRequestContext>(Metadata);
		};
		if(Mode==9) Bindings.OpenMutationCapture=[&](const FGuid&,TArray<FAssetDocumentRuntimeDiagnostic>&)
			->TUniquePtr<FAssetDocumentCoreMutationCaptureLease> {++CaptureOpens;return nullptr;};
		FAssetDocumentCoreEqualDeltaInputs Inputs;
		Inputs.RequestId=Mode==1?FGuid::NewGuid():Request.RequestId;Inputs.RuntimeRevision=Request.ExpectedRuntimeRevision;
		Inputs.TargetAssetPath=Request.TargetAssetPath;Inputs.ExactAssetClassPath=Request.RequestedAssetClassPath;
		Inputs.SemanticHash=TEXT("equal:v1");Inputs.CodecVersion=1;
		Inputs.ValidateLive=[] {FAssetDocumentContractValidationResult R;R.bValid=true;return R;};
		Inputs.VerifyDurable=[&]
		{
			++FreshChecks;FAssetDocumentFreshVerificationResult R;
			R.bFreshLoadProven=Mode!=4;R.bCanonicalMatch=Mode!=5;
			R.ActualSemanticHash=TEXT("equal:v1");R.ActualCodecVersion=1;return R;
		};
		FAssetDocumentRequestCoordinatorRuntime Coordinator(MoveTemp(Bindings));
		const auto Result=Coordinator.ExecuteEqualDelta(Request,Dependencies,MoveTemp(Inputs));
		Test.TestEqual(TEXT("only fully proven EqualDelta succeeds"),Result.TerminalStatus==EAssetDocumentRuntimeTerminalStatus::Succeeded,Mode==0);
		Test.TestTrue(TEXT("EqualDelta reports zero changed records"),Result.ChangedSet.IsEmpty());
		Test.TestFalse(TEXT("EqualDelta never exposes transaction identity or active transaction"),bTransactionSeen);
		Test.TestEqual(TEXT("EqualDelta never opens a mutation capture"),CaptureOpens,0);
		Test.TestTrue(TEXT("all EqualDelta success/failure phase edges remain legal"),AreObserverEdgesValid(Observer.ObservedPhases));
		Test.TestFalse(TEXT("EqualDelta never dispatches a Persist operation"),Host.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Persist));
		if(Mode==0)
		{
			Test.TestEqual(TEXT("independent durable proof runs once"),FreshChecks,1);
			CheckPhases(Test,TEXT("EqualDelta follows the normal legal Apply lifecycle"),Observer.ObservedPhases,
				{EAssetDocumentRuntimeRequestPhase::Validate,EAssetDocumentRuntimeRequestPhase::Preflight,EAssetDocumentRuntimeRequestPhase::Apply,
				 EAssetDocumentRuntimeRequestPhase::Repair,EAssetDocumentRuntimeRequestPhase::Persist,EAssetDocumentRuntimeRequestPhase::Verify,EAssetDocumentRuntimeRequestPhase::Complete});
		}
		if(Mode==1 || Mode==9) Test.TestEqual(TEXT("invalid no-write binding fails before native contexts"),Contexts,0);
		if(Mode==2 || Mode==3 || Mode==6 || Mode==7 || Mode==8) Test.TestEqual(TEXT("phase rejection never attempts durable completion"),FreshChecks,0);
	}
}
} // namespace AssetDocumentRequestCoordinatorRuntimeTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRequestCoordinatorRuntimeTest,
	"UEAssetDoc.AtomicFramework.RequestCoordinatorRuntime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRequestCoordinatorRuntimeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRequestCoordinatorRuntimeTests;

	TestReadOnlySuccess(*this);
	TestEqualDeltaExecution(*this);
	TestMutationSuccess(*this);
	TestPersistenceTargetBindingFailure(*this);
	TestValidationFailures(*this);
	TestContextCreationFailures(*this);
	TestBeginAndCaptureFailures(*this);
	TestProviderEnlistFailure(*this);
	TestApplyFailure(*this);
	TestRepairFailure(*this);
	TestPostRollbackFinalizerOnCoordinatorFailure(*this, false);
	TestPostRollbackFinalizerOnCoordinatorFailure(*this, true);
	TestIncompleteProviderCoverage(*this);
	TestInvalidDurableReceipt(*this);
	TestCommitFailure(*this);
	TestCancellationBetweenApplyAndRepair(*this);
	TestCancellationBeforeAtomicBegin(*this);
	TestInvalidMutationHostResult(*this);
	return true;
}

#endif
