// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentRuntimeModule.h"

#include "Modules/ModuleManager.h"
#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"

class FAssetDocumentEditorModule final : public IAssetDocumentRuntimeModule
{
public:
	void StartupModule() override
	{
		check(!HasActiveRuntimeRegistration());
		RegisterAssetDocumentEditorClassPickerFactories();
	}

	void ShutdownModule() override
	{
		UnregisterAssetDocumentEditorClassPickerFactories();
	}

	FName GetRuntimeModuleName() const override
	{
		return TEXT("AssetDocumentEditor");
	}
};

IMPLEMENT_MODULE(FAssetDocumentEditorModule, AssetDocumentEditor)
