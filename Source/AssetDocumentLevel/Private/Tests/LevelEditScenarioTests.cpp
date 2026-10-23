// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// The design's AI scenario (§5.5): "build a small lit room: floor, four walls, a
// ceiling light, a door Blueprint and a camera looking at the door", then fit the door
// with a component and hang the camera on the light (add_component, attach), driven
// with exactly the requests an AI sends through the MCP (query_level / edit_level
// JSON), on a normal map and on a World Partition map. It covers the recovery from a
// diagnostic, the dry run, the end state as query_level reports it, and Ctrl+Z undoing
// each request.

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMeshActor.h"
#include "LevelEditTestSupport.h"
#include "LevelQuery.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentLevelEditScenarioTests
{
using namespace AssetDocumentLevelEditTests;

FString BuildRequest(const FString& DoorAsset, const TCHAR* LightProperty)
{
	return FString::Printf(TEXT(R"({"description": "AI: build lit room", "ops": [
  {"op": "spawn", "id": "floor", "asset": "/Engine/BasicShapes/Cube.Cube", "label": "Floor", "folder": "Room",
   "transform": {"location": {"X": 0, "Y": 0, "Z": -10}, "scale": {"X": 8, "Y": 8, "Z": 0.2}},
   "components": {"StaticMeshComponent0": {"properties": {"OverrideMaterials": ["/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"]}}}},
  {"op": "spawn", "id": "wallN", "asset": "/Engine/BasicShapes/Cube.Cube", "label": "WallN", "folder": "Room/Walls",
   "transform": {"location": {"X": 400, "Y": 0, "Z": 150}, "scale": {"X": 0.2, "Y": 8, "Z": 3}}},
  {"op": "duplicate", "id": "wallS", "actor": "@wallN", "label": "WallS", "transform": {"location": {"X": -400}}},
  {"op": "spawn", "id": "wallE", "asset": "/Engine/BasicShapes/Cube.Cube", "label": "WallE", "folder": "Room/Walls",
   "transform": {"location": {"X": 0, "Y": 400, "Z": 150}, "scale": {"X": 8, "Y": 0.2, "Z": 3}}},
  {"op": "duplicate", "id": "wallW", "actor": "@wallE", "label": "WallW", "transform": {"location": {"Y": -400}}},
  {"op": "spawn", "id": "key", "class": "/Script/Engine.PointLight", "label": "KeyLight", "folder": "Room",
   "transform": {"location": {"X": 0, "Y": 0, "Z": 280}},
   "components": {"LightComponent0": {"properties": {"%s": 8000, "AttenuationRadius": 1200,
                                                     "LightColor": {"R": 255, "G": 214, "B": 170, "A": 255}}}}},
  {"op": "spawn", "id": "door", "asset": "%s", "label": "FrontDoor", "folder": "Room",
   "transform": {"location": {"X": 390, "Y": 0, "Z": 0}, "rotation": {"Yaw": 90}},
   "properties": {"bStartsOpen": false, "LinkedLight": "@key"}},
  {"op": "spawn", "id": "cam", "class": "/Script/Engine.CameraActor", "label": "RoomCam", "folder": "Room",
   "transform": {"location": {"X": -300, "Y": -300, "Z": 250}, "rotation": {"Pitch": -20, "Yaw": 45}},
   "components": {"CameraComponent": {"properties": {"FieldOfView": 70}}}}
 ],
 "after": {"select": ["@door"], "focus": ["@floor"]}})"), LightProperty, *DoorAsset);
}

void RunScenario(FAutomationTestBase& Test, bool bPartitioned)
{
	const FString Kind = bPartitioned ? TEXT("WP") : TEXT("normal");
	FScopedTestAsset Door(TEXT("BP_Door"));
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Trial_Room"), bPartitioned);
	if (!Test.TestNotNull(*(Kind + TEXT(": map created")), World))
	{
		return;
	}
	// The AI created the door Blueprint with an asset document beforehand.
	if (!Door.Apply(Test, DoorBlueprintDocument(Door.PackageName, TEXT("Create"), 1.0), TEXT("create BP_Door")))
	{
		return;
	}
	ResetUndo();
	using UE::AssetDocumentLevel::Private::RunLevelQuery;
	const FLevelToolResult WorldSummary = RunLevelQuery(ParseJsonObject(TEXT(R"({"what": "world"})")));
	Test.TestTrue(*(Kind + TEXT(": world summary")), WorldSummary.bSuccess
		&& WorldSummary.Payload->GetBoolField(TEXT("partitioned")) == bPartitioned);
	const int32 ActorsBefore = ListedActorPaths(*World->PersistentLevel).Num();

	// First attempt with a typo: the diagnostic names the exact field; nothing changes.
	const FLevelToolResult Typo = Edit(Test, BuildRequest(Door.PackageName, TEXT("Intensty")), *(Kind + TEXT(" typo")));
	Test.TestTrue(*(Kind + TEXT(": typo refused at its pointer")), HasCode(Typo, TEXT("Generic.UnknownProperty"),
		TEXT("/ops/5/components/LightComponent0/properties/Intensty")));
	Test.TestTrue(*(Kind + TEXT(": the message leads with the root cause")), Typo.Message.StartsWith(TEXT("Generic.UnknownProperty")));
	Test.TestEqual(*(Kind + TEXT(": typo changes nothing")), ListedActorPaths(*World->PersistentLevel).Num(), ActorsBefore);

	// Dry run of the corrected request, then the real one.
	const FString Request = BuildRequest(Door.PackageName, TEXT("Intensity"));
	const FLevelToolResult Dry = Edit(Test, Request.Replace(TEXT("\"description\""), TEXT("\"dry_run\": true, \"description\"")),
		*(Kind + TEXT(" dry run")));
	Test.TestTrue(*(Kind + TEXT(": dry run ok")), Dry.bSuccess && Dry.Payload->GetNumberField(TEXT("would_spawn")) == 6
		&& Dry.Payload->GetNumberField(TEXT("would_duplicate")) == 2);
	const FLevelToolResult Built = Edit(Test, Request, *(Kind + TEXT(" build")));
	if (!Test.TestTrue(*(Kind + TEXT(": build succeeds")), Built.bSuccess))
	{
		return;
	}
	Test.TestEqual(*(Kind + TEXT(": one Undo entry")), UndoQueueLength(), 1);

	// End state, as the AI checks it with query_level.
	const FLevelToolResult Room = RunLevelQuery(ParseJsonObject(TEXT(R"({"folder": "Room", "detail": "properties"})")));
	Test.TestEqual(*(Kind + TEXT(": query_level lists the 8 room actors")), Room.bSuccess ? static_cast<int32>(Room.Payload->GetNumberField(TEXT("total"))) : -1, 8);
	AActor* DoorActor = BatchActor(Built, TEXT("door"));
	AActor* Key = BatchActor(Built, TEXT("key"));
	ACameraActor* Cam = Cast<ACameraActor>(BatchActor(Built, TEXT("cam")));
	AActor* WallS = BatchActor(Built, TEXT("wallS"));
	if (Test.TestNotNull(*(Kind + TEXT(": door")), DoorActor) && Test.TestNotNull(*(Kind + TEXT(": key light")), Key))
	{
		Test.TestEqual(*(Kind + TEXT(": the door's LinkedLight is the key light")), ReadValue(DoorActor, TEXT("LinkedLight")), GetActorPath(*Key));
		Test.TestEqual(*(Kind + TEXT(": door closed")), ReadValue(DoorActor, TEXT("bStartsOpen")), FString(TEXT("false")));
		Test.TestEqual(*(Kind + TEXT(": key light intensity")), Cast<APointLight>(Key)->PointLightComponent->Intensity, 8000.0f);
		Test.TestTrue(*(Kind + TEXT(": door selected (after.select)")), DoorActor->IsSelected());
		Test.TestEqual(*(Kind + TEXT(": selection reported")), PayloadStrings(Built, TEXT("selection")), TArray<FString>({GetActorPath(*DoorActor)}));
	}
	if (Test.TestNotNull(*(Kind + TEXT(": camera")), Cam))
	{
		Test.TestEqual(*(Kind + TEXT(": field of view")), Cam->GetCameraComponent()->FieldOfView, 70.0f);
		Test.TestEqual(*(Kind + TEXT(": camera rotation as given")), ReadValue(Cam->GetRootComponent(), TEXT("RelativeRotation")),
			FString(TEXT("{\"Pitch\":-20,\"Roll\":0,\"Yaw\":45}")));
	}
	if (Test.TestNotNull(*(Kind + TEXT(": duplicated wall")), WallS))
	{
		Test.TestEqual(*(Kind + TEXT(": duplicate keeps the folder")), WallS->GetFolderPath(), FName(TEXT("Room/Walls")));
		Test.TestEqual(*(Kind + TEXT(": duplicate moved")), WallS->GetActorLocation(), FVector(-400, 0, 150));
		Test.TestEqual(*(Kind + TEXT(": duplicate keeps the scale")), WallS->GetActorScale3D(), FVector(0.2, 8, 3));
	}
	const TArray<FString> Dirty = PayloadStrings(Built, TEXT("dirty_packages"));
	if (bPartitioned)
	{
		// One external package per actor, and one per new actor folder ("Room", "Room/Walls").
		const int32 ActorPackages = Dirty.FilterByPredicate([](const FString& Package)
		{
			return Package.Contains(TEXT("__ExternalActors__"));
		}).Num();
		Test.TestEqual(*(Kind + TEXT(": one external package per actor")), ActorPackages, 8);
		Test.TestEqual(*(Kind + TEXT(": one external object package per new folder")), Dirty.Num() - ActorPackages, 2);
		Test.TestTrue(*(Kind + TEXT(": folders are external objects")), Dirty.ContainsByPredicate([](const FString& Package)
		{
			return Package.Contains(TEXT("__ExternalObjects__"));
		}));
		Test.TestNotNull(*(Kind + TEXT(": the door has its own package")),
			DoorActor != nullptr ? DoorActor->GetExternalPackage() : nullptr);
	}
	else
	{
		Test.TestEqual(*(Kind + TEXT(": the map package")), Dirty, TArray<FString>({Maps.PackageName(TEXT("Trial_Room"))}));
	}

	// A second request fits the door with a porch light on its frame (add_component) and
	// hangs the camera on the key light (attach keeps the world transform). (Undo of an
	// attach to a Blueprint actor makes the editor itself log a LogSceneComponent error,
	// so the camera's parent here is not a Blueprint.)
	if (DoorActor != nullptr && Cam != nullptr && Key != nullptr)
	{
		const FString DoorPath = GetActorPath(*DoorActor);
		const FString KeyPath = GetActorPath(*Key);
		const FString CamPath = GetActorPath(*Cam);
		const FTransform CamWorld = Cam->GetActorTransform();
		const FLevelToolResult Fitted = Edit(Test, FString::Printf(TEXT(R"({"description": "AI: fit door", "ops": [
  {"op": "add_component", "actor": "%s", "class": "/Script/Engine.PointLightComponent", "name": "PorchLight", "parent": "Frame",
   "properties": {"Intensity": 500}},
  {"op": "attach", "actor": "%s", "parent": "%s"}]})"), *DoorPath, *CamPath, *KeyPath), *(Kind + TEXT(" fit door")));
		Test.TestTrue(*(Kind + TEXT(": fitting succeeds")), Fitted.bSuccess);
		AActor* FittedDoor = FindActorByPath(DoorPath);
		AActor* FittedCam = FindActorByPath(CamPath);
		AActor* FittedKey = FindActorByPath(KeyPath);
		UPointLightComponent* Porch = nullptr;
		if (FittedDoor != nullptr)
		{
			TInlineComponentArray<UPointLightComponent*> Lights(FittedDoor);
			for (UPointLightComponent* Light : Lights)
			{
				Porch = Light != nullptr && Light->GetName() == TEXT("PorchLight") ? Light : Porch;
			}
		}
		Test.TestTrue(*(Kind + TEXT(": porch light on the frame")), Porch != nullptr && Porch->GetAttachParent() != nullptr
			&& Porch->GetAttachParent()->GetName() == TEXT("Frame") && Porch->Intensity == 500.0f);
		Test.TestTrue(*(Kind + TEXT(": camera on the key light")), FittedCam != nullptr && FittedKey != nullptr
			&& FittedCam->GetAttachParentActor() == FittedKey);
		Test.TestTrue(*(Kind + TEXT(": camera keeps its world transform")), FittedCam != nullptr
			&& FittedCam->GetActorTransform().Equals(CamWorld, 0.01));
		Test.TestTrue(*(Kind + TEXT(": Ctrl+Z the fitting")), GEditor->UndoTransaction());
		FittedCam = FindActorByPath(CamPath);
		Test.TestTrue(*(Kind + TEXT(": camera detached again")), FittedCam != nullptr && FittedCam->GetAttachParentActor() == nullptr);
	}

	// One Ctrl+Z undoes the whole build.
	Test.TestTrue(*(Kind + TEXT(": Ctrl+Z")), GEditor->UndoTransaction());
	Test.TestEqual(*(Kind + TEXT(": the room is gone")), ListedActorPaths(*World->PersistentLevel).Num(), ActorsBefore);
	const FLevelToolResult AfterUndo = RunLevelQuery(ParseJsonObject(TEXT(R"({"folder": "Room"})")));
	Test.TestEqual(*(Kind + TEXT(": query_level finds none")), AfterUndo.bSuccess ? static_cast<int32>(AfterUndo.Payload->GetNumberField(TEXT("total"))) : -1, 0);
	ResetUndo();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelEditScenarioTest,
	"UEAssetDoc.Level.Edit.Scenario",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelEditScenarioTest::RunTest(const FString& Parameters)
{
	AssetDocumentLevelEditScenarioTests::RunScenario(*this, false);
	AssetDocumentLevelEditScenarioTests::RunScenario(*this, true);
	return true;
}

#endif
