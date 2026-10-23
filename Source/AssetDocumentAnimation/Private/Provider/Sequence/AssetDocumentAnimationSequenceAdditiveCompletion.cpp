// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Sequence/AssetDocumentAnimationSequenceAdditiveCompletion.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceAdditiveCompletionState.h"
#include "AssetDocumentEditorSequenceAdditiveMutation.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/AssetUserData.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentSequenceAdditive
{
namespace
{
FAssetDocumentVerifiedLeaseResult AcquireEditorAssetLease(IAssetDocumentHandleResolver& Handles,
 const FAssetDocumentPrimitiveInvocation& I, FStringView Name, EAssetDocumentHandleAccess Access)
{
 const auto* Argument=I.HandleArguments.FindByPredicate([&](const auto& A){return A.Name==Name;});
 if (!Argument) return {};
 // The exact Additive contract consumes Editor-issued Asset capabilities;
 // never rewrite their owner metadata to the consuming Animation provider.
 FAssetDocumentHandleValidationContext C; C.RequestId=I.RequestId; C.TransactionId=I.TransactionId;
 C.RuntimeRevision=I.RuntimeRevision; C.ProviderId=TEXT("editor"); C.Phase=I.Phase;
 C.RequiredKind=EAssetDocumentHandleKind::Asset; C.RequiredAccess=Access;
 return Handles.AcquireVerifiedLease(Argument->Handle,C);
}
// ASSETDOCUMENT_READONLY_AUTHORITY_EXCEPTION(AD.ANIMATION.SEQUENCE-ADDITIVE-EVENT-v1)
// Fixed stock metadata constructs the public event; author values are native.
// No property name supplied by a request and no reflective read/write endpoint.
FByteProperty* FixedAdditiveProperty()
{
 FByteProperty* const Property = FindFProperty<FByteProperty>(UAnimSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimSequence, AdditiveAnimType));
 return Property && Property->GetOwnerClass() == UAnimSequence::StaticClass()
  && Property->ArrayDim == 1 && Property->Enum == StaticEnum<EAdditiveAnimationType>() ? Property : nullptr;
}
bool Complete(UAnimSequence& Target)
{
 FByteProperty* const Property = FixedAdditiveProperty();
 if (!Property) return false;
 FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
 Target.PostEditChangeProperty(Event);
 // Public synchronous wait finishes only this sequence's existing tasks. The
 // stock per-object IsCompiling query is protected, not an admitted API.
 Target.WaitOnExistingCompression();
 return Target.IsCompressedDataValid();
}
template <typename T> bool SameStructs(const TArray<T>& A, const TArray<T>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I)
  if (!T::StaticStruct()->CompareScriptStruct(&A[I], &B[I], 0)) return false;
 return true;
}
bool SameTransforms(const TArray<FTransform>& A, const TArray<FTransform>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I)
  if (A[I].GetTranslation() != B[I].GetTranslation() || A[I].GetRotation() != B[I].GetRotation()
   || A[I].GetScale3D() != B[I].GetScale3D()) return false;
 return true;
}
TArray<FAnimCurveBase> CurveDecorations(const IAnimationDataModel& Model)
{
 TArray<FAnimCurveBase> R;
 for (const auto& C : Model.GetFloatCurves()) R.Add(static_cast<const FAnimCurveBase&>(C));
 for (const auto& C : Model.GetTransformCurves()) R.Add(static_cast<const FAnimCurveBase&>(C));
 return R;
}
// Operation-local observation excludes only this participant's four author
// fields. It never restores retained data, Engine hashes or compression blobs.
struct FRetainedState
{
 TStrongObjectPtr<UObject> ModelObject;
 const IAnimationDataModel* Model;
 FGuid AuthorContentGuid;
 FFrameRate Rate;
 TArray<FName> BoneNames;
 TArray<FAnimCurveBase> Curves;
 TArray<FAnimNotifyEvent> Notifies;
 TArray<FAnimNotifyTrack> Tracks;
 TArray<FAnimSyncMarker> Markers;
 TArray<UObject*> Settings, Metadata, UserData;
 TArray<TStrongObjectPtr<UObject>> Anchors;
 TSoftObjectPtr<USkeletalMesh> RetargetAsset;
 TArray<FTransform> RetargetPose;
 USkeletalMesh* Preview;
 FName RetargetSource;
 float Length, Playback, CompressionError;
 int32 Frames, Keys, CompressionVersion;
 EAnimInterpolationType Interpolation;
 ERootMotionRootLock::Type RootLock;
 EStripAnimDataOnDedicatedServerSettings StripOnServer;
 bool bLoop, bRootMotion, bForceRoot, bNormalizedRoot, bRootFromMontage, bFrameStripping, bKeepCompression;
 static TArray<UObject*> ReadSettings(const UAnimSequence& S)
 { return {S.BoneCompressionSettings.Get(), S.CurveCompressionSettings.Get(), S.VariableFrameStrippingSettings.Get(), S.AssetImportData.Get()}; }
 explicit FRetainedState(const UAnimSequence& S)
  : ModelObject(S.GetDataModelInterface().GetObject()), Model(S.GetDataModel()), AuthorContentGuid(Model->GenerateGuid()),
    Rate(Model->GetFrameRate()), Curves(CurveDecorations(*Model)), Notifies(S.Notifies), Tracks(S.AnimNotifyTracks),
    Markers(S.AuthoredSyncMarkers), Settings(ReadSettings(S)), RetargetAsset(S.GetRetargetSourceAsset()),
    RetargetPose(S.RetargetSourceAssetReferencePose), Preview(S.GetPreviewMesh()), RetargetSource(S.RetargetSource),
    Length(S.GetPlayLength()), Playback(S.RateScale), CompressionError(S.CompressionErrorThresholdScale),
    Frames(Model->GetNumberOfFrames()), Keys(Model->GetNumberOfKeys()), CompressionVersion(S.CompressCommandletVersion),
    Interpolation(S.Interpolation), RootLock(S.RootMotionRootLock), StripOnServer(S.StripAnimDataOnDedicatedServer),
    bLoop(S.bLoop), bRootMotion(S.bEnableRootMotion), bForceRoot(S.bForceRootLock),
    bNormalizedRoot(S.bUseNormalizedRootMotionScale), bRootFromMontage(S.bRootMotionSettingsCopiedFromMontage),
    bFrameStripping(S.bAllowFrameStripping), bKeepCompression(S.bDoNotOverrideCompression)
 {
  Model->GetBoneTrackNames(BoneNames);
  for (auto* O : S.GetMetaData()) Metadata.Add(O);
  if (const auto* Data = S.GetAssetUserDataArray()) for (auto* O : *Data) UserData.Add(O);
  for (const auto* Objects : {&Settings, &Metadata, &UserData}) for (auto* O : *Objects) Anchors.Emplace(O);
  Anchors.Emplace(Preview);
  for (const auto& N : Notifies) { Anchors.Emplace(N.Notify.Get()); Anchors.Emplace(N.NotifyStateClass.Get()); }
 }
 bool Matches(const UAnimSequence& S) const
 {
  const auto* Current = S.GetDataModel();
  if (Current != Model || S.GetDataModelInterface().GetObject() != ModelObject.Get()) return false;
  TArray<FName> Names; Current->GetBoneTrackNames(Names);
  TArray<UObject*> M, D;
  for (auto* O : S.GetMetaData()) M.Add(O);
  if (const auto* Data = S.GetAssetUserDataArray()) for (auto* O : *Data) D.Add(O);
  return Current->GenerateGuid() == AuthorContentGuid && Current->GetFrameRate() == Rate
   && Current->GetNumberOfFrames() == Frames && Current->GetNumberOfKeys() == Keys && Names == BoneNames
   && SameStructs(Curves, CurveDecorations(*Current)) && SameStructs(Notifies, S.Notifies)
   && SameStructs(Tracks, S.AnimNotifyTracks) && Markers == S.AuthoredSyncMarkers && Settings == ReadSettings(S)
   && M == Metadata && D == UserData && Preview == S.GetPreviewMesh() && RetargetAsset == S.GetRetargetSourceAsset()
   && SameTransforms(RetargetPose, S.RetargetSourceAssetReferencePose) && RetargetSource == S.RetargetSource
   && Length == S.GetPlayLength() && Playback == S.RateScale && CompressionError == S.CompressionErrorThresholdScale
   && CompressionVersion == S.CompressCommandletVersion && Interpolation == S.Interpolation && RootLock == S.RootMotionRootLock
   && StripOnServer == S.StripAnimDataOnDedicatedServer && bLoop == S.bLoop && bRootMotion == S.bEnableRootMotion
   && bForceRoot == S.bForceRootLock && bNormalizedRoot == S.bUseNormalizedRootMotionScale
   && bRootFromMontage == S.bRootMotionSettingsCopiedFromMontage && bFrameStripping == S.bAllowFrameStripping
   && bKeepCompression == S.bDoNotOverrideCompression;
 }
};
bool ParseKind(const FAssetDocumentRuntimeValueDto& V, bool bPreparation)
{
 if (!ValidateAssetDocumentRuntimeValue(V).bValid || V.Kind != EAssetDocumentRuntimeValueKind::Object
  || V.ObjectKeys.Num() != (bPreparation ? 2 : 1) || V.Elements.Num() != V.ObjectKeys.Num()) return false;
 const int32 Kind = V.ObjectKeys.IndexOfByKey(TEXT("completion_kind"));
 return V.Elements.IsValidIndex(Kind) && V.Elements[Kind].Kind == EAssetDocumentRuntimeValueKind::String
  && V.Elements[Kind].StringValue == TEXT("additive_root");
}
bool ParseDesired(const FAssetDocumentRuntimeValueDto& Intent, FAuthorState& Desired)
{
 if (!ParseKind(Intent, true)) return false;
 const int32 Index = Intent.ObjectKeys.IndexOfByKey(TEXT("additive"));
 if (!Intent.Elements.IsValidIndex(Index)) return false;
 const auto& V = Intent.Elements[Index];
 // Private prepared projection of the existing four authored names. This is
 // not a runtime primitive value, a new Body/schema, or a region-null default.
 if (V.Kind != EAssetDocumentRuntimeValueKind::Object) return false;
 for (int32 I = 0; I < V.ObjectKeys.Num(); ++I)
 {
  const auto& Name = V.ObjectKeys[I]; const auto& Value = V.Elements[I];
  if (Name == TEXT("AdditiveAnimType"))
  {
   if (Value.Kind != EAssetDocumentRuntimeValueKind::String) return false;
   if (Value.StringValue == TEXT("AAT_None")) Desired.Type = AAT_None;
   else if (Value.StringValue == TEXT("AAT_LocalSpaceBase")) Desired.Type = AAT_LocalSpaceBase;
   else if (Value.StringValue == TEXT("AAT_RotationOffsetMeshSpace")) Desired.Type = AAT_RotationOffsetMeshSpace;
   else return false;
  }
  else if (Name == TEXT("RefPoseType"))
  {
   if (Value.Kind != EAssetDocumentRuntimeValueKind::String) return false;
   if (Value.StringValue == TEXT("ABPT_None")) Desired.Base = ABPT_None;
   else if (Value.StringValue == TEXT("ABPT_RefPose")) Desired.Base = ABPT_RefPose;
   else if (Value.StringValue == TEXT("ABPT_AnimScaled")) Desired.Base = ABPT_AnimScaled;
   else if (Value.StringValue == TEXT("ABPT_AnimFrame")) Desired.Base = ABPT_AnimFrame;
   else if (Value.StringValue == TEXT("ABPT_LocalAnimFrame")) Desired.Base = ABPT_LocalAnimFrame;
   else return false;
  }
  else if (Name == TEXT("RefFrameIndex"))
  {
   if (Value.Kind != EAssetDocumentRuntimeValueKind::Integer || Value.IntegerValue < 0 || Value.IntegerValue > MAX_int32) return false;
   Desired.Frame = int32(Value.IntegerValue);
  }
  else if (Name == TEXT("RefPoseSeq"))
  {
   if (Value.Kind == EAssetDocumentRuntimeValueKind::Null) Desired.Reference = nullptr;
   else
   {
    if (Value.Kind != EAssetDocumentRuntimeValueKind::String || !CanonicalPath(Value.StringValue)) return false;
    UObject* Reference = FSoftObjectPath(Value.StringValue).ResolveObject();
    if (!ExactObject(Reference, UAnimSequence::StaticClass()) || Reference->GetPathName() != Value.StringValue) return false;
    Desired.Reference = static_cast<UAnimSequence*>(Reference);
   }
  }
  else return false;
 }
 return true;
}

struct FSequenceIdentity
{
 TStrongObjectPtr<UAnimSequence> Sequence;
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<USkeleton> Skeleton;
 FString Path, PackagePath, SkeletonPath;
 uint32 Revision = 0;
 FGuid SkeletonGuid;
 explicit FSequenceIdentity(UAnimSequence* S)
  : Sequence(S), Package(S ? S->GetOutermost() : nullptr), Skeleton(S ? S->GetSkeleton() : nullptr),
    Path(S ? S->GetPathName() : FString()), PackagePath(S ? S->GetOutermost()->GetName() : FString()),
    SkeletonPath(S ? S->GetSkeleton()->GetPathName() : FString()), Revision(S ? S->GetUniqueID() : 0),
    SkeletonGuid(S ? S->GetSkeletonGuid() : FGuid()) {}
 bool Current(UAnimSequence* S) const
 {
  return !Sequence.IsValid() ? !S : S == Sequence.Get() && CanonicalSequence(S)
   && S->GetUniqueID() == Revision && S->GetPathName() == Path && S->GetOutermost() == Package.Get()
   && Package->GetName() == PackagePath && S->GetSkeleton() == Skeleton.Get()
   && Skeleton->GetPathName() == SkeletonPath && Skeleton->GetGuid() == SkeletonGuid;
 }
};
struct FReferenceState
{
 FSequenceIdentity Identity;
 FAuthorState Author;
 TUniquePtr<FRetainedState> Retained;
 bool bDirty = false;
 explicit FReferenceState(UAnimSequence* S)
  : Identity(S), Author(S ? FAuthorState::Read(*S) : FAuthorState()),
    Retained(S ? MakeUnique<FRetainedState>(*S) : nullptr), bDirty(S && S->GetOutermost()->IsDirty()) {}
 bool Current() const
 {
  auto* S = Identity.Sequence.Get();
  return Identity.Current(S) && (!S || (S->IsCompressedDataValid() && Author.Matches(*S) && Retained->Matches(*S)
   && S->GetOutermost()->IsDirty() == bDirty));
 }
};
struct FCapture
{
 FGuid Transaction;
 bool bOpened = false, bClosed = false, bWritten = false, bConsumed = false;
};
struct FPreparedState
{
 FSequenceIdentity Target;
 FAuthorState Old, Desired;
 FReferenceState OldReference, DesiredReference;
 FRetainedState Baseline;
 TUniquePtr<FRetainedState> AtWrite;
 TSharedRef<IAssetDocumentEditorSequenceAdditiveMutation> AtomicFields;
 FPreparedState(UAnimSequence& S, const FAuthorState& DesiredState, TSharedRef<IAssetDocumentEditorSequenceAdditiveMutation> Fields)
  : Target(&S), Old(FAuthorState::Read(S)), Desired(DesiredState), OldReference(Old.Reference),
    DesiredReference(Desired.Reference), Baseline(S), AtomicFields(MoveTemp(Fields)) {}
 bool ReferencesCurrent() const { return OldReference.Current() && DesiredReference.Current(); }
 bool BaselineCurrent() const
 {
  auto* S = Target.Sequence.Get();
  return Target.Current(S) && S->IsCompressedDataValid() && Old.Matches(*S) && Baseline.Matches(*S) && ReferencesCurrent();
 }
};
class FAdditiveInverse final : public IAssetDocumentRollbackAction
{
 TSharedRef<FPreparedState> State;
 TSharedRef<FCapture> Capture;
 bool bConsumed = false;
public:
 FAdditiveInverse(TSharedRef<FPreparedState> S, TSharedRef<FCapture> C) : State(MoveTemp(S)), Capture(MoveTemp(C)) {}
 FStringView GetRollbackComponentId() const override { return RollbackId; }
 FAssetDocumentContractValidationResult Rollback(FStringView) override
 {
  if (bConsumed || !ContextValid() || !Capture->bOpened || !Capture->bClosed)
   return Result(false, TEXT("AnimationProvider.SequenceAdditive.RollbackFailed"));
  bConsumed = true;
  if (!State->AtomicFields->DidWrite()) return State->AtomicFields->Restore();
  auto* Target = State->Target.Sequence.Get();
  if (!State->Target.Current(Target) || !State->ReferencesCurrent() || !State->AtWrite)
   return Result(false, TEXT("AnimationProvider.SequenceAdditive.RollbackFailed"));
  Target->WaitOnExistingCompression();
  const auto Restored = State->AtomicFields->Restore();
  if (!Restored.bValid) return Restored;
  const bool bCompleted = Complete(*Target);
  return Result(bCompleted && State->Target.Current(Target) && State->ReferencesCurrent()
   && State->AtomicFields->IsOriginalCurrent() && CompletedAuthorState(*Target, State->Old) && State->AtWrite->Matches(*Target),
   TEXT("AnimationProvider.SequenceAdditive.RollbackFailed"));
 }
};
using FValidator = TFunction<FAssetDocumentContractValidationResult(const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&)>;
struct FActiveCapture { FGuid Transaction; FValidator Validate; TSharedPtr<FPreparedState> State; TSharedPtr<FCapture> Capture; };
// One non-reentrant game-thread prepared lease; no token/request registry.
const FActiveCapture* ActiveCapture = nullptr;
}
}

FAssetDocumentContractValidationResult PrepareAnimationSequenceAdditiveCompletionBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 using namespace AssetDocumentSequenceAdditive;
 OutBatch = FAssetDocumentPreparedMutationBatch();
 if (!ContextValid() || !Request.RequestId.IsValid() || !Request.ExpectedRuntimeRevision
  || Request.Operation != EAssetDocumentRuntimeOperation::Apply || Request.RequestedAssetClassPath != TEXT("/Script/Engine.AnimSequence")
  || !CanonicalPath(Request.TargetAssetPath) || Intents.Num() != 1) return Result(false);
 const auto& Intent = Intents[0];
 if (Intent.StepIndex < 0 || Intent.Requirement.PrimitiveId != Primitive || Intent.Requirement.Version != 2
  || Intent.Requirement.RequiredByRouteId != TEXT("animation_profile") || Intent.InputSchemaId != Schema
  || (Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply && Intent.Phase != EAssetDocumentRuntimeRequestPhase::Repair)) return Result(false);
 auto* Target = Cast<UAnimSequence>(FSoftObjectPath(Request.TargetAssetPath).ResolveObject());
 if (!CanonicalSequence(Target) || Target->GetPathName() != Request.TargetAssetPath || !Target->IsCompressedDataValid() || !FixedAdditiveProperty()) return Result(false);
 const auto Old = FAuthorState::Read(*Target); auto Desired = Old;
 if (!StableAuthorState(*Target, Old)) return Result(false, TEXT("AnimationProvider.SequenceAdditive.UnsupportedPreimage"));
 if (!ParseDesired(Intent.Intent, Desired) || !StableAuthorState(*Target, Desired)) return Result(false);
 const auto& Fields = Intent.Intent.Elements[Intent.Intent.ObjectKeys.IndexOfByKey(TEXT("additive"))];
 if (Fields.ObjectKeys.Contains(TEXT("RefFrameIndex")))
 {
  const auto* Source = Desired.Reference ? Desired.Reference : Target;
  const int32 Sampled = Source->GetNumberOfSampledKeys();
  const int32 Keys = Sampled > 0 ? Sampled : Source->GetDataModel()->GetNumberOfKeys();
  if (Desired.Frame >= Keys) return Result(false);
 }
 TSharedPtr<IAssetDocumentEditorSequenceAdditiveMutation> Atomic;
 const auto Prepared = PrepareAssetDocumentEditorSequenceAdditiveMutation(Request, Intent, Atomic);
 if (!Prepared.bValid || !Atomic) return Prepared;
 if (Atomic->HasChanges())
  if (const auto* Data = Target->GetAssetUserDataArray())
   for (auto* D : *Data) if (D) return Result(false, TEXT("AnimationProvider.SequenceAdditive.CallbackOwnershipUnavailable"));
 auto State = MakeShared<FPreparedState>(*Target, Desired, Atomic.ToSharedRef());
 if (!State->BaselineCurrent()) return Result(false);
 auto Capture = MakeShared<FCapture>();
 FValidator Validator = [State, Capture, Intent, Request](const auto& I, auto& Handles)
 {
  if (!ContextValid() || !Capture->bOpened || Capture->bClosed || Capture->bConsumed
   || I.PrimitiveId != Primitive || I.Version != 2 || I.Phase != Intent.Phase || I.RequestId != Request.RequestId
   || I.RuntimeRevision != Request.ExpectedRuntimeRevision || I.TransactionId != Capture->Transaction
   || !ValidateAssetDocumentPrimitiveInvocation(I, MakeAnimationSequenceAdditiveCompletionDescriptor()).bValid
   || !ParseKind(I.ValueArguments, false) || !State->BaselineCurrent()) return Result(false);
  return State->AtomicFields->ValidateInvocation(I, Handles);
 };
 auto Binding = MakeShared<FActiveCapture>(); Binding->Validate = Validator; Binding->State = State; Binding->Capture = Capture;
 FAssetDocumentPreparedMutationBatch Batch;
 Batch.OpenMutationCapture = [State, Capture, Binding](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics) -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!ContextValid() || !Id.IsValid() || Capture->bOpened || Capture->bClosed || ActiveCapture || !State->BaselineCurrent())
  { Diagnostics.Append(Result(false).Diagnostics); return {}; }
  auto Opened = State->AtomicFields->OpenCapture(Id);
  if (!Opened.bValid) { Diagnostics.Append(Opened.Diagnostics); return {}; }
  Capture->Transaction = Id; Capture->bOpened = true; Binding->Transaction = Id; ActiveCapture = &Binding.Get();
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([Capture, Binding, State]
  { if (ActiveCapture == &Binding.Get()) ActiveCapture = nullptr; State->AtomicFields->CloseCapture(); Capture->bClosed = true; });
 };
 auto& C = Batch.Contributions.AddDefaulted_GetRef(); C.StepIndex = Intent.StepIndex;
 C.OwnedSurfaceIds = {Request.TargetAssetPath + TEXT("::AdditiveAnimType"), Request.TargetAssetPath + TEXT("::RefPoseType"),
  Request.TargetAssetPath + TEXT("::RefFrameIndex"), Request.TargetAssetPath + TEXT("::RefPoseSeq")};
 C.RawFootprint = Footprint(Request.TargetAssetPath, Target->GetOutermost()->GetName());
 C.InitialAction = MakeShared<FAdditiveInverse>(State, Capture);
 C.ValidateInvocation = [Validator](const auto& I, auto& Handles) { return Validator(I, Handles); };
 OutBatch = MoveTemp(Batch); return Result(true);
}

FAssetDocumentPrimitiveOutcome InvokeAnimationSequenceAdditiveCompletion(
 const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* T)
{
 using namespace AssetDocumentSequenceAdditive;
 static bool bInvoking = false;
 if (!ContextValid() || bInvoking || !ActiveCapture || ActiveCapture->Transaction != I.TransactionId)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Exact prepared additive capture required."));
 TGuardValue<bool> Guard(bInvoking, true);
 if (!T || T->GetState() != EAssetDocumentAtomicState::Mutating || T->GetTransactionId() != I.TransactionId
  || T->GetMutationToken().RequestId != I.RequestId || T->GetMutationToken().TransactionId != I.TransactionId
  || T->GetMutationToken().RuntimeRevision != I.RuntimeRevision)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.TransactionUnavailable"), TEXT("Matching active parent transaction required."));
 auto Valid = ActiveCapture->Validate(I, Handles);
 if (!Valid.bValid) { auto F = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Prepared additive validation failed.")); F.Diagnostics = Valid.Diagnostics; return F; }
 auto State = ActiveCapture->State; auto Capture = ActiveCapture->Capture;
 auto* Target = State->Target.Sequence.Get(); const bool bChanged = State->AtomicFields->HasChanges();
 auto ForwardInTargetLease=[&]()
 {
  auto Lease=AcquireEditorAssetLease(Handles,I,TEXT("sequence"),EAssetDocumentHandleAccess::Mutable);
  if (!Lease.Lease) return Result(false,TEXT("AnimationProvider.InvalidHandlePayload"));
  return WithAssetDocumentEditorAssetIdentity(*Lease.Lease,[&](const FAssetDocumentEditorAssetIdentityView& View)
  {
   if (&View.GetObject()!=Target || View.GetTypeRevision()!=Target->GetUniqueID()
    || View.GetCanonicalObjectPath()!=State->Target.Path || !State->BaselineCurrent()) return Result(false);
   State->AtWrite=MakeUnique<FRetainedState>(*Target);
   // The real Editor group enlists the complete inverse before PreEdit/first
   // write. Forward completion stays in this mutable identity callback too.
   auto Applied=State->AtomicFields->Apply(I,Handles,*T); Capture->bConsumed=true;
   if (!Applied.bValid) return Applied;
   if (!State->Target.Current(Target) || !State->ReferencesCurrent() || !State->AtomicFields->IsDesiredCurrent()
    || !StableAuthorState(*Target,State->Desired) || !State->AtWrite->Matches(*Target))
    return Result(false,TEXT("AnimationProvider.ReadbackMismatch"));
   if (bChanged && !Complete(*Target)) return Result(false,TEXT("AnimationProvider.CompletionFailed"));
   return Result(State->Target.Current(Target) && State->ReferencesCurrent() && State->AtomicFields->IsDesiredCurrent()
    && CompletedAuthorState(*Target,State->Desired) && State->AtWrite->Matches(*Target),TEXT("AnimationProvider.ReadbackMismatch"));
  });
 };
 const auto* Reference=I.HandleArguments.FindByPredicate([](const auto& A){return A.Name==TEXT("ref_pose_seq");});
 if (Reference)
 {
  auto Lease=AcquireEditorAssetLease(Handles,I,TEXT("ref_pose_seq"),EAssetDocumentHandleAccess::ReadOnly);
  if (!Lease.Lease) Valid=Result(false,TEXT("AnimationProvider.InvalidHandlePayload"));
  else Valid=WithAssetDocumentEditorAssetIdentity(*Lease.Lease,[&](const FAssetDocumentEditorAssetIdentityView& View)
  {
   return &View.GetObject()==State->Desired.Reference && View.GetTypeRevision()==State->Desired.Reference->GetUniqueID()
    && View.GetCanonicalObjectPath()==State->DesiredReference.Identity.Path && State->ReferencesCurrent()
    ? ForwardInTargetLease() : Result(false,TEXT("AnimationProvider.InvalidHandlePayload"));
  });
 }
 else Valid=ForwardInTargetLease();
 if (!Valid.bValid) { auto F=MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"),TEXT("Prepared atomic group/completion failed.")); F.Diagnostics=Valid.Diagnostics; return F; }
 FAssetDocumentPrimitiveOutcome O; O.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 O.Value.Kind = EAssetDocumentRuntimeValueKind::Object; O.Value.ObjectKeys = {TEXT("changed"), TEXT("completion_kind"), TEXT("verified")};
 FAssetDocumentRuntimeValueDto Changed, Kind, Verified; Changed.Kind = Verified.Kind = EAssetDocumentRuntimeValueKind::Boolean;
 Changed.BooleanValue = bChanged; Verified.BooleanValue = true; Kind.Kind = EAssetDocumentRuntimeValueKind::String; Kind.StringValue = TEXT("additive_root");
 O.Value.Elements = {Changed, Kind, Verified};
 if (bChanged) { auto& C = O.ChangedSet.AddDefaulted_GetRef(); C.AuthorityId = Primitive; C.StableObjectId = State->Target.Path; C.Path = TEXT("/Additive"); C.ChangeKind = TEXT("Replace"); }
 return O;
}

FAssetDocumentPrimitiveDescriptor MakeAnimationSequenceAdditiveCompletionDescriptor()
{
 using namespace AssetDocumentSequenceAdditive;
 FAssetDocumentPrimitiveDescriptor D;
 D.PrimitiveId = Primitive; D.Version = 2; D.ProviderId = TEXT("animation");
 D.Owner = EAssetDocumentPrimitiveOwner::Animation; D.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
 D.Access = EAssetDocumentPrimitiveAccess::Mutating;
 D.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {
  {TEXT("sequence"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, true},
  {TEXT("ref_pose_seq"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, false}};
 D.bRequiresActiveTransaction = true; D.InputSchemaId = Schema;
 D.OutputSchemaId = TEXT("animation.sequence.complete_edit.output.v2");
 D.MutationFootprintSchemaId = TEXT("animation.sequence.complete_edit.footprint.v2");
 D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.StableDiagnosticCodes = {TEXT("AnimationProvider.InvalidValueShape"), TEXT("AnimationProvider.InvalidHandlePayload"),
  TEXT("AnimationProvider.TargetClassMismatch"), TEXT("AnimationProvider.AuthorityUnavailable"),
  TEXT("AnimationProvider.OperationRejected"), TEXT("AnimationProvider.TransactionUnavailable"),
  TEXT("AnimationProvider.CompletionFailed"), TEXT("AnimationProvider.ReadbackMismatch"),
  TEXT("AnimationProvider.SequenceAdditive.InvalidState"), TEXT("AnimationProvider.SequenceAdditive.UnsupportedPreimage"),
  TEXT("AnimationProvider.SequenceAdditive.CallbackOwnershipUnavailable"), TEXT("AnimationProvider.SequenceAdditive.RollbackFailed"),
  TEXT("Editor.SequenceAdditive.Rejected")};
 auto& A = D.Admission;
 A.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
 A.ExactEngineVersion = TEXT("5.7.4"); A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 A.PublicModule = TEXT("Engine;CoreUObject;AssetDocumentEditor");
 A.PublicHeader = TEXT("Animation/AnimSequence.h;UObject/Object.h;UObject/PropertyAccessUtil.h;AssetDocumentEditorSequenceAdditiveMutation.h");
 A.ExportedAuthoritativeApi = TEXT("UObject::PreEditChange;UAnimSequence::PostEditChangeProperty;WaitOnExistingCompression;IsValidAdditive;PropertyAccessUtil::SetPropertyValue_Object;PrepareAssetDocumentEditorSequenceAdditiveMutation");
 A.AuthorityContextId = TEXT("animation.editor-game-thread.active-transaction.v1"); A.PrimaryOperationId = Primitive;
 A.IdentityLifetimeContractId = TEXT("animation.provider-lease.request-transaction-revision.v1");
 A.TransactionContractId = TEXT("animation.core-active-transaction.v1"); A.RollbackContractId = RollbackId;
 A.CompletionContractId = TEXT("animation.sequence.complete_edit.v2.additive.fixed-postedit-readback.v1");
 A.SaveReloadEvidenceId = TEXT("animation.sequence.complete_edit.v2.additive.launcher-5.7.4-save-reload.v1");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimationProvider.SequenceAdditive.EditorAtomic"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceAdditiveCore.LateFailureRollback")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimationProvider.SequenceAdditive.Contract"),
  TEXT("UEAssetDoc.AnimationProvider.SequenceAdditiveCore.HandlePreflight")};
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|Engine+Editor+CoreUObject|exact-four-field-Never-group|fixed-AdditiveAnimType-PostEdit+wait|animation.sequence.complete_edit.v2|additive_root");
 return D;
}

bool IsSequenceAdditiveCompletionAdmissionShape(const FAssetDocumentPrimitiveDescriptor& D)
{
 const auto E = MakeAnimationSequenceAdditiveCompletionDescriptor();
 if (D.PrimitiveId != E.PrimitiveId || D.Version != E.Version || D.ProviderId != E.ProviderId || D.Owner != E.Owner
  || D.ExecutionThread != E.ExecutionThread || D.Access != E.Access || D.AllowedPhases != E.AllowedPhases
  || D.AllowedHandleKinds != E.AllowedHandleKinds || D.HandleArgumentSchemas.Num() != 2 || !D.HandleResultSchemas.IsEmpty()
  || D.bRequiresActiveTransaction != E.bRequiresActiveTransaction || D.InputSchemaId != E.InputSchemaId
  || D.OutputSchemaId != E.OutputSchemaId || D.MutationFootprintSchemaId != E.MutationFootprintSchemaId
  || D.StableDiagnosticCodes != E.StableDiagnosticCodes || D.RequiredConsumerIds != E.RequiredConsumerIds) return false;
 for (int32 I = 0; I < 2; ++I)
 { const auto& H = D.HandleArgumentSchemas[I]; const auto& EH = E.HandleArgumentSchemas[I];
   if (H.Name != EH.Name || H.Kind != EH.Kind || H.RequiredAccess != EH.RequiredAccess || H.bRequired != EH.bRequired) return false; }
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
