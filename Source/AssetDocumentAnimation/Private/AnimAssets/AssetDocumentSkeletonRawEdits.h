// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

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
//   ParkCurveMetaData / UnparkCurveMetaData: the Details panel's Asset User Data arrays with the curve metadata
//     element moved out of AssetUserData to the end of AssetUserDataEditorOnly and back to its index (each move is an
//     element removed and one added: Modify, PreEditChange, the array edit, PostEditChangeProperty(ArrayRemove /
//     ArrayAdd)). While it is parked a write of AssetUserData leaves it alone. Return false when the element at the
//     index (Park) or at the end of AssetUserDataEditorOnly (Unpark) is not a curve metadata object.
// Behaviour tests: UEAssetDoc.AnimationAssets.RawEdits.Skeleton, UEAssetDoc.Fidelity.Skeleton.AssetUserData.

#include "CoreMinimal.h"

class USkeleton;
class USkeletalMeshSocket;

namespace UE::AssetDocument::Animation::SkeletonEdits
{
void RemoveSocket(USkeleton& Skeleton, USkeletalMeshSocket& Socket);

// Field is RelativeLocation, RelativeRotation, RelativeScale or bForceAlwaysAnimated; Write sets the value.
bool SetSocketField(USkeletalMeshSocket& Socket, FName Field, TFunctionRef<void(USkeletalMeshSocket&)> Write);

// The curve metadata object's index in AssetUserData (INDEX_NONE: none there).
int32 FindCurveMetaData(const USkeleton& Skeleton);
bool ParkCurveMetaData(USkeleton& Skeleton, int32 Index);
bool UnparkCurveMetaData(USkeleton& Skeleton, int32 Index);
} // namespace UE::AssetDocument::Animation::SkeletonEdits
