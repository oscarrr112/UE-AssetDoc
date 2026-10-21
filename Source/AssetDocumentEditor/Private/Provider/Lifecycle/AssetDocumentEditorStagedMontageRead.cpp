// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentEditorNativeState.h"
#include "Animation/AnimMontage.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/ClassReference/AssetDocumentEditorAssetPayload.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectThreadContext.h"

namespace EditorStagedMontageReadLocal
{
bool ContextCurrent()
{
 return IsInGameThread() && GIsEditor && !IsGarbageCollecting() && !UE::IsSavingPackage(nullptr)
  && !FUObjectThreadContext::Get().IsRoutingPostLoad;
}
bool CanonicalPath(const FString& Path)
{
 const FSoftObjectPath Soft(Path);
 return FPackageName::IsValidObjectPath(Path) && Soft.IsValid() && Soft.IsAsset() && Soft.ToString() == Path;
}
void Fail(TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics, const TCHAR* Code, const TCHAR* Message)
{
 auto& D = Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/EditorNativeState/StagedMontageRead"); D.Code = Code; D.Message = Message;
}
FAssetDocumentRuntimeValueDto String(const FString& Value)
{ FAssetDocumentRuntimeValueDto Out; Out.Kind = EAssetDocumentRuntimeValueKind::String; Out.StringValue = Value; return Out; }
FAssetDocumentRuntimeValueDto BoolValue(bool Value)
{ FAssetDocumentRuntimeValueDto Out; Out.Kind = EAssetDocumentRuntimeValueKind::Boolean; Out.BooleanValue = Value; return Out; }
}

struct FAssetDocumentEditorStagedMontageReadState
{
 TStrongObjectPtr<UAnimMontage> Target;
 FString ObjectPath, Filename;
 uint32 TypeRevision = 0;
 bool bClosed = false;
 bool Current() const
 {
  using namespace EditorStagedMontageReadLocal;
  return !bClosed && ContextCurrent() && IsValid(Target.Get()) && Target->GetClass() == UAnimMontage::StaticClass()
   && !Target->IsTemplate() && !Target->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad)
   && Target->GetOuter() == Target->GetOutermost() && Target->GetPathName() == ObjectPath
   && Target->GetUniqueID() == TypeRevision && FSoftObjectPath(ObjectPath).ResolveObject() == Target.Get()
   && FPaths::IsSamePath(Target->GetOutermost()->GetLoadedPath().GetLocalFullPath(), Filename);
 }
 void Close() { bClosed = true; Target.Reset(); }
};

namespace EditorStagedMontageReadLocal
{
// Lifetime decoration of the real Editor asset payload. Identity, path and type
// revision are supplied exclusively by that payload, never reconstructed here.
class FScopedAssetPayload final : public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
 FScopedAssetPayload(TSharedRef<FAssetDocumentEditorStagedMontageReadState> InState,
  TUniquePtr<IAssetDocumentNativeHandlePayload> InPayload) : State(InState), Payload(MoveTemp(InPayload)) {}
 FName GetPayloadTypeId() const override { return Payload->GetPayloadTypeId(); }
 bool IsAlive() const override
 {
  const auto Scope = State.Pin();
  return Scope && Scope->Current() && Payload->IsAlive();
 }
 bool TryGetObjectIdentity(UE::AssetDocument::EditorPrimitive::FObjectIdentityView& OutView) const override
 {
  if (!IsAlive()) return false;
  auto* Identity = UE::AssetDocument::EditorPrimitive::TryGetNativeIdentityPayload(*Payload,
   UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId);
  return Identity && Identity->TryGetObjectIdentity(OutView);
 }
private:
 TWeakPtr<FAssetDocumentEditorStagedMontageReadState> State;
 TUniquePtr<IAssetDocumentNativeHandlePayload> Payload;
};
}

FAssetDocumentEditorStagedMontageReadLease::FAssetDocumentEditorStagedMontageReadLease(
 TSharedRef<FAssetDocumentEditorStagedMontageReadState> InState, FAssetDocumentOpaqueHandle InHandle,
 FAssetDocumentRuntimeValueDto InValue) : State(InState), Handle(MoveTemp(InHandle)), Value(MoveTemp(InValue)) {}
FAssetDocumentEditorStagedMontageReadLease::~FAssetDocumentEditorStagedMontageReadLease() { Close(); }
const FAssetDocumentOpaqueHandle& FAssetDocumentEditorStagedMontageReadLease::GetAssetReadHandle() const { return Handle; }
const FAssetDocumentRuntimeValueDto& FAssetDocumentEditorStagedMontageReadLease::GetResolveValue() const { return Value; }
void FAssetDocumentEditorStagedMontageReadLease::Close() { if (State) { State->Close(); State.Reset(); } }

TUniquePtr<FAssetDocumentEditorStagedMontageReadLease> ResolveAssetDocumentEditorStagedMontageRead(
 const FAssetDocumentRuntimeRequestDto& OriginalRequest, const FAssetDocumentHandleValidationContext& VerifyContext,
 const FString& StagedObjectPath, const FString& StagedPackageFilename, IAssetDocumentHandleResolver& Handles,
 TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
 using namespace EditorStagedMontageReadLocal;
 Diagnostics.Reset();
 if (!ContextCurrent() || OriginalRequest.AbiVersion != AssetDocumentRuntimeContractVersion
  || OriginalRequest.Operation != EAssetDocumentRuntimeOperation::Apply || !OriginalRequest.RequestId.IsValid()
  || OriginalRequest.ExpectedRuntimeRevision == 0 || VerifyContext.RequestId != OriginalRequest.RequestId
  || !VerifyContext.TransactionId.IsValid() || VerifyContext.RuntimeRevision == 0
  || VerifyContext.ProviderId != TEXT("editor") || VerifyContext.Phase != EAssetDocumentRuntimeRequestPhase::Verify
  || VerifyContext.RequiredKind != EAssetDocumentHandleKind::Asset || VerifyContext.RequiredAccess != EAssetDocumentHandleAccess::ReadOnly
  || OriginalRequest.RequestedAssetClassPath != TEXT("/Script/Engine.AnimMontage")
  || !CanonicalPath(OriginalRequest.TargetAssetPath))
 {
  Fail(Diagnostics, TEXT("EditorNativeState.StagedReadInvalidContext"), TEXT("Staged read issuance requires the exact Apply request and current readonly Editor Verify context.")); return {};
 }
 if (!CanonicalPath(StagedObjectPath) || StagedObjectPath == OriginalRequest.TargetAssetPath
  || FPackageName::ObjectPathToObjectName(StagedObjectPath) != FPackageName::ObjectPathToObjectName(OriginalRequest.TargetAssetPath)
  || StagedPackageFilename.IsEmpty() || FPaths::IsRelative(StagedPackageFilename))
 {
  Fail(Diagnostics, TEXT("EditorNativeState.StagedReadInvalidTarget"), TEXT("Staged read requires a distinct canonical staged object path and absolute loaded filename.")); return {};
 }
 UAnimMontage* Target = Cast<UAnimMontage>(FSoftObjectPath(StagedObjectPath).ResolveObject());
 auto State = MakeShared<FAssetDocumentEditorStagedMontageReadState>();
 State->Target.Reset(Target); State->ObjectPath = StagedObjectPath; State->Filename = StagedPackageFilename;
 State->TypeRevision = Target ? Target->GetUniqueID() : 0;
 if (!State->Current())
 {
  State->Close(); Fail(Diagnostics, TEXT("EditorNativeState.StagedReadInvalidTarget"), TEXT("The exact staged Montage is not already loaded from the specified file.")); return {};
 }
 FAssetDocumentRuntimeValueDto Value; FString DiagnosticCode;
 const TArray<FAssetDocumentEditorValueField> Fields = {
  {TEXT("exists"), BoolValue(true)}, {TEXT("canonical_path"), String(Target->GetPathName())},
  {TEXT("canonical_class"), String(Target->GetClass()->GetPathName())},
  {TEXT("package_name"), String(Target->GetOutermost()->GetName())}, {TEXT("package_dirty"), BoolValue(Target->GetOutermost()->IsDirty())}};
 if (!TryBuildAssetDocumentEditorValueObject(Fields, Value, DiagnosticCode))
 {
  State->Close(); Fail(Diagnostics, TEXT("EditorNativeState.StagedReadInvalidTarget"), TEXT("The actual staged identity could not be encoded.")); return {};
 }
 auto Issued = Handles.Issue(EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly,
  MakeUnique<FScopedAssetPayload>(State, MakeAssetDocumentEditorAssetPayload(*Target)));
 if (!Issued.Handle.IsSet() || !Issued.Diagnostics.IsEmpty()
  || !ValidateAssetDocumentHandleMetadata(Issued.Handle.GetValue(), VerifyContext).bValid)
 {
  State->Close(); Diagnostics.Append(MoveTemp(Issued.Diagnostics));
  Fail(Diagnostics, TEXT("EditorNativeState.StagedReadIssueFailed"), TEXT("The bootstrap session did not issue the exact readonly verification identity.")); return {};
 }
 auto Acquired = Handles.AcquireVerifiedLease(Issued.Handle.GetValue(), VerifyContext);
 bool bExact = false;
 if (Acquired.Lease)
 {
  const auto Valid = WithAssetDocumentEditorAssetIdentity(*Acquired.Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
  {
   FAssetDocumentContractValidationResult R;
   R.bValid = State->Current() && &Identity.GetObject() == Target && Identity.GetCanonicalObjectPath() == State->ObjectPath
    && Identity.GetTypeRevision() == State->TypeRevision;
   return R;
  });
  bExact = Valid.bValid;
 }
 if (!bExact)
 {
  State->Close(); Diagnostics.Append(MoveTemp(Acquired.Diagnostics));
  Fail(Diagnostics, TEXT("EditorNativeState.StagedReadIssueFailed"), TEXT("The issued readonly handle does not expose the exact live staged payload.")); return {};
 }
 return TUniquePtr<FAssetDocumentEditorStagedMontageReadLease>(new FAssetDocumentEditorStagedMontageReadLease(State, Issued.Handle.GetValue(), MoveTemp(Value)));
}
