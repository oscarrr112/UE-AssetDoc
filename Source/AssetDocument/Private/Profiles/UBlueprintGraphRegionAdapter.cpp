// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Profiles/UBlueprintGraphRegionAdapter.h"

#include "Graphs/AssetDocumentGraphDiff.h"
#include "Graphs/AssetDocumentGraphParser.h"
#include "Graphs/K2GraphAdapter.h"
#include "Profiles/BlueprintGraphAdapterCommon.h"

#include "Engine/Blueprint.h"

FAssetDocumentCapabilityResult FUBlueprintGraphRegionAdapter::ValidateRegions(
	const FAssetDocumentCapabilityContext& Context,
	const TSharedRef<FJsonObject>& BodyObject,
	bool bIncludeDelegateSignatureGraphs) const
{
	const FAssetDocumentK2GraphAdapter K2GraphAdapter;
	for (const FAssetDocumentBlueprintGraphRegion& Region :
		FAssetDocumentBlueprintGraphAdapterCommon::GetGraphRegions(bIncludeDelegateSignatureGraphs))
	{
		TArray<FAssetDocumentGraphSpec> Graphs;
		const FAssetDocumentCapabilityResult ParseResult =
			FAssetDocumentBlueprintGraphAdapterCommon::ParseAndResolveDesiredGraphRegion(
				Context,
				BodyObject,
				Region,
				Graphs);
		if (!ParseResult.bSuccess)
		{
			return ParseResult;
		}
		if (Graphs.IsEmpty())
		{
			continue;
		}

		const FAssetDocumentCapabilityResult NodeClassResult =
			FAssetDocumentBlueprintGraphAdapterCommon::ValidateSupportedNodeClasses(Region, Graphs);
		if (!NodeClassResult.bSuccess)
		{
			return NodeClassResult;
		}

		const FAssetDocumentCapabilityResult PreflightResult =
			K2GraphAdapter.PreflightGraphRegion(Cast<UBlueprint>(Context.Asset), Region.K2Region, Graphs);
		if (!PreflightResult.bSuccess)
		{
			return PreflightResult;
		}
	}
	return FAssetDocumentCapabilityResult::Success();
}

FAssetDocumentCapabilityResult FUBlueprintGraphRegionAdapter::ExtractRegions(
	const FAssetDocumentCapabilityContext& Context,
	TSharedRef<FJsonObject>& OutBodyJson,
	bool bIncludeDelegateSignatureGraphs) const
{
	const UBlueprint* Blueprint = Cast<UBlueprint>(Context.Asset);
	if (!Blueprint)
	{
		for (const FAssetDocumentBlueprintGraphRegion& Region :
			FAssetDocumentBlueprintGraphAdapterCommon::GetGraphRegions(bIncludeDelegateSignatureGraphs))
		{
			OutBodyJson->SetArrayField(Region.Name, {});
		}
		return FAssetDocumentCapabilityResult::Success();
	}

	const FAssetDocumentK2GraphAdapter K2GraphAdapter;
	for (const FAssetDocumentBlueprintGraphRegion& Region :
		FAssetDocumentBlueprintGraphAdapterCommon::GetGraphRegions(bIncludeDelegateSignatureGraphs))
	{
		const FAssetDocumentK2GraphExtractResult ExtractResult =
			K2GraphAdapter.ExtractGraphRegion(Blueprint, Region.K2Region);
		OutBodyJson->SetField(
			Region.Name,
			FAssetDocumentGraphParser::WriteCanonicalGraphArray(ExtractResult.Graphs));
		FAssetDocumentBlueprintGraphAdapterCommon::MergeSkippedGraphEvidence(
			OutBodyJson,
			ExtractResult.SkippedNodes);
	}
	return FAssetDocumentCapabilityResult::Success();
}

FAssetDocumentCapabilityResult FUBlueprintGraphRegionAdapter::DiffRegions(
	const FAssetDocumentCapabilityContext& Context,
	const TSharedRef<FJsonObject>& DesiredBody,
	TArray<TSharedPtr<FJsonValue>>& OutDiffEntries,
	bool bIncludeDelegateSignatureGraphs) const
{
	const UBlueprint* Blueprint = Cast<UBlueprint>(Context.Asset);
	if (!Blueprint)
	{
		return FAssetDocumentBlueprintGraphAdapterCommon::Failure(
			TEXT("UBlueprint graph diff requires exact UBlueprint asset"),
			TEXT("/Body"),
			TEXT("UnsupportedAsset"));
	}

	const FAssetDocumentK2GraphAdapter K2GraphAdapter;
	const TSharedPtr<FJsonObject> Definitions = Context.Definitions ? *Context.Definitions : nullptr;
	for (const FAssetDocumentBlueprintGraphRegion& Region :
		FAssetDocumentBlueprintGraphAdapterCommon::GetGraphRegions(bIncludeDelegateSignatureGraphs))
	{
		TArray<FAssetDocumentGraphSpec> DesiredGraphs;
		const FAssetDocumentCapabilityResult DesiredParseResult =
			FAssetDocumentBlueprintGraphAdapterCommon::ParseDesiredGraphRegion(
				DesiredBody,
				Region,
				DesiredGraphs);
		if (!DesiredParseResult.bSuccess)
		{
			return DesiredParseResult;
		}
		if (FAssetDocumentBlueprintGraphAdapterCommon::IsEmptyFunctionOrMacroNoop(Region, DesiredGraphs))
		{
			continue;
		}

		const FAssetDocumentK2GraphExtractResult CurrentExtract =
			K2GraphAdapter.ExtractGraphRegion(Blueprint, Region.K2Region);
		const TArray<FAssetDocumentGraphDiffEntry> GraphEntries =
			FAssetDocumentGraphDiff::CompareUbergraphPages(
				DesiredGraphs,
				CurrentExtract.Graphs,
				Definitions);
		for (const FAssetDocumentGraphDiffEntry& GraphEntry : GraphEntries)
		{
			OutDiffEntries.Add(MakeShared<FJsonValueObject>(
				FAssetDocumentBlueprintGraphAdapterCommon::MakeCapabilityDiffEntry(Region, GraphEntry)));
		}

		FAssetDocumentBlueprintGraphAdapterCommon::AppendSkippedGraphDiffEntries(
			Region,
			CurrentExtract.SkippedNodes,
			OutDiffEntries);
	}
	return FAssetDocumentCapabilityResult::Success(TEXT("Diffed UBlueprint graph regions"));
}

FAssetDocumentCapabilityResult ApplyUBlueprintGraphRegions(
	FAssetDocumentCapabilityContext& Context,
	const TSharedRef<FJsonObject>& DesiredBody,
	bool& bOutChanged,
	bool bIncludeDelegateSignatureGraphs)
{
	bOutChanged = false;
	UBlueprint* Blueprint = Cast<UBlueprint>(Context.Asset);
	if (!Blueprint)
	{
		return FAssetDocumentBlueprintGraphAdapterCommon::Failure(
			TEXT("UBlueprint graph apply requires exact UBlueprint asset"),
			TEXT("/Body"),
			TEXT("UnsupportedAsset"));
	}

	const FAssetDocumentK2GraphAdapter K2GraphAdapter;
	for (const FAssetDocumentBlueprintGraphRegion& Region :
		FAssetDocumentBlueprintGraphAdapterCommon::GetGraphRegions(bIncludeDelegateSignatureGraphs))
	{
		TArray<FAssetDocumentGraphSpec> DesiredGraphs;
		const FAssetDocumentCapabilityResult DesiredParseResult =
			FAssetDocumentBlueprintGraphAdapterCommon::ParseAndResolveDesiredGraphRegion(
				Context,
				DesiredBody,
				Region,
				DesiredGraphs);
		if (!DesiredParseResult.bSuccess)
		{
			return DesiredParseResult;
		}

		const FAssetDocumentK2GraphApplyResult ApplyResult =
			K2GraphAdapter.ApplyGraphRegion(Blueprint, Region.K2Region, DesiredGraphs);
		if (!ApplyResult.Result.bSuccess)
		{
			return ApplyResult.Result;
		}
		bOutChanged |= ApplyResult.bChanged;
	}
	return FAssetDocumentCapabilityResult::Success(TEXT("Applied UBlueprint graph regions"));
}
