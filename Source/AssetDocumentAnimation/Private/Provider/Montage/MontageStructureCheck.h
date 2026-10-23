// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"

namespace AssetDocumentMontage
{
// Validate's Montage timeline check (also an early check of Apply's Preflight): the read-only preparation an apply makes
// of the same slots, sections, notifies and notify tracks (PrepareMontageStructuralEdit + CheckMontageNotifyPlan) on the
// live target, so Validate reports the segment, section, correspondence and notify diagnostics Apply would report.
// Input: {slots?, sections?, notifies?, notify_states?, notify_tracks?} in that order, the lowered values the replace
// primitives take (notify_tracks: [name]). Output: {} on success; the first diagnostic otherwise. No transaction, no write.
inline constexpr const TCHAR* StructureCheckPrimitive = TEXT("animation.montage.structure.check.v1");
FAssetDocumentPrimitiveDescriptor MakeMontageStructureCheckDescriptor();
FAssetDocumentPrimitiveOutcome InvokeMontageStructureCheck(const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction, const FString& ExpectedAssetPath);
}
