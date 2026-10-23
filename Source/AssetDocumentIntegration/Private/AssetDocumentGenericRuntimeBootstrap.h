// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Runtime/AssetDocumentPrimitiveRegistry.h"

class FAssetDocumentGenericRuntimeBootstrap final
{
public:
	FAssetDocumentGenericRuntimeBootstrap();
	~FAssetDocumentGenericRuntimeBootstrap();

	bool Activate();
	void Deactivate();
	bool IsActive() const;
	bool IsWaitingForPython() const;
	int32 GetActiveRuntimeCount() const;
	int32 GetProviderCount() const;
	int32 GetCoreCompositionCount() const;
	int32 GetRouteBindingCount() const;
	// Required set declared by the last activated Python revision (empty before the
	// first activation; retained across Deactivate()).
	TArray<FAssetDocumentPrimitiveRequirement> GetDeclaredRequirements() const;

private:
	bool InitializeRuntimeBinding();
	bool ActivateFrozenRevision();
	bool ComposeAndBindRoute();

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
