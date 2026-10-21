// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// duplicate: the editor's Duplicate (Ctrl+D / Ctrl+W) through
// UEditorActorSubsystem::DuplicateActors (UUnrealEdEngine::DuplicateActors), into the
// source's level or the given one, followed by the same field edits as set.

#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelEditTypes.h"
#include "LevelWorldIndex.h"
#include "Subsystems/EditorActorSubsystem.h"

namespace UE::AssetDocumentLevel::Private
{
ULevel* FindLoadedLevelForEdit(UWorld& World, const FString& Package);
TSharedRef<FJsonObject> MakeLevelOpResult(const TCHAR* Kind, const FString& CreatedPath);

namespace
{
class FLevelOpDuplicate final : public FLevelEditOp
{
public:
	FActorRef Source;
	FVector Offset = FVector::ZeroVector;
	FString LevelPackage;
	FLevelActorFieldEdits Edits;

	virtual const TCHAR* GetKind() const override { return TEXT("duplicate"); }

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		const int32 SourceIndex = Plan.Resolve(Source, Pointer + TEXT("/actor"));
		if (SourceIndex == INDEX_NONE)
		{
			return;
		}
		const FLevelPlanActor SourceEntry = Plan.Actors[SourceIndex];
		ULevel* Level = LevelPackage.IsEmpty() ? SourceEntry.Level.Get() : Plan.ResolveTargetLevel(LevelPackage, Pointer + TEXT("/level"));
		UClass* Class = SourceEntry.Class.Get();
		if (Level == nullptr || Class == nullptr)
		{
			return;
		}
		AActor* Live = SourceEntry.bExisting ? SourceEntry.Actor.Get() : nullptr;
		const int32 ActorIndex = Plan.AddPlanned(Index, BatchId, *Class, *Level, Live);
		++Plan.Duplicates;
		FLevelChange& Change = Plan.Changes.AddDefaulted_GetRef();
		Change.Op = Pointer;
		Change.Kind = TEXT("Add");
		Change.Object = TEXT("actor");
		Change.Actor = Plan.Actors[ActorIndex].Display();
		Change.Extra = MakeShared<FJsonObject>();
		Change.Extra->SetStringField(TEXT("duplicate_of"), SourceEntry.Display());
		Change.Extra->SetStringField(TEXT("class"), Class->GetPathName());
		Change.Extra->SetStringField(TEXT("level"), GetLevelPackageName(*Level));
		Plan.PreflightFields(ActorIndex, Edits, Pointer);
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		AActor* SourceActor = Apply.ResolveRef(Source, Pointer + TEXT("/actor"));
		UWorld* World = Apply.GetWorld();
		if (SourceActor == nullptr || World == nullptr)
		{
			return false;
		}
		ULevel* Level = LevelPackage.IsEmpty() ? SourceActor->GetLevel() : FindLoadedLevelForEdit(*World, LevelPackage);
		UEditorActorSubsystem* Actors = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
		if (Level == nullptr || Actors == nullptr)
		{
			Apply.AddError(Pointer, TEXT("Level.SpawnFailed"), TEXT("The target level or the editor actor subsystem is not available."));
			return false;
		}
		const FString SourcePath = GetActorPath(*SourceActor);
		// Duplicate goes through copy / paste, which selects the pasted actors; the
		// selection is put back (edit_level changes it only through after.select).
		const TArray<AActor*> Selected = Actors->GetSelectedLevelActors();
		UEditorActorSubsystem::FActorDuplicateParameters Parameters;
		Parameters.LevelOverride = Level;
		Parameters.bTransact = true;
		const TArray<AActor*> Created = Actors->DuplicateActors({SourceActor}, World, Offset, Parameters);
		if (Actors->GetSelectedLevelActors() != Selected)
		{
			Actors->SetSelectedLevelActors(Selected);
		}
		AActor* Actor = Created.Num() == 1 ? Created[0] : nullptr;
		if (Actor == nullptr)
		{
			Apply.AddError(Pointer, TEXT("Level.SpawnFailed"), FString::Printf(TEXT("The editor did not duplicate %s."), *SourcePath));
			return false;
		}
		const FString Path = GetActorPath(*Actor);
		if (!BatchId.IsEmpty())
		{
			Apply.BatchPaths.Add(BatchId, Path);
		}
		Apply.ExpectAlive(Path, Actor->GetClass(), Pointer);
		Apply.Touch(*Actor);
		++Apply.Duplicated;
		FLevelChange& Change = Apply.Changes.AddDefaulted_GetRef();
		Change.Op = Pointer;
		Change.Kind = TEXT("Add");
		Change.Object = TEXT("actor");
		Change.Actor = Path;
		Change.Extra = MakeShared<FJsonObject>();
		Change.Extra->SetStringField(TEXT("duplicate_of"), SourcePath);
		Change.Extra->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
		Change.Extra->SetStringField(TEXT("level"), GetLevelPackageOf(*Actor));
		Apply.OpResults[Index] = MakeLevelOpResult(GetKind(), Path);
		return Apply.ApplyFields(Path, Edits, Pointer);
	}
};

TUniquePtr<FLevelEditOp> ParseDuplicate(const FJsonObject& Json, const FString& Pointer, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("id"), TEXT("actor"), TEXT("offset"), TEXT("level"), TEXT("transform"), TEXT("label"),
		TEXT("folder"), TEXT("tags"), TEXT("properties"), TEXT("components"), TEXT("layers"), TEXT("data_layers")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpDuplicate> Op = MakeUnique<FLevelOpDuplicate>();
	ParseLevelBatchIdField(Json, Pointer, Op->BatchId, OutDiagnostics);
	ParseLevelActorRefField(Json, TEXT("actor"), Pointer, true, Op->Source, OutDiagnostics);
	if (Json.HasField(TEXT("offset")))
	{
		ParseLevelVector(Json.TryGetField(TEXT("offset")), Pointer + TEXT("/offset"), Op->Offset, OutDiagnostics);
	}
	if (Json.HasField(TEXT("level")) && (!Json.TryGetStringField(TEXT("level"), Op->LevelPackage) || Op->LevelPackage.IsEmpty()))
	{
		OutDiagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/level"), TEXT("'level' is a level package path (\"/Game/Maps/Room\"); "
			"omit it for the source actor's level.")));
	}
	ParseLevelActorFieldEdits(Json, Pointer, Op->Edits, OutDiagnostics);
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

const FLevelEditOpRegistration DuplicateRegistration(TEXT("duplicate"), &ParseDuplicate);
}
}
