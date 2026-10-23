// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

class FAssetDocumentHostTestProvider final : public IAssetDocumentPrimitiveProvider
{
public:
	FAssetDocumentHostTestProvider(
		FString InProviderId,
		TArray<FAssetDocumentPrimitiveDescriptor> InDescriptors,
		EAssetDocumentProviderAvailabilityState InState = EAssetDocumentProviderAvailabilityState::Available);

	FString GetProviderId() const override;
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override;
	FAssetDocumentProviderAvailability InspectAvailability() const override;
	FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override;

private:
	FString ProviderId;
	TArray<FAssetDocumentPrimitiveDescriptor> Descriptors;
	EAssetDocumentProviderAvailabilityState State;
};

FAssetDocumentPrimitiveDescriptor MakeAssetDocumentHostTestDescriptor(
	FString PrimitiveId = TEXT("editor.host_test.echo"),
	uint32 Version = 1,
	FString AuthorityFingerprint = TEXT("host-test-echo-v1"));

FAssetDocumentPrimitiveRequirement MakeAssetDocumentHostTestRequirement(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FString RouteId = TEXT("host.test.route"));
