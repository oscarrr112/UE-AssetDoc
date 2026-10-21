// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"

FAssetDocumentPrimitiveDescriptor MakeAnimationMontageProfileSnapshotDescriptor();
bool IsMontageProfileSnapshotAdmissionShape(const FAssetDocumentPrimitiveDescriptor& Descriptor);
ASSETDOCUMENTANIMATION_API FAssetDocumentPrimitiveOutcome InvokeAnimationMontageProfileSnapshot(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);

// Request owner supplies the exact path of its bound live target. The handle must
// still expose that very object; there is no raw-object observation override.
// Only an active owner-bound staged lease selects readonly temporary-root validation.
FAssetDocumentPrimitiveOutcome InvokeAnimationMontageProfileSnapshotBound(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction, const FString& ExpectedAssetPath, bool bStagedVerification = false);
