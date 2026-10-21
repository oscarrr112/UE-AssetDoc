// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentAuthorityExceptionRegistry.h"

struct ASSETDOCUMENT_API FAssetDocumentPrimitiveRequirement
{
	FString PrimitiveId;
	uint32 Version = 0;
	FString RequiredByRouteId;
};

struct ASSETDOCUMENT_API FAssetDocumentPrimitiveRegistrySnapshot
{
	uint64 RuntimeRevision = 0;
	bool bFrozen = false;
	TArray<FAssetDocumentPrimitiveDescriptor> Descriptors;
	TArray<FAssetDocumentProviderAvailability> Providers;
};

struct ASSETDOCUMENT_API FAssetDocumentPrimitiveResolution
{
	bool bResolved = false;
	FAssetDocumentPrimitiveDescriptor Descriptor;
	FString ProviderId;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

class ASSETDOCUMENT_API IAssetDocumentPrimitiveRegistry
{
public:
	virtual ~IAssetDocumentPrimitiveRegistry() = default;
	virtual FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const = 0;
	virtual FAssetDocumentPrimitiveResolution Resolve(
		const FAssetDocumentPrimitiveRequirement& Requirement) const = 0;
};

namespace UE::AssetDocument::PrimitiveRegistryContract::Private
{
struct FPrimitiveRegistryValidationContext
{
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors;
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Required;
	TConstArrayView<FAssetDocumentProviderAvailability> Providers;
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> AuthorityCatalog;
};

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
		Diagnostic.Path = TEXT("/PrimitiveRegistry");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

inline FAssetDocumentContractValidationResult MakeValidResult()
{
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidateNonEmptyConsumerClosure(
	const FPrimitiveRegistryValidationContext& Context)
{
	if (Context.Descriptors.IsEmpty() || Context.Required.IsEmpty() || Context.Providers.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("PrimitiveRegistry.EmptyConsumerClosure"),
			TEXT("Frozen descriptors, requirements, and provider families must all be non-empty."));
	}
	return MakeValidResult();
}

inline const TCHAR* GetCanonicalProviderId(EAssetDocumentPrimitiveOwner Owner)
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

inline bool HasCanonicalOwnerProvider(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString ProviderId = GetCanonicalProviderId(Descriptor.Owner);
	const FString PrimitivePrefix = ProviderId + TEXT(".");
	return !ProviderId.IsEmpty()
		&& Descriptor.ProviderId == ProviderId
		&& Descriptor.PrimitiveId.StartsWith(PrimitivePrefix);
}

inline FAssetDocumentContractValidationResult ValidateDescriptorContracts(
	const FPrimitiveRegistryValidationContext& Context)
{
	for (int32 Index = 0; Index < Context.Descriptors.Num(); ++Index)
	{
		const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Existing(
			Context.Descriptors.GetData(), Index);
		const FAssetDocumentContractValidationResult Result =
			ValidateAssetDocumentPrimitiveDescriptor(Context.Descriptors[Index], Existing);
		if (!Result.bValid)
		{
			return Result;
		}
	}
	return MakeValidResult();
}

inline FAssetDocumentContractValidationResult ValidateAuthorityCatalog(
	const FPrimitiveRegistryValidationContext& Context)
{
	return ValidateAssetDocumentAuthorityExceptionCatalog(Context.AuthorityCatalog);
}

inline FAssetDocumentContractValidationResult ValidateRuntimeDispositions(
	const FPrimitiveRegistryValidationContext& Context)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Context.Descriptors)
	{
		const EAssetDocumentPrimitiveDisposition Disposition = Descriptor.Admission.Disposition;
		if (Disposition == EAssetDocumentPrimitiveDisposition::StableBlocker
			|| Disposition == EAssetDocumentPrimitiveDisposition::GovernanceCandidate)
		{
			return MakeValidationResult(false, TEXT("PrimitiveRegistry.RuntimeDispositionLeakage"),
				TEXT("Stable blockers and governance candidates cannot enter the callable registry."));
		}
	}
	return MakeValidResult();
}

inline FAssetDocumentContractValidationResult ValidateOwnerProviderPrefixes(
	const FPrimitiveRegistryValidationContext& Context)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Context.Descriptors)
	{
		if (!HasCanonicalOwnerProvider(Descriptor))
		{
			return MakeValidationResult(false, TEXT("PrimitiveRegistry.OwnerProviderMismatch"),
				TEXT("Primitive owner, provider id, and primitive prefix must be canonical."));
		}
	}
	return MakeValidResult();
}

inline bool IsProviderReferenced(
	FStringView ProviderId,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		if (Descriptor.ProviderId == ProviderId)
		{
			return true;
		}
	}
	return false;
}

inline bool IsProviderRowValid(const FAssetDocumentProviderAvailability& Provider)
{
	return !Provider.ProviderId.IsEmpty()
		&& static_cast<uint8>(Provider.State)
			<= static_cast<uint8>(EAssetDocumentProviderAvailabilityState::Unavailable);
}

inline int32 CountProviderRows(
	FStringView ProviderId,
	TConstArrayView<FAssetDocumentProviderAvailability> Providers)
{
	int32 Count = 0;
	for (const FAssetDocumentProviderAvailability& Provider : Providers)
	{
		Count += Provider.ProviderId == ProviderId;
	}
	return Count;
}

inline const FAssetDocumentProviderAvailability* FindProviderRow(
	FStringView ProviderId,
	TConstArrayView<FAssetDocumentProviderAvailability> Providers)
{
	for (const FAssetDocumentProviderAvailability& Provider : Providers)
	{
		if (Provider.ProviderId == ProviderId)
		{
			return &Provider;
		}
	}
	return nullptr;
}

inline FAssetDocumentContractValidationResult ValidateProviderRows(
	const FPrimitiveRegistryValidationContext& Context)
{
	for (const FAssetDocumentProviderAvailability& Provider : Context.Providers)
	{
		if (!IsProviderRowValid(Provider)
			|| !IsProviderReferenced(Provider.ProviderId, Context.Descriptors))
		{
			return MakeValidationResult(false, TEXT("PrimitiveRegistry.ProviderCardinality"),
				TEXT("Provider availability rows must be valid and referenced exactly."));
		}
	}
	return MakeValidResult();
}

inline FAssetDocumentContractValidationResult ValidateDescriptorProviders(
	const FPrimitiveRegistryValidationContext& Context)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Context.Descriptors)
	{
		if (CountProviderRows(Descriptor.ProviderId, Context.Providers) != 1)
		{
			return MakeValidationResult(false, TEXT("PrimitiveRegistry.ProviderCardinality"),
				TEXT("Every primitive must resolve exactly one provider availability row."));
		}
		const FAssetDocumentProviderAvailability* Provider =
			FindProviderRow(Descriptor.ProviderId, Context.Providers);
		if (Provider->State != EAssetDocumentProviderAvailabilityState::Available)
		{
			return MakeValidationResult(false, TEXT("PrimitiveRegistry.ProviderUnavailable"),
				TEXT("Required primitive providers must be available before freeze."));
		}
	}
	return MakeValidResult();
}

inline bool IsRequirementIdentityValid(const FAssetDocumentPrimitiveRequirement& Requirement)
{
	return !Requirement.PrimitiveId.IsEmpty()
		&& Requirement.Version > 0
		&& !Requirement.RequiredByRouteId.IsEmpty();
}

inline bool HaveSameRequirement(
	const FAssetDocumentPrimitiveRequirement& Left,
	const FAssetDocumentPrimitiveRequirement& Right)
{
	return Left.PrimitiveId == Right.PrimitiveId
		&& Left.Version == Right.Version
		&& Left.RequiredByRouteId == Right.RequiredByRouteId;
}

inline bool HasEarlierDuplicateRequirement(
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Required,
	int32 Index)
{
	for (int32 ExistingIndex = 0; ExistingIndex < Index; ++ExistingIndex)
	{
		if (HaveSameRequirement(Required[Index], Required[ExistingIndex]))
		{
			return true;
		}
	}
	return false;
}

inline FAssetDocumentContractValidationResult ValidateRequirementRows(
	const FPrimitiveRegistryValidationContext& Context)
{
	for (int32 Index = 0; Index < Context.Required.Num(); ++Index)
	{
		if (!IsRequirementIdentityValid(Context.Required[Index])
			|| HasEarlierDuplicateRequirement(Context.Required, Index))
		{
			return MakeValidationResult(false, TEXT("PrimitiveRegistry.InvalidRequirement"),
				TEXT("Required primitive identities, routes, and tuples must be exact."));
		}
	}
	return MakeValidResult();
}

inline int32 CountMatchingDescriptors(
	const FAssetDocumentPrimitiveRequirement& Requirement,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors)
{
	int32 Count = 0;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		Count += Descriptor.PrimitiveId == Requirement.PrimitiveId
			&& Descriptor.Version == Requirement.Version;
	}
	return Count;
}

inline const FAssetDocumentPrimitiveDescriptor* FindMatchingDescriptor(
	const FAssetDocumentPrimitiveRequirement& Requirement,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		if (Descriptor.PrimitiveId == Requirement.PrimitiveId
			&& Descriptor.Version == Requirement.Version)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

inline FAssetDocumentContractValidationResult ValidateRequiredResolution(
	const FPrimitiveRegistryValidationContext& Context)
{
	for (const FAssetDocumentPrimitiveRequirement& Requirement : Context.Required)
	{
		if (CountMatchingDescriptors(Requirement, Context.Descriptors) != 1)
		{
			return MakeValidationResult(false, TEXT("PrimitiveRegistry.MissingRequiredPrimitive"),
				TEXT("Every requirement must resolve one exact primitive id and version."));
		}
		const FAssetDocumentPrimitiveDescriptor* Descriptor =
			FindMatchingDescriptor(Requirement, Context.Descriptors);
		if (!Descriptor->RequiredConsumerIds.Contains(Requirement.RequiredByRouteId))
		{
			return MakeValidationResult(false, TEXT("PrimitiveRegistry.ConsumerClosureMismatch"),
				TEXT("Required route is not in the descriptor consumer closure."));
		}
	}
	return MakeValidResult();
}

inline bool HasExactRequirement(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FStringView ConsumerId,
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Required)
{
	for (const FAssetDocumentPrimitiveRequirement& Requirement : Required)
	{
		if (Requirement.PrimitiveId == Descriptor.PrimitiveId
			&& Requirement.Version == Descriptor.Version
			&& Requirement.RequiredByRouteId == ConsumerId)
		{
			return true;
		}
	}
	return false;
}

inline bool HasAnyRequirement(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Required)
{
	for (const FAssetDocumentPrimitiveRequirement& Requirement : Required)
	{
		if (Requirement.PrimitiveId == Descriptor.PrimitiveId
			&& Requirement.Version == Descriptor.Version)
		{
			return true;
		}
	}
	return false;
}

inline bool HasCompleteConsumerClosure(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Required)
{
	for (const FString& ConsumerId : Descriptor.RequiredConsumerIds)
	{
		if (!HasExactRequirement(Descriptor, ConsumerId, Required))
		{
			return false;
		}
	}
	return true;
}

inline FAssetDocumentContractValidationResult ValidateDescriptorConsumerClosure(
	const FPrimitiveRegistryValidationContext& Context)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Context.Descriptors)
	{
		if (!HasAnyRequirement(Descriptor, Context.Required))
		{
			return MakeValidationResult(false, TEXT("PrimitiveRegistry.OrphanDescriptor"),
				TEXT("Every frozen descriptor must be required by at least one route."));
		}
		if (!HasCompleteConsumerClosure(Descriptor, Context.Required))
		{
			return MakeValidationResult(false, TEXT("PrimitiveRegistry.ConsumerClosureMismatch"),
				TEXT("Every descriptor consumer must have an exact frozen requirement."));
		}
	}
	return MakeValidResult();
}

inline bool IsGovernanceIdentity(
	FStringView Identity,
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog)
{
	for (const FAssetDocumentAuthorityCandidateRecord& Candidate : Catalog)
	{
		if (Candidate.GovernanceId == Identity
			|| FindAssetDocumentAuthorityReviewRecord(Candidate, Identity) != nullptr)
		{
			return true;
		}
	}
	return false;
}

inline bool HasRuntimeIdentityCollision(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog)
{
	return IsGovernanceIdentity(Descriptor.PrimitiveId, Catalog)
		|| IsGovernanceIdentity(Descriptor.ProviderId, Catalog);
}

inline bool HasConsumerIdentityCollision(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog)
{
	for (const FString& ConsumerId : Descriptor.RequiredConsumerIds)
	{
		if (IsGovernanceIdentity(ConsumerId, Catalog))
		{
			return true;
		}
	}
	return false;
}

inline bool HasDescriptorNamespaceLeakage(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog)
{
	return HasRuntimeIdentityCollision(Descriptor, Catalog)
		|| HasConsumerIdentityCollision(Descriptor, Catalog);
}

inline FAssetDocumentContractValidationResult ValidateRuntimeNamespaces(
	const FPrimitiveRegistryValidationContext& Context)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Context.Descriptors)
	{
		if (HasDescriptorNamespaceLeakage(Descriptor, Context.AuthorityCatalog))
		{
			return MakeValidationResult(false, TEXT("PrimitiveRegistry.GovernanceNamespaceLeakage"),
				TEXT("Governance identities cannot occupy runtime identity namespaces."));
		}
	}
	for (const FAssetDocumentPrimitiveRequirement& Requirement : Context.Required)
	{
		if (IsGovernanceIdentity(Requirement.RequiredByRouteId, Context.AuthorityCatalog))
		{
			return MakeValidationResult(false, TEXT("PrimitiveRegistry.GovernanceNamespaceLeakage"),
				TEXT("Governance identities cannot be runtime route identities."));
		}
	}
	return MakeValidResult();
}

inline bool HasCandidateReviewShape(const FAssetDocumentAuthorityReviewRecord& Review)
{
	return Review.ReviewKind == EAssetDocumentAuthorityReviewKind::Candidate
		&& Review.Verdict == EAssetDocumentIndependentVerdict::Pass
		&& Review.ReviewedState == EAssetDocumentAuthorityState::CandidateApproved;
}

inline bool HasBridgeReviewShape(const FAssetDocumentAuthorityReviewRecord& Review)
{
	return Review.ReviewKind == EAssetDocumentAuthorityReviewKind::Bridge
		&& Review.Verdict == EAssetDocumentIndependentVerdict::Pass
		&& Review.ReviewedState == EAssetDocumentAuthorityState::BridgeAdmitted;
}

inline bool HasReviewEvidence(const FAssetDocumentAuthorityReviewRecord& Review)
{
	return !Review.EvidenceHash.IsEmpty() && !Review.ReviewedHead.IsEmpty();
}

inline bool IsReviewerEligible(
	FStringView ReviewerId,
	const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	const FString* Ineligible[] = {
		&Candidate.ResearcherId, &Candidate.ImplementerId, &Candidate.ProviderOwnerId,
	};
	for (const FString* Identity : Ineligible)
	{
		if (ReviewerId == *Identity)
		{
			return false;
		}
	}
	return true;
}

inline bool HaveExactBridgeReviewShapes(
	const FAssetDocumentAuthorityReviewRecord& CandidateReview,
	const FAssetDocumentAuthorityReviewRecord& BridgeReview)
{
	return HasCandidateReviewShape(CandidateReview) && HasBridgeReviewShape(BridgeReview);
}

inline bool HaveBridgeReviewEvidence(
	const FAssetDocumentAuthorityReviewRecord& CandidateReview,
	const FAssetDocumentAuthorityReviewRecord& BridgeReview)
{
	return HasReviewEvidence(CandidateReview) && HasReviewEvidence(BridgeReview);
}

inline bool HaveIndependentBridgeReviewers(
	const FAssetDocumentAuthorityReviewRecord& CandidateReview,
	const FAssetDocumentAuthorityReviewRecord& BridgeReview,
	const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	if (CandidateReview.ReviewerId == BridgeReview.ReviewerId)
	{
		return false;
	}
	return IsReviewerEligible(CandidateReview.ReviewerId, Candidate)
		&& IsReviewerEligible(BridgeReview.ReviewerId, Candidate);
}

inline FAssetDocumentContractValidationResult ValidateBridgeReviewContract(
	const FAssetDocumentAuthorityReviewRecord& CandidateReview,
	const FAssetDocumentAuthorityReviewRecord& BridgeReview,
	const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	if (!HaveExactBridgeReviewShapes(CandidateReview, BridgeReview))
	{
		return MakeValidationResult(false, TEXT("PrimitiveRegistry.InvalidBridgeReview"),
			TEXT("Bridge review ids must resolve exact candidate and bridge Pass records."));
	}
	if (!HaveBridgeReviewEvidence(CandidateReview, BridgeReview))
	{
		return MakeValidationResult(false, TEXT("PrimitiveRegistry.InvalidBridgeReview"),
			TEXT("Bridge reviews require evidence hashes and reviewed heads."));
	}
	if (!HaveIndependentBridgeReviewers(CandidateReview, BridgeReview, Candidate))
	{
		return MakeValidationResult(false, TEXT("PrimitiveRegistry.InvalidBridgeReview"),
			TEXT("Bridge reviewers must be distinct and independently eligible."));
	}
	return MakeValidResult();
}

inline FAssetDocumentContractValidationResult ResolveAndValidateBridgeReviews(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	const FAssetDocumentAuthorityReviewRecord* CandidateReview = FindAssetDocumentAuthorityReviewRecord(
		Candidate, Descriptor.Admission.IndependentCandidateReviewId);
	const FAssetDocumentAuthorityReviewRecord* BridgeReview = FindAssetDocumentAuthorityReviewRecord(
		Candidate, Descriptor.Admission.IndependentBridgeReviewId);
	if (CandidateReview == nullptr || BridgeReview == nullptr)
	{
		return MakeValidationResult(false, TEXT("PrimitiveRegistry.InvalidBridgeReview"),
			TEXT("Bridge review references must resolve by exact id."));
	}
	return ValidateBridgeReviewContract(*CandidateReview, *BridgeReview, Candidate);
}

inline bool HasExactBridgeAuthorityBinding(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	return Candidate.PhysicalOwner == Descriptor.Owner
		&& Candidate.PhysicalOwner == Descriptor.Admission.PhysicalOwner
		&& Candidate.ImplementationSymbol == Descriptor.Admission.ExportedAuthoritativeApi;
}

inline FAssetDocumentContractValidationResult ValidateBridgeAuthorityBinding(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentAuthorityCandidateRecord& Candidate)
{
	if (!HasExactBridgeAuthorityBinding(Descriptor, Candidate))
	{
		return MakeValidationResult(false, TEXT("PrimitiveRegistry.BridgeAuthorityBindingMismatch"),
			TEXT("Bridge governance owner and implementation symbol must exactly bind the runtime descriptor."));
	}
	return MakeValidResult();
}

inline FAssetDocumentContractValidationResult ValidateBridgeDescriptor(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog)
{
	if (Descriptor.Admission.Disposition != EAssetDocumentPrimitiveDisposition::AdmittedBridge)
	{
		return MakeValidResult();
	}
	const FAssetDocumentAuthorityCandidateRecord* Candidate =
		FindAssetDocumentAuthorityCandidateRecord(Catalog, Descriptor.Admission.GovernanceId);
	if (Candidate == nullptr || Candidate->State != EAssetDocumentAuthorityState::BridgeAdmitted)
	{
		return MakeValidationResult(false, TEXT("PrimitiveRegistry.InvalidBridgeGovernance"),
			TEXT("Admitted bridges require exact BridgeAdmitted governance."));
	}
	const FAssetDocumentContractValidationResult Binding =
		ValidateBridgeAuthorityBinding(Descriptor, *Candidate);
	if (!Binding.bValid)
	{
		return Binding;
	}
	return ResolveAndValidateBridgeReviews(Descriptor, *Candidate);
}

inline FAssetDocumentContractValidationResult ValidateBridgeDescriptors(
	const FPrimitiveRegistryValidationContext& Context)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Context.Descriptors)
	{
		const FAssetDocumentContractValidationResult Result =
			ValidateBridgeDescriptor(Descriptor, Context.AuthorityCatalog);
		if (!Result.bValid)
		{
			return Result;
		}
	}
	return MakeValidResult();
}
} // namespace UE::AssetDocument::PrimitiveRegistryContract::Private

inline FAssetDocumentContractValidationResult ValidateAssetDocumentPrimitiveFreezeInput(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Required,
	TConstArrayView<FAssetDocumentProviderAvailability> Providers,
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> AuthorityCatalog)
{
	using namespace UE::AssetDocument::PrimitiveRegistryContract::Private;
	const FPrimitiveRegistryValidationContext Context{Descriptors, Required, Providers, AuthorityCatalog};
	using FValidator = FAssetDocumentContractValidationResult (*)(const FPrimitiveRegistryValidationContext&);
	static constexpr FValidator Validators[] = {
		&ValidateDescriptorContracts,
		&ValidateAuthorityCatalog,
		&ValidateRuntimeDispositions,
		&ValidateNonEmptyConsumerClosure,
		&ValidateOwnerProviderPrefixes,
		&ValidateProviderRows,
		&ValidateDescriptorProviders,
		&ValidateRequirementRows,
		&ValidateRequiredResolution,
		&ValidateDescriptorConsumerClosure,
		&ValidateRuntimeNamespaces,
		&ValidateBridgeDescriptors,
	};
	for (FValidator Validator : Validators)
	{
		const FAssetDocumentContractValidationResult Result = Validator(Context);
		if (!Result.bValid)
		{
			return Result;
		}
	}
	return MakeValidResult();
}
