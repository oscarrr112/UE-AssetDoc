// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentProfile.h"

class FUBlueprintAssetDocumentCapability final : public IAssetDocumentCapability
{
public:
	static const TArray<FName>& GetCanonicalBodyKeys();

	virtual FName GetName() const override;
	virtual TArray<FName> GetInternalAdapterNames() const override;
	virtual int32 GetApplyOrder() const override;
	virtual bool SupportsAsset(const UObject* Asset) const override;
	virtual bool SupportsClass(const UClass* AssetClass) const override;
	virtual TSharedRef<FJsonObject> GetSchemaHint() const override;
	virtual FAssetDocumentCapabilityResult Validate(const FAssetDocumentCapabilityContext& Context, const TSharedRef<FJsonValue>& BodyJson) const override;
	virtual FAssetDocumentCapabilityResult Preflight(FAssetDocumentCapabilityContext& Context, const TSharedRef<FJsonValue>& BodyJson) const override;
	virtual FAssetDocumentCapabilityResult Apply(FAssetDocumentCapabilityContext& Context, const TSharedRef<FJsonValue>& BodyJson) override;
	virtual FAssetDocumentCapabilityResult Extract(const FAssetDocumentCapabilityContext& Context, TSharedRef<FJsonObject>& OutBodyJson) const override;
	virtual FAssetDocumentCapabilityResult Diff(const FAssetDocumentCapabilityContext& Context, const TSharedRef<FJsonValue>& DesiredJson, TArray<TSharedPtr<FJsonValue>>& OutDiffEntries) const override;

private:
	FAssetDocumentCapabilityResult ValidateBodyObject(const FAssetDocumentCapabilityContext& Context, const TSharedRef<FJsonObject>& BodyObject) const;
};
