// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphValue.h"

// Details-panel writes of IK Rig settings structs and goal Details (AssetDocumentIKRigStructWrites.cpp).
namespace UE::AssetDocument::Graph::Rig
{
struct FStructWriteResult
{
	bool bChanged = false;
	// The fields written (struct writes only), in declaration order: the retargeter's struct viewer re-initializes
	// the op after an edit of some of them (UIKRigStructViewer::TriggerReinitIfNeeded).
	TArray<const FProperty*> Changed;
};

// Writes the settings object Op[Key] (complete sparse state: unlisted editable fields take their value in Defaults)
// into the struct of Type at Memory, as the IK Rig editor's struct wrapper does: Owner.Modify() (inside the caller's
// transaction), then the new values. Skip names fields owned by other document keys. Fails with
// IKRig.FieldNotEditable / IKRig.InvalidValue at Key/<field> and writes nothing.
bool WriteSettingsStruct(FReader& Op, const TCHAR* Key, const UScriptStruct& Type, void* Memory, const void* Defaults,
	UObject& Owner, TFunctionRef<bool(const FProperty&)> Skip, FStructWriteResult& OutResult);

// Writes the object Op[Key] (complete sparse state over Fields, unlisted ones take the class default) into Object as
// Details edits (PreEditChange / write / PostEditChangeProperty per changed field). DocumentKeys maps document keys to
// property names; a key it does not list fails with IKRig.FieldNotEditable.
bool WriteObjectFields(FReader& Op, const TCHAR* Key, UObject& Object, TConstArrayView<FName> Fields,
	const TMap<FString, FName>& DocumentKeys, FStructWriteResult& OutResult);
} // namespace UE::AssetDocument::Graph::Rig
