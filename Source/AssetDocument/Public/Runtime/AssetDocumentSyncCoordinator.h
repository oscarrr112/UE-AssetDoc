// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

struct ASSETDOCUMENT_API FAssetDocumentSyncEvidencePair
{
	FAssetDocumentRegionEvidence Asset;
	FAssetDocumentRegionEvidence Sidecar;
};

struct ASSETDOCUMENT_API FAssetDocumentSyncComparisonInput
{
	FAssetDocumentSyncEvidencePair Current;
	FAssetDocumentSyncEvidencePair LastDurable;
	bool bHasDurableBaseline = false;
};

struct ASSETDOCUMENT_API FAssetDocumentSyncDecision
{
	EAssetDocumentRuntimeSyncDirection Direction = EAssetDocumentRuntimeSyncDirection::EvidenceUnavailable;
	bool bMayWrite = false;
	// Eligibility only; state may advance solely through durable completion below.
	bool bMayAdvance = false;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

class ASSETDOCUMENT_API IAssetDocumentSyncCoordinator
{
public:
	virtual ~IAssetDocumentSyncCoordinator() = default;
	virtual FAssetDocumentSyncDecision Compare(
		const FAssetDocumentSyncComparisonInput& Input) const = 0;
	virtual FAssetDocumentContractValidationResult AdvanceAfterDurableCompletion(
		const FAssetDocumentSyncEvidencePair& Installed,
		const FAssetDocumentDurableCompletionReceipt& Receipt) = 0;
};

namespace UE::AssetDocument::SyncCoordinator::Private
{
inline FAssetDocumentContractValidationResult MakeValidationResult(
	bool bValid,
	const TCHAR* Path,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!bValid)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = Path;
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

inline bool IsValidEvidenceKind(EAssetDocumentEvidenceKind Kind)
{
	return static_cast<uint8>(Kind) <= static_cast<uint8>(EAssetDocumentEvidenceKind::EvidenceUnavailable);
}

inline FAssetDocumentContractValidationResult ValidateComparableEvidence(
	const FAssetDocumentRegionEvidence& Evidence)
{
	if (Evidence.SemanticHash.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("/RegionEvidence/SemanticHash"), TEXT("SyncEvidence.EmptySemanticHash"), TEXT("Comparable sync evidence requires a non-empty semantic hash."));
	}
	if (Evidence.CodecVersion == 0)
	{
		return MakeValidationResult(false, TEXT("/RegionEvidence/CodecVersion"), TEXT("SyncEvidence.InvalidCodecVersion"), TEXT("Comparable sync evidence requires a non-zero codec version."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidateUnavailableDiagnostic(
	const FAssetDocumentRegionEvidence& Evidence)
{
	if (!Evidence.Diagnostic.IsSet())
	{
		return MakeValidationResult(false, TEXT("/RegionEvidence/Diagnostic"), TEXT("SyncEvidence.MissingUnavailableDiagnostic"), TEXT("Unavailable sync evidence requires a stable diagnostic."));
	}
	if (Evidence.Diagnostic->Path.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("/RegionEvidence/Diagnostic/Path"), TEXT("SyncEvidence.EmptyDiagnosticPath"), TEXT("Unavailable sync evidence requires a stable diagnostic path."));
	}
	if (Evidence.Diagnostic->Code.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("/RegionEvidence/Diagnostic/Code"), TEXT("SyncEvidence.EmptyDiagnosticCode"), TEXT("Unavailable sync evidence requires a stable diagnostic code."));
	}
	if (Evidence.Diagnostic->Message.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("/RegionEvidence/Diagnostic/Message"), TEXT("SyncEvidence.EmptyDiagnosticMessage"), TEXT("Unavailable sync evidence requires a stable diagnostic message."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidateRegionEvidence(
	const FAssetDocumentRegionEvidence& Evidence)
{
	if (!IsValidEvidenceKind(Evidence.Kind))
	{
		return MakeValidationResult(false, TEXT("/RegionEvidence/Kind"), TEXT("SyncEvidence.InvalidKind"), TEXT("Sync evidence kind is outside the frozen enum."));
	}
	if (Evidence.Kind == EAssetDocumentEvidenceKind::EvidenceUnavailable)
	{
		return ValidateUnavailableDiagnostic(Evidence);
	}
	return ValidateComparableEvidence(Evidence);
}

inline FAssetDocumentContractValidationResult ValidateEvidencePair(
	const FAssetDocumentSyncEvidencePair& Pair)
{
	const FAssetDocumentContractValidationResult AssetResult = ValidateRegionEvidence(Pair.Asset);
	if (!AssetResult.bValid)
	{
		return AssetResult;
	}
	return ValidateRegionEvidence(Pair.Sidecar);
}

inline FAssetDocumentContractValidationResult ValidateComparisonInput(
	const FAssetDocumentSyncComparisonInput& Input)
{
	const FAssetDocumentContractValidationResult CurrentResult = ValidateEvidencePair(Input.Current);
	if (!CurrentResult.bValid)
	{
		return CurrentResult;
	}
	if (!Input.bHasDurableBaseline)
	{
		return MakeValidationResult(false, TEXT("/SyncComparison/LastDurable"), TEXT("Sync.MissingDurableBaseline"), TEXT("Sync comparison requires a durable typed-evidence baseline."));
	}
	return ValidateEvidencePair(Input.LastDurable);
}

inline TOptional<FAssetDocumentRuntimeDiagnostic> FindUnavailableDiagnostic(
	const FAssetDocumentSyncComparisonInput& Input)
{
	const FAssetDocumentRegionEvidence* Evidence[] = {
		&Input.Current.Asset,
		&Input.Current.Sidecar,
		&Input.LastDurable.Asset,
		&Input.LastDurable.Sidecar,
	};
	for (const FAssetDocumentRegionEvidence* Item : Evidence)
	{
		if (Item->Kind == EAssetDocumentEvidenceKind::EvidenceUnavailable)
		{
			return Item->Diagnostic;
		}
	}
	return {};
}

inline FAssetDocumentSyncDecision MakeUnavailableDecision(
	const FAssetDocumentRuntimeDiagnostic& Diagnostic)
{
	FAssetDocumentSyncDecision Decision;
	Decision.Diagnostics.Add(Diagnostic);
	return Decision;
}

inline FAssetDocumentSyncDecision MakeInvalidDecision(
	const FAssetDocumentContractValidationResult& Validation)
{
	FAssetDocumentSyncDecision Decision;
	Decision.Diagnostics = Validation.Diagnostics;
	return Decision;
}

inline bool IsSameComparableEvidence(
	const FAssetDocumentRegionEvidence& Current,
	const FAssetDocumentRegionEvidence& LastDurable)
{
	return Current.Kind == LastDurable.Kind
		&& Current.SemanticHash == LastDurable.SemanticHash
		&& Current.CodecVersion == LastDurable.CodecVersion;
}

inline FAssetDocumentSyncDecision MakeComparableDecision(
	bool bAssetChanged,
	bool bSidecarChanged)
{
	static constexpr EAssetDocumentRuntimeSyncDirection Directions[2][2] = {
		{EAssetDocumentRuntimeSyncDirection::NoChange, EAssetDocumentRuntimeSyncDirection::SidecarChanged},
		{EAssetDocumentRuntimeSyncDirection::AssetChanged, EAssetDocumentRuntimeSyncDirection::Conflict},
	};
	static constexpr bool MayWrite[2][2] = {
		{false, true},
		{true, false},
	};
	static constexpr bool MayAdvanceAfterDurableCompletion[2][2] = {
		{true, true},
		{true, false},
	};
	FAssetDocumentSyncDecision Decision;
	Decision.Direction = Directions[bAssetChanged][bSidecarChanged];
	Decision.bMayWrite = MayWrite[bAssetChanged][bSidecarChanged];
	Decision.bMayAdvance = MayAdvanceAfterDurableCompletion[bAssetChanged][bSidecarChanged];
	return Decision;
}
} // namespace UE::AssetDocument::SyncCoordinator::Private

inline FAssetDocumentContractValidationResult ValidateAssetDocumentRegionEvidence(
	const FAssetDocumentRegionEvidence& Evidence)
{
	return UE::AssetDocument::SyncCoordinator::Private::ValidateRegionEvidence(Evidence);
}

inline FAssetDocumentSyncDecision ClassifyAssetDocumentSyncDirection(
	const FAssetDocumentSyncComparisonInput& Input)
{
	using namespace UE::AssetDocument::SyncCoordinator::Private;
	const FAssetDocumentContractValidationResult Validation = ValidateComparisonInput(Input);
	if (!Validation.bValid)
	{
		return MakeInvalidDecision(Validation);
	}
	const TOptional<FAssetDocumentRuntimeDiagnostic> Unavailable = FindUnavailableDiagnostic(Input);
	if (Unavailable.IsSet())
	{
		return MakeUnavailableDecision(Unavailable.GetValue());
	}
	const bool bAssetChanged = !IsSameComparableEvidence(Input.Current.Asset, Input.LastDurable.Asset);
	const bool bSidecarChanged = !IsSameComparableEvidence(Input.Current.Sidecar, Input.LastDurable.Sidecar);
	return MakeComparableDecision(bAssetChanged, bSidecarChanged);
}
