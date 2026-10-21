// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/Fixtures/AssetDocumentAnimationProviderFixtures.h"

#include "AssetDocumentCoreRuntimeComposition.h"
#include "AssetDocumentEditorNativeState.h"
#include "AssetDocumentRollbackAction.h"
#include "Misc/AutomationTest.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

#include <type_traits>

namespace AnimationProviderRollbackContractTestsLocal
{
using FGetTransactionIdSignature =
	FGuid (IAssetDocumentAtomicTransaction::*)() const;
using FGetStateSignature =
	EAssetDocumentAtomicState
		(IAssetDocumentAtomicTransaction::*)() const;
using FGetMutationTokenSignature =
	const FAssetDocumentMutationToken&
		(IAssetDocumentAtomicTransaction::*)() const;
using FEnlistFootprintSignature =
	FAssetDocumentContractValidationResult
		(IAssetDocumentAtomicTransaction::*)(
			TConstArrayView<FAssetDocumentMutationFootprint>);
using FEnlistRollbackActionSignature =
	FAssetDocumentContractValidationResult
		(IAssetDocumentAtomicTransaction::*)(
			TSharedRef<IAssetDocumentRollbackAction>);
using FMarkReadySignature =
	FAssetDocumentContractValidationResult
		(IAssetDocumentAtomicTransaction::*)();
using FCommitSignature =
	FAssetDocumentContractValidationResult
		(IAssetDocumentAtomicTransaction::*)(
			const FAssetDocumentDurableCompletionReceipt&);
using FRollbackSignature =
	FAssetDocumentContractValidationResult
		(IAssetDocumentAtomicTransaction::*)(FStringView);
using FCaptureAndEnlistObjectReferencePreimageSignature =
	FAssetDocumentEditorTypedPreimageResult (*) (
		const FAssetDocumentEditorAssetIdentityView&,
		UObject&,
		TSharedRef<IPropertyRowGenerator>,
		TSharedRef<IPropertyHandle>,
		TConstArrayView<FString>,
		IAssetDocumentAtomicTransaction&);

static_assert(std::is_same_v<
	decltype(&IAssetDocumentAtomicTransaction::GetTransactionId),
	FGetTransactionIdSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentAtomicTransaction::GetState),
	FGetStateSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentAtomicTransaction::GetMutationToken),
	FGetMutationTokenSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentAtomicTransaction::EnlistFootprint),
	FEnlistFootprintSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentAtomicTransaction::EnlistRollbackAction),
	FEnlistRollbackActionSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentAtomicTransaction::MarkReadyToCommit),
	FMarkReadySignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentAtomicTransaction::Commit),
	FCommitSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentAtomicTransaction::Rollback),
	FRollbackSignature>);
static_assert(std::is_same_v<
	decltype(FAssetDocumentCoreMutationInputs::RollbackActions),
	TArray<TSharedRef<IAssetDocumentRollbackAction>>>);
static_assert(std::is_same_v<
	decltype(&CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage),
	FCaptureAndEnlistObjectReferencePreimageSignature>);

class FHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind,
		EAssetDocumentHandleAccess,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&&) override
	{
		return {};
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle&,
		const FAssetDocumentHandleValidationContext&) override
	{
		return {};
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation&) override
	{
		return {};
	}
};

class FCancellationView final : public IAssetDocumentCancellationView
{
public:
	virtual bool IsCancellationRequested() const override
	{
		return false;
	}
};

class FAnimationPreimageRollbackAction final
	: public IAssetDocumentRollbackAction
{
public:
	virtual FStringView GetRollbackComponentId() const override
	{
		return TEXTVIEW("animation.preimage");
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		++RollbackCount;
		ObservedFailureCode = FString(StableFailureCode);
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}

	int32 RollbackCount = 0;
	FString ObservedFailureCode;
};

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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimationProviderRollbackContractTest,
	"UEAssetDoc.AnimationProvider.RollbackContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationProviderRollbackContractTest::RunTest(const FString&)
{
	using namespace AnimationProviderRollbackContractTestsLocal;
	int32 MutatingCandidateCount = 0;
	int32 MutatingStableBlockerCount = 0;
	for (const FAssetDocumentAnimationT0DecisionRecord& Decision :
		GetAssetDocumentAnimationT0DecisionRecords())
	{
		if (Decision.bMutating)
		{
			++MutatingCandidateCount;
			TestEqual(
				FString::Printf(
					TEXT("%s has its exact typed rollback admission or retains its blocker"),
					Decision.DecisionId),
				Decision.FinalDisposition,
				(FStringView(Decision.DecisionId) == TEXTVIEW("animation.asset.skeleton.set.v1")
				 || FStringView(Decision.DecisionId) == TEXTVIEW("animation.asset.preview_mesh.set.v1")
				 || FStringView(Decision.DecisionId) == TEXTVIEW("animation.curve.float.replace.v2")
				 || FStringView(Decision.DecisionId) == TEXTVIEW("animation.montage.slot_tracks.replace.v1")
				 || FStringView(Decision.DecisionId) == TEXTVIEW("animation.montage.sections.mutate.v1"))
					? decltype(Decision.FinalDisposition)::Admitted
					: decltype(Decision.FinalDisposition)::StableBlocker);
			if (Decision.FinalDisposition
				== decltype(Decision.FinalDisposition)::StableBlocker)
			{
				++MutatingStableBlockerCount;
			}
		}
	}
	TestEqual(
		TEXT("all ordinary mutators require rollback composition"),
		MutatingCandidateCount,
		17);
	TestEqual(
		TEXT("twelve ordinary mutators remain blocked after five exact typed rollback admissions"),
		MutatingStableBlockerCount,
		12);

	FHandleResolver Handles;
	FCancellationView Cancellation;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	auto Sink = [&Diagnostics](
		EAssetDocumentRuntimeRequestPhase,
		const FAssetDocumentRuntimeDiagnostic& Diagnostic)
	{
		Diagnostics.Add(Diagnostic);
	};
	FAssetDocumentRollbackActionComposition EmptyComposition(
		Handles,
		{},
		Sink);
	TestTrue(
		TEXT("an empty pre-invocation composition is structurally valid"),
		EmptyComposition.IsValid());

	FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = FGuid::NewGuid();
	Receipt.RuntimeRevision = 1;
	Receipt.ReceiptHash = TEXT("sha256:p-anim-t0-rollback");
	Receipt.bSucceeded = true;
	Receipt.CompleteFootprint.Add({
		EAssetDocumentMutationFootprintKind::UObjectState,
		TEXT("Object:/Game/PANIM.Target"),
		TEXT("animation.preimage"),
	});

	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
		EmptyComposition.BeginLiveAfterPreflight(Receipt, Cancellation);
	TestNotNull(
		TEXT("empty composition begins before provider captures its preimage"),
		Transaction.Get());
	if (!Transaction)
	{
		return false;
	}
	TestEqual(
		TEXT("provider receives a live transaction"),
		Transaction->GetState(),
		EAssetDocumentAtomicState::Live);

	const FAssetDocumentContractValidationResult MissingAction =
		Transaction->EnlistFootprint(Receipt.CompleteFootprint);
	TestTrue(
		TEXT("mutation footprint fails closed before its rollback action exists"),
		HasDiagnostic(
			MissingAction.Diagnostics,
			TEXT("Atomic.IncompleteRollbackCoverage")));
	TestTrue(
		TEXT("coverage rejection reaches the transaction diagnostic sink"),
		HasDiagnostic(
			Diagnostics,
			TEXT("Atomic.IncompleteRollbackCoverage")));
	TestEqual(
		TEXT("coverage rejection leaves the transaction live for provider enlistment"),
		Transaction->GetState(),
		EAssetDocumentAtomicState::Live);

	TSharedRef<FAnimationPreimageRollbackAction> Action =
		MakeShared<FAnimationPreimageRollbackAction>();
	TestTrue(
		TEXT("provider enlists its captured Animation preimage before mutation"),
		Transaction->EnlistRollbackAction(Action).bValid);
	TestTrue(
		TEXT("declared Animation footprint enlists after its rollback action"),
		Transaction->EnlistFootprint(Receipt.CompleteFootprint).bValid);
	TestEqual(
		TEXT("covered footprint enters the mutating state"),
		Transaction->GetState(),
		EAssetDocumentAtomicState::Mutating);
	TestTrue(
		TEXT("transaction-owned Animation preimage rolls back"),
		Transaction->Rollback(TEXT("AD.PANIM.TEST.INJECTED_FAILURE")).bValid);
	TestEqual(
		TEXT("rollback reaches its terminal state"),
		Transaction->GetState(),
		EAssetDocumentAtomicState::RolledBack);
	TestEqual(
		TEXT("Animation preimage action executes exactly once"),
		Action->RollbackCount,
		1);
	TestEqual(
		TEXT("Animation preimage receives the stable failure code"),
		Action->ObservedFailureCode,
		FString(TEXT("AD.PANIM.TEST.INJECTED_FAILURE")));

	return true;
}

#endif
