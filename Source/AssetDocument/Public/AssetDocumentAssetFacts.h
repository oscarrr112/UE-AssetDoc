// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Read-only facts that inspect_asset_document_target adds for asset families a provider knows (e.g. the resolved
// shape list of a Control Rig Shape Library, which stays on the generic route). A describer returns null for assets
// it does not handle; its facts are added under FactsKey unless the payload already has that field.
// Registration is game-thread only; returns false for an empty or duplicate key.
using FAssetDocumentAssetFactsDescriber = TSharedPtr<FJsonObject> (*)(const UObject& Asset);

ASSETDOCUMENT_API bool RegisterAssetDocumentAssetFactsDescriber(FName FactsKey, FAssetDocumentAssetFactsDescriber Describer);
ASSETDOCUMENT_API bool UnregisterAssetDocumentAssetFactsDescriber(FName FactsKey);
