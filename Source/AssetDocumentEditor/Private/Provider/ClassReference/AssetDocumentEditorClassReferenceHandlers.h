// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Provider/AssetDocumentEditorPrimitiveHandler.h"

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorClassResolveCanonical(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorReferenceResolveAsset(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorClassReferencePrimitiveBindings();
