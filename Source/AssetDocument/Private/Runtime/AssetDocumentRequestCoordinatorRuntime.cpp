// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentRequestCoordinatorRuntime.h"
#include "Runtime/AssetDocumentRuntimeTestHooks.h"

namespace
{
FAssetDocumentRuntimeResultDto MakeRequestCoordinatorFailure(
	const FAssetDocumentRuntimeRequestDto& Request,
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	FAssetDocumentRuntimeResultDto Result;
	Result.RequestId = Request.RequestId;
	Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
	Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
	Result.RuntimeRevision = Request.ExpectedRuntimeRevision != 0
		? Request.ExpectedRuntimeRevision
		: 1;
	Result.Diagnostics = MoveTemp(Diagnostics);
	return Result;
}

FAssetDocumentRuntimeDiagnostic MakeRequestCoordinatorDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/RequestCoordinator");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
	return Diagnostic;
}

FAssetDocumentRuntimeDiagnostic MakePersistenceTargetBindingDiagnostic()
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/CoreRuntimeComposition/PersistenceTarget");
	Diagnostic.Code =
		TEXT("CoreRuntimeComposition.PersistenceTargetBindingFailed");
	Diagnostic.Message = TEXT("Persistence target binding failed.");
	Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
	return Diagnostic;
}

bool RequestCoordinatorFootprintItemsMatch(
	const FAssetDocumentMutationFootprint& Left,
	const FAssetDocumentMutationFootprint& Right)
{
	return Left.Kind == Right.Kind
		&& Left.StableId == Right.StableId
		&& Left.RollbackComponentId == Right.RollbackComponentId;
}

bool RequestCoordinatorFootprintsMatch(
	TConstArrayView<FAssetDocumentMutationFootprint> Left,
	TConstArrayView<FAssetDocumentMutationFootprint> Right)
{
	if (Left.Num() != Right.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Left.Num(); ++Index)
	{
		if (!RequestCoordinatorFootprintItemsMatch(
			Left[Index],
			Right[Index]))
		{
			return false;
		}
	}
	return true;
}

bool IsRequestCoordinatorTransactionTerminal(
	const IAssetDocumentAtomicTransaction& Transaction)
{
	const EAssetDocumentAtomicState State = Transaction.GetState();
	return State == EAssetDocumentAtomicState::Committed
		|| State == EAssetDocumentAtomicState::RolledBack
		|| State == EAssetDocumentAtomicState::Poisoned;
}

bool DoesRequestCoordinatorDurableReceiptIdentityMatch(
	const FAssetDocumentDurableCompletionReceipt& Receipt,
	const FAssetDocumentRuntimeRequestDto& Request,
	const IAssetDocumentAtomicTransaction& Transaction)
{
	return Receipt.RequestId == Request.RequestId
		&& Receipt.TransactionId == Transaction.GetTransactionId();
}

bool HasRequestCoordinatorDurableReceiptPayload(
	const FAssetDocumentDurableCompletionReceipt& Receipt)
{
	return !Receipt.JournalId.IsEmpty()
		&& !Receipt.PackageContentDigest.IsEmpty()
		&& !Receipt.SidecarContentDigest.IsEmpty()
		&& Receipt.bSyncStateAdvanced;
}

bool IsRequestCoordinatorDurableReceiptValid(
	const FAssetDocumentDurableCompletionReceipt& Receipt,
	const FAssetDocumentRuntimeRequestDto& Request,
	const IAssetDocumentAtomicTransaction& Transaction)
{
	return DoesRequestCoordinatorDurableReceiptIdentityMatch(
		Receipt,
		Request,
		Transaction)
		&& HasRequestCoordinatorDurableReceiptPayload(Receipt);
}

void EmitRequestCoordinatorDiagnostics(
	const FAssetDocumentRequestCoordinatorRuntimeBindings& Bindings,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	if (!Bindings.DiagnosticSink)
	{
		return;
	}
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Diagnostics)
	{
		Bindings.DiagnosticSink(Diagnostic);
	}
}

void ObserveReadOnlyFailure(
	const FAssetDocumentRuntimeRequestDto& Request,
	FAssetDocumentRequestCoordinatorReadOnlyDependencies& Dependencies)
{
	if (IsAssetDocumentRequestPhaseTransitionAllowed(
		EAssetDocumentRuntimeRequestPhase::Validate,
		EAssetDocumentRuntimeRequestPhase::Failed))
	{
		Dependencies.Observer.OnPhaseChanged(
			Request.RequestId,
			EAssetDocumentRuntimeRequestPhase::Failed);
	}
}

TUniquePtr<IAssetDocumentNativeRequestContext> CreateReadOnlyContext(
	const FAssetDocumentRequestCoordinatorRuntimeBindings& Bindings,
	const FAssetDocumentRuntimeRequestDto& Request,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	if (!Bindings.CreatePhaseContext)
	{
		return nullptr;
	}
	FAssetDocumentRequestContextMetadata Metadata;
	Metadata.RequestId = Request.RequestId;
	Metadata.PinnedRuntimeRevision = Request.ExpectedRuntimeRevision;
	Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	return Bindings.CreatePhaseContext(
		Metadata,
		FGuid(),
		nullptr,
		OutDiagnostics);
}

FAssetDocumentRuntimeResultDto FinishReadOnlyFailure(
	const FAssetDocumentRequestCoordinatorRuntimeBindings& Bindings,
	const FAssetDocumentRuntimeRequestDto& Request,
	FAssetDocumentRequestCoordinatorReadOnlyDependencies& Dependencies,
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	EmitRequestCoordinatorDiagnostics(Bindings, Diagnostics);
	ObserveReadOnlyFailure(Request, Dependencies);
	return MakeRequestCoordinatorFailure(Request, MoveTemp(Diagnostics));
}

bool IsMutationHostPhase(EAssetDocumentRuntimeRequestPhase Phase)
{
	return Phase == EAssetDocumentRuntimeRequestPhase::Apply
		|| Phase == EAssetDocumentRuntimeRequestPhase::Repair;
}

class FRequestCoordinatorMutationExecution final
{
public:
	FRequestCoordinatorMutationExecution(
		FAssetDocumentRequestCoordinatorRuntimeBindings& InBindings,
		const FAssetDocumentRuntimeRequestDto& InRequest,
		FAssetDocumentRequestCoordinatorDependencies& InDependencies)
		: Bindings(InBindings)
		, Request(InRequest)
		, Dependencies(InDependencies)
	{
	}

	FAssetDocumentRuntimeResultDto Run()
	{
		if (!ValidateInputs())
		{
			return FinishFailure();
		}
		if (!RunInitialHostPhases())
		{
			return FinishFailure();
		}
		if (!RunMutationHostPhases())
		{
			return FinishFailure();
		}
		if (!PersistAndCommit())
		{
			return FinishFailure();
		}
		return FinishSuccess();
	}

private:
	bool RunInitialHostPhases()
	{
		if (!RunHostPhase(EAssetDocumentRuntimeRequestPhase::Validate))
		{
			return false;
		}
		return RunHostPhase(EAssetDocumentRuntimeRequestPhase::Preflight);
	}

	bool RunMutationHostPhases()
	{
		if (!BeginTransactionAndCapture())
		{
			return false;
		}
		if (!RunHostPhase(EAssetDocumentRuntimeRequestPhase::Apply))
		{
			return false;
		}
		if (!ContinueAfterMutationBoundary())
		{
			return false;
		}
		return RunHostPhase(EAssetDocumentRuntimeRequestPhase::Repair);
	}

	bool ValidateInputs()
	{
		const FAssetDocumentContractValidationResult RequestValidation =
			ValidateAssetDocumentRuntimeRequest(Request);
		if (!RequestValidation.bValid)
		{
			AppendDiagnostics(RequestValidation.Diagnostics);
			return false;
		}
		if (!HasRequiredMutationBindings())
		{
			AddFailure(
				TEXT("RequestCoordinator.InvalidPhaseResult"),
				TEXT("Mutation execution dependencies are incomplete."));
			return false;
		}
		const FAssetDocumentContractValidationResult PreflightValidation =
			ValidateAssetDocumentPreflightReceipt(*Bindings.PreflightReceipt);
		if (!PreflightValidation.bValid)
		{
			AppendDiagnostics(PreflightValidation.Diagnostics);
			return false;
		}
		return ValidateBoundIdentity();
	}

	bool HasRequiredMutationBindings() const
	{
		return Bindings.PreflightReceipt != nullptr
			&& Bindings.PersistencePlanTemplate != nullptr
			&& Bindings.Verifier != nullptr
			&& Bindings.BindPersistenceTarget;
	}

	bool ValidateBoundIdentity()
	{
		if (!BoundIdentityMatches())
		{
			AddIdentityOrFootprintFailure();
			return false;
		}
		if (!RequestCoordinatorFootprintsMatch(
			Bindings.PreflightReceipt->CompleteFootprint,
			Bindings.PersistencePlanTemplate->Footprint))
		{
			AddIdentityOrFootprintFailure();
			return false;
		}
		return true;
	}

	bool BoundIdentityMatches() const
	{
		const FAssetDocumentPreflightReceipt& Preflight =
			*Bindings.PreflightReceipt;
		const FAssetDocumentPersistencePlan& Plan =
			*Bindings.PersistencePlanTemplate;
		return Preflight.RequestId == Request.RequestId
			&& Preflight.RuntimeRevision == Request.ExpectedRuntimeRevision
			&& Plan.RequestId == Request.RequestId
			&& !Plan.TransactionId.IsValid();
	}

	void AddIdentityOrFootprintFailure()
	{
		AddFailure(
			TEXT("RequestCoordinator.InvalidPhaseResult"),
			TEXT("Mutation execution identities or footprints do not match."));
	}

	bool RunHostPhase(EAssetDocumentRuntimeRequestPhase Phase)
	{
		if (!Observe(Phase))
		{
			return false;
		}
		TArray<FAssetDocumentRuntimeDiagnostic> ContextDiagnostics;
		TUniquePtr<IAssetDocumentNativeRequestContext> Context =
			CreatePhaseContext(Phase, ContextDiagnostics);
		if (!Context)
		{
			HandleContextCreationFailure(ContextDiagnostics);
			return false;
		}
		return DispatchHost(*Context);
	}

	TUniquePtr<IAssetDocumentNativeRequestContext> CreatePhaseContext(
		EAssetDocumentRuntimeRequestPhase Phase,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
	{
		if (!Bindings.CreatePhaseContext)
		{
			return nullptr;
		}
		FAssetDocumentRequestContextMetadata Metadata;
		Metadata.RequestId = Request.RequestId;
		Metadata.PinnedRuntimeRevision = Request.ExpectedRuntimeRevision;
		Metadata.Phase = Phase;
		FGuid TransactionId;
		if (Transaction)
		{
			TransactionId = Transaction->GetTransactionId();
		}
		IAssetDocumentAtomicTransaction* ActiveTransaction =
			IsMutationHostPhase(Phase) ? Transaction.Get() : nullptr;
		return Bindings.CreatePhaseContext(
			Metadata,
			TransactionId,
			ActiveTransaction,
			OutDiagnostics);
	}

	void HandleContextCreationFailure(
		TConstArrayView<FAssetDocumentRuntimeDiagnostic> ContextDiagnostics)
	{
		if (!ContextDiagnostics.IsEmpty())
		{
			AppendDiagnostics(ContextDiagnostics);
			return;
		}
		AddFailure(
			TEXT("RequestCoordinator.ContextCreationFailed"),
			TEXT("A phase-local request context could not be created."));
	}

	bool DispatchHost(IAssetDocumentNativeRequestContext& Context)
	{
		FAssetDocumentRuntimeResultDto Result =
			Dependencies.Host.Dispatch(Request, Context);
		const FAssetDocumentContractValidationResult Validation =
			ValidateAssetDocumentRuntimeResult(Request, Result);
		if (!Validation.bValid)
		{
			AppendDiagnostics(Validation.Diagnostics);
			if (Validation.Diagnostics.IsEmpty())
			{
				AddFailure(
					TEXT("RequestCoordinator.InvalidPhaseResult"),
					TEXT("A phase returned an invalid runtime result."));
			}
			return false;
		}
		AppendDiagnostics(Result.Diagnostics);
		ChangedSet.Append(Result.ChangedSet);
		LastResult = MoveTemp(Result);
		return LastResult.TerminalStatus
			== EAssetDocumentRuntimeTerminalStatus::Succeeded;
	}

	bool BeginTransactionAndCapture()
	{
		if (!BeginTransaction())
		{
			return false;
		}
		if (!BindPersistencePlan())
		{
			return false;
		}
		return OpenMutationCapture();
	}

	bool BeginTransaction()
	{
		Transaction =
			Dependencies.TransactionFactory.BeginLiveAfterPreflight(
				*Bindings.PreflightReceipt,
				Dependencies.Cancellation);
		if (!Transaction)
		{
			AddFailure(
				TEXT("RequestCoordinator.InvalidPhaseResult"),
				TEXT("Atomic begin did not produce a live transaction."));
			return false;
		}
		return true;
	}

	bool BindPersistencePlan()
	{
		BoundPersistencePlan = *Bindings.PersistencePlanTemplate;
		BoundPersistencePlan.TransactionId = Transaction->GetTransactionId();
		const FAssetDocumentContractValidationResult Validation =
			ValidateAssetDocumentPersistencePlan(BoundPersistencePlan);
		if (!Validation.bValid)
		{
			AppendDiagnostics(Validation.Diagnostics);
			return false;
		}
		return true;
	}

	bool OpenMutationCapture()
	{
		if (!Bindings.OpenMutationCapture)
		{
			AddFailure(
				TEXT("RequestCoordinator.MutationCaptureOpenFailed"),
				TEXT("Mutation capture opener is not bound."));
			return false;
		}
		TArray<FAssetDocumentRuntimeDiagnostic> CaptureDiagnostics;
		Capture = Bindings.OpenMutationCapture(
			Transaction->GetTransactionId(),
			CaptureDiagnostics);
		AppendDiagnostics(CaptureDiagnostics);
		if (!Capture || !Capture->IsValid())
		{
			if (CaptureDiagnostics.IsEmpty())
			{
				AddFailure(
					TEXT("RequestCoordinator.MutationCaptureOpenFailed"),
					TEXT("Mutation capture did not return a valid lease."));
			}
			return false;
		}
		return true;
	}

	bool ContinueAfterMutationBoundary()
	{
		if (!Dependencies.Cancellation.IsCancellationRequested())
		{
			return true;
		}
		AddFailure(
			TEXT("RequestCoordinator.InvalidPhaseResult"),
			TEXT("The request was cancelled at a legal mutation boundary."));
		RollbackFailureCode = TEXT("Atomic.CancelledAtBoundary");
		return false;
	}

	bool PersistAndCommit()
	{
		if (!PreparePersistence())
		{
			return false;
		}
		FAssetDocumentDurableCompletionReceipt Receipt;
		if (!PersistDurably(Receipt))
		{
			return false;
		}
		return CommitDurableReceipt(Receipt);
	}

	bool PreparePersistence()
	{
		CloseCapture();
		if (!BindPersistenceTarget())
		{
			return false;
		}
		const FAssetDocumentContractValidationResult Ready =
			Transaction->MarkReadyToCommit();
		if (!Ready.bValid)
		{
			AppendDiagnostics(Ready.Diagnostics);
			return false;
		}
		return Observe(EAssetDocumentRuntimeRequestPhase::Persist);
	}

	bool BindPersistenceTarget()
	{
		TArray<FAssetDocumentRuntimeDiagnostic> BindingDiagnostics;
		const bool bBound = Bindings.BindPersistenceTarget(
			Request,
			Transaction->GetTransactionId(),
			ChangedSet,
			BindingDiagnostics);
		if (!bBound && BindingDiagnostics.IsEmpty())
		{
			BindingDiagnostics.Add(
				MakePersistenceTargetBindingDiagnostic());
		}
		AppendDiagnostics(BindingDiagnostics);
		return bBound;
	}

	bool PersistDurably(
		FAssetDocumentDurableCompletionReceipt& OutReceipt)
	{
		OutReceipt =
			Dependencies.Persistence.Persist(
				BoundPersistencePlan,
				*Bindings.Verifier);
		if (!IsRequestCoordinatorDurableReceiptValid(
			OutReceipt,
			Request,
			*Transaction))
		{
			AddFailure(
				TEXT("RequestCoordinator.InvalidDurableReceipt"),
				TEXT("Persistence returned an invalid durable receipt."));
			return false;
		}
		return true;
	}

	bool CommitDurableReceipt(
		const FAssetDocumentDurableCompletionReceipt& Receipt)
	{
		if (!Observe(EAssetDocumentRuntimeRequestPhase::Verify))
		{
			return false;
		}
		const FAssetDocumentContractValidationResult Committed =
			Transaction->Commit(Receipt);
		if (!Committed.bValid)
		{
			AppendDiagnostics(Committed.Diagnostics);
			return false;
		}
		return Observe(EAssetDocumentRuntimeRequestPhase::Complete);
	}

	bool Observe(EAssetDocumentRuntimeRequestPhase Phase)
	{
		if (bHasObservedPhase
			&& !IsAssetDocumentRequestPhaseTransitionAllowed(
				CurrentPhase,
				Phase))
		{
			AddFailure(
				TEXT("RequestCoordinator.InvalidPhaseResult"),
				TEXT("The requested phase transition is not allowed."));
			return false;
		}
		Dependencies.Observer.OnPhaseChanged(Request.RequestId, Phase);
		CurrentPhase = Phase;
		bHasObservedPhase = true;
		if (FAssetDocumentRuntimeTestHooks::ConsumeRequestPhaseFailure(Phase))
		{
			AddFailure(
				TEXT("RequestCoordinator.TestInjectedFailure"),
				TEXT("A test injected a failure at this phase."));
			return false;
		}
		return true;
	}

	void AddFailure(const TCHAR* Code, const TCHAR* Message)
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Added;
		Added.Add(MakeRequestCoordinatorDiagnostic(Code, Message));
		AppendDiagnostics(Added);
		if (FailureCode.IsEmpty())
		{
			FailureCode = Code;
		}
	}

	void AppendDiagnostics(
		TConstArrayView<FAssetDocumentRuntimeDiagnostic> Added)
	{
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Added)
		{
			Diagnostics.Add(Diagnostic);
			if (Bindings.DiagnosticSink)
			{
				Bindings.DiagnosticSink(Diagnostic);
			}
			if (FailureCode.IsEmpty() && !Diagnostic.Code.IsEmpty())
			{
				FailureCode = Diagnostic.Code;
			}
		}
	}

	void CloseCapture()
	{
		if (Capture)
		{
			Capture->Close();
			Capture.Reset();
		}
	}

	FAssetDocumentRuntimeResultDto FinishFailure()
	{
		CloseCapture();
		RollbackLiveTransaction();
		ObserveFailureIfAllowed();
		return MakeRequestCoordinatorFailure(Request, MoveTemp(Diagnostics));
	}

	FString GetRollbackFailureCode() const
	{
		if (!RollbackFailureCode.IsEmpty())
		{
			return RollbackFailureCode;
		}
		return FailureCode.IsEmpty()
			? TEXT("RequestCoordinator.InvalidPhaseResult")
			: FailureCode;
	}

	void RollbackLiveTransaction()
	{
		if (!Transaction
			|| IsRequestCoordinatorTransactionTerminal(*Transaction))
		{
			return;
		}
		const FAssetDocumentContractValidationResult RolledBack =
			Transaction->Rollback(GetRollbackFailureCode());
		AppendDiagnostics(RolledBack.Diagnostics);
	}

	void ObserveFailureIfAllowed()
	{
		if (!bHasObservedPhase
			|| CurrentPhase == EAssetDocumentRuntimeRequestPhase::Failed
			|| !IsAssetDocumentRequestPhaseTransitionAllowed(
				CurrentPhase,
				EAssetDocumentRuntimeRequestPhase::Failed))
		{
			return;
		}
		Observe(EAssetDocumentRuntimeRequestPhase::Failed);
	}

	FAssetDocumentRuntimeResultDto FinishSuccess()
	{
		LastResult.Diagnostics = MoveTemp(Diagnostics);
		LastResult.ChangedSet = MoveTemp(ChangedSet);
		return MoveTemp(LastResult);
	}

	FAssetDocumentRequestCoordinatorRuntimeBindings& Bindings;
	const FAssetDocumentRuntimeRequestDto& Request;
	FAssetDocumentRequestCoordinatorDependencies& Dependencies;
	FAssetDocumentPersistencePlan BoundPersistencePlan;
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction;
	TUniquePtr<FAssetDocumentCoreMutationCaptureLease> Capture;
	FAssetDocumentRuntimeResultDto LastResult;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TArray<FAssetDocumentChangedRecord> ChangedSet;
	FString FailureCode;
	FString RollbackFailureCode;
	EAssetDocumentRuntimeRequestPhase CurrentPhase =
		EAssetDocumentRuntimeRequestPhase::Validate;
	bool bHasObservedPhase = false;
};
} // namespace

FAssetDocumentRequestCoordinatorRuntime::
	FAssetDocumentRequestCoordinatorRuntime(
		FAssetDocumentRequestCoordinatorRuntimeBindings InBindings)
	: Bindings(MoveTemp(InBindings))
{
}

FAssetDocumentRuntimeResultDto
FAssetDocumentRequestCoordinatorRuntime::ExecuteReadOnly(
	const FAssetDocumentRuntimeRequestDto& Request,
	FAssetDocumentRequestCoordinatorReadOnlyDependencies& Dependencies)
{
	const FAssetDocumentContractValidationResult RequestValidation =
		ValidateAssetDocumentRuntimeRequest(Request);
	if (!RequestValidation.bValid)
	{
		EmitRequestCoordinatorDiagnostics(
			Bindings,
			RequestValidation.Diagnostics);
		return MakeRequestCoordinatorFailure(
			Request,
			RequestValidation.Diagnostics);
	}

	Dependencies.Observer.OnPhaseChanged(
		Request.RequestId,
		EAssetDocumentRuntimeRequestPhase::Validate);
	TArray<FAssetDocumentRuntimeDiagnostic> ContextDiagnostics;
	TUniquePtr<IAssetDocumentNativeRequestContext> Context =
		CreateReadOnlyContext(Bindings, Request, ContextDiagnostics);
	if (!Context)
	{
		if (ContextDiagnostics.IsEmpty())
		{
			ContextDiagnostics.Add(MakeRequestCoordinatorDiagnostic(
				TEXT("RequestCoordinator.ContextCreationFailed"),
				TEXT("The Validate request context could not be created.")));
		}
		return FinishReadOnlyFailure(
			Bindings,
			Request,
			Dependencies,
			MoveTemp(ContextDiagnostics));
	}

	FAssetDocumentRuntimeResultDto Result =
		Dependencies.Host.Dispatch(Request, *Context);
	const FAssetDocumentContractValidationResult ResultValidation =
		ValidateAssetDocumentRuntimeResult(Request, Result);
	if (!ResultValidation.bValid)
	{
		return FinishReadOnlyFailure(
			Bindings,
			Request,
			Dependencies,
			ResultValidation.Diagnostics);
	}
	return Result;
}

FAssetDocumentRuntimeResultDto
FAssetDocumentRequestCoordinatorRuntime::Execute(
	const FAssetDocumentRuntimeRequestDto& Request,
	FAssetDocumentRequestCoordinatorDependencies& Dependencies)
{
	FRequestCoordinatorMutationExecution Execution(
		Bindings,
		Request,
		Dependencies);
	return Execution.Run();
}

FAssetDocumentRuntimeResultDto
FAssetDocumentRequestCoordinatorRuntime::ExecuteEqualDelta(
	const FAssetDocumentRuntimeRequestDto& Request,
	FAssetDocumentRequestCoordinatorReadOnlyDependencies& Dependencies,
	FAssetDocumentCoreEqualDeltaInputs&& Inputs)
{
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FAssetDocumentRuntimeResultDto LastResult;
	TOptional<EAssetDocumentRuntimeRequestPhase> CurrentPhase;
	auto Observe = [&](EAssetDocumentRuntimeRequestPhase Phase)
	{
		if (CurrentPhase.IsSet()
			&& !IsAssetDocumentRequestPhaseTransitionAllowed(CurrentPhase.GetValue(), Phase))
		{
			Diagnostics.Add(MakeRequestCoordinatorDiagnostic(
				TEXT("RequestCoordinator.EqualDeltaPhaseRejected"),
				TEXT("EqualDelta must use the existing legal request phase transitions.")));
			return false;
		}
		CurrentPhase = Phase;
		Dependencies.Observer.OnPhaseChanged(Request.RequestId, Phase);
		return true;
	};
	auto Fail = [&]()
	{
		if (CurrentPhase.IsSet()
			&& IsAssetDocumentRequestPhaseTransitionAllowed(CurrentPhase.GetValue(), EAssetDocumentRuntimeRequestPhase::Failed))
		{
			Observe(EAssetDocumentRuntimeRequestPhase::Failed);
		}
		EmitRequestCoordinatorDiagnostics(Bindings, Diagnostics);
		return MakeRequestCoordinatorFailure(Request, MoveTemp(Diagnostics));
	};
	auto CheckCancellation = [&]()
	{
		if (!Dependencies.Cancellation.IsCancellationRequested()) return true;
		Diagnostics.Add(MakeRequestCoordinatorDiagnostic(
			TEXT("RequestCoordinator.Cancelled"), TEXT("The EqualDelta request was cancelled.")));
		return false;
	};
	auto ValidateLive = [&]()
	{
		const auto Valid = Inputs.ValidateLive();
		Diagnostics.Append(Valid.Diagnostics);
		if (!Valid.bValid && Valid.Diagnostics.IsEmpty()) Diagnostics.Add(MakeRequestCoordinatorDiagnostic(
			TEXT("RequestCoordinator.EqualDeltaLiveEvidenceRejected"), TEXT("The exact live native read owner rejected EqualDelta.")));
		return Valid.bValid;
	};
	auto RunPhase = [&](EAssetDocumentRuntimeRequestPhase Phase, bool bRequireSemanticEvidence)
	{
		if (!CheckCancellation() || !Observe(Phase)) return false;
		FAssetDocumentRequestContextMetadata Metadata;
		Metadata.RequestId = Request.RequestId;
		Metadata.PinnedRuntimeRevision = Request.ExpectedRuntimeRevision;
		Metadata.Phase = Phase;
		// No transaction exists. Context destruction retires its scratch read handles.
		auto Context = Bindings.CreatePhaseContext(Metadata, FGuid(), nullptr, Diagnostics);
		if (!Context || Context->GetMetadata().RequestId != Metadata.RequestId
			|| Context->GetMetadata().PinnedRuntimeRevision != Metadata.PinnedRuntimeRevision
			|| Context->GetMetadata().Phase != Phase)
		{
			Diagnostics.Add(MakeRequestCoordinatorDiagnostic(
				TEXT("RequestCoordinator.EqualDeltaContextRejected"),
				TEXT("EqualDelta requires its exact request, revision and phase read context.")));
			return false;
		}
		auto Result = Dependencies.Host.Dispatch(Request, *Context);
		const auto Valid = ValidateAssetDocumentRuntimeResult(Request, Result);
		Diagnostics.Append(Valid.Diagnostics);
		Diagnostics.Append(Result.Diagnostics);
		if (!Valid.bValid || Result.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded) return false;
		if (!Result.ChangedSet.IsEmpty()
			|| (bRequireSemanticEvidence && (Result.SemanticHash != Inputs.SemanticHash || Result.CodecVersion != Inputs.CodecVersion)))
		{
			Diagnostics.Add(MakeRequestCoordinatorDiagnostic(
				TEXT("RequestCoordinator.EqualDeltaEvidenceMismatch"),
				TEXT("A no-write phase must report no changes and matching Python semantic evidence.")));
			return false;
		}
		LastResult = MoveTemp(Result);
		return true;
	};
	const auto RequestValidation = ValidateAssetDocumentRuntimeRequest(Request);
	Diagnostics.Append(RequestValidation.Diagnostics);
	if (!RequestValidation.bValid) return Fail();
	if (Request.Operation != EAssetDocumentRuntimeOperation::Apply
		|| Inputs.RequestId != Request.RequestId || Inputs.RuntimeRevision != Request.ExpectedRuntimeRevision
		|| Inputs.TargetAssetPath.IsEmpty() || Inputs.ExactAssetClassPath.IsEmpty()
		|| Inputs.TargetAssetPath != Request.TargetAssetPath || Inputs.ExactAssetClassPath != Request.RequestedAssetClassPath
		|| Inputs.SemanticHash.IsEmpty() || Inputs.CodecVersion == 0
		|| !Bindings.CreatePhaseContext || !Inputs.ValidateLive || !Inputs.VerifyDurable
		|| Bindings.PreflightReceipt || Bindings.PersistencePlanTemplate || Bindings.OpenMutationCapture
		|| Bindings.BindPersistenceTarget || Bindings.Verifier)
	{
		Diagnostics.Add(MakeRequestCoordinatorDiagnostic(
			TEXT("RequestCoordinator.EqualDeltaInputsRejected"),
			TEXT("EqualDelta requires an exact private no-write binding, without mutation inputs.")));
		return Fail();
	}
	if (!CheckCancellation() || !ValidateLive()
		|| !RunPhase(EAssetDocumentRuntimeRequestPhase::Validate, true)
		|| !RunPhase(EAssetDocumentRuntimeRequestPhase::Preflight, true)
		|| !RunPhase(EAssetDocumentRuntimeRequestPhase::Apply, false)
		|| !ValidateLive()
		|| !RunPhase(EAssetDocumentRuntimeRequestPhase::Repair, false)) return Fail();
	// Persist is the legal artifact-check phase; no host Persist callback or
	// persistence coordinator is invoked, and no installed artifact is written.
	if (!CheckCancellation() || !Observe(EAssetDocumentRuntimeRequestPhase::Persist)
		|| !RunPhase(EAssetDocumentRuntimeRequestPhase::Verify, true)) return Fail();
	const auto Fresh = Inputs.VerifyDurable();
	Diagnostics.Append(Fresh.Diagnostics);
	if (!Fresh.bFreshLoadProven || !Fresh.bCanonicalMatch
		|| Fresh.ActualSemanticHash != Inputs.SemanticHash || Fresh.ActualCodecVersion != Inputs.CodecVersion)
	{
		Diagnostics.Add(MakeRequestCoordinatorDiagnostic(
			TEXT("RequestCoordinator.EqualDeltaDurableEvidenceRejected"),
			TEXT("No-write completion requires matching independent installed-artifact evidence.")));
		return Fail();
	}
	if (!CheckCancellation() || !ValidateLive() || !Observe(EAssetDocumentRuntimeRequestPhase::Complete)) return Fail();
	LastResult.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
	LastResult.Diagnostics = MoveTemp(Diagnostics);
	return LastResult;
}
