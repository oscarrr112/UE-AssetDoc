// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentRollbackAction.h"
#include "Tests/AssetDocumentAtomicFrameworkTestDoubles.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentAtomicTransactionRuntimeTests
{
using namespace AssetDocumentAtomicFrameworkTestDoubles;

class FRollbackAction final : public IAssetDocumentRollbackAction
{
public:
	explicit FRollbackAction(
		FString InComponentId,
		FCallRecord* InRecord = nullptr,
		bool bInFailRollback = false)
		: ComponentId(MoveTemp(InComponentId))
		, Record(InRecord)
		, bFailRollback(bInFailRollback)
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return ComponentId;
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		if (BeforeRollback)
		{
			BeforeRollback();
		}
		++RollbackCount;
		if (Record != nullptr)
		{
			Record->Calls.Add(FString::Printf(
				TEXT("RollbackAction.%s"), *ComponentId));
		}
		FAssetDocumentContractValidationResult Result;
		Result.bValid = !bFailRollback;
		Result.Diagnostics = Diagnostics;
		return Result;
	}

	int32 RollbackCount = 0;
	TFunction<void()> BeforeRollback;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;

private:
	FString ComponentId;
	FCallRecord* Record = nullptr;
	bool bFailRollback = false;
};

class FPostRollbackFinalizer final : public IAssetDocumentPostRollbackFinalizer
{
public:
	explicit FPostRollbackFinalizer(
		FString InComponentId,
		FCallRecord* InRecord = nullptr,
		bool bInFailFinalize = false,
		int32* InFinalizeCountObservation = nullptr)
		: ComponentId(MoveTemp(InComponentId))
		, Record(InRecord)
		, bFailFinalize(bInFailFinalize)
		, FinalizeCountObservation(InFinalizeCountObservation)
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return ComponentId;
	}

	virtual FAssetDocumentContractValidationResult FinalizePostRollback(
		FStringView) override
	{
		if (BeforeFinalize)
		{
			BeforeFinalize();
		}
		++FinalizeCount;
		if (FinalizeCountObservation != nullptr)
		{
			++*FinalizeCountObservation;
		}
		if (Record != nullptr)
		{
			Record->Calls.Add(FString::Printf(
				TEXT("PostRollbackFinalizer.%s"), *ComponentId));
		}
		FAssetDocumentContractValidationResult Result;
		Result.bValid = !bFailFinalize;
		Result.Diagnostics = Diagnostics;
		return Result;
	}

	int32 FinalizeCount = 0;
	TFunction<void()> BeforeFinalize;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;

private:
	FString ComponentId;
	FCallRecord* Record = nullptr;
	bool bFailFinalize = false;
	int32* FinalizeCountObservation = nullptr;
};

bool HasExactAtomicDiagnostic(
	const FAssetDocumentContractValidationResult& Result,
	FStringView Code)
{
	return !Result.bValid
		&& Result.Diagnostics.Num() == 1
		&& Result.Diagnostics[0].Path == TEXT("/AtomicTransaction")
		&& Result.Diagnostics[0].Code == Code
		&& !Result.Diagnostics[0].Message.IsEmpty();
}

bool IsExactAtomicDiagnostic(
	const FAssetDocumentRuntimeDiagnostic& Diagnostic,
	FStringView Code)
{
	return Diagnostic.Path == TEXT("/AtomicTransaction")
		&& Diagnostic.Code == Code
		&& !Diagnostic.Message.IsEmpty();
}

FAssetDocumentMutationFootprint MakeFootprint(const TCHAR* StableId, const TCHAR* ComponentId)
{
	FAssetDocumentMutationFootprint Footprint;
	Footprint.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
	Footprint.StableId = StableId;
	Footprint.RollbackComponentId = ComponentId;
	return Footprint;
}

FAssetDocumentPreflightReceipt MakeReceipt()
{
	FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = FGuid::NewGuid();
	Receipt.RuntimeRevision = 7;
	Receipt.ReceiptHash = TEXT("sha256:preflight");
	Receipt.bSucceeded = true;
	Receipt.CompleteFootprint.Add(MakeFootprint(TEXT("asset:/Game/A"), TEXT("rollback.a")));
	Receipt.CompleteFootprint.Add(MakeFootprint(TEXT("file:/Game/A.sidecar"), TEXT("rollback.b")));
	return Receipt;
}

FAssetDocumentDurableCompletionReceipt MakeDurableReceipt(
	const FAssetDocumentPreflightReceipt& Preflight,
	const IAssetDocumentAtomicTransaction& Transaction)
{
	FAssetDocumentDurableCompletionReceipt Receipt;
	Receipt.RequestId = Preflight.RequestId;
	Receipt.TransactionId = Transaction.GetTransactionId();
	Receipt.JournalId = TEXT("journal:test");
	Receipt.PackageContentDigest = TEXT("sha256:package");
	Receipt.SidecarContentDigest = TEXT("sha256:sidecar");
	Receipt.bSyncStateAdvanced = true;
	return Receipt;
}

void TestFactoryAdmission(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FRollbackParticipant A(TEXT("rollback.a"), &Calls);
	FRollbackParticipant B(TEXT("rollback.b"), &Calls);
	FParticipantRegistry Registry;
	Registry.Add(A);
	Registry.Add(B);
	FCancellationView Cancellation;
	FAssetDocumentAtomicTransactionFactoryRuntime Factory(Handles, Registry);
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Factory.BeginLiveAfterPreflight(Receipt, Cancellation);
	Test.TestNotNull(TEXT("valid preflight obtains live transaction"), Transaction.Get());
	Test.TestEqual(TEXT("live transaction starts live"), Transaction->GetState(), EAssetDocumentAtomicState::Live);
	Test.TestEqual(TEXT("mutation token keeps request"), Transaction->GetMutationToken().RequestId, Receipt.RequestId);
	Test.TestTrue(TEXT("transaction identity is generated"), Transaction->GetTransactionId().IsValid());

	Cancellation.bCancelled = true;
	Test.TestNull(TEXT("cancelled preflight cannot begin"), Factory.BeginLiveAfterPreflight(Receipt, Cancellation).Get());
	FParticipantRegistry MissingRegistry;
	FAssetDocumentAtomicTransactionFactoryRuntime MissingFactory(Handles, MissingRegistry);
	Cancellation.bCancelled = false;
	TUniquePtr<IAssetDocumentAtomicTransaction> MissingCoverage =
		MissingFactory.BeginLiveAfterPreflight(Receipt, Cancellation);
	Test.TestNotNull(
		TEXT("missing rollback coverage is admitted until footprint enlistment"),
		MissingCoverage.Get());
	if (!MissingCoverage)
	{
		return;
	}
	const FAssetDocumentContractValidationResult MissingResult =
		MissingCoverage->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[0], 1));
	Test.TestTrue(
		TEXT("footprint without current rollback coverage fails closed"),
		HasExactAtomicDiagnostic(
			MissingResult,
			TEXTVIEW("Atomic.IncompleteRollbackCoverage")));
	Test.TestEqual(
		TEXT("missing coverage leaves transaction live"),
		MissingCoverage->GetState(),
		EAssetDocumentAtomicState::Live);
	TSharedPtr<FRollbackAction> Action =
		MakeShared<FRollbackAction>(TEXT("rollback.a"));
	Test.TestTrue(
		TEXT("action can be enlisted after atomic coverage rejection"),
		MissingCoverage->EnlistRollbackAction(Action.ToSharedRef()).bValid);
	Test.TestTrue(
		TEXT("footprint can be retried after coverage is supplied"),
		MissingCoverage->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);
	Test.TestTrue(
		TEXT("retried covered footprint rolls back"),
		MissingCoverage->Rollback(TEXT("test.coverage-retry")).bValid);
	Test.TestEqual(
		TEXT("failed coverage attempt appended no rollback target"),
		Action->RollbackCount,
		1);
}

void TestDynamicActionValidationAndAtomicity(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FParticipantRegistry EmptyRegistry;
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt,
			Handles,
			EmptyRegistry,
			FAssetDocumentAtomicDiagnosticSink{});

	TSharedPtr<FRollbackAction> Action =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	const FAssetDocumentContractValidationResult ActionResult =
		Transaction->EnlistRollbackAction(Action.ToSharedRef());
	Test.TestTrue(
		TEXT("declared dynamic action enlists"),
		ActionResult.bValid);
	if (!ActionResult.bValid)
	{
		return;
	}
	const FAssetDocumentContractValidationResult FootprintResult =
		Transaction->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[0], 1));
	Test.TestTrue(
		TEXT("matching footprint enlists after accepted action"),
		FootprintResult.bValid);
	if (!FootprintResult.bValid)
	{
		return;
	}
	Test.TestEqual(
		TEXT("matching footprint enters mutating"),
		Transaction->GetState(),
		EAssetDocumentAtomicState::Mutating);
	Test.TestTrue(
		TEXT("rollback executes the one accepted target"),
		Transaction->Rollback(TEXT("test.dynamic")).bValid);
	Test.TestEqual(
		TEXT("failed footprint did not append a duplicate target"),
		Action->RollbackCount,
		1);
}

void TestRejectedActionsAreNotRetained(FAutomationTestBase& Test)
{
	FHandleResolver Handles;
	FParticipantRegistry Registry;
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt,
			Handles,
			Registry,
			FAssetDocumentAtomicDiagnosticSink{});

	TSharedPtr<FRollbackAction> EmptyAction =
		MakeShared<FRollbackAction>(TEXT(""));
	TWeakPtr<FRollbackAction> WeakEmpty = EmptyAction;
	Test.TestTrue(
		TEXT("empty component ID returns the frozen diagnostic"),
		HasExactAtomicDiagnostic(
			Transaction->EnlistRollbackAction(EmptyAction.ToSharedRef()),
			TEXTVIEW("Atomic.RollbackActionEmptyComponentId")));
	EmptyAction.Reset();
	Test.TestFalse(
		TEXT("empty component action is not retained"),
		WeakEmpty.IsValid());

	TSharedPtr<FRollbackAction> UndeclaredAction =
		MakeShared<FRollbackAction>(TEXT("rollback.undeclared"));
	TWeakPtr<FRollbackAction> WeakUndeclared = UndeclaredAction;
	Test.TestTrue(
		TEXT("undeclared component returns the frozen diagnostic"),
		HasExactAtomicDiagnostic(
			Transaction->EnlistRollbackAction(UndeclaredAction.ToSharedRef()),
			TEXTVIEW("Atomic.RollbackActionComponentNotDeclared")));
	UndeclaredAction.Reset();
	Test.TestFalse(
		TEXT("undeclared component action is not retained"),
		WeakUndeclared.IsValid());
	Test.TestEqual(
		TEXT("rejected actions preserve live state"),
		Transaction->GetState(),
		EAssetDocumentAtomicState::Live);
}

void TestPostRollbackFinalizerAdmission(FAutomationTestBase& Test)
{
	FHandleResolver Handles;
	FParticipantRegistry Registry;
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt,
			Handles,
			Registry,
			FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FPostRollbackFinalizer> Finalizer =
		MakeShared<FPostRollbackFinalizer>(TEXT("rollback.a"));
	TWeakPtr<FPostRollbackFinalizer> WeakFinalizer = Finalizer;

	Test.TestTrue(
		TEXT("declared finalizer is accepted before its first footprint"),
		Transaction->EnlistPostRollbackFinalizer(Finalizer.ToSharedRef()).bValid);
	Finalizer.Reset();
	Test.TestEqual(
		TEXT("finalizer admission leaves the transaction live"),
		Transaction->GetState(),
		EAssetDocumentAtomicState::Live);
	Test.TestTrue(
		TEXT("accepted finalizer remains strongly owned after the caller releases it"),
		WeakFinalizer.IsValid());
	const TSharedPtr<FPostRollbackFinalizer> RetainedFinalizer = WeakFinalizer.Pin();
	if (Test.TestNotNull(
		TEXT("strong finalizer ownership permits callback-count observation after caller release"),
		RetainedFinalizer.Get()))
	{
		Test.TestEqual(
			TEXT("admission does not invoke the post-rollback callback"),
			RetainedFinalizer->FinalizeCount,
			0);
	}

	auto TestRejected = [&Test, &Receipt](
		const TCHAR* What,
		FString ComponentId,
		const TCHAR* ExpectedCode,
		bool bEnlistFootprintFirst,
		bool bAddAction,
		bool bAddParticipant)
	{
		FHandleResolver LocalHandles;
		FParticipantRegistry LocalRegistry;
		TArray<TPair<EAssetDocumentRuntimeRequestPhase, FAssetDocumentRuntimeDiagnostic>> LocalDiagnostics;
		TUniquePtr<FRollbackParticipant> Participant;
		if (bAddParticipant)
		{
			Participant = MakeUnique<FRollbackParticipant>(TEXT("rollback.a"));
			LocalRegistry.Add(*Participant);
		}
		TUniquePtr<IAssetDocumentAtomicTransaction> LocalTransaction =
			MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
				Receipt, LocalHandles, LocalRegistry,
				[&LocalDiagnostics](EAssetDocumentRuntimeRequestPhase Phase,
					const FAssetDocumentRuntimeDiagnostic& Diagnostic)
				{ LocalDiagnostics.Emplace(Phase, Diagnostic); });
		if (bAddAction || bEnlistFootprintFirst)
		{
			TSharedPtr<FRollbackAction> Action =
				MakeShared<FRollbackAction>(TEXT("rollback.a"));
			LocalTransaction->EnlistRollbackAction(Action.ToSharedRef());
		}
		if (bEnlistFootprintFirst)
		{
			LocalTransaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[0], 1));
		}
		TSharedPtr<FPostRollbackFinalizer> Candidate =
			MakeShared<FPostRollbackFinalizer>(MoveTemp(ComponentId));
		TWeakPtr<FPostRollbackFinalizer> WeakCandidate = Candidate;
		const FAssetDocumentContractValidationResult Result =
			LocalTransaction->EnlistPostRollbackFinalizer(Candidate.ToSharedRef());
		Test.TestTrue(What, HasExactAtomicDiagnostic(Result, ExpectedCode));
		Test.TestTrue(TEXT("rejected finalizer uses one exact Apply diagnostic"),
			LocalDiagnostics.Num() == 1
			&& LocalDiagnostics[0].Key == EAssetDocumentRuntimeRequestPhase::Apply
			&& IsExactAtomicDiagnostic(LocalDiagnostics[0].Value, ExpectedCode));
		Test.TestEqual(TEXT("rejected finalizer preserves transaction state"),
			LocalTransaction->GetState(),
			bEnlistFootprintFirst ? EAssetDocumentAtomicState::Mutating : EAssetDocumentAtomicState::Live);
		Candidate.Reset();
		Test.TestFalse(TEXT("rejected finalizer has zero retained ownership"), WeakCandidate.IsValid());
	};
	TestRejected(TEXT("empty finalizer ID is rejected"), TEXT(""),
		TEXT("Atomic.PostRollbackFinalizerEmptyComponentId"), false, false, false);
	TestRejected(TEXT("undeclared finalizer component is rejected"), TEXT("rollback.unknown"),
		TEXT("Atomic.PostRollbackFinalizerComponentNotDeclared"), false, false, false);
	TestRejected(TEXT("late finalizer is rejected"), TEXT("rollback.a"),
		TEXT("Atomic.PostRollbackFinalizerLateEnlistment"), true, false, false);
	TestRejected(TEXT("public action collision is rejected"), TEXT("rollback.a"),
		TEXT("Atomic.PostRollbackFinalizerDuplicateComponentId"), false, true, false);
	TestRejected(TEXT("private participant collision is rejected"), TEXT("rollback.a"),
		TEXT("Atomic.PostRollbackFinalizerDuplicateComponentId"), false, false, true);

	FHandleResolver MutatingHandles;
	FParticipantRegistry MutatingRegistry;
	TUniquePtr<IAssetDocumentAtomicTransaction> MutatingTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt, MutatingHandles, MutatingRegistry, FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FRollbackAction> MutatingAction = MakeShared<FRollbackAction>(TEXT("rollback.b"));
	TSharedPtr<FPostRollbackFinalizer> MutatingFinalizer = MakeShared<FPostRollbackFinalizer>(TEXT("rollback.a"));
	MutatingTransaction->EnlistRollbackAction(MutatingAction.ToSharedRef());
	MutatingTransaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[1], 1));
	Test.TestTrue(TEXT("mutating transaction accepts a declared finalizer before its footprint"),
		MutatingTransaction->EnlistPostRollbackFinalizer(MutatingFinalizer.ToSharedRef()).bValid);

	FHandleResolver DuplicateHandles;
	FParticipantRegistry DuplicateRegistry;
	TUniquePtr<IAssetDocumentAtomicTransaction> DuplicateTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt, DuplicateHandles, DuplicateRegistry, FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FPostRollbackFinalizer> FirstFinalizer =
		MakeShared<FPostRollbackFinalizer>(TEXT("rollback.a"));
	TSharedPtr<FPostRollbackFinalizer> SecondFinalizer =
		MakeShared<FPostRollbackFinalizer>(TEXT("rollback.b"));
	Test.TestTrue(TEXT("first finalizer claims the transaction-wide finalizer slot"),
		DuplicateTransaction->EnlistPostRollbackFinalizer(FirstFinalizer.ToSharedRef()).bValid);
	Test.TestTrue(TEXT("second finalizer is rejected even for a different component"),
		HasExactAtomicDiagnostic(
			DuplicateTransaction->EnlistPostRollbackFinalizer(SecondFinalizer.ToSharedRef()),
			TEXTVIEW("Atomic.PostRollbackFinalizerDuplicateComponentId")));
	DuplicateTransaction->Rollback(TEXT("test.duplicate-finalizer"));
	Test.TestTrue(TEXT("terminal transaction rejects finalizer before component validation"),
		HasExactAtomicDiagnostic(
			DuplicateTransaction->EnlistPostRollbackFinalizer(SecondFinalizer.ToSharedRef()),
			TEXTVIEW("Atomic.PostRollbackFinalizerInvalidState")));

	auto TestInvalidStatePriority = [&Test, &Receipt](EAssetDocumentAtomicState ExpectedState)
	{
		FHandleResolver StateHandles;
		FParticipantRegistry StateRegistry;
		TArray<TPair<EAssetDocumentRuntimeRequestPhase, FAssetDocumentRuntimeDiagnostic>> StateDiagnostics;
		TUniquePtr<IAssetDocumentAtomicTransaction> StateTransaction =
			MakeUnique<FAssetDocumentAtomicTransactionRuntime>(Receipt, StateHandles, StateRegistry,
				[&StateDiagnostics](EAssetDocumentRuntimeRequestPhase Phase,
					const FAssetDocumentRuntimeDiagnostic& Diagnostic)
				{ StateDiagnostics.Emplace(Phase, Diagnostic); });
		TSharedPtr<FRollbackAction> ActionA =
			MakeShared<FRollbackAction>(TEXT("rollback.a"), nullptr,
				ExpectedState == EAssetDocumentAtomicState::Poisoned);
		TSharedPtr<FRollbackAction> ActionB =
			MakeShared<FRollbackAction>(TEXT("rollback.b"));
		if (ExpectedState == EAssetDocumentAtomicState::ReadyToCommit
			|| ExpectedState == EAssetDocumentAtomicState::Committed)
		{
			StateTransaction->EnlistRollbackAction(ActionA.ToSharedRef());
			StateTransaction->EnlistRollbackAction(ActionB.ToSharedRef());
			StateTransaction->EnlistFootprint(Receipt.CompleteFootprint);
			StateTransaction->MarkReadyToCommit();
			if (ExpectedState == EAssetDocumentAtomicState::Committed)
			{
				StateTransaction->Commit(MakeDurableReceipt(Receipt, *StateTransaction));
			}
		}
		else if (ExpectedState == EAssetDocumentAtomicState::Poisoned)
		{
			StateTransaction->EnlistRollbackAction(ActionA.ToSharedRef());
			StateTransaction->EnlistFootprint(
				MakeArrayView(&Receipt.CompleteFootprint[0], 1));
			StateTransaction->Rollback(TEXT("test.poison-before-finalizer-enlist"));
		}
		else
		{
			StateTransaction->Rollback(TEXT("test.rollback-before-finalizer-enlist"));
		}
		Test.TestEqual(TEXT("test setup reaches the requested non-mutable state"),
			StateTransaction->GetState(), ExpectedState);
		StateDiagnostics.Reset();
		TSharedPtr<FPostRollbackFinalizer> EmptyFinalizer =
			MakeShared<FPostRollbackFinalizer>(TEXT(""));
		TWeakPtr<FPostRollbackFinalizer> WeakEmptyFinalizer = EmptyFinalizer;
		const FAssetDocumentContractValidationResult Rejected =
			StateTransaction->EnlistPostRollbackFinalizer(EmptyFinalizer.ToSharedRef());
		Test.TestTrue(TEXT("invalid state takes priority over empty component identity"),
			HasExactAtomicDiagnostic(
				Rejected,
				TEXTVIEW("Atomic.PostRollbackFinalizerInvalidState")));
		Test.TestTrue(TEXT("invalid-state rejection emits one exact Apply diagnostic"),
			StateDiagnostics.Num() == 1
			&& StateDiagnostics[0].Key == EAssetDocumentRuntimeRequestPhase::Apply
			&& IsExactAtomicDiagnostic(
				StateDiagnostics[0].Value,
				TEXTVIEW("Atomic.PostRollbackFinalizerInvalidState")));
		Test.TestEqual(TEXT("invalid-state rejection preserves the terminal state"),
			StateTransaction->GetState(), ExpectedState);
		EmptyFinalizer.Reset();
		Test.TestFalse(TEXT("invalid-state rejection retains no finalizer"),
			WeakEmptyFinalizer.IsValid());
	};
	TestInvalidStatePriority(EAssetDocumentAtomicState::ReadyToCommit);
	TestInvalidStatePriority(EAssetDocumentAtomicState::Committed);
	TestInvalidStatePriority(EAssetDocumentAtomicState::RolledBack);
	TestInvalidStatePriority(EAssetDocumentAtomicState::Poisoned);
}

void TestPostRollbackFinalizerLast(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FParticipantRegistry Registry;
	FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	Receipt.CompleteFootprint.Add(MakeFootprint(TEXT("asset:/Game/finalizer"), TEXT("rollback.finalizer")));
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt, Handles, Registry, FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FRollbackAction> ActionA =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	TSharedPtr<FRollbackAction> ActionB =
		MakeShared<FRollbackAction>(TEXT("rollback.b"), &Calls);
	TSharedPtr<FPostRollbackFinalizer> Finalizer =
		MakeShared<FPostRollbackFinalizer>(TEXT("rollback.finalizer"), &Calls);
	Test.TestTrue(TEXT("finalizer registers before any footprint"),
		Transaction->EnlistPostRollbackFinalizer(Finalizer.ToSharedRef()).bValid);
	Test.TestTrue(TEXT("ordinary actions remain independently registrable after finalizer"),
		Transaction->EnlistRollbackAction(ActionA.ToSharedRef()).bValid
		&& Transaction->EnlistRollbackAction(ActionB.ToSharedRef()).bValid);
	Test.TestTrue(TEXT("first ordinary footprint enlists"),
		Transaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);
	Test.TestTrue(TEXT("interleaved finalizer footprint activates without ordinary target"),
		Transaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[2], 1)).bValid);
	Test.TestTrue(TEXT("second ordinary footprint enlists after finalizer activation"),
		Transaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[1], 1)).bValid);
	Test.TestTrue(TEXT("active finalizer rollback succeeds"),
		Transaction->Rollback(TEXT("test.finalizer-last")).bValid);
	Test.TestTrue(TEXT("ordinary rollback completes before exactly one finalizer callback"),
		Calls.Calls == TArray<FString>{
			TEXT("Invalidate.Rollback"), TEXT("RollbackAction.rollback.b"),
			TEXT("RollbackAction.rollback.a"), TEXT("PostRollbackFinalizer.rollback.finalizer")});
	Test.TestEqual(TEXT("finalizer runs once after both ordinary targets"), Finalizer->FinalizeCount, 1);

	FCallRecord InterleavedCalls;
	FHandleResolver InterleavedHandles(&InterleavedCalls);
	FParticipantRegistry InterleavedRegistry;
	TUniquePtr<IAssetDocumentAtomicTransaction> InterleavedTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt, InterleavedHandles, InterleavedRegistry, FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FRollbackAction> InterleavedActionA =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &InterleavedCalls);
	TSharedPtr<FRollbackAction> InterleavedActionB =
		MakeShared<FRollbackAction>(TEXT("rollback.b"), &InterleavedCalls);
	TSharedPtr<FPostRollbackFinalizer> InterleavedFinalizer =
		MakeShared<FPostRollbackFinalizer>(TEXT("rollback.finalizer"), &InterleavedCalls);
	Test.TestTrue(TEXT("ordinary and finalizer registrations may be interleaved"),
		InterleavedTransaction->EnlistRollbackAction(InterleavedActionB.ToSharedRef()).bValid
		&& InterleavedTransaction->EnlistPostRollbackFinalizer(InterleavedFinalizer.ToSharedRef()).bValid
		&& InterleavedTransaction->EnlistRollbackAction(InterleavedActionA.ToSharedRef()).bValid);
	Test.TestTrue(TEXT("a different footprint interleaving remains covered"),
		InterleavedTransaction->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[1], 1)).bValid
		&& InterleavedTransaction->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[2], 1)).bValid
		&& InterleavedTransaction->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);
	Test.TestTrue(TEXT("finalizer remains absolutely last for the second interleaving"),
		InterleavedTransaction->Rollback(TEXT("test.finalizer-last-interleaved")).bValid
		&& InterleavedCalls.Calls == TArray<FString>{
			TEXT("Invalidate.Rollback"), TEXT("RollbackAction.rollback.a"),
			TEXT("RollbackAction.rollback.b"),
			TEXT("PostRollbackFinalizer.rollback.finalizer")});
}

void TestPostRollbackFinalizerActivation(FAutomationTestBase& Test)
{
	FHandleResolver Handles;
	FParticipantRegistry Registry;
	FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	Receipt.CompleteFootprint.Add(MakeFootprint(TEXT("asset:/Game/A.second"), TEXT("rollback.a")));
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt, Handles, Registry, FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FRollbackAction> Action = MakeShared<FRollbackAction>(TEXT("rollback.b"));
	TSharedPtr<FPostRollbackFinalizer> Finalizer = MakeShared<FPostRollbackFinalizer>(TEXT("rollback.a"));
	Transaction->EnlistRollbackAction(Action.ToSharedRef());
	Transaction->EnlistPostRollbackFinalizer(Finalizer.ToSharedRef());
	Test.TestTrue(TEXT("first matching footprint activates finalizer"),
		Transaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);
	Test.TestTrue(TEXT("second stable identity is covered without another ordinary target"),
		Transaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[2], 1)).bValid);
	Test.TestTrue(TEXT("remaining ordinary footprint completes coverage"),
		Transaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[1], 1)).bValid);
	Transaction->Rollback(TEXT("test.activation"));
	Test.TestEqual(TEXT("all matching footprint identities invoke one finalizer callback"), Finalizer->FinalizeCount, 1);

	FHandleResolver InactiveHandles;
	FParticipantRegistry InactiveRegistry;
	TUniquePtr<IAssetDocumentAtomicTransaction> Inactive =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt, InactiveHandles, InactiveRegistry, FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FPostRollbackFinalizer> InactiveFinalizer =
		MakeShared<FPostRollbackFinalizer>(TEXT("rollback.a"));
	Inactive->EnlistPostRollbackFinalizer(InactiveFinalizer.ToSharedRef());
	Test.TestTrue(TEXT("registered-only finalizer permits rollback"),
		Inactive->Rollback(TEXT("test.inactive")).bValid);
	Test.TestEqual(TEXT("registered-only finalizer is never invoked"), InactiveFinalizer->FinalizeCount, 0);

	FHandleResolver AtomicBatchHandles;
	FParticipantRegistry AtomicBatchRegistry;
	TUniquePtr<IAssetDocumentAtomicTransaction> AtomicBatchTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt, AtomicBatchHandles, AtomicBatchRegistry, FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FPostRollbackFinalizer> AtomicBatchFinalizer =
		MakeShared<FPostRollbackFinalizer>(TEXT("rollback.a"));
	AtomicBatchTransaction->EnlistPostRollbackFinalizer(AtomicBatchFinalizer.ToSharedRef());
	Test.TestTrue(TEXT("a later uncovered item rejects the whole footprint batch"),
		HasExactAtomicDiagnostic(
			AtomicBatchTransaction->EnlistFootprint(
				MakeArrayView(Receipt.CompleteFootprint.GetData(), 2)),
			TEXTVIEW("Atomic.IncompleteRollbackCoverage")));
	Test.TestEqual(TEXT("failed footprint batch leaves the transaction live"),
		AtomicBatchTransaction->GetState(), EAssetDocumentAtomicState::Live);
	Test.TestTrue(TEXT("failed footprint batch permits rollback without an active finalizer"),
		AtomicBatchTransaction->Rollback(TEXT("test.activation-atomicity")).bValid);
	Test.TestEqual(TEXT("failed footprint batch leaves no finalizer activation residue"),
		AtomicBatchFinalizer->FinalizeCount, 0);
}

void TestPostRollbackFinalizerFailures(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FParticipantRegistry Registry;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(Receipt, Handles, Registry,
			[&Diagnostics](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{ Diagnostics.Add(Diagnostic); });
	TSharedPtr<FRollbackAction> OrdinaryAction =
		MakeShared<FRollbackAction>(TEXT("rollback.b"), &Calls);
	TSharedPtr<FPostRollbackFinalizer> FailingFinalizer =
		MakeShared<FPostRollbackFinalizer>(TEXT("rollback.a"), &Calls, true);
	Transaction->EnlistRollbackAction(OrdinaryAction.ToSharedRef());
	Transaction->EnlistPostRollbackFinalizer(FailingFinalizer.ToSharedRef());
	Transaction->EnlistFootprint(Receipt.CompleteFootprint);
	Test.TestFalse(TEXT("finalizer failure poisons rollback"),
		Transaction->Rollback(TEXT("test.finalizer-failure")).bValid);
	Test.TestTrue(TEXT("ordinary success completes before failing finalizer"),
		Calls.Calls == TArray<FString>{TEXT("Invalidate.Rollback"),
			TEXT("RollbackAction.rollback.b"), TEXT("PostRollbackFinalizer.rollback.a")});
	Test.TestTrue(TEXT("finalizer and aggregate poison diagnostics are emitted without ordinary failure"),
		Diagnostics.ContainsByPredicate([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{ return Diagnostic.Code == TEXT("Atomic.Poisoned.PostRollbackFinalizerFailed"); })
		&& Diagnostics.ContainsByPredicate([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{ return Diagnostic.Code == TEXT("Atomic.Poisoned.RollbackFailed"); })
		&& !Diagnostics.ContainsByPredicate([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{ return Diagnostic.Code == TEXT("Atomic.Poisoned.ComponentFailed"); }));
	Test.TestEqual(TEXT("poisoned finalizer is not retried"), FailingFinalizer->FinalizeCount, 1);
	Test.TestFalse(TEXT("poisoned terminal rejects rollback retry"),
		Transaction->Rollback(TEXT("retry")).bValid);
}

void TestPostRollbackFinalizerOrdinaryFailure(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FParticipantRegistry Registry;
	FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	Receipt.CompleteFootprint.Add(MakeFootprint(TEXT("asset:/Game/C"), TEXT("rollback.c")));
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(Receipt, Handles, Registry,
			[&Diagnostics](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{ Diagnostics.Add(Diagnostic); });
	TSharedPtr<FRollbackAction> FailingAction =
		MakeShared<FRollbackAction>(TEXT("rollback.b"), &Calls, true);
	TSharedPtr<FRollbackAction> FollowingAction =
		MakeShared<FRollbackAction>(TEXT("rollback.c"), &Calls);
	TSharedPtr<FPostRollbackFinalizer> Finalizer =
		MakeShared<FPostRollbackFinalizer>(TEXT("rollback.a"), &Calls);
	Transaction->EnlistRollbackAction(FailingAction.ToSharedRef());
	Transaction->EnlistRollbackAction(FollowingAction.ToSharedRef());
	Transaction->EnlistPostRollbackFinalizer(Finalizer.ToSharedRef());
	Transaction->EnlistFootprint(Receipt.CompleteFootprint);
	Test.TestFalse(TEXT("ordinary rollback failure poisons after all ordinary and finalizer callbacks"),
		Transaction->Rollback(TEXT("test.ordinary-failure")).bValid);
	Test.TestTrue(TEXT("failing ordinary action never skips later ordinary action or finalizer"),
		Calls.Calls == TArray<FString>{TEXT("Invalidate.Rollback"),
			TEXT("RollbackAction.rollback.c"), TEXT("RollbackAction.rollback.b"),
			TEXT("PostRollbackFinalizer.rollback.a")});
	Test.TestTrue(TEXT("ordinary failure emits only component and aggregate poison terminals"),
		Diagnostics.ContainsByPredicate([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{ return Diagnostic.Code == TEXT("Atomic.Poisoned.ComponentFailed"); })
		&& Diagnostics.ContainsByPredicate([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{ return Diagnostic.Code == TEXT("Atomic.Poisoned.RollbackFailed"); })
		&& !Diagnostics.ContainsByPredicate([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{ return Diagnostic.Code == TEXT("Atomic.Poisoned.PostRollbackFinalizerFailed"); }));
	Test.TestEqual(TEXT("successful finalizer runs once after ordinary failure"), Finalizer->FinalizeCount, 1);
}

void TestPostRollbackFinalizerLifetimeCommitAndScopeExit(FAutomationTestBase& Test)
{
	FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	Receipt.CompleteFootprint.SetNum(1);
	FHandleResolver Handles;
	FParticipantRegistry Registry;
	int32 ScopeExitFinalizeCount = 0;
	bool bInvalidatedBeforeScopeExitFinalize = false;
	TSharedPtr<FPostRollbackFinalizer> Finalizer =
		MakeShared<FPostRollbackFinalizer>(TEXT("rollback.a"), nullptr, false, &ScopeExitFinalizeCount);
	TWeakPtr<FPostRollbackFinalizer> WeakFinalizer = Finalizer;
	{
		TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
			MakeUnique<FAssetDocumentAtomicTransactionRuntime>(Receipt, Handles, Registry, FAssetDocumentAtomicDiagnosticSink{});
		Transaction->EnlistPostRollbackFinalizer(Finalizer.ToSharedRef());
		Transaction->EnlistFootprint(Receipt.CompleteFootprint);
		Finalizer->BeforeFinalize = [&Handles, &bInvalidatedBeforeScopeExitFinalize]()
		{
			bInvalidatedBeforeScopeExitFinalize = Handles.Invalidations.Num() == 1
				&& Handles.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::Rollback;
		};
		Finalizer.Reset();
		Test.TestTrue(TEXT("transaction retains active finalizer until scope exit"), WeakFinalizer.IsValid());
	}
	Test.TestEqual(TEXT("scope exit invokes active finalizer once"), ScopeExitFinalizeCount, 1);
	Test.TestTrue(TEXT("handle invalidation precedes scope-exit finalizer callback"), bInvalidatedBeforeScopeExitFinalize);
	Test.TestFalse(TEXT("scope exit releases finalizer ownership"), WeakFinalizer.IsValid());

	FHandleResolver CommitHandles;
	FParticipantRegistry CommitRegistry;
	int32 CommitFinalizeCount = 0;
	TSharedPtr<FPostRollbackFinalizer> CommitFinalizer =
		MakeShared<FPostRollbackFinalizer>(TEXT("rollback.a"), nullptr, false, &CommitFinalizeCount);
	TWeakPtr<FPostRollbackFinalizer> WeakCommitFinalizer = CommitFinalizer;
	TUniquePtr<IAssetDocumentAtomicTransaction> CommitTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(Receipt, CommitHandles, CommitRegistry, FAssetDocumentAtomicDiagnosticSink{});
	CommitTransaction->EnlistPostRollbackFinalizer(CommitFinalizer.ToSharedRef());
	CommitTransaction->EnlistFootprint(Receipt.CompleteFootprint);
	CommitTransaction->MarkReadyToCommit();
	CommitFinalizer.Reset();
	Test.TestTrue(TEXT("commit retains active finalizer before terminal"), WeakCommitFinalizer.IsValid());
	Test.TestTrue(TEXT("commit never invokes finalizer"),
		CommitTransaction->Commit(MakeDurableReceipt(Receipt, *CommitTransaction)).bValid);
	Test.TestEqual(TEXT("commit callback count remains zero"), CommitFinalizeCount, 0);
	Test.TestFalse(TEXT("commit releases finalizer ownership"), WeakCommitFinalizer.IsValid());

	FHandleResolver RollbackHandles;
	FParticipantRegistry RollbackRegistry;
	int32 RollbackFinalizeCount = 0;
	TSharedPtr<FPostRollbackFinalizer> RollbackFinalizer =
		MakeShared<FPostRollbackFinalizer>(
			TEXT("rollback.a"), nullptr, false, &RollbackFinalizeCount);
	TWeakPtr<FPostRollbackFinalizer> WeakRollbackFinalizer = RollbackFinalizer;
	TUniquePtr<IAssetDocumentAtomicTransaction> RollbackTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt, RollbackHandles, RollbackRegistry, FAssetDocumentAtomicDiagnosticSink{});
	RollbackTransaction->EnlistPostRollbackFinalizer(RollbackFinalizer.ToSharedRef());
	RollbackTransaction->EnlistFootprint(Receipt.CompleteFootprint);
	RollbackFinalizer.Reset();
	Test.TestTrue(TEXT("rollback retains active finalizer before terminal"),
		WeakRollbackFinalizer.IsValid());
	Test.TestTrue(TEXT("explicit rollback succeeds after invoking the active finalizer"),
		RollbackTransaction->Rollback(TEXT("test.lifetime-rollback")).bValid);
	Test.TestEqual(TEXT("explicit rollback invokes finalizer once"), RollbackFinalizeCount, 1);
	Test.TestFalse(TEXT("explicit rollback releases finalizer ownership"),
		WeakRollbackFinalizer.IsValid());

	FHandleResolver PoisonHandles;
	FParticipantRegistry PoisonRegistry;
	int32 PoisonFinalizeCount = 0;
	TSharedPtr<FPostRollbackFinalizer> PoisonFinalizer =
		MakeShared<FPostRollbackFinalizer>(
			TEXT("rollback.a"), nullptr, true, &PoisonFinalizeCount);
	TWeakPtr<FPostRollbackFinalizer> WeakPoisonFinalizer = PoisonFinalizer;
	TUniquePtr<IAssetDocumentAtomicTransaction> PoisonTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt, PoisonHandles, PoisonRegistry, FAssetDocumentAtomicDiagnosticSink{});
	PoisonTransaction->EnlistPostRollbackFinalizer(PoisonFinalizer.ToSharedRef());
	PoisonTransaction->EnlistFootprint(Receipt.CompleteFootprint);
	PoisonFinalizer.Reset();
	Test.TestTrue(TEXT("poison path retains active finalizer before terminal"),
		WeakPoisonFinalizer.IsValid());
	Test.TestFalse(TEXT("finalizer failure selects the poisoned terminal"),
		PoisonTransaction->Rollback(TEXT("test.lifetime-poison")).bValid);
	Test.TestEqual(TEXT("poison path invokes finalizer once"), PoisonFinalizeCount, 1);
	Test.TestFalse(TEXT("poison terminal releases finalizer ownership"),
		WeakPoisonFinalizer.IsValid());
}

void TestDuplicateAndLateActionsPreserveAuthority(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FParticipantRegistry EmptyRegistry;
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt,
			Handles,
			EmptyRegistry,
			FAssetDocumentAtomicDiagnosticSink{});

	TSharedPtr<FRollbackAction> First =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	TSharedPtr<FRollbackAction> Duplicate =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	TWeakPtr<FRollbackAction> WeakDuplicate = Duplicate;
	const FAssetDocumentContractValidationResult FirstResult =
		Transaction->EnlistRollbackAction(First.ToSharedRef());
	Test.TestTrue(
		TEXT("first public action is accepted"),
		FirstResult.bValid);
	if (!FirstResult.bValid)
	{
		return;
	}
	Test.TestTrue(
		TEXT("duplicate public action returns the frozen diagnostic"),
		HasExactAtomicDiagnostic(
			Transaction->EnlistRollbackAction(Duplicate.ToSharedRef()),
			TEXTVIEW("Atomic.RollbackActionDuplicateComponentId")));
	Duplicate.Reset();
	Test.TestFalse(
		TEXT("duplicate action is not retained"),
		WeakDuplicate.IsValid());
	Test.TestTrue(
		TEXT("first action still covers its footprint"),
		Transaction->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);

	TSharedPtr<FRollbackAction> Late =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	TWeakPtr<FRollbackAction> WeakLate = Late;
	Test.TestTrue(
		TEXT("action after matching footprint returns late diagnostic before duplicate"),
		HasExactAtomicDiagnostic(
			Transaction->EnlistRollbackAction(Late.ToSharedRef()),
			TEXTVIEW("Atomic.RollbackActionLateEnlistment")));
	Late.Reset();
	Test.TestFalse(
		TEXT("late action is not retained"),
		WeakLate.IsValid());
	Test.TestTrue(
		TEXT("rollback keeps first public action authoritative"),
		Transaction->Rollback(TEXT("test.duplicate-late")).bValid);
	Test.TestEqual(
		TEXT("first public action executes once"),
		First->RollbackCount,
		1);
}

void TestPrivateParticipantCollisionAndLateAction(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FRollbackParticipant Participant(TEXT("rollback.a"), &Calls);
	FParticipantRegistry Registry;
	Registry.Add(Participant);
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();

	TUniquePtr<IAssetDocumentAtomicTransaction> CollisionTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt,
			Handles,
			Registry,
			FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FRollbackAction> Collision =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	TWeakPtr<FRollbackAction> WeakCollision = Collision;
	Test.TestTrue(
		TEXT("unused private participant rejects dynamic action as duplicate"),
		HasExactAtomicDiagnostic(
			CollisionTransaction->EnlistRollbackAction(Collision.ToSharedRef()),
			TEXTVIEW("Atomic.RollbackActionDuplicateComponentId")));
	Collision.Reset();
	Test.TestFalse(
		TEXT("private collision action is not retained"),
		WeakCollision.IsValid());
	Test.TestTrue(
		TEXT("private participant still covers footprint"),
		CollisionTransaction->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);
	Test.TestTrue(
		TEXT("private participant remains authoritative"),
		CollisionTransaction->Rollback(TEXT("test.private-collision")).bValid);
	Test.TestEqual(
		TEXT("private participant executes after collision"),
		Participant.RollbackCount,
		1);

	TUniquePtr<IAssetDocumentAtomicTransaction> LateTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt,
			Handles,
			Registry,
			FAssetDocumentAtomicDiagnosticSink{});
	Test.TestTrue(
		TEXT("private footprint enlists before late action"),
		LateTransaction->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);
	TSharedPtr<FRollbackAction> Late =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	Test.TestTrue(
		TEXT("private target also rejects matching action as late"),
		HasExactAtomicDiagnostic(
			LateTransaction->EnlistRollbackAction(Late.ToSharedRef()),
			TEXTVIEW("Atomic.RollbackActionLateEnlistment")));
	Test.TestTrue(
		TEXT("late rejection leaves private target authoritative"),
		LateTransaction->Rollback(TEXT("test.private-late")).bValid);
	Test.TestEqual(
		TEXT("private participant executes once per transaction"),
		Participant.RollbackCount,
		2);
}

void TestActionInvalidStatePrecedesOtherValidation(FAutomationTestBase& Test)
{
	auto TestRejectedState =
		[&Test](
			const TCHAR* Label,
			IAssetDocumentAtomicTransaction& Transaction,
			EAssetDocumentAtomicState ExpectedState)
		{
			TSharedPtr<FRollbackAction> Action =
				MakeShared<FRollbackAction>(TEXT(""));
			TWeakPtr<FRollbackAction> WeakAction = Action;
			const FAssetDocumentContractValidationResult Result =
				Transaction.EnlistRollbackAction(Action.ToSharedRef());
			Test.TestTrue(
				Label,
				HasExactAtomicDiagnostic(
					Result,
					TEXTVIEW("Atomic.RollbackActionInvalidState")));
			Action.Reset();
			Test.TestFalse(
				TEXT("invalid-state action is not retained"),
				WeakAction.IsValid());
			Test.TestEqual(
				TEXT("invalid-state rejection preserves state"),
				Transaction.GetState(),
				ExpectedState);
		};

	FHandleResolver Handles;
	FRollbackParticipant A(TEXT("rollback.a"));
	FRollbackParticipant B(TEXT("rollback.b"));
	FParticipantRegistry Registry;
	Registry.Add(A);
	Registry.Add(B);
	FCancellationView Cancellation;
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	FAssetDocumentAtomicTransactionFactoryRuntime Factory(Handles, Registry);

	TUniquePtr<IAssetDocumentAtomicTransaction> Ready =
		Factory.BeginLiveAfterPreflight(Receipt, Cancellation);
	Ready->EnlistFootprint(Receipt.CompleteFootprint);
	Ready->MarkReadyToCommit();
	TestRejectedState(
		TEXT("ready transaction rejects action before empty-ID validation"),
		*Ready,
		EAssetDocumentAtomicState::ReadyToCommit);

	TUniquePtr<IAssetDocumentAtomicTransaction> Committed =
		Factory.BeginLiveAfterPreflight(Receipt, Cancellation);
	Committed->EnlistFootprint(Receipt.CompleteFootprint);
	Committed->MarkReadyToCommit();
	Committed->Commit(MakeDurableReceipt(Receipt, *Committed));
	TestRejectedState(
		TEXT("committed transaction rejects action"),
		*Committed,
		EAssetDocumentAtomicState::Committed);

	TUniquePtr<IAssetDocumentAtomicTransaction> RolledBack =
		Factory.BeginLiveAfterPreflight(Receipt, Cancellation);
	RolledBack->Rollback(TEXT("test.rollback"));
	TestRejectedState(
		TEXT("rolled-back transaction rejects action"),
		*RolledBack,
		EAssetDocumentAtomicState::RolledBack);

	A.bFailRollback = true;
	TUniquePtr<IAssetDocumentAtomicTransaction> Poisoned =
		Factory.BeginLiveAfterPreflight(Receipt, Cancellation);
	Poisoned->EnlistFootprint(
		MakeArrayView(&Receipt.CompleteFootprint[0], 1));
	Poisoned->Rollback(TEXT("test.poison"));
	TestRejectedState(
		TEXT("poisoned transaction rejects action"),
		*Poisoned,
		EAssetDocumentAtomicState::Poisoned);
}

void TestInitialActionValidationAndPriority(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FRollbackParticipant A(TEXT("rollback.a"), &Calls);
	FRollbackParticipant B(TEXT("rollback.b"), &Calls);
	FParticipantRegistry Registry;
	Registry.Add(A);
	Registry.Add(B);
	FCancellationView Cancellation;
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	TSharedPtr<FRollbackAction> Initial =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	TArray<TSharedRef<IAssetDocumentRollbackAction>> InitialActions{
		Initial.ToSharedRef()};
	FAssetDocumentAtomicTransactionFactoryRuntime Factory(
		Handles,
		Registry,
		FAssetDocumentAtomicDiagnosticSink{},
		InitialActions);
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		Factory.BeginLiveAfterPreflight(Receipt, Cancellation);
	Test.TestNotNull(
		TEXT("valid initial action allows a live transaction"),
		Transaction.Get());
	if (!Transaction)
	{
		return;
	}
	Test.TestTrue(
		TEXT("initial public action covers matching footprint"),
		Transaction->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);
	Test.TestTrue(
		TEXT("initial public action target rolls back"),
		Transaction->Rollback(TEXT("test.initial")).bValid);
	Test.TestEqual(
		TEXT("initial public action wins over private compatibility alias"),
		Initial->RollbackCount,
		1);
	Test.TestEqual(
		TEXT("private alias is not also executed"),
		A.RollbackCount,
		0);

	TArray<EAssetDocumentRuntimeRequestPhase> Phases;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FAssetDocumentAtomicDiagnosticSink Sink =
		[&Phases, &Diagnostics](
			EAssetDocumentRuntimeRequestPhase Phase,
			const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			Phases.Add(Phase);
			Diagnostics.Add(Diagnostic);
		};
	TSharedPtr<FRollbackAction> Empty =
		MakeShared<FRollbackAction>(TEXT(""));
	FAssetDocumentAtomicTransactionFactoryRuntime EmptyFactory(
		Handles,
		Registry,
		Sink,
		TArray<TSharedRef<IAssetDocumentRollbackAction>>{
			Empty.ToSharedRef()});
	Test.TestNull(
		TEXT("empty initial action prevents live transaction exposure"),
		EmptyFactory.BeginLiveAfterPreflight(Receipt, Cancellation).Get());
	Test.TestTrue(
		TEXT("empty initial action emits exact preflight diagnostic"),
		Phases.Num() == 1
		&& Phases[0] == EAssetDocumentRuntimeRequestPhase::Preflight
		&& Diagnostics.Num() == 1
		&& IsExactAtomicDiagnostic(
			Diagnostics[0],
			TEXTVIEW("Atomic.RollbackActionEmptyComponentId")));

	Phases.Reset();
	Diagnostics.Reset();
	TSharedPtr<FRollbackAction> DuplicateA =
		MakeShared<FRollbackAction>(TEXT("rollback.a"));
	TSharedPtr<FRollbackAction> DuplicateB =
		MakeShared<FRollbackAction>(TEXT("rollback.a"));
	FAssetDocumentAtomicTransactionFactoryRuntime DuplicateFactory(
		Handles,
		Registry,
		Sink,
		TArray<TSharedRef<IAssetDocumentRollbackAction>>{
			DuplicateA.ToSharedRef(),
			DuplicateB.ToSharedRef()});
	Test.TestNull(
		TEXT("duplicate initial actions prevent live transaction exposure"),
		DuplicateFactory.BeginLiveAfterPreflight(Receipt, Cancellation).Get());
	Test.TestTrue(
		TEXT("duplicate initial actions emit exact preflight diagnostic"),
		Phases.Num() == 1
		&& Phases[0] == EAssetDocumentRuntimeRequestPhase::Preflight
		&& Diagnostics.Num() == 1
		&& IsExactAtomicDiagnostic(
			Diagnostics[0],
			TEXTVIEW("Atomic.RollbackActionDuplicateComponentId")));
}

void TestPublicActionRollbackOrderOnceAndCommit(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FParticipantRegistry Registry;
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt,
			Handles,
			Registry,
			FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FRollbackAction> A =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	TSharedPtr<FRollbackAction> B =
		MakeShared<FRollbackAction>(TEXT("rollback.b"), &Calls);
	Test.TestTrue(
		TEXT("B registration succeeds before A"),
		Transaction->EnlistRollbackAction(B.ToSharedRef()).bValid);
	Test.TestTrue(
		TEXT("A registration succeeds after B"),
		Transaction->EnlistRollbackAction(A.ToSharedRef()).bValid);
	Test.TestTrue(
		TEXT("A first footprint enlists"),
		Transaction->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);
	Test.TestTrue(
		TEXT("B second footprint enlists"),
		Transaction->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[1], 1)).bValid);
	Test.TestTrue(
		TEXT("public action rollback succeeds"),
		Transaction->Rollback(TEXT("test.public-order")).bValid);
	Test.TestTrue(
		TEXT("public actions roll back in reverse first-footprint order"),
		Calls.Calls.Num() == 3
		&& Calls.Calls[0] == TEXT("Invalidate.Rollback")
		&& Calls.Calls[1] == TEXT("RollbackAction.rollback.b")
		&& Calls.Calls[2] == TEXT("RollbackAction.rollback.a"));
	Test.TestEqual(
		TEXT("A public action executes once"),
		A->RollbackCount,
		1);
	Test.TestEqual(
		TEXT("B public action executes once"),
		B->RollbackCount,
		1);

	Calls.Calls.Reset();
	FAssetDocumentPreflightReceipt RepeatedReceipt = MakeReceipt();
	RepeatedReceipt.CompleteFootprint[1].RollbackComponentId =
		TEXT("rollback.a");
	TUniquePtr<IAssetDocumentAtomicTransaction> RepeatedTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			RepeatedReceipt,
			Handles,
			Registry,
			FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FRollbackAction> Repeated =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	Test.TestTrue(
		TEXT("repeated-component action enlists"),
		RepeatedTransaction->EnlistRollbackAction(
			Repeated.ToSharedRef()).bValid);
	Test.TestTrue(
		TEXT("first stable ID for repeated component enlists"),
		RepeatedTransaction->EnlistFootprint(
			MakeArrayView(
				&RepeatedReceipt.CompleteFootprint[0],
				1)).bValid);
	Test.TestTrue(
		TEXT("second stable ID for repeated component enlists"),
		RepeatedTransaction->EnlistFootprint(
			MakeArrayView(
				&RepeatedReceipt.CompleteFootprint[1],
				1)).bValid);
	Test.TestTrue(
		TEXT("repeated-component rollback succeeds"),
		RepeatedTransaction->Rollback(TEXT("test.once")).bValid);
	Test.TestEqual(
		TEXT("one component target executes exactly once"),
		Repeated->RollbackCount,
		1);

	Calls.Calls.Reset();
	TUniquePtr<IAssetDocumentAtomicTransaction> CommitTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt,
			Handles,
			Registry,
			FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FRollbackAction> CommitA =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	TSharedPtr<FRollbackAction> CommitB =
		MakeShared<FRollbackAction>(TEXT("rollback.b"), &Calls);
	CommitTransaction->EnlistRollbackAction(CommitA.ToSharedRef());
	CommitTransaction->EnlistRollbackAction(CommitB.ToSharedRef());
	CommitTransaction->EnlistFootprint(Receipt.CompleteFootprint);
	CommitTransaction->MarkReadyToCommit();
	Test.TestTrue(
		TEXT("commit succeeds with public rollback targets"),
		CommitTransaction->Commit(
			MakeDurableReceipt(Receipt, *CommitTransaction)).bValid);
	Test.TestEqual(
		TEXT("commit does not call A rollback action"),
		CommitA->RollbackCount,
		0);
	Test.TestEqual(
		TEXT("commit does not call B rollback action"),
		CommitB->RollbackCount,
		0);
}

void TestPublicActionFailureContinuesAndPoisons(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FParticipantRegistry Registry;
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt,
			Handles,
			Registry,
			FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FRollbackAction> A =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	TSharedPtr<FRollbackAction> B =
		MakeShared<FRollbackAction>(TEXT("rollback.b"), &Calls, true);
	Transaction->EnlistRollbackAction(A.ToSharedRef());
	Transaction->EnlistRollbackAction(B.ToSharedRef());
	Transaction->EnlistFootprint(Receipt.CompleteFootprint);
	Test.TestFalse(
		TEXT("public rollback failure returns failure"),
		Transaction->Rollback(TEXT("test.public-poison")).bValid);
	Test.TestTrue(
		TEXT("failing B does not prevent A and preserves reverse order"),
		Calls.Calls.Num() == 3
		&& Calls.Calls[0] == TEXT("Invalidate.Rollback")
		&& Calls.Calls[1] == TEXT("RollbackAction.rollback.b")
		&& Calls.Calls[2] == TEXT("RollbackAction.rollback.a"));
	Test.TestEqual(
		TEXT("failing B is attempted once"),
		B->RollbackCount,
		1);
	Test.TestEqual(
		TEXT("A is still attempted once"),
		A->RollbackCount,
		1);
	Test.TestEqual(
		TEXT("public action failure selects poisoned"),
		Transaction->GetState(),
		EAssetDocumentAtomicState::Poisoned);
}

void TestRollbackDiagnosticsPreserveExecutionOrder(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FParticipantRegistry Registry;
	FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	Receipt.CompleteFootprint.Add(MakeFootprint(TEXT("asset:/Game/C"), TEXT("rollback.c")));
	Receipt.CompleteFootprint.Add(MakeFootprint(TEXT("asset:/Game/D"), TEXT("rollback.d")));
	TArray<FAssetDocumentRuntimeDiagnostic> SinkDiagnostics;
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(Receipt, Handles, Registry,
			[&SinkDiagnostics](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{ SinkDiagnostics.Add(Diagnostic); });
	TSharedPtr<FRollbackAction> FollowingSuccess =
		MakeShared<FRollbackAction>(TEXT("rollback.b"), &Calls);
	TSharedPtr<FRollbackAction> SecondFailure =
		MakeShared<FRollbackAction>(TEXT("rollback.c"), &Calls, true);
	TSharedPtr<FRollbackAction> FirstFailure =
		MakeShared<FRollbackAction>(TEXT("rollback.d"), &Calls, true);
	TSharedPtr<FPostRollbackFinalizer> Finalizer =
		MakeShared<FPostRollbackFinalizer>(TEXT("rollback.a"), &Calls, true);
	FirstFailure->Diagnostics = {
		{TEXT("/Assets/D/Compile"), TEXT("test.rollback.compile"), TEXT("Original inverse compile failure.")},
		{TEXT("/Assets/D/Field"), TEXT("test.rollback.field"), TEXT("Original inverse field failure.")}};
	FAssetDocumentRuntimeDiagnosticContext& Context = FirstFailure->Diagnostics[0].Context.AddDefaulted_GetRef();
	Context.Key = TEXT("field");
	Context.Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Context.Value.StringValue = TEXT("OriginalField");
	SecondFailure->Diagnostics = {
		{TEXT("/Assets/C"), TEXT("test.rollback.second"), TEXT("Second action failure.")}};
	FollowingSuccess->Diagnostics = {
		{TEXT("/Assets/B"), TEXT("test.rollback.warning"), TEXT("Successful action warning."),
			EAssetDocumentDiagnosticSeverity::Warning}};
	Finalizer->Diagnostics = {
		{TEXT("/Assets/A/Finalize"), TEXT("test.rollback.finalizer"), TEXT("Original finalizer failure.")}};
	const TArray<FAssetDocumentRuntimeDiagnostic> ExpectedDetails{
		FirstFailure->Diagnostics[0], FirstFailure->Diagnostics[1],
		SecondFailure->Diagnostics[0], FollowingSuccess->Diagnostics[0], Finalizer->Diagnostics[0]};
	Test.TestTrue(TEXT("diagnostic regression actions and finalizer are admitted"),
		Transaction->EnlistRollbackAction(FirstFailure.ToSharedRef()).bValid
		&& Transaction->EnlistPostRollbackFinalizer(Finalizer.ToSharedRef()).bValid
		&& Transaction->EnlistRollbackAction(FollowingSuccess.ToSharedRef()).bValid
		&& Transaction->EnlistRollbackAction(SecondFailure.ToSharedRef()).bValid
		&& Transaction->EnlistFootprint(Receipt.CompleteFootprint).bValid);
	const FAssetDocumentContractValidationResult Result = Transaction->Rollback(TEXT("test.diagnostics"));
	Test.TestFalse(TEXT("later success does not erase earlier rollback failures"), Result.bValid);
	Test.TestEqual(TEXT("diagnostic-bearing rollback failures remain poisoned"),
		Transaction->GetState(), EAssetDocumentAtomicState::Poisoned);
	Test.TestTrue(TEXT("multiple failures continue through success and finalizer in footprint reverse order"),
		Calls.Calls == TArray<FString>{TEXT("Invalidate.Rollback"), TEXT("RollbackAction.rollback.d"),
			TEXT("RollbackAction.rollback.c"), TEXT("RollbackAction.rollback.b"),
			TEXT("PostRollbackFinalizer.rollback.a")});
	Test.TestEqual(TEXT("all original diagnostics follow one aggregate"), Result.Diagnostics.Num(), 6);
	if (Result.Diagnostics.Num() == 6)
	{
		Test.TestTrue(TEXT("stable aggregate remains first"),
			IsExactAtomicDiagnostic(Result.Diagnostics[0], TEXTVIEW("Atomic.Poisoned.RollbackFailed")));
		for (int32 Index = 0; Index < ExpectedDetails.Num(); ++Index)
		{
			const FAssetDocumentRuntimeDiagnostic& Actual = Result.Diagnostics[Index + 1];
			const FAssetDocumentRuntimeDiagnostic& Expected = ExpectedDetails[Index];
			Test.TestTrue(TEXT("original diagnostic fields retain execution and within-action order"),
				Actual.Path == Expected.Path && Actual.Code == Expected.Code
				&& Actual.Message == Expected.Message && Actual.Severity == Expected.Severity
				&& Actual.Context.Num() == Expected.Context.Num());
		}
		const TArray<FAssetDocumentRuntimeDiagnosticContext>& ActualContext = Result.Diagnostics[1].Context;
		Test.TestTrue(TEXT("original typed diagnostic context is preserved"),
			ActualContext.Num() == 1 && ActualContext[0].Key == TEXT("field")
			&& ActualContext[0].Value.Kind == EAssetDocumentRuntimeValueKind::String
			&& ActualContext[0].Value.StringValue == TEXT("OriginalField"));
	}
	Test.TestTrue(TEXT("atomic sink retains legacy terminals without duplicating returned details"),
		SinkDiagnostics.Num() == 4
		&& IsExactAtomicDiagnostic(SinkDiagnostics[0], TEXTVIEW("Atomic.Poisoned.ComponentFailed"))
		&& IsExactAtomicDiagnostic(SinkDiagnostics[1], TEXTVIEW("Atomic.Poisoned.ComponentFailed"))
		&& IsExactAtomicDiagnostic(SinkDiagnostics[2], TEXTVIEW("Atomic.Poisoned.PostRollbackFinalizerFailed"))
		&& IsExactAtomicDiagnostic(SinkDiagnostics[3], TEXTVIEW("Atomic.Poisoned.RollbackFailed")));
	TWeakPtr<FRollbackAction> WeakAction = FirstFailure;
	TWeakPtr<FPostRollbackFinalizer> WeakFinalizer = Finalizer;
	FirstFailure.Reset();
	Finalizer.Reset();
	Test.TestFalse(TEXT("diagnostic results do not retain poisoned action ownership"), WeakAction.IsValid());
	Test.TestFalse(TEXT("diagnostic results do not retain poisoned finalizer ownership"), WeakFinalizer.IsValid());
	const TArray<FString> CallsBeforeRetry = Calls.Calls;
	Test.TestFalse(TEXT("diagnostic-bearing poison rejects retries"), Transaction->Rollback(TEXT("retry")).bValid);
	Test.TestTrue(TEXT("rejected retry invokes no additional rollback callbacks"), Calls.Calls == CallsBeforeRetry);
}

void TestDynamicActionLifetimeAndRollbackInvalidation(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FParticipantRegistry Registry;
	FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	Receipt.CompleteFootprint.SetNum(1);
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt,
			Handles,
			Registry,
			FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FRollbackAction> Action =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &Calls);
	bool bObservedInvalidationBeforeCallback = false;
	Action->BeforeRollback =
		[&Handles, &bObservedInvalidationBeforeCallback]()
		{
			bObservedInvalidationBeforeCallback =
				Handles.Invalidations.Num() == 1
				&& Handles.Invalidations[0].Reason
					== EAssetDocumentHandleInvalidationReason::Rollback;
		};
	TWeakPtr<FRollbackAction> WeakAction = Action;
	Test.TestTrue(
		TEXT("dynamic action enlists before provider releases it"),
		Transaction->EnlistRollbackAction(Action.ToSharedRef()).bValid);
	Action.Reset();
	Test.TestTrue(
		TEXT("transaction owns dynamic action before terminal"),
		WeakAction.IsValid());
	Test.TestTrue(
		TEXT("owned dynamic action covers footprint"),
		Transaction->EnlistFootprint(Receipt.CompleteFootprint).bValid);
	Test.TestTrue(
		TEXT("owned dynamic action rollback succeeds"),
		Transaction->Rollback(TEXT("test.dynamic-lifetime")).bValid);
	Test.TestTrue(
		TEXT("resolver invalidation occurs before first action callback"),
		bObservedInvalidationBeforeCallback);
	Test.TestFalse(
		TEXT("rollback terminal releases dynamic action ownership"),
		WeakAction.IsValid());
}

void TestInitialActionLifetimeAfterFactoryRelease(FAutomationTestBase& Test)
{
	FHandleResolver Handles;
	FParticipantRegistry Registry;
	FCancellationView Cancellation;
	FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	Receipt.CompleteFootprint.SetNum(1);
	TSharedPtr<FRollbackAction> Action =
		MakeShared<FRollbackAction>(TEXT("rollback.a"));
	TWeakPtr<FRollbackAction> WeakAction = Action;
	TArray<TSharedRef<IAssetDocumentRollbackAction>> InitialActions{
		Action.ToSharedRef()};
	TUniquePtr<FAssetDocumentAtomicTransactionFactoryRuntime> Factory =
		MakeUnique<FAssetDocumentAtomicTransactionFactoryRuntime>(
			Handles,
			Registry,
			FAssetDocumentAtomicDiagnosticSink{},
			MoveTemp(InitialActions));
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		Factory->BeginLiveAfterPreflight(Receipt, Cancellation);
	Test.TestNotNull(
		TEXT("initial action factory creates transaction"),
		Transaction.Get());
	if (!Transaction)
	{
		return;
	}
	Factory.Reset();
	Action.Reset();
	Test.TestTrue(
		TEXT("transaction owns initial action after factory release"),
		WeakAction.IsValid());
	Transaction->EnlistFootprint(Receipt.CompleteFootprint);
	Test.TestTrue(
		TEXT("initial action rollback succeeds after factory release"),
		Transaction->Rollback(TEXT("test.initial-lifetime")).bValid);
	Test.TestFalse(
		TEXT("rollback terminal releases initial action ownership"),
		WeakAction.IsValid());
}

void TestCommitAndPoisonReleaseActionOwnership(FAutomationTestBase& Test)
{
	FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	Receipt.CompleteFootprint.SetNum(1);

	FCallRecord CommitCalls;
	FHandleResolver CommitHandles(&CommitCalls);
	FParticipantRegistry CommitRegistry;
	TUniquePtr<IAssetDocumentAtomicTransaction> CommitTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt,
			CommitHandles,
			CommitRegistry,
			FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FRollbackAction> CommitAction =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &CommitCalls);
	TWeakPtr<FRollbackAction> WeakCommitAction = CommitAction;
	CommitTransaction->EnlistRollbackAction(CommitAction.ToSharedRef());
	CommitTransaction->EnlistFootprint(Receipt.CompleteFootprint);
	CommitTransaction->MarkReadyToCommit();
	CommitAction.Reset();
	Test.TestTrue(
		TEXT("transaction owns action until commit terminal"),
		WeakCommitAction.IsValid());
	Test.TestTrue(
		TEXT("commit terminal succeeds for owned action"),
		CommitTransaction->Commit(
			MakeDurableReceipt(Receipt, *CommitTransaction)).bValid);
	Test.TestFalse(
		TEXT("commit terminal releases action ownership"),
		WeakCommitAction.IsValid());
	Test.TestFalse(
		TEXT("commit terminal never calls rollback action"),
		CommitCalls.Calls.Contains(TEXT("RollbackAction.rollback.a")));

	FCallRecord PoisonCalls;
	FHandleResolver PoisonHandles(&PoisonCalls);
	FParticipantRegistry PoisonRegistry;
	TUniquePtr<IAssetDocumentAtomicTransaction> PoisonTransaction =
		MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
			Receipt,
			PoisonHandles,
			PoisonRegistry,
			FAssetDocumentAtomicDiagnosticSink{});
	TSharedPtr<FRollbackAction> PoisonAction =
		MakeShared<FRollbackAction>(TEXT("rollback.a"), &PoisonCalls, true);
	TWeakPtr<FRollbackAction> WeakPoisonAction = PoisonAction;
	PoisonTransaction->EnlistRollbackAction(PoisonAction.ToSharedRef());
	PoisonTransaction->EnlistFootprint(Receipt.CompleteFootprint);
	PoisonAction.Reset();
	Test.TestTrue(
		TEXT("transaction owns failing action until poison terminal"),
		WeakPoisonAction.IsValid());
	Test.TestFalse(
		TEXT("failing action selects poison terminal"),
		PoisonTransaction->Rollback(TEXT("test.poison-lifetime")).bValid);
	Test.TestEqual(
		TEXT("poison terminal is fixed before ownership release"),
		PoisonTransaction->GetState(),
		EAssetDocumentAtomicState::Poisoned);
	Test.TestFalse(
		TEXT("poison terminal releases action ownership"),
		WeakPoisonAction.IsValid());
}

void TestFootprintAndCommit(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FRollbackParticipant A(TEXT("rollback.a"), &Calls);
	FRollbackParticipant B(TEXT("rollback.b"), &Calls);
	FParticipantRegistry Registry;
	Registry.Add(A);
	Registry.Add(B);
	FCancellationView Cancellation;
	FAssetDocumentAtomicTransactionFactoryRuntime Factory(Handles, Registry);
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Factory.BeginLiveAfterPreflight(Receipt, Cancellation);
	TArray<FAssetDocumentMutationFootprint> DuplicateBatch{
		Receipt.CompleteFootprint[0], Receipt.CompleteFootprint[0]};
	const FAssetDocumentContractValidationResult DuplicateResult = Transaction->EnlistFootprint(DuplicateBatch);
	Test.TestTrue(TEXT("duplicate stable IDs in one batch are rejected before enlistment"),
		!DuplicateResult.bValid && DuplicateResult.Diagnostics.Num() == 1
		&& DuplicateResult.Diagnostics[0].Code == TEXT("Atomic.FootprintAlreadyEnlisted")
		&& Transaction->GetState() == EAssetDocumentAtomicState::Live
		&& A.RollbackCount == 0 && B.RollbackCount == 0);
	Test.TestTrue(TEXT("unique enlistment remains available after duplicate batch rejection"),
		Transaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);

	Test.TestEqual(TEXT("first enlist enters mutating"), Transaction->GetState(), EAssetDocumentAtomicState::Mutating);
	Test.TestFalse(TEXT("incomplete footprint cannot become ready"), Transaction->MarkReadyToCommit().bValid);
	Test.TestFalse(TEXT("duplicate footprint is rejected"), Transaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);
	FAssetDocumentMutationFootprint Unknown = MakeFootprint(TEXT("asset:/Game/Unknown"), TEXT("rollback.a"));
	Test.TestFalse(TEXT("unknown footprint is rejected"), Transaction->EnlistFootprint(MakeArrayView(&Unknown, 1)).bValid);
	Test.TestTrue(TEXT("remaining declared footprint enlists"), Transaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[1], 1)).bValid);
	Test.TestTrue(TEXT("complete footprint becomes ready"), Transaction->MarkReadyToCommit().bValid);

	FAssetDocumentDurableCompletionReceipt Durable = MakeDurableReceipt(Receipt, *Transaction);
	FAssetDocumentDurableCompletionReceipt Invalid = Durable;
	Invalid.bSyncStateAdvanced = false;
	Test.TestFalse(TEXT("incomplete durable receipt is rejected"), Transaction->Commit(Invalid).bValid);
	Test.TestEqual(TEXT("rejected receipt performs no invalidation"), Handles.Invalidations.Num(), 0);
	Test.TestTrue(TEXT("complete durable receipt commits"), Transaction->Commit(Durable).bValid);
	Test.TestEqual(TEXT("commit invalidates exactly once"), Handles.Invalidations.Num(), 1);
	Test.TestEqual(TEXT("commit invalidation is first terminal call"), Calls.Calls[0], FString(TEXT("Invalidate.Commit")));
	Test.TestEqual(TEXT("commit selects committed state"), Transaction->GetState(), EAssetDocumentAtomicState::Committed);
	Test.TestFalse(TEXT("second terminal choice is rejected"), Transaction->Rollback(TEXT("late")).bValid);
}

void TestReverseRollbackAndPoison(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FRollbackParticipant A(TEXT("rollback.a"), &Calls);
	FRollbackParticipant B(TEXT("rollback.b"), &Calls);
	FParticipantRegistry Registry;
	Registry.Add(A);
	Registry.Add(B);
	FCancellationView Cancellation;
	FAssetDocumentAtomicTransactionFactoryRuntime Factory(Handles, Registry);
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Factory.BeginLiveAfterPreflight(Receipt, Cancellation);
	Transaction->EnlistFootprint(Receipt.CompleteFootprint);
	Test.TestTrue(TEXT("explicit rollback succeeds"), Transaction->Rollback(TEXT("test.failure")).bValid);
	Test.TestEqual(TEXT("rollback invalidates before participants"), Calls.Calls[0], FString(TEXT("Invalidate.Rollback")));
	Test.TestEqual(TEXT("last enlisted participant rolls back first"), Calls.Calls[1], FString(TEXT("Rollback.rollback.b")));
	Test.TestEqual(TEXT("first enlisted participant rolls back last"), Calls.Calls[2], FString(TEXT("Rollback.rollback.a")));
	Test.TestEqual(TEXT("successful rollback terminal"), Transaction->GetState(), EAssetDocumentAtomicState::RolledBack);

	Calls.Calls.Reset();
	A.bFailRollback = true;
	Transaction = Factory.BeginLiveAfterPreflight(Receipt, Cancellation);
	Transaction->EnlistFootprint(Receipt.CompleteFootprint);
	Test.TestFalse(TEXT("participant failure poisons rollback"), Transaction->Rollback(TEXT("test.poison")).bValid);
	Test.TestEqual(TEXT("poison still attempts all participants"), Calls.Calls.Num(), 3);
	Test.TestEqual(TEXT("participant failure selects poisoned"), Transaction->GetState(), EAssetDocumentAtomicState::Poisoned);
	const FAssetDocumentContractValidationResult PoisonedMutation =
		Transaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[0], 1));
	Test.TestEqual(TEXT("poisoned mutation returns stable code"), PoisonedMutation.Diagnostics[0].Code, FString(TEXT("Atomic.Poisoned")));
	Test.TestFalse(TEXT("poison retry is rejected"), Transaction->Rollback(TEXT("retry")).bValid);
}

void TestDestructorRollback(FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FRollbackParticipant A(TEXT("rollback.a"), &Calls);
	FRollbackParticipant B(TEXT("rollback.b"), &Calls);
	FParticipantRegistry Registry;
	Registry.Add(A);
	Registry.Add(B);
	FCancellationView Cancellation;
	FAssetDocumentAtomicTransactionFactoryRuntime Factory(Handles, Registry);
	const FAssetDocumentPreflightReceipt Receipt = MakeReceipt();
	{
		TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Factory.BeginLiveAfterPreflight(Receipt, Cancellation);
		Transaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[0], 1));
	}
	Test.TestEqual(TEXT("scope exit invalidates then rolls back enlisted participant"), Calls.Calls.Num(), 2);
	Test.TestEqual(TEXT("scope exit invalidation first"), Calls.Calls[0], FString(TEXT("Invalidate.Rollback")));
	Test.TestEqual(TEXT("scope exit rolls back enlisted participant"), A.RollbackCount, 1);
	Test.TestEqual(TEXT("scope exit skips unenlisted participant"), B.RollbackCount, 0);
}
} // namespace AssetDocumentAtomicTransactionRuntimeTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicTransactionRuntimeTest,
	"UEAssetDoc.AtomicFramework.TransactionRuntime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicTransactionRuntimeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAtomicTransactionRuntimeTests;
	TestFactoryAdmission(*this);
	TestDynamicActionValidationAndAtomicity(*this);
	TestRejectedActionsAreNotRetained(*this);
	TestPostRollbackFinalizerAdmission(*this);
	TestPostRollbackFinalizerLast(*this);
	TestPostRollbackFinalizerActivation(*this);
	TestPostRollbackFinalizerFailures(*this);
	TestPostRollbackFinalizerOrdinaryFailure(*this);
	TestPostRollbackFinalizerLifetimeCommitAndScopeExit(*this);
	TestDuplicateAndLateActionsPreserveAuthority(*this);
	TestPrivateParticipantCollisionAndLateAction(*this);
	TestActionInvalidStatePrecedesOtherValidation(*this);
	TestInitialActionValidationAndPriority(*this);
	TestPublicActionRollbackOrderOnceAndCommit(*this);
	TestPublicActionFailureContinuesAndPoisons(*this);
	TestRollbackDiagnosticsPreserveExecutionOrder(*this);
	TestDynamicActionLifetimeAndRollbackInvalidation(*this);
	TestInitialActionLifetimeAfterFactoryRelease(*this);
	TestCommitAndPoisonReleaseActionOwnership(*this);
	TestFootprintAndCommit(*this);
	TestReverseRollbackAndPoison(*this);
	TestDestructorRollback(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRollbackDiagnosticsTest,
	"UEAssetDoc.AtomicTransaction.RollbackDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRollbackDiagnosticsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAtomicTransactionRuntimeTests;
	TestRollbackDiagnosticsPreserveExecutionOrder(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPostRollbackFinalizerAdmissionTest,
	"UEAssetDoc.AtomicTransaction.PostRollbackFinalizerAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPostRollbackFinalizerAdmissionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAtomicTransactionRuntimeTests;
	TestPostRollbackFinalizerAdmission(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPostRollbackFinalizerLastTest,
	"UEAssetDoc.AtomicTransaction.PostRollbackFinalizerLast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPostRollbackFinalizerLastTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAtomicTransactionRuntimeTests;
	TestPostRollbackFinalizerLast(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPostRollbackFinalizerActivationTest,
	"UEAssetDoc.AtomicTransaction.PostRollbackFinalizerActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPostRollbackFinalizerActivationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAtomicTransactionRuntimeTests;
	TestPostRollbackFinalizerActivation(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPostRollbackFinalizerFailureTest,
	"UEAssetDoc.AtomicTransaction.PostRollbackFinalizerFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPostRollbackFinalizerFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAtomicTransactionRuntimeTests;
	TestPostRollbackFinalizerFailures(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPostRollbackFinalizerOrdinaryFailureTest,
	"UEAssetDoc.AtomicTransaction.PostRollbackFinalizerOrdinaryFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPostRollbackFinalizerOrdinaryFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAtomicTransactionRuntimeTests;
	TestPostRollbackFinalizerOrdinaryFailure(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPostRollbackFinalizerLifetimeTest,
	"UEAssetDoc.AtomicTransaction.PostRollbackFinalizerLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPostRollbackFinalizerLifetimeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAtomicTransactionRuntimeTests;
	TestPostRollbackFinalizerLifetimeCommitAndScopeExit(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPostRollbackFinalizerCommitAndScopeExitTest,
	"UEAssetDoc.AtomicTransaction.PostRollbackFinalizerCommitAndScopeExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPostRollbackFinalizerCommitAndScopeExitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAtomicTransactionRuntimeTests;
	TestPostRollbackFinalizerLifetimeCommitAndScopeExit(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPostRollbackFinalizerCompatibilityTest,
	"UEAssetDoc.AtomicTransaction.PostRollbackFinalizerCompatibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPostRollbackFinalizerCompatibilityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentAtomicTransactionRuntimeTests;
	TestPublicActionRollbackOrderOnceAndCommit(*this);
	TestPrivateParticipantCollisionAndLateAction(*this);
	TestReverseRollbackAndPoison(*this);
	return true;
}

#endif
