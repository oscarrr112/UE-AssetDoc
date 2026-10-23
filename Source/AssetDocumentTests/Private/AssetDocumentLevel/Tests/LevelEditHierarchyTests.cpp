// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// edit_level attach / detach / spawn attach_to (design §7.2 L3): the Outliner's drag onto a
// parent (UEditorEngine::CanParentActors + ParentActors) and Detach. World transforms are
// kept, the child moves to the folder of its topmost parent, refusals carry the editor's
// own reason (also for actors the batch creates, and for cycles made earlier in the
// batch), and transform writes around an attach are verified in the right space. Raw.Detach compares the raw detach with the editor's DetachSelectedActors.

#include "Engine/Selection.h"
#include "Engine/StaticMeshActor.h"
#include "LevelEditTestSupport.h"
#include "LevelEditTestTypes.h"
#include "Raw/LevelDetach.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentLevelEditHierarchyTests
{
using namespace AssetDocumentLevelEditTests;

FString Reason(const FLevelToolResult& Result)
{
	const FAssetDocumentDiagnostic* Rejected = FindCode(Result, TEXT("Level.AttachRejected"));
	FString Text;
	if (Rejected != nullptr && Rejected->Details.IsValid())
	{
		Rejected->Details->TryGetStringField(TEXT("reason"), Text);
	}
	return Text;
}

FString ParentPath(const FString& ChildPath)
{
	const AActor* Child = FindActorByPath(ChildPath);
	const AActor* Parent = Child != nullptr ? Child->GetAttachParentActor() : nullptr;
	return Parent != nullptr ? UE::AssetDocumentLevel::GetActorPath(*Parent) : FString();
}

FVector WorldLocation(const FString& Path)
{
	const AActor* Actor = FindActorByPath(Path);
	return Actor != nullptr ? Actor->GetActorLocation() : FVector(-1.0e9);
}

FName Folder(const FString& Path)
{
	const AActor* Actor = FindActorByPath(Path);
	return Actor != nullptr ? Actor->GetFolderPath() : FName(TEXT("<missing>"));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelEditHierarchyTest,
	"UEAssetDoc.Level.Edit.Hierarchy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelEditHierarchyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	using namespace AssetDocumentLevelEditHierarchyTests;
	FScopedTestAsset Door(TEXT("BP_Door"));
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	ULevel* Sub = World != nullptr ? Maps.AddSublevel(*World, TEXT("Sub")) : nullptr;
	if (!TestNotNull(TEXT("map with a sublevel"), Sub)
		|| !Door.Apply(*this, DoorBlueprintDocument(Door.PackageName, TEXT("Create"), 1.0), TEXT("create BP_Door")))
	{
		return false;
	}
	ULevel& Level = *World->PersistentLevel;
	AActor* ParentActor = SpawnActor(Level, AAssetDocumentLevelTestActor::StaticClass(), FVector(100, 0, 0), TEXT("Parent"));
	ParentActor->SetActorRotation(FRotator(0, 90, 0));
	ParentActor->SetFolderPath(TEXT("Props"));
	const FString ParentPathText = GetActorPath(*ParentActor);
	const FString ChildPath = GetActorPath(*SpawnActor(Level, AAssetDocumentLevelTestActor::StaticClass(), FVector(300, 0, 0), TEXT("Child")));
	const FString GrandPath = GetActorPath(*SpawnActor(Level, AAssetDocumentLevelTestActor::StaticClass(), FVector(300, 200, 0), TEXT("Grand")));
	AStaticMeshActor* Rock = Cast<AStaticMeshActor>(SpawnActor(Level, AStaticMeshActor::StaticClass(), FVector(0, -300, 0), TEXT("Rock")));
	const FString RockPath = GetActorPath(*Rock);
	const FString FarPath = GetActorPath(*SpawnActor(*Sub, AAssetDocumentLevelTestActor::StaticClass(), FVector(0, 600, 0), TEXT("Far")));
	TestEqual(TEXT("the rock is static"), Rock->GetRootComponent()->Mobility.GetValue(), EComponentMobility::Static);
	ResetUndo();

	// Attach a chain: world transforms kept, both move to the topmost parent's folder.
	const FLevelToolResult Chain = Edit(*this, FString::Printf(TEXT(R"({"description": "AI: group", "ops": [
		{"op": "attach", "actor": "%s", "parent": "%s"},
		{"op": "attach", "actor": "%s", "parent": "%s"}]})"), *ChildPath, *ParentPathText, *GrandPath, *ChildPath), TEXT("attach chain"));
	if (!TestTrue(TEXT("attach succeeds"), Chain.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("child on parent"), ParentPath(ChildPath), ParentPathText);
	TestEqual(TEXT("grandchild on child"), ParentPath(GrandPath), ChildPath);
	TestTrue(TEXT("child keeps its world location"), WorldLocation(ChildPath).Equals(FVector(300, 0, 0), 0.01));
	TestTrue(TEXT("grandchild keeps its world location"), WorldLocation(GrandPath).Equals(FVector(300, 200, 0), 0.01));
	TestEqual(TEXT("child in the parent's folder"), Folder(ChildPath), FName(TEXT("Props")));
	TestEqual(TEXT("grandchild in the topmost parent's folder"), Folder(GrandPath), FName(TEXT("Props")));
	const TSharedPtr<FJsonObject> Change = FindChange(Chain, TEXT("/ops/0"));
	TestTrue(TEXT("an attachment change"), Change.IsValid() && Change->GetStringField(TEXT("object")) == TEXT("attachment")
		&& Change->GetStringField(TEXT("after")) == ParentPathText);
	TestEqual(TEXT("one Undo entry"), UndoQueueLength(), 1);

	// Refusals carry the editor's reason, for placed actors and for actors of the batch.
	const int32 QueueBefore = UndoQueueLength();
	{
		const FLevelToolResult Cycle = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "attach", "actor": "%s", "parent": "%s"}]})"), *ParentPathText, *GrandPath), TEXT("cycle"));
		TestEqual(TEXT("cycle: the editor's reason"), Reason(Cycle), FString(TEXT("Parent cannot become the child of their descendant")));
		const FLevelToolResult PlannedCycle = Edit(*this, TEXT(R"({"ops": [
			{"op": "spawn", "id": "a", "class": "/Script/AssetDocumentTests.AssetDocumentLevelTestActor"},
			{"op": "spawn", "id": "b", "class": "/Script/AssetDocumentTests.AssetDocumentLevelTestActor"},
			{"op": "attach", "actor": "@a", "parent": "@b"},
			{"op": "attach", "actor": "@b", "parent": "@a"}]})"), TEXT("cycle in the batch"));
		TestTrue(TEXT("cycle in the batch refused at its op"), HasCode(PlannedCycle, TEXT("Level.AttachRejected"), TEXT("/ops/3")));
		TestEqual(TEXT("cycle in the batch: the editor's reason"), Reason(PlannedCycle),
			FString(TEXT("Parent cannot become the child of their descendant")));

		const FLevelToolResult Static = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "attach", "actor": "%s", "parent": "%s"}]})"), *RockPath, *ParentPathText), TEXT("static onto movable"));
		TestTrue(TEXT("static onto movable: the editor's reason"), Reason(Static).StartsWith(TEXT("Cannot attach actor with Static mobility")));
		const FLevelToolResult PlannedStatic = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "spawn", "id": "rock", "asset": "/Engine/BasicShapes/Cube.Cube"},
			{"op": "attach", "actor": "@rock", "parent": "%s"}]})"), *ParentPathText), TEXT("planned static onto movable"));
		TestTrue(TEXT("planned static onto movable: the same reason"),
			Reason(PlannedStatic).StartsWith(TEXT("Cannot attach actor with Static mobility")));

		const FLevelToolResult Cross = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "attach", "actor": "%s", "parent": "%s"}]})"), *FarPath, *ParentPathText), TEXT("across levels"));
		TestEqual(TEXT("across levels: the editor's reason"), Reason(Cross), FString(TEXT("Actors need to be in the same level!")));
		const FLevelToolResult PlannedCross = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "spawn", "id": "far", "class": "/Script/AssetDocumentTests.AssetDocumentLevelTestActor", "level": "%s"},
			{"op": "attach", "actor": "@far", "parent": "%s"}]})"), *Maps.PackageName(TEXT("Sub")), *ParentPathText),
			TEXT("planned across levels"));
		TestEqual(TEXT("planned across levels: the same reason"), Reason(PlannedCross), FString(TEXT("Actors need to be in the same level!")));

		for (const FLevelToolResult* Result : {&Cycle, &PlannedCycle, &Static, &PlannedStatic, &Cross, &PlannedCross})
		{
			TestEqual(TEXT("refused in preflight"), PayloadString(*Result, TEXT("phase")), FString(TEXT("preflight")));
		}
		TestEqual(TEXT("refusals add no Undo entry"), UndoQueueLength(), QueueBefore);
	}

	// The batch sees its own detach: re-rooting the chain (Parent under Child) is no
	// cycle once Child was detached earlier in the batch.
	{
		const FLevelToolResult Reroot = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "detach", "actor": "%s"},
			{"op": "attach", "actor": "%s", "parent": "%s"}]})"), *ChildPath, *ParentPathText, *ChildPath), TEXT("re-root"));
		TestTrue(TEXT("re-root succeeds"), Reroot.bSuccess);
		TestEqual(TEXT("child detached"), ParentPath(ChildPath), FString());
		TestEqual(TEXT("parent under child"), ParentPath(ParentPathText), ChildPath);
		TestTrue(TEXT("parent keeps its world location"), WorldLocation(ParentPathText).Equals(FVector(100, 0, 0), 0.01));
		TestTrue(TEXT("Undo re-root"), GEditor->UndoTransaction());
		TestEqual(TEXT("child back on parent"), ParentPath(ChildPath), ParentPathText);
		TestEqual(TEXT("parent unattached again"), ParentPath(ParentPathText), FString());
	}

	// Transform writes around an attach: a write before the attach is the
	// world place the attach keeps; a write after it is relative to the parent.
	{
		const FLevelToolResult Around = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "detach", "actor": "%s"},
			{"op": "set", "actor": "%s", "transform": {"location": {"X": 500, "Y": 0, "Z": 0}}},
			{"op": "attach", "actor": "%s", "parent": "%s"},
			{"op": "set", "actor": "%s", "transform": {"location": {"X": 10, "Y": 0, "Z": 0}, "rotation": {"Yaw": 30}}}]})"),
			*GrandPath, *GrandPath, *GrandPath, *ParentPathText, *GrandPath), TEXT("transforms around attach"));
		TestTrue(TEXT("transforms around attach succeed"), Around.bSuccess);
		const AActor* Grand = FindActorByPath(GrandPath);
		if (TestNotNull(TEXT("grandchild"), Grand))
		{
			TestEqual(TEXT("re-attached to parent"), ParentPath(GrandPath), ParentPathText);
			// Parent at (100, 0, 0), yaw 90: relative (10, 0, 0) is world (100, 10, 0).
			TestTrue(TEXT("relative location after attach"), Grand->GetActorLocation().Equals(FVector(100, 10, 0), 0.01));
			TestTrue(TEXT("relative rotation after attach"), Grand->GetActorQuat().Equals(FRotator(0, 120, 0).Quaternion(), 1.0e-4));
		}
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
		TestTrue(TEXT("grandchild restored"), WorldLocation(GrandPath).Equals(FVector(300, 200, 0), 0.01) && ParentPath(GrandPath) == ChildPath);

		// spawn attach_to: the transform is relative to the parent; a Blueprint child (its
		// construction script reruns when attached) is verified after the rerun.
		const FLevelToolResult Spawned = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "spawn", "id": "lamp", "class": "/Script/AssetDocumentTests.AssetDocumentLevelTestActor", "attach_to": "%s",
			 "transform": {"location": {"X": 0, "Y": 0, "Z": 20}}},
			{"op": "spawn", "id": "door", "asset": "%s", "attach_to": "%s", "transform": {"rotation": {"Yaw": 45}}}]})"),
			*ParentPathText, *Door.PackageName, *ParentPathText), TEXT("spawn attach_to"));
		TestTrue(TEXT("spawn attach_to succeeds"), Spawned.bSuccess);
		const AActor* Lamp = BatchActor(Spawned, TEXT("lamp"));
		const AActor* DoorActor = BatchActor(Spawned, TEXT("door"));
		TestTrue(TEXT("lamp attached at its relative place"), Lamp != nullptr && Lamp->GetAttachParentActor() != nullptr
			&& Lamp->GetActorLocation().Equals(FVector(100, 0, 20), 0.01));
		TestTrue(TEXT("lamp in the parent's folder"), Lamp != nullptr && Lamp->GetFolderPath() == FName(TEXT("Props")));
		TestTrue(TEXT("door rotated relative to the parent"), DoorActor != nullptr
			&& DoorActor->GetActorQuat().Equals(FRotator(0, 135, 0).Quaternion(), 1.0e-4));
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
	}

	// Attach to a component's socket.
	{
		const FLevelToolResult Socketed = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "add_component", "actor": "%s", "class": "/Script/AssetDocumentTests.AssetDocumentLevelTestSocketComponent", "name": "Arm"},
			{"op": "detach", "actor": "%s"},
			{"op": "attach", "actor": "%s", "parent": "%s", "component": "Arm", "socket": "Tip"}]})"),
			*ParentPathText, *GrandPath, *GrandPath, *ParentPathText), TEXT("attach to a socket"));
		TestTrue(TEXT("attach to a socket succeeds"), Socketed.bSuccess);
		const AActor* Grand = FindActorByPath(GrandPath);
		const USceneComponent* GrandRoot = Grand != nullptr ? Grand->GetRootComponent() : nullptr;
		TestTrue(TEXT("on Arm's socket"), GrandRoot != nullptr && GrandRoot->GetAttachParent() != nullptr
			&& GrandRoot->GetAttachParent()->GetName() == TEXT("Arm") && GrandRoot->GetAttachSocketName() == FName(TEXT("Tip")));
		TestTrue(TEXT("world location kept"), WorldLocation(GrandPath).Equals(FVector(300, 200, 0), 0.01));
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());

		const FLevelToolResult Missing = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "attach", "actor": "%s", "parent": "%s", "socket": "Nope"}]})"), *GrandPath, *ParentPathText), TEXT("no such socket"));
		TestTrue(TEXT("no such socket refused at the socket"), HasCode(Missing, TEXT("Level.AttachRejected"), TEXT("/ops/0/socket")));
	}

	// Detach keeps the world transform; Undo of the first batch restores the folders.
	{
		const FLevelToolResult Detached = Edit(*this, FString::Printf(TEXT(R"({"ops": [
			{"op": "detach", "actor": "%s"}, {"op": "detach", "actor": "%s"}]})"), *GrandPath, *RockPath), TEXT("detach"));
		TestTrue(TEXT("detach succeeds (an unattached actor is left alone)"), Detached.bSuccess);
		TestEqual(TEXT("grandchild detached"), ParentPath(GrandPath), FString());
		TestTrue(TEXT("grandchild keeps its world location"), WorldLocation(GrandPath).Equals(FVector(300, 200, 0), 0.01));
		TestEqual(TEXT("only the attached actor changes"), Changes(Detached).Num(), 1);
		TestTrue(TEXT("Undo detach"), GEditor->UndoTransaction());
		TestTrue(TEXT("Undo attach"), GEditor->UndoTransaction());
		TestEqual(TEXT("child unattached"), ParentPath(ChildPath), FString());
		TestEqual(TEXT("child's folder restored"), Folder(ChildPath), FName());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelRawDetachTest,
	"UEAssetDoc.Level.Raw.Detach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelRawDetachTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	if (!TestNotNull(TEXT("map created"), World))
	{
		return false;
	}
	TestTrue(TEXT("saved, so dirtiness is observable"), FScopedTestMaps::SaveWorld(*World));
	struct FOutcome
	{
		bool bAttached = true;
		FTransform World;
		FName Folder;
		FName GrandFolder;
		FString UndoTitle;
		bool bParentTransacted = false;
	};
	// A parent (rotated, in a folder, under a top parent in another folder), a child
	// attached to it, and a grandchild on the child.
	auto Run = [this, World](bool bRaw) -> FOutcome
	{
		ULevel& Level = *World->PersistentLevel;
		AActor* Top = SpawnActor(Level, AAssetDocumentLevelTestActor::StaticClass(), FVector(0, 0, 0), TEXT("Top"));
		AActor* Parent = SpawnActor(Level, AAssetDocumentLevelTestActor::StaticClass(), FVector(100, 0, 0), TEXT("Parent"));
		AActor* Child = SpawnActor(Level, AAssetDocumentLevelTestActor::StaticClass(), FVector(300, 50, 20), TEXT("Child"));
		AActor* Grand = SpawnActor(Level, AAssetDocumentLevelTestActor::StaticClass(), FVector(300, 200, 0), TEXT("Grand"));
		Top->SetFolderPath(TEXT("TopFolder"));
		Parent->SetActorRotation(FRotator(0, 90, 0));
		GEditor->ParentActors(Top, Parent, NAME_None);
		GEditor->ParentActors(Parent, Child, NAME_None);
		GEditor->ParentActors(Child, Grand, NAME_None);
		Parent->SetFolderPath(TEXT("Elsewhere"));
		const FString ChildPath = GetActorPath(*Child);
		const FString GrandPath = GetActorPath(*Grand);
		ResetUndo();
		if (bRaw)
		{
			TestTrue(TEXT("raw detach reports a detach"), UE::AssetDocumentLevel::Private::DetachLevelActor(*Child));
		}
		else
		{
			GEditor->SelectNone(false, true, false);
			GEditor->SelectActor(Child, true, false, true);
			TestTrue(TEXT("editor detach reports a detach"), GEditor->DetachSelectedActors());
			GEditor->SelectNone(false, true, false);
		}
		FOutcome Outcome;
		const AActor* After = FindActorByPath(ChildPath);
		const AActor* GrandAfter = FindActorByPath(GrandPath);
		Outcome.bAttached = After == nullptr || After->GetAttachParentActor() != nullptr;
		Outcome.World = After != nullptr ? After->GetActorTransform() : FTransform::Identity;
		Outcome.Folder = After != nullptr ? After->GetFolderPath() : NAME_None;
		Outcome.GrandFolder = GrandAfter != nullptr ? GrandAfter->GetFolderPath() : NAME_None;
		Outcome.UndoTitle = NewestUndoTitle();
		Outcome.bParentTransacted = UndoQueueLength() == 1;
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
		After = FindActorByPath(ChildPath);
		TestTrue(TEXT("Undo re-attaches"), After != nullptr && After->GetAttachParentActor() != nullptr);
		for (AActor* Actor : {Grand, Child, Parent, Top})
		{
			World->EditorDestroyActor(Actor, false);
		}
		ResetUndo();
		return Outcome;
	};
	const FOutcome Editor = Run(false);
	const FOutcome Raw = Run(true);
	TestFalse(TEXT("editor: detached"), Editor.bAttached);
	TestFalse(TEXT("raw: detached"), Raw.bAttached);
	TestTrue(TEXT("same world transform (kept)"), Raw.World.Equals(Editor.World, 0.01) && Raw.World.GetLocation().Equals(FVector(300, 50, 20), 0.01));
	TestEqual(TEXT("same folder (the old topmost parent's)"), Raw.Folder, Editor.Folder);
	TestEqual(TEXT("the topmost parent's folder"), Raw.Folder, FName(TEXT("TopFolder")));
	TestEqual(TEXT("same folder for the attached grandchild"), Raw.GrandFolder, Editor.GrandFolder);
	TestEqual(TEXT("same Undo title"), Raw.UndoTitle, Editor.UndoTitle);
	TestEqual(TEXT("one Undo entry each"), Raw.bParentTransacted, Editor.bParentTransacted);
	TestFalse(TEXT("an unattached actor is left alone"), UE::AssetDocumentLevel::Private::DetachLevelActor(
		*SpawnActor(*World->PersistentLevel, AAssetDocumentLevelTestActor::StaticClass(), FVector::ZeroVector, TEXT("Lone"))));
	return true;
}

#endif
