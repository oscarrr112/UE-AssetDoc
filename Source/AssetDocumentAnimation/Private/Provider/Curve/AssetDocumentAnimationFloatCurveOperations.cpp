// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Curve/AssetDocumentAnimationFloatCurveOperations.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveMutationState.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveSnapshot.h"

FAssetDocumentPrimitiveDescriptor MakeAnimationFloatCurveReplaceDescriptor()
{
 using namespace AssetDocumentAnimationFloatCurveMutation;
 FAssetDocumentPrimitiveDescriptor D;
 D.PrimitiveId = Primitive; D.Version = 2; D.ProviderId = TEXT("animation");
 D.Owner = EAssetDocumentPrimitiveOwner::Animation;
 D.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
 D.Access = EAssetDocumentPrimitiveAccess::Mutating;
 D.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {
  {TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, true}};
 D.bRequiresActiveTransaction = true;
 D.InputSchemaId = Schema; D.OutputSchemaId = D.PrimitiveId + TEXT(".output.v2");
 D.MutationFootprintSchemaId = D.PrimitiveId + TEXT(".footprint.v2");
 D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.StableDiagnosticCodes = {TEXT("AnimationProvider.InvalidValueShape"), TEXT("AnimationProvider.InvalidHandlePayload"),
  TEXT("AnimationProvider.TargetClassMismatch"), TEXT("AnimationProvider.AuthorityUnavailable"),
  TEXT("AnimationProvider.OperationRejected"), TEXT("AnimationProvider.TransactionUnavailable"),
  TEXT("AnimationProvider.CompletionFailed"), TEXT("AnimationProvider.ReadbackMismatch"),
  TEXT("AnimationProvider.Curve.InvalidTarget"), TEXT("AnimationProvider.Curve.UnsupportedPreimage"), TEXT("AnimationProvider.Curve.DataModelUnavailable"),
  TEXT("AnimationProvider.Curve.RollbackFailed")};
 auto& A = D.Admission;
 A.Disposition = EAssetDocumentPrimitiveDisposition::AdmittedBridge;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted;
 A.ExactEngineVersion = TEXT("5.7.4"); A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 A.PublicModule = TEXT("AssetDocumentAnimation+Engine");
 A.PublicHeader = TEXT("Animation/AnimData/IAnimationDataController.h;Animation/AnimData/AnimDataModel.h;Curves/RichCurve.h;Curves/RealCurve.h");
 A.ExportedAuthoritativeApi = TEXT("PublishExactFloatCurveState (approved plugin bridge; public Controller + exact member)");
 A.AuthorityContextId = TEXT("animation.editor-game-thread.active-transaction.v2");
 A.PrimaryOperationId = Primitive;
 A.IdentityLifetimeContractId = TEXT("animation.provider-lease.request-transaction-revision.v2");
 A.TransactionContractId = TEXT("animation.core-active-transaction.v2"); A.RollbackContractId = RollbackId;
 A.CompletionContractId = TEXT("animation.curve.float.replace.v2.complete-readback.v2");
 A.SaveReloadEvidenceId = TEXT("animation.curve.float.replace.v2.launcher-5.7.4-save-reload.v2");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimationProvider.FloatCurveMutation.FailureInverse")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimationProvider.FloatCurveMutation.Descriptor"),
  TEXT("UEAssetDoc.AnimationProvider.FloatCurveMutation.FullReplacement"),
  TEXT("UEAssetDoc.AnimationProvider.FloatCurveMutation.Guards")};
 A.GovernanceId = TEXT("ANIMATION-FLOAT-CURVE-EXACT-STATE-v1");
 A.SourceMarker = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(ANIMATION-FLOAT-CURVE-EXACT-STATE-v1)");
 A.RawLifecycleAllowlistId = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(ANIMATION-FLOAT-CURVE-EXACT-STATE-v1|animation.curve.float.replace.v2)");
 // Exact formal registry references are bound when the bridge record is admitted.
 A.IndependentCandidateReviewId = TEXT("candidate-review.initial/ANIMATION-FLOAT-CURVE-EXACT-STATE-v1");
 A.IndependentBridgeReviewId = TEXT("bridge-review.initial/ANIMATION-FLOAT-CURVE-EXACT-STATE-v1");
 A.HelperReplacementCondition = TEXT("Replace with a stock public controller preserving default and every key field without recomputation.");
 A.RemovalCondition = TEXT("Remove exact-member exception after equivalent forward/inverse and save/reload evidence passes.");
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|exact-stock-AnimDataModel|PublishExactFloatCurveState|animation.curve.float.replace.v2");
 return D;
}

bool IsFloatCurveReplaceAdmissionShape(const FAssetDocumentPrimitiveDescriptor& D)
{
 const auto E = MakeAnimationFloatCurveReplaceDescriptor();
 if (D.PrimitiveId != E.PrimitiveId || D.Version != E.Version || D.ProviderId != E.ProviderId
  || D.Owner != E.Owner || D.ExecutionThread != E.ExecutionThread || D.Access != E.Access
  || D.AllowedPhases != E.AllowedPhases || D.AllowedHandleKinds != E.AllowedHandleKinds
  || D.HandleArgumentSchemas.Num() != 1 || !D.HandleResultSchemas.IsEmpty()
  || D.bRequiresActiveTransaction != E.bRequiresActiveTransaction || D.InputSchemaId != E.InputSchemaId
  || D.OutputSchemaId != E.OutputSchemaId || D.MutationFootprintSchemaId != E.MutationFootprintSchemaId
  || D.StableDiagnosticCodes != E.StableDiagnosticCodes || D.RequiredConsumerIds != E.RequiredConsumerIds) return false;
 for (int32 I = 0; I < 1; ++I)
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
  && A.SourceMarker == B.SourceMarker && A.RawLifecycleAllowlistId == B.RawLifecycleAllowlistId && A.GovernanceId == B.GovernanceId
  && A.IndependentCandidateReviewId == B.IndependentCandidateReviewId && A.IndependentBridgeReviewId == B.IndependentBridgeReviewId
  && A.HelperReplacementCondition == B.HelperReplacementCondition && A.RemovalCondition == B.RemovalCondition;
}

FAssetDocumentPrimitiveOutcome InvokeAnimationFloatCurveReplace(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction)
{
 using namespace AssetDocumentAnimationFloatCurveMutation;
 static bool bInvoking = false;
 if (!ContextValid() || bInvoking)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Float curves require the non-reentrant Editor game thread."));
 TGuardValue<bool> Guard(bInvoking, true);
 if (!Transaction || Transaction->GetState() != EAssetDocumentAtomicState::Mutating
  || Transaction->GetTransactionId() != Invocation.TransactionId
  || Transaction->GetMutationToken().RequestId != Invocation.RequestId
  || Transaction->GetMutationToken().RuntimeRevision != Invocation.RuntimeRevision
  || Transaction->GetMutationToken().TransactionId != Invocation.TransactionId)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.TransactionUnavailable"), TEXT("Matching active transaction required."));
 TSharedPtr<FPrepared> State;
 auto Valid = AcquirePrepared(Invocation, Handles, State);
 if (!Valid.bValid)
 {
  auto Failure = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Valid prepared float-curve capture required."));
  Failure.Diagnostics = MoveTemp(Valid.Diagnostics); return Failure;
 }
 const bool bChanged = !EqualAnimationFloatCurveStateV2(State->Old, State->Desired);
 if (bChanged)
 {
  Valid = Transaction->EnlistFootprint(Footprint(State->TargetPath, State->PackagePath));
  if (Valid.bValid) Valid = MarkWrite(Invocation, Handles, State);
  if (!Valid.bValid)
  {
   auto Failure = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Float-curve footprint or capture changed before publication."));
   Failure.Diagnostics = MoveTemp(Valid.Diagnostics); return Failure;
  }
  if (!PublishExactFloatCurveState(*State->Target, State->Desired))
   return MakeAnimationProviderFailure(TEXT("AnimationProvider.CompletionFailed"), TEXT("Complete float-curve publication failed; Core must run the typed inverse."));
 }
 FAssetDocumentRuntimeValueDto Curves;
 if (!State->RetainedCurrent() || !MatchesAnimationFloatCurveState(*State->Target, State->Desired)
  || !CaptureAnimationFloatCurveSnapshotV2(State->Target->GetDataModel()->GetFloatCurves(), Curves))
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.ReadbackMismatch"), TEXT("Full float-curve or retained authored state differs after publication."));
 FAssetDocumentPrimitiveOutcome Outcome; Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 Outcome.Value.ObjectKeys = {TEXT("changed"), TEXT("curves")};
 FAssetDocumentRuntimeValueDto Changed; Changed.Kind = EAssetDocumentRuntimeValueKind::Boolean; Changed.BooleanValue = bChanged;
 Outcome.Value.Elements = {MoveTemp(Changed), MoveTemp(Curves)};
 if (bChanged)
 {
  auto& Change = Outcome.ChangedSet.AddDefaulted_GetRef(); Change.AuthorityId = Primitive;
  Change.StableObjectId = State->TargetPath; Change.Path = TEXT("/Curves"); Change.ChangeKind = TEXT("Replace");
 }
 return Outcome;
}
