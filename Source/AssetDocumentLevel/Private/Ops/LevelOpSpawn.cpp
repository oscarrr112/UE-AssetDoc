// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// spawn: the Content Browser / Place Actors drop of an actor class or an asset, at an
// explicit transform and level (Raw/LevelPlacement), followed by the same field edits
// as set (design §2.3, §5.2).

#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelEditTypes.h"
#include "LevelWorldIndex.h"
#include "Misc/PackageName.h"
#include "Raw/LevelPlacement.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AssetDocumentLevel::Private
{
UObject* LoadLevelSpawnSource(const FString& Path, bool bClass);
ULevel* FindLoadedLevelForEdit(UWorld& World, const FString& Package);
FTransform MakeLevelSpawnTransform(const FLevelActorFieldEdits& Edits);
TSharedRef<FJsonObject> MakeLevelOpResult(const TCHAR* Kind, const FString& CreatedPath);

namespace
{
class FLevelOpSpawn final : public FLevelEditOp
{
public:
	FString SourcePath;
	bool bClass = false;
	FString LevelPackage;
	FLevelActorFieldEdits Edits;
	TOptional<FActorRef> AttachTo;

	virtual const TCHAR* GetKind() const override { return TEXT("spawn"); }

	FString SourcePointer() const
	{
		return Pointer + (bClass ? TEXT("/class") : TEXT("/asset"));
	}

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		UObject* Source = LoadLevelSpawnSource(SourcePath, bClass);
		if (Source == nullptr)
		{
			Plan.AddError(SourcePointer(), TEXT("Level.AssetNotFound"), bClass
				? FString::Printf(TEXT("No actor class '%s' (a native class is \"/Script/Engine.PointLight\"; a Blueprint class is "
					"\"/Game/Path/BP_Name.BP_Name_C\", or pass the Blueprint as \"asset\")."), *SourcePath)
				: FString::Printf(TEXT("No asset '%s' (an object path such as \"/Engine/BasicShapes/Cube.Cube\")."), *SourcePath));
			return;
		}
		const FLevelPlacementCheck Check = CheckLevelPlacement(*Source);
		if (!Check.bCanPlace)
		{
			Plan.AddError(SourcePointer(), Check.Code, Check.Message);
			return;
		}
		ULevel* Level = Plan.ResolveTargetLevel(LevelPackage, Pointer + TEXT("/level"));
		if (Level == nullptr)
		{
			return;
		}
		UClass* Class = Check.ActorClass != nullptr ? Check.ActorClass : AActor::StaticClass();
		const int32 ActorIndex = Plan.AddPlanned(Index, BatchId, *Class, *Level, nullptr);
		++Plan.Spawns;
		FLevelChange& Change = Plan.Changes.AddDefaulted_GetRef();
		Change.Op = Pointer;
		Change.Kind = TEXT("Add");
		Change.Object = TEXT("actor");
		Change.Actor = Plan.Actors[ActorIndex].Display();
		Change.Extra = MakeShared<FJsonObject>();
		Change.Extra->SetStringField(TEXT("class"), Class->GetPathName());
		Change.Extra->SetStringField(TEXT("level"), GetLevelPackageName(*Level));
		if (AttachTo.IsSet())
		{
			// Attached right after placement, so "transform" is relative to the parent.
			const int32 ParentIndex = Plan.Resolve(AttachTo.GetValue(), Pointer + TEXT("/attach_to"));
			if (ParentIndex == INDEX_NONE || !Plan.PreflightAttach(ActorIndex, ParentIndex, FString(), FString(), Pointer + TEXT("/attach_to")))
			{
				return;
			}
			FLevelChange& Attachment = Plan.Changes.AddDefaulted_GetRef();
			Attachment.Op = Pointer + TEXT("/attach_to");
			Attachment.Kind = TEXT("Replace");
			Attachment.Object = TEXT("attachment");
			Attachment.Actor = Plan.Actors[ActorIndex].Display();
			Attachment.Before = MakeShared<FJsonValueNull>();
			Attachment.After = MakeShared<FJsonValueString>(Plan.Actors[ParentIndex].Display());
		}
		Plan.PreflightFields(ActorIndex, Edits, Pointer);
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		UWorld* World = Apply.GetWorld();
		UObject* Source = LoadLevelSpawnSource(SourcePath, bClass);
		ULevel* Level = World != nullptr ? FindLoadedLevelForEdit(*World, LevelPackage) : nullptr;
		if (Source == nullptr || Level == nullptr)
		{
			Apply.AddError(Pointer, TEXT("Level.SpawnFailed"), FString::Printf(TEXT("%s or its target level is no longer available."),
				*SourcePath));
			return false;
		}
		FString Error;
		AActor* Actor = PlaceInLevel(*Source, *Level, MakeLevelSpawnTransform(Edits), Error);
		if (Actor == nullptr)
		{
			Apply.AddError(Pointer, TEXT("Level.SpawnFailed"), Error);
			return false;
		}
		const FString Path = GetActorPath(*Actor);
		if (!BatchId.IsEmpty())
		{
			Apply.BatchPaths.Add(BatchId, Path);
		}
		Apply.ExpectAlive(Path, Actor->GetClass(), Pointer);
		Apply.Touch(*Actor);
		++Apply.Spawned;
		FLevelChange& Change = Apply.Changes.AddDefaulted_GetRef();
		Change.Op = Pointer;
		Change.Kind = TEXT("Add");
		Change.Object = TEXT("actor");
		Change.Actor = Path;
		Change.Extra = MakeShared<FJsonObject>();
		Change.Extra->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
		Change.Extra->SetStringField(TEXT("level"), GetLevelPackageOf(*Actor));
		Apply.OpResults[Index] = MakeLevelOpResult(GetKind(), Path);
		if (AttachTo.IsSet())
		{
			AActor* Parent = Apply.ResolveRef(AttachTo.GetValue(), Pointer + TEXT("/attach_to"));
			AActor* Placed = Apply.ResolvePath(Path);
			if (Parent == nullptr || Placed == nullptr
				|| !ApplyLevelAttach(Apply, *Placed, *Parent, FString(), FString(), Pointer + TEXT("/attach_to")))
			{
				return false;
			}
		}
		return Apply.ApplyFields(Path, Edits, Pointer);
	}
};

// A spawned actor's omitted transform members are the placement defaults (location 0,
// rotation 0, scale 1). They are written explicitly after placement, so the stored
// rotation is exactly the given one rather than the placement's quaternion round trip
// (a Roll of -8e-16 instead of 0).
void CompleteSpawnTransformMembers(FLevelActorFieldEdits& Edits)
{
	if (!Edits.Transform.IsSet())
	{
		return;
	}
	static const TCHAR* const Keys[3][3] = {{TEXT("X"), TEXT("Y"), TEXT("Z")}, {TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")},
		{TEXT("X"), TEXT("Y"), TEXT("Z")}};
	for (int32 Member = 0; Member < 3; ++Member)
	{
		TOptional<FLevelValue>& Value = Edits.Transform->Members[Member];
		if (!Value.IsSet() || Value->Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			continue;
		}
		for (const TCHAR* Key : Keys[Member])
		{
			if (!Value->ObjectKeys.Contains(FString(Key)))
			{
				FLevelValue Default;
				Default.Kind = EAssetDocumentRuntimeValueKind::Integer;
				Default.IntegerValue = static_cast<ELevelTransformMember>(Member) == ELevelTransformMember::Scale ? 1 : 0;
				Value->ObjectKeys.Add(Key);
				Value->Elements.Add(MoveTemp(Default));
			}
		}
	}
}

TUniquePtr<FLevelEditOp> ParseSpawn(const FJsonObject& Json, const FString& Pointer, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("id"), TEXT("class"), TEXT("asset"), TEXT("level"), TEXT("transform"), TEXT("label"),
		TEXT("folder"), TEXT("tags"), TEXT("properties"), TEXT("components"), TEXT("layers"), TEXT("data_layers"), TEXT("attach_to")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpSpawn> Op = MakeUnique<FLevelOpSpawn>();
	if (Json.HasField(TEXT("attach_to")))
	{
		FActorRef Parent;
		if (ParseLevelActorRefField(Json, TEXT("attach_to"), Pointer, true, Parent, OutDiagnostics))
		{
			Op->AttachTo = MoveTemp(Parent);
		}
	}
	ParseLevelBatchIdField(Json, Pointer, Op->BatchId, OutDiagnostics);
	const bool bHasClass = Json.HasField(TEXT("class"));
	const bool bHasAsset = Json.HasField(TEXT("asset"));
	if (bHasClass == bHasAsset)
	{
		OutDiagnostics.Add(MakeInvalidRequest(Pointer, TEXT("spawn names exactly one of \"class\" (an actor class path) or \"asset\" "
			"(an asset to place, as a Content Browser drag: a static mesh, a Blueprint, ...).")));
	}
	else
	{
		Op->bClass = bHasClass;
		const TCHAR* Field = bHasClass ? TEXT("class") : TEXT("asset");
		if (!Json.TryGetStringField(Field, Op->SourcePath) || Op->SourcePath.IsEmpty())
		{
			OutDiagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/") + Field, TEXT("an object path string.")));
		}
	}
	if (Json.HasField(TEXT("level")) && (!Json.TryGetStringField(TEXT("level"), Op->LevelPackage) || Op->LevelPackage.IsEmpty()))
	{
		OutDiagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/level"), TEXT("'level' is a level package path (\"/Game/Maps/Room\"); "
			"omit it for the current level.")));
	}
	ParseLevelActorFieldEdits(Json, Pointer, Op->Edits, OutDiagnostics);
	CompleteSpawnTransformMembers(Op->Edits);
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

const FLevelEditOpRegistration SpawnRegistration(TEXT("spawn"), &ParseSpawn);
}

// ---------------------------------------------------------------------------
// Helpers shared by the op files.

UObject* LoadLevelSpawnSource(const FString& Path, bool bClass)
{
	FString ObjectPath = Path;
	// "/Game/Blueprints/BP_Door" (a package path) names the asset of the same name.
	if (!ObjectPath.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(ObjectPath))
	{
		ObjectPath += TEXT(".") + FPackageName::GetShortName(ObjectPath);
	}
	UObject* Object = FSoftObjectPath(ObjectPath).TryLoad();
	if (bClass)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
		{
			return Blueprint->GeneratedClass;
		}
		if (Object == nullptr && !ObjectPath.EndsWith(TEXT("_C")))
		{
			// "/Game/BP_Door.BP_Door" given as a class: its generated class.
			Object = FSoftObjectPath(ObjectPath + TEXT("_C")).TryLoad();
		}
		return Cast<UClass>(Object);
	}
	return Object;
}

ULevel* FindLoadedLevelForEdit(UWorld& World, const FString& Package)
{
	if (Package.IsEmpty())
	{
		return World.GetCurrentLevel() != nullptr ? World.GetCurrentLevel() : World.PersistentLevel.Get();
	}
	for (ULevel* Level : GetLoadedLevels(World))
	{
		if (GetLevelPackageName(*Level) == Package)
		{
			return Level;
		}
	}
	return nullptr;
}

FTransform MakeLevelSpawnTransform(const FLevelActorFieldEdits& Edits)
{
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector::OneVector;
	if (Edits.Transform.IsSet())
	{
		auto Member = [](const FLevelValue& Value, const TCHAR* Key, double Fallback)
		{
			for (int32 Index = 0; Index < Value.ObjectKeys.Num() && Index < Value.Elements.Num(); ++Index)
			{
				if (Value.ObjectKeys[Index] == Key)
				{
					const FLevelValue& Number = Value.Elements[Index];
					return Number.Kind == EAssetDocumentRuntimeValueKind::Integer ? static_cast<double>(Number.IntegerValue) : Number.NumberValue;
				}
			}
			return Fallback;
		};
		const FLevelTransformEdit& Transform = Edits.Transform.GetValue();
		if (const TOptional<FLevelValue>& Value = Transform.Members[static_cast<int32>(ELevelTransformMember::Location)]; Value.IsSet())
		{
			Location = FVector(Member(*Value, TEXT("X"), 0.0), Member(*Value, TEXT("Y"), 0.0), Member(*Value, TEXT("Z"), 0.0));
		}
		if (const TOptional<FLevelValue>& Value = Transform.Members[static_cast<int32>(ELevelTransformMember::Rotation)]; Value.IsSet())
		{
			Rotation = FRotator(Member(*Value, TEXT("Pitch"), 0.0), Member(*Value, TEXT("Yaw"), 0.0), Member(*Value, TEXT("Roll"), 0.0));
		}
		if (const TOptional<FLevelValue>& Value = Transform.Members[static_cast<int32>(ELevelTransformMember::Scale)]; Value.IsSet())
		{
			Scale = FVector(Member(*Value, TEXT("X"), 1.0), Member(*Value, TEXT("Y"), 1.0), Member(*Value, TEXT("Z"), 1.0));
		}
	}
	return FTransform(Rotation, Location, Scale);
}

TSharedRef<FJsonObject> MakeLevelOpResult(const TCHAR* Kind, const FString& CreatedPath)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("op"), Kind);
	if (!CreatedPath.IsEmpty())
	{
		Result->SetStringField(TEXT("created"), CreatedPath);
	}
	return Result;
}
}
