// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentProfile.h"

class FUBlueprintGraphRegionAdapter
{
public:
	FAssetDocumentCapabilityResult ValidateRegions(
		const FAssetDocumentCapabilityContext& Context,
		const TSharedRef<FJsonObject>& BodyObject,
		bool bIncludeDelegateSignatureGraphs = false) const;

	FAssetDocumentCapabilityResult ExtractRegions(
		const FAssetDocumentCapabilityContext& Context,
		TSharedRef<FJsonObject>& OutBodyJson,
		bool bIncludeDelegateSignatureGraphs = false) const;

	FAssetDocumentCapabilityResult DiffRegions(
		const FAssetDocumentCapabilityContext& Context,
		const TSharedRef<FJsonObject>& DesiredBody,
		TArray<TSharedPtr<FJsonValue>>& OutDiffEntries,
		bool bIncludeDelegateSignatureGraphs = false) const;
};

FAssetDocumentCapabilityResult ApplyUBlueprintGraphRegions(
	FAssetDocumentCapabilityContext& Context,
	const TSharedRef<FJsonObject>& DesiredBody,
	bool& bOutChanged,
	bool bIncludeDelegateSignatureGraphs = false);
