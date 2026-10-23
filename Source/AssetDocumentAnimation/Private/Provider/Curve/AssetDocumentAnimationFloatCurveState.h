// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimCurveTypes.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

struct FAssetDocumentAnimationFloatCurveState
{
    FName Name;
    int32 Flags = 0;
    FLinearColor Color = FLinearColor::White;
    FString Comment;
    bool bHasDefault = false;
    float DefaultValue = MAX_flt;
    ERichCurveExtrapolation PreExtrapolation = RCCE_Constant;
    ERichCurveExtrapolation PostExtrapolation = RCCE_Constant;
    TArray<FRichCurveKey> Keys;
};

// Input is the snapshot curves array, not the enclosing {"curves": ...} object.
// Both functions clear OutStates on failure and publish only complete sets.
bool ParseAnimationFloatCurveStateV2(
    const FAssetDocumentRuntimeValueDto& Curves,
    TArray<FAssetDocumentAnimationFloatCurveState>& OutStates);

bool CaptureAnimationFloatCurveStateV2(
    TConstArrayView<FFloatCurve> Curves,
    TArray<FAssetDocumentAnimationFloatCurveState>& OutStates);

// Canonical typed state uses MAX_flt exactly when bHasDefault is false.
bool IsValidAnimationFloatCurveStateV2(
    TConstArrayView<FAssetDocumentAnimationFloatCurveState> States);

bool EqualAnimationFloatCurveStateV2(
    TConstArrayView<FAssetDocumentAnimationFloatCurveState> A,
    TConstArrayView<FAssetDocumentAnimationFloatCurveState> B);
