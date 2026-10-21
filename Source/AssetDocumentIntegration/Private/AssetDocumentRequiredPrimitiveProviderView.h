// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Runtime/AssetDocumentPrimitiveRegistry.h"
#include "Runtime/AssetDocumentMutationPreparation.h"

using FAssetDocumentPrepareMutationBatch = TFunction<FAssetDocumentContractValidationResult(
 const FAssetDocumentRuntimeRequestDto&, TConstArrayView<FAssetDocumentMutationIntent>, FAssetDocumentPreparedMutationBatch&)>;
using FAssetDocumentPreparedInvocation = TFunction<FAssetDocumentPrimitiveOutcome(
 const FString&, const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&,
 IAssetDocumentAtomicTransaction*, TFunctionRef<FAssetDocumentPrimitiveOutcome()>)>;

// Construction input bound to an exact provider instance, never an id-to-callback registry.
struct FAssetDocumentMutationPreparationBinding
{
 TSharedRef<IAssetDocumentPrimitiveProvider> Provider;
 FAssetDocumentPrepareMutationBatch Prepare;
};

// Published only after the entire selected consumer closure validates.
struct FAssetDocumentRequiredPrimitiveProjection
{
	TArray<TSharedRef<IAssetDocumentPrimitiveProvider>> Providers;
	TArray<FAssetDocumentPrimitiveDescriptor> Descriptors;
	TArray<FAssetDocumentPrimitiveRequirement> Requirements;
	TArray<FAssetDocumentProviderAvailability> Availability;
 TArray<FAssetDocumentMutationPreparationBinding> PreparationViews;
};

struct FAssetDocumentRequiredPrimitiveProjectionResult
{
	TSharedPtr<const FAssetDocumentRequiredPrimitiveProjection> Projection;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

// Seed is the declared (primitive, version, route) set, normally derived from the
// Python revision. Every selected descriptor's RequiredConsumerIds becomes the sorted
// set of seed routes that declare it, and Requirements is exactly the seed. A seed
// row no provider describes yields PrimitiveRegistry.MissingRequiredPrimitive.
FAssetDocumentRequiredPrimitiveProjectionResult BuildAssetDocumentRequiredPrimitiveProjection(
	TConstArrayView<TSharedRef<IAssetDocumentPrimitiveProvider>> Providers,
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Seed,
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> AuthorityCatalog,
 TConstArrayView<FAssetDocumentMutationPreparationBinding> PreparationBindings = {},
 FAssetDocumentPreparedInvocation InvokePrepared = {});
