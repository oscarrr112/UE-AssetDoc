// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// settings (POST /assetdoc/settings):
// UEAssetDoc.Settings.World.WriteAndUndo: World Settings of the open level (GameMode override, KillZ,
//   gravity) written as the World Settings panel does, one Undo entry that undo_editor undoes; reads and refusals.
// UEAssetDoc.Settings.World.SparseRead: the diff_only read of a template level (8 KB, almost all of it
//   the defaults of the HLOD setup entry) lists only the struct members that differ, also in arrays; written to another
//   level, it gives the same values.
// UEAssetDoc.Settings.Project.MapsAndModes: the default map and GameMode written to the project's
//   Config/DefaultEngine.ini as the Project Settings editor writes them, read back; the file and values restored.
// UEAssetDoc.Settings.Project.InputAndDeveloperSettings: the Input section (DefaultInput.ini) and a
//   UDeveloperSettings section (Physics, DefaultEngine.ini) written and read back; files and values restored.
// UEAssetDoc.Settings.Project.ListAndRefusals: the sections listed (Maps & Modes, Input); unknown
//   sections and properties, engine config files and malformed requests refused.
// UEAssetDoc.Settings.Project.ConfigScope: only files really in the project's Config/ are written: a Config/
//   directory, or a folder in it, that links out of the project is refused.
// UEAssetDoc.Settings.DryRun: dry_run: true checks a World Settings / Project Settings write and returns
//   the planned changes (as edit_level's dry run) without changing the values, the Undo history or the config file.
// UEAssetDoc.Settings.Project.PluginSettingsProtected: the plugin's own server and security settings (execute_python,
//   delete_assets, the auth token) are neither read nor written under any name of their section, dry runs included;
//   the settings object and its config file stay unchanged, and the section is listed as protected, not writable.
// UEAssetDoc.Settings.Project.ExecutionSettingsProtected: the Python plugin's settings (startup scripts, remote
//   execution) are read but never written, dry runs included; the values and the config file stay unchanged.

#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/InputSettings.h"
#include "AssetDocumentPathsTestSupport.h"
#include "AssetDocumentSettings.h"
#include "GameMapsSettings.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "LevelEditTestSupport.h"
#include "LevelEditorUndo.h"
#include "LevelMapTools.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Settings/EditorSettingsTool.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentSettingsTests
{
using namespace AssetDocumentLevelEditTests;
using UE::AssetDocumentLevel::Private::RunEditorSettings;

FLevelToolResult Settings(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	const FLevelToolResult Result = RunEditorSettings(ParseJsonObject(Json));
	Test.AddInfo(FString::Printf(TEXT("%s: %d %s"), Stage, Result.HttpStatus, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("  %s %s at %s: %s"), *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
	return Result;
}

TSharedPtr<FJsonValue> PropertyValue(const FLevelToolResult& Result, const TCHAR* Name)
{
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (Result.Payload.IsValid() && Result.Payload->TryGetObjectField(TEXT("properties"), Properties))
	{
		return (*Properties)->TryGetField(Name);
	}
	return nullptr;
}

FString Compact(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

TSharedPtr<FJsonObject> PayloadObject(const FLevelToolResult& Result, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	return Result.Payload.IsValid() && Result.Payload->TryGetObjectField(Field, Object) ? *Object : nullptr;
}

FString ProjectConfigFile(const TCHAR* Name)
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / Name);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSettingsWorldTest, "UEAssetDoc.Settings.World.WriteAndUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSettingsWorldTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSettingsTests;
	using UE::AssetDocumentLevel::Private::RunEditorUndo;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Arena"), false);
	if (!TestNotNull(TEXT("map created"), World) || !TestNotNull(TEXT("world settings"), World->GetWorldSettings()))
	{
		return false;
	}
	ResetUndo();
	const float KillZBefore = World->GetWorldSettings()->KillZ;

	// A fresh map: nothing differs from the defaults; named properties read as the panel shows them.
	const FLevelToolResult Fresh = Settings(*this, TEXT(R"({"section": "world"})"), TEXT("read defaults"));
	TestTrue(TEXT("read"), Fresh.bSuccess);
	const FLevelToolResult Named = Settings(*this, TEXT(R"({"section": "world", "properties": ["KillZ", "DefaultGameMode"]})"), TEXT("read named"));
	TestTrue(TEXT("KillZ read"), PropertyValue(Named, TEXT("KillZ")).IsValid());
	TestTrue(TEXT("DefaultGameMode read"), PropertyValue(Named, TEXT("DefaultGameMode")).IsValid());

	// Write: the GameMode override, KillZ and gravity in one Undo entry.
	const FLevelToolResult Written = Settings(*this, TEXT(R"({"action": "write", "section": "world", "description": "AI: arena rules",
		"properties": {"DefaultGameMode": "/Script/Engine.GameModeBase", "KillZ": -5000, "bGlobalGravitySet": true, "GlobalGravityZ": -490}})"),
		TEXT("write"));
	if (!TestTrue(TEXT("written"), Written.bSuccess))
	{
		return false;
	}
	AWorldSettings* WorldSettings = GetEditorWorld()->GetWorldSettings();
	TestEqual(TEXT("GameMode override"), WorldSettings->DefaultGameMode.Get(), AGameModeBase::StaticClass());
	TestEqual(TEXT("KillZ"), WorldSettings->KillZ, -5000.0f);
	TestTrue(TEXT("gravity override"), WorldSettings->bGlobalGravitySet);
	TestEqual(TEXT("gravity"), WorldSettings->GlobalGravityZ, -490.0f);
	TestEqual(TEXT("one Undo entry, titled by the description"), NewestUndoTitle(), FString(TEXT("AI: arena rules")));
	TestEqual(TEXT("four changes"), Changes(Written).Num(), 4);
	TestTrue(TEXT("the map is dirty, not saved"), GetEditorWorld()->GetPackage()->IsDirty());
	const FLevelToolResult Diff = Settings(*this, TEXT(R"({"section": "world"})"), TEXT("read diff"));
	TestTrue(TEXT("diff_only read shows KillZ"), PropertyValue(Diff, TEXT("KillZ")).IsValid());

	// undo_editor undoes it by title.
	const FLevelToolResult Undone = RunEditorUndo(ParseJsonObject(TEXT(R"({"expect_title": "AI: arena rules"})")));
	TestTrue(TEXT("undone"), Undone.bSuccess);
	WorldSettings = GetEditorWorld()->GetWorldSettings();
	TestNull(TEXT("no GameMode override"), WorldSettings->DefaultGameMode.Get());
	TestEqual(TEXT("KillZ restored"), WorldSettings->KillZ, KillZBefore);
	TestFalse(TEXT("no gravity override"), WorldSettings->bGlobalGravitySet);

	// Refusals: nothing changes, nothing is added to Undo.
	ResetUndo();
	const FLevelToolResult Unknown = Settings(*this, TEXT(R"({"action": "write", "section": "world", "properties": {"KillZed": 1}})"),
		TEXT("unknown"));
	TestTrue(TEXT("Generic.UnknownProperty"), HasCode(Unknown, TEXT("Generic.UnknownProperty")));
	const FLevelToolResult WrongType = Settings(*this, TEXT(R"({"action": "write", "section": "world", "properties": {"KillZ": "deep"}})"),
		TEXT("wrong type"));
	TestFalse(TEXT("a string for KillZ is refused"), WrongType.bSuccess);
	const FLevelToolResult NotAGameMode = Settings(*this, TEXT(R"({"action": "write", "section": "world",
		"properties": {"DefaultGameMode": "/Script/Engine.Actor"}})"), TEXT("not a game mode"));
	TestFalse(TEXT("a class that is not a GameMode is refused"), NotAGameMode.bSuccess);
	TestEqual(TEXT("KillZ unchanged"), GetEditorWorld()->GetWorldSettings()->KillZ, KillZBefore);
	TestEqual(TEXT("no Undo entry"), UndoQueueLength(), 0);
	const FLevelToolResult NoValues = Settings(*this, TEXT(R"({"action": "write", "section": "world", "properties": ["KillZ"]})"),
		TEXT("no values"));
	TestTrue(TEXT("write needs values"), HasCode(NoValues, TEXT("Settings.InvalidRequest")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSettingsWorldSparseTest, "UEAssetDoc.Settings.World.SparseRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSettingsWorldSparseTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSettingsTests;
	using UE::AssetDocumentLevel::Private::RunNewLevel;
	FScopedTestMaps Maps;
	// The trial's level: new_level with the basic template.
	const FLevelToolResult Basic = RunNewLevel(ParseJsonObject(FString::Printf(TEXT(R"({"path": "%s", "template": "basic"})"),
		*Maps.PackageName(TEXT("SparseBasic")))));
	if (!TestTrue(TEXT("basic level created"), Basic.bSuccess))
	{
		return false;
	}
	const FLevelToolResult Read = Settings(*this, TEXT(R"({"section": "world"})"), TEXT("diff_only read"));
	const TSharedPtr<FJsonObject> Sparse = PayloadObject(Read, TEXT("properties"));
	if (!TestTrue(TEXT("read"), Read.bSuccess && Sparse.IsValid()))
	{
		return false;
	}
	// The same properties read whole (what diff_only used to return).
	TArray<FString> Names;
	AssetDocumentServerJson::GetKeys(*Sparse, Names);
	TArray<FString> Quoted;
	for (const FString& Name : Names)
	{
		Quoted.Add(FString::Printf(TEXT("\"%s\""), *Name));
	}
	const FLevelToolResult WholeRead = Settings(*this, FString::Printf(TEXT(R"({"section": "world", "properties": [%s]})"),
		*FString::Join(Quoted, TEXT(","))), TEXT("whole read"));
	const TSharedPtr<FJsonObject> Whole = PayloadObject(WholeRead, TEXT("properties"));
	const FString SparseText = Compact(Sparse);
	const FString WholeText = Compact(Whole);
	AddInfo(FString::Printf(TEXT("World Settings diff_only read: %d chars (whole values: %d chars); %s"), SparseText.Len(), WholeText.Len(),
		*SparseText.Left(1500)));
	TestTrue(TEXT("the HLOD setup is read"), Names.Contains(TEXT("HierarchicalLODSetup")) || !WholeText.Contains(TEXT("HierarchicalLODSetup")));
	TestTrue(TEXT("struct members at their defaults are left out (a third of the whole or less)"), SparseText.Len() * 3 <= WholeText.Len());
	TestFalse(TEXT("no default member of the HLOD entry (bSimplifyMesh false)"), SparseText.Contains(TEXT("\"bSimplifyMesh\":false")));

	// Written to a new empty level (the copy), the read gives the same values back.
	TArray<TSharedPtr<FJsonValue>> ReadOnlyValues;
	const TArray<TSharedPtr<FJsonValue>>* ReadOnly = nullptr;
	TSet<FString> ReadOnlyNames;
	if (Read.Payload->TryGetArrayField(TEXT("read_only"), ReadOnly))
	{
		for (const TSharedPtr<FJsonValue>& Value : *ReadOnly)
		{
			ReadOnlyNames.Add(Value->AsString());
		}
	}
	const TSharedRef<FJsonObject> Writable = MakeShared<FJsonObject>();
	for (const auto& Entry : Sparse->Values)
	{
		if (!ReadOnlyNames.Contains(FString(*Entry.Key)))
		{
			Writable->SetField(Entry.Key, Entry.Value);
		}
	}
	const FLevelToolResult Copy = RunNewLevel(ParseJsonObject(FString::Printf(TEXT(R"({"path": "%s"})"), *Maps.PackageName(TEXT("SparseCopy")))));
	if (!TestTrue(TEXT("copy level created"), Copy.bSuccess))
	{
		return false;
	}
	const TSharedRef<FJsonObject> Write = MakeShared<FJsonObject>();
	Write->SetStringField(TEXT("action"), TEXT("write"));
	Write->SetStringField(TEXT("section"), TEXT("world"));
	Write->SetObjectField(TEXT("properties"), Writable);
	const FLevelToolResult Written = Settings(*this, Compact(Write), TEXT("write the read to the copy"));
	TestTrue(TEXT("the sparse read writes"), Written.bSuccess);
	TArray<FString> WritableNames;
	AssetDocumentServerJson::GetKeys(*Writable, WritableNames);
	TArray<FString> WritableQuoted;
	for (const FString& Name : WritableNames)
	{
		WritableQuoted.Add(FString::Printf(TEXT("\"%s\""), *Name));
	}
	const FString NamedRequest = FString::Printf(TEXT(R"({"section": "world", "properties": [%s]})"), *FString::Join(WritableQuoted, TEXT(",")));
	const FLevelToolResult CopyWhole = Settings(*this, NamedRequest, TEXT("copy whole read"));
	const TSharedPtr<FJsonObject> CopyValues = PayloadObject(CopyWhole, TEXT("properties"));
	for (const FString& Name : WritableNames)
	{
		const TSharedPtr<FJsonValue> Original = Whole.IsValid() ? Whole->TryGetField(Name) : nullptr;
		const TSharedPtr<FJsonValue> Copied = CopyValues.IsValid() ? CopyValues->TryGetField(Name) : nullptr;
		TestTrue(*FString::Printf(TEXT("%s: the copy holds the same value"), *Name),
			Original.IsValid() && Copied.IsValid() && FJsonValue::CompareEqual(*Original, *Copied));
	}
	const TSharedPtr<FJsonObject> CopySparse = PayloadObject(Settings(*this, TEXT(R"({"section": "world"})"), TEXT("copy diff_only read")), TEXT("properties"));
	TestEqual(TEXT("the copy's diff_only read lists the same writable values"), Compact(CopySparse), Compact(Writable));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSettingsProjectTest, "UEAssetDoc.Settings.Project.MapsAndModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSettingsProjectTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSettingsTests;
	FScopedTestMaps Maps;
	if (!TestNotNull(TEXT("map created"), Maps.NewMap(TEXT("Start"), false)))
	{
		return false;
	}
	const FString MapPath = Maps.PackageName(TEXT("Start")) + TEXT(".Start");
	const FString IniFile = ProjectConfigFile(TEXT("DefaultEngine.ini"));
	TArray<uint8> IniBefore;
	const bool bIniExisted = FFileHelper::LoadFileToArray(IniBefore, *IniFile);
	UGameMapsSettings* MapsSettings = GetMutableDefault<UGameMapsSettings>();
	FSoftObjectPath* DefaultMap = UGameMapsSettings::StaticClass()->FindPropertyByName(TEXT("GameDefaultMap"))
		->ContainerPtrToValuePtr<FSoftObjectPath>(MapsSettings);
	FSoftClassPath* GameMode = UGameMapsSettings::StaticClass()->FindPropertyByName(TEXT("GlobalDefaultGameMode"))
		->ContainerPtrToValuePtr<FSoftClassPath>(MapsSettings);
	const FSoftObjectPath DefaultMapBefore = *DefaultMap;
	const FSoftClassPath GameModeBefore = *GameMode;
	ON_SCOPE_EXIT
	{
		// Restore the file and the settings object.
		if (bIniExisted)
		{
			FFileHelper::SaveArrayToFile(IniBefore, *IniFile);
		}
		else
		{
			IFileManager::Get().Delete(*IniFile);
		}
		*DefaultMap = DefaultMapBefore;
		*GameMode = GameModeBefore;
	};

	const FLevelToolResult Written = Settings(*this, FString::Printf(TEXT(R"({"action": "write", "section": "Maps & Modes",
		"properties": {"GameDefaultMap": "%s", "GlobalDefaultGameMode": "/Script/Engine.GameMode"}})"), *MapPath), TEXT("write"));
	if (!TestTrue(TEXT("written"), Written.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("the section"), PayloadString(Written, TEXT("section")), FString(TEXT("Project/Maps")));
	TestEqual(TEXT("written to the project's DefaultEngine.ini"), PayloadString(Written, TEXT("config_file")),
		FString(TEXT("Config/DefaultEngine.ini")));
	TestEqual(TEXT("the settings object has the map"), DefaultMap->ToString(), MapPath);
	TestEqual(TEXT("and the GameMode"), UGameMapsSettings::GetGlobalDefaultGameMode(), FString(TEXT("/Script/Engine.GameMode")));
	TestEqual(TEXT("two changes"), Changes(Written).Num(), 2);
	const TSharedPtr<FJsonObject>* Keys = nullptr;
	FString StoredMap;
	TestTrue(TEXT("the written key is read back from the file"), Written.Payload->TryGetObjectField(TEXT("written_keys"), Keys)
		&& (*Keys)->TryGetStringField(TEXT("GameDefaultMap"), StoredMap) && StoredMap.Contains(Maps.PackageName(TEXT("Start"))));

	// The file itself, as another process would read it.
	FString IniText;
	FFileHelper::LoadFileToString(IniText, *IniFile);
	TestTrue(TEXT("the ini has the section"), IniText.Contains(TEXT("[/Script/EngineSettings.GameMapsSettings]")));
	TestTrue(TEXT("the ini has the map"), IniText.Contains(FString::Printf(TEXT("GameDefaultMap=%s"), *MapPath)));
	TestTrue(TEXT("the ini has the GameMode"), IniText.Contains(TEXT("GlobalDefaultGameMode=/Script/Engine.GameMode\n"))
		|| IniText.Contains(TEXT("GlobalDefaultGameMode=/Script/Engine.GameMode\r\n")));
	FConfigFile Parsed;
	Parsed.Read(IniFile);
	FString ParsedMode;
	TestTrue(TEXT("the ini parses with the GameMode"), Parsed.GetString(TEXT("/Script/EngineSettings.GameMapsSettings"),
		TEXT("GlobalDefaultGameMode"), ParsedMode) && ParsedMode == TEXT("/Script/Engine.GameMode"));

	// Read back through the tool, by another section name.
	const FLevelToolResult Read = Settings(*this, TEXT(R"({"section": "Project/Maps", "properties": ["GameDefaultMap", "GlobalDefaultGameMode"]})"),
		TEXT("read"));
	TestTrue(TEXT("read"), Read.bSuccess);
	TestTrue(TEXT("the map reads back"), JsonText(PropertyValue(Read, TEXT("GameDefaultMap"))).Contains(Maps.PackageName(TEXT("Start"))));
	TestTrue(TEXT("the GameMode reads back"), JsonText(PropertyValue(Read, TEXT("GlobalDefaultGameMode"))).Contains(TEXT("/Script/Engine.GameMode")));

	// Writing the same values again changes nothing.
	const FLevelToolResult Again = Settings(*this, FString::Printf(TEXT(R"({"action": "write", "section": "GameMapsSettings",
		"properties": {"GameDefaultMap": "%s"}})"), *MapPath), TEXT("again"));
	TestTrue(TEXT("unchanged write succeeds"), Again.bSuccess);
	TestEqual(TEXT("no changes"), Changes(Again).Num(), 0);

	// A bad value: refused, the file is untouched.
	FString IniBeforeBad;
	FFileHelper::LoadFileToString(IniBeforeBad, *IniFile);
	const FLevelToolResult Bad = Settings(*this, TEXT(R"({"action": "write", "section": "Maps & Modes",
		"properties": {"GlobalDefaultGameMode": 42}})"), TEXT("bad"));
	TestFalse(TEXT("bad value refused"), Bad.bSuccess);
	FString IniAfterBad;
	FFileHelper::LoadFileToString(IniAfterBad, *IniFile);
	TestEqual(TEXT("the file is untouched"), IniAfterBad, IniBeforeBad);
	TestEqual(TEXT("the GameMode is unchanged"), UGameMapsSettings::GetGlobalDefaultGameMode(), FString(TEXT("/Script/Engine.GameMode")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSettingsInputPhysicsTest, "UEAssetDoc.Settings.Project.InputAndDeveloperSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSettingsInputPhysicsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSettingsTests;
	const FString InputIni = ProjectConfigFile(TEXT("DefaultInput.ini"));
	const FString EngineIni = ProjectConfigFile(TEXT("DefaultEngine.ini"));
	TArray<uint8> InputBefore;
	TArray<uint8> EngineBefore;
	const bool bInputExisted = FFileHelper::LoadFileToArray(InputBefore, *InputIni);
	const bool bEngineExisted = FFileHelper::LoadFileToArray(EngineBefore, *EngineIni);
	UInputSettings* Input = GetMutableDefault<UInputSettings>();
	UPhysicsSettings* Physics = GetMutableDefault<UPhysicsSettings>();
	const bool bAltEnterBefore = Input->bAltEnterTogglesFullscreen != 0;
	const float GravityBefore = Physics->DefaultGravityZ;
	ON_SCOPE_EXIT
	{
		auto Restore = [](const FString& File, bool bExisted, const TArray<uint8>& Bytes)
		{
			if (bExisted)
			{
				FFileHelper::SaveArrayToFile(Bytes, *File);
			}
			else
			{
				IFileManager::Get().Delete(*File);
			}
		};
		Restore(InputIni, bInputExisted, InputBefore);
		Restore(EngineIni, bEngineExisted, EngineBefore);
		Input->bAltEnterTogglesFullscreen = bAltEnterBefore;
		Physics->DefaultGravityZ = GravityBefore;
	};

	const FLevelToolResult InputWrite = Settings(*this, FString::Printf(TEXT(R"({"action": "write", "section": "Input",
		"properties": {"bAltEnterTogglesFullscreen": %s}})"), bAltEnterBefore ? TEXT("false") : TEXT("true")), TEXT("input write"));
	TestTrue(TEXT("Input written"), InputWrite.bSuccess);
	TestEqual(TEXT("to DefaultInput.ini"), PayloadString(InputWrite, TEXT("config_file")), FString(TEXT("Config/DefaultInput.ini")));
	TestEqual(TEXT("the value changed"), Input->bAltEnterTogglesFullscreen != 0, !bAltEnterBefore);
	FString InputText;
	FFileHelper::LoadFileToString(InputText, *InputIni);
	TestTrue(TEXT("the ini has the key"), InputText.Contains(FString::Printf(TEXT("bAltEnterTogglesFullscreen=%s"),
		bAltEnterBefore ? TEXT("False") : TEXT("True"))));

	const FLevelToolResult PhysicsWrite = Settings(*this, TEXT(R"({"action": "write", "section": "Engine/Physics",
		"properties": {"DefaultGravityZ": -490}})"), TEXT("physics write"));
	TestTrue(TEXT("Physics written"), PhysicsWrite.bSuccess);
	TestEqual(TEXT("to DefaultEngine.ini"), PayloadString(PhysicsWrite, TEXT("config_file")), FString(TEXT("Config/DefaultEngine.ini")));
	TestEqual(TEXT("the gravity changed"), Physics->DefaultGravityZ, -490.0f);
	const TSharedPtr<FJsonObject>* Keys = nullptr;
	FString Stored;
	TestTrue(TEXT("the key is read back from the file"), PhysicsWrite.Payload.IsValid()
		&& PhysicsWrite.Payload->TryGetObjectField(TEXT("written_keys"), Keys) && (*Keys)->TryGetStringField(TEXT("DefaultGravityZ"), Stored)
		&& Stored.StartsWith(TEXT("-490")));
	const FLevelToolResult PhysicsRead = Settings(*this, TEXT(R"({"section": "Physics", "properties": ["DefaultGravityZ"]})"),
		TEXT("physics read"));
	const TSharedPtr<FJsonValue> Gravity = PropertyValue(PhysicsRead, TEXT("DefaultGravityZ"));
	TestTrue(TEXT("reads back"), Gravity.IsValid() && Gravity->Type == EJson::Number && Gravity->AsNumber() == -490.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSettingsRefusalsTest, "UEAssetDoc.Settings.Project.ListAndRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSettingsRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSettingsTests;
	using UE::AssetDocumentLevel::Private::IsProjectConfigFile;

	const FLevelToolResult Listed = Settings(*this, TEXT(R"({"action": "list"})"), TEXT("list"));
	TestTrue(TEXT("listed"), Listed.bSuccess);
	TArray<FString> Keys;
	TArray<FString> NotWritable;
	const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
	if (Listed.Payload.IsValid() && Listed.Payload->TryGetArrayField(TEXT("sections"), Sections))
	{
		for (const TSharedPtr<FJsonValue>& Section : *Sections)
		{
			const TSharedPtr<FJsonObject> Entry = Section->AsObject();
			Keys.Add(Entry->GetStringField(TEXT("section")));
			FString File;
			bool bWritable = true;
			if (Entry->TryGetStringField(TEXT("config_file"), File) && Entry->TryGetBoolField(TEXT("writable"), bWritable) && !bWritable)
			{
				NotWritable.Add(Entry->GetStringField(TEXT("section")));
			}
		}
	}
	// A section saved to an engine, plugin or per-user config file is never written.
	bool bRefusedOutsideProject = false;
	for (const FString& Key : NotWritable)
	{
		const FLevelToolResult Refused = Settings(*this, FString::Printf(TEXT(R"({"action": "write", "section": "%s", "properties": {"X": 1}})"),
			*Key), *Key);
		TestFalse(*FString::Printf(TEXT("%s is not written"), *Key), Refused.bSuccess);
		if (HasCode(Refused, TEXT("Settings.ConfigFileNotInProject"), TEXT("/section")))
		{
			bRefusedOutsideProject = true;
			break;
		}
	}
	TestTrue(*FString::Printf(TEXT("a section outside the project's Config/ is refused (%d candidates)"), NotWritable.Num()),
		bRefusedOutsideProject);
	TestTrue(TEXT("Maps & Modes listed"), Keys.Contains(TEXT("Project/Maps")));
	TestTrue(TEXT("Input listed"), Keys.Contains(TEXT("Engine/Input")));
	TestTrue(TEXT("developer settings listed (Physics)"), Keys.Contains(TEXT("Engine/Physics")));

	// Input reads (any UDeveloperSettings section reads the same way).
	const FLevelToolResult Input = Settings(*this, TEXT(R"({"section": "Input", "properties": ["DefaultPlayerInputClass", "DefaultInputComponentClass"]})"),
		TEXT("input"));
	TestTrue(TEXT("Input read"), Input.bSuccess);
	TestTrue(TEXT("the player input class"), PropertyValue(Input, TEXT("DefaultPlayerInputClass")).IsValid());

	const FLevelToolResult UnknownSection = Settings(*this, TEXT(R"({"section": "Maps and Modes"})"), TEXT("unknown section"));
	TestTrue(TEXT("unknown section"), HasCode(UnknownSection, TEXT("Settings.SectionNotFound"), TEXT("/section")));
	const FAssetDocumentDiagnostic* NotFound = FindCode(UnknownSection, TEXT("Settings.SectionNotFound"));
	TestTrue(TEXT("the close section is named"), NotFound != nullptr && NotFound->Message.Contains(TEXT("Project/Maps")));
	TestTrue(TEXT("unknown property"), HasCode(Settings(*this, TEXT(R"({"section": "Maps & Modes", "properties": ["GameDefaultMapp"]})"),
		TEXT("unknown property")), TEXT("Generic.UnknownProperty")));
	TestTrue(TEXT("unknown action"), HasCode(Settings(*this, TEXT(R"({"action": "delete", "section": "world"})"), TEXT("bad action")),
		TEXT("Settings.InvalidRequest"), TEXT("/action")));
	TestTrue(TEXT("unknown field"), HasCode(Settings(*this, TEXT(R"({"section": "world", "value": 1})"), TEXT("bad field")),
		TEXT("Settings.InvalidRequest"), TEXT("/value")));
	TestTrue(TEXT("description is World Settings only"), HasCode(Settings(*this, TEXT(R"({"action": "write", "section": "Maps & Modes",
		"description": "AI: x", "properties": {"bUseSplitscreen": true}})"), TEXT("project description")), TEXT("Settings.InvalidRequest")));

	// Only .ini files in the project's Config/ directory are written.
	FString Relative;
	TestTrue(TEXT("project DefaultEngine.ini"), IsProjectConfigFile(ProjectConfigFile(TEXT("DefaultEngine.ini")), &Relative));
	TestEqual(TEXT("relative path"), Relative, FString(TEXT("Config/DefaultEngine.ini")));
	TestFalse(TEXT("engine BaseEngine.ini"), IsProjectConfigFile(FPaths::ConvertRelativePathToFull(FPaths::EngineConfigDir() / TEXT("BaseEngine.ini"))));
	TestFalse(TEXT("escaping Config/"), IsProjectConfigFile(ProjectConfigFile(TEXT("../Saved/Config/Engine.ini"))));
	TestFalse(TEXT("not an ini"), IsProjectConfigFile(ProjectConfigFile(TEXT("DefaultEngine.txt"))));
	TestFalse(TEXT("per-user config"), IsProjectConfigFile(FPaths::ConvertRelativePathToFull(FPaths::GeneratedConfigDir() / TEXT("Editor.ini"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSettingsConfigScopeTest, "UEAssetDoc.Settings.Project.ConfigScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSettingsConfigScopeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPathsTestSupport;
	using UE::AssetDocumentLevel::Private::IsConfigFileInProject;
	using UE::AssetDocumentLevel::Private::IsProjectConfigFile;

	const FString Scratch = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentSettingsScope"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString Outside = MakeOutsideDirectory(TEXT("Config"));
	const FString LinkedProject = Scratch / TEXT("LinkedProject");
	const FString LinkInProjectConfig = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir())
		/ (TEXT("AssetDocumentScope_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT
	{
		RemoveDirectoryLink(LinkedProject / TEXT("Config"));
		RemoveDirectoryLink(LinkInProjectConfig);
		IFileManager::Get().DeleteDirectory(*Scratch, false, true);
		IFileManager::Get().DeleteDirectory(*Outside, false, true);
	};
	TestTrue(TEXT("an outside config file"), FFileHelper::SaveStringToFile(TEXT("[Section]\n"), *(Outside / TEXT("DefaultEngine.ini"))));

	// An ordinary project: its Config/ files are written.
	const FString Project = Scratch / TEXT("Project");
	TestTrue(TEXT("a project config file"), FFileHelper::SaveStringToFile(TEXT("[Section]\n"), *(Project / TEXT("Config/DefaultEngine.ini"))));
	FString Relative;
	TestTrue(TEXT("a file in an ordinary Config/"), IsConfigFileInProject(Project, Project / TEXT("Config"),
		Project / TEXT("Config/DefaultEngine.ini"), &Relative));
	TestEqual(TEXT("its project-relative path"), Relative, FString(TEXT("Config/DefaultEngine.ini")));

	// A project whose Config/ links out of it: its files are not the project's.
	if (TestTrue(TEXT("a linked Config/"), MakeDirectoryLink(LinkedProject / TEXT("Config"), Outside)))
	{
		TestFalse(TEXT("a Config/ that links out of the project is refused"), IsConfigFileInProject(LinkedProject,
			LinkedProject / TEXT("Config"), LinkedProject / TEXT("Config/DefaultEngine.ini")));
	}
	// A folder in this project's Config/ that links out of it.
	if (TestTrue(TEXT("a linked folder in Config/"), MakeDirectoryLink(LinkInProjectConfig, Outside)))
	{
		TestFalse(TEXT("a config file through a link out of the project is refused"),
			IsProjectConfigFile(LinkInProjectConfig / TEXT("DefaultEngine.ini")));
	}
	TestTrue(TEXT("this project's DefaultEngine.ini is still written"),
		IsProjectConfigFile(AssetDocumentSettingsTests::ProjectConfigFile(TEXT("DefaultEngine.ini"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSettingsDryRunTest, "UEAssetDoc.Settings.DryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSettingsDryRunTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSettingsTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Arena"), false);
	if (!TestNotNull(TEXT("map created"), World) || !TestNotNull(TEXT("world settings"), World->GetWorldSettings()))
	{
		return false;
	}
	ResetUndo();
	const float KillZBefore = World->GetWorldSettings()->KillZ;
	const auto FindChange = [](const FLevelToolResult& Result, const TCHAR* Property) -> TSharedPtr<FJsonObject>
	{
		for (const TSharedPtr<FJsonValue>& Value : Changes(Result))
		{
			const TSharedPtr<FJsonObject> Change = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Change.IsValid() && Change->GetStringField(TEXT("property")) == Property)
			{
				return Change;
			}
		}
		return nullptr;
	};

	// World Settings: the planned changes; nothing changes and nothing goes to Undo.
	const FLevelToolResult Dry = Settings(*this, TEXT(R"({"action": "write", "section": "world", "dry_run": true, "description": "AI: arena",
		"properties": {"KillZ": -5000, "DefaultGameMode": "/Script/Engine.GameModeBase", "bEnableWorldBoundsChecks": true}})"), TEXT("world dry run"));
	TestTrue(TEXT("world dry run succeeds"), Dry.bSuccess);
	TestTrue(TEXT("dry_run flag"), PayloadBool(Dry, TEXT("dry_run")));
	TestTrue(*FString::Printf(TEXT("the message says nothing changed (%s)"), *Dry.Message), Dry.Message.Contains(TEXT("nothing was changed")));
	const TSharedPtr<FJsonObject> KillZ = FindChange(Dry, TEXT("KillZ"));
	TestTrue(TEXT("KillZ planned with its after value"), KillZ.IsValid() && JsonText(KillZ->TryGetField(TEXT("after"))) == TEXT("-5000"));
	TestTrue(TEXT("DefaultGameMode planned"), FindChange(Dry, TEXT("DefaultGameMode")).IsValid());
	TestTrue(TEXT("world bounds checks are on by default"), World->GetWorldSettings()->bEnableWorldBoundsChecks);
	TestFalse(TEXT("a value already set is not a change"), FindChange(Dry, TEXT("bEnableWorldBoundsChecks")).IsValid());
	TestEqual(TEXT("KillZ unchanged"), GetEditorWorld()->GetWorldSettings()->KillZ, KillZBefore);
	TestNull(TEXT("no GameMode override"), GetEditorWorld()->GetWorldSettings()->DefaultGameMode.Get());
	TestEqual(TEXT("no Undo entry"), UndoQueueLength(), 0);
	const FLevelToolResult DryBad = Settings(*this, TEXT(R"({"action": "write", "section": "world", "dry_run": true,
		"properties": {"KillZ": "deep"}})"), TEXT("world dry run refusal"));
	TestFalse(TEXT("a dry run refuses a bad value"), DryBad.bSuccess);
	const FLevelToolResult NotBool = Settings(*this, TEXT(R"({"action": "write", "section": "world", "dry_run": "yes",
		"properties": {"KillZ": 1}})"), TEXT("dry_run not a boolean"));
	TestTrue(TEXT("dry_run must be a boolean"), HasCode(NotBool, TEXT("Settings.InvalidRequest")));

	// Project Settings: the config file is not touched.
	const FString IniFile = ProjectConfigFile(TEXT("DefaultEngine.ini"));
	FString IniBefore;
	FFileHelper::LoadFileToString(IniBefore, *IniFile);
	const FString ModeBefore = UGameMapsSettings::GetGlobalDefaultGameMode();
	const FLevelToolResult DryProject = Settings(*this, TEXT(R"({"action": "write", "section": "Maps & Modes", "dry_run": true,
		"properties": {"GlobalDefaultGameMode": "/Script/Engine.GameModeBase"}})"), TEXT("project dry run"));
	TestTrue(TEXT("project dry run succeeds"), DryProject.bSuccess);
	TestTrue(TEXT("project dry_run flag"), PayloadBool(DryProject, TEXT("dry_run")));
	TestEqual(TEXT("names the config file"), PayloadString(DryProject, TEXT("config_file")), FString(TEXT("Config/DefaultEngine.ini")));
	TestEqual(TEXT("one planned change"), Changes(DryProject).Num(), ModeBefore == TEXT("/Script/Engine.GameModeBase") ? 0 : 1);
	FString IniAfter;
	FFileHelper::LoadFileToString(IniAfter, *IniFile);
	TestEqual(TEXT("the file is untouched"), IniAfter, IniBefore);
	TestEqual(TEXT("the GameMode is unchanged"), UGameMapsSettings::GetGlobalDefaultGameMode(), ModeBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSettingsPluginProtectedTest, "UEAssetDoc.Settings.Project.PluginSettingsProtected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSettingsPluginProtectedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSettingsTests;
	UAssetDocumentSettings* PluginSettings = UAssetDocumentSettings::Get();
	if (!TestNotNull(TEXT("the plugin settings"), PluginSettings))
	{
		return false;
	}
	// The user switched execute_python and delete_assets off and the auth token on (in memory only; restored below).
	const bool bExecuteBefore = PluginSettings->bEnableExecutePython;
	const bool bDeleteBefore = PluginSettings->bEnableDeleteAssets;
	const bool bTokenBefore = PluginSettings->bRequireAuthToken;
	const int32 PortBefore = PluginSettings->HttpServerPort;
	const bool bRemoteBefore = PluginSettings->bAllowRemoteConnections;
	const bool bLogBefore = PluginSettings->bLogHttpRequests;
	PluginSettings->bEnableExecutePython = false;
	PluginSettings->bEnableDeleteAssets = false;
	PluginSettings->bRequireAuthToken = true;
	const FString IniFile = FPaths::ConvertRelativePathToFull(PluginSettings->GetDefaultConfigFilename());
	const bool bIniExisted = FPaths::FileExists(IniFile);
	FString IniBefore;
	FFileHelper::LoadFileToString(IniBefore, *IniFile);

	const TCHAR* const Sections[] = {TEXT("/Script/AssetDocumentServer.AssetDocumentSettings"), TEXT("Plugins/UE-AssetDoc"), TEXT("UE-AssetDoc"),
		TEXT("AssetDocumentSettings")};
	const TCHAR* const Writes[] = {TEXT(R"({"bEnableExecutePython": true})"), TEXT(R"({"bEnableDeleteAssets": true})"),
		TEXT(R"({"bRequireAuthToken": false})"), TEXT(R"({"HttpServerPort": 8600, "bAllowRemoteConnections": true, "bLogHttpRequests": true})")};
	for (const TCHAR* Section : Sections)
	{
		for (const TCHAR* Write : Writes)
		{
			for (const TCHAR* DryRun : {TEXT("false"), TEXT("true")})
			{
				const FString Stage = FString::Printf(TEXT("write %s %s (dry_run %s)"), Section, Write, DryRun);
				const FLevelToolResult Result = Settings(*this, FString::Printf(TEXT(R"({"action": "write", "section": "%s", "dry_run": %s, "properties": %s})"),
					Section, DryRun, Write), *Stage);
				TestFalse(*FString::Printf(TEXT("%s is refused"), *Stage), Result.bSuccess);
				TestTrue(*FString::Printf(TEXT("%s: Settings.Protected"), *Stage), HasCode(Result, TEXT("Settings.Protected"), TEXT("/section")));
				const FAssetDocumentDiagnostic* Protected = FindCode(Result, TEXT("Settings.Protected"));
				TestTrue(TEXT("the refusal says where the user changes it"), Protected != nullptr
					&& Protected->Message.Contains(TEXT("Project Settings > Plugins > UE-AssetDoc")));
			}
		}
		const FLevelToolResult Read = Settings(*this, FString::Printf(TEXT(R"({"section": "%s"})"), Section), *FString::Printf(TEXT("read %s"), Section));
		TestTrue(*FString::Printf(TEXT("read %s: Settings.Protected"), Section), HasCode(Read, TEXT("Settings.Protected"), TEXT("/section")));
	}
	TestFalse(TEXT("execute_python stays off"), PluginSettings->bEnableExecutePython);
	TestFalse(TEXT("delete_assets stays off"), PluginSettings->bEnableDeleteAssets);
	TestTrue(TEXT("the auth token stays required"), PluginSettings->bRequireAuthToken);
	TestEqual(TEXT("the port is unchanged"), PluginSettings->HttpServerPort, PortBefore);
	TestEqual(TEXT("remote connections are unchanged"), PluginSettings->bAllowRemoteConnections, bRemoteBefore);
	TestEqual(TEXT("request logging is unchanged"), PluginSettings->bLogHttpRequests, bLogBefore);
	FString IniAfter;
	FFileHelper::LoadFileToString(IniAfter, *IniFile);
	TestEqual(TEXT("the config file still exists or is still absent"), FPaths::FileExists(IniFile), bIniExisted);
	TestEqual(TEXT("the config file is unchanged"), IniAfter, IniBefore);

	// Listed, but neither writable nor readable.
	bool bListed = false;
	const FLevelToolResult Listed = Settings(*this, TEXT(R"({"action": "list"})"), TEXT("list"));
	const TArray<TSharedPtr<FJsonValue>>* Listing = nullptr;
	if (Listed.Payload.IsValid() && Listed.Payload->TryGetArrayField(TEXT("sections"), Listing))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Listing)
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			FString Class;
			if (Entry.IsValid() && Entry->TryGetStringField(TEXT("class"), Class) && Class == TEXT("/Script/AssetDocumentServer.AssetDocumentSettings"))
			{
				bListed = true;
				bool bWritable = true;
				bool bProtected = false;
				TestTrue(TEXT("listed as not writable"), Entry->TryGetBoolField(TEXT("writable"), bWritable) && !bWritable);
				TestTrue(TEXT("listed as protected"), Entry->TryGetBoolField(TEXT("protected"), bProtected) && bProtected);
			}
		}
	}
	TestTrue(TEXT("the plugin's section is listed"), bListed);

	// An engine section is still written as before (dry run: the file is not touched).
	const FLevelToolResult Engine = Settings(*this, TEXT(R"({"action": "write", "section": "Maps & Modes", "dry_run": true,
		"properties": {"bUseSplitscreen": false}})"), TEXT("engine section"));
	TestTrue(TEXT("an engine section is still writable"), Engine.bSuccess);

	PluginSettings->bEnableExecutePython = bExecuteBefore;
	PluginSettings->bEnableDeleteAssets = bDeleteBefore;
	PluginSettings->bRequireAuthToken = bTokenBefore;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSettingsExecutionProtectedTest, "UEAssetDoc.Settings.Project.ExecutionSettingsProtected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSettingsExecutionProtectedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSettingsTests;
	const TCHAR* const ClassPath = TEXT("/Script/PythonScriptPlugin.PythonScriptPluginSettings");
	UClass* PythonClass = FindObject<UClass>(nullptr, ClassPath);
	if (!TestNotNull(TEXT("the Python plugin's settings class is loaded"), PythonClass))
	{
		return false;
	}
	UObject* PythonSettings = PythonClass->GetDefaultObject();
	const auto ValueText = [PythonSettings](const TCHAR* Name)
	{
		FString Text;
		if (const FProperty* Property = PythonSettings->GetClass()->FindPropertyByName(Name))
		{
			Property->ExportText_InContainer(0, Text, PythonSettings, PythonSettings, PythonSettings, PPF_None);
		}
		return Text;
	};
	const FString ScriptsBefore = ValueText(TEXT("StartupScripts"));
	const FString RemoteBefore = ValueText(TEXT("bRemoteExecution"));
	const FString PathsBefore = ValueText(TEXT("AdditionalPaths"));
	const FString IniFile = FPaths::ConvertRelativePathToFull(PythonSettings->GetDefaultConfigFilename());
	const bool bIniExisted = FPaths::FileExists(IniFile);
	FString IniBefore;
	FFileHelper::LoadFileToString(IniBefore, *IniFile);

	const TCHAR* const Writes[] = {TEXT(R"json({"StartupScripts": ["print('startup')"]})json"), TEXT(R"json({"bRemoteExecution": true})json"),
		TEXT(R"json({"AdditionalPaths": [{"Path": "Scripts"}]})json")};
	for (const TCHAR* Write : Writes)
	{
		for (const TCHAR* DryRun : {TEXT("false"), TEXT("true")})
		{
			const FString Stage = FString::Printf(TEXT("write %s (dry_run %s)"), Write, DryRun);
			const FLevelToolResult Result = Settings(*this, FString::Printf(TEXT(R"({"action": "write", "section": "%s", "dry_run": %s, "properties": %s})"),
				ClassPath, DryRun, Write), *Stage);
			TestFalse(*FString::Printf(TEXT("%s is refused"), *Stage), Result.bSuccess);
			TestTrue(*FString::Printf(TEXT("%s: Settings.Protected"), *Stage), HasCode(Result, TEXT("Settings.Protected"), TEXT("/section")));
			const FAssetDocumentDiagnostic* Protected = FindCode(Result, TEXT("Settings.Protected"));
			TestTrue(TEXT("the refusal says why"), Protected != nullptr && Protected->Message.Contains(TEXT("Python")));
		}
	}
	TestEqual(TEXT("StartupScripts is unchanged"), ValueText(TEXT("StartupScripts")), ScriptsBefore);
	TestEqual(TEXT("bRemoteExecution is unchanged"), ValueText(TEXT("bRemoteExecution")), RemoteBefore);
	TestEqual(TEXT("AdditionalPaths is unchanged"), ValueText(TEXT("AdditionalPaths")), PathsBefore);
	FString IniAfter;
	FFileHelper::LoadFileToString(IniAfter, *IniFile);
	TestEqual(TEXT("the config file still exists or is still absent"), FPaths::FileExists(IniFile), bIniExisted);
	TestEqual(TEXT("the config file is unchanged"), IniAfter, IniBefore);

	// Reading stays allowed, and the listing marks the section protected.
	const FLevelToolResult Read = Settings(*this, FString::Printf(TEXT(R"({"section": "%s", "properties": ["bRemoteExecution"]})"), ClassPath),
		TEXT("read"));
	TestTrue(TEXT("the section is readable"), Read.bSuccess && PropertyValue(Read, TEXT("bRemoteExecution")).IsValid());
	bool bListed = false;
	const FLevelToolResult Listed = Settings(*this, TEXT(R"({"action": "list"})"), TEXT("list"));
	const TArray<TSharedPtr<FJsonValue>>* Listing = nullptr;
	if (Listed.Payload.IsValid() && Listed.Payload->TryGetArrayField(TEXT("sections"), Listing))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Listing)
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			FString Class;
			if (Entry.IsValid() && Entry->TryGetStringField(TEXT("class"), Class) && Class == ClassPath)
			{
				bListed = true;
				bool bWritable = true;
				bool bProtected = false;
				TestTrue(TEXT("listed as not writable"), Entry->TryGetBoolField(TEXT("writable"), bWritable) && !bWritable);
				TestTrue(TEXT("listed as protected"), Entry->TryGetBoolField(TEXT("protected"), bProtected) && bProtected);
			}
		}
	}
	TestTrue(TEXT("the Python section is listed"), bListed);
	return true;
}

#endif
