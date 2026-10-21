// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

struct FFloatCurve;

// Encodes an owned, handle-free v2 observation. Invalid native state publishes
// no partial snapshot. No authority to edit or restore a model is provided.
bool CaptureAnimationFloatCurveSnapshotV2(
    TConstArrayView<FFloatCurve> Curves,
    FAssetDocumentRuntimeValueDto& OutCurves);
