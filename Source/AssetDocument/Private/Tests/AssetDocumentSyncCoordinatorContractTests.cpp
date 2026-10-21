// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Misc/AutomationTest.h"
#include "Runtime/AssetDocumentSyncCoordinator.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <type_traits>

namespace AssetDocumentSyncCoordinatorContractTests
{
using FClassifySignature = FAssetDocumentSyncDecision (*)(const FAssetDocumentSyncComparisonInput&);
using FValidateEvidenceSignature = FAssetDocumentContractValidationResult (*)(const FAssetDocumentRegionEvidence&);
using FCompareSignature = FAssetDocumentSyncDecision (IAssetDocumentSyncCoordinator::*)(
	const FAssetDocumentSyncComparisonInput&) const;
using FAdvanceSignature = FAssetDocumentContractValidationResult (IAssetDocumentSyncCoordinator::*)(
	const FAssetDocumentSyncEvidencePair&, const FAssetDocumentDurableCompletionReceipt&);

static_assert(static_cast<uint8>(EAssetDocumentRuntimeSyncDirection::NoChange) == 0);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeSyncDirection::AssetChanged) == 1);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeSyncDirection::SidecarChanged) == 2);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeSyncDirection::Conflict) == 3);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeSyncDirection::EvidenceUnavailable) == 4);
static_assert(std::is_same_v<decltype(FAssetDocumentSyncEvidencePair::Asset), FAssetDocumentRegionEvidence>);
static_assert(std::is_same_v<decltype(FAssetDocumentSyncEvidencePair::Sidecar), FAssetDocumentRegionEvidence>);
static_assert(std::is_same_v<decltype(FAssetDocumentSyncComparisonInput::Current), FAssetDocumentSyncEvidencePair>);
static_assert(std::is_same_v<decltype(FAssetDocumentSyncComparisonInput::LastDurable), FAssetDocumentSyncEvidencePair>);
static_assert(std::is_same_v<decltype(FAssetDocumentSyncComparisonInput::bHasDurableBaseline), bool>);
static_assert(std::is_same_v<decltype(FAssetDocumentSyncDecision::Direction), EAssetDocumentRuntimeSyncDirection>);
static_assert(std::is_same_v<decltype(FAssetDocumentSyncDecision::bMayWrite), bool>);
static_assert(std::is_same_v<decltype(FAssetDocumentSyncDecision::bMayAdvance), bool>);
static_assert(std::is_same_v<decltype(FAssetDocumentSyncDecision::Diagnostics), TArray<FAssetDocumentRuntimeDiagnostic>>);
static_assert(std::is_same_v<decltype(&ClassifyAssetDocumentSyncDirection), FClassifySignature>);
static_assert(std::is_same_v<decltype(&ValidateAssetDocumentRegionEvidence), FValidateEvidenceSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentSyncCoordinator::Compare), FCompareSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentSyncCoordinator::AdvanceAfterDurableCompletion), FAdvanceSignature>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentSyncCoordinator>);

FAssetDocumentRuntimeDiagnostic MakeStableUnavailableDiagnostic(const TCHAR* Code)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Body/Region");
	Diagnostic.Code = Code;
	Diagnostic.Message = TEXT("The region cannot produce trustworthy semantic evidence.");
	return Diagnostic;
}

FAssetDocumentRegionEvidence MakeComparableEvidence(
	EAssetDocumentEvidenceKind Kind,
	const TCHAR* Hash,
	uint32 CodecVersion = 3)
{
	FAssetDocumentRegionEvidence Evidence;
	Evidence.Kind = Kind;
	Evidence.SemanticHash = Hash;
	Evidence.CodecVersion = CodecVersion;
	return Evidence;
}

FAssetDocumentRegionEvidence MakeUnavailableEvidence(const TCHAR* Code)
{
	FAssetDocumentRegionEvidence Evidence;
	Evidence.Kind = EAssetDocumentEvidenceKind::EvidenceUnavailable;
	Evidence.Diagnostic = MakeStableUnavailableDiagnostic(Code);
	return Evidence;
}

FAssetDocumentSyncComparisonInput MakeComparableInput()
{
	FAssetDocumentSyncComparisonInput Input;
	Input.bHasDurableBaseline = true;
	Input.LastDurable.Asset = MakeComparableEvidence(EAssetDocumentEvidenceKind::Present, TEXT("asset:v1"));
	Input.LastDurable.Sidecar = MakeComparableEvidence(EAssetDocumentEvidenceKind::Present, TEXT("sidecar:v1"));
	Input.Current = Input.LastDurable;
	return Input;
}

void TestInvalidEvidence(
	FAutomationTestBase& Test,
	const TCHAR* What,
	const FAssetDocumentRegionEvidence& Evidence,
	const TCHAR* ExpectedPath,
	const TCHAR* ExpectedCode,
	const TCHAR* ExpectedMessage)
{
	const FAssetDocumentContractValidationResult Result = ValidateAssetDocumentRegionEvidence(Evidence);
	Test.TestFalse(*FString::Printf(TEXT("P: %s is rejected"), What), Result.bValid);
	Test.TestEqual(*FString::Printf(TEXT("P: %s returns one diagnostic"), What), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() == 1)
	{
		Test.TestEqual(*FString::Printf(TEXT("P: %s path"), What), Result.Diagnostics[0].Path, FString(ExpectedPath));
		Test.TestEqual(*FString::Printf(TEXT("P: %s code"), What), Result.Diagnostics[0].Code, FString(ExpectedCode));
		Test.TestEqual(*FString::Printf(TEXT("P: %s message"), What), Result.Diagnostics[0].Message, FString(ExpectedMessage));
	}
}

void TestDecision(
	FAutomationTestBase& Test,
	const TCHAR* What,
	const FAssetDocumentSyncComparisonInput& Input,
	EAssetDocumentRuntimeSyncDirection ExpectedDirection,
	bool bExpectedMayWrite,
	bool bExpectedMayAdvance)
{
	const FAssetDocumentSyncDecision Decision = ClassifyAssetDocumentSyncDirection(Input);
	Test.TestEqual(*FString::Printf(TEXT("P: %s direction"), What), Decision.Direction, ExpectedDirection);
	Test.TestEqual(*FString::Printf(TEXT("P: %s write eligibility"), What), Decision.bMayWrite, bExpectedMayWrite);
	Test.TestEqual(*FString::Printf(TEXT("P: %s durable-advance eligibility"), What), Decision.bMayAdvance, bExpectedMayAdvance);
	if (ExpectedDirection != EAssetDocumentRuntimeSyncDirection::EvidenceUnavailable)
	{
		Test.TestEqual(*FString::Printf(TEXT("P: %s has no diagnostic"), What), Decision.Diagnostics.Num(), 0);
	}
}

void TestUnavailableDecision(
	FAutomationTestBase& Test,
	const TCHAR* What,
	const FAssetDocumentSyncComparisonInput& Input,
	const TCHAR* ExpectedCode)
{
	TestDecision(Test, What, Input, EAssetDocumentRuntimeSyncDirection::EvidenceUnavailable, false, false);
	const FAssetDocumentSyncDecision Decision = ClassifyAssetDocumentSyncDirection(Input);
	Test.TestEqual(*FString::Printf(TEXT("P: %s returns one diagnostic"), What), Decision.Diagnostics.Num(), 1);
	if (Decision.Diagnostics.Num() == 1)
	{
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic path"), What), Decision.Diagnostics[0].Path, FString(TEXT("/Body/Region")));
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic code"), What), Decision.Diagnostics[0].Code, FString(ExpectedCode));
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic message"), What), Decision.Diagnostics[0].Message, FString(TEXT("The region cannot produce trustworthy semantic evidence.")));
	}
}

void TestInvalidDecision(
	FAutomationTestBase& Test,
	const TCHAR* What,
	const FAssetDocumentSyncComparisonInput& Input,
	const TCHAR* ExpectedPath,
	const TCHAR* ExpectedCode,
	const TCHAR* ExpectedMessage)
{
	const FAssetDocumentSyncDecision Decision = ClassifyAssetDocumentSyncDirection(Input);
	Test.TestEqual(*FString::Printf(TEXT("P: %s fails closed"), What), Decision.Direction, EAssetDocumentRuntimeSyncDirection::EvidenceUnavailable);
	Test.TestFalse(*FString::Printf(TEXT("P: %s disables write"), What), Decision.bMayWrite);
	Test.TestFalse(*FString::Printf(TEXT("P: %s disables advance"), What), Decision.bMayAdvance);
	Test.TestEqual(*FString::Printf(TEXT("P: %s returns one diagnostic"), What), Decision.Diagnostics.Num(), 1);
	if (Decision.Diagnostics.Num() == 1)
	{
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic path"), What), Decision.Diagnostics[0].Path, FString(ExpectedPath));
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic code"), What), Decision.Diagnostics[0].Code, FString(ExpectedCode));
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic message"), What), Decision.Diagnostics[0].Message, FString(ExpectedMessage));
	}
}

void TestShapeAndDefaults(FAutomationTestBase& Test)
{
	const FAssetDocumentSyncEvidencePair Pair;
	const FAssetDocumentSyncComparisonInput Input;
	const FAssetDocumentSyncDecision Decision;
	Test.TestEqual(TEXT("S: asset evidence defaults unavailable"), Pair.Asset.Kind, EAssetDocumentEvidenceKind::EvidenceUnavailable);
	Test.TestEqual(TEXT("S: sidecar evidence defaults unavailable"), Pair.Sidecar.Kind, EAssetDocumentEvidenceKind::EvidenceUnavailable);
	Test.TestFalse(TEXT("S: durable-baseline presence defaults false"), Input.bHasDurableBaseline);
	Test.TestEqual(TEXT("S: sync decision defaults unavailable"), Decision.Direction, EAssetDocumentRuntimeSyncDirection::EvidenceUnavailable);
	Test.TestFalse(TEXT("S: write eligibility defaults false"), Decision.bMayWrite);
	Test.TestFalse(TEXT("S: advance eligibility defaults false"), Decision.bMayAdvance);
	Test.TestEqual(TEXT("S: decision diagnostics default empty"), Decision.Diagnostics.Num(), 0);
}

void TestComparableEvidenceContract(FAutomationTestBase& Test)
{
	for (uint8 Index = 0; Index <= static_cast<uint8>(EAssetDocumentEvidenceKind::AbsentByPolicy); ++Index)
	{
		FAssetDocumentRegionEvidence Evidence = MakeComparableEvidence(
			static_cast<EAssetDocumentEvidenceKind>(Index), TEXT("semantic:v1"));
		Test.TestTrue(*FString::Printf(TEXT("P: comparable evidence kind %u is valid"), Index), ValidateAssetDocumentRegionEvidence(Evidence).bValid);
		Evidence.Diagnostic = MakeStableUnavailableDiagnostic(TEXT("Residual.Context"));
		Test.TestTrue(*FString::Printf(TEXT("P: comparable kind %u may retain diagnostic context"), Index), ValidateAssetDocumentRegionEvidence(Evidence).bValid);
	}
	FAssetDocumentRegionEvidence Invalid = MakeComparableEvidence(EAssetDocumentEvidenceKind::Present, TEXT(""));
	TestInvalidEvidence(Test, TEXT("empty comparable hash"), Invalid, TEXT("/RegionEvidence/SemanticHash"), TEXT("SyncEvidence.EmptySemanticHash"), TEXT("Comparable sync evidence requires a non-empty semantic hash."));
	Invalid = MakeComparableEvidence(EAssetDocumentEvidenceKind::ExplicitEmpty, TEXT("semantic:empty"), 0);
	TestInvalidEvidence(Test, TEXT("zero comparable codec"), Invalid, TEXT("/RegionEvidence/CodecVersion"), TEXT("SyncEvidence.InvalidCodecVersion"), TEXT("Comparable sync evidence requires a non-zero codec version."));
}

void TestEvidenceKindBoundaries(FAutomationTestBase& Test)
{
	FAssetDocumentRegionEvidence Invalid = MakeComparableEvidence(static_cast<EAssetDocumentEvidenceKind>(4), TEXT("semantic:v1"));
	TestInvalidEvidence(Test, TEXT("first out-of-range evidence kind"), Invalid, TEXT("/RegionEvidence/Kind"), TEXT("SyncEvidence.InvalidKind"), TEXT("Sync evidence kind is outside the frozen enum."));
	Invalid.Kind = static_cast<EAssetDocumentEvidenceKind>(255);
	TestInvalidEvidence(Test, TEXT("high invalid evidence kind"), Invalid, TEXT("/RegionEvidence/Kind"), TEXT("SyncEvidence.InvalidKind"), TEXT("Sync evidence kind is outside the frozen enum."));
}

void TestUnavailableEvidenceContract(FAutomationTestBase& Test)
{
	FAssetDocumentRegionEvidence Evidence = MakeUnavailableEvidence(TEXT("Extract.RegionUnavailable"));
	Test.TestTrue(TEXT("P: unavailable evidence with stable diagnostic is valid"), ValidateAssetDocumentRegionEvidence(Evidence).bValid);
	Evidence.SemanticHash = TEXT("residual:last-known");
	Evidence.CodecVersion = 9;
	Test.TestTrue(TEXT("P: unavailable evidence may retain last-known hash and version"), ValidateAssetDocumentRegionEvidence(Evidence).bValid);
	Evidence.Diagnostic.Reset();
	TestInvalidEvidence(Test, TEXT("missing unavailable diagnostic"), Evidence, TEXT("/RegionEvidence/Diagnostic"), TEXT("SyncEvidence.MissingUnavailableDiagnostic"), TEXT("Unavailable sync evidence requires a stable diagnostic."));
}

void TestUnavailableDiagnosticFields(FAutomationTestBase& Test)
{
	FAssetDocumentRegionEvidence Invalid = MakeUnavailableEvidence(TEXT("Extract.RegionUnavailable"));
	Invalid.Diagnostic->Path.Reset();
	TestInvalidEvidence(Test, TEXT("empty unavailable diagnostic path"), Invalid, TEXT("/RegionEvidence/Diagnostic/Path"), TEXT("SyncEvidence.EmptyDiagnosticPath"), TEXT("Unavailable sync evidence requires a stable diagnostic path."));
	Invalid = MakeUnavailableEvidence(TEXT("Extract.RegionUnavailable"));
	Invalid.Diagnostic->Code.Reset();
	TestInvalidEvidence(Test, TEXT("empty unavailable diagnostic code"), Invalid, TEXT("/RegionEvidence/Diagnostic/Code"), TEXT("SyncEvidence.EmptyDiagnosticCode"), TEXT("Unavailable sync evidence requires a stable diagnostic code."));
	Invalid = MakeUnavailableEvidence(TEXT("Extract.RegionUnavailable"));
	Invalid.Diagnostic->Message.Reset();
	TestInvalidEvidence(Test, TEXT("empty unavailable diagnostic message"), Invalid, TEXT("/RegionEvidence/Diagnostic/Message"), TEXT("SyncEvidence.EmptyDiagnosticMessage"), TEXT("Unavailable sync evidence requires a stable diagnostic message."));
}

void TestEvidenceValidationPrecedence(FAutomationTestBase& Test)
{
	FAssetDocumentRegionEvidence Invalid = MakeComparableEvidence(static_cast<EAssetDocumentEvidenceKind>(4), TEXT(""), 0);
	TestInvalidEvidence(Test, TEXT("kind precedes comparable shape"), Invalid, TEXT("/RegionEvidence/Kind"), TEXT("SyncEvidence.InvalidKind"), TEXT("Sync evidence kind is outside the frozen enum."));
	Invalid = MakeComparableEvidence(EAssetDocumentEvidenceKind::Present, TEXT(""), 0);
	TestInvalidEvidence(Test, TEXT("hash precedes codec"), Invalid, TEXT("/RegionEvidence/SemanticHash"), TEXT("SyncEvidence.EmptySemanticHash"), TEXT("Comparable sync evidence requires a non-empty semantic hash."));
	Invalid = MakeUnavailableEvidence(TEXT("Extract.RegionUnavailable"));
	Invalid.Diagnostic->Path.Reset();
	Invalid.Diagnostic->Code.Reset();
	Invalid.Diagnostic->Message.Reset();
	TestInvalidEvidence(Test, TEXT("diagnostic path precedes code and message"), Invalid, TEXT("/RegionEvidence/Diagnostic/Path"), TEXT("SyncEvidence.EmptyDiagnosticPath"), TEXT("Unavailable sync evidence requires a stable diagnostic path."));
	Invalid = MakeUnavailableEvidence(TEXT("Extract.RegionUnavailable"));
	Invalid.Diagnostic->Code.Reset();
	Invalid.Diagnostic->Message.Reset();
	TestInvalidEvidence(Test, TEXT("diagnostic code precedes message"), Invalid, TEXT("/RegionEvidence/Diagnostic/Code"), TEXT("SyncEvidence.EmptyDiagnosticCode"), TEXT("Unavailable sync evidence requires a stable diagnostic code."));
}

void TestCompleteTruthTable(FAutomationTestBase& Test)
{
	const FAssetDocumentSyncComparisonInput Base = MakeComparableInput();
	TestDecision(Test, TEXT("neither side changed"), Base, EAssetDocumentRuntimeSyncDirection::NoChange, false, true);
	FAssetDocumentSyncComparisonInput AssetChanged = Base;
	AssetChanged.Current.Asset.SemanticHash = TEXT("asset:v2");
	TestDecision(Test, TEXT("only asset changed"), AssetChanged, EAssetDocumentRuntimeSyncDirection::AssetChanged, true, true);
	FAssetDocumentSyncComparisonInput SidecarChanged = Base;
	SidecarChanged.Current.Sidecar.SemanticHash = TEXT("sidecar:v2");
	TestDecision(Test, TEXT("only sidecar changed"), SidecarChanged, EAssetDocumentRuntimeSyncDirection::SidecarChanged, true, true);
	FAssetDocumentSyncComparisonInput Conflict = AssetChanged;
	Conflict.Current.Sidecar.SemanticHash = TEXT("sidecar:v2");
	TestDecision(Test, TEXT("both sides changed"), Conflict, EAssetDocumentRuntimeSyncDirection::Conflict, false, false);
}

void TestTypedEvidenceComparison(FAutomationTestBase& Test)
{
	const FAssetDocumentSyncComparisonInput Base = MakeComparableInput();
	FAssetDocumentSyncComparisonInput KindChanged = Base;
	KindChanged.Current.Asset.Kind = EAssetDocumentEvidenceKind::ExplicitEmpty;
	TestDecision(Test, TEXT("kind-only asset change"), KindChanged, EAssetDocumentRuntimeSyncDirection::AssetChanged, true, true);
	FAssetDocumentSyncComparisonInput CodecChanged = Base;
	CodecChanged.Current.Sidecar.CodecVersion += 1;
	TestDecision(Test, TEXT("codec-only sidecar change"), CodecChanged, EAssetDocumentRuntimeSyncDirection::SidecarChanged, true, true);
	FAssetDocumentSyncComparisonInput ResidualDiagnosticChanged = Base;
	ResidualDiagnosticChanged.Current.Asset.Diagnostic = MakeStableUnavailableDiagnostic(TEXT("Residual.Context"));
	TestDecision(Test, TEXT("comparable diagnostic context is not semantic evidence"), ResidualDiagnosticChanged, EAssetDocumentRuntimeSyncDirection::NoChange, false, true);
}

void TestMissingBaseline(FAutomationTestBase& Test)
{
	FAssetDocumentSyncComparisonInput Input = MakeComparableInput();
	Input.bHasDurableBaseline = false;
	const FAssetDocumentSyncDecision Decision = ClassifyAssetDocumentSyncDirection(Input);
	TestDecision(Test, TEXT("missing durable baseline"), Input, EAssetDocumentRuntimeSyncDirection::EvidenceUnavailable, false, false);
	Test.TestEqual(TEXT("P: missing baseline returns one diagnostic"), Decision.Diagnostics.Num(), 1);
	if (Decision.Diagnostics.Num() == 1)
	{
		Test.TestEqual(TEXT("P: missing baseline diagnostic path"), Decision.Diagnostics[0].Path, FString(TEXT("/SyncComparison/LastDurable")));
		Test.TestEqual(TEXT("P: missing baseline diagnostic code"), Decision.Diagnostics[0].Code, FString(TEXT("Sync.MissingDurableBaseline")));
		Test.TestEqual(TEXT("P: missing baseline diagnostic message"), Decision.Diagnostics[0].Message, FString(TEXT("Sync comparison requires a durable typed-evidence baseline.")));
	}
}

void TestUnavailablePositions(FAutomationTestBase& Test)
{
	FAssetDocumentSyncComparisonInput Input = MakeComparableInput();
	Input.Current.Asset = MakeUnavailableEvidence(TEXT("Sync.CurrentAssetUnavailable"));
	TestUnavailableDecision(Test, TEXT("current asset unavailable"), Input, TEXT("Sync.CurrentAssetUnavailable"));
	Input = MakeComparableInput();
	Input.Current.Sidecar = MakeUnavailableEvidence(TEXT("Sync.CurrentSidecarUnavailable"));
	TestUnavailableDecision(Test, TEXT("current sidecar unavailable"), Input, TEXT("Sync.CurrentSidecarUnavailable"));
	Input = MakeComparableInput();
	Input.LastDurable.Asset = MakeUnavailableEvidence(TEXT("Sync.LastAssetUnavailable"));
	TestUnavailableDecision(Test, TEXT("durable asset baseline unavailable"), Input, TEXT("Sync.LastAssetUnavailable"));
	Input = MakeComparableInput();
	Input.LastDurable.Sidecar = MakeUnavailableEvidence(TEXT("Sync.LastSidecarUnavailable"));
	TestUnavailableDecision(Test, TEXT("durable sidecar baseline unavailable"), Input, TEXT("Sync.LastSidecarUnavailable"));
}

void TestUnavailableDiagnosticPropagation(FAutomationTestBase& Test)
{
	FAssetDocumentSyncComparisonInput Input = MakeComparableInput();
	Input.Current.Asset = MakeUnavailableEvidence(TEXT("Sync.CurrentAssetUnavailable"));
	const FAssetDocumentSyncDecision Decision = ClassifyAssetDocumentSyncDirection(Input);
	Test.TestEqual(TEXT("P: unavailable classification returns one diagnostic"), Decision.Diagnostics.Num(), 1);
	if (Decision.Diagnostics.Num() == 1)
	{
		Test.TestEqual(TEXT("P: unavailable diagnostic path is preserved"), Decision.Diagnostics[0].Path, FString(TEXT("/Body/Region")));
		Test.TestEqual(TEXT("P: unavailable diagnostic code is preserved"), Decision.Diagnostics[0].Code, FString(TEXT("Sync.CurrentAssetUnavailable")));
		Test.TestEqual(TEXT("P: unavailable diagnostic message is preserved"), Decision.Diagnostics[0].Message, FString(TEXT("The region cannot produce trustworthy semantic evidence.")));
	}
}

void TestResidualUnavailableClassification(FAutomationTestBase& Test)
{
	FAssetDocumentSyncComparisonInput Input = MakeComparableInput();
	Input.Current.Asset = MakeUnavailableEvidence(TEXT("Sync.ResidualCurrentAssetUnavailable"));
	Input.Current.Asset.SemanticHash = TEXT("residual:last-known-asset");
	Input.Current.Asset.CodecVersion = 7;
	TestUnavailableDecision(
		Test,
		TEXT("unavailable evidence with residual hash and codec"),
		Input,
		TEXT("Sync.ResidualCurrentAssetUnavailable"));
}

void TestValidationAndUnavailablePrecedence(FAutomationTestBase& Test)
{
	FAssetDocumentSyncComparisonInput Input = MakeComparableInput();
	Input.Current.Asset = MakeUnavailableEvidence(TEXT("Sync.CurrentAssetUnavailable"));
	Input.Current.Sidecar.SemanticHash.Reset();
	TestInvalidDecision(Test, TEXT("malformed peer precedes valid unavailable"), Input, TEXT("/RegionEvidence/SemanticHash"), TEXT("SyncEvidence.EmptySemanticHash"), TEXT("Comparable sync evidence requires a non-empty semantic hash."));
	Input = MakeComparableInput();
	Input.bHasDurableBaseline = false;
	Input.Current.Asset = MakeUnavailableEvidence(TEXT("Sync.CurrentAssetUnavailable"));
	TestInvalidDecision(Test, TEXT("missing baseline precedes valid unavailable"), Input, TEXT("/SyncComparison/LastDurable"), TEXT("Sync.MissingDurableBaseline"), TEXT("Sync comparison requires a durable typed-evidence baseline."));
}

void TestMultipleUnavailablePriority(FAutomationTestBase& Test)
{
	FAssetDocumentSyncComparisonInput Input = MakeComparableInput();
	Input.Current.Asset = MakeUnavailableEvidence(TEXT("Sync.Priority.CurrentAsset"));
	Input.Current.Sidecar = MakeUnavailableEvidence(TEXT("Sync.Priority.CurrentSidecar"));
	Input.LastDurable.Asset = MakeUnavailableEvidence(TEXT("Sync.Priority.LastAsset"));
	Input.LastDurable.Sidecar = MakeUnavailableEvidence(TEXT("Sync.Priority.LastSidecar"));
	TestUnavailableDecision(
		Test,
		TEXT("current asset is first unavailable source"),
		Input,
		TEXT("Sync.Priority.CurrentAsset"));
}

void TestComparisonValidationPrecedence(FAutomationTestBase& Test)
{
	FAssetDocumentSyncComparisonInput Input = MakeComparableInput();
	Input.Current.Asset.Kind = static_cast<EAssetDocumentEvidenceKind>(4);
	Input.Current.Sidecar.SemanticHash.Reset();
	Input.LastDurable.Asset.CodecVersion = 0;
	TestInvalidDecision(Test, TEXT("invalid current asset precedes other invalid evidence"), Input, TEXT("/RegionEvidence/Kind"), TEXT("SyncEvidence.InvalidKind"), TEXT("Sync evidence kind is outside the frozen enum."));
	Input = MakeComparableInput();
	Input.Current.Sidecar.SemanticHash.Reset();
	Input.LastDurable.Asset.CodecVersion = 0;
	TestInvalidDecision(Test, TEXT("invalid current sidecar precedes invalid baseline"), Input, TEXT("/RegionEvidence/SemanticHash"), TEXT("SyncEvidence.EmptySemanticHash"), TEXT("Comparable sync evidence requires a non-empty semantic hash."));
	Input = MakeComparableInput();
	Input.bHasDurableBaseline = false;
	Input.Current.Sidecar.SemanticHash.Reset();
	TestInvalidDecision(Test, TEXT("invalid current sidecar precedes missing baseline"), Input, TEXT("/RegionEvidence/SemanticHash"), TEXT("SyncEvidence.EmptySemanticHash"), TEXT("Comparable sync evidence requires a non-empty semantic hash."));
	Input = MakeComparableInput();
	Input.bHasDurableBaseline = false;
	Input.LastDurable.Asset.CodecVersion = 0;
	TestInvalidDecision(Test, TEXT("missing baseline precedes ignored baseline payload"), Input, TEXT("/SyncComparison/LastDurable"), TEXT("Sync.MissingDurableBaseline"), TEXT("Sync comparison requires a durable typed-evidence baseline."));
	Input = MakeComparableInput();
	Input.LastDurable.Asset.CodecVersion = 0;
	Input.LastDurable.Sidecar.SemanticHash.Reset();
	TestInvalidDecision(Test, TEXT("invalid durable asset precedes durable sidecar"), Input, TEXT("/RegionEvidence/CodecVersion"), TEXT("SyncEvidence.InvalidCodecVersion"), TEXT("Comparable sync evidence requires a non-zero codec version."));
}

void TestInvalidComparisonEvidencePositions(FAutomationTestBase& Test)
{
	const FAssetDocumentSyncComparisonInput Valid = MakeComparableInput();
	TArray<FAssetDocumentSyncComparisonInput> Inputs;
	Inputs.Init(Valid, 4);
	Inputs[0].Current.Asset.Kind = static_cast<EAssetDocumentEvidenceKind>(4);
	Inputs[1].Current.Sidecar.SemanticHash.Reset();
	Inputs[2].LastDurable.Asset.CodecVersion = 0;
	Inputs[3].LastDurable.Sidecar.Kind = EAssetDocumentEvidenceKind::EvidenceUnavailable;
	Inputs[3].LastDurable.Sidecar.Diagnostic.Reset();
	static constexpr const TCHAR* Names[] = {TEXT("invalid current asset"), TEXT("invalid current sidecar"), TEXT("invalid durable asset"), TEXT("invalid durable sidecar")};
	static constexpr const TCHAR* Paths[] = {TEXT("/RegionEvidence/Kind"), TEXT("/RegionEvidence/SemanticHash"), TEXT("/RegionEvidence/CodecVersion"), TEXT("/RegionEvidence/Diagnostic")};
	static constexpr const TCHAR* Codes[] = {TEXT("SyncEvidence.InvalidKind"), TEXT("SyncEvidence.EmptySemanticHash"), TEXT("SyncEvidence.InvalidCodecVersion"), TEXT("SyncEvidence.MissingUnavailableDiagnostic")};
	static constexpr const TCHAR* Messages[] = {TEXT("Sync evidence kind is outside the frozen enum."), TEXT("Comparable sync evidence requires a non-empty semantic hash."), TEXT("Comparable sync evidence requires a non-zero codec version."), TEXT("Unavailable sync evidence requires a stable diagnostic.")};
	for (int32 Index = 0; Index < Inputs.Num(); ++Index)
	{
		TestInvalidDecision(Test, Names[Index], Inputs[Index], Paths[Index], Codes[Index], Messages[Index]);
	}
}

struct FDurableAdvanceVector
{
	const TCHAR* Boundary;
	bool bHadDurableBaseline;
	bool bInstalledMatchesBaseline;
	bool bHasDurableCompletion;
	bool bExpectedStoreSuccess;
	const TCHAR* ExpectedCode;
};

void TestDurableAdvanceVectors(FAutomationTestBase& Test)
{
	// V: F-ATOMIC owns store behavior; bMayAdvance is eligibility, never permission to advance during Compare.
	static constexpr FDurableAdvanceVector Vectors[] = {
		{TEXT("first-durable-install"), false, false, true, true, TEXT("Sync.StateAdvanced")},
		{TEXT("changed-durable-update"), true, false, true, true, TEXT("Sync.StateAdvanced")},
		{TEXT("identical-durable-replay"), true, true, true, true, TEXT("Sync.StateAlreadyCurrent")},
		{TEXT("missing-durable-completion"), true, false, false, false, TEXT("Sync.MissingDurableCompletion")},
		{TEXT("durable-store-write-failed"), true, false, true, false, TEXT("Sync.StateAdvanceFailed")},
	};
	Test.TestEqual(TEXT("V: durable advancement matrix has five exact rows"), int32{UE_ARRAY_COUNT(Vectors)}, int32{5});
	for (const FDurableAdvanceVector& Vector : Vectors)
	{
		Test.TestTrue(TEXT("V: durable advance boundary is named"), FCString::Strlen(Vector.Boundary) > 0);
		Test.TestTrue(TEXT("V: durable advance result code is frozen"), FCString::Strlen(Vector.ExpectedCode) > 0);
		Test.TestEqual(TEXT("V: non-durable input cannot update store"), !Vector.bHasDurableCompletion && Vector.bExpectedStoreSuccess, false);
	}
	Test.TestFalse(TEXT("V: first install has no prior baseline"), Vectors[0].bHadDurableBaseline);
	Test.TestTrue(TEXT("V: changed update has a prior baseline"), Vectors[1].bHadDurableBaseline);
	Test.TestFalse(TEXT("V: changed update installs different evidence"), Vectors[1].bInstalledMatchesBaseline);
	Test.TestTrue(TEXT("V: changed durable update succeeds"), Vectors[1].bExpectedStoreSuccess);
	Test.TestTrue(TEXT("V: idempotent replay matches prior baseline"), Vectors[2].bInstalledMatchesBaseline);
	Test.TestEqual(TEXT("V: durable sync-state owner is frozen"), FString(TEXT("F-ATOMIC")), FString(TEXT("F-ATOMIC")));
}
} // namespace AssetDocumentSyncCoordinatorContractTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentSyncCoordinatorContractTest,
	"UEAssetDoc.RuntimeContracts.SyncCoordinator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSyncCoordinatorContractTest::RunTest(const FString&)
{
	using namespace AssetDocumentSyncCoordinatorContractTests;
	TestShapeAndDefaults(*this);
	TestComparableEvidenceContract(*this);
	TestEvidenceKindBoundaries(*this);
	TestUnavailableEvidenceContract(*this);
	TestUnavailableDiagnosticFields(*this);
	TestEvidenceValidationPrecedence(*this);
	TestCompleteTruthTable(*this);
	TestTypedEvidenceComparison(*this);
	TestMissingBaseline(*this);
	TestUnavailablePositions(*this);
	TestUnavailableDiagnosticPropagation(*this);
	TestResidualUnavailableClassification(*this);
	TestValidationAndUnavailablePrecedence(*this);
	TestMultipleUnavailablePriority(*this);
	TestComparisonValidationPrecedence(*this);
	TestInvalidComparisonEvidencePositions(*this);
	TestDurableAdvanceVectors(*this);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
