// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonOperations.h"
#include "Provider/Asset/AssetDocumentAnimationPreviewMeshOperations.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceAdditiveCompletion.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceAsset.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceName.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompression.h"
#include "Provider/Montage/MontageStructuralProvider.h"
#include "Provider/Montage/MontageProfileSnapshot.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveOperations.h"

namespace AssetDocumentAnimationProviderCommon
{
FAssetDocumentContractValidationResult MakeValidationResult(
	bool bValid,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!bValid)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/Primitive");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

bool IsDecisionRowValid(
	const FAssetDocumentAnimationDecision& Decision)
{
	const bool bVersionTwo = Decision.PrimitiveId == TEXT("animation.curve.float.enumerate.v2")
		|| Decision.PrimitiveId == TEXT("animation.curve.float.replace.v2")
		|| Decision.PrimitiveId == TEXT("animation.sequence.complete_edit.v2")
        || Decision.PrimitiveId == TEXT("animation.sequence.retarget_source_asset.set.v2");
	if (Decision.PrimitiveId.IsEmpty() || Decision.Version != (bVersionTwo ? 2u : 1u)
		|| Decision.ConsumerId.IsEmpty() || Decision.AuthorityGateId.IsEmpty())
	{
		return false;
	}
	switch (Decision.Disposition)
	{
	case EAssetDocumentAnimationDecisionDisposition::Admitted:
		return Decision.ConsumerId == TEXT("animation_profile")
			&& Decision.StableBlockerDiagnostic.IsEmpty();
	case EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker:
		return Decision.ConsumerId == TEXT("animation_profile")
			&& !Decision.StableBlockerDiagnostic.IsEmpty();
	case EAssetDocumentAnimationDecisionDisposition::FrozenG1StableBlocker:
		return Decision.ConsumerId == TEXT("animation_provider_design")
			&& Decision.AuthorityGateId == TEXT("G1")
			&& !Decision.StableBlockerDiagnostic.IsEmpty();
	default:
		return false;
	}
}

bool HasExactDecisionCounts(
	int32 Admitted,
	int32 OrdinaryBlockers,
	int32 FrozenG1Blockers)
{
	return Admitted == 14
		&& OrdinaryBlockers == 16
		&& FrozenG1Blockers == 6;
}

bool IsReadOnlyAdmissionShape(
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return Descriptor.ProviderId == TEXT("animation")
		&& Descriptor.Owner == EAssetDocumentPrimitiveOwner::Animation
		&& Descriptor.Access == EAssetDocumentPrimitiveAccess::ReadOnly
		&& !Descriptor.bRequiresActiveTransaction
		&& Descriptor.MutationFootprintSchemaId.IsEmpty()
		&& Descriptor.AllowedPhases ==
			TArray<EAssetDocumentRuntimeRequestPhase>{
				EAssetDocumentRuntimeRequestPhase::Verify}
		&& Descriptor.RequiredConsumerIds ==
			TArray<FString>{TEXT("animation_profile")};
}

bool IsCurveSnapshotAdmissionShape(const FAssetDocumentPrimitiveDescriptor& D)
{
 return IsReadOnlyAdmissionShape(D) && D.Version == 2
  && D.ExecutionThread == EAssetDocumentExecutionThread::GameThread
  && D.AllowedHandleKinds == TArray<EAssetDocumentHandleKind>{EAssetDocumentHandleKind::Asset}
  && D.HandleResultSchemas.IsEmpty()
  && D.InputSchemaId == TEXT("animation.curve.float.enumerate.input.v2")
  && D.OutputSchemaId == TEXT("animation.curve.float.enumerate.output.v2")
  && D.Admission.AuthorityPath == EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper
  && D.Admission.Disposition == EAssetDocumentPrimitiveDisposition::OrdinaryHelper
  && D.Admission.PublicModule == TEXT("Engine")
  && D.Admission.PublicHeader == TEXT("Animation/AnimData/IAnimationDataModel.h;Animation/AnimSequenceBase.h")
  && D.Admission.ExportedAuthoritativeApi == TEXT("UAnimSequenceBase::GetDataModel;IAnimationDataModel::GetFloatCurves")
  && D.Admission.AuthorityFingerprint == TEXT("ue-5.7.4|animation|Engine|Animation/AnimData/IAnimationDataModel.h;Animation/AnimSequenceBase.h|UAnimSequenceBase::GetDataModel;IAnimationDataModel::GetFloatCurves|animation.curve.float.enumerate.v2");
}

bool HasExactAssetArgument(
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	if (Descriptor.HandleArgumentSchemas.Num() != 1)
	{
		return false;
	}
	const FAssetDocumentPrimitiveHandleArgumentSchema& Asset =
		Descriptor.HandleArgumentSchemas[0];
	return Asset.Name == TEXT("asset")
		&& Asset.Kind == EAssetDocumentHandleKind::Asset
		&& Asset.RequiredAccess == EAssetDocumentHandleAccess::ReadOnly
		&& Asset.bRequired;
}

int32 CountDecisionDescriptors(
	const FAssetDocumentAnimationDecision& Decision,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors)
{
	int32 Count = 0;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		Count += Descriptor.Version == Decision.Version
			&& Descriptor.PrimitiveId == Decision.PrimitiveId;
	}
	return Count;
}

int32 CountDecisionBindings(
	const FAssetDocumentAnimationDecision& Decision,
	TConstArrayView<FAssetDocumentAnimationPrimitiveBinding> Bindings)
{
	int32 Count = 0;
	for (const FAssetDocumentAnimationPrimitiveBinding& Binding : Bindings)
	{
		Count += Binding.Handler != nullptr
			&& Binding.Version == Decision.Version
			&& Binding.PrimitiveId == Decision.PrimitiveId;
	}
	return Count;
}

bool HasExactCallableProjection(
	const FAssetDocumentAnimationDecision& Decision,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	TConstArrayView<FAssetDocumentAnimationPrimitiveBinding> Bindings)
{
	const int32 ExpectedMatches = Decision.Disposition ==
		EAssetDocumentAnimationDecisionDisposition::Admitted ? 1 : 0;
	return CountDecisionDescriptors(Decision, Descriptors) == ExpectedMatches
		&& CountDecisionBindings(Decision, Bindings) == ExpectedMatches;
}

const FAssetDocumentPrimitiveHandleArgument* FindInputArgument(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FStringView ArgumentName)
{
	for (const FAssetDocumentPrimitiveHandleArgument& Argument :
		Invocation.HandleArguments)
	{
		if (Argument.Name == ArgumentName)
		{
			return &Argument;
		}
	}
	return nullptr;
}

void AddFootprint(
	TArray<FAssetDocumentMutationFootprint>& OutFootprint,
	EAssetDocumentMutationFootprintKind Kind,
	FStringView StableId,
	const TCHAR* RollbackComponentId)
{
	if (!StableId.IsEmpty())
	{
		OutFootprint.Add({Kind, FString(StableId), RollbackComponentId});
	}
}
} // namespace AssetDocumentAnimationProviderCommon


FAssetDocumentContractValidationResult ValidateAnimationDecisionClosure(
	TConstArrayView<FAssetDocumentAnimationDecision> Decisions)
{
	using namespace AssetDocumentAnimationProviderCommon;
	TSet<FString> Identities;
	int32 Admitted = 0;
	int32 OrdinaryBlockers = 0;
	int32 FrozenG1Blockers = 0;
	for (const FAssetDocumentAnimationDecision& Decision : Decisions)
	{
		const FString Identity = FString::Printf(
			TEXT("%s|%u"), *Decision.PrimitiveId, Decision.Version);
		if (!IsDecisionRowValid(Decision) || Identities.Contains(Identity))
		{
			return MakeValidationResult(false,
				TEXT("AnimationProvider.DecisionClosureInvalid"),
				TEXT("The Animation decision catalog is incomplete or duplicated."));
		}
		Identities.Add(Identity);
		Admitted += Decision.Disposition ==
			EAssetDocumentAnimationDecisionDisposition::Admitted;
		OrdinaryBlockers += Decision.Disposition ==
			EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker;
		FrozenG1Blockers += Decision.Disposition ==
			EAssetDocumentAnimationDecisionDisposition::FrozenG1StableBlocker;
	}
	return MakeValidationResult(
		Decisions.Num() == 36
			&& HasExactDecisionCounts(
				Admitted, OrdinaryBlockers, FrozenG1Blockers),
		TEXT("AnimationProvider.DecisionClosureInvalid"),
		TEXT("The Animation decision catalog must close exactly 14/16/6 rows."));
}

FAssetDocumentContractValidationResult ValidateAnimationAdmissionEvidence(
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	using namespace AssetDocumentAnimationProviderCommon;
	const FAssetDocumentContractValidationResult RuntimeValidation =
		ValidateAssetDocumentPrimitiveDescriptor(Descriptor);
	const bool bValid = RuntimeValidation.bValid
		&& (Descriptor.PrimitiveId == TEXT("animation.asset.skeleton.set.v1")
			? IsSkeletonSetAdmissionShape(Descriptor)
            : Descriptor.PrimitiveId == TEXT("animation.curve.float.replace.v2")
            ? IsFloatCurveReplaceAdmissionShape(Descriptor)
			: Descriptor.PrimitiveId == TEXT("animation.asset.preview_mesh.set.v1")
			? IsPreviewMeshSetAdmissionShape(Descriptor)
            : Descriptor.PrimitiveId == TEXT("animation.sequence.compression.settings.set.v1")
            ? IsSequenceCompressionAdmissionShape(Descriptor)
            : Descriptor.PrimitiveId == TEXT("animation.sequence.retarget_source_name.set.v1")
            ? IsSequenceRetargetSourceNameAdmissionShape(Descriptor)
            : Descriptor.PrimitiveId == TEXT("animation.sequence.retarget_source_asset.set.v2")
            ? IsSequenceRetargetSourceAssetAdmissionShape(Descriptor)
            : Descriptor.PrimitiveId == TEXT("animation.sequence.complete_edit.v2")
            ? IsSequenceAdditiveCompletionAdmissionShape(Descriptor)
			: (Descriptor.PrimitiveId == TEXT("animation.montage.slot_tracks.replace.v1") || Descriptor.PrimitiveId == TEXT("animation.montage.sections.mutate.v1"))
            ? IsMontageStructuralAdmissionShape(Descriptor)
            : Descriptor.PrimitiveId == TEXT("animation.montage.profile.snapshot.v1")
            ? IsMontageProfileSnapshotAdmissionShape(Descriptor)
            : (Descriptor.PrimitiveId == TEXT("animation.curve.float.enumerate.v2")
				? IsCurveSnapshotAdmissionShape(Descriptor) : IsReadOnlyAdmissionShape(Descriptor))
				&& HasExactAssetArgument(Descriptor))
		&& Descriptor.Admission.ExactEngineVersion == TEXT("5.7.4")
		&& Descriptor.Admission.PhysicalOwner ==
			EAssetDocumentPrimitiveOwner::Animation
		&& Descriptor.Admission.PrimaryOperationId == Descriptor.PrimitiveId;
	return bValid ? MakeValidationResult(true, TEXT(""), TEXT(""))
		: MakeValidationResult(false,
			TEXT("AnimationProvider.AdmissionEvidenceInvalid"),
			TEXT("The Animation descriptor lacks exact admitted authority evidence."));
}

FAssetDocumentContractValidationResult ValidateAnimationDescriptorBindingBijection(
	TConstArrayView<FAssetDocumentAnimationDecision> Decisions,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	TConstArrayView<FAssetDocumentAnimationPrimitiveBinding> Bindings)
{
	using namespace AssetDocumentAnimationProviderCommon;
	if (!ValidateAnimationDecisionClosure(Decisions).bValid
		|| Descriptors.Num() != 14 || Bindings.Num() != Descriptors.Num())
	{
		return MakeValidationResult(false,
			TEXT("AnimationProvider.BindingBijectionInvalid"),
			TEXT("The Animation callable projection has invalid counts or decisions."));
	}
	for (const FAssetDocumentAnimationDecision& Decision : Decisions)
	{
		if (!HasExactCallableProjection(Decision, Descriptors, Bindings))
		{
			return MakeValidationResult(false,
				TEXT("AnimationProvider.BindingBijectionInvalid"),
				TEXT("Only admitted Animation decisions may enter callable views."));
		}
	}
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		if (!ValidateAnimationAdmissionEvidence(Descriptor).bValid)
		{
			return MakeValidationResult(false,
				TEXT("AnimationProvider.BindingBijectionInvalid"),
				TEXT("An admitted Animation descriptor lacks exact evidence."));
		}
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

FAssetDocumentPrimitiveOutcome MakeAnimationProviderFailure(
	FStringView Code,
	FStringView Message)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = FString(Code);
	Diagnostic.Message = FString(Message);
	return Outcome;
}

FAssetDocumentVerifiedLeaseResult AcquireAnimationInputLease(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FStringView ArgumentName,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access)
{
	using namespace AssetDocumentAnimationProviderCommon;
	const FAssetDocumentPrimitiveHandleArgument* Argument =
		FindInputArgument(Invocation, ArgumentName);
	if (Argument == nullptr)
	{
		FAssetDocumentVerifiedLeaseResult Result;
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/Primitive/Handles");
		Diagnostic.Code = TEXT("AnimationProvider.InvalidHandlePayload");
		Diagnostic.Message = TEXT("The required Animation handle is absent.");
		return Result;
	}
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("animation");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = Kind;
	Context.RequiredAccess = Access;
	return Handles.AcquireVerifiedLease(Argument->Handle, Context);
}

TArray<FAssetDocumentMutationFootprint> BuildAnimationMutationFootprint(
	FStringView PrimitiveId,
	FStringView TargetStableId,
	FStringView PackageStableId,
	TConstArrayView<FString> InvalidatedStableIds)
{
	using namespace AssetDocumentAnimationProviderCommon;
	TArray<FAssetDocumentMutationFootprint> Footprint;
	AddFootprint(Footprint, EAssetDocumentMutationFootprintKind::UObjectState,
		TargetStableId, TEXT("rollback.animation.uobject"));
	AddFootprint(Footprint, EAssetDocumentMutationFootprintKind::Package,
		PackageStableId, TEXT("rollback.animation.package"));
	for (const FString& StableId : InvalidatedStableIds)
	{
		AddFootprint(Footprint, EAssetDocumentMutationFootprintKind::Handle,
			StableId, TEXT("rollback.animation.handle"));
	}
	(void)PrimitiveId;
	return Footprint;
}

FAssetDocumentPrimitiveOutcome FinalizeAnimationPrimitiveOutcome(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FAssetDocumentPrimitiveOutcome&& Outcome)
{
	if (Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded
		&& Descriptor.Access == EAssetDocumentPrimitiveAccess::ReadOnly
		&& !Outcome.ChangedSet.IsEmpty())
	{
		return MakeAnimationProviderFailure(
			TEXT("AnimationProvider.ReadbackMismatch"),
			TEXT("A read-only Animation primitive reported a changed set."));
	}
	if (!ValidateAssetDocumentPrimitiveOutcome(
		Invocation, Descriptor, Outcome).bValid)
	{
		return MakeAnimationProviderFailure(
			TEXT("AnimationProvider.OperationRejected"),
			TEXT("The Animation handler returned an invalid outcome."));
	}
	return MoveTemp(Outcome);
}
