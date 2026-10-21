// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Settings/EditorSettingsTool.h"

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentEditorObjectProperties.h"
#include "AssetDocumentServerEditorTransaction.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "ISettingsCategory.h"
#include "ISettingsContainer.h"
#include "ISettingsModule.h"
#include "Algo/LevenshteinDistance.h"
#include "ISettingsEditorModule.h"
#include "ISettingsSection.h"
#include "ISourceControlModule.h"
#include "LevelEditTypes.h"
#include "LevelEditValues.h"
#include "LevelEditorUndo.h"
#include "LevelQuery.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SSettingsEditorCheckoutNotice.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"

namespace UE::AssetDocumentLevel::Private
{
namespace
{
namespace ObjectProperties = UE::AssetDocument::EditorObjectProperties;

const TCHAR* const ProjectContainer = TEXT("Project");

FAssetDocumentDiagnostic SettingsInvalidRequest(const FString& Path, const FString& Message)
{
	return MakeLevelDiagnostic(Path, TEXT("Settings.InvalidRequest"), Message);
}

TArray<TSharedPtr<FJsonValue>> SettingsStringsJson(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& String : Strings)
	{
		Values.Add(MakeShared<FJsonValueString>(String));
	}
	return Values;
}

// One Project Settings section (or a config class named directly).
struct FSettingsSection
{
	FString Category;
	FString Name;
	FString DisplayName;
	TWeakObjectPtr<UObject> Object;
	TSharedPtr<ISettingsSection> Section;

	FString Key() const { return Category + TEXT("/") + Name; }
};

TArray<FSettingsSection> GatherProjectSections()
{
	TArray<FSettingsSection> Out;
	// The UDeveloperSettings sections (Physics, Rendering, plugin and game settings) are registered
	// when the Project Settings window opens; register them the same way.
	if (ISettingsEditorModule* SettingsEditor = FModuleManager::LoadModulePtr<ISettingsEditorModule>("SettingsEditor"))
	{
		SettingsEditor->UpdateSettings(/*bForce*/ true);
	}
	ISettingsModule* Module = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
	TSharedPtr<ISettingsContainer> Container = Module != nullptr ? Module->GetContainer(ProjectContainer) : nullptr;
	if (!Container.IsValid())
	{
		return Out;
	}
	TArray<TSharedPtr<ISettingsCategory>> Categories;
	Container->GetCategories(Categories);
	for (const TSharedPtr<ISettingsCategory>& Category : Categories)
	{
		TArray<TSharedPtr<ISettingsSection>> Sections;
		Category->GetSections(Sections);
		for (const TSharedPtr<ISettingsSection>& Section : Sections)
		{
			UObject* Object = Section.IsValid() ? Section->GetSettingsObject().Get() : nullptr;
			if (Object == nullptr)
			{
				continue; // A custom-widget section (no reflected settings object).
			}
			FSettingsSection& Entry = Out.AddDefaulted_GetRef();
			Entry.Category = Category->GetName().ToString();
			Entry.Name = Section->GetName().ToString();
			Entry.DisplayName = Section->GetDisplayName().ToString();
			Entry.Object = Object;
			Entry.Section = Section;
		}
	}
	Out.Sort([](const FSettingsSection& A, const FSettingsSection& B) { return A.Key() < B.Key(); });
	return Out;
}

// The config file the Project Settings editor writes for Object (SSettingsEditor::NotifyPostChange).
FString GetSettingsConfigFile(const UObject& Object, bool& bOutDefaultConfig)
{
	const UClass* Class = Object.GetClass();
	bOutDefaultConfig = Class->HasAnyClassFlags(CLASS_DefaultConfig);
	if (bOutDefaultConfig)
	{
		return FPaths::ConvertRelativePathToFull(Object.GetDefaultConfigFilename());
	}
	if (Class->HasAnyClassFlags(CLASS_Config))
	{
		return FPaths::ConvertRelativePathToFull(Class->GetConfigName());
	}
	return FString();
}

TSharedRef<FJsonObject> SectionJson(const FSettingsSection& Entry)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("section"), Entry.Key());
	Json->SetStringField(TEXT("display_name"), Entry.DisplayName);
	if (UObject* Object = Entry.Object.Get())
	{
		Json->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
		bool bDefaultConfig = false;
		const FString File = GetSettingsConfigFile(*Object, bDefaultConfig);
		FString Relative;
		if (File.IsEmpty())
		{
			Json->SetField(TEXT("config_file"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Json->SetStringField(TEXT("config_file"), IsProjectConfigFile(File, &Relative) ? Relative : FPaths::GetCleanFilename(File));
		}
		Json->SetBoolField(TEXT("writable"), !File.IsEmpty() && bDefaultConfig && IsProjectConfigFile(File));
	}
	return Json;
}

bool Matches(const FSettingsSection& Entry, const FString& Query)
{
	const UObject* Object = Entry.Object.Get();
	return Entry.Name.Equals(Query, ESearchCase::IgnoreCase)
		|| Entry.DisplayName.Equals(Query, ESearchCase::IgnoreCase)
		|| Entry.Key().Equals(Query, ESearchCase::IgnoreCase)
		|| (Object != nullptr && (Object->GetClass()->GetPathName().Equals(Query, ESearchCase::IgnoreCase)
			|| Object->GetClass()->GetName().Equals(Query, ESearchCase::IgnoreCase)));
}

// Resolves a section query; false with a Settings.SectionNotFound / SectionAmbiguous diagnostic.
bool ResolveSection(const FString& Query, FSettingsSection& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	const TArray<FSettingsSection> Sections = GatherProjectSections();
	TArray<const FSettingsSection*> Found;
	for (const FSettingsSection& Entry : Sections)
	{
		if (Matches(Entry, Query))
		{
			Found.Add(&Entry);
		}
	}
	if (Found.Num() == 1)
	{
		Out = *Found[0];
		return true;
	}
	if (Found.Num() > 1)
	{
		TArray<TSharedPtr<FJsonValue>> Candidates;
		for (const FSettingsSection* Entry : Found)
		{
			Candidates.Add(MakeShared<FJsonValueString>(Entry->Key()));
		}
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("candidates"), Candidates);
		Diagnostics.Add(MakeLevelDiagnostic(TEXT("/section"), TEXT("Settings.SectionAmbiguous"), FString::Printf(TEXT("'%s' names %d "
			"sections (details.candidates); use \"<Category>/<Section>\" or the settings class path."), *Query, Found.Num()), Details));
		return false;
	}
	// A config class named directly (a settings object the dialog does not list).
	if (Query.StartsWith(TEXT("/Script/")))
	{
		if (UClass* Class = FindObject<UClass>(nullptr, *Query))
		{
			if (Class->HasAnyClassFlags(CLASS_Config) && !Class->HasAnyClassFlags(CLASS_Abstract))
			{
				Out.Category = TEXT("Class");
				Out.Name = Class->GetName();
				Out.DisplayName = Class->GetDisplayNameText().ToString();
				Out.Object = Class->GetDefaultObject();
				return true;
			}
		}
	}
	// Close names: the sections whose name, display name, key or class is nearest to the query
	// (edit distance), or that contain a longer word of it.
	TArray<FString> Words;
	Query.Replace(TEXT("/"), TEXT(" ")).Replace(TEXT("."), TEXT(" ")).ParseIntoArray(Words, TEXT(" "));
	const FString Lower = Query.ToLower();
	TArray<TPair<int32, FString>> Scored;
	for (const FSettingsSection& Entry : Sections)
	{
		const UObject* Object = Entry.Object.Get();
		const TArray<FString> Names = {Entry.Name, Entry.DisplayName, Entry.Key(), Object != nullptr ? Object->GetClass()->GetName() : FString()};
		int32 Best = MAX_int32;
		for (const FString& Name : Names)
		{
			if (!Name.IsEmpty())
			{
				Best = FMath::Min(Best, static_cast<int32>(Algo::LevenshteinDistance(Lower, Name.ToLower())));
			}
		}
		const int32 Allowed = FMath::Max(3, Lower.Len() / 3);
		bool bWord = false;
		for (const FString& Word : Words)
		{
			for (const FString& Name : Names)
			{
				bWord |= Word.Len() >= 4 && Name.Contains(Word, ESearchCase::IgnoreCase);
			}
		}
		if (Best <= Allowed || bWord)
		{
			Scored.Emplace(bWord ? FMath::Min(Best, Allowed) : Best, FString::Printf(TEXT("%s (%s)"), *Entry.Key(), *Entry.DisplayName));
		}
	}
	Scored.StableSort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key < B.Key; });
	TArray<FString> Close;
	for (int32 Index = 0; Index < Scored.Num() && Index < 6; ++Index)
	{
		Close.Add(Scored[Index].Value);
	}
	TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(TEXT("close"), SettingsStringsJson(Close));
	Diagnostics.Add(MakeLevelDiagnostic(TEXT("/section"), TEXT("Settings.SectionNotFound"), FString::Printf(TEXT("No Project Settings "
		"section '%s'%s%s. Use \"world\" for the open level's World Settings; action \"list\" lists the sections."), *Query,
		Close.IsEmpty() ? TEXT("") : TEXT("; close: "), *FString::Join(Close, TEXT(", "))), Details));
	return false;
}

// The admitted fields a settings read lists for Object.
TArray<ObjectProperties::FAdmittedField> ListedFields(UObject& Object)
{
	TArray<ObjectProperties::FAdmittedField> Fields = ObjectProperties::ListFields(*Object.GetClass());
	Fields.RemoveAll([](const ObjectProperties::FAdmittedField& Field) { return Field.Property == nullptr; });
	return Fields;
}

bool ReadProperties(UObject& Object, const TOptional<TArray<FString>>& Names, bool bDiffOnly, TSharedRef<FJsonObject>& OutProperties,
	TArray<FString>& OutReadOnly, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	const TArray<ObjectProperties::FAdmittedField> Fields = ListedFields(Object);
	if (Names.IsSet())
	{
		for (int32 Index = 0; Index < Names->Num(); ++Index)
		{
			const FString& Name = (*Names)[Index];
			const ObjectProperties::FAdmittedField* Field = Fields.FindByPredicate([&Name](const ObjectProperties::FAdmittedField& Candidate)
			{
				return Candidate.Name == Name;
			});
			if (Field == nullptr)
			{
				Diagnostics.Add(MakeLevelDiagnostic(FString::Printf(TEXT("/properties/%d"), Index), TEXT("Generic.UnknownProperty"),
					FString::Printf(TEXT("'%s' is not a setting of %s (names are case-sensitive; read without 'properties' lists them)."),
						*Name, *Object.GetClass()->GetName())));
				continue;
			}
			FAssetDocumentRuntimeValueDto Value;
			if (ObjectProperties::Read(Object, *Field, Value))
			{
				OutProperties->SetField(Field->Name, LevelValueToJson(Value));
			}
			if (!Field->bWritable)
			{
				OutReadOnly.Add(Field->Name);
			}
		}
		return Diagnostics.IsEmpty();
	}
	for (const ObjectProperties::FAdmittedField& Field : Fields)
	{
		// diff_only: struct members and array elements' members equal to the defaults are left out as well (a write
		// of the result edits the current value, so it gives the same value).
		FAssetDocumentRuntimeValueDto Value;
		if (bDiffOnly ? ObjectProperties::ReadChanged(Object, Field, Value) : ObjectProperties::Read(Object, Field, Value))
		{
			OutProperties->SetField(Field.Name, LevelValueToJson(Value));
			if (!Field.bWritable)
			{
				OutReadOnly.Add(Field.Name);
			}
		}
	}
	return true;
}

struct FSettingsRequest
{
	FString Action = TEXT("read");
	FString Section;
	TOptional<TArray<FString>> ReadNames;
	TArray<TPair<FString, FLevelValue>> Writes;
	bool bDiffOnly = false;
	bool bHasDiffOnly = false;
	FString Description;
	bool bSave = false;
	// write: check and return the planned changes; nothing changes (as edit_level's dry_run).
	bool bDryRun = false;
};

bool ParseRequest(const FJsonObject& Json, FSettingsRequest& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	static const TSet<FString> Known = {TEXT("action"), TEXT("section"), TEXT("properties"), TEXT("diff_only"), TEXT("description"),
		TEXT("save"), TEXT("dry_run")};
	for (const auto& Field : Json.Values)
	{
		if (!Known.Contains(FString(*Field.Key)))
		{
			Diagnostics.Add(SettingsInvalidRequest(TEXT("/") + FString(*Field.Key), FString::Printf(TEXT("Unknown field '%s' (fields: action, section, "
				"properties, diff_only, description, save, dry_run)."), *Field.Key)));
		}
	}
	if (Json.HasField(TEXT("action")) && !Json.TryGetStringField(TEXT("action"), Out.Action))
	{
		Diagnostics.Add(SettingsInvalidRequest(TEXT("/action"), TEXT("'action' is \"list\", \"read\" or \"write\".")));
	}
	if (Out.Action != TEXT("list") && Out.Action != TEXT("read") && Out.Action != TEXT("write"))
	{
		Diagnostics.Add(SettingsInvalidRequest(TEXT("/action"), FString::Printf(TEXT("'action' is \"list\", \"read\" or \"write\", not '%s'."),
			*Out.Action)));
	}
	if (Out.Action != TEXT("list") && (!Json.TryGetStringField(TEXT("section"), Out.Section) || Out.Section.IsEmpty()))
	{
		Diagnostics.Add(SettingsInvalidRequest(TEXT("/section"), TEXT("'section' is required: \"world\" (the open level's World Settings) or a "
			"Project Settings section (e.g. \"Maps & Modes\", \"Project/Maps\", \"/Script/EngineSettings.GameMapsSettings\"); action "
			"\"list\" lists them.")));
	}
	const TSharedPtr<FJsonValue> Properties = Json.TryGetField(TEXT("properties"));
	if (Out.Action == TEXT("write"))
	{
		if (!Properties.IsValid() || Properties->Type != EJson::Object || Properties->AsObject()->Values.IsEmpty())
		{
			Diagnostics.Add(SettingsInvalidRequest(TEXT("/properties"), TEXT("write needs 'properties': {\"<Property>\": value, ...} in "
				"AssetDocument value forms (names as read returns them).")));
		}
		else
		{
			for (const auto& Member : Properties->AsObject()->Values)
			{
				Out.Writes.Emplace(Member.Key, JsonToLevelValue(Member.Value));
			}
		}
	}
	else if (Properties.IsValid())
	{
		TArray<FString> Names;
		if (Properties->Type != EJson::Array)
		{
			Diagnostics.Add(SettingsInvalidRequest(TEXT("/properties"), TEXT("read takes 'properties' as a list of property names.")));
		}
		else
		{
			for (const TSharedPtr<FJsonValue>& Name : Properties->AsArray())
			{
				FString Text;
				if (!Name.IsValid() || !Name->TryGetString(Text))
				{
					Diagnostics.Add(SettingsInvalidRequest(TEXT("/properties"), TEXT("'properties' lists property names (strings).")));
					break;
				}
				Names.Add(Text);
			}
			Out.ReadNames = MoveTemp(Names);
		}
	}
	if (Json.HasField(TEXT("diff_only")))
	{
		Out.bHasDiffOnly = true;
		if (!Json.TryGetBoolField(TEXT("diff_only"), Out.bDiffOnly))
		{
			Diagnostics.Add(SettingsInvalidRequest(TEXT("/diff_only"), TEXT("'diff_only' must be a boolean.")));
		}
	}
	if (Json.HasField(TEXT("description")) && !Json.TryGetStringField(TEXT("description"), Out.Description))
	{
		Diagnostics.Add(SettingsInvalidRequest(TEXT("/description"), TEXT("'description' must be a string.")));
	}
	if (Json.HasField(TEXT("save")) && !Json.TryGetBoolField(TEXT("save"), Out.bSave))
	{
		Diagnostics.Add(SettingsInvalidRequest(TEXT("/save"), TEXT("'save' must be a boolean.")));
	}
	// Strictly a JSON boolean: TryGetBoolField would read the string "yes" as true and run the write.
	const TSharedPtr<FJsonValue> DryRun = Json.TryGetField(TEXT("dry_run"));
	if (DryRun.IsValid() && DryRun->Type == EJson::Boolean)
	{
		Out.bDryRun = DryRun->AsBool();
	}
	else if (DryRun.IsValid())
	{
		Diagnostics.Add(SettingsInvalidRequest(TEXT("/dry_run"), TEXT("'dry_run' must be a boolean.")));
	}
	return Diagnostics.IsEmpty();
}

// A decoded write of one property, with its before value.
struct FSettingsWrite
{
	FString Name;
	FString Pointer;
	FLevelValue Authored;
	FLevelValue Before;
	bool bHasBefore = false;
	ObjectProperties::FPreparedWrite Prepared;
};

bool PrepareWrites(UObject& Object, const FSettingsRequest& Request, const ObjectProperties::FReferencePolicy& Policy,
	TArray<FSettingsWrite>& OutWrites, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	for (const TPair<FString, FLevelValue>& Entry : Request.Writes)
	{
		FSettingsWrite& Write = OutWrites.AddDefaulted_GetRef();
		Write.Name = Entry.Key;
		Write.Pointer = TEXT("/properties/") + EscapePointerSegment(Entry.Key);
		Write.Authored = Entry.Value;
		TArray<FAssetDocumentRuntimeDiagnostic> Problems;
		if (!ObjectProperties::Decode(Object, Entry.Key, Entry.Value, Policy, Write.Prepared, Problems))
		{
			for (const FAssetDocumentRuntimeDiagnostic& Problem : Problems)
			{
				Diagnostics.Add(ProjectLevelPropertyDiagnostic(Problem, Write.Pointer, Entry.Key));
			}
			continue;
		}
		Write.bHasBefore = ObjectProperties::Read(Object, Entry.Key, Write.Before);
	}
	return Diagnostics.IsEmpty();
}

// Commits the writes and reads every value back; false with diagnostics at the first failure.
bool CommitWrites(UObject& Object, TArray<FSettingsWrite>& Writes, const ObjectProperties::FReferencePolicy& Policy,
	TArray<TSharedPtr<FJsonValue>>& OutChanges, int32& OutCommitted, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	OutCommitted = 0;
	for (FSettingsWrite& Write : Writes)
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Problems;
		if (!ObjectProperties::Commit(Write.Prepared, Problems))
		{
			for (const FAssetDocumentRuntimeDiagnostic& Problem : Problems)
			{
				Diagnostics.Add(ProjectLevelPropertyDiagnostic(Problem, Write.Pointer, Write.Name));
			}
			return false;
		}
		++OutCommitted;
	}
	for (FSettingsWrite& Write : Writes)
	{
		FLevelValue Expected;
		FLevelValue Stored;
		TArray<FAssetDocumentRuntimeDiagnostic> Problems;
		if (!ObjectProperties::PrepareExpected(Object, Write.Name, Write.Authored, Policy, Expected, Problems)
			|| !ObjectProperties::EncodeAsAuthored(Object, Write.Name, Expected, Stored))
		{
			Diagnostics.Add(MakeLevelDiagnostic(Write.Pointer, TEXT("Settings.VerificationMismatch"), FString::Printf(TEXT("'%s' could "
				"not be read back."), *Write.Name)));
			return false;
		}
		FString Difference;
		if (!LevelValuesEqual(Expected, Stored, &Difference))
		{
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetField(TEXT("expected"), LevelValueToJson(Expected));
			Details->SetField(TEXT("stored"), LevelValueToJson(Stored));
			Diagnostics.Add(MakeLevelDiagnostic(Write.Pointer + Difference, TEXT("Settings.PropertyValueNotStoredExactly"),
				FString::Printf(TEXT("'%s' holds %s after the write, not the requested %s (a clamp or PostEditChange rule changed it)."),
					*Write.Name, *LevelValueToText(Stored), *LevelValueToText(Expected)), Details));
			return false;
		}
		FLevelValue After;
		ObjectProperties::Read(Object, Write.Name, After);
		if (!Write.bHasBefore || !LevelValuesEqual(Write.Before, After))
		{
			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("property"), Write.Name);
			Change->SetField(TEXT("before"), Write.bHasBefore ? LevelValueToJson(Write.Before) : MakeShared<FJsonValueNull>());
			Change->SetField(TEXT("after"), LevelValueToJson(After));
			OutChanges.Add(MakeShared<FJsonValueObject>(Change));
		}
	}
	return true;
}

// Writes the before values of the first Count writes back (best effort; project settings rollback).
void RestoreValues(UObject& Object, TArray<FSettingsWrite>& Writes, int32 Count)
{
	for (int32 Index = 0; Index < Count && Index < Writes.Num(); ++Index)
	{
		const FSettingsWrite& Write = Writes[Index];
		if (!Write.bHasBefore)
		{
			continue;
		}
		ObjectProperties::FPreparedWrite Restore;
		TArray<FAssetDocumentRuntimeDiagnostic> Ignored;
		if (ObjectProperties::Decode(Object, Write.Name, Write.Before, ObjectProperties::FReferencePolicy(), Restore, Ignored))
		{
			ObjectProperties::Commit(Restore, Ignored);
		}
	}
}

bool SettingsIsEditorBusy(FString& OutReason)
{
	if (GIsTransacting)
	{
		OutReason = TEXT("an Undo / Redo is being applied");
		return true;
	}
	if (GEditor->IsTransactionActive())
	{
		OutReason = FString::Printf(TEXT("an editor operation is in progress ('%s')"), *GEditor->GetTransactionName().ToString());
		return true;
	}
	if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveModalWindow().IsValid())
	{
		OutReason = TEXT("a modal dialog is open");
		return true;
	}
	return false;
}

} // namespace

// Unity-safe per-file helpers.
namespace EditorSettingsToolLocal
{
// A write's dry run: the checked writes and the change each would make (after = the authored
// value merged into the current one, as edit_level's dry run plans it); nothing changes. PostEditChange rules run only
// on the write, whose read-back checks the stored values.
FLevelToolResult PlanSettingsWrites(const TArray<FSettingsWrite>& Writes, const TSharedRef<FJsonObject>& Payload, const FString& Subject,
	const FString& Title, TArray<FAssetDocumentDiagnostic> Warnings)
{
	TArray<TSharedPtr<FJsonValue>> Changes;
	for (const FSettingsWrite& Write : Writes)
	{
		const FLevelValue After = Write.bHasBefore ? MergeAuthoredValue(Write.Before, Write.Authored) : Write.Authored;
		if (Write.bHasBefore && LevelValuesEqual(Write.Before, After))
		{
			continue;
		}
		TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("property"), Write.Name);
		Change->SetField(TEXT("before"), Write.bHasBefore ? LevelValueToJson(Write.Before) : MakeShared<FJsonValueNull>());
		Change->SetField(TEXT("after"), LevelValueToJson(After));
		Changes.Add(MakeShared<FJsonValueObject>(Change));
	}
	Payload->SetBoolField(TEXT("dry_run"), true);
	if (!Title.IsEmpty())
	{
		Payload->SetStringField(TEXT("description"), Title);
	}
	Payload->SetArrayField(TEXT("changes"), Changes);
	FLevelToolResult Result = FLevelToolResult::Success(FString::Printf(TEXT("Dry run: the write is valid and would change %d value(s) "
		"of %s; nothing was changed. PostEditChange rules run only on the write, where the read-back checks the stored values."),
		Changes.Num(), *Subject), Payload);
	Result.Diagnostics = MoveTemp(Warnings);
	return Result;
}
} // namespace EditorSettingsToolLocal

namespace
{
FLevelToolResult RunWorld(const FSettingsRequest& Request)
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	AWorldSettings* WorldSettings = World != nullptr ? World->GetWorldSettings() : nullptr;
	if (WorldSettings == nullptr)
	{
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT("/section"), TEXT("Settings.WorldSettingsUnavailable"),
			TEXT("The open level has no World Settings actor."))});
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("section"), TEXT("world"));
	Payload->SetStringField(TEXT("world"), World->GetPackage()->GetName());
	Payload->SetStringField(TEXT("actor"), WorldSettings->GetPathName());
	Payload->SetStringField(TEXT("class"), WorldSettings->GetClass()->GetPathName());

	TArray<FAssetDocumentDiagnostic> Diagnostics;
	if (Request.Action == TEXT("read"))
	{
		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		TArray<FString> ReadOnly;
		const bool bDiffOnly = Request.bHasDiffOnly ? Request.bDiffOnly : !Request.ReadNames.IsSet();
		if (!ReadProperties(*WorldSettings, Request.ReadNames, bDiffOnly, Properties, ReadOnly, Diagnostics))
		{
			return FLevelToolResult::Failure(MoveTemp(Diagnostics));
		}
		Payload->SetBoolField(TEXT("diff_only"), bDiffOnly && !Request.ReadNames.IsSet());
		Payload->SetObjectField(TEXT("properties"), Properties);
		Payload->SetArrayField(TEXT("read_only"), SettingsStringsJson(ReadOnly));
		return FLevelToolResult::Success(FString::Printf(TEXT("World Settings of %s: %d value(s)%s."), *World->GetPackage()->GetName(),
			Properties->Values.Num(), bDiffOnly && !Request.ReadNames.IsSet() ? TEXT(" that differ from the class defaults") : TEXT("")),
			Payload);
	}

	FString BusyReason;
	if (SettingsIsEditorBusy(BusyReason))
	{
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT(""), TEXT("Level.EditorBusy"), FString::Printf(TEXT("The editor is "
			"busy: %s. Nothing was changed; retry shortly."), *BusyReason))});
	}
	ObjectProperties::FReferencePolicy Policy;
	Policy.SameLevel = WorldSettings->GetLevel();
	TArray<FSettingsWrite> Writes;
	if (!PrepareWrites(*WorldSettings, Request, Policy, Writes, Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	UPackage* MapPackage = WorldSettings->GetPackage();
	const bool bWasDirty = MapPackage->IsDirty();
	const FString Title = Request.Description.IsEmpty() ? FString(TEXT("AI: World Settings")) : Request.Description;
	if (Request.bDryRun)
	{
		return EditorSettingsToolLocal::PlanSettingsWrites(Writes, Payload, TEXT("the World Settings"), Title, {});
	}
	TArray<TSharedPtr<FJsonValue>> Changes;
	FAssetDocumentOwnedEditorTransaction Transaction;
	bool bFailed = false;
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		if (!Transaction.Begin(FText::FromString(Title)) || !Transaction.IsOutermost())
		{
			Transaction.Rollback();
			return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT(""), TEXT("Level.EditorBusy"), TEXT("The editor could not open the "
				"transaction (another transaction or an Undo is in progress). Nothing was changed; retry shortly."))});
		}
		int32 Committed = 0;
		bFailed = !CommitWrites(*WorldSettings, Writes, Policy, Changes, Committed, Diagnostics);
		if (!bFailed && NonInteractive.WasInteractionRefused())
		{
			Diagnostics.Add(MakeLevelDiagnostic(TEXT(""), TEXT("Level.EditorDialogRefused"), TEXT("A World Settings change tried to open "
				"a dialog, which settings refuses; the change is rolled back.")));
			bFailed = true;
		}
	}
	if (bFailed)
	{
		const EAssetDocumentTransactionRollback Outcome = Transaction.Rollback();
		TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
		Failure->SetBoolField(TEXT("rolled_back"), Outcome == EAssetDocumentTransactionRollback::Undone
			|| Outcome == EAssetDocumentTransactionRollback::NothingRecorded);
		Failure->SetStringField(TEXT("rollback"), LexToString(Outcome));
		return FLevelToolResult::Failure(MoveTemp(Diagnostics), Failure);
	}
	const bool bUndoable = Transaction.Commit();
	TSharedRef<FJsonObject> TransactionJson = MakeShared<FJsonObject>();
	TransactionJson->SetStringField(TEXT("title"), Title);
	TransactionJson->SetBoolField(TEXT("undoable"), bUndoable);
	if (bUndoable)
	{
		TransactionJson->SetStringField(TEXT("id"), FormatUndoEntryId(Transaction.GetTransactionId()));
	}
	Payload->SetObjectField(TEXT("transaction"), TransactionJson);
	Payload->SetArrayField(TEXT("changes"), Changes);
	TArray<FString> Dirty;
	if (MapPackage->IsDirty())
	{
		Dirty.Add(MapPackage->GetName());
	}
	Payload->SetArrayField(TEXT("dirty_packages"), SettingsStringsJson(Dirty));
	Payload->SetBoolField(TEXT("already_dirty"), bWasDirty);
	TArray<FString> Saved;
	FLevelToolResult Result = FLevelToolResult::Success(Changes.IsEmpty()
		? FString(TEXT("World Settings unchanged (the values were already set; no Undo entry)."))
		: FString::Printf(TEXT("World Settings: %d value(s) changed (undo: '%s')."), Changes.Num(), *Title), Payload);
	if (Request.bSave && MapPackage->IsDirty())
	{
		TArray<UPackage*> Failed;
		{
			FAssetDocumentEditorNonInteractiveScope NonInteractive;
			FEditorFileUtils::PromptForCheckoutAndSave({MapPackage}, /*bCheckDirty*/ false, /*bPromptToSave*/ false, &Failed,
				/*bAlreadyCheckedOut*/ false, /*bCanBeDeclined*/ false);
		}
		if (Failed.IsEmpty() && !MapPackage->IsDirty())
		{
			Saved.Add(MapPackage->GetName());
			Result.Message += TEXT(" Saved the map.");
		}
		else
		{
			Result.HttpStatus = 207;
			Result.bSuccess = false;
			Result.Diagnostics.Add(MakeLevelDiagnostic(TEXT("/save"), TEXT("Level.SaveFailed"), FString::Printf(TEXT("The change is applied "
				"and in the Undo history, but %s was not saved; save it from the editor."), *MapPackage->GetName())));
		}
	}
	Payload->SetArrayField(TEXT("saved_packages"), SettingsStringsJson(Saved));
	return Result;
}

// Writes the changed properties to the section's config file as SSettingsEditor::NotifyPostChange does.
void WriteConfig(UObject& Object, const FSettingsSection& Entry, const TArray<FSettingsWrite>& Writes)
{
	static IConsoleVariable* NewPropertySaving = IConsoleManager::Get().FindConsoleVariable(TEXT("ini.UseNewPropertySaving"));
	bool bFullSection = false;
	if (NewPropertySaving == nullptr || NewPropertySaving->GetInt() == 0)
	{
		for (const FSettingsWrite& Write : Writes)
		{
			const FProperty* Property = Object.GetClass()->FindPropertyByName(*Write.Name);
			bFullSection |= Property != nullptr && (Property->IsA<FArrayProperty>() || Property->IsA<FSetProperty>()
				|| Property->IsA<FMapProperty>() || Property->ArrayDim > 1);
		}
	}
	if (Object.GetClass()->HasAnyClassFlags(CLASS_DefaultConfig) && !bFullSection)
	{
		if (!Entry.Section.IsValid() || Entry.Section->NotifySectionOnPropertyModified())
		{
			for (const FSettingsWrite& Write : Writes)
			{
				if (const FProperty* Property = Object.GetClass()->FindPropertyByName(*Write.Name))
				{
					Object.UpdateSinglePropertyInConfigFile(Property, Object.GetDefaultConfigFilename());
				}
			}
		}
	}
	else if (Entry.Section.IsValid())
	{
		Entry.Section->Save();
	}
	else if (Object.GetClass()->HasAnyClassFlags(CLASS_DefaultConfig))
	{
		Object.TryUpdateDefaultConfigFile();
	}
	else
	{
		Object.SaveConfig();
	}
}

FLevelToolResult RunProject(const FSettingsRequest& Request)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	FSettingsSection Entry;
	if (!ResolveSection(Request.Section, Entry, Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	UObject* Object = Entry.Object.Get();
	if (Object == nullptr)
	{
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT("/section"), TEXT("Settings.SectionNotFound"),
			TEXT("The section has no settings object."))});
	}
	TSharedRef<FJsonObject> Payload = SectionJson(Entry);
	if (Request.Action == TEXT("read"))
	{
		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		TArray<FString> ReadOnly;
		if (!ReadProperties(*Object, Request.ReadNames, /*bDiffOnly*/ false, Properties, ReadOnly, Diagnostics))
		{
			return FLevelToolResult::Failure(MoveTemp(Diagnostics));
		}
		Payload->SetObjectField(TEXT("properties"), Properties);
		Payload->SetArrayField(TEXT("read_only"), SettingsStringsJson(ReadOnly));
		return FLevelToolResult::Success(FString::Printf(TEXT("%s (%s): %d value(s)."), *Entry.DisplayName, *Entry.Key(),
			Properties->Values.Num()), Payload);
	}

	// write
	if (Entry.Section.IsValid() && !Entry.Section->CanEdit())
	{
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT("/section"), TEXT("Settings.SectionReadOnly"), FString::Printf(
			TEXT("The Project Settings editor does not allow editing %s now."), *Entry.Key()))}, Payload);
	}
	bool bDefaultConfig = false;
	const FString ConfigFile = GetSettingsConfigFile(*Object, bDefaultConfig);
	FString Relative;
	if (ConfigFile.IsEmpty())
	{
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT("/section"), TEXT("Settings.NotConfigBacked"), FString::Printf(
			TEXT("%s is not saved to a config file; it cannot be written."), *Entry.Key()))}, Payload);
	}
	if (!bDefaultConfig || !IsProjectConfigFile(ConfigFile, &Relative))
	{
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("config_file"), ConfigFile);
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT("/section"), TEXT("Settings.ConfigFileNotInProject"), FString::Printf(
			TEXT("%s is saved to %s, which is not a default config file in the project's Config/ directory (engine, plugin and per-user "
				"config files are not written). Change it in the editor if needed."), *Entry.Key(), *FPaths::GetCleanFilename(ConfigFile)),
			Details)}, Payload);
	}
	TArray<FSettingsWrite> Writes;
	if (!PrepareWrites(*Object, Request, ObjectProperties::FReferencePolicy(), Writes, Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics), Payload);
	}
	TArray<FAssetDocumentDiagnostic> Warnings;
	bool bRestart = false;
	for (const FSettingsWrite& Write : Writes)
	{
		const FProperty* Property = Object->GetClass()->FindPropertyByName(*Write.Name);
		if (Property != nullptr && !Property->HasAnyPropertyFlags(CPF_Config))
		{
			Warnings.Add(MakeLevelDiagnostic(Write.Pointer, TEXT("Settings.PropertyNotInConfig"), FString::Printf(TEXT("'%s' is not a "
				"config property: it changes in this session but is not saved to %s."), *Write.Name, *Relative)));
			Warnings.Last().Severity = TEXT("warning");
		}
		bRestart |= Property != nullptr && Property->GetBoolMetaData(TEXT("ConfigRestartRequired"));
	}
	if (Request.bDryRun)
	{
		return EditorSettingsToolLocal::PlanSettingsWrites(Writes, Payload, Entry.Key(), FString(), MoveTemp(Warnings));
	}

	// Snapshot the file, then make it writable as the settings editor does.
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const bool bFileExisted = PlatformFile.FileExists(*ConfigFile);
	TArray<uint8> FileBefore;
	if (bFileExisted)
	{
		FFileHelper::LoadFileToArray(FileBefore, *ConfigFile);
	}
	bool bCheckedOut = false;
	FText WritableError;
	if (ISourceControlModule::Get().IsEnabled())
	{
		bCheckedOut = SettingsHelpers::CheckOutOrAddFile(ConfigFile, /*bForceSourceControlUpdate*/ false, /*ShowErrorInNotification*/ false,
			&WritableError);
	}
	if (!bCheckedOut)
	{
		SettingsHelpers::MakeWritable(ConfigFile, /*ShowErrorInNotification*/ false, &WritableError);
	}
	if (bFileExisted && PlatformFile.IsReadOnly(*ConfigFile))
	{
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT("/section"), TEXT("Settings.ConfigFileReadOnly"), FString::Printf(
			TEXT("%s is read-only and could not be checked out or made writable%s%s. Nothing was changed."), *Relative,
			WritableError.IsEmpty() ? TEXT("") : TEXT(": "), *WritableError.ToString()))}, Payload);
	}

	TArray<TSharedPtr<FJsonValue>> Changes;
	int32 Committed = 0;
	bool bFailed = false;
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		bFailed = !CommitWrites(*Object, Writes, ObjectProperties::FReferencePolicy(), Changes, Committed, Diagnostics);
		if (!bFailed)
		{
			WriteConfig(*Object, Entry, Writes);
		}
	}
	// Read the written keys back from the file.
	TSharedRef<FJsonObject> Written = MakeShared<FJsonObject>();
	if (!bFailed)
	{
		FConfigFile File;
		File.Read(ConfigFile);
		const FString SectionName = Object->GetClass()->GetPathName();
		for (const FSettingsWrite& Write : Writes)
		{
			const FProperty* Property = Object->GetClass()->FindPropertyByName(*Write.Name);
			if (Property == nullptr || !Property->HasAnyPropertyFlags(CPF_Config))
			{
				continue;
			}
			const bool bContainer = Property->IsA<FArrayProperty>() || Property->IsA<FSetProperty>() || Property->IsA<FMapProperty>()
				|| Property->ArrayDim > 1;
			FString Stored;
			if (File.GetString(*SectionName, *Write.Name, Stored))
			{
				Written->SetStringField(Write.Name, Stored);
				continue;
			}
			if (bContainer)
			{
				continue; // Arrays are written as +Name / .Name lines (or omitted when equal to the base config).
			}
			// A changed scalar value must be in the file.
			if (Changes.ContainsByPredicate([&Write](const TSharedPtr<FJsonValue>& Change)
				{
					return Change->AsObject()->GetStringField(TEXT("property")) == Write.Name;
				}))
			{
				Diagnostics.Add(MakeLevelDiagnostic(Write.Pointer, TEXT("Settings.ConfigWriteFailed"), FString::Printf(TEXT("'%s' is not "
					"in [%s] of %s after the write."), *Write.Name, *SectionName, *Relative)));
				bFailed = true;
			}
		}
	}
	if (bFailed)
	{
		RestoreValues(*Object, Writes, Committed);
		if (bFileExisted)
		{
			FFileHelper::SaveArrayToFile(FileBefore, *ConfigFile);
		}
		else
		{
			PlatformFile.DeleteFile(*ConfigFile);
		}
		TSharedRef<FJsonObject> Failure = SectionJson(Entry);
		Failure->SetBoolField(TEXT("rolled_back"), true);
		return FLevelToolResult::Failure(MoveTemp(Diagnostics), Failure);
	}
	if (bRestart)
	{
		Warnings.Add(MakeLevelDiagnostic(TEXT("/properties"), TEXT("Settings.RestartRequired"), TEXT("A changed setting takes effect "
			"after an editor restart (ConfigRestartRequired).")));
		Warnings.Last().Severity = TEXT("warning");
	}
	Payload->SetStringField(TEXT("config_file"), Relative);
	Payload->SetArrayField(TEXT("changes"), Changes);
	Payload->SetObjectField(TEXT("written_keys"), Written);
	Payload->SetBoolField(TEXT("checked_out"), bCheckedOut);
	FLevelToolResult Result = FLevelToolResult::Success(Changes.IsEmpty()
		? FString::Printf(TEXT("%s unchanged (the values were already set)."), *Entry.DisplayName)
		: FString::Printf(TEXT("%s: %d value(s) changed and written to %s (not in the Undo history)."), *Entry.DisplayName,
			Changes.Num(), *Relative), Payload);
	Result.Diagnostics = MoveTemp(Warnings);
	return Result;
}

FLevelToolResult RunList()
{
	TArray<TSharedPtr<FJsonValue>> Sections;
	for (const FSettingsSection& Entry : GatherProjectSections())
	{
		Sections.Add(MakeShared<FJsonValueObject>(SectionJson(Entry)));
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("sections"), Sections);
	return FLevelToolResult::Success(FString::Printf(TEXT("%d Project Settings sections (and \"world\": the open level's World "
		"Settings)."), Sections.Num()), Payload);
}
} // namespace

bool IsProjectConfigFile(const FString& Filename, FString* OutRelative)
{
	FString Full = FPaths::ConvertRelativePathToFull(Filename);
	FPaths::NormalizeFilename(Full);
	FPaths::CollapseRelativeDirectories(Full);
	FString ConfigDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir());
	FPaths::NormalizeDirectoryName(ConfigDir);
	ConfigDir += TEXT("/");
	if (!Full.EndsWith(TEXT(".ini"), ESearchCase::IgnoreCase) || !Full.StartsWith(ConfigDir, ESearchCase::IgnoreCase)
		|| Full.Contains(TEXT("/../")))
	{
		return false;
	}
	if (OutRelative != nullptr)
	{
		*OutRelative = TEXT("Config/") + Full.RightChop(ConfigDir.Len());
	}
	return true;
}

FLevelToolResult RunEditorSettings(const TSharedPtr<FJsonObject>& RequestJson)
{
	check(IsInGameThread());
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	FSettingsRequest Request;
	if (!RequestJson.IsValid() || !ParseRequest(*RequestJson, Request, Diagnostics))
	{
		if (Diagnostics.IsEmpty())
		{
			Diagnostics.Add(SettingsInvalidRequest(TEXT(""), TEXT("The request must be a JSON object.")));
		}
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (!CheckLevelToolAdmission(Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (Request.Action == TEXT("list"))
	{
		return RunList();
	}
	if (Request.Section.Equals(TEXT("world"), ESearchCase::IgnoreCase))
	{
		return RunWorld(Request);
	}
	if (Request.bSave || !Request.Description.IsEmpty())
	{
		return FLevelToolResult::Failure({SettingsInvalidRequest(Request.bSave ? TEXT("/save") : TEXT("/description"), TEXT("'save' and "
			"'description' apply to section \"world\" only: Project Settings writes go to the config file at once and are not in the "
			"Undo history."))});
	}
	return RunProject(Request);
}

FLevelToolResult RunEditorSettingsFromBody(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody.IsEmpty() ? FString(TEXT("{}")) : RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return FLevelToolResult::Failure({SettingsInvalidRequest(TEXT(""), TEXT("The request body must be a JSON object ({\"action\", "
			"\"section\", \"properties\"}).")) });
	}
	return RunEditorSettings(Json);
}
}
