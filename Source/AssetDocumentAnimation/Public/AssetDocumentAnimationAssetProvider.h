// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

// Standalone animation asset primitives (provider id "animation_asset", owner Animation): the
// Skeleton editor's slots, sockets, virtual bones, blend profiles, retargeting modes and curve
// metadata; the Blend Space editor's axes and samples (BlendSpace, BlendSpace1D, AimOffsetBlendSpace,
// AimOffsetBlendSpace1D); the Pose Asset editor's poses and additive settings; the Anim Composite
// editor's segments. Same conventions as the data provider (one "asset" handle, value DTOs with
// schema_version 1, whole-package footprint, no typed inverse: Core restores the package snapshot
// on rollback); the invocation plumbing is the graph module's table provider. Contract:
// docs/architecture/asset-document-animation-assets-matrix.md. Stateless; assembled by the runtime
// bootstrap.
ASSETDOCUMENTANIMATION_API TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentAnimationAssetPrimitiveProvider();
