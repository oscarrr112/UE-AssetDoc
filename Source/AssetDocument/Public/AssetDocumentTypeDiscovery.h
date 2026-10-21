// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Type discovery hooks (list_types, inspect of a /Script struct). Core lists and describes types by
// reflection; a provider whose editor offers types through its own picker registers hooks so discovery
// answers as that picker does (e.g. StateTree node structs: the node and instance data fields an author
// sets, and which node types a schema allows).
struct FAssetDocumentTypeDiscoveryHooks
{
	// inspect of a struct: rewrite the reflected description (Kind, Path, Super, Fields[...]) for structs
	// this provider owns. Returns false for structs it does not handle.
	bool (*DescribeStruct)(const UScriptStruct& Struct, FJsonObject& InOutPayload) = nullptr;
	// list_types with 'schema': returns false when Schema is not a schema class this provider knows;
	// otherwise sets bOutAllowed to whether the editor's picker under that schema offers Type.
	bool (*IsTypeAllowed)(const UClass& Schema, const UStruct& Type, bool& bOutAllowed) = nullptr;
};

// Game-thread only; returns false for an empty or duplicate id.
ASSETDOCUMENT_API bool RegisterAssetDocumentTypeDiscoveryHooks(FName HooksId, const FAssetDocumentTypeDiscoveryHooks& Hooks);
ASSETDOCUMENT_API bool UnregisterAssetDocumentTypeDiscoveryHooks(FName HooksId);

// One entry of a struct description's field list: {Name, Type, EnumValues?}.
ASSETDOCUMENT_API TSharedPtr<FJsonObject> DescribeAssetDocumentTypeField(const FProperty& Property);
