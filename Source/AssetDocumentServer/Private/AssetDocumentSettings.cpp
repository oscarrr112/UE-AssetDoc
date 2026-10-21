// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentSettings.h"

UAssetDocumentSettings::UAssetDocumentSettings()
{
}

UAssetDocumentSettings* UAssetDocumentSettings::Get()
{
	return GetMutableDefault<UAssetDocumentSettings>();
}
