// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentGenericValidateTrial.h"

#include "AssetDocumentEditorTrialScope.h"
#include "AssetDocumentRollbackAction.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

namespace
{
FAssetDocumentContractValidationResult TrialValid()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentContractValidationResult TrialInvalid(const TCHAR* Code, const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/ValidateTrial");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

// The transaction of a trial apply: the edit only touches a transient copy that is discarded,
// so there is nothing to snapshot, commit or persist.
class FValidateTrialTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	FValidateTrialTransaction(const FGuid& RequestId, uint64 RuntimeRevision)
	{
		Token.RequestId = RequestId;
		Token.TransactionId = FGuid::NewGuid();
		Token.RuntimeRevision = RuntimeRevision;
	}

	virtual FGuid GetTransactionId() const override { return Token.TransactionId; }
	virtual EAssetDocumentAtomicState GetState() const override { return State; }
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override
	{
		(void)Footprint;
		return IsOpen() ? TrialValid()
			: TrialInvalid(TEXT("ValidateTrial.Closed"), TEXT("The trial transaction has ended."));
	}
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		return TrialInvalid(TEXT("ValidateTrial.NoCommit"), TEXT("A trial apply is never committed."));
	}
	virtual FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt& Receipt) override
	{
		(void)Receipt;
		return TrialInvalid(TEXT("ValidateTrial.NoCommit"), TEXT("A trial apply is never committed."));
	}
	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) override
	{
		if (!IsOpen())
		{
			return TrialValid();
		}
		State = EAssetDocumentAtomicState::RolledBack;
		FAssetDocumentContractValidationResult Result = TrialValid();
		for (int32 Index = Actions.Num() - 1; Index >= 0; --Index)
		{
			const FAssetDocumentContractValidationResult Rolled = Actions[Index]->Rollback(StableFailureCode);
			Result.bValid &= Rolled.bValid;
			Result.Diagnostics.Append(Rolled.Diagnostics);
		}
		for (const TSharedRef<IAssetDocumentPostRollbackFinalizer>& Finalizer : Finalizers)
		{
			const FAssetDocumentContractValidationResult Finalized = Finalizer->FinalizePostRollback(StableFailureCode);
			Result.bValid &= Finalized.bValid;
			Result.Diagnostics.Append(Finalized.Diagnostics);
		}
		Actions.Reset();
		Finalizers.Reset();
		return Result;
	}
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		if (!IsOpen())
		{
			return TrialInvalid(TEXT("ValidateTrial.Closed"), TEXT("The trial transaction has ended."));
		}
		Actions.Add(MoveTemp(Action));
		return TrialValid();
	}
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer) override
	{
		if (!IsOpen())
		{
			return TrialInvalid(TEXT("ValidateTrial.Closed"), TEXT("The trial transaction has ended."));
		}
		Finalizers.Add(MoveTemp(Finalizer));
		return TrialValid();
	}

private:
	bool IsOpen() const { return State == EAssetDocumentAtomicState::Mutating; }

	FAssetDocumentMutationToken Token;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Mutating;
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
	TArray<TSharedRef<IAssetDocumentPostRollbackFinalizer>> Finalizers;
};

FAssetDocumentRuntimeResultDto MakeTrialFailure(
	const FAssetDocumentRuntimeRequestDto& Request,
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	FAssetDocumentRuntimeResultDto Result;
	Result.RequestId = Request.RequestId;
	Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
	Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
	Result.RuntimeRevision = Request.ExpectedRuntimeRevision;
	Result.Diagnostics = MoveTemp(Diagnostics);
	return Result;
}
}

FAssetDocumentRuntimeResultDto RunAssetDocumentValidateTrial(
	IAssetDocumentPythonRuntime& Runtime,
	const FAssetDocumentRuntimeRequestDto& Request)
{
	FAssetDocumentRuntimeRequestDto TrialRequest = Request;
	TrialRequest.Operation = EAssetDocumentRuntimeOperation::Validate;
	FValidateTrialTransaction Transaction(Request.RequestId, Request.ExpectedRuntimeRevision);
	FAssetDocumentRuntimeResultDto Result;
	TArray<FAssetDocumentRuntimeDiagnostic> ScopeDiagnostics;
	bool bScopeRestored = true;
	{
		FAssetDocumentEditorTrialScope Scope;
		{
			FAssetDocumentRequestContextMetadata Metadata;
			Metadata.RequestId = Request.RequestId;
			Metadata.PinnedRuntimeRevision = Request.ExpectedRuntimeRevision;
			Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
			FAssetDocumentPythonRequestContextCreateResult Context =
				Runtime.CreateRequestContext(Metadata, Transaction.GetTransactionId(), &Transaction);
			if (!Context.Context)
			{
				TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics = MoveTemp(Context.Diagnostics);
				if (Diagnostics.IsEmpty())
				{
					FAssetDocumentRuntimeDiagnostic& Diagnostic = Diagnostics.AddDefaulted_GetRef();
					Diagnostic.Path = TEXT("/GenericRuntime/Validate");
					Diagnostic.Code = TEXT("GenericRuntime.ContextCreationFailed");
					Diagnostic.Message = TEXT("The trial apply request context could not be created.");
				}
				return MakeTrialFailure(Request, MoveTemp(Diagnostics));
			}
			Result = Runtime.GetHost().Dispatch(TrialRequest, *Context.Context);
			// The request context retires the trial's scratch handles (and with them the copy)
			// when it goes out of scope here.
		}
		const FAssetDocumentContractValidationResult Rolled = Transaction.Rollback(TEXT("ValidateTrial.Discarded"));
		ScopeDiagnostics.Append(Rolled.Diagnostics);
		bScopeRestored = Scope.Finish(ScopeDiagnostics) && Rolled.bValid;
	}
	if (!bScopeRestored)
	{
		return MakeTrialFailure(Request, MoveTemp(ScopeDiagnostics));
	}
	return Result;
}
