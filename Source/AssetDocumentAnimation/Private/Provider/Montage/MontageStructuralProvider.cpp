// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageStructuralProvider.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentEditorNativeState.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentMontage
{
namespace
{
bool Keys(const FAssetDocumentRuntimeValueDto& V, const TArray<FString>& Expected)
{
 return V.Kind == EAssetDocumentRuntimeValueKind::Object && V.ObjectKeys == Expected && V.Elements.Num() == Expected.Num();
}
bool ReadName(const FAssetDocumentRuntimeValueDto& V, FName& Out)
{
 if (V.Kind != EAssetDocumentRuntimeValueKind::String || V.StringValue.Len() >= NAME_SIZE) return false;
 Out = FName(*V.StringValue);
 // Canonical None is permitted only where the typed validator permits absence.
 return Out.ToString() == V.StringValue;
}
bool ReadFloat(const FAssetDocumentRuntimeValueDto& V, float& Out)
{
 if (V.Kind != EAssetDocumentRuntimeValueKind::Number && V.Kind != EAssetDocumentRuntimeValueKind::Integer) return false;
 const double Value = V.Kind == EAssetDocumentRuntimeValueKind::Integer ? double(V.IntegerValue) : V.NumberValue;
 if (!FMath::IsFinite(Value) || Value < -double(MAX_flt) || Value > double(MAX_flt)) return false;
 Out = float(Value);
 return FMath::IsFinite(Out);
}
bool ReadInteger(const FAssetDocumentRuntimeValueDto& V, int32& Out)
{
 if (V.Kind == EAssetDocumentRuntimeValueKind::Integer)
 {
  if (V.IntegerValue < MIN_int32 || V.IntegerValue > MAX_int32) return false;
  Out = int32(V.IntegerValue); return true;
 }
 if (V.Kind != EAssetDocumentRuntimeValueKind::Number || !FMath::IsFinite(V.NumberValue)
  || V.NumberValue < double(MIN_int32) || V.NumberValue > double(MAX_int32)) return false;
 Out = int32(V.NumberValue);
 return double(Out) == V.NumberValue;
}
UAnimSequenceBase* ResolveAnimation(const FAssetDocumentRuntimeValueDto& V)
{
 if (V.Kind != EAssetDocumentRuntimeValueKind::String) return nullptr;
 const FSoftObjectPath Path(V.StringValue);
 if (!Path.IsValid() || !Path.GetSubPathString().IsEmpty() || Path.ToString() != V.StringValue
  || !FPackageName::IsValidObjectPath(V.StringValue)) return nullptr;
 auto* Animation = Cast<UAnimSequenceBase>(Path.ResolveObject());
 return IsValid(Animation) && !Animation->IsTemplate() && !Animation->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad)
  && Animation->GetPathName() == V.StringValue && Animation->CanBeUsedInComposition() ? Animation : nullptr;
}
}

FAssetDocumentContractValidationResult ParseMontageStructuralArguments(
 const FAssetDocumentRuntimeValueDto& Value, FStringView PrimitiveId, FStructuralIntent& OutIntent)
{
 OutIntent = FStructuralIntent();
 if (!IsStructuralContext() || !ValidateAssetDocumentRuntimeValue(Value).bValid) return Result(false);
 const bool bSlots = PrimitiveId == SlotsPrimitive;
 if ((!bSlots && PrimitiveId != SectionsPrimitive)
  || (bSlots && !Keys(Value, {TEXT("slots")}) && !Keys(Value, {TEXT("slots"), TEXT("sections")}))
  || (!bSlots && !Keys(Value, {TEXT("sections")}))) return Result(false);
 FStructuralIntent Parsed;
 if (bSlots)
 {
  const auto& Rows = Value.Elements[0];
  if (Rows.Kind != EAssetDocumentRuntimeValueKind::Array) return Result(false);
  Parsed.Slots.Emplace();
  for (const auto& Row : Rows.Elements)
  {
   if (!Keys(Row, {TEXT("name"), TEXT("segments")}) || Row.Elements[1].Kind != EAssetDocumentRuntimeValueKind::Array) return Result(false);
   FSlotEdit Slot;
   if (!ReadName(Row.Elements[0], Slot.Name) || Slot.Name.IsNone()) return Result(false);
   for (const auto& Segment : Row.Elements[1].Elements)
   {
    if (!Keys(Segment, {TEXT("animation"), TEXT("start"), TEXT("source_start"), TEXT("source_end"), TEXT("rate"),
     TEXT("loops"), TEXT("previous_slot"), TEXT("previous_segment")})) return Result(false);
    const auto& E = Segment.Elements;
    FSegmentEdit Edit;
    Edit.Animation = ResolveAnimation(E[0]);
    if (!Edit.Animation) return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"));
    if (!ReadFloat(E[1], Edit.Start) || !ReadFloat(E[2], Edit.SourceStart) || !ReadFloat(E[3], Edit.SourceEnd)
     || !ReadFloat(E[4], Edit.Rate) || !ReadInteger(E[5], Edit.Loops) || !ReadName(E[6], Edit.PreviousSlot)
     || !ReadInteger(E[7], Edit.PreviousSegment)) return Result(false);
    Slot.Segments.Add(MoveTemp(Edit));
   }
   Parsed.Slots->Add(MoveTemp(Slot));
  }
 }
 if (!bSlots || Value.Elements.Num() == 2)
 {
  const auto& Rows = Value.Elements.Last();
  if (Rows.Kind != EAssetDocumentRuntimeValueKind::Array) return Result(false);
  Parsed.Sections.Emplace();
  for (const auto& Row : Rows.Elements)
  {
   if (!Keys(Row, {TEXT("name"), TEXT("next"), TEXT("time"), TEXT("slot_name"), TEXT("method")})) return Result(false);
   const auto& E = Row.Elements;
   FSectionEdit Edit;
   int32 Method = 0;
   if (!ReadName(E[0], Edit.Name) || Edit.Name.IsNone() || !ReadName(E[1], Edit.Next) || !ReadFloat(E[2], Edit.Time)
    || !ReadName(E[3], Edit.Slot) || !ReadInteger(E[4], Method)
    || (Method != EAnimLinkMethod::Absolute && Method != EAnimLinkMethod::Relative && Method != EAnimLinkMethod::Proportional)) return Result(false);
   Edit.Method = static_cast<EAnimLinkMethod::Type>(Method);
   Parsed.Sections->Add(MoveTemp(Edit));
  }
 }
 OutIntent = MoveTemp(Parsed);
 return Result(true);
}
}

namespace
{
FAssetDocumentPrimitiveDescriptor MakeStructuralDescriptor(bool bSlots)
{
 using namespace AssetDocumentMontage;
 FAssetDocumentPrimitiveDescriptor D;
 D.PrimitiveId = bSlots ? SlotsPrimitive : SectionsPrimitive;
 D.Version = 1; D.ProviderId = TEXT("animation"); D.Owner = EAssetDocumentPrimitiveOwner::Animation;
 D.ExecutionThread = EAssetDocumentExecutionThread::GameThread; D.Access = EAssetDocumentPrimitiveAccess::Mutating;
 D.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {{TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, true}};
 D.bRequiresActiveTransaction = true;
 D.InputSchemaId = D.PrimitiveId + TEXT(".input.v1"); D.OutputSchemaId = D.PrimitiveId + TEXT(".output.v1");
 D.MutationFootprintSchemaId = D.PrimitiveId + TEXT(".footprint.v1"); D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.StableDiagnosticCodes = {TEXT("AnimationProvider.InvalidValueShape"), TEXT("AnimationProvider.InvalidHandlePayload"),
  TEXT("AnimationProvider.TransactionUnavailable"), TEXT("AnimationProvider.Montage.InvalidIntent"),
  TEXT("AnimationProvider.Montage.InvalidContext"), TEXT("AnimationProvider.Montage.PreparedStateDrift"),
  TEXT("AnimationProvider.Montage.DependencyUnavailable"), TEXT("AnimationProvider.Montage.InvalidSlot"),
  TEXT("AnimationProvider.Montage.InvalidSegment"), TEXT("AnimationProvider.Montage.IncompatibleAnimation"),
  TEXT("AnimationProvider.Montage.RecursiveAnimation"), TEXT("AnimationProvider.Montage.CompletionFailed"),
  TEXT("AnimationProvider.Montage.ReadbackMismatch"), TEXT("AnimationProvider.Montage.RollbackFailed")};
 auto& A = D.Admission;
 A.Disposition = EAssetDocumentPrimitiveDisposition::AdmittedBridge;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted;
 A.ExactEngineVersion = TEXT("5.7.4"); A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 A.PublicModule = TEXT("AssetDocumentAnimation");
 A.PublicHeader = TEXT("Private/Provider/Montage/MontageStructuralOperations.h");
 A.ExportedAuthoritativeApi = bSlots
  ? TEXT("PublishMontageSlots;PublishMontageSections;PublishMontageNotifyEvents;UAnimCompositeBase::SetCompositeLength")
  : TEXT("PublishMontageSections;PublishMontageNotifyEvents");
 A.AuthorityContextId = bSlots
  ? TEXT("AD.ANIMATION.MONTAGE.SLOTS-PUBLISH.v1|AD.ANIMATION.MONTAGE.SECTIONS-PUBLISH.v1|AD.ANIMATION.MONTAGE.NOTIFIES-PUBLISH.v1")
  : TEXT("AD.ANIMATION.MONTAGE.SECTIONS-PUBLISH.v1|AD.ANIMATION.MONTAGE.NOTIFIES-PUBLISH.v1");
 A.PrimaryOperationId = D.PrimitiveId;
 A.IdentityLifetimeContractId = TEXT("animation.montage.verified-target.loaded-path-pinned-dependencies.v1");
 A.TransactionContractId = TEXT("animation.core-active-transaction.prepared-composition.v1");
 A.RollbackContractId = RollbackId;
 A.CompletionContractId = TEXT("animation.montage.structural.visible-equivalence.v1");
 A.SaveReloadEvidenceId = TEXT("animation.montage.structural.launcher-5.7.4-save-reload.v1");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimationProvider.MontageStructural.FailureRollback")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimationProvider.MontageStructural.Slots"),
  TEXT("UEAssetDoc.AnimationProvider.MontageStructural.Sections"),
  TEXT("UEAssetDoc.AnimationProvider.MontageStructural.PreparedCapture"),
  TEXT("UEAssetDoc.AnimationProvider.Authority"), TEXT("UEAssetDoc.AnimationProvider.Descriptors")};
 A.GovernanceId = bSlots ? TEXT("ANIMATION-MONTAGE-SLOTS-PUBLISH-v1") : TEXT("ANIMATION-MONTAGE-SECTIONS-PUBLISH-v1");
 A.SourceMarker = FString::Printf(TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(%s)"), *A.GovernanceId);
 A.RawLifecycleAllowlistId = FString::Printf(TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(%s|%s)"), *A.GovernanceId, *D.PrimitiveId);
 // Exact formal catalog references bind the real native candidate and boundary reviews.
 A.IndependentCandidateReviewId = TEXT("candidate-review.initial/") + A.GovernanceId;
 A.IndependentBridgeReviewId = TEXT("bridge-review.initial/") + A.GovernanceId;
 A.HelperReplacementCondition = TEXT("Stock public APIs provide complete typed montage collection publication and equivalent restoration.");
 A.RemovalCondition = TEXT("Migrate the unique structural caller and remove the named private publication exceptions.");
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|montage-typed-publication|loaded-path-dependencies|") + D.PrimitiveId;
 return D;
}
FAssetDocumentPrimitiveOutcome Failure(const FAssetDocumentContractValidationResult& Valid)
{
 auto Out = MakeAnimationProviderFailure(TEXT("AnimationProvider.Montage.InvalidIntent"), TEXT("The exact montage structural operation was rejected."));
 if (!Valid.Diagnostics.IsEmpty()) Out.Diagnostics = Valid.Diagnostics;
 return Out;
}
}

FAssetDocumentPrimitiveDescriptor MakeAnimationMontageSlotsDescriptor() { return MakeStructuralDescriptor(true); }
FAssetDocumentPrimitiveDescriptor MakeAnimationMontageSectionsDescriptor() { return MakeStructuralDescriptor(false); }

bool IsMontageStructuralAdmissionShape(const FAssetDocumentPrimitiveDescriptor& D)
{
 using namespace AssetDocumentMontage;
 if (D.PrimitiveId != SlotsPrimitive && D.PrimitiveId != SectionsPrimitive) return false;
 const auto E = MakeStructuralDescriptor(D.PrimitiveId == SlotsPrimitive);
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

FAssetDocumentPrimitiveOutcome InvokeAnimationMontageStructural(const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction)
{
 using namespace AssetDocumentMontage;
 if (!IsStructuralContext() || !Transaction || Transaction->GetState() != EAssetDocumentAtomicState::Mutating
  || Transaction->GetTransactionId() != Invocation.TransactionId || Transaction->GetMutationToken().TransactionId != Invocation.TransactionId
  || Transaction->GetMutationToken().RequestId != Invocation.RequestId || Transaction->GetMutationToken().RuntimeRevision != Invocation.RuntimeRevision)
  return Failure(Result(false, TEXT("AnimationProvider.TransactionUnavailable")));
 if ((Invocation.PrimitiveId != SlotsPrimitive && Invocation.PrimitiveId != SectionsPrimitive)
  || !ValidateAssetDocumentPrimitiveInvocation(Invocation, MakeStructuralDescriptor(Invocation.PrimitiveId == SlotsPrimitive)).bValid)
  return Failure(Result(false, TEXT("AnimationProvider.InvalidValueShape")));
 auto Target = AcquireAnimationInputLease(Handles, Invocation, TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
 if (!Target.Lease)
 {
  auto Valid = Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
  if (!Target.Diagnostics.IsEmpty()) Valid.Diagnostics = MoveTemp(Target.Diagnostics);
  return Failure(Valid);
 }
 FStructuralIntent Intent;
 auto Valid = ParseMontageStructuralArguments(Invocation.ValueArguments, Invocation.PrimitiveId, Intent);
 if (!Valid.bValid) return Failure(Valid);
 TSharedPtr<FPreparedStructuralEdit> Plan;
 Valid = WithAssetDocumentEditorAssetIdentity(*Target.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  UObject& Object = View.GetObject();
  if (Object.GetClass() != UAnimMontage::StaticClass() || View.GetCanonicalObjectPath() != Object.GetPathName()
   || View.GetTypeRevision() != Object.GetUniqueID()) return Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
  return PrepareMontageStructuralEdit(static_cast<UAnimMontage&>(Object), Intent, Invocation.RequestId, Invocation.RuntimeRevision, Plan);
 });
 if (!Valid.bValid) return Failure(Valid);
 Valid = ApplyMontageStructuralEdit(Plan.ToSharedRef(), *Target.Lease, Invocation.Phase, *Transaction);
 if (!Valid.bValid) return Failure(Valid);
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded; Out.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 Out.Value.ObjectKeys = {TEXT("changed")};
 auto& Changed = Out.Value.Elements.AddDefaulted_GetRef();
 Changed.Kind = EAssetDocumentRuntimeValueKind::Boolean; Changed.BooleanValue = Plan->bChanged;
 if (Plan->bChanged)
 {
  auto& Change = Out.ChangedSet.AddDefaulted_GetRef();
  Change.AuthorityId = Invocation.PrimitiveId; Change.StableObjectId = Plan->TargetPath;
  Change.Path = TEXT("/MontageStructure"); Change.ChangeKind = TEXT("Replace");
 }
 return Out;
}
