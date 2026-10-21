// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Provider/AssetDocumentUMGPrimitiveHandler.h"

FAssetDocumentPrimitiveOutcome InvokeMovieScenePossessableAdd(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome InvokeMovieScenePossessableRemove(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome InvokeWidgetAnimationSequenceBind(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome InvokeWidgetAnimationSequenceUnbind(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);
