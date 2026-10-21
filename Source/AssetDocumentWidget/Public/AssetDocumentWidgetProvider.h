// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

// Widget Blueprint primitives (provider id "umg", owner UMG): Designer widget tree,
// widget/slot/navigation properties, property bindings and widget animations of a
// UWidgetBlueprint. Same conventions as the graph and member providers (one "asset"
// handle, value DTOs with schema_version 1, whole-package footprint, no typed inverse);
// the invocation plumbing is the graph module's table provider. The runtime bootstrap
// assembles this provider; it is stateless.
ASSETDOCUMENTWIDGET_API TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentWidgetPrimitiveProvider();
