// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

// Niagara primitives (provider id "fx", owner FX): reads of Niagara systems, emitters, scripts
// and parameter collections as the Niagara editors show them (system and emitter stacks through
// the editor's own stack view models). Same conventions as the graph and data providers (one
// "asset" handle, value DTOs with schema_version 1); the invocation plumbing is the graph module's
// table provider. The runtime bootstrap assembles this provider; it is stateless.
ASSETDOCUMENTNIAGARA_API TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentNiagaraPrimitiveProvider();
