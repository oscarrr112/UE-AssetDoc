// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetOpsTestTypes.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAssetOpsTestSupport
{
// Assets one test creates under /Game/__AssetDocumentTests__/AssetOps/<unique>; the destructor unloads
// them and deletes their files.
class FScopedTestAssets
{
public:
	FScopedTestAssets()
		: Root(FString::Printf(TEXT("/Game/__AssetDocumentTests__/AssetOps/T%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12)))
	{
	}

	~FScopedTestAssets()
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr && !GEditor->IsTransactionActive())
		{
			GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentAssetOpsTests", "Cleanup", "AssetOps test cleanup"));
		}
		TArray<UPackage*> Loaded;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			if (It->GetName().StartsWith(Root + TEXT("/")) && IsValid(*It))
			{
				It->SetDirtyFlag(false);
				Loaded.Add(*It);
			}
		}
		if (!Loaded.IsEmpty())
		{
			UPackageTools::UnloadPackages(Loaded);
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		const FString Directory = FPaths::ProjectContentDir() / Root.RightChop(FString(TEXT("/Game/")).Len());
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.*"), true, false, false);
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
		if (!Files.IsEmpty())
		{
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
		}
	}

	FScopedTestAssets(const FScopedTestAssets&) = delete;
	FScopedTestAssets& operator=(const FScopedTestAssets&) = delete;

	FString PackageName(const TCHAR* Name) const { return Root / Name; }
	FString ObjectPath(const TCHAR* Name) const { return Root / Name + TEXT(".") + Name; }
	FString File(const TCHAR* Name) const
	{
		return FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(PackageName(Name), FPackageName::GetAssetPackageExtension()));
	}

	UAssetDocumentAssetOpsTestRefAsset* NewRefAsset(const TCHAR* Name) const
	{
		UPackage* Package = CreatePackage(*PackageName(Name));
		UAssetDocumentAssetOpsTestRefAsset* Asset = NewObject<UAssetDocumentAssetOpsTestRefAsset>(Package, FName(Name),
			RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		return Asset;
	}

	// Saves the packages and rescans their files, so the asset registry knows their references.
	static bool Save(const TArray<UObject*>& Assets)
	{
		TArray<UPackage*> Packages;
		TArray<FString> Files;
		for (UObject* Asset : Assets)
		{
			Packages.Add(Asset->GetPackage());
			Files.Add(FPackageName::LongPackageNameToFilename(Asset->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension()));
		}
		const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ false);
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
		return bSaved;
	}

private:
	FString Root;
};

inline bool AssetExists(const FString& ObjectPath)
{
	UObject* Object = FSoftObjectPath(ObjectPath).ResolveObject();
	return (IsValid(Object) && !Object->IsA<UObjectRedirector>())
		|| FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
}
}
#endif
