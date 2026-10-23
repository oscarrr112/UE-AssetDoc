// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Math/Box.h"
#include "Misc/Optional.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class UClass;
class ULevel;
class UWorld;

// What the level tools see of the editor world: the actors the World Outliner lists
// (persistent level and loaded sublevels) and, for a World Partition world, the
// actors that are not loaded, from their actor descriptors (no load happens).
namespace UE::AssetDocumentLevel::Private
{
// The Outliner's actor list: valid, listed in the Scene Outliner, not a builder
// brush, not the World Settings, not transient (engine-spawned at runtime).
ASSETDOCUMENTLEVEL_API bool IsListedActor(const AActor& Actor);

// "/Game/Maps/Room" for the persistent level of /Game/Maps/Room.Room.
FString GetLevelPackageName(const ULevel& Level);

// Persistent level first, then the loaded streaming levels in streaming order.
TArray<ULevel*> GetLoadedLevels(UWorld& World);

// The dirty packages of the world's loaded levels: map / sublevel packages and the
// external packages of their actors and actor folders (one file per object, OFPA).
TSet<FString> GatherDirtyLevelPackages(UWorld& World);

void ForEachListedActor(UWorld& World, TFunctionRef<void(AActor&)> Visit);

// Data layer asset paths of a loaded actor (instance names for asset-less instances).
TArray<FString> GetActorDataLayers(const AActor& Actor);

struct FUnloadedActor
{
	FGuid Guid;
	FString Path;
	FString Name;
	FString Label;
	// Blueprint class path when the actor is a Blueprint instance, else the native class.
	FString ClassPath;
	UClass* NativeClass = nullptr;
	FString LevelPackage;
	FString ExternalPackage;
	FString Folder;
	FBox Bounds = FBox(ForceInit);
	FVector Location = FVector::ZeroVector;
	TArray<FString> DataLayers;
	TArray<FName> Tags;
};

// World Partition actors whose descriptor exists but whose actor is not loaded.
// Empty for a world without World Partition.
TArray<FUnloadedActor> GatherUnloadedActors(UWorld& World);

// While alive (game thread), GetUnloadedActors gathers the descriptors of World once
// and reuses them: for a request that resolves many references against a world that
// does not change meanwhile (edit_level's preflight).
class FScopedUnloadedActorsCache
{
public:
	explicit FScopedUnloadedActorsCache(UWorld& InWorld);
	~FScopedUnloadedActorsCache();
	FScopedUnloadedActorsCache(const FScopedUnloadedActorsCache&) = delete;
	FScopedUnloadedActorsCache& operator=(const FScopedUnloadedActorsCache&) = delete;

	TWeakObjectPtr<UWorld> World;
	TOptional<TArray<FUnloadedActor>> Actors;

private:
	FScopedUnloadedActorsCache* Previous = nullptr;
};

// GatherUnloadedActors(World), from the active cache when there is one for World
// (Storage holds the result otherwise).
const TArray<FUnloadedActor>& GetUnloadedActors(UWorld& World, TArray<FUnloadedActor>& Storage);

// Number of World Partition actor descriptors (loaded or not); 0 without World Partition.
int32 CountActorDescriptors(UWorld& World);
}
