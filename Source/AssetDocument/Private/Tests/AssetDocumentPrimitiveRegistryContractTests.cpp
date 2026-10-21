// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Runtime/AssetDocumentPrimitiveRegistry.h"

#include "Misc/AutomationTest.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentPrimitiveRegistryContractTests
{
#define ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(Type, Member, Expected) \
	static_assert(std::is_same_v<decltype(Type::Member), Expected>)
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveRequirement, PrimitiveId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveRequirement, Version, uint32);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveRequirement, RequiredByRouteId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveRegistrySnapshot, RuntimeRevision, uint64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveRegistrySnapshot, bFrozen, bool);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveRegistrySnapshot, Descriptors, TArray<FAssetDocumentPrimitiveDescriptor>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveRegistrySnapshot, Providers, TArray<FAssetDocumentProviderAvailability>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveResolution, bResolved, bool);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveResolution, Descriptor, FAssetDocumentPrimitiveDescriptor);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveResolution, ProviderId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveResolution, Diagnostics, TArray<FAssetDocumentRuntimeDiagnostic>);
#undef ASSET_DOCUMENT_ASSERT_MEMBER_TYPE

using FValidateFreezeInputSignature = FAssetDocumentContractValidationResult (*)(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor>,
	TConstArrayView<FAssetDocumentPrimitiveRequirement>,
	TConstArrayView<FAssetDocumentProviderAvailability>,
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord>);
using FSnapshotSignature = FAssetDocumentPrimitiveRegistrySnapshot (IAssetDocumentPrimitiveRegistry::*)() const;
using FResolveSignature = FAssetDocumentPrimitiveResolution (IAssetDocumentPrimitiveRegistry::*)(
	const FAssetDocumentPrimitiveRequirement&) const;
static_assert(std::is_same_v<decltype(&ValidateAssetDocumentPrimitiveFreezeInput), FValidateFreezeInputSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentPrimitiveRegistry::Snapshot), FSnapshotSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentPrimitiveRegistry::Resolve), FResolveSignature>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentPrimitiveRegistry>);

struct FFreezeFixture
{
	TArray<FAssetDocumentPrimitiveDescriptor> Descriptors;
	TArray<FAssetDocumentPrimitiveRequirement> Required;
	TArray<FAssetDocumentProviderAvailability> Providers;
	TArray<FAssetDocumentAuthorityCandidateRecord> AuthorityCatalog;
};

const TCHAR* GetCanonicalProviderId(EAssetDocumentPrimitiveOwner Owner)
{
	static const TCHAR* ProviderIds[] = {
		TEXT("core"), TEXT("editor"), TEXT("ai"),
		TEXT("animation"), TEXT("blueprint"), TEXT("umg"),
		TEXT("graph"), TEXT("data"), TEXT("animation_asset"),
		TEXT("fx"), TEXT("audio"), TEXT("rig"),
		TEXT("seq"),
		TEXT("physics"),
	};
	const uint8 OwnerIndex = static_cast<uint8>(Owner);
	return OwnerIndex < UE_ARRAY_COUNT(ProviderIds) ? ProviderIds[OwnerIndex] : TEXT("");
}

FAssetDocumentPrimitiveDescriptor MakeOrdinaryDescriptor(
	EAssetDocumentPrimitiveOwner Owner = EAssetDocumentPrimitiveOwner::Editor,
	const TCHAR* Suffix = TEXT("asset.apply_delta"),
	const TCHAR* ConsumerId = TEXT("route.apply"))
{
	const FString ProviderId = GetCanonicalProviderId(Owner);
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = FString::Printf(TEXT("%s.%s"), *ProviderId, Suffix);
	Descriptor.Version = 1;
	Descriptor.ProviderId = ProviderId;
	Descriptor.Owner = Owner;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::Mutating;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, true});
	Descriptor.bRequiresActiveTransaction = true;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.MutationFootprintSchemaId = Descriptor.PrimitiveId + TEXT(".footprint.v1");
	Descriptor.StableDiagnosticCodes = {TEXT("AD.RUNTIME.PRIMITIVE_FAILED")};
	Descriptor.RequiredConsumerIds = {ConsumerId};
	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.3");
	Evidence.PhysicalOwner = Owner;
	Evidence.PublicModule = TEXT("UnrealEd");
	Evidence.PublicHeader = TEXT("Editor.h");
	Evidence.ExportedAuthoritativeApi = Descriptor.PrimitiveId + TEXT(".api");
	Evidence.AuthorityContextId = Descriptor.PrimitiveId + TEXT(".context.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId + TEXT(".operation.v1");
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.live-required.v1");
	Evidence.RollbackContractId = TEXT("rollback.snapshot.v1");
	Evidence.CompletionContractId = TEXT("completion.post-edit.v1");
	Evidence.SaveReloadEvidenceId = TEXT("save-reload.fresh-load.v1");
	Evidence.FailureInjectionTestIds = {Descriptor.PrimitiveId + TEXT(".failure.v1")};
	Evidence.VerificationTestIds = {Descriptor.PrimitiveId + TEXT(".verify.v1")};
	Evidence.AuthorityFingerprint = Descriptor.PrimitiveId + TEXT(".fingerprint.v1");
	return Descriptor;
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

FAssetDocumentPrimitiveDescriptor MakeSyntheticBridgeDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		EAssetDocumentPrimitiveOwner::UMG, TEXT("synthetic_bridge.invoke"), TEXT("route.synthetic_bridge"));
	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::AdmittedBridge;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted;
	Evidence.GovernanceId = TEXT("UMG-SYNTHETIC-BRIDGE-v1");
	Evidence.IndependentCandidateReviewId = TEXT("synthetic-candidate-review");
	Evidence.IndependentBridgeReviewId = TEXT("synthetic-bridge-review");
	Evidence.SourceMarker = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(UMG-SYNTHETIC-BRIDGE-v1)");
	Evidence.RawLifecycleAllowlistId = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(UMG-SYNTHETIC-BRIDGE-v1|umg.synthetic_bridge.invoke.operation.v1)");
	Evidence.HelperReplacementCondition = TEXT("Replace if Engine exports this synthetic fixture helper.");
	Evidence.RemovalCondition = TEXT("Remove after helper replacement verification.");
	Evidence.ExportedAuthoritativeApi = TEXT("AssetDocumentUMG::SyntheticBridgeFixture");
	return Descriptor;
}

FFreezeFixture MakeFrozenBridgeFixture(const FAssetDocumentAuthorityCandidateRecord& Candidate, const TCHAR* Suffix)
{
	const FString ConsumerId = FString::Printf(TEXT("route.%s"), Suffix);
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		EAssetDocumentPrimitiveOwner::AI, Suffix, *ConsumerId);
	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::AdmittedBridge;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted;
	Evidence.GovernanceId = Candidate.GovernanceId;
	Evidence.IndependentCandidateReviewId = FString::Printf(
		TEXT("candidate-review.initial/%s"), *Candidate.GovernanceId);
	Evidence.IndependentBridgeReviewId = FString::Printf(
		TEXT("bridge-review.initial/%s"), *Candidate.GovernanceId);
	Evidence.SourceMarker = FString::Printf(
		TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(%s)"), *Candidate.GovernanceId);
	Evidence.RawLifecycleAllowlistId = FString::Printf(
		TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(%s|%s)"),
		*Candidate.GovernanceId, *Evidence.PrimaryOperationId);
	Evidence.HelperReplacementCondition = Candidate.HelperReplacementCondition;
	Evidence.RemovalCondition = TEXT("Remove the structural quarantine dependency after the stock lifecycle replacement is verified.");
	Evidence.ExportedAuthoritativeApi = Candidate.ImplementationSymbol;

	FFreezeFixture Fixture;
	Fixture.Descriptors.Add(Descriptor);
	Fixture.Required.Add({Descriptor.PrimitiveId, Descriptor.Version, ConsumerId});
	Fixture.Providers.Add({TEXT("ai"), EAssetDocumentProviderAvailabilityState::Available, {}});
	const TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Frozen = GetG1FrozenAuthorityExceptionRecords();
	Fixture.AuthorityCatalog.Append(Frozen.GetData(), Frozen.Num());
	return Fixture;
}

FFreezeFixture MakeFreezeFixture()
{
	FFreezeFixture Fixture;
	Fixture.Descriptors.Add(MakeOrdinaryDescriptor());
	Fixture.Required.Add({Fixture.Descriptors[0].PrimitiveId, 1, TEXT("route.apply")});
	Fixture.Providers.Add({TEXT("editor"), EAssetDocumentProviderAvailabilityState::Available, {}});
	const TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Frozen = GetG1FrozenAuthorityExceptionRecords();
	Fixture.AuthorityCatalog.Append(Frozen.GetData(), Frozen.Num());
	return Fixture;
}

FAssetDocumentContractValidationResult ValidateFixture(const FFreezeFixture& Fixture)
{
	return ValidateAssetDocumentPrimitiveFreezeInput(
		Fixture.Descriptors, Fixture.Required, Fixture.Providers, Fixture.AuthorityCatalog);
}

void TestInvalidFixture(
	FAutomationTestBase& Test,
	const TCHAR* What,
	const FFreezeFixture& Fixture,
	const TCHAR* ExpectedCode)
{
	const FAssetDocumentContractValidationResult Result = ValidateFixture(Fixture);
	Test.TestFalse(*FString::Printf(TEXT("P: %s is rejected"), What), Result.bValid);
	Test.TestEqual(*FString::Printf(TEXT("P: %s returns one diagnostic"), What), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() == 1)
	{
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic is stable"), What), Result.Diagnostics[0].Code, FString(ExpectedCode));
	}
}

void TestDefaults(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveRequirement Requirement;
	Test.TestTrue(TEXT("S: requirement primitive id defaults empty"), Requirement.PrimitiveId.IsEmpty());
	Test.TestEqual(TEXT("S: requirement version defaults zero"), Requirement.Version, uint32{0});
	Test.TestTrue(TEXT("S: requirement route defaults empty"), Requirement.RequiredByRouteId.IsEmpty());
	const FAssetDocumentPrimitiveRegistrySnapshot Snapshot;
	Test.TestEqual(TEXT("S: snapshot revision defaults zero"), Snapshot.RuntimeRevision, uint64{0});
	Test.TestFalse(TEXT("S: snapshot defaults unfrozen"), Snapshot.bFrozen);
	Test.TestTrue(TEXT("S: snapshot descriptors default empty"), Snapshot.Descriptors.IsEmpty());
	Test.TestTrue(TEXT("S: snapshot providers default empty"), Snapshot.Providers.IsEmpty());
	const FAssetDocumentPrimitiveResolution Resolution;
	Test.TestFalse(TEXT("S: resolution defaults unresolved"), Resolution.bResolved);
	Test.TestTrue(TEXT("S: resolution provider defaults empty"), Resolution.ProviderId.IsEmpty());
	Test.TestTrue(TEXT("S: resolution diagnostics default empty"), Resolution.Diagnostics.IsEmpty());
}

void TestEmptyFreezeInputFamilies(FAutomationTestBase& Test)
{
	FFreezeFixture Invalid = MakeFreezeFixture();
	Invalid.Descriptors.Reset();
	TestInvalidFixture(Test, TEXT("empty descriptor family"), Invalid,
		TEXT("PrimitiveRegistry.EmptyConsumerClosure"));

	Invalid = MakeFreezeFixture();
	Invalid.Required.Reset();
	TestInvalidFixture(Test, TEXT("empty requirement family"), Invalid,
		TEXT("PrimitiveRegistry.EmptyConsumerClosure"));

	Invalid = MakeFreezeFixture();
	Invalid.Providers.Reset();
	TestInvalidFixture(Test, TEXT("empty provider family"), Invalid,
		TEXT("PrimitiveRegistry.EmptyConsumerClosure"));

	FFreezeFixture WithoutExceptions = MakeFreezeFixture();
	WithoutExceptions.AuthorityCatalog.Reset();
	Test.TestTrue(TEXT("P: empty authority exception catalog remains valid for ordinary primitives"),
		ValidateFixture(WithoutExceptions).bValid);
}

FAssetDocumentPrimitiveDescriptor MakeStableBlockerDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = TEXT("umg.designer_binding.apply_delta");
	Descriptor.Version = 1;
	Descriptor.ProviderId = TEXT("umg");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::UMG;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply};
	Descriptor.StableDiagnosticCodes = {TEXT("AD.G1.BLOCK.UMG.DESIGNER_BINDING_EVIDENCE")};
	Descriptor.Admission.Disposition = EAssetDocumentPrimitiveDisposition::StableBlocker;
	Descriptor.Admission.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::StableBlocker;
	Descriptor.Admission.PhysicalOwner = EAssetDocumentPrimitiveOwner::UMG;
	Descriptor.Admission.PrimaryOperationId = TEXT("umg.designer_binding.apply_delta.v1");
	Descriptor.Admission.AuthorityFingerprint = TEXT("fingerprint.umg.designer_binding.apply_delta.v1");
	return Descriptor;
}

FFreezeFixture MakeSingleDescriptorFixture(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	FFreezeFixture Fixture;
	Fixture.Descriptors.Add(Descriptor);
	for (const FString& ConsumerId : Descriptor.RequiredConsumerIds)
	{
		Fixture.Required.Add({Descriptor.PrimitiveId, Descriptor.Version, ConsumerId});
	}
	Fixture.Providers.Add({Descriptor.ProviderId, EAssetDocumentProviderAvailabilityState::Available, {}});
	const TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Frozen = GetG1FrozenAuthorityExceptionRecords();
	Fixture.AuthorityCatalog.Append(Frozen.GetData(), Frozen.Num());
	return Fixture;
}

void TestPositiveOrdinaryAndOwnerPrefixes(FAutomationTestBase& Test)
{
	for (uint8 OwnerIndex = 0; OwnerIndex <= static_cast<uint8>(EAssetDocumentPrimitiveOwner::Physics); ++OwnerIndex)
	{
		const EAssetDocumentPrimitiveOwner Owner = static_cast<EAssetDocumentPrimitiveOwner>(OwnerIndex);
		if (FCString::Strlen(GetCanonicalProviderId(Owner)) == 0)
		{
			continue; // a reserved owner value (none at present)
		}
		const FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
			Owner, TEXT("synthetic.operation"), TEXT("route.synthetic"));
		const FFreezeFixture Fixture = MakeSingleDescriptorFixture(Descriptor);
		const FAssetDocumentContractValidationResult Result = ValidateFixture(Fixture);
		Test.TestTrue(
			*FString::Printf(TEXT("P: owner %u canonical provider and primitive prefix validate%s"), OwnerIndex,
				Result.Diagnostics.IsEmpty() ? TEXT("") : *(TEXT(" (") + Result.Diagnostics[0].Code + TEXT(")"))),
			Result.bValid);
	}
	Test.TestTrue(TEXT("P: ordinary fixture validates against immutable six-row governance catalog"),
		ValidateFixture(MakeFreezeFixture()).bValid);
}

void TestDescriptorValidationAndUniqueness(FAutomationTestBase& Test)
{
	FFreezeFixture Invalid = MakeFreezeFixture();
	FAssetDocumentPrimitiveDescriptor Duplicate = Invalid.Descriptors[0];
	Duplicate.Admission.PrimaryOperationId = TEXT("editor.asset.other_operation.v1");
	Duplicate.InputSchemaId = TEXT("editor.asset.other_operation.input.v1");
	Duplicate.OutputSchemaId = TEXT("editor.asset.other_operation.output.v1");
	Invalid.Descriptors.Add(Duplicate);
	TestInvalidFixture(Test, TEXT("duplicate primitive id/version"), Invalid, TEXT("PrimitiveDescriptor.DuplicateIdentity"));

	Invalid = MakeFreezeFixture();
	Duplicate = Invalid.Descriptors[0];
	Duplicate.PrimitiveId = TEXT("editor.asset.apply_delta_alias");
	Duplicate.Admission.AuthorityFingerprint = TEXT("caller-provided-alias-fingerprint");
	Invalid.Descriptors.Add(Duplicate);
	TestInvalidFixture(Test, TEXT("alias with duplicate semantic authority"), Invalid, TEXT("PrimitiveDescriptor.DuplicateAuthority"));

	Invalid = MakeFreezeFixture();
	Invalid.Descriptors[0].Admission.SaveReloadEvidenceId.Reset();
	TestInvalidFixture(Test, TEXT("incomplete T02 admission"), Invalid, TEXT("PrimitiveDescriptor.InvalidAdmission"));
}

void TestOwnerProviderAndRuntimeLeakage(FAutomationTestBase& Test)
{
	FFreezeFixture Invalid = MakeFreezeFixture();
	Invalid.Descriptors[0].Owner = EAssetDocumentPrimitiveOwner::UMG;
	Invalid.Descriptors[0].Admission.PhysicalOwner = EAssetDocumentPrimitiveOwner::UMG;
	TestInvalidFixture(Test, TEXT("owner/provider mismatch"), Invalid, TEXT("PrimitiveRegistry.OwnerProviderMismatch"));

	Invalid = MakeFreezeFixture();
	Invalid.Descriptors[0].PrimitiveId = TEXT("umg.asset.apply_delta");
	TestInvalidFixture(Test, TEXT("primitive id outside provider prefix"), Invalid, TEXT("PrimitiveRegistry.OwnerProviderMismatch"));

	Invalid = MakeFreezeFixture();
	Invalid.Descriptors[0].Admission.Disposition = EAssetDocumentPrimitiveDisposition::GovernanceCandidate;
	TestInvalidFixture(Test, TEXT("governance candidate leaked into runtime"), Invalid, TEXT("PrimitiveDescriptor.InvalidPath"));

	Invalid = MakeSingleDescriptorFixture(MakeStableBlockerDescriptor());
	TestInvalidFixture(Test, TEXT("stable blocker leaked into frozen callable registry"), Invalid,
		TEXT("PrimitiveRegistry.RuntimeDispositionLeakage"));
}

void TestRequiredResolutionAndConsumerClosure(FAutomationTestBase& Test)
{
	FFreezeFixture Invalid = MakeFreezeFixture();
	Invalid.Required[0].PrimitiveId = TEXT("editor.asset.missing");
	TestInvalidFixture(Test, TEXT("missing required primitive"), Invalid, TEXT("PrimitiveRegistry.MissingRequiredPrimitive"));

	Invalid = MakeFreezeFixture();
	Invalid.Required[0].Version = 2;
	TestInvalidFixture(Test, TEXT("required primitive version mismatch"), Invalid, TEXT("PrimitiveRegistry.MissingRequiredPrimitive"));

	Invalid = MakeFreezeFixture();
	Invalid.Descriptors.Add(MakeOrdinaryDescriptor(
		EAssetDocumentPrimitiveOwner::Editor, TEXT("asset.orphan_operation"), TEXT("route.orphan")));
	TestInvalidFixture(Test, TEXT("orphan descriptor"), Invalid, TEXT("PrimitiveRegistry.OrphanDescriptor"));

	Invalid = MakeFreezeFixture();
	Invalid.Required[0].RequiredByRouteId.Reset();
	TestInvalidFixture(Test, TEXT("empty required route"), Invalid, TEXT("PrimitiveRegistry.InvalidRequirement"));

	Invalid = MakeFreezeFixture();
	Invalid.Required[0].RequiredByRouteId = TEXT("route.other");
	TestInvalidFixture(Test, TEXT("requirement outside descriptor consumer closure"), Invalid,
		TEXT("PrimitiveRegistry.ConsumerClosureMismatch"));

	Invalid = MakeFreezeFixture();
	Invalid.Descriptors[0].RequiredConsumerIds.Add(TEXT("route.unfrozen"));
	TestInvalidFixture(Test, TEXT("descriptor consumer omitted from frozen requirements"), Invalid,
		TEXT("PrimitiveRegistry.ConsumerClosureMismatch"));

	Invalid = MakeFreezeFixture();
	const FAssetDocumentPrimitiveRequirement DuplicateRequirement = Invalid.Required[0];
	Invalid.Required.Add(DuplicateRequirement);
	TestInvalidFixture(Test, TEXT("duplicate exact requirement"), Invalid, TEXT("PrimitiveRegistry.InvalidRequirement"));
}

void TestProviderAvailabilityClosure(FAutomationTestBase& Test)
{
	FFreezeFixture Invalid = MakeFreezeFixture();
	Invalid.Providers[0].State = EAssetDocumentProviderAvailabilityState::Unavailable;
	TestInvalidFixture(Test, TEXT("unavailable required provider"), Invalid, TEXT("PrimitiveRegistry.ProviderUnavailable"));

	Invalid = MakeFreezeFixture();
	const FAssetDocumentProviderAvailability DuplicateProvider = Invalid.Providers[0];
	Invalid.Providers.Add(DuplicateProvider);
	TestInvalidFixture(Test, TEXT("more than one available provider record"), Invalid,
		TEXT("PrimitiveRegistry.ProviderCardinality"));

	Invalid = MakeFreezeFixture();
	Invalid.Providers.Reset();
	TestInvalidFixture(Test, TEXT("missing provider availability"), Invalid,
		TEXT("PrimitiveRegistry.EmptyConsumerClosure"));

	Invalid = MakeFreezeFixture();
	Invalid.Providers.Add({TEXT("umg"), EAssetDocumentProviderAvailabilityState::Available, {}});
	TestInvalidFixture(Test, TEXT("orphan provider availability"), Invalid, TEXT("PrimitiveRegistry.ProviderCardinality"));
}

FFreezeFixture MakeSyntheticBridgeFixture()
{
	FFreezeFixture Fixture = MakeSingleDescriptorFixture(MakeSyntheticBridgeDescriptor());
	Fixture.AuthorityCatalog = {MakeSyntheticBridgeCandidate()};
	return Fixture;
}

void RebindDescriptorOwner(
	FFreezeFixture& Fixture,
	EAssetDocumentPrimitiveOwner Owner)
{
	FAssetDocumentPrimitiveDescriptor& Descriptor = Fixture.Descriptors[0];
	const FString ProviderId = GetCanonicalProviderId(Owner);
	Descriptor.Owner = Owner;
	Descriptor.Admission.PhysicalOwner = Owner;
	Descriptor.ProviderId = ProviderId;
	Descriptor.PrimitiveId = ProviderId + TEXT(".synthetic_bridge.invoke");
	Fixture.Required[0].PrimitiveId = Descriptor.PrimitiveId;
	Fixture.Providers[0].ProviderId = ProviderId;
}

void RebindBridgeGovernance(
	FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FString& GovernanceId)
{
	Descriptor.Admission.GovernanceId = GovernanceId;
	Descriptor.Admission.SourceMarker = FString::Printf(
		TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(%s)"), *GovernanceId);
	Descriptor.Admission.RawLifecycleAllowlistId = FString::Printf(
		TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(%s|%s)"),
		*GovernanceId, *Descriptor.Admission.PrimaryOperationId);
}

FAssetDocumentAuthorityReviewRecord MakeExtraReview(
	const TCHAR* ReviewId,
	EAssetDocumentAuthorityReviewKind Kind,
	EAssetDocumentIndependentVerdict Verdict,
	EAssetDocumentAuthorityState ReviewedState)
{
	const FString ReviewerId = FString::Printf(TEXT("reviewer.%s"), ReviewId);
	return MakeReview(ReviewId, Kind, *ReviewerId, Verdict, ReviewedState);
}

void TestAuthorityCatalogDelegation(FAutomationTestBase& Test)
{
	FFreezeFixture Invalid = MakeFreezeFixture();
	const FAssetDocumentAuthorityCandidateRecord DuplicateCandidate = Invalid.AuthorityCatalog[0];
	Invalid.AuthorityCatalog.Add(DuplicateCandidate);
	TestInvalidFixture(Test, TEXT("T04-invalid duplicate governance catalog"), Invalid,
		TEXT("AuthorityCatalog.DuplicateId"));

	Invalid = MakeFreezeFixture();
	Invalid.Required[0].RequiredByRouteId = Invalid.AuthorityCatalog[0].GovernanceId;
	Invalid.Descriptors[0].RequiredConsumerIds[0] = Invalid.Required[0].RequiredByRouteId;
	TestInvalidFixture(Test, TEXT("governance id leaked into runtime consumer namespace"), Invalid,
		TEXT("PrimitiveRegistry.GovernanceNamespaceLeakage"));
}

void TestBridgeResolutionAndCandidateOnlyEvidence(FAutomationTestBase& Test)
{
	const TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Frozen = GetG1FrozenAuthorityExceptionRecords();
	Test.TestEqual(TEXT("P: immutable governance catalog has fifteen rows after BT admission"), Frozen.Num(), 15);
	int32 BridgeCount = 0;
	for (const FAssetDocumentAuthorityCandidateRecord& Candidate : Frozen)
	{
		BridgeCount += Candidate.State == EAssetDocumentAuthorityState::BridgeAdmitted;
	}
	Test.TestEqual(TEXT("P: immutable governance catalog admits exactly nine real bridges"), BridgeCount, 9);
	Test.TestTrue(TEXT("P: isolated synthetic T04 bridge fixture validates registry freeze input"),
		ValidateFixture(MakeSyntheticBridgeFixture()).bValid);

	FFreezeFixture Invalid = MakeSyntheticBridgeFixture();
	RebindBridgeGovernance(Invalid.Descriptors[0], TEXT("UMG-MISSING-BRIDGE-v1"));
	TestInvalidFixture(Test, TEXT("unresolved governance id"), Invalid,
		TEXT("PrimitiveRegistry.InvalidBridgeGovernance"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.Descriptors[0].Admission.IndependentCandidateReviewId = TEXT("missing-candidate-review");
	TestInvalidFixture(Test, TEXT("unresolved candidate review id"), Invalid,
		TEXT("PrimitiveRegistry.InvalidBridgeReview"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.Descriptors[0].Admission.IndependentBridgeReviewId = TEXT("missing-bridge-review");
	TestInvalidFixture(Test, TEXT("unresolved bridge review id"), Invalid,
		TEXT("PrimitiveRegistry.InvalidBridgeReview"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.AuthorityCatalog[0].State = EAssetDocumentAuthorityState::CandidateApproved;
	Invalid.AuthorityCatalog[0].Reviews.SetNum(1);
	Invalid.Descriptors[0].Admission.IndependentBridgeReviewId = TEXT("missing-bridge-review");
	TestInvalidFixture(Test, TEXT("candidate-only evidence cannot admit a bridge"), Invalid,
		TEXT("PrimitiveRegistry.InvalidBridgeGovernance"));
}

void TestFrozenBridgeOwnerSymbolAndReviewBindings(FAutomationTestBase& Test)
{
	struct FBindingExpectation
	{
		const TCHAR* GovernanceId;
		const TCHAR* ImplementationSymbol;
		const TCHAR* Suffix;
	};
	static const FBindingExpectation Expectations[] = {
		{
			TEXT("AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1"),
			TEXT("AssetDocumentAIBlackboardStructuralPublication::StageKeyType"),
			TEXT("blackboard.structural_staging_catalog_probe"),
		},
		{
			TEXT("AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1"),
			TEXT("AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot"),
			TEXT("blackboard.structural_publication_catalog_probe"),
		},
	};
	const TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Frozen = GetG1FrozenAuthorityExceptionRecords();
	for (const FBindingExpectation& Expected : Expectations)
	{
		const FAssetDocumentAuthorityCandidateRecord* Candidate =
			FindAssetDocumentAuthorityCandidateRecord(Frozen, Expected.GovernanceId);
		Test.TestNotNull(TEXT("P: frozen structural bridge resolves by exact governance id"), Candidate);
		if (Candidate == nullptr)
		{
			continue;
		}
		Test.TestEqual(TEXT("P: frozen structural bridge physical owner is AI"), Candidate->PhysicalOwner, EAssetDocumentPrimitiveOwner::AI);
		Test.TestEqual(TEXT("P: frozen structural bridge implementation symbol is exact"),
			Candidate->ImplementationSymbol, FString(Expected.ImplementationSymbol));
		Test.TestTrue(TEXT("P: real frozen owner/symbol/review binding validates through primitive freeze input"),
			ValidateFixture(MakeFrozenBridgeFixture(*Candidate, Expected.Suffix)).bValid);
	}
}

void TestBridgeAuthorityBinding(FAutomationTestBase& Test)
{
	FFreezeFixture Invalid = MakeSyntheticBridgeFixture();
	RebindDescriptorOwner(Invalid, EAssetDocumentPrimitiveOwner::Blueprint);
	TestInvalidFixture(Test, TEXT("bridge borrows governance from another physical owner"), Invalid,
		TEXT("PrimitiveRegistry.BridgeAuthorityBindingMismatch"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.Descriptors[0].Admission.ExportedAuthoritativeApi =
		TEXT("AssetDocumentUMG::DifferentBridgeSymbol");
	TestInvalidFixture(Test, TEXT("bridge borrows same-owner governance for another symbol"), Invalid,
		TEXT("PrimitiveRegistry.BridgeAuthorityBindingMismatch"));

	Invalid = MakeSyntheticBridgeFixture();
	RebindDescriptorOwner(Invalid, EAssetDocumentPrimitiveOwner::Blueprint);
	Invalid.Descriptors[0].Admission.IndependentCandidateReviewId = TEXT("missing-candidate-review");
	TestInvalidFixture(Test, TEXT("owner binding mismatch precedes invalid review reference"), Invalid,
		TEXT("PrimitiveRegistry.BridgeAuthorityBindingMismatch"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.Descriptors[0].Admission.ExportedAuthoritativeApi =
		TEXT("AssetDocumentUMG::DifferentBridgeSymbol");
	Invalid.Descriptors[0].Admission.IndependentBridgeReviewId = TEXT("missing-bridge-review");
	TestInvalidFixture(Test, TEXT("symbol binding mismatch precedes invalid review reference"), Invalid,
		TEXT("PrimitiveRegistry.BridgeAuthorityBindingMismatch"));
}

void TestBridgeExactReviewShape(FAutomationTestBase& Test)
{
	FFreezeFixture Invalid = MakeSyntheticBridgeFixture();
	Invalid.Descriptors[0].Admission.IndependentCandidateReviewId = TEXT("synthetic-bridge-review");
	TestInvalidFixture(Test, TEXT("candidate review id resolves to bridge kind"), Invalid,
		TEXT("PrimitiveRegistry.InvalidBridgeReview"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.Descriptors[0].Admission.IndependentBridgeReviewId = TEXT("synthetic-candidate-review");
	TestInvalidFixture(Test, TEXT("bridge review id resolves to candidate kind"), Invalid,
		TEXT("PrimitiveRegistry.InvalidBridgeReview"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.AuthorityCatalog[0].Reviews.Add(MakeExtraReview(
		TEXT("candidate-wrong-state"), EAssetDocumentAuthorityReviewKind::Candidate,
		EAssetDocumentIndependentVerdict::Pass, EAssetDocumentAuthorityState::CandidatePendingReview));
	Invalid.Descriptors[0].Admission.IndependentCandidateReviewId = TEXT("candidate-wrong-state");
	TestInvalidFixture(Test, TEXT("candidate review has wrong reviewed state"), Invalid,
		TEXT("PrimitiveRegistry.InvalidBridgeReview"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.AuthorityCatalog[0].Reviews.Add(MakeExtraReview(
		TEXT("bridge-wrong-state"), EAssetDocumentAuthorityReviewKind::Bridge,
		EAssetDocumentIndependentVerdict::Pass, EAssetDocumentAuthorityState::BridgePendingReview));
	Invalid.Descriptors[0].Admission.IndependentBridgeReviewId = TEXT("bridge-wrong-state");
	TestInvalidFixture(Test, TEXT("bridge review has wrong reviewed state"), Invalid,
		TEXT("PrimitiveRegistry.InvalidBridgeReview"));
}

void TestBridgeExactReviewVerdicts(FAutomationTestBase& Test)
{
	FFreezeFixture Invalid = MakeSyntheticBridgeFixture();
	Invalid.AuthorityCatalog[0].Reviews.Add(MakeExtraReview(
		TEXT("candidate-rework"), EAssetDocumentAuthorityReviewKind::Candidate,
		EAssetDocumentIndependentVerdict::Rework, EAssetDocumentAuthorityState::NeedsMoreEvidence));
	Invalid.Descriptors[0].Admission.IndependentCandidateReviewId = TEXT("candidate-rework");
	TestInvalidFixture(Test, TEXT("candidate review verdict is not Pass"), Invalid,
		TEXT("PrimitiveRegistry.InvalidBridgeReview"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.AuthorityCatalog[0].Reviews.Add(MakeExtraReview(
		TEXT("bridge-reject"), EAssetDocumentAuthorityReviewKind::Bridge,
		EAssetDocumentIndependentVerdict::Reject, EAssetDocumentAuthorityState::BridgeRejected));
	Invalid.Descriptors[0].Admission.IndependentBridgeReviewId = TEXT("bridge-reject");
	TestInvalidFixture(Test, TEXT("bridge review verdict is not Pass"), Invalid,
		TEXT("PrimitiveRegistry.InvalidBridgeReview"));
}

void TestBridgeEvidenceAndIndependence(FAutomationTestBase& Test)
{
	FFreezeFixture Invalid = MakeSyntheticBridgeFixture();
	Invalid.AuthorityCatalog[0].Reviews[0].EvidenceHash.Reset();
	TestInvalidFixture(Test, TEXT("candidate review evidence hash empty"), Invalid,
		TEXT("AuthorityCatalog.InvalidReview"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.AuthorityCatalog[0].Reviews[1].ReviewedHead.Reset();
	TestInvalidFixture(Test, TEXT("bridge review head empty"), Invalid,
		TEXT("AuthorityCatalog.InvalidReview"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.AuthorityCatalog[0].Reviews[0].ReviewerId = Invalid.AuthorityCatalog[0].ResearcherId;
	TestInvalidFixture(Test, TEXT("candidate reviewer equals researcher"), Invalid,
		TEXT("AuthorityCatalog.InvalidReview"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.AuthorityCatalog[0].Reviews[1].ReviewerId = Invalid.AuthorityCatalog[0].ImplementerId;
	TestInvalidFixture(Test, TEXT("bridge reviewer equals implementer"), Invalid,
		TEXT("AuthorityCatalog.InvalidReview"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.AuthorityCatalog[0].Reviews[1].ReviewerId = Invalid.AuthorityCatalog[0].ProviderOwnerId;
	TestInvalidFixture(Test, TEXT("bridge reviewer equals provider owner"), Invalid,
		TEXT("AuthorityCatalog.InvalidReview"));

	Invalid = MakeSyntheticBridgeFixture();
	Invalid.AuthorityCatalog[0].Reviews[1].ReviewerId = Invalid.AuthorityCatalog[0].Reviews[0].ReviewerId;
	TestInvalidFixture(Test, TEXT("candidate and bridge reviews reuse reviewer"), Invalid,
		TEXT("AuthorityCatalog.InvalidReview"));
}

struct FRegistrySuccessorVector
{
	const TCHAR* Scenario;
	const TCHAR* ExpectedCode;
	const TCHAR* Owner;
};

static const FRegistrySuccessorVector SuccessorVectors[] = {
	{TEXT("lookup.unknown_primitive"), TEXT("AD.RUNTIME.PRIMITIVE_UNAVAILABLE"), TEXT("F-HOST-ABI")},
	{TEXT("lookup.version_mismatch"), TEXT("AD.RUNTIME.PRIMITIVE_UNAVAILABLE"), TEXT("F-HOST-ABI")},
	{TEXT("lookup.provider_unavailable"), TEXT("AD.RUNTIME.UNSUPPORTED_PROVIDER"), TEXT("F-HOST-ABI")},
	{TEXT("freeze.validation_failed"), TEXT("AD.RUNTIME.PRIMITIVE_CATALOG_INVALID"), TEXT("F-HOST-ABI")},
	{TEXT("freeze.after_ingress_activation"), TEXT("AD.RUNTIME.PRIMITIVE_REGISTRY_ALREADY_ACTIVE"), TEXT("F-HOST-ABI")},
	{TEXT("revision.stale_lookup"), TEXT("AD.RUNTIME.REVISION_MISMATCH"), TEXT("F-HOST-ABI")},
	{TEXT("revision.failed_swap_keeps_last_known_good"), TEXT("AD.RUNTIME.REVISION_ACTIVATION_FAILED"), TEXT("F-HOST-ABI")},
	{TEXT("concurrency.concurrent_freeze"), TEXT("AD.RUNTIME.PRIMITIVE_REGISTRY_BUSY"), TEXT("F-HOST-ABI")},
	{TEXT("concurrency.lookup_pins_request_revision"), TEXT("AD.RUNTIME.REVISION_PIN_MISMATCH"), TEXT("F-HOST-ABI")},
};

void TestSuccessorOwnedVectors(FAutomationTestBase& Test)
{
	Test.TestEqual(TEXT("V: exact lookup/freeze/revision/concurrency vector count is frozen"),
		static_cast<int32>(UE_ARRAY_COUNT(SuccessorVectors)), int32{9});
	for (const FRegistrySuccessorVector& Vector : SuccessorVectors)
	{
		Test.TestFalse(TEXT("V: successor vector scenario is named"), FStringView(Vector.Scenario).IsEmpty());
		Test.TestTrue(TEXT("V: successor vector code is stable runtime diagnostic"),
			FStringView(Vector.ExpectedCode).StartsWith(TEXTVIEW("AD.RUNTIME.")));
		Test.TestEqual(TEXT("V: storage/freeze/revision/concurrency behavior owner is F-HOST-ABI"),
			FString(Vector.Owner), FString(TEXT("F-HOST-ABI")));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPrimitiveRegistryContractTest,
	"UEAssetDoc.RuntimeContracts.PrimitiveRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPrimitiveRegistryContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestDefaults(*this);
	TestEmptyFreezeInputFamilies(*this);
	TestPositiveOrdinaryAndOwnerPrefixes(*this);
	TestDescriptorValidationAndUniqueness(*this);
	TestOwnerProviderAndRuntimeLeakage(*this);
	TestRequiredResolutionAndConsumerClosure(*this);
	TestProviderAvailabilityClosure(*this);
	TestAuthorityCatalogDelegation(*this);
	TestBridgeResolutionAndCandidateOnlyEvidence(*this);
	TestFrozenBridgeOwnerSymbolAndReviewBindings(*this);
	TestBridgeAuthorityBinding(*this);
	TestBridgeExactReviewShape(*this);
	TestBridgeExactReviewVerdicts(*this);
	TestBridgeEvidenceAndIndependence(*this);
	TestSuccessorOwnedVectors(*this);
	return true;
}
} // namespace AssetDocumentPrimitiveRegistryContractTests

#endif // WITH_DEV_AUTOMATION_TESTS
