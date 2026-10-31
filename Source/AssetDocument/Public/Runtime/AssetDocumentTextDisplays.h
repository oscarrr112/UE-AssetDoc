// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

// The display strings of texts a read wrote in a localization form (LOCTABLE(...), INVTEXT(...), NSLOCTEXT(...)).
// A document written before a text was read in that form holds the text's display string instead; the Python
// runtime compares such a string as the same text (asset_document/text_forms.py), which needs the display string of
// a string-table reference. The Python host clears the record before each primitive call and hands it to the
// runtime after it. Game thread only.
namespace UE::AssetDocument::TextDisplays
{
// Records that the text written as Form displays as Display (nothing when they are equal).
ASSETDOCUMENT_API void Record(const FString& Form, const FString& Display);

// Forgets every record.
ASSETDOCUMENT_API void Reset();

// The records since the last Reset or Take, which are then forgotten.
ASSETDOCUMENT_API TMap<FString, FString> Take();
}
