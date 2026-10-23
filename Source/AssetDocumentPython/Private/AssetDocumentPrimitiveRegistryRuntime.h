// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveRegistry.h"

struct FAssetDocumentPrimitiveRegistryBuildInput
{
	uint64 RuntimeRevision = 0;
	TArray<FAssetDocumentPrimitiveRequirement> Required;
	TArray<TSharedRef<IAssetDocumentPrimitiveProvider>> Providers;
	TArray<FAssetDocumentAuthorityCandidateRecord> AuthorityCatalog;
};

struct FAssetDocumentPrimitiveRegistryBuildResult
{
	TSharedPtr<class FAssetDocumentPrimitiveRegistryRuntime> Registry;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

class FAssetDocumentPrimitiveRegistryRuntime final : public IAssetDocumentPrimitiveRegistry
{
public:
	static FAssetDocumentPrimitiveRegistryBuildResult CreateFrozen(
		const FAssetDocumentPrimitiveRegistryBuildInput& Input);
	FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const override;
	FAssetDocumentPrimitiveResolution Resolve(
		const FAssetDocumentPrimitiveRequirement& Requirement) const override;
	TSharedPtr<IAssetDocumentPrimitiveProvider> FindProvider(FStringView ProviderId) const;

private:
	uint64 RuntimeRevision = 0;
	TArray<FAssetDocumentPrimitiveDescriptor> Descriptors;
	TArray<FAssetDocumentProviderAvailability> ProviderAvailability;
	TMap<FString, TSharedRef<IAssetDocumentPrimitiveProvider>> ProvidersById;
};
