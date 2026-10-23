// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentProfile.h"
#include "Graphs/AssetDocumentGraphTypes.h"
#include "Graphs/AssetDocumentNodeAdapter.h"

class UBlueprint;
class UEdGraphNode;

enum class EAssetDocumentK2GraphRegion : uint8
{
	UbergraphPages,
	FunctionGraphs,
	MacroGraphs,
	DelegateSignatureGraphs,
};

struct FAssetDocumentK2GraphExtractResult
{
	TArray<FAssetDocumentGraphSpec> Graphs;
	TArray<TSharedPtr<FJsonValue>> SkippedNodes;
};

struct FAssetDocumentK2GraphApplyResult
{
	FAssetDocumentCapabilityResult Result;
	bool bChanged = false;
};

class FAssetDocumentK2GraphAdapter
{
public:
	static FAssetDocumentNodeAdapterRegistry CreateTier1NodeAdapterRegistry();

	FAssetDocumentK2GraphExtractResult ExtractGraphRegion(const UBlueprint* Blueprint, EAssetDocumentK2GraphRegion Region) const;
	FAssetDocumentCapabilityResult PreflightGraphRegion(UBlueprint* Blueprint, EAssetDocumentK2GraphRegion Region, const TArray<FAssetDocumentGraphSpec>& DesiredGraphs) const;
	FAssetDocumentCapabilityResult PreflightGraphRegion(UBlueprint* CurrentBlueprint, UBlueprint* DesiredStateBlueprint, EAssetDocumentK2GraphRegion Region, const TArray<FAssetDocumentGraphSpec>& DesiredGraphs) const;
	FAssetDocumentK2GraphApplyResult ApplyGraphRegion(UBlueprint* Blueprint, EAssetDocumentK2GraphRegion Region, const TArray<FAssetDocumentGraphSpec>& DesiredGraphs) const;

	FAssetDocumentK2GraphExtractResult ExtractUbergraphPages(const UBlueprint* Blueprint) const;
	FAssetDocumentCapabilityResult PreflightUbergraphPages(UBlueprint* Blueprint, const TArray<FAssetDocumentGraphSpec>& DesiredGraphs) const;
	FAssetDocumentK2GraphApplyResult ApplyUbergraphPages(UBlueprint* Blueprint, const TArray<FAssetDocumentGraphSpec>& DesiredGraphs) const;

private:
	bool ExtractNode(const UBlueprint* Blueprint, const UEdGraphNode* Node, FAssetDocumentNodeSpec& OutNode) const;
};
