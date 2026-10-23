// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Runtime/AssetDocumentPrimitiveProvider.h"

class IPropertyHandle;
class IPropertyRowGenerator;
class ISinglePropertyView;
class FProperty;
class UObject;
class UBehaviorTree;
class UBehaviorTreeGraph;
class UEdGraph;

namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback
{
enum class EAuthoredGraphReadbackVersion : uint8 { V1 = 1, V2 = 2 };
ASSETDOCUMENTAI_API FAssetDocumentRuntimeValueDto StringValue(FString Value);
ASSETDOCUMENTAI_API FAssetDocumentRuntimeValueDto IntegerValue(int64 Value);
ASSETDOCUMENTAI_API FAssetDocumentRuntimeValueDto NumberValue(double Value);
ASSETDOCUMENTAI_API FAssetDocumentRuntimeValueDto BooleanValue(bool Value);
ASSETDOCUMENTAI_API FAssetDocumentRuntimeValueDto NullValue();
ASSETDOCUMENTAI_API FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values);
ASSETDOCUMENTAI_API FAssetDocumentRuntimeValueDto ArrayValue(TArray<FAssetDocumentRuntimeValueDto> Values);

ASSETDOCUMENTAI_API bool EncodeAuthoredCanonicalValue(const FAssetDocumentRuntimeValueDto& Value,
	TArray<uint8>& Bytes, int64 Limit = MAX_int32);
ASSETDOCUMENTAI_API bool BuildAuthoredGraphProjection(UBehaviorTree& Tree, FAssetDocumentRuntimeValueDto& OutValue, bool& Overflow,
	EAuthoredGraphReadbackVersion Version = EAuthoredGraphReadbackVersion::V1);
ASSETDOCUMENTAI_API bool BuildAuthoredGraphProjection(UBehaviorTree& ScratchTree, UBehaviorTreeGraph& ScratchGraph,
	FAssetDocumentRuntimeValueDto& OutValue, bool& Overflow,
	EAuthoredGraphReadbackVersion Version = EAuthoredGraphReadbackVersion::V1);
bool BuildAuthoredBoundGraphProjection(const UEdGraph& Graph, FAssetDocumentRuntimeValueDto& OutValue,
	bool& Overflow, EAuthoredGraphReadbackVersion Version);
void ConfigureReadOnlyValueOrRows(IPropertyRowGenerator& Rows);

bool ReadScalarProperty(
	const IPropertyHandle& Handle,
	FAssetDocumentRuntimeValueDto& OutValue);
// Budgets mirror the public RuntimeValue contract; final assembled trees are
// validated again because reference/struct envelopes also consume nodes.
struct FPropertyReadContext
{
	int32 RemainingNodes = 4096;
	bool bOverflow = false;
	EAuthoredGraphReadbackVersion Version = EAuthoredGraphReadbackVersion::V1;
	UObject* RootObject = nullptr;
	TSet<const UObject*> ActiveObjects;
};
bool IsAuthoredProperty(const IPropertyHandle& Handle);
// The stock detail tree omits class-hidden categories even when hidden-property
// visibility is enabled. Keep the standalone view alive with its exact handle.
// The conditional service exception is its actual bCallTickOnSearchStart bool.
struct FHiddenCategoryScalarHandle
{
	TSharedPtr<ISinglePropertyView> View;
	TSharedPtr<IPropertyHandle> Handle;
};
bool IsHiddenCategoryAuthoredScalar(const UObject& Object, const FProperty& Property);
bool MakeHiddenCategoryScalarHandle(UObject& Object, const FProperty& Property,
	FHiddenCategoryScalarHandle& Out);
bool ReadAuthoredProperty(IPropertyHandle& Handle, FPropertyReadContext& Context,
	int32 Depth, FAssetDocumentRuntimeValueDto& OutValue);
bool ReadAuthoredStruct(IPropertyHandle& Handle, FPropertyReadContext& Context,
	int32 Depth, FAssetDocumentRuntimeValueDto& OutValue);
bool ReadAuthoredContainer(IPropertyHandle& Handle, FPropertyReadContext& Context,
	int32 Depth, FAssetDocumentRuntimeValueDto& OutValue);
ASSETDOCUMENTAI_API bool BuildAuthoredProperties(
	UObject& Object,
	FAssetDocumentRuntimeValueDto& OutProperties, bool* OutOverflow = nullptr,
	EAuthoredGraphReadbackVersion Version = EAuthoredGraphReadbackVersion::V1);
}
