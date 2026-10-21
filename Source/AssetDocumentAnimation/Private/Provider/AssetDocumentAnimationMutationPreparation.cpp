// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentAnimationMutationPreparation.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonState.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonPreparation.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Provider/Asset/AssetDocumentAnimationPreviewMeshOperations.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceAdditiveCompletion.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceAsset.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceName.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetReferencesComposition.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompression.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompressionDocumentInputs.h"
#include "Provider/Montage/MontageStructuralProvider.h"
#include "Provider/Montage/MontageSettings.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveOperations.h"

namespace AssetDocumentAnimationSkeleton
{
struct FCapture
{
 FGuid Transaction;
 bool bOpened = false;
 bool bClosed = false;
};
struct FPreparedSkeleton
{
 TStrongObjectPtr<UAnimMontage> Target;
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<USkeleton> Old;
 TStrongObjectPtr<USkeleton> Desired;
 FString TargetPath, PackagePath, OldPath, DesiredPath, Operation;
 FGuid OldGuid, DesiredGuid;
 uint64 TargetRevision = 0, DesiredRevision = 0;
 TUniquePtr<FRetainedState> Retained;
 TUniquePtr<const FRetainedState> ProjectedRetained;
 TSharedPtr<const AssetDocumentMontage::FPreparedStructuralEdit> Structure;
 bool bCreate = false;
 bool ReferencesCurrent() const
 {
  return (!Desired.IsValid() ? Operation == TEXT("clear") :
   ExactObject(Desired.Get(), USkeleton::StaticClass()) && Desired->GetPathName() == DesiredPath
   && Desired->GetGuid() == DesiredGuid && Desired->GetUniqueID() == DesiredRevision
   && FSoftObjectPath(DesiredPath).ResolveObject() == Desired.Get());
 }
 bool TargetCurrent(UAnimMontage* Current) const
 {
  return ExactObject(Current, UAnimMontage::StaticClass()) && Current->GetPathName() == TargetPath
   && FSoftObjectPath(TargetPath).ResolveObject() == Current && Current->GetOutermost()->GetName() == PackagePath
   && (bCreate || (Current == Target.Get() && Current->GetOutermost() == Package.Get()
    && Current->GetUniqueID() == TargetRevision));
 }
 bool OldReferenceCurrent() const
 {
  return !Old.IsValid() || (ExactObject(Old.Get(), USkeleton::StaticClass())
   && Old->GetPathName() == OldPath && Old->GetGuid() == OldGuid
   && FSoftObjectPath(OldPath).ResolveObject() == Old.Get());
 }
 bool RetainedAtRollback() const
 {
  // The phase comes from the actual sole publisher, never from accepting either shape.
  return (Structure && Structure->bSlotsPublished ? ProjectedRetained.Get() : Retained.Get())->Matches(*Target);
 }
 bool ForwardBaselineCurrent() const
 {
  if (!Structure) return BaselineCurrent();
  return Structure->bApplied && AssetDocumentMontage::ReadbackPreparedStructure(*Structure).bValid
   && (!Structure->bReplaceSlots || Structure->bSlotsPublished)
   && ReferencesCurrent() && OldReferenceCurrent() && TargetCurrent(Target.Get())
   && Target->GetSkeleton() == Old.Get() && Target->GetSkeletonGuid() == OldGuid
   && ProjectedRetained->Matches(*Target);
 }
 bool BaselineCurrent() const
 {
  return ReferencesCurrent() && (bCreate ? FSoftObjectPath(TargetPath).ResolveObject() == nullptr :
   TargetCurrent(Target.Get()) && Target->GetSkeleton() == Old.Get() && Target->GetSkeletonGuid() == OldGuid
   && OldReferenceCurrent() && Retained->Matches(*Target.Get()));
 }
};
class FSkeletonInverse final : public IAssetDocumentRollbackAction
{
public:
 FSkeletonInverse(TSharedRef<FPreparedSkeleton> InState, TSharedRef<FCapture> InCapture)
  : State(MoveTemp(InState)), Capture(MoveTemp(InCapture)) {}
 FStringView GetRollbackComponentId() const override { return RollbackId; }
 FAssetDocumentContractValidationResult Rollback(FStringView) override
 {
  if (bConsumed) return Result(false, TEXT("AnimationProvider.Skeleton.RollbackFailed"));
  bConsumed = true;
  if (!ContextValid() || !Capture->bOpened || !Capture->bClosed || !State->TargetCurrent(State->Target.Get())
   || !State->OldReferenceCurrent())
   return Result(false, TEXT("AnimationProvider.Skeleton.RollbackFailed"));
  if (State->Structure && !State->RetainedAtRollback()) return Result(false, TEXT("AnimationProvider.Skeleton.RollbackFailed"));
  if (!State->Structure || State->Target->GetSkeleton() != State->Old.Get() || State->Target->GetSkeletonGuid() != State->OldGuid)
  {
#if WITH_DEV_AUTOMATION_TESTS
   if (TestHooks) ++TestHooks->InverseSetterCalls;
#endif
   State->Target->SetSkeleton(State->Old.Get());
  }
  bool bMatches = State->TargetCurrent(State->Target.Get()) && State->Target->GetSkeleton() == State->Old.Get()
   && State->Target->GetSkeletonGuid() == State->OldGuid && State->RetainedAtRollback();
#if WITH_DEV_AUTOMATION_TESTS
  bMatches = bMatches && !(TestHooks && TestHooks->bFailRollbackReadback);
#endif
  return Result(bMatches, TEXT("AnimationProvider.Skeleton.RollbackFailed"));
 }
private:
 TSharedRef<FPreparedSkeleton> State;
 TSharedRef<FCapture> Capture;
 bool bConsumed = false;
};

FAssetDocumentContractValidationResult ValidateLive(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 const FAssetDocumentMutationIntent& Intent, const FAssetDocumentRuntimeRequestDto& Request,
 const FPreparedSkeleton& State, const FCapture& Capture)
{
 if (!ContextValid() || !Capture.bOpened || Capture.bClosed || Invocation.TransactionId != Capture.Transaction
  || Invocation.RequestId != Request.RequestId || Invocation.RuntimeRevision != Request.ExpectedRuntimeRevision
  || Invocation.PrimitiveId != Primitive || Invocation.Version != 1 || Invocation.Phase != Intent.Phase)
  return Result(false, TEXT("AnimationProvider.OperationRejected"));
 FString Operation, Unused;
 if (!ParseOperation(Invocation.ValueArguments, false, Operation, Unused) || Operation != State.Operation
  || !ExactHandles(Invocation, Operation == TEXT("set"))) return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 TUniquePtr<IAssetDocumentVerifiedHandleLease> TargetLease, ReferenceLease;
 UObject* NativeTarget = nullptr;
 auto Valid = Acquire(Invocation, Handles, TEXT("asset"), UAnimMontage::StaticClass(), EAssetDocumentHandleAccess::Mutable, TargetLease, NativeTarget);
 if (!Valid.bValid) return Valid;
 auto* Montage = static_cast<UAnimMontage*>(NativeTarget);
 if (!State.TargetCurrent(Montage) || !State.ReferencesCurrent() || !CanonicalSkeleton(*Montage)
  || (!State.bCreate && !State.ForwardBaselineCurrent())) return Result(false);
 if (Operation == TEXT("set"))
 {
  UObject* NativeReference = nullptr;
  Valid = Acquire(Invocation, Handles, TEXT("skeleton"), USkeleton::StaticClass(), EAssetDocumentHandleAccess::ReadOnly, ReferenceLease, NativeReference);
  if (!Valid.bValid) return Valid;
  if (NativeReference != State.Desired.Get()) return Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
 }
 return CompatibleResult(Montage->SlotAnimTracks, State.Desired.Get());
}
}

static FAssetDocumentContractValidationResult PrepareAnimationSkeletonMutationBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch,
 TSharedPtr<const AssetDocumentMontage::FPreparedStructuralEdit> Structure = {})
{
 using namespace AssetDocumentAnimationSkeleton;
 OutBatch = FAssetDocumentPreparedMutationBatch();
 static bool bPreparing = false;
 if (!ContextValid() || bPreparing || !Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0
  || Request.Operation != EAssetDocumentRuntimeOperation::Apply)
  return Result(false, TEXT("AnimationProvider.OperationRejected"));
 TGuardValue<bool> PreparingGuard(bPreparing, true);
 if (!CanonicalPath(Request.TargetAssetPath) || Request.RequestedAssetClassPath != TEXT("/Script/Engine.AnimMontage"))
  return Result(false, TEXT("AnimationProvider.Skeleton.InvalidTarget"));
 if (Intents.Num() > 1) return Result(false, TEXT("AnimationProvider.OperationRejected"));
 if (Intents.IsEmpty()) return Result(true);
 const auto& Intent = Intents[0];
 if (Intent.StepIndex < 0 || Intent.Requirement.PrimitiveId != Primitive || Intent.Requirement.Version != 1
  || Intent.Requirement.RequiredByRouteId != TEXT("animation_profile") || Intent.InputSchemaId != Schema
  || (Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply && Intent.Phase != EAssetDocumentRuntimeRequestPhase::Repair))
  return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 auto State = MakeShared<FPreparedSkeleton>();
 State->Structure = MoveTemp(Structure);
 if (State->Structure && (State->Structure->RequestId != Request.RequestId
  || State->Structure->RuntimeRevision != Request.ExpectedRuntimeRevision
  || State->Structure->TargetPath != Request.TargetAssetPath || State->Structure->bApplied
  || State->Structure->bMutated || State->Structure->bSlotsPublished
  || !AssetDocumentMontage::BaselineCurrent(*State->Structure))) return Result(false);
 if (!ParseOperation(Intent.Intent, true, State->Operation, State->DesiredPath))
  return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 State->TargetPath = Request.TargetAssetPath;
 State->PackagePath = FSoftObjectPath(Request.TargetAssetPath).GetLongPackageName();
 if (State->Operation == TEXT("set"))
 {
  UObject* Desired = FSoftObjectPath(State->DesiredPath).ResolveObject();
  if (!ExactObject(Desired, USkeleton::StaticClass())) return Result(false, TEXT("AnimationProvider.TargetClassMismatch"));
  State->Desired.Reset(static_cast<USkeleton*>(Desired));
  State->DesiredGuid = State->Desired->GetGuid();
  State->DesiredRevision = State->Desired->GetUniqueID();
 }
 UObject* Target = FSoftObjectPath(Request.TargetAssetPath).ResolveObject();
 State->bCreate = Target == nullptr;
 if (!State->bCreate)
 {
  if (!ExactObject(Target, UAnimMontage::StaticClass())) return Result(false, TEXT("AnimationProvider.TargetClassMismatch"));
  State->Target.Reset(static_cast<UAnimMontage*>(Target)); State->Package.Reset(Target->GetOutermost());
  State->TargetRevision = Target->GetUniqueID();
  if (!CanonicalSkeleton(*State->Target.Get())) return Result(false);
  State->Old.Reset(State->Target->GetSkeleton()); State->OldGuid = State->Target->GetSkeletonGuid();
  State->OldPath = State->Old.IsValid() ? State->Old->GetPathName() : FString();
  State->Retained = MakeUnique<FRetainedState>(*State->Target.Get());
  if (State->Structure)
  {
   if (State->Structure->Target.Get() != State->Target.Get()) return Result(false);
   State->ProjectedRetained = MakeUnique<FRetainedState>(State->Structure->DesiredSlots);
  }
  const auto Compatibility = CompatibleResult(State->Structure ? State->ProjectedRetained->Tracks : State->Retained->Tracks, State->Desired.Get());
  if (!Compatibility.bValid) return Compatibility;
 }
 if ((State->Structure && State->bCreate) || !State->BaselineCurrent()) return Result(false);
 auto Capture = MakeShared<FCapture>();
 FAssetDocumentPreparedMutationBatch Prepared;
 Prepared.OpenMutationCapture = [State, Capture](const FGuid& Transaction, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!ContextValid() || !Transaction.IsValid() || Capture->bOpened || Capture->bClosed || !State->BaselineCurrent())
  {
   Diagnostics.Append(Result(false, TEXT("AnimationProvider.OperationRejected")).Diagnostics);
   return nullptr;
  }
  Capture->Transaction = Transaction; Capture->bOpened = true;
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([Capture]() { Capture->bClosed = true; });
 };
 FAssetDocumentPreparedMutationContribution Contribution;
 Contribution.StepIndex = Intent.StepIndex;
 Contribution.OwnedSurfaceIds = {State->TargetPath + TEXT("::Skeleton")};
 Contribution.RawFootprint = Footprint(State->TargetPath, State->PackagePath);
 if (!State->bCreate) Contribution.InitialAction = MakeShared<FSkeletonInverse>(State, Capture);
 Contribution.ValidateInvocation = [State, Capture, Intent, Request](const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles)
 {
  return ValidateLive(Invocation, Handles, Intent, Request, *State, *Capture);
 };
 Prepared.Contributions.Add(MoveTemp(Contribution));
 OutBatch = MoveTemp(Prepared);
 return Result(true);
}

FAssetDocumentContractValidationResult PrepareAnimationSkeletonMutationBatchAfterStructure(
 const FAssetDocumentRuntimeRequestDto& Request, const FAssetDocumentMutationIntent& SkeletonIntent,
 TSharedRef<const AssetDocumentMontage::FPreparedStructuralEdit> Structure,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 return PrepareAnimationSkeletonMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&SkeletonIntent, 1), OutBatch, Structure);
}

FAssetDocumentContractValidationResult PrepareAssetDocumentAnimationSequenceCompressionDocumentBatch(
 const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentAnimationRequestProvider& Owner,
 const FAssetDocumentRuntimeValueDto& DocumentInputs, int32 StepIndex, EAssetDocumentRuntimeRequestPhase Phase,
 TConstArrayView<FAssetDocumentMutationIntent> OtherIntents,
 TArray<FAssetDocumentMutationIntent>& OutActualIntents, FAssetDocumentPreparedMutationBatch& OutBatch)
{
 return AssetDocumentSequenceCompressionDocumentInputs::Prepare(
  Request, Owner, DocumentInputs, StepIndex, Phase, OtherIntents, OutActualIntents, OutBatch);
}

// Family capture remains request-owned; Core still coalesces physical resources.
FAssetDocumentContractValidationResult PrepareAssetDocumentAnimationMutationBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 OutBatch = FAssetDocumentPreparedMutationBatch();
 const bool bSequence = Request.RequestedAssetClassPath == TEXT("/Script/Engine.AnimSequence");
 if (Intents.IsEmpty()) return bSequence ? PrepareAnimationPreviewMeshMutationBatch(Request, Intents, OutBatch)
  : PrepareAnimationSkeletonMutationBatch(Request, Intents, OutBatch);
 TSet<FString> Surfaces;
 TSet<int32> Steps;
 for (const auto& Intent : Intents)
 {
  const auto& Id = Intent.Requirement.PrimitiveId;
  if ((bSequence && Id != TEXT("animation.asset.preview_mesh.set.v1") && Id != TEXT("animation.sequence.complete_edit.v2")
    && Id != TEXT("animation.sequence.retarget_source_asset.set.v2") && Id != TEXT("animation.sequence.retarget_source_name.set.v1") && Id != TEXT("animation.sequence.compression.settings.set.v1"))
   || (!bSequence && Id != TEXT("animation.asset.skeleton.set.v1") && Id != TEXT("animation.asset.preview_mesh.set.v1")
    && Id != TEXT("animation.curve.float.replace.v2")
    && Id != TEXT("animation.montage.slot_tracks.replace.v1") && Id != TEXT("animation.montage.sections.mutate.v1")
    && Id != AssetDocumentMontageSettings::Primitive)
   || Intent.StepIndex < 0 || Steps.Contains(Intent.StepIndex) || Surfaces.Contains(Id))
   return AssetDocumentAnimationSkeleton::Result(false, TEXT("AnimationProvider.InvalidValueShape"));
  Steps.Add(Intent.StepIndex); Surfaces.Add(Id);
 }
 FAssetDocumentPreparedMutationBatch Combined;
 TArray<FAssetDocumentCoreMutationCaptureOpener> Openers;
 const auto* RetargetAsset = bSequence ? Intents.FindByPredicate([](const auto& I)
  { return I.Requirement.PrimitiveId == TEXT("animation.sequence.retarget_source_asset.set.v2"); }) : nullptr;
 const auto* RetargetName = bSequence ? Intents.FindByPredicate([](const auto& I)
  { return I.Requirement.PrimitiveId == TEXT("animation.sequence.retarget_source_name.set.v1"); }) : nullptr;
 if (RetargetAsset && RetargetName)
 {
  const FAssetDocumentMutationIntent Pair[] = {*RetargetAsset, *RetargetName};
  FAssetDocumentPreparedMutationBatch Family;
  const auto Valid = PrepareAnimationSequenceRetargetReferencesBatch(Request, Pair, Family);
  if (!Valid.bValid) return Valid;
  if (!Family.OpenMutationCapture || Family.PackageFinalizer.IsValid())
   return AssetDocumentAnimationSkeleton::Result(false, TEXT("AnimationProvider.OperationRejected"));
  Combined.Contributions.Append(MoveTemp(Family.Contributions)); Openers.Add(MoveTemp(Family.OpenMutationCapture));
 }
 for (const auto& Intent : Intents)
 {
  if (RetargetAsset && RetargetName && (&Intent == RetargetAsset || &Intent == RetargetName)) continue;
  FAssetDocumentPreparedMutationBatch Family;
  const TConstArrayView<FAssetDocumentMutationIntent> One(&Intent, 1);
  auto Valid = Intent.Requirement.PrimitiveId == TEXT("animation.sequence.compression.settings.set.v1")
   ? PrepareAnimationSequenceCompressionBatch(Request, One, Family)
   : Intent.Requirement.PrimitiveId == TEXT("animation.sequence.retarget_source_name.set.v1")
   ? PrepareAnimationSequenceRetargetSourceNameBatch(Request, One, Family)
   : Intent.Requirement.PrimitiveId == TEXT("animation.sequence.retarget_source_asset.set.v2")
   ? PrepareAnimationSequenceRetargetSourceAssetBatch(Request, One, Family)
   : Intent.Requirement.PrimitiveId == TEXT("animation.sequence.complete_edit.v2")
   ? PrepareAnimationSequenceAdditiveCompletionBatch(Request, One, Family)
   : Intent.Requirement.PrimitiveId == TEXT("animation.asset.skeleton.set.v1")
   ? PrepareAnimationSkeletonMutationBatch(Request, One, Family)
   : Intent.Requirement.PrimitiveId == TEXT("animation.asset.preview_mesh.set.v1")
   ? PrepareAnimationPreviewMeshMutationBatch(Request, One, Family)
   : Intent.Requirement.PrimitiveId == TEXT("animation.curve.float.replace.v2")
   ? PrepareAnimationFloatCurveMutationBatch(Request, One, Family)
   : Intent.Requirement.PrimitiveId == AssetDocumentMontageSettings::Primitive
   ? PrepareAnimationMontageSettingsBatch(Request, One, Family)
   : PrepareAnimationMontageStructuralMutationBatch(Request, One, Family);
  if (!Valid.bValid) return Valid;
  if (!Family.OpenMutationCapture || Family.PackageFinalizer.IsValid())
   return AssetDocumentAnimationSkeleton::Result(false, TEXT("AnimationProvider.OperationRejected"));
  Combined.Contributions.Append(MoveTemp(Family.Contributions));
  Openers.Add(MoveTemp(Family.OpenMutationCapture));
 }
 Combined.Contributions.Sort([](const auto& A, const auto& B) { return A.StepIndex < B.StepIndex; });
 Combined.OpenMutationCapture = [Openers = MoveTemp(Openers)](
  const FGuid& Transaction, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics) mutable
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  TArray<TUniquePtr<FAssetDocumentCoreMutationCaptureLease>> Leases;
  for (auto& Open : Openers)
  {
   auto Lease = Open(Transaction, Diagnostics);
   if (!Lease || !Lease->IsValid())
   {
    for (int32 Index = Leases.Num() - 1; Index >= 0; --Index) Leases[Index]->Close();
    return nullptr;
   }
   Leases.Add(MoveTemp(Lease));
  }
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([Leases = MoveTemp(Leases)]() mutable
  {
   for (int32 Index = Leases.Num() - 1; Index >= 0; --Index) Leases[Index]->Close();
   Leases.Reset();
  });
 };
 OutBatch = MoveTemp(Combined);
 return AssetDocumentAnimationSkeleton::Result(true);
}
