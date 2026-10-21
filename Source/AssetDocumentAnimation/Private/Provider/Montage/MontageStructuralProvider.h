// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Provider/Montage/MontageStructuralPreparation.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

FAssetDocumentPrimitiveDescriptor MakeAnimationMontageSlotsDescriptor();
FAssetDocumentPrimitiveDescriptor MakeAnimationMontageSectionsDescriptor();
bool IsMontageStructuralAdmissionShape(const FAssetDocumentPrimitiveDescriptor& Descriptor);
FAssetDocumentPrimitiveOutcome InvokeAnimationMontageStructural(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);

namespace AssetDocumentMontage
{
inline constexpr const TCHAR* SlotsPrimitive = TEXT("animation.montage.slot_tracks.replace.v1");
inline constexpr const TCHAR* SectionsPrimitive = TEXT("animation.montage.sections.mutate.v1");
// Closed canonical value shape; references resolve only already-loaded animation assets.
FAssetDocumentContractValidationResult ParseMontageStructuralArguments(
 const FAssetDocumentRuntimeValueDto& Value, FStringView PrimitiveId, FStructuralIntent& OutIntent);
}
