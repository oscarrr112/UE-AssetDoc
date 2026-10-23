// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Animation Blueprint adapters of the generic graph provider (contract
// docs/architecture/asset-document-graph-primitives.md §5, AnimBlueprint addendum).
// Registered by FAssetDocumentAnimationModule into the public adapter registry:
//   - Anim schema adapter: UAnimationGraphSchema (AnimGraph, layers, state graphs),
//     UAnimationTransitionSchema and UAnimationConduitGraphSchema (rule graphs).
//     Keys "Anim:<Kind>" for anim graph nodes; K2 nodes delegate to the K2 adapter.
//   - State machine schema adapter: UAnimationStateMachineSchema, keys "SM:<Kind>".
//   - UAnimBlueprint host adapter (anim layers) and Blueprint settings adapter
//     (preview, sync groups, parent asset overrides, thumbnail).

#include "CoreMinimal.h"
#include "AssetDocumentGraphAdapters.h"

class UEdGraphSchema;

namespace UE::AssetDocument::AnimBlueprint
{
using FValue = FAssetDocumentRuntimeValueDto;

TSharedRef<IAssetDocumentGraphSchemaAdapter> MakeAnimGraphSchemaAdapter(UClass* SchemaClass);
TSharedRef<IAssetDocumentGraphSchemaAdapter> MakeStateMachineSchemaAdapter();
TSharedRef<IAssetDocumentGraphHostAdapter> MakeAnimBlueprintHostAdapter();
TSharedRef<IAssetDocumentBlueprintSettingsAdapter> MakeAnimBlueprintSettingsAdapter();
TSharedRef<IAssetDocumentBlueprintRebuildAdapter> MakeAnimBlueprintRebuildAdapter();

// The K2 adapter of the registry (anim graphs are K2 graphs underneath).
TSharedPtr<IAssetDocumentGraphSchemaAdapter> FindK2SchemaAdapter();

// Small value helpers (the provider's own helpers are private to AssetDocumentGraph).
inline FValue MakeStringValue(FString Text)
{
	FValue Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Value.StringValue = MoveTemp(Text);
	return Value;
}

inline FValue MakeBoolValue(bool bValue)
{
	FValue Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Value.BooleanValue = bValue;
	return Value;
}

inline FValue MakeNumberValue(double Number)
{
	FValue Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Number;
	Value.NumberValue = Number;
	return Value;
}

inline FValue MakeObjectValue()
{
	FValue Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	return Value;
}

inline FValue MakeArrayValue(TArray<FValue> Elements = {})
{
	FValue Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Array;
	Value.Elements = MoveTemp(Elements);
	return Value;
}

inline void SetField(FValue& Object, FString Key, FValue Field)
{
	const int32 Index = Object.ObjectKeys.IndexOfByKey(Key);
	if (Index != INDEX_NONE)
	{
		Object.Elements[Index] = MoveTemp(Field);
		return;
	}
	Object.ObjectKeys.Add(MoveTemp(Key));
	Object.Elements.Add(MoveTemp(Field));
}

inline const FValue* FindValueField(const FValue& Object, FStringView Key)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return nullptr;
	}
	for (int32 Index = 0; Index < Object.ObjectKeys.Num() && Index < Object.Elements.Num(); ++Index)
	{
		if (FStringView(Object.ObjectKeys[Index]) == Key)
		{
			return &Object.Elements[Index];
		}
	}
	return nullptr;
}

inline bool ReadNumber(const FValue& Value, double& Out)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Number)
	{
		Out = Value.NumberValue;
		return true;
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
	{
		Out = static_cast<double>(Value.IntegerValue);
		return true;
	}
	return false;
}
} // namespace UE::AssetDocument::AnimBlueprint
