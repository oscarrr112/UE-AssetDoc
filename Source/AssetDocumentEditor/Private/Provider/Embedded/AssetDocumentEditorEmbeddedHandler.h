// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Provider/AssetDocumentEditorPrimitiveHandler.h"

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorEmbeddedObjectCreate(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorEmbeddedPrimitiveBindings();
