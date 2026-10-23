// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class UWorld;

// The pre-image digest of an edit_level batch (design §3.1 step 4) and the
// post-rollback comparison: everything the batch can touch, as text.
namespace UE::AssetDocumentLevel::Private
{
struct FLevelDigestValue
{
	FString ActorPath;
	// "" for the actor itself.
	FString Component;
	FString Property;
};

struct FLevelEditDigestScope
{
	// Existing actors the batch resolves (edited, deleted, duplicated from).
	TArray<FString> ActorPaths;
	// Property values the batch writes on existing actors.
	TArray<FLevelDigestValue> Values;
	// Level packages whose actor list is compared (target levels of spawn / duplicate,
	// levels of touched actors).
	TArray<FString> LevelPackages;
	// Whether the world's Outliner folders are compared (folder ops in the batch).
	bool bFolders = false;
};

struct FLevelEditDigest
{
	// Sorted key -> text.
	TMap<FString, FString> Entries;
};

FLevelEditDigest CaptureLevelEditDigest(UWorld& World, const FLevelEditDigestScope& Scope);

// The keys that differ (at most MaxDifferences, in key order), e.g.
// "actor:/Game/Maps/Room.Room:PersistentLevel.PointLight_0/label: 'A' -> 'B'".
TArray<FString> CompareLevelEditDigests(const FLevelEditDigest& Before, const FLevelEditDigest& After, int32 MaxDifferences = 20);
}
