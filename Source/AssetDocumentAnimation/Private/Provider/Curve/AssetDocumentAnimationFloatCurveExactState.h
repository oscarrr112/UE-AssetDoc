// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once

#include "Provider/Curve/AssetDocumentAnimationFloatCurveState.h"

class UAnimSequenceBase;

bool IsExactAnimationFloatCurveTarget(UAnimSequenceBase& Asset);
bool MatchesAnimationFloatCurveState(UAnimSequenceBase& Asset,
    TConstArrayView<FAssetDocumentAnimationFloatCurveState> Desired);

// Private C0 authority exception. Only the prepared forward and typed inverse
// call this after complete preimage capture and Core footprint enlistment.
// Handles and mutable members never escape this leaf.
bool PublishExactFloatCurveState(UAnimSequenceBase& Asset,
    TConstArrayView<FAssetDocumentAnimationFloatCurveState> Desired);

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAnimationFloatCurveExactStateTesting
{
struct FHooks
{
    int32 PublishCalls = 0;
    int32 OpenBrackets = 0;
    int32 CloseBrackets = 0;
    int32 ExactPatches = 0;
    bool bFailAfterFirstPatch = false;
    bool bFailAfterControllerKeys = false;
    TFunction<bool(UAnimSequenceBase&)> ShouldFailAfterFirstPatch;
};
void SetHooks(FHooks* Hooks);
FHooks* GetHooks();
}
#endif
