// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphAdapters.h"

class UBlueprint;
class UBlueprintNodeSpawner;

namespace UE::AssetDocument::Graph
{
// Built-in adapters registered by FAssetDocumentGraphModule.
TSharedRef<IAssetDocumentGraphSchemaAdapter> MakeK2SchemaAdapter();
TSharedRef<IAssetDocumentGraphSchemaAdapter> MakeGenericSchemaAdapter();
TSharedRef<IAssetDocumentGraphHostAdapter> MakeBlueprintHostAdapter();

namespace K2Keys
{
// Canonical owner text: classes are written by their authoritative (non-skeleton)
// class path, every other object by its path name.
FString OwnerPath(const UObject* Object);
// "K2Node_IfThenElse" -> "IfThenElse".
FString KindOf(const UClass* NodeClass);
// Base key of one database action (no @binding / #signature suffix).
// Graph is only used as the template-node outer for macro/local-variable spawners.
bool ComputeSpawnerBaseKey(const UObject* DatabaseOwner, const UBlueprintNodeSpawner& Spawner, UEdGraph* Graph, FString& OutKey);
// Reverse mapping: action key of an existing node (empty when not derivable).
FString ComputeNodeKey(const UEdGraphNode& Node);
// What a plain spawner's CustomizeNodeDelegate sets on its node (e.g. a Get Subsystem node's class), as
// key text "<value>[,<value>...]"; empty when it sets nothing the node class declares.
FString CustomizedIdentity(const UBlueprintNodeSpawner& Spawner);
// The action list K2 lookups use in Graph (kept up to date from what changed since the last lookup)
// against the list keyed from scratch: true when they are the same; else false and the first difference.
bool CompareCachedActionList(UBlueprint& Blueprint, UEdGraph& Graph, FString& OutDifference);
} // namespace K2Keys
} // namespace UE::AssetDocument::Graph
