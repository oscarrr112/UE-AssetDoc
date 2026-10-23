// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Asset/AssetDocumentAnimationPreviewMeshState.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentAnimationPreviewMesh
{
namespace
{
enum class EPreimage { Empty, ResolvedValid, UnresolvedOrMissing, Incompatible };
// ASSETDOCUMENT_READONLY_AUTHORITY_EXCEPTION(AD.ANIMATION.PREVIEW-MESH-CLASSIFICATION-v1)
// P1 preparation/rechecks for exact Montage/Sequence and the unchanged read-only
// Montage profile helper consume this classification-only probe.
// Remove when stock Engine exposes an authoritative empty-vs-unresolved query.
// Never return the soft path in a DTO, mutate the property, or admit generic properties.
EPreimage ProbePreviewMeshPreimage(const UAnimationAsset& Target, USkeletalMesh*& Mesh)
{
 Mesh = nullptr;
 if (!ExactTarget(&Target)) return EPreimage::Incompatible;
 const FSoftObjectProperty* Property = FindFProperty<FSoftObjectProperty>(UAnimationAsset::StaticClass(), TEXT("PreviewSkeletalMesh"));
 if (!Property || Property->GetOwnerStruct() != UAnimationAsset::StaticClass()
  || Property->PropertyClass != USkeletalMesh::StaticClass() || Property->ArrayDim != 1)
  return EPreimage::UnresolvedOrMissing;
 const FSoftObjectPtr Reference = Property->GetPropertyValue_InContainer(&Target);
 if (Reference.IsNull()) return EPreimage::Empty;
 if (!CanonicalPath(Reference.ToSoftObjectPath().ToString())) return EPreimage::Incompatible;
 // Resolve a copy: unlike non-const GetPreviewMesh, this never repairs incompatible state.
 UObject* Object = Reference.ToSoftObjectPath().ResolveObject();
 if (!Object) Object = Reference.ToSoftObjectPath().TryLoad();
 Mesh = Cast<USkeletalMesh>(Object);
 if (!Mesh) return Object ? EPreimage::Incompatible : EPreimage::UnresolvedOrMissing;
 if (Object->GetPathName() != Reference.ToSoftObjectPath().ToString()
  || !Compatible(Target, Mesh)) return EPreimage::Incompatible;
 return EPreimage::ResolvedValid;
}
struct FCapture
{
 FGuid Transaction;
 bool bOpened = false, bClosed = false;
 bool bForwardWritten = false;
};
struct FPreparedPreviewMesh
{
 TStrongObjectPtr<UAnimationAsset> Target;
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<USkeletalMesh> Old, Desired;
 TUniquePtr<FSkeletonIdentity> TargetSkeleton, OldSkeleton, DesiredSkeleton;
 FString TargetPath, PackagePath, OldPath, DesiredPath, Operation;
 UClass* TargetClass = nullptr;
 uint64 TargetRevision = 0, DesiredRevision = 0;
 TUniquePtr<FRetainedState> Retained;
 bool bCreate = false;
 bool ReferencesCurrent() const
 {
  return !Desired.IsValid() ? Operation == TEXT("clear") :
   ExactObject(Desired.Get(), USkeletalMesh::StaticClass()) && Desired->GetPathName() == DesiredPath
   && Desired->GetUniqueID() == DesiredRevision && FSoftObjectPath(DesiredPath).ResolveObject() == Desired.Get()
   && DesiredSkeleton->Matches(Desired->GetSkeleton());
 }
 bool TargetCurrent(UAnimationAsset* Current) const
 {
  return ExactTarget(Current) && Current->GetClass() == TargetClass && Current->GetPathName() == TargetPath
   && FSoftObjectPath(TargetPath).ResolveObject() == Current && Current->GetOutermost()->GetName() == PackagePath
   && (TargetClass != UAnimSequence::StaticClass() || Current->GetOuter() == Current->GetOutermost())
   && (bCreate || (Current == Target.Get() && Current->GetOutermost() == Package.Get() && Current->GetUniqueID() == TargetRevision));
 }
 bool PreimageCurrent(const UAnimationAsset& Current) const
 {
  USkeletalMesh* Mesh = nullptr;
  const EPreimage Kind = ProbePreviewMeshPreimage(Current, Mesh);
  return Old.IsValid() ? Kind == EPreimage::ResolvedValid && Mesh == Old.Get()
   && Old->GetPathName() == OldPath && FSoftObjectPath(OldPath).ResolveObject() == Old.Get()
   && OldSkeleton->Matches(Old->GetSkeleton()) : Kind == EPreimage::Empty;
 }
 bool BaselineCurrent() const
 {
  return ReferencesCurrent() && (bCreate ? FSoftObjectPath(TargetPath).ResolveObject() == nullptr :
   TargetCurrent(Target.Get()) && CanonicalSkeleton(*Target) && TargetSkeleton->Matches(Target->GetSkeleton())
   && PreimageCurrent(*Target) && Retained->Matches(*Target));
 }
};
class FPreviewMeshInverse final : public IAssetDocumentRollbackAction
{
public:
 FPreviewMeshInverse(TSharedRef<FPreparedPreviewMesh> S, TSharedRef<FCapture> C) : State(MoveTemp(S)), Capture(MoveTemp(C)) {}
 FStringView GetRollbackComponentId() const override { return RollbackId; }
 FAssetDocumentContractValidationResult Rollback(FStringView) override
 {
  if (bConsumed) return Result(false, TEXT("AnimationProvider.PreviewMesh.RollbackFailed"));
  bConsumed = true;
  if (!ContextValid() || !Capture->bOpened || !Capture->bClosed)
   return Result(false, TEXT("AnimationProvider.PreviewMesh.RollbackFailed"));
  // Other prepared families may have changed dependent state before our forward
  // was rejected. We own no write in that case and must not poison their inverse.
  if (!Capture->bForwardWritten) return Result(true);
  if (!State->TargetCurrent(State->Target.Get())
   || !State->TargetSkeleton->Matches(State->Target->GetSkeleton()) || !CanonicalSkeleton(*State->Target)
   || (State->Old.IsValid() && (!ExactObject(State->Old.Get(), USkeletalMesh::StaticClass())
    || State->Old->GetPathName() != State->OldPath || !State->OldSkeleton->Matches(State->Old->GetSkeleton()))))
   return Result(false, TEXT("AnimationProvider.PreviewMesh.RollbackFailed"));
  // A no-op or never-invoked contribution must not perform an unnecessary write.
  if (!State->PreimageCurrent(*State->Target))
  {
#if WITH_DEV_AUTOMATION_TESTS
   if (TestHooks) ++TestHooks->InverseSetterCalls;
#endif
   State->Target->SetPreviewMesh(State->Old.Get(), false);
  }
  bool bMatches = State->TargetCurrent(State->Target.Get()) && State->PreimageCurrent(*State->Target)
   && State->TargetSkeleton->Matches(State->Target->GetSkeleton()) && State->Retained->Matches(*State->Target);
#if WITH_DEV_AUTOMATION_TESTS
  bMatches = bMatches && !(TestHooks && TestHooks->bFailRollbackReadback);
#endif
  return Result(bMatches, TEXT("AnimationProvider.PreviewMesh.RollbackFailed"));
 }
private:
 TSharedRef<FPreparedPreviewMesh> State;
 TSharedRef<FCapture> Capture;
 bool bConsumed = false;
};
FAssetDocumentContractValidationResult ValidateLive(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 const FAssetDocumentMutationIntent& Intent, const FAssetDocumentRuntimeRequestDto& Request,
 const FPreparedPreviewMesh& State, const FCapture& Capture)
{
 if (!ContextValid() || !Capture.bOpened || Capture.bClosed || Invocation.TransactionId != Capture.Transaction
  || Invocation.RequestId != Request.RequestId || Invocation.RuntimeRevision != Request.ExpectedRuntimeRevision
  || Invocation.PrimitiveId != Primitive || Invocation.Version != 1 || Invocation.Phase != Intent.Phase)
  return Result(false, TEXT("AnimationProvider.OperationRejected"));
 FString Operation, Unused;
 if (!ValidateAssetDocumentPrimitiveInvocation(Invocation, MakeAnimationPreviewMeshSetDescriptor()).bValid
  || !ParseOperation(Invocation.ValueArguments, false, Operation, Unused) || Operation != State.Operation
  || !ExactHandles(Invocation, Operation == TEXT("set"))) return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 TUniquePtr<IAssetDocumentVerifiedHandleLease> TargetLease, ReferenceLease;
 UObject* Native = nullptr;
 auto Valid = AcquireTarget(Invocation, Handles, TargetLease, Native);
 if (!Valid.bValid) return Valid;
 auto* Target = static_cast<UAnimationAsset*>(Native);
 if (!State.TargetCurrent(Target) || !State.ReferencesCurrent() || !CanonicalSkeleton(*Target)
  || (!State.bCreate && !State.BaselineCurrent())) return Result(false);
 if (State.bCreate)
 {
  USkeletalMesh* Existing = nullptr;
  if (ProbePreviewMeshPreimage(*Target, Existing) != EPreimage::Empty)
   return Result(false, TEXT("AnimationProvider.PreviewMesh.UnsupportedPreimage"));
 }
 if (Operation == TEXT("set"))
 {
  Native = nullptr;
  Valid = Acquire(Invocation, Handles, TEXT("preview_mesh"), USkeletalMesh::StaticClass(), EAssetDocumentHandleAccess::ReadOnly, ReferenceLease, Native);
  if (!Valid.bValid) return Valid;
  if (Native != State.Desired.Get()) return Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
 }
 return Result(Compatible(*Target, State.Desired.Get()), TEXT("AnimationProvider.PreviewMesh.IncompatibleReference"));
}
// This private binding makes the prepared preflight mandatory for direct provider
// invocation as well. Core still owns capture, rollback ordering and transaction.
using FPreparedValidator = TFunction<FAssetDocumentContractValidationResult(const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&)>;
struct FActivePreparedCapture { FGuid Transaction; FPreparedValidator Validate; TSharedPtr<FCapture> Capture; };
// Single non-reentrant game-thread capture, not a request/handle registry.
const FActivePreparedCapture* ActivePreparedCapture = nullptr;
}
bool ReadMontageProfilePreviewMesh(const UAnimMontage& Target, USkeletalMesh*& OutMesh)
{
 OutMesh = nullptr;
 if (!ContextValid()) return false;
 USkeletalMesh* Mesh = nullptr;
 const EPreimage Kind = ProbePreviewMeshPreimage(Target, Mesh);
 if (Kind != EPreimage::Empty && Kind != EPreimage::ResolvedValid) return false;
 OutMesh = Mesh;
 return true;
}
}

FAssetDocumentContractValidationResult ValidatePreparedAnimationPreviewMeshInvocation(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles)
{
 using namespace AssetDocumentAnimationPreviewMesh;
 if (!ContextValid()) return Result(false, TEXT("AnimationProvider.OperationRejected"));
 return ActivePreparedCapture && ActivePreparedCapture->Transaction == Invocation.TransactionId
  ? ActivePreparedCapture->Validate(Invocation, Handles) : Result(false, TEXT("AnimationProvider.OperationRejected"));
}

FAssetDocumentContractValidationResult MarkPreparedAnimationPreviewMeshWrite(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles)
{
 using namespace AssetDocumentAnimationPreviewMesh;
 auto Valid = ValidatePreparedAnimationPreviewMeshInvocation(Invocation, Handles);
 if (!Valid.bValid) return Valid;
 auto& Capture = *ActivePreparedCapture->Capture;
 if (Capture.bForwardWritten) return Result(false, TEXT("AnimationProvider.OperationRejected"));
 Capture.bForwardWritten = true;
 return Result(true);
}

FAssetDocumentContractValidationResult PrepareAnimationPreviewMeshMutationBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 using namespace AssetDocumentAnimationPreviewMesh;
 OutBatch = FAssetDocumentPreparedMutationBatch();
 static bool bPreparing = false;
 if (!ContextValid() || bPreparing || !Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0
  || Request.Operation != EAssetDocumentRuntimeOperation::Apply)
  return Result(false, TEXT("AnimationProvider.OperationRejected"));
 TGuardValue<bool> Guard(bPreparing, true);
 const bool bSequence = Request.RequestedAssetClassPath == TEXT("/Script/Engine.AnimSequence");
 if (!CanonicalPath(Request.TargetAssetPath) || (!bSequence && Request.RequestedAssetClassPath != TEXT("/Script/Engine.AnimMontage")))
  return Result(false);
 UObject* Target = FSoftObjectPath(Request.TargetAssetPath).ResolveObject();
 // Sequence is an existing-only upstream leaf; omission does not create a target.
 if (bSequence && (!ExactObject(Target, UAnimSequence::StaticClass()) || Target->GetOuter() != Target->GetOutermost()
  || Target->GetPathName() != Request.TargetAssetPath)) return Result(false);
 if (Intents.Num() > 1) return Result(false, TEXT("AnimationProvider.OperationRejected"));
 if (Intents.IsEmpty()) return Result(true);
 const auto& Intent = Intents[0];
 if (Intent.StepIndex < 0 || Intent.Requirement.PrimitiveId != Primitive || Intent.Requirement.Version != 1
  || Intent.Requirement.RequiredByRouteId != TEXT("animation_profile") || Intent.InputSchemaId != Schema
  || (Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply && Intent.Phase != EAssetDocumentRuntimeRequestPhase::Repair))
  return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 auto State = MakeShared<FPreparedPreviewMesh>();
 State->TargetClass = bSequence ? UAnimSequence::StaticClass() : UAnimMontage::StaticClass();
 if (!ParseOperation(Intent.Intent, true, State->Operation, State->DesiredPath))
  return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 State->TargetPath = Request.TargetAssetPath; State->PackagePath = FSoftObjectPath(Request.TargetAssetPath).GetLongPackageName();
 if (State->Operation == TEXT("set"))
 {
  UObject* Desired = FSoftObjectPath(State->DesiredPath).ResolveObject();
  if (!ExactObject(Desired, USkeletalMesh::StaticClass())) return Result(false, TEXT("AnimationProvider.TargetClassMismatch"));
  State->Desired.Reset(static_cast<USkeletalMesh*>(Desired)); State->DesiredRevision = Desired->GetUniqueID();
  State->DesiredSkeleton = MakeUnique<FSkeletonIdentity>(State->Desired->GetSkeleton());
 }
 State->bCreate = Target == nullptr;
 if (!State->bCreate)
 {
  if (!ExactObject(Target, State->TargetClass)) return Result(false, TEXT("AnimationProvider.TargetClassMismatch"));
  State->Target.Reset(static_cast<UAnimationAsset*>(Target)); State->Package.Reset(Target->GetOutermost());
  State->TargetRevision = Target->GetUniqueID();
  if (!CanonicalSkeleton(*State->Target)) return Result(false);
  USkeletalMesh* Old = nullptr;
  const EPreimage Kind = ProbePreviewMeshPreimage(*State->Target, Old);
  if (Kind != EPreimage::Empty && Kind != EPreimage::ResolvedValid)
   return Result(false, TEXT("AnimationProvider.PreviewMesh.UnsupportedPreimage"));
  State->Old.Reset(Old); State->OldPath = Old ? Old->GetPathName() : FString();
  State->OldSkeleton = MakeUnique<FSkeletonIdentity>(Old ? Old->GetSkeleton() : nullptr);
  State->TargetSkeleton = MakeUnique<FSkeletonIdentity>(State->Target->GetSkeleton());
  State->Retained = MakeUnique<FRetainedState>(*State->Target);
  if (!Compatible(*State->Target, State->Desired.Get())) return Result(false, TEXT("AnimationProvider.PreviewMesh.IncompatibleReference"));
 }
 if (!State->BaselineCurrent()) return Result(false);
 auto Capture = MakeShared<FCapture>();
 FPreparedValidator Validator = [State, Capture, Intent, Request](const auto& Invocation, auto& Handles)
 { return ValidateLive(Invocation, Handles, Intent, Request, *State, *Capture); };
 auto Binding = MakeShared<FActivePreparedCapture>(); Binding->Validate = Validator; Binding->Capture = Capture;
 FAssetDocumentPreparedMutationBatch Prepared;
 Prepared.OpenMutationCapture = [State, Capture, Binding](const FGuid& Transaction, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!ContextValid() || !Transaction.IsValid() || Capture->bOpened || Capture->bClosed
   || ActivePreparedCapture != nullptr || !State->BaselineCurrent())
  {
   Diagnostics.Append(Result(false, TEXT("AnimationProvider.OperationRejected")).Diagnostics); return nullptr;
  }
  Capture->Transaction = Transaction; Capture->bOpened = true;
  Binding->Transaction = Transaction; ActivePreparedCapture = &Binding.Get();
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([Capture, Binding]()
  { if (ActivePreparedCapture == &Binding.Get()) ActivePreparedCapture = nullptr; Capture->bClosed = true; });
 };
 auto& Contribution = Prepared.Contributions.AddDefaulted_GetRef();
 Contribution.StepIndex = Intent.StepIndex;
 Contribution.OwnedSurfaceIds = {State->TargetPath + TEXT("::PreviewMesh")};
 Contribution.RawFootprint = Footprint(State->TargetPath, State->PackagePath);
 if (!State->bCreate) Contribution.InitialAction = MakeShared<FPreviewMeshInverse>(State, Capture);
 Contribution.ValidateInvocation = [Validator](const auto& Invocation, auto& Handles) { return Validator(Invocation, Handles); };
 OutBatch = MoveTemp(Prepared); return Result(true);
}
