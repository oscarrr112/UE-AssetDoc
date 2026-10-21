// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentAnimationRequestBinding.h"

namespace
{
FAssetDocumentContractValidationResult RejectRequest()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = false;
	auto& D = Result.Diagnostics.AddDefaulted_GetRef();
	D.Path = TEXT("/GenericRuntime/Animation");
	D.Code = TEXT("GenericRuntime.AnimationRequestBindingMismatch");
	D.Message = TEXT("Animation requires one active provider bound to the exact Service request and runtime revision.");
	return Result;
}
}

FAssetDocumentAnimationRequestBinding::FAssetDocumentAnimationRequestBinding(FFactory InFactory)
	: Factory(MoveTemp(InFactory))
{
	const auto Catalog = Factory();
	ProviderId = Catalog->GetProviderId();
	Descriptors.Append(Catalog->DescribePrimitives());
	Availability = Catalog->InspectAvailability();
}
FString FAssetDocumentAnimationRequestBinding::GetProviderId() const { return ProviderId; }
TConstArrayView<FAssetDocumentPrimitiveDescriptor> FAssetDocumentAnimationRequestBinding::DescribePrimitives() const { return Descriptors; }
FAssetDocumentProviderAvailability FAssetDocumentAnimationRequestBinding::InspectAvailability() const { return Availability; }

FAssetDocumentContractValidationResult FAssetDocumentAnimationRequestBinding::BindRequest(const FAssetDocumentRuntimeRequestDto& Request)
{
	if (Active || !Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0) return RejectRequest();
	Active = Factory();
	const auto Bound = Active->BindRequest(Request);
	if (!Bound.bValid) { Active->CloseRequest(); Active.Reset(); return Bound; }
	RequestId = Request.RequestId;
	Revision = Request.ExpectedRuntimeRevision;
	BoundTargetPath = Request.TargetAssetPath;
	BoundAssetClassPath = Request.RequestedAssetClassPath;
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}
void FAssetDocumentAnimationRequestBinding::CloseRequest()
{
	if (Active) Active->CloseRequest();
	Active.Reset();
	RequestId.Invalidate();
	Revision = 0;
	StagedRevision = 0;
	BoundTargetPath.Reset();
	BoundAssetClassPath.Reset();
}
TUniquePtr<FAssetDocumentAnimationStagedReadLease> FAssetDocumentAnimationRequestBinding::BindStagedRead(
	UAnimMontage& Target, const FString& Path, uint64 FreshRevision, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	if (!Active || StagedRevision != 0 || FreshRevision == 0)
	{ Diagnostics.Append(RejectRequest().Diagnostics); return {}; }
	auto Lease = Active->BindStagedRead(Target, Path, FreshRevision, Diagnostics);
	if (Lease) StagedRevision = FreshRevision;
	return Lease;
}
FAssetDocumentContractValidationResult FAssetDocumentAnimationRequestBinding::Prepare(
	const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
	FAssetDocumentPreparedMutationBatch& Batch)
{
	if (!Active || Request.RequestId != RequestId || Request.ExpectedRuntimeRevision != Revision) return RejectRequest();
	return Active->Prepare(Request, Intents, Batch);
}
FAssetDocumentContractValidationResult FAssetDocumentAnimationRequestBinding::PrepareSequenceCompressionDocument(
	const FAssetDocumentRuntimeRequestDto& Request, const FAssetDocumentRuntimeValueDto& DocumentInputs,
	int32 StepIndex, EAssetDocumentRuntimeRequestPhase Phase, TConstArrayView<FAssetDocumentMutationIntent> OtherIntents,
	TArray<FAssetDocumentMutationIntent>& ActualIntents, FAssetDocumentPreparedMutationBatch& Batch)
{
	ActualIntents.Reset(); Batch = {};
	if (!Active || StagedRevision != 0 || Request.RequestId != RequestId || Request.ExpectedRuntimeRevision != Revision) return RejectRequest();
	return PrepareAssetDocumentAnimationSequenceCompressionDocumentBatch(
		Request, *Active, DocumentInputs, StepIndex, Phase, OtherIntents, ActualIntents, Batch);
}
FAssetDocumentContractValidationResult FAssetDocumentAnimationRequestBinding::PrepareSequenceReferencesDocument(
	const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentNativeRequestContext& ReadonlyContext,
	const FGuid& ReadonlyTransaction, const FAssetDocumentRuntimeValueDto& DocumentInputs,
	int32 NameStepIndex, int32 AssetStepIndex, EAssetDocumentRuntimeRequestPhase Phase,
	TConstArrayView<FAssetDocumentMutationIntent> OtherIntents,
	TArray<FAssetDocumentMutationIntent>& ActualIntents, FAssetDocumentPreparedMutationBatch& Batch)
{
	ActualIntents.Reset(); Batch = {};
	if (!Active || StagedRevision != 0 || Request.RequestId != RequestId || Request.ExpectedRuntimeRevision != Revision
		|| Request.TargetAssetPath != BoundTargetPath || Request.RequestedAssetClassPath != BoundAssetClassPath) return RejectRequest();
	return PrepareAssetDocumentAnimationSequenceReferencesDocumentBatch(
		Request, *Active, ReadonlyContext, ReadonlyTransaction, DocumentInputs, NameStepIndex, AssetStepIndex,
		Phase, OtherIntents, ActualIntents, Batch);
}
FAssetDocumentPrimitiveOutcome FAssetDocumentAnimationRequestBinding::Invoke(
	const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* Transaction)
{
	const bool bStagedSnapshot = StagedRevision != 0
		&& Invocation.PrimitiveId == TEXT("animation.montage.profile.snapshot.v1")
		&& Invocation.Phase == EAssetDocumentRuntimeRequestPhase::Verify && Transaction == nullptr;
	if (!Active || Invocation.RequestId != RequestId
		|| Invocation.RuntimeRevision != (bStagedSnapshot ? StagedRevision : Revision)
		|| (StagedRevision != 0 && !bStagedSnapshot))
	{
		FAssetDocumentPrimitiveOutcome Outcome;
		Outcome.Diagnostics = RejectRequest().Diagnostics;
		return Outcome;
	}
	return Active->Invoke(Invocation, Handles, Transaction);
}
