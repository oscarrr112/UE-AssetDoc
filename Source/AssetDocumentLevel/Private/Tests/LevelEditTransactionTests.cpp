// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// edit_level transaction semantics (design §3.1, §3.3, decision D1): one batch is one
// Undo entry; a failure mid-batch is undone exactly (digest check) without touching
// the user's previous Undo entry; a failure before anything was recorded never undoes
// the user's edit; busy editor / play session refusals touch nothing.

#include "Components/PointLightComponent.h"
#include "Engine/PointLight.h"
#include "LevelEditTestSupport.h"
#include "ScopedTransaction.h"
#include "Tests/AutomationEditorCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentLevelEditTransactionTests
{
using namespace AssetDocumentLevelEditTests;

// The user's own edit: one Undo entry that moves Actor.
void UserMove(AActor& Actor, const FVector& Location)
{
	const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentLevelEditTests", "UserMove", "User move"));
	Actor.Modify();
	Actor.GetRootComponent()->Modify();
	Actor.SetActorLocation(Location);
}

struct FBatchScene
{
	UWorld* World = nullptr;
	FString UserPath;
	FString LampPath;
	FString DoomedPath;
};

bool BuildScene(FAutomationTestBase& Test, const FScopedTestMaps& Maps, FBatchScene& Scene)
{
	Scene.World = Maps.NewMap(TEXT("Room"), false);
	if (!Test.TestNotNull(TEXT("map created"), Scene.World))
	{
		return false;
	}
	ULevel& Level = *Scene.World->PersistentLevel;
	Scene.UserPath = GetActorPath(*SpawnActor(Level, APointLight::StaticClass(), FVector::ZeroVector, TEXT("UserLamp")));
	Scene.LampPath = GetActorPath(*SpawnActor(Level, APointLight::StaticClass(), FVector(100, 0, 0), TEXT("Lamp")));
	Scene.DoomedPath = GetActorPath(*SpawnActor(Level, APointLight::StaticClass(), FVector(200, 0, 0), TEXT("Doomed")));
	ResetUndo();
	UserMove(*FindActorByPath(Scene.UserPath), FVector(0, 0, 77));
	return true;
}

FString BatchJson(const FBatchScene& Scene)
{
	return FString::Printf(TEXT(R"({"description": "AI: batch", "ops": [
		{"op": "spawn", "id": "new", "class": "/Script/Engine.PointLight", "label": "New", "transform": {"location": {"X": 900}}},
		{"op": "set", "actor": "%s", "label": "Renamed", "folder": "Moved", "tags": ["t"],
		 "transform": {"location": {"Z": 300}}, "components": {"LightComponent0": {"properties": {"Intensity": 123}}}},
		{"op": "duplicate", "id": "copy", "actor": "%s", "offset": {"X": 50}, "label": "Copy"},
		{"op": "delete", "actors": ["%s"]}]})"), *Scene.LampPath, *Scene.LampPath, *Scene.DoomedPath);
}

// What the batch can change, as text.
FString SceneState(const FBatchScene& Scene)
{
	TArray<FString> Lines = ListedActorPaths(*Scene.World->PersistentLevel);
	for (const FString& Path : {Scene.UserPath, Scene.LampPath, Scene.DoomedPath})
	{
		AActor* Actor = FindActorByPath(Path);
		Lines.Add(Actor == nullptr ? Path + TEXT(" <gone>") : FString::Printf(TEXT("%s %s %s %s %s %s"), *Path, *Actor->GetActorLabel(false),
			*Actor->GetFolderPath().ToString(), *Actor->GetActorLocation().ToString(), *FString::JoinBy(Actor->Tags, TEXT(","),
			[](const FName& Tag) { return Tag.ToString(); }), *ReadValue(Cast<APointLight>(Actor)->PointLightComponent, TEXT("Intensity"))));
	}
	return FString::Join(Lines, TEXT("\n"));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelTransactionUndoRedoTest,
	"UEAssetDoc.Level.Transaction.UndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelTransactionUndoRedoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTransactionTests;
	FScopedTestMaps Maps;
	FBatchScene Scene;
	if (!BuildScene(*this, Maps, Scene))
	{
		return false;
	}
	const FString Before = SceneState(Scene);
	TestEqual(TEXT("the user's edit is the only Undo entry"), UndoQueueLength(), 1);

	const FLevelToolResult Result = Edit(*this, BatchJson(Scene), TEXT("batch"));
	if (!TestTrue(TEXT("batch succeeds"), Result.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("one Undo entry for four ops"), UndoQueueLength(), 2);
	TestEqual(TEXT("titled by description"), NewestUndoTitle(), FString(TEXT("AI: batch")));
	TestTrue(TEXT("undoable"), Result.Payload->GetObjectField(TEXT("transaction"))->GetBoolField(TEXT("undoable")));
	const FString After = SceneState(Scene);
	TestNotEqual(TEXT("the batch changed the level"), After, Before);
	AActor* Lamp = FindActorByPath(Scene.LampPath);
	TestEqual(TEXT("renamed"), Lamp->GetActorLabel(false), FString(TEXT("Renamed")));
	TestEqual(TEXT("partial location"), Lamp->GetActorLocation(), FVector(100, 0, 300));
	AActor* Copy = BatchActor(Result, TEXT("copy"));
	if (TestNotNull(TEXT("duplicate"), Copy))
	{
		TestEqual(TEXT("duplicate offset applies to the duplicated actor's state at that point"), Copy->GetActorLocation(), FVector(150, 0, 300));
		TestEqual(TEXT("duplicate label"), Copy->GetActorLabel(false), FString(TEXT("Copy")));
		TestEqual(TEXT("duplicate keeps the earlier op's intensity"), Cast<APointLight>(Copy)->PointLightComponent->Intensity, 123.0f);
		TestFalse(TEXT("duplicate is not selected"), Copy->IsSelected());
	}
	TestNull(TEXT("deleted"), FindActorByPath(Scene.DoomedPath));

	TestTrue(TEXT("Ctrl+Z"), GEditor->UndoTransaction());
	TestEqual(TEXT("one Undo restores the level"), SceneState(Scene), Before);
	TestNull(TEXT("spawned actor gone"), FindActorByPath(BatchPath(Result, TEXT("new"))));
	TestTrue(TEXT("Ctrl+Y"), GEditor->RedoTransaction());
	TestEqual(TEXT("Redo re-applies the batch"), SceneState(Scene), After);
	TestTrue(TEXT("Ctrl+Z again"), GEditor->UndoTransaction());
	TestTrue(TEXT("the user's own edit is still undoable"), GEditor->UndoTransaction());
	TestEqual(TEXT("the user's move undone"), FindActorByPath(Scene.UserPath)->GetActorLocation(), FVector::ZeroVector);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelTransactionRollbackMidBatchTest,
	"UEAssetDoc.Level.Transaction.RollbackMidBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelTransactionRollbackMidBatchTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTransactionTests;
	FScopedTestMaps Maps;
	FBatchScene Scene;
	if (!BuildScene(*this, Maps, Scene))
	{
		return false;
	}
	const FString Before = SceneState(Scene);
	for (int32 AfterOp = 0; AfterOp < 4; ++AfterOp)
	{
		const FString Stage = FString::Printf(TEXT("failure after op %d"), AfterOp);
		FLevelToolResult Result;
		{
			UE::AssetDocumentLevel::Private::FScopedLevelEditFailureInjection Injection(AfterOp);
			Result = Edit(*this, BatchJson(Scene), *Stage);
		}
		TestFalse(*(Stage + TEXT(": fails")), Result.bSuccess);
		TestTrue(*(Stage + TEXT(": injected failure reported")), HasCode(Result, TEXT("Level.TestInjectedFailure"),
			*FString::Printf(TEXT("/ops/%d"), AfterOp)));
		TestEqual(*(Stage + TEXT(": phase")), PayloadString(Result, TEXT("phase")), FString(TEXT("apply")));
		TestTrue(*(Stage + TEXT(": rolled back")), PayloadBool(Result, TEXT("rolled_back")));
		TestEqual(*(Stage + TEXT(": rollback")), PayloadString(Result, TEXT("rollback")), FString(TEXT("Undone")));
		TestEqual(*(Stage + TEXT(": digest equal")), PayloadString(Result, TEXT("rollback_check")), FString(TEXT("equal")));
		TestEqual(*(Stage + TEXT(": level as before")), SceneState(Scene), Before);
		TestEqual(*(Stage + TEXT(": Undo history unchanged")), UndoQueueLength(), 1);
		TestEqual(*(Stage + TEXT(": the user's edit is the newest entry")), NewestUndoTitle(), FString(TEXT("User move")));
	}
	TestTrue(TEXT("the user's edit is still undoable"), GEditor->UndoTransaction());
	TestEqual(TEXT("the user's move undone"), FindActorByPath(Scene.UserPath)->GetActorLocation(), FVector::ZeroVector);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelTransactionEmptyBatchFailureTest,
	"UEAssetDoc.Level.Transaction.EmptyBatchFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelTransactionEmptyBatchFailureTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTransactionTests;
	FScopedTestMaps Maps;
	FBatchScene Scene;
	if (!BuildScene(*this, Maps, Scene))
	{
		return false;
	}
	FLevelToolResult Result;
	{
		UE::AssetDocumentLevel::Private::FScopedLevelEditFailureInjection Injection(-1);
		Result = Edit(*this, BatchJson(Scene), TEXT("failure before the first op"));
	}
	TestFalse(TEXT("fails"), Result.bSuccess);
	TestEqual(TEXT("nothing was recorded"), PayloadString(Result, TEXT("rollback")), FString(TEXT("NothingRecorded")));
	TestTrue(TEXT("rolled back"), PayloadBool(Result, TEXT("rolled_back")));
	TestEqual(TEXT("the user's move is NOT undone"), FindActorByPath(Scene.UserPath)->GetActorLocation(), FVector(0, 0, 77));
	TestEqual(TEXT("Undo history unchanged"), UndoQueueLength(), 1);
	TestEqual(TEXT("the user's edit is the newest entry"), NewestUndoTitle(), FString(TEXT("User move")));
	TestFalse(TEXT("no transaction left open"), GEditor->IsTransactionActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelTransactionBusyTest,
	"UEAssetDoc.Level.Transaction.Busy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelTransactionBusyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTransactionTests;
	FScopedTestMaps Maps;
	FBatchScene Scene;
	if (!BuildScene(*this, Maps, Scene))
	{
		return false;
	}
	const FString Before = SceneState(Scene);
	FLevelToolResult Result;
	{
		// The user is mid-interaction (a gizmo drag holds a transaction open).
		const FScopedTransaction UserDrag(NSLOCTEXT("AssetDocumentLevelEditTests", "Drag", "User drag"));
		Result = Edit(*this, BatchJson(Scene), TEXT("busy"));
	}
	TestFalse(TEXT("refused"), Result.bSuccess);
	TestTrue(TEXT("Level.EditorBusy"), HasCode(Result, TEXT("Level.EditorBusy")));
	TestEqual(TEXT("phase preflight"), PayloadString(Result, TEXT("phase")), FString(TEXT("preflight")));
	TestEqual(TEXT("nothing touched"), SceneState(Scene), Before);
	TestTrue(TEXT("works once the interaction ends"), Edit(*this, BatchJson(Scene), TEXT("after the drag")).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelTransactionPlaySessionTest,
	"UEAssetDoc.Level.Transaction.PlaySession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelTransactionPlaySessionTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTransactionTests;
	{
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
	}
	ResetUndo();
	const FString Spawn = TEXT(R"({"ops": [{"op": "spawn", "class": "/Script/Engine.PointLight"}]})");
	const int32 ActorsBefore = ListedActorPaths(*GetEditorWorld()->PersistentLevel).Num();
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	const double Started = FPlatformTime::Seconds();
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Started, Spawn, ActorsBefore]()
	{
		if (GEditor->PlayWorld == nullptr)
		{
			if (FPlatformTime::Seconds() - Started > 60.0)
			{
				AddError(TEXT("the play session did not start"));
				return true;
			}
			return false;
		}
		const FLevelToolResult Refused = Edit(*this, Spawn, TEXT("during PIE"));
		TestFalse(TEXT("refused during a play session"), Refused.bSuccess);
		TestTrue(TEXT("Level.PlaySessionActive"), HasCode(Refused, TEXT("Level.PlaySessionActive")));
		TestEqual(TEXT("nothing spawned in the editor world"), ListedActorPaths(*GetEditorWorld()->PersistentLevel).Num(), ActorsBefore);
		TestEqual(TEXT("no Undo entry"), UndoQueueLength(), 0);
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Started, Spawn]()
	{
		if (GEditor->PlayWorld != nullptr && FPlatformTime::Seconds() - Started < 120.0)
		{
			return false;
		}
		TestNull(TEXT("the play session ended"), GEditor->PlayWorld.Get());
		TestTrue(TEXT("edit works again"), Edit(*this, Spawn, TEXT("after PIE")).bSuccess);
		ResetUndo();
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		GEditor->NewMap(false);
		return true;
	}));
	return true;
}

#endif
