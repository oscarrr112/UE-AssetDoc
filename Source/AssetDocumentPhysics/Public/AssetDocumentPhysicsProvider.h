// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

// Physics asset primitives (provider id "physics", owner Physics): the documents of Physics Assets
// (profile.physics_asset.v1). Same conventions as the rig, data and audio providers (one "asset" handle, value DTOs
// with schema_version 1, whole-package footprint, no typed inverse). physics.asset.read.v1 reads the authored state
// as the Physics Asset Editor shows it, physics.status.v1 the data validation and structural checks, and
// physics.asset.edit.v1 makes the editor's edits. The runtime bootstrap assembles this provider; it is stateless.
ASSETDOCUMENTPHYSICS_API TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentPhysicsPrimitiveProvider();
