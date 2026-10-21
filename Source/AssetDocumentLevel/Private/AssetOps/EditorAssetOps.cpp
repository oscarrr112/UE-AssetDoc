// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetOps/EditorAssetOps.h"

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetExportTask.h"
#include "AssetDocumentAssetCompilation.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "Exporters/Exporter.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "LevelEditTypes.h"
#include "LevelQuery.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundWave.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

namespace UE::AssetDocumentLevel::Private
{
namespace AssetOpsTool
{
// One entry of "assets": what the client wrote, the package and the asset object path.
struct FAssetTarget
{
	FString Input;
	FString Package;
	FString ObjectPath;
	FString Pointer;
};

TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Json;
	for (const FString& Value : Values)
	{
		Json.Add(MakeShared<FJsonValueString>(Value));
	}
	return Json;
}

// A refused asset: a payload row {asset, reason[, formats]} and its diagnostic.
// A MetaSound Source / Patch (MetasoundEngine's classes, by name: this module does not depend on MetaSound).
bool IsMetaSoundForAssetEditor(const UObject& Asset)
{
	for (const UClass* Class = Asset.GetClass(); Class != nullptr; Class = Class->GetSuperClass())
	{
		if (Class->GetOutermost()->GetName() == TEXT("/Script/MetasoundEngine")
			&& (Class->GetName() == TEXT("MetaSoundSource") || Class->GetName() == TEXT("MetaSoundPatch")))
		{
			return true;
		}
	}
	return false;
}

struct FRefusals
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	TArray<FAssetDocumentDiagnostic> Diagnostics;

	void Add(const FAssetTarget& Target, const TCHAR* Code, const FString& Reason, const TArray<FString>* Formats = nullptr)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset"), Target.Input);
		Row->SetStringField(TEXT("reason"), Reason);
		TSharedPtr<FJsonObject> Details;
		if (Formats != nullptr)
		{
			Row->SetArrayField(TEXT("formats"), StringValues(*Formats));
			Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("formats"), StringValues(*Formats));
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		Diagnostics.Add(MakeLevelDiagnostic(Target.Pointer, Code, FString::Printf(TEXT("%s: %s"), *Target.Input, *Reason), Details));
	}
};

// 200 when everything was done, 207 when some assets were refused, 400 when nothing was done.
FLevelToolResult Finish(int32 Done, FRefusals& Refusals, const TSharedRef<FJsonObject>& Payload, const FString& Message)
{
	Payload->SetArrayField(TEXT("refused"), Refusals.Rows);
	if (Refusals.Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Success(Message, Payload);
	}
	if (Done == 0)
	{
		return FLevelToolResult::Failure(MoveTemp(Refusals.Diagnostics), Payload);
	}
	FLevelToolResult Result = FLevelToolResult::Success(Refusals.Rows.IsEmpty() ? Message
		: FString::Printf(TEXT("%s %d asset(s) were refused (payload.refused)."), *Message, Refusals.Rows.Num()), Payload);
	Result.HttpStatus = 207;
	Result.bSuccess = false;
	Result.Diagnostics = FAssetDocumentResult::OrderRootCauseFirst(Refusals.Diagnostics);
	return Result;
}

bool CheckFields(const FJsonObject& Json, TConstArrayView<const TCHAR*> Allowed, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	for (const auto& Field : Json.Values)
	{
		if (!Allowed.ContainsByPredicate([&Field](const TCHAR* Name) { return Field.Key == Name; }))
		{
			TArray<FString> Names;
			for (const TCHAR* Name : Allowed)
			{
				Names.Add(Name);
			}
			Diagnostics.Add(MakeInvalidRequest(TEXT("/") + FString(*Field.Key), FString::Printf(TEXT("Unknown field '%s' (fields: %s)."), *Field.Key,
				*FString::Join(Names, TEXT(", ")))));
		}
	}
	return Diagnostics.IsEmpty();
}

bool ReadBool(const FJsonObject& Json, const TCHAR* Field, bool& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	const TSharedPtr<FJsonValue> Value = Json.TryGetField(Field);
	if (!Value.IsValid())
	{
		return true;
	}
	if (Value->Type != EJson::Boolean)
	{
		Diagnostics.Add(MakeInvalidRequest(FString(TEXT("/")) + Field, FString::Printf(TEXT("'%s' must be a boolean."), Field)));
		return false;
	}
	Out = Value->AsBool();
	return true;
}

bool ReadString(const FJsonObject& Json, const TCHAR* Field, FString& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	const TSharedPtr<FJsonValue> Value = Json.TryGetField(Field);
	if (!Value.IsValid())
	{
		return true;
	}
	if (Value->Type != EJson::String)
	{
		Diagnostics.Add(MakeInvalidRequest(FString(TEXT("/")) + Field, FString::Printf(TEXT("'%s' must be a string."), Field)));
		return false;
	}
	Out = Value->AsString().TrimStartAndEnd();
	return true;
}

// "assets": package or object paths. The object path of a package path is the asset the registry
// lists in it (else <Package>.<Name>).
bool ReadAssets(const FJsonObject& Json, bool bRequired, TArray<FAssetTarget>& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Json.HasField(TEXT("assets")))
	{
		if (bRequired)
		{
			Diagnostics.Add(MakeInvalidRequest(TEXT("/assets"), TEXT("'assets' lists the assets by package or object path, like "
				"[\"/Game/Audio/Beep\"].")));
		}
		return !bRequired;
	}
	if (!Json.TryGetArrayField(TEXT("assets"), Values) || Values->IsEmpty())
	{
		Diagnostics.Add(MakeInvalidRequest(TEXT("/assets"), TEXT("'assets' must be a non-empty array of asset paths.")));
		return false;
	}
	for (int32 Index = 0; Index < Values->Num(); ++Index)
	{
		const FString Pointer = FString::Printf(TEXT("/assets/%d"), Index);
		FString Input;
		if (!(*Values)[Index]->TryGetString(Input) || Input.TrimStartAndEnd().IsEmpty())
		{
			Diagnostics.Add(MakeInvalidRequest(Pointer, TEXT("Each asset is a package or object path string.")));
			continue;
		}
		Input.TrimStartAndEndInline();
		FAssetTarget Target;
		Target.Input = Input;
		Target.Pointer = Pointer;
		int32 Dot = INDEX_NONE;
		Target.Package = Input.FindChar(TEXT('.'), Dot) ? Input.Left(Dot) : Input;
		FText Reason;
		if (!FPackageName::IsValidLongPackageName(Target.Package, /*bIncludeReadOnlyRoots*/ true, &Reason))
		{
			Diagnostics.Add(MakeInvalidRequest(Pointer, FString::Printf(TEXT("'%s' is not an asset path (%s); use one like /Game/Audio/Beep."),
				*Input, *Reason.ToString())));
			continue;
		}
		if (Dot != INDEX_NONE)
		{
			Target.ObjectPath = Input;
		}
		else
		{
			TArray<FAssetData> Assets;
			IAssetRegistry::GetChecked().GetAssetsByPackageName(FName(*Target.Package), Assets, /*bIncludeOnlyOnDiskAssets*/ false);
			Target.ObjectPath = Assets.IsEmpty()
				? Target.Package + TEXT(".") + FPackageName::GetShortName(Target.Package)
				: Assets[0].GetObjectPathString();
		}
		Out.Add(MoveTemp(Target));
	}
	return Diagnostics.IsEmpty();
}

bool OnDisk(const FString& Package)
{
	return FPackageName::DoesPackageExist(Package);
}

bool CheckIdle(TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	if (!CheckLevelToolAdmission(Diagnostics))
	{
		return false;
	}
	if (GIsTransacting || GEditor->IsTransactionActive() || UE::IsSavingPackage(nullptr)
		|| (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveModalWindow().IsValid()))
	{
		Diagnostics.Add(MakeLevelDiagnostic(TEXT(""), TEXT("Level.EditorBusy"), TEXT("The editor is busy (a transaction, an Undo, a save or a "
			"modal dialog is in progress). Nothing was changed; retry shortly.")));
		return false;
	}
	return true;
}

// The packages of the map open in the editor: its world, streaming levels and their actor / object packages.
TSet<FString> OpenMapPackages()
{
	TSet<FString> Packages;
	const UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (World == nullptr)
	{
		return Packages;
	}
	for (const ULevel* Level : World->GetLevels())
	{
		if (Level == nullptr)
		{
			continue;
		}
		Packages.Add(Level->GetPackage()->GetName());
		for (const UPackage* External : Level->GetPackage()->GetExternalPackages())
		{
			Packages.Add(External->GetName());
		}
	}
	return Packages;
}

UObject* LoadTargetAsset(const FAssetTarget& Target)
{
	UObject* Object = FindObject<UObject>(nullptr, *Target.ObjectPath);
	return Object != nullptr ? Object : LoadObject<UObject>(nullptr, *Target.ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
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

FLevelToolResult InvalidJson()
{
	return FLevelToolResult::Failure({MakeInvalidRequest(TEXT(""), TEXT("The request must be a JSON object."))});
}

// ---- save_assets ------------------------------------------------------------------------------

// Saves Packages as the editor's Save (source control checkout, no prompt); returns those that did not save.
TArray<UPackage*> SavePackagesAsEditor(const TArray<UPackage*>& Packages)
{
	TArray<UPackage*> Failed;
	if (Packages.IsEmpty())
	{
		return Failed;
	}
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
		FEditorFileUtils::PromptForCheckoutAndSave(Packages, /*bCheckDirty*/ false, /*bPromptToSave*/ false, &Failed,
			/*bAlreadyCheckedOut*/ false, /*bCanBeDeclined*/ false);
	}
	for (UPackage* Package : Packages)
	{
		if (!Failed.Contains(Package) && (Package->IsDirty() || !OnDisk(Package->GetName())))
		{
			Failed.Add(Package);
		}
	}
	return Failed;
}

bool IsUntitled(const UPackage& Package)
{
	return Package.GetName().StartsWith(TEXT("/Temp/"));
}
}

FLevelToolResult RunSaveAssets(const TSharedPtr<FJsonObject>& RequestJson)
{
	using namespace AssetOpsTool;
	check(IsInGameThread());
	if (!RequestJson.IsValid())
	{
		return InvalidJson();
	}
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	TArray<FAssetTarget> Targets;
	bool bForce = false;
	bool bAllDirty = false;
	FString Only = TEXT("all");
	if (!CheckIdle(Diagnostics) || !CheckFields(*RequestJson, {TEXT("assets"), TEXT("force"), TEXT("all_dirty"), TEXT("only")}, Diagnostics)
		|| !ReadBool(*RequestJson, TEXT("force"), bForce, Diagnostics) || !ReadBool(*RequestJson, TEXT("all_dirty"), bAllDirty, Diagnostics)
		|| !ReadString(*RequestJson, TEXT("only"), Only, Diagnostics) || !ReadAssets(*RequestJson, false, Targets, Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (bAllDirty == !Targets.IsEmpty())
	{
		return FLevelToolResult::Failure({MakeInvalidRequest(TEXT(""), TEXT("Pass either \"assets\" (the assets to save) or \"all_dirty\": true "
			"(every package with unsaved changes)."))});
	}
	if (!bAllDirty && RequestJson->HasField(TEXT("only")))
	{
		return FLevelToolResult::Failure({MakeInvalidRequest(TEXT("/only"), TEXT("'only' filters \"all_dirty\"."))});
	}
	if (bAllDirty && bForce)
	{
		return FLevelToolResult::Failure({MakeInvalidRequest(TEXT("/force"), TEXT("'force' re-saves the listed \"assets\"; \"all_dirty\" saves "
			"what has unsaved changes."))});
	}
	if (Only != TEXT("all") && Only != TEXT("maps") && Only != TEXT("content"))
	{
		return FLevelToolResult::Failure({MakeInvalidRequest(TEXT("/only"), TEXT("'only' is \"all\", \"maps\" or \"content\"."))});
	}

	TArray<FString> Clean;
	TArray<TSharedPtr<FJsonValue>> NotSaved;
	FRefusals Refusals;
	TArray<UPackage*> ToSave;
	TMap<UPackage*, FAssetTarget> TargetOf;
	if (bAllDirty)
	{
		TArray<UPackage*> Dirty;
		if (Only != TEXT("content"))
		{
			FEditorFileUtils::GetDirtyWorldPackages(Dirty);
		}
		if (Only != TEXT("maps"))
		{
			FEditorFileUtils::GetDirtyContentPackages(Dirty);
		}
		for (UPackage* Package : Dirty)
		{
			if (IsUntitled(*Package))
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("package"), Package->GetName());
				Row->SetStringField(TEXT("reason"), TEXT("a never-saved (Untitled) map has no file name; save it from the editor "
					"(File > Save Current Level As)"));
				NotSaved.Add(MakeShared<FJsonValueObject>(Row));
			}
			else
			{
				ToSave.AddUnique(Package);
			}
		}
	}
	else
	{
		for (const FAssetTarget& Target : Targets)
		{
			UPackage* Package = FindPackage(nullptr, *Target.Package);
			if (Package == nullptr && !OnDisk(Target.Package))
			{
				Refusals.Add(Target, TEXT("Assets.NotFound"), TEXT("no such asset (find_assets lists assets by path and class)"));
				continue;
			}
			if (Package == nullptr && bForce)
			{
				Package = LoadPackage(nullptr, *Target.Package, LOAD_None);
			}
			if (Package == nullptr || (!Package->IsDirty() && !bForce))
			{
				// Not loaded, or loaded without changes: the file on disk is current.
				Clean.AddUnique(Target.Package);
				continue;
			}
			if (IsUntitled(*Package))
			{
				Refusals.Add(Target, TEXT("Assets.SaveFailed"), TEXT("a never-saved (Untitled) map has no file name; save it from the editor"));
				continue;
			}
			if (!ToSave.Contains(Package))
			{
				ToSave.Add(Package);
				TargetOf.Add(Package, Target);
			}
		}
	}

	const TArray<UPackage*> Failed = SavePackagesAsEditor(ToSave);
	TArray<FString> Saved;
	for (UPackage* Package : ToSave)
	{
		if (!Failed.Contains(Package))
		{
			Saved.Add(Package->GetName());
			continue;
		}
		const FString Reason = TEXT("the editor did not save it (read-only file, source control or a save error; see the editor log)");
		if (const FAssetTarget* Target = TargetOf.Find(Package))
		{
			Refusals.Add(*Target, TEXT("Assets.SaveFailed"), Reason);
		}
		else
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("package"), Package->GetName());
			Row->SetStringField(TEXT("reason"), Reason);
			NotSaved.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("saved"), StringValues(Saved));
	Payload->SetArrayField(TEXT("not_saved"), NotSaved);
	Payload->SetArrayField(TEXT("clean"), StringValues(Clean));
	FString Message = FString::Printf(TEXT("Saved %d package(s)."), Saved.Num());
	if (!Clean.IsEmpty())
	{
		Message += FString::Printf(TEXT(" %d had no unsaved changes (payload.clean; \"force\": true re-saves them)."), Clean.Num());
	}
	if (!NotSaved.IsEmpty())
	{
		Message += FString::Printf(TEXT(" %d package(s) with unsaved changes were not saved (payload.not_saved)."), NotSaved.Num());
		Refusals.Diagnostics.Add(MakeLevelDiagnostic(TEXT("/all_dirty"), TEXT("Assets.SaveFailed"), FString::Printf(
			TEXT("%d package(s) with unsaved changes were not saved (payload.not_saved says why)."), NotSaved.Num())));
	}
	return Finish(Saved.Num() + Clean.Num(), Refusals, Payload, Message);
}

// ---- reload_assets ----------------------------------------------------------------------------

FLevelToolResult RunReloadAssets(const TSharedPtr<FJsonObject>& RequestJson)
{
	using namespace AssetOpsTool;
	check(IsInGameThread());
	if (!RequestJson.IsValid())
	{
		return InvalidJson();
	}
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	TArray<FAssetTarget> Targets;
	bool bForce = false;
	if (!CheckIdle(Diagnostics) || !CheckFields(*RequestJson, {TEXT("assets"), TEXT("force")}, Diagnostics)
		|| !ReadBool(*RequestJson, TEXT("force"), bForce, Diagnostics) || !ReadAssets(*RequestJson, true, Targets, Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	const TSet<FString> OpenMap = OpenMapPackages();
	FRefusals Refusals;
	TArray<FString> NotLoaded;
	TArray<UPackage*> ToReload;
	TArray<FString> Discarded;
	for (const FAssetTarget& Target : Targets)
	{
		UPackage* Package = FindPackage(nullptr, *Target.Package);
		if (!OnDisk(Target.Package))
		{
			Refusals.Add(Target, Package != nullptr ? TEXT("Assets.NeverSaved") : TEXT("Assets.NotFound"), Package != nullptr
				? TEXT("it was never saved, so there is nothing on disk to reload")
				: TEXT("no such asset (find_assets lists assets by path and class)"));
			continue;
		}
		if (Package == nullptr)
		{
			// Not in memory: the next load reads the file.
			NotLoaded.AddUnique(Target.Package);
			continue;
		}
		if (OpenMap.Contains(Package->GetName()))
		{
			Refusals.Add(Target, TEXT("Assets.OpenMap"), TEXT("it belongs to the map open in the editor; reopen the map with open_level instead"));
			continue;
		}
		if (Package->IsDirty() && !bForce)
		{
			Refusals.Add(Target, TEXT("Assets.UnsavedChanges"), TEXT("it has unsaved changes that a reload would discard; save it "
				"(save_assets) or pass \"force\": true to discard them"));
			continue;
		}
		if (Package->IsDirty())
		{
			Discarded.Add(Package->GetName());
		}
		ToReload.AddUnique(Package);
	}
	TArray<FString> Reloaded;
	if (!ToReload.IsEmpty())
	{
		TArray<FString> Names;
		for (UPackage* Package : ToReload)
		{
			Names.Add(Package->GetName());
		}
		FText Error;
		{
			FAssetDocumentEditorNonInteractiveScope NonInteractive;
			TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
			AssetDocumentAssetCompilation::FinishPackageCompilation(ToReload);
			UPackageTools::ReloadPackages(ToReload, Error, EReloadPackagesInteractionMode::AssumePositive);
		}
		for (const FString& Name : Names)
		{
			const UPackage* After = FindPackage(nullptr, *Name);
			if (After != nullptr && !After->IsDirty())
			{
				Reloaded.Add(Name);
			}
			else
			{
				const FAssetTarget* Target = Targets.FindByPredicate([&Name](const FAssetTarget& Candidate) { return Candidate.Package == Name; });
				Refusals.Add(*Target, TEXT("Assets.ReloadFailed"), Error.IsEmpty()
					? FString(TEXT("the editor did not reload it (see the editor log)")) : Error.ToString());
			}
		}
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("reloaded"), StringValues(Reloaded));
	Payload->SetArrayField(TEXT("not_loaded"), StringValues(NotLoaded));
	if (!Discarded.IsEmpty())
	{
		Payload->SetArrayField(TEXT("discarded_changes"), StringValues(Discarded));
	}
	FString Message = FString::Printf(TEXT("Reloaded %d package(s) from disk."), Reloaded.Num());
	if (!NotLoaded.IsEmpty())
	{
		Message += FString::Printf(TEXT(" %d were not loaded (the next load reads the file; payload.not_loaded)."), NotLoaded.Num());
	}
	return Finish(Reloaded.Num() + NotLoaded.Num(), Refusals, Payload, Message);
}

// ---- asset_editor -----------------------------------------------------------------------------

namespace AssetOpsTool
{
// Why the editor of Asset is still open after a close: the pending edits its handler knows of (with the ways out), or
// the editor's own refusal.
FString DescribeEditorNotClosed(const UObject& Asset, const FString& PendingButton)
{
	if (!PendingButton.IsEmpty())
	{
		return FString::Printf(TEXT("its editor holds changes that are not applied to the asset yet (its %s button is enabled), and "
			"closing it asks whether to apply them. %s"), *PendingButton, *UE::AssetDocument::OpenEditor::DescribePendingEditsWayOut(Asset));
	}
	return TEXT("the editor did not close (it asked a question an MCP request cannot answer, or refused; see the editor log)");
}

// An asset of the user's, not an editor's working copy (the Material editor's PreviewMaterial_N, a transient object in
// /Engine/Transient).
bool IsUserAsset(const UObject& Object)
{
	const UPackage* Package = Object.GetPackage();
	return Package != nullptr && Package != GetTransientPackage() && !Object.HasAnyFlags(RF_Transient)
		&& !Package->HasAnyFlags(RF_Transient) && !Package->GetName().StartsWith(TEXT("/Engine/Transient"));
}

bool IsEditorOpen(UAssetEditorSubsystem& Editors, UObject& Asset)
{
	return Editors.FindEditorForAsset(&Asset, /*bFocusIfOpen*/ false) != nullptr;
}
} // namespace AssetOpsTool

FLevelToolResult RunAssetEditor(const TSharedPtr<FJsonObject>& RequestJson)
{
	using namespace AssetOpsTool;
	check(IsInGameThread());
	if (!RequestJson.IsValid())
	{
		return InvalidJson();
	}
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	TArray<FAssetTarget> Targets;
	FString Action;
	bool bDiscardUnapplied = false;
	if (!CheckIdle(Diagnostics) || !CheckFields(*RequestJson, {TEXT("action"), TEXT("assets"), TEXT("discard_unapplied")}, Diagnostics)
		|| !ReadString(*RequestJson, TEXT("action"), Action, Diagnostics)
		|| !ReadBool(*RequestJson, TEXT("discard_unapplied"), bDiscardUnapplied, Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (Action != TEXT("open") && Action != TEXT("close") && Action != TEXT("list") && Action != TEXT("apply_editor"))
	{
		return FLevelToolResult::Failure({MakeInvalidRequest(TEXT("/action"), TEXT("'action' is \"open\", \"close\", \"apply_editor\" or \"list\"."))});
	}
	if (bDiscardUnapplied && Action != TEXT("close"))
	{
		return FLevelToolResult::Failure({MakeInvalidRequest(TEXT("/discard_unapplied"), TEXT("'discard_unapplied' goes with \"close\"."))});
	}
	if (!ReadAssets(*RequestJson, Action != TEXT("list"), Targets, Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (Editors == nullptr)
	{
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT(""), TEXT("Level.EditorBusy"), TEXT("The asset editor subsystem is not available."))});
	}
	FRefusals Refusals;
	TArray<FString> Done;
	TArray<FString> NotOpen;
	TArray<FString> Discarded;
	for (const FAssetTarget& Target : Targets)
	{
		if (Action == TEXT("open"))
		{
			UObject* Asset = LoadTargetAsset(Target);
			if (Asset == nullptr)
			{
				Refusals.Add(Target, TEXT("Assets.NotFound"), TEXT("no such asset (find_assets lists assets by path and class)"));
				continue;
			}
			if (!FApp::CanEverRenderAudio() && IsMetaSoundForAssetEditor(*Asset))
			{
				// KI-16 (engine): without an audio device the MetaSound editor's node palette registers the MetaSounds
				// that hold Harmonix Fusion patches, and Harmonix asserts (KeyzoneSettings.cpp:48): the editor would crash.
				Refusals.Add(Target, TEXT("Assets.EditorNeedsAudio"), TEXT("the MetaSound editor cannot open in an editor without an "
					"audio device (-NoSound): the engine crashes building its node palette. Documents still work "
					"(extract_asset_document / apply_asset_document_file); restart the editor with audio to open it"));
				continue;
			}
			bool bOpened = false;
			{
				FAssetDocumentEditorNonInteractiveScope NonInteractive;
				bOpened = Editors->OpenEditorForAsset(Asset);
			}
			if (bOpened)
			{
				Done.Add(Asset->GetPathName());
			}
			else
			{
				Refusals.Add(Target, TEXT("Assets.EditorNotOpened"), TEXT("the editor opened no asset editor for it (the asset type has none, "
					"or the editor refused; see the editor log)"));
			}
			continue;
		}
		UObject* Asset = FindObject<UObject>(nullptr, *Target.ObjectPath);
		if (Asset == nullptr || !IsEditorOpen(*Editors, *Asset))
		{
			if (Asset == nullptr && !OnDisk(Target.Package))
			{
				Refusals.Add(Target, TEXT("Assets.NotFound"), TEXT("no such asset (find_assets lists assets by path and class)"));
			}
			else
			{
				NotOpen.Add(Target.ObjectPath);
			}
			continue;
		}
		const FString PendingButton = UE::AssetDocument::OpenEditor::GetPendingEditsButton(*Asset);
		if (Action == TEXT("apply_editor"))
		{
			// The editor's own Apply: the pending edits are written to the asset, whose package then has unsaved changes.
			FString Reason;
			if (UE::AssetDocument::OpenEditor::ApplyPendingEdits(*Asset, Reason))
			{
				Done.Add(Asset->GetPathName());
			}
			else
			{
				Refusals.Add(Target, TEXT("Assets.NothingToApply"), Reason);
			}
			continue;
		}
		// close: an editor with pending edits asks before it closes, which an MCP request cannot answer; it closes only
		// with discard_unapplied (the prompt's "No").
		if (!PendingButton.IsEmpty() && bDiscardUnapplied)
		{
			FString Reason;
			if (UE::AssetDocument::OpenEditor::CloseDiscardingPendingEdits(*Asset, Reason))
			{
				Discarded.Add(Asset->GetPathName());
			}
			else
			{
				Refusals.Add(Target, TEXT("Assets.EditorNotClosed"), FString::Printf(TEXT("%s. %s"), *Reason,
					*UE::AssetDocument::OpenEditor::DescribePendingEditsWayOut(*Asset)));
				continue;
			}
		}
		else if (PendingButton.IsEmpty())
		{
			Editors->CloseAllEditorsForAsset(Asset);
		}
		if (IsEditorOpen(*Editors, *Asset))
		{
			Refusals.Add(Target, TEXT("Assets.EditorNotClosed"), DescribeEditorNotClosed(*Asset, PendingButton));
		}
		else
		{
			Done.Add(Asset->GetPathName());
		}
	}
	TArray<FString> Open;
	for (const UObject* Asset : Editors->GetAllEditedAssets())
	{
		if (Asset != nullptr && IsUserAsset(*Asset))
		{
			Open.AddUnique(Asset->GetPathName());
		}
	}
	Open.Sort();
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	FString Message;
	if (Action == TEXT("open"))
	{
		Payload->SetArrayField(TEXT("opened"), StringValues(Done));
		Message = FString::Printf(TEXT("Opened the editor of %d asset(s)."), Done.Num());
	}
	else if (Action == TEXT("close"))
	{
		Payload->SetArrayField(TEXT("closed"), StringValues(Done));
		Payload->SetArrayField(TEXT("not_open"), StringValues(NotOpen));
		if (!Discarded.IsEmpty())
		{
			Payload->SetArrayField(TEXT("discarded_unapplied"), StringValues(Discarded));
		}
		Message = FString::Printf(TEXT("Closed the editors of %d asset(s)%s%s."), Done.Num(),
			Discarded.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", %d without their unapplied edits"), Discarded.Num()),
			NotOpen.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; %d had none open"), NotOpen.Num()));
	}
	else if (Action == TEXT("apply_editor"))
	{
		Payload->SetArrayField(TEXT("applied"), StringValues(Done));
		Payload->SetArrayField(TEXT("not_open"), StringValues(NotOpen));
		Message = FString::Printf(TEXT("Applied the pending edits of %d editor(s) to their assets, which now have unsaved changes "
			"(save_assets saves them; extract them again before applying a document)%s."), Done.Num(),
			NotOpen.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; %d had no editor open"), NotOpen.Num()));
	}
	else
	{
		Message = FString::Printf(TEXT("%d asset(s) have an editor open."), Open.Num());
	}
	Payload->SetArrayField(TEXT("open"), StringValues(Open));
	return Finish(Done.Num() + NotOpen.Num() + (Action == TEXT("list") ? 1 : 0), Refusals, Payload, Message);
}

// ---- export_assets ----------------------------------------------------------------------------

namespace AssetOpsTool
{
// The formats the editor's exporters offer for Object (lower case).
TArray<FString> ExportFormats(UObject& Object)
{
	TArray<FString> Formats;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(UExporter::StaticClass()) || It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}
		const UExporter* Exporter = It->GetDefaultObject<UExporter>();
		if (Exporter == nullptr || !Exporter->SupportsObject(&Object))
		{
			continue;
		}
		for (const FString& Extension : Exporter->FormatExtension)
		{
			Formats.AddUnique(Extension.ToLower());
		}
	}
	Formats.Sort();
	return Formats;
}

FString DefaultExportFormat(const UObject& Object)
{
	if (Object.IsA<USoundWave>())
	{
		return TEXT("wav");
	}
	if (Object.IsA<UTexture>())
	{
		return TEXT("png");
	}
	return FString();
}

// The export folder: inside the project directory (relative paths are relative to it).
bool ResolveExportFolder(const FString& Requested, FString& OutFolder, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	const FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString Folder = Requested.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("Exports")
		: FPaths::IsRelative(Requested) ? Project / Requested : Requested;
	Folder = FPaths::ConvertRelativePathToFull(Folder);
	FPaths::NormalizeDirectoryName(Folder);
	FPaths::CollapseRelativeDirectories(Folder);
	FString ProjectRoot = Project;
	FPaths::NormalizeDirectoryName(ProjectRoot);
	if (!FPaths::IsUnderDirectory(Folder, ProjectRoot) || Folder.Contains(TEXT("..")))
	{
		Diagnostics.Add(MakeInvalidRequest(TEXT("/folder"), FString::Printf(TEXT("'folder' must be inside the project (%s), like "
			"\"Saved/Exports/Audio\"; '%s' is not."), *ProjectRoot, *Requested)));
		return false;
	}
	OutFolder = Folder;
	return true;
}
}

FLevelToolResult RunExportAssets(const TSharedPtr<FJsonObject>& RequestJson)
{
	using namespace AssetOpsTool;
	check(IsInGameThread());
	if (!RequestJson.IsValid())
	{
		return InvalidJson();
	}
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	TArray<FAssetTarget> Targets;
	FString RequestedFolder;
	FString Format;
	bool bOverwrite = false;
	FString Folder;
	if (!CheckIdle(Diagnostics) || !CheckFields(*RequestJson, {TEXT("assets"), TEXT("folder"), TEXT("format"), TEXT("overwrite")}, Diagnostics)
		|| !ReadString(*RequestJson, TEXT("folder"), RequestedFolder, Diagnostics) || !ReadString(*RequestJson, TEXT("format"), Format, Diagnostics)
		|| !ReadBool(*RequestJson, TEXT("overwrite"), bOverwrite, Diagnostics) || !ReadAssets(*RequestJson, true, Targets, Diagnostics)
		|| !ResolveExportFolder(RequestedFolder, Folder, Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	Format = Format.ToLower();
	Format.RemoveFromStart(TEXT("."));

	FRefusals Refusals;
	TArray<TSharedPtr<FJsonValue>> Exported;
	TSet<FString> Files;
	for (const FAssetTarget& Target : Targets)
	{
		UObject* Asset = LoadTargetAsset(Target);
		if (Asset == nullptr)
		{
			Refusals.Add(Target, TEXT("Assets.NotFound"), TEXT("no such asset (find_assets lists assets by path and class)"));
			continue;
		}
		const FString AssetFormat = Format.IsEmpty() ? DefaultExportFormat(*Asset) : Format;
		UExporter* Exporter = AssetFormat.IsEmpty() ? nullptr : UExporter::FindExporter(Asset, *AssetFormat);
		if (Exporter == nullptr)
		{
			const TArray<FString> Formats = ExportFormats(*Asset);
			Refusals.Add(Target, TEXT("Assets.NoExporter"), Formats.IsEmpty()
				? FString::Printf(TEXT("the editor has no exporter for %s assets"), *Asset->GetClass()->GetName())
				: AssetFormat.IsEmpty()
				? FString::Printf(TEXT("%s assets have no default format; pass \"format\" (%s)"), *Asset->GetClass()->GetName(), *FString::Join(Formats, TEXT(", ")))
				: FString::Printf(TEXT("no %s exporter for %s assets (formats: %s)"), *AssetFormat, *Asset->GetClass()->GetName(),
					*FString::Join(Formats, TEXT(", "))), &Formats);
			continue;
		}
		const FString File = Folder / Asset->GetName() + TEXT(".") + AssetFormat;
		if (Files.Contains(File) || (!bOverwrite && IFileManager::Get().FileExists(*File)))
		{
			Refusals.Add(Target, TEXT("Assets.FileExists"), Files.Contains(File)
				? FString::Printf(TEXT("another asset of this call exports to %s; export it in its own call with another folder"), *File)
				: FString::Printf(TEXT("%s exists; pass \"overwrite\": true or another folder"), *File));
			continue;
		}
		IFileManager::Get().MakeDirectory(*Folder, /*Tree*/ true);
		TStrongObjectPtr<UAssetExportTask> Task(NewObject<UAssetExportTask>());
		Task->Object = Asset;
		Task->Exporter = Exporter;
		Task->Filename = File;
		Task->bSelected = false;
		Task->bReplaceIdentical = true;
		Task->bPrompt = false;
		Task->bAutomated = true;
		bool bExported = false;
		{
			FAssetDocumentEditorNonInteractiveScope NonInteractive;
			TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
			bExported = UExporter::RunAssetExportTask(Task.Get());
		}
		if (!bExported || !IFileManager::Get().FileExists(*File))
		{
			Refusals.Add(Target, TEXT("Assets.ExportFailed"), Task->Errors.IsEmpty()
				? FString::Printf(TEXT("the %s exporter wrote nothing (see the editor log)"), *Exporter->GetClass()->GetName())
				: FString::Join(Task->Errors, TEXT("; ")));
			continue;
		}
		Files.Add(File);
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset"), Asset->GetPathName());
		Row->SetStringField(TEXT("file"), File);
		Row->SetStringField(TEXT("exporter"), Exporter->GetClass()->GetName());
		Row->SetNumberField(TEXT("bytes"), static_cast<double>(IFileManager::Get().FileSize(*File)));
		Exported.Add(MakeShared<FJsonValueObject>(Row));
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("folder"), Folder);
	Payload->SetArrayField(TEXT("exported"), Exported);
	return Finish(Exported.Num(), Refusals, Payload, FString::Printf(TEXT("Exported %d asset(s) to %s."), Exported.Num(), *Folder));
}

FLevelToolResult RunSaveAssetsFromBody(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Json;
	FLevelToolResult Parsed = AssetOpsTool::ParseBody(RequestBody, Json);
	return Parsed.bSuccess ? RunSaveAssets(Json) : Parsed;
}

FLevelToolResult RunReloadAssetsFromBody(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Json;
	FLevelToolResult Parsed = AssetOpsTool::ParseBody(RequestBody, Json);
	return Parsed.bSuccess ? RunReloadAssets(Json) : Parsed;
}

FLevelToolResult RunAssetEditorFromBody(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Json;
	FLevelToolResult Parsed = AssetOpsTool::ParseBody(RequestBody, Json);
	return Parsed.bSuccess ? RunAssetEditor(Json) : Parsed;
}

FLevelToolResult RunExportAssetsFromBody(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Json;
	FLevelToolResult Parsed = AssetOpsTool::ParseBody(RequestBody, Json);
	return Parsed.bSuccess ? RunExportAssets(Json) : Parsed;
}
}
