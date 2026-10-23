// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Asset/AssetDocumentAnimationSkeletonOperations.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonState.h"

#if WITH_DEV_AUTOMATION_TESTS
AssetDocumentAnimationSkeletonTesting::FHooks* AssetDocumentAnimationSkeleton::TestHooks = nullptr;
void AssetDocumentAnimationSkeletonTesting::SetHooks(FHooks* Hooks)
{
 AssetDocumentAnimationSkeleton::TestHooks = Hooks;
}
#endif

FAssetDocumentPrimitiveDescriptor MakeAnimationSkeletonSetDescriptor()
{
 using namespace AssetDocumentAnimationSkeleton;
 FAssetDocumentPrimitiveDescriptor D;
 D.PrimitiveId = Primitive; D.Version = 1; D.ProviderId = TEXT("animation");
 D.Owner = EAssetDocumentPrimitiveOwner::Animation;
 D.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
 D.Access = EAssetDocumentPrimitiveAccess::Mutating;
 D.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {
  {TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, true},
  {TEXT("skeleton"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, false}};
 D.bRequiresActiveTransaction = true;
 D.InputSchemaId = Schema; D.OutputSchemaId = D.PrimitiveId + TEXT(".output.v1");
 D.MutationFootprintSchemaId = D.PrimitiveId + TEXT(".footprint.v1");
 D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.StableDiagnosticCodes = {TEXT("AnimationProvider.InvalidValueShape"), TEXT("AnimationProvider.InvalidHandlePayload"),
  TEXT("AnimationProvider.TargetClassMismatch"), TEXT("AnimationProvider.AuthorityUnavailable"),
  TEXT("AnimationProvider.OperationRejected"), TEXT("AnimationProvider.TransactionUnavailable"),
  TEXT("AnimationProvider.CompletionFailed"), TEXT("AnimationProvider.ReadbackMismatch"),
  TEXT("AnimationProvider.Skeleton.InvalidTarget"), TEXT("AnimationProvider.Skeleton.IncompatibleReference"),
  TEXT("AnimationProvider.Skeleton.RollbackFailed")};
 auto& A = D.Admission;
 A.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
 A.ExactEngineVersion = TEXT("5.7.4"); A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 A.PublicModule = TEXT("Engine"); A.PublicHeader = TEXT("Animation/AnimationAsset.h");
 A.ExportedAuthoritativeApi = TEXT("UAnimationAsset::SetSkeleton;GetSkeleton;GetSkeletonGuid");
 A.AuthorityContextId = TEXT("animation.editor-game-thread.active-transaction.v1");
 A.PrimaryOperationId = Primitive;
 A.IdentityLifetimeContractId = TEXT("animation.provider-lease.request-transaction-revision.v1");
 A.TransactionContractId = TEXT("animation.core-active-transaction.v1");
 A.RollbackContractId = RollbackId;
 A.CompletionContractId = TEXT("animation.asset.skeleton.set.v1.authoritative-readback.v1");
 A.SaveReloadEvidenceId = TEXT("animation.asset.skeleton.set.v1.launcher-5.7.4-save-reload.v1");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.SkeletonS1.ApplyFileRollback"),
  TEXT("UEAssetDoc.SkeletonS1.CreateApplyFile")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimationProvider.Authority"),
  TEXT("UEAssetDoc.AnimationProvider.Descriptors"),
  TEXT("UEAssetDoc.AnimationProvider.Skeleton.Descriptor"),
  TEXT("UEAssetDoc.AnimationProvider.Skeleton.Preparation"),
  TEXT("UEAssetDoc.AnimationProvider.Skeleton.RetainedCompatibilityAndDrift"),
  TEXT("UEAssetDoc.AnimationProvider.Skeleton.NoncanonicalBaseline"),
  TEXT("UEAssetDoc.SkeletonS1.Composition"), TEXT("UEAssetDoc.SkeletonS1.ApplyFile")};
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|Engine|Animation/AnimationAsset.h|UAnimationAsset::SetSkeleton;GetSkeleton;GetSkeletonGuid|animation.asset.skeleton.set.v1");
 return D;
}

bool IsSkeletonSetAdmissionShape(const FAssetDocumentPrimitiveDescriptor& D)
{
 const auto E = MakeAnimationSkeletonSetDescriptor();
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

FAssetDocumentPrimitiveOutcome InvokeAnimationSkeletonSet(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction)
{
 using namespace AssetDocumentAnimationSkeleton;
 static bool bInvoking = false;
 if (!ContextValid() || bInvoking)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Skeleton mutation requires the non-reentrant Editor game thread."));
 TGuardValue<bool> InvokingGuard(bInvoking, true);
 if (!Transaction || Transaction->GetState() != EAssetDocumentAtomicState::Mutating
  || Transaction->GetTransactionId() != Invocation.TransactionId
  || Transaction->GetMutationToken().RequestId != Invocation.RequestId
  || Transaction->GetMutationToken().RuntimeRevision != Invocation.RuntimeRevision
  || Transaction->GetMutationToken().TransactionId != Invocation.TransactionId)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.TransactionUnavailable"), TEXT("Skeleton mutation requires the matching active transaction."));
 FString Operation, Unused;
 if (!ValidateAssetDocumentPrimitiveInvocation(Invocation, MakeAnimationSkeletonSetDescriptor()).bValid
  || !ParseOperation(Invocation.ValueArguments, false, Operation, Unused) || !ExactHandles(Invocation, Operation == TEXT("set")))
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidValueShape"), TEXT("Skeleton mutation requires exact operation and handles."));
 TUniquePtr<IAssetDocumentVerifiedHandleLease> TargetLease, ReferenceLease;
 UObject* NativeTarget = nullptr;
 auto Valid = Acquire(Invocation, Handles, TEXT("asset"), UAnimMontage::StaticClass(), EAssetDocumentHandleAccess::Mutable, TargetLease, NativeTarget);
 if (!Valid.bValid)
 {
  auto Failure = MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("Invalid montage identity."));
  Failure.Diagnostics = MoveTemp(Valid.Diagnostics); return Failure;
 }
 auto& Target = *static_cast<UAnimMontage*>(NativeTarget);
 UObject* NativeReference = nullptr;
 if (Operation == TEXT("set"))
 {
  Valid = Acquire(Invocation, Handles, TEXT("skeleton"), USkeleton::StaticClass(), EAssetDocumentHandleAccess::ReadOnly, ReferenceLease, NativeReference);
  if (!Valid.bValid)
  {
   auto Failure = MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("Invalid Skeleton identity."));
   Failure.Diagnostics = MoveTemp(Valid.Diagnostics); return Failure;
  }
 }
 auto* Desired = static_cast<USkeleton*>(NativeReference);
 if (!CanonicalSkeleton(Target)) return MakeAnimationProviderFailure(TEXT("AnimationProvider.Skeleton.InvalidTarget"), TEXT("The target Skeleton GUID baseline is not canonical."));
 if (!Compatible(Target, Desired)) return MakeAnimationProviderFailure(TEXT("AnimationProvider.Skeleton.IncompatibleReference"), IncompatibleReason(Target.SlotAnimTracks, Desired));
 const FGuid DesiredGuid = Desired ? Desired->GetGuid() : FGuid();
 const FString DesiredPath = Desired ? Desired->GetPathName() : FString();
 const FString TargetPath = Target.GetPathName();
 TStrongObjectPtr<UAnimMontage> TargetAnchor(&Target);
 TStrongObjectPtr<USkeleton> DesiredAnchor(Desired);
 TStrongObjectPtr<UPackage> Package(Target.GetOutermost());
 const FString PackagePath = Package->GetName();
 const FRetainedState Retained(Target);
 const bool bChanged = Target.GetSkeleton() != Desired || Target.GetSkeletonGuid() != DesiredGuid;
 if (bChanged)
 {
  Valid = Transaction->EnlistFootprint(Footprint(TargetPath, PackagePath));
  if (!Valid.bValid)
  {
   auto Failure = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Skeleton rollback coverage could not be enlisted."));
   Failure.Diagnostics = MoveTemp(Valid.Diagnostics); return Failure;
  }
#if WITH_DEV_AUTOMATION_TESTS
  if (TestHooks) ++TestHooks->ForwardSetterCalls;
#endif
  Target.SetSkeleton(Desired);
#if WITH_DEV_AUTOMATION_TESTS
  if (TestHooks && TestHooks->AfterSetter) TestHooks->AfterSetter();
#endif
 }
 bool bForcedReadbackFailure = false;
#if WITH_DEV_AUTOMATION_TESTS
 bForcedReadbackFailure = bChanged && TestHooks && TestHooks->bFailForwardReadback;
#endif
 if (bForcedReadbackFailure || !ExactObject(&Target, UAnimMontage::StaticClass()) || Target.GetPathName() != TargetPath
  || Target.GetOutermost() != Package.Get() || Package->GetName() != PackagePath
  || FSoftObjectPath(TargetPath).ResolveObject() != &Target || Target.GetSkeleton() != Desired
  || Target.GetSkeletonGuid() != DesiredGuid || !Retained.Matches(Target)
  || (Desired && (!ExactObject(Desired, USkeleton::StaticClass()) || Desired->GetPathName() != DesiredPath || Desired->GetGuid() != DesiredGuid)))
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.ReadbackMismatch"), TEXT("Skeleton setter did not preserve the exact identity, GUID or retained tracks."));
 FAssetDocumentPrimitiveOutcome Outcome;
 Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 Outcome.Value.ObjectKeys = {TEXT("skeleton_path"), TEXT("skeleton_guid"), TEXT("changed")};
 FAssetDocumentRuntimeValueDto PathValue, GuidValue, Changed;
 if (Desired)
 {
  PathValue.Kind = GuidValue.Kind = EAssetDocumentRuntimeValueKind::String;
  PathValue.StringValue = DesiredPath; GuidValue.StringValue = DesiredGuid.ToString(EGuidFormats::DigitsWithHyphens);
 }
 Changed.Kind = EAssetDocumentRuntimeValueKind::Boolean; Changed.BooleanValue = bChanged;
 Outcome.Value.Elements = {MoveTemp(PathValue), MoveTemp(GuidValue), MoveTemp(Changed)};
 if (bChanged)
 {
  auto& Change = Outcome.ChangedSet.AddDefaulted_GetRef();
  Change.AuthorityId = Primitive; Change.StableObjectId = TargetPath;
  Change.Path = TEXT("/Skeleton"); Change.ChangeKind = Desired ? TEXT("Set") : TEXT("Clear");
 }
 return Outcome;
}
