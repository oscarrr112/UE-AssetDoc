// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

// Editor-equivalent raw edit: anim graph node property bindings ("Bind" on a pin
// in the details panel). The binding store, UAnimGraphNodeBinding_Base::PropertyBindings,
// is private to AnimGraph, so it is reached by reflection; the steps mirror
// UAnimGraphNodeBinding_Base::MakePropertyBindingWidget's OnAddBinding / OnRemoveBinding.
// A path whose last segment is a function binds the function (Type Function; the picker offers
// pure functions with no parameters and a compatible return value, OnCanBindFunction).
// Behaviour test: UEAssetDoc.AnimBlueprint.Adapters.NodeBindingRawEdit.

#include "CoreMinimal.h"
#include "AssetDocumentGraphAdapters.h"

class UAnimGraphNode_Base;

namespace UE::AssetDocument::AnimBlueprint::BindingRawEdits
{
// {"<PinProperty>": {"Path": "A.B" | "GetValue", "OnlyUpdateWhenActive"?: true}}; null when the node has no bindings.
FAssetDocumentRuntimeValueDto QueryNodeBindings(const UAnimGraphNode_Base& Node);

// Replaces the node's bindings with Value (an object as above; {} removes all).
bool SetNodeBindings(UAnimGraphNode_Base& Node, const FAssetDocumentRuntimeValueDto& Value, FAssetDocumentGraphError& OutError);
} // namespace UE::AssetDocument::AnimBlueprint::BindingRawEdits
