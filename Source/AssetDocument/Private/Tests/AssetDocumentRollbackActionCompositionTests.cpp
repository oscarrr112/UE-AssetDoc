// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentRollbackAction.h"

#include "Misc/AutomationTest.h"
#include "Tests/AssetDocumentAtomicFrameworkTestDoubles.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentRollbackActionCompositionTests
{
using namespace AssetDocumentAtomicFrameworkTestDoubles;

class FTestRollbackAction final : public IAssetDocumentRollbackAction
{
public:
	explicit FTestRollbackAction(
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
		++RollbackCount;
		ObservedFailureCodes.Add(FString(StableFailureCode));
		if (Record != nullptr)
		{
			Record->Calls.Add(FString::Printf(
				TEXT("Rollback.%s"), *ComponentId));
		}
		FAssetDocumentContractValidationResult Result;
		Result.bValid = !bFailRollback;
		return Result;
	}

	int32 RollbackCount = 0;
	TArray<FString> ObservedFailureCodes;

private:
	FString ComponentId;
	FCallRecord* Record = nullptr;
	bool bFailRollback = false;
};

FAssetDocumentPreflightReceipt MakeReceipt(
	TConstArrayView<FString> ComponentIds)
{
	FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = FGuid::NewGuid();
	Receipt.RuntimeRevision = 1;
	Receipt.ReceiptHash = TEXT("sha256:rollback-action-red");
	Receipt.bSucceeded = true;
	for (int32 Index = 0; Index < ComponentIds.Num(); ++Index)
	{
		FAssetDocumentMutationFootprint& Item =
			Receipt.CompleteFootprint.AddDefaulted_GetRef();
		Item.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
		Item.StableId = FString::Printf(TEXT("item.%d"), Index);
		Item.RollbackComponentId = ComponentIds[Index];
	}
	return Receipt;
}

bool HasDiagnostic(
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics,
	FStringView Code)
{
	return Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

bool HasExactDiagnostic(
	const FAssetDocumentContractValidationResult& Result,
	FStringView Code)
{
	return !Result.bValid
		&& Result.Diagnostics.Num() == 1
		&& Result.Diagnostics[0].Path == TEXT("/RollbackActionComposition")
		&& Result.Diagnostics[0].Code == Code;
}

void TestRollbackCallOrder(
	FAutomationTestBase& Test,
	const FCallRecord& Calls)
{
	Test.TestEqual(TEXT("rollback call count"), Calls.Calls.Num(), 3);
	if (Calls.Calls.Num() != 3)
	{
		return;
	}
	Test.TestEqual(
		TEXT("invalidation is first"),
		Calls.Calls[0], FString(TEXT("Invalidate.Rollback")));
	Test.TestEqual(
		TEXT("package rolls back second"),
		Calls.Calls[1], FString(TEXT("Rollback.rollback.package")));
	Test.TestEqual(
		TEXT("uobject rolls back third"),
		Calls.Calls[2], FString(TEXT("Rollback.rollback.uobject")));
}

void TestObservedFailureCodes(
	FAutomationTestBase& Test,
	const FTestRollbackAction& UObjectAction,
	const FTestRollbackAction& PackageAction,
	const TCHAR* ExpectedCode)
{
	Test.TestEqual(
		TEXT("uobject failure code count"),
		UObjectAction.ObservedFailureCodes.Num(), 1);
	Test.TestEqual(
		TEXT("package failure code count"),
		PackageAction.ObservedFailureCodes.Num(), 1);
	if (UObjectAction.ObservedFailureCodes.Num() != 1
		|| PackageAction.ObservedFailureCodes.Num() != 1)
	{
		return;
	}
	Test.TestEqual(
		TEXT("uobject receives exact failure code"),
		UObjectAction.ObservedFailureCodes[0], FString(ExpectedCode));
	Test.TestEqual(
		TEXT("package receives exact failure code"),
		PackageAction.ObservedFailureCodes[0], FString(ExpectedCode));
}

void TestEmptyCompositionDefersAtomicCoverage(
	FAutomationTestBase& Test)
{
	FHandleResolver Handles;
	FCancellationView Cancellation;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	auto Sink = [&Diagnostics](
		EAssetDocumentRuntimeRequestPhase,
		const FAssetDocumentRuntimeDiagnostic& Diagnostic)
	{
		Diagnostics.Add(Diagnostic);
	};
	FAssetDocumentRollbackActionComposition Empty(Handles, {}, Sink);
	const FAssetDocumentPreflightReceipt Receipt =
		MakeReceipt({TEXT("rollback.uobject")});
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		Empty.BeginLiveAfterPreflight(Receipt, Cancellation);

	Test.TestTrue(TEXT("empty composition is valid"), Empty.IsValid());
	Test.TestNotNull(
		TEXT("empty composition can begin before provider-time coverage"),
		Transaction.Get());
	if (!Transaction)
	{
		return;
	}
	const FAssetDocumentContractValidationResult Enlistment =
		Transaction->EnlistFootprint(Receipt.CompleteFootprint);
	Test.TestTrue(
		TEXT("empty composition rejects uncovered footprint atomically"),
		HasDiagnostic(
			Enlistment.Diagnostics,
			TEXT("Atomic.IncompleteRollbackCoverage")));
	Test.TestEqual(
		TEXT("coverage rejection preserves live state"),
		Transaction->GetState(),
		EAssetDocumentAtomicState::Live);
	Test.TestTrue(
		TEXT("empty composition projects deferred Atomic coverage terminal"),
		HasDiagnostic(
			Diagnostics, TEXT("Atomic.IncompleteRollbackCoverage")));
}

void TestPreRegisteredActionIsTransactionOwned(
	FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FCancellationView Cancellation;
	TSharedPtr<FTestRollbackAction> Action =
		MakeShared<FTestRollbackAction>(TEXT("rollback.uobject"), &Calls);
	TWeakPtr<FTestRollbackAction> WeakAction = Action;
	const FAssetDocumentPreflightReceipt Receipt =
		MakeReceipt({TEXT("rollback.uobject")});
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction;
	{
		TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions{
			Action.ToSharedRef()};
		FAssetDocumentRollbackActionComposition Composition(
			Handles, MoveTemp(Actions), {});
		Transaction =
			Composition.BeginLiveAfterPreflight(Receipt, Cancellation);
	}
	Action.Reset();

	Test.TestNotNull(
		TEXT("pre-registered composition creates transaction"),
		Transaction.Get());
	Test.TestTrue(
		TEXT("transaction owns pre-registered action after composition release"),
		WeakAction.IsValid());
	if (!Transaction || !WeakAction.IsValid())
	{
		return;
	}
	Test.TestTrue(
		TEXT("transaction-owned pre-registered action covers footprint"),
		Transaction->EnlistFootprint(Receipt.CompleteFootprint).bValid);
	Test.TestTrue(
		TEXT("transaction-owned pre-registered action rolls back"),
		Transaction->Rollback(TEXT("test.initial-owned")).bValid);
	Test.TestTrue(
		TEXT("pre-registered action executes exactly once"),
		Calls.Calls.Num() == 2
		&& Calls.Calls[0] == TEXT("Invalidate.Rollback")
		&& Calls.Calls[1] == TEXT("Rollback.rollback.uobject"));
	Test.TestFalse(
		TEXT("rollback terminal releases pre-registered action ownership"),
		WeakAction.IsValid());
}

void TestPreRegisteredAndDynamicActionsCompose(
	FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FCancellationView Cancellation;
	TSharedRef<FTestRollbackAction> Initial =
		MakeShared<FTestRollbackAction>(TEXT("rollback.uobject"), &Calls);
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions{Initial};
	FAssetDocumentRollbackActionComposition Composition(
		Handles, MoveTemp(Actions), {});
	const FAssetDocumentPreflightReceipt Receipt =
		MakeReceipt({TEXT("rollback.uobject"), TEXT("rollback.dynamic")});
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		Composition.BeginLiveAfterPreflight(Receipt, Cancellation);
	Test.TestNotNull(
		TEXT("mixed initial and dynamic transaction begins"),
		Transaction.Get());
	if (!Transaction)
	{
		return;
	}
	TSharedRef<FTestRollbackAction> Dynamic =
		MakeShared<FTestRollbackAction>(TEXT("rollback.dynamic"), &Calls);
	Test.TestTrue(
		TEXT("dynamic action coexists with different initial action"),
		Transaction->EnlistRollbackAction(Dynamic).bValid);
	Test.TestTrue(
		TEXT("initial component footprint enlists first"),
		Transaction->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);
	Test.TestTrue(
		TEXT("dynamic component footprint enlists second"),
		Transaction->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[1], 1)).bValid);
	Test.TestTrue(
		TEXT("mixed initial and dynamic rollback succeeds"),
		Transaction->Rollback(TEXT("test.mixed")).bValid);
	Test.TestTrue(
		TEXT("mixed actions roll back once in reverse footprint order"),
		Calls.Calls.Num() == 3
		&& Calls.Calls[0] == TEXT("Invalidate.Rollback")
		&& Calls.Calls[1] == TEXT("Rollback.rollback.dynamic")
		&& Calls.Calls[2] == TEXT("Rollback.rollback.uobject"));
	Test.TestEqual(TEXT("initial action executes once"), Initial->RollbackCount, 1);
	Test.TestEqual(TEXT("dynamic action executes once"), Dynamic->RollbackCount, 1);

	Calls.Calls.Reset();
	TUniquePtr<IAssetDocumentAtomicTransaction> Collision =
		Composition.BeginLiveAfterPreflight(Receipt, Cancellation);
	Test.TestNotNull(TEXT("collision transaction begins"), Collision.Get());
	if (!Collision)
	{
		return;
	}
	TSharedPtr<FTestRollbackAction> Duplicate =
		MakeShared<FTestRollbackAction>(TEXT("rollback.uobject"), &Calls);
	TWeakPtr<FTestRollbackAction> WeakDuplicate = Duplicate;
	Test.TestTrue(
		TEXT("dynamic collision with initial action fails closed"),
		HasDiagnostic(
			Collision->EnlistRollbackAction(Duplicate.ToSharedRef()).Diagnostics,
			TEXT("Atomic.RollbackActionDuplicateComponentId")));
	Duplicate.Reset();
	Test.TestFalse(
		TEXT("rejected colliding action is not retained"),
		WeakDuplicate.IsValid());
	Test.TestTrue(
		TEXT("initial action remains authoritative after collision"),
		Collision->EnlistFootprint(
			MakeArrayView(&Receipt.CompleteFootprint[0], 1)).bValid);
	Test.TestTrue(
		TEXT("collision transaction rolls back original target"),
		Collision->Rollback(TEXT("test.collision")).bValid);
	Test.TestEqual(
		TEXT("initial action executes exactly once after collision"),
		Initial->RollbackCount,
		2);
}

void TestInvalidComponentIdsFailClosed(
	FAutomationTestBase& Test)
{
	FHandleResolver Handles;
	FCancellationView Cancellation;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	auto Sink = [&Diagnostics](
		EAssetDocumentRuntimeRequestPhase,
		const FAssetDocumentRuntimeDiagnostic& Diagnostic)
	{
		Diagnostics.Add(Diagnostic);
	};

	TArray<TSharedRef<IAssetDocumentRollbackAction>> EmptyIdActions;
	EmptyIdActions.Add(MakeShared<FTestRollbackAction>(TEXT("")));
	FAssetDocumentRollbackActionComposition EmptyId(
		Handles, MoveTemp(EmptyIdActions), Sink);
	Test.TestFalse(TEXT("empty component ID is invalid"), EmptyId.IsValid());
	Test.TestTrue(
		TEXT("empty component ID has exact diagnostic"),
		HasExactDiagnostic(
			EmptyId.GetValidation(),
			TEXT("RollbackActionComposition.EmptyComponentId")));
	Test.TestNull(
		TEXT("empty component ID cannot begin"),
		EmptyId.BeginLiveAfterPreflight(
			MakeReceipt({TEXT("rollback.uobject")}), Cancellation).Get());

	Diagnostics.Reset();
	TArray<TSharedRef<IAssetDocumentRollbackAction>> DuplicateActions;
	DuplicateActions.Add(
		MakeShared<FTestRollbackAction>(TEXT("rollback.uobject")));
	DuplicateActions.Add(
		MakeShared<FTestRollbackAction>(TEXT("rollback.uobject")));
	FAssetDocumentRollbackActionComposition Duplicate(
		Handles, MoveTemp(DuplicateActions), Sink);
	Test.TestFalse(TEXT("duplicate component ID is invalid"), Duplicate.IsValid());
	Test.TestTrue(
		TEXT("duplicate component ID has exact diagnostic"),
		HasExactDiagnostic(
			Duplicate.GetValidation(),
			TEXT("RollbackActionComposition.DuplicateComponentId")));
	Test.TestNull(
		TEXT("duplicate component ID cannot begin"),
		Duplicate.BeginLiveAfterPreflight(
			MakeReceipt({TEXT("rollback.uobject")}), Cancellation).Get());
	Test.TestTrue(
		TEXT("invalid begin projects stored diagnostic"),
		HasDiagnostic(
			Diagnostics,
			TEXT("RollbackActionComposition.DuplicateComponentId")));
}

void TestRollbackOrderAndOnceOnly(
	FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FCancellationView Cancellation;
	TSharedRef<FTestRollbackAction> UObjectAction =
		MakeShared<FTestRollbackAction>(TEXT("rollback.uobject"), &Calls);
	TSharedRef<FTestRollbackAction> PackageAction =
		MakeShared<FTestRollbackAction>(TEXT("rollback.package"), &Calls);
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions{
		UObjectAction, PackageAction};
	FAssetDocumentRollbackActionComposition Composition(
		Handles, MoveTemp(Actions), {});
	const FAssetDocumentPreflightReceipt Receipt =
		MakeReceipt({TEXT("rollback.uobject"), TEXT("rollback.package")});
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		Composition.BeginLiveAfterPreflight(Receipt, Cancellation);

	Test.TestNotNull(TEXT("exact actions provide formal coverage"), Transaction.Get());
	if (!Transaction)
	{
		return;
	}
	Test.TestTrue(
		TEXT("complete footprint enlists once"),
		Transaction->EnlistFootprint(Receipt.CompleteFootprint).bValid);
	Test.TestTrue(
		TEXT("rollback succeeds"), Transaction->Rollback(TEXT("test.failure")).bValid);
	TestRollbackCallOrder(Test, Calls);
	Test.TestEqual(TEXT("uobject invoked once"), UObjectAction->RollbackCount, 1);
	Test.TestEqual(TEXT("package invoked once"), PackageAction->RollbackCount, 1);
	TestObservedFailureCodes(
		Test, UObjectAction.Get(), PackageAction.Get(), TEXT("test.failure"));
	Test.TestEqual(TEXT("rollback acquires no handles"), Handles.AcquiredHandles.Num(), 0);

	const FAssetDocumentContractValidationResult Second =
		Transaction->Rollback(TEXT("test.retry"));
	Test.TestTrue(
		TEXT("second rollback keeps existing terminal"),
		!Second.bValid
		&& HasDiagnostic(
			Second.Diagnostics, TEXT("Atomic.TerminalAlreadyChosen")));
	Test.TestEqual(TEXT("second rollback does not retry uobject"), UObjectAction->RollbackCount, 1);
	Test.TestEqual(TEXT("second rollback does not retry package"), PackageAction->RollbackCount, 1);
}

void TestFailurePoisonsAndContinues(
	FAutomationTestBase& Test)
{
	FCallRecord Calls;
	FHandleResolver Handles(&Calls);
	FCancellationView Cancellation;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TSharedRef<FTestRollbackAction> UObjectAction =
		MakeShared<FTestRollbackAction>(TEXT("rollback.uobject"), &Calls);
	TSharedRef<FTestRollbackAction> PackageAction =
		MakeShared<FTestRollbackAction>(
			TEXT("rollback.package"), &Calls, true);
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions{
		UObjectAction, PackageAction};
	FAssetDocumentRollbackActionComposition Composition(
		Handles,
		MoveTemp(Actions),
		[&Diagnostics](
			EAssetDocumentRuntimeRequestPhase,
			const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			Diagnostics.Add(Diagnostic);
		});
	const FAssetDocumentPreflightReceipt Receipt =
		MakeReceipt({TEXT("rollback.uobject"), TEXT("rollback.package")});
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		Composition.BeginLiveAfterPreflight(Receipt, Cancellation);
	Test.TestNotNull(TEXT("failure case obtains transaction"), Transaction.Get());
	if (!Transaction)
	{
		return;
	}
	Test.TestTrue(
		TEXT("failure case enlists complete footprint"),
		Transaction->EnlistFootprint(Receipt.CompleteFootprint).bValid);
	const FAssetDocumentContractValidationResult Result =
		Transaction->Rollback(TEXT("test.poison"));

	Test.TestFalse(TEXT("component failure poisons rollback"), Result.bValid);
	TestRollbackCallOrder(Test, Calls);
	Test.TestEqual(TEXT("failed package invoked once"), PackageAction->RollbackCount, 1);
	Test.TestEqual(TEXT("remaining uobject still invoked"), UObjectAction->RollbackCount, 1);
	TestObservedFailureCodes(
		Test, UObjectAction.Get(), PackageAction.Get(), TEXT("test.poison"));
	Test.TestTrue(
		TEXT("component failure diagnostic is preserved"),
		HasDiagnostic(
			Diagnostics, TEXT("Atomic.Poisoned.ComponentFailed")));
	Test.TestTrue(
		TEXT("rollback failure diagnostic is preserved"),
		HasDiagnostic(
			Diagnostics, TEXT("Atomic.Poisoned.RollbackFailed")));
	Test.TestEqual(
		TEXT("failure selects poisoned state"),
		Transaction->GetState(), EAssetDocumentAtomicState::Poisoned);
	Test.TestEqual(TEXT("failure path performs no acquire"), Handles.AcquiredHandles.Num(), 0);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRollbackActionCompositionTest,
	"UEAssetDoc.AtomicFramework.RollbackActionComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRollbackActionCompositionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentRollbackActionCompositionTests;
	TestEmptyCompositionDefersAtomicCoverage(*this);
	TestPreRegisteredActionIsTransactionOwned(*this);
	TestPreRegisteredAndDynamicActionsCompose(*this);
	TestInvalidComponentIdsFailClosed(*this);
	TestRollbackOrderAndOnceOnly(*this);
	TestFailurePoisonsAndContinues(*this);
	return true;
}
#endif
