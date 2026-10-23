// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class AActor;

// Raw path B1 (docs/architecture/level-editing-tools-design.md §5.3): the body of
// UEditorEngine::DetachSelectedActors (UnrealEd/Private/EditorEngine.cpp) for one
// actor instead of the selection: the old parent is modified without dirtying its
// package, the root detaches keeping its world transform, and the actor (with its
// attached children) moves to the folder of the old parent's topmost attach parent.
// Selection is never changed. Behaviour test: UEAssetDoc.Level.Raw.Detach.
namespace UE::AssetDocumentLevel::Private
{
// False when the actor is not attached (nothing changes).
ASSETDOCUMENTLEVEL_API bool DetachLevelActor(AActor& Actor);
}
