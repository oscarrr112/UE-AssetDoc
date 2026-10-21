// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Raw path B1 (design §5.3, risk R4): Raw/LevelPlacement places like the editor's
// official scripting path UEditorActorSubsystem::SpawnActorFromObject (the drag path
// with the viewport cursor), but at an explicit transform and level. Compared on the
// same assets: class, components, flags, package dirtiness, folder, Undo.

#include "Components/StaticMeshComponent.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "LevelEditTestSupport.h"
#include "Raw/LevelPlacement.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentLevelRawPlacementTests
{
FString ComponentSummary(AActor& Actor)
{
	TArray<FString> Lines;
	TInlineComponentArray<UActorComponent*> Components(&Actor);
	for (UActorComponent* Component : Components)
	{
		if (Component != nullptr && !Component->IsVisualizationComponent())
		{
			Lines.Add(FString::Printf(TEXT("%s:%s:%d"), *Component->GetName(), *Component->GetClass()->GetName(),
				static_cast<int32>(Component->CreationMethod)));
		}
	}
	Lines.Sort();
	return FString::Join(Lines, TEXT(","));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentLevelRawPlacementTest,
	"UEAssetDoc.Level.Raw.Placement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentLevelRawPlacementTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentLevelEditTests;
	using namespace AssetDocumentLevelRawPlacementTests;
	FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	ULevel* Sub = World != nullptr ? Maps.AddSublevel(*World, TEXT("Sub")) : nullptr;
	UEditorActorSubsystem* Actors = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
	if (!TestNotNull(TEXT("map with a sublevel"), Sub) || !TestNotNull(TEXT("actor subsystem"), Actors))
	{
		return false;
	}
	TestTrue(TEXT("saved, so dirtiness is observable"), FScopedTestMaps::SaveWorld(*World));
	ResetUndo();

	UObject* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UObject* LightClass = APointLight::StaticClass();
	const FVector Location(120, -40, 60);
	const FRotator Rotation(0, 30, 0);
	for (UObject* Asset : {Cube, LightClass})
	{
		const FString Stage = Asset->GetName();
		World->GetPackage()->SetDirtyFlag(false);
		Sub->GetPackage()->SetDirtyFlag(false);
		AActor* Official = Actors->SpawnActorFromObject(Asset, Location, Rotation);
		const int32 QueueAfterOfficial = UndoQueueLength();
		FString Error;
		AActor* Ours = nullptr;
		{
			const FScopedTransaction Transaction(FText::FromString(TEXT("Raw placement")));
			UE::AssetDocumentLevel::Private::PlaceInLevel(*Asset, *Sub, FTransform(Rotation, Location), Error);
			for (AActor* Actor : Sub->Actors)
			{
				if (Actor != nullptr && IsValid(Actor) && Actor->GetClass() == (Official != nullptr ? Official->GetClass() : nullptr))
				{
					Ours = Actor;
				}
			}
		}
		if (!TestNotNull(*(Stage + TEXT(": the official path placed")), Official) || !TestNotNull(*(Stage + TEXT(": ours placed ") + Error), Ours))
		{
			continue;
		}
		TestEqual(*(Stage + TEXT(": same class")), Ours->GetClass(), Official->GetClass());
		TestEqual(*(Stage + TEXT(": same components")), ComponentSummary(*Ours), ComponentSummary(*Official));
		TestEqual(*(Stage + TEXT(": same transactional flag")), Ours->HasAnyFlags(RF_Transactional), Official->HasAnyFlags(RF_Transactional));
		TestEqual(*(Stage + TEXT(": same folder")), Ours->GetFolderPath(), Official->GetFolderPath());
		TestEqual(*(Stage + TEXT(": same label")), Ours->GetActorLabel(false).Left(4), Official->GetActorLabel(false).Left(4));
		TestEqual(*(Stage + TEXT(": the official path uses the current level")), Official->GetLevel(), World->PersistentLevel.Get());
		TestEqual(*(Stage + TEXT(": ours the given level")), Ours->GetLevel(), Sub);
		TestTrue(*(Stage + TEXT(": the official path dirties its level")), World->GetPackage()->IsDirty());
		TestTrue(*(Stage + TEXT(": ours dirties its level")), Sub->GetPackage()->IsDirty());
		TestTrue(*(Stage + TEXT(": transform as given")), Ours->GetActorTransform().Equals(FTransform(Rotation, Location), 1e-4));
		TestFalse(*(Stage + TEXT(": not selected")), Ours->IsSelected());
		if (AStaticMeshActor* OfficialMesh = Cast<AStaticMeshActor>(Official))
		{
			TestEqual(*(Stage + TEXT(": same mesh")), Cast<AStaticMeshActor>(Ours)->GetStaticMeshComponent()->GetStaticMesh(),
				OfficialMesh->GetStaticMeshComponent()->GetStaticMesh());
		}
		TestEqual(*(Stage + TEXT(": one Undo entry each")), UndoQueueLength(), QueueAfterOfficial + 1);
		const FString OursPath = GetActorPath(*Ours);
		const FString OfficialPath = GetActorPath(*Official);
		TestTrue(*(Stage + TEXT(": Undo ours")), GEditor->UndoTransaction());
		TestNull(*(Stage + TEXT(": Undo removes ours")), FindActorByPath(OursPath));
		TestTrue(*(Stage + TEXT(": Undo the official")), GEditor->UndoTransaction());
		TestNull(*(Stage + TEXT(": Undo removes the official one")), FindActorByPath(OfficialPath));
	}

	// The check used by preflight agrees with what placement does.
	const UE::AssetDocumentLevel::Private::FLevelPlacementCheck CubeCheck = UE::AssetDocumentLevel::Private::CheckLevelPlacement(*Cube);
	TestTrue(TEXT("cube placeable"), CubeCheck.bCanPlace);
	TestEqual(TEXT("cube actor class"), CubeCheck.ActorClass, AStaticMeshActor::StaticClass());
	return true;
}

#endif
