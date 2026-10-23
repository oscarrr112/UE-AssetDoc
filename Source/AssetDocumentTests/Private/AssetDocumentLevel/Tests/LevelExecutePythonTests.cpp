// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// execute_python's editor transaction (design doc §3.3, phase L0): a failing script is
// rolled back, the user's previous Undo entry is never touched, no transaction stays
// open, and a successful script is exactly one Undo entry.

#include "AssetDocumentServerPythonExecution.h"
#include "EditorAssetOpsTestSupport.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/PointLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "LevelTestSupport.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "ScopedTransaction.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentLevelExecutePythonTests
{
using namespace AssetDocumentLevelTests;

int32 CountActors(UWorld& World)
{
	int32 Count = 0;
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		Count += IsValid(*It) ? 1 : 0;
	}
	return Count;
}

FString UndoTitle()
{
	return GEditor->Trans->GetUndoContext(false).Title.ToString();
}

// A fresh untitled map with one user actor whose label the user set in an Undo entry
// titled "User label". Returns the actor (rooted by the caller's weak pointer only).
AActor* PrepareUserEdit(FAutomationTestBase& Test, const TCHAR* Label)
{
	TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
	GEditor->NewMap(false);
	UWorld* World = GetEditorWorld();
	if (!Test.TestNotNull(TEXT("editor world"), World))
	{
		return nullptr;
	}
	AActor* Actor = SpawnActor(*World->PersistentLevel, APointLight::StaticClass(), FVector::ZeroVector, TEXT("UserActor"));
	if (!Test.TestNotNull(TEXT("user actor spawned"), Actor))
	{
		return nullptr;
	}
	{
		const FScopedTransaction Transaction(FText::FromString(TEXT("User label")));
		Actor->SetActorLabel(Label);
	}
	Test.TestEqual(TEXT("the user's label edit is the newest undo entry"), UndoTitle(), FString(TEXT("User label")));
	return Actor;
}

// The deliberate failure's traceback is logged as errors.
void ExpectTraceback(FAutomationTestBase& Test, const TCHAR* Message)
{
	Test.AddExpectedErrorPlain(TEXT("Traceback (most recent call last)"));
	Test.AddExpectedErrorPlain(TEXT("File \"<string>\""));
	Test.AddExpectedErrorPlain(Message);
}

const TCHAR* const ChangeThenFailScript =
	TEXT("import unreal\n")
	TEXT("sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n")
	TEXT("sub.spawn_actor_from_class(unreal.PointLight, unreal.Vector(0, 0, 300))\n")
	TEXT("for actor in sub.get_all_level_actors():\n")
	TEXT("    if actor.get_actor_label() == 'UserActor-Before':\n")
	TEXT("        actor.set_actor_label('ChangedByScript')\n")
	TEXT("raise RuntimeError('deliberate failure after changes')\n");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelExecutePythonFailureRollsBackTest,
	"UEAssetDoc.Level.ExecutePython.FailureRollsBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelExecutePythonFailureRollsBackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelExecutePythonTests;
	FScopedTestMaps Maps;
	TWeakObjectPtr<AActor> UserActor = PrepareUserEdit(*this, TEXT("UserActor-Before"));
	UWorld* World = GetEditorWorld();
	if (!UserActor.IsValid() || World == nullptr)
	{
		return false;
	}
	const int32 ActorsBefore = CountActors(*World);
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();
	const FString UserPath = UserActor->GetPathName();

	ExpectTraceback(*this, TEXT("deliberate failure after changes"));
	const FAssetDocumentServerPythonExecutionResult Result = ExecuteAssetDocumentServerPython(ChangeThenFailScript, TEXT("AI script"));

	TestFalse(TEXT("the script failed"), Result.bSucceeded);
	TestEqual(TEXT("the script's changes were undone"), FString(LexToString(Result.Rollback)), FString(TEXT("Undone")));
	TestTrue(TEXT("nothing it did is outside the Undo history"), Result.NotUndoable.IsEmpty());
	TestTrue(TEXT("the result lists what the Undo restored (the user actor whose label changed)"),
		Result.RolledBackObjects.ContainsByPredicate([&UserPath](const FString& Object) { return Object.StartsWith(UserPath + TEXT(" ")); }));
	TestEqual(TEXT("the listed count is the number of objects"), Result.RolledBackObjectCount >= Result.RolledBackObjects.Num(), true);
	TestFalse(TEXT("no editor transaction stays open"), GEditor->IsTransactionActive());
	TestEqual(TEXT("the spawned actor is gone"), CountActors(*World), ActorsBefore);
	TestTrue(TEXT("the user actor survives"), UserActor.IsValid());
	if (UserActor.IsValid())
	{
		TestEqual(TEXT("the label the script changed is restored"), UserActor->GetActorLabel(), FString(TEXT("UserActor-Before")));
	}
	TestEqual(TEXT("the undo history has no entry for the failed script"), GEditor->Trans->GetQueueLength(), QueueBefore);
	TestEqual(TEXT("the user's edit is still the newest undo entry"), UndoTitle(), FString(TEXT("User label")));

	// Undo is usable and undoes the user's own edit.
	TestTrue(TEXT("Undo works after the failed script"), GEditor->UndoTransaction());
	if (UserActor.IsValid())
	{
		TestEqual(TEXT("Undo reverted the user's label edit"), UserActor->GetActorLabel(), FString(TEXT("UserActor")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelExecutePythonSuccessIsOneUndoEntryTest,
	"UEAssetDoc.Level.ExecutePython.SuccessIsOneUndoEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelExecutePythonSuccessIsOneUndoEntryTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelExecutePythonTests;
	FScopedTestMaps Maps;
	TWeakObjectPtr<AActor> UserActor = PrepareUserEdit(*this, TEXT("UserActor-Before"));
	UWorld* World = GetEditorWorld();
	if (!UserActor.IsValid() || World == nullptr)
	{
		return false;
	}
	const int32 ActorsBefore = CountActors(*World);
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();

	const FAssetDocumentServerPythonExecutionResult Result = ExecuteAssetDocumentServerPython(
		TEXT("import unreal\n")
		TEXT("sub = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n")
		TEXT("a = sub.spawn_actor_from_class(unreal.PointLight, unreal.Vector(0, 0, 300))\n")
		TEXT("a.set_actor_label('ScriptLightA')\n")
		TEXT("b = sub.spawn_actor_from_class(unreal.PointLight, unreal.Vector(0, 300, 300))\n")
		TEXT("b.set_actor_label('ScriptLightB')\n"),
		TEXT("AI script: two lights"));

	TestTrue(TEXT("the script succeeded"), Result.bSucceeded);
	TestTrue(TEXT("the script left an undo entry"), Result.bUndoEntryAdded);
	TestFalse(TEXT("no editor transaction stays open"), GEditor->IsTransactionActive());
	TestEqual(TEXT("two actors were spawned"), CountActors(*World), ActorsBefore + 2);
	TestEqual(TEXT("exactly one undo entry was added"), GEditor->Trans->GetQueueLength(), QueueBefore + 1);
	TestEqual(TEXT("the entry is titled with the description"), UndoTitle(), FString(TEXT("AI script: two lights")));
	TestTrue(TEXT("the result names the entry's id (undo_editor's expect_id)"),
		Result.UndoEntryId.IsValid() && Result.UndoEntryId == GEditor->Trans->GetUndoContext(false).TransactionId);

	TestTrue(TEXT("one Undo"), GEditor->UndoTransaction());
	TestEqual(TEXT("one Undo removes everything the script did"), CountActors(*World), ActorsBefore);
	TestEqual(TEXT("the user's edit is next"), UndoTitle(), FString(TEXT("User label")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelExecutePythonEmptyFailureKeepsUserEditTest,
	"UEAssetDoc.Level.ExecutePython.EmptyFailureKeepsUserEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelExecutePythonEmptyFailureKeepsUserEditTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelExecutePythonTests;
	FScopedTestMaps Maps;
	TWeakObjectPtr<AActor> UserActor = PrepareUserEdit(*this, TEXT("UserActor-Kept"));
	UWorld* World = GetEditorWorld();
	if (!UserActor.IsValid() || World == nullptr)
	{
		return false;
	}
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();

	ExpectTraceback(*this, TEXT("deliberate failure without changes"));
	// A script that fails before recording anything: the engine discards its empty
	// transaction, so an unconditional Undo would undo the user's label edit.
	const FAssetDocumentServerPythonExecutionResult Result = ExecuteAssetDocumentServerPython(
		TEXT("raise RuntimeError('deliberate failure without changes')\n"), TEXT("AI script"));

	TestFalse(TEXT("the script failed"), Result.bSucceeded);
	TestEqual(TEXT("nothing was recorded"), FString(LexToString(Result.Rollback)), FString(TEXT("NothingRecorded")));
	TestFalse(TEXT("no editor transaction stays open"), GEditor->IsTransactionActive());
	if (UserActor.IsValid())
	{
		TestEqual(TEXT("the user's label edit is not undone"), UserActor->GetActorLabel(), FString(TEXT("UserActor-Kept")));
	}
	TestEqual(TEXT("the undo history is unchanged"), GEditor->Trans->GetQueueLength(), QueueBefore);
	TestEqual(TEXT("the user's edit is still the newest undo entry"), UndoTitle(), FString(TEXT("User label")));
	return true;
}
// 0q: code that merely mentions a .py file name, even in a comment, ran as
// "load the file <first line ...>.py". Only an input that is one path is a file.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelExecutePythonCodeMentioningPyFileTest,
	"UEAssetDoc.Level.ExecutePython.CodeMentioningPyFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelExecutePythonCodeMentioningPyFileTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelExecutePythonTests;
	const FAssetDocumentServerPythonExecutionResult Commented = ExecuteAssetDocumentServerPython(
		TEXT("import unreal  # same as Tools/copy_ik_hand_gun.py\n")
		TEXT("unreal.log('AssetDocumentPyFileMention ' + str(6 * 7))\n"),
		TEXT("AI script"));
	TestTrue(TEXT("code with a .py name in a comment runs as code"), Commented.bSucceeded);
	TestTrue(TEXT("its output is captured"), Commented.Logs.ContainsByPredicate([](const FAssetDocumentServerPythonLogEntry& Entry)
	{
		return Entry.Message.Contains(TEXT("AssetDocumentPyFileMention 42"));
	}));
	const FAssetDocumentServerPythonExecutionResult Literal = ExecuteAssetDocumentServerPython(
		TEXT("name = 'export_sfx_wav.py' \nresult = name.upper()\n"), TEXT("AI script"));
	TestTrue(TEXT("code with a .py string literal runs as code"), Literal.bSucceeded);

	// A whole input that is one path still runs that file.
	const FString Script = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AssetDocumentServerTests")
		/ FString::Printf(TEXT("run_me_%s.py"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
	FFileHelper::SaveStringToFile(TEXT("import unreal\nunreal.log('AssetDocumentPyFileRan')\n"), *Script);
	const FAssetDocumentServerPythonExecutionResult File = ExecuteAssetDocumentServerPython(Script, TEXT("AI script"));
	IFileManager::Get().Delete(*Script, false, true);
	TestTrue(TEXT("a path runs the file"), File.bSucceeded);
	TestTrue(TEXT("the file ran"), File.Logs.ContainsByPredicate([](const FAssetDocumentServerPythonLogEntry& Entry)
	{
		return Entry.Message.Contains(TEXT("AssetDocumentPyFileRan"));
	}));
	return true;
}

// 0r: a script that renamed an asset and then raised was undone, which left
// the asset under both names. Work the Undo history cannot take back is kept and reported.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelExecutePythonNonUndoableKeptTest,
	"UEAssetDoc.Level.ExecutePython.NonUndoableKept",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelExecutePythonNonUndoableKeptTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelExecutePythonTests;
	using namespace AssetDocumentAssetOpsTestSupport;
	FScopedTestAssets Assets;
	UAssetDocumentAssetOpsTestRefAsset* Source = Assets.NewRefAsset(TEXT("AS_Source"));
	Source->Value = 7;
	if (!TestTrue(TEXT("source saved"), FScopedTestAssets::Save({Source})))
	{
		return false;
	}
	const int32 QueueBefore = GEditor->Trans->GetQueueLength();
	ExpectTraceback(*this, TEXT("deliberate failure after a rename"));
	const FAssetDocumentServerPythonExecutionResult Result = ExecuteAssetDocumentServerPython(FString::Printf(TEXT(
		"import unreal\n"
		"lib = unreal.EditorAssetLibrary\n"
		"copy = lib.duplicate_asset('%s', '%s')\n"
		"copy.set_editor_property('value', 9)\n"
		"lib.rename_asset('%s', '%s')\n"
		"raise RuntimeError('deliberate failure after a rename')\n"),
		*Assets.PackageName(TEXT("AS_Source")), *Assets.PackageName(TEXT("RT2_Copy")),
		*Assets.PackageName(TEXT("RT2_Copy")), *Assets.PackageName(TEXT("MN_Copy"))), TEXT("AI script: copy and rename"));

	TestFalse(TEXT("the script failed"), Result.bSucceeded);
	TestFalse(TEXT("no editor transaction stays open"), GEditor->IsTransactionActive());
	TestEqual(TEXT("nothing was undone"), FString(LexToString(Result.Rollback)), FString(TEXT("Kept")));
	TestTrue(TEXT("the result names the rename"), Result.NotUndoable.ContainsByPredicate([&Assets](const FString& Operation)
	{
		return Operation.StartsWith(TEXT("renamed ")) && Operation.Contains(Assets.ObjectPath(TEXT("MN_Copy")));
	}));
	TestTrue(TEXT("and the duplicate"), Result.NotUndoable.ContainsByPredicate([&Assets](const FString& Operation)
	{
		return Operation.StartsWith(TEXT("created ")) && Operation.Contains(TEXT("RT2_Copy"));
	}));
	// One consistent state: the asset the script renamed is under its new name only.
	UObject* Renamed = FSoftObjectPath(Assets.ObjectPath(TEXT("MN_Copy"))).ResolveObject();
	UAssetDocumentAssetOpsTestRefAsset* Copy = Cast<UAssetDocumentAssetOpsTestRefAsset>(Renamed);
	if (TestNotNull(TEXT("the renamed asset is loaded under its new name"), Copy))
	{
		TestEqual(TEXT("with the value the script set"), Copy->Value, 9);
	}
	TestTrue(TEXT("the asset registry lists it under its new name"),
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Assets.ObjectPath(TEXT("MN_Copy")))).IsValid());
	UObject* Old = FSoftObjectPath(Assets.ObjectPath(TEXT("RT2_Copy"))).ResolveObject();
	TestTrue(TEXT("the old name holds no asset (at most a redirector)"), Old == nullptr || Old->IsA<UObjectRedirector>());
	TestTrue(TEXT("the source is untouched"), Source->Value == 7);
	TestTrue(TEXT("the undo history did not lose the user's entries"), GEditor->Trans->GetQueueLength() >= QueueBefore);
	return true;
}
#endif
