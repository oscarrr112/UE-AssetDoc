// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Animation/AnimMontage.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

namespace AssetDocumentMontage
{
// Private native input, constructed by the future verified provider caller, never a JSON/ABI escape.
struct FSegmentEdit
{
 UAnimSequenceBase* Animation = nullptr;
 float Start = 0.f, SourceStart = 0.f, SourceEnd = 0.f, Rate = 1.f;
 int32 Loops = 1;
 // Operation-local correspondence to the frozen preimage, not persistent identity.
 FName PreviousSlot;
 int32 PreviousSegment = INDEX_NONE;
};
struct FSlotEdit
{
 FName Name;
 TArray<FSegmentEdit> Segments;
};
struct FSectionEdit
{
 FName Name, Next;
 float Time = 0.f;
 FName Slot;
 EAnimLinkMethod::Type Method = EAnimLinkMethod::Absolute;
};
// Private authoring context derived only from the composite's declared notify keys.
// This is not a serialized bypass or authority to remove unmanaged events.
enum class EManagedNotifyReplacement : uint8 { None = 0, Events = 1, States = 2, Both = 3 };
ENUM_CLASS_FLAGS(EManagedNotifyReplacement);
struct FStructuralIntent
{
 TOptional<TArray<FSlotEdit>> Slots;
 TOptional<TArray<FSectionEdit>> Sections;
 EManagedNotifyReplacement ReplaceManagedFamilies = EManagedNotifyReplacement::None;
};
struct FPreparedStructuralEdit;

// Read-only preparation. All referenced assets must already be loaded and remain pinned.
FAssetDocumentContractValidationResult PrepareMontageStructuralEdit(
 UAnimMontage& Target, const FStructuralIntent& Intent,
 const FGuid& RequestId, uint64 RuntimeRevision,
 TSharedPtr<FPreparedStructuralEdit>& OutPrepared);
TArray<FAssetDocumentMutationFootprint> GetMontageStructuralFootprint(const FPreparedStructuralEdit& Prepared);

// Handler-local forward plan; the real composition validates its prepared contribution
// before dispatch and the transaction adapter admits the exact footprint before any write.
FAssetDocumentContractValidationResult ApplyMontageStructuralEdit(
 TSharedRef<FPreparedStructuralEdit> Prepared,
 const IAssetDocumentVerifiedHandleLease& TargetLease,
 EAssetDocumentRuntimeRequestPhase Phase, IAssetDocumentAtomicTransaction& Transaction);

// Exact private leaf shared by standalone and combined authoring. Caller has already
// enlisted its footprint and owns the once-only bApplied decision. This function checks
// original baseline/token and any attached capture, writes only slots/length/sections,
// and never enlists. Composite callers attach the capture; the standalone handler
// plan is separately guarded by the existing prepared contribution validator.
FAssetDocumentContractValidationResult ApplyPreparedStructurePrefixNoEnlist(
 TSharedRef<FPreparedStructuralEdit> State, IAssetDocumentAtomicTransaction& Transaction, uint8 Failure = 0);
// Checks the desired structural domain only; the composite owns final mixed timeline readback.
FAssetDocumentContractValidationResult ReadbackPreparedStructure(const FPreparedStructuralEdit& State);

#if WITH_DEV_AUTOMATION_TESTS
namespace Testing
{
enum class EFailurePoint : uint8 { None, AfterSlots, AfterLength, AfterSections, AfterNotifies, AfterCompletion };
// Native fixture entry exercises the same operation; transport/lease coverage belongs to shared wiring.
FAssetDocumentContractValidationResult ApplyNative(
 TSharedRef<FPreparedStructuralEdit> Prepared, IAssetDocumentAtomicTransaction& Transaction,
 EFailurePoint Failure = EFailurePoint::None);
}
#endif
}
