// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentEditorNativeState.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyEdit.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectThreadContext.h"

namespace EditorStagedMontageNotifyPropertiesLocal
{
FAssetDocumentPrimitiveOutcome Reject(const TCHAR* Code, const TCHAR* Message)
{
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
 Out.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 auto& D = Out.Diagnostics.AddDefaulted_GetRef();
 D.Code = Code; D.Path = TEXT("/Notify/Properties"); D.Message = Message;
 return Out;
}
FAssetDocumentPrimitiveOutcome Reject(TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
 auto Out = Reject(TEXT("PropertyWrite.AccessFailed"), TEXT("Staged notify property configuration failed."));
 if (!Diagnostics.IsEmpty()) Out.Diagnostics = MoveTemp(Diagnostics);
 return Out;
}
bool SameHandle(const FAssetDocumentOpaqueHandle& A, const FAssetDocumentOpaqueHandle& B)
{
 return A.RequestId == B.RequestId && A.TransactionId == B.TransactionId && A.RuntimeRevision == B.RuntimeRevision
  && A.ProviderId == B.ProviderId && A.Slot == B.Slot && A.Generation == B.Generation && A.Kind == B.Kind && A.Access == B.Access;
}
bool ContextCurrent(const FAssetDocumentPrimitiveInvocation& Invocation,
 const IAssetDocumentVerifiedHandleLease& Lease, const IAssetDocumentAtomicTransaction& Transaction)
{
 const auto& H = Lease.GetHandle();
 const auto& T = Transaction.GetMutationToken();
 return IsInGameThread() && GIsEditor && !IsGarbageCollecting() && !UE::IsSavingPackage(nullptr)
  && !FUObjectThreadContext::Get().IsRoutingPostLoad && Invocation.Version == 1
  && Invocation.Phase == EAssetDocumentRuntimeRequestPhase::Apply
  && (Invocation.PrimitiveId == TEXT("animation.montage.notifies.replace.v1")
   || Invocation.PrimitiveId == TEXT("animation.montage.notify_states.replace.v1"))
  && Invocation.RequestId.IsValid() && Invocation.TransactionId.IsValid() && Invocation.RuntimeRevision != 0
  && Invocation.ValueArguments.Kind == EAssetDocumentRuntimeValueKind::Object
  && ValidateAssetDocumentRuntimeValue(Invocation.ValueArguments).bValid
  && Invocation.HandleArguments.Num() == 1 && Invocation.HandleArguments[0].Name == TEXT("asset")
  && SameHandle(Invocation.HandleArguments[0].Handle, H)
  && H.ProviderId == TEXT("animation") && H.Generation != 0
  && H.Kind == EAssetDocumentHandleKind::Asset && H.Access == EAssetDocumentHandleAccess::Mutable
  && H.RequestId == Invocation.RequestId && H.TransactionId == Invocation.TransactionId && H.RuntimeRevision == Invocation.RuntimeRevision
  && Transaction.GetState() == EAssetDocumentAtomicState::Mutating && Transaction.GetTransactionId() == Invocation.TransactionId
  && T.RequestId == Invocation.RequestId && T.TransactionId == Invocation.TransactionId && T.RuntimeRevision == Invocation.RuntimeRevision;
}
bool ExactValue(const FAssetDocumentRuntimeValueDto& A, const FAssetDocumentRuntimeValueDto& B)
{
 if (A.Kind != B.Kind || A.BooleanValue != B.BooleanValue || A.IntegerValue != B.IntegerValue
  || A.NumberValue != B.NumberValue || A.StringValue != B.StringValue || A.BytesValue != B.BytesValue
  || A.ObjectKeys != B.ObjectKeys || A.Elements.Num() != B.Elements.Num()) return false;
 for (int32 I = 0; I < A.Elements.Num(); ++I)
  if (!ExactValue(A.Elements[I], B.Elements[I])) return false;
 return true;
}
bool ExactKeys(const FAssetDocumentRuntimeValueDto& Value, const TArray<FString>& Keys)
{
 return Value.Kind == EAssetDocumentRuntimeValueKind::Object
  && Value.ObjectKeys == Keys && Value.Elements.Num() == Keys.Num();
}
bool RequestedObject(const FAssetDocumentPrimitiveInvocation& Invocation, const UObject& Object,
 bool bState, const FAssetDocumentRuntimeValueDto& Properties)
{
 const bool bStatePrimitive = Invocation.PrimitiveId == TEXT("animation.montage.notify_states.replace.v1");
 auto Root = Invocation.ValueArguments;
 if (!Root.ObjectKeys.IsEmpty() && Root.ObjectKeys.Last() == TEXT("structure"))
 {
  if (Root.Elements.Num() != Root.ObjectKeys.Num() || Root.Elements.Last().Kind != EAssetDocumentRuntimeValueKind::Object) return false;
  Root.ObjectKeys.Pop(); Root.Elements.Pop();
 }
 // NotifyTracks travel with the families (the notify track list, after them); they configure no notify.
 if (!Root.ObjectKeys.IsEmpty() && Root.ObjectKeys.Last() == TEXT("notify_tracks"))
 {
  if (Root.Elements.Num() != Root.ObjectKeys.Num() || Root.Elements.Last().Kind != EAssetDocumentRuntimeValueKind::Array) return false;
  Root.ObjectKeys.Pop(); Root.Elements.Pop();
 }
 if (bStatePrimitive ? (!bState || !ExactKeys(Root, {TEXT("notify_states")}))
  : (!ExactKeys(Root, {TEXT("notifies")}) && !ExactKeys(Root, {TEXT("notifies"), TEXT("notify_states")}))) return false;
 for (const auto& Family : Root.Elements)
  if (Family.Kind != EAssetDocumentRuntimeValueKind::Array) return false;
 const int32 Index = Root.ObjectKeys.IndexOfByKey(bState ? TEXT("notify_states") : TEXT("notifies"));
 if (!Root.Elements.IsValidIndex(Index)) return false;
 const TArray<FString> RowKeys = bState
  ? TArray<FString>{TEXT("time"), TEXT("duration"), TEXT("track_index"), TEXT("object")}
  : TArray<FString>{TEXT("time"), TEXT("track_index"), TEXT("object")};
 // Match the actual staged family against the real frozen request. Animation
 // owns allocation provenance and full timeline validation; this is DTO identity.
 for (const auto& Row : Root.Elements[Index].Elements)
 {
  if (!ExactKeys(Row, RowKeys)) return false;
  const auto& Requested = Row.Elements.Last();
  if (!ExactKeys(Requested, {TEXT("class"), TEXT("properties")})) return false;
  if (Requested.Elements[0].Kind == EAssetDocumentRuntimeValueKind::String
   && Requested.Elements[0].StringValue == Object.GetClass()->GetPathName()
   && ExactValue(Requested.Elements[1], Properties)) return true;
 }
 return false;
}
bool AdmittedObject(const UObject* Object)
{
 return IsValid(Object) && !Object->IsTemplate() && !Object->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad);
}
struct FIdentity
{
 TStrongObjectPtr<UAnimMontage> Montage;
 TStrongObjectPtr<UObject> Notify;
 TStrongObjectPtr<UClass> NotifyClass;
 FString MontagePath, NotifyPath, ClassPath;
 uint32 MontageRevision = 0, NotifyRevision = 0, ClassRevision = 0;
 bool bState = false;
 bool Matches() const
 {
  if (!AdmittedObject(Montage.Get()) || Montage->GetClass() != UAnimMontage::StaticClass()
   || Montage->GetPathName() != MontagePath || Montage->GetUniqueID() != MontageRevision
   || !AdmittedObject(Notify.Get()) || !IsValid(NotifyClass.Get()) || Notify->GetClass() != NotifyClass.Get()
   || Notify->GetOuter() != Montage.Get() || Notify->GetPathName() != NotifyPath || Notify->GetUniqueID() != NotifyRevision
   || NotifyClass->GetPathName() != ClassPath || NotifyClass->GetUniqueID() != ClassRevision
   || NotifyClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
   || !NotifyClass->IsChildOf(bState ? UAnimNotifyState::StaticClass() : UAnimNotify::StaticClass())
   || !Notify->HasAnyFlags(RF_Transactional)
   || !Notify->GetName().StartsWith(bState ? TEXT("AssetDocumentManaged_NotifyState_") : TEXT("AssetDocumentManaged_Notify_"), ESearchCase::CaseSensitive)) return false;
  // A staged object is not yet installed. Allocation provenance remains owned
  // by Animation's real per-operation allocation collection, not by this check.
  for (const auto& Event : Montage->Notifies)
   if (Event.Notify == Notify.Get() || Event.NotifyStateClass == Notify.Get()) return false;
  return true;
 }
};
struct FPreparedField
{
 FString Name;
 TUniquePtr<FAssetDocumentEditorPropertyTokenPayload> Token;
 FAssetDocumentEditorPreparedPropertyWrite Write;
};
}

FAssetDocumentPrimitiveOutcome ConfigureAssetDocumentEditorStagedMontageNotifyProperties(
 const FAssetDocumentPrimitiveInvocation& Invocation, const IAssetDocumentVerifiedHandleLease& MontageLease,
 UObject& ExactStagedNotify, const FAssetDocumentRuntimeValueDto& Properties, IAssetDocumentAtomicTransaction& Transaction)
{
 using namespace EditorStagedMontageNotifyPropertiesLocal;
 if (!ContextCurrent(Invocation, MontageLease, Transaction))
  return Reject(TEXT("EditorNativeState.StagedNotifyInvalidContext"), TEXT("Staged notify configuration requires the exact Apply invocation, verified Montage lease and active transaction."));
 if (Properties.Kind != EAssetDocumentRuntimeValueKind::Object || !ValidateAssetDocumentRuntimeValue(Properties).bValid)
  return Reject(TEXT("PrimitiveInvocation.InvalidValueShape"), TEXT("Notify properties require an object of admitted authored field names and typed values."));
 FIdentity Identity;
 auto Valid = WithAssetDocumentEditorAssetIdentity(MontageLease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  FAssetDocumentContractValidationResult R;
  UObject& Object = View.GetObject();
  R.bValid = Object.GetClass() == UAnimMontage::StaticClass() && View.GetCanonicalObjectPath() == Object.GetPathName()
   && View.GetTypeRevision() == Object.GetUniqueID();
  if (R.bValid)
  {
   Identity.Montage.Reset(static_cast<UAnimMontage*>(&Object));
   Identity.MontagePath = Object.GetPathName(); Identity.MontageRevision = Object.GetUniqueID();
  }
  return R;
 });
 if (!Valid.bValid)
 {
  if (!Valid.Diagnostics.IsEmpty()) return Reject(MoveTemp(Valid.Diagnostics));
  return Reject(TEXT("EditorNativeState.StagedNotifyInvalidTarget"), TEXT("The verified native identity is not the exact Montage target."));
 }
 if (!AdmittedObject(&ExactStagedNotify))
  return Reject(TEXT("EditorNativeState.StagedNotifyInvalidTarget"), TEXT("The staged notify must be a live non-template object."));
 Identity.bState = ExactStagedNotify.IsA(UAnimNotifyState::StaticClass());
 Identity.Notify.Reset(&ExactStagedNotify); Identity.NotifyClass.Reset(ExactStagedNotify.GetClass());
 Identity.NotifyPath = ExactStagedNotify.GetPathName(); Identity.NotifyRevision = ExactStagedNotify.GetUniqueID();
 Identity.ClassPath = Identity.NotifyClass->GetPathName(); Identity.ClassRevision = Identity.NotifyClass->GetUniqueID();
 if (!Identity.Matches() || !RequestedObject(Invocation, ExactStagedNotify, Identity.bState, Properties))
  return Reject(TEXT("EditorNativeState.StagedNotifyInvalidTarget"), TEXT("The staged notify family, direct owner, class, managed name or unpublished identity is invalid."));
 const auto Fields = GetAssetDocumentEditorAdmittedPropertyFields(*Identity.NotifyClass.Get());
 TArray<FPreparedField> Prepared;
 Prepared.Reserve(Properties.ObjectKeys.Num());
 TSet<FString> Seen;
 // Decode every field before any setter; the existing top-level identity builder
 // remains unchanged and is deliberately not used for this staged subobject.
 for (int32 I = 0; I < Properties.ObjectKeys.Num(); ++I)
 {
  const FString& Name = Properties.ObjectKeys[I];
  if (Seen.Contains(Name)) return Reject(TEXT("PropertyBatchWrite.DuplicateProperty"), TEXT("A notify property name is duplicated."));
  Seen.Add(Name);
  const auto* Field = Fields.FindByPredicate([&](const auto& Candidate) { return Candidate.Property && Candidate.Property->GetAuthoredName() == Name; });
  if (!Field) return Reject(TEXT("PropertyBatchWrite.UnknownProperty"), TEXT("A notify property is not admitted by the existing safe property surface."));
  auto& Row = Prepared.AddDefaulted_GetRef(); Row.Name = Name;
  Row.Token = MakeUnique<FAssetDocumentEditorPropertyTokenPayload>(*Field->DeclaringClass, *Field->Property,
   Field->CodecId, Field->bReadable, Field->bWritable, Field->TypeRevision);
 }
 // Keep field-path-backed tokens before decoding anything: reference decoding
 // can load dependencies, so a later field must not use a cached raw FProperty.
 for (int32 I = 0; I < Prepared.Num(); ++I)
 {
  auto& Row = Prepared[I];
  FAssetDocumentEditorPropertyTokenView Token;
  if (!ContextCurrent(Invocation, MontageLease, Transaction) || !Identity.Matches()
   || !MontageLease.GetPayloadChecked().IsAlive() || !Row.Token->TryGetView(Token) || !ExactStagedNotify.IsA(Token.DeclaringClass))
   return Reject(TEXT("PropertyToken.InvalidPayload"), TEXT("The staged notify property token is no longer current."));
  FAssetDocumentEditorPropertyEditResult Failure;
  if (!TryPrepareAssetDocumentEditorAdmittedPropertyValue(Token, Properties.Elements[I], Row.Write, Failure))
   return Reject(MoveTemp(Failure.Diagnostics));
 }
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded; Out.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 for (auto& Row : Prepared)
 {
  if (!ContextCurrent(Invocation, MontageLease, Transaction) || !Identity.Matches() || !MontageLease.GetPayloadChecked().IsAlive())
   return Reject(TEXT("EditorNativeState.StagedNotifyIdentityDrift"), TEXT("The staged notify or active operation changed during configuration."));
  FAssetDocumentEditorPropertyTokenView Token;
  if (!Row.Token->TryGetView(Token) || !Token.bWritable || !ExactStagedNotify.IsA(Token.DeclaringClass))
   return Reject(TEXT("PropertyToken.InvalidPayload"), TEXT("The staged notify property token is no longer current."));
  auto Edit = ApplyAssetDocumentEditorPreparedPropertyWriteWithoutEnlist(ExactStagedNotify, Token, MoveTemp(Row.Write), Invocation.PrimitiveId);
  if (!Edit.bSucceeded) return Reject(MoveTemp(Edit.Diagnostics));
  if (!Identity.Matches() || !Row.Token->IsAlive())
   return Reject(TEXT("EditorNativeState.StagedNotifyIdentityDrift"), TEXT("A property notification changed the staged notify identity."));
  const int32 ValueIndex = Edit.Value.ObjectKeys.IndexOfByKey(TEXT("value"));
  if (Edit.Value.Kind != EAssetDocumentRuntimeValueKind::Object || !Edit.Value.Elements.IsValidIndex(ValueIndex))
   return Reject(TEXT("PropertyWrite.ReadbackFailed"), TEXT("The authoritative property readback is unavailable."));
  Out.Value.ObjectKeys.Add(Row.Name); Out.Value.Elements.Add(MoveTemp(Edit.Value.Elements[ValueIndex]));
  Out.ChangedSet.Append(MoveTemp(Edit.ChangedSet));
 }
 if (!ContextCurrent(Invocation, MontageLease, Transaction) || !Identity.Matches() || !MontageLease.GetPayloadChecked().IsAlive())
  return Reject(TEXT("EditorNativeState.StagedNotifyIdentityDrift"), TEXT("The staged notify configuration lost its exact identity."));
 return Out;
}
