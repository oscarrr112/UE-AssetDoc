// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Editor Undo history x AssetDocument apply, seen from the level tools
// (AssetDocumentUndoHistory.h FinishApply):
// UEAssetDoc.Level.UndoHistory.AssetApplyClearsLevelUndo: a level batch that spawned a
//   Blueprint actor references the Blueprint in its Undo entry, so an apply that changes the Blueprint
//   removes that entry, and the older batch that recorded the level's actor list the removed batch
//   changed again; newer batches and batches on other objects stay undoable. The result says so
//   (Undo.HistoryTrimmed). An apply to an unrelated asset keeps everything.
// UEAssetDoc.Level.UndoHistory.AssetApplyKeepsUnrelatedLevelUndo: a
//   level batch, then the create of a Camera Animation Sequence (whose apply records its own Sequencer
//   transactions): undo_editor still undoes the batch by its title, the user's edit on top still stops it
//   (Undo.EntryMismatch), and no Undo entry touches the applied asset.

#include "LevelEditTestSupport.h"
#include "LevelEditorUndo.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentLevelUndoHistoryTests
{
using namespace AssetDocumentLevelEditTests;

TArray<FString> UndoTitles()
{
	TArray<FString> Titles;
	for (int32 Index = 0; Index < GEditor->Trans->GetQueueLength(); ++Index)
	{
		const FTransaction* Transaction = GEditor->Trans->GetTransaction(Index);
		Titles.Add(Transaction != nullptr ? Transaction->GetTitle().ToString() : FString());
	}
	return Titles;
}

FLevelToolResult Undo(FAutomationTestBase& Test, const FString& Title, bool bRedo = false)
{
	const FLevelToolResult Result = UE::AssetDocumentLevel::Private::RunEditorUndo(ParseJsonObject(FString::Printf(
		TEXT(R"({"direction": "%s", "expect_title": "%s"})"), bRedo ? TEXT("redo") : TEXT("undo"), *Title)));
	Test.AddInfo(FString::Printf(TEXT("undo_editor %s '%s': %d %s"), bRedo ? TEXT("redo") : TEXT("undo"), *Title, Result.HttpStatus,
		*Result.Message));
	return Result;
}

const FAssetDocumentDiagnostic* FindDiagnostic(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.FindByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelUndoHistoryAssetApplyClearsLevelUndoTest,
	"UEAssetDoc.Level.UndoHistory.AssetApplyClearsLevelUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelUndoHistoryAssetApplyClearsLevelUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelUndoHistoryTests;
	FScopedTestAsset Door(TEXT("BP_Door"));
	FScopedTestAsset Unrelated(TEXT("DA_Unrelated"));
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	if (!TestNotNull(TEXT("map created"), World)
		|| !Door.Apply(*this, DoorBlueprintDocument(Door.PackageName, TEXT("Create"), 1.0), TEXT("create BP_Door"))
		|| !Unrelated.Apply(*this, FString::Printf(TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",")
			TEXT("\"Class\":\"/Script/AssetDocumentTests.TestDataAsset\",\"Action\":\"Create\",\"Properties\":{\"TestString\":\"a\"}}"),
			*Unrelated.PackageName), TEXT("create unrelated")))
	{
		return false;
	}
	ResetUndo();

	// 1. A lamp (records the level's actor list), 2. its colour (the lamp only), 3. a door of the Blueprint
	// (the level's actor list again), 4. the lamp dimmed (the lamp only).
	const FLevelToolResult Lamp = Edit(*this, TEXT(R"({"description": "AI: lamp", "ops": [
		{"op": "spawn", "id": "lamp", "class": "/Script/Engine.PointLight", "label": "Lamp"}]})"), TEXT("spawn lamp"));
	const FString LampPath = BatchPath(Lamp, TEXT("lamp"));
	TestTrue(TEXT("lamp colour"), Edit(*this, FString::Printf(TEXT(R"({"description": "AI: lamp colour", "ops": [
		{"op": "set", "actor": "%s", "components": {"LightComponent0": {"properties": {"LightColor": {"R": 255, "G": 0, "B": 0, "A": 255}}}}}]})"),
		*LampPath), TEXT("lamp colour")).bSuccess);
	const FLevelToolResult Spawned = Edit(*this, FString::Printf(TEXT(R"({"description": "AI: door", "ops": [
		{"op": "spawn", "id": "door", "asset": "%s", "properties": {"Brightness": 2}}]})"), *Door.PackageName), TEXT("spawn door"));
	if (!TestTrue(TEXT("lamp spawned"), Lamp.bSuccess) || !TestTrue(TEXT("door spawned"), Spawned.bSuccess))
	{
		return false;
	}
	TestTrue(TEXT("lamp dimmed"), Edit(*this, FString::Printf(TEXT(R"({"description": "AI: dim lamp", "ops": [
		{"op": "set", "actor": "%s", "components": {"LightComponent0": {"properties": {"Intensity": 100}}}}]})"), *LampPath),
		TEXT("dim lamp")).bSuccess);
	const FString DoorPath = BatchPath(Spawned, TEXT("door"));
	TestEqual(TEXT("the level batches are in Undo"), UndoTitles(),
		TArray<FString>{TEXT("AI: lamp"), TEXT("AI: lamp colour"), TEXT("AI: door"), TEXT("AI: dim lamp")});

	// An apply to an asset the Undo history does not reference keeps it.
	const FAssetDocumentResult UnrelatedResult = Unrelated.ApplyResult(*this, FString::Printf(TEXT("{\"SchemaVersion\":1,")
		TEXT("\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"/Script/AssetDocumentTests.TestDataAsset\",\"Action\":\"Update\",")
		TEXT("\"Properties\":{\"TestString\":\"b\"}}"), *Unrelated.PackageName), TEXT("update unrelated"));
	TestTrue(TEXT("unrelated apply succeeds"), UnrelatedResult.IsSuccess());
	TestEqual(TEXT("unrelated apply keeps the level Undo history"), UndoQueueLength(), 4);
	TestNull(TEXT("and says nothing about it"), FindDiagnostic(UnrelatedResult, TEXT("Undo.HistoryTrimmed")));

	// An apply that changes the spawned actor's Blueprint removes the door batch, and the lamp batch that
	// recorded the level's actor list before the door changed it; the other batches stay.
	const FAssetDocumentResult DoorResult = Door.ApplyResult(*this, DoorBlueprintDocument(Door.PackageName, TEXT("Update"), 5.0),
		TEXT("update BP_Door"));
	TestTrue(TEXT("Blueprint apply succeeds"), DoorResult.IsSuccess());
	AddInfo(FString::Printf(TEXT("Undo after the Blueprint apply: %s"), *FString::Join(UndoTitles(), TEXT(" | "))));
	TestEqual(TEXT("Blueprint apply removes the door batch and the older batch on the same level actor list"), UndoTitles(),
		TArray<FString>{TEXT("AI: lamp colour"), TEXT("AI: dim lamp")});
	const FAssetDocumentDiagnostic* Trimmed = FindDiagnostic(DoorResult, TEXT("Undo.HistoryTrimmed"));
	if (TestNotNull(TEXT("the result says the history was trimmed"), Trimmed))
	{
		TestEqual(TEXT("as info"), Trimmed->Severity, FString(TEXT("info")));
		TestTrue(TEXT("naming the removed entries"), Trimmed->Message.Contains(TEXT("'AI: door'")) && Trimmed->Message.Contains(TEXT("'AI: lamp'")));
		TestTrue(TEXT("and the asset"), Trimmed->Message.Contains(Door.PackageName));
	}
	AActor* Door2 = FindActorByPath(DoorPath);
	if (TestNotNull(TEXT("the placed door survives (reinstanced)"), Door2))
	{
		TestEqual(TEXT("its instance value is kept"), ReadValue(Door2, TEXT("Brightness")), FString(TEXT("2")));
	}

	// The kept batches undo by title, newest first, and the door and the lamp stay in the level.
	TestTrue(TEXT("dim lamp undoes"), Undo(*this, TEXT("AI: dim lamp")).bSuccess);
	TestTrue(TEXT("lamp colour undoes"), Undo(*this, TEXT("AI: lamp colour")).bSuccess);
	TestFalse(TEXT("nothing older to undo"), GEditor->Trans->CanUndo());
	TestNotNull(TEXT("the lamp stays"), FindActorByPath(LampPath));
	TestNotNull(TEXT("the door stays"), FindActorByPath(DoorPath));
	TestTrue(TEXT("both redo"), Undo(*this, TEXT("AI: lamp colour"), true).bSuccess && Undo(*this, TEXT("AI: dim lamp"), true).bSuccess);

	// Later level batches are undoable again.
	TestTrue(TEXT("a later batch works"), Edit(*this, FString::Printf(TEXT(R"({"ops": [
		{"op": "set", "actor": "%s", "properties": {"Brightness": 3}}]})"), *DoorPath), TEXT("later batch")).bSuccess);
	TestEqual(TEXT("and is undoable"), UndoQueueLength(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelUndoHistoryAssetApplyKeepsUnrelatedLevelUndoTest,
	"UEAssetDoc.Level.UndoHistory.AssetApplyKeepsUnrelatedLevelUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelUndoHistoryAssetApplyKeepsUnrelatedLevelUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelUndoHistoryTests;
	FScopedTestAsset Shake(TEXT("CAS_SmallShake"));
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Cine"), false);
	if (!TestNotNull(TEXT("map created"), World))
	{
		return false;
	}
	const FLevelToolResult Built = Edit(*this, TEXT(R"({"description": "AI: build", "save": true, "ops": [
		{"op": "spawn", "id": "door", "class": "/Script/Engine.StaticMeshActor", "label": "Door"},
		{"op": "spawn", "id": "light", "class": "/Script/Engine.PointLight", "label": "FlickerLight"}]})"), TEXT("build"));
	if (!TestTrue(TEXT("level built"), Built.bSuccess))
	{
		return false;
	}
	const FString DoorPath = BatchPath(Built, TEXT("door"));
	const FString LightPath = BatchPath(Built, TEXT("light"));
	ResetUndo();
	TestTrue(TEXT("tag door"), Edit(*this, FString::Printf(TEXT(R"({"description": "AI: tag door", "ops": [
		{"op": "set", "actor": "%s", "tags": ["Swinging"]}]})"), *DoorPath), TEXT("tag door")).bSuccess);
	TestEqual(TEXT("the door is tagged"), FindActorByPath(DoorPath)->Tags.Num(), 1);

	// The trial's unrelated apply: a Camera Animation Sequence create (Sequencer records its own transactions).
	FScopedLevelEditingViewport Viewport;
	const FAssetDocumentResult Created = Shake.ApplyResult(*this, CameraAnimationDocument(Shake.PackageName), TEXT("create CAS"));
	if (!TestTrue(TEXT("the sequence is created"), Created.IsSuccess()))
	{
		return false;
	}
	TestNull(TEXT("no user history was removed"), FindDiagnostic(Created, TEXT("Undo.HistoryTrimmed")));
	TestNull(TEXT("nor cleared"), FindDiagnostic(Created, TEXT("Undo.HistoryCleared")));
	TestEqual(TEXT("the batch is the only Undo entry (the apply's own are gone)"), UndoTitles(), TArray<FString>{TEXT("AI: tag door")});
	TestFalse(TEXT("no Undo entry touches the sequence"), UE::AssetDocument::UndoHistory::ReferencesPackage(Shake.PackageName));

	// (a) undo_editor undoes the batch by its title, and redoes it.
	TestTrue(TEXT("the batch undoes"), Undo(*this, TEXT("AI: tag door")).bSuccess);
	TestEqual(TEXT("the tag is gone"), FindActorByPath(DoorPath)->Tags.Num(), 0);
	TestTrue(TEXT("and redoes"), Undo(*this, TEXT("AI: tag door"), true).bSuccess);
	TestEqual(TEXT("the tag is back"), FindActorByPath(DoorPath)->Tags.Num(), 1);

	// (c) the user's edit on top still stops it.
	{
		const FScopedTransaction Transaction(FText::FromString(TEXT("Move FlickerLight (human-style)")));
		AActor* Light = FindActorByPath(LightPath);
		Light->Modify();
		Light->SetActorLocation(FVector(0.0, 0.0, 300.0));
	}
	TestTrue(TEXT("the user's entry on top: Undo.EntryMismatch"), HasCode(Undo(*this, TEXT("AI: tag door")), TEXT("Undo.EntryMismatch")));
	TestEqual(TEXT("the tag stays"), FindActorByPath(DoorPath)->Tags.Num(), 1);

	// (b) undoing everything never touches the sequence.
	const UPackage* Package = FindPackage(nullptr, *Shake.PackageName);
	TestTrue(TEXT("the user's move undoes"), Undo(*this, TEXT("Move FlickerLight (human-style)")).bSuccess);
	TestTrue(TEXT("then the batch"), Undo(*this, TEXT("AI: tag door")).bSuccess);
	TestFalse(TEXT("nothing else to undo"), GEditor->Trans->CanUndo());
	TestTrue(TEXT("the sequence is untouched"), Package != nullptr && !Package->IsDirty()
		&& FindObject<UObject>(nullptr, *Shake.ObjectPath()) != nullptr);
	return true;
}

#endif
