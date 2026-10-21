// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Montage/MontageStructureCheck.h"
#include "Provider/Montage/MontageNotifyStaging.h"
#include "Provider/Montage/MontageStructuralProvider.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentEditorNativeState.h"

namespace AssetDocumentMontage
{
namespace
{
FAssetDocumentPrimitiveOutcome MontageCheckFailure(const FAssetDocumentContractValidationResult& Valid)
{
 auto Out = MakeAnimationProviderFailure(TEXT("AnimationProvider.Montage.InvalidIntent"), TEXT("The Montage timeline check failed."));
 if (!Valid.Diagnostics.IsEmpty()) Out.Diagnostics = Valid.Diagnostics;
 return Out;
}
FAssetDocumentContractValidationResult CheckMontageTimeline(UAnimMontage& Target, const FAssetDocumentRuntimeValueDto& Structure,
 const FAssetDocumentRuntimeValueDto& Notify, const FAssetDocumentRuntimeValueDto* Tracks, const FAssetDocumentPrimitiveInvocation& Invocation)
{
 TArray<FName> TrackNames;
 if (Tracks)
 {
  auto Valid = ParseMontageNotifyTrackNames(*Tracks, TrackNames);
  if (!Valid.bValid) return Valid;
 }
 const bool bNotify = !Notify.ObjectKeys.IsEmpty();
 const bool bStates = bNotify && !Notify.ObjectKeys.Contains(TEXT("notifies"));
 const bool bReplaceStates = Notify.ObjectKeys.Contains(TEXT("notify_states"));
 TArray<FNotifyEdit> Edits;
 if (bNotify)
 {
  auto Valid = ParseMontageNotifyArguments(Notify, bStates, Edits);
  if (!Valid.bValid) return Valid;
 }
 TSharedPtr<FPreparedStructuralEdit> Prepared;
 if (!Structure.ObjectKeys.IsEmpty())
 {
  FStructuralIntent Intent;
  auto Valid = ParseMontageStructuralArguments(Structure, Structure.ObjectKeys.Contains(TEXT("slots")) ? SlotsPrimitive : SectionsPrimitive, Intent);
  if (!Valid.bValid) return Valid;
  Intent.ReplaceManagedFamilies = !bNotify ? EManagedNotifyReplacement::None : bStates ? EManagedNotifyReplacement::States
   : bReplaceStates ? EManagedNotifyReplacement::Both : EManagedNotifyReplacement::Events;
  Valid = PrepareMontageStructuralEdit(Target, Intent, Invocation.RequestId, Invocation.RuntimeRevision, Prepared);
  if (!Valid.bValid) return Valid;
 }
 if (!bNotify) return Result(true);
 // Apply adds the editor's track "1" to a Montage without tracks before placing notifies on it.
 const int32 TrackCount = Tracks ? TrackNames.Num()
  : Target.AnimNotifyTracks.IsEmpty() && !Edits.IsEmpty() ? 1 : Target.AnimNotifyTracks.Num();
 return CheckMontageNotifyPlan(Target, Edits, Prepared.Get(), TrackCount, Tracks != nullptr, !bStates, bReplaceStates);
}
}

FAssetDocumentPrimitiveDescriptor MakeMontageStructureCheckDescriptor()
{
 FAssetDocumentPrimitiveDescriptor D;
 D.PrimitiveId = StructureCheckPrimitive; D.Version = 1; D.ProviderId = TEXT("animation"); D.Owner = EAssetDocumentPrimitiveOwner::Animation;
 D.ExecutionThread = EAssetDocumentExecutionThread::GameThread; D.Access = EAssetDocumentPrimitiveAccess::ReadOnly;
 D.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Preflight};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {{TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, true}};
 D.InputSchemaId = FString(StructureCheckPrimitive) + TEXT(".input.v1"); D.OutputSchemaId = FString(StructureCheckPrimitive) + TEXT(".output.v1");
 D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.StableDiagnosticCodes = {TEXT("AnimationProvider.InvalidValueShape"), TEXT("AnimationProvider.InvalidHandlePayload"),
  TEXT("AnimationProvider.TargetClassMismatch"), TEXT("AnimationProvider.Montage.InvalidIntent"), TEXT("AnimationProvider.Montage.InvalidSegment"),
  TEXT("AnimationProvider.Montage.SegmentGap"), TEXT("AnimationProvider.Montage.SegmentCorrespondence"),
  TEXT("AnimationProvider.Montage.DependencyUnavailable"), TEXT("AnimationProvider.Montage.NotifyTrackMissing"),
  TEXT("AnimationProvider.Montage.NotifyBeyondLength"), TEXT("AnimationProvider.Montage.NotifyNeedsSlot"),
  TEXT("AnimationProvider.Montage.NotifyNeedsLength")};
 auto& A = D.Admission;
 A.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
 A.ExactEngineVersion = TEXT("5.7.4"); A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 A.PublicModule = TEXT("Engine;AssetDocumentEditor");
 A.PublicHeader = TEXT("Animation/AnimMontage.h;AssetDocumentEditorNativeState.h");
 A.ExportedAuthoritativeApi = TEXT("UAnimSequenceBase::GetPlayLength;FAnimSegment::GetLength;UAnimMontage::CalculateOffsetForNotify");
 A.AuthorityContextId = TEXT("animation.montage.structure-check.read-only.v1");
 A.PrimaryOperationId = StructureCheckPrimitive;
 A.IdentityLifetimeContractId = TEXT("animation.provider-lease.read-only.v1");
 A.TransactionContractId = TEXT("animation.read-only.no-active-transaction.v1");
 A.RollbackContractId = TEXT("animation.read-only.no-rollback.v1");
 A.CompletionContractId = TEXT("animation.montage.structure-check.prepared-plan.v1");
 A.SaveReloadEvidenceId = FString(StructureCheckPrimitive) + TEXT(".read-only-no-persistence.v1");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimMontage.Authoring.SegmentDiagnostics")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimMontage.Authoring.SegmentDiagnostics"),
  TEXT("UEAssetDoc.AnimMontage.Authoring.SegmentStartTolerance")};
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|montage-structure-check|prepared-plan|preflight.v1");
 return D;
}

FAssetDocumentPrimitiveOutcome InvokeMontageStructureCheck(const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction, const FString& ExpectedAssetPath)
{
 const auto& Value = Invocation.ValueArguments;
 FAssetDocumentRuntimeValueDto Structure, Notify;
 Structure.Kind = Notify.Kind = EAssetDocumentRuntimeValueKind::Object;
 const FAssetDocumentRuntimeValueDto* Tracks = nullptr;
 bool bShape = !Transaction && IsStructuralContext()
  && ValidateAssetDocumentPrimitiveInvocation(Invocation, MakeMontageStructureCheckDescriptor()).bValid
  && Value.Kind == EAssetDocumentRuntimeValueKind::Object && Value.ObjectKeys.Num() == Value.Elements.Num();
 static const TCHAR* const Order[] = {TEXT("slots"), TEXT("sections"), TEXT("notifies"), TEXT("notify_states"), TEXT("notify_tracks")};
 constexpr int32 OrderCount = static_cast<int32>(UE_ARRAY_COUNT(Order));
 int32 Next = 0;
 for (int32 I = 0; bShape && I < Value.ObjectKeys.Num(); ++I)
 {
  while (Next < OrderCount && Value.ObjectKeys[I] != Order[Next]) ++Next;
  if (Next == OrderCount) { bShape = false; break; }
  if (Next < 2) { Structure.ObjectKeys.Add(Value.ObjectKeys[I]); Structure.Elements.Add(Value.Elements[I]); }
  else if (Next < 4) { Notify.ObjectKeys.Add(Value.ObjectKeys[I]); Notify.Elements.Add(Value.Elements[I]); }
  else Tracks = &Value.Elements[I];
  ++Next;
 }
 if (!bShape || (Tracks && Notify.ObjectKeys.IsEmpty()))
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidValueShape"),
   TEXT("The Montage timeline check takes {slots?, sections?, notifies?, notify_states?, notify_tracks?} in Preflight, without a transaction."));
 auto Acquired = AcquireAnimationInputLease(Handles, Invocation, TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly);
 if (!Acquired.Lease) return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("The Montage timeline check requires a verified readonly asset lease."));
 TOptional<FAssetDocumentContractValidationResult> Checked;
 const auto Identity = WithAssetDocumentEditorAssetIdentity(*Acquired.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  UObject& Object = View.GetObject();
  const bool bValid = IsValid(&Object) && Object.GetClass() == UAnimMontage::StaticClass()
   && (ExpectedAssetPath.IsEmpty() || Object.GetPathName() == ExpectedAssetPath)
   && View.GetCanonicalObjectPath() == Object.GetPathName() && View.GetTypeRevision() == Object.GetUniqueID();
  if (bValid) Checked.Emplace(CheckMontageTimeline(static_cast<UAnimMontage&>(Object), Structure, Notify, Tracks, Invocation));
  return Result(bValid);
 });
 if (!Identity.bValid || !Checked.IsSet())
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.TargetClassMismatch"), TEXT("The verified asset is not this request's live UAnimMontage."));
 if (!Checked->bValid) return MontageCheckFailure(*Checked);
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 Out.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 return Out;
}
}
