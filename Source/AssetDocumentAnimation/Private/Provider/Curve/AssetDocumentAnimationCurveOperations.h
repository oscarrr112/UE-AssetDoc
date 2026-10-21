// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

FAssetDocumentPrimitiveOutcome BuildAnimationFloatCurveEnumeration(
	UObject& Target,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles);

FAssetDocumentPrimitiveOutcome InvokeAnimationFloatCurveEnumerate(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

// v2 is a complete value snapshot, with no Curve child handles.
FAssetDocumentPrimitiveOutcome BuildAnimationFloatCurveEnumerationV2(
    UObject& Target,
    const FAssetDocumentPrimitiveInvocation& Invocation);

FAssetDocumentPrimitiveOutcome InvokeAnimationFloatCurveEnumerateV2(
    const FAssetDocumentPrimitiveInvocation& Invocation,
    IAssetDocumentHandleResolver& Handles,
    IAssetDocumentAtomicTransaction* ActiveTransaction);
