// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Extra list_types kinds served by provider modules (GET /assetdoc/assetdocument/list-types?kind=<Kind>),
// e.g. the MetaSound node classes, data types and interfaces of AssetDocumentAudio. A lister reads the query
// (name / path / base filters are its own to interpret; paging is done by the caller) and appends one row per type;
// every row has a string "Path" (the sort key and identity). It returns false with OutError for a bad query.
// The built-in kinds (class, struct, enum) cannot be registered. Registration is game-thread only; returns false
// for an empty, built-in or duplicate kind.
using FAssetDocumentTypeLister = bool (*)(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);

ASSETDOCUMENT_API bool RegisterAssetDocumentTypeLister(FName Kind, FAssetDocumentTypeLister Lister);
ASSETDOCUMENT_API bool UnregisterAssetDocumentTypeLister(FName Kind);
