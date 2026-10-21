// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

class FProperty;

// Float precision of reflected property values.
//
// A JSON number for a float property is the decimal an author means: like the
// Details panel, which parses "0.3" into the nearest float, the document value is
// converted to the nearest float32 and accepted. Documents carry that stored value
// as the shortest decimal that reads back as the same float (0.3, not
// 0.30000001192092896), so Extract output is readable and re-applies unchanged,
// and Diff / no-op / preflight / verify compare at the stored precision. Doubles
// hold every JSON number exactly and are left as written.
namespace UE::AssetDocument::EditorFloatPrecision
{
// The shortest decimal (as a double) that converts back to Value. Non-finite
// values are returned unchanged.
ASSETDOCUMENTEDITOR_API double ShortestFloatDecimal(float Value);

// Rewrites every float32 number inside Value (a document value of Property: struct
// members by authored name, instanced subobject / instanced struct Properties,
// array, set and map values, fixed-size arrays) to the shortest decimal of the
// float it is stored as. Values whose shape does not match the property are left
// as they are (the codec reports them). Returns the number of numbers changed.
ASSETDOCUMENTEDITOR_API int32 NormalizeValue(const FProperty& Property, FAssetDocumentRuntimeValueDto& Value);

// NormalizeValue for each entry of a document's top-level "Properties", the
// reflected properties of its "Class" (generic documents and the profiles that
// author Properties the same way). Documents without both, or whose class is not
// found, are left unchanged. Returns the number of numbers changed.
ASSETDOCUMENTEDITOR_API int32 NormalizeDocumentProperties(FAssetDocumentRuntimeValueDto& Document);
}
