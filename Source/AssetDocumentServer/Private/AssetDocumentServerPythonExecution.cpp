// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentServerPythonExecution.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "IPythonScriptPlugin.h"
#include "Misc/Base64.h"
#include "PythonScriptTypes.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"

namespace AssetDocumentServerPythonExecutionPrivate
{
constexpr int32 MaxListedObjects = 50;

// The whole input is one script path: optionally quoted, ending in .py, optionally followed by
// arguments for sys.argv. Anything else is code (0q: the engine's ExecuteFile mode runs the
// first "<something>.py" it finds as a file, even inside a comment of a multi-line script).
bool IsScriptPath(const FString& Code)
{
	const FString Line = Code.TrimStartAndEnd();
	if (Line.IsEmpty() || Line.Contains(TEXT("\n")) || Line.Contains(TEXT("\r")))
	{
		return false;
	}
	FString Path;
	if (Line.StartsWith(TEXT("\"")))
	{
		const int32 Close = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
		if (Close == INDEX_NONE || (Close + 1 < Line.Len() && !FChar::IsWhitespace(Line[Close + 1])))
		{
			return false;
		}
		Path = Line.Mid(1, Close - 1).TrimEnd();
	}
	else
	{
		// Characters of code, never of a script path a client passes.
		for (const TCHAR Character : {TEXT('#'), TEXT('='), TEXT(';'), TEXT('\''), TEXT('`')})
		{
			int32 Unused = INDEX_NONE;
			if (Line.FindChar(Character, Unused))
			{
				return false;
			}
		}
		if (Line.EndsWith(TEXT(".py"), ESearchCase::IgnoreCase))
		{
			Path = Line;
		}
		else
		{
			int32 Space = INDEX_NONE;
			for (int32 Index = 0; Index < Line.Len() && Space == INDEX_NONE; ++Index)
			{
				Space = FChar::IsWhitespace(Line[Index]) ? Index : INDEX_NONE;
			}
			Path = Space == INDEX_NONE ? Line : Line.Left(Space);
		}
	}
	return Path.Len() > 3 && Path.EndsWith(TEXT(".py"), ESearchCase::IgnoreCase);
}

// Code the engine would take for a file path (it mentions ".py") runs through exec() of its
// UTF-8 text, so the engine sees no file name; line numbers and the "<string>" file name of a
// traceback stay those of the code, and it runs in the same console globals as other code.
FString AsCodeCommand(const FString& Code)
{
	if (!Code.Contains(TEXT(".py"), ESearchCase::IgnoreCase) || IsScriptPath(Code))
	{
		return Code;
	}
	const FTCHARToUTF8 Utf8(*Code);
	const FString Encoded = FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), static_cast<uint32>(Utf8.Length()));
	return FString::Printf(
		TEXT("exec(compile(__import__('base64').b64decode('%s').decode('utf-8'), '<string>', 'exec'))"), *Encoded);
}

// Work a script does that the Undo history cannot take back (0r): assets created, renamed or
// deleted, packages saved, a map opened. Recorded while the script runs.
class FNonUndoableWatch
{
public:
	FNonUndoableWatch()
	{
		if (IAssetRegistry* Registry = IAssetRegistry::Get())
		{
			AddedHandle = Registry->OnAssetAdded().AddRaw(this, &FNonUndoableWatch::OnAdded);
			RemovedHandle = Registry->OnAssetRemoved().AddRaw(this, &FNonUndoableWatch::OnRemoved);
			RenamedHandle = Registry->OnAssetRenamed().AddRaw(this, &FNonUndoableWatch::OnRenamed);
		}
		SavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(this, &FNonUndoableWatch::OnSaved);
		MapHandle = FEditorDelegates::OnMapOpened.AddRaw(this, &FNonUndoableWatch::OnMapOpened);
	}

	~FNonUndoableWatch()
	{
		if (IAssetRegistry* Registry = IAssetRegistry::Get())
		{
			Registry->OnAssetAdded().Remove(AddedHandle);
			Registry->OnAssetRemoved().Remove(RemovedHandle);
			Registry->OnAssetRenamed().Remove(RenamedHandle);
		}
		UPackage::PackageSavedWithContextEvent.Remove(SavedHandle);
		FEditorDelegates::OnMapOpened.Remove(MapHandle);
	}

	FNonUndoableWatch(const FNonUndoableWatch&) = delete;
	FNonUndoableWatch& operator=(const FNonUndoableWatch&) = delete;

	TArray<FString> Operations;

private:
	void Add(FString Operation)
	{
		Operations.AddUnique(MoveTemp(Operation));
	}
	void OnAdded(const FAssetData& Asset) { Add(FString::Printf(TEXT("created %s"), *Asset.GetObjectPathString())); }
	void OnRemoved(const FAssetData& Asset) { Add(FString::Printf(TEXT("deleted %s"), *Asset.GetObjectPathString())); }
	void OnRenamed(const FAssetData& Asset, const FString& OldObjectPath)
	{
		Add(FString::Printf(TEXT("renamed %s to %s"), *OldObjectPath, *Asset.GetObjectPathString()));
	}
	void OnSaved(const FString& Filename, UPackage* Package, FObjectPostSaveContext Context)
	{
		if (Package != nullptr && !Context.IsProceduralSave())
		{
			Add(FString::Printf(TEXT("saved %s"), *Package->GetName()));
		}
	}
	void OnMapOpened(const FString& Filename, bool bAsTemplate) { Add(FString::Printf(TEXT("opened map %s"), *Filename)); }

	FDelegateHandle AddedHandle;
	FDelegateHandle RemovedHandle;
	FDelegateHandle RenamedHandle;
	FDelegateHandle SavedHandle;
	FDelegateHandle MapHandle;
};

// The objects an Undo restores, by path (actors with their label), at most MaxListedObjects.
TArray<FString> DescribeObjects(const TArray<UObject*>& Objects, int32& OutTotal)
{
	TArray<FString> Names;
	for (const UObject* Object : Objects)
	{
		if (Object == nullptr || Object->IsA<UPackage>())
		{
			continue;
		}
		const AActor* Actor = Cast<AActor>(Object);
		Names.AddUnique(Actor != nullptr
			? FString::Printf(TEXT("%s (actor '%s')"), *Object->GetPathName(), *Actor->GetActorLabel())
			: Object->GetPathName());
	}
	Names.Sort();
	OutTotal = Names.Num();
	if (Names.Num() > MaxListedObjects)
	{
		Names.SetNum(MaxListedObjects);
	}
	return Names;
}
}

FAssetDocumentServerPythonExecutionResult ExecuteAssetDocumentServerPython(
	const FString& Code,
	const FString& Description)
{
	using namespace AssetDocumentServerPythonExecutionPrivate;
	check(IsInGameThread());
	FAssetDocumentServerPythonExecutionResult Result;
	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (PythonPlugin == nullptr)
	{
		Result.Error = TEXT("PythonScriptPlugin is not available");
		return Result;
	}

	FAssetDocumentOwnedEditorTransaction Transaction;
	if (!Transaction.Begin(FText::FromString(Description)))
	{
		Result.Error = TEXT("The editor cannot open an undo transaction right now (an undo/redo is being applied); retry shortly.");
		return Result;
	}

	FPythonCommandEx PythonCommand;
	PythonCommand.Command = AsCodeCommand(Code);
	PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Private;
	{
		FNonUndoableWatch Watch;
		Result.bSucceeded = PythonPlugin->ExecPythonCommandEx(PythonCommand);
		Result.NotUndoable = MoveTemp(Watch.Operations);
	}

	if (Result.bSucceeded)
	{
		Result.bUndoEntryAdded = Transaction.Commit();
		if (Result.bUndoEntryAdded)
		{
			Result.UndoEntryId = Transaction.GetTransactionId();
		}
	}
	else if (!Result.NotUndoable.IsEmpty())
	{
		// Undoing only the undoable part would leave a half state (an asset renamed on disk but its
		// object renamed back in memory, ...): keep everything the script did, as without a transaction.
		Result.Rollback = EAssetDocumentTransactionRollback::Kept;
		Result.bUndoEntryAdded = Transaction.Commit();
		if (Result.bUndoEntryAdded)
		{
			Result.UndoEntryId = Transaction.GetTransactionId();
		}
	}
	else
	{
		const TArray<UObject*> Recorded = Transaction.IsStillActive() ? Transaction.GetRecordedObjects() : TArray<UObject*>();
		Result.RolledBackObjects = DescribeObjects(Recorded, Result.RolledBackObjectCount);
		Result.Rollback = Transaction.Rollback();
		if (Result.Rollback != EAssetDocumentTransactionRollback::Undone)
		{
			Result.RolledBackObjects.Reset();
			Result.RolledBackObjectCount = 0;
		}
	}

	Result.CommandResult = PythonCommand.CommandResult;
	for (const FPythonLogOutputEntry& LogEntry : PythonCommand.LogOutput)
	{
		FAssetDocumentServerPythonLogEntry& Entry = Result.Logs.AddDefaulted_GetRef();
		switch (LogEntry.Type)
		{
		case EPythonLogOutputType::Warning:
			Entry.Type = TEXT("warning");
			break;
		case EPythonLogOutputType::Error:
			Entry.Type = TEXT("error");
			break;
		case EPythonLogOutputType::Info:
		default:
			Entry.Type = TEXT("info");
			break;
		}
		Entry.Message = LogEntry.Output;
	}
	return Result;
}
