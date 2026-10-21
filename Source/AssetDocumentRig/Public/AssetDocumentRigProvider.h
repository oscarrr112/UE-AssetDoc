// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

// Rig asset primitives (provider id "rig", owner Rig): the documents of IK Rigs and IK Retargeters
// (profile.ik_rig.v1 / profile.ik_retargeter.v1). Same conventions as the data, graph and audio providers (one
// "asset" handle, value DTOs with schema_version 1, whole-package footprint, no typed inverse); the invocation
// plumbing is the graph module's table provider. rig.ikrig.read.v1 and rig.ikretargeter.read.v1 read the authored
// state as the IK Rig / IK Retargeter editors show it, rig.status.v1 runs the editors' processor initialization
// (their output log), rig.catalog.v1 lists solver and op types. For Control Rigs (profile.control_rig.v1),
// rig.controlrig.read.v1 reads the authored state as the Control Rig editor shows it and rig.controlrig.catalog.v1
// lists RigVM node / value types and rig modules. The runtime bootstrap assembles this provider; it is stateless
// (the Control Rig read keeps the records of its last paged read for the following pages).
ASSETDOCUMENTRIG_API TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentRigPrimitiveProvider();
