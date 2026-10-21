// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "LevelWorldIndex.h"

#include "ActorEditorUtils.h"
#include "ActorFolder.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHelpers.h"

namespace UE::AssetDocumentLevel::Private
{
bool IsListedActor(const AActor& Actor)
{
	// Transient actors (the default physics volume, the gameplay debugger manager, ...)
	// are spawned by the engine at runtime: not level content, never saved.
	return IsValid(&Actor)
		&& !Actor.IsTemplate()
		&& !Actor.HasAnyFlags(RF_Transient)
		&& Actor.IsListedInSceneOutliner()
		&& !FActorEditorUtils::IsABuilderBrush(&Actor)
		&& !Actor.IsA<AWorldSettings>();
}

FString GetLevelPackageName(const ULevel& Level)
{
	return Level.GetOutermost()->GetName();
}

TArray<ULevel*> GetLoadedLevels(UWorld& World)
{
	TArray<ULevel*> Levels;
	if (World.PersistentLevel != nullptr)
	{
		Levels.Add(World.PersistentLevel);
	}
	for (ULevelStreaming* Streaming : World.GetStreamingLevels())
	{
		ULevel* Level = Streaming != nullptr ? Streaming->GetLoadedLevel() : nullptr;
		if (Level != nullptr && !Levels.Contains(Level))
		{
			Levels.Add(Level);
		}
	}
	return Levels;
}

TSet<FString> GatherDirtyLevelPackages(UWorld& World)
{
	TSet<FString> Dirty;
	auto Add = [&Dirty](const UPackage* Package)
	{
		if (Package != nullptr && Package->IsDirty())
		{
			Dirty.Add(Package->GetName());
		}
	};
	for (ULevel* Level : GetLoadedLevels(World))
	{
		Add(Level->GetPackage());
		for (const AActor* Actor : Level->Actors)
		{
			if (Actor != nullptr)
			{
				Add(Actor->GetExternalPackage());
			}
		}
		Level->ForEachActorFolder([&Add](UActorFolder* Folder)
		{
			if (Folder != nullptr)
			{
				Add(Folder->GetExternalPackage());
			}
			return true;
		});
	}
	return Dirty;
}

void ForEachListedActor(UWorld& World, TFunctionRef<void(AActor&)> Visit)
{
	for (ULevel* Level : GetLoadedLevels(World))
	{
		for (AActor* Actor : Level->Actors)
		{
			if (Actor != nullptr && IsListedActor(*Actor))
			{
				Visit(*Actor);
			}
		}
	}
}

TArray<FString> GetActorDataLayers(const AActor& Actor)
{
	TArray<FString> Result;
	for (const UDataLayerAsset* Asset : Actor.GetDataLayerAssets())
	{
		if (Asset != nullptr)
		{
			Result.Add(Asset->GetPathName());
		}
	}
	Result.Sort();
	return Result;
}

namespace
{
FString DescribeDataLayer(const UDataLayerManager* Manager, FName InstanceName)
{
	const UDataLayerInstance* Instance = Manager != nullptr ? Manager->GetDataLayerInstanceFromName(InstanceName) : nullptr;
	const UDataLayerAsset* Asset = Instance != nullptr ? Instance->GetAsset() : nullptr;
	return Asset != nullptr ? Asset->GetPathName() : InstanceName.ToString();
}
}

namespace
{
FScopedUnloadedActorsCache* GActiveUnloadedActorsCache = nullptr;
}

FScopedUnloadedActorsCache::FScopedUnloadedActorsCache(UWorld& InWorld)
	: World(&InWorld)
	, Previous(GActiveUnloadedActorsCache)
{
	check(IsInGameThread());
	GActiveUnloadedActorsCache = this;
}

FScopedUnloadedActorsCache::~FScopedUnloadedActorsCache()
{
	GActiveUnloadedActorsCache = Previous;
}

const TArray<FUnloadedActor>& GetUnloadedActors(UWorld& World, TArray<FUnloadedActor>& Storage)
{
	FScopedUnloadedActorsCache* Cache = IsInGameThread() ? GActiveUnloadedActorsCache : nullptr;
	if (Cache != nullptr && Cache->World.Get() == &World)
	{
		if (!Cache->Actors.IsSet())
		{
			Cache->Actors = GatherUnloadedActors(World);
		}
		return Cache->Actors.GetValue();
	}
	Storage = GatherUnloadedActors(World);
	return Storage;
}

TArray<FUnloadedActor> GatherUnloadedActors(UWorld& World)
{
	TArray<FUnloadedActor> Result;
	UWorldPartition* WorldPartition = World.GetWorldPartition();
	if (WorldPartition == nullptr || World.PersistentLevel == nullptr)
	{
		return Result;
	}
	const UDataLayerManager* Manager = WorldPartition->GetDataLayerManager();
	const FString LevelPackage = GetLevelPackageName(*World.PersistentLevel);
	FWorldPartitionHelpers::ForEachActorDescInstance(WorldPartition, AActor::StaticClass(),
		[&](const FWorldPartitionActorDescInstance* Desc)
		{
			if (Desc == nullptr || Desc->IsLoaded() || !Desc->IsListedInSceneOutliner())
			{
				return true;
			}
			FUnloadedActor& Entry = Result.AddDefaulted_GetRef();
			Entry.Guid = Desc->GetGuid();
			Entry.Path = Desc->GetActorSoftPath().ToString();
			Entry.Name = Desc->GetActorNameString();
			Entry.Label = Desc->GetActorLabel().IsNone() ? FString() : Desc->GetActorLabelString();
			const FTopLevelAssetPath BaseClass = Desc->GetBaseClass();
			Entry.ClassPath = BaseClass.IsValid() ? BaseClass.ToString() : Desc->GetNativeClass().ToString();
			Entry.NativeClass = Desc->GetActorDesc() != nullptr ? Desc->GetActorDesc()->GetActorNativeClass() : nullptr;
			Entry.LevelPackage = LevelPackage;
			Entry.ExternalPackage = Desc->GetActorPackage().ToString();
			Entry.Folder = Desc->GetFolderPath().IsNone() ? FString() : Desc->GetFolderPath().ToString();
			Entry.Bounds = Desc->GetEditorBounds();
			Entry.Location = Desc->GetActorTransform().GetLocation();
			if (Desc->HasResolvedDataLayerInstanceNames())
			{
				for (const FName& Name : Desc->GetDataLayerInstanceNames().ToArray())
				{
					Entry.DataLayers.Add(DescribeDataLayer(Manager, Name));
				}
			}
			else
			{
				// Unresolved: the descriptor's own list (asset paths for data layer assets).
				for (const FName& Name : Desc->GetDataLayers())
				{
					Entry.DataLayers.Add(Name.ToString());
				}
			}
			Entry.DataLayers.Sort();
			Entry.Tags = Desc->GetTags();
			return true;
		});
	return Result;
}

int32 CountActorDescriptors(UWorld& World)
{
	int32 Count = 0;
	if (UWorldPartition* WorldPartition = World.GetWorldPartition())
	{
		FWorldPartitionHelpers::ForEachActorDescInstance(WorldPartition, AActor::StaticClass(),
			[&Count](const FWorldPartitionActorDescInstance* Desc)
			{
				Count += Desc != nullptr && Desc->IsListedInSceneOutliner() ? 1 : 0;
				return true;
			});
	}
	return Count;
}
}
