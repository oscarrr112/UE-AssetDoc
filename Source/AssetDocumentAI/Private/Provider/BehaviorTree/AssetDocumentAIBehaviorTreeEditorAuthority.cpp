// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeEditorModule.h"
#include "BehaviorTreeFactory.h"
#include "Editor.h"
#include "IBehaviorTreeEditor.h"
#include "Modules/ModuleManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UObjectGlobals.h"

namespace AssetDocumentAIBehaviorTreeEditorAuthority
{
bool IsBootstrapTreeValid(const UBehaviorTree& BootstrapTree)
{
	return BootstrapTree.GetClass() == UBehaviorTree::StaticClass()
		&& BootstrapTree.GetOuter() == GetTransientPackage()
		&& BootstrapTree.HasAllFlags(RF_Transient | RF_Transactional)
		&& !BootstrapTree.HasAnyFlags(RF_Public | RF_Standalone)
		&& !BootstrapTree.IsAsset();
}
} // namespace AssetDocumentAIBehaviorTreeEditorAuthority

bool EnsureAssetDocumentAIBehaviorTreeEditorAuthority()
{
	using namespace AssetDocumentAIBehaviorTreeEditorAuthority;

	if (!IsInGameThread() || !GIsEditor || IsRunningCommandlet() || GEditor == nullptr)
	{
		return false;
	}

	UAssetEditorSubsystem* AssetEditorSubsystem =
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (AssetEditorSubsystem == nullptr)
	{
		return false;
	}

	FBehaviorTreeEditorModule* BehaviorTreeEditorModule =
		FModuleManager::LoadModulePtr<FBehaviorTreeEditorModule>(
			TEXT("BehaviorTreeEditor"));
	if (BehaviorTreeEditorModule == nullptr)
	{
		return false;
	}
	if (BehaviorTreeEditorModule->GetClassCache().IsValid())
	{
		return true;
	}

	UBehaviorTreeFactory* Factory = UBehaviorTreeFactory::StaticClass()
		->GetDefaultObject<UBehaviorTreeFactory>();
	if (Factory == nullptr)
	{
		return false;
	}

	const FName BootstrapName = MakeUniqueObjectName(
		GetTransientPackage(),
		UBehaviorTree::StaticClass(),
		TEXT("AssetDocumentAIEditorAuthorityBootstrap"));
	UBehaviorTree* BootstrapTree = Cast<UBehaviorTree>(Factory->FactoryCreateNew(
		UBehaviorTree::StaticClass(),
		GetTransientPackage(),
		BootstrapName,
		RF_Transient | RF_Transactional,
		nullptr,
		nullptr));
	if (BootstrapTree == nullptr || !IsBootstrapTreeValid(*BootstrapTree))
	{
		return false;
	}

	const int32 OpenEditorCountBefore =
		AssetEditorSubsystem->GetAllOpenEditors().Num();
	const int32 EditedAssetCountBefore =
		AssetEditorSubsystem->GetAllEditedAssets().Num();
	TSharedPtr<IBehaviorTreeEditor> Toolkit =
		BehaviorTreeEditorModule->CreateBehaviorTreeEditor(
			EToolkitMode::Standalone,
			nullptr,
			BootstrapTree);
	TWeakPtr<IBehaviorTreeEditor> WeakToolkit = Toolkit;
	const bool bCacheReady = Toolkit.IsValid()
		&& BehaviorTreeEditorModule->GetClassCache().IsValid();
	const bool bCloseAccepted = Toolkit.IsValid()
		&& Toolkit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
	Toolkit.Reset();

	const bool bNoToolkitResidue = !WeakToolkit.IsValid()
		&& AssetEditorSubsystem->FindEditorForAsset(BootstrapTree, false) == nullptr
		&& AssetEditorSubsystem->GetAllEditedAssets().Num() == EditedAssetCountBefore
		&& AssetEditorSubsystem->GetAllOpenEditors().Num() == OpenEditorCountBefore;
	return bCacheReady
		&& bCloseAccepted
		&& BehaviorTreeEditorModule->GetClassCache().IsValid()
		&& bNoToolkitResidue;
}
