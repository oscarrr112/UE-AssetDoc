// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Provider/AssetDocumentUMGPrimitiveHandler.h"

FAssetDocumentPrimitiveOutcome InvokeWidgetAnimationQuery(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);
