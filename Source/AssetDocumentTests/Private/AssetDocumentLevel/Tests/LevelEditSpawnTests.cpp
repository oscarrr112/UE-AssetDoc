// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// edit_level spawn: the placement factory is the editor's (actor class -> its factory
// or AddActor; static mesh -> AStaticMeshActor; Blueprint -> its generated class), the
// transform is stored exactly as given, sublevel targeting, refusals.

#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "LevelEditTestSupport.h"
#include "LevelEditTestTypes.h"
#include "LevelUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelEditSpawnClassTest,
	"UEAssetDoc.Level.Edit.SpawnClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelEditSpawnClassTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	if (!TestNotNull(TEXT("map created"), World))
	{
		return false;
	}
	ULevel* Sub = Maps.AddSublevel(*World, TEXT("Sub"));
	if (!TestNotNull(TEXT("sublevel created"), Sub))
	{
		return false;
	}
	const FString SubPackage = Maps.PackageName(TEXT("Sub"));
	ResetUndo();

	const FLevelToolResult Result = Edit(*this, FString::Printf(TEXT(R"({"description": "AI: lights", "ops": [
		{"op": "spawn", "id": "key", "class": "/Script/Engine.PointLight", "label": "KeyLight", "folder": "Room/Lights",
		 "tags": ["lit", "key"],
		 "transform": {"location": {"X": 10.5, "Y": -20, "Z": 280}, "rotation": {"Pitch": -20, "Yaw": 45}, "scale": {"X": 2, "Y": 2, "Z": 2}},
		 "components": {"LightComponent0": {"properties": {"Intensity": 8000, "AttenuationRadius": 1200,
		                                                   "LightColor": {"R": 255, "G": 214, "B": 170, "A": 255}}}}},
		{"op": "spawn", "id": "probe", "class": "/Script/AssetDocumentTests.AssetDocumentLevelTestActor", "label": "Probe",
		 "transform": {"location": {"Z": 50}}, "properties": {"Score": 12.5}},
		{"op": "spawn", "id": "subLight", "class": "/Script/Engine.PointLight", "level": "%s", "label": "SubLight"}
	]})"), *SubPackage), TEXT("spawn classes"));
	if (!TestTrue(TEXT("spawn succeeds"), Result.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("HTTP 200"), Result.HttpStatus, 200);
	TestEqual(TEXT("one Undo entry"), UndoQueueLength(), 1);
	TestEqual(TEXT("titled by description"), NewestUndoTitle(), FString(TEXT("AI: lights")));

	APointLight* Key = Cast<APointLight>(BatchActor(Result, TEXT("key")));
	if (TestNotNull(TEXT("key light spawned"), Key))
	{
		TestEqual(TEXT("class via the editor's light factory"), Key->GetClass(), APointLight::StaticClass());
		TestEqual(TEXT("persistent level (the current level)"), Key->GetLevel(), World->PersistentLevel.Get());
		TestEqual(TEXT("label"), Key->GetActorLabel(false), FString(TEXT("KeyLight")));
		TestEqual(TEXT("folder"), Key->GetFolderPath(), FName(TEXT("Room/Lights")));
		TestEqual(TEXT("tags"), Key->Tags, TArray<FName>({FName(TEXT("lit")), FName(TEXT("key"))}));
		USceneComponent* Root = Key->GetRootComponent();
		TestEqual(TEXT("location stored as given"), ReadValue(Root, TEXT("RelativeLocation")), FString(TEXT("{\"X\":10.5,\"Y\":-20,\"Z\":280}")));
		TestEqual(TEXT("rotation stored as given (R7)"), ReadValue(Root, TEXT("RelativeRotation")),
			FString(TEXT("{\"Pitch\":-20,\"Roll\":0,\"Yaw\":45}")));
		TestEqual(TEXT("scale"), ReadValue(Root, TEXT("RelativeScale3D")), FString(TEXT("{\"X\":2,\"Y\":2,\"Z\":2}")));
		TestEqual(TEXT("world location follows"), Key->GetActorLocation(), FVector(10.5, -20, 280));
		TestEqual(TEXT("intensity"), Key->PointLightComponent->Intensity, 8000.0f);
		TestEqual(TEXT("attenuation"), Key->PointLightComponent->AttenuationRadius, 1200.0f);
		TestEqual(TEXT("light color"), Key->PointLightComponent->LightColor, FColor(255, 214, 170, 255));
		TestTrue(TEXT("transactional like a placed actor"), Key->HasAnyFlags(RF_Transactional));
		TestFalse(TEXT("not selected"), Key->IsSelected());
	}
	AAssetDocumentLevelTestActor* Probe = Cast<AAssetDocumentLevelTestActor>(BatchActor(Result, TEXT("probe")));
	if (TestNotNull(TEXT("class without a factory placed through AddActor"), Probe))
	{
		TestEqual(TEXT("probe score"), Probe->Score, 12.5f);
		TestEqual(TEXT("partial location"), Probe->GetActorLocation(), FVector(0, 0, 50));
	}
	AActor* SubLight = BatchActor(Result, TEXT("subLight"));
	if (TestNotNull(TEXT("sublevel light spawned"), SubLight))
	{
		TestEqual(TEXT("spawned into the sublevel"), SubLight->GetLevel(), Sub);
		TestEqual(TEXT("the current level is unchanged"), World->GetCurrentLevel(), World->PersistentLevel.Get());
	}

	// Response shape.
	const TSharedPtr<FJsonObject>* ActorsJson = nullptr;
	if (TestTrue(TEXT("payload.actors"), Result.Payload->TryGetObjectField(TEXT("actors"), ActorsJson)))
	{
		const TSharedPtr<FJsonObject> KeyJson = (*ActorsJson)->GetObjectField(TEXT("key"));
		TestEqual(TEXT("actors.key.class"), KeyJson->GetStringField(TEXT("class")), FString(TEXT("/Script/Engine.PointLight")));
		TestEqual(TEXT("actors.key.label"), KeyJson->GetStringField(TEXT("label")), FString(TEXT("KeyLight")));
		TestEqual(TEXT("actors.subLight.level"), (*ActorsJson)->GetObjectField(TEXT("subLight"))->GetStringField(TEXT("level")), SubPackage);
	}
	const TSharedPtr<FJsonObject> AddChange = FindChange(Result, TEXT("/ops/0"));
	TestTrue(TEXT("Add change for /ops/0"), AddChange.IsValid() && AddChange->GetStringField(TEXT("kind")) == TEXT("Add"));
	const TSharedPtr<FJsonObject> IntensityChange = FindChange(Result, TEXT("/ops/0/components/LightComponent0/properties/Intensity"));
	if (TestTrue(TEXT("Intensity change listed"), IntensityChange.IsValid()))
	{
		// "before" is the value as placed: the placement applies the project's default light
		// units, so it is not necessarily the class default (5000).
		const TSharedPtr<FJsonValue> PlacedValue = IntensityChange->TryGetField(TEXT("before"));
		TestTrue(TEXT("before = the placed value"), PlacedValue.IsValid() && PlacedValue->Type == EJson::Number);
		TestEqual(TEXT("after"), JsonText(IntensityChange->TryGetField(TEXT("after"))), FString(TEXT("8000")));
		TestEqual(TEXT("component"), IntensityChange->GetStringField(TEXT("component")), FString(TEXT("LightComponent0")));
	}
	TestTrue(TEXT("map package dirty"), PayloadStrings(Result, TEXT("dirty_packages")).Contains(Maps.PackageName(TEXT("Room"))));
	TestTrue(TEXT("sublevel package dirty"), PayloadStrings(Result, TEXT("dirty_packages")).Contains(SubPackage));
	TestTrue(TEXT("nothing saved"), PayloadStrings(Result, TEXT("saved_packages")).IsEmpty());

	// One Ctrl+Z removes the whole batch.
	const FString KeyPath = BatchPath(Result, TEXT("key"));
	TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
	TestNull(TEXT("Undo removed the key light"), FindActorByPath(KeyPath));
	TestNull(TEXT("Undo removed the sublevel light"), FindActorByPath(BatchPath(Result, TEXT("subLight"))));
	TestTrue(TEXT("Redo"), GEditor->RedoTransaction());
	APointLight* Redone = Cast<APointLight>(FindActorByPath(KeyPath));
	if (TestNotNull(TEXT("Redo restores the key light"), Redone))
	{
		TestEqual(TEXT("Redo restores its intensity"), Redone->PointLightComponent->Intensity, 8000.0f);
	}

	// Refusals (preflight: nothing changes, no Undo entry).
	const int32 QueueBefore = UndoQueueLength();
	const TArray<FString> PersistentBefore = ListedActorPaths(*World->PersistentLevel);
	{
		const FLevelToolResult Refused = Edit(*this, TEXT(R"({"ops": [
			{"op": "spawn", "class": "/Script/Engine.WorldSettings"},
			{"op": "spawn", "class": "/Script/Engine.NoSuchActor"},
			{"op": "spawn", "class": "/Script/Engine.PointLight", "level": "/Game/NoSuchMap"},
			{"op": "spawn", "class": "/Script/Engine.PointLight", "lable": "Typo"}
		]})"), TEXT("refusals"));
		TestFalse(TEXT("refused"), Refused.bSuccess);
		TestEqual(TEXT("HTTP 400"), Refused.HttpStatus, 400);
		TestTrue(TEXT("unknown field found at parse"), HasCode(Refused, TEXT("Level.InvalidRequest"), TEXT("/ops/3/lable")));
	}
	{
		const FLevelToolResult Refused = Edit(*this, TEXT(R"({"ops": [
			{"op": "spawn", "class": "/Script/Engine.WorldSettings"},
			{"op": "spawn", "class": "/Script/Engine.NoSuchActor"},
			{"op": "spawn", "class": "/Script/Engine.PointLight", "level": "/Game/NoSuchMap"}
		]})"), TEXT("preflight refusals"));
		TestTrue(TEXT("not placeable"), HasCode(Refused, TEXT("Level.ClassNotPlaceable"), TEXT("/ops/0/class")));
		TestTrue(TEXT("unknown class"), HasCode(Refused, TEXT("Level.AssetNotFound"), TEXT("/ops/1/class")));
		TestTrue(TEXT("unknown level"), HasCode(Refused, TEXT("Level.LevelNotFound"), TEXT("/ops/2/level")));
		TestEqual(TEXT("phase preflight"), PayloadString(Refused, TEXT("phase")), FString(TEXT("preflight")));
		TestFalse(TEXT("nothing to roll back"), PayloadBool(Refused, TEXT("rolled_back")));
	}
	{
		// The Levels panel's lock (on the streaming level when there is one).
		FLevelUtils::ToggleLevelLock(Sub);
		TestTrue(TEXT("sublevel locked"), FLevelUtils::IsLevelLocked(Sub));
		const FLevelToolResult Locked = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "spawn", "class": "/Script/Engine.PointLight", "level": "%s"}]})"), *SubPackage), TEXT("locked level"));
		FLevelUtils::ToggleLevelLock(Sub);
		TestTrue(TEXT("locked level refused"), HasCode(Locked, TEXT("Level.LevelLocked"), TEXT("/ops/0/level")));
	}
	TestEqual(TEXT("refusals add no Undo entry"), UndoQueueLength(), QueueBefore);
	TestEqual(TEXT("refusals spawn nothing"), ListedActorPaths(*World->PersistentLevel), PersistentBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelEditSpawnAssetTest,
	"UEAssetDoc.Level.Edit.SpawnAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelEditSpawnAssetTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	if (!TestNotNull(TEXT("map created"), World))
	{
		return false;
	}
	ResetUndo();
	const FLevelToolResult Result = Edit(*this, TEXT(R"({"ops": [
		{"op": "spawn", "id": "floor", "asset": "/Engine/BasicShapes/Cube.Cube", "label": "Floor", "folder": "Room",
		 "transform": {"location": {"X": 0, "Y": 0, "Z": -10}, "scale": {"X": 8, "Y": 8, "Z": 0.2}},
		 "components": {"StaticMeshComponent0": {"properties": {
		    "OverrideMaterials": ["/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"]}}}},
		{"op": "spawn", "id": "pillar", "asset": "/Engine/BasicShapes/Cylinder", "transform": {"location": {"X": 300}}}
	]})"), TEXT("spawn assets"));
	if (!TestTrue(TEXT("spawn succeeds"), Result.bSuccess))
	{
		return false;
	}
	AStaticMeshActor* Floor = Cast<AStaticMeshActor>(BatchActor(Result, TEXT("floor")));
	if (TestNotNull(TEXT("a static mesh places as AStaticMeshActor (the editor's factory)"), Floor))
	{
		UStaticMeshComponent* Mesh = Floor->GetStaticMeshComponent();
		TestEqual(TEXT("mesh"), Mesh->GetStaticMesh()->GetPathName(), FString(TEXT("/Engine/BasicShapes/Cube.Cube")));
		TestEqual(TEXT("override material"), Mesh->OverrideMaterials.Num(), 1);
		TestEqual(TEXT("scale"), ReadValue(Floor->GetRootComponent(), TEXT("RelativeScale3D")), FString(TEXT("{\"X\":8,\"Y\":8,\"Z\":0.2}")));
		TestEqual(TEXT("label"), Floor->GetActorLabel(false), FString(TEXT("Floor")));
	}
	AStaticMeshActor* Pillar = Cast<AStaticMeshActor>(BatchActor(Result, TEXT("pillar")));
	if (TestNotNull(TEXT("a package path names its asset"), Pillar))
	{
		TestEqual(TEXT("cylinder mesh"), Pillar->GetStaticMeshComponent()->GetStaticMesh()->GetPathName(),
			FString(TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
	}
	TestEqual(TEXT("one Undo entry"), UndoQueueLength(), 1);

	const FLevelToolResult NoFactory = Edit(*this, TEXT(R"({"ops": [
		{"op": "spawn", "asset": "/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"},
		{"op": "spawn", "asset": "/Engine/BasicShapes/NoSuchMesh.NoSuchMesh"}]})"), TEXT("refusals"));
	TestTrue(TEXT("an asset without a placement factory"), HasCode(NoFactory, TEXT("Level.NoFactoryForAsset"), TEXT("/ops/0/asset")));
	TestTrue(TEXT("a missing asset"), HasCode(NoFactory, TEXT("Level.AssetNotFound"), TEXT("/ops/1/asset")));

	// save: true saves exactly the packages the batch dirtied (decision D2).
	TestTrue(TEXT("map saved once"), FScopedTestMaps::SaveWorld(*World));
	const FLevelToolResult Saved = Edit(*this, FString::Printf(TEXT(R"({"save": true, "ops": [
		{"op": "set", "actor": "%s", "label": "SavedFloor"}]})"), *BatchPath(Result, TEXT("floor"))), TEXT("save"));
	TestTrue(TEXT("saved batch succeeds"), Saved.bSuccess);
	TestEqual(TEXT("saved_packages is the map"), PayloadStrings(Saved, TEXT("saved_packages")),
		TArray<FString>({Maps.PackageName(TEXT("Room"))}));
	TestFalse(TEXT("the map is no longer dirty"), World->GetPackage()->IsDirty());
	TestTrue(TEXT("still undoable"), GEditor->Trans->CanUndo());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelEditSpawnBlueprintTest,
	"UEAssetDoc.Level.Edit.SpawnBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelEditSpawnBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	FScopedTestAsset Door(TEXT("BP_Door"));
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	if (!TestNotNull(TEXT("map created"), World)
		|| !Door.Apply(*this, DoorBlueprintDocument(Door.PackageName, TEXT("Create"), 1.0), TEXT("create BP_Door")))
	{
		return false;
	}
	ResetUndo();
	const FLevelToolResult Result = Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "spawn", "id": "door", "asset": "%s", "label": "FrontDoor",
		 "transform": {"location": {"X": 400}, "rotation": {"Yaw": 90}},
		 "properties": {"Brightness": 3.5, "bStartsOpen": false},
		 "components": {"Lamp": {"properties": {"Intensity": 2500}}}},
		{"op": "spawn", "id": "door2", "class": "%s", "transform": {"location": {"X": 800}}}
	]})"), *Door.PackageName, *Door.ClassPath()), TEXT("spawn Blueprint"));
	if (!TestTrue(TEXT("spawn succeeds"), Result.bSuccess))
	{
		return false;
	}
	AActor* Spawned = BatchActor(Result, TEXT("door"));
	if (TestNotNull(TEXT("door spawned"), Spawned))
	{
		TestEqual(TEXT("the Blueprint's generated class"), Spawned->GetClass()->GetPathName(), Door.ClassPath());
		TestEqual(TEXT("instance-editable variable"), ReadValue(Spawned, TEXT("Brightness")), FString(TEXT("3.5")));
		TestEqual(TEXT("bool variable"), ReadValue(Spawned, TEXT("bStartsOpen")), FString(TEXT("false")));
		UActorComponent* Lamp = FindObjectFast<UActorComponent>(Spawned, TEXT("Lamp"));
		TestNotNull(TEXT("Blueprint component Lamp"), Lamp);
		TestEqual(TEXT("SCS component override survives the construction reruns of the later writes"), ReadValue(Lamp, TEXT("Intensity")),
			FString(TEXT("2500")));
		// The construction script reruns of the later writes restore the transform through a
		// quaternion (as in the Details panel): the same orientation, last-bit noise allowed.
		TestTrue(TEXT("rotation"), Spawned->GetRootComponent()->GetRelativeRotation().Equals(FRotator(0, 90, 0), 1.0e-6));
	}
	AActor* Second = BatchActor(Result, TEXT("door2"));
	TestTrue(TEXT("\"class\": the generated class path places the Blueprint too"),
		Second != nullptr && Second->GetClass()->GetPathName() == Door.ClassPath());

	const FLevelToolResult Refused = Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "spawn", "asset": "%s", "properties": {"Secret": 5}},
		{"op": "spawn", "asset": "%s", "components": {"Nope": {"properties": {"Intensity": 1}}}}]})"),
		*Door.PackageName, *Door.PackageName), TEXT("Blueprint refusals"));
	TestTrue(TEXT("a variable that is not instance editable"),
		HasCode(Refused, TEXT("Level.PropertyNotEditableOnInstance"), TEXT("/ops/0/properties/Secret")));
	const FAssetDocumentDiagnostic* Unknown = FindCode(Refused, TEXT("Level.ComponentNotFound"));
	if (TestNotNull(TEXT("an unknown component of the Blueprint class"), Unknown))
	{
		TestEqual(TEXT("at the component"), Unknown->Path, FString(TEXT("/ops/1/components/Nope")));
		const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
		TestTrue(TEXT("lists the class's components"), Unknown->Details.IsValid()
			&& Unknown->Details->TryGetArrayField(TEXT("components"), Names) && JsonText(MakeShared<FJsonValueArray>(*Names)).Contains(TEXT("Lamp")));
	}
	return true;
}

#endif
