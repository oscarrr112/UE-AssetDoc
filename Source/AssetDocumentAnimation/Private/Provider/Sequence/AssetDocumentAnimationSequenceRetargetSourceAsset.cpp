// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceAsset.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceAssetState.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetReferencesComposition.h"

void PublishAnimationSequenceRetargetSourceAsset(UAnimSequence& Sequence, USkeletalMesh* Mesh)
{
 if (Mesh) Sequence.SetRetargetSourceAsset(Mesh); else Sequence.ClearRetargetSourceAsset();
 Sequence.UpdateRetargetSourceAssetData();
}

namespace AssetDocumentSequenceRetargetSource
{
namespace
{
bool Parse(const FAssetDocumentRuntimeValueDto& V, bool bPreparation, FString& Operation, FString& Path)
{
 if (!ValidateAssetDocumentRuntimeValue(V).bValid || V.Kind != EAssetDocumentRuntimeValueKind::Object
  || V.ObjectKeys.Num() != (bPreparation ? 2 : 1) || V.Elements.Num() != V.ObjectKeys.Num()) return false;
 const int32 Op = V.ObjectKeys.IndexOfByKey(TEXT("operation"));
 if (!V.Elements.IsValidIndex(Op) || V.Elements[Op].Kind != EAssetDocumentRuntimeValueKind::String) return false;
 Operation = V.Elements[Op].StringValue;
 if (Operation != TEXT("set") && Operation != TEXT("clear")) return false;
 if (!bPreparation) return true;
 const int32 Source = V.ObjectKeys.IndexOfByKey(TEXT("retarget_source_asset_path"));
 if (!V.Elements.IsValidIndex(Source)) return false;
 const auto& Ref = V.Elements[Source];
 if (Operation == TEXT("clear")) return Ref.Kind == EAssetDocumentRuntimeValueKind::Null;
 if (Ref.Kind != EAssetDocumentRuntimeValueKind::String || !CanonicalPath(Ref.StringValue)) return false;
 Path = Ref.StringValue; return true;
}
bool ReadPreimage(const UAnimSequence& S, USkeletalMesh*& Mesh, TArray<FTransform>& Pose)
{
 Mesh = nullptr; Pose = S.RetargetSourceAssetReferencePose;
 const auto Ref = S.GetRetargetSourceAsset();
 if (Ref.IsNull()) return Pose.IsEmpty();
 const FString Path = Ref.ToSoftObjectPath().ToString();
 if (!CanonicalPath(Path)) return false;
 Mesh = Cast<USkeletalMesh>(Ref.ToSoftObjectPath().ResolveObject());
 if (!CanonicalMesh(Mesh, *S.GetSkeleton()) || Mesh->GetPathName() != Path) return false;
 TArray<FTransform> Native;
 FAnimationRuntime::MakeSkeletonRefPoseFromMesh(Mesh, S.GetSkeleton(), Native);
 return SamePose(Pose, Native);
}
FAssetDocumentVerifiedLeaseResult AcquireEditor(IAssetDocumentHandleResolver& Handles,
 const FAssetDocumentPrimitiveInvocation& I, const TCHAR* Name, EAssetDocumentHandleAccess Access)
{
 const auto* Arg = I.HandleArguments.FindByPredicate([&](const auto& A) { return A.Name == Name; });
 if (!Arg) return {};
 FAssetDocumentHandleValidationContext C; C.RequestId = I.RequestId; C.TransactionId = I.TransactionId;
 C.RuntimeRevision = I.RuntimeRevision; C.ProviderId = TEXT("editor"); C.Phase = I.Phase;
 C.RequiredKind = EAssetDocumentHandleKind::Asset; C.RequiredAccess = Access;
 // Consume the actual immutable Editor identity. Never relabel the issuer.
 return Handles.AcquireVerifiedLease(Arg->Handle, C);
}
struct FPreparedState
{
 FIdentity Target;
 FSkeletonInput Skeleton;
 FRetainedState Retained;
 FReferenceSequenceInput ReferenceSequence;
 TUniquePtr<FMeshInput> Old, Desired;
 TArray<FTransform> OldPose, DesiredPose;
 FSoftObjectPath OldPath, DesiredPath;
 FString Operation;
 bool bChanged;
 FPreparedState(UAnimSequence& S, USkeletalMesh* OldMesh, USkeletalMesh* DesiredMesh,
  TArray<FTransform> Pose, FString Op) : Target(&S), Skeleton(*S.GetSkeleton(), S.RetargetSource), Retained(S),
  ReferenceSequence(S.RefPoseSeq), Old(OldMesh ? MakeUnique<FMeshInput>(*OldMesh, *S.GetSkeleton()) : nullptr),
  Desired(DesiredMesh ? MakeUnique<FMeshInput>(*DesiredMesh, *S.GetSkeleton()) : nullptr), OldPose(MoveTemp(Pose)),
  OldPath(S.GetRetargetSourceAsset().ToSoftObjectPath()), DesiredPath(DesiredMesh), Operation(MoveTemp(Op)), bChanged(false)
 {
  if (DesiredMesh) FAnimationRuntime::MakeSkeletonRefPoseFromMesh(DesiredMesh, S.GetSkeleton(), DesiredPose);
  bChanged = OldPath != DesiredPath || !SamePose(OldPose, DesiredPose);
 }
 UAnimSequence* Sequence() const { return Cast<UAnimSequence>(Target.Object.Get()); }
 USkeletalMesh* Mesh(bool bOriginal) const
 { const auto* Input = bOriginal ? Old.Get() : Desired.Get(); return Input ? Cast<USkeletalMesh>(Input->Identity.Object.Get()) : nullptr; }
 bool InputsCurrent() const
 {
  const auto* S = Cast<USkeleton>(Skeleton.Identity.Object.Get());
  return Skeleton.Current() && ReferenceSequence.Current() && (!Old || Old->Current(*S)) && (!Desired || Desired->Current(*S));
 }
 bool TargetCurrent() const
 { return Target.Current() && CanonicalSequence(Sequence()) && Sequence()->GetSkeleton() == Skeleton.Identity.Object.Get(); }
 bool AuthorMatches(bool bOriginal) const
 {
  const auto* S = Sequence();
  return S->GetRetargetSourceAsset().ToSoftObjectPath() == (bOriginal ? OldPath : DesiredPath)
   && SamePose(S->RetargetSourceAssetReferencePose, bOriginal ? OldPose : DesiredPose);
 }
 bool BaselineCurrent() const
 {
  return TargetCurrent() && InputsCurrent() && Retained.Matches(*Sequence()) && AuthorMatches(true)
   && CompressedCurrent(*Sequence()) && !Sequence()->HasResidency(static_cast<const ITargetPlatform*>(nullptr));
 }
 bool Completed(bool bOriginal) const
 { return TargetCurrent() && InputsCurrent() && Retained.Matches(*Sequence()) && AuthorMatches(bOriginal) && CompressedCurrent(*Sequence())
   && !Sequence()->HasResidency(static_cast<const ITargetPlatform*>(nullptr)); }
};
// This same public chain is used forward and inverse. The stored author pose,
// hashes, private maps and compressed bytes are never assigned by this provider.
bool PublishAndComplete(FPreparedState& State, bool bOriginal)
{
 auto* S = State.Sequence(); auto* Mesh = State.Mesh(bOriginal);
 PublishAnimationSequenceRetargetSourceAsset(*S, Mesh);
 if (!State.TargetCurrent() || !State.InputsCurrent() || !State.Retained.Matches(*S) || !State.AuthorMatches(bOriginal)) return false;
 S->ClearAllCachedCookedPlatformData();
 S->CacheDerivedDataForCurrentPlatform(); S->WaitOnExistingCompression();
 return State.Completed(bOriginal);
}
struct FCapture
{
 FGuid Transaction;
 bool bOpened = false, bClosed = false, bWritten = false, bConsumed = false;
};
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
  if (bConsumed || !ContextValid() || !Capture->bOpened || !Capture->bClosed)
   return Result(false, TEXT("AnimationProvider.RetargetSource.RollbackFailed"));
  bConsumed = true;
  // Nothing was published by this participant. A changed readonly input is a
  // preflight refusal, not permission to restore it or poison a no-write abort.
  if (!Capture->bWritten) return Result(true);
  if (!State->TargetCurrent() || !State->InputsCurrent() || !State->Retained.Matches(*State->Sequence()))
   return Result(false, TEXT("AnimationProvider.RetargetSource.RollbackFailed"));
  State->Sequence()->WaitOnExistingCompression();
  if (!State->TargetCurrent() || !State->InputsCurrent() || !State->Retained.Matches(*State->Sequence()))
   return Result(false, TEXT("AnimationProvider.RetargetSource.RollbackFailed"));
  return Result(PublishAndComplete(*State, true), TEXT("AnimationProvider.RetargetSource.RollbackFailed"));
 }
};
using FValidator = TFunction<FAssetDocumentContractValidationResult(const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&)>;
struct FActiveCapture
{ FGuid Transaction; FValidator Validate; TSharedPtr<FPreparedState> State; TSharedPtr<FCapture> Capture; };
// Single operation-local, non-reentrant game-thread lease, not a token registry.
const FActiveCapture* ActiveCapture = nullptr;
template <typename Consumer>
FAssetDocumentContractValidationResult WithIdentities(const FPreparedState& S, const FAssetDocumentPrimitiveInvocation& I,
 IAssetDocumentHandleResolver& Handles, Consumer&& Body)
{
 auto TargetLease = AcquireEditor(Handles, I, TEXT("sequence"), EAssetDocumentHandleAccess::Mutable);
 if (!TargetLease.Lease) return Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
 auto InTarget = [&]()
 {
  return WithAssetDocumentEditorAssetIdentity(*TargetLease.Lease, [&](const FAssetDocumentEditorAssetIdentityView& V)
  {
   return &V.GetObject() == S.Sequence() && V.GetTypeRevision() == S.Target.Revision
    && V.GetCanonicalObjectPath() == S.Target.Path ? Body() : Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
  });
 };
 if (S.Operation == TEXT("clear")) return InTarget();
 auto SourceLease = AcquireEditor(Handles, I, TEXT("retarget_source_asset"), EAssetDocumentHandleAccess::ReadOnly);
 if (!SourceLease.Lease) return Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
 return WithAssetDocumentEditorAssetIdentity(*SourceLease.Lease, [&](const FAssetDocumentEditorAssetIdentityView& V)
 {
  return &V.GetObject() == S.Mesh(false) && V.GetTypeRevision() == S.Desired->Identity.Revision
   && V.GetCanonicalObjectPath() == S.Desired->Identity.Path ? InTarget() : Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
 });
}
}
}

FAssetDocumentContractValidationResult PrepareAnimationSequenceRetargetSourceAssetBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 using namespace AssetDocumentSequenceRetargetSource;
 OutBatch = {};
 if (!ContextValid() || !Request.RequestId.IsValid() || !Request.ExpectedRuntimeRevision
  || Request.Operation != EAssetDocumentRuntimeOperation::Apply || Request.RequestedAssetClassPath != TEXT("/Script/Engine.AnimSequence")
  || !CanonicalPath(Request.TargetAssetPath) || Intents.Num() != 1) return Result(false);
 const auto& Intent = Intents[0]; FString Operation, Path;
 if (Intent.StepIndex < 0 || Intent.Requirement.PrimitiveId != Primitive || Intent.Requirement.Version != 2
  || Intent.Requirement.RequiredByRouteId != TEXT("animation_profile") || Intent.InputSchemaId != Schema
  || (Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply && Intent.Phase != EAssetDocumentRuntimeRequestPhase::Repair)
  || !Parse(Intent.Intent, true, Operation, Path)) return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 auto* Target = Cast<UAnimSequence>(FSoftObjectPath(Request.TargetAssetPath).ResolveObject());
 if (!CanonicalSequence(Target) || Target->GetPathName() != Request.TargetAssetPath) return Result(false);
 USkeletalMesh* Old = nullptr; TArray<FTransform> OldPose;
 if (!ReadPreimage(*Target, Old, OldPose)) return Result(false, TEXT("AnimationProvider.RetargetSource.PreimageUnrepresentable"));
 USkeletalMesh* Desired = Operation == TEXT("set") ? Cast<USkeletalMesh>(FSoftObjectPath(Path).ResolveObject()) : nullptr;
 if (Operation == TEXT("set") && (!CanonicalMesh(Desired, *Target->GetSkeleton()) || Desired->GetPathName() != Path))
  return Result(false, TEXT("AnimationProvider.RetargetSource.IncompatibleReference"));
 auto* Reference = Target->RefPoseSeq.Get();
 if (!SettingsReady(*Target) || !CompressedCurrent(*Target) || Target->HasResidency(static_cast<const ITargetPlatform*>(nullptr))
  || Target->GetSkeleton()->GetOutermost() == Target->GetOutermost()
  || (Old && Old->GetOutermost() == Target->GetOutermost()) || (Desired && Desired->GetOutermost() == Target->GetOutermost())
  || (Reference && (Reference == Target || !CanonicalSequence(Reference) || Reference->GetSkeleton() != Target->GetSkeleton()
   || Reference->GetOutermost() == Target->GetOutermost() || !SettingsReady(*Reference) || !CompressedCurrent(*Reference)))) return Result(false);
 auto State = MakeShared<FPreparedState>(*Target, Old, Desired, MoveTemp(OldPose), Operation);
 if (!State->BaselineCurrent()) return Result(false);
 auto Capture = MakeShared<FCapture>();
 FValidator Validator = [State, Capture, Intent, Request](const auto& I, auto& Handles)
 {
  FString Op, Unused;
  if (!ContextValid() || !Capture->bOpened || Capture->bClosed || Capture->bConsumed
   || I.PrimitiveId != Primitive || I.Version != 2 || I.Phase != Intent.Phase || I.RequestId != Request.RequestId
   || I.RuntimeRevision != Request.ExpectedRuntimeRevision || I.TransactionId != Capture->Transaction
   || !ValidateAssetDocumentPrimitiveInvocation(I, MakeAnimationSequenceRetargetSourceAssetDescriptor()).bValid
   || !Parse(I.ValueArguments, false, Op, Unused) || Op != State->Operation
   || I.HandleArguments.Num() != (Op == TEXT("set") ? 2 : 1) || !State->BaselineCurrent()) return Result(false);
  return WithIdentities(*State, I, Handles, [&] { return Result(State->BaselineCurrent()); });
 };
 auto Binding = MakeShared<FActiveCapture>(); Binding->Validate = Validator; Binding->State = State; Binding->Capture = Capture;
 FAssetDocumentPreparedMutationBatch Batch;
 Batch.OpenMutationCapture = [State, Capture, Binding](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!ContextValid() || !Id.IsValid() || Capture->bOpened || Capture->bClosed || ActiveCapture
   || HasAnimationSequenceRetargetReferencesCapture() || !State->BaselineCurrent())
  { Diagnostics.Append(Result(false).Diagnostics); return {}; }
  Capture->Transaction = Id; Capture->bOpened = true; Binding->Transaction = Id; ActiveCapture = &Binding.Get();
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([Capture, Binding]
  { if (ActiveCapture == &Binding.Get()) ActiveCapture = nullptr; Capture->bClosed = true; });
 };
 auto& C = Batch.Contributions.AddDefaulted_GetRef(); C.StepIndex = Intent.StepIndex;
 C.OwnedSurfaceIds = {Request.TargetAssetPath + TEXT("::RetargetSourceAsset"), Request.TargetAssetPath + TEXT("::RetargetSourceAssetReferencePose")};
 C.RawFootprint = Footprint(Request.TargetAssetPath, Target->GetOutermost()->GetName());
 C.InitialAction = MakeShared<FInverse>(State, Capture); C.ValidateInvocation = MoveTemp(Validator);
 OutBatch = MoveTemp(Batch); return Result(true);
}

bool HasAnimationSequenceRetargetSourceAssetCapture()
{ return AssetDocumentSequenceRetargetSource::ActiveCapture != nullptr; }

FAssetDocumentPrimitiveOutcome InvokeAnimationSequenceRetargetSourceAsset(
 const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* T)
{
 if (HasAnimationSequenceRetargetReferencesCapture()) return InvokeAnimationSequenceRetargetReferences(I, Handles, T);
 using namespace AssetDocumentSequenceRetargetSource;
 static bool bInvoking = false;
 if (!ContextValid() || bInvoking || !ActiveCapture || ActiveCapture->Transaction != I.TransactionId)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Exact prepared RetargetSourceAsset capture required."));
 TGuardValue<bool> Guard(bInvoking, true);
 if (!T || T->GetState() != EAssetDocumentAtomicState::Mutating || T->GetTransactionId() != I.TransactionId
  || T->GetMutationToken().RequestId != I.RequestId || T->GetMutationToken().TransactionId != I.TransactionId
  || T->GetMutationToken().RuntimeRevision != I.RuntimeRevision)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.TransactionUnavailable"), TEXT("Matching active parent transaction required."));
 auto Valid = ActiveCapture->Validate(I, Handles);
 if (!Valid.bValid) { auto F = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Prepared validation failed.")); F.Diagnostics = Valid.Diagnostics; return F; }
 auto State = ActiveCapture->State; auto Capture = ActiveCapture->Capture;
 Valid = WithIdentities(*State, I, Handles, [&]
 {
  if (!State->BaselineCurrent()) return Result(false);
  if (State->bChanged)
  {
   const auto Enlisted = T->EnlistFootprint(Footprint(State->Target.Path, State->Target.PackagePath));
   if (!Enlisted.bValid) return Enlisted;
   Capture->bWritten = true; Capture->bConsumed = true;
   State->Sequence()->WaitOnExistingCompression();
   if (!State->BaselineCurrent()) return Result(false, TEXT("AnimationProvider.ReadbackMismatch"));
   if (!PublishAndComplete(*State, false)) return Result(false, TEXT("AnimationProvider.CompletionFailed"));
  }
  else Capture->bConsumed = true;
  return Result(State->Completed(false), TEXT("AnimationProvider.ReadbackMismatch"));
 });
 if (!Valid.bValid) { auto F = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Public retarget mutation/completion failed.")); F.Diagnostics = Valid.Diagnostics; return F; }
 FAssetDocumentPrimitiveOutcome O; O.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 O.Value.Kind = EAssetDocumentRuntimeValueKind::Object; O.Value.ObjectKeys = {TEXT("changed"), TEXT("operation"), TEXT("verified")};
 FAssetDocumentRuntimeValueDto Changed, Op, Verified; Changed.Kind = Verified.Kind = EAssetDocumentRuntimeValueKind::Boolean;
 Changed.BooleanValue = State->bChanged; Verified.BooleanValue = true; Op.Kind = EAssetDocumentRuntimeValueKind::String; Op.StringValue = State->Operation;
 O.Value.Elements = {Changed, Op, Verified};
 if (State->bChanged) { auto& C = O.ChangedSet.AddDefaulted_GetRef(); C.AuthorityId = Primitive; C.StableObjectId = State->Target.Path; C.Path = TEXT("/References/RetargetSourceAsset"); C.ChangeKind = TEXT("Replace"); }
 return O;
}

FAssetDocumentPrimitiveDescriptor MakeAnimationSequenceRetargetSourceAssetDescriptor()
{
 using namespace AssetDocumentSequenceRetargetSource;
 FAssetDocumentPrimitiveDescriptor D;
 D.PrimitiveId = Primitive; D.Version = 2; D.ProviderId = TEXT("animation"); D.Owner = EAssetDocumentPrimitiveOwner::Animation;
 D.ExecutionThread = EAssetDocumentExecutionThread::GameThread; D.Access = EAssetDocumentPrimitiveAccess::Mutating;
 D.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {{TEXT("sequence"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, true},
  {TEXT("retarget_source_asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, false}};
 D.bRequiresActiveTransaction = true; D.InputSchemaId = Schema;
 D.OutputSchemaId = TEXT("animation.sequence.retarget_source_asset.set.v2.output.v2");
 D.MutationFootprintSchemaId = TEXT("animation.sequence.retarget_source_asset.set.v2.footprint.v2"); D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.StableDiagnosticCodes = {TEXT("AnimationProvider.InvalidValueShape"), TEXT("AnimationProvider.InvalidHandlePayload"),
  TEXT("AnimationProvider.TargetClassMismatch"), TEXT("AnimationProvider.AuthorityUnavailable"), TEXT("AnimationProvider.OperationRejected"),
  TEXT("AnimationProvider.TransactionUnavailable"), TEXT("AnimationProvider.CompletionFailed"), TEXT("AnimationProvider.ReadbackMismatch"),
  TEXT("AnimationProvider.RetargetSource.InvalidState"), TEXT("AnimationProvider.RetargetSource.PreimageUnrepresentable"),
  TEXT("AnimationProvider.RetargetSource.IncompatibleReference"), TEXT("AnimationProvider.RetargetSource.RollbackFailed")};
 auto& A = D.Admission; A.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper; A.ExactEngineVersion = TEXT("5.7.4");
 A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation; A.PublicModule = TEXT("Engine;CoreUObject");
 A.PublicHeader = TEXT("Animation/AnimSequence.h;AnimationRuntime.h;Animation/AnimCompressionTypes.h;UObject/UObjectGlobals.h");
 A.ExportedAuthoritativeApi = TEXT("UAnimSequence::SetRetargetSourceAsset;ClearRetargetSourceAsset;UpdateRetargetSourceAssetData;ClearAllCachedCookedPlatformData;CacheDerivedDataForCurrentPlatform;WaitOnExistingCompression;FAnimationRuntime::MakeSkeletonRefPoseFromMesh");
 A.AuthorityContextId = TEXT("animation.editor-game-thread.active-transaction.v1"); A.PrimaryOperationId = Primitive;
 A.IdentityLifetimeContractId = TEXT("animation.editor-issued-asset-identity.request-transaction-revision.v1");
 A.TransactionContractId = TEXT("animation.core-active-transaction.v1"); A.RollbackContractId = RollbackId;
 A.CompletionContractId = TEXT("animation.sequence.retarget-source.public-update-clear-cache-wait-readback.v1");
 A.SaveReloadEvidenceId = TEXT("animation.sequence.retarget-source.launcher-5.7.4-save-distinct-fresh.v1");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimationProvider.SequenceRetargetSourceAssetCore.LateFailureRollback")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimationProvider.SequenceRetargetSourceAsset.Contract"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceRetargetSourceAssetCore.HandlePreflight"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceRetargetSourceAssetCore.SetClearNoop")};
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|Engine+CoreUObject|Editor-issued-Asset|exact-public-representable-preimage|Set-Clear+Update+ClearCookedCache+Cache+wait|retarget_source_asset.set.v2");
 return D;
}

bool IsSequenceRetargetSourceAssetAdmissionShape(const FAssetDocumentPrimitiveDescriptor& D)
{
 const auto E = MakeAnimationSequenceRetargetSourceAssetDescriptor();
 if (D.PrimitiveId != E.PrimitiveId || D.Version != E.Version || D.ProviderId != E.ProviderId || D.Owner != E.Owner
  || D.ExecutionThread != E.ExecutionThread || D.Access != E.Access || D.AllowedPhases != E.AllowedPhases
  || D.AllowedHandleKinds != E.AllowedHandleKinds || D.HandleArgumentSchemas.Num() != E.HandleArgumentSchemas.Num()
  || !D.HandleResultSchemas.IsEmpty() || D.bRequiresActiveTransaction != E.bRequiresActiveTransaction || D.InputSchemaId != E.InputSchemaId
  || D.OutputSchemaId != E.OutputSchemaId || D.MutationFootprintSchemaId != E.MutationFootprintSchemaId
  || D.StableDiagnosticCodes != E.StableDiagnosticCodes || D.RequiredConsumerIds != E.RequiredConsumerIds) return false;
 for (int32 I = 0; I < E.HandleArgumentSchemas.Num(); ++I)
 { const auto& H = D.HandleArgumentSchemas[I]; const auto& EH = E.HandleArgumentSchemas[I];
  if (H.Name != EH.Name || H.Kind != EH.Kind || H.RequiredAccess != EH.RequiredAccess || H.bRequired != EH.bRequired) return false; }
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
