// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentEditorStagedBehaviorTreeRead.h"
#include "AssetDocumentEditorNativeState.h"
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

namespace EditorStagedBehaviorTreeReadLocal
{
constexpr TCHAR StagedBTClassPath[] = TEXT("/Script/AIModule.BehaviorTree");
bool BTStagedContextCurrent()
{
 return IsInGameThread() && GIsEditor && !IsGarbageCollecting() && !UE::IsSavingPackage(nullptr)
  && !FUObjectThreadContext::Get().IsRoutingPostLoad;
}
bool BTStagedCanonicalPath(const FString& Path)
{
 const FSoftObjectPath Soft(Path);
 return FPackageName::IsValidObjectPath(Path) && Soft.IsValid() && Soft.IsAsset() && Soft.ToString() == Path;
}
void BTStagedFail(TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics, const TCHAR* Code, const TCHAR* Message)
{
 auto& D = Diagnostics.AddDefaulted_GetRef();
 D.Path = TEXT("/EditorNativeState/StagedBehaviorTreeRead"); D.Code = Code; D.Message = Message;
}
FAssetDocumentRuntimeValueDto BTStagedBool(bool Value)
{
 FAssetDocumentRuntimeValueDto Out; Out.Kind = EAssetDocumentRuntimeValueKind::Boolean; Out.BooleanValue = Value; return Out;
}
FAssetDocumentRuntimeValueDto BTStagedString(const FString& Value)
{
 FAssetDocumentRuntimeValueDto Out; Out.Kind = EAssetDocumentRuntimeValueKind::String; Out.StringValue = Value; return Out;
}
}

struct FAssetDocumentEditorStagedBehaviorTreeReadState
{
 TStrongObjectPtr<UObject> Target;
 FString ObjectPath, OriginalPath, Filename;
 uint32 TypeRevision = 0;
 bool bClosed = false;
 bool Current() const
 {
  using namespace EditorStagedBehaviorTreeReadLocal;
  const UObject* Object = Target.Get();
  return !bClosed && BTStagedContextCurrent() && IsValid(Object)
   && Object->GetClass()->GetPathName() == StagedBTClassPath && !Object->IsTemplate()
   && !Object->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_BeginDestroyed | RF_FinishDestroyed)
   && Object->GetOuter() == Object->GetOutermost() && Object->GetPathName() == ObjectPath
   && Object->GetUniqueID() == TypeRevision && FSoftObjectPath(ObjectPath).ResolveObject() == Object
   && FSoftObjectPath(OriginalPath).ResolveObject() != Object
   && Object->GetOutermost()->HasAnyPackageFlags(PKG_ForDiffing)
   && FPaths::IsSamePath(Object->GetOutermost()->GetLoadedPath().GetLocalFullPath(), Filename);
 }
 void Close() { bClosed = true; Target.Reset(); }
};

namespace EditorStagedBehaviorTreeReadLocal
{
class FBTStagedScopedAssetPayload final : public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
 FBTStagedScopedAssetPayload(TSharedRef<FAssetDocumentEditorStagedBehaviorTreeReadState> InState,
  TUniquePtr<IAssetDocumentNativeHandlePayload> InPayload) : State(InState), Payload(MoveTemp(InPayload)) {}
 FName GetPayloadTypeId() const override { return Payload->GetPayloadTypeId(); }
 bool IsAlive() const override
 {
  const auto Scope = State.Pin(); return Scope && Scope->Current() && Payload->IsAlive();
 }
 bool TryGetObjectIdentity(UE::AssetDocument::EditorPrimitive::FObjectIdentityView& OutView) const override
 {
  if (!IsAlive()) return false;
  auto* Identity = UE::AssetDocument::EditorPrimitive::TryGetNativeIdentityPayload(*Payload,
   UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId);
  return Identity && Identity->TryGetObjectIdentity(OutView);
 }
private:
 TWeakPtr<FAssetDocumentEditorStagedBehaviorTreeReadState> State;
 TUniquePtr<IAssetDocumentNativeHandlePayload> Payload;
};
}

FAssetDocumentEditorStagedBehaviorTreeReadLease::FAssetDocumentEditorStagedBehaviorTreeReadLease(
 TSharedRef<FAssetDocumentEditorStagedBehaviorTreeReadState> InState, FAssetDocumentOpaqueHandle InHandle,
 FAssetDocumentRuntimeValueDto InValue) : State(InState), Handle(MoveTemp(InHandle)), Value(MoveTemp(InValue)) {}
FAssetDocumentEditorStagedBehaviorTreeReadLease::~FAssetDocumentEditorStagedBehaviorTreeReadLease() { Close(); }
const FAssetDocumentOpaqueHandle& FAssetDocumentEditorStagedBehaviorTreeReadLease::GetAssetReadHandle() const { return Handle; }
const FAssetDocumentRuntimeValueDto& FAssetDocumentEditorStagedBehaviorTreeReadLease::GetResolveValue() const { return Value; }
bool FAssetDocumentEditorStagedBehaviorTreeReadLease::IsCurrent() const { return State && State->Current(); }
void FAssetDocumentEditorStagedBehaviorTreeReadLease::Close() { if (State) { State->Close(); State.Reset(); } }

TUniquePtr<FAssetDocumentEditorStagedBehaviorTreeReadLease> ResolveAssetDocumentEditorStagedBehaviorTreeRead(
 const FAssetDocumentRuntimeRequestDto& OriginalRequest, const FAssetDocumentHandleValidationContext& VerifyContext,
 const FString& StagedObjectPath, const FString& StagedPackageFilename, uint64 FreshRevision,
 IAssetDocumentHandleResolver& Handles, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
 using namespace EditorStagedBehaviorTreeReadLocal;
 Diagnostics.Reset();
 if (!BTStagedContextCurrent() || OriginalRequest.AbiVersion != AssetDocumentRuntimeContractVersion
  || OriginalRequest.Operation != EAssetDocumentRuntimeOperation::Apply || !OriginalRequest.RequestId.IsValid()
  || OriginalRequest.ExpectedRuntimeRevision == 0 || VerifyContext.RequestId != OriginalRequest.RequestId
  || !VerifyContext.TransactionId.IsValid() || FreshRevision == 0 || VerifyContext.RuntimeRevision != FreshRevision
  || VerifyContext.ProviderId != TEXT("editor") || VerifyContext.Phase != EAssetDocumentRuntimeRequestPhase::Verify
  || VerifyContext.RequiredKind != EAssetDocumentHandleKind::Asset || VerifyContext.RequiredAccess != EAssetDocumentHandleAccess::ReadOnly
  || OriginalRequest.RequestedAssetClassPath != StagedBTClassPath || !BTStagedCanonicalPath(OriginalRequest.TargetAssetPath))
 {
  BTStagedFail(Diagnostics, TEXT("EditorNativeState.StagedBTReadInvalidContext"), TEXT("Staged BT issuance requires the exact Apply request and bound readonly Verify revision.")); return {};
 }
 if (!BTStagedCanonicalPath(StagedObjectPath) || StagedObjectPath == OriginalRequest.TargetAssetPath
  || FPackageName::ObjectPathToObjectName(StagedObjectPath) != FPackageName::ObjectPathToObjectName(OriginalRequest.TargetAssetPath)
  || StagedPackageFilename.IsEmpty() || FPaths::IsRelative(StagedPackageFilename))
 {
  BTStagedFail(Diagnostics, TEXT("EditorNativeState.StagedBTReadInvalidTarget"), TEXT("Staged BT requires a distinct canonical object path and absolute loaded filename.")); return {};
 }
 UObject* Target = FSoftObjectPath(StagedObjectPath).ResolveObject();
 auto State = MakeShared<FAssetDocumentEditorStagedBehaviorTreeReadState>();
 State->Target.Reset(Target); State->ObjectPath = StagedObjectPath; State->OriginalPath = OriginalRequest.TargetAssetPath;
 State->Filename = StagedPackageFilename; State->TypeRevision = Target ? Target->GetUniqueID() : 0;
 if (!State->Current())
 {
  State->Close(); BTStagedFail(Diagnostics, TEXT("EditorNativeState.StagedBTReadInvalidTarget"), TEXT("The exact BT must already be loaded for diff from this staged file, distinct from the live target.")); return {};
 }
 FAssetDocumentRuntimeValueDto Value; FString DiagnosticCode;
 const TArray<FAssetDocumentEditorValueField> Fields = {
  {TEXT("exists"), BTStagedBool(true)}, {TEXT("canonical_path"), BTStagedString(Target->GetPathName())},
  {TEXT("canonical_class"), BTStagedString(Target->GetClass()->GetPathName())},
  {TEXT("package_name"), BTStagedString(Target->GetOutermost()->GetName())}, {TEXT("package_dirty"), BTStagedBool(Target->GetOutermost()->IsDirty())}};
 if (!TryBuildAssetDocumentEditorValueObject(Fields, Value, DiagnosticCode))
 {
  State->Close(); BTStagedFail(Diagnostics, TEXT("EditorNativeState.StagedBTReadInvalidTarget"), TEXT("Actual staged identity could not be encoded.")); return {};
 }
 auto Issued = Handles.Issue(EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly,
  MakeUnique<FBTStagedScopedAssetPayload>(State, MakeAssetDocumentEditorAssetPayload(*Target)));
 if (!Issued.Handle.IsSet() || !Issued.Diagnostics.IsEmpty()
  || !ValidateAssetDocumentHandleMetadata(Issued.Handle.GetValue(), VerifyContext).bValid)
 {
  State->Close(); Diagnostics.Append(MoveTemp(Issued.Diagnostics));
  BTStagedFail(Diagnostics, TEXT("EditorNativeState.StagedBTReadIssueFailed"), TEXT("The session did not issue the bound readonly staged identity.")); return {};
 }
 auto Acquired = Handles.AcquireVerifiedLease(Issued.Handle.GetValue(), VerifyContext);
 bool Exact = false;
 if (Acquired.Lease)
 {
  Exact = WithAssetDocumentEditorAssetIdentity(*Acquired.Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
  {
   FAssetDocumentContractValidationResult R;
   R.bValid = State->Current() && &Identity.GetObject() == Target
    && Identity.GetCanonicalObjectPath() == StagedObjectPath && Identity.GetTypeRevision() == State->TypeRevision;
   return R;
  }).bValid;
 }
 if (!Exact)
 {
  State->Close(); Diagnostics.Append(MoveTemp(Acquired.Diagnostics));
  BTStagedFail(Diagnostics, TEXT("EditorNativeState.StagedBTReadIssueFailed"), TEXT("The readonly lease did not expose the actual staged target.")); return {};
 }
 return TUniquePtr<FAssetDocumentEditorStagedBehaviorTreeReadLease>(new FAssetDocumentEditorStagedBehaviorTreeReadLease(State, Issued.Handle.GetValue(), MoveTemp(Value)));
}
