// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Provider/AssetDocumentEditorPrimitiveHandler.h"

// editor.enhanced_input.check_domain.v1 (Preflight, read-only on the scratch): the Enhanced Input checks of
// the generic profile for an Input Action / Input Mapping Context, as Warning diagnostics on a successful
// outcome (the editor allows all of these): a Swizzle modifier that moves X away on a Boolean / Axis1D action,
// a mapping without an action, and Blueprint input-action nodes whose value pin type no longer matches the
// action's ValueType. The scratch holds the document's final state (after the property preflight writes).
FAssetDocumentPrimitiveOutcome
HandleAssetDocumentEditorEnhancedInputCheckDomain(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

// The warnings of the check above for one Input Action / Input Mapping Context (others: none). TargetPath is
// the asset the document edits (its Blueprint referencers are those of an edited Input Action).
TArray<FAssetDocumentRuntimeDiagnostic> CheckAssetDocumentEditorEnhancedInputDomain(
	const UObject& Asset,
	const FString& TargetPath);
