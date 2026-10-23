// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Raw/LevelPlacement.h"

#include "ActorFactories/ActorFactory.h"
#include "AssetRegistry/AssetData.h"
#include "AssetSelection.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Elements/Actor/ActorElementData.h"
#include "Elements/Framework/TypedElementHandle.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Factories/AssetFactoryInterface.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Subsystems/PlacementSubsystem.h"

namespace UE::AssetDocumentLevel::Private
{
namespace
{
// UPlacementSubsystem::FindAssetFactoryFromAssetData for an asset; the actor class's
// factory (GEditor->FindActorFactoryForActorClass) for a class, as TryPlacingAssetObject.
TScriptInterface<IAssetFactoryInterface> FindFactory(UObject& Asset)
{
	if (UClass* Class = Cast<UClass>(&Asset))
	{
		return TScriptInterface<IAssetFactoryInterface>(GEditor->FindActorFactoryForActorClass(Class));
	}
	UPlacementSubsystem* Placement = GEditor->GetEditorSubsystem<UPlacementSubsystem>();
	return Placement != nullptr
		? Placement->FindAssetFactoryFromAssetData(FAssetData(&Asset, FAssetData::ECreationFlags::AllowBlueprintClass))
		: TScriptInterface<IAssetFactoryInterface>();
}

// AssetSelection.cpp PlaceAssetUsingFactory with ActorTransform = Transform and
// DesiredLevel = Level (no cursor snapping), bSelectResult = false.
TArray<AActor*> PlaceUsingFactory(UObject& Asset, const TScriptInterface<IAssetFactoryInterface>& Factory, ULevel& Level,
	const FTransform& Transform)
{
	TArray<AActor*> Placed;
	if (!Factory)
	{
		return Placed;
	}
	// PlaceAssetInCurrentLevel's check when a factory is given.
	if (!Factory->CanPlaceElementsFromAssetData(FAssetData(&Asset, FAssetData::ECreationFlags::AllowBlueprintClass)))
	{
		return Placed;
	}
	UActorFactory* ActorFactory = Cast<UActorFactory>(Factory.GetObject());
	const EObjectFlags ObjectFlags = RF_Transactional;
	TArray<FTransform> Transforms;
	Transforms.Add(Transform);
	if (!ActorPlacementUtils::IsLevelValidForActorPlacement(&Level, Transforms))
	{
		return Placed;
	}

	TArray<FTypedElementHandle> Items;
	{
		FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentLevel", "PlaceObject", "Place Object"));
		if (UPlacementSubsystem* Placement = GEditor->GetEditorSubsystem<UPlacementSubsystem>())
		{
			FAssetPlacementInfo Info;
			Info.AssetToPlace = FAssetData(&Asset);
			Info.PreferredLevel = &Level;
			Info.FinalizedTransform = Transform;
			Info.FactoryOverride = Factory;
			FPlacementOptions Options;
			Options.bIsCreatingPreviewElements = false;
			Items = Placement->PlaceAsset(Info, Options);
			for (const FTypedElementHandle& Item : Items)
			{
				if (AActor* Actor = ActorElementDataUtil::GetActorFromHandle(Item, true))
				{
					Actor->SetFlags(ObjectFlags);
					Placed.Add(Actor);
				}
			}
		}
		// The legacy fallback of PlaceAssetUsingFactory.
		if (Items.IsEmpty() && ActorFactory != nullptr)
		{
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.ObjectFlags = ObjectFlags;
			if (AActor* Actor = ActorFactory->CreateActor(&Asset, &Level, Transform, SpawnParameters))
			{
				Placed.Add(Actor);
			}
		}
	}
	for (AActor* Actor : Placed)
	{
		Actor->InvalidateLightingCache();
		Actor->PostEditChange();
		Actor->MarkPackageDirty();
	}
	if (!Placed.IsEmpty())
	{
		GEditor->RedrawLevelEditingViewports();
		ULevel::LevelDirtiedEvent.Broadcast();
	}
	return Placed;
}
}

FLevelPlacementCheck CheckLevelPlacement(UObject& Asset)
{
	FLevelPlacementCheck Check;
	if (UClass* Class = Cast<UClass>(&Asset))
	{
		if (!Class->IsChildOf(AActor::StaticClass()))
		{
			Check.Code = TEXT("Level.ClassNotPlaceable");
			Check.Message = FString::Printf(TEXT("%s is not an actor class."), *Class->GetPathName());
			return Check;
		}
		if (Class->HasAnyClassFlags(CLASS_NotPlaceable | CLASS_Abstract | CLASS_Deprecated))
		{
			Check.Code = TEXT("Level.ClassNotPlaceable");
			Check.Message = FString::Printf(TEXT("%s is %s, so it cannot be placed in a level."), *Class->GetPathName(),
				Class->HasAnyClassFlags(CLASS_Abstract) ? TEXT("abstract")
				: Class->HasAnyClassFlags(CLASS_Deprecated) ? TEXT("deprecated") : TEXT("marked NotPlaceable"));
			return Check;
		}
		Check.bCanPlace = true;
		Check.ActorClass = Class;
		return Check;
	}
	if (UBlueprint* Blueprint = Cast<UBlueprint>(&Asset))
	{
		UClass* Generated = Blueprint->GeneratedClass;
		if (Generated == nullptr || !Generated->IsChildOf(AActor::StaticClass())
			|| Generated->HasAnyClassFlags(CLASS_NotPlaceable | CLASS_Abstract))
		{
			Check.Code = TEXT("Level.ClassNotPlaceable");
			Check.Message = Generated == nullptr
				? FString::Printf(TEXT("Blueprint %s has no generated class (it never compiled); compile it first."), *Blueprint->GetPathName())
				: FString::Printf(TEXT("Blueprint %s (%s) is not a placeable actor Blueprint."), *Blueprint->GetPathName(), *Generated->GetPathName());
			return Check;
		}
	}
	const TScriptInterface<IAssetFactoryInterface> Factory = FindFactory(Asset);
	UActorFactory* ActorFactory = Cast<UActorFactory>(Factory.GetObject());
	if (!Factory)
	{
		Check.Code = TEXT("Level.NoFactoryForAsset");
		Check.Message = FString::Printf(TEXT("No placement factory accepts %s (a %s), so it cannot be placed in a level the way "
			"the Content Browser drag does. Spawn an actor class and set its asset property instead."), *Asset.GetPathName(),
			*Asset.GetClass()->GetName());
		return Check;
	}
	Check.bCanPlace = true;
	if (UBlueprint* Blueprint = Cast<UBlueprint>(&Asset))
	{
		Check.ActorClass = Blueprint->GeneratedClass;
	}
	else if (ActorFactory != nullptr)
	{
		Check.ActorClass = ActorFactory->GetDefaultActorClass(FAssetData(&Asset));
	}
	return Check;
}

AActor* PlaceInLevel(UObject& Asset, ULevel& Level, const FTransform& Transform, FString& OutError)
{
	TArray<AActor*> Placed;
	UObject* Placing = &Asset;
	if (UClass* Class = Cast<UClass>(&Asset); Class != nullptr && Class->IsChildOf(AActor::StaticClass()))
	{
		// TryPlacingAssetObject for a class: its factory, else AddActor.
		if (UActorFactory* Factory = GEditor->FindActorFactoryForActorClass(Class))
		{
			Placed = PlaceUsingFactory(Asset, TScriptInterface<IAssetFactoryInterface>(Factory), Level, Transform);
		}
		if (Placed.IsEmpty() && !Class->HasAnyClassFlags(CLASS_NotPlaceable | CLASS_Abstract))
		{
			if (AActor* Actor = GEditor->AddActor(&Level, Class, Transform, /*bSilent*/ true, RF_Transactional, /*bSelectActor*/ false))
			{
				Placed.Add(Actor);
			}
		}
	}
	else
	{
		// TryPlacingAssetObject for an asset (Blueprints included).
		bool bShouldPlace = true;
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(&Asset))
		{
			bShouldPlace = Blueprint->GeneratedClass != nullptr
				&& !Blueprint->GeneratedClass->HasAnyClassFlags(CLASS_NotPlaceable | CLASS_Abstract);
		}
		if (bShouldPlace)
		{
			Placed = PlaceUsingFactory(Asset, FindFactory(Asset), Level, Transform);
			for (AActor* Actor : Placed)
			{
				// "Not clear why this would be necessary, but kept for legacy behavior." (engine)
				Actor->PostEditMove(true);
			}
		}
	}
	if (!Placed.IsEmpty())
	{
		FEditorDelegates::OnNewActorsPlaced.Broadcast(Placing, Placed);
		return Placed[0];
	}
	OutError = FString::Printf(TEXT("The editor placed nothing for %s in %s."), *Asset.GetPathName(), *Level.GetOutermost()->GetName());
	return nullptr;
}
}
