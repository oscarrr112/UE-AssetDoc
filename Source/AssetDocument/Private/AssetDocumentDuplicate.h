// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentTypes.h"

namespace UE::AssetDocument
{
// A package path under a mounted root other than /Game (engine and plugin content): it can be
// read (extract, inspect) and duplicated, never authored.
bool IsReadOnlyContentPath(const FString& PackageName);

// POST /assetdoc/assetdocument/duplicate: the Content Browser's Duplicate
// (IAssetTools::DuplicateAsset) of Request.Source into the absent /Game path Request.Target,
// then the editor's Save of the new package. An asset whose class registered a duplicate fixup
// (AssetDocumentDuplicateFixups.h) is copied by the same steps, with that fixup before the copy
// post-loads. Dependencies are not duplicated (the copy references what the source references).
// A map is copied with the external packages of its actors and objects (one file per actor; every
// actor of a World Partition map that is not open is loaded first, as the editor's Save As does);
// an open World Partition map is refused, as the Content Browser refuses it. A failure leaves no
// asset, package or file behind.
// Diagnostic codes: Duplicate.SourceNotFound, Duplicate.UnsupportedSource,
// Duplicate.InvalidTarget, Duplicate.TargetExists, Duplicate.Failed, Duplicate.SaveFailed.
FAssetDocumentResult DuplicateAsset(const FAssetDocumentDuplicateRequest& Request);
}
