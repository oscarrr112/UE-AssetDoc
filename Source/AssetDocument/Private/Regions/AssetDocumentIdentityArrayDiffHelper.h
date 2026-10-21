// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentProfile.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct FAssetDocumentIdentityArrayDiffElement
{
	FString Identity;
	FString PathToken;
	TSharedPtr<FJsonValue> Value;
};

struct FAssetDocumentIdentityArrayDiffEntryContext
{
	FString Identity;
	FString Path;
	TSharedPtr<FJsonValue> CurrentValue;
	TSharedPtr<FJsonValue> DesiredValue;
	bool bHasCurrent = false;
	bool bHasDesired = false;
};

struct FAssetDocumentIdentityArrayDiffOptions
{
	FString RegionPath;
	FString ExtraChange = TEXT("extra");
	FString MissingChange = TEXT("missing");
	FString ChangedChange = TEXT("changed");
	bool bEmitUnchanged = true;
};

struct FAssetDocumentIdentityArrayDiffHooks
{
	TFunction<bool(const FAssetDocumentIdentityArrayDiffEntryContext&)> AreElementsEqual;
	TFunction<FString(const FAssetDocumentIdentityArrayDiffEntryContext&)> MakePath;
	TFunction<FString(const FAssetDocumentIdentityArrayDiffEntryContext&)> MakeChange;
};

class FAssetDocumentIdentityArrayDiffHelper
{
public:
	static FAssetDocumentCapabilityResult Diff(
		const FAssetDocumentIdentityArrayDiffOptions& Options,
		const TArray<FAssetDocumentIdentityArrayDiffElement>& CurrentElements,
		const TArray<FAssetDocumentIdentityArrayDiffElement>& DesiredElements,
		const FAssetDocumentIdentityArrayDiffHooks& Hooks,
		TArray<TSharedPtr<FJsonValue>>& OutDiffEntries);
};
