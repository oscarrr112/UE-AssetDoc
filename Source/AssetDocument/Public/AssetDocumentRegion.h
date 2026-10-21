// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentProfile.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct ASSETDOCUMENT_API FAssetDocumentRegionContext
{
	UObject* Asset = nullptr;
	UClass* AssetClass = nullptr;
	FString TargetAssetPath;
	FString SourceDocumentPath;
	const TSharedPtr<FJsonObject>* Definitions = nullptr;
	/** Whole desired Body for operations whose region semantics depend on sibling regions. */
	TSharedPtr<FJsonObject> DesiredBody;
	FAssetDocumentResult* Result = nullptr;
	bool bIsDryRun = false;

	const FAssetDocumentRegionPolicy* Policy = nullptr;
	FName RegionId;
	FString BodyPath;
	FString JsonPointer;

	FString GetJsonPointerPath() const
	{
		return JsonPointer.IsEmpty() ? BodyPath : JsonPointer;
	}
};

struct ASSETDOCUMENT_API FAssetDocumentRegionBinding
{
	FName BodyKey;
	FName RegionId;
	FName AdapterName;
	int32 ApplyOrder = 0;
	bool bRequired = false;
};

class ASSETDOCUMENT_API IAssetDocumentRegionAdapter
{
public:
	virtual ~IAssetDocumentRegionAdapter() = default;

	virtual FName GetName() const = 0;
	virtual bool SupportsRegion(const FAssetDocumentRegionContext& Context) const = 0;
	virtual TSharedRef<FJsonObject> GetSchemaHint(const FAssetDocumentRegionContext& Context) const = 0;

	virtual FAssetDocumentCapabilityResult ValidateRegion(
		const FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue) const = 0;

	virtual FAssetDocumentCapabilityResult PreflightRegion(
		FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue) const
	{
		return ValidateRegion(Context, DesiredValue);
	}

	virtual FAssetDocumentCapabilityResult ApplyRegion(
		FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue,
		bool& bOutChanged) = 0;

	virtual FAssetDocumentCapabilityResult ExtractRegion(
		const FAssetDocumentRegionContext& Context,
		TSharedPtr<FJsonValue>& OutCurrentValue) const = 0;

	virtual FAssetDocumentCapabilityResult DiffRegion(
		const FAssetDocumentRegionContext& Context,
		const TSharedPtr<FJsonValue>& DesiredValue,
		TArray<TSharedPtr<FJsonValue>>& OutDiffEntries) const = 0;
};
