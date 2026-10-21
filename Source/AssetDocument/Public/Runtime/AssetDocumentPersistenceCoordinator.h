// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

enum class EAssetDocumentPersistenceStep : uint8
{
	StagePackage = 0,
	StageSidecar = 1,
	FreshLoadStagedPackage = 2,
	CanonicalVerify = 3,
	PrepareJournal = 4,
	AtomicInstall = 5,
	DurableComplete = 6,
	Recover = 7,
};

struct ASSETDOCUMENT_API FAssetDocumentImmutableSidecarPayload
{
	TArray<uint8> Utf8Bytes;
	FString SemanticHash;
	uint32 CodecVersion = 0;
};

struct ASSETDOCUMENT_API FAssetDocumentPersistencePlan
{
	FGuid RequestId;
	FGuid TransactionId;
	FString TargetPackagePath;
	FString TargetSidecarPath;
	FString StagingRoot;
	FString JournalId;
	FAssetDocumentImmutableSidecarPayload Sidecar;
	TArray<FAssetDocumentMutationFootprint> Footprint;
};

struct ASSETDOCUMENT_API FAssetDocumentFreshVerificationInput
{
	FString StagedPackagePath;
	FString StagedSidecarPath;
	uint64 FreshHandleRevision = 0;
	FString ExpectedSemanticHash;
	uint32 ExpectedCodecVersion = 0;
};

struct ASSETDOCUMENT_API FAssetDocumentFreshVerificationResult
{
	bool bFreshLoadProven = false;
	bool bCanonicalMatch = false;
	FString ActualSemanticHash;
	uint32 ActualCodecVersion = 0;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

class ASSETDOCUMENT_API IAssetDocumentStagedArtifactVerifier
{
public:
	virtual ~IAssetDocumentStagedArtifactVerifier() = default;
	virtual FAssetDocumentFreshVerificationResult VerifyFresh(
		const FAssetDocumentFreshVerificationInput& Input) = 0;
};

class ASSETDOCUMENT_API IAssetDocumentPersistenceCoordinator
{
public:
	virtual ~IAssetDocumentPersistenceCoordinator() = default;
	virtual FAssetDocumentDurableCompletionReceipt Persist(
		const FAssetDocumentPersistencePlan& Plan,
		IAssetDocumentStagedArtifactVerifier& Verifier) = 0;
};

namespace UE::AssetDocument::PersistenceCoordinator::Private
{
inline FAssetDocumentContractValidationResult MakeValidationResult(
	bool bValid,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!bValid)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/PersistencePlan");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

inline bool IsValidPersistenceStep(EAssetDocumentPersistenceStep Step)
{
	return static_cast<uint8>(Step) <= static_cast<uint8>(EAssetDocumentPersistenceStep::Recover);
}

inline bool IsValidPersistenceFootprintKind(EAssetDocumentMutationFootprintKind Kind)
{
	return static_cast<uint8>(Kind) <= static_cast<uint8>(EAssetDocumentMutationFootprintKind::Handle);
}

inline FAssetDocumentContractValidationResult ValidatePlanIdentity(
	const FAssetDocumentPersistencePlan& Plan)
{
	if (!Plan.RequestId.IsValid())
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.InvalidRequestId"), TEXT("Persistence plan requires a valid request identity."));
	}
	if (!Plan.TransactionId.IsValid())
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.InvalidTransactionId"), TEXT("Persistence plan requires a valid transaction identity."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidatePlanTargets(
	const FAssetDocumentPersistencePlan& Plan)
{
	if (!FPackageName::IsValidLongPackageName(Plan.TargetPackagePath))
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.InvalidTargetPackagePath"), TEXT("Persistence plan requires a valid long target package path."));
	}
	if (Plan.TargetSidecarPath.TrimStartAndEnd().IsEmpty())
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.EmptyTargetSidecarPath"), TEXT("Persistence plan requires a target sidecar path."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidatePlanStagingRoot(
	const FAssetDocumentPersistencePlan& Plan)
{
	if (Plan.StagingRoot.TrimStartAndEnd().IsEmpty())
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.EmptyStagingRoot"), TEXT("Persistence plan requires an isolated staging root."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidateStagingIsolation(
	const FAssetDocumentPersistencePlan& Plan)
{
	if (FPaths::IsSamePath(Plan.TargetSidecarPath, Plan.StagingRoot)
		|| FPaths::IsUnderDirectory(Plan.TargetSidecarPath, Plan.StagingRoot))
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.NonIsolatedStagingRoot"), TEXT("Persistence staging root must be isolated from the final sidecar target."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidatePlanJournal(
	const FAssetDocumentPersistencePlan& Plan)
{
	if (Plan.JournalId.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.EmptyJournalId"), TEXT("Persistence plan requires a stable recovery journal identity."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidatePlanSidecar(
	const FAssetDocumentImmutableSidecarPayload& Sidecar)
{
	if (Sidecar.Utf8Bytes.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.EmptySidecarBytes"), TEXT("Persistence plan requires Python-produced UTF-8 sidecar bytes."));
	}
	if (Sidecar.SemanticHash.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.EmptySemanticHash"), TEXT("Persistence plan requires the Python-produced semantic hash."));
	}
	if (Sidecar.CodecVersion == 0)
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.InvalidCodecVersion"), TEXT("Persistence plan requires a non-zero document codec version."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidatePersistenceFootprintItem(
	const FAssetDocumentMutationFootprint& Item)
{
	if (!IsValidPersistenceFootprintKind(Item.Kind))
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.InvalidFootprintKind"), TEXT("Persistence footprint kind is outside the frozen enum."));
	}
	if (Item.StableId.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.EmptyFootprintStableId"), TEXT("Every persistence footprint item requires a stable identity."));
	}
	if (Item.RollbackComponentId.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.EmptyRollbackComponentId"), TEXT("Every persistence footprint item requires a rollback component identity."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidatePersistenceFootprint(
	TConstArrayView<FAssetDocumentMutationFootprint> Footprint)
{
	if (Footprint.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("PersistencePlan.EmptyFootprint"), TEXT("Persistence plan requires the complete immutable mutation footprint."));
	}
	TSet<FString> StableIds;
	for (const FAssetDocumentMutationFootprint& Item : Footprint)
	{
		const FAssetDocumentContractValidationResult ItemResult = ValidatePersistenceFootprintItem(Item);
		if (!ItemResult.bValid)
		{
			return ItemResult;
		}
		if (StableIds.Contains(Item.StableId))
		{
			return MakeValidationResult(false, TEXT("PersistencePlan.DuplicateFootprintStableId"), TEXT("Persistence footprint stable identities must be unique."));
		}
		StableIds.Add(Item.StableId);
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidatePlanHeader(
	const FAssetDocumentPersistencePlan& Plan)
{
	const FAssetDocumentContractValidationResult Identity = ValidatePlanIdentity(Plan);
	if (!Identity.bValid)
	{
		return Identity;
	}
	const FAssetDocumentContractValidationResult Targets = ValidatePlanTargets(Plan);
	if (!Targets.bValid)
	{
		return Targets;
	}
	const FAssetDocumentContractValidationResult StagingRoot = ValidatePlanStagingRoot(Plan);
	if (!StagingRoot.bValid)
	{
		return StagingRoot;
	}
	const FAssetDocumentContractValidationResult Isolation = ValidateStagingIsolation(Plan);
	if (!Isolation.bValid)
	{
		return Isolation;
	}
	return ValidatePlanJournal(Plan);
}
} // namespace UE::AssetDocument::PersistenceCoordinator::Private

inline bool IsAssetDocumentPersistenceTransitionAllowed(
	EAssetDocumentPersistenceStep From,
	EAssetDocumentPersistenceStep To)
{
	using namespace UE::AssetDocument::PersistenceCoordinator::Private;
	if (!IsValidPersistenceStep(From))
	{
		return false;
	}
	if (!IsValidPersistenceStep(To))
	{
		return false;
	}
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
	return Allowed[static_cast<uint8>(From)][static_cast<uint8>(To)];
}

inline FAssetDocumentContractValidationResult ValidateAssetDocumentPersistencePlan(
	const FAssetDocumentPersistencePlan& Plan)
{
	using namespace UE::AssetDocument::PersistenceCoordinator::Private;
	const FAssetDocumentContractValidationResult Header = ValidatePlanHeader(Plan);
	if (!Header.bValid)
	{
		return Header;
	}
	const FAssetDocumentContractValidationResult Sidecar = ValidatePlanSidecar(Plan.Sidecar);
	if (!Sidecar.bValid)
	{
		return Sidecar;
	}
	return ValidatePersistenceFootprint(Plan.Footprint);
}
