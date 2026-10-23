// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentPolicy.h"

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class FAssetDocumentCanonicalJson
{
public:
	static FString WriteCanonicalJson(const TSharedPtr<FJsonValue>& Value, const FAssetDocumentRegionPolicy* Policy = nullptr);
	static FString HashJsonValue(const TSharedPtr<FJsonValue>& Value, const FAssetDocumentRegionPolicy* Policy = nullptr);

	// Canonical region inputs reserve extract-only field names at every object depth.
	// Invalid values are normalized to a JSON null value.
	static TSharedPtr<FJsonValue> CloneWithoutExtractOnlyFields(const TSharedPtr<FJsonValue>& Value, const FAssetDocumentRegionPolicy* Policy = nullptr);
};
