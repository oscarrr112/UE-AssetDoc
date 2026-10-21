// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentDataMaterial.h"
#include "AssetDocumentDataOpenEditorHandler.h"
#include "AssetDocumentDataUserTypes.h"
#include "AssetDocumentRuntimeModule.h"
#include "Modules/ModuleManager.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

// The "data" provider is assembled by the runtime bootstrap. The module registers the Core
// package-snapshot rebuilders of User Defined Structs / Enums (rollback recompiles the struct
// from its restored editor data and restores the dependents the editor updated) and of
// Materials / Material Functions (rollback recompiles the restored graph), and the open-editor handlers of Data
// Tables (an open Data Table editor shows a rolled-back table) and of materials and material instances
// (AssetDocumentDataMaterialOpenEditor.cpp).
class FAssetDocumentDataModule final : public IAssetDocumentRuntimeModule
{
public:
	virtual void StartupModule() override
	{
		using namespace UE::AssetDocument::Graph::Data::UserTypes;
		StructRebuilder = MakeUserStructRebuilder();
		EnumRebuilder = MakeUserEnumRebuilder();
		RegisterAssetDocumentDerivedStateRebuilder(StructRebuilder.ToSharedRef());
		RegisterAssetDocumentDerivedStateRebuilder(EnumRebuilder.ToSharedRef());
		MaterialRebuilder = UE::AssetDocument::Graph::Data::Material::MakeMaterialRebuilder();
		RegisterAssetDocumentDerivedStateRebuilder(MaterialRebuilder.ToSharedRef());
		DataTableOpenEditorHandler = UE::AssetDocument::Graph::Data::MakeDataTableOpenEditorHandler();
		RegisterAssetDocumentOpenEditorHandler(DataTableOpenEditorHandler.ToSharedRef());
		UE::AssetDocument::Graph::Data::Material::RegisterOpenEditorHandlers();
	}

	virtual void ShutdownModule() override
	{
		UE::AssetDocument::Graph::Data::Material::UnregisterOpenEditorHandlers();
		if (DataTableOpenEditorHandler.IsValid())
		{
			UnregisterAssetDocumentOpenEditorHandler(DataTableOpenEditorHandler->GetId());
		}
		if (MaterialRebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(MaterialRebuilder->GetId());
		}
		if (EnumRebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(EnumRebuilder->GetId());
		}
		if (StructRebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(StructRebuilder->GetId());
		}
	}

	virtual FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentData"); }

private:
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> StructRebuilder;
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> EnumRebuilder;
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> MaterialRebuilder;
	TSharedPtr<IAssetDocumentOpenEditorHandler> DataTableOpenEditorHandler;
};

IMPLEMENT_MODULE(FAssetDocumentDataModule, AssetDocumentData)
