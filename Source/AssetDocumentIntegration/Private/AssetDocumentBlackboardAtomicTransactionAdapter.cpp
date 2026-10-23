// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentBlackboardAtomicTransactionAdapter.h"

#include "AssetDocumentRollbackAction.h"

namespace
{
FAssetDocumentContractValidationResult WrapFailure(const TCHAR* Code, const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = false;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/AtomicTransaction");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

bool FootprintsMatch(TConstArrayView<FAssetDocumentMutationFootprint> Left, TConstArrayView<FAssetDocumentMutationFootprint> Right)
{
	if (Left.Num() != Right.Num()) return false;
	for (int32 Index = 0; Index < Left.Num(); ++Index)
	{
		if (Left[Index].Kind != Right[Index].Kind || Left[Index].StableId != Right[Index].StableId
			|| Left[Index].RollbackComponentId != Right[Index].RollbackComponentId) return false;
	}
	return true;
}

class FQualifiedRollbackAction final : public IAssetDocumentRollbackAction
{
public:
	FQualifiedRollbackAction(TSharedRef<IAssetDocumentRollbackAction> InInner, FString InQualifiedComponent)
		: Inner(MoveTemp(InInner)), QualifiedComponent(MoveTemp(InQualifiedComponent)) {}
	virtual FStringView GetRollbackComponentId() const override { return QualifiedComponent; }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) override { return Inner->Rollback(StableFailureCode); }
private:
	TSharedRef<IAssetDocumentRollbackAction> Inner;
	FString QualifiedComponent;
};

class FQualifiedPostRollbackFinalizer final : public IAssetDocumentPostRollbackFinalizer
{
public:
	FQualifiedPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer> InInner, FString InQualifiedComponent)
		: Inner(MoveTemp(InInner)), QualifiedComponent(MoveTemp(InQualifiedComponent)) {}
	virtual FStringView GetRollbackComponentId() const override { return QualifiedComponent; }
	virtual FAssetDocumentContractValidationResult FinalizePostRollback(FStringView StableFailureCode) override { return Inner->FinalizePostRollback(StableFailureCode); }
private:
	TSharedRef<IAssetDocumentPostRollbackFinalizer> Inner;
	FString QualifiedComponent;
};
} // namespace

FAssetDocumentBlackboardAtomicTransactionAdapter::FAssetDocumentBlackboardAtomicTransactionAdapter(
	IAssetDocumentAtomicTransaction& InUnderlying,
	FAssetDocumentBlackboardFootprintProjection InProjection,
	TArray<FAssetDocumentMutationFootprint> InOptionalCreatePrefix)
	: Underlying(&InUnderlying), Projection(MoveTemp(InProjection)), OptionalCreatePrefix(MoveTemp(InOptionalCreatePrefix))
{
	bPrefixConsumed = OptionalCreatePrefix.IsEmpty();
	// A factory Create may carry no Body steps (an empty Blackboard); then the Create prefix
	// is the whole footprint. Without a prefix, an empty projection has nothing to enlist.
	bRejected = Projection.Steps.IsEmpty() && OptionalCreatePrefix.IsEmpty();
}

FGuid FAssetDocumentBlackboardAtomicTransactionAdapter::GetTransactionId() const { return Underlying ? Underlying->GetTransactionId() : FGuid(); }
EAssetDocumentAtomicState FAssetDocumentBlackboardAtomicTransactionAdapter::GetState() const { return Underlying ? Underlying->GetState() : EAssetDocumentAtomicState::Poisoned; }
const FAssetDocumentMutationToken& FAssetDocumentBlackboardAtomicTransactionAdapter::GetMutationToken() const
{
	check(Underlying);
	return Underlying->GetMutationToken();
}

FAssetDocumentContractValidationResult FAssetDocumentBlackboardAtomicTransactionAdapter::Reject(const TCHAR* Code, const TCHAR* Message)
{
	bRejected = true;
	return WrapFailure(Code, Message);
}

const FAssetDocumentBlackboardFootprintProjectionStep* FAssetDocumentBlackboardAtomicTransactionAdapter::GetNextStep() const
{
	return Projection.Steps.IsValidIndex(NextStepIndex) ? &Projection.Steps[NextStepIndex] : nullptr;
}

const FAssetDocumentMutationFootprint* FAssetDocumentBlackboardAtomicTransactionAdapter::FindQualifiedComponent(FStringView RawComponentId) const
{
	const FAssetDocumentBlackboardFootprintProjectionStep* Step = GetNextStep();
	if (!Step) return nullptr;
	for (int32 Index = 0; Index < Step->RawFootprint.Num(); ++Index)
	{
		if (Step->RawFootprint[Index].RollbackComponentId == RawComponentId) return &Step->QualifiedFootprint[Index];
	}
	return nullptr;
}

bool FAssetDocumentBlackboardAtomicTransactionAdapter::IsExactPrefix(TConstArrayView<FAssetDocumentMutationFootprint> Footprint) const
{
	return !bPrefixConsumed && FootprintsMatch(Footprint, OptionalCreatePrefix);
}

FAssetDocumentContractValidationResult FAssetDocumentBlackboardAtomicTransactionAdapter::EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Footprint)
{
	if (bRejected || !Underlying) return Reject(TEXT("BlackboardAdapter.Poisoned"), TEXT("The Blackboard adapter is not usable."));
	if (IsExactPrefix(Footprint))
	{
		const FAssetDocumentContractValidationResult Result = Underlying->EnlistFootprint(Footprint);
		if (!Result.bValid) bRejected = true; else bPrefixConsumed = true;
		return Result;
	}
	if (!bPrefixConsumed) return Reject(TEXT("BlackboardAdapter.PrefixRequired"), TEXT("The optional Editor Create prefix must be consumed unchanged before Blackboard rows."));
	const FAssetDocumentBlackboardFootprintProjectionStep* Step = GetNextStep();
	if (!Step || !FootprintsMatch(Footprint, Step->RawFootprint)) return Reject(TEXT("BlackboardAdapter.RawFootprintMismatch"), TEXT("Only the exact next projected raw footprint batch may be enlisted."));
	const FAssetDocumentContractValidationResult Result = Underlying->EnlistFootprint(Step->QualifiedFootprint);
	if (!Result.bValid) { bRejected = true; return Result; }
	++NextStepIndex;
	return Result;
}

FAssetDocumentContractValidationResult FAssetDocumentBlackboardAtomicTransactionAdapter::MarkReadyToCommit()
{
	if (bRejected || !Underlying) return Reject(TEXT("BlackboardAdapter.Poisoned"), TEXT("The Blackboard adapter is not usable."));
	const FAssetDocumentContractValidationResult Complete = ValidateCompleteEnlistment();
	if (!Complete.bValid) return Reject(TEXT("BlackboardAdapter.IncompleteProjection"), TEXT("Every prefix and projected Blackboard step must be enlisted before commit."));
	const FAssetDocumentContractValidationResult Result = Underlying->MarkReadyToCommit();
	if (!Result.bValid) bRejected = true;
	return Result;
}

FAssetDocumentContractValidationResult FAssetDocumentBlackboardAtomicTransactionAdapter::ValidateCompleteEnlistment() const
{
	if (bRejected || !Underlying) return WrapFailure(TEXT("BlackboardAdapter.Poisoned"), TEXT("The Blackboard adapter is not usable."));
	if (!bPrefixConsumed || NextStepIndex != Projection.Steps.Num()) return WrapFailure(TEXT("BlackboardAdapter.IncompleteProjection"), TEXT("Every prefix and projected Blackboard step must be enlisted before commit."));
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentContractValidationResult FAssetDocumentBlackboardAtomicTransactionAdapter::Commit(const FAssetDocumentDurableCompletionReceipt& Receipt)
{
	if (bRejected) return Reject(TEXT("BlackboardAdapter.Poisoned"), TEXT("A rejected Blackboard adapter cannot commit."));
	return Underlying ? Underlying->Commit(Receipt) : Reject(TEXT("BlackboardAdapter.MissingUnderlying"), TEXT("The adapter requires an underlying transaction."));
}

FAssetDocumentContractValidationResult FAssetDocumentBlackboardAtomicTransactionAdapter::Rollback(FStringView StableFailureCode)
{
	return Underlying ? Underlying->Rollback(StableFailureCode) : Reject(TEXT("BlackboardAdapter.MissingUnderlying"), TEXT("The adapter requires an underlying transaction."));
}

FAssetDocumentContractValidationResult FAssetDocumentBlackboardAtomicTransactionAdapter::EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction> Action)
{
	if (bRejected || !Underlying) return Reject(TEXT("BlackboardAdapter.Poisoned"), TEXT("The Blackboard adapter is not usable."));
	const FString RawComponent(Action->GetRollbackComponentId());
	if (OptionalCreatePrefix.ContainsByPredicate([&RawComponent](const FAssetDocumentMutationFootprint& Row) { return Row.RollbackComponentId == RawComponent; }))
	{
		const FAssetDocumentContractValidationResult Result = Underlying->EnlistRollbackAction(MoveTemp(Action));
		if (!Result.bValid) bRejected = true;
		return Result;
	}
	const FAssetDocumentMutationFootprint* Qualified = FindQualifiedComponent(RawComponent);
	if (!Qualified || WrappedQualifiedComponents.Contains(Qualified->RollbackComponentId)) return Reject(TEXT("BlackboardAdapter.RollbackComponentMismatch"), TEXT("Rollback action must cover one unconsumed component of the next Blackboard step."));
	const FAssetDocumentContractValidationResult Result = Underlying->EnlistRollbackAction(MakeShared<FQualifiedRollbackAction>(MoveTemp(Action), Qualified->RollbackComponentId));
	if (!Result.bValid) { bRejected = true; return Result; }
	WrappedQualifiedComponents.Add(Qualified->RollbackComponentId);
	return Result;
}

FAssetDocumentContractValidationResult FAssetDocumentBlackboardAtomicTransactionAdapter::EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer)
{
	if (bRejected || !Underlying) return Reject(TEXT("BlackboardAdapter.Poisoned"), TEXT("The Blackboard adapter is not usable."));
	const FString RawComponent(Finalizer->GetRollbackComponentId());
	if (OptionalCreatePrefix.ContainsByPredicate([&RawComponent](const FAssetDocumentMutationFootprint& Row) { return Row.RollbackComponentId == RawComponent; }))
	{
		const FAssetDocumentContractValidationResult Result = Underlying->EnlistPostRollbackFinalizer(MoveTemp(Finalizer));
		if (!Result.bValid) bRejected = true;
		return Result;
	}
	const FAssetDocumentMutationFootprint* Qualified = FindQualifiedComponent(RawComponent);
	if (!Qualified || WrappedQualifiedComponents.Contains(Qualified->RollbackComponentId)) return Reject(TEXT("BlackboardAdapter.FinalizerComponentMismatch"), TEXT("Finalizer must cover one unconsumed component of the next Blackboard step."));
	const FAssetDocumentContractValidationResult Result = Underlying->EnlistPostRollbackFinalizer(MakeShared<FQualifiedPostRollbackFinalizer>(MoveTemp(Finalizer), Qualified->RollbackComponentId));
	if (!Result.bValid) { bRejected = true; return Result; }
	WrappedQualifiedComponents.Add(Qualified->RollbackComponentId);
	return Result;
}
