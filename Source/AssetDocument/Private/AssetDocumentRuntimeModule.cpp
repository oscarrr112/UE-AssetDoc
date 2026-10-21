// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentRuntimeModule.h"

IAssetDocumentRuntimeModule::~IAssetDocumentRuntimeModule() = default;

bool IAssetDocumentRuntimeModule::HasActiveRuntimeRegistration() const
{
	return false;
}
