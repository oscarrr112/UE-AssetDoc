// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/PropertyAccessUtil.h"

class FProperty;
class UObject;

// Profile-declared property access (the UE editor's own write sequence without
// PropertyAccessUtil's CPF_Edit/BlueprintVisible permission gate).
//
// A Profile may declare serialized, non-edit properties (for example
// UCurveFloat::FloatCurve) as part of its authored surface. Admission still
// requires a serialized, non-EditConst property; the declaration only replaces
// the editor-visibility check. Writes run Modify/PreEditChange, copy the
// complete value and PostEditChangeChainProperty exactly like
// PropertyAccessUtil::SetPropertyValue_DirectComplete, skipping identical values.

// True when a property may be admitted by declaration: it is serialized
// (not Transient/DuplicateTransient/Deprecated/SkipSerialization) and not EditConst.
bool IsAssetDocumentEditorDeclarableProperty(const FProperty& Property);

// Copies the complete property value of a declarable property into OutValue.
bool GetAssetDocumentEditorDeclaredPropertyValue(
	const UObject& Object,
	const FProperty& Property,
	void* OutValue);

// Editor-equivalent write of a declarable property on a non-template object.
ASSETDOCUMENTEDITOR_API bool SetAssetDocumentEditorDeclaredPropertyValue(
	UObject& Object,
	const FProperty& Property,
	const void* Value,
	EPropertyAccessChangeNotifyMode NotifyMode);
