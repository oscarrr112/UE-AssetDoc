// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Graphs/AssetDocumentGraphTypes.h"

struct FAssetDocumentGraphGuidCompletionResult
{
	bool bSuccess = false;
	FString Code;
	FString Path;
	FString Message;
	TSharedPtr<FJsonValue> CompletedRegion;
};

struct FAssetDocumentGraphDiffEntry
{
	FString Path;
	FString Status;
	FString Message;
	TSharedPtr<FJsonValue> Desired;
	TSharedPtr<FJsonValue> Current;

	TSharedRef<FJsonObject> ToJsonObject() const;
	TSharedRef<FJsonObject> ToBodyDiffEntryJsonObject() const;
};

class FAssetDocumentGraphDiff
{
public:
	static TArray<FAssetDocumentGraphDiffEntry> CompareUbergraphPages(
		const TArray<FAssetDocumentGraphSpec>& DesiredGraphs,
		const TArray<FAssetDocumentGraphSpec>& CurrentGraphs,
		const TSharedPtr<FJsonObject>& Definitions);

	static TArray<FAssetDocumentGraphDiffEntry> CompareGraphRegion(
		const TArray<FAssetDocumentGraphSpec>& DesiredGraphs,
		const TArray<FAssetDocumentGraphSpec>& CurrentGraphs,
		const FString& RegionPath);

	static FAssetDocumentGraphGuidCompletionResult CompleteMissingGuids(
		const TSharedPtr<FJsonValue>& AuthoredRegion,
		const TSharedPtr<FJsonValue>& FreshRegion,
		const FString& RegionPath);

	static FAssetDocumentGraphDiffEntry MakeUnsupported(const FString& Path, const FString& Message);
};
