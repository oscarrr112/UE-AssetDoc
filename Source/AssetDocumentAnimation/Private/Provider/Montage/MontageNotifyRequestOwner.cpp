// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Montage/MontageNotifyRequestOwner.h"
#include "Provider/Montage/MontageNotifyStaging.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "AssetDocumentAnimationPrimitiveProvider.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonPreparation.h"
#include "AssetDocumentAnimationMutationPreparation.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Provider/Montage/MontageProfileSnapshot.h"
#include "Provider/Montage/MontageStructureCheck.h"
#include "Provider/Montage/MontageSettings.h"
#include "Provider/Sequence/AnimationAssetAuthoring.h"
#include "Animation/AnimSequence.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentEditorNativeState.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentMontage
{
struct FRequestReadBinding
{
 FAssetDocumentRuntimeRequestDto Request;
 TStrongObjectPtr<UAnimMontage> StagedTarget;
 FString StagedObjectPath;
 FString StagedFilePath;
 uint32 StagedTypeRevision = 0;
 uint64 StagedRuntimeRevision = 0;
 bool bBound = false;
 bool bClosed = false;
 bool bPrepared = false;
 bool bRequiresCapture = false;
 bool bCaptureOpened = false;
 bool bCaptureClosed = false;
 bool bStagedUsed = false;
 bool bStagedConsumed = false;
};
namespace
{
constexpr const TCHAR* EventPrimitive = TEXT("animation.montage.notifies.replace.v1");
constexpr const TCHAR* StatePrimitive = TEXT("animation.montage.notify_states.replace.v1");
bool IsNotifyPrimitive(FStringView Id) { return Id == EventPrimitive || Id == StatePrimitive; }
FAssetDocumentContractValidationResult Reject(const TCHAR* Message)
{
 return Result(false, TEXT("AnimationProvider.Montage.NotifyOwnerRejected"), Message);
}
FAssetDocumentPrimitiveOutcome Failed(const TCHAR* Message)
{
 FAssetDocumentPrimitiveOutcome O;
 O.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
 O.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 O.Diagnostics = Reject(Message).Diagnostics;
 return O;
}
}

FAssetDocumentPrimitiveDescriptor MakeMontageNotifyReplacementDescriptor(bool bStates)
{
 FAssetDocumentPrimitiveDescriptor D;
 D.PrimitiveId = bStates ? StatePrimitive : EventPrimitive;
 D.Version = 1; D.ProviderId = TEXT("animation"); D.Owner = EAssetDocumentPrimitiveOwner::Animation;
 D.ExecutionThread = EAssetDocumentExecutionThread::GameThread; D.Access = EAssetDocumentPrimitiveAccess::Mutating;
 D.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {{TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, true}};
 D.bRequiresActiveTransaction = true; D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.InputSchemaId = D.PrimitiveId + TEXT(".input.v1"); D.OutputSchemaId = D.PrimitiveId + TEXT(".output.v1");
 D.MutationFootprintSchemaId = D.PrimitiveId + TEXT(".footprint.v1");
 D.StableDiagnosticCodes = {TEXT("AnimationProvider.Montage.NotifyOwnerRejected"), TEXT("AnimationProvider.Montage.InvalidIntent"),
  TEXT("AnimationProvider.Montage.PreparedStateDrift"), TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("AnimationProvider.TransactionUnavailable"),
  TEXT("AnimationProvider.Montage.InvalidContext"), TEXT("AnimationProvider.Montage.NotifyStagingFailed"), TEXT("AnimationProvider.Montage.NotifyPlacementRejected"),
  TEXT("AnimationProvider.Montage.DependencyUnavailable"), TEXT("AnimationProvider.Montage.ReadbackMismatch"),
  TEXT("EditorNativeState.StagedNotifyInvalidContext"), TEXT("EditorNativeState.StagedNotifyInvalidTarget"), TEXT("EditorNativeState.StagedNotifyIdentityDrift"),
  TEXT("PrimitiveInvocation.InvalidValueShape"), TEXT("PropertyBatchWrite.DuplicateProperty"), TEXT("PropertyBatchWrite.UnknownProperty"),
  TEXT("PropertyToken.InvalidPayload"), TEXT("PropertyWrite.DecodeFailed"), TEXT("PropertyWrite.AccessFailed"), TEXT("PropertyWrite.ReadbackFailed")};
 auto& A = D.Admission;
 A.Disposition = EAssetDocumentPrimitiveDisposition::AdmittedBridge;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted;
 A.ExactEngineVersion = TEXT("5.7.4"); A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 A.PublicModule = TEXT("AssetDocumentAnimation"); A.PublicHeader = TEXT("AssetDocumentAnimationMutationPreparation.h");
 A.ExportedAuthoritativeApi = TEXT("StageMontageNotifyObject;ConfigureAssetDocumentEditorStagedMontageNotifyProperties;PublishMontageNotifyEvents");
 A.AuthorityContextId = TEXT("AD.ANIMATION.MONTAGE.NOTIFY-STAGING.v1|AD.ANIMATION.MONTAGE.NOTIFIES-PUBLISH.v1");
 A.PrimaryOperationId = D.PrimitiveId;
 A.IdentityLifetimeContractId = TEXT("animation.montage.request-local-actual-staged-set.v1");
 A.TransactionContractId = TEXT("animation.core-active-transaction.prepared-composition.v1");
 A.RollbackContractId = TEXT("animation.montage.notify-staging.rollback.v1");
 A.CompletionContractId = TEXT("animation.montage.notify-mixed-timeline-configured-publication.v1");
 A.SaveReloadEvidenceId = TEXT("animation.montage.notify.launcher-5.7.4-save-reload.v1");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimationProvider.MontageStructural.NotifyReplacement.OwnerLifecycle"),
  TEXT("UEAssetDoc.MontageNotifyIntegration.ApplyFile.Rollback")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimationProvider.MontageStructural.NotifyReplacement.OwnerLifecycle"),
  TEXT("UEAssetDoc.Editor.StagedMontageNotify.PropertiesSuccess"), TEXT("UEAssetDoc.MontageNotifyIntegration.ApplyFile.Success")};
 A.GovernanceId = bStates ? TEXT("ANIMATION-MONTAGE-NOTIFY-STATES-REPLACE-v1") : TEXT("ANIMATION-MONTAGE-NOTIFIES-REPLACE-v1");
 A.SourceMarker = FString::Printf(TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(%s)"), *A.GovernanceId);
 A.RawLifecycleAllowlistId = FString::Printf(TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(%s|%s)"), *A.GovernanceId, *D.PrimitiveId);
 A.IndependentCandidateReviewId = TEXT("candidate-review.initial/") + A.GovernanceId;
 A.IndependentBridgeReviewId = TEXT("bridge-review.initial/") + A.GovernanceId;
 A.HelperReplacementCondition = TEXT("Stock exported APIs cover safe staged notify configuration and whole mixed-timeline publication with equivalent rollback.");
 A.RemovalCondition = TEXT("Migrate the unique native caller and retire its private allocation/publication exceptions.");
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|request-local-staged-notify-configure-mixed-publication|") + D.PrimitiveId;
 return D;
}

FMontageNotifyRequestOwner::FMontageNotifyRequestOwner()
 : Inner(CreateAssetDocumentAnimationPrimitiveProvider()), ReadBinding(MakeShared<FRequestReadBinding>())
{
 for (const auto& D : Inner->DescribePrimitives()) Descriptors.Add(D);
 Descriptors.Add(MakeMontageNotifyReplacementDescriptor(false));
 Descriptors.Add(MakeMontageNotifyReplacementDescriptor(true));
 Descriptors.Add(MakeAnimationMontageSettingsDescriptor());
 Descriptors.Add(MakeMontageStructureCheckDescriptor());
 Descriptors.Add(MakeAnimationAssetAuthoringSnapshotDescriptor());
 Descriptors.Add(MakeAnimationAssetAuthoringEditDescriptor());
}
FString FMontageNotifyRequestOwner::GetProviderId() const { return Inner->GetProviderId(); }
TConstArrayView<FAssetDocumentPrimitiveDescriptor> FMontageNotifyRequestOwner::DescribePrimitives() const { return Descriptors; }
FAssetDocumentProviderAvailability FMontageNotifyRequestOwner::InspectAvailability() const { return Inner->InspectAvailability(); }

FAssetDocumentContractValidationResult FMontageNotifyRequestOwner::BindRequest(const FAssetDocumentRuntimeRequestDto& Request)
{
 const FSoftObjectPath Path(Request.TargetAssetPath);
 if (!IsStructuralContext() || ReadBinding->bClosed || ReadBinding->bBound || bPreparationAttempted
  || !Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0
  || (Request.RequestedAssetClassPath != UAnimMontage::StaticClass()->GetPathName()
   && Request.RequestedAssetClassPath != UAnimSequence::StaticClass()->GetPathName())
  || !Path.IsValid() || !Path.GetSubPathString().IsEmpty() || Path.ToString() != Request.TargetAssetPath
  || !FPackageName::IsValidObjectPath(Request.TargetAssetPath))
  return Reject(TEXT("A request-local Animation owner binds one exact Montage or Sequence request and revision once."));
 ReadBinding->Request = Request; ReadBinding->bBound = true;
 RequestId = Request.RequestId; Revision = Request.ExpectedRuntimeRevision; TargetPath = Request.TargetAssetPath;
 return Result(true);
}
void FMontageNotifyRequestOwner::CloseRequest()
{
 ReadBinding->bClosed = true; ReadBinding->StagedTarget.Reset();
}
TUniquePtr<FAssetDocumentAnimationStagedReadLease> FMontageNotifyRequestOwner::BindStagedRead(
 UAnimMontage& Target, const FString& StagedPackagePath, uint64 RuntimeRevision,
 TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
 if (!IsStructuralContext() || !ReadBinding->bBound || ReadBinding->bClosed || ReadBinding->bStagedUsed
  || !ReadBinding->bPrepared || (ReadBinding->bRequiresCapture && !ReadBinding->bCaptureClosed)
  || RuntimeRevision == 0 || !IsValid(&Target) || Target.GetClass() != UAnimMontage::StaticClass()
  || Target.IsTemplate() || Target.HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad)
  || Target.GetOuter() != Target.GetOutermost() || Target.GetPathName() == TargetPath
  || Target.GetName() != FPackageName::ObjectPathToObjectName(TargetPath)
  || StagedPackagePath.IsEmpty()
  || !FPaths::IsSamePath(Target.GetOutermost()->GetLoadedPath().GetLocalFullPath(), StagedPackagePath))
 {
  Diagnostics.Append(Reject(TEXT("Staged snapshot requires the exact loaded staged Montage, completed capture and pinned request revision once.")).Diagnostics);
  return {};
 }
 ReadBinding->StagedTarget.Reset(&Target); ReadBinding->bStagedUsed = true;
 ReadBinding->StagedObjectPath = Target.GetPathName(); ReadBinding->StagedFilePath = StagedPackagePath;
 ReadBinding->StagedTypeRevision = Target.GetUniqueID(); ReadBinding->StagedRuntimeRevision = RuntimeRevision;
 return MakeUnique<FAssetDocumentAnimationStagedReadLease>([Binding = ReadBinding]() { Binding->StagedTarget.Reset(); });
}

FAssetDocumentContractValidationResult FMontageNotifyRequestOwner::Prepare(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 OutBatch = FAssetDocumentPreparedMutationBatch();
 if (!ReadBinding->bBound)
 {
  const auto Bound = BindRequest(Request);
  if (!Bound.bValid) return Bound;
 }
 if (!IsStructuralContext() || ReadBinding->bClosed || bPreparationAttempted
  || Request.RequestId != RequestId || Request.ExpectedRuntimeRevision != Revision || Request.TargetAssetPath != TargetPath
  || Request.RequestedAssetClassPath != ReadBinding->Request.RequestedAssetClassPath
  || Request.Operation != ReadBinding->Request.Operation)
  return Reject(TEXT("The request owner accepts exactly one preparation attempt for its bound request."));
 bPreparationAttempted = true;
 if (!RequestId.IsValid() || Revision == 0 || Request.Operation != EAssetDocumentRuntimeOperation::Apply)
  return Reject(TEXT("Preparation requires the actual Apply request and revision."));
 auto BindCapture = [Binding = ReadBinding](FAssetDocumentPreparedMutationBatch& Batch)
 {
  // The existing staged-read API remains Montage-only, including its body.
  Binding->bPrepared = Binding->Request.RequestedAssetClassPath == UAnimMontage::StaticClass()->GetPathName();
  Binding->bRequiresCapture = !Batch.Contributions.IsEmpty();
  if (!Batch.OpenMutationCapture) return;
  auto Open = MoveTemp(Batch.OpenMutationCapture);
  Batch.OpenMutationCapture = [Binding, Open = MoveTemp(Open)](const FGuid& TransactionId,
   TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics) mutable -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
  {
   if (Binding->bClosed || Binding->bCaptureOpened) return {};
   auto Lease = Open(TransactionId, Diagnostics);
   if (!Lease || !Lease->IsValid()) return {};
   Binding->bCaptureOpened = true;
   return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([Binding, Lease = MoveTemp(Lease)]() mutable
   { Lease->Close(); Lease.Reset(); Binding->bCaptureClosed = true; });
  };
 };
 if (Request.RequestedAssetClassPath == UAnimSequence::StaticClass()->GetPathName())
 {
  const auto Valid = PrepareAssetDocumentAnimationMutationBatch(Request, Intents, OutBatch);
  if (Valid.bValid) BindCapture(OutBatch);
  return Valid;
 }
 if (FSoftObjectPath(TargetPath).ResolveObject() == nullptr
  && !FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(TargetPath)))
 {
  const auto Valid = PrepareDeferredCreate(Intents, OutBatch);
  if (Valid.bValid) BindCapture(OutBatch);
  return Valid;
 }
 int32 NotifyCount = 0;
 for (const auto& Intent : Intents) if (IsNotifyPrimitive(Intent.Requirement.PrimitiveId)) ++NotifyCount;
 if (NotifyCount == 0)
 {
  const auto Valid = PrepareAssetDocumentAnimationMutationBatch(Request, Intents, OutBatch);
  if (Valid.bValid) BindCapture(OutBatch);
  return Valid;
 }
 // Both notify families share one complete timeline contribution. Disjoint
 // Animation families retain their original capture and inverse contracts.
 if (NotifyCount != 1)
  return Reject(TEXT("Both notify families must share one complete timeline intent rather than overlapping contributions."));
 const auto* NotifyIntent = Intents.FindByPredicate([](const auto& I) { return IsNotifyPrimitive(I.Requirement.PrimitiveId); });
 const auto& Intent = *NotifyIntent;
 const bool bStructure = Intent.Intent.ObjectKeys.Contains(TEXT("structure"));
 TArray<FAssetDocumentMutationIntent> OtherIntents;
 int32 PreviousRank = -1, PreviousStep = -1;
 for (const auto& Input : Intents)
 {
  const auto& Id = Input.Requirement.PrimitiveId;
  const int32 Rank = Id == TEXT("animation.curve.float.replace.v2") ? 0
   : Id == TEXT("animation.asset.preview_mesh.set.v1") ? 1
   : Id == TEXT("animation.asset.skeleton.set.v1") ? (bStructure ? 3 : 2) : IsNotifyPrimitive(Id) ? (bStructure ? 2 : 3)
   : Id == AssetDocumentMontageSettings::Primitive ? 4 : INDEX_NONE;
  // These dependencies are real retained-state contracts: curve snapshots pin
  // preview/skeleton; preview pins skeleton. Structural timelines precede skeleton.
  if (Rank == INDEX_NONE || Rank <= PreviousRank || Input.StepIndex <= PreviousStep)
   return Reject(bStructure
    ? TEXT("Structural composition requires curves, preview, one combined timeline, skeleton, then settings in that order.")
    : TEXT("Animation composition requires curves, preview, skeleton, one complete notify intent, then settings in that order."));
  PreviousRank = Rank; PreviousStep = Input.StepIndex;
  if (!IsNotifyPrimitive(Id)) OtherIntents.Add(Input);
 }
 if (Request.RequestedAssetClassPath != TEXT("/Script/Engine.AnimMontage") || Intent.StepIndex < 0
  || Intent.Requirement.Version != 1 || Intent.Requirement.RequiredByRouteId != TEXT("animation_profile")
  || Intent.InputSchemaId != Intent.Requirement.PrimitiveId + TEXT(".input.v1")
  || Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply)
  return Reject(TEXT("Notify replacement requires the exact Montage Apply intent."));
 const FSoftObjectPath Path(TargetPath);
 if (!Path.IsValid() || !Path.GetSubPathString().IsEmpty() || Path.ToString() != TargetPath || !FPackageName::IsValidObjectPath(TargetPath))
  return Reject(TEXT("Notify target must be a canonical top-level loaded asset path."));
 auto* Target = Cast<UAnimMontage>(Path.ResolveObject());
 if (!IsValid(Target) || Target->GetClass() != UAnimMontage::StaticClass() || Target->GetPathName() != TargetPath
  || Target->IsTemplate() || Target->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad))
  return Reject(TEXT("The exact Montage target must already be loaded."));
 FAssetDocumentPreparedMutationBatch Candidate;
 TSharedPtr<FPreparedNotifyStaging> State;
 auto Valid = PrepareMontageNotifyReplacement(*Target, Intent.Intent, Intent.Requirement.PrimitiveId == StatePrimitive,
  Request, Intent.StepIndex, Candidate, State);
 if (!Valid.bValid) return Valid;
 if (!State.IsValid() || !Candidate.OpenMutationCapture || Candidate.Contributions.Num() != 1
  || Candidate.Contributions[0].StepIndex != Intent.StepIndex || !Candidate.Contributions[0].InitialAction
  || !Candidate.Contributions[0].ValidateInvocation || Candidate.PackageFinalizer.IsValid())
  return Reject(TEXT("Composite preparation did not return one complete captured inverse contribution."));
 const auto Structure = GetMontageNotifyPreparedStructure(*State);
 for (const auto& OtherIntent : OtherIntents)
 {
  FAssetDocumentPreparedMutationBatch Other;
  Valid = Structure && OtherIntent.Requirement.PrimitiveId == TEXT("animation.asset.skeleton.set.v1")
   ? PrepareAnimationSkeletonMutationBatchAfterStructure(Request, OtherIntent, Structure.ToSharedRef(), Other)
   : PrepareAssetDocumentAnimationMutationBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&OtherIntent, 1), Other);
  if (!Valid.bValid) return Valid;
  if (!Other.OpenMutationCapture || Other.PackageFinalizer.IsValid()) return Reject(TEXT("Other Animation families require their existing capture contract."));
  Other.Contributions.Append(MoveTemp(Candidate.Contributions));
  Other.Contributions.Sort([](const auto& A, const auto& B) { return A.StepIndex < B.StepIndex; });
  auto Earlier = MoveTemp(Other.OpenMutationCapture); auto Timeline = MoveTemp(Candidate.OpenMutationCapture);
  Other.OpenMutationCapture = [Earlier = MoveTemp(Earlier), Timeline = MoveTemp(Timeline)](
   const FGuid& TransactionId, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics) mutable
   -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
  {
   auto First = Earlier(TransactionId, Diagnostics);
   if (!First || !First->IsValid()) return {};
   auto Last = Timeline(TransactionId, Diagnostics);
   if (!Last || !Last->IsValid()) { First->Close(); return {}; }
   return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([First = MoveTemp(First), Last = MoveTemp(Last)]() mutable
   { Last->Close(); First->Close(); Last.Reset(); First.Reset(); });
  };
  Candidate = MoveTemp(Other);
 }
 Prepared = MoveTemp(State); Primitive = Intent.Requirement.PrimitiveId; Phase = Intent.Phase;
 OutBatch = MoveTemp(Candidate); BindCapture(OutBatch); return Result(true);
}

FAssetDocumentContractValidationResult FMontageNotifyRequestOwner::PrepareDeferredCreate(
 TConstArrayView<FAssetDocumentMutationIntent> Intents, FAssetDocumentPreparedMutationBatch& OutBatch)
{
 const FString ObjectId = TEXT("Object:") + TargetPath;
 const FString PackageId = TEXT("Package:") + FPackageName::ObjectPathToPackageName(TargetPath);
 TArray<FAssetDocumentMutationIntent> Sorted(Intents.GetData(), Intents.Num());
 Sorted.Sort([](const auto& A, const auto& B) { return A.StepIndex < B.StepIndex; });
 TSet<FString> Families;
 for (const auto& Intent : Sorted)
 {
  const FString& Id = Intent.Requirement.PrimitiveId;
  const bool bKnown = IsNotifyPrimitive(Id) || Id == AssetDocumentMontageSettings::Primitive
   || Id == TEXT("animation.curve.float.replace.v2") || Id == TEXT("animation.asset.preview_mesh.set.v1")
   || Id == TEXT("animation.asset.skeleton.set.v1") || Id == TEXT("animation.montage.slot_tracks.replace.v1")
   || Id == TEXT("animation.montage.sections.mutate.v1");
  if (!bKnown || Families.Contains(Id) || Intent.StepIndex < 0
   || Intent.Requirement.RequiredByRouteId != TEXT("animation_profile")
   || Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply)
   return Reject(TEXT("A created Montage admits each exact Animation family at most once in Apply."));
  Families.Add(Id);
  FAssetDocumentPreparedMutationContribution Contribution;
  Contribution.StepIndex = Intent.StepIndex;
  Contribution.RawFootprint = {{EAssetDocumentMutationFootprintKind::UObjectState, ObjectId, TEXT("rollback.uobject")},
   {EAssetDocumentMutationFootprintKind::Package, PackageId, TEXT("rollback.package")}};
  Contribution.OwnedSurfaceIds = {TargetPath + TEXT("::@created.") + Id};
  Contribution.bCoveredByPackageSnapshot = true;
  Contribution.ValidateInvocation = [Intent, Request = RequestId, Pinned = Revision](
   const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver&)
  {
   // The family's own prepared validator runs inside InvokeDeferred.
   return Result(Invocation.RequestId == Request && Invocation.RuntimeRevision == Pinned
    && Invocation.TransactionId.IsValid() && Invocation.PrimitiveId == Intent.Requirement.PrimitiveId
    && Invocation.Version == Intent.Requirement.Version && Invocation.Phase == Intent.Phase,
    TEXT("AnimationProvider.OperationRejected"));
  };
  OutBatch.Contributions.Add(MoveTemp(Contribution));
 }
 OutBatch.OpenMutationCapture = [](const FGuid& TransactionId, TArray<FAssetDocumentRuntimeDiagnostic>&)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  return TransactionId.IsValid() ? MakeUnique<FAssetDocumentCoreMutationCaptureLease>([]() {}) : nullptr;
 };
 DeferredIntents = MoveTemp(Sorted);
 bDeferredCreate = true;
 return Result(true);
}

FAssetDocumentPrimitiveOutcome FMontageNotifyRequestOwner::InvokeDeferred(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction)
{
 if (!IsStructuralContext() || !DeferredIntents.IsValidIndex(DeferredNext) || !Transaction
  || Transaction->GetState() != EAssetDocumentAtomicState::Mutating || !Invocation.TransactionId.IsValid()
  || Transaction->GetTransactionId() != Invocation.TransactionId)
  return Failed(TEXT("A created Montage step must run once, in order, inside the request transaction."));
 const FAssetDocumentMutationIntent Intent = DeferredIntents[DeferredNext++];
 if (Invocation.PrimitiveId != Intent.Requirement.PrimitiveId || Invocation.Version != Intent.Requirement.Version
  || Invocation.Phase != Intent.Phase)
  return Failed(TEXT("The invocation differs from the next frozen created-Montage step."));
 auto* Target = Cast<UAnimMontage>(FSoftObjectPath(TargetPath).ResolveObject());
 if (!IsValid(Target) || Target->GetClass() != UAnimMontage::StaticClass() || Target->GetPathName() != TargetPath)
  return Failed(TEXT("The created Montage must exist before its Body is applied."));
 // The same family preparation an existing target uses, now on the new asset.
 FAssetDocumentPreparedMutationBatch Batch;
 TSharedPtr<FPreparedNotifyStaging> Notify;
 const bool bNotify = IsNotifyPrimitive(Intent.Requirement.PrimitiveId);
 auto Valid = bNotify
  ? PrepareMontageNotifyReplacement(*Target, Intent.Intent, Intent.Requirement.PrimitiveId == StatePrimitive,
   ReadBinding->Request, Intent.StepIndex, Batch, Notify)
  : PrepareAssetDocumentAnimationMutationBatch(ReadBinding->Request, TConstArrayView<FAssetDocumentMutationIntent>(&Intent, 1), Batch);
 auto Fail = [](FAssetDocumentContractValidationResult R)
 {
  FAssetDocumentPrimitiveOutcome O; O.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
  O.Value.Kind = EAssetDocumentRuntimeValueKind::Object; O.Diagnostics = MoveTemp(R.Diagnostics);
  if (O.Diagnostics.IsEmpty()) O.Diagnostics = Reject(TEXT("Created-Montage family preparation failed.")).Diagnostics;
  return O;
 };
 if (!Valid.bValid) return Fail(MoveTemp(Valid));
 if (!Batch.OpenMutationCapture || Batch.Contributions.Num() != 1 || Batch.Contributions[0].StepIndex != Intent.StepIndex
  || !Batch.Contributions[0].ValidateInvocation || Batch.PackageFinalizer.IsValid() || (bNotify && !Notify.IsValid()))
  return Failed(TEXT("Created-Montage family preparation did not return one captured contribution."));
 TArray<FAssetDocumentRuntimeDiagnostic> CaptureDiagnostics;
 auto Capture = Batch.OpenMutationCapture(Invocation.TransactionId, CaptureDiagnostics);
 if (!Capture || !Capture->IsValid())
 {
  FAssetDocumentContractValidationResult R; R.Diagnostics = MoveTemp(CaptureDiagnostics);
  return Fail(MoveTemp(R));
 }
 // Its typed inverse is not armed: deleting the created asset is the rollback.
 FAssetDocumentPrimitiveOutcome Outcome;
 Valid = Batch.Contributions[0].ValidateInvocation(Invocation, Handles);
 if (!Valid.bValid) Outcome = Fail(MoveTemp(Valid));
 else if (bNotify) Outcome = InvokeMontageNotifyReplacement(Notify.ToSharedRef(), Invocation, Handles, Transaction);
 else if (Intent.Requirement.PrimitiveId == AssetDocumentMontageSettings::Primitive)
  Outcome = InvokeAnimationMontageSettings(Invocation, Handles, Transaction);
 else Outcome = Inner->Invoke(Invocation, Handles, Transaction);
 Capture->Close();
 return Outcome;
}

FAssetDocumentPrimitiveOutcome FMontageNotifyRequestOwner::Invoke(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction)
{
 if (ReadBinding->bClosed) return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("The request owner is closed."));
 if (Invocation.PrimitiveId == TEXT("animation.montage.profile.snapshot.v1"))
 {
  if (!IsStructuralContext() || !ReadBinding->bBound || Invocation.RequestId != RequestId
   || Invocation.RuntimeRevision != (ReadBinding->bStagedUsed ? ReadBinding->StagedRuntimeRevision : Revision) || Transaction
   || (ReadBinding->bStagedUsed && (!ReadBinding->StagedTarget.IsValid()
    || ReadBinding->bStagedConsumed || Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Verify
    || ReadBinding->StagedTarget->GetPathName() != ReadBinding->StagedObjectPath
    || ReadBinding->StagedTarget->GetUniqueID() != ReadBinding->StagedTypeRevision
    || !FPaths::IsSamePath(ReadBinding->StagedTarget->GetOutermost()->GetLoadedPath().GetLocalFullPath(), ReadBinding->StagedFilePath))))
   return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("Snapshot must match the live request or its one-shot staged Verify lease."));
  const FString ExpectedPath = ReadBinding->StagedTarget.IsValid() ? ReadBinding->StagedObjectPath : TargetPath;
  if (ReadBinding->bStagedUsed) ReadBinding->bStagedConsumed = true;
  return InvokeAnimationMontageProfileSnapshotBound(Invocation, Handles, Transaction, ExpectedPath, ReadBinding->StagedTarget.IsValid());
 }
 if (Invocation.PrimitiveId == StructureCheckPrimitive)
 {
  // Read-only: the bound live Montage of this request (Validate's or Apply's Preflight), never a staged copy.
  if (!IsStructuralContext() || !ReadBinding->bBound || ReadBinding->bStagedUsed || Invocation.RequestId != RequestId
   || Invocation.RuntimeRevision != Revision || Transaction
   || ReadBinding->Request.RequestedAssetClassPath != UAnimMontage::StaticClass()->GetPathName())
   return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("The Montage timeline check reads this request's live Montage."));
  return InvokeMontageStructureCheck(Invocation, Handles, Transaction, TargetPath);
 }
 if (AssetDocumentAnimationAuthoring::IsPrimitive(Invocation.PrimitiveId))
 {
  // Snapshot-covered sequence authoring: no preparation, bound to this
  // request's exact target (created or existing).
  if (!ReadBinding->bBound || ReadBinding->bStagedUsed || Invocation.RequestId != RequestId
   || !AssetDocumentAnimationAuthoring::IsAdmittedClassPath(ReadBinding->Request.RequestedAssetClassPath))
   return MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Animation authoring requires a bound AnimSequence request."));
  return InvokeAnimationAssetAuthoring(Invocation, Handles, Transaction, TargetPath);
 }
 if (ReadBinding->bBound && (Invocation.RequestId != RequestId || Invocation.RuntimeRevision != Revision
  || ReadBinding->bStagedUsed)) return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("Mutation remains bound to its original request and cannot use a staged read lease."));
 if (ReadBinding->bBound)
 {
  const auto* Descriptor = Descriptors.FindByPredicate([&](const auto& D)
   { return D.PrimitiveId == Invocation.PrimitiveId && D.Version == Invocation.Version; });
  if (Descriptor && Descriptor->Access == EAssetDocumentPrimitiveAccess::Mutating)
  {
   const bool bSequence = ReadBinding->Request.RequestedAssetClassPath == UAnimSequence::StaticClass()->GetPathName();
   const bool bSequenceCompression = bSequence && Invocation.PrimitiveId.Equals(TEXT("animation.sequence.compression.settings.set.v1"), ESearchCase::CaseSensitive);
   const bool bSequenceCompletion = bSequence && (
    Invocation.PrimitiveId.Equals(TEXT("animation.sequence.complete_edit.v2"), ESearchCase::CaseSensitive)
    || Invocation.PrimitiveId.Equals(TEXT("animation.sequence.retarget_source_asset.set.v2"), ESearchCase::CaseSensitive)
    || Invocation.PrimitiveId.Equals(TEXT("animation.sequence.retarget_source_name.set.v1"), ESearchCase::CaseSensitive));
   if (bSequence && Invocation.PrimitiveId != TEXT("animation.asset.preview_mesh.set.v1") && !bSequenceCompression && !bSequenceCompletion)
    return MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Sequence admits only its exact prepared native leaves."));
   FAssetDocumentVerifiedLeaseResult Acquired;
   if (bSequenceCompression || bSequenceCompletion)
   {
    const auto* Argument = Invocation.HandleArguments.FindByPredicate([](const auto& A)
     { return A.Name.Equals(TEXT("sequence"), ESearchCase::CaseSensitive); });
    if (Argument)
    {
     FAssetDocumentHandleValidationContext Context;
     Context.RequestId = Invocation.RequestId; Context.TransactionId = Invocation.TransactionId;
     Context.RuntimeRevision = Invocation.RuntimeRevision; Context.ProviderId = TEXT("editor");
     Context.Phase = Invocation.Phase; Context.RequiredKind = EAssetDocumentHandleKind::Asset;
     Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
     Acquired = Handles.AcquireVerifiedLease(Argument->Handle, Context);
    }
   }
   else Acquired = AcquireAnimationInputLease(Handles, Invocation, TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
   if (!Acquired.Lease) return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("Bound mutation requires its original target's mutable asset lease."));
   const auto Identity = WithAssetDocumentEditorAssetIdentity(*Acquired.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
   {
    auto& Target = View.GetObject();
    return Result(IsValid(&Target) && Target.GetClass() == (bSequence ? UAnimSequence::StaticClass() : UAnimMontage::StaticClass())
     && Target.GetPathName() == TargetPath && View.GetCanonicalObjectPath() == TargetPath
     && View.GetTypeRevision() == Target.GetUniqueID());
   });
   if (!Identity.bValid) return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("A staged or unrelated target cannot receive this request's mutation."));
  }
 }
 if (bDeferredCreate)
 {
  const auto* Descriptor = Descriptors.FindByPredicate([&](const auto& D)
   { return D.PrimitiveId == Invocation.PrimitiveId && D.Version == Invocation.Version; });
  if (Descriptor && Descriptor->Access == EAssetDocumentPrimitiveAccess::Mutating)
   return InvokeDeferred(Invocation, Handles, Transaction);
 }
 if (Invocation.PrimitiveId == AssetDocumentMontageSettings::Primitive)
  return ReadBinding->bBound && ReadBinding->Request.RequestedAssetClassPath == UAnimMontage::StaticClass()->GetPathName()
   ? InvokeAnimationMontageSettings(Invocation, Handles, Transaction)
   : MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Montage settings require a bound Montage request."));
 if (!IsNotifyPrimitive(Invocation.PrimitiveId)) return Inner->Invoke(Invocation, Handles, Transaction);
 if (!IsStructuralContext() || !bPreparationAttempted || !Prepared.IsValid() || bInvocationAttempted
  || Invocation.RequestId != RequestId || Invocation.RuntimeRevision != Revision
  || Invocation.PrimitiveId != Primitive || Invocation.Version != 1 || Invocation.Phase != Phase
  || !Transaction || Transaction->GetState() != EAssetDocumentAtomicState::Mutating
  || !Invocation.TransactionId.IsValid() || Transaction->GetTransactionId() != Invocation.TransactionId
  || Transaction->GetMutationToken().TransactionId != Invocation.TransactionId
  || Transaction->GetMutationToken().RequestId != RequestId || Transaction->GetMutationToken().RuntimeRevision != Revision)
  return Failed(TEXT("Notify invocation must match this owner's prepared request, transaction and unused family."));
 bInvocationAttempted = true;
 // The composite validates frozen arguments, verified target identity, capture
 // lifetime and exact prepared footprint. No global lookup or late action.
 return InvokeMontageNotifyReplacement(Prepared.ToSharedRef(), Invocation, Handles, Transaction);
}
TSharedRef<FMontageNotifyRequestOwner> CreateMontageNotifyRequestOwner()
{
 return MakeShared<FMontageNotifyRequestOwner>();
}
}

TSharedRef<IAssetDocumentAnimationRequestProvider> CreateAssetDocumentAnimationRequestProvider()
{
 return AssetDocumentMontage::CreateMontageNotifyRequestOwner();
}
