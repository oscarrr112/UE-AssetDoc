// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/Level.h"
#include "Engine/LevelStreamingAlwaysLoaded.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "LevelEditorSubsystem.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentLevelTests
{
inline TSharedPtr<FJsonObject> ParseJsonObject(const FString& Text)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	FJsonSerializer::Deserialize(Reader, Json);
	return Json;
}

inline UWorld* GetEditorWorld()
{
	return GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
}

// Spawns like the editor's Place Actors path (UEditorEngine::AddActor): transactional,
// into the given level, not selected.
inline AActor* SpawnActor(ULevel& Level, UClass* Class, const FVector& Location, const TCHAR* Label)
{
	AActor* Actor = GEditor->AddActor(&Level, Class, FTransform(Location), /*bSilent*/ true, RF_Transactional, /*bSelectActor*/ false);
	if (Actor != nullptr && Label != nullptr)
	{
		Actor->SetActorLabel(Label);
	}
	return Actor;
}

// The maps one test creates under /Game/__AssetDocumentTests__/Level/<unique>. The
// destructor switches the editor to a new untitled map and deletes the map files and
// their external actor / object packages from disk.
class FScopedTestMaps
{
public:
	FScopedTestMaps()
		: Root(FString::Printf(TEXT("/Game/__AssetDocumentTests__/Level/T%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12)))
	{
	}

	~FScopedTestMaps()
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		if (GEditor != nullptr)
		{
			if (GEditor->PlayWorld != nullptr)
			{
				GEditor->EndPlayMap();
			}
			GEditor->NewMap(false);
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TArray<FString> Files;
		const FString RelativeRoot = Root.RightChop(FString(TEXT("/Game/")).Len());
		const TArray<FString> Directories = {
			FPaths::ProjectContentDir() / RelativeRoot,
			FPaths::ProjectContentDir() / TEXT("__ExternalActors__") / RelativeRoot,
			FPaths::ProjectContentDir() / TEXT("__ExternalObjects__") / RelativeRoot,
		};
		for (const FString& Directory : Directories)
		{
			IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.*"), true, false, false);
		}
		for (const FString& Directory : Directories)
		{
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
		}
		if (!Files.IsEmpty())
		{
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
		}
	}

	FScopedTestMaps(const FScopedTestMaps&) = delete;
	FScopedTestMaps& operator=(const FScopedTestMaps&) = delete;

	FString PackageName(const TCHAR* Name) const
	{
		return Root / Name;
	}

	// A new saved map at <root>/<Name>, opened as the editor world.
	UWorld* NewMap(const TCHAR* Name, bool bPartitioned) const
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		ULevelEditorSubsystem* LevelEditor = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
		return LevelEditor != nullptr && LevelEditor->NewLevel(PackageName(Name), bPartitioned) ? GetEditorWorld() : nullptr;
	}

	// A new saved sublevel <root>/<Name> added to World (always loaded); the persistent
	// level stays the current level.
	ULevel* AddSublevel(UWorld& World, const TCHAR* Name) const
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName(Name), FPackageName::GetMapPackageExtension());
		ULevelStreaming* Streaming = UEditorLevelUtils::CreateNewStreamingLevelForWorld(World, ULevelStreamingAlwaysLoaded::StaticClass(),
			Filename, /*bMoveSelectedActorsIntoNewLevel*/ false, /*InTemplateWorld*/ nullptr, /*bInUseSaveAs*/ false);
		World.SetCurrentLevel(World.PersistentLevel);
		return Streaming != nullptr ? Streaming->GetLoadedLevel() : nullptr;
	}

	// Saves the map package and every dirty external actor package of its persistent level.
	static bool SaveWorld(UWorld& World)
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		TArray<UPackage*> Packages = {World.GetPackage()};
		for (AActor* Actor : World.PersistentLevel->Actors)
		{
			if (Actor != nullptr && Actor->GetExternalPackage() != nullptr)
			{
				Packages.AddUnique(Actor->GetExternalPackage());
			}
		}
		for (UPackage* Package : World.PersistentLevel->GetLoadedExternalObjectPackages())
		{
			Packages.AddUnique(Package);
		}
		return FEditorFileUtils::PromptForCheckoutAndSave(Packages, /*bCheckDirty*/ false, /*bPromptToSave*/ false)
			== FEditorFileUtils::PR_Success;
	}

	// Opens the saved map <root>/<Name> as the editor world.
	UWorld* LoadMap(const TCHAR* Name) const
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName(Name), FPackageName::GetMapPackageExtension());
		return FEditorFileUtils::LoadMap(Filename, /*LoadAsTemplate*/ false, /*bShowProgress*/ false) ? GetEditorWorld() : nullptr;
	}

private:
	FString Root;
};
}
#endif
