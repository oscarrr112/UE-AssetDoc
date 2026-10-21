// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentRegionRuntime.h"

#include "AssetDocumentJsonRegionUtils.h"

namespace
{
FAssetDocumentCapabilityResult CheckAdapterSupport(
	const FAssetDocumentRegionContext& Context,
	const IAssetDocumentRegionAdapter& Adapter)
{
	if (!Adapter.SupportsRegion(Context))
	{
		return FAssetDocumentJsonRegionUtils::Failure(
			Context.GetJsonPointerPath(),
			TEXT("UnsupportedRegionAdapter"),
			FString::Printf(
				TEXT("Adapter %s does not support region %s"),
				*Adapter.GetName().ToString(),
				*Context.RegionId.ToString()));
	}

	return FAssetDocumentCapabilityResult::Success();
}

FAssetDocumentCapabilityResult CheckExplicitNull(
	const FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue)
{
	if (!DesiredValue.IsValid() || DesiredValue->Type != EJson::Null)
	{
		return FAssetDocumentCapabilityResult::Success();
	}

	const bool bAllowsNull = Context.Policy && Context.Policy->ExplicitDeleteValues.Contains(FAssetDocumentExplicitDeleteValues::Null());
	if (bAllowsNull)
	{
		return FAssetDocumentCapabilityResult::Success();
	}

	return FAssetDocumentJsonRegionUtils::Failure(
		Context.GetJsonPointerPath(),
		TEXT("UnexpectedNullBodySection"),
		FString::Printf(TEXT("Body region %s does not accept null"), *Context.RegionId.ToString()));
}

FAssetDocumentCapabilityResult CheckDesiredRegion(
	const FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue,
	const IAssetDocumentRegionAdapter& Adapter)
{
	FAssetDocumentCapabilityResult Result = CheckExplicitNull(Context, DesiredValue);
	if (!Result.bSuccess)
	{
		return Result;
	}

	return CheckAdapterSupport(Context, Adapter);
}
}

FAssetDocumentCapabilityResult FAssetDocumentRegionRuntime::Validate(
	const FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue,
	const IAssetDocumentRegionAdapter& Adapter)
{
	const FAssetDocumentCapabilityResult CheckResult = CheckDesiredRegion(Context, DesiredValue, Adapter);
	if (!CheckResult.bSuccess)
	{
		return CheckResult;
	}

	return Adapter.ValidateRegion(Context, DesiredValue);
}

FAssetDocumentCapabilityResult FAssetDocumentRegionRuntime::Preflight(
	FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue,
	const IAssetDocumentRegionAdapter& Adapter)
{
	const FAssetDocumentCapabilityResult CheckResult = CheckDesiredRegion(Context, DesiredValue, Adapter);
	if (!CheckResult.bSuccess)
	{
		return CheckResult;
	}

	return Adapter.PreflightRegion(Context, DesiredValue);
}

FAssetDocumentCapabilityResult FAssetDocumentRegionRuntime::Apply(
	FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue,
	IAssetDocumentRegionAdapter& Adapter,
	bool& bOutChanged)
{
	bOutChanged = false;

	const FAssetDocumentCapabilityResult CheckResult = CheckDesiredRegion(Context, DesiredValue, Adapter);
	if (!CheckResult.bSuccess)
	{
		return CheckResult;
	}

	return Adapter.ApplyRegion(Context, DesiredValue, bOutChanged);
}

FAssetDocumentCapabilityResult FAssetDocumentRegionRuntime::Extract(
	const FAssetDocumentRegionContext& Context,
	const IAssetDocumentRegionAdapter& Adapter,
	TSharedPtr<FJsonValue>& OutCurrentValue)
{
	const FAssetDocumentCapabilityResult CheckResult = CheckAdapterSupport(Context, Adapter);
	if (!CheckResult.bSuccess)
	{
		return CheckResult;
	}

	return Adapter.ExtractRegion(Context, OutCurrentValue);
}

FAssetDocumentCapabilityResult FAssetDocumentRegionRuntime::Diff(
	const FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue,
	const IAssetDocumentRegionAdapter& Adapter,
	TArray<TSharedPtr<FJsonValue>>& OutDiffEntries)
{
	const FAssetDocumentCapabilityResult CheckResult = CheckDesiredRegion(Context, DesiredValue, Adapter);
	if (!CheckResult.bSuccess)
	{
		return CheckResult;
	}

	return Adapter.DiffRegion(Context, DesiredValue, OutDiffEntries);
}

FAssetDocumentCapabilityResult FAssetDocumentRegionRuntime::DiffByCanonicalExtract(
	const FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue,
	const IAssetDocumentRegionAdapter& Adapter,
	TArray<TSharedPtr<FJsonValue>>& OutDiffEntries)
{
	const FAssetDocumentCapabilityResult CheckResult = CheckDesiredRegion(Context, DesiredValue, Adapter);
	if (!CheckResult.bSuccess)
	{
		return CheckResult;
	}

	TSharedPtr<FJsonValue> CurrentValue;
	const FAssetDocumentCapabilityResult ExtractResult = Adapter.ExtractRegion(Context, CurrentValue);
	if (!ExtractResult.bSuccess)
	{
		return ExtractResult;
	}

	if (FAssetDocumentJsonRegionUtils::JsonValueToComparableString(CurrentValue) !=
		FAssetDocumentJsonRegionUtils::JsonValueToComparableString(DesiredValue))
	{
		FAssetDocumentJsonRegionUtils::AddDiffEntry(
			OutDiffEntries,
			Context.GetJsonPointerPath(),
			TEXT("changed"),
			CurrentValue,
			DesiredValue);
	}

	return FAssetDocumentCapabilityResult::Success(TEXT("Diffed region by canonical extract"));
}
