// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "CoreMinimal.h"

class UAnimationAsset;
namespace AssetDocumentAnimationSkeleton { struct FRetainedState; }
namespace AssetDocumentAnimationPreviewMesh
{
// Operation-local observation only. No authority to publish or restore any of
// the retained authored state, and no DTO/handle leaves this private owner.
struct FRetainedState
{
 explicit FRetainedState(const UAnimationAsset& Target);
 ~FRetainedState();
 bool Matches(const UAnimationAsset& Target) const;
private:
 struct FSequenceState;
 TUniquePtr<AssetDocumentAnimationSkeleton::FRetainedState> Montage;
 TUniquePtr<FSequenceState> Sequence;
};
}
