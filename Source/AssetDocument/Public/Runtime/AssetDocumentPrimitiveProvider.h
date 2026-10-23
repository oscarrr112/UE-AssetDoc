// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

enum class EAssetDocumentPrimitiveOwner : uint8
{
	Core = 0,
	Editor = 1,
	AI = 2,
	Animation = 3,
	Blueprint = 4,
	UMG = 5,
	Graph = 6,
	Data = 7,
	AnimationAsset = 8,
	// FX (Niagara, AssetDocumentNiagara) is reserved at 9 so the two batch-2 owners merge without renumbering.
	FX = 9,
	Audio = 10,
	Rig = 11,
	Cinematics = 12,
	Physics = 13,
};

enum class EAssetDocumentProviderAvailabilityState : uint8
{
	Available = 0,
	Unavailable = 1,
};

enum class EAssetDocumentPrimitiveDisposition : uint8
{
	OrdinaryHelper = 0,
	FrameworkRawLifecycleAllowlisted = 1,
	StableBlocker = 2,
	GovernanceCandidate = 3,
	AdmittedBridge = 4,
};

enum class EAssetDocumentPrimitiveAuthorityPath : uint8
{
	ExistingExportedHelper = 0,
	NewExportedHelper = 1,
	FrameworkRawLifecycleAllowlist = 2,
	BridgeAdmitted = 3,
	StableBlocker = 4,
};

enum class EAssetDocumentPrimitiveOutcomeStatus : uint8
{
	Succeeded = 0,
	Failed = 1,
	Unavailable = 2,
};

enum class EAssetDocumentHandleResultCardinality : uint8
{
	ExactlyOne = 0,
	ZeroOrOne = 1,
	ZeroOrMore = 2,
	OneOrMore = 3,
};

struct ASSETDOCUMENT_API FAssetDocumentPrimitiveAdmissionEvidence
{
	uint32 ContractVersion = AssetDocumentRuntimeContractVersion;
	EAssetDocumentPrimitiveDisposition Disposition = EAssetDocumentPrimitiveDisposition::StableBlocker;
	EAssetDocumentPrimitiveAuthorityPath AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::StableBlocker;
	FString ExactEngineVersion;
	EAssetDocumentPrimitiveOwner PhysicalOwner = EAssetDocumentPrimitiveOwner::Core;
	FString PublicModule;
	FString PublicHeader;
	FString ExportedAuthoritativeApi;
	FString AuthorityContextId;
	FString PrimaryOperationId;
	FString IdentityLifetimeContractId;
	FString TransactionContractId;
	FString RollbackContractId;
	FString CompletionContractId;
	FString SaveReloadEvidenceId;
	TArray<FString> FailureInjectionTestIds;
	TArray<FString> VerificationTestIds;
	FString SourceMarker;
	FString RawLifecycleAllowlistId;
	FString GovernanceId;
	FString IndependentCandidateReviewId;
	FString IndependentBridgeReviewId;
	FString HelperReplacementCondition;
	FString RemovalCondition;
	FString AuthorityFingerprint;
};

struct ASSETDOCUMENT_API FAssetDocumentPrimitiveHandleArgumentSchema
{
	FString Name;
	EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Object;
	EAssetDocumentHandleAccess RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	bool bRequired = true;
};

struct ASSETDOCUMENT_API FAssetDocumentPrimitiveHandleArgument
{
	FString Name;
	FAssetDocumentOpaqueHandle Handle;
};

struct ASSETDOCUMENT_API FAssetDocumentPrimitiveHandleResultSchema
{
	FString Name;
	EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Object;
	EAssetDocumentHandleAccess GrantedAccess = EAssetDocumentHandleAccess::ReadOnly;
	EAssetDocumentHandleResultCardinality Cardinality = EAssetDocumentHandleResultCardinality::ExactlyOne;
};

struct ASSETDOCUMENT_API FAssetDocumentPrimitiveHandleResult
{
	FString Name;
	TArray<FAssetDocumentOpaqueHandle> Handles;
};

struct ASSETDOCUMENT_API FAssetDocumentPrimitiveDescriptor
{
	FString PrimitiveId;
	uint32 Version = 0;
	FString ProviderId;
	EAssetDocumentPrimitiveOwner Owner = EAssetDocumentPrimitiveOwner::Core;
	EAssetDocumentExecutionThread ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	EAssetDocumentPrimitiveAccess Access = EAssetDocumentPrimitiveAccess::ReadOnly;
	TArray<EAssetDocumentRuntimeRequestPhase> AllowedPhases;
	TArray<EAssetDocumentHandleKind> AllowedHandleKinds;
	TArray<FAssetDocumentPrimitiveHandleArgumentSchema> HandleArgumentSchemas;
	TArray<FAssetDocumentPrimitiveHandleResultSchema> HandleResultSchemas;
	bool bRequiresActiveTransaction = false;
	FString InputSchemaId;
	FString OutputSchemaId;
	FString MutationFootprintSchemaId;
	TArray<FString> StableDiagnosticCodes;
	TArray<FString> RequiredConsumerIds;
	FAssetDocumentPrimitiveAdmissionEvidence Admission;
};

struct ASSETDOCUMENT_API FAssetDocumentPrimitiveInvocation
{
	FString PrimitiveId;
	uint32 Version = 0;
	FGuid RequestId;
	FGuid TransactionId;
	uint64 RuntimeRevision = 0;
	EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	FAssetDocumentRuntimeValueDto ValueArguments;
	TArray<FAssetDocumentPrimitiveHandleArgument> HandleArguments;
};

struct ASSETDOCUMENT_API FAssetDocumentPrimitiveOutcome
{
	EAssetDocumentPrimitiveOutcomeStatus Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	FAssetDocumentRuntimeValueDto Value;
	TArray<FAssetDocumentPrimitiveHandleResult> HandleResults;
	TArray<FAssetDocumentChangedRecord> ChangedSet;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

struct ASSETDOCUMENT_API FAssetDocumentProviderAvailability
{
	FString ProviderId;
	EAssetDocumentProviderAvailabilityState State = EAssetDocumentProviderAvailabilityState::Unavailable;
	TOptional<FAssetDocumentRuntimeDiagnostic> Diagnostic;
};

class IAssetDocumentHandleResolver;
class IAssetDocumentAtomicTransaction;

class ASSETDOCUMENT_API IAssetDocumentPrimitiveProvider
{
public:
	virtual ~IAssetDocumentPrimitiveProvider() = default;
	virtual FString GetProviderId() const = 0;
	virtual TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const = 0;
	virtual FAssetDocumentProviderAvailability InspectAvailability() const = 0;
	virtual FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction* ActiveTransaction) = 0;
};

namespace UE::AssetDocument::PrimitiveAdmission::Private
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
		Diagnostic.Path = TEXT("/Primitive");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

inline bool IsValidOwner(EAssetDocumentPrimitiveOwner Owner)
{
	return static_cast<uint8>(Owner) <= static_cast<uint8>(EAssetDocumentPrimitiveOwner::Physics);
}

inline bool IsValidAccess(EAssetDocumentPrimitiveAccess Access)
{
	return static_cast<uint8>(Access) <= static_cast<uint8>(EAssetDocumentPrimitiveAccess::Mutating);
}

inline bool IsValidPhase(EAssetDocumentRuntimeRequestPhase Phase)
{
	return static_cast<uint8>(Phase) <= static_cast<uint8>(EAssetDocumentRuntimeRequestPhase::Failed);
}

inline bool IsValidHandleKind(EAssetDocumentHandleKind Kind)
{
	return static_cast<uint8>(Kind) <= static_cast<uint8>(EAssetDocumentHandleKind::Scratch);
}

inline bool IsValidHandleAccess(EAssetDocumentHandleAccess Access)
{
	return static_cast<uint8>(Access) <= static_cast<uint8>(EAssetDocumentHandleAccess::Mutable);
}

inline bool IsValidResultCardinality(EAssetDocumentHandleResultCardinality Cardinality)
{
	return static_cast<uint8>(Cardinality)
		<= static_cast<uint8>(EAssetDocumentHandleResultCardinality::OneOrMore);
}

template <typename ItemType>
inline bool HasDuplicates(TConstArrayView<ItemType> Values)
{
	TSet<ItemType> Seen;
	for (const ItemType& Value : Values)
	{
		if (Seen.Contains(Value))
		{
			return true;
		}
		Seen.Add(Value);
	}
	return false;
}

inline bool IsNonEmptyStringListValid(TConstArrayView<FString> Values)
{
	if (Values.IsEmpty() || HasDuplicates(Values))
	{
		return false;
	}
	for (const FString& Value : Values)
	{
		if (Value.IsEmpty())
		{
			return false;
		}
	}
	return true;
}

inline bool IsOptionalStringListValid(TConstArrayView<FString> Values)
{
	return Values.IsEmpty() || IsNonEmptyStringListValid(Values);
}

inline bool ArePhasesValid(TConstArrayView<EAssetDocumentRuntimeRequestPhase> Phases)
{
	if (Phases.IsEmpty() || HasDuplicates(Phases))
	{
		return false;
	}
	for (EAssetDocumentRuntimeRequestPhase Phase : Phases)
	{
		if (!IsValidPhase(Phase))
		{
			return false;
		}
	}
	return true;
}

inline bool AreHandleKindsValid(TConstArrayView<EAssetDocumentHandleKind> Kinds)
{
	if (HasDuplicates(Kinds))
	{
		return false;
	}
	for (EAssetDocumentHandleKind Kind : Kinds)
	{
		if (!IsValidHandleKind(Kind))
		{
			return false;
		}
	}
	return true;
}

inline bool IsHandleSchemaValid(
	const FAssetDocumentPrimitiveHandleArgumentSchema& Schema,
	TConstArrayView<EAssetDocumentHandleKind> AllowedKinds)
{
	return !Schema.Name.IsEmpty()
		&& IsValidHandleKind(Schema.Kind)
		&& IsValidHandleAccess(Schema.RequiredAccess)
		&& AllowedKinds.Contains(Schema.Kind);
}

inline bool AreHandleSchemasValid(
	TConstArrayView<FAssetDocumentPrimitiveHandleArgumentSchema> Schemas,
	TConstArrayView<EAssetDocumentHandleKind> AllowedKinds)
{
	TSet<FString> SeenNames;
	for (const FAssetDocumentPrimitiveHandleArgumentSchema& Schema : Schemas)
	{
		if (!IsHandleSchemaValid(Schema, AllowedKinds) || SeenNames.Contains(Schema.Name))
		{
			return false;
		}
		SeenNames.Add(Schema.Name);
	}
	return true;
}

inline bool IsHandleResultSchemaValid(
	const FAssetDocumentPrimitiveHandleResultSchema& Schema,
	TConstArrayView<EAssetDocumentHandleKind> AllowedKinds)
{
	return !Schema.Name.IsEmpty()
		&& IsValidHandleKind(Schema.Kind)
		&& IsValidHandleAccess(Schema.GrantedAccess)
		&& IsValidResultCardinality(Schema.Cardinality)
		&& AllowedKinds.Contains(Schema.Kind);
}

inline bool AreHandleResultSchemasValid(
	TConstArrayView<FAssetDocumentPrimitiveHandleResultSchema> Schemas,
	TConstArrayView<EAssetDocumentHandleKind> AllowedKinds)
{
	TSet<FString> SeenNames;
	for (const FAssetDocumentPrimitiveHandleResultSchema& Schema : Schemas)
	{
		if (!IsHandleResultSchemaValid(Schema, AllowedKinds) || SeenNames.Contains(Schema.Name))
		{
			return false;
		}
		SeenNames.Add(Schema.Name);
	}
	return true;
}

inline bool IsDescriptorIdentityPresent(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return !Descriptor.PrimitiveId.IsEmpty()
		&& Descriptor.Version > 0
		&& !Descriptor.ProviderId.IsEmpty();
}

inline bool AreDescriptorEnumsValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return IsValidOwner(Descriptor.Owner)
		&& Descriptor.ExecutionThread == EAssetDocumentExecutionThread::GameThread
		&& IsValidAccess(Descriptor.Access)
		&& IsValidOwner(Descriptor.Admission.PhysicalOwner);
}

inline bool IsDescriptorIdentityValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return IsDescriptorIdentityPresent(Descriptor)
		&& AreDescriptorEnumsValid(Descriptor)
		&& Descriptor.Owner == Descriptor.Admission.PhysicalOwner;
}

inline bool IsStableBlockerSchemaValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return Descriptor.AllowedHandleKinds.IsEmpty()
		&& Descriptor.HandleArgumentSchemas.IsEmpty()
		&& Descriptor.HandleResultSchemas.IsEmpty()
		&& Descriptor.InputSchemaId.IsEmpty()
		&& Descriptor.OutputSchemaId.IsEmpty();
}

inline bool IsStableBlockerFootprintValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return Descriptor.MutationFootprintSchemaId.IsEmpty()
		&& !Descriptor.bRequiresActiveTransaction;
}

inline bool IsCallableMutationShapeValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	if (Descriptor.Access == EAssetDocumentPrimitiveAccess::Mutating)
	{
		return Descriptor.bRequiresActiveTransaction && !Descriptor.MutationFootprintSchemaId.IsEmpty();
	}
	return true;
}

inline bool IsCallableSchemaValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return !Descriptor.InputSchemaId.IsEmpty()
		&& !Descriptor.OutputSchemaId.IsEmpty()
		&& IsCallableMutationShapeValid(Descriptor);
}

inline bool IsPathDependentSchemaValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	if (Descriptor.Admission.Disposition == EAssetDocumentPrimitiveDisposition::StableBlocker)
	{
		return IsStableBlockerSchemaValid(Descriptor) && IsStableBlockerFootprintValid(Descriptor);
	}
	return IsCallableSchemaValid(Descriptor);
}

inline bool AreDescriptorHandleCollectionsValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return AreHandleKindsValid(Descriptor.AllowedHandleKinds)
		&& AreHandleSchemasValid(Descriptor.HandleArgumentSchemas, Descriptor.AllowedHandleKinds)
		&& AreHandleResultSchemasValid(Descriptor.HandleResultSchemas, Descriptor.AllowedHandleKinds);
}

inline bool IsDescriptorSchemaValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return ArePhasesValid(Descriptor.AllowedPhases)
		&& AreDescriptorHandleCollectionsValid(Descriptor)
		&& IsNonEmptyStringListValid(Descriptor.StableDiagnosticCodes)
		&& IsPathDependentSchemaValid(Descriptor);
}

inline bool HasAdmissionIdentity(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return Evidence.ContractVersion == AssetDocumentRuntimeContractVersion
		&& IsValidOwner(Evidence.PhysicalOwner)
		&& !Evidence.AuthorityFingerprint.IsEmpty();
}

inline bool HasPublicAuthorityEvidence(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return !Evidence.ExactEngineVersion.IsEmpty()
		&& !Evidence.PublicModule.IsEmpty()
		&& !Evidence.PublicHeader.IsEmpty()
		&& !Evidence.ExportedAuthoritativeApi.IsEmpty();
}

inline bool HasAuthorityContractEvidence(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return !Evidence.AuthorityContextId.IsEmpty()
		&& !Evidence.PrimaryOperationId.IsEmpty()
		&& !Evidence.IdentityLifetimeContractId.IsEmpty()
		&& !Evidence.TransactionContractId.IsEmpty();
}

inline bool HasCompletionEvidence(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return !Evidence.RollbackContractId.IsEmpty()
		&& !Evidence.CompletionContractId.IsEmpty()
		&& !Evidence.SaveReloadEvidenceId.IsEmpty();
}

inline bool HasCallableEvidenceCore(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return HasAdmissionIdentity(Evidence)
		&& HasPublicAuthorityEvidence(Evidence)
		&& HasAuthorityContractEvidence(Evidence)
		&& HasCompletionEvidence(Evidence);
}

inline bool HasTestEvidence(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return IsNonEmptyStringListValid(Evidence.FailureInjectionTestIds)
		&& IsNonEmptyStringListValid(Evidence.VerificationTestIds);
}

inline bool AreTestEvidenceListsStructurallyValid(
	const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return IsOptionalStringListValid(Evidence.FailureInjectionTestIds)
		&& IsOptionalStringListValid(Evidence.VerificationTestIds);
}

inline bool IsCallableAdmissionEvidenceValid(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return HasCallableEvidenceCore(Evidence) && HasTestEvidence(Evidence);
}

inline bool HasRequiredConsumerEvidence(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return IsNonEmptyStringListValid(Descriptor.RequiredConsumerIds);
}

inline bool IsBlockerAdmissionEvidenceValid(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return HasAdmissionIdentity(Evidence) && !Evidence.PrimaryOperationId.IsEmpty();
}

inline bool IsAdmissionEvidenceValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	if (!AreTestEvidenceListsStructurallyValid(Descriptor.Admission))
	{
		return false;
	}
	if (Descriptor.Admission.Disposition == EAssetDocumentPrimitiveDisposition::StableBlocker)
	{
		return IsBlockerAdmissionEvidenceValid(Descriptor.Admission);
	}
	return IsCallableAdmissionEvidenceValid(Descriptor.Admission)
		&& HasRequiredConsumerEvidence(Descriptor);
}

inline bool HasNoGovernanceFields(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return Evidence.GovernanceId.IsEmpty()
		&& Evidence.IndependentCandidateReviewId.IsEmpty()
		&& Evidence.IndependentBridgeReviewId.IsEmpty();
}

inline bool HasNoRawLifecycleFields(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return Evidence.SourceMarker.IsEmpty()
		&& Evidence.RawLifecycleAllowlistId.IsEmpty()
		&& Evidence.HelperReplacementCondition.IsEmpty()
		&& Evidence.RemovalCondition.IsEmpty();
}

inline bool IsOrdinaryPathValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const bool bExportedHelper = Descriptor.Admission.AuthorityPath == EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper
		|| Descriptor.Admission.AuthorityPath == EAssetDocumentPrimitiveAuthorityPath::NewExportedHelper;
	return Descriptor.Admission.Disposition == EAssetDocumentPrimitiveDisposition::OrdinaryHelper
		&& bExportedHelper
		&& HasNoGovernanceFields(Descriptor.Admission)
		&& HasNoRawLifecycleFields(Descriptor.Admission);
}

inline bool HasForbiddenMarkerTerm(const FString& Marker)
{
	return Marker.Contains(TEXT("wrapper"), ESearchCase::IgnoreCase)
		|| Marker.Contains(TEXT("fallback"), ESearchCase::IgnoreCase);
}

inline bool IsFrameworkSourceMarkerValid(
	const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	const FString Expected = FString::Printf(
		TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_LIFECYCLE_ALLOWLIST(%s)"),
		*Evidence.PrimaryOperationId);
	return Evidence.SourceMarker == Expected && !HasForbiddenMarkerTerm(Evidence.SourceMarker);
}

inline bool IsFrameworkRawAllowlistValid(
	const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	const FString Expected = FString::Printf(
		TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_CALLSITE(%s)"),
		*Evidence.PrimaryOperationId);
	return Evidence.RawLifecycleAllowlistId == Expected;
}

inline bool IsFrameworkRawOperationValid(const FString& OperationId)
{
	return OperationId.StartsWith(TEXT("framework.factory."))
		|| OperationId.StartsWith(TEXT("framework.lifecycle."))
		|| OperationId.StartsWith(TEXT("framework.scratch."));
}

inline bool HasFrameworkRawReferences(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return IsFrameworkSourceMarkerValid(Evidence)
		&& IsFrameworkRawAllowlistValid(Evidence)
		&& !Evidence.HelperReplacementCondition.IsEmpty()
		&& !Evidence.RemovalCondition.IsEmpty();
}

inline bool IsEditorFrameworkRawOwner(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return Descriptor.Owner == EAssetDocumentPrimitiveOwner::Editor;
}

inline bool IsFrameworkRawDispositionPath(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return Descriptor.Admission.Disposition == EAssetDocumentPrimitiveDisposition::FrameworkRawLifecycleAllowlisted
		&& Descriptor.Admission.AuthorityPath == EAssetDocumentPrimitiveAuthorityPath::FrameworkRawLifecycleAllowlist;
}

inline bool IsFrameworkRawPathValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return IsFrameworkRawDispositionPath(Descriptor)
		&& IsEditorFrameworkRawOwner(Descriptor)
		&& IsFrameworkRawOperationValid(Descriptor.Admission.PrimaryOperationId)
		&& HasFrameworkRawReferences(Descriptor.Admission)
		&& HasNoGovernanceFields(Descriptor.Admission);
}

inline bool IsBridgeSourceMarkerValid(
	const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	const FString Expected = FString::Printf(
		TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(%s)"),
		*Evidence.GovernanceId);
	return Evidence.SourceMarker == Expected && !HasForbiddenMarkerTerm(Evidence.SourceMarker);
}

inline bool IsBridgeAllowlistValid(
	const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	const FString Expected = FString::Printf(
		TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(%s|%s)"),
		*Evidence.GovernanceId,
		*Evidence.PrimaryOperationId);
	return Evidence.RawLifecycleAllowlistId == Expected;
}

inline bool HasBridgeReferences(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return IsBridgeAllowlistValid(Evidence)
		&& !Evidence.GovernanceId.IsEmpty()
		&& !Evidence.IndependentCandidateReviewId.IsEmpty()
		&& !Evidence.IndependentBridgeReviewId.IsEmpty();
}

inline bool HasBridgeLifecycle(const FAssetDocumentPrimitiveAdmissionEvidence& Evidence)
{
	return IsBridgeSourceMarkerValid(Evidence)
		&& !Evidence.HelperReplacementCondition.IsEmpty()
		&& !Evidence.RemovalCondition.IsEmpty();
}

inline bool IsBridgePathValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const bool bPathMatches = Descriptor.Admission.Disposition == EAssetDocumentPrimitiveDisposition::AdmittedBridge
		&& Descriptor.Admission.AuthorityPath == EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted;
	return bPathMatches
		&& HasBridgeReferences(Descriptor.Admission)
		&& HasBridgeLifecycle(Descriptor.Admission);
}

inline bool HasNoCallableBlockerFields(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return Descriptor.Admission.ExportedAuthoritativeApi.IsEmpty()
		&& Descriptor.Admission.SourceMarker.IsEmpty()
		&& Descriptor.Admission.RawLifecycleAllowlistId.IsEmpty()
		&& Descriptor.RequiredConsumerIds.IsEmpty();
}

inline bool IsStableBlockerPathValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const bool bPathMatches = Descriptor.Admission.Disposition == EAssetDocumentPrimitiveDisposition::StableBlocker
		&& Descriptor.Admission.AuthorityPath == EAssetDocumentPrimitiveAuthorityPath::StableBlocker;
	return bPathMatches
		&& HasNoGovernanceFields(Descriptor.Admission)
		&& HasNoRawLifecycleFields(Descriptor.Admission)
		&& HasNoCallableBlockerFields(Descriptor);
}

inline bool IsGovernanceCandidateRuntimePathValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	(void)Descriptor;
	return false;
}

inline bool IsAdmissionPathValid(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	using FPathValidator = bool (*)(const FAssetDocumentPrimitiveDescriptor&);
	static constexpr FPathValidator Validators[] = {
		&IsOrdinaryPathValid,
		&IsFrameworkRawPathValid,
		&IsStableBlockerPathValid,
		&IsGovernanceCandidateRuntimePathValid,
		&IsBridgePathValid,
	};
	const uint8 DispositionIndex = static_cast<uint8>(Descriptor.Admission.Disposition);
	if (DispositionIndex >= UE_ARRAY_COUNT(Validators))
	{
		return false;
	}
	return Validators[DispositionIndex](Descriptor);
}

inline FAssetDocumentContractValidationResult ValidateDescriptorCore(
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	if (!IsDescriptorIdentityValid(Descriptor))
	{
		return MakeValidationResult(false, TEXT("PrimitiveDescriptor.InvalidIdentity"), TEXT("Primitive identity, owner, or execution metadata is invalid."));
	}
	if (!IsDescriptorSchemaValid(Descriptor))
	{
		return MakeValidationResult(false, TEXT("PrimitiveDescriptor.InvalidSchema"), TEXT("Primitive schemas, phases, handle arguments, or diagnostics are invalid."));
	}
	if (!IsAdmissionEvidenceValid(Descriptor))
	{
		return MakeValidationResult(false, TEXT("PrimitiveDescriptor.InvalidAdmission"), TEXT("Primitive authority or lifecycle evidence is incomplete."));
	}
	if (!IsAdmissionPathValid(Descriptor))
	{
		return MakeValidationResult(false, TEXT("PrimitiveDescriptor.InvalidPath"), TEXT("Primitive disposition and authority path are not mutually valid."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

template <typename ItemType>
inline bool HaveSameSet(TConstArrayView<ItemType> Left, TConstArrayView<ItemType> Right)
{
	if (Left.Num() != Right.Num())
	{
		return false;
	}
	for (const ItemType& Item : Left)
	{
		if (!Right.Contains(Item))
		{
			return false;
		}
	}
	return true;
}

inline const FAssetDocumentPrimitiveHandleArgumentSchema* FindHandleSchema(
	TConstArrayView<FAssetDocumentPrimitiveHandleArgumentSchema> Schemas,
	FStringView Name)
{
	for (const FAssetDocumentPrimitiveHandleArgumentSchema& Schema : Schemas)
	{
		if (Schema.Name == Name)
		{
			return &Schema;
		}
	}
	return nullptr;
}

inline bool DoHandleSchemasMatch(
	const FAssetDocumentPrimitiveHandleArgumentSchema& Left,
	const FAssetDocumentPrimitiveHandleArgumentSchema& Right)
{
	return Left.Kind == Right.Kind
		&& Left.RequiredAccess == Right.RequiredAccess
		&& Left.bRequired == Right.bRequired;
}

inline bool HaveSameHandleSchemas(
	TConstArrayView<FAssetDocumentPrimitiveHandleArgumentSchema> Left,
	TConstArrayView<FAssetDocumentPrimitiveHandleArgumentSchema> Right)
{
	if (Left.Num() != Right.Num())
	{
		return false;
	}
	for (const FAssetDocumentPrimitiveHandleArgumentSchema& Schema : Left)
	{
		const FAssetDocumentPrimitiveHandleArgumentSchema* Match = FindHandleSchema(Right, Schema.Name);
		if (Match == nullptr || !DoHandleSchemasMatch(Schema, *Match))
		{
			return false;
		}
	}
	return true;
}

inline const FAssetDocumentPrimitiveHandleResultSchema* FindHandleResultSchema(
	TConstArrayView<FAssetDocumentPrimitiveHandleResultSchema> Schemas,
	FStringView Name)
{
	for (const FAssetDocumentPrimitiveHandleResultSchema& Schema : Schemas)
	{
		if (Schema.Name == Name)
		{
			return &Schema;
		}
	}
	return nullptr;
}

inline bool DoHandleResultSchemasMatch(
	const FAssetDocumentPrimitiveHandleResultSchema& Left,
	const FAssetDocumentPrimitiveHandleResultSchema& Right)
{
	return Left.Kind == Right.Kind
		&& Left.GrantedAccess == Right.GrantedAccess
		&& Left.Cardinality == Right.Cardinality;
}

inline bool HaveSameHandleResultSchemas(
	TConstArrayView<FAssetDocumentPrimitiveHandleResultSchema> Left,
	TConstArrayView<FAssetDocumentPrimitiveHandleResultSchema> Right)
{
	if (Left.Num() != Right.Num())
	{
		return false;
	}
	for (const FAssetDocumentPrimitiveHandleResultSchema& Schema : Left)
	{
		const FAssetDocumentPrimitiveHandleResultSchema* Match = FindHandleResultSchema(Right, Schema.Name);
		if (Match == nullptr || !DoHandleResultSchemasMatch(Schema, *Match))
		{
			return false;
		}
	}
	return true;
}

inline bool HaveSameHandleContract(
	const FAssetDocumentPrimitiveDescriptor& Left,
	const FAssetDocumentPrimitiveDescriptor& Right)
{
	return HaveSameSet<EAssetDocumentHandleKind>(Left.AllowedHandleKinds, Right.AllowedHandleKinds)
		&& HaveSameHandleSchemas(Left.HandleArgumentSchemas, Right.HandleArgumentSchemas)
		&& HaveSameHandleResultSchemas(Left.HandleResultSchemas, Right.HandleResultSchemas);
}

inline bool HaveSameAuthorityIdentity(
	const FAssetDocumentPrimitiveDescriptor& Left,
	const FAssetDocumentPrimitiveDescriptor& Right)
{
	return Left.Owner == Right.Owner
		&& Left.Admission.PrimaryOperationId == Right.Admission.PrimaryOperationId
		&& Left.InputSchemaId == Right.InputSchemaId
		&& Left.OutputSchemaId == Right.OutputSchemaId;
}

inline bool HaveSameAuthorityBehavior(
	const FAssetDocumentPrimitiveDescriptor& Left,
	const FAssetDocumentPrimitiveDescriptor& Right)
{
	return HaveSameSet<EAssetDocumentRuntimeRequestPhase>(Left.AllowedPhases, Right.AllowedPhases)
		&& HaveSameHandleContract(Left, Right)
		&& Left.Admission.CompletionContractId == Right.Admission.CompletionContractId;
}

inline bool HaveSameComputedAuthorityFingerprint(
	const FAssetDocumentPrimitiveDescriptor& Left,
	const FAssetDocumentPrimitiveDescriptor& Right)
{
	return HaveSameAuthorityIdentity(Left, Right) && HaveSameAuthorityBehavior(Left, Right);
}

inline bool HaveSamePrimitiveIdentity(
	const FAssetDocumentPrimitiveDescriptor& Left,
	const FAssetDocumentPrimitiveDescriptor& Right)
{
	return Left.PrimitiveId == Right.PrimitiveId && Left.Version == Right.Version;
}

inline FAssetDocumentContractValidationResult ValidateDescriptorDuplicates(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> ExistingDescriptors)
{
	for (const FAssetDocumentPrimitiveDescriptor& Existing : ExistingDescriptors)
	{
		if (HaveSamePrimitiveIdentity(Descriptor, Existing))
		{
			return MakeValidationResult(false, TEXT("PrimitiveDescriptor.DuplicateIdentity"), TEXT("Primitive id and version must be globally unique."));
		}
		if (HaveSameComputedAuthorityFingerprint(Descriptor, Existing))
		{
			return MakeValidationResult(false, TEXT("PrimitiveDescriptor.DuplicateAuthority"), TEXT("Primitive authority fingerprint duplicates an existing operation."));
		}
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline bool IsInvocationCallable(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return Descriptor.Admission.Disposition != EAssetDocumentPrimitiveDisposition::StableBlocker
		&& Descriptor.Admission.Disposition != EAssetDocumentPrimitiveDisposition::GovernanceCandidate;
}

inline bool InvocationMatchesDescriptor(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return Invocation.PrimitiveId == Descriptor.PrimitiveId && Invocation.Version == Descriptor.Version;
}

inline bool IsInvocationEnvelopeValid(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return Invocation.RequestId.IsValid()
		&& Invocation.RuntimeRevision > 0
		&& Descriptor.AllowedPhases.Contains(Invocation.Phase);
}

inline bool IsInvocationTransactionValid(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return !Descriptor.bRequiresActiveTransaction || Invocation.TransactionId.IsValid();
}

inline bool IsInvocationIdentityValid(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return IsInvocationCallable(Descriptor)
		&& InvocationMatchesDescriptor(Invocation, Descriptor)
		&& IsInvocationEnvelopeValid(Invocation, Descriptor)
		&& IsInvocationTransactionValid(Invocation, Descriptor);
}

inline bool AreValueArgumentsValid(const FAssetDocumentPrimitiveInvocation& Invocation)
{
	return Invocation.ValueArguments.Kind == EAssetDocumentRuntimeValueKind::Object
		&& ValidateAssetDocumentRuntimeValue(Invocation.ValueArguments).bValid;
}

inline bool IsInvocationHandleNameValid(
	const FAssetDocumentPrimitiveHandleArgument& Argument,
	const TSet<FString>& SeenNames,
	TConstArrayView<FString> ValueArgumentNames)
{
	return !Argument.Name.IsEmpty()
		&& !SeenNames.Contains(Argument.Name)
		&& !ValueArgumentNames.Contains(Argument.Name);
}

inline bool DoesInvocationHandleMatch(
	const FAssetDocumentPrimitiveHandleArgument& Argument,
	const FAssetDocumentPrimitiveHandleArgumentSchema& Schema)
{
	return Argument.Handle.Kind == Schema.Kind && Argument.Handle.Access == Schema.RequiredAccess;
}

inline bool IsInvocationHandleValid(
	const FAssetDocumentPrimitiveHandleArgument& Argument,
	const TSet<FString>& SeenNames,
	TConstArrayView<FString> ValueArgumentNames,
	TConstArrayView<FAssetDocumentPrimitiveHandleArgumentSchema> Schemas)
{
	if (!IsInvocationHandleNameValid(Argument, SeenNames, ValueArgumentNames))
	{
		return false;
	}
	const FAssetDocumentPrimitiveHandleArgumentSchema* Schema = FindHandleSchema(Schemas, Argument.Name);
	return Schema != nullptr && DoesInvocationHandleMatch(Argument, *Schema);
}

inline bool AreRequiredInvocationHandlesPresent(
	TConstArrayView<FAssetDocumentPrimitiveHandleArgumentSchema> Schemas,
	const TSet<FString>& SeenNames)
{
	for (const FAssetDocumentPrimitiveHandleArgumentSchema& Schema : Schemas)
	{
		if (Schema.bRequired && !SeenNames.Contains(Schema.Name))
		{
			return false;
		}
	}
	return true;
}

inline bool AreInvocationHandlesValid(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	TSet<FString> SeenNames;
	for (const FAssetDocumentPrimitiveHandleArgument& Argument : Invocation.HandleArguments)
	{
		if (!IsInvocationHandleValid(
			Argument, SeenNames, Invocation.ValueArguments.ObjectKeys, Descriptor.HandleArgumentSchemas))
		{
			return false;
		}
		SeenNames.Add(Argument.Name);
	}
	return AreRequiredInvocationHandlesPresent(Descriptor.HandleArgumentSchemas, SeenNames);
}

inline bool IsValidOutcomeStatus(EAssetDocumentPrimitiveOutcomeStatus Status)
{
	return static_cast<uint8>(Status)
		<= static_cast<uint8>(EAssetDocumentPrimitiveOutcomeStatus::Unavailable);
}

inline bool IsOutcomeValueValid(const FAssetDocumentPrimitiveOutcome& Outcome)
{
	return Outcome.Value.Kind == EAssetDocumentRuntimeValueKind::Object
		&& ValidateAssetDocumentRuntimeValue(Outcome.Value).bValid;
}

inline bool IsHandleResultCountAllowed(
	EAssetDocumentHandleResultCardinality Cardinality,
	int32 Count)
{
	switch (Cardinality)
	{
	case EAssetDocumentHandleResultCardinality::ExactlyOne: return Count == 1;
	case EAssetDocumentHandleResultCardinality::ZeroOrOne: return Count <= 1;
	case EAssetDocumentHandleResultCardinality::ZeroOrMore: return true;
	case EAssetDocumentHandleResultCardinality::OneOrMore: return Count >= 1;
	default: return false;
	}
}

inline bool IsRequiredHandleResultSchema(
	const FAssetDocumentPrimitiveHandleResultSchema& Schema)
{
	return Schema.Cardinality == EAssetDocumentHandleResultCardinality::ExactlyOne
		|| Schema.Cardinality == EAssetDocumentHandleResultCardinality::OneOrMore;
}

inline bool HaveSameHandleInvocationIdentity(
	const FAssetDocumentOpaqueHandle& Left,
	const FAssetDocumentOpaqueHandle& Right)
{
	return Left.RequestId == Right.RequestId
		&& Left.TransactionId == Right.TransactionId
		&& Left.RuntimeRevision == Right.RuntimeRevision
		&& Left.ProviderId == Right.ProviderId;
}

inline bool HaveSameHandleStorageIdentity(
	const FAssetDocumentOpaqueHandle& Left,
	const FAssetDocumentOpaqueHandle& Right)
{
	return Left.Slot == Right.Slot
		&& Left.Generation == Right.Generation
		&& Left.Kind == Right.Kind
		&& Left.Access == Right.Access;
}

inline bool AreSameHandleIdentity(
	const FAssetDocumentOpaqueHandle& Left,
	const FAssetDocumentOpaqueHandle& Right)
{
	return HaveSameHandleInvocationIdentity(Left, Right)
		&& HaveSameHandleStorageIdentity(Left, Right);
}

inline bool IsUniqueReturnedHandle(
	const FAssetDocumentOpaqueHandle& Handle,
	TConstArrayView<FAssetDocumentOpaqueHandle> SeenHandles)
{
	for (const FAssetDocumentOpaqueHandle& Seen : SeenHandles)
	{
		if (AreSameHandleIdentity(Handle, Seen))
		{
			return false;
		}
	}
	return true;
}

inline bool HasReturnedRequestIdentity(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	return Handle.RequestId.IsValid() && Handle.RequestId == Invocation.RequestId;
}

inline bool HasReturnedTransactionIdentity(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	return Handle.TransactionId.IsValid() && Handle.TransactionId == Invocation.TransactionId;
}

inline bool HasReturnedRevisionIdentity(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	return Handle.RuntimeRevision > 0 && Handle.RuntimeRevision == Invocation.RuntimeRevision;
}

inline bool HasReturnedProviderIdentity(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return !Handle.ProviderId.IsEmpty() && Handle.ProviderId == Descriptor.ProviderId;
}

inline bool DoesReturnedHandleScopeMatch(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return HasReturnedRequestIdentity(Handle, Invocation)
		&& HasReturnedTransactionIdentity(Handle, Invocation)
		&& HasReturnedRevisionIdentity(Handle, Invocation)
		&& HasReturnedProviderIdentity(Handle, Descriptor);
}

inline bool DoesReturnedHandleSchemaMatch(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentPrimitiveHandleResultSchema& Schema)
{
	return Handle.Slot > 0 && Handle.Generation > 0
		&& Handle.Kind == Schema.Kind && Handle.Access == Schema.GrantedAccess;
}

inline bool DoesReturnedHandleMatch(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentPrimitiveHandleResultSchema& Schema)
{
	return DoesReturnedHandleScopeMatch(Handle, Invocation, Descriptor)
		&& DoesReturnedHandleSchemaMatch(Handle, Schema);
}

inline bool AreReturnedHandlesValid(
	const FAssetDocumentPrimitiveHandleResult& Result,
	const FAssetDocumentPrimitiveHandleResultSchema& Schema,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	TArray<FAssetDocumentOpaqueHandle>& SeenHandles)
{
	for (const FAssetDocumentOpaqueHandle& Handle : Result.Handles)
	{
		if (!DoesReturnedHandleMatch(Handle, Invocation, Descriptor, Schema)
			|| !IsUniqueReturnedHandle(Handle, SeenHandles))
		{
			return false;
		}
		SeenHandles.Add(Handle);
	}
	return true;
}

inline bool IsHandleResultNameValid(
	const FAssetDocumentPrimitiveHandleResult& Result,
	const TSet<FString>& SeenNames)
{
	return !Result.Name.IsEmpty()
		&& !SeenNames.Contains(Result.Name);
}

inline bool IsHandleResultGroupValid(
	const FAssetDocumentPrimitiveHandleResult& Result,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const TSet<FString>& SeenNames,
	TArray<FAssetDocumentOpaqueHandle>& SeenHandles)
{
	if (!IsHandleResultNameValid(Result, SeenNames))
	{
		return false;
	}
	const FAssetDocumentPrimitiveHandleResultSchema* Schema =
		FindHandleResultSchema(Descriptor.HandleResultSchemas, Result.Name);
	return Schema != nullptr
		&& IsHandleResultCountAllowed(Schema->Cardinality, Result.Handles.Num())
		&& AreReturnedHandlesValid(Result, *Schema, Invocation, Descriptor, SeenHandles);
}

inline bool AreRequiredHandleResultsPresent(
	TConstArrayView<FAssetDocumentPrimitiveHandleResultSchema> Schemas,
	const TSet<FString>& SeenNames)
{
	for (const FAssetDocumentPrimitiveHandleResultSchema& Schema : Schemas)
	{
		if (IsRequiredHandleResultSchema(Schema) && !SeenNames.Contains(Schema.Name))
		{
			return false;
		}
	}
	return true;
}

inline bool AreHandleResultCountsBounded(
	TConstArrayView<FAssetDocumentPrimitiveHandleResult> Results)
{
	static constexpr int32 MaximumHandleResultCount = 4096;
	int32 Total = 0;
	for (const FAssetDocumentPrimitiveHandleResult& Result : Results)
	{
		if (Result.Handles.Num() > MaximumHandleResultCount - Total)
		{
			return false;
		}
		Total += Result.Handles.Num();
	}
	return Results.Num() <= MaximumHandleResultCount;
}

inline bool AreOutcomeHandleResultsValid(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	TSet<FString> SeenNames;
	TArray<FAssetDocumentOpaqueHandle> SeenHandles;
	for (const FAssetDocumentPrimitiveHandleResult& Result : Outcome.HandleResults)
	{
		if (!IsHandleResultGroupValid(
			Result, Invocation, Descriptor, SeenNames, SeenHandles))
		{
			return false;
		}
		SeenNames.Add(Result.Name);
	}
	return AreRequiredHandleResultsPresent(Descriptor.HandleResultSchemas, SeenNames);
}
} // namespace UE::AssetDocument::PrimitiveAdmission::Private

inline FAssetDocumentContractValidationResult ValidateAssetDocumentPrimitiveDescriptor(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> ExistingDescriptors = {})
{
	using namespace UE::AssetDocument::PrimitiveAdmission::Private;
	const FAssetDocumentContractValidationResult CoreResult = ValidateDescriptorCore(Descriptor);
	if (!CoreResult.bValid)
	{
		return CoreResult;
	}
	return ValidateDescriptorDuplicates(Descriptor, ExistingDescriptors);
}

inline FAssetDocumentContractValidationResult ValidateAssetDocumentPrimitiveInvocation(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	using namespace UE::AssetDocument::PrimitiveAdmission::Private;
	if (!ValidateAssetDocumentPrimitiveDescriptor(Descriptor).bValid || !IsInvocationIdentityValid(Invocation, Descriptor))
	{
		return MakeValidationResult(false, TEXT("PrimitiveInvocation.InvalidIdentity"), TEXT("Invocation identity, phase, transaction, or descriptor is invalid."));
	}
	if (!AreValueArgumentsValid(Invocation))
	{
		return MakeValidationResult(false, TEXT("PrimitiveInvocation.InvalidValueArguments"), TEXT("Value arguments must be a valid T01 object value."));
	}
	if (!AreInvocationHandlesValid(Invocation, Descriptor))
	{
		return MakeValidationResult(false, TEXT("PrimitiveInvocation.InvalidHandleArguments"), TEXT("Typed handle arguments do not match the descriptor schema."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

namespace UE::AssetDocument::PrimitiveAdmission::Private
{
inline FAssetDocumentContractValidationResult ValidateOutcomeEnvelope(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	if (!ValidateAssetDocumentPrimitiveInvocation(Invocation, Descriptor).bValid)
	{
		return MakeValidationResult(false, TEXT("PrimitiveOutcome.InvalidIdentity"), TEXT("Outcome invocation or descriptor identity is invalid."));
	}
	if (!IsValidOutcomeStatus(Outcome.Status))
	{
		return MakeValidationResult(false, TEXT("PrimitiveOutcome.InvalidStatus"), TEXT("Outcome status is outside the frozen enum."));
	}
	if (!IsOutcomeValueValid(Outcome))
	{
		return MakeValidationResult(false, TEXT("PrimitiveOutcome.InvalidValue"), TEXT("Outcome value must be a valid T01 object value."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidateNonSucceededOutcome(
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	if (Outcome.HandleResults.IsEmpty())
	{
		return MakeValidationResult(true, TEXT(""), TEXT(""));
	}
	return MakeValidationResult(false, TEXT("PrimitiveOutcome.HandleLeakOnFailure"), TEXT("Failed or unavailable outcomes cannot return handles."));
}

inline FAssetDocumentContractValidationResult ValidateSucceededOutcome(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	if (!AreHandleResultCountsBounded(Outcome.HandleResults))
	{
		return MakeValidationResult(false, TEXT("PrimitiveOutcome.HandleResultLimitExceeded"), TEXT("Outcome handle results exceed the contract bound."));
	}
	if (!AreOutcomeHandleResultsValid(Invocation, Descriptor, Outcome))
	{
		return MakeValidationResult(false, TEXT("PrimitiveOutcome.InvalidHandleResults"), TEXT("Named handle results do not match the descriptor or invocation."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidateOutcomeHandles(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		return ValidateNonSucceededOutcome(Outcome);
	}
	return ValidateSucceededOutcome(Invocation, Descriptor, Outcome);
}
} // namespace UE::AssetDocument::PrimitiveAdmission::Private

inline FAssetDocumentContractValidationResult ValidateAssetDocumentPrimitiveOutcome(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	using namespace UE::AssetDocument::PrimitiveAdmission::Private;
	const FAssetDocumentContractValidationResult Envelope =
		ValidateOutcomeEnvelope(Invocation, Descriptor, Outcome);
	if (!Envelope.bValid)
	{
		return Envelope;
	}
	return ValidateOutcomeHandles(Invocation, Descriptor, Outcome);
}
