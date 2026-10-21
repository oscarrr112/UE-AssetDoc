// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Runtime/AssetDocumentAuthorityExceptionRegistry.h"

#include "Misc/AutomationTest.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentAuthorityExceptionRegistryContractTests
{
static_assert(static_cast<uint8>(EAssetDocumentAuthorityState::ObservedGap) == 0);
static_assert(static_cast<uint8>(EAssetDocumentAuthorityState::Researching) == 1);
static_assert(static_cast<uint8>(EAssetDocumentAuthorityState::CandidatePendingReview) == 2);
static_assert(static_cast<uint8>(EAssetDocumentAuthorityState::CandidateApproved) == 3);
static_assert(static_cast<uint8>(EAssetDocumentAuthorityState::NeedsMoreEvidence) == 4);
static_assert(static_cast<uint8>(EAssetDocumentAuthorityState::CandidateRejected) == 5);
static_assert(static_cast<uint8>(EAssetDocumentAuthorityState::HelperResolved) == 6);
static_assert(static_cast<uint8>(EAssetDocumentAuthorityState::HelperImplemented) == 7);
static_assert(static_cast<uint8>(EAssetDocumentAuthorityState::BridgePendingReview) == 8);
static_assert(static_cast<uint8>(EAssetDocumentAuthorityState::BridgeAdmitted) == 9);
static_assert(static_cast<uint8>(EAssetDocumentAuthorityState::BridgeRejected) == 10);
static_assert(static_cast<uint8>(EAssetDocumentIndependentVerdict::Pass) == 0);
static_assert(static_cast<uint8>(EAssetDocumentIndependentVerdict::Rework) == 1);
static_assert(static_cast<uint8>(EAssetDocumentIndependentVerdict::Reject) == 2);
static_assert(static_cast<uint8>(EAssetDocumentAuthorityReviewKind::Candidate) == 0);
static_assert(static_cast<uint8>(EAssetDocumentAuthorityReviewKind::Bridge) == 1);

#define ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(Type, Member, Expected) \
	static_assert(std::is_same_v<decltype(Type::Member), Expected>)
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityReviewRecord, ReviewId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityReviewRecord, ReviewKind, EAssetDocumentAuthorityReviewKind);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityReviewRecord, ReviewerId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityReviewRecord, Verdict, EAssetDocumentIndependentVerdict);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityReviewRecord, ReviewedState, EAssetDocumentAuthorityState);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityReviewRecord, EvidenceHash, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityReviewRecord, ReviewedHead, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityCandidateRecord, GovernanceId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityCandidateRecord, PhysicalOwner, EAssetDocumentPrimitiveOwner);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityCandidateRecord, ExactSurface, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityCandidateRecord, ImplementationSymbol, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityCandidateRecord, State, EAssetDocumentAuthorityState);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityCandidateRecord, ResearcherId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityCandidateRecord, ImplementerId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityCandidateRecord, ProviderOwnerId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityCandidateRecord, Reviews, TArray<FAssetDocumentAuthorityReviewRecord>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityCandidateRecord, ResubmissionCondition, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentAuthorityCandidateRecord, HelperReplacementCondition, FString);
#undef ASSET_DOCUMENT_ASSERT_MEMBER_TYPE

using FTransitionSignature = bool (*)(EAssetDocumentAuthorityState, EAssetDocumentAuthorityState);
using FFindCandidateSignature = const FAssetDocumentAuthorityCandidateRecord* (*)(
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord>, FStringView);
using FFindReviewSignature = const FAssetDocumentAuthorityReviewRecord* (*)(
	const FAssetDocumentAuthorityCandidateRecord&, FStringView);
using FValidateCatalogSignature = FAssetDocumentContractValidationResult (*)(
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord>);
using FGetFrozenSignature = TConstArrayView<FAssetDocumentAuthorityCandidateRecord> (*)();
using FValidateFrozenSignature = FAssetDocumentContractValidationResult (*)();
static_assert(std::is_same_v<decltype(&IsAssetDocumentAuthorityTransitionAllowed), FTransitionSignature>);
static_assert(std::is_same_v<decltype(&FindAssetDocumentAuthorityCandidateRecord), FFindCandidateSignature>);
static_assert(std::is_same_v<decltype(&FindAssetDocumentAuthorityReviewRecord), FFindReviewSignature>);
static_assert(std::is_same_v<decltype(&ValidateAssetDocumentAuthorityExceptionCatalog), FValidateCatalogSignature>);
static_assert(std::is_same_v<decltype(&GetG1FrozenAuthorityExceptionRecords), FGetFrozenSignature>);
static_assert(std::is_same_v<decltype(&ValidateG1FrozenAuthorityExceptionRecords), FValidateFrozenSignature>);

struct FFrozenExpectation
{
	const TCHAR* GovernanceId;
	EAssetDocumentPrimitiveOwner Owner;
	EAssetDocumentIndependentVerdict Verdict;
	EAssetDocumentAuthorityState State;
	const TCHAR* Surface;
	const TCHAR* ImplementationSymbol;
	const TCHAR* ProviderOwnerId;
	const TCHAR* ResubmissionCondition;
	const TCHAR* HelperReplacementCondition;
	const TCHAR* ReviewId;
	const TCHAR* ReviewerId;
	const TCHAR* EvidenceHash;
	const TCHAR* ReviewedHead;
};

static const FFrozenExpectation FrozenExpectations[] = {
	{
		TEXT("UMG-DESIGNER-BINDING-APPLY-DELTA-v1"), EAssetDocumentPrimitiveOwner::UMG,
		EAssetDocumentIndependentVerdict::Rework, EAssetDocumentAuthorityState::NeedsMoreEvidence,
		TEXT("(WidgetName, PropertyName)-identified Designer Binding add/update/remove"), TEXT(""),
		TEXT("p-umg-provider-owner"),
		TEXT("Freeze binding permission, extension policy, derived GUID, typed DTO/symbol, rollback, compile, and fresh-load evidence before independent re-review."), TEXT(""),
		TEXT("candidate-review.rereview/UMG-DESIGNER-BINDING-APPLY-DELTA-v1"),
		TEXT("umg-authority-exception-candidate-rereviewer"),
		TEXT("bda3d730c4f06ca2cf1d17ff70084dfc2a57ca46afe2c8a7076da1b1f7a03726"),
		TEXT("34e3e23bd932dc5951200e802c403f4638219e65"),
	},
	{
		TEXT("UMG-WIDGET-ANIMATION-CREATE-v1"), EAssetDocumentPrimitiveOwner::UMG,
		EAssetDocumentIndependentVerdict::Pass, EAssetDocumentAuthorityState::CandidateApproved,
		TEXT("one UWidgetAnimation plus owned UMovieScene creation and installation"),
		TEXT("AssetDocumentUMG::CreateWidgetAnimationHelperFirst"),
		TEXT("p-umg-provider-owner"), TEXT(""),
		TEXT("Replace with an exported UMGEditor CreateWidgetAnimation lifecycle helper when available; no bridge is admitted."),
		TEXT("candidate-review.initial/UMG-WIDGET-ANIMATION-CREATE-v1"),
		TEXT("authority-exception-candidate-reviewer"),
		TEXT("e06474f8da5605da77cbad596ba6ea1c0014d79ede162b68b0c96f546f64009a"),
		TEXT("4ca528c57e9f14751b38b5136aa831952691d44d"),
	},
	{
		TEXT("UMG-WIDGET-ANIMATION-DELETE-v1"), EAssetDocumentPrimitiveOwner::UMG,
		EAssetDocumentIndependentVerdict::Rework, EAssetDocumentAuthorityState::NeedsMoreEvidence,
		TEXT("one installed WidgetAnimation removal and variable/compile lifecycle"), TEXT(""),
		TEXT("p-umg-provider-owner"),
		TEXT("Freeze referenced-animation policy, transient GC retention, rollback order, typed DTO/symbol, compile, and fresh-load evidence before independent re-review."), TEXT(""),
		TEXT("candidate-review.rereview/UMG-WIDGET-ANIMATION-DELETE-v1"),
		TEXT("umg-authority-exception-candidate-rereviewer"),
		TEXT("bda3d730c4f06ca2cf1d17ff70084dfc2a57ca46afe2c8a7076da1b1f7a03726"),
		TEXT("34e3e23bd932dc5951200e802c403f4638219e65"),
	},
	{
		TEXT("UMG-ANIMATION-BINDING-APPLY-DELTA-v1"), EAssetDocumentPrimitiveOwner::UMG,
		EAssetDocumentIndependentVerdict::Reject, EAssetDocumentAuthorityState::CandidateRejected,
		TEXT("MovieScene possessable to Widget/root/slot binding add/update/remove"), TEXT(""),
		TEXT("p-umg-provider-owner"), TEXT(""), TEXT(""),
		TEXT("candidate-review.initial/UMG-ANIMATION-BINDING-APPLY-DELTA-v1"),
		TEXT("authority-exception-candidate-reviewer"),
		TEXT("e06474f8da5605da77cbad596ba6ea1c0014d79ede162b68b0c96f546f64009a"),
		TEXT("4ca528c57e9f14751b38b5136aa831952691d44d"),
	},
	{
		TEXT("BLUEPRINT-TIMELINE-TRACK-APPLY-DELTA-v1"), EAssetDocumentPrimitiveOwner::Blueprint,
		EAssetDocumentIndependentVerdict::Pass, EAssetDocumentAuthorityState::CandidateApproved,
		TEXT("one Timeline track lifecycle, distinct from template lifecycle and key replacement"),
		TEXT("AssetDocumentBlueprint::ApplyTimelineTrackDeltaHelperFirst"),
		TEXT("p-blueprint-provider-owner"), TEXT(""),
		TEXT("Replace with an exported UnrealEd/Kismet Timeline track lifecycle helper when available; no bridge is admitted."),
		TEXT("candidate-review.initial/BLUEPRINT-TIMELINE-TRACK-APPLY-DELTA-v1"),
		TEXT("authority-exception-candidate-reviewer"),
		TEXT("e06474f8da5605da77cbad596ba6ea1c0014d79ede162b68b0c96f546f64009a"),
		TEXT("4ca528c57e9f14751b38b5136aa831952691d44d"),
	},
	{
		TEXT("BLUEPRINT-TIMELINE-CURVE-APPLY-KEYS-v1"), EAssetDocumentPrimitiveOwner::Blueprint,
		EAssetDocumentIndependentVerdict::Reject, EAssetDocumentAuthorityState::CandidateRejected,
		TEXT("key replacement on an already resolved owned Timeline curve"), TEXT(""),
		TEXT("p-blueprint-provider-owner"), TEXT(""), TEXT(""),
		TEXT("candidate-review.initial/BLUEPRINT-TIMELINE-CURVE-APPLY-KEYS-v1"),
		TEXT("authority-exception-candidate-reviewer"),
		TEXT("e06474f8da5605da77cbad596ba6ea1c0014d79ede162b68b0c96f546f64009a"),
		TEXT("4ca528c57e9f14751b38b5136aa831952691d44d"),
	},
};

struct FBridgeExpectation
{
	const TCHAR* GovernanceId;
	const TCHAR* Surface;
	const TCHAR* ImplementationSymbol;
	const TCHAR* CandidateReviewerId;
	const TCHAR* CandidateEvidenceHash;
	const TCHAR* BridgeReviewerId;
	const TCHAR* BridgeEvidenceHash;
};

static const FBridgeExpectation BridgeExpectations[] = {
	{
		TEXT("AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1"),
		TEXT("One NewObject<UBlackboardKeyType> staging/materialization callsite for add and replace-class only."),
		TEXT("AssetDocumentAIBlackboardStructuralPublication::StageKeyType"),
		TEXT("/root/main_keytype_candidate_review"),
		TEXT("d53888bc04f986eb835e442e7fa87389700a1e9fe8f4b465c11ed6fef38b6d65"),
		TEXT("/root/main_keytype_bridge_review"),
		TEXT("7ecd0a82233442960c4097248442eb0aa6aca0c0287af98ace9490fd6b8c4217"),
	},
	{
		TEXT("AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1"),
		TEXT("One complete UBlackboardData::Keys snapshot publication callsite for the four v2 rows and the exact immutable v1 compatibility caller."),
		TEXT("AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot"),
		TEXT("/root/main_local_keys_candidate_review"),
		TEXT("c67dc7df1cf67317bf9e10e1f1ff41a2d07a8b329d4081406806be5cbcd60746"),
		TEXT("/root/main_local_keys_bridge_review"),
		TEXT("66f0e1194458077c4ae50c3150e5b9556a383d85d32b08efddd3dd6153debd5d"),
	},
};

bool IsExpectedBridgeGovernanceId(FStringView GovernanceId)
{
	for (const FBridgeExpectation& Expected : BridgeExpectations)
	{
		if (GovernanceId == Expected.GovernanceId)
		{
			return true;
		}
	}
	return false;
}

void TestDefaults(FAutomationTestBase& Test)
{
	const FAssetDocumentAuthorityReviewRecord Review;
	Test.TestTrue(TEXT("S: review id defaults empty"), Review.ReviewId.IsEmpty());
	Test.TestEqual(TEXT("S: review kind defaults candidate"), Review.ReviewKind, EAssetDocumentAuthorityReviewKind::Candidate);
	Test.TestTrue(TEXT("S: reviewer id defaults empty"), Review.ReviewerId.IsEmpty());
	Test.TestEqual(TEXT("S: verdict defaults rework"), Review.Verdict, EAssetDocumentIndependentVerdict::Rework);
	Test.TestEqual(TEXT("S: reviewed state defaults candidate pending"), Review.ReviewedState, EAssetDocumentAuthorityState::CandidatePendingReview);
	Test.TestTrue(TEXT("S: evidence hash defaults empty"), Review.EvidenceHash.IsEmpty());
	Test.TestTrue(TEXT("S: reviewed head defaults empty"), Review.ReviewedHead.IsEmpty());

	const FAssetDocumentAuthorityCandidateRecord Candidate;
	Test.TestTrue(TEXT("S: governance id defaults empty"), Candidate.GovernanceId.IsEmpty());
	Test.TestEqual(TEXT("S: physical owner defaults core"), Candidate.PhysicalOwner, EAssetDocumentPrimitiveOwner::Core);
	Test.TestTrue(TEXT("S: exact surface defaults empty"), Candidate.ExactSurface.IsEmpty());
	Test.TestTrue(TEXT("S: implementation symbol defaults empty"), Candidate.ImplementationSymbol.IsEmpty());
	Test.TestEqual(TEXT("S: state defaults observed gap"), Candidate.State, EAssetDocumentAuthorityState::ObservedGap);
	Test.TestTrue(TEXT("S: researcher defaults empty"), Candidate.ResearcherId.IsEmpty());
	Test.TestTrue(TEXT("S: implementer defaults empty"), Candidate.ImplementerId.IsEmpty());
	Test.TestTrue(TEXT("S: provider owner defaults empty"), Candidate.ProviderOwnerId.IsEmpty());
	Test.TestTrue(TEXT("S: reviews default empty"), Candidate.Reviews.IsEmpty());
	Test.TestTrue(TEXT("S: resubmission condition defaults empty"), Candidate.ResubmissionCondition.IsEmpty());
	Test.TestTrue(TEXT("S: helper replacement condition defaults empty"), Candidate.HelperReplacementCondition.IsEmpty());
}

bool IsExpectedTransition(EAssetDocumentAuthorityState From, EAssetDocumentAuthorityState To)
{
	static const TPair<EAssetDocumentAuthorityState, EAssetDocumentAuthorityState> Expected[] = {
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
	for (const auto& Edge : Expected)
	{
		if (Edge.Key == From && Edge.Value == To)
		{
			return true;
		}
	}
	return false;
}

void TestTransitionGraph(FAutomationTestBase& Test)
{
	for (uint8 From = 0; From <= static_cast<uint8>(EAssetDocumentAuthorityState::BridgeRejected); ++From)
	{
		for (uint8 To = 0; To <= static_cast<uint8>(EAssetDocumentAuthorityState::BridgeRejected); ++To)
		{
			const EAssetDocumentAuthorityState FromState = static_cast<EAssetDocumentAuthorityState>(From);
			const EAssetDocumentAuthorityState ToState = static_cast<EAssetDocumentAuthorityState>(To);
			Test.TestEqual(
				*FString::Printf(TEXT("P: transition %u -> %u is exact"), From, To),
				IsAssetDocumentAuthorityTransitionAllowed(FromState, ToState),
				IsExpectedTransition(FromState, ToState));
		}
	}
}

void TestFrozenExpectation(
	FAutomationTestBase& Test,
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog,
	const FFrozenExpectation& Expected)
{
	const FAssetDocumentAuthorityCandidateRecord* Candidate =
		FindAssetDocumentAuthorityCandidateRecord(Catalog, Expected.GovernanceId);
	Test.TestNotNull(*FString::Printf(TEXT("P: %s is queryable"), Expected.GovernanceId), Candidate);
	if (Candidate == nullptr)
	{
		return;
	}
	Test.TestEqual(TEXT("P: physical owner is exact"), Candidate->PhysicalOwner, Expected.Owner);
	Test.TestEqual(TEXT("P: state is exact"), Candidate->State, Expected.State);
	Test.TestEqual(TEXT("P: reviewed surface is exact"), Candidate->ExactSurface, FString(Expected.Surface));
	Test.TestEqual(TEXT("P: implementation symbol is exact"), Candidate->ImplementationSymbol, FString(Expected.ImplementationSymbol));
	Test.TestEqual(TEXT("P: researcher identity is exact"), Candidate->ResearcherId, FString(TEXT("w0-umg-candidate-researcher")));
	Test.TestEqual(TEXT("P: implementer identity is exact"), Candidate->ImplementerId, FString(TEXT("g1-contract-owner")));
	Test.TestEqual(TEXT("P: provider owner identity is exact"), Candidate->ProviderOwnerId, FString(Expected.ProviderOwnerId));
	Test.TestEqual(TEXT("P: resubmission condition is exact"), Candidate->ResubmissionCondition, FString(Expected.ResubmissionCondition));
	Test.TestEqual(TEXT("P: helper replacement condition is exact"), Candidate->HelperReplacementCondition, FString(Expected.HelperReplacementCondition));
	const FAssetDocumentAuthorityReviewRecord* Review =
		FindAssetDocumentAuthorityReviewRecord(*Candidate, Expected.ReviewId);
	Test.TestNotNull(TEXT("P: evidence review is queryable"), Review);
	if (Review == nullptr)
	{
		return;
	}
	Test.TestEqual(TEXT("P: review kind is candidate"), Review->ReviewKind, EAssetDocumentAuthorityReviewKind::Candidate);
	Test.TestEqual(TEXT("P: verdict is exact"), Review->Verdict, Expected.Verdict);
	Test.TestEqual(TEXT("P: reviewed state is exact"), Review->ReviewedState, Expected.State);
	Test.TestEqual(TEXT("P: reviewer identity is exact"), Review->ReviewerId, FString(Expected.ReviewerId));
	Test.TestEqual(TEXT("P: evidence hash is exact"), Review->EvidenceHash, FString(Expected.EvidenceHash));
	Test.TestEqual(TEXT("P: reviewed head is exact"), Review->ReviewedHead, FString(Expected.ReviewedHead));
}

void TestFrozenConditions(FAutomationTestBase& Test, const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	const bool bNeedsMoreEvidence = Candidate.State == EAssetDocumentAuthorityState::NeedsMoreEvidence;
	const bool bImplementationBearing = Candidate.State == EAssetDocumentAuthorityState::CandidateApproved
		|| Candidate.State == EAssetDocumentAuthorityState::BridgeAdmitted;
	Test.TestEqual(TEXT("P: only NeedsMoreEvidence retains resubmission"), !Candidate.ResubmissionCondition.IsEmpty(), bNeedsMoreEvidence);
	Test.TestEqual(TEXT("P: implementation-bearing rows retain helper replacement"), !Candidate.HelperReplacementCondition.IsEmpty(), bImplementationBearing);
	Test.TestEqual(TEXT("P: implementation-bearing rows name an implementation symbol"), !Candidate.ImplementationSymbol.IsEmpty(), bImplementationBearing);
}

void TestFrozenBridgeExpectation(
	FAutomationTestBase& Test,
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog,
	const FBridgeExpectation& Expected)
{
	const FAssetDocumentAuthorityCandidateRecord* Candidate =
		FindAssetDocumentAuthorityCandidateRecord(Catalog, Expected.GovernanceId);
	Test.TestNotNull(TEXT("P: admitted Blackboard structural bridge is queryable"), Candidate);
	if (Candidate == nullptr)
	{
		return;
	}
	Test.TestEqual(TEXT("P: structural bridge owner is AI"), Candidate->PhysicalOwner, EAssetDocumentPrimitiveOwner::AI);
	Test.TestEqual(TEXT("P: structural bridge state is admitted"), Candidate->State, EAssetDocumentAuthorityState::BridgeAdmitted);
	Test.TestEqual(TEXT("P: structural bridge exact surface is frozen"), Candidate->ExactSurface, FString(Expected.Surface));
	Test.TestEqual(TEXT("P: structural bridge implementation symbol is frozen"), Candidate->ImplementationSymbol, FString(Expected.ImplementationSymbol));
	Test.TestEqual(TEXT("P: structural bridge retains exactly candidate and bridge reviews"), Candidate->Reviews.Num(), 2);

	const FString CandidateReviewId = FString::Printf(TEXT("candidate-review.initial/%s"), Expected.GovernanceId);
	const FString BridgeReviewId = FString::Printf(TEXT("bridge-review.initial/%s"), Expected.GovernanceId);
	const FAssetDocumentAuthorityReviewRecord* CandidateReview =
		FindAssetDocumentAuthorityReviewRecord(*Candidate, CandidateReviewId);
	const FAssetDocumentAuthorityReviewRecord* BridgeReview =
		FindAssetDocumentAuthorityReviewRecord(*Candidate, BridgeReviewId);
	Test.TestNotNull(TEXT("P: exact candidate review is bound"), CandidateReview);
	Test.TestNotNull(TEXT("P: exact bridge review is bound"), BridgeReview);
	if (CandidateReview == nullptr || BridgeReview == nullptr)
	{
		return;
	}
	Test.TestEqual(TEXT("P: candidate review kind is exact"), CandidateReview->ReviewKind, EAssetDocumentAuthorityReviewKind::Candidate);
	Test.TestEqual(TEXT("P: candidate review verdict is Pass"), CandidateReview->Verdict, EAssetDocumentIndependentVerdict::Pass);
	Test.TestEqual(TEXT("P: candidate review state is CandidateApproved"), CandidateReview->ReviewedState, EAssetDocumentAuthorityState::CandidateApproved);
	Test.TestEqual(TEXT("P: candidate reviewer is the eligible Task-2 reviewer"), CandidateReview->ReviewerId, FString(Expected.CandidateReviewerId));
	Test.TestEqual(TEXT("P: candidate review evidence digest is exact"), CandidateReview->EvidenceHash, FString(Expected.CandidateEvidenceHash));
	Test.TestEqual(TEXT("P: candidate review head is exact"), CandidateReview->ReviewedHead, FString(TEXT("aebdffa6c30242629d4cd0df6156fc9e23eadc68")));
	Test.TestEqual(TEXT("P: bridge review kind is exact"), BridgeReview->ReviewKind, EAssetDocumentAuthorityReviewKind::Bridge);
	Test.TestEqual(TEXT("P: bridge review verdict is Pass"), BridgeReview->Verdict, EAssetDocumentIndependentVerdict::Pass);
	Test.TestEqual(TEXT("P: bridge review state is BridgeAdmitted"), BridgeReview->ReviewedState, EAssetDocumentAuthorityState::BridgeAdmitted);
	Test.TestEqual(TEXT("P: bridge reviewer is the distinct eligible Task-2 reviewer"), BridgeReview->ReviewerId, FString(Expected.BridgeReviewerId));
	Test.TestEqual(TEXT("P: bridge review evidence digest is exact"), BridgeReview->EvidenceHash, FString(Expected.BridgeEvidenceHash));
	Test.TestEqual(TEXT("P: bridge review head is exact"), BridgeReview->ReviewedHead, FString(TEXT("aebdffa6c30242629d4cd0df6156fc9e23eadc68")));
	Test.TestNotEqual(TEXT("P: candidate and bridge reviewers are distinct"), CandidateReview->ReviewerId, BridgeReview->ReviewerId);
}

void TestFrozenCatalog(FAutomationTestBase& Test)
{
	const TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog = GetG1FrozenAuthorityExceptionRecords();
	Test.TestEqual(TEXT("P: frozen catalog contains fifteen governance rows"), Catalog.Num(), 15);
	Test.TestTrue(TEXT("P: frozen catalog validates"), ValidateG1FrozenAuthorityExceptionRecords().bValid);
	Test.TestTrue(TEXT("P: generic validator accepts the frozen catalog"), ValidateAssetDocumentAuthorityExceptionCatalog(Catalog).bValid);
	Test.TestNull(TEXT("P: unknown governance id is absent"), FindAssetDocumentAuthorityCandidateRecord(Catalog, TEXT("UMG-UNKNOWN-v1")));
	int32 Approved = 0;
	int32 NeedsEvidence = 0;
	int32 Rejected = 0;
	int32 BridgeAdmitted = 0;
	for (const FAssetDocumentAuthorityCandidateRecord& Candidate : Catalog)
	{
		Approved += Candidate.State == EAssetDocumentAuthorityState::CandidateApproved;
		NeedsEvidence += Candidate.State == EAssetDocumentAuthorityState::NeedsMoreEvidence;
		Rejected += Candidate.State == EAssetDocumentAuthorityState::CandidateRejected;
		BridgeAdmitted += Candidate.State == EAssetDocumentAuthorityState::BridgeAdmitted;
		Test.TestEqual(TEXT("P: only four exact approved AI governance ids contain dots"),
			Candidate.GovernanceId.Contains(TEXT(".")), (IsExpectedBridgeGovernanceId(Candidate.GovernanceId)
             || Candidate.GovernanceId == TEXT("AD.AI.BT.GRAPH_INSTALL.v1")
             || Candidate.GovernanceId == TEXT("AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1")));
		for (const FAssetDocumentAuthorityReviewRecord& Review : Candidate.Reviews)
		{
			if (Candidate.State != EAssetDocumentAuthorityState::BridgeAdmitted)
			{
				Test.TestEqual(TEXT("P: historical frozen rows retain candidate reviews only"), Review.ReviewKind, EAssetDocumentAuthorityReviewKind::Candidate);
			}
		}
		TestFrozenConditions(Test, Candidate);
	}
	Test.TestEqual(TEXT("P: exactly two candidates are approved"), Approved, 2);
	Test.TestEqual(TEXT("P: exactly two candidates need more evidence"), NeedsEvidence, 2);
	Test.TestEqual(TEXT("P: exactly two candidates are rejected"), Rejected, 2);
	Test.TestEqual(TEXT("P: exactly nine bridges are admitted"), BridgeAdmitted, 9);
	if (Catalog.Num() == 15)
	{
		Test.TestEqual(TEXT("P: structural staging is appended first"), Catalog[6].GovernanceId, FString(BridgeExpectations[0].GovernanceId));
		Test.TestEqual(TEXT("P: structural publication is appended second"), Catalog[7].GovernanceId, FString(BridgeExpectations[1].GovernanceId));
	}
 for (int32 I = 8; I < FMath::Min(10, Catalog.Num()); ++I)
 {
  const auto& C = Catalog[I];
  Test.TestEqual(TEXT("Montage physical owner"), C.PhysicalOwner, EAssetDocumentPrimitiveOwner::Animation);
  Test.TestEqual(TEXT("exact Montage append order"), C.GovernanceId, FString(I == 8 ? TEXT("ANIMATION-MONTAGE-SLOTS-PUBLISH-v1") : TEXT("ANIMATION-MONTAGE-SECTIONS-PUBLISH-v1")));
  Test.TestEqual(TEXT("Montage has two actual reviews"), C.Reviews.Num(), 2);
  if (C.Reviews.Num() == 2)
  {
   Test.TestNotEqual(TEXT("Montage reviewer identities differ"), C.Reviews[0].ReviewerId, C.Reviews[1].ReviewerId);
   Test.TestEqual(TEXT("native candidate reviewed head"), C.Reviews[0].ReviewedHead, FString(TEXT("4aeea7541f3000b2211ab6719b6041d1b6998f85")));
   Test.TestEqual(TEXT("prepared boundary reviewed head"), C.Reviews[1].ReviewedHead, FString(TEXT("80e7074aa58df8a441df54c68e3609fa7faf2d6a")));
  }
 }
 if (Catalog.Num() == 15)
 {
  const auto& C = Catalog[10];
  Test.TestEqual(TEXT("C0 exact append identity"), C.GovernanceId, FString(TEXT("ANIMATION-FLOAT-CURVE-EXACT-STATE-v1")));
  Test.TestEqual(TEXT("C0 physical owner"), C.PhysicalOwner, EAssetDocumentPrimitiveOwner::Animation);
  Test.TestEqual(TEXT("C0 actual source reviews"), C.Reviews.Num(), 2);
  if (C.Reviews.Num() == 2)
  {
   Test.TestNotEqual(TEXT("C0 reviewers differ"), C.Reviews[0].ReviewerId, C.Reviews[1].ReviewerId);
   Test.TestEqual(TEXT("C0 candidate source head"), C.Reviews[0].ReviewedHead, FString(TEXT("1e3f186d5ee49e20d5a402026450dc1284d09942")));
   Test.TestEqual(TEXT("C0 bridge source head"), C.Reviews[1].ReviewedHead, FString(TEXT("2ec1903b0672f82b1a45c928184bc2a1b5b8ccd0")));
   Test.TestEqual(TEXT("C0 bridge review kind"), C.Reviews[1].ReviewKind, EAssetDocumentAuthorityReviewKind::Bridge);
  }
 }
 for (int32 I = 11; I < FMath::Min(13, Catalog.Num()); ++I)
 {
  const auto& C = Catalog[I];
  Test.TestEqual(TEXT("notify exact append identity"), C.GovernanceId, FString(I == 11 ? TEXT("ANIMATION-MONTAGE-NOTIFIES-REPLACE-v1") : TEXT("ANIMATION-MONTAGE-NOTIFY-STATES-REPLACE-v1")));
  Test.TestEqual(TEXT("notify actual reviews"), C.Reviews.Num(), 2);
  if (C.Reviews.Num() == 2)
  {
   Test.TestNotEqual(TEXT("notify reviewers independent"), C.Reviews[0].ReviewerId, C.Reviews[1].ReviewerId);
   Test.TestEqual(TEXT("notify candidate source head"), C.Reviews[0].ReviewedHead, FString(TEXT("dce40e326922a2ed49f14040391cacf6fe8b5b6c")));
   Test.TestEqual(TEXT("notify bridge source head"), C.Reviews[1].ReviewedHead, FString(TEXT("ff48b6ee1149262917d1e1153edc4939338fae84")));
  }
 }
	for (const FFrozenExpectation& Expected : FrozenExpectations)
	{
		TestFrozenExpectation(Test, Catalog, Expected);
	}
	for (const TCHAR* Id : {TEXT("AD.AI.BT.GRAPH_INSTALL.v1"), TEXT("AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1")})
	{
		const auto* C = FindAssetDocumentAuthorityCandidateRecord(Catalog, Id);
		if (!Test.TestNotNull(TEXT("exact BT bridge record exists"), C)) continue;
		Test.TestEqual(TEXT("BT bridge has both actual review roles"), C->Reviews.Num(), 2);
		if (C->Reviews.Num() == 2)
		{
			Test.TestNotEqual(TEXT("BT candidate and bridge reviewers differ"), C->Reviews[0].ReviewerId, C->Reviews[1].ReviewerId);
			Test.TestEqual(TEXT("latest candidate covers V2 and INSTALL"), C->Reviews[0].ReviewedHead, FString(TEXT("2d07c76b5a65155346639f8967b4592a6218b17b")));
		}
		TArray<FAssetDocumentAuthorityCandidateRecord> Invalid{*C};
		Invalid[0].PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
		Test.TestFalse(TEXT("exact BT identity rejects wrong owner"), ValidateAssetDocumentAuthorityExceptionCatalog(Invalid).bValid);
		Invalid[0] = *C; Invalid[0].GovernanceId += TEXT(".unapproved-v1");
		Test.TestFalse(TEXT("similar dotted identity is not admitted"), ValidateAssetDocumentAuthorityExceptionCatalog(Invalid).bValid);
		Invalid[0] = *C; Invalid[0].GovernanceId.LeftChopInline(1); Invalid[0].GovernanceId += TEXT("2");
		Test.TestFalse(TEXT("unapproved version is not admitted"), ValidateAssetDocumentAuthorityExceptionCatalog(Invalid).bValid);
	}
	for (const FBridgeExpectation& Expected : BridgeExpectations)
	{
		TestFrozenBridgeExpectation(Test, Catalog, Expected);
	}
}

FAssetDocumentAuthorityReviewRecord MakeReview(
	const TCHAR* ReviewId,
	EAssetDocumentAuthorityReviewKind Kind,
	const TCHAR* ReviewerId,
	EAssetDocumentIndependentVerdict Verdict,
	EAssetDocumentAuthorityState ReviewedState)
{
	FAssetDocumentAuthorityReviewRecord Review;
	Review.ReviewId = ReviewId;
	Review.ReviewKind = Kind;
	Review.ReviewerId = ReviewerId;
	Review.Verdict = Verdict;
	Review.ReviewedState = ReviewedState;
	Review.EvidenceHash = FString::Printf(TEXT("sha256:%s"), ReviewId);
	Review.ReviewedHead = TEXT("0123456789abcdef0123456789abcdef01234567");
	return Review;
}

FAssetDocumentAuthorityCandidateRecord MakeSyntheticBridgeCandidate()
{
	FAssetDocumentAuthorityCandidateRecord Candidate;
	Candidate.GovernanceId = TEXT("UMG-SYNTHETIC-BRIDGE-v1");
	Candidate.PhysicalOwner = EAssetDocumentPrimitiveOwner::UMG;
	Candidate.ExactSurface = TEXT("intentional test-only bridge governance fixture");
	Candidate.ImplementationSymbol = TEXT("AssetDocumentUMG::SyntheticBridgeFixture");
	Candidate.State = EAssetDocumentAuthorityState::BridgeAdmitted;
	Candidate.ResearcherId = TEXT("w0-umg-synthetic-researcher");
	Candidate.ImplementerId = TEXT("p-umg-synthetic-implementer");
	Candidate.ProviderOwnerId = TEXT("p-umg-synthetic-owner");
	Candidate.HelperReplacementCondition = TEXT("Remove this fixture after the bridge evidence contract changes.");
	Candidate.Reviews.Add(MakeReview(
		TEXT("synthetic-candidate-review"), EAssetDocumentAuthorityReviewKind::Candidate,
		TEXT("synthetic-candidate-reviewer"), EAssetDocumentIndependentVerdict::Pass,
		EAssetDocumentAuthorityState::CandidateApproved));
	Candidate.Reviews.Add(MakeReview(
		TEXT("synthetic-bridge-review"), EAssetDocumentAuthorityReviewKind::Bridge,
		TEXT("synthetic-bridge-reviewer"), EAssetDocumentIndependentVerdict::Pass,
		EAssetDocumentAuthorityState::BridgeAdmitted));
	return Candidate;
}

void TestInvalidCatalog(
	FAutomationTestBase& Test,
	const TCHAR* What,
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog)
{
	const FAssetDocumentContractValidationResult Result = ValidateAssetDocumentAuthorityExceptionCatalog(Catalog);
	Test.TestFalse(*FString::Printf(TEXT("P: %s is rejected"), What), Result.bValid);
	Test.TestTrue(*FString::Printf(TEXT("P: %s emits a diagnostic"), What), !Result.Diagnostics.IsEmpty());
}

void TestSyntheticBridgePositive(FAutomationTestBase& Test)
{
	const FAssetDocumentAuthorityCandidateRecord Candidate = MakeSyntheticBridgeCandidate();
	Test.TestTrue(
		TEXT("P: isolated synthetic bridge with separate independent Pass reviews validates"),
		ValidateAssetDocumentAuthorityExceptionCatalog(MakeArrayView(&Candidate, 1)).bValid);
	const FAssetDocumentAuthorityReviewRecord* CandidateReview =
		FindAssetDocumentAuthorityReviewRecord(Candidate, TEXT("synthetic-candidate-review"));
	const FAssetDocumentAuthorityReviewRecord* BridgeReview =
		FindAssetDocumentAuthorityReviewRecord(Candidate, TEXT("synthetic-bridge-review"));
	Test.TestNotNull(TEXT("P: synthetic candidate review resolves by exact id"), CandidateReview);
	Test.TestNotNull(TEXT("P: synthetic bridge review resolves by exact id"), BridgeReview);
	Test.TestNull(TEXT("P: unknown review id is absent"), FindAssetDocumentAuthorityReviewRecord(Candidate, TEXT("missing-review")));
}

void TestSyntheticBridgeReviewShape(FAutomationTestBase& Test)
{
	FAssetDocumentAuthorityCandidateRecord Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[0].ReviewKind = EAssetDocumentAuthorityReviewKind::Bridge;
	TestInvalidCatalog(Test, TEXT("candidate review with bridge kind"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[0].Verdict = EAssetDocumentIndependentVerdict::Rework;
	TestInvalidCatalog(Test, TEXT("candidate review without Pass"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[0].ReviewedState = EAssetDocumentAuthorityState::CandidatePendingReview;
	TestInvalidCatalog(Test, TEXT("candidate review with wrong reviewed state"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[1].ReviewKind = EAssetDocumentAuthorityReviewKind::Candidate;
	TestInvalidCatalog(Test, TEXT("bridge review with candidate kind"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[1].Verdict = EAssetDocumentIndependentVerdict::Reject;
	TestInvalidCatalog(Test, TEXT("bridge review without Pass"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[1].ReviewedState = EAssetDocumentAuthorityState::BridgePendingReview;
	TestInvalidCatalog(Test, TEXT("bridge review with wrong reviewed state"), MakeArrayView(&Invalid, 1));
}

void TestSyntheticBridgeEvidenceAndIndependence(FAutomationTestBase& Test)
{
	FAssetDocumentAuthorityCandidateRecord Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[1].ReviewerId = Invalid.Reviews[0].ReviewerId;
	TestInvalidCatalog(Test, TEXT("same reviewer for candidate and bridge"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[0].ReviewerId = Invalid.ResearcherId;
	TestInvalidCatalog(Test, TEXT("candidate researcher self-review"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[1].ReviewerId = Invalid.ImplementerId;
	TestInvalidCatalog(Test, TEXT("bridge implementer self-review"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[1].ReviewerId = Invalid.ProviderOwnerId;
	TestInvalidCatalog(Test, TEXT("bridge provider owner self-review"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[0].EvidenceHash.Reset();
	TestInvalidCatalog(Test, TEXT("candidate review without evidence hash"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[0].ReviewedHead.Reset();
	TestInvalidCatalog(Test, TEXT("candidate review without reviewed head"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[1].EvidenceHash.Reset();
	TestInvalidCatalog(Test, TEXT("bridge review without evidence hash"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticBridgeCandidate();
	Invalid.Reviews[1].ReviewedHead.Reset();
	TestInvalidCatalog(Test, TEXT("bridge review without reviewed head"), MakeArrayView(&Invalid, 1));
}

FAssetDocumentAuthorityCandidateRecord MakeSyntheticApprovedCandidate()
{
	FAssetDocumentAuthorityCandidateRecord Candidate = MakeSyntheticBridgeCandidate();
	Candidate.GovernanceId = TEXT("UMG-SYNTHETIC-CANDIDATE-v1");
	Candidate.ImplementationSymbol = TEXT("AssetDocumentUMG::SyntheticCandidateFixture");
	Candidate.State = EAssetDocumentAuthorityState::CandidateApproved;
	Candidate.Reviews.SetNum(1);
	return Candidate;
}

FAssetDocumentAuthorityCandidateRecord MakeSyntheticNeedsEvidenceCandidate()
{
	FAssetDocumentAuthorityCandidateRecord Candidate = MakeSyntheticApprovedCandidate();
	Candidate.GovernanceId = TEXT("UMG-SYNTHETIC-REWORK-v1");
	Candidate.ImplementationSymbol.Reset();
	Candidate.State = EAssetDocumentAuthorityState::NeedsMoreEvidence;
	Candidate.ResubmissionCondition = TEXT("Freeze the missing exact candidate contract and resubmit.");
	Candidate.HelperReplacementCondition.Reset();
	Candidate.Reviews[0].Verdict = EAssetDocumentIndependentVerdict::Rework;
	Candidate.Reviews[0].ReviewedState = EAssetDocumentAuthorityState::NeedsMoreEvidence;
	return Candidate;
}

void TestExactDottedGovernanceIdentityExceptions(FAutomationTestBase& Test)
{
	for (const FBridgeExpectation& Expected : BridgeExpectations)
	{
		EAssetDocumentPrimitiveOwner Owner = EAssetDocumentPrimitiveOwner::Core;
		Test.TestTrue(TEXT("P: exact structural governance literal resolves an owner"),
			UE::AssetDocument::AuthorityGovernance::Private::ResolveGovernanceOwner(Expected.GovernanceId, Owner));
		Test.TestEqual(TEXT("P: exact structural governance literal resolves AI"), Owner, EAssetDocumentPrimitiveOwner::AI);
		FAssetDocumentAuthorityCandidateRecord Candidate = MakeSyntheticBridgeCandidate();
		Candidate.GovernanceId = Expected.GovernanceId;
		Candidate.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
		Test.TestTrue(TEXT("P: exact structural governance literal has a valid identity"),
			UE::AssetDocument::AuthorityGovernance::Private::HasGovernanceIdentity(Candidate));
	}

	static const TCHAR* RejectedDottedOrAdIds[] = {
		TEXT("AD.AI.BLACKBOARD.KEYTYPE-STAGING-v2"),
		TEXT("AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1.extra"),
		TEXT("AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1-extra"),
		TEXT("AD.AI.BLACKBOARD.OTHER-v1"),
		TEXT("AI-DOTTED.GOVERNANCE-v1"),
		TEXT("EDITOR-AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1"),
	};
	for (const TCHAR* GovernanceId : RejectedDottedOrAdIds)
	{
		FAssetDocumentAuthorityCandidateRecord Candidate = MakeSyntheticBridgeCandidate();
		Candidate.GovernanceId = GovernanceId;
		Candidate.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
		Test.TestFalse(TEXT("P: every other dotted or AD governance id is rejected"),
			UE::AssetDocument::AuthorityGovernance::Private::HasGovernanceIdentity(Candidate));
	}

	EAssetDocumentPrimitiveOwner HistoricalOwner = EAssetDocumentPrimitiveOwner::Core;
	Test.TestTrue(TEXT("P: historical hyphenated AI prefix still resolves"),
		UE::AssetDocument::AuthorityGovernance::Private::ResolveGovernanceOwner(TEXTVIEW("AI-HISTORICAL-v1"), HistoricalOwner));
	Test.TestEqual(TEXT("P: historical hyphenated AI prefix still maps to AI"), HistoricalOwner, EAssetDocumentPrimitiveOwner::AI);
}

void TestCatalogIdentityRejections(FAutomationTestBase& Test)
{
	const FAssetDocumentAuthorityCandidateRecord Valid = MakeSyntheticApprovedCandidate();
	Test.TestTrue(TEXT("P: generic candidate catalog validates"),
		ValidateAssetDocumentAuthorityExceptionCatalog(MakeArrayView(&Valid, 1)).bValid);
	FAssetDocumentAuthorityCandidateRecord Invalid = Valid;
	Invalid.GovernanceId = TEXT("UMG-SYNTHETIC.CANDIDATE-v1");
	TestInvalidCatalog(Test, TEXT("runtime primitive namespace collision"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.PhysicalOwner = EAssetDocumentPrimitiveOwner::Blueprint;
	TestInvalidCatalog(Test, TEXT("governance prefix and physical owner mismatch"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.ExactSurface.Reset();
	TestInvalidCatalog(Test, TEXT("missing exact surface"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.ImplementationSymbol.Reset();
	TestInvalidCatalog(Test, TEXT("approved candidate without implementation symbol"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.HelperReplacementCondition.Reset();
	TestInvalidCatalog(Test, TEXT("approved candidate without helper replacement condition"), MakeArrayView(&Invalid, 1));
}

void TestCatalogIndependenceIdentityRejections(FAutomationTestBase& Test)
{
	const FAssetDocumentAuthorityCandidateRecord Valid = MakeSyntheticApprovedCandidate();
	FAssetDocumentAuthorityCandidateRecord Invalid = Valid;
	Invalid.ResearcherId.Reset();
	TestInvalidCatalog(Test, TEXT("missing researcher identity"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.ImplementerId.Reset();
	TestInvalidCatalog(Test, TEXT("missing implementer identity"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.ProviderOwnerId.Reset();
	TestInvalidCatalog(Test, TEXT("missing provider owner identity"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.Reviews[0].ReviewerId = Invalid.ResearcherId;
	TestInvalidCatalog(Test, TEXT("reviewer equals researcher"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.Reviews[0].ReviewerId = Invalid.ImplementerId;
	TestInvalidCatalog(Test, TEXT("reviewer equals implementer"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.Reviews[0].ReviewerId = Invalid.ProviderOwnerId;
	TestInvalidCatalog(Test, TEXT("reviewer equals provider owner"), MakeArrayView(&Invalid, 1));
}

void TestCatalogReviewFieldRejections(FAutomationTestBase& Test)
{
	const FAssetDocumentAuthorityCandidateRecord Valid = MakeSyntheticApprovedCandidate();
	FAssetDocumentAuthorityCandidateRecord Invalid = Valid;
	Invalid.Reviews[0].ReviewId.Reset();
	TestInvalidCatalog(Test, TEXT("missing review id"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.Reviews[0].ReviewerId.Reset();
	TestInvalidCatalog(Test, TEXT("missing reviewer id"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.Reviews[0].ReviewKind = static_cast<EAssetDocumentAuthorityReviewKind>(2);
	TestInvalidCatalog(Test, TEXT("unknown review kind"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.Reviews[0].Verdict = static_cast<EAssetDocumentIndependentVerdict>(3);
	TestInvalidCatalog(Test, TEXT("unknown review verdict"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.Reviews[0].ReviewedState = static_cast<EAssetDocumentAuthorityState>(11);
	TestInvalidCatalog(Test, TEXT("unknown reviewed state"), MakeArrayView(&Invalid, 1));
	Invalid = Valid;
	Invalid.State = static_cast<EAssetDocumentAuthorityState>(11);
	TestInvalidCatalog(Test, TEXT("unknown candidate state"), MakeArrayView(&Invalid, 1));
}

void TestCatalogDuplicateRejections(FAutomationTestBase& Test)
{
	const FAssetDocumentAuthorityCandidateRecord Valid = MakeSyntheticApprovedCandidate();
	FAssetDocumentAuthorityCandidateRecord DuplicateGovernance = Valid;
	DuplicateGovernance.Reviews[0].ReviewId = TEXT("synthetic-duplicate-governance-review");
	TArray<FAssetDocumentAuthorityCandidateRecord> Catalog = {Valid, DuplicateGovernance};
	TestInvalidCatalog(Test, TEXT("duplicate governance id"), Catalog);
	FAssetDocumentAuthorityCandidateRecord Second = Valid;
	Second.GovernanceId = TEXT("UMG-SYNTHETIC-SECOND-v1");
	Catalog = {Valid, Second};
	TestInvalidCatalog(Test, TEXT("duplicate review id across candidates"), Catalog);
	Second.Reviews[0].ReviewId = TEXT("synthetic-second-candidate-review");
	Catalog = {Valid, Second};
	Test.TestTrue(TEXT("P: distinct governance and review identities validate"),
		ValidateAssetDocumentAuthorityExceptionCatalog(Catalog).bValid);
	FAssetDocumentAuthorityCandidateRecord OwnCollision = Valid;
	OwnCollision.Reviews[0].ReviewId = OwnCollision.GovernanceId;
	TestInvalidCatalog(Test, TEXT("candidate review id collides with its governance id"),
		MakeArrayView(&OwnCollision, 1));
	FAssetDocumentAuthorityCandidateRecord CrossCollision = Second;
	CrossCollision.Reviews[0].ReviewId = Valid.GovernanceId;
	Catalog = {Valid, CrossCollision};
	TestInvalidCatalog(Test, TEXT("review id collides with another candidate governance id"), Catalog);
}

void TestCatalogStateEvidenceRejections(FAutomationTestBase& Test)
{
	FAssetDocumentAuthorityCandidateRecord Invalid = MakeSyntheticApprovedCandidate();
	Invalid.Reviews[0].Verdict = EAssetDocumentIndependentVerdict::Rework;
	TestInvalidCatalog(Test, TEXT("approved state without candidate Pass"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticApprovedCandidate();
	Invalid.Reviews[0].ReviewedState = EAssetDocumentAuthorityState::CandidatePendingReview;
	TestInvalidCatalog(Test, TEXT("approved state without approved-state review"), MakeArrayView(&Invalid, 1));
	Invalid = MakeSyntheticNeedsEvidenceCandidate();
	Test.TestTrue(TEXT("P: NeedsMoreEvidence with resubmission condition validates"),
		ValidateAssetDocumentAuthorityExceptionCatalog(MakeArrayView(&Invalid, 1)).bValid);
	Invalid.ResubmissionCondition.Reset();
	TestInvalidCatalog(Test, TEXT("NeedsMoreEvidence without resubmission condition"), MakeArrayView(&Invalid, 1));
}

void TestFrozenReworkHistory(FAutomationTestBase& Test)
{
	const TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog = GetG1FrozenAuthorityExceptionRecords();
	static const TCHAR* ReworkedIds[] = {
		TEXT("UMG-DESIGNER-BINDING-APPLY-DELTA-v1"),
		TEXT("UMG-WIDGET-ANIMATION-DELETE-v1"),
	};
	for (const TCHAR* GovernanceId : ReworkedIds)
	{
		const FAssetDocumentAuthorityCandidateRecord* Candidate =
			FindAssetDocumentAuthorityCandidateRecord(Catalog, GovernanceId);
		Test.TestNotNull(TEXT("P: twice-reworked candidate exists"), Candidate);
		if (Candidate == nullptr)
		{
			continue;
		}
		Test.TestEqual(TEXT("P: twice-reworked candidate retains both reviews"), Candidate->Reviews.Num(), 2);
		const FString InitialId = FString::Printf(TEXT("candidate-review.initial/%s"), GovernanceId);
		const FAssetDocumentAuthorityReviewRecord* Initial =
			FindAssetDocumentAuthorityReviewRecord(*Candidate, InitialId);
		Test.TestNotNull(TEXT("P: initial REWORK evidence is retained"), Initial);
		if (Initial != nullptr)
		{
			Test.TestEqual(TEXT("P: initial verdict remains REWORK"), Initial->Verdict, EAssetDocumentIndependentVerdict::Rework);
			Test.TestEqual(TEXT("P: initial evidence hash is exact"), Initial->EvidenceHash,
				FString(TEXT("e06474f8da5605da77cbad596ba6ea1c0014d79ede162b68b0c96f546f64009a")));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAuthorityExceptionRegistryContractTest,
	"UEAssetDoc.RuntimeContracts.AuthorityExceptionRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAuthorityExceptionRegistryContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestDefaults(*this);
	TestTransitionGraph(*this);
	TestFrozenCatalog(*this);
	TestFrozenReworkHistory(*this);
	TestExactDottedGovernanceIdentityExceptions(*this);
	TestCatalogIdentityRejections(*this);
	TestCatalogIndependenceIdentityRejections(*this);
	TestCatalogReviewFieldRejections(*this);
	TestCatalogDuplicateRejections(*this);
	TestCatalogStateEvidenceRejections(*this);
	TestSyntheticBridgePositive(*this);
	TestSyntheticBridgeReviewShape(*this);
	TestSyntheticBridgeEvidenceAndIndependence(*this);
	return true;
}
} // namespace AssetDocumentAuthorityExceptionRegistryContractTests

#endif // WITH_DEV_AUTOMATION_TESTS
