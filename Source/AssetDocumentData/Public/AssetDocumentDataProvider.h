// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

// Data table family primitives (provider id "data", owner Data): rows of a UDataTable
// (incl. UCompositeDataTable parents and UMirrorDataTable), curves of a UCurveTable
// (incl. UCompositeCurveTable parents) and entries of a UStringTable. Same conventions
// as the graph and widget providers (one "asset" handle, value DTOs with schema_version 1,
// whole-package footprint, no typed inverse: Core restores the package snapshot on
// rollback); the invocation plumbing is the graph module's table provider. The runtime
// bootstrap assembles this provider; it is stateless.
ASSETDOCUMENTDATA_API TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentDataPrimitiveProvider();
