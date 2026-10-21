// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Regions/AssetDocumentGraphRegionWrapperAdapter.h"

#include "AssetDocumentJsonRegionUtils.h"

namespace
{
FAssetDocumentCapabilityContext ToCapabilityContext(const FAssetDocumentRegionContext& Context)
{
	FAssetDocumentCapabilityContext CapabilityContext;
	CapabilityContext.Asset = Context.Asset;
	CapabilityContext.AssetClass = Context.AssetClass;
	CapabilityContext.TargetAssetPath = Context.TargetAssetPath;
	CapabilityContext.SourceDocumentPath = Context.SourceDocumentPath;
	CapabilityContext.Definitions = Context.Definitions;
	CapabilityContext.bIsDryRun = Context.bIsDryRun;
	CapabilityContext.Result = Context.Result;
	return CapabilityContext;
}

FAssetDocumentCapabilityResult GraphWrapperUnsupportedLifecycleFailure(
	const FAssetDocumentRegionContext& Context,
	const TCHAR* Lifecycle)
{
	return FAssetDocumentCapabilityResult::Failure(
		FString::Printf(TEXT("Graph region wrapper '%s' does not implement %s"), *Context.RegionId.ToString(), Lifecycle),
		Context.GetJsonPointerPath(),
		TEXT("UnsupportedGraphRegionLifecycle"));
}

FAssetDocumentCapabilityResult RequireGraphBodyObject(
	const FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue,
	TSharedPtr<FJsonObject>& OutBodyObject)
{
	return FAssetDocumentJsonRegionUtils::RequireObjectValue(DesiredValue, Context.GetJsonPointerPath(), OutBodyObject);
}
}

FAssetDocumentGraphRegionWrapperAdapter::FAssetDocumentGraphRegionWrapperAdapter(
	FAssetDocumentGraphRegionWrapperConfig InConfig,
	FAssetDocumentGraphRegionWrapperHooks InHooks)
	: Config(MoveTemp(InConfig))
	, Hooks(MoveTemp(InHooks))
{
}

FName FAssetDocumentGraphRegionWrapperAdapter::GetName() const
{
	return Config.AdapterName;
}

bool FAssetDocumentGraphRegionWrapperAdapter::SupportsRegion(const FAssetDocumentRegionContext& Context) const
{
	const bool bMatchesSyntheticBodyRegion =
		Context.RegionId == FName(*Config.RegionId)
		|| Context.BodyPath == Config.BodyPath;
	return bMatchesSyntheticBodyRegion && Context.JsonPointer == Config.JsonPointer;
}

TSharedRef<FJsonObject> FAssetDocumentGraphRegionWrapperAdapter::GetSchemaHint(
	const FAssetDocumentRegionContext& Context) const
{
	TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("Adapter"), GetName().ToString());
	if (!Config.SchemaLabel.IsEmpty())
	{
		Schema->SetStringField(TEXT("Label"), Config.SchemaLabel);
	}
	return Schema;
}

FAssetDocumentCapabilityResult FAssetDocumentGraphRegionWrapperAdapter::ValidateRegion(
	const FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue) const
{
	TSharedPtr<FJsonObject> BodyObject;
	FAssetDocumentCapabilityResult Result = RequireGraphBodyObject(Context, DesiredValue, BodyObject);
	if (!Result.bSuccess)
	{
		return Result;
	}

	if (!Hooks.Validate)
	{
		return GraphWrapperUnsupportedLifecycleFailure(Context, TEXT("validate"));
	}

	return Hooks.Validate(ToCapabilityContext(Context), BodyObject.ToSharedRef());
}

FAssetDocumentCapabilityResult FAssetDocumentGraphRegionWrapperAdapter::PreflightRegion(
	FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue) const
{
	TSharedPtr<FJsonObject> BodyObject;
	FAssetDocumentCapabilityResult Result = RequireGraphBodyObject(Context, DesiredValue, BodyObject);
	if (!Result.bSuccess)
	{
		return Result;
	}

	if (!Hooks.Preflight)
	{
		return GraphWrapperUnsupportedLifecycleFailure(Context, TEXT("preflight"));
	}

	FAssetDocumentCapabilityContext CapabilityContext = ToCapabilityContext(Context);
	return Hooks.Preflight(CapabilityContext, BodyObject.ToSharedRef());
}

FAssetDocumentCapabilityResult FAssetDocumentGraphRegionWrapperAdapter::ApplyRegion(
	FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue,
	bool& bOutChanged)
{
	TSharedPtr<FJsonObject> BodyObject;
	FAssetDocumentCapabilityResult Result = RequireGraphBodyObject(Context, DesiredValue, BodyObject);
	if (!Result.bSuccess)
	{
		bOutChanged = false;
		return Result;
	}

	if (!Hooks.Apply)
	{
		return GraphWrapperUnsupportedLifecycleFailure(Context, TEXT("apply"));
	}

	FAssetDocumentCapabilityContext CapabilityContext = ToCapabilityContext(Context);
	return Hooks.Apply(CapabilityContext, BodyObject.ToSharedRef(), bOutChanged);
}

FAssetDocumentCapabilityResult FAssetDocumentGraphRegionWrapperAdapter::ExtractRegion(
	const FAssetDocumentRegionContext& Context,
	TSharedPtr<FJsonValue>& OutCurrentValue) const
{
	OutCurrentValue.Reset();
	if (!Hooks.Extract)
	{
		return GraphWrapperUnsupportedLifecycleFailure(Context, TEXT("extract"));
	}

	TSharedRef<FJsonObject> BodyObject = MakeShared<FJsonObject>();
	const FAssetDocumentCapabilityResult Result = Hooks.Extract(ToCapabilityContext(Context), BodyObject);
	if (!Result.bSuccess)
	{
		return Result;
	}

	OutCurrentValue = MakeShared<FJsonValueObject>(BodyObject);
	return Result;
}

FAssetDocumentCapabilityResult FAssetDocumentGraphRegionWrapperAdapter::DiffRegion(
	const FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue,
	TArray<TSharedPtr<FJsonValue>>& OutDiffEntries) const
{
	TSharedPtr<FJsonObject> BodyObject;
	FAssetDocumentCapabilityResult Result = RequireGraphBodyObject(Context, DesiredValue, BodyObject);
	if (!Result.bSuccess)
	{
		return Result;
	}

	if (!Hooks.Diff)
	{
		return GraphWrapperUnsupportedLifecycleFailure(Context, TEXT("diff"));
	}

	return Hooks.Diff(ToCapabilityContext(Context), BodyObject.ToSharedRef(), OutDiffEntries);
}
