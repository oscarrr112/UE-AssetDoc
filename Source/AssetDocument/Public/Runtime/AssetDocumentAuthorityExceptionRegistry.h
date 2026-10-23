// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

enum class EAssetDocumentAuthorityState : uint8
{
	ObservedGap = 0,
	Researching = 1,
	CandidatePendingReview = 2,
	CandidateApproved = 3,
	NeedsMoreEvidence = 4,
	CandidateRejected = 5,
	HelperResolved = 6,
	HelperImplemented = 7,
	BridgePendingReview = 8,
	BridgeAdmitted = 9,
	BridgeRejected = 10,
};

enum class EAssetDocumentIndependentVerdict : uint8
{
	Pass = 0,
	Rework = 1,
	Reject = 2,
};

enum class EAssetDocumentAuthorityReviewKind : uint8
{
	Candidate = 0,
	Bridge = 1,
};

struct ASSETDOCUMENT_API FAssetDocumentAuthorityReviewRecord
{
	FString ReviewId;
	EAssetDocumentAuthorityReviewKind ReviewKind = EAssetDocumentAuthorityReviewKind::Candidate;
	FString ReviewerId;
	EAssetDocumentIndependentVerdict Verdict = EAssetDocumentIndependentVerdict::Rework;
	EAssetDocumentAuthorityState ReviewedState = EAssetDocumentAuthorityState::CandidatePendingReview;
	FString EvidenceHash;
	FString ReviewedHead;
};

struct ASSETDOCUMENT_API FAssetDocumentAuthorityCandidateRecord
{
	FString GovernanceId;
	EAssetDocumentPrimitiveOwner PhysicalOwner = EAssetDocumentPrimitiveOwner::Core;
	FString ExactSurface;
	FString ImplementationSymbol;
	EAssetDocumentAuthorityState State = EAssetDocumentAuthorityState::ObservedGap;
	FString ResearcherId;
	FString ImplementerId;
	FString ProviderOwnerId;
	TArray<FAssetDocumentAuthorityReviewRecord> Reviews;
	FString ResubmissionCondition;
	FString HelperReplacementCondition;
};

namespace UE::AssetDocument::AuthorityGovernance::Private
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
		Diagnostic.Path = TEXT("/AuthorityExceptionCatalog");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

inline bool IsValidState(EAssetDocumentAuthorityState State)
{
	return static_cast<uint8>(State) <= static_cast<uint8>(EAssetDocumentAuthorityState::BridgeRejected);
}

inline bool IsValidVerdict(EAssetDocumentIndependentVerdict Verdict)
{
	return static_cast<uint8>(Verdict) <= static_cast<uint8>(EAssetDocumentIndependentVerdict::Reject);
}

inline bool IsValidReviewKind(EAssetDocumentAuthorityReviewKind Kind)
{
	return static_cast<uint8>(Kind) <= static_cast<uint8>(EAssetDocumentAuthorityReviewKind::Bridge);
}

inline bool IsBlackboardStructuralGovernanceId(FStringView GovernanceId)
{
	return GovernanceId == TEXTVIEW("AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1")
		|| GovernanceId == TEXTVIEW("AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1");
}

inline bool ResolveGovernanceOwner(FStringView GovernanceId, EAssetDocumentPrimitiveOwner& OutOwner)
{
	if (IsBlackboardStructuralGovernanceId(GovernanceId)
		|| GovernanceId == TEXTVIEW("AD.AI.BT.GRAPH_INSTALL.v1")
		|| GovernanceId == TEXTVIEW("AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1"))
	{
		OutOwner = EAssetDocumentPrimitiveOwner::AI;
		return true;
	}

	static const TPair<FStringView, EAssetDocumentPrimitiveOwner> PrefixOwners[] = {
		{TEXTVIEW("CORE-"), EAssetDocumentPrimitiveOwner::Core},
		{TEXTVIEW("EDITOR-"), EAssetDocumentPrimitiveOwner::Editor},
		{TEXTVIEW("AI-"), EAssetDocumentPrimitiveOwner::AI},
		{TEXTVIEW("ANIMATION-"), EAssetDocumentPrimitiveOwner::Animation},
		{TEXTVIEW("BLUEPRINT-"), EAssetDocumentPrimitiveOwner::Blueprint},
		{TEXTVIEW("UMG-"), EAssetDocumentPrimitiveOwner::UMG},
	};
	for (const auto& Entry : PrefixOwners)
	{
		if (GovernanceId.StartsWith(Entry.Key))
		{
			OutOwner = Entry.Value;
			return true;
		}
	}
	return false;
}

inline bool HasGovernanceIdentity(const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	EAssetDocumentPrimitiveOwner ExpectedOwner = EAssetDocumentPrimitiveOwner::Core;
	const bool bOwnerResolved = ResolveGovernanceOwner(Candidate.GovernanceId, ExpectedOwner);
	return bOwnerResolved
		&& ExpectedOwner == Candidate.PhysicalOwner
		&& (Candidate.GovernanceId.EndsWith(TEXT("-v1"))
			|| Candidate.GovernanceId == TEXT("AD.AI.BT.GRAPH_INSTALL.v1"))
		&& (IsBlackboardStructuralGovernanceId(Candidate.GovernanceId)
			|| Candidate.GovernanceId == TEXT("AD.AI.BT.GRAPH_INSTALL.v1")
			|| Candidate.GovernanceId == TEXT("AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1")
			|| !Candidate.GovernanceId.Contains(TEXT(".")));
}

inline bool HasCandidateRoleIdentities(const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	return !Candidate.ResearcherId.IsEmpty()
		&& !Candidate.ImplementerId.IsEmpty()
		&& !Candidate.ProviderOwnerId.IsEmpty();
}

inline bool IsReviewerIndependent(
	const FAssetDocumentAuthorityReviewRecord& Review,
	const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	return Review.ReviewerId != Candidate.ResearcherId
		&& Review.ReviewerId != Candidate.ImplementerId
		&& Review.ReviewerId != Candidate.ProviderOwnerId;
}

inline bool HasReviewIdentityAndEvidence(const FAssetDocumentAuthorityReviewRecord& Review)
{
	const bool bHasIdentity = !Review.ReviewId.IsEmpty() && !Review.ReviewerId.IsEmpty();
	const bool bHasEvidence = !Review.EvidenceHash.IsEmpty() && !Review.ReviewedHead.IsEmpty();
	return bHasIdentity && bHasEvidence;
}

inline bool HasValidReviewEnums(const FAssetDocumentAuthorityReviewRecord& Review)
{
	return IsValidReviewKind(Review.ReviewKind)
		&& IsValidVerdict(Review.Verdict)
		&& IsValidState(Review.ReviewedState);
}

inline bool IsReviewRecordValid(
	const FAssetDocumentAuthorityReviewRecord& Review,
	const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	return HasReviewIdentityAndEvidence(Review)
		&& HasValidReviewEnums(Review)
		&& IsReviewerIndependent(Review, Candidate);
}
} // namespace UE::AssetDocument::AuthorityGovernance::Private

inline bool IsAssetDocumentAuthorityTransitionAllowed(
	EAssetDocumentAuthorityState From,
	EAssetDocumentAuthorityState To)
{
	using FStateEdge = TPair<EAssetDocumentAuthorityState, EAssetDocumentAuthorityState>;
	static const FStateEdge AllowedEdges[] = {
		{EAssetDocumentAuthorityState::ObservedGap, EAssetDocumentAuthorityState::Researching},
		{EAssetDocumentAuthorityState::Researching, EAssetDocumentAuthorityState::CandidatePendingReview},
		{EAssetDocumentAuthorityState::CandidatePendingReview, EAssetDocumentAuthorityState::CandidateApproved},
		{EAssetDocumentAuthorityState::CandidatePendingReview, EAssetDocumentAuthorityState::NeedsMoreEvidence},
		{EAssetDocumentAuthorityState::CandidatePendingReview, EAssetDocumentAuthorityState::CandidateRejected},
		{EAssetDocumentAuthorityState::CandidateApproved, EAssetDocumentAuthorityState::HelperResolved},
		{EAssetDocumentAuthorityState::CandidateApproved, EAssetDocumentAuthorityState::HelperImplemented},
		{EAssetDocumentAuthorityState::CandidateApproved, EAssetDocumentAuthorityState::BridgePendingReview},
		{EAssetDocumentAuthorityState::BridgePendingReview, EAssetDocumentAuthorityState::BridgeAdmitted},
		{EAssetDocumentAuthorityState::BridgePendingReview, EAssetDocumentAuthorityState::BridgeRejected},
	};
	return MakeArrayView(AllowedEdges).Contains(FStateEdge{From, To});
}

inline const FAssetDocumentAuthorityCandidateRecord* FindAssetDocumentAuthorityCandidateRecord(
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog,
	FStringView GovernanceId)
{
	for (const FAssetDocumentAuthorityCandidateRecord& Candidate : Catalog)
	{
		if (Candidate.GovernanceId == GovernanceId)
		{
			return &Candidate;
		}
	}
	return nullptr;
}

inline const FAssetDocumentAuthorityReviewRecord* FindAssetDocumentAuthorityReviewRecord(
	const FAssetDocumentAuthorityCandidateRecord& Candidate,
	FStringView ReviewId)
{
	for (const FAssetDocumentAuthorityReviewRecord& Review : Candidate.Reviews)
	{
		if (Review.ReviewId == ReviewId)
		{
			return &Review;
		}
	}
	return nullptr;
}

namespace UE::AssetDocument::AuthorityGovernance::Private
{
struct FRequiredReviewShape
{
	EAssetDocumentAuthorityState CandidateState;
	EAssetDocumentAuthorityReviewKind ReviewKind;
	EAssetDocumentIndependentVerdict Verdict;
	EAssetDocumentAuthorityState ReviewedState;
};

inline const FAssetDocumentAuthorityReviewRecord* FindMatchingReview(
	const FAssetDocumentAuthorityCandidateRecord& Candidate,
	EAssetDocumentAuthorityReviewKind Kind,
	EAssetDocumentIndependentVerdict Verdict,
	EAssetDocumentAuthorityState ReviewedState)
{
	for (const FAssetDocumentAuthorityReviewRecord& Review : Candidate.Reviews)
	{
		if (Review.ReviewKind == Kind && Review.Verdict == Verdict && Review.ReviewedState == ReviewedState)
		{
			return &Review;
		}
	}
	return nullptr;
}

inline bool HasBridgeReviewEvidence(
	const FAssetDocumentAuthorityCandidateRecord& Candidate,
	EAssetDocumentIndependentVerdict BridgeVerdict,
	EAssetDocumentAuthorityState BridgeState)
{
	const FAssetDocumentAuthorityReviewRecord* CandidateReview = FindMatchingReview(
		Candidate, EAssetDocumentAuthorityReviewKind::Candidate,
		EAssetDocumentIndependentVerdict::Pass, EAssetDocumentAuthorityState::CandidateApproved);
	const FAssetDocumentAuthorityReviewRecord* BridgeReview = FindMatchingReview(
		Candidate, EAssetDocumentAuthorityReviewKind::Bridge, BridgeVerdict, BridgeState);
	if (CandidateReview == nullptr || BridgeReview == nullptr)
	{
		return false;
	}
	return CandidateReview->ReviewerId != BridgeReview->ReviewerId;
}

inline bool HasRequiredStateReview(const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	static const FRequiredReviewShape Required[] = {
		{EAssetDocumentAuthorityState::CandidateApproved, EAssetDocumentAuthorityReviewKind::Candidate,
			EAssetDocumentIndependentVerdict::Pass, EAssetDocumentAuthorityState::CandidateApproved},
		{EAssetDocumentAuthorityState::NeedsMoreEvidence, EAssetDocumentAuthorityReviewKind::Candidate,
			EAssetDocumentIndependentVerdict::Rework, EAssetDocumentAuthorityState::NeedsMoreEvidence},
		{EAssetDocumentAuthorityState::CandidateRejected, EAssetDocumentAuthorityReviewKind::Candidate,
			EAssetDocumentIndependentVerdict::Reject, EAssetDocumentAuthorityState::CandidateRejected},
		{EAssetDocumentAuthorityState::HelperResolved, EAssetDocumentAuthorityReviewKind::Candidate,
			EAssetDocumentIndependentVerdict::Pass, EAssetDocumentAuthorityState::CandidateApproved},
		{EAssetDocumentAuthorityState::HelperImplemented, EAssetDocumentAuthorityReviewKind::Candidate,
			EAssetDocumentIndependentVerdict::Pass, EAssetDocumentAuthorityState::CandidateApproved},
		{EAssetDocumentAuthorityState::BridgePendingReview, EAssetDocumentAuthorityReviewKind::Candidate,
			EAssetDocumentIndependentVerdict::Pass, EAssetDocumentAuthorityState::CandidateApproved},
	};
	for (const FRequiredReviewShape& Shape : Required)
	{
		if (Candidate.State == Shape.CandidateState)
		{
			return FindMatchingReview(Candidate, Shape.ReviewKind, Shape.Verdict, Shape.ReviewedState) != nullptr;
		}
	}
	return true;
}

inline bool HasStateReviewEvidence(const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	if (Candidate.State == EAssetDocumentAuthorityState::BridgeAdmitted)
	{
		return HasBridgeReviewEvidence(
			Candidate, EAssetDocumentIndependentVerdict::Pass, EAssetDocumentAuthorityState::BridgeAdmitted);
	}
	if (Candidate.State == EAssetDocumentAuthorityState::BridgeRejected)
	{
		return HasBridgeReviewEvidence(
			Candidate, EAssetDocumentIndependentVerdict::Reject, EAssetDocumentAuthorityState::BridgeRejected);
	}
	return HasRequiredStateReview(Candidate);
}

inline bool HasApprovedStateFields(const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	return !Candidate.ImplementationSymbol.IsEmpty()
		&& Candidate.ResubmissionCondition.IsEmpty()
		&& !Candidate.HelperReplacementCondition.IsEmpty();
}

inline bool HasNeedsEvidenceStateFields(const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	return Candidate.ImplementationSymbol.IsEmpty()
		&& !Candidate.ResubmissionCondition.IsEmpty()
		&& Candidate.HelperReplacementCondition.IsEmpty();
}

inline bool HasRejectedStateFields(const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	return Candidate.ImplementationSymbol.IsEmpty()
		&& Candidate.ResubmissionCondition.IsEmpty()
		&& Candidate.HelperReplacementCondition.IsEmpty();
}

inline bool HasStateFields(const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	static const EAssetDocumentAuthorityState ApprovedStates[] = {
		EAssetDocumentAuthorityState::CandidateApproved,
		EAssetDocumentAuthorityState::HelperResolved,
		EAssetDocumentAuthorityState::HelperImplemented,
		EAssetDocumentAuthorityState::BridgePendingReview,
		EAssetDocumentAuthorityState::BridgeAdmitted,
		EAssetDocumentAuthorityState::BridgeRejected,
	};
	if (MakeArrayView(ApprovedStates).Contains(Candidate.State))
	{
		return HasApprovedStateFields(Candidate);
	}
	if (Candidate.State == EAssetDocumentAuthorityState::NeedsMoreEvidence)
	{
		return HasNeedsEvidenceStateFields(Candidate);
	}
	if (Candidate.State == EAssetDocumentAuthorityState::CandidateRejected)
	{
		return HasRejectedStateFields(Candidate);
	}
	return true;
}

inline bool AreReviewRecordsValid(const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	for (const FAssetDocumentAuthorityReviewRecord& Review : Candidate.Reviews)
	{
		if (!IsReviewRecordValid(Review, Candidate))
		{
			return false;
		}
	}
	return true;
}

inline bool HasCandidateIdentityAndSurface(const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	return HasGovernanceIdentity(Candidate) && !Candidate.ExactSurface.IsEmpty();
}

inline bool HasCandidateRolesAndState(const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	return HasCandidateRoleIdentities(Candidate) && IsValidState(Candidate.State);
}

inline bool HasCandidateReviewContract(const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	return AreReviewRecordsValid(Candidate) && HasStateReviewEvidence(Candidate);
}

inline FAssetDocumentContractValidationResult ValidateCandidateRecord(
	const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	if (!HasCandidateIdentityAndSurface(Candidate))
	{
		return MakeValidationResult(false, TEXT("AuthorityCatalog.InvalidIdentity"), TEXT("Governance id, owner, or exact surface is invalid."));
	}
	if (!HasCandidateRolesAndState(Candidate))
	{
		return MakeValidationResult(false, TEXT("AuthorityCatalog.InvalidRoles"), TEXT("Candidate role identities or state are invalid."));
	}
	if (!HasStateFields(Candidate))
	{
		return MakeValidationResult(false, TEXT("AuthorityCatalog.InvalidStateFields"), TEXT("Candidate state fields do not preserve helper-first governance."));
	}
	if (!HasCandidateReviewContract(Candidate))
	{
		return MakeValidationResult(false, TEXT("AuthorityCatalog.InvalidReview"), TEXT("Independent review evidence does not support the candidate state."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline bool AddUniqueReviewIds(
	const FAssetDocumentAuthorityCandidateRecord& Candidate,
	TSet<FString>& IdentityIds)
{
	for (const FAssetDocumentAuthorityReviewRecord& Review : Candidate.Reviews)
	{
		if (IdentityIds.Contains(Review.ReviewId))
		{
			return false;
		}
		IdentityIds.Add(Review.ReviewId);
	}
	return true;
}

inline bool HasUniqueCatalogIds(TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog)
{
	TSet<FString> IdentityIds;
	for (const FAssetDocumentAuthorityCandidateRecord& Candidate : Catalog)
	{
		if (IdentityIds.Contains(Candidate.GovernanceId))
		{
			return false;
		}
		IdentityIds.Add(Candidate.GovernanceId);
		if (!AddUniqueReviewIds(Candidate, IdentityIds))
		{
			return false;
		}
	}
	return true;
}

inline FAssetDocumentContractValidationResult ValidateCatalogRecords(
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog)
{
	for (const FAssetDocumentAuthorityCandidateRecord& Candidate : Catalog)
	{
		const FAssetDocumentContractValidationResult Result = ValidateCandidateRecord(Candidate);
		if (!Result.bValid)
		{
			return Result;
		}
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}
} // namespace UE::AssetDocument::AuthorityGovernance::Private

inline FAssetDocumentContractValidationResult ValidateAssetDocumentAuthorityExceptionCatalog(
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog)
{
	using namespace UE::AssetDocument::AuthorityGovernance::Private;
	const FAssetDocumentContractValidationResult RecordResult = ValidateCatalogRecords(Catalog);
	if (!RecordResult.bValid)
	{
		return RecordResult;
	}
	if (!HasUniqueCatalogIds(Catalog))
	{
		return MakeValidationResult(false, TEXT("AuthorityCatalog.DuplicateId"), TEXT("Governance and review ids must be globally unique."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

namespace UE::AssetDocument::AuthorityGovernance::Private
{
inline FAssetDocumentAuthorityReviewRecord MakeReviewRecord(
	const FString& ReviewId,
	const TCHAR* ReviewerId,
	EAssetDocumentIndependentVerdict Verdict,
	EAssetDocumentAuthorityState ReviewedState,
	const TCHAR* EvidenceHash,
	const TCHAR* ReviewedHead)
{
	FAssetDocumentAuthorityReviewRecord Review;
	Review.ReviewId = ReviewId;
	Review.ReviewerId = ReviewerId;
	Review.Verdict = Verdict;
	Review.ReviewedState = ReviewedState;
	Review.EvidenceHash = EvidenceHash;
	Review.ReviewedHead = ReviewedHead;
	return Review;
}

inline FAssetDocumentAuthorityReviewRecord MakeInitialCandidateReview(
	const TCHAR* GovernanceId,
	EAssetDocumentIndependentVerdict Verdict,
	EAssetDocumentAuthorityState ReviewedState)
{
	return MakeReviewRecord(
		FString::Printf(TEXT("candidate-review.initial/%s"), GovernanceId),
		TEXT("authority-exception-candidate-reviewer"), Verdict, ReviewedState,
		TEXT("e06474f8da5605da77cbad596ba6ea1c0014d79ede162b68b0c96f546f64009a"),
		TEXT("4ca528c57e9f14751b38b5136aa831952691d44d"));
}

inline FAssetDocumentAuthorityReviewRecord MakeRereview(
	const TCHAR* GovernanceId,
	EAssetDocumentAuthorityState ReviewedState)
{
	return MakeReviewRecord(
		FString::Printf(TEXT("candidate-review.rereview/%s"), GovernanceId),
		TEXT("umg-authority-exception-candidate-rereviewer"),
		EAssetDocumentIndependentVerdict::Rework, ReviewedState,
		TEXT("bda3d730c4f06ca2cf1d17ff70084dfc2a57ca46afe2c8a7076da1b1f7a03726"),
		TEXT("34e3e23bd932dc5951200e802c403f4638219e65"));
}

inline FAssetDocumentAuthorityCandidateRecord MakeFrozenCandidate(
	const TCHAR* GovernanceId,
	EAssetDocumentPrimitiveOwner Owner,
	const TCHAR* Surface,
	const TCHAR* ImplementationSymbol,
	EAssetDocumentAuthorityState State,
	EAssetDocumentIndependentVerdict Verdict,
	const TCHAR* ResubmissionCondition,
	const TCHAR* HelperReplacementCondition)
{
	FAssetDocumentAuthorityCandidateRecord Candidate;
	Candidate.GovernanceId = GovernanceId;
	Candidate.PhysicalOwner = Owner;
	Candidate.ExactSurface = Surface;
	Candidate.ImplementationSymbol = ImplementationSymbol;
	Candidate.State = State;
	Candidate.ResearcherId = TEXT("umg-candidate-researcher");
	Candidate.ImplementerId = TEXT("contract-owner");
	Candidate.ProviderOwnerId = Owner == EAssetDocumentPrimitiveOwner::UMG
		? TEXT("umg-provider-owner")
		: TEXT("blueprint-provider-owner");
	Candidate.ResubmissionCondition = ResubmissionCondition;
	Candidate.HelperReplacementCondition = HelperReplacementCondition;
	Candidate.Reviews.Add(MakeInitialCandidateReview(GovernanceId, Verdict, State));
	return Candidate;
}

inline FAssetDocumentAuthorityCandidateRecord MakeDesignerBindingRecord()
{
	const TCHAR* GovernanceId = TEXT("UMG-DESIGNER-BINDING-APPLY-DELTA-v1");
	FAssetDocumentAuthorityCandidateRecord Candidate = MakeFrozenCandidate(
		GovernanceId, EAssetDocumentPrimitiveOwner::UMG,
		TEXT("(WidgetName, PropertyName)-identified Designer Binding add/update/remove"), TEXT(""),
		EAssetDocumentAuthorityState::NeedsMoreEvidence, EAssetDocumentIndependentVerdict::Rework,
		TEXT("Freeze binding permission, extension policy, derived GUID, typed DTO/symbol, rollback, compile, and fresh-load evidence before independent re-review."),
		TEXT(""));
	Candidate.Reviews.Add(MakeRereview(GovernanceId, EAssetDocumentAuthorityState::NeedsMoreEvidence));
	return Candidate;
}

inline FAssetDocumentAuthorityCandidateRecord MakeWidgetAnimationCreateRecord()
{
	return MakeFrozenCandidate(
		TEXT("UMG-WIDGET-ANIMATION-CREATE-v1"), EAssetDocumentPrimitiveOwner::UMG,
		TEXT("one UWidgetAnimation plus owned UMovieScene creation and installation"),
		TEXT("AssetDocumentUMG::CreateWidgetAnimationHelperFirst"),
		EAssetDocumentAuthorityState::CandidateApproved, EAssetDocumentIndependentVerdict::Pass,
		TEXT(""),
		TEXT("Replace with an exported UMGEditor CreateWidgetAnimation lifecycle helper when available; no bridge is admitted."));
}

inline FAssetDocumentAuthorityCandidateRecord MakeWidgetAnimationDeleteRecord()
{
	const TCHAR* GovernanceId = TEXT("UMG-WIDGET-ANIMATION-DELETE-v1");
	FAssetDocumentAuthorityCandidateRecord Candidate = MakeFrozenCandidate(
		GovernanceId, EAssetDocumentPrimitiveOwner::UMG,
		TEXT("one installed WidgetAnimation removal and variable/compile lifecycle"), TEXT(""),
		EAssetDocumentAuthorityState::NeedsMoreEvidence, EAssetDocumentIndependentVerdict::Rework,
		TEXT("Freeze referenced-animation policy, transient GC retention, rollback order, typed DTO/symbol, compile, and fresh-load evidence before independent re-review."),
		TEXT(""));
	Candidate.Reviews.Add(MakeRereview(GovernanceId, EAssetDocumentAuthorityState::NeedsMoreEvidence));
	return Candidate;
}

inline FAssetDocumentAuthorityCandidateRecord MakeAnimationBindingRejectedRecord()
{
	return MakeFrozenCandidate(
		TEXT("UMG-ANIMATION-BINDING-APPLY-DELTA-v1"), EAssetDocumentPrimitiveOwner::UMG,
		TEXT("MovieScene possessable to Widget/root/slot binding add/update/remove"), TEXT(""),
		EAssetDocumentAuthorityState::CandidateRejected, EAssetDocumentIndependentVerdict::Reject,
		TEXT(""), TEXT(""));
}

inline FAssetDocumentAuthorityCandidateRecord MakeTimelineTrackRecord()
{
	return MakeFrozenCandidate(
		TEXT("BLUEPRINT-TIMELINE-TRACK-APPLY-DELTA-v1"), EAssetDocumentPrimitiveOwner::Blueprint,
		TEXT("one Timeline track lifecycle, distinct from template lifecycle and key replacement"),
		TEXT("AssetDocumentBlueprint::ApplyTimelineTrackDeltaHelperFirst"),
		EAssetDocumentAuthorityState::CandidateApproved, EAssetDocumentIndependentVerdict::Pass,
		TEXT(""),
		TEXT("Replace with an exported UnrealEd/Kismet Timeline track lifecycle helper when available; no bridge is admitted."));
}

inline FAssetDocumentAuthorityCandidateRecord MakeTimelineCurveRejectedRecord()
{
	return MakeFrozenCandidate(
		TEXT("BLUEPRINT-TIMELINE-CURVE-APPLY-KEYS-v1"), EAssetDocumentPrimitiveOwner::Blueprint,
		TEXT("key replacement on an already resolved owned Timeline curve"), TEXT(""),
		EAssetDocumentAuthorityState::CandidateRejected, EAssetDocumentIndependentVerdict::Reject,
		TEXT(""), TEXT(""));
}

inline FAssetDocumentAuthorityCandidateRecord MakeBlackboardStructuralBridgeRecord(
	const TCHAR* GovernanceId,
	const TCHAR* Surface,
	const TCHAR* ImplementationSymbol,
	const TCHAR* CandidateReviewerId,
	const TCHAR* CandidateEvidenceHash,
	const TCHAR* BridgeReviewerId,
	const TCHAR* BridgeEvidenceHash)
{
	static const TCHAR* ReviewedHead = TEXT("aebdffa6c30242629d4cd0df6156fc9e23eadc68");
	FAssetDocumentAuthorityCandidateRecord Candidate;
	Candidate.GovernanceId = GovernanceId;
	Candidate.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	Candidate.ExactSurface = Surface;
	Candidate.ImplementationSymbol = ImplementationSymbol;
	Candidate.State = EAssetDocumentAuthorityState::BridgeAdmitted;
	Candidate.ResearcherId = TEXT("blackboard-structural-researcher");
	Candidate.ImplementerId = TEXT("blackboard-structural-publication-implementer");
	Candidate.ProviderOwnerId = TEXT("blackboard-provider-owner");
	Candidate.HelperReplacementCondition =
		TEXT("Remove this exception when a stock public/exported Blackboard lifecycle helper covers the exact reviewed surface.");
	Candidate.Reviews.Add(MakeReviewRecord(
		FString::Printf(TEXT("candidate-review.initial/%s"), GovernanceId),
		CandidateReviewerId, EAssetDocumentIndependentVerdict::Pass,
		EAssetDocumentAuthorityState::CandidateApproved, CandidateEvidenceHash, ReviewedHead));
	FAssetDocumentAuthorityReviewRecord BridgeReview = MakeReviewRecord(
		FString::Printf(TEXT("bridge-review.initial/%s"), GovernanceId),
		BridgeReviewerId, EAssetDocumentIndependentVerdict::Pass,
		EAssetDocumentAuthorityState::BridgeAdmitted, BridgeEvidenceHash, ReviewedHead);
	BridgeReview.ReviewKind = EAssetDocumentAuthorityReviewKind::Bridge;
	Candidate.Reviews.Add(MoveTemp(BridgeReview));
	return Candidate;
}

inline FAssetDocumentAuthorityCandidateRecord MakeBlackboardKeyTypeStagingRecord()
{
	return MakeBlackboardStructuralBridgeRecord(
		TEXT("AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1"),
		TEXT("One NewObject<UBlackboardKeyType> staging/materialization callsite for add and replace-class only."),
		TEXT("AssetDocumentAIBlackboardStructuralPublication::StageKeyType"),
		TEXT("blackboard-keytype-candidate-reviewer"),
		TEXT("d53888bc04f986eb835e442e7fa87389700a1e9fe8f4b465c11ed6fef38b6d65"),
		TEXT("blackboard-keytype-bridge-reviewer"),
		TEXT("7ecd0a82233442960c4097248442eb0aa6aca0c0287af98ace9490fd6b8c4217"));
}

inline FAssetDocumentAuthorityCandidateRecord MakeBlackboardLocalKeysPublicationRecord()
{
	return MakeBlackboardStructuralBridgeRecord(
		TEXT("AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1"),
		TEXT("One complete UBlackboardData::Keys snapshot publication callsite for the four v2 rows and the exact immutable v1 compatibility caller."),
		TEXT("AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot"),
		TEXT("blackboard-local-keys-candidate-reviewer"),
		TEXT("c67dc7df1cf67317bf9e10e1f1ff41a2d07a8b329d4081406806be5cbcd60746"),
		TEXT("blackboard-local-keys-bridge-reviewer"),
		TEXT("66f0e1194458077c4ae50c3150e5b9556a383d85d32b08efddd3dd6153debd5d"));
}

// Montage slots and sections snapshot publication. The governance ids follow the catalog's existing id format; no
// generic identity rule is broadened.
inline FAssetDocumentAuthorityCandidateRecord MakeMontageStructuralBridgeRecord(bool bSlots)
{
 FAssetDocumentAuthorityCandidateRecord C;
 C.GovernanceId = bSlots ? TEXT("ANIMATION-MONTAGE-SLOTS-PUBLISH-v1") : TEXT("ANIMATION-MONTAGE-SECTIONS-PUBLISH-v1");
 C.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 C.ExactSurface = bSlots
  ? TEXT("Typed Montage slots snapshot publication with section and retained mixed notify links and public length completion; no managed notify object replacement.")
  : TEXT("Typed Montage sections snapshot publication and retained mixed notify links; no managed notify object replacement.");
 C.ImplementationSymbol = bSlots
  ? TEXT("PublishMontageSlots;PublishMontageSections;PublishMontageNotifyEvents;UAnimCompositeBase::SetCompositeLength")
  : TEXT("PublishMontageSections;PublishMontageNotifyEvents");
 C.State = EAssetDocumentAuthorityState::BridgeAdmitted;
 C.ResearcherId = TEXT("montage-native-seams-researcher");
 C.ImplementerId = TEXT("assetdocument-maintainer"); C.ProviderOwnerId = TEXT("assetdocument-maintainer");
 C.HelperReplacementCondition = TEXT("Migrate the unique structural caller when stock public APIs cover typed collection publication and equivalent restoration, then remove these private exceptions.");
 C.Reviews.Add(MakeReviewRecord(FString(TEXT("candidate-review.initial/")) + C.GovernanceId,
  TEXT("montage-source-reviewer"), EAssetDocumentIndependentVerdict::Pass,
  EAssetDocumentAuthorityState::CandidateApproved, TEXT("cfe9113147e5b0034f1125d0fee606add90d0f193a9acf352834de9557a83709"), TEXT("4aeea7541f3000b2211ab6719b6041d1b6998f85")));
 auto B = MakeReviewRecord(FString(TEXT("bridge-review.initial/")) + C.GovernanceId,
  TEXT("montage-structural-fixtures-reviewer"), EAssetDocumentIndependentVerdict::Pass,
  EAssetDocumentAuthorityState::BridgeAdmitted, TEXT("9c62436673d3e6915d966f36b80e8686239b4c1c5f4aac1bf6482e996428b5c3"), TEXT("80e7074aa58df8a441df54c68e3609fa7faf2d6a"));
 B.ReviewKind = EAssetDocumentAuthorityReviewKind::Bridge; C.Reviews.Add(MoveTemp(B));
 return C;
}

// Exact float-curve state publication, in the catalog's existing id format. Its two reviews are source reviews, not
// runtime or save evidence.
inline FAssetDocumentAuthorityCandidateRecord MakeAnimationFloatCurveBridgeRecord()
{
 FAssetDocumentAuthorityCandidateRecord C;
 C.GovernanceId = TEXT("ANIMATION-FLOAT-CURVE-EXACT-STATE-v1");
 C.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 C.ExactSurface = TEXT("Ordered complete float-curve replacement on exact stock Montage/Composite UAnimDataModel; sole private default/nine-key-field member patch after public controller edits and before public attributes notification.");
 C.ImplementationSymbol = TEXT("PublishExactFloatCurveState (approved plugin bridge; public Controller + exact member)");
 C.State = EAssetDocumentAuthorityState::BridgeAdmitted;
 C.ResearcherId = TEXT("float-curve-researcher");
 C.ImplementerId = TEXT("assetdocument-maintainer"); C.ProviderOwnerId = TEXT("assetdocument-maintainer");
 C.HelperReplacementCondition = TEXT("Replace the unique exact-member patch when stock public controller APIs preserve all default and key bits with equivalent forward/inverse/save behavior.");
 C.Reviews.Add(MakeReviewRecord(TEXT("candidate-review.initial/ANIMATION-FLOAT-CURVE-EXACT-STATE-v1"),
  TEXT("float-curve-candidate-reviewer"), EAssetDocumentIndependentVerdict::Pass,
  EAssetDocumentAuthorityState::CandidateApproved, TEXT("995e94b9de2329ae89a395166275912c962c8c4406515b8144bad699edcc29b5"), TEXT("1e3f186d5ee49e20d5a402026450dc1284d09942")));
 auto B = MakeReviewRecord(TEXT("bridge-review.initial/ANIMATION-FLOAT-CURVE-EXACT-STATE-v1"),
  TEXT("float-curve-bridge-reviewer"), EAssetDocumentIndependentVerdict::Pass,
  EAssetDocumentAuthorityState::BridgeAdmitted, TEXT("cacacf35cbb5546f10699c63d0f604d17b7bc5e5893a7dadf8d20be29ab955e9"), TEXT("2ec1903b0672f82b1a45c928184bc2a1b5b8ccd0"));
 B.ReviewKind = EAssetDocumentAuthorityReviewKind::Bridge; C.Reviews.Add(MoveTemp(B));
 return C;
}

// Montage notify and notify-state replacement. Both reviews are independent source reviews; runtime evidence is
// separate.
inline FAssetDocumentAuthorityCandidateRecord MakeMontageNotifyBridgeRecord(bool bStates)
{
 FAssetDocumentAuthorityCandidateRecord C;
 C.GovernanceId = bStates ? TEXT("ANIMATION-MONTAGE-NOTIFY-STATES-REPLACE-v1") : TEXT("ANIMATION-MONTAGE-NOTIFIES-REPLACE-v1");
 C.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 C.ExactSurface = TEXT("One request-local concrete notify family replacement, actual staged allocation set, existing Editor safe property configuration, and full retained mixed-timeline publication; Apply only.");
 C.ImplementationSymbol = TEXT("StageMontageNotifyObject;ConfigureAssetDocumentEditorStagedMontageNotifyProperties;PublishMontageNotifyEvents");
 C.State = EAssetDocumentAuthorityState::BridgeAdmitted;
 C.ResearcherId = TEXT("montage-native-seams-researcher"); C.ImplementerId = TEXT("assetdocument-maintainer"); C.ProviderOwnerId = TEXT("assetdocument-maintainer");
 C.HelperReplacementCondition = TEXT("Stock exported APIs cover safe staged notify configuration and whole mixed-timeline publication with equivalent rollback.");
 C.Reviews.Add(MakeReviewRecord(FString(TEXT("candidate-review.initial/")) + C.GovernanceId,
  TEXT("montage-notify-candidate-reviewer"), EAssetDocumentIndependentVerdict::Pass,
  EAssetDocumentAuthorityState::CandidateApproved, TEXT("4e82009337ae72c48268b2d6d46da22e59861312b1a65a1147d21fdce53c6375"), TEXT("dce40e326922a2ed49f14040391cacf6fe8b5b6c")));
 auto B = MakeReviewRecord(FString(TEXT("bridge-review.initial/")) + C.GovernanceId,
  TEXT("montage-source-reviewer"), EAssetDocumentIndependentVerdict::Pass,
  EAssetDocumentAuthorityState::BridgeAdmitted, TEXT("cebd7cf87a329cc9f78b26944d806a147799e3bde456b46d3a91d2b7783a769f"), TEXT("ff48b6ee1149262917d1e1153edc4939338fae84"));
 B.ReviewKind = EAssetDocumentAuthorityReviewKind::Bridge; C.Reviews.Add(MoveTemp(B));
 return C;
}

// Behavior Tree graph installation and Blackboard ValueOr cache invalidation: an independent source review of the
// candidate and a separate independent review of the bridge.
inline FAssetDocumentAuthorityCandidateRecord MakeBehaviorTreeBridgeRecord(bool bInstall)
{
 FAssetDocumentAuthorityCandidateRecord C;
 C.GovernanceId = bInstall ? TEXT("AD.AI.BT.GRAPH_INSTALL.v1")
  : TEXT("AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1");
 C.ExactSurface = bInstall ? TEXT("Exact optional-old BehaviorTree graph installation, owned aggregate transfer, scratch/live capability transition and inverse ownership with preregistered terminal completion.")
  : TEXT("Exact CurrentGraph eleven-family ValueOr cache invalidation through ValueOrDecoratorMirror completion for refresh v1/v2/v3, current_graph and installed_replacement lifecycles, including v3 nullable Blackboard admission while preserving authored inventory and inverse completion.");
 C.ImplementationSymbol = bInstall ? TEXT("InstallExactGraph>CaptureAndRegisterInstalledReplacementCompletion>UObject::Rename>UBehaviorTree::BTGraph>UBehaviorTree::LastEditedDocuments>RecompleteGraphInstallInverse")
  : TEXT("RunAssetDocumentBehaviorTreeDerivedCompletion/ValueOrDecoratorMirror/CurrentGraph>CompleteExactCurrentGraphValueOrInventory>InvalidateExactValueOrBlackboardKeyCache");
 C.HelperReplacementCondition = bInstall ? TEXT("Replace with stock exported graph ownership transfer including optional-old rollback and scratch/live capability transition.")
  : TEXT("Replace only with a stock public exported exact ValueOr cache invalidation lifecycle covering all eleven families.");
 C.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
 C.State = EAssetDocumentAuthorityState::BridgeAdmitted;
 C.ResearcherId = TEXT("assetdocument-maintainer"); C.ImplementerId = TEXT("assetdocument-maintainer"); C.ProviderOwnerId = TEXT("assetdocument-maintainer");
 C.Reviews.Add(MakeReviewRecord(FString(TEXT("candidate-review.A2/")) + C.GovernanceId,
  TEXT("behavior-tree-source-reviewer"), EAssetDocumentIndependentVerdict::Pass,
  EAssetDocumentAuthorityState::CandidateApproved, TEXT("a8a1c63275a70e047d775b4dabc71f626b3484baca4e74418454ddfc3115b378"), TEXT("2d07c76b5a65155346639f8967b4592a6218b17b")));
 auto B = MakeReviewRecord(FString(bInstall ? TEXT("bridge-review.D5/") : TEXT("bridge-review.BTProduction/")) + C.GovernanceId,
  bInstall ? TEXT("behavior-tree-bridge-projection-reviewer") : TEXT("behavior-tree-production-boundary-reviewer"), EAssetDocumentIndependentVerdict::Pass,
  EAssetDocumentAuthorityState::BridgeAdmitted,
  bInstall ? TEXT("8a4cde32d19ef46644acfa6599c40fe765a90a03da3a885b6b950d69896fd773") : TEXT("0259a302a91345388a67f9f8f60fa9dcf82158c8fabdfbb923acc6fee3becd05"),
  bInstall ? TEXT("ca953c4edc9063faca22344994b2ea7f1300e17f") : TEXT("74321d579e4c7c61bbde093fd2bb8b99f5084843"));
 B.ReviewKind = EAssetDocumentAuthorityReviewKind::Bridge; C.Reviews.Add(MoveTemp(B));
 return C;
}

inline TArray<FAssetDocumentAuthorityCandidateRecord> BuildFrozenCatalog()
{
	return {
		MakeDesignerBindingRecord(),
		MakeWidgetAnimationCreateRecord(),
		MakeWidgetAnimationDeleteRecord(),
		MakeAnimationBindingRejectedRecord(),
		MakeTimelineTrackRecord(),
		MakeTimelineCurveRejectedRecord(),
		MakeBlackboardKeyTypeStagingRecord(),
		MakeBlackboardLocalKeysPublicationRecord(),
        MakeMontageStructuralBridgeRecord(true),
        MakeMontageStructuralBridgeRecord(false),
        MakeAnimationFloatCurveBridgeRecord(),
        MakeMontageNotifyBridgeRecord(false),
        MakeMontageNotifyBridgeRecord(true),
        MakeBehaviorTreeBridgeRecord(true),
        MakeBehaviorTreeBridgeRecord(false),
	};
}

struct FFrozenIdentity
{
	const TCHAR* GovernanceId;
	EAssetDocumentPrimitiveOwner Owner;
	EAssetDocumentAuthorityState State;
};

inline bool MatchesFrozenIdentity(
	const FAssetDocumentAuthorityCandidateRecord& Candidate,
	const FFrozenIdentity& Expected)
{
	return Candidate.GovernanceId == Expected.GovernanceId
		&& Candidate.PhysicalOwner == Expected.Owner
		&& Candidate.State == Expected.State;
}

inline bool HasExactFrozenIdentities(TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog)
{
	static const FFrozenIdentity Expected[] = {
		{TEXT("UMG-DESIGNER-BINDING-APPLY-DELTA-v1"), EAssetDocumentPrimitiveOwner::UMG, EAssetDocumentAuthorityState::NeedsMoreEvidence},
		{TEXT("UMG-WIDGET-ANIMATION-CREATE-v1"), EAssetDocumentPrimitiveOwner::UMG, EAssetDocumentAuthorityState::CandidateApproved},
		{TEXT("UMG-WIDGET-ANIMATION-DELETE-v1"), EAssetDocumentPrimitiveOwner::UMG, EAssetDocumentAuthorityState::NeedsMoreEvidence},
		{TEXT("UMG-ANIMATION-BINDING-APPLY-DELTA-v1"), EAssetDocumentPrimitiveOwner::UMG, EAssetDocumentAuthorityState::CandidateRejected},
		{TEXT("BLUEPRINT-TIMELINE-TRACK-APPLY-DELTA-v1"), EAssetDocumentPrimitiveOwner::Blueprint, EAssetDocumentAuthorityState::CandidateApproved},
		{TEXT("BLUEPRINT-TIMELINE-CURVE-APPLY-KEYS-v1"), EAssetDocumentPrimitiveOwner::Blueprint, EAssetDocumentAuthorityState::CandidateRejected},
		{TEXT("AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1"), EAssetDocumentPrimitiveOwner::AI, EAssetDocumentAuthorityState::BridgeAdmitted},
		{TEXT("AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1"), EAssetDocumentPrimitiveOwner::AI, EAssetDocumentAuthorityState::BridgeAdmitted},
        {TEXT("ANIMATION-MONTAGE-SLOTS-PUBLISH-v1"), EAssetDocumentPrimitiveOwner::Animation, EAssetDocumentAuthorityState::BridgeAdmitted},
        {TEXT("ANIMATION-MONTAGE-SECTIONS-PUBLISH-v1"), EAssetDocumentPrimitiveOwner::Animation, EAssetDocumentAuthorityState::BridgeAdmitted},
        {TEXT("ANIMATION-FLOAT-CURVE-EXACT-STATE-v1"), EAssetDocumentPrimitiveOwner::Animation, EAssetDocumentAuthorityState::BridgeAdmitted},
        {TEXT("ANIMATION-MONTAGE-NOTIFIES-REPLACE-v1"), EAssetDocumentPrimitiveOwner::Animation, EAssetDocumentAuthorityState::BridgeAdmitted},
        {TEXT("ANIMATION-MONTAGE-NOTIFY-STATES-REPLACE-v1"), EAssetDocumentPrimitiveOwner::Animation, EAssetDocumentAuthorityState::BridgeAdmitted},
        {TEXT("AD.AI.BT.GRAPH_INSTALL.v1"), EAssetDocumentPrimitiveOwner::AI, EAssetDocumentAuthorityState::BridgeAdmitted},
        {TEXT("AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1"), EAssetDocumentPrimitiveOwner::AI, EAssetDocumentAuthorityState::BridgeAdmitted},
	};
	if (Catalog.Num() != UE_ARRAY_COUNT(Expected))
	{
		return false;
	}
	for (int32 Index = 0; Index < Catalog.Num(); ++Index)
	{
		if (!MatchesFrozenIdentity(Catalog[Index], Expected[Index]))
		{
			return false;
		}
	}
	return true;
}
} // namespace UE::AssetDocument::AuthorityGovernance::Private

inline TConstArrayView<FAssetDocumentAuthorityCandidateRecord> GetFrozenAuthorityExceptionRecords()
{
	static const TArray<FAssetDocumentAuthorityCandidateRecord> Catalog =
		UE::AssetDocument::AuthorityGovernance::Private::BuildFrozenCatalog();
	return Catalog;
}

inline FAssetDocumentContractValidationResult ValidateFrozenAuthorityExceptionRecords()
{
	using namespace UE::AssetDocument::AuthorityGovernance::Private;
	const TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog = GetFrozenAuthorityExceptionRecords();
	const FAssetDocumentContractValidationResult GenericResult =
		ValidateAssetDocumentAuthorityExceptionCatalog(Catalog);
	if (!GenericResult.bValid)
	{
		return GenericResult;
	}
	if (!HasExactFrozenIdentities(Catalog))
	{
		return MakeValidationResult(false, TEXT("AuthorityCatalog.FrozenIdentityMismatch"), TEXT("Frozen authority identities or states changed."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}
