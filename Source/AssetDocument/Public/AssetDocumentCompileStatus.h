// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Compile status of asset families whose editor compiles them, as Apply / Extract / Inspect report it
// ("Compile": {Status, Messages[{Severity, Message, Graph?, Node?, NodeGuid?}]}, Status one of UpToDate,
// UpToDateWithWarnings, Dirty, Error). The Blueprint family is built in; providers register a describer
// for their own asset classes (e.g. StateTree). A describer returns null for assets it does not handle.
// Registration is game-thread only; returns false for an empty or duplicate id.
using FAssetDocumentCompileStatusDescriber = TSharedPtr<FJsonObject> (*)(const UObject& Asset);

ASSETDOCUMENT_API bool RegisterAssetDocumentCompileStatusDescriber(FName DescriberId, FAssetDocumentCompileStatusDescriber Describer);
ASSETDOCUMENT_API bool UnregisterAssetDocumentCompileStatusDescriber(FName DescriberId);
