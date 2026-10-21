// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentRegion.h"

#include "CoreMinimal.h"

class FAssetDocumentDeferredRegionAdapter final : public IAssetDocumentRegionAdapter
{
public:
	explicit FAssetDocumentDeferredRegionAdapter(
		FName InName = DefaultAdapterName(),
		FString InUnsupportedRegionCode = TEXT("UnsupportedRegion"),
		FString InUnsupportedRegionMessage = TEXT(""));

	static FName DefaultAdapterName();
	static bool FindDeclaredPolicyForBodyKey(
		const TArray<FAssetDocumentRegionPolicy>& RegionPolicies,
		const FString& BodyKey,
		FAssetDocumentRegionPolicy& OutPolicy);
	static FAssetDocumentRegionContext MakeContextFromDeclaredPolicy(
		const FAssetDocumentCapabilityContext& CapabilityContext,
		const FString& BodyKey,
		const FAssetDocumentRegionPolicy& Policy);

	virtual FName GetName() const override;
	virtual bool SupportsRegion(const FAssetDocumentRegionContext& Context) const override;
	virtual TSharedRef<FJsonObject> GetSchemaHint(const FAssetDocumentRegionContext& Context) const override;

	virtual FAssetDocumentCapabilityResult ValidateRegion(
		const FAssetDocumentRegionContext& Context,
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
	FName Name;
	FString UnsupportedRegionCode;
	FString UnsupportedRegionMessage;
};
