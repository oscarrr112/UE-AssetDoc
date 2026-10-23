// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Provider/AssetDocumentUMGPrimitiveHandler.h"

FAssetDocumentPrimitiveOutcome InvokeAssetDocumentUMGFloatTrackAdd(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome InvokeAssetDocumentUMGTransformTrackAdd(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome InvokeAssetDocumentUMGTrackSectionRemove(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGMovieScenePrimitiveBindings();
