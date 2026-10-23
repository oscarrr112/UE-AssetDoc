// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentProfile.h"
#include "Graphs/AssetDocumentGraphDiff.h"
#include "Graphs/AssetDocumentNodeAdapter.h"
#include "Graphs/K2GraphAdapter.h"

struct FAssetDocumentBlueprintGraphRegion
{
	FString Name;
	FString Path;
	EAssetDocumentK2GraphRegion K2Region = EAssetDocumentK2GraphRegion::UbergraphPages;
};

class FAssetDocumentBlueprintGraphAdapterCommon final
{
public:
	FAssetDocumentBlueprintGraphAdapterCommon() = delete;

	static const TArray<FAssetDocumentBlueprintGraphRegion>& GetGraphRegions(
		bool bIncludeDelegateSignatureGraphs = false);

	static bool IsEmptyFunctionOrMacroNoop(
		const FAssetDocumentBlueprintGraphRegion& Region,
		const TArray<FAssetDocumentGraphSpec>& DesiredGraphs);

	static FAssetDocumentCapabilityResult Failure(
		const FString& Message,
		const FString& Path,
		const FString& Code);

	static FAssetDocumentCapabilityResult ParseDesiredGraphRegion(
		const TSharedRef<FJsonObject>& DesiredBody,
		const FAssetDocumentBlueprintGraphRegion& Region,
		TArray<FAssetDocumentGraphSpec>& OutGraphs);

	static FAssetDocumentCapabilityResult ParseAndResolveDesiredGraphRegion(
		const FAssetDocumentCapabilityContext& Context,
		const TSharedRef<FJsonObject>& DesiredBody,
		const FAssetDocumentBlueprintGraphRegion& Region,
		TArray<FAssetDocumentGraphSpec>& OutGraphs);

	static FAssetDocumentCapabilityResult ValidateSupportedNodeClasses(
		const FAssetDocumentBlueprintGraphRegion& Region,
		const TArray<FAssetDocumentGraphSpec>& Graphs);

	static void MergeSkippedGraphEvidence(
		TSharedRef<FJsonObject>& OutBodyJson,
		const TArray<TSharedPtr<FJsonValue>>& SkippedNodes);

	static TSharedRef<FJsonObject> MakeCapabilityDiffEntry(
		const FAssetDocumentBlueprintGraphRegion& Region,
		const FAssetDocumentGraphDiffEntry& GraphEntry);

	static void AppendSkippedGraphDiffEntries(
		const FAssetDocumentBlueprintGraphRegion& Region,
		const TArray<TSharedPtr<FJsonValue>>& SkippedNodes,
		TArray<TSharedPtr<FJsonValue>>& OutDiffEntries);

private:
	static FAssetDocumentCapabilityResult InvalidRegionTypeFailure(
		const FAssetDocumentBlueprintGraphRegion& Region);

	static FAssetDocumentCapabilityResult GraphDiagnosticsFailure(
		const TArray<FAssetDocumentGraphDiagnostic>& Diagnostics,
		const FString& FallbackPath);

	static FString MakeNodePath(
		const FAssetDocumentBlueprintGraphRegion& Region,
		int32 GraphIndex,
		int32 NodeIndex);

	static UClass* ResolveNodeClass(const FString& ClassPath);

	static FAssetDocumentUnsupportedNodeDiagnostic MakeUnsupportedNodeDiagnostic(
		const FAssetDocumentNodeSpec& Node,
		const FString& Path,
		const FString& Code,
		const FString& Reason,
		const FString& SuggestedAction);

	static FAssetDocumentCapabilityResult UnsupportedGraphFailure(
		const FString& FallbackPath,
		const TArray<FAssetDocumentUnsupportedNodeDiagnostic>& UnsupportedDiagnostics);

	static TSharedRef<FJsonObject> MakeRawCapabilityDiffEntry(
		const FString& Path,
		const FString& Status,
		const TSharedPtr<FJsonValue>& Current,
		const TSharedPtr<FJsonValue>& Desired,
		const FString& Change,
		const FString& Message,
		const FString& Code);

	static FString RewriteGraphDiffPath(
		const FString& Path,
		const FAssetDocumentBlueprintGraphRegion& Region);

	static FString MakeSkippedGraphPath(
		const FAssetDocumentBlueprintGraphRegion& Region,
		const TSharedPtr<FJsonObject>& SkippedNode);

	static FString MakeSkippedNodeMessage(const TSharedPtr<FJsonObject>& SkippedNode);
};
