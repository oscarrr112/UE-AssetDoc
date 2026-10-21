// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

// Raw edits of a USkeleton that the exported editor surface (IEditableSkeleton) does not cover.
// Each leaves the skeleton as one editor action leaves it; everything else the Skeleton
// primitives do goes through IEditableSkeleton or USkeleton's exported API.
//
//   RemoveSocket: FEditableSkeleton::HandleDeleteSockets for a skeleton socket (Skeleton->Modify,
//     Sockets.Remove, then the preview assets attached to that socket name are detached), without a
//     preview scene (no Persona viewport is involved). Takes the call's transaction.
//   SetSocketField: the socket Details panel (property handle write on a USkeletalMeshSocket):
//     SetFlags(RF_Transactional), Modify, PreEditChange(Property), value write,
//     PostEditChangeProperty(ValueSet) - the socket broadcasts its ChangedEvent from there.
// Behaviour test: UEAssetDoc.AnimationAssets.RawEdits.Skeleton.

#include "CoreMinimal.h"

class USkeleton;
class USkeletalMeshSocket;

namespace UE::AssetDocument::Animation::SkeletonEdits
{
void RemoveSocket(USkeleton& Skeleton, USkeletalMeshSocket& Socket);

// Field is RelativeLocation, RelativeRotation, RelativeScale or bForceAlwaysAnimated; Write sets the value.
bool SetSocketField(USkeletalMeshSocket& Socket, FName Field, TFunctionRef<void(USkeletalMeshSocket&)> Write);
} // namespace UE::AssetDocument::Animation::SkeletonEdits
