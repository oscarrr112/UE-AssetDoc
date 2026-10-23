// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentRegion.h"

#include "CoreMinimal.h"

struct FAssetDocumentGraphRegionWrapperConfig
{
	FName AdapterName;
	FString RegionId;
	FString BodyPath;
	FString JsonPointer = TEXT("/Body");
	FString SchemaLabel;
};

struct FAssetDocumentGraphRegionWrapperHooks
{
	TFunction<FAssetDocumentCapabilityResult(
		const FAssetDocumentCapabilityContext&,
		const TSharedRef<FJsonObject>&)> Validate;

	TFunction<FAssetDocumentCapabilityResult(
		FAssetDocumentCapabilityContext&,
		const TSharedRef<FJsonObject>&)> Preflight;

	TFunction<FAssetDocumentCapabilityResult(
		FAssetDocumentCapabilityContext&,
		const TSharedRef<FJsonObject>&,
		bool&)> Apply;

	TFunction<FAssetDocumentCapabilityResult(
		const FAssetDocumentCapabilityContext&,
		TSharedRef<FJsonObject>&)> Extract;

	TFunction<FAssetDocumentCapabilityResult(
		const FAssetDocumentCapabilityContext&,
		const TSharedRef<FJsonObject>&,
		TArray<TSharedPtr<FJsonValue>>&)> Diff;
};

class FAssetDocumentGraphRegionWrapperAdapter final : public IAssetDocumentRegionAdapter
{
public:
	FAssetDocumentGraphRegionWrapperAdapter(
		FAssetDocumentGraphRegionWrapperConfig InConfig,
		FAssetDocumentGraphRegionWrapperHooks InHooks);

	virtual FName GetName() const override;
	virtual bool SupportsRegion(const FAssetDocumentRegionContext& Context) const override;
	virtual TSharedRef<FJsonObject> GetSchemaHint(const FAssetDocumentRegionContext& Context) const override;

	virtual FAssetDocumentCapabilityResult ValidateRegion(
		const FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue) const override;

	virtual FAssetDocumentCapabilityResult PreflightRegion(
		FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue) const override;

	virtual FAssetDocumentCapabilityResult ApplyRegion(
		FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue,
		bool& bOutChanged) override;

	virtual FAssetDocumentCapabilityResult ExtractRegion(
		const FAssetDocumentRegionContext& Context,
		TSharedPtr<FJsonValue>& OutCurrentValue) const override;

	virtual FAssetDocumentCapabilityResult DiffRegion(
		const FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue,
		TArray<TSharedPtr<FJsonValue>>& OutDiffEntries) const override;

private:
	FAssetDocumentGraphRegionWrapperConfig Config;
	FAssetDocumentGraphRegionWrapperHooks Hooks;
};
