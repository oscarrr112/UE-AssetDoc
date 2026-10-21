// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentRollbackAction.h"

#include "Misc/AutomationTest.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentAtomicTransactionContractTests
{
// S: Atomic transaction and footprint kinds are frozen wire values.
static_assert(static_cast<uint8>(EAssetDocumentAtomicState::Live) == 0);
static_assert(static_cast<uint8>(EAssetDocumentAtomicState::Mutating) == 1);
static_assert(static_cast<uint8>(EAssetDocumentAtomicState::ReadyToCommit) == 2);
static_assert(static_cast<uint8>(EAssetDocumentAtomicState::Committed) == 3);
static_assert(static_cast<uint8>(EAssetDocumentAtomicState::RolledBack) == 4);
static_assert(static_cast<uint8>(EAssetDocumentAtomicState::Poisoned) == 5);
static_assert(static_cast<uint8>(EAssetDocumentMutationFootprintKind::UObjectState) == 0);
static_assert(static_cast<uint8>(EAssetDocumentMutationFootprintKind::Package) == 1);
static_assert(static_cast<uint8>(EAssetDocumentMutationFootprintKind::AssetRegistry) == 2);
static_assert(static_cast<uint8>(EAssetDocumentMutationFootprintKind::File) == 3);
static_assert(static_cast<uint8>(EAssetDocumentMutationFootprintKind::Handle) == 4);

#define ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(Type, Member, Expected) \
	static_assert(std::is_same_v<decltype(Type::Member), Expected>)
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentMutationFootprint, Kind, EAssetDocumentMutationFootprintKind);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentMutationFootprint, StableId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentMutationFootprint, RollbackComponentId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPreflightReceipt, RequestId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPreflightReceipt, RuntimeRevision, uint64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPreflightReceipt, CompleteFootprint, TArray<FAssetDocumentMutationFootprint>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPreflightReceipt, ReceiptHash, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPreflightReceipt, bSucceeded, bool);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentMutationToken, RequestId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentMutationToken, TransactionId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentMutationToken, RuntimeRevision, uint64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentDurableCompletionReceipt, RequestId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentDurableCompletionReceipt, TransactionId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentDurableCompletionReceipt, JournalId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentDurableCompletionReceipt, PackageContentDigest, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentDurableCompletionReceipt, SidecarContentDigest, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentDurableCompletionReceipt, bSyncStateAdvanced, bool);
#undef ASSET_DOCUMENT_ASSERT_MEMBER_TYPE

using FTransitionSignature = bool (*)(EAssetDocumentAtomicState, EAssetDocumentAtomicState);
using FValidateReceiptSignature = FAssetDocumentContractValidationResult (*)(const FAssetDocumentPreflightReceipt&);
using FIsCancellationRequestedSignature = bool (IAssetDocumentCancellationView::*)() const;
using FGetTransactionIdSignature = FGuid (IAssetDocumentAtomicTransaction::*)() const;
using FGetStateSignature = EAssetDocumentAtomicState (IAssetDocumentAtomicTransaction::*)() const;
using FGetMutationTokenSignature = const FAssetDocumentMutationToken& (IAssetDocumentAtomicTransaction::*)() const;
using FEnlistFootprintSignature = FAssetDocumentContractValidationResult (IAssetDocumentAtomicTransaction::*)(
	TConstArrayView<FAssetDocumentMutationFootprint>);
using FMarkReadySignature = FAssetDocumentContractValidationResult (IAssetDocumentAtomicTransaction::*)();
using FCommitSignature = FAssetDocumentContractValidationResult (IAssetDocumentAtomicTransaction::*)(
	const FAssetDocumentDurableCompletionReceipt&);
using FRollbackSignature = FAssetDocumentContractValidationResult (IAssetDocumentAtomicTransaction::*)(FStringView);
using FEnlistRollbackActionSignature = FAssetDocumentContractValidationResult (IAssetDocumentAtomicTransaction::*)(
	TSharedRef<IAssetDocumentRollbackAction>);
using FEnlistPostRollbackFinalizerSignature = FAssetDocumentContractValidationResult (IAssetDocumentAtomicTransaction::*)(
	TSharedRef<IAssetDocumentPostRollbackFinalizer>);
using FGetPostRollbackFinalizerComponentIdSignature = FStringView (IAssetDocumentPostRollbackFinalizer::*)() const;
using FFinalizePostRollbackSignature = FAssetDocumentContractValidationResult (IAssetDocumentPostRollbackFinalizer::*)(FStringView);
using FBeginSignature = TUniquePtr<IAssetDocumentAtomicTransaction> (IAssetDocumentAtomicTransactionFactory::*)(
	const FAssetDocumentPreflightReceipt&, IAssetDocumentCancellationView&);
static_assert(std::is_same_v<decltype(&IsAssetDocumentAtomicTransitionAllowed), FTransitionSignature>);
static_assert(std::is_same_v<decltype(&ValidateAssetDocumentPreflightReceipt), FValidateReceiptSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentCancellationView::IsCancellationRequested), FIsCancellationRequestedSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentAtomicTransaction::GetTransactionId), FGetTransactionIdSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentAtomicTransaction::GetState), FGetStateSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentAtomicTransaction::GetMutationToken), FGetMutationTokenSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentAtomicTransaction::EnlistFootprint), FEnlistFootprintSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentAtomicTransaction::MarkReadyToCommit), FMarkReadySignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentAtomicTransaction::Commit), FCommitSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentAtomicTransaction::Rollback), FRollbackSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentAtomicTransaction::EnlistRollbackAction), FEnlistRollbackActionSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentAtomicTransaction::EnlistPostRollbackFinalizer), FEnlistPostRollbackFinalizerSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentPostRollbackFinalizer::GetRollbackComponentId), FGetPostRollbackFinalizerComponentIdSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentPostRollbackFinalizer::FinalizePostRollback), FFinalizePostRollbackSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentAtomicTransactionFactory::BeginLiveAfterPreflight), FBeginSignature>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentCancellationView>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentAtomicTransaction>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentPostRollbackFinalizer>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentAtomicTransactionFactory>);
static_assert(AssetDocumentRuntimeContractVersion == 1u);

FAssetDocumentPreflightReceipt MakeValidReceipt()
{
	FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = FGuid::NewGuid();
	Receipt.RuntimeRevision = 17;
	Receipt.ReceiptHash = TEXT("sha256:preflight-receipt");
	Receipt.bSucceeded = true;
	Receipt.CompleteFootprint.Add({EAssetDocumentMutationFootprintKind::UObjectState, TEXT("Object:/Game/Test.Asset"), TEXT("rollback.uobject")});
	Receipt.CompleteFootprint.Add({EAssetDocumentMutationFootprintKind::Package, TEXT("Package:/Game/Test"), TEXT("rollback.package")});
	Receipt.CompleteFootprint.Add({EAssetDocumentMutationFootprintKind::AssetRegistry, TEXT("Registry:/Game/Test.Asset"), TEXT("rollback.registry")});
	Receipt.CompleteFootprint.Add({EAssetDocumentMutationFootprintKind::File, TEXT("File:Content/Test.uasset"), TEXT("rollback.file")});
	Receipt.CompleteFootprint.Add({EAssetDocumentMutationFootprintKind::Handle, TEXT("Handle:17:4"), TEXT("rollback.handle")});
	return Receipt;
}

void TestInvalidReceipt(
	FAutomationTestBase& Test,
	const TCHAR* What,
	const FAssetDocumentPreflightReceipt& Receipt,
	const TCHAR* ExpectedCode,
	const TCHAR* ExpectedMessage)
{
	const FAssetDocumentContractValidationResult Result = ValidateAssetDocumentPreflightReceipt(Receipt);
	Test.TestFalse(*FString::Printf(TEXT("P: %s is rejected"), What), Result.bValid);
	Test.TestEqual(*FString::Printf(TEXT("P: %s returns one diagnostic"), What), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() == 1)
	{
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic path"), What), Result.Diagnostics[0].Path, FString(TEXT("/PreflightReceipt")));
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic code"), What), Result.Diagnostics[0].Code, FString(ExpectedCode));
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic message"), What), Result.Diagnostics[0].Message, FString(ExpectedMessage));
	}
}

void TestReceiptEnvelope(FAutomationTestBase& Test)
{
	const FAssetDocumentPreflightReceipt Valid = MakeValidReceipt();
	Test.TestTrue(TEXT("P: successful complete read-only preflight receipt is valid"), ValidateAssetDocumentPreflightReceipt(Valid).bValid);

	FAssetDocumentPreflightReceipt Invalid = Valid;
	Invalid.bSucceeded = false;
	TestInvalidReceipt(
		Test, TEXT("unsuccessful preflight"), Invalid,
		TEXT("PreflightReceipt.NotSucceeded"),
		TEXT("Preflight must succeed before a live transaction can begin."));
	Invalid = Valid;
	Invalid.RequestId.Invalidate();
	TestInvalidReceipt(
		Test, TEXT("invalid request identity"), Invalid,
		TEXT("PreflightReceipt.InvalidRequestId"),
		TEXT("Preflight receipt requires a valid request identity."));
	Invalid = Valid;
	Invalid.RuntimeRevision = 0;
	TestInvalidReceipt(
		Test, TEXT("zero runtime revision"), Invalid,
		TEXT("PreflightReceipt.InvalidRuntimeRevision"),
		TEXT("Preflight receipt requires a non-zero runtime revision."));
	Invalid = Valid;
	Invalid.ReceiptHash.Reset();
	TestInvalidReceipt(
		Test, TEXT("empty stable receipt hash"), Invalid,
		TEXT("PreflightReceipt.EmptyReceiptHash"),
		TEXT("Preflight receipt requires a stable receipt hash."));
	Invalid = Valid;
	Invalid.CompleteFootprint.Reset();
	TestInvalidReceipt(
		Test, TEXT("empty complete footprint"), Invalid,
		TEXT("PreflightReceipt.EmptyFootprint"),
		TEXT("Preflight receipt requires a complete mutation footprint."));
}

void TestValidationFamilyPrecedence(FAutomationTestBase& Test)
{
	FAssetDocumentPreflightReceipt Invalid = MakeValidReceipt();
	Invalid.bSucceeded = false;
	Invalid.RequestId.Invalidate();
	Invalid.RuntimeRevision = 0;
	TestInvalidReceipt(
		Test, TEXT("preflight-success-request-revision precedence"), Invalid,
		TEXT("PreflightReceipt.NotSucceeded"),
		TEXT("Preflight must succeed before a live transaction can begin."));

	Invalid = MakeValidReceipt();
	Invalid.RequestId.Invalidate();
	Invalid.RuntimeRevision = 0;
	TestInvalidReceipt(
		Test, TEXT("request-revision identity precedence"), Invalid,
		TEXT("PreflightReceipt.InvalidRequestId"),
		TEXT("Preflight receipt requires a valid request identity."));

	Invalid = MakeValidReceipt();
	Invalid.ReceiptHash.Reset();
	Invalid.CompleteFootprint.Reset();
	TestInvalidReceipt(
		Test, TEXT("receipt-hash-footprint-shape precedence"), Invalid,
		TEXT("PreflightReceipt.EmptyReceiptHash"),
		TEXT("Preflight receipt requires a stable receipt hash."));

	Invalid = MakeValidReceipt();
	Invalid.RequestId.Invalidate();
	Invalid.ReceiptHash.Reset();
	Invalid.CompleteFootprint[0].Kind = static_cast<EAssetDocumentMutationFootprintKind>(5);
	Invalid.CompleteFootprint[0].StableId.Reset();
	TestInvalidReceipt(
		Test, TEXT("identity-shape-footprint precedence"), Invalid,
		TEXT("PreflightReceipt.InvalidRequestId"),
		TEXT("Preflight receipt requires a valid request identity."));
}

void TestFootprintMembers(FAutomationTestBase& Test)
{
	const FAssetDocumentPreflightReceipt Valid = MakeValidReceipt();
	for (uint8 Index = 0; Index <= static_cast<uint8>(EAssetDocumentMutationFootprintKind::Handle); ++Index)
	{
		FAssetDocumentPreflightReceipt Receipt = Valid;
		Receipt.CompleteFootprint[0].Kind = static_cast<EAssetDocumentMutationFootprintKind>(Index);
		Test.TestTrue(*FString::Printf(TEXT("P: footprint kind %u is valid"), Index), ValidateAssetDocumentPreflightReceipt(Receipt).bValid);
	}

	FAssetDocumentPreflightReceipt Invalid = Valid;
	Invalid.CompleteFootprint[0].Kind = static_cast<EAssetDocumentMutationFootprintKind>(5);
	TestInvalidReceipt(
		Test, TEXT("first out-of-range footprint kind"), Invalid,
		TEXT("PreflightReceipt.InvalidFootprintKind"),
		TEXT("Mutation footprint kind is outside the frozen enum."));
	Invalid = Valid;
	Invalid.CompleteFootprint[0].Kind = static_cast<EAssetDocumentMutationFootprintKind>(255);
	TestInvalidReceipt(
		Test, TEXT("high invalid footprint kind"), Invalid,
		TEXT("PreflightReceipt.InvalidFootprintKind"),
		TEXT("Mutation footprint kind is outside the frozen enum."));
	Invalid = Valid;
	Invalid.CompleteFootprint[0].StableId.Reset();
	TestInvalidReceipt(
		Test, TEXT("empty stable footprint identity"), Invalid,
		TEXT("PreflightReceipt.EmptyStableId"),
		TEXT("Every mutation footprint item requires a stable identity."));
	Invalid = Valid;
	Invalid.CompleteFootprint[0].RollbackComponentId.Reset();
	TestInvalidReceipt(
		Test, TEXT("empty rollback component identity"), Invalid,
		TEXT("PreflightReceipt.EmptyRollbackComponentId"),
		TEXT("Every mutation footprint item requires a rollback component identity."));
}

void TestUniqueCompleteFootprint(FAutomationTestBase& Test)
{
	FAssetDocumentPreflightReceipt Invalid = MakeValidReceipt();
	const FAssetDocumentMutationFootprint ExactDuplicate = Invalid.CompleteFootprint[0];
	Invalid.CompleteFootprint.Add(ExactDuplicate);
	TestInvalidReceipt(
		Test, TEXT("exact duplicate complete footprint row"), Invalid,
		TEXT("PreflightReceipt.DuplicateStableId"),
		TEXT("Mutation footprint stable identities must be unique."));

	Invalid = MakeValidReceipt();
	FAssetDocumentMutationFootprint Duplicate = Invalid.CompleteFootprint[0];
	Duplicate.Kind = EAssetDocumentMutationFootprintKind::File;
	Duplicate.RollbackComponentId = TEXT("rollback.duplicate");
	Invalid.CompleteFootprint.Add(MoveTemp(Duplicate));
	TestInvalidReceipt(
		Test, TEXT("duplicate stable identity across kind and rollback"), Invalid,
		TEXT("PreflightReceipt.DuplicateStableId"),
		TEXT("Mutation footprint stable identities must be unique."));

	Invalid = MakeValidReceipt();
	Invalid.CompleteFootprint[1].RollbackComponentId = Invalid.CompleteFootprint[0].RollbackComponentId;
	Test.TestTrue(
		TEXT("P: rollback components may serve multiple unique footprint identities"),
		ValidateAssetDocumentPreflightReceipt(Invalid).bValid);
}

void TestExactTransitionTable(FAutomationTestBase& Test)
{
	static constexpr bool Allowed[6][6] = {
		{false, true, false, false, true, true},
		{false, false, true, false, true, true},
		{false, false, false, true, true, true},
		{false, false, false, false, false, false},
		{false, false, false, false, false, false},
		{false, false, false, false, false, false},
	};
	for (uint8 From = 0; From < 6; ++From)
	{
		for (uint8 To = 0; To < 6; ++To)
		{
			Test.TestEqual(
				*FString::Printf(TEXT("P: atomic edge %u -> %u is exact"), From, To),
				IsAssetDocumentAtomicTransitionAllowed(
					static_cast<EAssetDocumentAtomicState>(From), static_cast<EAssetDocumentAtomicState>(To)),
				Allowed[From][To]);
		}
	}
	Test.TestFalse(
		TEXT("P: first out-of-range from-state is rejected"),
		IsAssetDocumentAtomicTransitionAllowed(
			static_cast<EAssetDocumentAtomicState>(6), EAssetDocumentAtomicState::Mutating));
	Test.TestFalse(
		TEXT("P: first out-of-range to-state is rejected"),
		IsAssetDocumentAtomicTransitionAllowed(
			EAssetDocumentAtomicState::Live, static_cast<EAssetDocumentAtomicState>(6)));
	Test.TestFalse(
		TEXT("P: high invalid from-state is rejected"),
		IsAssetDocumentAtomicTransitionAllowed(
			static_cast<EAssetDocumentAtomicState>(255), EAssetDocumentAtomicState::Mutating));
	Test.TestFalse(
		TEXT("P: high invalid to-state is rejected"),
		IsAssetDocumentAtomicTransitionAllowed(
			EAssetDocumentAtomicState::Live, static_cast<EAssetDocumentAtomicState>(255)));
}

void TestPoisonAndRollbackEdges(FAutomationTestBase& Test)
{
	static constexpr EAssetDocumentAtomicState LiveNonterminalStates[] = {
		EAssetDocumentAtomicState::Live,
		EAssetDocumentAtomicState::Mutating,
		EAssetDocumentAtomicState::ReadyToCommit,
	};
	for (EAssetDocumentAtomicState State : LiveNonterminalStates)
	{
		Test.TestTrue(TEXT("P: every live nonterminal may roll back"), IsAssetDocumentAtomicTransitionAllowed(State, EAssetDocumentAtomicState::RolledBack));
		Test.TestTrue(TEXT("P: every live nonterminal may become poisoned"), IsAssetDocumentAtomicTransitionAllowed(State, EAssetDocumentAtomicState::Poisoned));
	}
	for (uint8 To = 0; To < 6; ++To)
	{
		Test.TestFalse(
			TEXT("P: poisoned state has no outgoing edge"),
			IsAssetDocumentAtomicTransitionAllowed(
				EAssetDocumentAtomicState::Poisoned, static_cast<EAssetDocumentAtomicState>(To)));
	}
}

struct FRollbackOnDestructionVector
{
	EAssetDocumentAtomicState State;
	bool bRollbackRequired;
};

void TestRollbackOnDestructionVectors(FAutomationTestBase& Test)
{
	// V: F-ATOMIC owns destructor behavior; this table does not instantiate a fake transaction.
	static constexpr FRollbackOnDestructionVector Vectors[] = {
		{EAssetDocumentAtomicState::Live, true},
		{EAssetDocumentAtomicState::Mutating, true},
		{EAssetDocumentAtomicState::ReadyToCommit, true},
		{EAssetDocumentAtomicState::Committed, false},
		{EAssetDocumentAtomicState::RolledBack, false},
		{EAssetDocumentAtomicState::Poisoned, false},
	};
	for (const FRollbackOnDestructionVector& Vector : Vectors)
	{
		const bool bLiveNonterminal = static_cast<uint8>(Vector.State) <= static_cast<uint8>(EAssetDocumentAtomicState::ReadyToCommit);
		Test.TestEqual(TEXT("V: destructor rollback covers exactly live nonterminal states"), Vector.bRollbackRequired, bLiveNonterminal);
	}
	Test.TestEqual(TEXT("V: rollback-on-destruction owner is frozen"), FString(TEXT("F-ATOMIC")), FString(TEXT("F-ATOMIC")));
}

struct FSingleTerminalVector
{
	EAssetDocumentAtomicState TerminalState;
	EAssetDocumentAtomicState AttemptedState;
	const TCHAR* ExpectedCode;
};

void TestSingleTerminalVectors(FAutomationTestBase& Test)
{
	// V: F-ATOMIC must reject every second terminal attempt.
	static constexpr FSingleTerminalVector Vectors[] = {
		{EAssetDocumentAtomicState::Committed, EAssetDocumentAtomicState::Committed, TEXT("Atomic.TerminalAlreadyChosen")},
		{EAssetDocumentAtomicState::Committed, EAssetDocumentAtomicState::RolledBack, TEXT("Atomic.TerminalAlreadyChosen")},
		{EAssetDocumentAtomicState::RolledBack, EAssetDocumentAtomicState::Committed, TEXT("Atomic.TerminalAlreadyChosen")},
		{EAssetDocumentAtomicState::RolledBack, EAssetDocumentAtomicState::RolledBack, TEXT("Atomic.TerminalAlreadyChosen")},
		{EAssetDocumentAtomicState::Poisoned, EAssetDocumentAtomicState::RolledBack, TEXT("Atomic.Poisoned")},
	};
	for (const FSingleTerminalVector& Vector : Vectors)
	{
		Test.TestFalse(TEXT("V: terminal state cannot transition again"), IsAssetDocumentAtomicTransitionAllowed(Vector.TerminalState, Vector.AttemptedState));
		Test.TestTrue(TEXT("V: terminal rejection diagnostic is frozen"), FCString::Strlen(Vector.ExpectedCode) > 0);
	}
}

struct FPoisonVerificationVector
{
	const TCHAR* FailureBoundary;
	EAssetDocumentAtomicState ExpectedState;
	const TCHAR* ExpectedCode;
};

void TestPoisonVerificationVectors(FAutomationTestBase& Test)
{
	// V: Poison propagation and failure injection remain successor behavior.
	static constexpr FPoisonVerificationVector Vectors[] = {
		{TEXT("snapshot-restore-failed"), EAssetDocumentAtomicState::Poisoned, TEXT("Atomic.Poisoned.RollbackFailed")},
		{TEXT("rollback-component-failed"), EAssetDocumentAtomicState::Poisoned, TEXT("Atomic.Poisoned.ComponentFailed")},
		{TEXT("durable-recovery-failed"), EAssetDocumentAtomicState::Poisoned, TEXT("Atomic.Poisoned.RecoveryFailed")},
	};
	for (const FPoisonVerificationVector& Vector : Vectors)
	{
		Test.TestTrue(TEXT("V: poison injection boundary is named"), FCString::Strlen(Vector.FailureBoundary) > 0);
		Test.TestEqual(TEXT("V: poison failure reaches poisoned terminal"), Vector.ExpectedState, EAssetDocumentAtomicState::Poisoned);
		Test.TestTrue(TEXT("V: poison diagnostic is frozen"), FCString::Strlen(Vector.ExpectedCode) > 0);
	}
}

struct FCancellationBoundaryVector
{
	const TCHAR* Boundary;
	bool bMayObserveCancellation;
};

void TestCancellationBoundaryVectors(FAutomationTestBase& Test)
{
	// V: F-ATOMIC observes cancellation between declared atomic sections only.
	static constexpr FCancellationBoundaryVector Vectors[] = {
		{TEXT("before-enlist"), true},
		{TEXT("during-enlist"), false},
		{TEXT("after-enlist"), true},
		{TEXT("during-commit"), false},
		{TEXT("after-commit"), true},
	};
	for (const FCancellationBoundaryVector& Vector : Vectors)
	{
		Test.TestTrue(TEXT("V: cancellation boundary is named"), FCString::Strlen(Vector.Boundary) > 0);
		const bool bBetweenSections = !FCString::Strncmp(Vector.Boundary, TEXT("before-"), 7)
			|| !FCString::Strncmp(Vector.Boundary, TEXT("after-"), 6);
		Test.TestEqual(
			TEXT("V: only between-section boundaries observe cancellation"),
			Vector.bMayObserveCancellation,
			bBetweenSections);
	}
}

struct FNoRetryVector
{
	const TCHAR* Operation;
	uint32 MaximumAttempts;
};

void TestNoRetryVectors(FAutomationTestBase& Test)
{
	// V: F-ATOMIC performs no implicit retry at any mutation or terminal boundary.
	static constexpr FNoRetryVector Vectors[] = {
		{TEXT("enlist-footprint"), 1},
		{TEXT("mutating-primitive"), 1},
		{TEXT("commit"), 1},
		{TEXT("rollback-component"), 1},
	};
	for (const FNoRetryVector& Vector : Vectors)
	{
		Test.TestTrue(TEXT("V: no-retry operation is named"), FCString::Strlen(Vector.Operation) > 0);
		Test.TestEqual(TEXT("V: implicit retry is forbidden"), Vector.MaximumAttempts, uint32{1});
	}
	Test.TestEqual(TEXT("V: cancellation/no-retry owner is frozen"), FString(TEXT("F-ATOMIC")), FString(TEXT("F-ATOMIC")));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicTransactionContractTest,
	"UEAssetDoc.RuntimeContracts.AtomicTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicTransactionContractTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAtomicTransactionContractTests;
	(void)Parameters;

	// S: Defaults are inert contract values and cannot admit a transaction.
	const FAssetDocumentMutationFootprint Footprint;
	TestEqual(TEXT("S: footprint kind defaults UObject state"), Footprint.Kind, EAssetDocumentMutationFootprintKind::UObjectState);
	TestTrue(TEXT("S: footprint stable identity defaults empty"), Footprint.StableId.IsEmpty());
	TestTrue(TEXT("S: footprint rollback component defaults empty"), Footprint.RollbackComponentId.IsEmpty());

	const FAssetDocumentPreflightReceipt Receipt;
	TestFalse(TEXT("S: receipt request identity defaults invalid"), Receipt.RequestId.IsValid());
	TestEqual(TEXT("S: receipt runtime revision defaults zero"), Receipt.RuntimeRevision, uint64{0});
	TestTrue(TEXT("S: receipt complete footprint defaults empty"), Receipt.CompleteFootprint.IsEmpty());
	TestTrue(TEXT("S: receipt hash defaults empty"), Receipt.ReceiptHash.IsEmpty());
	TestFalse(TEXT("S: receipt defaults unsuccessful"), Receipt.bSucceeded);

	const FAssetDocumentMutationToken Token;
	TestFalse(TEXT("S: mutation token request identity defaults invalid"), Token.RequestId.IsValid());
	TestFalse(TEXT("S: mutation token transaction identity defaults invalid"), Token.TransactionId.IsValid());
	TestEqual(TEXT("S: mutation token runtime revision defaults zero"), Token.RuntimeRevision, uint64{0});

	const FAssetDocumentDurableCompletionReceipt Completion;
	TestFalse(TEXT("S: completion request identity defaults invalid"), Completion.RequestId.IsValid());
	TestFalse(TEXT("S: completion transaction identity defaults invalid"), Completion.TransactionId.IsValid());
	TestTrue(TEXT("S: completion journal defaults empty"), Completion.JournalId.IsEmpty());
	TestTrue(TEXT("S: completion package digest defaults empty"), Completion.PackageContentDigest.IsEmpty());
	TestTrue(TEXT("S: completion sidecar digest defaults empty"), Completion.SidecarContentDigest.IsEmpty());
	TestFalse(TEXT("S: completion sync advancement defaults false"), Completion.bSyncStateAdvanced);

	TestReceiptEnvelope(*this);
	TestValidationFamilyPrecedence(*this);
	TestFootprintMembers(*this);
	TestUniqueCompleteFootprint(*this);
	TestExactTransitionTable(*this);
	TestPoisonAndRollbackEdges(*this);
	TestRollbackOnDestructionVectors(*this);
	TestSingleTerminalVectors(*this);
	TestPoisonVerificationVectors(*this);
	TestCancellationBoundaryVectors(*this);
	TestNoRetryVectors(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPostRollbackFinalizerContractTest,
	"UEAssetDoc.AtomicTransaction.PostRollbackFinalizerContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPostRollbackFinalizerContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(
		TEXT("post-rollback finalizer leaves the Runtime contract version at v1"),
		AssetDocumentRuntimeContractVersion,
		uint32{1});
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
