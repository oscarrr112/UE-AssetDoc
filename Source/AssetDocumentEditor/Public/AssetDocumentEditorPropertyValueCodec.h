// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"
#include "Templates/Function.h"

class FProperty;
class UEnum;
class UObject;

// The generic route's typed property value codec (the one editor.property.* uses for
// Properties), exposed for providers that store reflected values outside UObject
// properties: DataTable rows, curve table curves. One property value at ValuePtr
// (the property's own value memory, not its container). Codec ids and failure codes
// are the codec's own (TypedValueCodec.*).
//
// Values here have no owning UObject, so instanced subobjects (Instanced/EditInlineNew
// UObject members, at any depth) are not represented: the Details panel creates them
// only under an owning object (SPropertyEditorEditInline::OnClassPicked does nothing in
// a struct-only view such as the DataTable row editor), and a decoded subobject would
// be left in the transient package. Inspect reports such properties unsupported, and a
// FInstancedStruct value whose struct holds one fails to decode
// (TypedValueCodec.InstancedObjectNeedsOwner). Hard/soft references, FText, structs,
// containers and FInstancedStruct are represented as on the generic route.
namespace UE::AssetDocument::EditorPropertyValueCodec
{
// True when the codec represents Property; OutCodecId names its document form
// (bool.v1, integer.v1, number.v1, string.v1, name.v1, enum.v1, struct.v1, array.v1, ...).
ASSETDOCUMENTEDITOR_API bool Inspect(FProperty& Property, FString& OutCodecId);

ASSETDOCUMENTEDITOR_API bool Encode(
	FProperty& Property, const void* ValuePtr,
	FAssetDocumentRuntimeValueDto& OutValue, FString& OutDiagnosticCode);

// Decodes Value into ValuePtr as an edit of the value already there (struct
// members the value omits keep their current value; an edited FText keeps its
// key); ValuePtr is unchanged on failure.
ASSETDOCUMENTEDITOR_API bool Decode(
	FProperty& Property, const FAssetDocumentRuntimeValueDto& Value,
	void* ValuePtr, FString& OutDiagnosticCode);

// As above; Owner is the asset holding the value (its package keeps edited text
// ids, picker filter functions run on it).
ASSETDOCUMENTEDITOR_API bool Decode(
	FProperty& Property, const FAssetDocumentRuntimeValueDto& Value,
	void* ValuePtr, FString& OutDiagnosticCode, UObject* Owner);

// A sparse read (extract's diff_only): Value (Property's document form) without the parts equal to Base (the same
// property's document form of the value an edit of it starts from, e.g. the class default's). A struct keeps only the
// members that differ from Base's, recursively; an array element is pruned against Base's element at the same index,
// or, past Base's end, against a default element (the one a partial edit adds). Decoding the result as an edit of
// Base gives Value back. Maps, sets, static arrays, instanced values and atomic structs (vectors, rotators, colors,
// ...) stay whole. EncodeValue encodes one value of a property as the caller's Value and Base were encoded (the
// default element); the overload without it uses Encode. False (Value unchanged) when Value equals Base.
ASSETDOCUMENTEDITOR_API bool PruneToChanged(FProperty& Property, FAssetDocumentRuntimeValueDto& Value,
	const FAssetDocumentRuntimeValueDto& Base,
	TFunctionRef<bool(FProperty& Property, const void* ValuePtr, FAssetDocumentRuntimeValueDto& OutValue)> EncodeValue);
ASSETDOCUMENTEDITOR_API bool PruneToChanged(FProperty& Property, FAssetDocumentRuntimeValueDto& Value,
	const FAssetDocumentRuntimeValueDto& Base);

// The enumerator index an author's spelling names: the enumerator's name ("NewEnumerator2" or
// "E_State::NewEnumerator2"), its authored name (a User Defined Enum's display name, what the editor's
// dropdowns show) or its display name, ignoring case. INDEX_NONE when it names none, or several.
ASSETDOCUMENTEDITOR_API int32 FindEnumIndexBySpelling(const UEnum& Enum, const FString& Spelling);

// The enumerators an author may write, by authored name ("Melee, Ranged" for a User Defined Enum).
ASSETDOCUMENTEDITOR_API FString DescribeEnumSpellings(const UEnum& Enum);
}
