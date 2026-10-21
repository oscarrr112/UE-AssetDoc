// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// edit_level organisation (design §7.2 L3): label, tags, folders (create, rename and
// delete with the Outliner's semantics), layers on a level without World Partition and
// data layers on a World Partition level; each with the editor's refusals.

#include "DataLayer/DataLayerEditorSubsystem.h"
#include "EditorActorFolders.h"
#include "Engine/PointLight.h"
#include "Folder.h"
#include "LevelEditTestSupport.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentLevelEditOrganisationTests
{
using namespace AssetDocumentLevelEditTests;

bool HasFolder(UWorld& World, const TCHAR* Path)
{
	return FActorFolders::Get().ContainsFolder(World, FFolder(FFolder::GetWorldRootFolder(&World).GetRootObject(), FName(Path)));
}

FName FolderOf(const FString& Path)
{
	const AActor* Actor = FindActorByPath(Path);
	return Actor != nullptr ? Actor->GetFolderPath() : FName(TEXT("<missing>"));
}

TArray<FString> LayersOf(const FString& Path)
{
	TArray<FString> Names;
	if (const AActor* Actor = FindActorByPath(Path))
	{
		for (const FName& Layer : Actor->Layers)
		{
			Names.Add(Layer.ToString());
		}
	}
	Names.Sort();
	return Names;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelEditOrganisationTest,
	"UEAssetDoc.Level.Edit.Organisation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelEditOrganisationTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditOrganisationTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	if (!TestNotNull(TEXT("map created"), World))
	{
		return false;
	}
	ULevel& Level = *World->PersistentLevel;
	AActor* SpawnedA = SpawnActor(Level, APointLight::StaticClass(), FVector(0, 0, 0), TEXT("A"));
	AActor* SpawnedB = SpawnActor(Level, APointLight::StaticClass(), FVector(100, 0, 0), TEXT("B"));
	SpawnedA->SetFolderPath(TEXT("Props/Lights"));
	SpawnedB->SetFolderPath(TEXT("Props"));
	const FString APath = GetActorPath(*SpawnedA);
	const FString BPath = GetActorPath(*SpawnedB);
	const FString CPath = GetActorPath(*SpawnActor(Level, APointLight::StaticClass(), FVector(200, 0, 0), TEXT("C")));
	ResetUndo();

	// Label, tags, a new empty folder, a folder for an actor, layers.
	const FLevelToolResult Organised = Edit(*this, FString::Printf(TEXT(R"({"description": "AI: organise", "ops": [
		{"op": "set", "actor": "%s", "label": "Lamp A", "tags": ["warm", "key"]},
		{"op": "create_folder", "path": "Empty/Sub"},
		{"op": "create_folder", "path": "Props"},
		{"op": "set", "actor": "%s", "folder": "Props/Lights"},
		{"op": "set", "actor": "%s", "layers": ["Lights", "Gameplay"]}]})"), *APath, *CPath, *BPath), TEXT("organise"));
	if (!TestTrue(TEXT("organise succeeds"), Organised.bSuccess))
	{
		return false;
	}
	const AActor* A = FindActorByPath(APath);
	TestEqual(TEXT("label"), A != nullptr ? A->GetActorLabel() : FString(), FString(TEXT("Lamp A")));
	TestTrue(TEXT("tags"), A != nullptr && A->Tags == TArray<FName>({TEXT("warm"), TEXT("key")}));
	TestTrue(TEXT("the new folder and its parent exist"), HasFolder(*World, TEXT("Empty/Sub")) && HasFolder(*World, TEXT("Empty")));
	TestNull(TEXT("an existing folder is no change"), FindChange(Organised, TEXT("/ops/2")).Get());
	TestEqual(TEXT("folder set"), FolderOf(CPath), FName(TEXT("Props/Lights")));
	TestEqual(TEXT("layers"), LayersOf(BPath), TArray<FString>({TEXT("Gameplay"), TEXT("Lights")}));

	// Rename moves the folder's actors and sub-folders.
	const FLevelToolResult Renamed = Edit(*this, TEXT(R"({"ops": [{"op": "rename_folder", "from": "Props", "to": "Stuff"}]})"),
		TEXT("rename folder"));
	TestTrue(TEXT("rename succeeds"), Renamed.bSuccess);
	TestEqual(TEXT("actor in a sub-folder follows"), FolderOf(APath), FName(TEXT("Stuff/Lights")));
	TestEqual(TEXT("actor in the folder follows"), FolderOf(BPath), FName(TEXT("Stuff")));
	TestFalse(TEXT("the old folder is gone"), HasFolder(*World, TEXT("Props")));
	TestTrue(TEXT("the new sub-folder exists"), HasFolder(*World, TEXT("Stuff/Lights")));

	// Delete moves the folder's actors and sub-folders up to its parent (the Outliner's Delete).
	const FLevelToolResult Deleted = Edit(*this, TEXT(R"({"ops": [{"op": "delete_folder", "path": "Stuff"}]})"), TEXT("delete folder"));
	TestTrue(TEXT("delete succeeds"), Deleted.bSuccess);
	TestEqual(TEXT("its actor moves to the root"), FolderOf(BPath), FName());
	TestEqual(TEXT("its sub-folder moves up"), FolderOf(APath), FName(TEXT("Lights")));
	TestFalse(TEXT("the folder is gone"), HasFolder(*World, TEXT("Stuff")));
	TestNotNull(TEXT("no actor is deleted"), FindActorByPath(BPath));

	// Refusals.
	const int32 QueueBefore = UndoQueueLength();
	{
		const FLevelToolResult Missing = Edit(*this, TEXT(R"({"ops": [{"op": "rename_folder", "from": "Nope", "to": "Other"}]})"),
			TEXT("unknown folder"));
		const FAssetDocumentDiagnostic* NotFound = FindCode(Missing, TEXT("Level.FolderNotFound"));
		const TArray<TSharedPtr<FJsonValue>>* Known = nullptr;
		TestTrue(TEXT("unknown folder: the known folders"), NotFound != nullptr && NotFound->Details.IsValid()
			&& NotFound->Details->TryGetArrayField(TEXT("folders"), Known) && JsonText(MakeShared<FJsonValueArray>(*Known)).Contains(TEXT("Lights")));
		TestTrue(TEXT("rename onto an existing folder"), HasCode(Edit(*this, TEXT(R"({"ops": [
			{"op": "rename_folder", "from": "Lights", "to": "Empty"}]})"), TEXT("rename onto existing")), TEXT("Level.InvalidRequest"),
			TEXT("/ops/0/to")));
		TestTrue(TEXT("rename into itself"), HasCode(Edit(*this, TEXT(R"({"ops": [
			{"op": "rename_folder", "from": "Empty", "to": "Empty/Inner"}]})"), TEXT("rename into itself")), TEXT("Level.InvalidRequest"),
			TEXT("/ops/0/to")));
		TestTrue(TEXT("data layers on a level without World Partition"), HasCode(Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "set", "actor": "%s", "data_layers": ["Anything"]}]})"), *APath), TEXT("data layers")), TEXT("Level.DataLayersUnavailable"),
			TEXT("/ops/0/data_layers")));
		// A numbered folder name the plan cannot know: deleting "Empty" moves "Empty/Sub" up
		// as "Sub1" because "Sub" exists; renaming onto it fails cleanly in apply (the
		// engine would hit a check()) and the batch rolls back.
		const FLevelToolResult Collision = Edit(*this, TEXT(R"({"ops": [
			{"op": "create_folder", "path": "Sub"},
			{"op": "delete_folder", "path": "Empty"},
			{"op": "rename_folder", "from": "Lights", "to": "Sub1"}]})"), TEXT("rename onto an engine-numbered folder"));
		TestTrue(TEXT("collision refused at /to"), HasCode(Collision, TEXT("Level.InvalidRequest"), TEXT("/ops/2/to")));
		TestTrue(TEXT("collision rolled back"), PayloadBool(Collision, TEXT("rolled_back")));
		TestTrue(TEXT("folders restored after the rollback"), HasFolder(*World, TEXT("Empty/Sub")) && !HasFolder(*World, TEXT("Sub1"))
			&& HasFolder(*World, TEXT("Lights")));
		TestEqual(TEXT("refusals add no Undo entry"), UndoQueueLength(), QueueBefore);
	}

	// Each request is one Undo entry.
	TestTrue(TEXT("Undo delete, rename, organise"), GEditor->UndoTransaction() && GEditor->UndoTransaction() && GEditor->UndoTransaction());
	TestEqual(TEXT("folders restored"), FolderOf(APath), FName(TEXT("Props/Lights")));
	TestEqual(TEXT("folder restored"), FolderOf(CPath), FName());
	TestEqual(TEXT("label restored"), FindActorByPath(APath) != nullptr ? FindActorByPath(APath)->GetActorLabel() : FString(), FString(TEXT("A")));
	TestEqual(TEXT("layers restored"), LayersOf(BPath), TArray<FString>());

	// Data layers on a World Partition level (layers are not used there).
	UWorld* Open = Maps.NewMap(TEXT("Open"), true);
	UDataLayerEditorSubsystem* DataLayers = UDataLayerEditorSubsystem::Get();
	if (!TestNotNull(TEXT("partitioned map created"), Open) || !TestNotNull(TEXT("data layer subsystem"), DataLayers))
	{
		return false;
	}
	FDataLayerCreationParameters Creation;
	Creation.bIsPrivate = true;
	const TWeakObjectPtr<UDataLayerInstance> Instance = DataLayers->CreateDataLayerInstance(Creation);
	if (!TestTrue(TEXT("a private data layer"), Instance.IsValid()))
	{
		return false;
	}
	const FString LayerName = Instance->GetDataLayerShortName();
	ResetUndo();
	const FLevelToolResult Layered = Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "spawn", "id": "d", "class": "/Script/Engine.PointLight", "data_layers": ["%s"]}]})"), *LayerName), TEXT("data layers"));
	TestTrue(TEXT("spawn into a data layer"), Layered.bSuccess);
	const FString DPath = BatchPath(Layered, TEXT("d"));
	const AActor* D = FindActorByPath(DPath);
	TestTrue(TEXT("in the data layer"), D != nullptr && D->GetDataLayerInstances().Contains(Instance.Get()));
	const FLevelToolResult Cleared = Edit(*this, FString::Printf(TEXT(R"({"ops": [{"op": "set", "actor": "%s", "data_layers": []}]})"),
		*DPath), TEXT("clear data layers"));
	TestTrue(TEXT("clear data layers"), Cleared.bSuccess);
	D = FindActorByPath(DPath);
	TestTrue(TEXT("in no data layer"), D != nullptr && D->GetDataLayerInstances().IsEmpty());
	const FLevelToolResult Unknown = Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "set", "actor": "%s", "data_layers": ["NoSuchLayer"]},
		{"op": "set", "actor": "%s", "layers": ["Lights"]}]})"), *DPath, *DPath), TEXT("data layer refusals"));
	const FAssetDocumentDiagnostic* NotFoundLayer = FindCode(Unknown, TEXT("Level.DataLayerNotFound"));
	TestTrue(TEXT("unknown data layer at its position"), NotFoundLayer != nullptr && NotFoundLayer->Path == TEXT("/ops/0/data_layers/0"));
	TestTrue(TEXT("layers on a World Partition level"), HasCode(Unknown, TEXT("Level.LayersUnavailable"), TEXT("/ops/1/layers")));
	TestTrue(TEXT("Undo clear"), GEditor->UndoTransaction());
	D = FindActorByPath(DPath);
	TestTrue(TEXT("Undo restores the data layer"), D != nullptr && D->GetDataLayerInstances().Contains(Instance.Get()));

	// Folders are objects with their own package on World Partition levels: delete marks
	// the folder deleted and its sub-folder resolves through the parent.
	const FLevelToolResult Placed = Edit(*this, TEXT(R"({"ops": [
		{"op": "spawn", "id": "e", "class": "/Script/Engine.PointLight", "folder": "Props/Lights"},
		{"op": "spawn", "id": "f", "class": "/Script/Engine.PointLight", "folder": "Props"}]})"), TEXT("WP folders"));
	const FString EPath = BatchPath(Placed, TEXT("e"));
	const FString FPath = BatchPath(Placed, TEXT("f"));
	TestTrue(TEXT("WP folders created"), Placed.bSuccess && HasFolder(*Open, TEXT("Props/Lights")));
	const FLevelToolResult WPDeleted = Edit(*this, TEXT(R"({"ops": [{"op": "delete_folder", "path": "Props"}]})"), TEXT("WP delete folder"));
	TestTrue(TEXT("WP delete folder succeeds"), WPDeleted.bSuccess);
	TestEqual(TEXT("WP: the sub-folder's actor moves up"), FolderOf(EPath), FName(TEXT("Lights")));
	TestEqual(TEXT("WP: the folder's actor moves to the root"), FolderOf(FPath), FName());
	TestFalse(TEXT("WP: the folder is gone"), HasFolder(*Open, TEXT("Props")));
	TestTrue(TEXT("WP: the sub-folder resolves through the parent"), HasFolder(*Open, TEXT("Lights")));
	TestTrue(TEXT("WP: the deleted folder's package is dirty"), PayloadStrings(WPDeleted, TEXT("dirty_packages")).ContainsByPredicate(
		[](const FString& Package) { return Package.Contains(TEXT("__ExternalObjects__")); }));
	TestTrue(TEXT("WP: Undo delete folder"), GEditor->UndoTransaction());
	TestEqual(TEXT("WP: Undo restores the folder"), FolderOf(EPath), FName(TEXT("Props/Lights")));
	ResetUndo();
	return true;
}

#endif
