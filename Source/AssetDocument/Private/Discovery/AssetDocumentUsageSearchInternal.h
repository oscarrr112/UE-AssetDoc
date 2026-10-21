// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FAssetData;

namespace UE::AssetDocument::Discovery
{
// find_assets `uses`: keeps the candidates whose graphs use every term (AssetDocumentUsageSearch.h), one row per
// asset {ObjectPath, Class, Source?, Matches?, Graphs?[{Graph, Nodes[{NodeGuid, Title, Action | Class}]}]}.
// OutSummary: {Searched?, NotSearched?{Total, Reasons[{Reason, Count, Detail}], Assets[{ObjectPath, Class, Reason}]}}.
bool FilterAssetsByUsage(const FString& Uses, const TArray<FAssetData>& Candidates, TArray<TSharedPtr<FJsonObject>>& OutRows,
	TSharedPtr<FJsonObject>& OutSummary, FString& OutError);
}
