// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Graphs/AssetDocumentGraphTypes.h"

class FAssetDocumentGraphParser
{
public:
	static FAssetDocumentGraphParseResult ParseGraphArray(
		const TArray<TSharedPtr<FJsonValue>>& GraphValues,
		const FAssetDocumentGraphParseOptions& Options = FAssetDocumentGraphParseOptions());

	static FAssetDocumentGraphParseResult ParseSingleGraph(
		const TSharedRef<FJsonObject>& GraphObject,
		const FAssetDocumentGraphParseOptions& Options = FAssetDocumentGraphParseOptions());

	static FAssetDocumentGraphParseResult ParseGraphRegion(
		const TSharedRef<FJsonObject>& RegionObject,
		const FAssetDocumentGraphParseOptions& Options = FAssetDocumentGraphParseOptions());

	static TSharedRef<FJsonValue> WriteCanonicalGraphArray(const TArray<FAssetDocumentGraphSpec>& Graphs);
	static TSharedRef<FJsonObject> WriteCanonicalGraphRegion(const TArray<FAssetDocumentGraphSpec>& Graphs);
};
