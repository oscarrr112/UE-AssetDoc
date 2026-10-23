// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// The editor tools:
// UEAssetDoc.Level.UndoTool.UndoRedo: undo_editor undoes / redoes only the entry the caller names
//   (by the title or id edit_level returned); any other entry on top, e.g. the user's own edit, is refused with
//   Undo.EntryMismatch naming it, and nothing changes.
// UEAssetDoc.Level.MapTools.OpenAndNew: open_level / new_level change the editor's map like File >
//   Open Level / New Level, never losing unsaved changes (refused, or saved first on request).
// UEAssetDoc.Level.MapTools.FolderOnlySave: a folder-only edit_level batch with save saves what it
//   changed (the map, already dirty before; a World Partition folder package); an empty folder of a level without actor
//   folder objects is session-only in the editor, and the result says so (Level.FolderNotSaved).
// UEAssetDoc.Level.NotFoundCandidates: an actor or component reference that matches nothing names the
//   close ones in the message ("CeilingLamp" for "CeilingLight").
// UEAssetDoc.Level.ViewportScreenshot.Redraws: the viewport screenshot shows an edit made a moment
//   before, with no editor tick in between (it showed the last presented frame). Skipped without a level viewport.

#include "AssetDocumentViewportCapture.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "LevelEditorViewport.h"
#include "EngineUtils.h"
#include "LevelEditTestSupport.h"
#include "LevelEditorUndo.h"
#include "LevelMapTools.h"
#include "ScopedTransaction.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentLevelEditorToolsTests
{
using namespace AssetDocumentLevelEditTests;

FLevelToolResult Run(FAutomationTestBase& Test, FLevelToolResult (*Tool)(const TSharedPtr<FJsonObject>&), const FString& Json, const TCHAR* Stage)
{
	const FLevelToolResult Result = Tool(ParseJsonObject(Json));
	Test.AddInfo(FString::Printf(TEXT("%s: %d %s"), Stage, Result.HttpStatus, *Result.Message));
	return Result;
}

FString TransactionField(const FLevelToolResult& Result, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Transaction = nullptr;
	FString Value;
	if (Result.Payload.IsValid() && Result.Payload->TryGetObjectField(TEXT("transaction"), Transaction))
	{
		(*Transaction)->TryGetStringField(Field, Value);
	}
	return Value;
}

FString Message(const FLevelToolResult& Result, const TCHAR* Code)
{
	const FAssetDocumentDiagnostic* Diagnostic = FindCode(Result, Code);
	return Diagnostic != nullptr ? Diagnostic->Message : FString();
}

AActor* FindByLabel(const TCHAR* Label)
{
	UWorld* World = GetEditorWorld();
	for (TActorIterator<AActor> It(World); World != nullptr && It; ++It)
	{
		if (It->GetActorLabel() == Label)
		{
			return *It;
		}
	}
	return nullptr;
}

FString OpenPackage()
{
	UWorld* World = GetEditorWorld();
	return World != nullptr ? World->GetPackage()->GetName() : FString();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelUndoToolTest, "UEAssetDoc.Level.UndoTool.UndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelUndoToolTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditorToolsTests;
	using UE::AssetDocumentLevel::Private::RunEditorUndo;
	FScopedTestMaps Maps;
	if (!TestNotNull(TEXT("map created"), Maps.NewMap(TEXT("Room"), false)))
	{
		return false;
	}
	ResetUndo();
	const FLevelToolResult Spawned = Edit(*this, TEXT(R"({"description": "AI: lamp", "ops": [
		{"op": "spawn", "id": "lamp", "class": "/Script/Engine.PointLight", "label": "Lamp"}]})"), TEXT("spawn"));
	const FString Title = TransactionField(Spawned, TEXT("title"));
	const FString Id = TransactionField(Spawned, TEXT("id"));
	const FString LampPath = BatchPath(Spawned, TEXT("lamp"));
	if (!TestTrue(TEXT("spawned"), Spawned.bSuccess) || !TestFalse(TEXT("edit_level returns the entry's id"), Id.IsEmpty()))
	{
		return false;
	}
	TestEqual(TEXT("the title is the Undo entry's"), Title, NewestUndoTitle());

	// Naming no entry is refused: the tool never undoes blind.
	TestTrue(TEXT("no expectation refused"), HasCode(Run(*this, &RunEditorUndo, TEXT(R"({"direction": "undo"})"), TEXT("blind")),
		TEXT("Level.InvalidRequest"), TEXT("/expect_title")));

	// Another title: refused, naming the actual entry.
	const FLevelToolResult Wrong = Run(*this, &RunEditorUndo, TEXT(R"({"expect_title": "AI: something else"})"), TEXT("wrong title"));
	TestTrue(TEXT("wrong title: Undo.EntryMismatch"), HasCode(Wrong, TEXT("Undo.EntryMismatch"), TEXT("/expect_title")));
	TestTrue(TEXT("the message names the actual entry"), Message(Wrong, TEXT("Undo.EntryMismatch")).Contains(Title));
	TestNotNull(TEXT("nothing undone"), FindActorByPath(LampPath));

	// The user's own edit on top: refused, the user's edit stays.
	AActor* Lamp = FindActorByPath(LampPath);
	{
		const FScopedTransaction Transaction(FText::FromString(TEXT("User: move lamp")));
		Lamp->Modify();
		Lamp->SetActorLocation(FVector(0.0, 0.0, 500.0));
	}
	const FLevelToolResult OverUser = Run(*this, &RunEditorUndo, FString::Printf(TEXT(R"({"expect_title": "%s"})"), *Title), TEXT("over user"));
	TestTrue(TEXT("the user's entry on top: Undo.EntryMismatch"), HasCode(OverUser, TEXT("Undo.EntryMismatch")));
	TestTrue(TEXT("naming the user's entry"), Message(OverUser, TEXT("Undo.EntryMismatch")).Contains(TEXT("User: move lamp")));
	TestEqual(TEXT("the user's move stays"), FindActorByPath(LampPath)->GetActorLocation().Z, 500.0);
	TestTrue(TEXT("the user undoes their move"), GEditor->UndoTransaction());

	// The right entry (title and id): undone; the result says what is next.
	const FLevelToolResult Undone = Run(*this, &RunEditorUndo, FString::Printf(TEXT(R"({"expect_title": "%s", "expect_id": "%s"})"),
		*Title, *Id), TEXT("undo"));
	TestTrue(TEXT("undone"), Undone.bSuccess);
	TestNull(TEXT("the lamp is gone"), FindActorByPath(LampPath));
	TestEqual(TEXT("the result names the entry"), PayloadString(Undone, TEXT("title")), Title);
	TestTrue(TEXT("and that it can be redone"), PayloadBool(Undone, TEXT("can_redo")));

	// Redo by id; the next Redo is then the user's move, which is not ours to redo.
	const FLevelToolResult Redone = Run(*this, &RunEditorUndo, FString::Printf(TEXT(R"({"direction": "redo", "expect_id": "%s"})"), *Id),
		TEXT("redo"));
	TestTrue(TEXT("redone"), Redone.bSuccess);
	TestNotNull(TEXT("the lamp is back"), FindActorByPath(LampPath));
	TestTrue(TEXT("the user's move is not redone by our title"), HasCode(Run(*this, &RunEditorUndo, FString::Printf(
		TEXT(R"({"direction": "redo", "expect_title": "%s"})"), *Title), TEXT("redo user")), TEXT("Undo.EntryMismatch")));
	TestTrue(TEXT("an unknown id is refused"), HasCode(Run(*this, &RunEditorUndo,
		TEXT(R"({"expect_id": "00000000000000000000000000000001"})"), TEXT("other id")), TEXT("Undo.EntryMismatch"), TEXT("/expect_id")));

	ResetUndo();
	TestTrue(TEXT("an empty history: Undo.NothingToUndo"), HasCode(Run(*this, &RunEditorUndo, FString::Printf(
		TEXT(R"({"expect_title": "%s"})"), *Title), TEXT("empty")), TEXT("Undo.NothingToUndo")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelMapToolsTest, "UEAssetDoc.Level.MapTools.OpenAndNew",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelMapToolsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditorToolsTests;
	using UE::AssetDocumentLevel::Private::RunNewLevel;
	using UE::AssetDocumentLevel::Private::RunOpenLevel;
	FScopedTestMaps Maps;
	const FString Other = Maps.PackageName(TEXT("Hallway"));
	const FString Room = Maps.PackageName(TEXT("Room"));
	if (!TestNotNull(TEXT("Hallway created"), Maps.NewMap(TEXT("Hallway"), false))
		|| !TestNotNull(TEXT("Room created"), Maps.NewMap(TEXT("Room"), false)))
	{
		return false;
	}
	// An unsaved change in Room.
	TestTrue(TEXT("lamp spawned"), Edit(*this, TEXT(R"({"ops": [{"op": "spawn", "class": "/Script/Engine.PointLight", "label": "Lamp"}]})"),
		TEXT("spawn")).bSuccess);

	// A map that does not exist: the close one is named.
	const FLevelToolResult Missing = Run(*this, &RunOpenLevel, FString::Printf(TEXT(R"({"path": "%s"})"), *Maps.PackageName(TEXT("Halway"))),
		TEXT("typo"));
	TestTrue(TEXT("Level.MapNotFound"), HasCode(Missing, TEXT("Level.MapNotFound"), TEXT("/path")));
	TestTrue(*FString::Printf(TEXT("names the close map (%s)"), *Message(Missing, TEXT("Level.MapNotFound"))),
		Message(Missing, TEXT("Level.MapNotFound")).Contains(FString::Printf(TEXT("Close: %s"), *Other)));

	// Opening another map would lose the lamp: refused, nothing changes.
	const FLevelToolResult Refused = Run(*this, &RunOpenLevel, FString::Printf(TEXT(R"({"path": "%s"})"), *Other), TEXT("unsaved"));
	TestTrue(TEXT("Level.UnsavedChangesWouldBeLost"), HasCode(Refused, TEXT("Level.UnsavedChangesWouldBeLost")));
	TestTrue(TEXT("the message lists the dirty map"), Message(Refused, TEXT("Level.UnsavedChangesWouldBeLost")).Contains(Room));
	TestEqual(TEXT("Room stays open"), OpenPackage(), Room);
	TestNotNull(TEXT("with the lamp"), FindByLabel(TEXT("Lamp")));

	// save_first: Room is saved, then Hallway opens.
	const FLevelToolResult Opened = Run(*this, &RunOpenLevel, FString::Printf(TEXT(R"({"path": "%s.Hallway", "save_first": true})"), *Other),
		TEXT("save first"));
	TestTrue(TEXT("opened"), Opened.bSuccess);
	TestEqual(TEXT("Hallway is open"), OpenPackage(), Other);
	TestTrue(TEXT("Room was saved first"), PayloadStrings(Opened, TEXT("saved_before")).Contains(Room));
	TestTrue(TEXT("the payload is the world summary"), Opened.Payload.IsValid() && Opened.Payload->HasField(TEXT("world")));
	TestEqual(TEXT("a map change clears Undo"), UndoQueueLength(), 0);
	const FLevelToolResult Back = Run(*this, &RunOpenLevel, FString::Printf(TEXT(R"({"path": "%s"})"), *Room), TEXT("back"));
	TestTrue(TEXT("Room reopens"), Back.bSuccess);
	TestNotNull(TEXT("with the saved lamp"), FindByLabel(TEXT("Lamp")));
	const FLevelToolResult Again = Run(*this, &RunOpenLevel, FString::Printf(TEXT(R"({"path": "%s"})"), *Room), TEXT("again"));
	TestTrue(TEXT("the open map: nothing reloaded"), Again.bSuccess && PayloadBool(Again, TEXT("already_open")));
	// save_first on the open map: nothing is reloaded, its unsaved changes are saved.
	TestTrue(TEXT("second lamp spawned"), Edit(*this, TEXT(R"({"ops": [{"op": "spawn", "class": "/Script/Engine.PointLight", "label": "Lamp2"}]})"),
		TEXT("spawn 2")).bSuccess);
	TestTrue(TEXT("Room is dirty"), GetEditorWorld()->GetPackage()->IsDirty());
	const FLevelToolResult SavedOpen = Run(*this, &RunOpenLevel, FString::Printf(TEXT(R"({"path": "%s", "save_first": true})"), *Room),
		TEXT("save first, open map"));
	TestTrue(TEXT("the open map with save_first: nothing reloaded"), SavedOpen.bSuccess && PayloadBool(SavedOpen, TEXT("already_open")));
	TestTrue(TEXT("the open map with save_first: saved"), PayloadStrings(SavedOpen, TEXT("saved_before")).Contains(Room)
		&& !GetEditorWorld()->GetPackage()->IsDirty());
	TestNotNull(TEXT("the second lamp stays"), FindByLabel(TEXT("Lamp2")));

	// new_level: the New Level dialog's empty levels and a template, saved at path and opened.
	const FString Empty = Maps.PackageName(TEXT("NewEmpty"));
	const FLevelToolResult NewEmpty = Run(*this, &RunNewLevel, FString::Printf(TEXT(R"({"path": "%s"})"), *Empty), TEXT("new empty"));
	TestTrue(TEXT("empty level created"), NewEmpty.bSuccess);
	TestEqual(TEXT("and open"), OpenPackage(), Empty);
	TestTrue(TEXT("and saved"), FPackageName::DoesPackageExist(Empty));
	TestNull(TEXT("not partitioned"), GetEditorWorld()->GetWorldPartition());
	const FString OpenWorld = Maps.PackageName(TEXT("NewOpenWorld"));
	const FLevelToolResult NewOpenWorld = Run(*this, &RunNewLevel, FString::Printf(TEXT(R"({"path": "%s", "template": "empty_open_world"})"),
		*OpenWorld), TEXT("new empty open world"));
	TestTrue(TEXT("empty open world created"), NewOpenWorld.bSuccess && OpenPackage() == OpenWorld);
	TestNotNull(TEXT("partitioned"), GetEditorWorld()->GetWorldPartition());
	const FString Basic = Maps.PackageName(TEXT("NewBasic"));
	const FLevelToolResult NewBasic = Run(*this, &RunNewLevel, FString::Printf(TEXT(R"({"path": "%s", "template": "basic"})"), *Basic),
		TEXT("new basic"));
	TestTrue(TEXT("basic template level created"), NewBasic.bSuccess && OpenPackage() == Basic);
	TestTrue(TEXT("with the template's actors"), GetEditorWorld()->PersistentLevel->Actors.Num() > 2);
	TestTrue(TEXT("an existing map: Level.MapExists"), HasCode(Run(*this, &RunNewLevel, FString::Printf(TEXT(R"({"path": "%s"})"), *Room),
		TEXT("exists")), TEXT("Level.MapExists")));
	const FLevelToolResult NoTemplate = Run(*this, &RunNewLevel, FString::Printf(TEXT(R"({"path": "%s", "template": "cave"})"),
		*Maps.PackageName(TEXT("NewCave"))), TEXT("no template"));
	TestTrue(TEXT("Level.TemplateNotFound lists the templates"), HasCode(NoTemplate, TEXT("Level.TemplateNotFound"))
		&& Message(NoTemplate, TEXT("Level.TemplateNotFound")).Contains(TEXT("empty_open_world")));

	// A never-saved level with changes cannot be saved by path: refused even with save_first.
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}
	SpawnActor(*GetEditorWorld()->PersistentLevel, APointLight::StaticClass(), FVector::ZeroVector, TEXT("Unsaved"));
	const FLevelToolResult Untitled = Run(*this, &RunOpenLevel, FString::Printf(TEXT(R"({"path": "%s", "save_first": true})"), *Room),
		TEXT("untitled"));
	TestTrue(TEXT("never-saved level refused"), HasCode(Untitled, TEXT("Level.UnsavedChangesWouldBeLost")));
	TestNotNull(TEXT("its actor stays"), FindByLabel(TEXT("Unsaved")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelFolderOnlySaveTest, "UEAssetDoc.Level.MapTools.FolderOnlySave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelFolderOnlySaveTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditorToolsTests;
	FScopedTestMaps Maps;
	const FString Room = Maps.PackageName(TEXT("Room"));
	if (!TestNotNull(TEXT("Room created"), Maps.NewMap(TEXT("Room"), false)))
	{
		return false;
	}
	TestTrue(TEXT("a lamp in Lights, saved"), Edit(*this, TEXT(R"({"ops": [{"op": "spawn", "class": "/Script/Engine.PointLight",
		"label": "Lamp", "folder": "Lights"}], "save": true})"), TEXT("spawn")).bSuccess);
	// The map has unsaved changes before the folder batch (the already dirty map must not be left out).
	TestTrue(TEXT("an unsaved spawn"), Edit(*this, TEXT(R"({"ops": [{"op": "spawn", "class": "/Script/Engine.PointLight", "label": "Spare"}]})"),
		TEXT("unsaved spawn")).bSuccess);
	const FLevelToolResult Renamed = Edit(*this, TEXT(R"({"ops": [{"op": "rename_folder", "from": "Lights", "to": "Lamps"}], "save": true})"),
		TEXT("rename folder"));
	TestTrue(TEXT("a folder-only batch with save: the map is saved"), Renamed.bSuccess
		&& PayloadStrings(Renamed, TEXT("saved_packages")).Contains(Room) && !GetEditorWorld()->GetPackage()->IsDirty());
	// An empty folder of a level without actor folder objects is not part of any package: the result says so.
	const FLevelToolResult Created = Edit(*this, TEXT(R"({"ops": [{"op": "create_folder", "path": "Empty"}], "save": true})"),
		TEXT("create empty folder"));
	TestTrue(TEXT("the empty folder is created"), Created.bSuccess);
	TestTrue(*FString::Printf(TEXT("Level.FolderNotSaved names it (%s)"), *Message(Created, TEXT("Level.FolderNotSaved"))),
		Message(Created, TEXT("Level.FolderNotSaved")).Contains(TEXT("Empty")));
	TestTrue(*FString::Printf(TEXT("the message says nothing was saved (%s)"), *Created.Message), Created.Message.Contains(TEXT("Nothing to save")));
	const FLevelToolResult Filled = Edit(*this, TEXT(R"({"ops": [{"op": "create_folder", "path": "Props"},
		{"op": "spawn", "class": "/Script/Engine.PointLight", "label": "Prop", "folder": "Props"}], "save": true})"), TEXT("create and fill"));
	TestTrue(TEXT("a folder that gets an actor: no warning, saved"), Filled.bSuccess && !HasCode(Filled, TEXT("Level.FolderNotSaved"))
		&& PayloadStrings(Filled, TEXT("saved_packages")).Contains(Room));

	// World Partition: a folder is an object with its own package, saved by a folder-only batch.
	if (!TestNotNull(TEXT("partitioned map created"), Maps.NewMap(TEXT("World"), true)))
	{
		return false;
	}
	const FLevelToolResult Partitioned = Edit(*this, TEXT(R"({"ops": [{"op": "create_folder", "path": "Zone"}], "save": true})"),
		TEXT("create folder, partitioned"));
	TestTrue(TEXT("the folder package is saved"), Partitioned.bSuccess && PayloadStrings(Partitioned, TEXT("saved_packages")).Num() == 1
		&& !HasCode(Partitioned, TEXT("Level.FolderNotSaved")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelNotFoundCandidatesTest, "UEAssetDoc.Level.NotFoundCandidates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelNotFoundCandidatesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditorToolsTests;
	FScopedTestMaps Maps;
	if (!TestNotNull(TEXT("map created"), Maps.NewMap(TEXT("Room"), false)))
	{
		return false;
	}
	const FLevelToolResult Spawned = Edit(*this, TEXT(R"({"ops": [
		{"op": "spawn", "id": "light", "class": "/Script/Engine.PointLight", "label": "CeilingLight"},
		{"op": "spawn", "class": "/Script/Engine.PointLight", "label": "Floor"}]})"), TEXT("spawn"));
	const FString LightPath = BatchPath(Spawned, TEXT("light"));
	if (!TestFalse(TEXT("spawned"), LightPath.IsEmpty()))
	{
		return false;
	}
	auto Candidates = [](const FAssetDocumentDiagnostic* Diagnostic)
	{
		TArray<FString> Values;
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Diagnostic != nullptr && Diagnostic->Details.IsValid() && Diagnostic->Details->TryGetArrayField(TEXT("candidates"), Array))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				Values.Add(Value->AsString());
			}
		}
		return Values;
	};

	const FLevelToolResult ByLabel = Edit(*this, TEXT(R"({"ops": [{"op": "set", "actor": {"label": "CeilingLamp"}, "label": "X"}]})"),
		TEXT("label typo"));
	const FAssetDocumentDiagnostic* Label = FindCode(ByLabel, TEXT("Level.ActorNotFound"));
	TestTrue(*FString::Printf(TEXT("the message names the close label (%s)"), Label != nullptr ? *Label->Message : TEXT("none")),
		Label != nullptr && Label->Message.Contains(TEXT("Close labels: CeilingLight.")));
	TestEqual(TEXT("details.candidates holds its path"), Candidates(Label), TArray<FString>{LightPath});

	const FLevelToolResult ByPath = Edit(*this, FString::Printf(TEXT(R"({"ops": [{"op": "set", "actor": "%sx", "label": "X"}]})"), *LightPath),
		TEXT("path typo"));
	const FAssetDocumentDiagnostic* Path = FindCode(ByPath, TEXT("Level.ActorNotFound"));
	TestTrue(*FString::Printf(TEXT("the message names the close actor (%s)"), Path != nullptr ? *Path->Message : TEXT("none")),
		Path != nullptr && Path->Message.Contains(FString::Printf(TEXT("Close actors: %s"), *LightPath)));

	const FLevelToolResult ByComponent = Edit(*this, FString::Printf(TEXT(R"({"ops": [{"op": "set", "actor": "%s",
		"components": {"LightComponent": {"properties": {"Intensity": 5}}}}]})"), *LightPath), TEXT("component typo"));
	const FAssetDocumentDiagnostic* Component = FindCode(ByComponent, TEXT("Level.ComponentNotFound"));
	TestTrue(*FString::Printf(TEXT("the message names the close component (%s)"), Component != nullptr ? *Component->Message : TEXT("none")),
		Component != nullptr && Component->Message.Contains(TEXT("Close: LightComponent0")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelViewportScreenshotTest, "UEAssetDoc.Level.ViewportScreenshot.Redraws",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelViewportScreenshotTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditorToolsTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	FLevelEditorViewportClient* Client = FAssetDocumentViewportCapture::FindViewportClient();
	if (!TestNotNull(TEXT("map created"), World))
	{
		return false;
	}
	if (Client == nullptr || Client->Viewport == nullptr || Client->Viewport->GetSizeXY().X <= 0)
	{
		AddInfo(TEXT("No Level Editor viewport in this session: skipped."));
		return true;
	}
	// Unlit, looking down +X at the origin of an empty level.
	const FVector OldLocation = Client->GetViewLocation();
	const FRotator OldRotation = Client->GetViewRotation();
	const EViewModeIndex OldMode = Client->GetViewMode();
	Client->SetViewMode(VMI_Unlit);
	Client->SetViewLocation(FVector(-600.0, 0.0, 0.0));
	Client->SetViewRotation(FRotator::ZeroRotator);
	auto Bright = [](const TArray<FColor>& Pixels)
	{
		int32 Count = 0;
		for (const FColor& Pixel : Pixels)
		{
			Count += Pixel.R + Pixel.G + Pixel.B > 150 ? 1 : 0;
		}
		return Count;
	};
	TArray<FColor> Before;
	FIntPoint Size;
	int32 Status = 0;
	FString Error;
	const bool bBefore = FAssetDocumentViewportCapture::Capture(Client, Before, Size, Status, Error);
	if (!bBefore)
	{
		AddInfo(FString::Printf(TEXT("The viewport cannot be read in this session (%d %s): skipped."), Status, *Error));
	}
	else
	{
		// A big white cube in front of the camera, then a capture with no tick in between.
		AStaticMeshActor* Cube = Cast<AStaticMeshActor>(SpawnActor(*World->PersistentLevel, AStaticMeshActor::StaticClass(),
			FVector::ZeroVector, TEXT("Cube")));
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (TestNotNull(TEXT("cube spawned"), Cube) && TestNotNull(TEXT("cube mesh"), Mesh))
		{
			Cube->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			Cube->SetActorScale3D(FVector(4.0));
			TArray<FColor> After;
			TestTrue(*FString::Printf(TEXT("captured after the edit (%s)"), *Error), FAssetDocumentViewportCapture::Capture(Client, After, Size,
				Status, Error));
			AddInfo(FString::Printf(TEXT("bright pixels: %d before, %d after (of %d)"), Bright(Before), Bright(After), After.Num()));
			TestTrue(TEXT("the screenshot shows the cube placed a moment before"), Bright(After) > Bright(Before) + After.Num() / 20);
		}
	}
	Client->SetViewMode(OldMode);
	Client->SetViewLocation(OldLocation);
	Client->SetViewRotation(OldRotation);
	Client->Invalidate();
	return true;
}

#endif
