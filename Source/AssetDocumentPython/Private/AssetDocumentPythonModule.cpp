// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentRuntimeModule.h"
#include "Modules/ModuleManager.h"

class FAssetDocumentPythonModule final : public IAssetDocumentRuntimeModule
{
public:
	FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentPython"); }
};

IMPLEMENT_MODULE(FAssetDocumentPythonModule, AssetDocumentPython)
