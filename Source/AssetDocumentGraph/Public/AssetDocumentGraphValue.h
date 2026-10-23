// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphAdapters.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

class FJsonValue;

namespace UE::AssetDocument::Graph
{
using FValue = FAssetDocumentRuntimeValueDto;

ASSETDOCUMENTGRAPH_API FValue MakeNull();
ASSETDOCUMENTGRAPH_API FValue MakeBool(bool bValue);
ASSETDOCUMENTGRAPH_API FValue MakeInt(int64 Value);
ASSETDOCUMENTGRAPH_API FValue MakeNumber(double Value);
ASSETDOCUMENTGRAPH_API FValue MakeString(FString Value);
ASSETDOCUMENTGRAPH_API FValue MakeArray(TArray<FValue> Elements);
ASSETDOCUMENTGRAPH_API FValue MakeStringArray(TConstArrayView<FString> Values);
ASSETDOCUMENTGRAPH_API FValue MakeEmptyObject();

class ASSETDOCUMENTGRAPH_API FObjectBuilder
{
public:
	FObjectBuilder& Add(FString Key, FValue Value);
	FObjectBuilder& AddString(FString Key, FString Value) { return Add(MoveTemp(Key), MakeString(MoveTemp(Value))); }
	FObjectBuilder& AddBool(FString Key, bool bValue) { return Add(MoveTemp(Key), MakeBool(bValue)); }
	FObjectBuilder& AddInt(FString Key, int64 Value) { return Add(MoveTemp(Key), MakeInt(Value)); }
	FValue Build();

private:
	FValue Object = MakeEmptyObject();
};

ASSETDOCUMENTGRAPH_API const FValue* FindField(const FValue& Object, FStringView Key);
ASSETDOCUMENTGRAPH_API int32 CountValueNodes(const FValue& Value);

// Strict reader for one DTO object. Every getter records a Graph.* error into
// the shared FAssetDocumentGraphError on the first failure and returns false.
class ASSETDOCUMENTGRAPH_API FReader
{
public:
	FReader(const FValue& InObject, FString InPath, FAssetDocumentGraphError& InError);

	bool IsObject() const;
	// Rejects any field outside Allowed (plus "schema_version", which must equal 1).
	bool OnlyFields(std::initializer_list<const TCHAR*> Allowed);
	bool Has(FStringView Key) const;
	const FValue* Find(FStringView Key) const;
	// Has() and the optional getters read a null as an absent field. Where a null would otherwise be dropped without
	// a word, this refuses it: true when Key is absent or not null, else Fail(Code, "<Key> is null: <Message>", Key).
	bool RefuseNull(FStringView Key, FStringView Code, FStringView Message);

	bool String(FStringView Key, FString& Out, bool bRequired = true);
	bool Bool(FStringView Key, bool& Out, bool bRequired = false);
	bool Number(FStringView Key, double& Out, bool bRequired = true);
	bool Int(FStringView Key, int64& Out, bool bRequired = true);
	bool Guid(FStringView Key, FGuid& Out, bool bRequired = true);
	bool Object(FStringView Key, const FValue*& Out, bool bRequired = true);
	bool Array(FStringView Key, const FValue*& Out, bool bRequired = true);
	FReader Child(FStringView Key, const FValue& Value) const;

	FString FieldPath(FStringView Key) const;
	bool Fail(FStringView Code, FStringView Message, FStringView Key = FStringView());

private:
	const FValue& Object_;
	FString Path;
	FAssetDocumentGraphError& Error;
};

ASSETDOCUMENTGRAPH_API bool TryParseGuid(FStringView Text, FGuid& OutGuid);

// JSON bridge used by tests and fixture capture.
ASSETDOCUMENTGRAPH_API TSharedPtr<FJsonValue> ValueToJson(const FValue& Value);
ASSETDOCUMENTGRAPH_API FValue JsonToValue(const TSharedPtr<FJsonValue>& Json);
ASSETDOCUMENTGRAPH_API bool TryParseJsonValue(const FString& Text, FValue& OutValue);
ASSETDOCUMENTGRAPH_API FString ValueToJsonString(const FValue& Value, bool bPretty = true);
} // namespace UE::AssetDocument::Graph
