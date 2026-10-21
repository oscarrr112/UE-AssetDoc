// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Lifecycle/AssetDocumentEditorOrphanPackageCleanup.h"

#include "AssetDocumentAssetCompilation.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "Utils/AssetDocumentEngineCompat.h"

bool DiscardAssetDocumentEditorOrphanCreatedPackage(const FString& PackageName)
{
	UPackage* Package = FindPackage(nullptr, *PackageName);
	if (Package == nullptr)
	{
		return true;
	}
	// Only a package that never reached disk is discarded here.
	if (FPackageName::DoesPackageExist(PackageName))
	{
		return false;
	}
	AssetDocumentAssetCompilation::FinishPackageCompilation({Package});
	TArray<UObject*> Objects;
	GetObjectsWithPackage(Package, Objects);
	for (UObject* Object : Objects)
	{
		if (Object->HasAnyFlags(RF_Standalone | RF_Public))
		{
			FAssetRegistryModule::AssetDeleted(Object);
		}
		Object->ClearFlags(RF_Standalone | RF_Public);
		Object->MarkAsGarbage();
	}
	ResetLoaders(Package);
	Package->SetDirtyFlag(false);
	Package->ClearFlags(RF_Standalone | RF_Public);
	const FName Discarded = MakeUniqueObjectName(
		nullptr, UPackage::StaticClass(), FName(TEXT("/Temp/AssetDocumentDiscardedPackage")));
	Package->Rename(*Discarded.ToString(), nullptr,
		REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
	Package->MarkAsGarbage();
	return FindPackage(nullptr, *PackageName) == nullptr;
}

bool DiscardAssetDocumentEditorEmptyUnsavedPackage(const FString& PackageName)
{
	UPackage* Package = FindPackage(nullptr, *PackageName);
	if (Package == nullptr || Package->IsDirty() || FPackageName::DoesPackageExist(PackageName))
	{
		return false;
	}
	TArray<UObject*> Objects;
	GetObjectsWithPackage(Package, Objects, AssetDocumentObjects::IncludeNested, RF_NoFlags, EInternalObjectFlags::Garbage);
	if (!Objects.IsEmpty())
	{
		return false;
	}
	return DiscardAssetDocumentEditorOrphanCreatedPackage(PackageName);
}
