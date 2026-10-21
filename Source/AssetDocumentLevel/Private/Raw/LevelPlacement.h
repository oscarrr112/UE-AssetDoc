// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UClass;
class ULevel;
class UObject;

// Raw path B1 (docs/architecture/level-editing-tools-design.md §5.3, decision D12):
// the editor's placement of an asset or actor class (Content Browser / Place Actors
// drag: FLevelEditorViewportClient::TryPlacingAssetObject and
// AssetSelection.cpp PlaceAssetUsingFactory) with an explicit transform and target
// level instead of the viewport cursor and the current level. Every other step of
// those functions is kept, in order. Selection is never changed.
// Behaviour test: UEAssetDoc.Level.Raw.Placement.
namespace UE::AssetDocumentLevel::Private
{
struct FLevelPlacementCheck
{
	bool bCanPlace = false;
	// The class the placed actor will have (the actor class, the Blueprint's generated
	// class, or the factory's actor class for an asset).
	UClass* ActorClass = nullptr;
	// Level.ClassNotPlaceable or Level.NoFactoryForAsset with a message when refused.
	FString Code;
	FString Message;
};

// What placing Asset would do, without placing anything: an actor class (UClass),
// a Blueprint, or any asset an actor factory accepts (static mesh, sound, ...).
FLevelPlacementCheck CheckLevelPlacement(UObject& Asset);

// Places Asset into Level at Transform; RF_Transactional, so it records into an
// open transaction. Null on failure (OutError says why).
AActor* PlaceInLevel(UObject& Asset, ULevel& Level, const FTransform& Transform, FString& OutError);
}
