// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

struct FAssetData;

// find_assets `uses` (GET /assetdoc/assetdocument/find-assets?uses=...): which assets' graphs use a node.
// Core parses the terms, matches them and pages the result; a scanner registered by a graph provider module
// (AssetDocumentGraph: the Blueprint family's saved Find-in-Blueprints index, and the loaded graphs of any asset
// its graph host adapters know) reports the nodes of one asset.

// One node of a graph asset.
struct FAssetDocumentUsageNode
{
	// Graph name (the graph path when the graph was read loaded).
	FString Graph;
	// 32 upper-case hex digits (the NodeGuid an extract writes).
	FString NodeGuid;
	FString Title;
	// The node's action key; empty when not known. A key read from the index leaves an owner segment the index
	// does not record empty ("K2:MacroInstance::ForEachLoop"); an empty segment matches any term segment.
	FString Action;
	// Node class path (/Script/BlueprintGraph.K2Node_CallFunction).
	FString NodeClass;
	// The function the node calls or implements (calls, events); empty for other nodes.
	FString Function;
	// Class paths the function may belong to (its declaring class first when known).
	TArray<FString> FunctionOwners;
};

enum class EAssetDocumentUsageScan : uint8
{
	// Not an asset family this scanner knows (core tries the next scanner, then skips the asset).
	NotHandled,
	Searched,
	// A graph asset that could not be searched; Reason says why.
	NotSearched,
};

struct FAssetDocumentUsageScan
{
	// "Index" (saved Find-in-Blueprints data, the asset was not loaded) or "Loaded" (its graphs in memory).
	FString Source;
	// NotSearched: a short code ("NotIndexed", "NotLoaded") and a sentence for the AI.
	FString Reason;
	FString Detail;
	// Every node of the asset, or only the nodes that can match (a scanner may prefilter with the query tokens).
	TArray<FAssetDocumentUsageNode> Nodes;
};

// A name every node a term alternative matches has in its saved data (a node class name, a function name).
struct FAssetDocumentUsageToken
{
	FString Text;
	bool bIgnoreCase = false;
};

struct FAssetDocumentUsageQuery
{
	// Per term (every term must match): tokens one of which the saved data of a matching asset contains. A term
	// with no tokens cannot be prefiltered.
	TArray<TArray<FAssetDocumentUsageToken>> TermTokens;
	// Every term names Blueprint content only (K2 / Anim / SM action keys, Blueprint node classes, functions): asset
	// families without Blueprint graphs cannot match and are skipped instead of being reported as not searched.
	bool bBlueprintTermsOnly = false;
};

struct FAssetDocumentUsageScanner
{
	EAssetDocumentUsageScan (*Scan)(const FAssetData& Asset, const FAssetDocumentUsageQuery& Query, FAssetDocumentUsageScan& OutScan) = nullptr;
	// Optional: the canonical action key of an authored key or alias (as the documents accept it); the input
	// when the scanner does not know the key's tag. "Self" owner segments are kept (they match any owner).
	FString (*NormalizeActionKey)(const FString& Key) = nullptr;
	// Optional: the short name of the node class every node with this canonical key has ("K2:IfThenElse" ->
	// "K2Node_IfThenElse"), or empty when not known. Used as an index prefilter token.
	FString (*ActionKeyNodeClass)(const FString& CanonicalKey) = nullptr;
	// Optional: true for action-key tags of Blueprint graphs ("K2", "Anim", "SM").
	bool (*IsBlueprintTag)(const FString& Tag) = nullptr;
};

// Game-thread only; false for an empty or duplicate id.
ASSETDOCUMENT_API bool RegisterAssetDocumentUsageScanner(FName ScannerId, const FAssetDocumentUsageScanner& Scanner);
ASSETDOCUMENT_API bool UnregisterAssetDocumentUsageScanner(FName ScannerId);
