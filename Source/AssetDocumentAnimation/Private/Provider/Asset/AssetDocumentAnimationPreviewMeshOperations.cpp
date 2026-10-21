// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Asset/AssetDocumentAnimationPreviewMeshOperations.h"
#include "Provider/Asset/AssetDocumentAnimationPreviewMeshState.h"

#if WITH_DEV_AUTOMATION_TESTS
AssetDocumentAnimationPreviewMeshTesting::FHooks* AssetDocumentAnimationPreviewMesh::TestHooks = nullptr;
void AssetDocumentAnimationPreviewMeshTesting::SetHooks(FHooks* Hooks)
{
 AssetDocumentAnimationPreviewMesh::TestHooks = Hooks;
}
#endif

FAssetDocumentPrimitiveDescriptor MakeAnimationPreviewMeshSetDescriptor()
{
 using namespace AssetDocumentAnimationPreviewMesh;
 FAssetDocumentPrimitiveDescriptor D;
 D.PrimitiveId = Primitive; D.Version = 1; D.ProviderId = TEXT("animation");
 D.Owner = EAssetDocumentPrimitiveOwner::Animation;
 D.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
 D.Access = EAssetDocumentPrimitiveAccess::Mutating;
 D.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {
  {TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, true},
  {TEXT("preview_mesh"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, false}};
 D.bRequiresActiveTransaction = true;
 D.InputSchemaId = Schema; D.OutputSchemaId = D.PrimitiveId + TEXT(".output.v1");
 D.MutationFootprintSchemaId = D.PrimitiveId + TEXT(".footprint.v1");
 D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.StableDiagnosticCodes = {TEXT("AnimationProvider.InvalidValueShape"), TEXT("AnimationProvider.InvalidHandlePayload"),
  TEXT("AnimationProvider.TargetClassMismatch"), TEXT("AnimationProvider.AuthorityUnavailable"),
  TEXT("AnimationProvider.OperationRejected"), TEXT("AnimationProvider.TransactionUnavailable"),
  TEXT("AnimationProvider.CompletionFailed"), TEXT("AnimationProvider.ReadbackMismatch"),
  TEXT("AnimationProvider.PreviewMesh.InvalidTarget"), TEXT("AnimationProvider.PreviewMesh.UnsupportedPreimage"), TEXT("AnimationProvider.PreviewMesh.IncompatibleReference"),
  TEXT("AnimationProvider.PreviewMesh.RollbackFailed")};
 auto& A = D.Admission;
 A.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
 A.ExactEngineVersion = TEXT("5.7.4"); A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 A.PublicModule = TEXT("Engine"); A.PublicHeader = TEXT("Animation/AnimationAsset.h");
 A.ExportedAuthoritativeApi = TEXT("UAnimationAsset::SetPreviewMesh;GetPreviewMesh const");
 A.AuthorityContextId = TEXT("animation.editor-game-thread.active-transaction.v1");
 A.PrimaryOperationId = Primitive;
 A.IdentityLifetimeContractId = TEXT("animation.provider-lease.request-transaction-revision.v1");
 A.TransactionContractId = TEXT("animation.core-active-transaction.v1");
 A.RollbackContractId = RollbackId;
 A.CompletionContractId = TEXT("animation.asset.preview_mesh.set.v1.authoritative-readback.v1");
 A.SaveReloadEvidenceId = TEXT("animation.asset.preview_mesh.set.v1.launcher-5.7.4-save-reload.v1");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimationProvider.PreviewMesh.FailureInverse")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimationProvider.PreviewMesh.Descriptor"),
  TEXT("UEAssetDoc.AnimationProvider.PreviewMesh.Preparation"),
  TEXT("UEAssetDoc.AnimationProvider.PreviewMesh.ForwardInverse")};
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|Engine|Animation/AnimationAsset.h|UAnimationAsset::SetPreviewMesh;GetPreviewMesh const|animation.asset.preview_mesh.set.v1");
 return D;
}

bool IsPreviewMeshSetAdmissionShape(const FAssetDocumentPrimitiveDescriptor& D)
{
 const auto E = MakeAnimationPreviewMeshSetDescriptor();
 if (D.PrimitiveId != E.PrimitiveId || D.Version != E.Version || D.ProviderId != E.ProviderId
  || D.Owner != E.Owner || D.ExecutionThread != E.ExecutionThread || D.Access != E.Access
  || D.AllowedPhases != E.AllowedPhases || D.AllowedHandleKinds != E.AllowedHandleKinds
  || D.HandleArgumentSchemas.Num() != 2 || !D.HandleResultSchemas.IsEmpty()
  || D.bRequiresActiveTransaction != E.bRequiresActiveTransaction || D.InputSchemaId != E.InputSchemaId
  || D.OutputSchemaId != E.OutputSchemaId || D.MutationFootprintSchemaId != E.MutationFootprintSchemaId
  || D.StableDiagnosticCodes != E.StableDiagnosticCodes || D.RequiredConsumerIds != E.RequiredConsumerIds) return false;
 for (int32 I = 0; I < 2; ++I)
 {
  const auto& A = D.HandleArgumentSchemas[I]; const auto& B = E.HandleArgumentSchemas[I];
  if (A.Name != B.Name || A.Kind != B.Kind || A.RequiredAccess != B.RequiredAccess || A.bRequired != B.bRequired) return false;
 }
 const auto& A = D.Admission; const auto& B = E.Admission;
 return A.ContractVersion == B.ContractVersion && A.Disposition == B.Disposition && A.AuthorityPath == B.AuthorityPath
  && A.ExactEngineVersion == B.ExactEngineVersion && A.PhysicalOwner == B.PhysicalOwner
  && A.PublicModule == B.PublicModule && A.PublicHeader == B.PublicHeader && A.ExportedAuthoritativeApi == B.ExportedAuthoritativeApi
  && A.AuthorityContextId == B.AuthorityContextId && A.PrimaryOperationId == B.PrimaryOperationId
  && A.IdentityLifetimeContractId == B.IdentityLifetimeContractId && A.TransactionContractId == B.TransactionContractId
  && A.RollbackContractId == B.RollbackContractId && A.CompletionContractId == B.CompletionContractId
  && A.SaveReloadEvidenceId == B.SaveReloadEvidenceId && A.FailureInjectionTestIds == B.FailureInjectionTestIds
  && A.VerificationTestIds == B.VerificationTestIds && A.AuthorityFingerprint == B.AuthorityFingerprint
  && A.SourceMarker.IsEmpty() && A.RawLifecycleAllowlistId.IsEmpty() && A.GovernanceId.IsEmpty()
  && A.IndependentCandidateReviewId.IsEmpty() && A.IndependentBridgeReviewId.IsEmpty()
  && A.HelperReplacementCondition.IsEmpty() && A.RemovalCondition.IsEmpty();
}

FAssetDocumentPrimitiveOutcome InvokeAnimationPreviewMeshSet(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction)
{
 using namespace AssetDocumentAnimationPreviewMesh;
 static bool bInvoking = false;
 if (!ContextValid() || bInvoking)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("PreviewMesh requires the non-reentrant Editor game thread."));
 TGuardValue<bool> Guard(bInvoking, true);
 if (!Transaction || Transaction->GetState() != EAssetDocumentAtomicState::Mutating
  || Transaction->GetTransactionId() != Invocation.TransactionId
  || Transaction->GetMutationToken().RequestId != Invocation.RequestId
  || Transaction->GetMutationToken().RuntimeRevision != Invocation.RuntimeRevision
  || Transaction->GetMutationToken().TransactionId != Invocation.TransactionId)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.TransactionUnavailable"), TEXT("Matching active transaction required."));
 FString Operation, Unused;
 if (!ValidateAssetDocumentPrimitiveInvocation(Invocation, MakeAnimationPreviewMeshSetDescriptor()).bValid
  || !ParseOperation(Invocation.ValueArguments, false, Operation, Unused) || !ExactHandles(Invocation, Operation == TEXT("set")))
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidValueShape"), TEXT("Exact PreviewMesh operation and handles required."));
 const auto Prepared = ValidatePreparedAnimationPreviewMeshInvocation(Invocation, Handles);
 if (!Prepared.bValid)
 {
  auto Failure = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Valid prepared PreviewMesh capture required."));
  Failure.Diagnostics = Prepared.Diagnostics; return Failure;
 }
 TUniquePtr<IAssetDocumentVerifiedHandleLease> TargetLease, ReferenceLease;
 UObject* Native = nullptr;
 auto Valid = AcquireTarget(Invocation, Handles, TargetLease, Native);
 if (!Valid.bValid)
 {
  auto Failure = MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("Invalid target."));
  Failure.Diagnostics = MoveTemp(Valid.Diagnostics); return Failure;
 }
 auto& Target = *static_cast<UAnimationAsset*>(Native);
 Native = nullptr;
 if (Operation == TEXT("set"))
 {
  Valid = Acquire(Invocation, Handles, TEXT("preview_mesh"), USkeletalMesh::StaticClass(), EAssetDocumentHandleAccess::ReadOnly, ReferenceLease, Native);
  if (!Valid.bValid)
  {
   auto Failure = MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("Invalid preview mesh."));
   Failure.Diagnostics = MoveTemp(Valid.Diagnostics); return Failure;
  }
 }
 auto* Desired = static_cast<USkeletalMesh*>(Native);
 if (!CanonicalSkeleton(Target) || !Compatible(Target, Desired))
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.PreviewMesh.IncompatibleReference"), TEXT("Preview mesh Skeleton is incompatible."));
 const FString TargetPath = Target.GetPathName(), DesiredPath = Desired ? Desired->GetPathName() : FString();
 UClass* TargetClass = Target.GetClass();
 TStrongObjectPtr<UAnimationAsset> TargetAnchor(&Target);
 TStrongObjectPtr<USkeletalMesh> DesiredAnchor(Desired);
 TStrongObjectPtr<UPackage> Package(Target.GetOutermost());
 const FString PackagePath = Package->GetName();
 const FSkeletonIdentity TargetSkeleton(Target.GetSkeleton()), MeshSkeleton(Desired ? Desired->GetSkeleton() : nullptr);
 const FRetainedState Retained(Target);
 // P0 baseline classification is owned exclusively by the prepared preflight.
 const bool bChanged = ReadPreviewMesh(Target) != Desired;
 if (bChanged)
 {
  Valid = Transaction->EnlistFootprint(Footprint(TargetPath, PackagePath));
  if (!Valid.bValid)
  {
   auto Failure = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Rollback footprint enlistment failed."));
   Failure.Diagnostics = MoveTemp(Valid.Diagnostics); return Failure;
  }
  Valid = MarkPreparedAnimationPreviewMeshWrite(Invocation, Handles);
  if (!Valid.bValid)
  {
   auto Failure = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("PreviewMesh capture changed before setter."));
   Failure.Diagnostics = MoveTemp(Valid.Diagnostics); return Failure;
  }
#if WITH_DEV_AUTOMATION_TESTS
  if (TestHooks) ++TestHooks->ForwardSetterCalls;
#endif
  Target.SetPreviewMesh(Desired, false);
#if WITH_DEV_AUTOMATION_TESTS
  if (TestHooks && TestHooks->AfterSetter) TestHooks->AfterSetter();
#endif
 }
 bool bForcedFailure = false;
#if WITH_DEV_AUTOMATION_TESTS
 bForcedFailure = bChanged && TestHooks && TestHooks->bFailForwardReadback;
#endif
 if (bForcedFailure || !ExactTarget(&Target) || Target.GetClass() != TargetClass || Target.GetPathName() != TargetPath
  || Target.GetOutermost() != Package.Get() || Package->GetName() != PackagePath
  || FSoftObjectPath(TargetPath).ResolveObject() != &Target || ReadPreviewMesh(Target) != Desired
  || !TargetSkeleton.Matches(Target.GetSkeleton()) || !CanonicalSkeleton(Target) || !Retained.Matches(Target)
  || (Desired && (!ExactObject(Desired, USkeletalMesh::StaticClass()) || Desired->GetPathName() != DesiredPath
   || FSoftObjectPath(DesiredPath).ResolveObject() != Desired || !MeshSkeleton.Matches(Desired->GetSkeleton()) || !Compatible(Target, Desired))))
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.ReadbackMismatch"), TEXT("PreviewMesh authoritative readback or retained identity changed."));
 FAssetDocumentPrimitiveOutcome Outcome; Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 Outcome.Value.ObjectKeys = {TEXT("changed"), TEXT("preview_mesh")};
 FAssetDocumentRuntimeValueDto Changed, Path;
 Changed.Kind = EAssetDocumentRuntimeValueKind::Boolean; Changed.BooleanValue = bChanged;
 if (Desired) { Path.Kind = EAssetDocumentRuntimeValueKind::String; Path.StringValue = DesiredPath; }
 Outcome.Value.Elements = {MoveTemp(Changed), MoveTemp(Path)};
 if (bChanged)
 {
  auto& Change = Outcome.ChangedSet.AddDefaulted_GetRef(); Change.AuthorityId = Primitive;
  Change.StableObjectId = TargetPath; Change.Path = TEXT("/PreviewMesh"); Change.ChangeKind = Desired ? TEXT("Set") : TEXT("Clear");
 }
 return Outcome;
}
