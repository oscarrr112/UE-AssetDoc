// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentPluginPaths.h"

#include "Interfaces/IPluginManager.h"

FString GetAssetDocumentPluginBaseDir()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEAssetDoc"));
	return Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
}
