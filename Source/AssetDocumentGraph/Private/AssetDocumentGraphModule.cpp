// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Adapters/AssetDocumentGraphBuiltinAdapters.h"
#include "AssetDocumentGraphBlueprintOpenEditorHandler.h"
#include "AssetDocumentGraphBlueprintRebuilder.h"
#include "AssetDocumentGraphUsageScanner.h"
#include "Blueprint/AssetDocumentBlueprintNodeTypeLister.h"
#include "Blueprint/AssetDocumentBlueprintTrialTarget.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"
#include "AssetDocumentRuntimeModule.h"
#include "Modules/ModuleManager.h"

class FAssetDocumentGraphModule final : public IAssetDocumentRuntimeModule
{
public:
	virtual void StartupModule() override
	{
		using namespace UE::AssetDocument::Graph;
		// Registration order matters only for equal-depth ties (later wins).
		GenericSchema = MakeGenericSchemaAdapter();
		K2Schema = MakeK2SchemaAdapter();
		BlueprintHost = MakeBlueprintHostAdapter();
		RegisterAssetDocumentGraphSchemaAdapter(GenericSchema.ToSharedRef());
		RegisterAssetDocumentGraphSchemaAdapter(K2Schema.ToSharedRef());
		RegisterAssetDocumentGraphHostAdapter(BlueprintHost.ToSharedRef());
		// Core package-snapshot rollback of a Blueprint regenerates its derived
		// classes/CDO through this rebuilder.
		BlueprintRebuilder = MakeBlueprintDerivedStateRebuilder();
		RegisterAssetDocumentDerivedStateRebuilder(BlueprintRebuilder.ToSharedRef());
		HostedBlueprintRebuilder = MakeHostedBlueprintDerivedStateRebuilder();
		RegisterAssetDocumentDerivedStateRebuilder(HostedBlueprintRebuilder.ToSharedRef());
		// An open Blueprint editor shows every apply (closes the tabs of removed graphs, refreshes as its Undo does).
		BlueprintOpenEditorHandler = MakeBlueprintOpenEditorHandler();
		RegisterAssetDocumentOpenEditorHandler(BlueprintOpenEditorHandler.ToSharedRef());
		RegisterGraphUsageScanner();
		RegisterBlueprintNodeTypeLister();
		RegisterBlueprintTrialTargetMaker();
	}

	virtual void ShutdownModule() override
	{
		UE::AssetDocument::Graph::UnregisterBlueprintTrialTargetMaker();
		UE::AssetDocument::Graph::UnregisterBlueprintNodeTypeLister();
		UE::AssetDocument::Graph::UnregisterGraphUsageScanner();
		if (BlueprintOpenEditorHandler.IsValid())
		{
			UnregisterAssetDocumentOpenEditorHandler(BlueprintOpenEditorHandler->GetId());
		}
		if (HostedBlueprintRebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(HostedBlueprintRebuilder->GetId());
		}
		if (BlueprintRebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(BlueprintRebuilder->GetId());
		}
		if (BlueprintHost.IsValid())
		{
			UnregisterAssetDocumentGraphHostAdapter(BlueprintHost.ToSharedRef());
		}
		if (K2Schema.IsValid())
		{
			UnregisterAssetDocumentGraphSchemaAdapter(K2Schema.ToSharedRef());
		}
		if (GenericSchema.IsValid())
		{
			UnregisterAssetDocumentGraphSchemaAdapter(GenericSchema.ToSharedRef());
		}
	}

	virtual FName GetRuntimeModuleName() const override
	{
		return TEXT("AssetDocumentGraph");
	}

private:
	TSharedPtr<IAssetDocumentGraphSchemaAdapter> GenericSchema;
	TSharedPtr<IAssetDocumentGraphSchemaAdapter> K2Schema;
	TSharedPtr<IAssetDocumentGraphHostAdapter> BlueprintHost;
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> BlueprintRebuilder;
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> HostedBlueprintRebuilder;
	TSharedPtr<IAssetDocumentOpenEditorHandler> BlueprintOpenEditorHandler;
};

IMPLEMENT_MODULE(FAssetDocumentGraphModule, AssetDocumentGraph)
