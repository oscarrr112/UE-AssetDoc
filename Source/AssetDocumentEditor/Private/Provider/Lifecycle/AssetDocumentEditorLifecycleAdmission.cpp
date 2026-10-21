// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Lifecycle/AssetDocumentEditorLifecycleAdmission.h"

#include "Engine/DataAsset.h"
#include "Factories/DataAssetFactory.h"

namespace
{
bool IsDataAssetClassAdmitted(const UClass* AssetClass)
{
	return AssetClass != nullptr
		&& AssetClass->IsChildOf(UDataAsset::StaticClass())
		&& !AssetClass->HasAnyClassFlags(
			CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
}
}

bool IsAssetDocumentEditorBehaviorTreeFactoryClassAdmitted(
	const UClass* AssetClass)
{
	return AssetClass != nullptr
		&& AssetClass->GetPathName() == TEXT("/Script/AIModule.BehaviorTree")
		&& !AssetClass->HasAnyClassFlags(
			CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
}

bool IsAssetDocumentEditorScratchClassAdmitted(const UClass* AssetClass)
{
	return IsDataAssetClassAdmitted(AssetClass);
}

bool IsAssetDocumentEditorFactoryClassAdmitted(const UClass* AssetClass)
{
	return IsDataAssetClassAdmitted(AssetClass)
		|| IsAssetDocumentEditorBehaviorTreeFactoryClassAdmitted(AssetClass);
}

bool IsAssetDocumentEditorDataAssetFactoryAdmitted(
	const UFactory* Factory,
	const UClass* AssetClass)
{
	return Factory != nullptr
		&& Factory->GetClass() == UDataAssetFactory::StaticClass()
		&& IsDataAssetClassAdmitted(AssetClass);
}

UDataAssetFactory* GetAssetDocumentEditorTransientScratchFactory(
	const UClass* AssetClass)
{
	UDataAssetFactory* Factory =
		UDataAssetFactory::StaticClass()
			->GetDefaultObject<UDataAssetFactory>();
	if (!IsAssetDocumentEditorDataAssetFactoryAdmitted(
		Factory, AssetClass))
	{
		return nullptr;
	}
	return Factory->DataAssetClass == nullptr
			|| Factory->DataAssetClass == AssetClass
		? Factory
		: nullptr;
}
