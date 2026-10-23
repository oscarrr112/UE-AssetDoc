// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageProfileSnapshot.h"
#include "Provider/Montage/MontageManagedNotifyIdentity.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Provider/Montage/MontageSettings.h"
#include "Provider/Asset/AssetDocumentAnimationPreviewMeshState.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveSnapshot.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentEditorNativeState.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"

namespace MontageProfileSnapshotLocal
{
constexpr const TCHAR* Primitive = TEXT("animation.montage.profile.snapshot.v1");
using FValue = FAssetDocumentRuntimeValueDto;
FValue String(const FString& Value) { FValue Out; Out.Kind = EAssetDocumentRuntimeValueKind::String; Out.StringValue = Value; return Out; }
FValue Number(float Value) { FValue Out; Out.Kind = EAssetDocumentRuntimeValueKind::Number; Out.NumberValue = double(Value); return Out; }
FValue Integer(int32 Value) { FValue Out; Out.Kind = EAssetDocumentRuntimeValueKind::Integer; Out.IntegerValue = Value; return Out; }
FValue Array() { FValue Out; Out.Kind = EAssetDocumentRuntimeValueKind::Array; return Out; }
FValue Object(TArray<FString> Keys, TArray<FValue> Values)
{ FValue Out; Out.Kind = EAssetDocumentRuntimeValueKind::Object; Out.ObjectKeys = MoveTemp(Keys); Out.Elements = MoveTemp(Values); return Out; }
FAssetDocumentPrimitiveOutcome Unsupported(const TCHAR* Surface)
{
 auto Out = MakeAnimationProviderFailure(TEXT("AnimationProvider.Montage.SnapshotUnsupported"), TEXT("The live Montage surface is outside the supported snapshot domain."));
 Out.Diagnostics[0].Path = FString(TEXT("/Montage/")) + Surface;
 return Out;
}
bool CanonicalStagedTargetPath(const FString& Path)
{
 const FSoftObjectPath Soft(Path);
 return Path.StartsWith(TEXT("/Temp/AssetDocument/Runtime/Staging/"), ESearchCase::CaseSensitive)
  && Soft.IsValid() && Soft.GetSubPathString().IsEmpty() && Soft.ToString() == Path
  && !Soft.GetAssetName().IsEmpty() && FPackageName::IsValidObjectPath(Path)
  && FPackageName::IsValidLongPackageName(Soft.GetLongPackageName(), true);
}
bool CanonicalLive(const UObject* Object, const FString& StagedTargetPath = FString())
{
 return IsValid(Object) && !Object->IsTemplate() && !Object->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad)
  && (AssetDocumentAnimationPreviewMesh::CanonicalPath(Object->GetPathName())
   || (!StagedTargetPath.IsEmpty() && Object->GetPathName() == StagedTargetPath && CanonicalStagedTargetPath(StagedTargetPath)))
  && FSoftObjectPath(Object->GetPathName()).ResolveObject() == Object;
}
FAssetDocumentPrimitiveOutcome Capture(UAnimMontage& Target, const FAssetDocumentPrimitiveInvocation& Invocation,
 const IAssetDocumentVerifiedHandleLease& Lease, const FString& StagedTargetPath)
{
 using namespace AssetDocumentMontage;
 TStrongObjectPtr<UAnimMontage> Pinned(&Target);
 const FString Path = Target.GetPathName(); const uint32 Revision = Target.GetUniqueID();
 FValue Skeleton;
 if (auto* Actual = Target.GetSkeleton())
 {
  if (!CanonicalLive(Actual) || Actual->GetClass() != USkeleton::StaticClass()) return Unsupported(TEXT("Skeleton"));
  Skeleton = String(Actual->GetPathName());
 }
 USkeletalMesh* Mesh = nullptr;
 if (!AssetDocumentAnimationPreviewMesh::ReadMontageProfilePreviewMesh(Target, Mesh)) return Unsupported(TEXT("PreviewMesh"));
 FValue Preview;
 if (Mesh)
 {
  if (!CanonicalLive(Mesh)) return Unsupported(TEXT("PreviewMesh"));
  Preview = String(Mesh->GetPathName());
 }
 const IAnimationDataModel* Model = Target.GetDataModel();
 FValue Curves;
 if (!Model || !CaptureAnimationFloatCurveSnapshotV2(Model->GetFloatCurves(), Curves)) return Unsupported(TEXT("Curves"));
 FValue Slots = Array(), Sections = Array(), NotifyRows = Array(), States = Array();
 for (const auto& Slot : Target.SlotAnimTracks)
 {
  if (Slot.SlotName.IsNone()) return Unsupported(TEXT("SlotAnimTracks"));
  FValue Segments = Array();
  for (int32 I = 0; I < Slot.AnimTrack.AnimSegments.Num(); ++I)
  {
   const auto& Segment = Slot.AnimTrack.AnimSegments[I];
   const UAnimSequenceBase* Animation = Segment.GetAnimReference();
   if (!CanonicalLive(Animation) || !Animation->CanBeUsedInComposition()
    || !FMath::IsFinite(Segment.StartPos) || !FMath::IsFinite(Segment.AnimStartTime)
    || !FMath::IsFinite(Segment.AnimEndTime) || !FMath::IsFinite(Segment.AnimPlayRate)) return Unsupported(TEXT("SlotAnimTracks"));
   Segments.Elements.Add(Object({TEXT("animation"), TEXT("start"), TEXT("source_start"), TEXT("source_end"), TEXT("rate"),
    TEXT("loops"), TEXT("previous_slot"), TEXT("previous_segment")},
    {String(Animation->GetPathName()), Number(Segment.StartPos), Number(Segment.AnimStartTime), Number(Segment.AnimEndTime),
     Number(Segment.AnimPlayRate), Integer(Segment.LoopingCount), String(Slot.SlotName.ToString()), Integer(I)}));
  }
  Slots.Elements.Add(Object({TEXT("name"), TEXT("segments")}, {String(Slot.SlotName.ToString()), MoveTemp(Segments)}));
 }
 for (const auto& Section : Target.CompositeSections)
 {
  const int32 Slot = Section.GetSlotIndex();
  const auto Method = Section.GetLinkMethod();
  if (!Target.SlotAnimTracks.IsValidIndex(Slot) || !FMath::IsFinite(Section.GetTime()) || Section.SectionName.IsNone()
   || (Method != EAnimLinkMethod::Absolute && Method != EAnimLinkMethod::Relative && Method != EAnimLinkMethod::Proportional))
   return Unsupported(TEXT("CompositeSections"));
  Sections.Elements.Add(Object({TEXT("name"), TEXT("next"), TEXT("time"), TEXT("slot_name"), TEXT("method")},
   {String(Section.SectionName.ToString()), String(Section.NextSectionName.ToString()), Number(Section.GetTime()),
    String(Target.SlotAnimTracks[Slot].SlotName.ToString()), Integer(int32(Method))}));
 }
 for (const auto& Event : Target.Notifies)
 {
  for (const bool bState : {false, true})
  {
   if (!IsManagedMontageNotifyEvent(Target, Event, bState)) continue;
   UObject* Notify = bState ? static_cast<UObject*>(Event.NotifyStateClass.Get()) : static_cast<UObject*>(Event.Notify.Get());
   if (!Notify || !FMath::IsFinite(Event.GetTime()) || !Target.AnimNotifyTracks.IsValidIndex(Event.TrackIndex)
    || Event.GetSlotIndex() != 0 || (bState && (!FMath::IsFinite(Event.GetDuration()) || Event.GetDuration() <= 0.f
     || Event.EndLink.GetSlotIndex() != 0))) return Unsupported(bState ? TEXT("NotifyStates") : TEXT("Notifies"));
   auto Properties = ReadAssetDocumentEditorMontageNotifyProperties(Invocation, Lease, *Notify);
   if (Properties.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) return Properties;
   FValue Row = Object({TEXT("time")}, {Number(Event.GetTime())});
   if (bState) { Row.ObjectKeys.Add(TEXT("duration")); Row.Elements.Add(Number(Event.GetDuration())); }
   Row.ObjectKeys.Append({TEXT("track_index"), TEXT("object")});
   Row.Elements.Add(Integer(Event.TrackIndex));
   Row.Elements.Add(Object({TEXT("class"), TEXT("properties")}, {String(Notify->GetClass()->GetPathName()), MoveTemp(Properties.Value)}));
   (bState ? States : NotifyRows).Elements.Add(MoveTemp(Row));
  }
 }
 // NotifyTracks: the notify panel's tracks, by index (TrackIndex), as their names (colours are display only).
 FValue Tracks = Array();
 for (const auto& Track : Target.AnimNotifyTracks)
  Tracks.Elements.Add(Object({TEXT("name")}, {String(Track.TrackName.ToString())}));
 FValue Settings;
 TArray<FAssetDocumentRuntimeDiagnostic> SettingsDiagnostics;
 if (!CaptureAnimationMontageSettings(Target, Settings, SettingsDiagnostics))
 {
  auto Failed = Unsupported(TEXT("Settings"));
  if (!SettingsDiagnostics.IsEmpty()) Failed.Diagnostics = MoveTemp(SettingsDiagnostics);
  return Failed;
 }
 FAssetDocumentPrimitiveOutcome Out;
 Out.Value = Object({TEXT("skeleton"), TEXT("preview_mesh"), TEXT("curves"), TEXT("slots"), TEXT("sections"), TEXT("notifies"), TEXT("notify_states"), TEXT("notify_tracks"), TEXT("settings")},
  {MoveTemp(Skeleton), MoveTemp(Preview), MoveTemp(Curves), MoveTemp(Slots), MoveTemp(Sections), MoveTemp(NotifyRows), MoveTemp(States),
   MoveTemp(Tracks), MoveTemp(Settings)});
 if (!IsStructuralContext() || !CanonicalLive(&Target, StagedTargetPath) || Target.GetPathName() != Path || Target.GetUniqueID() != Revision
  || !Lease.GetPayloadChecked().IsAlive() || !ValidateAssetDocumentRuntimeValue(Out.Value).bValid) return Unsupported(TEXT("Identity"));
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 return Out;
}
}

FAssetDocumentPrimitiveDescriptor MakeAnimationMontageProfileSnapshotDescriptor()
{
 using namespace MontageProfileSnapshotLocal;
 FAssetDocumentPrimitiveDescriptor D;
 D.PrimitiveId = Primitive; D.Version = 1; D.ProviderId = TEXT("animation"); D.Owner = EAssetDocumentPrimitiveOwner::Animation;
 D.ExecutionThread = EAssetDocumentExecutionThread::GameThread; D.Access = EAssetDocumentPrimitiveAccess::ReadOnly;
 D.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Verify};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {{TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, true}};
 D.InputSchemaId = FString(Primitive) + TEXT(".input.v1"); D.OutputSchemaId = FString(Primitive) + TEXT(".output.v1");
 D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.StableDiagnosticCodes = {TEXT("AnimationProvider.InvalidValueShape"), TEXT("AnimationProvider.InvalidHandlePayload"),
  TEXT("AnimationProvider.TargetClassMismatch"), TEXT("AnimationProvider.Montage.SnapshotUnsupported"),
  TEXT("EditorNativeState.NotifySnapshotInvalidContext"), TEXT("EditorNativeState.NotifySnapshotInvalidTarget"),
  TEXT("EditorNativeState.NotifySnapshotIdentityDrift"), TEXT("PropertyToken.InvalidPayload"),
  TEXT("PropertyRead.AccessFailed"), TEXT("PropertyRead.EncodeFailed")};
 auto& A = D.Admission;
 A.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
 A.ExactEngineVersion = TEXT("5.7.4"); A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 A.PublicModule = TEXT("Engine;AssetDocumentEditor");
 A.PublicHeader = TEXT("Animation/AnimMontage.h;Animation/AnimData/IAnimationDataModel.h;AssetDocumentEditorNativeState.h");
 A.ExportedAuthoritativeApi = TEXT("UAnimationAsset::GetSkeleton;IAnimationDataModel::GetFloatCurves;ReadAssetDocumentEditorMontageNotifyProperties");
 A.AuthorityContextId = TEXT("animation.montage.profile.read-only.v1|AD.ANIMATION.PREVIEW-MESH-CLASSIFICATION-v1");
 A.PrimaryOperationId = Primitive;
 A.IdentityLifetimeContractId = TEXT("animation.provider-lease.read-only.v1");
 A.TransactionContractId = TEXT("animation.read-only.no-active-transaction.v1");
 A.RollbackContractId = TEXT("animation.read-only.no-rollback.v1");
 A.CompletionContractId = TEXT("animation.authoritative-readback.v1");
 A.SaveReloadEvidenceId = FString(Primitive) + TEXT(".read-only-no-persistence.v1");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimationProvider.MontageProfileSnapshot")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimationProvider.MontageProfileSnapshot"),
  TEXT("UEAssetDoc.AnimationProvider.Authority"), TEXT("UEAssetDoc.AnimationProvider.Descriptors")};
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|montage-profile|actual-safe-properties|curve-v2|preflight-verify.v1");
 return D;
}

static FAssetDocumentPrimitiveOutcome InvokeSnapshot(const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction, const FString& ExpectedAssetPath, bool bStagedVerification = false)
{
 using namespace MontageProfileSnapshotLocal;
 const FString StagedTargetPath = bStagedVerification ? ExpectedAssetPath : FString();
 if ((bStagedVerification && (Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Verify || !CanonicalStagedTargetPath(StagedTargetPath)))
  || Transaction || !AssetDocumentMontage::IsStructuralContext()
  || !ValidateAssetDocumentPrimitiveInvocation(Invocation, MakeAnimationMontageProfileSnapshotDescriptor()).bValid
  || Invocation.ValueArguments.Kind != EAssetDocumentRuntimeValueKind::Object
  || !Invocation.ValueArguments.ObjectKeys.IsEmpty() || !Invocation.ValueArguments.Elements.IsEmpty())
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidValueShape"), TEXT("Montage snapshot requires an empty object and the exact readonly Preflight or Verify invocation."));
 auto Acquired = AcquireAnimationInputLease(Handles, Invocation, TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly);
 if (!Acquired.Lease) return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("The Montage snapshot requires a verified readonly asset lease."));
 TOptional<FAssetDocumentPrimitiveOutcome> Snapshot;
 const auto Identity = WithAssetDocumentEditorAssetIdentity(*Acquired.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  FAssetDocumentContractValidationResult R;
  UObject& Target = View.GetObject();
  R.bValid = CanonicalLive(&Target, StagedTargetPath) && Target.GetClass() == UAnimMontage::StaticClass()
   && (ExpectedAssetPath.IsEmpty() || Target.GetPathName() == ExpectedAssetPath)
   && View.GetCanonicalObjectPath() == Target.GetPathName() && View.GetTypeRevision() == Target.GetUniqueID();
  if (R.bValid) Snapshot.Emplace(Capture(static_cast<UAnimMontage&>(Target), Invocation, *Acquired.Lease, StagedTargetPath));
  return R;
 });
 if (!Identity.bValid || !Snapshot.IsSet()) return MakeAnimationProviderFailure(TEXT("AnimationProvider.TargetClassMismatch"), TEXT("The verified asset is not the exact live UAnimMontage."));
 return MoveTemp(Snapshot.GetValue());
}

FAssetDocumentPrimitiveOutcome InvokeAnimationMontageProfileSnapshot(const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction)
{
 return InvokeSnapshot(Invocation, Handles, Transaction, FString());
}

FAssetDocumentPrimitiveOutcome InvokeAnimationMontageProfileSnapshotBound(const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction, const FString& ExpectedAssetPath, bool bStagedVerification)
{
 using namespace MontageProfileSnapshotLocal;
 if (bStagedVerification ? !CanonicalStagedTargetPath(ExpectedAssetPath)
  : !AssetDocumentAnimationPreviewMesh::CanonicalPath(ExpectedAssetPath))
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("The bound snapshot target path must be canonical and nonempty."));
 return InvokeSnapshot(Invocation, Handles, Transaction, ExpectedAssetPath, bStagedVerification);
}

bool IsMontageProfileSnapshotAdmissionShape(const FAssetDocumentPrimitiveDescriptor& D)
{
 using namespace MontageProfileSnapshotLocal;
 if (D.PrimitiveId != Primitive) return false;
 const auto E = MakeAnimationMontageProfileSnapshotDescriptor();
 if (D.Version != E.Version || D.ProviderId != E.ProviderId || D.Owner != E.Owner || D.ExecutionThread != E.ExecutionThread
  || D.Access != E.Access || D.AllowedPhases != E.AllowedPhases || D.AllowedHandleKinds != E.AllowedHandleKinds
  || D.HandleArgumentSchemas.Num() != 1 || !D.HandleResultSchemas.IsEmpty() || D.bRequiresActiveTransaction != E.bRequiresActiveTransaction
  || D.InputSchemaId != E.InputSchemaId || D.OutputSchemaId != E.OutputSchemaId || D.MutationFootprintSchemaId != E.MutationFootprintSchemaId
  || D.StableDiagnosticCodes != E.StableDiagnosticCodes || D.RequiredConsumerIds != E.RequiredConsumerIds) return false;
 const auto& X = D.HandleArgumentSchemas[0]; const auto& Y = E.HandleArgumentSchemas[0];
 if (X.Name != Y.Name || X.Kind != Y.Kind || X.RequiredAccess != Y.RequiredAccess || X.bRequired != Y.bRequired) return false;
 const auto& A = D.Admission; const auto& B = E.Admission;
 return A.ContractVersion == B.ContractVersion && A.Disposition == B.Disposition && A.AuthorityPath == B.AuthorityPath
  && A.ExactEngineVersion == B.ExactEngineVersion && A.PhysicalOwner == B.PhysicalOwner && A.PublicModule == B.PublicModule
  && A.PublicHeader == B.PublicHeader && A.ExportedAuthoritativeApi == B.ExportedAuthoritativeApi
  && A.AuthorityContextId == B.AuthorityContextId && A.PrimaryOperationId == B.PrimaryOperationId
  && A.IdentityLifetimeContractId == B.IdentityLifetimeContractId && A.TransactionContractId == B.TransactionContractId
  && A.RollbackContractId == B.RollbackContractId && A.CompletionContractId == B.CompletionContractId
  && A.SaveReloadEvidenceId == B.SaveReloadEvidenceId && A.FailureInjectionTestIds == B.FailureInjectionTestIds
  && A.VerificationTestIds == B.VerificationTestIds && A.SourceMarker == B.SourceMarker
  && A.RawLifecycleAllowlistId == B.RawLifecycleAllowlistId && A.GovernanceId == B.GovernanceId
  && A.IndependentCandidateReviewId == B.IndependentCandidateReviewId && A.IndependentBridgeReviewId == B.IndependentBridgeReviewId
  && A.HelperReplacementCondition == B.HelperReplacementCondition && A.RemovalCondition == B.RemovalCondition
  && A.AuthorityFingerprint == B.AuthorityFingerprint;
}
