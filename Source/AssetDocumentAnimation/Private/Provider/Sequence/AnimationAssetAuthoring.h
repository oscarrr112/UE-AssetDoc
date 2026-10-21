// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

// Authoring surfaces of a UAnimSequenceBase-derived asset that a human edits in
// the sequence editor: frame data, float curves, notify tracks, notifies, notify
// states, sync markers, preview mesh, hard object references, metadata and asset
// user data. Plain reflected fields go through the generic Editor property batch.
// The code is class-neutral over UAnimSequenceBase; it is admitted for exact
// UAnimSequence targets (UAnimMontage keeps its own primitives). The mutation is
// snapshot-covered: rollback is Core's package snapshot or the created-asset
// rollback; no inverse is written.
namespace AssetDocumentAnimationAuthoring
{
inline constexpr const TCHAR* SnapshotPrimitive = TEXT("animation.asset.authoring.snapshot.v1");
inline constexpr const TCHAR* EditPrimitive = TEXT("animation.asset.authoring.edit.v1");
bool IsPrimitive(FStringView PrimitiveId);
bool IsAdmittedClassPath(FStringView ClassPath);
}

FAssetDocumentPrimitiveDescriptor MakeAnimationAssetAuthoringSnapshotDescriptor();
FAssetDocumentPrimitiveDescriptor MakeAnimationAssetAuthoringEditDescriptor();

// Dispatch for the request-bound Animation owner. ExpectedTargetPath is the
// bound request's exact target; any other object is rejected.
FAssetDocumentPrimitiveOutcome InvokeAnimationAssetAuthoring(const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction, const FString& ExpectedTargetPath);
