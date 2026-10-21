// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceName.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceNameState.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetReferencesComposition.h"

namespace AssetDocumentSequenceRetargetSourceName
{
namespace
{
struct FCapture { FGuid Transaction; bool bOpened = false, bClosed = false, bTouched = false, bConsumed = false; };
class FInverse final : public IAssetDocumentRollbackAction
{
 TSharedRef<FPreparedState> State;
 TSharedRef<FCapture> Capture;
 bool bConsumed = false;
public:
 FInverse(TSharedRef<FPreparedState> S, TSharedRef<FCapture> C) : State(MoveTemp(S)), Capture(MoveTemp(C)) {}
 FStringView GetRollbackComponentId() const override { return RollbackId; }
 FAssetDocumentContractValidationResult Rollback(FStringView) override
 {
  if (bConsumed || !Retained::ContextValid() || !Capture->bOpened || !Capture->bClosed)
   return Result(false, TEXT("AnimationProvider.RetargetSourceName.RollbackFailed"));
  bConsumed = true;
  if (!Capture->bTouched) return State->Editor->Restore();
  if (!State->UnchangedInputs()) return Result(false, TEXT("AnimationProvider.RetargetSourceName.RollbackFailed"));
  State->Sequence()->WaitOnExistingCompression();
  if (!State->UnchangedInputs()) return Result(false, TEXT("AnimationProvider.RetargetSourceName.RollbackFailed"));
  const auto Restored = State->Editor->Restore();
  return Restored.bValid ? Result(Complete(*State, true), TEXT("AnimationProvider.RetargetSourceName.RollbackFailed")) : Restored;
 }
};
using FValidator = TFunction<FAssetDocumentContractValidationResult(const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&)>;
struct FActiveCapture { FGuid Transaction; FValidator Validate; TSharedPtr<FPreparedState> State; TSharedPtr<FCapture> Capture; };
// One operation-local, non-reentrant game-thread binding, not a handle registry.
const FActiveCapture* ActiveCapture = nullptr;
template <typename Consumer>
FAssetDocumentContractValidationResult WithTargetIdentity(const FPreparedState& S, const FAssetDocumentPrimitiveInvocation& I,
 IAssetDocumentHandleResolver& Handles, Consumer&& Body)
{
 if (I.HandleArguments.Num() != 1 || I.HandleArguments[0].Name != TEXT("sequence")) return Result(false);
 FAssetDocumentHandleValidationContext C; C.RequestId = I.RequestId; C.TransactionId = I.TransactionId;
 C.RuntimeRevision = I.RuntimeRevision; C.ProviderId = TEXT("editor"); C.Phase = I.Phase;
 C.RequiredKind = EAssetDocumentHandleKind::Asset; C.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
 auto L = Handles.AcquireVerifiedLease(I.HandleArguments[0].Handle, C);
 if (!L.Lease) return Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
 return WithAssetDocumentEditorAssetIdentity(*L.Lease, [&](const auto& V)
 {
  return &V.GetObject() == S.Sequence() && V.GetTypeRevision() == S.Target.Revision && V.GetCanonicalObjectPath() == S.Target.Path
   ? Body() : Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
 });
}
}
}

FAssetDocumentContractValidationResult PrepareAnimationSequenceRetargetSourceNameBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 using namespace AssetDocumentSequenceRetargetSourceName;
 OutBatch = {};
 if (!Retained::ContextValid() || Intents.Num() != 1 || !Retained::CanonicalPath(Request.TargetAssetPath)) return Result(false);
 const auto& Intent = Intents[0]; TSharedPtr<IAssetDocumentEditorSequenceRetargetSourceNameMutation> Editor;
 const auto Prepared = PrepareAssetDocumentEditorSequenceRetargetSourceNameMutation(Request, Intent, Editor);
 if (!Prepared.bValid) return Prepared;
 auto* Target = Cast<UAnimSequence>(FSoftObjectPath(Request.TargetAssetPath).ResolveObject());
 if (!Retained::CanonicalSequence(Target) || Target->GetPathName() != Request.TargetAssetPath) return Result(false);
 USkeletalMesh* Mesh = nullptr;
 if (!ReadSaveStableMesh(*Target, Mesh)) return Result(false, TEXT("AnimationProvider.RetargetSourceName.PreimageUnrepresentable"));
 auto* Reference = Target->RefPoseSeq.Get();
 if (!Retained::SettingsReady(*Target) || !Retained::CompressedCurrent(*Target) || Target->HasResidency(static_cast<const ITargetPlatform*>(nullptr))
  || Target->GetSkeleton()->GetOutermost() == Target->GetOutermost() || (Mesh && Mesh->GetOutermost() == Target->GetOutermost())
  || (Reference && (Reference == Target || !Retained::CanonicalSequence(Reference) || Reference->GetSkeleton() != Target->GetSkeleton()
   || Reference->GetOutermost() == Target->GetOutermost() || !Retained::SettingsReady(*Reference) || !Retained::CompressedCurrent(*Reference)))) return Result(false);
 auto State = MakeShared<FPreparedState>(*Target, Mesh, Editor);
 const int32 Bones = Target->GetSkeleton()->GetReferenceSkeleton().GetNum();
 if (!PoseReady(State->OriginalEffective, Bones) || !PoseReady(State->DesiredEffective, Bones) || !State->BaselineCurrent()) return Result(false);
 auto Capture = MakeShared<FCapture>();
 FValidator Validator = [State, Capture, Intent, Request](const auto& I, auto& Handles)
 {
  if (!Retained::ContextValid() || !Capture->bOpened || Capture->bClosed || Capture->bConsumed
   || I.PrimitiveId != Primitive || I.Version != 1 || I.Phase != Intent.Phase || I.RequestId != Request.RequestId
   || I.RuntimeRevision != Request.ExpectedRuntimeRevision || I.TransactionId != Capture->Transaction
   || !ValidateAssetDocumentPrimitiveInvocation(I, MakeAnimationSequenceRetargetSourceNameDescriptor()).bValid || !State->BaselineCurrent()) return Result(false);
  return State->Editor->ValidateInvocation(I, Handles);
 };
 auto Binding = MakeShared<FActiveCapture>(); Binding->Validate = Validator; Binding->State = State; Binding->Capture = Capture;
 FAssetDocumentPreparedMutationBatch Batch;
 Batch.OpenMutationCapture = [State, Capture, Binding](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!Retained::ContextValid() || !Id.IsValid() || Capture->bOpened || Capture->bClosed || ActiveCapture
   || HasAnimationSequenceRetargetReferencesCapture() || !State->BaselineCurrent())
  { Diagnostics.Append(Result(false).Diagnostics); return {}; }
  const auto Opened = State->Editor->OpenCapture(Id);
  if (!Opened.bValid) { Diagnostics.Append(Opened.Diagnostics); return {}; }
  Capture->Transaction = Id; Capture->bOpened = true; Binding->Transaction = Id; ActiveCapture = &Binding.Get();
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([State, Capture, Binding]
  { if (ActiveCapture == &Binding.Get()) ActiveCapture = nullptr; State->Editor->CloseCapture(); Capture->bClosed = true; });
 };
 auto& C = Batch.Contributions.AddDefaulted_GetRef(); C.StepIndex = Intent.StepIndex;
 C.OwnedSurfaceIds = {Request.TargetAssetPath + TEXT("::RetargetSource")};
 C.RawFootprint = Retained::Footprint(Request.TargetAssetPath, Target->GetOutermost()->GetName());
 C.InitialAction = MakeShared<FInverse>(State, Capture); C.ValidateInvocation = MoveTemp(Validator);
 OutBatch = MoveTemp(Batch); return Result(true);
}

bool HasAnimationSequenceRetargetSourceNameCapture()
{ return AssetDocumentSequenceRetargetSourceName::ActiveCapture != nullptr; }

FAssetDocumentPrimitiveOutcome InvokeAnimationSequenceRetargetSourceName(
 const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* T)
{
 if (HasAnimationSequenceRetargetReferencesCapture()) return InvokeAnimationSequenceRetargetReferences(I, Handles, T);
 using namespace AssetDocumentSequenceRetargetSourceName;
 static bool bInvoking = false;
 if (!Retained::ContextValid() || bInvoking || !ActiveCapture || ActiveCapture->Transaction != I.TransactionId)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Exact prepared RetargetSource FName capture required."));
 TGuardValue<bool> Guard(bInvoking, true);
 if (!T || T->GetState() != EAssetDocumentAtomicState::Mutating || T->GetTransactionId() != I.TransactionId
  || T->GetMutationToken().RequestId != I.RequestId || T->GetMutationToken().TransactionId != I.TransactionId
  || T->GetMutationToken().RuntimeRevision != I.RuntimeRevision)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.TransactionUnavailable"), TEXT("Matching active parent transaction required."));
 auto Valid = ActiveCapture->Validate(I, Handles);
 if (!Valid.bValid) { auto F = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Prepared validation failed.")); F.Diagnostics = Valid.Diagnostics; return F; }
 auto State = ActiveCapture->State; auto Capture = ActiveCapture->Capture;
 const bool Changed = State->Editor->HasChanges();
 Valid = WithTargetIdentity(*State, I, Handles, [&]
 {
  if (!State->BaselineCurrent()) return Result(false);
  // The fixed Editor participant alone consumes this active raw batch and
  // arms the parent preimage before its setter or our first completion wait.
  const auto Applied = State->Editor->Apply(I, Handles, *T); Capture->bConsumed = true;
  Capture->bTouched |= State->Editor->DidWrite();
  if (!Applied.bValid) return Applied;
  if (Changed && !Complete(*State, false)) return Result(false, TEXT("AnimationProvider.CompletionFailed"));
  return Result(State->Completed(false), TEXT("AnimationProvider.ReadbackMismatch"));
 });
 if (!Valid.bValid) { auto F = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Fixed name write/public completion failed.")); F.Diagnostics = Valid.Diagnostics; return F; }
 FAssetDocumentPrimitiveOutcome O; O.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 O.Value.Kind = EAssetDocumentRuntimeValueKind::Object; O.Value.ObjectKeys = {TEXT("changed"), TEXT("retarget_source_name"), TEXT("verified")};
 FAssetDocumentRuntimeValueDto Change, Name, Verified; Change.Kind = Verified.Kind = EAssetDocumentRuntimeValueKind::Boolean;
 Change.BooleanValue = Changed; Verified.BooleanValue = true; Name.Kind = EAssetDocumentRuntimeValueKind::String;
 Name.StringValue = State->Sequence()->RetargetSource.ToString(); O.Value.Elements = {Change, Name, Verified};
 if (Changed) { auto& C = O.ChangedSet.AddDefaulted_GetRef(); C.AuthorityId = Primitive; C.StableObjectId = State->Target.Path;
  C.Path = TEXT("/References/RetargetSource"); C.ChangeKind = TEXT("Replace"); }
 return O;
}

FAssetDocumentPrimitiveDescriptor MakeAnimationSequenceRetargetSourceNameDescriptor()
{
 using namespace AssetDocumentSequenceRetargetSourceName;
 FAssetDocumentPrimitiveDescriptor D; D.PrimitiveId = Primitive; D.Version = 1; D.ProviderId = TEXT("animation");
 D.Owner = EAssetDocumentPrimitiveOwner::Animation; D.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
 D.Access = EAssetDocumentPrimitiveAccess::Mutating; D.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {{TEXT("sequence"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, true}};
 D.bRequiresActiveTransaction = true; D.InputSchemaId = Schema;
 D.OutputSchemaId = TEXT("animation.sequence.retarget_source_name.set.v1.output.v1");
 D.MutationFootprintSchemaId = TEXT("animation.sequence.retarget_source_name.set.v1.footprint.v1"); D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.StableDiagnosticCodes = {TEXT("AnimationProvider.InvalidValueShape"), TEXT("AnimationProvider.InvalidHandlePayload"),
  TEXT("AnimationProvider.TargetClassMismatch"), TEXT("AnimationProvider.AuthorityUnavailable"), TEXT("AnimationProvider.OperationRejected"),
  TEXT("AnimationProvider.TransactionUnavailable"), TEXT("AnimationProvider.CompletionFailed"), TEXT("AnimationProvider.ReadbackMismatch"),
  TEXT("AnimationProvider.RetargetSourceName.InvalidState"), TEXT("AnimationProvider.RetargetSourceName.PreimageUnrepresentable"),
  TEXT("AnimationProvider.RetargetSourceName.RollbackFailed"), TEXT("Editor.SequenceRetargetSourceName.Rejected"), TEXT("Editor.SequenceRetargetSourceName.IntentShape")};
 auto& A = D.Admission; A.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper; A.ExactEngineVersion = TEXT("5.7.4");
 A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation; A.PublicModule = TEXT("Engine;CoreUObject;AssetDocumentEditor");
 A.PublicHeader = TEXT("Animation/AnimSequence.h;Animation/Skeleton.h;AnimationRuntime.h;UObject/PropertyAccessUtil.h;AssetDocumentEditorSequenceRetargetSourceNameMutation.h");
 A.ExportedAuthoritativeApi = TEXT("PropertyAccessUtil::SetPropertyValue_Object;PrepareAssetDocumentEditorSequenceRetargetSourceNameMutation;UAnimSequence::ClearAllCachedCookedPlatformData;CacheDerivedDataForCurrentPlatform;WaitOnExistingCompression;GetRetargetTransforms;GetRetargetTransformsSourceName;USkeleton::GetRefLocalPoses;FAnimationRuntime::MakeSkeletonRefPoseFromMesh");
 A.AuthorityContextId = TEXT("animation.editor-game-thread.active-transaction.v1"); A.PrimaryOperationId = Primitive;
 A.IdentityLifetimeContractId = TEXT("animation.editor-issued-asset-identity.request-transaction-revision.v1");
 A.TransactionContractId = TEXT("animation.core-active-transaction.v1"); A.RollbackContractId = RollbackId;
 A.CompletionContractId = TEXT("animation.sequence.retarget-source-name.fixed-Never-clear-cache-wait-effective-readback.v1");
 A.SaveReloadEvidenceId = TEXT("animation.sequence.retarget-source-name.launcher-5.7.4-save-distinct-fresh.v1");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimationProvider.SequenceRetargetSourceNameCore.LateFailureRollback")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimationProvider.SequenceRetargetSourceName.EditorFixedSetter"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceRetargetSourceName.Contract"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceRetargetSourceNameCore.HandlePreflight"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceRetargetSourceNameCore.SetNoneNoop")};
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|Engine+Editor+CoreUObject|fixed-FName-Never-helper|full-readonly-map+save-stable-retained-pose|ClearCookedCache+Cache+wait+effective-pose|retarget_source_name.set.v1");
 return D;
}

bool IsSequenceRetargetSourceNameAdmissionShape(const FAssetDocumentPrimitiveDescriptor& D)
{
 const auto E = MakeAnimationSequenceRetargetSourceNameDescriptor();
 if (D.PrimitiveId != E.PrimitiveId || D.Version != E.Version || D.ProviderId != E.ProviderId || D.Owner != E.Owner
  || D.ExecutionThread != E.ExecutionThread || D.Access != E.Access || D.AllowedPhases != E.AllowedPhases
  || D.AllowedHandleKinds != E.AllowedHandleKinds || D.HandleArgumentSchemas.Num() != 1 || !D.HandleResultSchemas.IsEmpty()
  || D.bRequiresActiveTransaction != E.bRequiresActiveTransaction || D.InputSchemaId != E.InputSchemaId
  || D.OutputSchemaId != E.OutputSchemaId || D.MutationFootprintSchemaId != E.MutationFootprintSchemaId
  || D.StableDiagnosticCodes != E.StableDiagnosticCodes || D.RequiredConsumerIds != E.RequiredConsumerIds) return false;
 const auto& H = D.HandleArgumentSchemas[0]; const auto& EH = E.HandleArgumentSchemas[0];
 if (H.Name != EH.Name || H.Kind != EH.Kind || H.RequiredAccess != EH.RequiredAccess || H.bRequired != EH.bRequired) return false;
 const auto& A = D.Admission; const auto& B = E.Admission;
 return A.ContractVersion == B.ContractVersion && A.Disposition == B.Disposition && A.AuthorityPath == B.AuthorityPath
  && A.ExactEngineVersion == B.ExactEngineVersion && A.PhysicalOwner == B.PhysicalOwner && A.PublicModule == B.PublicModule
  && A.PublicHeader == B.PublicHeader && A.ExportedAuthoritativeApi == B.ExportedAuthoritativeApi && A.AuthorityContextId == B.AuthorityContextId
  && A.PrimaryOperationId == B.PrimaryOperationId && A.IdentityLifetimeContractId == B.IdentityLifetimeContractId
  && A.TransactionContractId == B.TransactionContractId && A.RollbackContractId == B.RollbackContractId
  && A.CompletionContractId == B.CompletionContractId && A.SaveReloadEvidenceId == B.SaveReloadEvidenceId
  && A.FailureInjectionTestIds == B.FailureInjectionTestIds && A.VerificationTestIds == B.VerificationTestIds
  && A.AuthorityFingerprint == B.AuthorityFingerprint && A.SourceMarker.IsEmpty() && A.RawLifecycleAllowlistId.IsEmpty()
  && A.GovernanceId.IsEmpty() && A.IndependentCandidateReviewId.IsEmpty() && A.IndependentBridgeReviewId.IsEmpty()
  && A.HelperReplacementCondition.IsEmpty() && A.RemovalCondition.IsEmpty();
}
