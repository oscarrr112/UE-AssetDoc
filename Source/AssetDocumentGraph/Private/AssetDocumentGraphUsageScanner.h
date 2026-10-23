// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

namespace UE::AssetDocument::Graph
{
namespace UsageScan
{
// An authored action key in canonical form: aliases resolved ("K2:Branch" -> "K2:IfThenElse"), a member of
// the asset's own class owned by "Self" ("K2:Get:Health" -> "K2:VariableGet:Self:Health").
FString NormalizeActionKey(const FString& InKey);
}

// The find_assets `uses` scanner of Blueprint-family and graph-hosted assets (AssetDocumentUsageSearch.h).
void RegisterGraphUsageScanner();
void UnregisterGraphUsageScanner();
}
