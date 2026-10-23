// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Regions/AssetDocumentDeferredRegionAdapter.h"

#include "AssetDocumentJsonRegionUtils.h"

namespace
{
FString RegionBodyPath(const FAssetDocumentRegionContext& Context)
{
	return Context.BodyPath.IsEmpty() ? Context.RegionId.ToString() : Context.BodyPath;
}

bool IsArrayLikeRegion(const FAssetDocumentRegionContext& Context)
{
	if (!Context.Policy)
	{
		return false;
	}

	return Context.Policy->RegionKind == EAssetDocumentRegionKind::Array
		|| Context.Policy->RegionKind == EAssetDocumentRegionKind::Graph
		|| Context.Policy->RegionKind == EAssetDocumentRegionKind::Timeline;
}

bool IsObjectRegion(const FAssetDocumentRegionContext& Context)
{
	return Context.Policy && Context.Policy->RegionKind == EAssetDocumentRegionKind::Object;
}

FAssetDocumentCapabilityResult InvalidTypeFailure(
	const FAssetDocumentRegionContext& Context,
	const FString& ExpectedShape)
{
	return FAssetDocumentJsonRegionUtils::Failure(
		Context.GetJsonPointerPath(),
		TEXT("InvalidBodySectionType"),
		FString::Printf(TEXT("%s must be %s when authored"), *RegionBodyPath(Context), *ExpectedShape));
}

FAssetDocumentCapabilityResult UnsupportedRegionFailure(
	const FAssetDocumentRegionContext& Context,
	const FString& Code,
	const FString& Message)
{
	const FString FailureMessage = Message.IsEmpty()
		? FString::Printf(TEXT("%s is declared but deferred and cannot be non-empty yet"), *RegionBodyPath(Context))
		: Message;
	return FAssetDocumentJsonRegionUtils::Failure(Context.GetJsonPointerPath(), Code, FailureMessage);
}

TSharedPtr<FJsonValue> MakeDeclaredDefault(const FAssetDocumentRegionContext& Context)
{
	if (IsObjectRegion(Context))
	{
		return MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
	}

	return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
}
}

FAssetDocumentDeferredRegionAdapter::FAssetDocumentDeferredRegionAdapter(
	FName InName,
	FString InUnsupportedRegionCode,
	FString InUnsupportedRegionMessage)
	: Name(InName)
	, UnsupportedRegionCode(MoveTemp(InUnsupportedRegionCode))
	, UnsupportedRegionMessage(MoveTemp(InUnsupportedRegionMessage))
{
}

FName FAssetDocumentDeferredRegionAdapter::DefaultAdapterName()
{
	return TEXT("AssetDocumentDeferredRegionAdapter");
}

bool FAssetDocumentDeferredRegionAdapter::FindDeclaredPolicyForBodyKey(
	const TArray<FAssetDocumentRegionPolicy>& RegionPolicies,
	const FString& BodyKey,
	FAssetDocumentRegionPolicy& OutPolicy)
{
	const FString DeclaredBodyPath = FString::Printf(TEXT("Body.%s"), *BodyKey);
	for (const FAssetDocumentRegionPolicy& Policy : RegionPolicies)
	{
		if (Policy.BodyPath == DeclaredBodyPath || Policy.RegionId.ToString() == DeclaredBodyPath)
		{
			OutPolicy = Policy;
			return true;
		}
	}
	return false;
}

FAssetDocumentRegionContext FAssetDocumentDeferredRegionAdapter::MakeContextFromDeclaredPolicy(
	const FAssetDocumentCapabilityContext& CapabilityContext,
	const FString& BodyKey,
	const FAssetDocumentRegionPolicy& Policy)
{
	FAssetDocumentRegionContext RegionContext;
	RegionContext.Asset = CapabilityContext.Asset;
	RegionContext.AssetClass = CapabilityContext.AssetClass;
	RegionContext.TargetAssetPath = CapabilityContext.TargetAssetPath;
	RegionContext.SourceDocumentPath = CapabilityContext.SourceDocumentPath;
	RegionContext.Definitions = CapabilityContext.Definitions;
	RegionContext.Result = CapabilityContext.Result;
	RegionContext.bIsDryRun = CapabilityContext.bIsDryRun;
	RegionContext.Policy = &Policy;
	RegionContext.RegionId = Policy.RegionId;
	RegionContext.BodyPath = Policy.BodyPath;
	RegionContext.JsonPointer = FAssetDocumentJsonRegionUtils::MakeBodyPath(BodyKey);
	return RegionContext;
}

FName FAssetDocumentDeferredRegionAdapter::GetName() const
{
	return Name;
}

bool FAssetDocumentDeferredRegionAdapter::SupportsRegion(const FAssetDocumentRegionContext& Context) const
{
	return IsObjectRegion(Context) || IsArrayLikeRegion(Context);
}

TSharedRef<FJsonObject> FAssetDocumentDeferredRegionAdapter::GetSchemaHint(const FAssetDocumentRegionContext& Context) const
{
	TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("Adapter"), GetName().ToString());
	Schema->SetStringField(TEXT("Authoring"), TEXT("deferred-empty-only"));
	Schema->SetStringField(TEXT("DeclaredDefault"), IsObjectRegion(Context) ? TEXT("object") : TEXT("array"));
	return Schema;
}

FAssetDocumentCapabilityResult FAssetDocumentDeferredRegionAdapter::ValidateRegion(
	const FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue) const
{
	if (!DesiredValue.IsValid() || DesiredValue->Type == EJson::Null)
	{
		return FAssetDocumentCapabilityResult::Success(TEXT("Deferred region omitted"));
	}

	if (IsObjectRegion(Context))
	{
		if (DesiredValue->Type != EJson::Object)
		{
			return InvalidTypeFailure(Context, TEXT("an object"));
		}

		const TSharedPtr<FJsonObject> DesiredObject = DesiredValue->AsObject();
		if (DesiredObject.IsValid() && DesiredObject->Values.Num() > 0)
		{
			return UnsupportedRegionFailure(Context, UnsupportedRegionCode, UnsupportedRegionMessage);
		}

		return FAssetDocumentCapabilityResult::Success(TEXT("Validated empty deferred object region"));
	}

	if (IsArrayLikeRegion(Context))
	{
		if (DesiredValue->Type != EJson::Array)
		{
			return InvalidTypeFailure(Context, TEXT("an array"));
		}

		if (DesiredValue->AsArray().Num() > 0)
		{
			return UnsupportedRegionFailure(Context, UnsupportedRegionCode, UnsupportedRegionMessage);
		}

		return FAssetDocumentCapabilityResult::Success(TEXT("Validated empty deferred array region"));
	}

	return FAssetDocumentJsonRegionUtils::Failure(
		Context.GetJsonPointerPath(),
		TEXT("UnsupportedRegionAdapter"),
		FString::Printf(TEXT("Adapter %s does not support region %s"), *GetName().ToString(), *Context.RegionId.ToString()));
}

FAssetDocumentCapabilityResult FAssetDocumentDeferredRegionAdapter::ApplyRegion(
	FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue,
	bool& bOutChanged)
{
	bOutChanged = false;
	return ValidateRegion(Context, DesiredValue);
}

FAssetDocumentCapabilityResult FAssetDocumentDeferredRegionAdapter::ExtractRegion(
	const FAssetDocumentRegionContext& Context,
	TSharedPtr<FJsonValue>& OutCurrentValue) const
{
	OutCurrentValue = MakeDeclaredDefault(Context);
	return FAssetDocumentCapabilityResult::Success(TEXT("Extracted declared deferred region default"));
}

FAssetDocumentCapabilityResult FAssetDocumentDeferredRegionAdapter::DiffRegion(
	const FAssetDocumentRegionContext& Context,
	const TSharedPtr<FJsonValue>& DesiredValue,
	TArray<TSharedPtr<FJsonValue>>& OutDiffEntries) const
{
	const FAssetDocumentCapabilityResult ValidateResult = ValidateRegion(Context, DesiredValue);
	if (!ValidateResult.bSuccess)
	{
		return ValidateResult;
	}

	if (!DesiredValue.IsValid() || DesiredValue->Type == EJson::Null)
	{
		return FAssetDocumentCapabilityResult::Success(TEXT("Deferred region omitted from diff"));
	}

	TSharedPtr<FJsonValue> CurrentValue;
	const FAssetDocumentCapabilityResult ExtractResult = ExtractRegion(Context, CurrentValue);
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

	return FAssetDocumentCapabilityResult::Success(TEXT("Diffed deferred region"));
}
