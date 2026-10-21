// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentPrimitiveRegistry.h"

struct FAssetDocumentEditorTargetLifecycleCatalogEntry
{
 // Empty for the class-neutral generic factory row, which serves every class
 // that has no class-specific row.
 FString ExactAssetClassPath;
 FString LifecycleId;
 FString ScratchCategoryId;
 FString FactoryId;
 // Ordered: clone scratch, create scratch, create asset, rollback created.
 // Class-neutral rows leave RequiredByRouteId empty: the registering Profile's route owns them.
 TArray<FAssetDocumentPrimitiveRequirement> RequiredPrimitives;
 TArray<FAssetDocumentPrimitiveHandleArgumentSchema> CreateContextHandles;
 FString CreateContextExactClassPath;

 bool IsClassNeutral() const { return ExactAssetClassPath.IsEmpty(); }
};

ASSETDOCUMENTEDITOR_API TConstArrayView<FAssetDocumentEditorTargetLifecycleCatalogEntry>
GetAssetDocumentEditorTargetLifecycleCatalog();

// The lifecycle that owns ExactAssetClassPath in Catalog: its class-specific row
// when one exists, otherwise the class-neutral generic factory row. When
// LifecycleId is non-empty the selected row must also carry that id.
// Returns nullptr when nothing matches (or the class has ambiguous specific rows).
ASSETDOCUMENTEDITOR_API const FAssetDocumentEditorTargetLifecycleCatalogEntry*
FindAssetDocumentEditorTargetLifecycle(
 TConstArrayView<FAssetDocumentEditorTargetLifecycleCatalogEntry> Catalog,
 FStringView ExactAssetClassPath,
 FStringView LifecycleId = FStringView());

ASSETDOCUMENTEDITOR_API const FAssetDocumentEditorTargetLifecycleCatalogEntry*
FindAssetDocumentEditorTargetLifecycle(
 FStringView ExactAssetClassPath,
 FStringView LifecycleId = FStringView());

// True when Lifecycle can create a new ExactAssetClassPath asset: class-specific
// rows own their class; the generic row requires an editor factory for the class
// (see SelectAssetDocumentEditorGenericFactory).
ASSETDOCUMENTEDITOR_API bool CanAssetDocumentEditorTargetLifecycleCreate(
 const FAssetDocumentEditorTargetLifecycleCatalogEntry& Lifecycle,
 FStringView ExactAssetClassPath);
