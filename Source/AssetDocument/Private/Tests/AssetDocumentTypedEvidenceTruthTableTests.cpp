// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentTypedSyncRuntime.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace TypedEvidenceTruthTableTestsLocal
{
FAssetDocumentContractValidationResult MakeValidResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

class FTypedSyncStoreRecorder final : public IAssetDocumentTypedSyncStateStore
{
public:
	virtual FAssetDocumentContractValidationResult Load(
		const FAssetDocumentSyncStateKey& Key,
		TOptional<FAssetDocumentTypedSyncStateRecord>& OutRecord,
		TArray<uint8>& OutEncodedBytes,
		bool& bOutExisted) override
	{
		++LoadCalls;
		return MakeValidResult();
	}
	virtual FAssetDocumentContractValidationResult Store(
		const FAssetDocumentSyncStateKey& Key,
		const FAssetDocumentTypedSyncStateRecord& Record) override
	{
		++StoreCalls;
		LastKey = Key;
		LastRecord = Record;
		return MakeValidResult();
	}
	virtual FAssetDocumentContractValidationResult Restore(
		const FAssetDocumentSyncStateKey& Key,
		TArrayView64<const uint8> EncodedPreimage,
		bool bExisted) override
	{
		++RestoreCalls;
		return MakeValidResult();
	}

	int32 LoadCalls = 0;
	int32 StoreCalls = 0;
	int32 RestoreCalls = 0;
	FAssetDocumentSyncStateKey LastKey;
	FAssetDocumentTypedSyncStateRecord LastRecord;
};

FAssetDocumentRegionEvidence MakeEvidence(EAssetDocumentEvidenceKind Kind, const TCHAR* Hash)
{
	FAssetDocumentRegionEvidence Evidence;
	Evidence.Kind = Kind;
	Evidence.SemanticHash = Hash;
	Evidence.CodecVersion = 3;
	return Evidence;
}

FAssetDocumentSyncComparisonInput MakeBaseInput(EAssetDocumentEvidenceKind Kind)
{
	FAssetDocumentSyncComparisonInput Input;
	Input.bHasDurableBaseline = true;
	Input.LastDurable.Asset = MakeEvidence(Kind, TEXT("asset:v1"));
	Input.LastDurable.Sidecar = MakeEvidence(Kind, TEXT("sidecar:v1"));
	Input.Current = Input.LastDurable;
	return Input;
}

FAssetDocumentDurableCompletionReceipt MakeReceipt()
{
	FAssetDocumentDurableCompletionReceipt Receipt;
	Receipt.RequestId = FGuid::NewGuid();
	Receipt.TransactionId = FGuid::NewGuid();
	Receipt.JournalId = TEXT("typed-sync");
	Receipt.PackageContentDigest = TEXT("sha256:package");
	Receipt.SidecarContentDigest = TEXT("sha256:sidecar");
	return Receipt;
}

bool EvidenceEqual(const FAssetDocumentRegionEvidence& Left, const FAssetDocumentRegionEvidence& Right)
{
	return Left.Kind == Right.Kind && Left.SemanticHash == Right.SemanticHash
		&& Left.CodecVersion == Right.CodecVersion;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentTypedEvidenceTruthTableTest,
	"UEAssetDoc.Sync.TypedEvidenceTruthTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTypedEvidenceTruthTableTest::RunTest(const FString& Parameters)
{
	using namespace TypedEvidenceTruthTableTestsLocal;
	FTypedSyncStoreRecorder Store;
	const FAssetDocumentSyncStateKey Key{TEXT("/Game/Atomic"), TEXT("Body")};
	FAssetDocumentSyncCoordinatorRuntime Coordinator(Key, Store);
	FAssetDocumentSyncComparisonInput AssetChanged = MakeBaseInput(EAssetDocumentEvidenceKind::Present);
	AssetChanged.Current.Asset.SemanticHash = TEXT("asset:v2");
	TestEqual(TEXT("typed sync compare produces frozen AssetChanged decision"), Coordinator.Compare(AssetChanged).Direction,
		EAssetDocumentRuntimeSyncDirection::AssetChanged);

	const EAssetDocumentEvidenceKind ComparableKinds[] = {
		EAssetDocumentEvidenceKind::Present,
		EAssetDocumentEvidenceKind::ExplicitEmpty,
		EAssetDocumentEvidenceKind::AbsentByPolicy};
	for (EAssetDocumentEvidenceKind Kind : ComparableKinds)
	{
		const FAssetDocumentSyncComparisonInput Base = MakeBaseInput(Kind);
		const FAssetDocumentSyncDecision NoChange = Coordinator.Compare(Base);
		TestEqual(TEXT("comparable kind no-change direction"), NoChange.Direction, EAssetDocumentRuntimeSyncDirection::NoChange);
		TestFalse(TEXT("no-change does not write"), NoChange.bMayWrite);
		TestTrue(TEXT("no-change may durably advance"), NoChange.bMayAdvance);
		FAssetDocumentSyncComparisonInput SidecarChanged = Base;
		SidecarChanged.Current.Sidecar.SemanticHash += TEXT(":changed");
		TestEqual(TEXT("sidecar change direction"), Coordinator.Compare(SidecarChanged).Direction,
			EAssetDocumentRuntimeSyncDirection::SidecarChanged);
		FAssetDocumentSyncComparisonInput Conflict = AssetChanged;
		Conflict.LastDurable = Base.LastDurable;
		Conflict.Current = Base.Current;
		Conflict.Current.Asset.SemanticHash += TEXT(":changed");
		Conflict.Current.Sidecar.SemanticHash += TEXT(":changed");
		const FAssetDocumentSyncDecision ConflictDecision = Coordinator.Compare(Conflict);
		TestEqual(TEXT("two-sided change conflicts"), ConflictDecision.Direction, EAssetDocumentRuntimeSyncDirection::Conflict);
		TestFalse(TEXT("conflict cannot write or advance"), ConflictDecision.bMayWrite || ConflictDecision.bMayAdvance);
	}
	TestEqual(TEXT("compare performs no state-store writes"), Store.StoreCalls, 0);

	const FAssetDocumentSyncEvidencePair Installed = MakeBaseInput(EAssetDocumentEvidenceKind::ExplicitEmpty).Current;
	const FAssetDocumentDurableCompletionReceipt Receipt = MakeReceipt();
	TestTrue(TEXT("durable completion advances typed record"), Coordinator.AdvanceAfterDurableCompletion(Installed, Receipt).bValid);
	TestEqual(TEXT("durable completion writes exactly once"), Store.StoreCalls, 1);
	TestTrue(TEXT("durable record binds receipt identity"), Store.LastRecord.RequestId == Receipt.RequestId
		&& Store.LastRecord.TransactionId == Receipt.TransactionId && Store.LastRecord.JournalId == Receipt.JournalId);
	TestTrue(TEXT("durable record preserves typed installed evidence"), EvidenceEqual(Store.LastRecord.LastDurable.Asset, Installed.Asset)
		&& EvidenceEqual(Store.LastRecord.LastDurable.Sidecar, Installed.Sidecar));

	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/TypedSync_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	FAssetDocumentJournaledTypedSyncStateStore FileStore(Root);
	FAssetDocumentTypedSyncStateRecord Original = Store.LastRecord;
	TestTrue(TEXT("binary typed state stores durably"), FileStore.Store(Key, Original).bValid);
	TOptional<FAssetDocumentTypedSyncStateRecord> Loaded;
	TArray<uint8> Preimage;
	bool bExisted = false;
	TestTrue(TEXT("binary typed state loads with recovery preimage"), FileStore.Load(Key, Loaded, Preimage, bExisted).bValid
		&& bExisted && Loaded.IsSet() && !Preimage.IsEmpty());
	FAssetDocumentTypedSyncStateRecord Replacement = Original;
	Replacement.RequestId = FGuid::NewGuid();
	TestTrue(TEXT("replacement typed state stores"), FileStore.Store(Key, Replacement).bValid);
	TestTrue(TEXT("typed sync preimage restores through same store"), FileStore.Restore(Key, Preimage, true).bValid);
	Loaded.Reset();
	TArray<uint8> RestoredBytes;
	TestTrue(TEXT("restored typed state matches original identity"), FileStore.Load(Key, Loaded, RestoredBytes, bExisted).bValid
		&& Loaded.IsSet() && Loaded->RequestId == Original.RequestId);
	return true;
}

#endif
