// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

// Audio asset primitives (provider id "audio", owner Audio): the documents of MetaSound Sources and
// Patches (profile.metasound_source.v1 / profile.metasound_patch.v1). Same conventions as the data and
// graph providers (one "asset" handle, value DTOs with schema_version 1, whole-package footprint, no typed
// inverse); the invocation plumbing is the graph module's table provider. Phase M0 is read-only:
// audio.metasound.read.v1 (the document as the MetaSound editor shows it after opening) and
// audio.metasound.catalog.v1 (registered node classes, data types and interfaces). The runtime bootstrap
// assembles this provider; it is stateless.
ASSETDOCUMENTAUDIO_API TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentAudioPrimitiveProvider();
