// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentPythonRuntime.h"
#include "Runtime/AssetDocumentPersistenceCoordinator.h"

class FAssetDocumentAnimationRequestBinding;
class FAssetDocumentStagedMontageReadBinding;
class FAssetDocumentStagedBehaviorTreeReadBinding;
class UPackage;

class FAssetDocumentGenericPersistenceVerifier final
	: public IAssetDocumentStagedArtifactVerifier
{
public:
	FAssetDocumentGenericPersistenceVerifier(
		IAssetDocumentPythonRuntime& InRuntime,
		FAssetDocumentRuntimeRequestDto InRequest,
  TSharedPtr<FAssetDocumentAnimationRequestBinding> InAnimation = nullptr,
  TSharedPtr<FAssetDocumentStagedMontageReadBinding> InStagedRead = nullptr,
  TSharedPtr<FAssetDocumentStagedBehaviorTreeReadBinding> InBehaviorTreeStagedRead = nullptr);

	FAssetDocumentFreshVerificationResult VerifyFresh(
		const FAssetDocumentFreshVerificationInput& Input) override;

private:
	IAssetDocumentPythonRuntime& Runtime;
	FAssetDocumentRuntimeRequestDto Request;
	TSharedPtr<FAssetDocumentAnimationRequestBinding> Animation;
	TSharedPtr<FAssetDocumentStagedMontageReadBinding> StagedRead;
 TSharedPtr<FAssetDocumentStagedBehaviorTreeReadBinding> BehaviorTreeStagedRead;
};
