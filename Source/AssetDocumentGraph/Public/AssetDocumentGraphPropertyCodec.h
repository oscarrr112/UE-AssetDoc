// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphValue.h"

class FProperty;
class UObject;

namespace UE::AssetDocument::Graph
{
// Typed JSON for node / template properties:
// bool -> bool, integers -> int, floats -> number, enums -> enumerator name,
// str/name/text -> string, object/class/soft refs -> path string or null,
// structs -> object of fields, arrays -> array, anything else -> UE text export string.
ASSETDOCUMENTGRAPH_API FValue EncodePropertyValue(const FProperty& Property, const void* ValuePtr);
ASSETDOCUMENTGRAPH_API bool DecodePropertyValue(const FProperty& Property, void* ValuePtr, const FValue& Value, FString& OutError);

// Every CPF_Edit property of Object (keyed by property name).
ASSETDOCUMENTGRAPH_API FValue EncodeEditableProperties(UObject& Object);

struct FResolvedPropertyPath
{
	FProperty* TopLevel = nullptr;
	FProperty* Leaf = nullptr;
	void* LeafPtr = nullptr;
};

// "Prop" or "StructProp.Field.SubField".
ASSETDOCUMENTGRAPH_API bool ResolvePropertyPath(UObject& Object, FStringView Path, FResolvedPropertyPath& Out, FString& OutError);

ASSETDOCUMENTGRAPH_API bool IsPropertyEditable(const FProperty& Property);
} // namespace UE::AssetDocument::Graph
