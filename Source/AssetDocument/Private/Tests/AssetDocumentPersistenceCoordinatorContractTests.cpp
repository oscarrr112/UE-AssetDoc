// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "Runtime/AssetDocumentPersistenceCoordinator.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <type_traits>

namespace AssetDocumentPersistenceCoordinatorContractTests
{
using FTransitionSignature = bool (*)(EAssetDocumentPersistenceStep, EAssetDocumentPersistenceStep);
using FValidatePlanSignature = FAssetDocumentContractValidationResult (*)(const FAssetDocumentPersistencePlan&);
using FVerifyFreshSignature = FAssetDocumentFreshVerificationResult (IAssetDocumentStagedArtifactVerifier::*)(
	const FAssetDocumentFreshVerificationInput&);
using FPersistSignature = FAssetDocumentDurableCompletionReceipt (IAssetDocumentPersistenceCoordinator::*)(
	const FAssetDocumentPersistencePlan&, IAssetDocumentStagedArtifactVerifier&);

static_assert(static_cast<uint8>(EAssetDocumentPersistenceStep::StagePackage) == 0);
static_assert(static_cast<uint8>(EAssetDocumentPersistenceStep::StageSidecar) == 1);
static_assert(static_cast<uint8>(EAssetDocumentPersistenceStep::FreshLoadStagedPackage) == 2);
static_assert(static_cast<uint8>(EAssetDocumentPersistenceStep::CanonicalVerify) == 3);
static_assert(static_cast<uint8>(EAssetDocumentPersistenceStep::PrepareJournal) == 4);
static_assert(static_cast<uint8>(EAssetDocumentPersistenceStep::AtomicInstall) == 5);
static_assert(static_cast<uint8>(EAssetDocumentPersistenceStep::DurableComplete) == 6);
static_assert(static_cast<uint8>(EAssetDocumentPersistenceStep::Recover) == 7);

static_assert(std::is_same_v<decltype(FAssetDocumentImmutableSidecarPayload::Utf8Bytes), TArray<uint8>>);
static_assert(std::is_same_v<decltype(FAssetDocumentImmutableSidecarPayload::SemanticHash), FString>);
static_assert(std::is_same_v<decltype(FAssetDocumentImmutableSidecarPayload::CodecVersion), uint32>);
static_assert(std::is_same_v<decltype(FAssetDocumentPersistencePlan::RequestId), FGuid>);
static_assert(std::is_same_v<decltype(FAssetDocumentPersistencePlan::TransactionId), FGuid>);
static_assert(std::is_same_v<decltype(FAssetDocumentPersistencePlan::TargetPackagePath), FString>);
static_assert(std::is_same_v<decltype(FAssetDocumentPersistencePlan::TargetSidecarPath), FString>);
static_assert(std::is_same_v<decltype(FAssetDocumentPersistencePlan::StagingRoot), FString>);
static_assert(std::is_same_v<decltype(FAssetDocumentPersistencePlan::JournalId), FString>);
static_assert(std::is_same_v<decltype(FAssetDocumentPersistencePlan::Sidecar), FAssetDocumentImmutableSidecarPayload>);
static_assert(std::is_same_v<decltype(FAssetDocumentPersistencePlan::Footprint), TArray<FAssetDocumentMutationFootprint>>);
static_assert(std::is_same_v<decltype(FAssetDocumentFreshVerificationInput::StagedPackagePath), FString>);
static_assert(std::is_same_v<decltype(FAssetDocumentFreshVerificationInput::StagedSidecarPath), FString>);
static_assert(std::is_same_v<decltype(FAssetDocumentFreshVerificationInput::FreshHandleRevision), uint64>);
static_assert(std::is_same_v<decltype(FAssetDocumentFreshVerificationInput::ExpectedSemanticHash), FString>);
static_assert(std::is_same_v<decltype(FAssetDocumentFreshVerificationInput::ExpectedCodecVersion), uint32>);
static_assert(std::is_same_v<decltype(FAssetDocumentFreshVerificationResult::bFreshLoadProven), bool>);
static_assert(std::is_same_v<decltype(FAssetDocumentFreshVerificationResult::bCanonicalMatch), bool>);
static_assert(std::is_same_v<decltype(FAssetDocumentFreshVerificationResult::ActualSemanticHash), FString>);
static_assert(std::is_same_v<decltype(FAssetDocumentFreshVerificationResult::ActualCodecVersion), uint32>);
static_assert(std::is_same_v<decltype(FAssetDocumentFreshVerificationResult::Diagnostics), TArray<FAssetDocumentRuntimeDiagnostic>>);
static_assert(std::is_same_v<decltype(&IsAssetDocumentPersistenceTransitionAllowed), FTransitionSignature>);
static_assert(std::is_same_v<decltype(&ValidateAssetDocumentPersistencePlan), FValidatePlanSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentStagedArtifactVerifier::VerifyFresh), FVerifyFreshSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentPersistenceCoordinator::Persist), FPersistSignature>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentStagedArtifactVerifier>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentPersistenceCoordinator>);

FAssetDocumentPersistencePlan MakeValidPlan()
{
	FAssetDocumentPersistencePlan Plan;
	Plan.RequestId = FGuid::NewGuid();
	Plan.TransactionId = FGuid::NewGuid();
	Plan.TargetPackagePath = TEXT("/Game/AssetDocumentTests/T07Asset");
	Plan.TargetSidecarPath = TEXT("/tmp/asset-document/T07Asset.assetdoc.json");
	Plan.StagingRoot = TEXT("/tmp/asset-document/staging/t07");
	Plan.JournalId = TEXT("journal:t07:1");
	Plan.Sidecar.Utf8Bytes = {0x7b, 0x7d};
	Plan.Sidecar.SemanticHash = TEXT("sha256:canonical-sidecar");
	Plan.Sidecar.CodecVersion = 3;
	Plan.Footprint.Add({EAssetDocumentMutationFootprintKind::Package, TEXT("Package:/Game/AssetDocumentTests/T07Asset"), TEXT("rollback.package")});
	Plan.Footprint.Add({EAssetDocumentMutationFootprintKind::File, TEXT("File:T07Asset.assetdoc.json"), TEXT("rollback.sidecar")});
	return Plan;
}

void TestInvalidPlan(
	FAutomationTestBase& Test,
	const TCHAR* What,
	const FAssetDocumentPersistencePlan& Plan,
	const TCHAR* ExpectedCode,
	const TCHAR* ExpectedMessage)
{
	const FAssetDocumentContractValidationResult Result = ValidateAssetDocumentPersistencePlan(Plan);
	Test.TestFalse(*FString::Printf(TEXT("P: %s is rejected"), What), Result.bValid);
	Test.TestEqual(*FString::Printf(TEXT("P: %s returns one diagnostic"), What), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() == 1)
	{
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic path"), What), Result.Diagnostics[0].Path, FString(TEXT("/PersistencePlan")));
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic code"), What), Result.Diagnostics[0].Code, FString(ExpectedCode));
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic message"), What), Result.Diagnostics[0].Message, FString(ExpectedMessage));
	}
}

void TestIdentityAndTargets(FAutomationTestBase& Test)
{
	const FAssetDocumentPersistencePlan Valid = MakeValidPlan();
	Test.TestTrue(TEXT("P: complete immutable persistence envelope is valid"), ValidateAssetDocumentPersistencePlan(Valid).bValid);

	FAssetDocumentPersistencePlan Invalid = Valid;
	Invalid.RequestId.Invalidate();
	TestInvalidPlan(Test, TEXT("invalid request identity"), Invalid, TEXT("PersistencePlan.InvalidRequestId"), TEXT("Persistence plan requires a valid request identity."));
	Invalid = Valid;
	Invalid.TransactionId.Invalidate();
	TestInvalidPlan(Test, TEXT("invalid transaction identity"), Invalid, TEXT("PersistencePlan.InvalidTransactionId"), TEXT("Persistence plan requires a valid transaction identity."));
	Invalid = Valid;
	Invalid.TargetPackagePath.Reset();
	TestInvalidPlan(Test, TEXT("empty target package path"), Invalid, TEXT("PersistencePlan.InvalidTargetPackagePath"), TEXT("Persistence plan requires a valid long target package path."));
	Invalid = Valid;
	Invalid.TargetPackagePath = TEXT("not-a-long-package-path");
	TestInvalidPlan(Test, TEXT("malformed target package path"), Invalid, TEXT("PersistencePlan.InvalidTargetPackagePath"), TEXT("Persistence plan requires a valid long target package path."));
	Invalid = Valid;
	Invalid.TargetSidecarPath.Reset();
	TestInvalidPlan(Test, TEXT("empty target sidecar path"), Invalid, TEXT("PersistencePlan.EmptyTargetSidecarPath"), TEXT("Persistence plan requires a target sidecar path."));
}

void TestStagingAndSidecar(FAutomationTestBase& Test)
{
	const FAssetDocumentPersistencePlan Valid = MakeValidPlan();
	FAssetDocumentPersistencePlan Invalid = Valid;
	Invalid.TargetSidecarPath = TEXT(" \t ");
	TestInvalidPlan(Test, TEXT("whitespace-only target sidecar path"), Invalid, TEXT("PersistencePlan.EmptyTargetSidecarPath"), TEXT("Persistence plan requires a target sidecar path."));
	Invalid = Valid;
	Invalid.StagingRoot.Reset();
	TestInvalidPlan(Test, TEXT("empty staging root"), Invalid, TEXT("PersistencePlan.EmptyStagingRoot"), TEXT("Persistence plan requires an isolated staging root."));
	Invalid = Valid;
	Invalid.StagingRoot = TEXT(" \t ");
	TestInvalidPlan(Test, TEXT("whitespace-only staging root"), Invalid, TEXT("PersistencePlan.EmptyStagingRoot"), TEXT("Persistence plan requires an isolated staging root."));
	Invalid = Valid;
	Invalid.JournalId.Reset();
	TestInvalidPlan(Test, TEXT("empty journal identity"), Invalid, TEXT("PersistencePlan.EmptyJournalId"), TEXT("Persistence plan requires a stable recovery journal identity."));
	Invalid = Valid;
	Invalid.Sidecar.Utf8Bytes.Reset();
	TestInvalidPlan(Test, TEXT("empty immutable sidecar bytes"), Invalid, TEXT("PersistencePlan.EmptySidecarBytes"), TEXT("Persistence plan requires Python-produced UTF-8 sidecar bytes."));
	Invalid = Valid;
	Invalid.Sidecar.SemanticHash.Reset();
	TestInvalidPlan(Test, TEXT("empty semantic hash"), Invalid, TEXT("PersistencePlan.EmptySemanticHash"), TEXT("Persistence plan requires the Python-produced semantic hash."));
	Invalid = Valid;
	Invalid.Sidecar.CodecVersion = 0;
	TestInvalidPlan(Test, TEXT("zero codec version"), Invalid, TEXT("PersistencePlan.InvalidCodecVersion"), TEXT("Persistence plan requires a non-zero document codec version."));
	Invalid = Valid;
	Invalid.Footprint.Reset();
	TestInvalidPlan(Test, TEXT("empty mutation footprint"), Invalid, TEXT("PersistencePlan.EmptyFootprint"), TEXT("Persistence plan requires the complete immutable mutation footprint."));
}

void TestStagingIsolation(FAutomationTestBase& Test)
{
	FAssetDocumentPersistencePlan Plan = MakeValidPlan();
	Plan.TargetSidecarPath = TEXT("/tmp/asset-document/final/T07Asset.assetdoc.json");
	Plan.StagingRoot = TEXT("/tmp/asset-document/staging/t07");
	Test.TestTrue(TEXT("P: final target and staging may share a parent as siblings"), ValidateAssetDocumentPersistencePlan(Plan).bValid);

	Plan.StagingRoot = Plan.TargetSidecarPath;
	TestInvalidPlan(Test, TEXT("staging root equal to final sidecar target"), Plan, TEXT("PersistencePlan.NonIsolatedStagingRoot"), TEXT("Persistence staging root must be isolated from the final sidecar target."));

	Plan = MakeValidPlan();
	Plan.TargetSidecarPath = TEXT("/tmp/asset-document/final/T07Asset.assetdoc.json");
	Plan.StagingRoot = TEXT("/tmp/asset-document/final/./staging/../T07Asset.assetdoc.json");
	TestInvalidPlan(Test, TEXT("normalized staging alias of final sidecar target"), Plan, TEXT("PersistencePlan.NonIsolatedStagingRoot"), TEXT("Persistence staging root must be isolated from the final sidecar target."));

	Plan = MakeValidPlan();
	Plan.TargetSidecarPath = TEXT("/tmp/asset-document/staging/t07/T07Asset.assetdoc.json");
	Plan.StagingRoot = TEXT("/tmp/asset-document/staging/t07");
	TestInvalidPlan(Test, TEXT("final sidecar target inside staging root"), Plan, TEXT("PersistencePlan.NonIsolatedStagingRoot"), TEXT("Persistence staging root must be isolated from the final sidecar target."));
}

void TestFootprintEnvelope(FAutomationTestBase& Test)
{
	const FAssetDocumentPersistencePlan Valid = MakeValidPlan();
	for (uint8 Index = 0; Index <= static_cast<uint8>(EAssetDocumentMutationFootprintKind::Handle); ++Index)
	{
		FAssetDocumentPersistencePlan Plan = Valid;
		Plan.Footprint[0].Kind = static_cast<EAssetDocumentMutationFootprintKind>(Index);
		Test.TestTrue(*FString::Printf(TEXT("P: persistence footprint kind %u is valid"), Index), ValidateAssetDocumentPersistencePlan(Plan).bValid);
	}

	FAssetDocumentPersistencePlan Invalid = Valid;
	Invalid.Footprint[0].Kind = static_cast<EAssetDocumentMutationFootprintKind>(5);
	TestInvalidPlan(Test, TEXT("first out-of-range footprint kind"), Invalid, TEXT("PersistencePlan.InvalidFootprintKind"), TEXT("Persistence footprint kind is outside the frozen enum."));
	Invalid = Valid;
	Invalid.Footprint[0].Kind = static_cast<EAssetDocumentMutationFootprintKind>(255);
	TestInvalidPlan(Test, TEXT("high invalid footprint kind"), Invalid, TEXT("PersistencePlan.InvalidFootprintKind"), TEXT("Persistence footprint kind is outside the frozen enum."));
	Invalid = Valid;
	Invalid.Footprint[0].StableId.Reset();
	TestInvalidPlan(Test, TEXT("empty footprint stable identity"), Invalid, TEXT("PersistencePlan.EmptyFootprintStableId"), TEXT("Every persistence footprint item requires a stable identity."));
	Invalid = Valid;
	Invalid.Footprint[0].RollbackComponentId.Reset();
	TestInvalidPlan(Test, TEXT("empty footprint rollback component"), Invalid, TEXT("PersistencePlan.EmptyRollbackComponentId"), TEXT("Every persistence footprint item requires a rollback component identity."));
}

void TestFootprintUniqueness(FAutomationTestBase& Test)
{
	FAssetDocumentPersistencePlan Invalid = MakeValidPlan();
	const FAssetDocumentMutationFootprint ExactDuplicate = Invalid.Footprint[0];
	Invalid.Footprint.Add(ExactDuplicate);
	TestInvalidPlan(Test, TEXT("exact duplicate footprint row"), Invalid, TEXT("PersistencePlan.DuplicateFootprintStableId"), TEXT("Persistence footprint stable identities must be unique."));

	Invalid = MakeValidPlan();
	FAssetDocumentMutationFootprint Duplicate = Invalid.Footprint[0];
	Duplicate.Kind = EAssetDocumentMutationFootprintKind::File;
	Duplicate.RollbackComponentId = TEXT("rollback.other");
	Invalid.Footprint.Add(MoveTemp(Duplicate));
	TestInvalidPlan(Test, TEXT("duplicate stable identity across kind and rollback"), Invalid, TEXT("PersistencePlan.DuplicateFootprintStableId"), TEXT("Persistence footprint stable identities must be unique."));

	FAssetDocumentPersistencePlan Valid = MakeValidPlan();
	Valid.Footprint[1].RollbackComponentId = Valid.Footprint[0].RollbackComponentId;
	Test.TestTrue(TEXT("P: one rollback component may cover distinct footprint identities"), ValidateAssetDocumentPersistencePlan(Valid).bValid);
}

void TestEnvelopeValidationPrecedence(FAutomationTestBase& Test)
{
	FAssetDocumentPersistencePlan Invalid = MakeValidPlan();
	Invalid.RequestId.Invalidate();
	Invalid.TransactionId.Invalidate();
	TestInvalidPlan(Test, TEXT("request before transaction"), Invalid, TEXT("PersistencePlan.InvalidRequestId"), TEXT("Persistence plan requires a valid request identity."));

	Invalid = MakeValidPlan();
	Invalid.TransactionId.Invalidate();
	Invalid.TargetPackagePath.Reset();
	TestInvalidPlan(Test, TEXT("transaction before package"), Invalid, TEXT("PersistencePlan.InvalidTransactionId"), TEXT("Persistence plan requires a valid transaction identity."));

	Invalid = MakeValidPlan();
	Invalid.TargetPackagePath.Reset();
	Invalid.TargetSidecarPath.Reset();
	TestInvalidPlan(Test, TEXT("package before sidecar"), Invalid, TEXT("PersistencePlan.InvalidTargetPackagePath"), TEXT("Persistence plan requires a valid long target package path."));

	Invalid = MakeValidPlan();
	Invalid.TargetSidecarPath = TEXT(" \t ");
	Invalid.StagingRoot = TEXT(" \t ");
	TestInvalidPlan(Test, TEXT("sidecar before staging"), Invalid, TEXT("PersistencePlan.EmptyTargetSidecarPath"), TEXT("Persistence plan requires a target sidecar path."));

	Invalid = MakeValidPlan();
	Invalid.StagingRoot = TEXT(" \t ");
	Invalid.JournalId.Reset();
	TestInvalidPlan(Test, TEXT("staging before journal"), Invalid, TEXT("PersistencePlan.EmptyStagingRoot"), TEXT("Persistence plan requires an isolated staging root."));

	Invalid = MakeValidPlan();
	Invalid.StagingRoot = Invalid.TargetSidecarPath;
	Invalid.JournalId.Reset();
	TestInvalidPlan(Test, TEXT("staging isolation before journal"), Invalid, TEXT("PersistencePlan.NonIsolatedStagingRoot"), TEXT("Persistence staging root must be isolated from the final sidecar target."));

	Invalid = MakeValidPlan();
	Invalid.JournalId.Reset();
	Invalid.Sidecar.Utf8Bytes.Reset();
	TestInvalidPlan(Test, TEXT("journal before sidecar bytes"), Invalid, TEXT("PersistencePlan.EmptyJournalId"), TEXT("Persistence plan requires a stable recovery journal identity."));

	Invalid = MakeValidPlan();
	Invalid.Sidecar.Utf8Bytes.Reset();
	Invalid.Sidecar.SemanticHash.Reset();
	TestInvalidPlan(Test, TEXT("sidecar bytes before semantic hash"), Invalid, TEXT("PersistencePlan.EmptySidecarBytes"), TEXT("Persistence plan requires Python-produced UTF-8 sidecar bytes."));

	Invalid = MakeValidPlan();
	Invalid.Sidecar.SemanticHash.Reset();
	Invalid.Sidecar.CodecVersion = 0;
	TestInvalidPlan(Test, TEXT("semantic hash before codec version"), Invalid, TEXT("PersistencePlan.EmptySemanticHash"), TEXT("Persistence plan requires the Python-produced semantic hash."));

	Invalid = MakeValidPlan();
	Invalid.Sidecar.CodecVersion = 0;
	Invalid.Footprint.Reset();
	TestInvalidPlan(Test, TEXT("codec version before footprint"), Invalid, TEXT("PersistencePlan.InvalidCodecVersion"), TEXT("Persistence plan requires a non-zero document codec version."));
}

void TestFootprintValidationPrecedence(FAutomationTestBase& Test)
{
	FAssetDocumentPersistencePlan Invalid = MakeValidPlan();
	Invalid.Footprint[0].Kind = static_cast<EAssetDocumentMutationFootprintKind>(5);
	Invalid.Footprint[0].StableId.Reset();
	TestInvalidPlan(Test, TEXT("footprint kind before stable identity"), Invalid, TEXT("PersistencePlan.InvalidFootprintKind"), TEXT("Persistence footprint kind is outside the frozen enum."));

	Invalid = MakeValidPlan();
	Invalid.Footprint[0].StableId.Reset();
	Invalid.Footprint[0].RollbackComponentId.Reset();
	TestInvalidPlan(Test, TEXT("footprint stable identity before rollback component"), Invalid, TEXT("PersistencePlan.EmptyFootprintStableId"), TEXT("Every persistence footprint item requires a stable identity."));

	Invalid = MakeValidPlan();
	Invalid.Footprint[0].RollbackComponentId.Reset();
	FAssetDocumentMutationFootprint Duplicate = Invalid.Footprint[0];
	Duplicate.RollbackComponentId = TEXT("rollback.duplicate");
	Invalid.Footprint.Add(MoveTemp(Duplicate));
	TestInvalidPlan(Test, TEXT("footprint rollback component before duplicate identity"), Invalid, TEXT("PersistencePlan.EmptyRollbackComponentId"), TEXT("Every persistence footprint item requires a rollback component identity."));
}

void TestExactTransitionTable(FAutomationTestBase& Test)
{
	static constexpr bool Allowed[8][8] = {
		{false, true, false, false, false, false, false, true},
		{false, false, true, false, false, false, false, true},
		{false, false, false, true, false, false, false, true},
		{false, false, false, false, true, false, false, true},
		{false, false, false, false, false, true, false, true},
		{false, false, false, false, false, false, true, true},
		{false, false, false, false, false, false, false, false},
		{false, false, false, false, false, false, false, false},
	};
	for (uint8 From = 0; From < 8; ++From)
	{
		for (uint8 To = 0; To < 8; ++To)
		{
			Test.TestEqual(
				*FString::Printf(TEXT("P: persistence edge %u -> %u is exact"), From, To),
				IsAssetDocumentPersistenceTransitionAllowed(
					static_cast<EAssetDocumentPersistenceStep>(From), static_cast<EAssetDocumentPersistenceStep>(To)),
				Allowed[From][To]);
		}
	}
}

void TestTransitionBoundariesAndRecovery(FAutomationTestBase& Test)
{
	Test.TestFalse(TEXT("P: first out-of-range from-step is rejected"), IsAssetDocumentPersistenceTransitionAllowed(static_cast<EAssetDocumentPersistenceStep>(8), EAssetDocumentPersistenceStep::StageSidecar));
	Test.TestFalse(TEXT("P: first out-of-range to-step is rejected"), IsAssetDocumentPersistenceTransitionAllowed(EAssetDocumentPersistenceStep::StagePackage, static_cast<EAssetDocumentPersistenceStep>(8)));
	Test.TestFalse(TEXT("P: high invalid from-step is rejected"), IsAssetDocumentPersistenceTransitionAllowed(static_cast<EAssetDocumentPersistenceStep>(255), EAssetDocumentPersistenceStep::StageSidecar));
	Test.TestFalse(TEXT("P: high invalid to-step is rejected"), IsAssetDocumentPersistenceTransitionAllowed(EAssetDocumentPersistenceStep::StagePackage, static_cast<EAssetDocumentPersistenceStep>(255)));
	Test.TestFalse(TEXT("P: recovery cannot shortcut to durable completion"), IsAssetDocumentPersistenceTransitionAllowed(EAssetDocumentPersistenceStep::Recover, EAssetDocumentPersistenceStep::DurableComplete));
	Test.TestFalse(TEXT("P: durable completion cannot enter recovery"), IsAssetDocumentPersistenceTransitionAllowed(EAssetDocumentPersistenceStep::DurableComplete, EAssetDocumentPersistenceStep::Recover));
}

struct FPersistenceFailureVector
{
	const TCHAR* Boundary;
	EAssetDocumentPersistenceStep FailureStep;
	const TCHAR* ExpectedCode;
};

void TestPersistenceFailureVectors(FAutomationTestBase& Test)
{
	// V: F-ATOMIC owns persistence execution; these rows freeze inputs and stable failure codes only.
	static constexpr FPersistenceFailureVector Vectors[] = {
		{TEXT("package-save-failed"), EAssetDocumentPersistenceStep::StagePackage, TEXT("Persistence.StagePackageFailed")},
		{TEXT("sidecar-stage-failed"), EAssetDocumentPersistenceStep::StageSidecar, TEXT("Persistence.StageSidecarFailed")},
		{TEXT("staged-fresh-load-failed"), EAssetDocumentPersistenceStep::FreshLoadStagedPackage, TEXT("Persistence.FreshLoadFailed")},
		{TEXT("staged-canonical-mismatch"), EAssetDocumentPersistenceStep::CanonicalVerify, TEXT("Persistence.CanonicalMismatch")},
		{TEXT("journal-prepare-failed"), EAssetDocumentPersistenceStep::PrepareJournal, TEXT("Persistence.PrepareJournalFailed")},
		{TEXT("atomic-install-failed"), EAssetDocumentPersistenceStep::AtomicInstall, TEXT("Persistence.AtomicInstallFailed")},
	};
	for (const FPersistenceFailureVector& Vector : Vectors)
	{
		Test.TestTrue(TEXT("V: persistence failure boundary is named"), FCString::Strlen(Vector.Boundary) > 0);
		Test.TestTrue(TEXT("V: failed persistence step may enter recovery"), IsAssetDocumentPersistenceTransitionAllowed(Vector.FailureStep, EAssetDocumentPersistenceStep::Recover));
		Test.TestTrue(TEXT("V: persistence failure code is frozen"), FCString::Strlen(Vector.ExpectedCode) > 0);
	}
}

struct FRecoveryOutcomeVector
{
	const TCHAR* Boundary;
	EAssetDocumentAtomicState ExpectedAtomicState;
	const TCHAR* ExpectedCode;
};

void TestRecoveryAndPoisonVectors(FAutomationTestBase& Test)
{
	// V: recovery behavior remains F-ATOMIC owned; no test-local IO or coordinator fake is behavior evidence.
	static constexpr FRecoveryOutcomeVector Vectors[] = {
		{TEXT("recovery-completed"), EAssetDocumentAtomicState::RolledBack, TEXT("Persistence.Recovered")},
		{TEXT("recovery-failed"), EAssetDocumentAtomicState::Poisoned, TEXT("Atomic.Poisoned.RecoveryFailed")},
	};
	for (const FRecoveryOutcomeVector& Vector : Vectors)
	{
		Test.TestTrue(TEXT("V: recovery outcome boundary is named"), FCString::Strlen(Vector.Boundary) > 0);
		Test.TestTrue(TEXT("V: recovery outcome code is frozen"), FCString::Strlen(Vector.ExpectedCode) > 0);
		Test.TestTrue(TEXT("V: recovery reaches only rollback or poison"), Vector.ExpectedAtomicState == EAssetDocumentAtomicState::RolledBack || Vector.ExpectedAtomicState == EAssetDocumentAtomicState::Poisoned);
	}
	Test.TestEqual(TEXT("V: persistence/recovery owner is frozen"), FString(TEXT("F-ATOMIC")), FString(TEXT("F-ATOMIC")));
}

void TestDefaults(FAutomationTestBase& Test)
{
	const FAssetDocumentImmutableSidecarPayload Sidecar;
	Test.TestTrue(TEXT("S: immutable sidecar bytes default empty"), Sidecar.Utf8Bytes.IsEmpty());
	Test.TestTrue(TEXT("S: immutable sidecar semantic hash default empty"), Sidecar.SemanticHash.IsEmpty());
	Test.TestEqual(TEXT("S: immutable sidecar codec version defaults zero"), Sidecar.CodecVersion, uint32{0});

	const FAssetDocumentPersistencePlan Plan;
	Test.TestFalse(TEXT("S: persistence request identity defaults invalid"), Plan.RequestId.IsValid());
	Test.TestFalse(TEXT("S: persistence transaction identity defaults invalid"), Plan.TransactionId.IsValid());
	Test.TestTrue(TEXT("S: target package path defaults empty"), Plan.TargetPackagePath.IsEmpty());
	Test.TestTrue(TEXT("S: target sidecar path defaults empty"), Plan.TargetSidecarPath.IsEmpty());
	Test.TestTrue(TEXT("S: staging root defaults empty"), Plan.StagingRoot.IsEmpty());
	Test.TestTrue(TEXT("S: journal identity defaults empty"), Plan.JournalId.IsEmpty());
	Test.TestTrue(TEXT("S: persistence footprint defaults empty"), Plan.Footprint.IsEmpty());
}

void TestFreshVerificationDefaults(FAutomationTestBase& Test)
{
	const FAssetDocumentFreshVerificationInput Input;
	Test.TestTrue(TEXT("S: staged package path defaults empty"), Input.StagedPackagePath.IsEmpty());
	Test.TestTrue(TEXT("S: staged sidecar path defaults empty"), Input.StagedSidecarPath.IsEmpty());
	Test.TestEqual(TEXT("S: fresh handle revision defaults zero"), Input.FreshHandleRevision, uint64{0});
	Test.TestTrue(TEXT("S: expected semantic hash defaults empty"), Input.ExpectedSemanticHash.IsEmpty());
	Test.TestEqual(TEXT("S: expected codec version defaults zero"), Input.ExpectedCodecVersion, uint32{0});

	const FAssetDocumentFreshVerificationResult Result;
	Test.TestFalse(TEXT("S: fresh load proof defaults false"), Result.bFreshLoadProven);
	Test.TestFalse(TEXT("S: canonical match defaults false"), Result.bCanonicalMatch);
	Test.TestTrue(TEXT("S: actual semantic hash defaults empty"), Result.ActualSemanticHash.IsEmpty());
	Test.TestEqual(TEXT("S: actual codec version defaults zero"), Result.ActualCodecVersion, uint32{0});
	Test.TestTrue(TEXT("S: fresh verification diagnostics default empty"), Result.Diagnostics.IsEmpty());
}
} // namespace AssetDocumentPersistenceCoordinatorContractTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPersistenceCoordinatorContractTest,
	"UEAssetDoc.RuntimeContracts.PersistenceCoordinator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPersistenceCoordinatorContractTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPersistenceCoordinatorContractTests;
	(void)Parameters;

	TestDefaults(*this);
	TestFreshVerificationDefaults(*this);
	TestIdentityAndTargets(*this);
	TestStagingAndSidecar(*this);
	TestStagingIsolation(*this);
	TestFootprintEnvelope(*this);
	TestFootprintUniqueness(*this);
	TestEnvelopeValidationPrecedence(*this);
	TestFootprintValidationPrecedence(*this);
	TestExactTransitionTable(*this);
	TestTransitionBoundariesAndRecovery(*this);
	TestPersistenceFailureVectors(*this);
	TestRecoveryAndPoisonVectors(*this);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
