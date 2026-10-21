// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphModel.h"
#include "AssetDocumentGraphValue.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

class IAssetDocumentAtomicTransaction;

namespace UE::AssetDocument::Graph
{
inline const TCHAR* ProviderId = TEXT("graph");

// Per-invocation state handed to a primitive handler. The asset is already
// resolved from the verified "asset" lease; mutating calls already enlisted the
// whole-package footprint (Core restores the package snapshot on rollback).
struct ASSETDOCUMENTGRAPH_API FCall
{
	const FAssetDocumentPrimitiveInvocation& Invocation;
	UObject& Asset;
	FAssetGraphs Graphs;
	FAssetDocumentGraphError& Error;
	FReader Args;
	bool bMutating = false;

	FCall(const FAssetDocumentPrimitiveInvocation& InInvocation, UObject& InAsset, FAssetDocumentGraphError& InError)
		: Invocation(InInvocation), Asset(InAsset), Error(InError), Args(InInvocation.ValueArguments, FString(), InError)
	{
	}

	// Re-reads the graph list after structural edits.
	bool Refresh();
};

using FHandler = bool (*)(FCall& Call, FObjectBuilder& Result);

struct FBinding
{
	const TCHAR* PrimitiveId = nullptr;
	FHandler Handler = nullptr;
};

ASSETDOCUMENTGRAPH_API TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetGraphPrimitiveDescriptors();
ASSETDOCUMENTGRAPH_API TConstArrayView<FBinding> GetGraphPrimitiveBindings();

// Blueprint member primitives (canonical provider "blueprint"): parent, interfaces,
// variables, components, class defaults and Class Settings of a UBlueprint.
inline const TCHAR* BlueprintMemberProviderId = TEXT("blueprint");
ASSETDOCUMENTGRAPH_API TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetBlueprintMemberPrimitiveDescriptors();
ASSETDOCUMENTGRAPH_API TConstArrayView<FBinding> GetBlueprintMemberPrimitiveBindings();

// Five closest keys of Candidates for an unknown key (token match + edit distance).
ASSETDOCUMENTGRAPH_API TArray<FString> SuggestActionKeys(const FString& Unknown, TConstArrayView<FString> Candidates, int32 Max = 5);

// Value budget: outcomes are bounded to RuntimeValueMaximumNodes (4096) nodes.
constexpr int32 QueryValueNodeBudget = 3600;
constexpr int32 EnumerateDefaultLimit = 200;
constexpr int32 EnumerateMaximumLimit = 400;
} // namespace UE::AssetDocument::Graph
