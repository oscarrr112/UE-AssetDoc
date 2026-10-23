// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

// The base directory of the UEAssetDoc plugin (the folder that holds UEAssetDoc.uplugin) as the plugin manager
// found it, whatever the plugin folder is called in the project. Empty when the plugin is not loaded.
ASSETDOCUMENT_API FString GetAssetDocumentPluginBaseDir();
