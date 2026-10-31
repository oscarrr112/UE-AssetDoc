// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class UObject;

// The comparison of a marker's "Text" (the engine's export text of a value the document has no form for) with the text
// the current value reads as. Text names objects by path, and a path into the asset itself depends on where the asset
// is: the asset after a move, and Validate's copy of it (/Engine/Transient.<Name> or <Name>_<N>, or a staged package
// under /Temp/AssetDocument/ValidateTrial/), read the same content with another path. Both texts are compared with
// those paths named as the asset's own path names them; a path into any other asset or a native type
// (/Script/Engine.Actor:Tags) is compared as it is. The Python runtime compares markers by the same rule
// (asset_document/markers.py rebase_text).
namespace UE::AssetDocument::EditorMarkerText
{
// Text with every object path into the asset AssetPath ("/Game/A/B.B") named as AssetPath names it: the asset's own
// package in any folder ("/Game/Old/B.B:Inner" reads "/Game/A/B.B:Inner"), and Validate's copy of it
// ("/Engine/Transient.B_1:Inner" reads "/Game/A/B.B:Inner"). Text is returned as it is when AssetPath is empty.
ASSETDOCUMENTEDITOR_API FString Rebase(const FString& Text, const FString& AssetPath);

// The asset path marker text of a value held by Within (an object of an asset, or of Validate's copy of one) is
// compared under: the asset's path ("/Game/A/B.B"; the copied asset's for a trial copy). Empty when Within is null or
// a native object.
ASSETDOCUMENTEDITOR_API FString GetAssetPath(const UObject* Within);

// Whether the authored marker text is the text the current value held by Within reads as.
ASSETDOCUMENTEDITOR_API bool IsSameText(const FString& Authored, const FString& Current, const UObject* Within);
}
