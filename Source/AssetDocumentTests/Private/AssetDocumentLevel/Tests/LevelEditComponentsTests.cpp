// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// edit_level add_component / remove_component / reparent_component (design §7.2 L3): the
// Details panel's component tree (USubobjectDataSubsystem) on placed actors. Instance
// components are added with a name, parent, socket, asset and values; only instance
// components are removed or reparented; class components are refused with how they were
// made; names follow the panel's rules (and the Blueprint's names); one Undo entry.

#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "LevelEditTestSupport.h"
#include "LevelEditTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentLevelEditComponentsTests
{
UActorComponent* FindComponentNamed(AActor* Actor, const TCHAR* Name)
{
	if (Actor == nullptr)
	{
		return nullptr;
	}
	TInlineComponentArray<UActorComponent*> Components(Actor);
	for (UActorComponent* Component : Components)
	{
		if (Component != nullptr && Component->GetName() == Name)
		{
			return Component;
		}
	}
	return nullptr;
}

FString AttachParentName(UActorComponent* Component)
{
	const USceneComponent* Scene = Cast<USceneComponent>(Component);
	return Scene != nullptr && Scene->GetAttachParent() != nullptr ? Scene->GetAttachParent()->GetName() : FString();
}

FString CreationDetail(const FAssetDocumentDiagnostic* Diagnostic)
{
	FString Creation;
	if (Diagnostic != nullptr && Diagnostic->Details.IsValid())
	{
		Diagnostic->Details->TryGetStringField(TEXT("creation"), Creation);
	}
	return Creation;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelEditComponentsTest,
	"UEAssetDoc.Level.Edit.Components",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelEditComponentsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	using namespace AssetDocumentLevelEditComponentsTests;
	FScopedTestAsset Door(TEXT("BP_Door"));
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	if (!TestNotNull(TEXT("map created"), World) || !Door.Apply(*this, DoorBlueprintDocument(Door.PackageName, TEXT("Create"), 1.0),
		TEXT("create BP_Door")))
	{
		return false;
	}
	ULevel& Level = *World->PersistentLevel;
	const FVector RigLocation(200, 0, 0);
	const FString RigPath = GetActorPath(*SpawnActor(Level, AAssetDocumentLevelTestActor::StaticClass(), RigLocation, TEXT("Rig")));
	UClass* DoorClass = LoadObject<UClass>(nullptr, *Door.ClassPath());
	AActor* DoorSpawned = DoorClass != nullptr ? SpawnActor(Level, DoorClass, FVector(-200, 0, 0), TEXT("Door")) : nullptr;
	const FString BuiltPath = GetActorPath(*SpawnActor(Level, AAssetDocumentLevelTestBuiltActor::StaticClass(), FVector(0, 300, 0),
		TEXT("Built")));
	if (!TestNotNull(TEXT("door placed"), DoorSpawned))
	{
		return false;
	}
	const FString DoorPath = GetActorPath(*DoorSpawned);
	ResetUndo();

	// Add: a socket parent added earlier in the batch, a light on its socket with values,
	// a mesh with its asset (the Add Component menu's asset broker), a non-scene component.
	const FLevelToolResult Added = Edit(*this, FString::Printf(TEXT(R"({"description": "AI: rig", "ops": [
		{"op": "add_component", "actor": "%s", "class": "/Script/AssetDocumentTests.AssetDocumentLevelTestSocketComponent", "name": "Arm"},
		{"op": "add_component", "actor": "%s", "class": "/Script/Engine.PointLightComponent", "name": "Bulb", "parent": "Arm",
		 "socket": "Tip", "properties": {"Intensity": 1234}},
		{"op": "add_component", "actor": "%s", "class": "/Script/Engine.StaticMeshComponent", "name": "Body",
		 "asset": "/Engine/BasicShapes/Cube.Cube"},
		{"op": "add_component", "actor": "%s", "class": "/Script/Engine.RotatingMovementComponent", "name": "Spin"}]})"),
		*RigPath, *RigPath, *RigPath, *RigPath), TEXT("add"));
	if (!TestTrue(TEXT("add succeeds"), Added.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("one Undo entry"), UndoQueueLength(), 1);
	AActor* Rig = FindActorByPath(RigPath);
	UActorComponent* Arm = FindComponentNamed(Rig, TEXT("Arm"));
	UPointLightComponent* Bulb = Cast<UPointLightComponent>(FindComponentNamed(Rig, TEXT("Bulb")));
	UStaticMeshComponent* Body = Cast<UStaticMeshComponent>(FindComponentNamed(Rig, TEXT("Body")));
	TestNotNull(TEXT("Arm added"), Arm);
	TestNotNull(TEXT("Spin added"), FindComponentNamed(Rig, TEXT("Spin")));
	if (TestNotNull(TEXT("Bulb added"), Bulb))
	{
		TestEqual(TEXT("an instance component"), Bulb->CreationMethod, EComponentCreationMethod::Instance);
		TestEqual(TEXT("Bulb on Arm"), AttachParentName(Bulb), FString(TEXT("Arm")));
		TestEqual(TEXT("Bulb on the socket"), Bulb->GetAttachSocketName(), FName(TEXT("Tip")));
		TestEqual(TEXT("Bulb's value"), Bulb->Intensity, 1234.0f);
		TestTrue(TEXT("Bulb at the socket"), Bulb->GetComponentLocation().Equals(RigLocation + FVector(0, 0, 50), 0.01));
	}
	if (TestNotNull(TEXT("Body added"), Body))
	{
		TestEqual(TEXT("Body has the asset"), Body->GetStaticMesh() != nullptr ? Body->GetStaticMesh()->GetPathName() : FString(),
			FString(TEXT("/Engine/BasicShapes/Cube.Cube")));
		TestEqual(TEXT("Body under the root"), AttachParentName(Body), FString(TEXT("TestComponent")));
	}
	const TSharedPtr<FJsonObject> BulbChange = FindChange(Added, TEXT("/ops/1"));
	TestTrue(TEXT("an Add component change"), BulbChange.IsValid() && BulbChange->GetStringField(TEXT("kind")) == TEXT("Add")
		&& BulbChange->GetStringField(TEXT("object")) == TEXT("component"));
	TestTrue(TEXT("the map is dirty"), PayloadStrings(Added, TEXT("dirty_packages")).Contains(Maps.PackageName(TEXT("Room"))));

	// Reparent keeps the world transform; removing a parent promotes its children (as the
	// Details panel's Delete does).
	const FLevelToolResult Moved = Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "reparent_component", "actor": "%s", "component": "Bulb", "parent": "Body"},
		{"op": "remove_component", "actor": "%s", "component": "Arm"}]})"), *RigPath, *RigPath), TEXT("reparent and remove"));
	TestTrue(TEXT("reparent and remove succeed"), Moved.bSuccess);
	Rig = FindActorByPath(RigPath);
	Bulb = Cast<UPointLightComponent>(FindComponentNamed(Rig, TEXT("Bulb")));
	TestNull(TEXT("Arm removed"), FindComponentNamed(Rig, TEXT("Arm")));
	if (TestNotNull(TEXT("Bulb kept"), Bulb))
	{
		TestEqual(TEXT("Bulb on Body"), AttachParentName(Bulb), FString(TEXT("Body")));
		TestTrue(TEXT("Bulb's world location kept"), Bulb->GetComponentLocation().Equals(RigLocation + FVector(0, 0, 50), 0.01));
	}
	const FLevelToolResult Promote = Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "remove_component", "actor": "%s", "component": "Body"}]})"), *RigPath), TEXT("remove a parent"));
	TestTrue(TEXT("removing a parent succeeds"), Promote.bSuccess);
	TestEqual(TEXT("its child moves to its parent"), AttachParentName(FindComponentNamed(FindActorByPath(RigPath), TEXT("Bulb"))),
		FString(TEXT("TestComponent")));

	// Each batch is one Undo entry; undoing all three restores the class components only.
	TestTrue(TEXT("Undo"), GEditor->UndoTransaction() && GEditor->UndoTransaction() && GEditor->UndoTransaction());
	Rig = FindActorByPath(RigPath);
	TestNull(TEXT("Bulb gone after Undo"), FindComponentNamed(Rig, TEXT("Bulb")));
	TestNull(TEXT("Arm gone after Undo"), FindComponentNamed(Rig, TEXT("Arm")));
	TestNotNull(TEXT("the native root stays"), FindComponentNamed(Rig, TEXT("TestComponent")));

	// Components of an actor created by the same batch.
	{
		const FLevelToolResult Planned = Edit(*this, TEXT(R"({"ops": [
			{"op": "spawn", "id": "lamp", "class": "/Script/AssetDocumentTests.AssetDocumentLevelTestActor", "label": "Lamp"},
			{"op": "add_component", "actor": "@lamp", "class": "/Script/Engine.PointLightComponent", "name": "Bulb"},
			{"op": "set", "actor": "@lamp", "components": {"Bulb": {"properties": {"Intensity": 77}}}}]})"), TEXT("planned actor"));
		TestTrue(TEXT("components of a planned actor"), Planned.bSuccess);
		UPointLightComponent* PlannedBulb = Cast<UPointLightComponent>(FindComponentNamed(BatchActor(Planned, TEXT("lamp")), TEXT("Bulb")));
		TestTrue(TEXT("its component has the value"), PlannedBulb != nullptr && PlannedBulb->Intensity == 77.0f);
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
	}

	// Refusals, all in preflight: class components are not removable (native, Blueprint,
	// construction script), names follow the Details panel and the Blueprint, the class
	// must be a component the Add Component menu offers, sockets must exist.
	AActor* BuiltActor = FindActorByPath(BuiltPath);
	FString ScriptComponent;
	TInlineComponentArray<UActorComponent*> BuiltComponents(BuiltActor);
	for (UActorComponent* Component : BuiltComponents)
	{
		if (Component != nullptr && Component->CreationMethod == EComponentCreationMethod::UserConstructionScript)
		{
			ScriptComponent = Component->GetName();
		}
	}
	TestFalse(TEXT("the construction script made a component"), ScriptComponent.IsEmpty());
	const int32 QueueBefore = UndoQueueLength();
	struct FRefusal
	{
		FString Ops;
		const TCHAR* Code;
		const TCHAR* Path;
		const TCHAR* Creation;
	};
	const TArray<FRefusal> Refusals = {
		{FString::Printf(TEXT(R"({"op": "remove_component", "actor": "%s", "component": "TestComponent"})"), *RigPath),
			TEXT("Level.ComponentNotRemovable"), TEXT("/ops/0/component"), TEXT("Native")},
		{FString::Printf(TEXT(R"({"op": "remove_component", "actor": "%s", "component": "Frame"})"), *DoorPath),
			TEXT("Level.ComponentNotRemovable"), TEXT("/ops/0/component"), TEXT("Blueprint")},
		{FString::Printf(TEXT(R"({"op": "remove_component", "actor": "%s", "component": "%s"})"), *BuiltPath, *ScriptComponent),
			TEXT("Level.ComponentNotRemovable"), TEXT("/ops/0/component"), TEXT("ConstructionScript")},
		{FString::Printf(TEXT(R"({"op": "set", "actor": "%s", "components": {"%s": {"properties": {"bVisible": false}}}})"), *BuiltPath,
			*ScriptComponent), TEXT("Level.ComponentNotEditable"), nullptr, TEXT("ConstructionScript")},
		{FString::Printf(TEXT(R"({"op": "reparent_component", "actor": "%s", "component": "Lamp", "parent": "Frame"})"), *DoorPath),
			TEXT("Level.ComponentNotEditable"), TEXT("/ops/0/component"), TEXT("Blueprint")},
		{FString::Printf(TEXT(R"({"op": "add_component", "actor": "%s", "class": "/Script/Engine.PointLightComponent", "name": "Brightness"})"),
			*DoorPath), TEXT("Level.ComponentNameTaken"), TEXT("/ops/0/name"), nullptr},
		{FString::Printf(TEXT(R"({"op": "add_component", "actor": "%s", "class": "/Script/Engine.PointLightComponent", "name": "Lamp"})"),
			*DoorPath), TEXT("Level.ComponentNameTaken"), TEXT("/ops/0/name"), nullptr},
		{FString::Printf(TEXT(R"({"op": "add_component", "actor": "%s", "class": "/Script/Engine.PointLightComponent", "name": "Bad Name"})"),
			*RigPath), TEXT("Level.InvalidRequest"), TEXT("/ops/0/name"), nullptr},
		{FString::Printf(TEXT(R"({"op": "add_component", "actor": "%s", "class": "/Script/Engine.PointLight", "name": "NotAComponent"})"),
			*RigPath), TEXT("Level.ComponentClassInvalid"), TEXT("/ops/0/class"), nullptr},
		{FString::Printf(TEXT(R"({"op": "add_component", "actor": "%s", "class": "/Script/Engine.NoSuchComponent", "name": "Missing"})"),
			*RigPath), TEXT("Level.ComponentClassInvalid"), TEXT("/ops/0/class"), nullptr},
		{FString::Printf(TEXT(R"({"op": "add_component", "actor": "%s", "class": "/Script/Engine.PointLightComponent", "name": "OnSocket",
			"parent": "TestComponent", "socket": "Nope"})"), *RigPath), TEXT("Level.AttachRejected"), TEXT("/ops/0/socket"), nullptr},
		{FString::Printf(TEXT(R"({"op": "remove_component", "actor": "%s", "component": "Nope"})"), *RigPath),
			TEXT("Level.ComponentNotFound"), TEXT("/ops/0/component"), nullptr},
		// A construction-script component of an actor the batch creates is unknown until the
		// actor exists (the message says so).
		{TEXT(R"({"op": "spawn", "id": "b", "class": "/Script/AssetDocumentTests.AssetDocumentLevelTestBuiltActor"},
			{"op": "set", "actor": "@b", "components": {"SceneComponent_0": {"properties": {"bVisible": false}}}})"),
			TEXT("Level.ComponentNotFound"), TEXT("/ops/1/components/SceneComponent_0"), nullptr},
	};
	for (int32 Index = 0; Index < Refusals.Num(); ++Index)
	{
		const FRefusal& Refusal = Refusals[Index];
		const FString Stage = FString::Printf(TEXT("refusal %d (%s)"), Index, Refusal.Code);
		const FLevelToolResult Refused = Edit(*this, FString::Printf(TEXT(R"({"ops": [%s]})"), *Refusal.Ops), *Stage);
		TestFalse(*(Stage + TEXT(": refused")), Refused.bSuccess);
		TestTrue(*(Stage + TEXT(": code and path")), HasCode(Refused, Refusal.Code, Refusal.Path));
		TestEqual(*(Stage + TEXT(": phase preflight")), PayloadString(Refused, TEXT("phase")), FString(TEXT("preflight")));
		if (Refusal.Creation != nullptr)
		{
			TestEqual(*(Stage + TEXT(": how the component was made")), CreationDetail(FindCode(Refused, Refusal.Code)),
				FString(Refusal.Creation));
		}
	}
	TestEqual(TEXT("refusals add no Undo entry"), UndoQueueLength(), QueueBefore);
	TestNotNull(TEXT("the door keeps Frame"), FindComponentNamed(FindActorByPath(DoorPath), TEXT("Frame")));
	return true;
}

#endif
