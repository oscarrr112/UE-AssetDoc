// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "Provider/AssetDocumentUMGPrimitiveHandler.h"

FAssetDocumentPrimitiveOutcome InvokeDesignerBindingQuery(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome InvokeVariableNotifyLifecycle(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);
