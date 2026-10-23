// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentCompileStatus.h"
#include "AssetDocumentEditorDialogFactory.h"
#include "AssetDocumentPhysicsPrimitives.h"
#include "AssetDocumentRuntimeModule.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/PhysicsAssetFactory.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

namespace
{
FTopLevelAssetPath PhysicsAssetFactoryPath()
{
	return FTopLevelAssetPath(TEXT("/Script/UnrealEd"), TEXT("PhysicsAssetFactory"));
}

// What UPhysicsAssetFactory does after its "New Physics Asset" dialog (CreatePhysicsAssetFromMesh), up to the
// generation: the new asset targets the factory's skeletal mesh (FPhysicsAssetUtils::CreateFromSkeletalMesh starts by
// setting it as the preview mesh). The bodies are the document's: its Bodies, or the editor's generation with the
// document's Generation settings (physics.asset.edit.v1 "generate"). The asset is never assigned to the mesh
// (design §8 decision 2: the mesh is another package the document does not name).
bool InitPhysicsAssetFromFactory(const UFactory& Factory, UObject& NewAsset, FString& OutMessage)
{
	const UPhysicsAssetFactory* PhysicsFactory = Cast<UPhysicsAssetFactory>(&Factory);
	UPhysicsAsset* Asset = Cast<UPhysicsAsset>(&NewAsset);
	if (PhysicsFactory == nullptr || Asset == nullptr)
	{
		OutMessage = TEXT("the Physics Asset factory makes Physics Assets only");
		return false;
	}
	if (PhysicsFactory->TargetSkeletalMesh == nullptr)
	{
		OutMessage = TEXT("a physics asset is made for a skeletal mesh (FactoryContext)");
		return false;
	}
	Asset->PreviewSkeletalMesh = PhysicsFactory->TargetSkeletalMesh.Get();
	return true;
}
}

// The "physics" provider is assembled by the runtime bootstrap. The module registers the Physics Asset status
// describer (data validation and structural checks, design §5), the rollback rebuilder (what load and the editor's
// Undo rebuild), what the Physics Asset factory does after its modal dialog (design §4.3: create) and the open-editor
// apply handler (AssetDocumentPhysicsOpenEditor.cpp: close, apply, reopen).
class FAssetDocumentPhysicsModule final : public IAssetDocumentRuntimeModule
{
public:
	virtual void StartupModule() override
	{
		using namespace UE::AssetDocument::Graph::Physics;
		RegisterAssetDocumentCompileStatusDescriber(CompileDescriberId, &DescribeStatus);
		Rebuilder = MakePhysicsRebuilder();
		RegisterAssetDocumentDerivedStateRebuilder(Rebuilder.ToSharedRef());
		RegisterAssetDocumentEditorDialogFactory(PhysicsAssetFactoryPath(), &InitPhysicsAssetFromFactory);
		OpenEditorHandler = MakeOpenEditorHandler();
		RegisterAssetDocumentOpenEditorHandler(OpenEditorHandler.ToSharedRef());
	}

	virtual void ShutdownModule() override
	{
		if (OpenEditorHandler.IsValid())
		{
			UnregisterAssetDocumentOpenEditorHandler(OpenEditorHandler->GetId());
			OpenEditorHandler.Reset();
		}
		UnregisterAssetDocumentEditorDialogFactory(PhysicsAssetFactoryPath());
		if (Rebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(Rebuilder->GetId());
			Rebuilder.Reset();
		}
		UnregisterAssetDocumentCompileStatusDescriber(CompileDescriberId);
	}

	virtual FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentPhysics"); }

private:
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> Rebuilder;
	TSharedPtr<IAssetDocumentOpenEditorHandler> OpenEditorHandler;
	static inline const FName CompileDescriberId = TEXT("AssetDocumentPhysics.PhysicsAsset");
};

IMPLEMENT_MODULE(FAssetDocumentPhysicsModule, AssetDocumentPhysics)
