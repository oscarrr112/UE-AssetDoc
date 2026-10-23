// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// The "layers" and "data_layers" fields of spawn / duplicate / set: whole lists, as the
// Layers panel (ULayersSubsystem, non-partitioned levels; missing layers are created)
// and the Data Layers outliner (UDataLayerEditorSubsystem, World Partition) assign
// them (design §2.3, §3.7, §5.2).

#include "DataLayer/DataLayerEditorSubsystem.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Layers/LayersSubsystem.h"
#include "LevelEditTypes.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"

namespace UE::AssetDocumentLevel::Private
{
namespace
{
TSharedPtr<FJsonValue> LayerStringsJson(TArray<FString> Values)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& Value : Values)
	{
		Out.Add(MakeShared<FJsonValueString>(Value));
	}
	return MakeShared<FJsonValueArray>(Out);
}

TArray<FString> LayerNames(const AActor& Actor)
{
	TArray<FString> Names;
	for (const FName& Layer : Actor.Layers)
	{
		Names.Add(Layer.ToString());
	}
	Names.Sort();
	return Names;
}

// A data layer as the result shows it: its asset path, or the instance's name for a
// data layer without an asset of its own (private data layers).
FString DescribeDataLayer(const UDataLayerInstance& Instance)
{
	const UDataLayerAsset* Asset = Instance.GetAsset();
	return Asset != nullptr && Asset->IsAsset() ? Asset->GetPathName() : Instance.GetDataLayerShortName();
}

TArray<FString> DataLayerNames(const AActor& Actor)
{
	TArray<FString> Names;
	for (const UDataLayerInstance* Instance : Actor.GetDataLayerInstances())
	{
		if (Instance != nullptr)
		{
			Names.AddUnique(DescribeDataLayer(*Instance));
		}
	}
	Names.Sort();
	return Names;
}

TSharedRef<FJsonObject> KnownDetails(const TArray<FString>& Known)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Name : Known)
	{
		Values.Add(MakeShared<FJsonValueString>(Name));
	}
	TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(TEXT("data_layers"), Values);
	return Details;
}
}

void FLevelEditPlan::PreflightLayers(int32 ActorIndex, const FLevelActorFieldEdits& Edits)
{
	const FLevelPlanActor& Entry = Actors[ActorIndex];
	const FString Display = Entry.Display();
	AActor* Live = Entry.bExisting ? Entry.Actor.Get() : nullptr;
	const ULevel* Level = Entry.Level.Get();
	auto AddListChange = [this, &Display](const FString& Op, const TCHAR* Object, TSharedPtr<FJsonValue> Before, TArray<FString> After)
	{
		FLevelChange& Change = Changes.AddDefaulted_GetRef();
		Change.Op = Op;
		Change.Kind = TEXT("Replace");
		Change.Object = Object;
		Change.Actor = Display;
		Change.Before = MoveTemp(Before);
		After.Sort();
		Change.After = LayerStringsJson(MoveTemp(After));
	};

	if (Edits.Layers.IsSet())
	{
		const AActor* Default = Entry.Class.IsValid() ? Entry.Class->GetDefaultObject<AActor>() : nullptr;
		const bool bSupported = Live != nullptr ? Live->SupportsLayers()
			: Level != nullptr && !Level->bIsPartitioned && Default != nullptr && !Default->bHiddenEd;
		if (!bSupported)
		{
			AddError(Edits.LayersPointer, TEXT("Level.LayersUnavailable"), FString::Printf(TEXT("%s cannot be in layers: %s"), *Display,
				Level != nullptr && Level->bIsPartitioned ? TEXT("World Partition levels use data layers (\"data_layers\") instead of layers.")
				: TEXT("the Layers panel does not take this actor (hidden in the editor, or part of a Level Instance).")));
		}
		else
		{
			AddListChange(Edits.LayersPointer, TEXT("layers"), Live != nullptr ? LayerStringsJson(LayerNames(*Live)) : nullptr,
				Edits.Layers.GetValue());
		}
	}

	if (Edits.DataLayers.IsSet())
	{
		if (!World.IsPartitionedWorld() || (Level != nullptr && !Level->bIsPartitioned))
		{
			AddError(Edits.DataLayersPointer, TEXT("Level.DataLayersUnavailable"), FString::Printf(TEXT("%s is not in a World Partition "
				"level, so it has no data layers (use \"layers\" for a non-partitioned level)."), *Display));
			return;
		}
		TArray<UDataLayerInstance*> Instances;
		TArray<FString> Names;
		for (int32 Position = 0; Position < Edits.DataLayers->Num(); ++Position)
		{
			FString Error;
			TArray<FString> Known;
			UDataLayerInstance* Instance = FindLevelDataLayerInstance(World, (*Edits.DataLayers)[Position], Error, &Known);
			if (Instance == nullptr)
			{
				AddError(FString::Printf(TEXT("%s/%d"), *Edits.DataLayersPointer, Position), TEXT("Level.DataLayerNotFound"), Error,
					KnownDetails(Known));
				continue;
			}
			Instances.AddUnique(Instance);
			Names.AddUnique(DescribeDataLayer(*Instance));
		}
		UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
		if (Live != nullptr && Subsystem != nullptr && Instances.Num() == Edits.DataLayers->Num()
			&& !Subsystem->IsActorValidForDataLayerInstances(Live, Instances))
		{
			AddError(Edits.DataLayersPointer, TEXT("Level.DataLayersUnavailable"), FString::Printf(TEXT("The editor does not put %s into "
				"these data layers (the actor cannot be in data layers, or a data layer is read-only)."), *Display));
			return;
		}
		AddListChange(Edits.DataLayersPointer, TEXT("data_layers"), Live != nullptr ? LayerStringsJson(DataLayerNames(*Live)) : nullptr,
			MoveTemp(Names));
	}
}

bool FLevelEditApply::ApplyLayers(const FString& ActorPath, const FLevelActorFieldEdits& Edits)
{
	if (Edits.Layers.IsSet())
	{
		AActor* Actor = ResolvePath(ActorPath);
		ULayersSubsystem* Layers = GEditor != nullptr ? GEditor->GetEditorSubsystem<ULayersSubsystem>() : nullptr;
		if (Actor == nullptr || Layers == nullptr || !Layers->IsActorValidForLayer(Actor))
		{
			AddError(Edits.LayersPointer, TEXT("Level.LayersUnavailable"), FString::Printf(TEXT("%s cannot be in layers."), *ActorPath));
			return false;
		}
		const TArray<FString> Before = LayerNames(*Actor);
		TArray<FName> Remove;
		for (const FName& Layer : Actor->Layers)
		{
			if (!Edits.Layers->Contains(Layer.ToString()))
			{
				Remove.AddUnique(Layer);
			}
		}
		TArray<FName> Add;
		for (const FString& Layer : Edits.Layers.GetValue())
		{
			if (!Actor->Layers.Contains(FName(*Layer)))
			{
				Add.Add(FName(*Layer));
			}
		}
		if (!Remove.IsEmpty())
		{
			Layers->RemoveActorFromLayers(Actor, Remove);
		}
		if (!Add.IsEmpty())
		{
			Layers->AddActorToLayers(Actor, Add);
		}
		TArray<FString> Wanted = Edits.Layers.GetValue();
		Wanted.Sort();
		if (Before != Wanted)
		{
			FLevelChange& Change = AddChange(Edits.LayersPointer, TEXT("Replace"), TEXT("layers"), ActorPath);
			Change.Before = LayerStringsJson(Before);
			Change.After = LayerStringsJson(Wanted);
			Touch(*Actor);
		}
		ListExpectations.RemoveAll([&ActorPath](const FListExpectation& Existing)
		{
			return Existing.ActorPath == ActorPath && Existing.Field == TEXT("layers");
		});
		ListExpectations.Add({ActorPath, TEXT("layers"), Wanted, Edits.LayersPointer});
	}

	if (Edits.DataLayers.IsSet())
	{
		AActor* Actor = ResolvePath(ActorPath);
		UWorld* World = GetWorld();
		UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
		if (Actor == nullptr || World == nullptr || Subsystem == nullptr)
		{
			AddError(Edits.DataLayersPointer, TEXT("Level.DataLayersUnavailable"), FString::Printf(TEXT("%s has no data layers."), *ActorPath));
			return false;
		}
		TArray<UDataLayerInstance*> Wanted;
		for (int32 Position = 0; Position < Edits.DataLayers->Num(); ++Position)
		{
			FString Error;
			UDataLayerInstance* Instance = FindLevelDataLayerInstance(*World, (*Edits.DataLayers)[Position], Error);
			if (Instance == nullptr)
			{
				AddError(FString::Printf(TEXT("%s/%d"), *Edits.DataLayersPointer, Position), TEXT("Level.DataLayerNotFound"), Error);
				return false;
			}
			Wanted.AddUnique(Instance);
		}
		if (!Subsystem->IsActorValidForDataLayerInstances(Actor, Wanted))
		{
			AddError(Edits.DataLayersPointer, TEXT("Level.DataLayersUnavailable"), FString::Printf(TEXT("The editor does not put %s into "
				"these data layers."), *ActorPath));
			return false;
		}
		const TArray<FString> Before = DataLayerNames(*Actor);
		for (const UDataLayerInstance* Current : Actor->GetDataLayerInstances())
		{
			if (Current != nullptr && !Wanted.Contains(Current))
			{
				Subsystem->RemoveActorFromDataLayer(Actor, const_cast<UDataLayerInstance*>(Current));
			}
		}
		const TArray<const UDataLayerInstance*> Remaining = Actor->GetDataLayerInstances();
		TArray<FString> WantedNames;
		for (UDataLayerInstance* Instance : Wanted)
		{
			WantedNames.AddUnique(DescribeDataLayer(*Instance));
			if (!Remaining.Contains(Instance))
			{
				Subsystem->AddActorToDataLayer(Actor, Instance);
			}
		}
		WantedNames.Sort();
		if (Before != WantedNames)
		{
			FLevelChange& Change = AddChange(Edits.DataLayersPointer, TEXT("Replace"), TEXT("data_layers"), ActorPath);
			Change.Before = LayerStringsJson(Before);
			Change.After = LayerStringsJson(WantedNames);
			Touch(*Actor);
		}
		ListExpectations.RemoveAll([&ActorPath](const FListExpectation& Existing)
		{
			return Existing.ActorPath == ActorPath && Existing.Field == TEXT("data_layers");
		});
		ListExpectations.Add({ActorPath, TEXT("data_layers"), WantedNames, Edits.DataLayersPointer});
	}
	return true;
}

bool FLevelEditApply::VerifyLists()
{
	const int32 Before = Diagnostics.Num();
	for (const FListExpectation& Expectation : ListExpectations)
	{
		AActor* Actor = DeletedPaths.Contains(Expectation.ActorPath) ? nullptr : ResolvePath(Expectation.ActorPath);
		if (Actor == nullptr)
		{
			continue;
		}
		const TArray<FString> Stored = Expectation.Field == TEXT("layers") ? LayerNames(*Actor) : DataLayerNames(*Actor);
		if (Stored != Expectation.Values)
		{
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetField(TEXT("expected"), LayerStringsJson(Expectation.Values));
			Details->SetField(TEXT("stored"), LayerStringsJson(Stored));
			AddError(Expectation.Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("The %s of %s are [%s] after the batch, "
				"not [%s]."), *Expectation.Field, *Expectation.ActorPath, *FString::Join(Stored, TEXT(", ")),
				*FString::Join(Expectation.Values, TEXT(", "))), Details);
		}
	}
	return Diagnostics.Num() == Before;
}
}
