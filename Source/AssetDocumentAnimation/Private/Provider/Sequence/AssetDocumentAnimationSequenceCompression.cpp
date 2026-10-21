// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompression.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompressionState.h"

namespace AssetDocumentSequenceCompression
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
   return Result(false, TEXT("AnimationProvider.SequenceCompression.RollbackFailed"));
  bConsumed = true;
  if (!Capture->bTouched) return State->Editor->Restore();
  if (!State->UnchangedInputs()) return Result(false, TEXT("AnimationProvider.SequenceCompression.RollbackFailed"));
  State->Sequence()->WaitOnExistingCompression();
  if (!State->UnchangedInputs()) return Result(false, TEXT("AnimationProvider.SequenceCompression.RollbackFailed"));
  const auto Restored = State->Editor->Restore();
  return Restored.bValid ? Result(Complete(*State, true), TEXT("AnimationProvider.SequenceCompression.RollbackFailed")) : Restored;
 }
};
using FValidator = TFunction<FAssetDocumentContractValidationResult(const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&)>;
struct FActiveCapture { FGuid Transaction; FValidator Validate; TSharedPtr<FPreparedState> State; TSharedPtr<FCapture> Capture; };
// One synchronous operation-local capture, never a registry or minted handle.
const FActiveCapture* ActiveCapture = nullptr;
}
}

FAssetDocumentContractValidationResult PrepareAnimationSequenceCompressionBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 using namespace AssetDocumentSequenceCompression;
 OutBatch = {};
 if (!Retained::ContextValid() || Intents.Num() != 1 || !Retained::CanonicalPath(Request.TargetAssetPath)) return Result(false);
 const auto& Intent = Intents[0]; TSharedPtr<IAssetDocumentEditorSequenceCompressionMutation> Editor;
 const auto Prepared = PrepareAssetDocumentEditorSequenceCompressionMutation(Request, Intent, Editor);
 if (!Prepared.bValid) return Prepared;
 auto* Target = Cast<UAnimSequence>(FSoftObjectPath(Request.TargetAssetPath).ResolveObject());
 if (!Retained::CanonicalSequence(Target) || !Target->GetPathName().Equals(Request.TargetAssetPath, ESearchCase::CaseSensitive)) return Result(false);
 USkeletalMesh* Mesh = nullptr;
 if (!NameInputs::ReadSaveStableMesh(*Target, Mesh)) return Result(false, TEXT("AnimationProvider.SequenceCompression.PreimageUnrepresentable"));
 auto* Reference = Target->RefPoseSeq.Get();
 if (!Retained::SettingsReady(*Target) || !UsableCurrent(*Target) || Target->HasResidency(static_cast<const ITargetPlatform*>(nullptr))
  || Target->GetSkeleton()->GetOutermost() == Target->GetOutermost() || (Mesh && Mesh->GetOutermost() == Target->GetOutermost())
  || (Reference && (Reference == Target || !Retained::CanonicalSequence(Reference) || Reference->GetSkeleton() != Target->GetSkeleton()
   || Reference->GetOutermost() == Target->GetOutermost()
   || !Retained::SettingsReady(*Reference) || !Retained::CompressedCurrent(*Reference)))) return Result(false);
 auto State = MakeShared<FPreparedState>(*Target, Mesh, Editor);
 State->AddSettings(Target->BoneCompressionSettings.Get()); State->AddSettings(Target->CurveCompressionSettings.Get());
 State->AddSettings(Target->VariableFrameStrippingSettings.Get());
 // Editor already closed and validated this exact fields projection. Selection
 // here only freezes readonly inputs; live identity authority stays in leases.
 const auto& Fields = Intent.Intent.Elements[0];
 for (int32 Idx = 0; Idx < Fields.ObjectKeys.Num(); ++Idx)
  if (Fields.ObjectKeys[Idx].Equals(TEXT("BoneCompressionSettings"), ESearchCase::CaseSensitive)
   || Fields.ObjectKeys[Idx].Equals(TEXT("CurveCompressionSettings"), ESearchCase::CaseSensitive)
   || Fields.ObjectKeys[Idx].Equals(TEXT("VariableFrameStrippingSettings"), ESearchCase::CaseSensitive))
   State->AddSettings(FSoftObjectPath(Fields.Elements[Idx].StringValue).ResolveObject());
 if (!NameInputs::PoseReady(State->EffectivePose, Target->GetSkeleton()->GetReferenceSkeleton().GetNum()) || !State->BaselineCurrent()) return Result(false);
 auto Capture = MakeShared<FCapture>();
 FValidator Validator = [State, Capture, Intent, Request](const auto& I, auto& Handles)
 {
  if (!Retained::ContextValid() || !Capture->bOpened || Capture->bClosed || Capture->bConsumed
   || !I.PrimitiveId.Equals(Primitive, ESearchCase::CaseSensitive) || I.Version != 1 || I.Phase != Intent.Phase || I.RequestId != Request.RequestId
   || I.RuntimeRevision != Request.ExpectedRuntimeRevision || I.TransactionId != Capture->Transaction
   || !ValidateAssetDocumentPrimitiveInvocation(I, MakeAnimationSequenceCompressionDescriptor()).bValid || !State->BaselineCurrent()) return Result(false);
  return State->Editor->ValidateInvocation(I, Handles);
 };
 auto Binding = MakeShared<FActiveCapture>(); Binding->Validate = Validator; Binding->State = State; Binding->Capture = Capture;
 FAssetDocumentPreparedMutationBatch Batch;
 Batch.OpenMutationCapture = [State, Capture, Binding](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!Retained::ContextValid() || !Id.IsValid() || Capture->bOpened || Capture->bClosed || ActiveCapture || !State->BaselineCurrent())
  { Diagnostics.Append(Result(false).Diagnostics); return {}; }
  const auto Opened = State->Editor->OpenCapture(Id);
  if (!Opened.bValid) { Diagnostics.Append(Opened.Diagnostics); return {}; }
  Capture->Transaction = Id; Capture->bOpened = true; Binding->Transaction = Id; ActiveCapture = &Binding.Get();
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([State, Capture, Binding]
  { if (ActiveCapture == &Binding.Get()) ActiveCapture = nullptr; State->Editor->CloseCapture(); Capture->bClosed = true; });
 };
 auto& C = Batch.Contributions.AddDefaulted_GetRef(); C.StepIndex = Intent.StepIndex;
 for (const TCHAR* Field : {TEXT("BoneCompressionSettings"), TEXT("CurveCompressionSettings"), TEXT("VariableFrameStrippingSettings"),
  TEXT("CompressionErrorThresholdScale"), TEXT("bDoNotOverrideCompression")}) C.OwnedSurfaceIds.Add(Request.TargetAssetPath + TEXT("::Compression.") + Field);
 C.RawFootprint = Retained::Footprint(Request.TargetAssetPath, Target->GetOutermost()->GetName());
 C.InitialAction = MakeShared<FInverse>(State, Capture); C.ValidateInvocation = MoveTemp(Validator);
 OutBatch = MoveTemp(Batch); return Result(true);
}

FAssetDocumentPrimitiveOutcome InvokeAnimationSequenceCompression(
 const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* T)
{
 using namespace AssetDocumentSequenceCompression;
 static bool bInvoking = false;
 if (!Retained::ContextValid() || bInvoking || !ActiveCapture || ActiveCapture->Transaction != I.TransactionId)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Exact prepared five-field compression capture required."));
 TGuardValue<bool> Guard(bInvoking, true);
 if (!T || T->GetState() != EAssetDocumentAtomicState::Mutating || T->GetTransactionId() != I.TransactionId
  || T->GetMutationToken().RequestId != I.RequestId || T->GetMutationToken().TransactionId != I.TransactionId
  || T->GetMutationToken().RuntimeRevision != I.RuntimeRevision)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.TransactionUnavailable"), TEXT("Matching active parent transaction required."));
 auto Valid = ActiveCapture->Validate(I, Handles);
 if (!Valid.bValid) { auto F = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Prepared validation failed.")); F.Diagnostics = Valid.Diagnostics; return F; }
 auto State = ActiveCapture->State; auto Capture = ActiveCapture->Capture;
 const bool Changed = State->Editor->HasChanges();
 Valid = State->Editor->WithVerifiedAssets(I, Handles, [&]
 {
  if (!State->BaselineCurrent()) return Result(false);
  // Only Editor consumes/enlists this batch. Its first setter follows the one
  // parent preimage; all supplied readonly and target callbacks cover completion.
  const auto Applied = State->Editor->Apply(I, Handles, *T); Capture->bConsumed = true;
  Capture->bTouched |= State->Editor->DidWrite();
  if (!Applied.bValid) return Applied;
  if (Changed && !Complete(*State, false)) return Result(false, TEXT("AnimationProvider.CompletionFailed"));
  return Result(State->Completed(false), TEXT("AnimationProvider.ReadbackMismatch"));
 });
 if (!Valid.bValid) { auto F = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Fixed compression write/public completion failed.")); F.Diagnostics = Valid.Diagnostics; return F; }
 FAssetDocumentPrimitiveOutcome O; O.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 O.Value.Kind = EAssetDocumentRuntimeValueKind::Object; O.Value.ObjectKeys = {TEXT("changed"), TEXT("verified")};
 FAssetDocumentRuntimeValueDto Change, Verified; Change.Kind = Verified.Kind = EAssetDocumentRuntimeValueKind::Boolean;
 Change.BooleanValue = Changed; Verified.BooleanValue = true; O.Value.Elements = {Change, Verified};
 if (Changed) { auto& C = O.ChangedSet.AddDefaulted_GetRef(); C.AuthorityId = Primitive; C.StableObjectId = State->Target.Path;
  C.Path = TEXT("/Compression"); C.ChangeKind = TEXT("Replace"); }
 return O;
}

FAssetDocumentPrimitiveDescriptor MakeAnimationSequenceCompressionDescriptor()
{
 using namespace AssetDocumentSequenceCompression;
 FAssetDocumentPrimitiveDescriptor D; D.PrimitiveId = Primitive; D.Version = 1; D.ProviderId = TEXT("animation");
 D.Owner = EAssetDocumentPrimitiveOwner::Animation; D.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
 D.Access = EAssetDocumentPrimitiveAccess::Mutating; D.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {{TEXT("sequence"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, true},
  {TEXT("bone_compression_settings"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, false},
  {TEXT("curve_compression_settings"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, false},
  {TEXT("frame_stripping_settings"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, false}};
 D.bRequiresActiveTransaction = true; D.InputSchemaId = Schema;
 D.OutputSchemaId = TEXT("animation.sequence.compression.settings.set.v1.output.v1");
 D.MutationFootprintSchemaId = TEXT("animation.sequence.compression.settings.set.v1.footprint.v1"); D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.StableDiagnosticCodes = {TEXT("AnimationProvider.InvalidValueShape"), TEXT("AnimationProvider.InvalidHandlePayload"),
  TEXT("AnimationProvider.TargetClassMismatch"), TEXT("AnimationProvider.AuthorityUnavailable"), TEXT("AnimationProvider.OperationRejected"),
  TEXT("AnimationProvider.TransactionUnavailable"), TEXT("AnimationProvider.CompletionFailed"), TEXT("AnimationProvider.ReadbackMismatch"),
  TEXT("AnimationProvider.SequenceCompression.InvalidState"), TEXT("AnimationProvider.SequenceCompression.PreimageUnrepresentable"),
  TEXT("AnimationProvider.SequenceCompression.RollbackFailed"), TEXT("Editor.SequenceCompression.Rejected"), TEXT("Editor.SequenceCompression.IntentShape")};
 auto& A = D.Admission; A.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper; A.ExactEngineVersion = TEXT("5.7.4");
 A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation; A.PublicModule = TEXT("Engine;CoreUObject;AssetDocumentEditor");
 A.PublicHeader = TEXT("Animation/AnimSequence.h;Animation/AnimBoneCompressionSettings.h;Animation/AnimCurveCompressionSettings.h;Animation/VariableFrameStrippingSettings.h;Animation/AnimCompressionTypes.h;UObject/PropertyAccessUtil.h;UObject/PerPlatformProperties.h;AssetCompilingManager.h;IAssetCompilingManager.h;AssetDocumentEditorSequenceCompressionMutation.h");
 A.ExportedAuthoritativeApi = TEXT("PropertyAccessUtil::SetPropertyValue_Object;IsSinglePropertyIdentical;PrepareAssetDocumentEditorSequenceCompressionMutation;UAnimSequence::ClearAllCachedCookedPlatformData;CacheDerivedDataForCurrentPlatform;WaitOnExistingCompression;GetCompressedData;FAssetCompilingManager::Get;GetRegisteredManagers;IAssetCompilingManager::GetAssetTypeName;GetNumRemainingAssets;UAnimBoneCompressionSettings::AreSettingsValid;PopulateDDCKey;UAnimCurveCompressionSettings::AreSettingsValid;PopulateDDCKey;FPerPlatformBool::Serialize;FPerPlatformInt::Serialize");
 A.AuthorityContextId = TEXT("animation.editor-game-thread.active-transaction.v1"); A.PrimaryOperationId = Primitive;
 A.IdentityLifetimeContractId = TEXT("animation.editor-issued-asset-identity.request-transaction-revision.v1");
 A.TransactionContractId = TEXT("animation.core-active-transaction.v1"); A.RollbackContractId = RollbackId;
 A.CompletionContractId = TEXT("animation.sequence.compression-supplied-settings.fixed-Never-five-field-clear-cache-wait-current-platform.v1");
 A.SaveReloadEvidenceId = TEXT("animation.sequence.compression-supplied-settings.launcher-5.7.4-save-distinct-fresh.v1");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimationProvider.SequenceCompressionCore.PartialFailureRollback"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceCompressionCore.LateFailureRollback")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimationProvider.SequenceCompression.EditorFixedSetter"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceCompression.Contract"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceCompressionCore.HandlePreflight"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceCompressionCore.SetSuppliedNoop"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceCompressionCore.PartialFailureRollback"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceCompressionCore.LateFailureRollback")};
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|Engine+Editor+CoreUObject|fixed-five-field-Never|old+new-readonly-settings|full-retained-author|public-animation-queue-quiescence|ClearCookedCache+Cache+wait+usable-current-platform|compression.settings.set.v1");
 return D;
}

bool IsSequenceCompressionAdmissionShape(const FAssetDocumentPrimitiveDescriptor& D)
{
 const auto E = MakeAnimationSequenceCompressionDescriptor();
 auto Same = [](const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); };
 auto SameStrings = [&](const TArray<FString>& A, const TArray<FString>& B)
 { if (A.Num() != B.Num()) return false; for (int32 I = 0; I < A.Num(); ++I) if (!Same(A[I], B[I])) return false; return true; };
 if (!Same(D.PrimitiveId, E.PrimitiveId) || D.Version != E.Version || !Same(D.ProviderId, E.ProviderId) || D.Owner != E.Owner
  || D.ExecutionThread != E.ExecutionThread || D.Access != E.Access || D.AllowedPhases != E.AllowedPhases
  || D.AllowedHandleKinds != E.AllowedHandleKinds || D.HandleArgumentSchemas.Num() != E.HandleArgumentSchemas.Num() || !D.HandleResultSchemas.IsEmpty()
  || D.bRequiresActiveTransaction != E.bRequiresActiveTransaction || !Same(D.InputSchemaId, E.InputSchemaId)
  || !Same(D.OutputSchemaId, E.OutputSchemaId) || !Same(D.MutationFootprintSchemaId, E.MutationFootprintSchemaId)
  || !SameStrings(D.StableDiagnosticCodes, E.StableDiagnosticCodes) || !SameStrings(D.RequiredConsumerIds, E.RequiredConsumerIds)) return false;
 for (int32 Idx = 0; Idx < E.HandleArgumentSchemas.Num(); ++Idx)
 {
  const auto& H = D.HandleArgumentSchemas[Idx]; const auto& EH = E.HandleArgumentSchemas[Idx];
  if (!Same(H.Name, EH.Name) || H.Kind != EH.Kind || H.RequiredAccess != EH.RequiredAccess || H.bRequired != EH.bRequired) return false;
 }
 const auto& A = D.Admission; const auto& B = E.Admission;
 return A.ContractVersion == B.ContractVersion && A.Disposition == B.Disposition && A.AuthorityPath == B.AuthorityPath
  && Same(A.ExactEngineVersion, B.ExactEngineVersion) && A.PhysicalOwner == B.PhysicalOwner && Same(A.PublicModule, B.PublicModule)
  && Same(A.PublicHeader, B.PublicHeader) && Same(A.ExportedAuthoritativeApi, B.ExportedAuthoritativeApi) && Same(A.AuthorityContextId, B.AuthorityContextId)
  && Same(A.PrimaryOperationId, B.PrimaryOperationId) && Same(A.IdentityLifetimeContractId, B.IdentityLifetimeContractId)
  && Same(A.TransactionContractId, B.TransactionContractId) && Same(A.RollbackContractId, B.RollbackContractId)
  && Same(A.CompletionContractId, B.CompletionContractId) && Same(A.SaveReloadEvidenceId, B.SaveReloadEvidenceId)
  && SameStrings(A.FailureInjectionTestIds, B.FailureInjectionTestIds) && SameStrings(A.VerificationTestIds, B.VerificationTestIds)
  && Same(A.AuthorityFingerprint, B.AuthorityFingerprint) && A.SourceMarker.IsEmpty() && A.RawLifecycleAllowlistId.IsEmpty()
  && A.GovernanceId.IsEmpty() && A.IndependentCandidateReviewId.IsEmpty() && A.IndependentBridgeReviewId.IsEmpty()
  && A.HelperReplacementCondition.IsEmpty() && A.RemovalCondition.IsEmpty();
}
