// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentRegion.h"

#include "CoreMinimal.h"

struct FAssetDocumentRegionRuntime
{
	static FAssetDocumentCapabilityResult Validate(
		const FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue,
		const IAssetDocumentRegionAdapter& Adapter);

	static FAssetDocumentCapabilityResult Preflight(
		FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue,
		const IAssetDocumentRegionAdapter& Adapter);

	static FAssetDocumentCapabilityResult Apply(
		FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue,
		IAssetDocumentRegionAdapter& Adapter,
		bool& bOutChanged);

	static FAssetDocumentCapabilityResult Extract(
		const FAssetDocumentRegionContext& Context,
		const IAssetDocumentRegionAdapter& Adapter,
		TSharedPtr<FJsonValue>& OutCurrentValue);

	static FAssetDocumentCapabilityResult Diff(
		const FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue,
		const IAssetDocumentRegionAdapter& Adapter,
		TArray<TSharedPtr<FJsonValue>>& OutDiffEntries);

	static FAssetDocumentCapabilityResult DiffByCanonicalExtract(
		const FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue,
		const IAssetDocumentRegionAdapter& Adapter,
		TArray<TSharedPtr<FJsonValue>>& OutDiffEntries);
};
