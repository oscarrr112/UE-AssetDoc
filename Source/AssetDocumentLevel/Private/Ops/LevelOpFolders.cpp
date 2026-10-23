// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// create_folder / rename_folder / delete_folder: the Outliner's folder commands on the
// world's folders (FActorFolders), with the Outliner's delete semantics: the folder's
// actors and sub-folders move up to its parent (design §2.3, §5.2).

#include "ActorFolder.h"
#include "EditorActorFolders.h"
#include "EditorFolderUtils.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Folder.h"
#include "GameFramework/Actor.h"
#include "LevelEditTypes.h"
#include "LevelWorldIndex.h"
#include "UObject/Package.h"

namespace UE::AssetDocumentLevel::Private
{
TSharedRef<FJsonObject> MakeLevelOpResult(const TCHAR* Kind, const FString& CreatedPath);

namespace
{
FString ParentFolderOf(const FString& Path)
{
	int32 Slash = INDEX_NONE;
	return Path.FindLastChar(TEXT('/'), Slash) ? Path.Left(Slash) : FString();
}

FString LeafOf(const FString& Path)
{
	int32 Slash = INDEX_NONE;
	return Path.FindLastChar(TEXT('/'), Slash) ? Path.RightChop(Slash + 1) : Path;
}

bool IsSameOrBelow(const FString& Path, const FString& Folder)
{
	return Path == Folder || Path.StartsWith(Folder + TEXT("/"));
}

FFolder MakeWorldFolder(UWorld& World, const FString& Path)
{
	return FFolder(FFolder::GetWorldRootFolder(&World).GetRootObject(), Path.IsEmpty() ? FFolder::GetEmptyPath() : FName(*Path));
}

// Records the external packages of the actor folder objects at or below Path (World
// Partition levels), so the response lists them and save writes them.
void RecordFolderPackages(FLevelEditApply& Apply, UWorld& World, const FString& Path)
{
	FActorFolders::Get().ForEachFolder(World, [&Apply, &Path](const FFolder& Each)
	{
		const UActorFolder* ActorFolder = IsSameOrBelow(Each.GetPath().ToString(), Path) ? Each.GetActorFolder() : nullptr;
		const UPackage* Package = ActorFolder != nullptr ? ActorFolder->GetExternalPackage() : nullptr;
		if (Package != nullptr)
		{
			Apply.FolderPackages.AddUnique(Package->GetName());
		}
		return true;
	});
}

// Without actor folder objects (a level without World Partition) the Outliner's folders live in the actors' folder
// paths; an empty folder is kept for the editor session only.
bool HasFolderObjects(const UWorld& World)
{
	return World.PersistentLevel != nullptr && World.PersistentLevel->IsUsingActorFolders();
}

bool RequireFolder(FLevelEditPlan& Plan, const FString& Path, const FString& Pointer)
{
	if (Plan.GetFolders().Contains(Path))
	{
		return true;
	}
	TArray<FString> Known = Plan.GetFolders().Array();
	Known.Sort();
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Folder : Known)
	{
		Values.Add(MakeShared<FJsonValueString>(Folder));
	}
	TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(TEXT("folders"), Values);
	Plan.AddError(Pointer, TEXT("Level.FolderNotFound"), FString::Printf(TEXT("The world has no folder '%s'."), *Path), Details);
	return false;
}

// Registers every loaded actor in or below Folder with the plan, so the rollback check
// covers the actors a folder command moves.
void RegisterFolderActors(FLevelEditPlan& Plan, const FString& Folder)
{
	ForEachListedActor(Plan.World, [&Plan, &Folder](AActor& Actor)
	{
		if (IsSameOrBelow(GetLevelFolderText(Actor), Folder))
		{
			Plan.FindOrAddExisting(Actor);
		}
	});
}

FLevelChange& AddFolderChange(TArray<FLevelChange>& Changes, const FString& Op, const TCHAR* Kind, const FString& Path)
{
	FLevelChange& Change = Changes.AddDefaulted_GetRef();
	Change.Op = Op;
	Change.Kind = Kind;
	Change.Object = TEXT("folder");
	Change.Extra = MakeShared<FJsonObject>();
	Change.Extra->SetStringField(TEXT("path"), Path);
	return Change;
}

class FLevelOpCreateFolder final : public FLevelEditOp
{
public:
	FString Path;

	virtual const TCHAR* GetKind() const override { return TEXT("create_folder"); }

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		if (Plan.GetFolders().Contains(Path))
		{
			// Already there: nothing to do (create_folder is idempotent).
			return;
		}
		for (FString Folder = Path; !Folder.IsEmpty(); Folder = ParentFolderOf(Folder))
		{
			Plan.GetFolders().Add(Folder);
		}
		AddFolderChange(Plan.Changes, Pointer, TEXT("Add"), Path);
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		UWorld* World = Apply.GetWorld();
		if (World == nullptr)
		{
			return false;
		}
		Apply.OpResults[Index] = MakeLevelOpResult(GetKind(), FString());
		const FFolder Folder = MakeWorldFolder(*World, Path);
		if (!FActorFolders::Get().ContainsFolder(*World, Folder))
		{
			FActorFolders::Get().CreateFolder(*World, Folder);
			RecordFolderPackages(Apply, *World, Path);
			if (!HasFolderObjects(*World))
			{
				Apply.SessionOnlyFolders.AddUnique(Path);
			}
			++Apply.FolderOps;
			AddFolderChange(Apply.Changes, Pointer, TEXT("Add"), Path);
		}
		Apply.ExpectFolder(Path, true, Pointer);
		return true;
	}
};

class FLevelOpRenameFolder final : public FLevelEditOp
{
public:
	FString From;
	FString To;

	virtual const TCHAR* GetKind() const override { return TEXT("rename_folder"); }

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		if (!RequireFolder(Plan, From, Pointer + TEXT("/from")))
		{
			return;
		}
		if (IsSameOrBelow(To, From))
		{
			Plan.AddError(Pointer + TEXT("/to"), TEXT("Level.InvalidRequest"), FString::Printf(TEXT("'%s' is '%s' or inside it; a folder "
				"cannot be moved into itself."), *To, *From));
			return;
		}
		if (Plan.GetFolders().Contains(To))
		{
			Plan.AddError(Pointer + TEXT("/to"), TEXT("Level.InvalidRequest"), FString::Printf(TEXT("Folder '%s' already exists; the "
				"Outliner does not merge folders. Move its actors with set {\"folder\"} or pick another name."), *To));
			return;
		}
		RegisterFolderActors(Plan, From);
		TSet<FString>& Folders = Plan.GetFolders();
		TArray<FString> Moved;
		for (const FString& Folder : Folders)
		{
			if (IsSameOrBelow(Folder, From))
			{
				Moved.Add(Folder);
			}
		}
		for (const FString& Folder : Moved)
		{
			Folders.Remove(Folder);
			Folders.Add(To + Folder.RightChop(From.Len()));
		}
		for (FString Folder = ParentFolderOf(To); !Folder.IsEmpty(); Folder = ParentFolderOf(Folder))
		{
			Folders.Add(Folder);
		}
		FLevelChange& Change = AddFolderChange(Plan.Changes, Pointer, TEXT("Replace"), From);
		Change.Before = MakeShared<FJsonValueString>(From);
		Change.After = MakeShared<FJsonValueString>(To);
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		UWorld* World = Apply.GetWorld();
		if (World == nullptr)
		{
			return false;
		}
		Apply.OpResults[Index] = MakeLevelOpResult(GetKind(), FString());
		// Renaming onto an existing folder hits a check() in the engine (actor folder
		// levels). Preflight refuses known folders; this catches names the plan cannot
		// know (a delete_folder earlier in the batch gives a moved sub-folder a numbered
		// name when its plain name is taken).
		if (FActorFolders::Get().ContainsFolder(*World, MakeWorldFolder(*World, To)))
		{
			Apply.AddError(Pointer + TEXT("/to"), TEXT("Level.InvalidRequest"), FString::Printf(TEXT("Folder '%s' already exists; the "
				"Outliner does not merge folders. Move its actors with set {\"folder\"} or pick another name."), *To));
			return false;
		}
		RecordFolderPackages(Apply, *World, From);
		if (!FActorFolders::Get().RenameFolderInWorld(*World, MakeWorldFolder(*World, From), MakeWorldFolder(*World, To)))
		{
			Apply.AddError(Pointer, TEXT("Level.FolderNotFound"), FString::Printf(TEXT("The editor did not rename folder '%s' to '%s'."),
				*From, *To));
			return false;
		}
		RecordFolderPackages(Apply, *World, To);
		++Apply.FolderOps;
		Apply.MoveFolderExpectations(From, To);
		int32 Moved = 0;
		ForEachListedActor(*World, [&Apply, &Moved, this](AActor& Actor)
		{
			if (IsSameOrBelow(GetLevelFolderText(Actor), To))
			{
				Apply.Touch(Actor);
				++Moved;
			}
		});
		if (Moved == 0 && !HasFolderObjects(*World))
		{
			Apply.SessionOnlyFolders.AddUnique(To);
		}
		FLevelChange& Change = AddFolderChange(Apply.Changes, Pointer, TEXT("Replace"), From);
		Change.Before = MakeShared<FJsonValueString>(From);
		Change.After = MakeShared<FJsonValueString>(To);
		Apply.ExpectFolder(From, false, Pointer);
		Apply.ExpectFolder(To, true, Pointer);
		return true;
	}
};

class FLevelOpDeleteFolder final : public FLevelEditOp
{
public:
	FString Path;

	virtual const TCHAR* GetKind() const override { return TEXT("delete_folder"); }

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		if (!RequireFolder(Plan, Path, Pointer + TEXT("/path")))
		{
			return;
		}
		RegisterFolderActors(Plan, Path);
		const FString Parent = ParentFolderOf(Path);
		TSet<FString>& Folders = Plan.GetFolders();
		TArray<FString> Below;
		for (const FString& Folder : Folders)
		{
			if (IsSameOrBelow(Folder, Path))
			{
				Below.Add(Folder);
			}
		}
		for (const FString& Folder : Below)
		{
			Folders.Remove(Folder);
			if (Folder != Path)
			{
				const FString Rest = Folder.RightChop(Path.Len() + 1);
				Folders.Add(Parent.IsEmpty() ? Rest : Parent + TEXT("/") + Rest);
			}
		}
		AddFolderChange(Plan.Changes, Pointer, TEXT("Remove"), Path);
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		UWorld* World = Apply.GetWorld();
		if (World == nullptr)
		{
			return false;
		}
		Apply.OpResults[Index] = MakeLevelOpResult(GetKind(), FString());
		const FFolder Folder = MakeWorldFolder(*World, Path);
		if (!FActorFolders::Get().ContainsFolder(*World, Folder))
		{
			Apply.AddError(Pointer + TEXT("/path"), TEXT("Level.FolderNotFound"), FString::Printf(TEXT("The world has no folder '%s'."), *Path));
			return false;
		}
		// FActorFolderTreeItem::Delete: the folder's actors (attached children follow
		// their parent) and direct sub-folders move to the parent folder; levels that use
		// actor folder objects resolve both through the folder's parent instead.
		const FString Parent = ParentFolderOf(Path);
		const FFolder ParentFolder = MakeWorldFolder(*World, Parent);
		TArray<AActor*> Direct;
		ForEachListedActor(*World, [&Direct, this](AActor& Actor)
		{
			const AActor* AttachParent = Actor.GetAttachParentActor();
			if (GetLevelFolderText(Actor) == Path && (AttachParent == nullptr || GetLevelFolderText(*AttachParent) != Path))
			{
				Direct.Add(&Actor);
			}
		});
		for (AActor* Actor : Direct)
		{
			if (Actor->GetLevel() != nullptr && !Actor->GetLevel()->IsUsingActorFolders())
			{
				Actor->SetFolderPath_Recursively(Parent.IsEmpty() ? FName(NAME_None) : FName(*Parent));
			}
			Apply.Touch(*Actor);
		}
		TArray<FString> Children;
		FActorFolders::Get().ForEachFolderWithRootObject(*World, Folder.GetRootObject(), [&Children, this](const FFolder& Each)
		{
			const FString EachPath = Each.GetPath().ToString();
			if (ParentFolderOf(EachPath) == Path && Each.GetActorFolder() == nullptr)
			{
				Children.Add(EachPath);
			}
			return true;
		});
		for (const FString& Child : Children)
		{
			FActorFolders::Get().RenameFolderInWorld(*World, MakeWorldFolder(*World, Child),
				FActorFolders::Get().GetFolderName(*World, ParentFolder, FName(*LeafOf(Child))));
		}
		RecordFolderPackages(Apply, *World, Path);
		FActorFolders::Get().DeleteFolder(*World, Folder);
		++Apply.FolderOps;
		Apply.MoveFolderExpectations(Path, Parent);
		AddFolderChange(Apply.Changes, Pointer, TEXT("Remove"), Path);
		Apply.ExpectFolder(Path, false, Pointer);
		return true;
	}
};

bool ParseFolderField(const FJsonObject& Json, const TCHAR* Field, const FString& Pointer, FString& Out,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	if (!Json.TryGetStringField(Field, Out))
	{
		Out.Reset();
	}
	Out.RemoveFromEnd(TEXT("/"));
	if (Out.IsEmpty() || Out.StartsWith(TEXT("/")) || Out.Contains(TEXT("//")) || Out.TrimStartAndEnd() != Out)
	{
		OutDiagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/") + Field, FString::Printf(TEXT("'%s' is an Outliner folder path such as "
			"\"Room/Lights\" (no leading '/', not empty)."), Field)));
		return false;
	}
	return true;
}

TUniquePtr<FLevelEditOp> ParseCreateFolder(const FJsonObject& Json, const FString& Pointer, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("path")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpCreateFolder> Op = MakeUnique<FLevelOpCreateFolder>();
	ParseFolderField(Json, TEXT("path"), Pointer, Op->Path, OutDiagnostics);
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

TUniquePtr<FLevelEditOp> ParseRenameFolder(const FJsonObject& Json, const FString& Pointer, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("from"), TEXT("to")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpRenameFolder> Op = MakeUnique<FLevelOpRenameFolder>();
	ParseFolderField(Json, TEXT("from"), Pointer, Op->From, OutDiagnostics);
	ParseFolderField(Json, TEXT("to"), Pointer, Op->To, OutDiagnostics);
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

TUniquePtr<FLevelEditOp> ParseDeleteFolder(const FJsonObject& Json, const FString& Pointer, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("path")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpDeleteFolder> Op = MakeUnique<FLevelOpDeleteFolder>();
	ParseFolderField(Json, TEXT("path"), Pointer, Op->Path, OutDiagnostics);
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

const FLevelEditOpRegistration CreateFolderRegistration(TEXT("create_folder"), &ParseCreateFolder);
const FLevelEditOpRegistration RenameFolderRegistration(TEXT("rename_folder"), &ParseRenameFolder);
const FLevelEditOpRegistration DeleteFolderRegistration(TEXT("delete_folder"), &ParseDeleteFolder);
}
}
