// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentRuntimeModule.h"
#include "Modules/ModuleManager.h"

class FAssetDocumentUMGModule final : public IAssetDocumentRuntimeModule
{
public:
	void StartupModule() override { check(!HasActiveRuntimeRegistration()); }
	FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentUMG"); }
};

IMPLEMENT_MODULE(FAssetDocumentUMGModule, AssetDocumentUMG)
