// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentAtomicFile.h"
#include "Tests/AssetDocumentAtomicTestMount.h"
#include "AssetDocumentRollbackAction.h"
#include "Tests/AssetDocumentAtomicFrameworkTestDoubles.h"
#include "Runtime/AssetDocumentPackageStaging.h"
#include "Runtime/AssetDocumentPersistenceRuntime.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentAtomicAllPhaseRollbackTests
{
using namespace AssetDocumentAtomicFrameworkTestDoubles;

enum class EFrozenOracleCell : uint8
{
	Unset,
	Pass,
	Fail,
	NotApplicable,
};

struct FFrozenRowOracle
{
	int32 Row = 0;
	EFrozenOracleCell Diagnostic = EFrozenOracleCell::Unset;
	EFrozenOracleCell Identity = EFrozenOracleCell::Unset;
	EFrozenOracleCell TransactionState = EFrozenOracleCell::Unset;
	EFrozenOracleCell Baselines = EFrozenOracleCell::Unset;
	EFrozenOracleCell Ownership = EFrozenOracleCell::Unset;
	EFrozenOracleCell HandleOrder = EFrozenOracleCell::Unset;
	EFrozenOracleCell SyncTiming = EFrozenOracleCell::Unset;
	EFrozenOracleCell PoisonOutcome = EFrozenOracleCell::Unset;
	EFrozenOracleCell RetryEligibility = EFrozenOracleCell::Unset;
	EFrozenOracleCell CollaboratorCounts = EFrozenOracleCell::Unset;
};

EFrozenOracleCell Observed(bool bPassed)
{
	return bPassed ? EFrozenOracleCell::Pass : EFrozenOracleCell::Fail;
}

EFrozenOracleCell NotApplicable(bool bObservedAbsentOrZero)
{
	return bObservedAbsentOrZero ? EFrozenOracleCell::NotApplicable : EFrozenOracleCell::Fail;
}

void CheckFrozenRowOracle(FAutomationTestBase& Test, const FFrozenRowOracle& Oracle)
{
	const TArray<TPair<const TCHAR*, EFrozenOracleCell>> Cells{
		{TEXT("diagnostic"), Oracle.Diagnostic}, {TEXT("identity"), Oracle.Identity},
		{TEXT("transaction-state"), Oracle.TransactionState}, {TEXT("baselines"), Oracle.Baselines},
		{TEXT("ownership"), Oracle.Ownership}, {TEXT("handle-order"), Oracle.HandleOrder},
		{TEXT("sync-timing"), Oracle.SyncTiming}, {TEXT("poison-outcome"), Oracle.PoisonOutcome},
		{TEXT("retry-eligibility"), Oracle.RetryEligibility}, {TEXT("collaborator-counts"), Oracle.CollaboratorCounts}};
	for (const TPair<const TCHAR*, EFrozenOracleCell>& Cell : Cells)
	{
		const FString Label = FString::Printf(TEXT("Row%02d frozen oracle: %s is explicit and satisfied"), Oracle.Row, Cell.Key);
		Test.TestTrue(*Label, Cell.Value == EFrozenOracleCell::Pass || Cell.Value == EFrozenOracleCell::NotApplicable);
	}

	const bool bTransactionOnlyRow = Oracle.Row <= 6 || Oracle.Row == 17 || Oracle.Row == 18
		|| Oracle.Row == 21 || Oracle.Row == 22;
	if (bTransactionOnlyRow)
	{
		Test.TestEqual(
			*FString::Printf(TEXT("Row%02d frozen oracle: package and sidecar baselines are explicit absent/zero"), Oracle.Row),
			Oracle.Baselines,
			EFrozenOracleCell::NotApplicable);
		Test.TestEqual(
			*FString::Printf(TEXT("Row%02d frozen oracle: UObject/package/registry/file ownership is explicit zero"), Oracle.Row),
			Oracle.Ownership,
			EFrozenOracleCell::NotApplicable);
	}
	else
	{
		Test.TestEqual(
			*FString::Printf(TEXT("Row%02d frozen oracle: no live transaction terminal is explicit zero"), Oracle.Row),
			Oracle.TransactionState,
			EFrozenOracleCell::NotApplicable);
	}
}

FAssetDocumentContractValidationResult Valid()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentContractValidationResult Invalid(const TCHAR* Code)
{
	FAssetDocumentContractValidationResult Result;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/AllPhaseRollback");
	Diagnostic.Code = Code;
	Diagnostic.Message = TEXT("Injected all-phase failure.");
	return Result;
}

bool Write(const FString& Path, TConstArrayView<uint8> Bytes)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

TArray<uint8> Read(const FString& Path)
{
	TArray<uint8> Bytes;
	if (IFileManager::Get().FileExists(*Path))
	{
		FFileHelper::LoadFileToArray(Bytes, *Path);
	}
	return Bytes;
}

FAssetDocumentMutationFootprint Footprint(const TCHAR* StableId, const TCHAR* ComponentId)
{
	FAssetDocumentMutationFootprint Item;
	Item.Kind = EAssetDocumentMutationFootprintKind::File;
	Item.StableId = StableId;
	Item.RollbackComponentId = ComponentId;
	return Item;
}

FAssetDocumentPreflightReceipt Preflight()
{
	FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = FGuid::NewGuid();
	Receipt.RuntimeRevision = 41;
	Receipt.CompleteFootprint = {Footprint(TEXT("asset"), TEXT("a")), Footprint(TEXT("sidecar"), TEXT("b"))};
	Receipt.ReceiptHash = TEXT("sha256:all-phase");
	Receipt.bSucceeded = true;
	return Receipt;
}

struct FTransactionResourceObservation
{
	struct FCreatedDirtyCounts
	{
		int32 Created = 0;
		int32 Dirtied = 0;
	};

	bool HasExplicitAbsentPackageAndSidecarBaselines() const
	{
		return !PackageHeaderBytes.IsSet() && !PackageCompanionBytes.IsSet() && !SidecarBytes.IsSet();
	}

	bool HasExplicitZeroOwnership() const
	{
		return UObjectOwnership.Created == 0 && UObjectOwnership.Dirtied == 0
			&& PackageOwnership.Created == 0 && PackageOwnership.Dirtied == 0
			&& RegistryOwnership.Created == 0 && RegistryOwnership.Dirtied == 0
			&& FileOwnership.Created == 0 && FileOwnership.Dirtied == 0;
	}

	TOptional<TArray<uint8>> PackageHeaderBytes;
	TOptional<TArray<uint8>> PackageCompanionBytes;
	TOptional<TArray<uint8>> SidecarBytes;
	FCreatedDirtyCounts UObjectOwnership;
	FCreatedDirtyCounts PackageOwnership;
	FCreatedDirtyCounts RegistryOwnership;
	FCreatedDirtyCounts FileOwnership;
};

struct FTransactionFixture
{
	explicit FTransactionFixture(bool bRegisterA = true)
		: Handles(&Calls), A(TEXT("a"), &Calls), B(TEXT("b"), &Calls)
		, Factory(Handles, Registry,
			[this](EAssetDocumentRuntimeRequestPhase Phase, const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{
				Diagnostics.Emplace(Phase, Diagnostic);
			})
	{
		if (bRegisterA)
		{
			Registry.Add(A);
		}
		Registry.Add(B);
	}

	TUniquePtr<IAssetDocumentAtomicTransaction> Begin(const FAssetDocumentPreflightReceipt& Receipt)
	{
		return Factory.BeginLiveAfterPreflight(Receipt, Cancellation);
	}

	FCallRecord Calls;
	FHandleResolver Handles;
	FRollbackParticipant A;
	FRollbackParticipant B;
	FParticipantRegistry Registry;
	FCancellationView Cancellation;
	FTransactionResourceObservation Resources;
	TArray<TPair<EAssetDocumentRuntimeRequestPhase, FAssetDocumentRuntimeDiagnostic>> Diagnostics;
	FAssetDocumentAtomicTransactionFactoryRuntime Factory;
};

class FAllPhasePostRollbackFinalizer final : public IAssetDocumentPostRollbackFinalizer
{
public:
	explicit FAllPhasePostRollbackFinalizer(
		FString InComponentId,
		FCallRecord& InCalls,
		bool bInFail = false)
		: ComponentId(MoveTemp(InComponentId))
		, Calls(InCalls)
		, bFail(bInFail)
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return ComponentId;
	}

	virtual FAssetDocumentContractValidationResult FinalizePostRollback(FStringView) override
	{
		++CallCount;
		Calls.Calls.Add(TEXT("PostRollbackFinalizer.a"));
		FAssetDocumentContractValidationResult Result;
		Result.bValid = !bFail;
		return Result;
	}

	int32 CallCount = 0;

private:
	FString ComponentId;
	FCallRecord& Calls;
	bool bFail = false;
};

bool HasAtomicDiagnostic(
	const FTransactionFixture& Fixture,
	EAssetDocumentRuntimeRequestPhase Phase,
	const TCHAR* Code)
{
	return Fixture.Diagnostics.ContainsByPredicate([Phase, Code](const auto& Entry)
	{
		return Entry.Key == Phase && Entry.Value.Path == TEXT("/AtomicTransaction")
			&& Entry.Value.Code == Code && !Entry.Value.Message.IsEmpty();
	});
}

bool HasOnlyAtomicDiagnostics(
	const FTransactionFixture& Fixture,
	EAssetDocumentRuntimeRequestPhase Phase,
	const TCHAR* Code,
	int32 Count)
{
	return Fixture.Diagnostics.Num() == Count
		&& Fixture.Diagnostics.ContainsByPredicate([Phase, Code](const auto& Entry)
		{
			return Entry.Key == Phase && Entry.Value.Path == TEXT("/AtomicTransaction")
				&& Entry.Value.Code == Code && !Entry.Value.Message.IsEmpty();
		});
}

bool HasNoSyncEffects(const FTransactionFixture& Fixture)
{
	return !Fixture.Calls.Calls.ContainsByPredicate([](const FString& Call)
		{
			return Call.Contains(TEXT("Sync"));
		})
		&& !Fixture.Diagnostics.ContainsByPredicate([](const auto& Entry)
		{
			return Entry.Value.Code.StartsWith(TEXT("Sync."));
		});
}

bool CanBeginFreshTransaction(const FAssetDocumentPreflightReceipt& Receipt)
{
	FTransactionFixture Fixture;
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Fixture.Begin(Receipt);
	if (Transaction == nullptr)
	{
		return false;
	}
	Transaction->Rollback(TEXT("Oracle.FreshTransaction"));
	return true;
}

void Rows21And22(FAutomationTestBase& Test)
{
	{
		FTransactionFixture Fixture(false);
		const FAssetDocumentPreflightReceipt Receipt = Preflight();
		Fixture.B.bFailRollback = true;
		TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Fixture.Begin(Receipt);
		TSharedRef<FAllPhasePostRollbackFinalizer> Finalizer =
			MakeShared<FAllPhasePostRollbackFinalizer>(TEXT("a"), Fixture.Calls);
		const bool bEnlisted = Transaction->EnlistPostRollbackFinalizer(Finalizer).bValid;
		Transaction->EnlistFootprint(Receipt.CompleteFootprint);
		const bool bPoisoned = !Transaction->Rollback(TEXT("Row21.OrdinaryFailure")).bValid;
		const bool bRetryRejected = !Transaction->Rollback(TEXT("Row21.Retry")).bValid;
		Test.TestTrue(TEXT("Row21 ordinary failure still invokes the finalizer last"),
			bEnlisted && bPoisoned
			&& Transaction->GetState() == EAssetDocumentAtomicState::Poisoned
			&& Fixture.Calls.Calls == TArray<FString>{
				TEXT("Invalidate.Rollback"), TEXT("Rollback.b"), TEXT("PostRollbackFinalizer.a")});
		Test.TestTrue(TEXT("Row21 preserves ordinary failure and aggregate diagnostics without a finalizer failure"),
			HasAtomicDiagnostic(Fixture, EAssetDocumentRuntimeRequestPhase::Failed,
				TEXT("Atomic.Poisoned.ComponentFailed"))
			&& HasAtomicDiagnostic(Fixture, EAssetDocumentRuntimeRequestPhase::Failed,
				TEXT("Atomic.Poisoned.RollbackFailed"))
			&& !HasAtomicDiagnostic(Fixture, EAssetDocumentRuntimeRequestPhase::Failed,
				TEXT("Atomic.Poisoned.PostRollbackFinalizerFailed")));
		CheckFrozenRowOracle(Test, {21, Observed(HasAtomicDiagnostic(Fixture,
			EAssetDocumentRuntimeRequestPhase::Failed, TEXT("Atomic.Poisoned.ComponentFailed"))),
			Observed(Receipt.RequestId.IsValid()), Observed(bPoisoned),
			NotApplicable(Fixture.Resources.HasExplicitAbsentPackageAndSidecarBaselines()),
			NotApplicable(Fixture.Resources.HasExplicitZeroOwnership()),
			Observed(Fixture.Handles.Invalidations.Num() == 1), NotApplicable(HasNoSyncEffects(Fixture)),
			Observed(bPoisoned), Observed(bRetryRejected),
			Observed(Finalizer->CallCount == 1
				&& Fixture.Calls.Calls.Last() == TEXT("PostRollbackFinalizer.a"))});
	}
	{
		FTransactionFixture Fixture(false);
		const FAssetDocumentPreflightReceipt Receipt = Preflight();
		TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Fixture.Begin(Receipt);
		TSharedRef<FAllPhasePostRollbackFinalizer> Finalizer =
			MakeShared<FAllPhasePostRollbackFinalizer>(TEXT("a"), Fixture.Calls, true);
		Transaction->EnlistPostRollbackFinalizer(Finalizer);
		Transaction->EnlistFootprint(Receipt.CompleteFootprint);
		const bool bPoisoned = !Transaction->Rollback(TEXT("Row22.PostRollbackFinalizerFailure")).bValid;
		Test.TestTrue(TEXT("Row22 finalizer failure poisons after ordinary rollback coverage"),
			bPoisoned && Transaction->GetState() == EAssetDocumentAtomicState::Poisoned
			&& Finalizer->CallCount == 1 && Fixture.Calls.Calls.Last() == TEXT("PostRollbackFinalizer.a")
			&& HasAtomicDiagnostic(Fixture, EAssetDocumentRuntimeRequestPhase::Failed,
				TEXT("Atomic.Poisoned.PostRollbackFinalizerFailed"))
			&& HasAtomicDiagnostic(Fixture, EAssetDocumentRuntimeRequestPhase::Failed,
				TEXT("Atomic.Poisoned.RollbackFailed")));
		CheckFrozenRowOracle(Test, {22,
			Observed(HasAtomicDiagnostic(Fixture, EAssetDocumentRuntimeRequestPhase::Failed,
				TEXT("Atomic.Poisoned.PostRollbackFinalizerFailed"))),
			Observed(Receipt.RequestId.IsValid()), Observed(bPoisoned),
			NotApplicable(Fixture.Resources.HasExplicitAbsentPackageAndSidecarBaselines()),
			NotApplicable(Fixture.Resources.HasExplicitZeroOwnership()),
			Observed(Fixture.Handles.Invalidations.Num() == 1), NotApplicable(HasNoSyncEffects(Fixture)),
			Observed(bPoisoned), Observed(!Transaction->Rollback(TEXT("retry")).bValid),
			Observed(Finalizer->CallCount == 1)});
	}
}

void Rows01To06(FAutomationTestBase& Test)
{
	{
		FTransactionFixture Fixture;
		Fixture.Cancellation.bCancelled = true;
		const FAssetDocumentPreflightReceipt Receipt = Preflight();
		const bool bRejected = Fixture.Begin(Receipt) == nullptr;
		const bool bRetryRejected = Fixture.Begin(Receipt) == nullptr;
		Test.TestTrue(TEXT("Row01 cancelled before begin has no transaction or effects"), bRejected && Fixture.Calls.Calls.IsEmpty());
		CheckFrozenRowOracle(Test, {1,
			Observed(HasOnlyAtomicDiagnostics(Fixture, EAssetDocumentRuntimeRequestPhase::Preflight,
				TEXT("Atomic.CancelledBeforeBegin"), 2)),
			Observed(Receipt.RequestId.IsValid()), NotApplicable(bRejected && bRetryRejected),
			NotApplicable(Fixture.Resources.HasExplicitAbsentPackageAndSidecarBaselines()),
			NotApplicable(Fixture.Resources.HasExplicitZeroOwnership()),
			Observed(Fixture.Handles.Invalidations.IsEmpty()), NotApplicable(HasNoSyncEffects(Fixture)),
			Observed(bRejected), Observed(bRetryRejected),
			Observed(Fixture.Calls.Calls.IsEmpty() && Fixture.Handles.Invalidations.IsEmpty()
				&& Fixture.A.RollbackCount == 0 && Fixture.B.RollbackCount == 0
				&& Fixture.Diagnostics.Num() == 2)});
	}
	{
		FTransactionFixture Fixture;
		const FAssetDocumentPreflightReceipt Receipt = Preflight();
		FGuid TransactionId;
		{ TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Fixture.Begin(Receipt);
		  TransactionId = Transaction->GetTransactionId(); }
		Test.TestTrue(TEXT("Row02 live destructor chooses rollback invalidation first"),
			Fixture.Calls.Calls == TArray<FString>{TEXT("Invalidate.Rollback")});
		const bool bDestructorRolledBack = Fixture.Calls.Calls == TArray<FString>{TEXT("Invalidate.Rollback")}
			&& Fixture.Handles.Invalidations.Num() == 1
			&& Fixture.Handles.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::Rollback;
		CheckFrozenRowOracle(Test, {2, NotApplicable(Fixture.Diagnostics.IsEmpty()),
			Observed(Receipt.RequestId.IsValid() && TransactionId.IsValid()), Observed(bDestructorRolledBack),
			NotApplicable(Fixture.Resources.HasExplicitAbsentPackageAndSidecarBaselines()),
			NotApplicable(Fixture.Resources.HasExplicitZeroOwnership()),
			Observed(Fixture.Handles.Invalidations.Num() == 1
				&& Fixture.Handles.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::Rollback),
			NotApplicable(HasNoSyncEffects(Fixture)), Observed(Fixture.Diagnostics.IsEmpty()),
			Observed(CanBeginFreshTransaction(Receipt)),
			Observed(Fixture.Calls.Calls == TArray<FString>{TEXT("Invalidate.Rollback")}
				&& Fixture.Handles.Invalidations.Num() == 1
				&& Fixture.A.RollbackCount == 0 && Fixture.B.RollbackCount == 0)});
	}
	{
		FTransactionFixture Fixture;
		const FAssetDocumentPreflightReceipt Receipt = Preflight();
		bool bIdentityMatches = false;
		{ TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Fixture.Begin(Receipt);
		  bIdentityMatches = Transaction->GetMutationToken().RequestId == Receipt.RequestId
			&& Transaction->GetTransactionId().IsValid();
		  Transaction->EnlistFootprint(MakeArrayView(&Receipt.CompleteFootprint[0], 1)); }
		Test.TestTrue(TEXT("Row03 first enlist rolls back exact enlisted identity"),
			Fixture.Calls.Calls == TArray<FString>{TEXT("Invalidate.Rollback"), TEXT("Rollback.a")}
			&& Fixture.A.RollbackCount == 1 && Fixture.B.RollbackCount == 0);
		const bool bDestructorRolledBack = Fixture.Calls.Calls
			== TArray<FString>{TEXT("Invalidate.Rollback"), TEXT("Rollback.a")};
		CheckFrozenRowOracle(Test, {3, NotApplicable(Fixture.Diagnostics.IsEmpty()),
			Observed(bIdentityMatches),
			Observed(bDestructorRolledBack),
			NotApplicable(Fixture.Resources.HasExplicitAbsentPackageAndSidecarBaselines()),
			NotApplicable(Fixture.Resources.HasExplicitZeroOwnership()),
			Observed(Fixture.Handles.Invalidations.Num() == 1
				&& Fixture.Handles.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::Rollback),
			NotApplicable(HasNoSyncEffects(Fixture)), Observed(Fixture.Diagnostics.IsEmpty()),
			Observed(CanBeginFreshTransaction(Receipt)),
			Observed(Fixture.Calls.Calls == TArray<FString>{TEXT("Invalidate.Rollback"), TEXT("Rollback.a")}
				&& Fixture.Handles.Invalidations.Num() == 1
				&& Fixture.A.RollbackCount == 1 && Fixture.B.RollbackCount == 0)});
	}
	{
		FTransactionFixture Fixture;
		const FAssetDocumentPreflightReceipt Receipt = Preflight();
		TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Fixture.Begin(Receipt);
		Transaction->EnlistFootprint(Receipt.CompleteFootprint);
		const bool bRolledBack = Transaction->Rollback(TEXT("Apply.ParticipantFailed")).bValid;
		Test.TestTrue(TEXT("Row04 apply failure uses unified reverse rollback"), bRolledBack
			&& Fixture.Calls.Calls == TArray<FString>{TEXT("Invalidate.Rollback"), TEXT("Rollback.b"), TEXT("Rollback.a")});
		CheckFrozenRowOracle(Test, {4, NotApplicable(Fixture.Diagnostics.IsEmpty()),
			Observed(Transaction->GetMutationToken().RequestId == Receipt.RequestId && Transaction->GetTransactionId().IsValid()),
			Observed(Transaction->GetState() == EAssetDocumentAtomicState::RolledBack),
			NotApplicable(Fixture.Resources.HasExplicitAbsentPackageAndSidecarBaselines()),
			NotApplicable(Fixture.Resources.HasExplicitZeroOwnership()),
			Observed(Fixture.Handles.Invalidations.Num() == 1
				&& Fixture.Handles.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::Rollback),
			NotApplicable(HasNoSyncEffects(Fixture)), Observed(Transaction->GetState() != EAssetDocumentAtomicState::Poisoned),
			Observed(CanBeginFreshTransaction(Receipt)), Observed(Fixture.Calls.Calls.Num() == 3)});
	}
	{
		FTransactionFixture Fixture;
		const FAssetDocumentPreflightReceipt Receipt = Preflight();
		TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Fixture.Begin(Receipt);
		Transaction->EnlistFootprint(Receipt.CompleteFootprint);
		FAssetDocumentOpaqueHandle ExpectedHandle;
		ExpectedHandle.RequestId = Receipt.RequestId;
		ExpectedHandle.TransactionId = Transaction->GetTransactionId();
		ExpectedHandle.RuntimeRevision = Receipt.RuntimeRevision;
		ExpectedHandle.ProviderId = TEXT("all-phase");
		ExpectedHandle.Slot = 1;
		ExpectedHandle.Generation = 1;
		Fixture.Handles.NextIssuedHandle = ExpectedHandle;
		FAssetDocumentIssuedHandleResult Issued = Fixture.Handles.Issue(
			EAssetDocumentHandleKind::Object, EAssetDocumentHandleAccess::ReadOnly, {});
		Fixture.Handles.bSucceedNextAcquire = true;
		const bool bHandleResolvedBeforeBoundary = Issued.Handle.IsSet()
			&& Fixture.Handles.AcquireVerifiedLease(Issued.Handle.GetValue(), {}).Lease.IsValid();
		FAssetDocumentHandleInvalidation Compile;
		Compile.Reason = EAssetDocumentHandleInvalidationReason::Compile;
		Compile.RequestId = Receipt.RequestId;
		Fixture.Handles.Invalidate(Compile);
		Transaction->Rollback(TEXT("Repair.CompileFailed"));
		Test.TestTrue(TEXT("Row05 compile invalidation precedes unified rollback"), Fixture.Handles.Invalidations.Num() == 2
			&& Fixture.Handles.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::Compile
			&& Fixture.Handles.Invalidations[1].Reason == EAssetDocumentHandleInvalidationReason::Rollback);
		FAssetDocumentHandleValidationContext Context;
		const bool bOldHandleRejected = Issued.Handle.IsSet()
			&& !Fixture.Handles.AcquireVerifiedLease(Issued.Handle.GetValue(), Context).Lease.IsValid()
			&& Fixture.Handles.LastAcquiredHandle.IsSet()
			&& Fixture.Handles.LastAcquiredHandle->RequestId == Issued.Handle->RequestId
			&& Fixture.Handles.LastAcquiredHandle->TransactionId == Issued.Handle->TransactionId
			&& Fixture.Handles.AcquiredHandles.Num() == 2
			&& Fixture.Handles.AcquiredHandles[0].RequestId == Issued.Handle->RequestId
			&& Fixture.Handles.AcquiredHandles[1].RequestId == Issued.Handle->RequestId
			&& Fixture.Handles.Operations == TArray<FString>{TEXT("Issue"), TEXT("Acquire"),
				TEXT("Invalidate.Compile"), TEXT("Invalidate.TransactionBoundary"), TEXT("Acquire")};
		CheckFrozenRowOracle(Test, {5, NotApplicable(Fixture.Diagnostics.IsEmpty()),
			Observed(Transaction->GetMutationToken().RequestId == Receipt.RequestId && Transaction->GetTransactionId().IsValid()),
			Observed(Transaction->GetState() == EAssetDocumentAtomicState::RolledBack),
			NotApplicable(Fixture.Resources.HasExplicitAbsentPackageAndSidecarBaselines()),
			NotApplicable(Fixture.Resources.HasExplicitZeroOwnership()),
			Observed(Fixture.Handles.Invalidations.Num() == 2
				&& Fixture.Handles.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::Compile
				&& Fixture.Handles.Invalidations[1].Reason == EAssetDocumentHandleInvalidationReason::Rollback
				&& bHandleResolvedBeforeBoundary && bOldHandleRejected),
			NotApplicable(HasNoSyncEffects(Fixture)), Observed(Fixture.Diagnostics.IsEmpty()),
			Observed(CanBeginFreshTransaction(Receipt)),
			Observed(Fixture.Calls.Calls == TArray<FString>{
				TEXT("Invalidate.Rollback"), TEXT("Invalidate.Rollback"),
				TEXT("Rollback.b"), TEXT("Rollback.a")}
				&& Fixture.Handles.Operations == TArray<FString>{TEXT("Issue"), TEXT("Acquire"),
					TEXT("Invalidate.Compile"), TEXT("Invalidate.TransactionBoundary"), TEXT("Acquire")}
				&& Fixture.A.RollbackCount == 1 && Fixture.B.RollbackCount == 1)});
	}
	{
		FTransactionFixture Fixture;
		const FAssetDocumentPreflightReceipt Receipt = Preflight();
		TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Fixture.Begin(Receipt);
		Transaction->EnlistFootprint(Receipt.CompleteFootprint);
		const bool bRolledBack = Transaction->Rollback(TEXT("Atomic.CancelledAtBoundary")).bValid;
		Test.TestTrue(TEXT("Row06 boundary cancellation uses the same rollback terminal"), bRolledBack
			&& Transaction->GetState() == EAssetDocumentAtomicState::RolledBack && Fixture.A.RollbackCount == 1
			&& Fixture.B.RollbackCount == 1);
		CheckFrozenRowOracle(Test, {6,
			Observed(HasOnlyAtomicDiagnostics(Fixture, EAssetDocumentRuntimeRequestPhase::Apply,
				TEXT("Atomic.CancelledAtBoundary"), 1)),
			Observed(Transaction->GetMutationToken().RequestId == Receipt.RequestId && Transaction->GetTransactionId().IsValid()),
			Observed(Transaction->GetState() == EAssetDocumentAtomicState::RolledBack),
			NotApplicable(Fixture.Resources.HasExplicitAbsentPackageAndSidecarBaselines()),
			NotApplicable(Fixture.Resources.HasExplicitZeroOwnership()),
			Observed(Fixture.Handles.Invalidations.Num() == 1
				&& Fixture.Handles.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::Rollback),
			NotApplicable(HasNoSyncEffects(Fixture)), Observed(Transaction->GetState() != EAssetDocumentAtomicState::Poisoned),
			Observed(CanBeginFreshTransaction(Receipt)),
			Observed(Fixture.Calls.Calls == TArray<FString>{
				TEXT("Invalidate.Rollback"), TEXT("Rollback.b"), TEXT("Rollback.a")}
				&& Fixture.Handles.Invalidations.Num() == 1
				&& Fixture.A.RollbackCount == 1 && Fixture.B.RollbackCount == 1
				&& Fixture.Diagnostics.Num() == 1)});
	}
}

class FPackageBackend final : public IAssetDocumentPackageSaveBackend
{
public:
	explicit FPackageBackend(FString InRoot) : Root(MoveTemp(InRoot)) {}
	bool bFail = false;

	virtual FAssetDocumentContractValidationResult SaveToStaging(
		const FString& StagedHeaderPath,
		const FString& TargetHeaderPath,
		FAssetDocumentPackageArtifactSet& OutArtifacts) override
	{
		if (bFail)
		{
			return Invalid(TEXT("Persistence.StagePackageFailed"));
		}
		const TArray<uint8> Header{0x4e, 0x45, 0x57, 0x48};
		const TArray<uint8> Companion{0x4e, 0x45, 0x57, 0x43};
		const FString CompanionStage = FPaths::Combine(FPaths::GetPath(StagedHeaderPath), TEXT("Asset.uexp"));
		Write(StagedHeaderPath, Header);
		Write(CompanionStage, Companion);
		OutArtifacts.Artifacts = {
			{StagedHeaderPath, FPaths::Combine(Root, TEXT("Installed/Asset.uasset")), ComputeAssetDocumentContentDigest(Header)},
			{CompanionStage, FPaths::Combine(Root, TEXT("Installed/Asset.uexp")), ComputeAssetDocumentContentDigest(Companion)}};
		return Valid();
	}
private:
	FString Root;
};

enum class EFailurePoint : uint8
{
	None, StagePackage, StageSidecar, Fresh, Canonical, Prepare, HeaderInstall,
	CompanionInstall, SidecarInstall, SyncAdvance, DurableMark, Cleanup, RecoveryCleanup
};

class FVerifier final : public IAssetDocumentStagedArtifactVerifier
{
public:
	EFailurePoint Point = EFailurePoint::None;
	virtual FAssetDocumentFreshVerificationResult VerifyFresh(const FAssetDocumentFreshVerificationInput& Input) override
	{
		FAssetDocumentFreshVerificationResult Result;
		Result.bFreshLoadProven = Point != EFailurePoint::Fresh;
		Result.bCanonicalMatch = Point != EFailurePoint::Canonical;
		Result.ActualSemanticHash = Input.ExpectedSemanticHash;
		Result.ActualCodecVersion = Input.ExpectedCodecVersion;
		return Result;
	}
};

class FHandleRecorder final : public IAssetDocumentHandleResolver
{
public:
	virtual FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override { return {}; }
	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) override { return {}; }
	virtual FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation& Event) override
	{
		Invalidations.Add(Event);
		return {};
	}
	TArray<FAssetDocumentHandleInvalidation> Invalidations;
};

struct FPersistenceOutcome
{
	FAssetDocumentDurableCompletionReceipt Receipt;
	TArray<FString> Calls;
	TArray<FAssetDocumentPersistenceFailure> Failures;
	TArray<FAssetDocumentHandleInvalidation> Invalidations;
	TArray<uint8> Header;
	TArray<uint8> Companion;
	TArray<uint8> Sidecar;
	TArray<uint8> SyncBytes;
	TArray<uint8> BeforeSyncBytes;
	TArray<uint8> PreStageHeader;
	TArray<uint8> PreStageCompanion;
	TArray<uint8> PreStageSidecar;
	FAssetDocumentPersistencePlan Plan;
	TOptional<EAssetDocumentJournalState> SyncStateAtStore;
	bool bObjectLoaded = false;
	bool bPackageLoaded = false;
	bool bRegistryOwned = false;
	bool bHeaderExists = false;
	bool bCompanionExists = false;
	bool bSidecarExists = false;
	bool bStagedHeaderExists = false;
	bool bStagedCompanionExists = false;
	bool bStagedSidecarExists = false;
};

class FJournalStateObservingStore final : public IAssetDocumentTypedSyncStateStore
{
public:
	FJournalStateObservingStore(
		IAssetDocumentTypedSyncStateStore& InInner,
		FAssetDocumentPersistenceJournal& InJournal,
		FPersistenceOutcome& InOutcome)
		: Inner(InInner), Journal(InJournal), Outcome(InOutcome)
	{
	}

	virtual FAssetDocumentContractValidationResult Load(
		const FAssetDocumentSyncStateKey& Key,
		TOptional<FAssetDocumentTypedSyncStateRecord>& OutRecord,
		TArray<uint8>& OutEncodedBytes,
		bool& bOutExisted) override
	{
		return Inner.Load(Key, OutRecord, OutEncodedBytes, bOutExisted);
	}

	virtual FAssetDocumentContractValidationResult Store(
		const FAssetDocumentSyncStateKey& Key,
		const FAssetDocumentTypedSyncStateRecord& Record) override
	{
		Outcome.SyncStateAtStore = Journal.GetCurrentStateForTest();
		return Inner.Store(Key, Record);
	}

	virtual FAssetDocumentContractValidationResult Restore(
		const FAssetDocumentSyncStateKey& Key,
		TArrayView64<const uint8> EncodedPreimage,
		bool bExisted) override
	{
		return Inner.Restore(Key, EncodedPreimage, bExisted);
	}

private:
	IAssetDocumentTypedSyncStateStore& Inner;
	FAssetDocumentPersistenceJournal& Journal;
	FPersistenceOutcome& Outcome;
};

FAssetDocumentRegionEvidence Evidence(const TCHAR* Hash)
{
	FAssetDocumentRegionEvidence Result;
	Result.Kind = EAssetDocumentEvidenceKind::Present;
	Result.SemanticHash = Hash;
	Result.CodecVersion = 1;
	return Result;
}

FAssetDocumentPersistencePlan PersistencePlan(const FString& Root)
{
	FAssetDocumentPersistencePlan Plan;
	Plan.RequestId = FGuid::NewGuid();
	Plan.TransactionId = FGuid::NewGuid();
	// Under the test's one content mount (AssetDocumentAtomicTestMount.h: no registry scan races
	// the placeholder packages).
	Plan.TargetPackagePath = FAssetDocumentAtomicTestMount::PackageNameFor(Root);
	Plan.TargetSidecarPath = FPaths::Combine(Root, TEXT("Installed/Asset.assetdoc"));
	Plan.StagingRoot = FPaths::Combine(Root, TEXT("Staging"));
	Plan.JournalId = TEXT("all-phase");
	Plan.Sidecar.Utf8Bytes = {0x4e, 0x45, 0x57, 0x53};
	Plan.Sidecar.SemanticHash = TEXT("sidecar:new");
	Plan.Sidecar.CodecVersion = 1;
	Plan.Footprint = {Footprint(TEXT("asset"), TEXT("a")), Footprint(TEXT("sidecar"), TEXT("b"))};
	return Plan;
}

bool InjectAtEvent(EFailurePoint Point, FStringView Event)
{
	const bool bWriteFailure = (Point == EFailurePoint::Prepare && Event == TEXT("PrepareJournal"))
		|| (Point == EFailurePoint::HeaderInstall && Event == TEXT("InstallPackageHeader"))
		|| (Point == EFailurePoint::CompanionInstall && Event == TEXT("InstallPackageCompanions"))
		|| (Point == EFailurePoint::SidecarInstall && Event == TEXT("InstallSidecar"))
		|| (Point == EFailurePoint::SyncAdvance && Event == TEXT("AdvanceSync"))
		|| (Point == EFailurePoint::DurableMark && Event == TEXT("MarkDurableComplete"));
	if (bWriteFailure)
	{
		FAssetDocumentAtomicFile::FailNextWriteAtForTest(EAssetDocumentAtomicFileFailurePoint::Flush);
		return true;
	}
	if (Point == EFailurePoint::Cleanup && Event == TEXT("Cleanup"))
	{
		FAssetDocumentPersistenceJournal::FailNextCleanupForTest();
		return true;
	}
	return false;
}

FPersistenceOutcome RunPersistence(const FString& Root, EFailurePoint Point, bool bInitializeBaseline = true)
{
	FPersistenceOutcome Out;
	const FString InstalledRoot = FPaths::Combine(Root, TEXT("Installed"));
	const FString JournalRoot = FPaths::Combine(Root, TEXT("Journal"));
	const FString StateRoot = FPaths::Combine(Root, TEXT("Sync"));
	const FString BackupRoot = FPaths::Combine(JournalRoot, TEXT("Backups"));
	IFileManager::Get().MakeDirectory(*InstalledRoot, true);
	if (bInitializeBaseline)
	{
		Write(FPaths::Combine(InstalledRoot, TEXT("Asset.uasset")), TArray<uint8>{0x4f, 0x4c, 0x44, 0x48});
		Write(FPaths::Combine(InstalledRoot, TEXT("Asset.assetdoc")), TArray<uint8>{0x4f, 0x4c, 0x44, 0x53});
	}
	const FAssetDocumentSyncStateKey SyncKey{TEXT("/Game/Atomic/Asset"), TEXT("Body")};
	FAssetDocumentJournaledTypedSyncStateStore Store(StateRoot);
	if (bInitializeBaseline)
	{
		FAssetDocumentTypedSyncStateRecord Baseline;
		Baseline.LastDurable.Asset = Evidence(TEXT("asset:old"));
		Baseline.LastDurable.Sidecar = Evidence(TEXT("sidecar:old"));
		Baseline.RequestId = FGuid::NewGuid();
		Baseline.TransactionId = FGuid::NewGuid();
		Baseline.JournalId = TEXT("baseline");
		Store.Store(SyncKey, Baseline);
	}
	{
		TOptional<FAssetDocumentTypedSyncStateRecord> BeforeRecord;
		bool bBeforeExisted = false;
		Store.Load(SyncKey, BeforeRecord, Out.BeforeSyncBytes, bBeforeExisted);
	}
	FAssetDocumentPersistenceJournal Journal(JournalRoot,
		[&Store, SyncKey](TArrayView64<const uint8> Bytes, bool bExisted) { return Store.Restore(SyncKey, Bytes, bExisted); },
		[&Store, SyncKey](const FGuid& RequestId, const FGuid& TransactionId, FStringView JournalId)
		{
			TOptional<FAssetDocumentTypedSyncStateRecord> Record;
			TArray<uint8> Bytes;
			bool bExisted = false;
			const FAssetDocumentContractValidationResult Loaded = Store.Load(SyncKey, Record, Bytes, bExisted);
			return Loaded.bValid && bExisted && Record.IsSet() && Record->RequestId == RequestId
				&& Record->TransactionId == TransactionId && Record->JournalId == JournalId
				? Valid() : Invalid(TEXT("Atomic.Poisoned.RecoveryFailed"));
		});
	FJournalStateObservingStore ObservingStore(Store, Journal, Out);
	FPackageBackend Backend(Root);
	Backend.bFail = Point == EFailurePoint::StagePackage;
	FAssetDocumentPackageStager PackageStager(Backend);
	FAssetDocumentSidecarByteIo SidecarIo;
	FAssetDocumentSyncCoordinatorRuntime Sync(SyncKey, ObservingStore);
	FHandleRecorder Handles;
	FVerifier Verifier;
	Verifier.Point = Point;
	FAssetDocumentSyncEvidencePair Installed{Evidence(TEXT("asset:new")), Evidence(TEXT("sidecar:new"))};
	bool bEventInjectionConsumed = false;
	Out.Plan = PersistencePlan(Root);
	FAssetDocumentPersistenceDependencies Dependencies{
		PackageStager, SidecarIo, Journal, ObservingStore, Sync, Handles, SyncKey, Installed, BackupRoot, 43,
		Out.Plan.RequestId, Out.Plan.TransactionId, Out.Plan.JournalId, Out.Plan.StagingRoot,
		Out.Plan.TargetPackagePath, Out.Plan.TargetSidecarPath,
		[&Out, &bEventInjectionConsumed, Point, InstalledRoot](FStringView Event)
		{
			Out.Calls.Add(FString(Event));
			if (Event == TEXT("StagePackage"))
			{
				Out.PreStageHeader = Read(FPaths::Combine(InstalledRoot, TEXT("Asset.uasset")));
				Out.PreStageCompanion = Read(FPaths::Combine(InstalledRoot, TEXT("Asset.uexp")));
				Out.PreStageSidecar = Read(FPaths::Combine(InstalledRoot, TEXT("Asset.assetdoc")));
			}
			if (!bEventInjectionConsumed)
			{
				bEventInjectionConsumed = InjectAtEvent(Point, Event);
			}
		},
		[&Out](EAssetDocumentPersistenceStep Step, const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{ Out.Failures.Add({Step, Diagnostic}); }};
	FAssetDocumentPersistenceCoordinatorRuntime Runtime(MoveTemp(Dependencies));
	if (Point == EFailurePoint::StageSidecar)
	{
		FAssetDocumentAtomicFile::FailNextWriteAtForTest(EAssetDocumentAtomicFileFailurePoint::Flush);
	}
	if (Point == EFailurePoint::RecoveryCleanup)
	{
		FAssetDocumentPersistenceJournal::FailNextCleanupForTest();
	}
	Out.Receipt = Runtime.Persist(Out.Plan, Verifier);
	Out.Invalidations = Handles.Invalidations;
	Out.Header = Read(FPaths::Combine(InstalledRoot, TEXT("Asset.uasset")));
	Out.Companion = Read(FPaths::Combine(InstalledRoot, TEXT("Asset.uexp")));
	Out.Sidecar = Read(FPaths::Combine(InstalledRoot, TEXT("Asset.assetdoc")));
	const FString ObjectPath = Out.Plan.TargetPackagePath + TEXT(".Asset");
	Out.bObjectLoaded = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath) != nullptr;
	Out.bPackageLoaded = FindPackage(nullptr, *Out.Plan.TargetPackagePath) != nullptr;
	Out.bRegistryOwned = FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(
		FSoftObjectPath(ObjectPath)).IsValid();
	Out.bHeaderExists = IFileManager::Get().FileExists(*FPaths::Combine(InstalledRoot, TEXT("Asset.uasset")));
	Out.bCompanionExists = IFileManager::Get().FileExists(*FPaths::Combine(InstalledRoot, TEXT("Asset.uexp")));
	Out.bSidecarExists = IFileManager::Get().FileExists(*FPaths::Combine(InstalledRoot, TEXT("Asset.assetdoc")));
	Out.bStagedHeaderExists = IFileManager::Get().FileExists(*FPaths::Combine(Out.Plan.StagingRoot, TEXT("Asset.uasset")));
	Out.bStagedCompanionExists = IFileManager::Get().FileExists(*FPaths::Combine(Out.Plan.StagingRoot, TEXT("Asset.uexp")));
	Out.bStagedSidecarExists = IFileManager::Get().FileExists(*FPaths::Combine(Out.Plan.StagingRoot, TEXT("Asset.assetdoc")));
	TOptional<FAssetDocumentTypedSyncStateRecord> Record;
	bool bExisted = false;
	Store.Load(SyncKey, Record, Out.SyncBytes, bExisted);
	return Out;
}

bool IsExactBaseline(const FPersistenceOutcome& Out)
{
	return Out.Header == TArray<uint8>{0x4f, 0x4c, 0x44, 0x48}
		&& Out.Companion.IsEmpty() && Out.Sidecar == TArray<uint8>{0x4f, 0x4c, 0x44, 0x53};
}

bool HasCode(const FPersistenceOutcome& Out, const TCHAR* Code)
{
	return Out.Failures.ContainsByPredicate([Code](const FAssetDocumentPersistenceFailure& Failure)
	{
		return Failure.Diagnostic.Code == Code;
	});
}

bool HasExactFailure(
	const FPersistenceOutcome& Out,
	EAssetDocumentPersistenceStep Step,
	const TCHAR* Code)
{
	return Out.Failures.ContainsByPredicate([Step, Code](const FAssetDocumentPersistenceFailure& Failure)
	{
		return Failure.Step == Step && Failure.Diagnostic.Code == Code
			&& !Failure.Diagnostic.Path.IsEmpty() && !Failure.Diagnostic.Message.IsEmpty();
	});
}

bool HasBoundIdentity(const FPersistenceOutcome& Out)
{
	if (!Out.Plan.RequestId.IsValid() || !Out.Plan.TransactionId.IsValid())
	{
		return false;
	}
	if (!Out.Receipt.RequestId.IsValid())
	{
		return !Out.Receipt.TransactionId.IsValid();
	}
	return Out.Receipt.RequestId == Out.Plan.RequestId && Out.Receipt.TransactionId == Out.Plan.TransactionId;
}

bool HasNoStagedOwnershipLeak(const FPersistenceOutcome& Out)
{
	return !Out.bStagedHeaderExists && !Out.bStagedCompanionExists && !Out.bStagedSidecarExists;
}

bool HasBaselineOwnershipLedger(const FPersistenceOutcome& Out)
{
	return !Out.bObjectLoaded && !Out.bPackageLoaded && !Out.bRegistryOwned
		&& Out.bHeaderExists && !Out.bCompanionExists && Out.bSidecarExists
		&& HasNoStagedOwnershipLeak(Out);
}

bool HasCommittedOwnershipLedger(const FPersistenceOutcome& Out)
{
	return !Out.bObjectLoaded && !Out.bPackageLoaded && !Out.bRegistryOwned
		&& Out.bHeaderExists && Out.bCompanionExists && Out.bSidecarExists
		&& HasNoStagedOwnershipLeak(Out);
}

bool HasOnlyReloadInvalidation(const FPersistenceOutcome& Out)
{
	return Out.Invalidations.Num() == 1
		&& Out.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::PackageReload
		&& Out.Invalidations[0].RequestId == Out.Plan.RequestId
		&& Out.Invalidations[0].TransactionId == Out.Plan.TransactionId;
}

bool HasUnchangedSync(const FPersistenceOutcome& Out)
{
	return !Out.BeforeSyncBytes.IsEmpty() && Out.SyncBytes == Out.BeforeSyncBytes;
}

bool HasNoAtomicTransactionEffects(const FPersistenceOutcome& Out)
{
	return !Out.Receipt.RequestId.IsValid() && !Out.Receipt.TransactionId.IsValid()
		&& !Out.Invalidations.ContainsByPredicate([](const FAssetDocumentHandleInvalidation& Event)
		{
			return Event.Reason == EAssetDocumentHandleInvalidationReason::Commit
				|| Event.Reason == EAssetDocumentHandleInvalidationReason::Rollback;
		});
}

bool HasNoLiveTransactionTerminalEffects(const FPersistenceOutcome& Out)
{
	return !Out.Calls.ContainsByPredicate([](const FString& Call)
		{
			return Call == TEXT("BeginTransaction")
				|| Call == TEXT("CommitTransaction")
				|| Call == TEXT("RollbackTransaction");
		})
		&& !Out.Invalidations.ContainsByPredicate([](const FAssetDocumentHandleInvalidation& Event)
		{
			return Event.Reason == EAssetDocumentHandleInvalidationReason::Commit
				|| Event.Reason == EAssetDocumentHandleInvalidationReason::Rollback;
		});
}

bool HasSingleFailure(
	const FPersistenceOutcome& Out,
	EAssetDocumentPersistenceStep Step,
	const TCHAR* Code)
{
	return Out.Failures.Num() == 1 && HasExactFailure(Out, Step, Code);
}

bool HasFailureThenRecovery(
	const FPersistenceOutcome& Out,
	EAssetDocumentPersistenceStep Step,
	const TCHAR* Code)
{
	return Out.Failures.Num() == 2
		&& Out.Failures[0].Step == Step
		&& Out.Failures[0].Diagnostic.Code == Code
		&& !Out.Failures[0].Diagnostic.Path.IsEmpty()
		&& !Out.Failures[0].Diagnostic.Message.IsEmpty()
		&& Out.Failures[1].Step == EAssetDocumentPersistenceStep::Recover
		&& Out.Failures[1].Diagnostic.Code == TEXT("Persistence.Recovered")
		&& !Out.Failures[1].Diagnostic.Path.IsEmpty()
		&& !Out.Failures[1].Diagnostic.Message.IsEmpty();
}

bool HasExactSuccessfulPersistenceCalls(const FPersistenceOutcome& Out)
{
	return Out.Calls == TArray<FString>{
		TEXT("RecoverIncomplete"), TEXT("StagePackage"), TEXT("StageSidecar"),
		TEXT("InvalidatePackageReload"), TEXT("VerifyFresh"), TEXT("VerifyCanonical"),
		TEXT("CapturePackageInstalledBaseline"), TEXT("CaptureSidecarInstalledBaseline"),
		TEXT("LoadTypedSyncBaseline"), TEXT("PrepareJournal"), TEXT("InstallPackageHeader"),
		TEXT("InstallPackageCompanions"), TEXT("InstallSidecar"), TEXT("AdvanceSync"),
		TEXT("MarkDurableComplete"), TEXT("Cleanup")};
}

void Rows07To15(FAutomationTestBase& Test, const FString& Root)
{
	const FString Row07Root = FPaths::Combine(Root, TEXT("07"));
	const FPersistenceOutcome Row07 = RunPersistence(Row07Root, EFailurePoint::StagePackage);
	const FPersistenceOutcome Row07Retry = RunPersistence(Row07Root, EFailurePoint::None, false);
	Test.TestTrue(TEXT("Row07 package stage failure preserves exact baseline and sync"),
		!Row07.Receipt.RequestId.IsValid() && IsExactBaseline(Row07) && HasUnchangedSync(Row07)
		&& HasCode(Row07, TEXT("Persistence.StagePackageFailed")));
	CheckFrozenRowOracle(Test, {7,
		Observed(HasSingleFailure(Row07, EAssetDocumentPersistenceStep::StagePackage, TEXT("Persistence.StagePackageFailed"))),
		Observed(HasBoundIdentity(Row07)), NotApplicable(HasNoAtomicTransactionEffects(Row07)), Observed(IsExactBaseline(Row07)),
		Observed(HasBaselineOwnershipLedger(Row07)), NotApplicable(Row07.Invalidations.IsEmpty()),
		Observed(HasUnchangedSync(Row07)), Observed(!HasCode(Row07, TEXT("Atomic.Poisoned.RecoveryFailed"))),
		Observed(Row07Retry.Receipt.bSyncStateAdvanced && HasExactSuccessfulPersistenceCalls(Row07Retry)),
		Observed(Row07.Calls == TArray<FString>{TEXT("RecoverIncomplete"), TEXT("StagePackage")}
			&& HasExactSuccessfulPersistenceCalls(Row07Retry))});
	const FString Row08Root = FPaths::Combine(Root, TEXT("08"));
	const FPersistenceOutcome Row08 = RunPersistence(Row08Root, EFailurePoint::StageSidecar);
	const FPersistenceOutcome Row08Retry = RunPersistence(Row08Root, EFailurePoint::None, false);
	Test.TestTrue(TEXT("Row08 sidecar stage failure cleans staging and preserves target"),
		!Row08.Receipt.RequestId.IsValid() && IsExactBaseline(Row08)
		&& HasUnchangedSync(Row08)
		&& !IFileManager::Get().FileExists(*FPaths::Combine(Row08Root, TEXT("Staging/Asset.uasset")))
		&& !IFileManager::Get().FileExists(*FPaths::Combine(Row08Root, TEXT("Staging/Asset.uexp")))
		&& HasCode(Row08, TEXT("Persistence.StageSidecarFailed")));
	CheckFrozenRowOracle(Test, {8,
		Observed(HasSingleFailure(Row08, EAssetDocumentPersistenceStep::StageSidecar, TEXT("Persistence.StageSidecarFailed"))),
		Observed(HasBoundIdentity(Row08)), NotApplicable(HasNoAtomicTransactionEffects(Row08)), Observed(IsExactBaseline(Row08)),
		Observed(HasBaselineOwnershipLedger(Row08)), NotApplicable(Row08.Invalidations.IsEmpty()),
		Observed(HasUnchangedSync(Row08)), Observed(!HasCode(Row08, TEXT("Atomic.Poisoned.RecoveryFailed"))),
		Observed(Row08Retry.Receipt.bSyncStateAdvanced && HasExactSuccessfulPersistenceCalls(Row08Retry)),
		Observed(Row08.Calls == TArray<FString>{
			TEXT("RecoverIncomplete"), TEXT("StagePackage"), TEXT("StageSidecar")}
			&& HasExactSuccessfulPersistenceCalls(Row08Retry))});
	const FString Row09Root = FPaths::Combine(Root, TEXT("09"));
	const FPersistenceOutcome Row09 = RunPersistence(Row09Root, EFailurePoint::Fresh);
	const FPersistenceOutcome Row09Retry = RunPersistence(Row09Root, EFailurePoint::None, false);
	Test.TestTrue(TEXT("Row09 fresh failure invalidates reload before verifier and has no journal"),
		Row09.Calls.Last() == TEXT("VerifyCanonical") && IsExactBaseline(Row09)
		&& HasUnchangedSync(Row09) && Row09.Invalidations.Num() == 1
		&& Row09.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::PackageReload
		&& HasCode(Row09, TEXT("Persistence.FreshLoadFailed")));
	CheckFrozenRowOracle(Test, {9,
		Observed(HasSingleFailure(Row09, EAssetDocumentPersistenceStep::FreshLoadStagedPackage, TEXT("Persistence.FreshLoadFailed"))),
		Observed(HasBoundIdentity(Row09)), NotApplicable(HasNoAtomicTransactionEffects(Row09)), Observed(IsExactBaseline(Row09)),
		Observed(HasBaselineOwnershipLedger(Row09)), Observed(HasOnlyReloadInvalidation(Row09)),
		Observed(HasUnchangedSync(Row09)), Observed(!HasCode(Row09, TEXT("Atomic.Poisoned.RecoveryFailed"))),
		Observed(Row09Retry.Receipt.bSyncStateAdvanced && HasExactSuccessfulPersistenceCalls(Row09Retry)),
		Observed(Row09.Calls == TArray<FString>{TEXT("RecoverIncomplete"), TEXT("StagePackage"), TEXT("StageSidecar"),
			TEXT("InvalidatePackageReload"), TEXT("VerifyFresh"), TEXT("VerifyCanonical")}
			&& HasExactSuccessfulPersistenceCalls(Row09Retry))});
	const FString Row10Root = FPaths::Combine(Root, TEXT("10"));
	const FPersistenceOutcome Row10 = RunPersistence(Row10Root, EFailurePoint::Canonical);
	const FPersistenceOutcome Row10Retry = RunPersistence(Row10Root, EFailurePoint::None, false);
	Test.TestTrue(TEXT("Row10 canonical mismatch has zero prepare install or sync"),
		Row10.Calls.Last() == TEXT("VerifyCanonical") && IsExactBaseline(Row10) && HasUnchangedSync(Row10)
		&& HasCode(Row10, TEXT("Persistence.CanonicalMismatch")));
	CheckFrozenRowOracle(Test, {10,
		Observed(HasSingleFailure(Row10, EAssetDocumentPersistenceStep::FreshLoadStagedPackage, TEXT("Persistence.CanonicalMismatch"))),
		Observed(HasBoundIdentity(Row10)), NotApplicable(HasNoAtomicTransactionEffects(Row10)), Observed(IsExactBaseline(Row10)),
		Observed(HasBaselineOwnershipLedger(Row10)), Observed(HasOnlyReloadInvalidation(Row10)),
		Observed(HasUnchangedSync(Row10)), Observed(!HasCode(Row10, TEXT("Atomic.Poisoned.RecoveryFailed"))),
		Observed(Row10Retry.Receipt.bSyncStateAdvanced && HasExactSuccessfulPersistenceCalls(Row10Retry)),
		Observed(Row10.Calls == TArray<FString>{TEXT("RecoverIncomplete"), TEXT("StagePackage"), TEXT("StageSidecar"),
			TEXT("InvalidatePackageReload"), TEXT("VerifyFresh"), TEXT("VerifyCanonical")}
			&& HasExactSuccessfulPersistenceCalls(Row10Retry))});
	const FString Row11Root = FPaths::Combine(Root, TEXT("11"));
	const FPersistenceOutcome Row11 = RunPersistence(Row11Root, EFailurePoint::Prepare);
	const FPersistenceOutcome Row11Retry = RunPersistence(Row11Root, EFailurePoint::None, false);
	Test.TestTrue(TEXT("Row11 prepare failure preserves target and has no implicit retry"),
		IsExactBaseline(Row11) && HasUnchangedSync(Row11) && HasCode(Row11, TEXT("Persistence.PrepareJournalFailed"))
		&& Row11Retry.Receipt.bSyncStateAdvanced);
	CheckFrozenRowOracle(Test, {11,
		Observed(HasSingleFailure(Row11, EAssetDocumentPersistenceStep::PrepareJournal, TEXT("Persistence.PrepareJournalFailed"))),
		Observed(HasBoundIdentity(Row11)), NotApplicable(HasNoAtomicTransactionEffects(Row11)), Observed(IsExactBaseline(Row11)),
		Observed(HasBaselineOwnershipLedger(Row11)), Observed(HasOnlyReloadInvalidation(Row11)),
		Observed(HasUnchangedSync(Row11)), Observed(!HasCode(Row11, TEXT("Atomic.Poisoned.RecoveryFailed"))),
		Observed(Row11Retry.Receipt.bSyncStateAdvanced && HasExactSuccessfulPersistenceCalls(Row11Retry)),
		Observed(Row11.Calls == TArray<FString>{TEXT("RecoverIncomplete"), TEXT("StagePackage"), TEXT("StageSidecar"),
			TEXT("InvalidatePackageReload"), TEXT("VerifyFresh"), TEXT("VerifyCanonical"),
			TEXT("CapturePackageInstalledBaseline"), TEXT("CaptureSidecarInstalledBaseline"),
			TEXT("LoadTypedSyncBaseline"), TEXT("PrepareJournal")}
			&& HasExactSuccessfulPersistenceCalls(Row11Retry))});
	for (const TPair<int32, EFailurePoint>& Case : TArray<TPair<int32, EFailurePoint>>{
		{12, EFailurePoint::HeaderInstall}, {13, EFailurePoint::CompanionInstall}, {14, EFailurePoint::SidecarInstall}})
	{
		const FString CaseRoot = FPaths::Combine(Root, FString::FromInt(Case.Key));
		const FPersistenceOutcome Out = RunPersistence(CaseRoot, Case.Value);
		const FPersistenceOutcome Retry = RunPersistence(CaseRoot, EFailurePoint::None, false);
		Test.TestTrue(*FString::Printf(TEXT("Row%02d install failure restores all exact baselines"), Case.Key),
			!Out.Receipt.RequestId.IsValid() && IsExactBaseline(Out) && HasUnchangedSync(Out));
		CheckFrozenRowOracle(Test, {Case.Key,
			Observed(HasFailureThenRecovery(Out, EAssetDocumentPersistenceStep::AtomicInstall,
				TEXT("Persistence.AtomicInstallFailed"))),
			Observed(HasBoundIdentity(Out)), NotApplicable(HasNoAtomicTransactionEffects(Out)), Observed(IsExactBaseline(Out)),
			Observed(HasBaselineOwnershipLedger(Out)), Observed(HasOnlyReloadInvalidation(Out)),
			Observed(HasUnchangedSync(Out)), Observed(!HasCode(Out, TEXT("Atomic.Poisoned.RecoveryFailed"))),
			Observed(Retry.Receipt.bSyncStateAdvanced && HasExactSuccessfulPersistenceCalls(Retry)),
			Observed(Out.Calls == (Case.Key < 14
				? TArray<FString>{TEXT("RecoverIncomplete"), TEXT("StagePackage"), TEXT("StageSidecar"),
					TEXT("InvalidatePackageReload"), TEXT("VerifyFresh"), TEXT("VerifyCanonical"),
					TEXT("CapturePackageInstalledBaseline"), TEXT("CaptureSidecarInstalledBaseline"),
					TEXT("LoadTypedSyncBaseline"), TEXT("PrepareJournal"), TEXT("InstallPackageHeader"),
					TEXT("InstallPackageCompanions")}
				: TArray<FString>{TEXT("RecoverIncomplete"), TEXT("StagePackage"), TEXT("StageSidecar"),
					TEXT("InvalidatePackageReload"), TEXT("VerifyFresh"), TEXT("VerifyCanonical"),
					TEXT("CapturePackageInstalledBaseline"), TEXT("CaptureSidecarInstalledBaseline"),
					TEXT("LoadTypedSyncBaseline"), TEXT("PrepareJournal"), TEXT("InstallPackageHeader"),
					TEXT("InstallPackageCompanions"), TEXT("InstallSidecar")})
				&& HasExactSuccessfulPersistenceCalls(Retry))});
	}
	const FString Row15Root = FPaths::Combine(Root, TEXT("15"));
	const FPersistenceOutcome Row15 = RunPersistence(Row15Root, EFailurePoint::SyncAdvance);
	const FPersistenceOutcome Row15Retry = RunPersistence(Row15Root, EFailurePoint::None, false);
	Test.TestTrue(TEXT("Row15 typed sync advance failure recovers package sidecar and typed preimage"),
		!Row15.Receipt.RequestId.IsValid() && IsExactBaseline(Row15) && HasUnchangedSync(Row15)
		&& HasCode(Row15, TEXT("Sync.StateAdvanceFailed")));
	CheckFrozenRowOracle(Test, {15,
		Observed(HasFailureThenRecovery(Row15, EAssetDocumentPersistenceStep::AtomicInstall, TEXT("Sync.StateAdvanceFailed"))),
		Observed(HasBoundIdentity(Row15)), NotApplicable(HasNoAtomicTransactionEffects(Row15)), Observed(IsExactBaseline(Row15)),
		Observed(HasBaselineOwnershipLedger(Row15)), Observed(HasOnlyReloadInvalidation(Row15)),
		Observed(HasUnchangedSync(Row15) && Row15.SyncStateAtStore.IsSet()
			&& Row15.SyncStateAtStore.GetValue() == EAssetDocumentJournalState::SyncInstalled),
		Observed(!HasCode(Row15, TEXT("Atomic.Poisoned.RecoveryFailed"))),
		Observed(Row15Retry.Receipt.bSyncStateAdvanced && HasExactSuccessfulPersistenceCalls(Row15Retry)),
		Observed(Row15.Calls == TArray<FString>{TEXT("RecoverIncomplete"), TEXT("StagePackage"), TEXT("StageSidecar"),
			TEXT("InvalidatePackageReload"), TEXT("VerifyFresh"), TEXT("VerifyCanonical"),
			TEXT("CapturePackageInstalledBaseline"), TEXT("CaptureSidecarInstalledBaseline"),
			TEXT("LoadTypedSyncBaseline"), TEXT("PrepareJournal"), TEXT("InstallPackageHeader"),
			TEXT("InstallPackageCompanions"), TEXT("InstallSidecar"), TEXT("AdvanceSync")}
			&& HasExactSuccessfulPersistenceCalls(Row15Retry))});
}

void Row16(FAutomationTestBase& Test, const FString& Root)
{
	const FString PreCommitRoot = FPaths::Combine(Root, TEXT("16A"));
	const FPersistenceOutcome PreCommit = RunPersistence(PreCommitRoot, EFailurePoint::DurableMark);
	const FPersistenceOutcome PreCommitRetry = RunPersistence(PreCommitRoot, EFailurePoint::None, false);
	Test.TestTrue(TEXT("Row16A durable mark failure restores sync and returns invalid receipt"),
		!PreCommit.Receipt.RequestId.IsValid() && IsExactBaseline(PreCommit) && HasUnchangedSync(PreCommit));
	CheckFrozenRowOracle(Test, {16,
		Observed(HasFailureThenRecovery(PreCommit, EAssetDocumentPersistenceStep::DurableComplete,
			TEXT("Persistence.PrepareJournalFailed"))),
		Observed(HasBoundIdentity(PreCommit)), NotApplicable(HasNoAtomicTransactionEffects(PreCommit)),
		Observed(IsExactBaseline(PreCommit)),
		Observed(HasBaselineOwnershipLedger(PreCommit)), Observed(HasOnlyReloadInvalidation(PreCommit)),
		Observed(HasUnchangedSync(PreCommit)), Observed(!HasCode(PreCommit, TEXT("Atomic.Poisoned.RecoveryFailed"))),
		Observed(PreCommitRetry.Receipt.bSyncStateAdvanced && HasExactSuccessfulPersistenceCalls(PreCommitRetry)),
		Observed(PreCommit.Calls == TArray<FString>{TEXT("RecoverIncomplete"), TEXT("StagePackage"), TEXT("StageSidecar"),
			TEXT("InvalidatePackageReload"), TEXT("VerifyFresh"), TEXT("VerifyCanonical"),
			TEXT("CapturePackageInstalledBaseline"), TEXT("CaptureSidecarInstalledBaseline"),
			TEXT("LoadTypedSyncBaseline"), TEXT("PrepareJournal"), TEXT("InstallPackageHeader"),
			TEXT("InstallPackageCompanions"), TEXT("InstallSidecar"), TEXT("AdvanceSync"),
			TEXT("MarkDurableComplete")}
			&& HasExactSuccessfulPersistenceCalls(PreCommitRetry))});
	const FString CleanupRoot = FPaths::Combine(Root, TEXT("16B"));
	const FPersistenceOutcome Committed = RunPersistence(CleanupRoot, EFailurePoint::Cleanup);
	const FPersistenceOutcome Next = RunPersistence(CleanupRoot, EFailurePoint::None, false);
	Test.TestTrue(TEXT("Row16B cleanup defer keeps valid receipt and next request cleans before staging"),
		Committed.Receipt.bSyncStateAdvanced && HasCode(Committed, TEXT("Persistence.CleanupDeferred"))
		&& Next.Receipt.bSyncStateAdvanced && Next.Calls.Num() > 1
		&& Next.Calls[0] == TEXT("RecoverIncomplete") && Next.Calls[1] == TEXT("StagePackage"));
	CheckFrozenRowOracle(Test, {16,
		Observed(HasSingleFailure(Committed, EAssetDocumentPersistenceStep::DurableComplete,
			TEXT("Persistence.CleanupDeferred"))
			&& HasSingleFailure(Next, EAssetDocumentPersistenceStep::Recover, TEXT("Persistence.Recovered"))),
		Observed(HasBoundIdentity(Committed) && HasBoundIdentity(Next)),
		NotApplicable(HasNoLiveTransactionTerminalEffects(Committed)
			&& HasNoLiveTransactionTerminalEffects(Next)),
		Observed(Committed.Header == TArray<uint8>{0x4e, 0x45, 0x57, 0x48}
			&& Committed.Companion == TArray<uint8>{0x4e, 0x45, 0x57, 0x43}
			&& Committed.Sidecar == TArray<uint8>{0x4e, 0x45, 0x57, 0x53}
			&& Next.BeforeSyncBytes == Committed.SyncBytes),
		Observed(HasCommittedOwnershipLedger(Committed) && HasCommittedOwnershipLedger(Next)),
		Observed(HasOnlyReloadInvalidation(Committed) && HasOnlyReloadInvalidation(Next)),
		Observed(Committed.Receipt.bSyncStateAdvanced && Next.Receipt.bSyncStateAdvanced),
		Observed(!HasCode(Committed, TEXT("Atomic.Poisoned.RecoveryFailed"))
			&& !HasCode(Next, TEXT("Atomic.Poisoned.RecoveryFailed"))),
		Observed(Next.Receipt.bSyncStateAdvanced && HasExactSuccessfulPersistenceCalls(Next)),
		Observed(HasExactSuccessfulPersistenceCalls(Committed)
			&& HasExactSuccessfulPersistenceCalls(Next))});
}

void Rows17And18(FAutomationTestBase& Test)
{
	{
		FTransactionFixture Fixture;
		const FAssetDocumentPreflightReceipt Receipt = Preflight();
		TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Fixture.Begin(Receipt);
		Transaction->EnlistFootprint(Receipt.CompleteFootprint);
		Transaction->MarkReadyToCommit();
		FAssetDocumentDurableCompletionReceipt InvalidReceipt;
		InvalidReceipt.RequestId = Receipt.RequestId;
		InvalidReceipt.TransactionId = Transaction->GetTransactionId();
		const bool bRejected = !Transaction->Commit(InvalidReceipt).bValid;
		Transaction->Rollback(TEXT("Commit.InvalidReceipt"));
		Test.TestTrue(TEXT("Row17 rejected receipt has rollback invalidation but no commit invalidation"), bRejected
			&& Transaction->GetState() == EAssetDocumentAtomicState::RolledBack
			&& Fixture.Handles.Invalidations.Num() == 1
			&& Fixture.Handles.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::Rollback);
		CheckFrozenRowOracle(Test, {17,
			Observed(HasAtomicDiagnostic(Fixture, EAssetDocumentRuntimeRequestPhase::Complete, TEXT("Atomic.InvalidDurableCompletion"))),
			Observed(Transaction->GetMutationToken().RequestId == Receipt.RequestId && Transaction->GetTransactionId().IsValid()),
			Observed(Transaction->GetState() == EAssetDocumentAtomicState::RolledBack),
			NotApplicable(Fixture.Resources.HasExplicitAbsentPackageAndSidecarBaselines()),
			NotApplicable(Fixture.Resources.HasExplicitZeroOwnership()),
			Observed(Fixture.Handles.Invalidations.Num() == 1
				&& Fixture.Handles.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::Rollback),
			NotApplicable(HasNoSyncEffects(Fixture)), Observed(Transaction->GetState() != EAssetDocumentAtomicState::Poisoned),
			Observed(CanBeginFreshTransaction(Receipt)),
			Observed(Fixture.Calls.Calls == TArray<FString>{
				TEXT("Invalidate.Rollback"), TEXT("Rollback.b"), TEXT("Rollback.a")}
				&& Fixture.Handles.Invalidations.Num() == 1
				&& Fixture.A.RollbackCount == 1 && Fixture.B.RollbackCount == 1
				&& Fixture.Diagnostics.Num() == 1)});
	}
	{
		FTransactionFixture Fixture;
		Fixture.A.bFailRollback = true;
		const FAssetDocumentPreflightReceipt Receipt = Preflight();
		TUniquePtr<IAssetDocumentAtomicTransaction> Transaction = Fixture.Begin(Receipt);
		Transaction->EnlistFootprint(Receipt.CompleteFootprint);
		const bool bPoisoned = !Transaction->Rollback(TEXT("Rollback.Failed")).bValid;
		const bool bRetryRejected = !Transaction->Rollback(TEXT("Rollback.Retry")).bValid;
		Test.TestTrue(TEXT("Row18 rollback participant failure poisons after attempting all once and rejects retry"),
			bPoisoned && bRetryRejected && Transaction->GetState() == EAssetDocumentAtomicState::Poisoned
			&& Fixture.A.RollbackCount == 1 && Fixture.B.RollbackCount == 1);
		const bool bExactPoisonDiagnostics = Fixture.Diagnostics.Num() == 3
			&& Fixture.Diagnostics[0].Key == EAssetDocumentRuntimeRequestPhase::Failed
			&& Fixture.Diagnostics[0].Value.Code == TEXT("Atomic.Poisoned.ComponentFailed")
			&& Fixture.Diagnostics[1].Key == EAssetDocumentRuntimeRequestPhase::Failed
			&& Fixture.Diagnostics[1].Value.Code == TEXT("Atomic.Poisoned.RollbackFailed")
			&& Fixture.Diagnostics[2].Key == EAssetDocumentRuntimeRequestPhase::Failed
			&& Fixture.Diagnostics[2].Value.Code == TEXT("Atomic.TerminalAlreadyChosen")
			&& !Fixture.Diagnostics[0].Value.Path.IsEmpty() && !Fixture.Diagnostics[0].Value.Message.IsEmpty()
			&& !Fixture.Diagnostics[1].Value.Path.IsEmpty() && !Fixture.Diagnostics[1].Value.Message.IsEmpty()
			&& !Fixture.Diagnostics[2].Value.Path.IsEmpty() && !Fixture.Diagnostics[2].Value.Message.IsEmpty();
		CheckFrozenRowOracle(Test, {18,
			Observed(bExactPoisonDiagnostics),
			Observed(Transaction->GetMutationToken().RequestId == Receipt.RequestId && Transaction->GetTransactionId().IsValid()),
			Observed(Transaction->GetState() == EAssetDocumentAtomicState::Poisoned),
			NotApplicable(Fixture.Resources.HasExplicitAbsentPackageAndSidecarBaselines()),
			NotApplicable(Fixture.Resources.HasExplicitZeroOwnership()),
			Observed(Fixture.Handles.Invalidations.Num() == 1
				&& Fixture.Handles.Invalidations[0].Reason == EAssetDocumentHandleInvalidationReason::Rollback),
			NotApplicable(HasNoSyncEffects(Fixture)), Observed(bPoisoned), Observed(bRetryRejected),
			Observed(Fixture.Calls.Calls == TArray<FString>{
				TEXT("Invalidate.Rollback"), TEXT("Rollback.b"), TEXT("Rollback.a")}
				&& Fixture.Handles.Invalidations.Num() == 1
				&& Fixture.A.RollbackCount == 1 && Fixture.B.RollbackCount == 1
				&& bExactPoisonDiagnostics)});
	}
}

void LeaveIncompleteHeaderInstall(const FString& Root)
{
	const FString InstalledRoot = FPaths::Combine(Root, TEXT("Installed"));
	const FString JournalRoot = FPaths::Combine(Root, TEXT("Journal"));
	const FString BackupRoot = FPaths::Combine(JournalRoot, TEXT("Backups"));
	Write(FPaths::Combine(InstalledRoot, TEXT("Asset.uasset")), TArray<uint8>{0x4f, 0x4c, 0x44, 0x48});
	Write(FPaths::Combine(InstalledRoot, TEXT("Asset.assetdoc")), TArray<uint8>{0x4f, 0x4c, 0x44, 0x53});
	const FAssetDocumentSyncStateKey SyncKey{TEXT("/Game/Atomic/Asset"), TEXT("Body")};
	FAssetDocumentJournaledTypedSyncStateStore Store(FPaths::Combine(Root, TEXT("Sync")));
	FAssetDocumentTypedSyncStateRecord Baseline;
	Baseline.LastDurable.Asset = Evidence(TEXT("asset:old"));
	Baseline.LastDurable.Sidecar = Evidence(TEXT("sidecar:old"));
	Baseline.RequestId = FGuid::NewGuid();
	Baseline.TransactionId = FGuid::NewGuid();
	Baseline.JournalId = TEXT("baseline");
	Store.Store(SyncKey, Baseline);

	const FAssetDocumentPersistencePlan Plan = PersistencePlan(Root);
	FPackageBackend Backend(Root);
	FAssetDocumentPackageStager PackageStager(Backend);
	FAssetDocumentSidecarByteIo SidecarIo;
	FAssetDocumentPackageArtifactSet Package;
	FAssetDocumentByteArtifact Sidecar;
	PackageStager.Stage(Plan, Package);
	SidecarIo.Stage(Plan, Sidecar);
	FAssetDocumentPersistenceJournalRecord Record;
	Record.RequestId = Plan.RequestId;
	Record.TransactionId = Plan.TransactionId;
	Record.JournalId = Plan.JournalId;
	PackageStager.CaptureInstalledBaseline(Package, BackupRoot, Record.Artifacts);
	FAssetDocumentJournalArtifact SidecarJournal;
	SidecarIo.CaptureInstalledBaseline(Sidecar, FPaths::Combine(BackupRoot, TEXT("sidecar.bin")), SidecarJournal);
	Record.Artifacts.Add(SidecarJournal);
	for (FAssetDocumentJournalArtifact& Artifact : Record.Artifacts)
	{
		Artifact.TargetPath = FPaths::ConvertRelativePathToFull(Artifact.TargetPath);
		Artifact.BackupPath = FPaths::ConvertRelativePathToFull(Artifact.BackupPath);
		FPaths::NormalizeFilename(Artifact.TargetPath);
		FPaths::NormalizeFilename(Artifact.BackupPath);
	}
	TOptional<FAssetDocumentTypedSyncStateRecord> Loaded;
	Store.Load(SyncKey, Loaded, Record.SyncPreimageBytes, Record.bSyncBaselineExisted);
	Record.SyncPreimageContentDigest = ComputeAssetDocumentContentDigest(Record.SyncPreimageBytes);
	FAssetDocumentPersistenceJournal Journal(JournalRoot);
	Journal.Prepare(Record);
	const int32 HeaderIndex = Package.Artifacts.IndexOfByPredicate([](const FAssetDocumentPackageArtifact& Artifact)
	{
		return FPaths::GetExtension(Artifact.TargetPath, true) == TEXT(".uasset");
	});
	Journal.MarkArtifactInstalled(HeaderIndex);
	FAssetDocumentByteArtifact Header;
	Header.StagedPath = Package.Artifacts[HeaderIndex].StagedPath;
	Header.TargetPath = Package.Artifacts[HeaderIndex].TargetPath;
	Header.ContentDigest = Package.Artifacts[HeaderIndex].ContentDigest;
	SidecarIo.Install(Header);
}

void Rows19And20(FAutomationTestBase& Test, const FString& Root)
{
	const FString Row19Root = FPaths::Combine(Root, TEXT("19"));
	LeaveIncompleteHeaderInstall(Row19Root);
	const FPersistenceOutcome Retry = RunPersistence(Row19Root, EFailurePoint::None, false);
	Test.TestTrue(TEXT("Row19 incomplete journal recovers before staging and permits fresh transaction"),
		Retry.Receipt.bSyncStateAdvanced && Retry.Calls[0] == TEXT("RecoverIncomplete")
		&& Retry.PreStageHeader == TArray<uint8>{0x4f, 0x4c, 0x44, 0x48}
		&& Retry.PreStageCompanion.IsEmpty()
		&& Retry.PreStageSidecar == TArray<uint8>{0x4f, 0x4c, 0x44, 0x53});
	CheckFrozenRowOracle(Test, {19,
		Observed(HasSingleFailure(Retry, EAssetDocumentPersistenceStep::Recover, TEXT("Persistence.Recovered"))),
		Observed(HasBoundIdentity(Retry)),
		NotApplicable(HasNoLiveTransactionTerminalEffects(Retry)),
		Observed(Retry.PreStageHeader == TArray<uint8>{0x4f, 0x4c, 0x44, 0x48}
			&& Retry.PreStageCompanion.IsEmpty()
			&& Retry.PreStageSidecar == TArray<uint8>{0x4f, 0x4c, 0x44, 0x53}),
		Observed(HasCommittedOwnershipLedger(Retry)
			&& Retry.PreStageHeader == TArray<uint8>{0x4f, 0x4c, 0x44, 0x48}
			&& Retry.PreStageCompanion.IsEmpty()
			&& Retry.PreStageSidecar == TArray<uint8>{0x4f, 0x4c, 0x44, 0x53}),
		Observed(HasOnlyReloadInvalidation(Retry)),
		Observed(!Retry.BeforeSyncBytes.IsEmpty() && Retry.SyncBytes != Retry.BeforeSyncBytes
			&& Retry.Receipt.bSyncStateAdvanced),
		Observed(!HasCode(Retry, TEXT("Atomic.Poisoned.RecoveryFailed"))),
		Observed(Retry.Receipt.bSyncStateAdvanced && HasExactSuccessfulPersistenceCalls(Retry)),
		Observed(HasExactSuccessfulPersistenceCalls(Retry) && Retry.Failures.Num() == 1)});
	const FString Row20Root = FPaths::Combine(Root, TEXT("20"));
	const FPersistenceOutcome Committed = RunPersistence(Row20Root, EFailurePoint::Cleanup);
	Write(FPaths::Combine(Row20Root, TEXT("Installed/Asset.uasset")), TArray<uint8>{0xba, 0xd0});
	const FPersistenceOutcome Poisoned = RunPersistence(Row20Root, EFailurePoint::None, false);
	const FPersistenceOutcome PoisonedRetry = RunPersistence(Row20Root, EFailurePoint::None, false);
	Test.TestTrue(TEXT("Row20 recovery failure poisons and performs zero staging sync or retry"),
		Committed.Receipt.bSyncStateAdvanced && !Poisoned.Receipt.RequestId.IsValid()
		&& Poisoned.Calls == TArray<FString>{TEXT("RecoverIncomplete")}
		&& HasCode(Poisoned, TEXT("Atomic.Poisoned.RecoveryFailed")));
	CheckFrozenRowOracle(Test, {20,
		Observed(HasSingleFailure(Poisoned, EAssetDocumentPersistenceStep::Recover,
			TEXT("Atomic.Poisoned.RecoveryFailed"))
			&& HasSingleFailure(PoisonedRetry, EAssetDocumentPersistenceStep::Recover,
				TEXT("Atomic.Poisoned.RecoveryFailed"))),
		Observed(HasBoundIdentity(Poisoned) && HasBoundIdentity(PoisonedRetry)),
		NotApplicable(HasNoAtomicTransactionEffects(Poisoned)
			&& HasNoAtomicTransactionEffects(PoisonedRetry)),
		Observed(Poisoned.Header == TArray<uint8>{0xba, 0xd0}
			&& PoisonedRetry.Header == Poisoned.Header
			&& PoisonedRetry.Companion == Poisoned.Companion
			&& PoisonedRetry.Sidecar == Poisoned.Sidecar),
		Observed(HasCommittedOwnershipLedger(Poisoned)
			&& HasCommittedOwnershipLedger(PoisonedRetry)),
		NotApplicable(Poisoned.Invalidations.IsEmpty() && PoisonedRetry.Invalidations.IsEmpty()),
		Observed(HasUnchangedSync(Poisoned) && HasUnchangedSync(PoisonedRetry)),
		Observed(!Poisoned.Receipt.RequestId.IsValid()
			&& !PoisonedRetry.Receipt.RequestId.IsValid()),
		Observed(!PoisonedRetry.Receipt.RequestId.IsValid()
			&& PoisonedRetry.Calls == TArray<FString>{TEXT("RecoverIncomplete")}),
		Observed(Poisoned.Calls == TArray<FString>{TEXT("RecoverIncomplete")}
			&& PoisonedRetry.Calls == TArray<FString>{TEXT("RecoverIncomplete")}
			&& Poisoned.Invalidations.IsEmpty() && PoisonedRetry.Invalidations.IsEmpty())});
}
} // namespace AssetDocumentAtomicAllPhaseRollbackTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicAllPhaseRollbackTest,
	"UEAssetDoc.AtomicFramework.AllPhaseRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicAllPhaseRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAtomicAllPhaseRollbackTests;
	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentAllPhase"), FGuid::NewGuid().ToString());
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const FAssetDocumentAtomicTestMount Mount(Root);
	Rows01To06(*this);
	Rows07To15(*this, Root);
	Row16(*this, Root);
	Rows17And18(*this);
	Rows19And20(*this, Root);
	Rows21And22(*this);
	return true;
}

#endif
