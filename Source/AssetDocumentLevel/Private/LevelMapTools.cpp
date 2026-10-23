// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "LevelMapTools.h"

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentNameSuggestions.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/TemplateMapInfo.h"
#include "Editor/UnrealEdEngine.h"
#include "EditorModeManager.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "LevelEditorSubsystem.h"
#include "LevelEditTypes.h"
#include "LevelQuery.h"
#include "LevelWorldIndex.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealEdGlobals.h"
#include "UObject/Package.h"

namespace UE::AssetDocumentLevel::Private
{
namespace
{
constexpr int32 MaxListedPackages = 12;

TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Json;
	for (const FString& Value : Values)
	{
		Json.Add(MakeShared<FJsonValueString>(Value));
	}
	return Json;
}

FString ListSome(const TArray<FString>& Values)
{
	TArray<FString> Head(Values.GetData(), FMath::Min(Values.Num(), MaxListedPackages));
	FString Text = FString::Join(Head, TEXT(", "));
	if (Values.Num() > Head.Num())
	{
		Text += FString::Printf(TEXT(" (+%d more)"), Values.Num() - Head.Num());
	}
	return Text;
}

struct FMapRequest
{
	FString Package;
	FString Template;
	bool bSaveFirst = false;
};

bool ParseMapRequest(const TSharedPtr<FJsonObject>& Json, bool bNew, FMapRequest& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	if (!Json.IsValid())
	{
		Diagnostics.Add(MakeInvalidRequest(TEXT(""), TEXT("The request must be a JSON object.")));
		return false;
	}
	const TCHAR* Fields = bNew ? TEXT("path, template, save_first") : TEXT("path, save_first");
	for (const auto& Field : Json->Values)
	{
		const bool bKnown = Field.Key == TEXT("path") || Field.Key == TEXT("save_first") || (bNew && Field.Key == TEXT("template"));
		if (!bKnown)
		{
			Diagnostics.Add(MakeInvalidRequest(TEXT("/") + FString(*Field.Key), FString::Printf(TEXT("Unknown field '%s' (fields: %s)."), *Field.Key, Fields)));
		}
		else if (Field.Key == TEXT("save_first") ? Field.Value->Type != EJson::Boolean : Field.Value->Type != EJson::String)
		{
			Diagnostics.Add(MakeInvalidRequest(TEXT("/") + FString(*Field.Key), FString::Printf(TEXT("'%s' must be a %s."), *Field.Key,
				Field.Key == TEXT("save_first") ? TEXT("boolean") : TEXT("string"))));
		}
	}
	if (!Diagnostics.IsEmpty())
	{
		return false;
	}
	FString Path;
	Json->TryGetStringField(TEXT("path"), Path);
	Path.TrimStartAndEndInline();
	// "/Game/Maps/Room.Room" (object path) or "/Game/Maps/Room" (package).
	int32 Dot = INDEX_NONE;
	Out.Package = Path.FindChar(TEXT('.'), Dot) ? Path.Left(Dot) : Path;
	FText Reason;
	if (Out.Package.IsEmpty() || !FPackageName::IsValidLongPackageName(Out.Package, /*bIncludeReadOnlyRoots*/ !bNew, &Reason))
	{
		Diagnostics.Add(MakeInvalidRequest(TEXT("/path"), FString::Printf(TEXT("'path' is the map's package path, like /Game/Maps/Room%s."),
			Reason.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Reason.ToString()))));
		return false;
	}
	if (bNew && (Out.Package.StartsWith(TEXT("/Engine/")) || Out.Package.StartsWith(TEXT("/Temp/"))))
	{
		Diagnostics.Add(MakeInvalidRequest(TEXT("/path"), TEXT("A new map goes into the project's content (/Game/...) or a content plugin, not /Engine or /Temp.")));
		return false;
	}
	Out.Template = TEXT("empty");
	Json->TryGetStringField(TEXT("template"), Out.Template);
	Json->TryGetBoolField(TEXT("save_first"), Out.bSaveFirst);
	return true;
}

bool MapExists(const FString& Package)
{
	FString Filename;
	return FPackageName::DoesPackageExist(Package, &Filename)
		&& FPaths::GetExtension(Filename, /*bIncludeDot*/ true) == FPackageName::GetMapPackageExtension();
}

UWorld* GetEditorWorld()
{
	return GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
}

// Level.MapNotFound naming the maps closest to the one asked for (by map name), else some maps of the project.
FAssetDocumentDiagnostic MapNotFound(const FString& Package)
{
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssetsByClass(UWorld::StaticClass()->GetClassPathName(), Assets);
	TArray<FString> Packages;
	TArray<FString> Names;
	for (const FAssetData& Asset : Assets)
	{
		const FString Name = Asset.PackageName.ToString();
		if (!Name.StartsWith(TEXT("/Temp/")) && !Name.StartsWith(TEXT("/Engine/")) && !Packages.Contains(Name))
		{
			Packages.Add(Name);
			Names.Add(FPackageName::GetShortName(Name));
		}
	}
	const TArray<FString> CloseNames = UE::AssetDocument::SuggestCloseNames(FPackageName::GetShortName(Package), Names, 5);
	TArray<FString> Close;
	for (const FString& Name : CloseNames)
	{
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (Names[Index] == Name && Close.Num() < 5)
			{
				Close.AddUnique(Packages[Index]);
			}
		}
	}
	Packages.Sort();
	TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(TEXT("candidates"), ToJsonStrings(Close.IsEmpty() ? TArray<FString>(Packages.GetData(),
		FMath::Min(Packages.Num(), MaxListedPackages)) : Close));
	const FString Hint = !Close.IsEmpty() ? FString::Printf(TEXT(" Close: %s."), *FString::Join(Close, TEXT(", ")))
		: Packages.IsEmpty() ? FString(TEXT(" The project has no maps.")) : FString::Printf(TEXT(" Maps: %s."), *ListSome(Packages));
	return MakeLevelDiagnostic(TEXT("/path"), TEXT("Level.MapNotFound"), FString::Printf(TEXT("There is no map %s.%s"), *Package, *Hint), Details);
}

bool CheckNotBusy(TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	if (GIsTransacting || GEditor->IsTransactionActive() || UE::IsSavingPackage(nullptr)
		|| (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveModalWindow().IsValid()))
	{
		Diagnostics.Add(MakeLevelDiagnostic(TEXT(""), TEXT("Level.EditorBusy"), TEXT("The editor is busy (a transaction, an Undo, a save or a "
			"modal dialog is in progress). Nothing was changed; retry shortly.")));
		return false;
	}
	return true;
}

// Refuses when the editor world has unsaved changes a map change would lose, or saves them first (save_first).
bool HandleUnsavedChanges(UWorld& World, bool bSaveFirst, TArray<FString>& OutSaved, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	TArray<FString> Dirty = GatherDirtyLevelPackages(World).Array();
	if (Dirty.IsEmpty())
	{
		return true;
	}
	Dirty.Sort();
	const FString WorldPackage = World.GetPackage()->GetName();
	TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(TEXT("packages"), ToJsonStrings(Dirty));
	if (!FPackageName::DoesPackageExist(WorldPackage))
	{
		Details->SetBoolField(TEXT("never_saved"), true);
		Diagnostics.Add(MakeLevelDiagnostic(TEXT(""), TEXT("Level.UnsavedChangesWouldBeLost"), FString::Printf(TEXT("The open level %s was "
			"never saved and has changes; changing the map would lose them, so nothing was done. The tools cannot pick a file name "
			"for it: ask the user to save it (File > Save Current Level As) or to discard it, then retry."), *WorldPackage), Details));
		return false;
	}
	if (!bSaveFirst)
	{
		Diagnostics.Add(MakeLevelDiagnostic(TEXT("/save_first"), TEXT("Level.UnsavedChangesWouldBeLost"), FString::Printf(TEXT("The open level "
			"has unsaved changes in %d package(s): %s. Changing the map would lose them, so nothing was done. Retry with "
			"\"save_first\": true to save exactly these packages first (the Save of the editor's prompt), or ask the user; "
			"they may be the user's own edits."), Dirty.Num(), *ListSome(Dirty)), Details));
		return false;
	}
	TArray<UPackage*> ToSave;
	for (const FString& Name : Dirty)
	{
		if (UPackage* Package = FindPackage(nullptr, *Name))
		{
			ToSave.Add(Package);
		}
	}
	TArray<UPackage*> Failed;
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		FEditorFileUtils::PromptForCheckoutAndSave(ToSave, /*bCheckDirty*/ false, /*bPromptToSave*/ false, &Failed,
			/*bAlreadyCheckedOut*/ false, /*bCanBeDeclined*/ false);
	}
	TArray<FString> NotSaved;
	for (UPackage* Package : ToSave)
	{
		if (Failed.Contains(Package) || Package->IsDirty())
		{
			NotSaved.Add(Package->GetName());
		}
		else
		{
			OutSaved.Add(Package->GetName());
		}
	}
	if (!NotSaved.IsEmpty())
	{
		Details->SetArrayField(TEXT("not_saved"), ToJsonStrings(NotSaved));
		Diagnostics.Add(MakeLevelDiagnostic(TEXT("/save_first"), TEXT("Level.SaveFailed"), FString::Printf(TEXT("%d of %d unsaved packages "
			"could not be saved (%s); the map was not changed. Save them from the editor (it says why), then retry."), NotSaved.Num(),
			ToSave.Num(), *ListSome(NotSaved)), Details));
		return false;
	}
	return true;
}

FLevelToolResult MapChanged(const FString& Verb, const FString& Package, const FString& Template, const TArray<FString>& Saved)
{
	UWorld* World = GetEditorWorld();
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("level"), Package);
	if (!Template.IsEmpty())
	{
		Payload->SetStringField(TEXT("template"), Template);
	}
	Payload->SetArrayField(TEXT("saved_before"), ToJsonStrings(Saved));
	if (World != nullptr)
	{
		Payload->SetObjectField(TEXT("world"), MakeLevelWorldSummary(*World));
	}
	FString Message = FString::Printf(TEXT("%s %s."), *Verb, *Package);
	if (!Saved.IsEmpty())
	{
		Message += FString::Printf(TEXT(" Saved %d package(s) of the previous level first."), Saved.Num());
	}
	Message += TEXT(" The Undo history was cleared (as on every map change in the editor).");
	return FLevelToolResult::Success(Message, Payload);
}

void DeactivateEditorModes()
{
	if (ULevelEditorSubsystem* LevelEditor = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
	{
		if (FEditorModeTools* Modes = LevelEditor->GetLevelEditorModeManager())
		{
			Modes->DeactivateAllModes();
		}
	}
}

// The New Level dialog's choices: the two empty levels, then the project's template maps by display name
// ("Basic" -> "basic", "Open World" -> "open_world").
struct FNewLevelTemplate
{
	FString Name;
	FString MapPackage; // empty: an empty level
	bool bPartitioned = false;
};

TArray<FNewLevelTemplate> NewLevelTemplates()
{
	TArray<FNewLevelTemplate> Templates = {{TEXT("empty"), FString(), false}, {TEXT("empty_open_world"), FString(), true}};
	if (GUnrealEd != nullptr)
	{
		for (const FTemplateMapInfo& Info : GUnrealEd->GetTemplateMapInfos())
		{
			const FString MapPackage = Info.Map.GetLongPackageName();
			FString Name = (Info.DisplayName.IsEmpty() ? FPackageName::GetShortName(MapPackage) : Info.DisplayName.ToString()).ToLower();
			Name.ReplaceCharInline(TEXT(' '), TEXT('_'));
			if (!MapPackage.IsEmpty())
			{
				Templates.Add({Name, MapPackage, false});
			}
		}
	}
	return Templates;
}

FLevelToolResult ParseBody(const FString& RequestBody, TSharedPtr<FJsonObject>& OutJson)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody.IsEmpty() ? FString(TEXT("{}")) : RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, OutJson) || !OutJson.IsValid())
	{
		return FLevelToolResult::Failure({MakeInvalidRequest(TEXT(""), TEXT("The request body must be a JSON object."))});
	}
	return FLevelToolResult::Success(FString(), nullptr);
}
}

FLevelToolResult RunOpenLevel(const TSharedPtr<FJsonObject>& RequestJson)
{
	check(IsInGameThread());
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	FMapRequest Request;
	if (!CheckLevelToolAdmission(Diagnostics) || !ParseMapRequest(RequestJson, false, Request, Diagnostics) || !CheckNotBusy(Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	UWorld* World = GetEditorWorld();
	if (World->GetPackage()->GetName() == Request.Package)
	{
		// Nothing is reloaded, so nothing would be lost; save_first still saves the open level's unsaved changes, as
		// asked.
		TArray<FString> Saved;
		if (Request.bSaveFirst && !HandleUnsavedChanges(*World, true, Saved, Diagnostics))
		{
			return FLevelToolResult::Failure(MoveTemp(Diagnostics));
		}
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("level"), Request.Package);
		Payload->SetBoolField(TEXT("already_open"), true);
		Payload->SetArrayField(TEXT("saved_before"), ToJsonStrings(Saved));
		Payload->SetObjectField(TEXT("world"), MakeLevelWorldSummary(*World));
		FString Message = FString::Printf(TEXT("%s is already open; nothing was reloaded."), *Request.Package);
		if (!Saved.IsEmpty())
		{
			Message += FString::Printf(TEXT(" Saved %d package(s) first."), Saved.Num());
		}
		return FLevelToolResult::Success(Message, Payload);
	}
	if (!MapExists(Request.Package))
	{
		return FLevelToolResult::Failure({MapNotFound(Request.Package)});
	}
	TArray<FString> Saved;
	if (!HandleUnsavedChanges(*World, Request.bSaveFirst, Saved, Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	bool bLoaded = false;
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		const FString Filename = FPackageName::LongPackageNameToFilename(Request.Package, FPackageName::GetMapPackageExtension());
		bLoaded = FEditorFileUtils::LoadMap(Filename, /*LoadAsTemplate*/ false, /*bShowProgress*/ true);
	}
	UWorld* Opened = GetEditorWorld();
	if (!bLoaded || Opened == nullptr || Opened->GetPackage()->GetName() != Request.Package)
	{
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT("/path"), TEXT("Level.OpenFailed"), FString::Printf(TEXT("The editor could "
			"not open %s (see the editor's Message Log); the open level is now %s."), *Request.Package,
			Opened != nullptr ? *Opened->GetPackage()->GetName() : TEXT("none")))});
	}
	return MapChanged(TEXT("Opened"), Request.Package, FString(), Saved);
}

FLevelToolResult RunNewLevel(const TSharedPtr<FJsonObject>& RequestJson)
{
	check(IsInGameThread());
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	FMapRequest Request;
	if (!CheckLevelToolAdmission(Diagnostics) || !ParseMapRequest(RequestJson, true, Request, Diagnostics) || !CheckNotBusy(Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	const TArray<FNewLevelTemplate> Templates = NewLevelTemplates();
	const FNewLevelTemplate* Template = Templates.FindByPredicate([&Request](const FNewLevelTemplate& Candidate)
	{
		return Candidate.Name == Request.Template.ToLower()
			|| (!Candidate.MapPackage.IsEmpty() && Candidate.MapPackage == FPackageName::ObjectPathToPackageName(Request.Template));
	});
	if (Template == nullptr)
	{
		TArray<FString> Names;
		for (const FNewLevelTemplate& Candidate : Templates)
		{
			Names.Add(Candidate.Name);
		}
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("templates"), ToJsonStrings(Names));
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT("/template"), TEXT("Level.TemplateNotFound"), FString::Printf(TEXT("There is "
			"no New Level template '%s'. Templates: %s."), *Request.Template, *FString::Join(Names, TEXT(", "))), Details)});
	}
	if (MapExists(Request.Package) || FindPackage(nullptr, *Request.Package) != nullptr)
	{
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT("/path"), TEXT("Level.MapExists"), FString::Printf(TEXT("%s already exists; "
			"open it with open_level or pick another path."), *Request.Package))});
	}
	UWorld* World = GetEditorWorld();
	TArray<FString> Saved;
	if (!HandleUnsavedChanges(*World, Request.bSaveFirst, Saved, Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}

	// As the New Level dialog: an empty level (NewMap), or the template map loaded as an untitled copy; then saved at path.
	bool bCreated = false;
	bool bSaved = false;
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		DeactivateEditorModes();
		if (Template->MapPackage.IsEmpty())
		{
			bCreated = GEditor->NewMap(Template->bPartitioned) != nullptr;
		}
		else
		{
			const FString Filename = FPackageName::LongPackageNameToFilename(Template->MapPackage, FPackageName::GetMapPackageExtension());
			bCreated = FEditorFileUtils::LoadMap(Filename, /*LoadAsTemplate*/ true, /*bShowProgress*/ true);
		}
		UWorld* Created = bCreated ? GetEditorWorld() : nullptr;
		bSaved = Created != nullptr && UEditorLoadingAndSavingUtils::SaveMap(Created, Request.Package);
	}
	UWorld* Opened = GetEditorWorld();
	if (!bCreated || !bSaved || Opened == nullptr || Opened->GetPackage()->GetName() != Request.Package)
	{
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT("/path"), TEXT("Level.CreateFailed"), FString::Printf(TEXT("The editor "
			"could not %s (see the editor's Message Log); the open level is now %s."), bCreated
				? *FString::Printf(TEXT("save the new level at %s"), *Request.Package)
				: *FString::Printf(TEXT("create a level from template '%s'"), *Template->Name),
			Opened != nullptr ? *Opened->GetPackage()->GetName() : TEXT("none")))});
	}
	return MapChanged(TEXT("Created and opened"), Request.Package, Template->Name, Saved);
}

FLevelToolResult RunOpenLevelFromBody(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Json;
	FLevelToolResult Parsed = ParseBody(RequestBody, Json);
	return Parsed.bSuccess ? RunOpenLevel(Json) : Parsed;
}

FLevelToolResult RunNewLevelFromBody(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Json;
	FLevelToolResult Parsed = ParseBody(RequestBody, Json);
	return Parsed.bSuccess ? RunNewLevel(Json) : Parsed;
}
}
