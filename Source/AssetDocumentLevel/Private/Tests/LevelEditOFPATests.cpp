// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// edit_level on a World Partition map with one file per actor (OFPA): dirty_packages are
// the external packages of the actors the batch changed, "save" writes exactly those
// (the map package stays untouched), deleting and saving removes the actor's file, and a spawn that is undone leaves nothing for a later save to write.
// NavigationSaved: the navigation mesh the engine adds for a first Nav Mesh Bounds Volume is saved with the batch.

#include "Engine/PointLight.h"
#include "EngineUtils.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationSystem.h"
#include "LevelEditTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentLevelEditOFPATests
{
FString PackageFile(const FString& PackageName)
{
	return FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
}

FString ExternalPackage(const FString& ActorPath)
{
	const AActor* Actor = AssetDocumentLevelEditTests::FindActorByPath(ActorPath);
	return Actor != nullptr && Actor->GetExternalPackage() != nullptr ? Actor->GetExternalPackage()->GetName() : FString();
}

// The package files of the test maps' actors and objects on disk.
TArray<FString> ExternalFiles(const FString& MapPackage)
{
	const FString Relative = FPaths::GetPath(MapPackage).RightChop(FString(TEXT("/Game/")).Len());
	TArray<FString> Files;
	for (const TCHAR* Root : {TEXT("__ExternalActors__"), TEXT("__ExternalObjects__")})
	{
		IFileManager::Get().FindFilesRecursive(Files, *(FPaths::ProjectContentDir() / Root / Relative), TEXT("*.uasset"), true, false, false);
	}
	Files.Sort();
	return Files;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelOFPADirtyAndSaveTest,
	"UEAssetDoc.Level.OFPA.DirtyAndSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelOFPADirtyAndSaveTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	using namespace AssetDocumentLevelEditOFPATests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Open"), true);
	if (!TestNotNull(TEXT("partitioned map created"), World))
	{
		return false;
	}
	const FString MapPackage = Maps.PackageName(TEXT("Open"));
	const FString APath = GetActorPath(*SpawnActor(*World->PersistentLevel, APointLight::StaticClass(), FVector(0, 0, 100), TEXT("A")));
	const FString BPath = GetActorPath(*SpawnActor(*World->PersistentLevel, APointLight::StaticClass(), FVector(300, 0, 100), TEXT("B")));
	if (!TestTrue(TEXT("map saved"), FScopedTestMaps::SaveWorld(*World)))
	{
		return false;
	}
	const FString APackage = ExternalPackage(APath);
	const FString BPackage = ExternalPackage(BPath);
	TestTrue(TEXT("OFPA: each actor has its own file"), !APackage.IsEmpty() && IFileManager::Get().FileExists(*PackageFile(APackage))
		&& !BPackage.IsEmpty() && IFileManager::Get().FileExists(*PackageFile(BPackage)));
	ResetUndo();
	const FDateTime MapStamp = IFileManager::Get().GetTimeStamp(*FPackageName::LongPackageNameToFilename(MapPackage,
		FPackageName::GetMapPackageExtension()));

	// A change and a spawn, saved: dirty_packages are the two external packages, and the
	// save writes exactly those.
	const FLevelToolResult Changed = Edit(*this, FString::Printf(TEXT(R"({"save": true, "ops": [
		{"op": "set", "actor": "%s", "label": "Lamp A"},
		{"op": "spawn", "id": "c", "class": "/Script/Engine.PointLight", "label": "C"}]})"), *APath), TEXT("change and spawn, saved"));
	if (!TestTrue(TEXT("change and spawn succeed"), Changed.bSuccess))
	{
		return false;
	}
	const FString CPackage = ExternalPackage(BatchPath(Changed, TEXT("c")));
	TArray<FString> Expected = {APackage, CPackage};
	Expected.Sort();
	TestEqual(TEXT("dirty_packages: the two external packages"), PayloadStrings(Changed, TEXT("dirty_packages")), Expected);
	TArray<FString> SavedPackages = PayloadStrings(Changed, TEXT("saved_packages"));
	SavedPackages.Sort();
	TestEqual(TEXT("saved_packages: exactly those"), SavedPackages, Expected);
	TestTrue(TEXT("the new actor's file is written"), IFileManager::Get().FileExists(*PackageFile(CPackage)));
	const UPackage* Map = FindPackage(nullptr, *MapPackage);
	TestTrue(TEXT("the map package is not dirty"), Map != nullptr && !Map->IsDirty());
	TestEqual(TEXT("the map file is not rewritten"), IFileManager::Get().GetTimeStamp(*FPackageName::LongPackageNameToFilename(MapPackage,
		FPackageName::GetMapPackageExtension())), MapStamp);
	for (const FString& Package : Expected)
	{
		const UPackage* Loaded = FindPackage(nullptr, *Package);
		TestTrue(*FString::Printf(TEXT("%s saved (not dirty)"), *Package), Loaded != nullptr && !Loaded->IsDirty());
	}

	// Delete and save: the actor's file is removed (as the editor's save of a deleted
	// OFPA actor does).
	const FLevelToolResult Deleted = Edit(*this, FString::Printf(TEXT(R"({"save": true, "ops": [
		{"op": "delete", "actors": ["%s"]}]})"), *BPath), TEXT("delete, saved"));
	TestTrue(TEXT("delete succeeds"), Deleted.bSuccess);
	TestEqual(TEXT("dirty_packages: the deleted actor's package"), PayloadStrings(Deleted, TEXT("dirty_packages")), TArray<FString>({BPackage}));
	TestEqual(TEXT("saved_packages: the deleted actor's package"), PayloadStrings(Deleted, TEXT("saved_packages")), TArray<FString>({BPackage}));
	TestFalse(TEXT("its file is deleted"), IFileManager::Get().FileExists(*PackageFile(BPackage)));
	TestNull(TEXT("no Level.SaveFailed"), FindCode(Deleted, TEXT("Level.SaveFailed")));

	// A folder created earlier and not saved: saving an actor placed in it also saves the
	// folder's package (else the actor would lose its folder on reload).
	const FLevelToolResult Created = Edit(*this, TEXT(R"({"ops": [{"op": "create_folder", "path": "Fresh/Inner"}]})"), TEXT("folder, not saved"));
	TestTrue(TEXT("folder created"), Created.bSuccess);
	const FLevelToolResult Into = Edit(*this, TEXT(R"({"save": true, "ops": [
		{"op": "spawn", "id": "g", "class": "/Script/Engine.PointLight", "folder": "Fresh/Inner"}]})"), TEXT("spawn into it, saved"));
	TestTrue(TEXT("spawn into the folder succeeds"), Into.bSuccess);
	const TArray<FString> IntoSaved = PayloadStrings(Into, TEXT("saved_packages"));
	const int32 FolderPackages = IntoSaved.FilterByPredicate([](const FString& Package)
	{
		return Package.Contains(TEXT("__ExternalObjects__"));
	}).Num();
	TestEqual(TEXT("the folder chain's packages are saved with the actor"), FolderPackages, 2);
	TestEqual(TEXT("they were already dirty"), PayloadStrings(Into, TEXT("already_dirty")).Num(), 2);
	for (const FString& Package : IntoSaved)
	{
		TestTrue(*FString::Printf(TEXT("%s written"), *Package), IFileManager::Get().FileExists(*PackageFile(Package)));
	}

	// A spawn that is undone leaves nothing for a later save.
	const TArray<FString> FilesBefore = ExternalFiles(MapPackage);
	const FLevelToolResult Spawned = Edit(*this, TEXT(R"({"ops": [{"op": "spawn", "id": "e", "class": "/Script/Engine.PointLight"}]})"),
		TEXT("spawn, not saved"));
	TestTrue(TEXT("spawn succeeds"), Spawned.bSuccess);
	const FString EPackage = ExternalPackage(BatchPath(Spawned, TEXT("e")));
	TestEqual(TEXT("dirty_packages: the new actor's package"), PayloadStrings(Spawned, TEXT("dirty_packages")), TArray<FString>({EPackage}));
	TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
	// Save All, restricted to this map's packages: every dirty package of the world.
	TArray<UPackage*> Dirty;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		const FString Name = It->GetName();
		if (It->IsDirty() && (Name == MapPackage || Name.Contains(FPaths::GetPath(MapPackage).RightChop(FString(TEXT("/Game/")).Len()))))
		{
			Dirty.Add(*It);
		}
	}
	if (!Dirty.IsEmpty())
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		FEditorFileUtils::PromptForCheckoutAndSave(Dirty, /*bCheckDirty*/ true, /*bPromptToSave*/ false);
	}
	TestEqual(TEXT("no new file after Undo and save"), ExternalFiles(MapPackage), FilesBefore);
	TestFalse(TEXT("the undone actor has no file"), IFileManager::Get().FileExists(*PackageFile(EPackage)));
	ResetUndo();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelOFPANavigationSavedTest,
	"UEAssetDoc.Level.OFPA.NavigationSaved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelOFPANavigationSavedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	using namespace AssetDocumentLevelEditOFPATests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Nav"), true);
	if (!TestNotNull(TEXT("partitioned map created"), World) || !TestTrue(TEXT("map saved"), FScopedTestMaps::SaveWorld(*World)))
	{
		return false;
	}
	ResetUndo();
	if (!TestNotNull(TEXT("the editor world has a navigation system"), FNavigationSystem::GetCurrent<UNavigationSystemV1>(World)))
	{
		return false;
	}
	TestFalse(TEXT("no navigation mesh before"), TActorIterator<ARecastNavMesh>(World) ? true : false);

	// The first Nav Mesh Bounds Volume, saved: the RecastNavMesh the engine adds for it is saved with it.
	const FLevelToolResult Placed = Edit(*this, TEXT(R"({"save": true, "ops": [
		{"op": "spawn", "id": "nav", "class": "/Script/NavigationSystem.NavMeshBoundsVolume",
		 "transform": {"location": {"X": 0, "Y": 0, "Z": 0}, "scale": {"X": 10, "Y": 10, "Z": 2}}}]})"), TEXT("nav bounds, saved"));
	if (!TestTrue(TEXT("spawn succeeds"), Placed.bSuccess))
	{
		return false;
	}
	const TArray<FString> Saved = PayloadStrings(Placed, TEXT("saved_packages"));
	TArray<ARecastNavMesh*> NavData;
	for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
	{
		NavData.Add(*It);
	}
	if (!TestEqual(TEXT("the engine added its navigation mesh"), NavData.Num(), 1))
	{
		return false;
	}
	const UPackage* NavPackage = NavData[0]->GetPackage();
	TestTrue(FString::Printf(TEXT("its package %s is among the saved packages (%s)"), *NavPackage->GetName(), *FString::Join(Saved, TEXT(", "))),
		Saved.Contains(NavPackage->GetName()));
	TestFalse(TEXT("its package is saved (not dirty)"), NavPackage->IsDirty());
	TestTrue(TEXT("its file is written"), IFileManager::Get().FileExists(*PackageFile(NavPackage->GetName())));
	TestTrue(TEXT("dirty_packages lists it"), PayloadStrings(Placed, TEXT("dirty_packages")).Contains(NavPackage->GetName()));
	TestNull(TEXT("no Level.SaveFailed"), FindCode(Placed, TEXT("Level.SaveFailed")));
	ResetUndo();
	return true;
}

#endif
