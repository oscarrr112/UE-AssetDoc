// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Provider/AssetDocumentAnimationPrimitiveCatalog.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

FAssetDocumentContractValidationResult ValidateAnimationDecisionClosure(
	TConstArrayView<FAssetDocumentAnimationDecision> Decisions);

FAssetDocumentContractValidationResult ValidateAnimationDescriptorBindingBijection(
	TConstArrayView<FAssetDocumentAnimationDecision> Decisions,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	TConstArrayView<FAssetDocumentAnimationPrimitiveBinding> Bindings);

ASSETDOCUMENTANIMATION_API FAssetDocumentContractValidationResult ValidateAnimationAdmissionEvidence(
	const FAssetDocumentPrimitiveDescriptor& Descriptor);

FAssetDocumentPrimitiveOutcome MakeAnimationProviderFailure(
	FStringView Code,
	FStringView Message);

FAssetDocumentVerifiedLeaseResult AcquireAnimationInputLease(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FStringView ArgumentName,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access);

TArray<FAssetDocumentMutationFootprint> BuildAnimationMutationFootprint(
	FStringView PrimitiveId,
	FStringView TargetStableId,
	FStringView PackageStableId,
	TConstArrayView<FString> InvalidatedStableIds);

FAssetDocumentPrimitiveOutcome FinalizeAnimationPrimitiveOutcome(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FAssetDocumentPrimitiveOutcome&& Outcome);
