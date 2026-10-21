// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentEditorNativeState.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectThreadContext.h"

namespace EditorMontageNotifyPropertySnapshotLocal
{
FAssetDocumentPrimitiveOutcome Failure(const TCHAR* Code, const TCHAR* Message)
{
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
 auto& D = Out.Diagnostics.AddDefaulted_GetRef();
 D.Code = Code; D.Path = TEXT("/Montage/Notify/Properties"); D.Message = Message;
 return Out;
}
bool CurrentContext(const FAssetDocumentPrimitiveInvocation& Invocation, const IAssetDocumentVerifiedHandleLease& Lease)
{
 const auto& H = Lease.GetHandle();
 if (!IsInGameThread() || !GIsEditor || IsGarbageCollecting() || UE::IsSavingPackage(nullptr)
  || FUObjectThreadContext::Get().IsRoutingPostLoad || Invocation.Version != 1
  || Invocation.PrimitiveId != TEXT("animation.montage.profile.snapshot.v1")
  || (Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Preflight && Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Verify)
  || !Invocation.RequestId.IsValid() || Invocation.RuntimeRevision == 0
  || !ValidateAssetDocumentRuntimeValue(Invocation.ValueArguments).bValid
  || Invocation.ValueArguments.Kind != EAssetDocumentRuntimeValueKind::Object
  || !Invocation.ValueArguments.ObjectKeys.IsEmpty() || !Invocation.ValueArguments.Elements.IsEmpty()
  || Invocation.HandleArguments.Num() != 1 || Invocation.HandleArguments[0].Name != TEXT("asset")
  || H.ProviderId != TEXT("animation") || H.Generation == 0 || H.Kind != EAssetDocumentHandleKind::Asset
  || H.Access != EAssetDocumentHandleAccess::ReadOnly || H.RequestId != Invocation.RequestId
  || H.TransactionId != Invocation.TransactionId || H.RuntimeRevision != Invocation.RuntimeRevision) return false;
 const auto& A = Invocation.HandleArguments[0].Handle;
 return A.RequestId == H.RequestId && A.TransactionId == H.TransactionId && A.RuntimeRevision == H.RuntimeRevision
  && A.ProviderId == H.ProviderId && A.Slot == H.Slot && A.Generation == H.Generation && A.Kind == H.Kind && A.Access == H.Access
  && Lease.GetPayloadChecked().IsAlive();
}
bool Live(const UObject* Object)
{
 return IsValid(Object) && !Object->IsTemplate() && !Object->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad);
}
}

FAssetDocumentPrimitiveOutcome ReadAssetDocumentEditorMontageNotifyProperties(
 const FAssetDocumentPrimitiveInvocation& Invocation, const IAssetDocumentVerifiedHandleLease& MontageLease,
 UObject& ExactLiveNotify)
{
 using namespace EditorMontageNotifyPropertySnapshotLocal;
 if (!CurrentContext(Invocation, MontageLease))
  return Failure(TEXT("EditorNativeState.NotifySnapshotInvalidContext"), TEXT("Notify observation requires the exact readonly Montage snapshot invocation."));
 TStrongObjectPtr<UAnimMontage> Target;
 FString TargetPath;
 uint64 TargetRevision = 0;
 const auto Identity = WithAssetDocumentEditorAssetIdentity(MontageLease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  FAssetDocumentContractValidationResult R;
  UObject& Object = View.GetObject();
  R.bValid = Live(&Object) && Object.GetClass() == UAnimMontage::StaticClass()
   && View.GetCanonicalObjectPath() == Object.GetPathName() && View.GetTypeRevision() == Object.GetUniqueID();
  if (R.bValid) { Target.Reset(static_cast<UAnimMontage*>(&Object)); TargetPath = Object.GetPathName(); TargetRevision = Object.GetUniqueID(); }
  return R;
 });
 if (!Identity.bValid || !Live(&ExactLiveNotify))
  return Failure(TEXT("EditorNativeState.NotifySnapshotInvalidTarget"), TEXT("The live Montage or Notify identity is unavailable."));
 TStrongObjectPtr<UObject> Notify(&ExactLiveNotify);
 TStrongObjectPtr<UClass> Class(ExactLiveNotify.GetClass());
 const FString NotifyPath = ExactLiveNotify.GetPathName(), ClassPath = Class->GetPathName();
 const uint32 NotifyRevision = ExactLiveNotify.GetUniqueID(), ClassRevision = Class->GetUniqueID();
 const bool bState = ExactLiveNotify.IsA(UAnimNotifyState::StaticClass());
 auto IdentityCurrent = [&]()
 {
  if (!CurrentContext(Invocation, MontageLease) || !Live(Target.Get()) || Target->GetPathName() != TargetPath
   || Target->GetUniqueID() != TargetRevision || !Live(Notify.Get()) || Notify->GetOuter() != Target.Get()
   || Notify->GetPathName() != NotifyPath || Notify->GetUniqueID() != NotifyRevision || Notify->GetClass() != Class.Get()
   || !IsValid(Class.Get()) || Class->GetPathName() != ClassPath || Class->GetUniqueID() != ClassRevision
   || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
   || !Class->IsChildOf(bState ? UAnimNotifyState::StaticClass() : UAnimNotify::StaticClass())) return false;
  return Target->Notifies.ContainsByPredicate([&](const FAnimNotifyEvent& Event)
  { return bState ? Event.NotifyStateClass.Get() == Notify.Get() : Event.Notify.Get() == Notify.Get(); });
 };
 if (!IdentityCurrent())
  return Failure(TEXT("EditorNativeState.NotifySnapshotInvalidTarget"), TEXT("Notify observation requires an installed direct child of the exact family."));
 struct FField { FString Name; TUniquePtr<FAssetDocumentEditorPropertyTokenPayload> Token; };
 TArray<FField> Fields;
 for (const auto& Field : GetAssetDocumentEditorAdmittedPropertyFields(*Class.Get()))
 {
  if (!Field.bReadable || !Field.bWritable) continue;
  if (!Field.Property || !Field.DeclaringClass)
   return Failure(TEXT("PropertyToken.InvalidPayload"), TEXT("An admitted property field is unavailable."));
  auto& Row = Fields.AddDefaulted_GetRef(); Row.Name = Field.Property->GetAuthoredName();
  Row.Token = MakeUnique<FAssetDocumentEditorPropertyTokenPayload>(*Field.DeclaringClass, *Field.Property,
   Field.CodecId, Field.bReadable, Field.bWritable, Field.TypeRevision);
 }
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded; Out.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 for (const auto& Field : Fields)
 {
  FAssetDocumentEditorPropertyTokenView Token;
  if (!IdentityCurrent() || !Field.Token->TryGetView(Token) || !Token.bReadable || !Token.bWritable || !Notify->IsA(Token.DeclaringClass))
   return Failure(TEXT("PropertyToken.InvalidPayload"), TEXT("The live Notify property token is no longer current."));
  auto Read = ReadAssetDocumentEditorAdmittedProperty(*Notify.Get(), Token);
  if (!Read.bSucceeded)
  {
   auto Failed = Failure(TEXT("PropertyRead.AccessFailed"), TEXT("A live Notify property could not be read."));
   if (!Read.Diagnostics.IsEmpty()) Failed.Diagnostics = MoveTemp(Read.Diagnostics);
   return Failed;
  }
  const int32 Index = Read.Value.ObjectKeys.IndexOfByKey(TEXT("value"));
  if (Read.Value.Kind != EAssetDocumentRuntimeValueKind::Object || !Read.Value.Elements.IsValidIndex(Index)
   || Out.Value.ObjectKeys.Contains(Field.Name))
   return Failure(TEXT("PropertyRead.EncodeFailed"), TEXT("The admitted property readback is not unique and complete."));
  Out.Value.ObjectKeys.Add(Field.Name); Out.Value.Elements.Add(MoveTemp(Read.Value.Elements[Index]));
 }
 if (!IdentityCurrent() || !ValidateAssetDocumentRuntimeValue(Out.Value).bValid)
  return Failure(TEXT("EditorNativeState.NotifySnapshotIdentityDrift"), TEXT("The live Notify identity changed while reading."));
 return Out;
}
