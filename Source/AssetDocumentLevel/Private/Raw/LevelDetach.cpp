// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Raw/LevelDetach.h"

#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

namespace UE::AssetDocumentLevel::Private
{
bool DetachLevelActor(AActor& Actor)
{
	USceneComponent* Root = Actor.GetRootComponent();
	if (Root == nullptr || Root->GetAttachParent() == nullptr)
	{
		return false;
	}
	const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentLevel", "DetachActors", "Detach actors"));
	AActor* OldParent = Root->GetAttachParent()->GetOwner();
	if (OldParent != nullptr)
	{
		OldParent->Modify(false);
	}
	Root->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	if (OldParent != nullptr)
	{
		// The detached actor moves into the outliner folder of the old parent's top-most attach ancestor.
		const AActor* TopMost = OldParent;
		while (const AActor* Next = TopMost->GetAttachParentActor())
		{
			TopMost = Next;
		}
		Actor.SetFolderPath_Recursively(TopMost->GetFolderPath());
	}
	return true;
}
}
