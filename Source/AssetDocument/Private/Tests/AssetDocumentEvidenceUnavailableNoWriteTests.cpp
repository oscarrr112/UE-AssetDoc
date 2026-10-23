// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentTypedSyncRuntime.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
class FNoWriteTypedSyncStore final : public IAssetDocumentTypedSyncStateStore
{
public:
	virtual FAssetDocumentContractValidationResult Load(
		const FAssetDocumentSyncStateKey& Key,
		TOptional<FAssetDocumentTypedSyncStateRecord>& OutRecord,
		TArray<uint8>& OutEncodedBytes,
		bool& bOutExisted) override { ++Calls; return {}; }
	virtual FAssetDocumentContractValidationResult Store(
		const FAssetDocumentSyncStateKey& Key,
		const FAssetDocumentTypedSyncStateRecord& Record) override { ++Calls; return {}; }
	virtual FAssetDocumentContractValidationResult Restore(
		const FAssetDocumentSyncStateKey& Key,
		TArrayView64<const uint8> EncodedPreimage,
		bool bExisted) override { ++Calls; return {}; }
	int32 Calls = 0;
};

FAssetDocumentRegionEvidence Comparable(const TCHAR* Hash)
{
	FAssetDocumentRegionEvidence Evidence;
	Evidence.Kind = EAssetDocumentEvidenceKind::Present;
	Evidence.SemanticHash = Hash;
	Evidence.CodecVersion = 3;
	return Evidence;
}

FAssetDocumentRegionEvidence Unavailable(const TCHAR* Code)
{
	FAssetDocumentRegionEvidence Evidence;
	Evidence.Kind = EAssetDocumentEvidenceKind::EvidenceUnavailable;
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Body/Region");
	Diagnostic.Code = Code;
	Diagnostic.Message = TEXT("Evidence is unavailable.");
	Evidence.Diagnostic = MoveTemp(Diagnostic);
	return Evidence;
}

FAssetDocumentSyncComparisonInput ValidInput()
{
	FAssetDocumentSyncComparisonInput Input;
	Input.bHasDurableBaseline = true;
	Input.Current.Asset = Comparable(TEXT("asset"));
	Input.Current.Sidecar = Comparable(TEXT("sidecar"));
	Input.LastDurable = Input.Current;
	return Input;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEvidenceUnavailableNoWriteTest,
	"UEAssetDoc.Sync.EvidenceUnavailableNoWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEvidenceUnavailableNoWriteTest::RunTest(const FString& Parameters)
{
	FNoWriteTypedSyncStore Store;
	FAssetDocumentSyncCoordinatorRuntime Coordinator({TEXT("/Game/Atomic"), TEXT("Body")}, Store);
	for (int32 Position = 0; Position < 4; ++Position)
	{
		FAssetDocumentSyncComparisonInput Input = ValidInput();
		FAssetDocumentRegionEvidence* Positions[] = {
			&Input.Current.Asset, &Input.Current.Sidecar, &Input.LastDurable.Asset, &Input.LastDurable.Sidecar};
		*Positions[Position] = Unavailable(*FString::Printf(TEXT("Sync.Unavailable.%d"), Position));
		const FAssetDocumentSyncDecision Decision = Coordinator.Compare(Input);
		TestEqual(TEXT("unavailable position fails closed"), Decision.Direction,
			EAssetDocumentRuntimeSyncDirection::EvidenceUnavailable);
		TestFalse(TEXT("unavailable position cannot write or advance"), Decision.bMayWrite || Decision.bMayAdvance);
		TestEqual(TEXT("unavailable position preserves one diagnostic"), Decision.Diagnostics.Num(), 1);
		if (Decision.Diagnostics.Num() == 1)
		{
			TestEqual(TEXT("unavailable diagnostic code is propagated"), Decision.Diagnostics[0].Code,
				FString::Printf(TEXT("Sync.Unavailable.%d"), Position));
		}
	}
	FAssetDocumentSyncComparisonInput Malformed = ValidInput();
	Malformed.Current.Asset.SemanticHash.Reset();
	const FAssetDocumentSyncDecision MalformedDecision = Coordinator.Compare(Malformed);
	TestEqual(TEXT("malformed evidence fails closed"), MalformedDecision.Direction,
		EAssetDocumentRuntimeSyncDirection::EvidenceUnavailable);
	TestEqual(TEXT("malformed evidence returns one diagnostic"), MalformedDecision.Diagnostics.Num(), 1);
	if (MalformedDecision.Diagnostics.Num() == 1)
	{
		TestEqual(TEXT("malformed evidence preserves validator diagnostic"), MalformedDecision.Diagnostics[0].Code,
			FString(TEXT("SyncEvidence.EmptySemanticHash")));
	}
	FAssetDocumentDurableCompletionReceipt Receipt;
	FAssetDocumentSyncEvidencePair Installed = ValidInput().Current;
	Installed.Asset = Unavailable(TEXT("Sync.AdvanceUnavailable"));
	const FAssetDocumentContractValidationResult Advance = Coordinator.AdvanceAfterDurableCompletion(Installed, Receipt);
	TestFalse(TEXT("unavailable installed evidence cannot advance"), Advance.bValid);
	TestEqual(TEXT("unavailable advance returns one diagnostic"), Advance.Diagnostics.Num(), 1);
	if (Advance.Diagnostics.Num() == 1)
	{
		TestEqual(TEXT("advance propagates unavailable diagnostic"), Advance.Diagnostics[0].Code,
			FString(TEXT("Sync.AdvanceUnavailable")));
	}
	TestEqual(TEXT("evidence unavailable and malformed paths make zero store calls"), Store.Calls, 0);
	return true;
}

#endif
