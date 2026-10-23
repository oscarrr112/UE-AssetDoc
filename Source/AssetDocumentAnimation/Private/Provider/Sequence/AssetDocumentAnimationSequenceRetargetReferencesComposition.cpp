// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetReferencesComposition.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceAsset.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceName.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceNameState.h"

namespace AssetDocumentSequenceRetargetReferences
{
namespace Retained = AssetDocumentSequenceRetargetSource;
namespace Named = AssetDocumentSequenceRetargetSourceName;
namespace
{
enum class ESurface : uint8 { Asset, Name };
struct FFrozenTuple
{
 FName Name;
 FSoftObjectPath Asset;
 TArray<FTransform> Pose;
 Named::FEffectiveSourceInput Effective;
 bool Matches(const UAnimSequence& S) const
 {
  return Named::ExactName(Name, S.RetargetSource) && Asset.ToString() == S.GetRetargetSourceAsset().ToSoftObjectPath().ToString()
   && Retained::SamePose(Pose, S.RetargetSourceAssetReferencePose)
   && Retained::SamePose(Effective.Pose, S.GetRetargetTransforms())
   && Named::ExactName(Effective.Source, S.GetRetargetTransformsSourceName());
 }
};
struct FPreparedPair
{
 const FAssetDocumentRuntimeRequestDto Request;
 const FAssetDocumentMutationIntent Steps[2];
 const ESurface Surfaces[2];
 Retained::FIdentity Target;
 Retained::FSkeletonInput Skeleton;
 Named::FRetargetMapInput Map;
 Named::FRetainedAuthorState Author;
 Retained::FReferenceSequenceInput Reference;
 TUniquePtr<Retained::FMeshInput> OldMesh, DesiredMesh;
 TSharedPtr<IAssetDocumentEditorSequenceRetargetSourceNameMutation> Editor;
 FFrozenTuple Tuples[3];
 const FString Operation;
 bool Changed[2] = {}, Written[2] = {};
 FGuid Transaction;
 int32 Next = 0;
 bool bOpened = false, bClosed = false, bInvoking = false;

 FPreparedPair(const FAssetDocumentRuntimeRequestDto& R, const FAssetDocumentMutationIntent& First,
  const FAssetDocumentMutationIntent& Second, UAnimSequence& S, USkeletalMesh* Old, USkeletalMesh* Desired,
  FString Op, TSharedPtr<IAssetDocumentEditorSequenceRetargetSourceNameMutation> E)
  : Request(R), Steps{First, Second},
   Surfaces{First.Requirement.PrimitiveId == Retained::Primitive ? ESurface::Asset : ESurface::Name,
    Second.Requirement.PrimitiveId == Retained::Primitive ? ESurface::Asset : ESurface::Name},
   Target(&S), Skeleton(*S.GetSkeleton(), S.RetargetSource), Map(*S.GetSkeleton()), Author(S), Reference(S.RefPoseSeq.Get()),
   OldMesh(Old ? MakeUnique<Retained::FMeshInput>(*Old, *S.GetSkeleton()) : nullptr),
   DesiredMesh(Desired ? MakeUnique<Retained::FMeshInput>(*Desired, *S.GetSkeleton()) : nullptr), Editor(MoveTemp(E)), Operation(MoveTemp(Op))
 {
  Tuples[0] = {S.RetargetSource, S.GetRetargetSourceAsset().ToSoftObjectPath(), S.RetargetSourceAssetReferencePose,
   {S.GetRetargetTransforms(), S.GetRetargetTransformsSourceName()}};
  TArray<FTransform> DesiredPose;
  if (Desired) FAnimationRuntime::MakeSkeletonRefPoseFromMesh(Desired, S.GetSkeleton(), DesiredPose);
  for (int32 Index = 0; Index < 2; ++Index)
  {
   Tuples[Index + 1] = Tuples[Index];
   if (Surfaces[Index] == ESurface::Name)
   {
    Tuples[Index + 1].Name = Editor->GetDesiredName(); Changed[Index] = Editor->HasChanges();
   }
   else
   {
    Tuples[Index + 1].Asset = FSoftObjectPath(Desired); Tuples[Index + 1].Pose = DesiredPose;
    Changed[Index] = Tuples[0].Asset.ToString() != Tuples[Index + 1].Asset.ToString()
     || !Retained::SamePose(Tuples[0].Pose, DesiredPose);
   }
   Tuples[Index + 1].Effective = Named::FreezeEffectiveSource(S, Tuples[Index + 1].Name, Tuples[Index + 1].Pose);
  }
 }
 UAnimSequence* Sequence() const { return Cast<UAnimSequence>(Target.Object.Get()); }
 USkeletalMesh* Mesh(bool Original) const
 { const auto* M = Original ? OldMesh.Get() : DesiredMesh.Get(); return M ? Cast<USkeletalMesh>(M->Identity.Object.Get()) : nullptr; }
 bool ReadonlyCurrent() const
 {
  if (!Target.Current() || !Retained::CanonicalSequence(Sequence()) || Sequence()->GetSkeleton() != Skeleton.Identity.Object.Get()) return false;
  const auto* S = Cast<USkeleton>(Skeleton.Identity.Object.Get());
  return Skeleton.Current() && Map.Matches(*S) && Author.Matches(*Sequence()) && Reference.Current()
   && (!OldMesh || OldMesh->Current(*S)) && (!DesiredMesh || DesiredMesh->Current(*S));
 }
 bool Completed(int32 Stage) const
 {
  return ReadonlyCurrent() && Tuples[Stage].Matches(*Sequence()) && Retained::CompressedCurrent(*Sequence())
   && !Sequence()->HasResidency(static_cast<const ITargetPlatform*>(nullptr));
 }
 bool Complete(int32 Stage)
 {
  if (!ReadonlyCurrent() || !Tuples[Stage].Matches(*Sequence())) return false;
  auto* S = Sequence(); S->ClearAllCachedCookedPlatformData(); S->CacheDerivedDataForCurrentPlatform(); S->WaitOnExistingCompression();
  return Completed(Stage);
 }
 // During an armed failed write only that step's owned field is recoverable.
 // Its peer must still be the exact frozen pre-step state, after later Core
 // subactions have reversed. Never recapture a drifted peer as a new preimage.
 bool RollbackInputsCurrent(int32 Index) const
 {
  if (!ReadonlyCurrent()) return false;
  const auto* S = Sequence(); const auto& Before = Tuples[Index];
  return Surfaces[Index] == ESurface::Asset ? Named::ExactName(Before.Name, S->RetargetSource)
   : Before.Asset.ToString() == S->GetRetargetSourceAsset().ToSoftObjectPath().ToString()
    && Retained::SamePose(Before.Pose, S->RetargetSourceAssetReferencePose);
 }
 FAssetDocumentContractValidationResult Rollback(int32 Index)
 {
  const TCHAR* Code = Surfaces[Index] == ESurface::Asset ? TEXT("AnimationProvider.RetargetSource.RollbackFailed")
   : TEXT("AnimationProvider.RetargetSourceName.RollbackFailed");
  if (!Retained::ContextValid() || bInvoking) return Retained::Result(false, Code);
  // Core may abort after an opener refused the frozen readonly inputs. No
  // capture and no write means there is no owned effect to undo or poison.
  if (!bOpened) return Retained::Result(!bClosed && !Written[Index], Code);
  if (!bClosed) return Retained::Result(false, Code);
  if (!Written[Index]) return Retained::Result(true);
  if (!RollbackInputsCurrent(Index)) return Retained::Result(false, Code);
  Sequence()->WaitOnExistingCompression();
  if (!RollbackInputsCurrent(Index)) return Retained::Result(false, Code);
  if (Surfaces[Index] == ESurface::Asset) PublishAnimationSequenceRetargetSourceAsset(*Sequence(), Mesh(true));
  else if (!Editor->Restore().bValid) return Retained::Result(false, Code);
  return Retained::Result(Complete(Index), Code);
 }
};
// One non-reentrant, game-thread operation binding. Every closure/action below
// owns the same strong frozen pair across the local preparation lifetime.
FPreparedPair* ActivePair = nullptr;
class FStepInverse final : public IAssetDocumentRollbackAction
{
 TSharedRef<FPreparedPair> State;
 int32 Index;
 bool bConsumed = false;
public:
 FStepInverse(TSharedRef<FPreparedPair> S, int32 I) : State(MoveTemp(S)), Index(I) {}
 FStringView GetRollbackComponentId() const override
 { return State->Surfaces[Index] == ESurface::Asset ? Retained::RollbackId : Named::RollbackId; }
 FAssetDocumentContractValidationResult Rollback(FStringView) override
 {
  if (bConsumed) return Retained::Result(false, TEXT("AnimationProvider.OperationRejected"));
  bConsumed = true; return State->Rollback(Index);
 }
};
template <typename Consumer>
FAssetDocumentContractValidationResult WithIdentities(const FPreparedPair& S, int32 Index,
 const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles, Consumer&& Body)
{
 auto Acquire = [&](const TCHAR* Name, EAssetDocumentHandleAccess Access)
 {
  const auto* A = I.HandleArguments.FindByPredicate([&](const auto& V) { return V.Name == Name; });
  if (!A) return FAssetDocumentVerifiedLeaseResult();
  FAssetDocumentHandleValidationContext C; C.RequestId = I.RequestId; C.TransactionId = I.TransactionId;
  C.RuntimeRevision = I.RuntimeRevision; C.ProviderId = TEXT("editor"); C.Phase = I.Phase;
  C.RequiredKind = EAssetDocumentHandleKind::Asset; C.RequiredAccess = Access;
  return Handles.AcquireVerifiedLease(A->Handle, C);
 };
 auto Target = Acquire(TEXT("sequence"), EAssetDocumentHandleAccess::Mutable);
 if (!Target.Lease) return Retained::Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
 auto OnTarget = [&]()
 {
  return WithAssetDocumentEditorAssetIdentity(*Target.Lease, [&](const auto& V)
  {
   return &V.GetObject() == S.Sequence() && V.GetTypeRevision() == S.Target.Revision && V.GetCanonicalObjectPath() == S.Target.Path
    ? Body() : Retained::Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
  });
 };
 if (S.Surfaces[Index] == ESurface::Name || S.Operation == TEXT("clear")) return OnTarget();
 auto Mesh = Acquire(TEXT("retarget_source_asset"), EAssetDocumentHandleAccess::ReadOnly);
 if (!Mesh.Lease) return Retained::Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
 return WithAssetDocumentEditorAssetIdentity(*Mesh.Lease, [&](const auto& V)
 {
  return &V.GetObject() == S.Mesh(false) && V.GetTypeRevision() == S.DesiredMesh->Identity.Revision
   && V.GetCanonicalObjectPath() == S.DesiredMesh->Identity.Path ? OnTarget() : Retained::Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
 });
}
FAssetDocumentContractValidationResult Validate(const FPreparedPair& S, int32 Index,
 const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles)
{
 const auto& Intent = S.Steps[Index];
 if (!Retained::ContextValid() || ActivePair != &S || !S.bOpened || S.bClosed || S.Next != Index
  || I.PrimitiveId != Intent.Requirement.PrimitiveId || I.Version != Intent.Requirement.Version || I.Phase != Intent.Phase
  || I.RequestId != S.Request.RequestId || I.RuntimeRevision != S.Request.ExpectedRuntimeRevision || I.TransactionId != S.Transaction
  || !S.Completed(Index)) return Retained::Result(false, TEXT("AnimationProvider.OperationRejected"));
 const bool Name = S.Surfaces[Index] == ESurface::Name;
 const auto Descriptor = Name ? MakeAnimationSequenceRetargetSourceNameDescriptor() : MakeAnimationSequenceRetargetSourceAssetDescriptor();
 if (!ValidateAssetDocumentPrimitiveInvocation(I, Descriptor).bValid) return Retained::Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 if (Name) return S.Editor->ValidateInvocation(I, Handles);
 const auto& V = I.ValueArguments;
 if (V.Kind != EAssetDocumentRuntimeValueKind::Object || V.ObjectKeys != TArray<FString>{TEXT("operation")}
  || V.Elements.Num() != 1 || V.Elements[0].Kind != EAssetDocumentRuntimeValueKind::String || V.Elements[0].StringValue != S.Operation
  || I.HandleArguments.Num() != (S.Operation == TEXT("set") ? 2 : 1)) return Retained::Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 return WithIdentities(S, Index, I, Handles, [&] { return Retained::Result(S.Completed(Index)); });
}
FAssetDocumentPrimitiveOutcome Outcome(const FPreparedPair& S, int32 Index)
{
 const bool Name = S.Surfaces[Index] == ESurface::Name;
 FAssetDocumentPrimitiveOutcome O; O.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 O.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 O.Value.ObjectKeys = {TEXT("changed"), Name ? TEXT("retarget_source_name") : TEXT("operation"), TEXT("verified")};
 FAssetDocumentRuntimeValueDto Changed, Value, Verified; Changed.Kind = Verified.Kind = EAssetDocumentRuntimeValueKind::Boolean;
 Changed.BooleanValue = S.Changed[Index]; Verified.BooleanValue = true; Value.Kind = EAssetDocumentRuntimeValueKind::String;
 Value.StringValue = Name ? S.Sequence()->RetargetSource.ToString() : S.Operation; O.Value.Elements = {Changed, Value, Verified};
 if (S.Changed[Index])
 {
  auto& C = O.ChangedSet.AddDefaulted_GetRef(); C.AuthorityId = S.Steps[Index].Requirement.PrimitiveId; C.StableObjectId = S.Target.Path;
  C.Path = Name ? TEXT("/References/RetargetSource") : TEXT("/References/RetargetSourceAsset"); C.ChangeKind = TEXT("Replace");
 }
 return O;
}
}
}

FAssetDocumentContractValidationResult PrepareAnimationSequenceRetargetReferencesBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 using namespace AssetDocumentSequenceRetargetReferences;
 OutBatch = {};
 if (Intents.Num() != 2 || !Retained::ContextValid()) return Retained::Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 const auto* Asset = Intents.FindByPredicate([](const auto& I) { return I.Requirement.PrimitiveId == Retained::Primitive; });
 const auto* Name = Intents.FindByPredicate([](const auto& I) { return I.Requirement.PrimitiveId == Named::Primitive; });
 if (!Asset || !Name || Asset->StepIndex == Name->StepIndex) return Retained::Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 // Reuse the full standalone, pure native admission checks; neither temporary
 // batch opens a capture, mutates, waits or supplies the paired preimage.
 FAssetDocumentPreparedMutationBatch AssetAdmission, NameAdmission;
 auto Valid = PrepareAnimationSequenceRetargetSourceAssetBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(Asset, 1), AssetAdmission);
 if (!Valid.bValid) return Valid;
 Valid = PrepareAnimationSequenceRetargetSourceNameBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(Name, 1), NameAdmission);
 if (!Valid.bValid) return Valid;
 TSharedPtr<IAssetDocumentEditorSequenceRetargetSourceNameMutation> Editor;
 Valid = PrepareAssetDocumentEditorSequenceRetargetSourceNameMutation(Request, *Name, Editor);
 if (!Valid.bValid) return Valid;
 auto* Target = Cast<UAnimSequence>(FSoftObjectPath(Request.TargetAssetPath).ResolveObject());
 USkeletalMesh* Old = nullptr;
 if (!Retained::CanonicalSequence(Target) || !Named::ReadSaveStableMesh(*Target, Old)) return Retained::Result(false);
 const auto& Values = Asset->Intent;
 const FString Operation = Values.Elements[Values.ObjectKeys.IndexOfByKey(TEXT("operation"))].StringValue;
 auto* Desired = Operation == TEXT("set") ? Cast<USkeletalMesh>(FSoftObjectPath(
  Values.Elements[Values.ObjectKeys.IndexOfByKey(TEXT("retarget_source_asset_path"))].StringValue).ResolveObject()) : nullptr;
 if (Operation == TEXT("set") && !Retained::CanonicalMesh(Desired, *Target->GetSkeleton())) return Retained::Result(false);
 auto State = MakeShared<FPreparedPair>(Request, Asset->StepIndex < Name->StepIndex ? *Asset : *Name,
  Asset->StepIndex < Name->StepIndex ? *Name : *Asset, *Target, Old, Desired, Operation, Editor);
 const int32 Bones = Target->GetSkeleton()->GetReferenceSkeleton().GetNum();
 for (const auto& Tuple : State->Tuples) if (!Named::PoseReady(Tuple.Effective.Pose, Bones)) return Retained::Result(false);
 if (!State->Completed(0) || !Editor->IsOriginalCurrent()) return Retained::Result(false);
 FAssetDocumentPreparedMutationBatch Batch;
 Batch.OpenMutationCapture = [State](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!Retained::ContextValid() || !Id.IsValid() || State->bOpened || State->bClosed || ActivePair
   || HasAnimationSequenceRetargetSourceAssetCapture() || HasAnimationSequenceRetargetSourceNameCapture() || !State->Completed(0))
  { Diagnostics.Append(Retained::Result(false).Diagnostics); return {}; }
  const auto Opened = State->Editor->OpenCapture(Id);
  if (!Opened.bValid) { Diagnostics.Append(Opened.Diagnostics); return {}; }
  State->Transaction = Id; State->bOpened = true; ActivePair = &State.Get();
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([State]
  { if (ActivePair == &State.Get()) ActivePair = nullptr; State->Editor->CloseCapture(); State->bClosed = true; });
 };
 for (int32 Index = 0; Index < 2; ++Index)
 {
  auto& C = Batch.Contributions.AddDefaulted_GetRef(); C.StepIndex = State->Steps[Index].StepIndex;
  C.RawFootprint = Retained::Footprint(State->Target.Path, State->Target.PackagePath);
  C.OwnedSurfaceIds = State->Surfaces[Index] == ESurface::Name ? TArray<FString>{State->Target.Path + TEXT("::RetargetSource")}
   : TArray<FString>{State->Target.Path + TEXT("::RetargetSourceAsset"), State->Target.Path + TEXT("::RetargetSourceAssetReferencePose")};
  C.InitialAction = MakeShared<FStepInverse>(State, Index);
  C.ValidateInvocation = [State, Index](const auto& I, auto& Handles) { return Validate(*State, Index, I, Handles); };
 }
 OutBatch = MoveTemp(Batch); return Retained::Result(true);
}

bool HasAnimationSequenceRetargetReferencesCapture()
{ return AssetDocumentSequenceRetargetReferences::ActivePair != nullptr; }

FAssetDocumentPrimitiveOutcome InvokeAnimationSequenceRetargetReferences(
 const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* T)
{
 using namespace AssetDocumentSequenceRetargetReferences;
 auto* State = ActivePair;
 if (!Retained::ContextValid() || !State || State->bInvoking || State->Next >= 2)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Exact paired retarget capture required."));
 TGuardValue<bool> Guard(State->bInvoking, true);
 if (!T || T->GetState() != EAssetDocumentAtomicState::Mutating || T->GetTransactionId() != State->Transaction
  || T->GetMutationToken().RequestId != I.RequestId || T->GetMutationToken().TransactionId != I.TransactionId
  || T->GetMutationToken().RuntimeRevision != I.RuntimeRevision)
  return MakeAnimationProviderFailure(TEXT("AnimationProvider.TransactionUnavailable"), TEXT("Original active paired parent required."));
 const int32 Index = State->Next;
 auto Valid = Validate(*State, Index, I, Handles);
 if (Valid.bValid) Valid = WithIdentities(*State, Index, I, Handles, [&]
 {
  if (!State->Completed(Index)) return Retained::Result(false);
  if (State->Surfaces[Index] == ESurface::Name)
  {
   const auto Applied = State->Editor->Apply(I, Handles, *T); State->Written[Index] = State->Editor->DidWrite();
   if (!Applied.bValid) return Applied;
  }
  else if (State->Changed[Index])
  {
   const auto Enlisted = T->EnlistFootprint(Retained::Footprint(State->Target.Path, State->Target.PackagePath));
   if (!Enlisted.bValid) return Enlisted;
   State->Written[Index] = true; State->Sequence()->WaitOnExistingCompression();
   if (!State->Completed(Index)) return Retained::Result(false, TEXT("AnimationProvider.ReadbackMismatch"));
   PublishAnimationSequenceRetargetSourceAsset(*State->Sequence(), State->Mesh(false));
  }
  return Retained::Result(State->Changed[Index] ? State->Complete(Index + 1) : State->Completed(Index + 1), TEXT("AnimationProvider.CompletionFailed"));
 });
 if (!Valid.bValid)
 { auto F = MakeAnimationProviderFailure(TEXT("AnimationProvider.OperationRejected"), TEXT("Paired retarget publication/completion refused.")); F.Diagnostics = Valid.Diagnostics; return F; }
 ++State->Next; return Outcome(*State, Index);
}
