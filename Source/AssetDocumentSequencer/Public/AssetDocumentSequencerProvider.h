// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

// Sequencer primitives (provider id "seq", owner Cinematics): the documents of Level Sequences
// (profile.level_sequence.v1). Same conventions as the rig, data and physics providers (one "asset" handle, value DTOs
// with schema_version 1, no typed inverse). Phase S1 is read only: seq.read.v1 reads the authored state as Sequencer
// shows it, seq.inspect.v1 the facts an author needs (locator resolution in the open editor world, animatable
// properties, channel catalogue, director endpoints, references, status) and seq.catalog.v1 the list_types kinds.
// The runtime bootstrap assembles this provider; it is stateless.
ASSETDOCUMENTSEQUENCER_API TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentSequencerPrimitiveProvider();
