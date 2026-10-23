// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Provider/Montage/MontageStructuralOperations.h"
#include "Provider/Montage/MontageNotifyArguments.h"
#include "Runtime/AssetDocumentMutationPreparation.h"

namespace AssetDocumentMontage
{
struct FPreparedNotifyStaging;
TSharedPtr<const FPreparedStructuralEdit> GetMontageNotifyPreparedStructure(const FPreparedNotifyStaging& State);
// notifies.replace may replace both declared managed families in one capture/inverse/footprint;
// notify_states.replace remains state-only. Omitted family blocks preserve that family's events.
FAssetDocumentContractValidationResult PrepareMontageNotifyReplacement(
 UAnimMontage& Target, const FAssetDocumentRuntimeValueDto& FrozenArguments, bool bStates,
 const FAssetDocumentRuntimeRequestDto& Request, int32 StepIndex,
 FAssetDocumentPreparedMutationBatch& OutBatch, TSharedPtr<FPreparedNotifyStaging>& OutState);
// The notify tracks a document lists (notify_tracks: ["1", "Hit", ...], NotifyTracks[].Name): at least one, as the
// editor's notify panel keeps them; names are labels, a notify's TrackIndex is the track's position.
FAssetDocumentContractValidationResult ParseMontageNotifyTrackNames(const FAssetDocumentRuntimeValueDto& Value, TArray<FName>& OutNames);
// Read-only checks of a notify replacement on Target (after its prepared Structure, if any) with TrackCount notify tracks:
// every TrackIndex exists, every notify ends within the length, retained notifies keep their track, and one notify starts
// per track and time. Apply's preparation and Validate's structure check (animation.montage.structure.check.v1) share it.
FAssetDocumentContractValidationResult CheckMontageNotifyPlan(const UAnimMontage& Target, TConstArrayView<FNotifyEdit> Edits,
 const FPreparedStructuralEdit* Structure, int32 TrackCount, bool bTracksAuthored, bool bReplaceNotifies, bool bReplaceStates);
FAssetDocumentPrimitiveOutcome InvokeMontageNotifyReplacement(TSharedRef<FPreparedNotifyStaging> State,
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);
// One ordered class family per prepared operation. No event is attached here.
FAssetDocumentContractValidationResult PrepareMontageNotifyStaging(
 UAnimMontage& Target, TConstArrayView<UClass*> Classes, bool bStates,
 const FAssetDocumentRuntimeRequestDto& Request, int32 StepIndex,
 FAssetDocumentPreparedMutationBatch& OutBatch, TSharedPtr<FPreparedNotifyStaging>& OutState);
FAssetDocumentContractValidationResult StageMontageNotifyObjects(
 TSharedRef<FPreparedNotifyStaging> State, const IAssetDocumentVerifiedHandleLease& TargetLease,
 IAssetDocumentAtomicTransaction& Transaction, TArray<UObject*>& OutObjects);
#if WITH_DEV_AUTOMATION_TESTS
namespace Testing
{
FAssetDocumentContractValidationResult StageNative(TSharedRef<FPreparedNotifyStaging> State,
 IAssetDocumentAtomicTransaction& Transaction, TArray<UObject*>& OutObjects, int32 FailAfterObject = INDEX_NONE);
}
#endif
}
