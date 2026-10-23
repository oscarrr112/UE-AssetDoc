// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentBlackboardFootprintProjection.h"
#include "AssetDocumentCoreRuntimeComposition.h"
#include "AssetDocumentTypes.h"
#include "AssetDocumentRuntimeTargetActivation.h"
#include "AssetDocumentMutationComposition.h"
#include "AssetDocumentRequiredPrimitiveProviderView.h"
#include "../../AssetDocument/Private/Runtime/AssetDocumentRequestCoordinatorRuntime.h"

struct FAssetDocumentGenericMutationPreparationResult
{
	TUniquePtr<FAssetDocumentCoreMutationInputs> Mutation;
	TOptional<FAssetDocumentPreparedTargetDecision> TargetDecision;
 TSharedPtr<FAssetDocumentMutationComposition> MutationComposition;
	TOptional<FAssetDocumentBlackboardFootprintProjection> BlackboardProjection;
	TArray<FAssetDocumentMutationFootprint> OptionalBlackboardCreatePrefix;

	explicit operator bool() const
	{
		return Mutation.IsValid();
	}
};

class FAssetDocumentGenericMutationPreparation final
{
public:
	static FAssetDocumentGenericMutationPreparationResult Prepare(
		const FAssetDocumentApplyFileRequest& ServiceRequest,
		const FAssetDocumentRuntimeRequestDto& RuntimeRequest,
		const FAssetDocumentRuntimeResultDto& Validated,
		const FAssetDocumentRuntimeResultDto& Preflight,
		IAssetDocumentStagedArtifactVerifier& Verifier,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
		const FAssetDocumentRuntimeTargetActivation* TargetActivation = nullptr,
  const IAssetDocumentPrimitiveRegistry* Registry = nullptr,
  const FAssetDocumentRequiredPrimitiveProjection* ProviderProjection = nullptr);
};
