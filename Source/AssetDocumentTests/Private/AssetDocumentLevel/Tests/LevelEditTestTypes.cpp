// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "LevelEditTestTypes.h"

#if WITH_EDITOR
void UAssetDocumentLevelTestComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Clamped = FMath::Clamp(Clamped, 0.0f, 10.0f);
	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

namespace
{
const FName TestSocketName(TEXT("Tip"));
const FTransform TestSocketTransform(FVector(0.0, 0.0, 50.0));
}

bool UAssetDocumentLevelTestSocketComponent::DoesSocketExist(FName InSocketName) const
{
	return InSocketName == TestSocketName || Super::DoesSocketExist(InSocketName);
}

FTransform UAssetDocumentLevelTestSocketComponent::GetSocketTransform(FName InSocketName, ERelativeTransformSpace TransformSpace) const
{
	if (InSocketName != TestSocketName)
	{
		return Super::GetSocketTransform(InSocketName, TransformSpace);
	}
	switch (TransformSpace)
	{
	case RTS_Component:
	case RTS_ParentBoneSpace:
		return TestSocketTransform;
	case RTS_Actor:
	{
		const FTransform World = TestSocketTransform * GetComponentTransform();
		const AActor* Owner = GetOwner();
		return Owner != nullptr ? World.GetRelativeTransform(Owner->GetTransform()) : World;
	}
	default:
		return TestSocketTransform * GetComponentTransform();
	}
}

void UAssetDocumentLevelTestSocketComponent::QuerySupportedSockets(TArray<FComponentSocketDescription>& OutSockets) const
{
	Super::QuerySupportedSockets(OutSockets);
	OutSockets.Add(FComponentSocketDescription(TestSocketName, EComponentSocketType::Socket));
}

AAssetDocumentLevelTestActor::AAssetDocumentLevelTestActor()
{
	TestComponent = CreateDefaultSubobject<UAssetDocumentLevelTestComponent>(TEXT("TestComponent"));
	RootComponent = TestComponent;
}

void AAssetDocumentLevelTestActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Score = FMath::Min(Score, 100.0f);
}

AAssetDocumentLevelTestBuiltActor::AAssetDocumentLevelTestBuiltActor()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AAssetDocumentLevelTestBuiltActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	USceneComponent* Built = NewObject<USceneComponent>(this, NAME_None, RF_Transactional);
	Built->CreationMethod = EComponentCreationMethod::UserConstructionScript;
	Built->SetupAttachment(GetRootComponent());
	Built->RegisterComponent();
}
