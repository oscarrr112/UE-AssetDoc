// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "LevelEditDigest.h"

#include "AssetDocumentEditorObjectProperties.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelEditTypes.h"
#include "LevelEditValues.h"
#include "LevelWorldIndex.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AssetDocumentLevel::Private
{
namespace ObjectProperties = UE::AssetDocument::EditorObjectProperties;

namespace
{

FString ReadText(UObject& Object, const TCHAR* Property)
{
	FLevelValue Value;
	return ObjectProperties::Read(Object, Property, Value) ? LevelValueToText(Value) : FString(TEXT("<unreadable>"));
}

AActor* FindLiveActor(const FString& Path)
{
	AActor* Actor = Cast<AActor>(FSoftObjectPath(Path).ResolveObject());
	return Actor != nullptr && IsListedActor(*Actor) ? Actor : nullptr;
}

void CaptureActor(const FString& Path, TMap<FString, FString>& Entries)
{
	const FString Key = TEXT("actor:") + Path;
	AActor* Actor = FindLiveActor(Path);
	if (Actor == nullptr)
	{
		Entries.Add(Key, TEXT("<absent>"));
		return;
	}
	Entries.Add(Key, Actor->GetClass()->GetPathName());
	Entries.Add(Key + TEXT("/label"), Actor->GetActorLabel(false));
	const FName Folder = Actor->GetFolderPath();
	Entries.Add(Key + TEXT("/folder"), Folder.IsNone() ? FString() : Folder.ToString());
	const AActor* Parent = Actor->GetAttachParentActor();
	Entries.Add(Key + TEXT("/parent"), Parent != nullptr
		? GetActorPath(*Parent) + TEXT(" ") + Actor->GetAttachParentSocketName().ToString() : FString());
	if (USceneComponent* Root = Actor->GetRootComponent())
	{
		Entries.Add(Key + TEXT("/transform"), ReadText(*Root, TEXT("RelativeLocation")) + TEXT(" ")
			+ ReadText(*Root, TEXT("RelativeRotation")) + TEXT(" ") + ReadText(*Root, TEXT("RelativeScale3D")));
	}
	TArray<FString> Tags;
	for (const FName& Tag : Actor->Tags)
	{
		Tags.Add(Tag.ToString());
	}
	Entries.Add(Key + TEXT("/tags"), FString::Join(Tags, TEXT(",")));
	TArray<FString> Layers;
	for (const FName& Layer : Actor->Layers)
	{
		Layers.Add(Layer.ToString());
	}
	Entries.Add(Key + TEXT("/layers"), FString::Join(Layers, TEXT(",")));
	TArray<FString> DataLayers = GetActorDataLayers(*Actor);
	DataLayers.Sort();
	Entries.Add(Key + TEXT("/data_layers"), FString::Join(DataLayers, TEXT(",")));
	TArray<FString> Components;
	TInlineComponentArray<UActorComponent*> ActorComponents(Actor);
	for (UActorComponent* Component : ActorComponents)
	{
		if (Component == nullptr || !IsValid(Component) || Component->IsVisualizationComponent())
		{
			continue;
		}
		const USceneComponent* Scene = Cast<USceneComponent>(Component);
		const USceneComponent* AttachParent = Scene != nullptr ? Scene->GetAttachParent() : nullptr;
		Components.Add(FString::Printf(TEXT("%s:%s:%s:%s"), *Component->GetName(), *Component->GetClass()->GetName(),
			AttachParent != nullptr ? *AttachParent->GetName() : TEXT(""),
			Scene != nullptr && !Scene->GetAttachSocketName().IsNone() ? *Scene->GetAttachSocketName().ToString() : TEXT("")));
	}
	Components.Sort();
	Entries.Add(Key + TEXT("/components"), FString::Join(Components, TEXT(",")));
}
}

FLevelEditDigest CaptureLevelEditDigest(UWorld& World, const FLevelEditDigestScope& Scope)
{
	FLevelEditDigest Digest;
	for (const FString& Path : Scope.ActorPaths)
	{
		CaptureActor(Path, Digest.Entries);
	}
	for (const FLevelDigestValue& Value : Scope.Values)
	{
		const FString Key = FString::Printf(TEXT("value:%s/%s/%s"), *Value.ActorPath, *Value.Component, *Value.Property);
		AActor* Actor = FindLiveActor(Value.ActorPath);
		UObject* Owner = Actor == nullptr ? nullptr
			: Value.Component.IsEmpty() ? static_cast<UObject*>(Actor) : FindLevelActorComponent(*Actor, Value.Component);
		Digest.Entries.Add(Key, Owner != nullptr ? ReadText(*Owner, *Value.Property) : FString(TEXT("<absent>")));
	}
	for (ULevel* Level : GetLoadedLevels(World))
	{
		const FString Package = GetLevelPackageName(*Level);
		if (!Scope.LevelPackages.Contains(Package))
		{
			continue;
		}
		TArray<FString> Paths;
		for (AActor* Actor : Level->Actors)
		{
			if (Actor != nullptr && IsListedActor(*Actor))
			{
				Paths.Add(GetActorPath(*Actor));
			}
		}
		Paths.Sort();
		Digest.Entries.Add(TEXT("level:") + Package, FString::Join(Paths, TEXT("\n")));
	}
	if (Scope.bFolders)
	{
		TArray<FString> Folders = GatherLevelFolders(World).Array();
		Folders.Sort();
		Digest.Entries.Add(TEXT("folders"), FString::Join(Folders, TEXT("\n")));
	}
	Digest.Entries.KeySort(TLess<FString>());
	return Digest;
}

TArray<FString> CompareLevelEditDigests(const FLevelEditDigest& Before, const FLevelEditDigest& After, int32 MaxDifferences)
{
	TArray<FString> Keys;
	Before.Entries.GetKeys(Keys);
	for (const TPair<FString, FString>& Entry : After.Entries)
	{
		Keys.AddUnique(Entry.Key);
	}
	Keys.Sort();
	TArray<FString> Differences;
	for (const FString& Key : Keys)
	{
		const FString* Old = Before.Entries.Find(Key);
		const FString* New = After.Entries.Find(Key);
		if (Old != nullptr && New != nullptr && Old->Equals(*New, ESearchCase::CaseSensitive))
		{
			continue;
		}
		if (Key.StartsWith(TEXT("level:")) && Old != nullptr && New != nullptr)
		{
			TArray<FString> OldPaths;
			TArray<FString> NewPaths;
			Old->ParseIntoArray(OldPaths, TEXT("\n"));
			New->ParseIntoArray(NewPaths, TEXT("\n"));
			TArray<FString> Added;
			TArray<FString> Removed;
			for (const FString& Path : NewPaths)
			{
				if (!OldPaths.Contains(Path))
				{
					Added.Add(Path);
				}
			}
			for (const FString& Path : OldPaths)
			{
				if (!NewPaths.Contains(Path))
				{
					Removed.Add(Path);
				}
			}
			Differences.Add(FString::Printf(TEXT("%s: leftover actors [%s], missing actors [%s]"), *Key,
				*FString::Join(Added, TEXT(", ")), *FString::Join(Removed, TEXT(", "))));
		}
		else
		{
			Differences.Add(FString::Printf(TEXT("%s: '%s' -> '%s'"), *Key, Old != nullptr ? **Old : TEXT("<none>"),
				New != nullptr ? **New : TEXT("<none>")));
		}
		if (Differences.Num() >= MaxDifferences)
		{
			break;
		}
	}
	return Differences;
}
}
