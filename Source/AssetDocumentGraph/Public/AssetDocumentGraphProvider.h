// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "UObject/TopLevelAssetPath.h"
#include "AssetDocumentGraphPrimitives.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

class UBlueprint;

// Generic, schema-agnostic EdGraph primitives (provider id "graph").
// Contract: docs/architecture/asset-document-graph-primitives.md.
// The runtime bootstrap assembles this provider; it is stateless.
ASSETDOCUMENTGRAPH_API TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentGraphPrimitiveProvider();

// Blueprint member primitives (provider id "blueprint", owner Blueprint): parent class,
// interfaces, member variables, components (SCS, inherited and native
// overrides), class defaults and Class Settings of an exact UBlueprint. Same
// conventions as the graph provider: one "asset" handle, value DTOs with
// schema_version 1, whole-package footprint, no typed inverse.
ASSETDOCUMENTGRAPH_API TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentBlueprintMemberPrimitiveProvider();

// Table-driven provider over a descriptor/binding table pair, for provider modules that
// plug their own primitives into the graph provider's invocation plumbing (verified
// "asset" lease, whole-package footprint enlistment, outcome validation) -- e.g. the
// Widget Blueprint provider "umg" in AssetDocumentWidget. Handlers receive an
// UE::AssetDocument::Graph::FCall (AssetDocumentGraphPrimitives.h); the tables must be
// stable for the lifetime of the provider.
ASSETDOCUMENTGRAPH_API TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentGraphTablePrimitiveProvider(
	const TCHAR* ProviderId,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> (*Descriptors)(),
	TConstArrayView<UE::AssetDocument::Graph::FBinding> (*Bindings)());

// The same table-driven plumbing for assets that have no graphs (e.g. the data table
// provider "data" in AssetDocumentData): handlers get an FCall whose Graphs is empty.
ASSETDOCUMENTGRAPH_API TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentAssetTablePrimitiveProvider(
	const TCHAR* ProviderId,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> (*Descriptors)(),
	TConstArrayView<UE::AssetDocument::Graph::FBinding> (*Bindings)());

// Blueprints stored inside another asset (a Level Sequence's Director Blueprint): the "graph" and "blueprint" providers
// resolve the verified "asset" handle of such a host to its Blueprint, so the Blueprint profile's components edit it
// through the host's own handles (same package footprint, same transaction). A host class with a resolver but no
// Blueprint now fails those primitives with Graph.NoHostedBlueprint. Rollback: the host package's snapshot rebuilds
// the hosted Blueprint as it rebuilds a Blueprint package (refresh, compile, class defaults restored). Game thread.
// Registered by host class path (the class and its subclasses), so a module's ShutdownModule unregisters without
// touching the class: on a normal editor exit the UObject exit purge runs before ShutdownModule.
using FAssetDocumentHostedBlueprintResolver = UBlueprint* (*)(UObject& Host);
ASSETDOCUMENTGRAPH_API void RegisterAssetDocumentHostedBlueprint(const FTopLevelAssetPath& HostClassPath, FAssetDocumentHostedBlueprintResolver Resolver);
ASSETDOCUMENTGRAPH_API void UnregisterAssetDocumentHostedBlueprint(const FTopLevelAssetPath& HostClassPath);
// The Blueprint Host holds (null: none now). bOutHostClass: a resolver is registered for Host's class or a super class.
ASSETDOCUMENTGRAPH_API UBlueprint* ResolveAssetDocumentHostedBlueprint(UObject& Host, bool* bOutHostClass = nullptr);
