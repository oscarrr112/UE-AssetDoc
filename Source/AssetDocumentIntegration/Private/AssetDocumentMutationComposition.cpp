// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentMutationComposition.h"
#include "Misc/PackageName.h"

namespace
{
FAssetDocumentContractValidationResult CompositionSuccess() { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
FAssetDocumentContractValidationResult CompositionFailure(const TCHAR* Code, const TCHAR* Message)
{
 FAssetDocumentContractValidationResult R; R.bValid = false;
 auto& D = R.Diagnostics.AddDefaulted_GetRef();
 D.Path = TEXT("/MutationComposition"); D.Code = Code; D.Message = Message;
 return R;
}
bool SameRows(TConstArrayView<FAssetDocumentMutationFootprint> A, TConstArrayView<FAssetDocumentMutationFootprint> B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I)
  if (A[I].Kind != B[I].Kind || A[I].StableId != B[I].StableId || A[I].RollbackComponentId != B[I].RollbackComponentId) return false;
 return true;
}
FAssetDocumentPrimitiveOutcome FailedOutcome(const FAssetDocumentContractValidationResult& R)
{
 FAssetDocumentPrimitiveOutcome O; O.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
 O.Diagnostics = R.Diagnostics; O.Value.Kind = EAssetDocumentRuntimeValueKind::Object; return O;
}
}

struct FAssetDocumentMutationComposition::FState
{
 struct FStep
 {
  FAssetDocumentMutationIntent Intent;
  FString ProviderId;
  FAssetDocumentPreparedMutationContribution Contribution;
 };
 FGuid RequestId, TransactionId;
 uint64 Revision = 0;
 TArray<FStep> Steps;
 TArray<FAssetDocumentMutationFootprint> Footprint;
 TArray<TSharedRef<IAssetDocumentRollbackAction>> FactoryActions;
 TArray<FAssetDocumentCoreMutationCaptureOpener> Openers;
 TSharedPtr<IAssetDocumentPostRollbackFinalizer> Finalizer;
 TArray<int32> Armed;
 int32 Next = 0;
 bool bCreate = false, bOpen = false, bInstalled = false, bRejected = false;
 // SnapshotCoveredMutationV1: no frozen steps; enlistments inside an active
 // invocation are absorbed when they name only the target package.
 bool bSnapshotCovered = false;
 // Existing target whose package Core snapshots (enlisted first, restored last).
 bool bPackageSnapshotRestores = false;
 FString TargetPackageName;
 bool bInvoking = false, bEnlisted = false, bRolledBack = false;

 // The generic property batch's typed preimage only replays each field through the
 // editor setter (PreEditChange/PostEditChange). That can rewrite other values or
 // rebuild subobjects (PaperSprite regenerates CollisionGeometry and its BodySetup),
 // so its readback may never match and would poison a rollback the package
 // snapshot completes anyway (the snapshot is the rollback mechanism).
 // Domain inverses (animation, ...) keep running before the snapshot.
 bool IsRedundantUnderSnapshot(const FStep& Step) const
 {
  return bPackageSnapshotRestores && Step.ProviderId == TEXT("editor")
   && Step.Intent.Requirement.PrimitiveId == TEXT("editor.property.write_admitted_batch.v1");
 }
 FAssetDocumentContractValidationResult Reject(const TCHAR* Code, const TCHAR* Message)
 {
  bRejected = true; return CompositionFailure(Code, Message);
 }
 FAssetDocumentContractValidationResult Complete() const
 {
  if (bRejected || !bInstalled || bInvoking || (!bSnapshotCovered && Next != Steps.Num()))
   return CompositionFailure(TEXT("MutationComposition.Incomplete"), TEXT("Every frozen mutation step must be consumed before persistence."));
  return CompositionSuccess();
 }
 class FAction final : public IAssetDocumentRollbackAction
 {
  TSharedRef<FState> State;
 public:
  explicit FAction(TSharedRef<FState> In) : State(MoveTemp(In)) {}
  FStringView GetRollbackComponentId() const override { return TEXT("rollback.uobject"); }
  FAssetDocumentContractValidationResult Rollback(FStringView Code) override
  {
   if (State->bRolledBack) return CompositionFailure(TEXT("MutationComposition.RollbackConsumed"), TEXT("The composed inverse is single-use."));
   State->bRolledBack = true;
   FAssetDocumentContractValidationResult Result; Result.bValid = true;
   for (int32 I = State->Armed.Num() - 1; I >= 0; --I)
   {
    // Snapshot-only steps have no typed inverse; Core's package snapshot restores them.
    const auto& Step = State->Steps[State->Armed[I]];
    const auto& Action = Step.Contribution.InitialAction;
    if (!Action || State->IsRedundantUnderSnapshot(Step)) continue;
    auto R = Action->Rollback(Code);
    Result.bValid &= R.bValid;
    Result.Diagnostics.Append(R.Diagnostics);
   }
   return Result;
  }
 };
 class FFinalizer final : public IAssetDocumentPostRollbackFinalizer
 {
  TSharedRef<FState> State;
 public:
  explicit FFinalizer(TSharedRef<FState> In) : State(MoveTemp(In)) {}
  FStringView GetRollbackComponentId() const override { return TEXT("rollback.package"); }
  FAssetDocumentContractValidationResult FinalizePostRollback(FStringView Code) override
  {
   return State->Finalizer->FinalizePostRollback(Code);
  }
 };
 // Existing-target package coverage when Core's snapshot restores the dirty flag.
 class FSnapshotPackageFinalizer final : public IAssetDocumentPostRollbackFinalizer
 {
 public:
  FStringView GetRollbackComponentId() const override { return TEXT("rollback.package"); }
  FAssetDocumentContractValidationResult FinalizePostRollback(FStringView) override { return CompositionSuccess(); }
 };
 bool IsActiveStepSnapshotCovered() const
 {
  return bInvoking && (bSnapshotCovered || (Steps.IsValidIndex(Next) && Steps[Next].Contribution.bCoveredByPackageSnapshot));
 }
 // Snapshot-covered requests may touch only the target package (its object,
 // package or factory rows carry the package name; the factory File row its file).
 bool TouchesOnlyTarget(TConstArrayView<FAssetDocumentMutationFootprint> Rows) const
 {
  if (!bSnapshotCovered) return true;
  const FString TargetFile = FPackageName::LongPackageNameToFilename(TargetPackageName, FPackageName::GetAssetPackageExtension());
  for (const auto& Row : Rows)
  {
   // The factory's File row names the target's own package file.
   if (Row.Kind == EAssetDocumentMutationFootprintKind::File && Row.StableId.EndsWith(TEXT("|") + TargetFile, ESearchCase::CaseSensitive))
    continue;
   const int32 At = Row.StableId.Find(TargetPackageName, ESearchCase::CaseSensitive);
   const int32 End = At + TargetPackageName.Len();
   if (At == INDEX_NONE || (End < Row.StableId.Len() && Row.StableId[End] != TEXT('.') && Row.StableId[End] != TEXT('|')
    && Row.StableId[End] != TEXT(':')))
    return false;
  }
  return true;
 }
 class FAdapter final : public IAssetDocumentAtomicTransaction
 {
  TSharedRef<FState> State;
  IAssetDocumentAtomicTransaction& Inner;
 public:
  FAdapter(TSharedRef<FState> In, IAssetDocumentAtomicTransaction& Transaction) : State(MoveTemp(In)), Inner(Transaction) {}
  FGuid GetTransactionId() const override { return Inner.GetTransactionId(); }
  EAssetDocumentAtomicState GetState() const override { return Inner.GetState(); }
  const FAssetDocumentMutationToken& GetMutationToken() const override { return Inner.GetMutationToken(); }
  FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Rows) override
  {
   // A provider without preparation declares its own rows; the frozen object/package
   // rows plus Core's package snapshot already cover them.
   if (!State->bRejected && State->IsActiveStepSnapshotCovered())
   {
    if (!State->TouchesOnlyTarget(Rows))
     return State->Reject(TEXT("MutationComposition.UncoveredResource"), TEXT("A snapshot-covered request may only mutate its target package."));
    State->bEnlisted = true; return CompositionSuccess();
   }
   if (State->bRejected || !State->bInvoking || State->bEnlisted || !State->Steps.IsValidIndex(State->Next)
    || !SameRows(Rows, State->Steps[State->Next].Contribution.RawFootprint))
    return State->Reject(TEXT("MutationComposition.RawFootprintMismatch"), TEXT("Only the exact raw batch of the active validated invocation may be consumed once."));
   State->bEnlisted = true; return CompositionSuccess();
  }
  FAssetDocumentContractValidationResult MarkReadyToCommit() override
  { auto R = State->Complete(); return R.bValid ? Inner.MarkReadyToCommit() : R; }
  FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt& Receipt) override
  { auto R = State->Complete(); return R.bValid ? Inner.Commit(Receipt) : R; }
  FAssetDocumentContractValidationResult Rollback(FStringView Code) override { return Inner.Rollback(Code); }
  FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction>) override
  {
   if (!State->bRejected && State->IsActiveStepSnapshotCovered()) return CompositionSuccess();
   return State->Reject(TEXT("MutationComposition.LateAction"), TEXT("All inverse actions must have been prepared before BeginLive."));
  }
  FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
  {
   if (!State->bRejected && State->IsActiveStepSnapshotCovered()) return CompositionSuccess();
   return State->Reject(TEXT("MutationComposition.LateFinalizer"), TEXT("Only the prepared Editor package finalizer may be installed."));
  }
 };
};

FAssetDocumentMutationComposition::FAssetDocumentMutationComposition(TSharedRef<FState> InState) : State(MoveTemp(InState)) {}
FAssetDocumentMutationComposition::~FAssetDocumentMutationComposition() = default;

TSharedPtr<FAssetDocumentMutationComposition> FAssetDocumentMutationComposition::Freeze(
 const FAssetDocumentRuntimeRequestDto& Request, TArray<FAssetDocumentMutationIntent> Intents,
 TArray<FProviderBatch> Batches, TArray<FAssetDocumentMutationFootprint> FactoryFootprint,
 TArray<TSharedRef<IAssetDocumentRollbackAction>> FactoryActions,
 FAssetDocumentCoreMutationCaptureOpener FactoryOpener, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics,
 bool bPackageSnapshot)
{
 auto S = MakeShared<FState>(); S->RequestId = Request.RequestId; S->Revision = Request.ExpectedRuntimeRevision;
 S->bCreate = !FactoryFootprint.IsEmpty();
 auto Reject = [&](const TCHAR* Message) -> TSharedPtr<FAssetDocumentMutationComposition>
 {
  Diagnostics.Append(CompositionFailure(TEXT("MutationComposition.InvalidPreparation"), Message).Diagnostics); return {};
 };
 const FString ObjectId = TEXT("Object:") + Request.TargetAssetPath;
 const FString PackageId = TEXT("Package:") + FPackageName::ObjectPathToPackageName(Request.TargetAssetPath);
 if (!Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0 || Request.Operation != EAssetDocumentRuntimeOperation::Apply)
  return Reject(TEXT("Preparation requires an exact Apply request identity."));
 S->Steps.SetNum(Intents.Num());
 for (int32 I = 0; I < Intents.Num(); ++I)
 {
  if (Intents[I].StepIndex != I) return Reject(TEXT("Mutation indices must be contiguous."));
  S->Steps[I].Intent = MoveTemp(Intents[I]);
 }
 if (S->bCreate)
 {
  if (FactoryActions.Num() != 1 || !FactoryOpener) return Reject(TEXT("Create requires its unique original factory inverse and capture opener."));
  const FString Component(FactoryActions[0]->GetRollbackComponentId());
  TSet<FString> Seen;
  bool bObject = false, bPackage = false;
  for (auto& Row : FactoryFootprint)
  {
   if (Row.RollbackComponentId != Component || Row.StableId.IsEmpty() || Seen.Contains(Row.StableId))
    return Reject(TEXT("Factory coverage must retain exact unique identities and one original component."));
   Seen.Add(Row.StableId);
   if (Row.Kind == EAssetDocumentMutationFootprintKind::UObjectState) { if (Row.StableId == Component + TEXT("|UObjectState|") + Request.TargetAssetPath) Row.StableId = ObjectId; if (Row.StableId != ObjectId || bObject) return Reject(TEXT("Factory object identity is not canonical.")); bObject = true; }
   if (Row.Kind == EAssetDocumentMutationFootprintKind::Package) { if (Row.StableId == Component + TEXT("|Package|") + FPackageName::ObjectPathToPackageName(Request.TargetAssetPath)) Row.StableId = PackageId; if (Row.StableId != PackageId || bPackage) return Reject(TEXT("Factory package identity is not canonical.")); bPackage = true; }
  }
  if (!bObject || !bPackage) return Reject(TEXT("Factory coverage lacks the exact target object/package."));
  S->Footprint = MoveTemp(FactoryFootprint); S->FactoryActions = MoveTemp(FactoryActions); S->Openers.Add(MoveTemp(FactoryOpener));
 }
 else
 {
  if (!FactoryActions.IsEmpty() || FactoryOpener) return Reject(TEXT("Existing preparation cannot contain factory state."));
  S->Footprint = {{EAssetDocumentMutationFootprintKind::UObjectState, ObjectId, TEXT("rollback.uobject")},
   {EAssetDocumentMutationFootprintKind::Package, PackageId, TEXT("rollback.package")}};
 }
 // With Core's package snapshot an existing target needs no typed inverse.
 const bool bSnapshotCovers = bPackageSnapshot && !S->bCreate;
 // A created target is removed whole by its factory rollback, so steps that
 // populate it need no typed inverse either (covered like a snapshot step).
 const bool bWholeTargetCovered = bSnapshotCovers || S->bCreate;
 TSet<int32> Covered; TSet<FString> Surfaces, Providers;
 bool bEditor = false;
 for (auto& Provider : Batches)
 {
  if (Provider.ProviderId.IsEmpty() || Providers.Contains(Provider.ProviderId)) return Reject(TEXT("Provider batch identities must be unique."));
  Providers.Add(Provider.ProviderId);
  auto& Batch = Provider.Batch;
  if (Provider.ProviderId == TEXT("editor")) bEditor = true;
  if (Batch.PackageFinalizer)
  {
   if (S->bCreate || Provider.ProviderId != TEXT("editor") || S->Finalizer
    || Batch.PackageFinalizer->GetRollbackComponentId() != TEXT("rollback.package"))
    return Reject(TEXT("Only one existing Editor package finalizer is permitted."));
   S->Finalizer = MoveTemp(Batch.PackageFinalizer);
  }
  if (Batch.OpenMutationCapture) S->Openers.Add(MoveTemp(Batch.OpenMutationCapture));
  for (auto& Contribution : Batch.Contributions)
  {
   const int32 Index = Contribution.StepIndex;
   if (!S->Steps.IsValidIndex(Index) || Covered.Contains(Index) || !Contribution.ValidateInvocation
    || (!S->bCreate && !Contribution.InitialAction && !bSnapshotCovers) || (S->bCreate && Contribution.InitialAction)
    || (Contribution.bCoveredByPackageSnapshot && !bWholeTargetCovered))
    return Reject(TEXT("Every step requires exactly one provider validator and the correct initial inverse coverage."));
   if (Contribution.RawFootprint.IsEmpty() != Contribution.OwnedSurfaceIds.IsEmpty())
    return Reject(TEXT("A nonempty authored surface requires physical resource coverage."));
   TSet<FString> RawIds;
   for (const auto& Row : Contribution.RawFootprint)
   {
    if (Row.StableId.IsEmpty() || Row.RollbackComponentId.IsEmpty() || RawIds.Contains(Row.StableId)) return Reject(TEXT("Raw resource identities must be unique and nonempty."));
    FString PhysicalId = Row.StableId;
    if (S->bCreate && Row.Kind == EAssetDocumentMutationFootprintKind::UObjectState && PhysicalId == Row.RollbackComponentId + TEXT("|UObjectState|") + Request.TargetAssetPath) PhysicalId = ObjectId;
    if (S->bCreate && Row.Kind == EAssetDocumentMutationFootprintKind::Package && PhysicalId == Row.RollbackComponentId + TEXT("|Package|") + FPackageName::ObjectPathToPackageName(Request.TargetAssetPath)) PhysicalId = PackageId;
    if (RawIds.Contains(PhysicalId)) return Reject(TEXT("Raw aliases duplicate one physical resource."));
    RawIds.Add(PhysicalId);
    const auto* Physical = S->Footprint.FindByPredicate([&](const auto& R){ return R.StableId == PhysicalId && R.Kind == Row.Kind; });
    const bool bExistingRawObjectOrPackage = (Row.Kind == EAssetDocumentMutationFootprintKind::UObjectState && Row.StableId == ObjectId && Row.RollbackComponentId == TEXT("rollback.uobject"))
     || (Row.Kind == EAssetDocumentMutationFootprintKind::Package && Row.StableId == PackageId && Row.RollbackComponentId == TEXT("rollback.package"));
    if (!Physical || (S->bCreate && Physical->RollbackComponentId != Row.RollbackComponentId && !bExistingRawObjectOrPackage))
     return Reject(TEXT("Provider resources must match the canonical target and preserve factory components."));
    if (!S->bCreate && Row.Kind != EAssetDocumentMutationFootprintKind::UObjectState && Row.Kind != EAssetDocumentMutationFootprintKind::Package)
     return Reject(TEXT("Existing composition supports only the target object and package."));
   }
   if (!Contribution.RawFootprint.IsEmpty() && (!RawIds.Contains(ObjectId) || !RawIds.Contains(PackageId)))
    return Reject(TEXT("Every mutating contribution must cover both its exact object and package."));
   for (const auto& Surface : Contribution.OwnedSurfaceIds)
   {
    // Provider contracts publish canonical physical field ids, never Body paths or aliases.
    if (!Surface.StartsWith(Request.TargetAssetPath + TEXT("::")) || Surface.Len() == Request.TargetAssetPath.Len() + 2 || Surfaces.Contains(Surface))
     return Reject(TEXT("Authored surface identities must be canonical, nonempty, and disjoint across all providers."));
    Surfaces.Add(Surface);
   }
   Covered.Add(Index); S->Steps[Index].ProviderId = Provider.ProviderId;
   S->Steps[Index].Contribution = MoveTemp(Contribution);
  }
 }
 if ((!bEditor && !bSnapshotCovers) || (!S->bCreate && !S->Finalizer && !bSnapshotCovers) || Covered.Num() != S->Steps.Num())
  return Reject(TEXT("Preparation requires Editor package coverage and exactly one contribution for every step."));
 if (!S->bCreate && !S->Finalizer) S->Finalizer = MakeShared<FState::FSnapshotPackageFinalizer>();
 S->bPackageSnapshotRestores = bSnapshotCovers;
 S->Armed.Reserve(S->Steps.Num());
 return MakeShareable(new FAssetDocumentMutationComposition(S));
}

TSharedPtr<FAssetDocumentMutationComposition> FAssetDocumentMutationComposition::FreezeSnapshotCovered(
 const FAssetDocumentRuntimeRequestDto& Request, TArray<FAssetDocumentMutationFootprint> FactoryFootprint,
 TArray<TSharedRef<IAssetDocumentRollbackAction>> FactoryActions, FAssetDocumentCoreMutationCaptureOpener FactoryOpener,
 bool bPackageSnapshot, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
 auto Reject = [&](const TCHAR* Message) -> TSharedPtr<FAssetDocumentMutationComposition>
 {
  Diagnostics.Append(CompositionFailure(TEXT("MutationComposition.InvalidPreparation"), Message).Diagnostics); return {};
 };
 if (!Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0 || Request.Operation != EAssetDocumentRuntimeOperation::Apply)
  return Reject(TEXT("Preparation requires an exact Apply request identity."));
 auto S = MakeShared<FState>(); S->RequestId = Request.RequestId; S->Revision = Request.ExpectedRuntimeRevision;
 S->bSnapshotCovered = true; S->bCreate = !FactoryFootprint.IsEmpty();
 S->TargetPackageName = FPackageName::ObjectPathToPackageName(Request.TargetAssetPath);
 if (S->bCreate)
 {
  if (FactoryActions.Num() != 1 || !FactoryOpener) return Reject(TEXT("Create requires its unique original factory inverse and capture opener."));
  if (!S->TouchesOnlyTarget(FactoryFootprint)) return Reject(TEXT("Factory coverage must name the exact target package."));
  S->Footprint = MoveTemp(FactoryFootprint); S->FactoryActions = MoveTemp(FactoryActions); S->Openers.Add(MoveTemp(FactoryOpener));
  return MakeShareable(new FAssetDocumentMutationComposition(S));
 }
 if (!FactoryActions.IsEmpty() || FactoryOpener) return Reject(TEXT("Existing preparation cannot contain factory state."));
 if (!bPackageSnapshot)
 {
  Diagnostics.Append(CompositionFailure(TEXT("MutationComposition.SnapshotUnavailable"),
   TEXT("A snapshot-covered update requires Core's package snapshot of the existing target (CVar AssetDocument.CorePackageSnapshot and a derived-state rebuilder for its class).")).Diagnostics);
  return {};
 }
 S->Footprint = {{EAssetDocumentMutationFootprintKind::UObjectState, TEXT("Object:") + Request.TargetAssetPath, TEXT("rollback.uobject")},
  {EAssetDocumentMutationFootprintKind::Package, TEXT("Package:") + S->TargetPackageName, TEXT("rollback.package")}};
 S->Finalizer = MakeShared<FState::FSnapshotPackageFinalizer>();
 return MakeShareable(new FAssetDocumentMutationComposition(S));
}

TConstArrayView<FAssetDocumentMutationFootprint> FAssetDocumentMutationComposition::GetFootprint() const { return State->Footprint; }
TArray<TSharedRef<IAssetDocumentRollbackAction>> FAssetDocumentMutationComposition::MakeInitialActions()
{
 if (State->bCreate) return State->FactoryActions;
 return {MakeShared<FState::FAction>(State)};
}
FAssetDocumentCoreMutationCaptureOpener FAssetDocumentMutationComposition::MakeCaptureOpener()
{
 return [S = State](const FGuid& TransactionId, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics) -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (S->bOpen || !TransactionId.IsValid()) { Diagnostics.Append(S->Reject(TEXT("MutationComposition.BindingMismatch"), TEXT("Prepared state can bind one transaction only.")).Diagnostics); return {}; }
  S->bOpen = true; S->TransactionId = TransactionId;
  auto Leases = MakeShared<TArray<TUniquePtr<FAssetDocumentCoreMutationCaptureLease>>>();
  for (auto& Opener : S->Openers)
  {
   auto Lease = Opener(TransactionId, Diagnostics);
   if (!Lease || !Lease->IsValid()) { S->bRejected = true; return {}; }
   Leases->Add(MoveTemp(Lease));
  }
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([Leases]() mutable { Leases->Reset(); });
 };
}
IAssetDocumentAtomicTransaction* FAssetDocumentMutationComposition::BindTransaction(IAssetDocumentAtomicTransaction& Transaction,
 const FAssetDocumentRequestContextMetadata& Metadata, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
 const auto& Token = Transaction.GetMutationToken();
 if (State->bRejected || !State->bOpen || State->TransactionId != Transaction.GetTransactionId()
  || Token.RequestId != State->RequestId || Token.RuntimeRevision != State->Revision
  || Metadata.RequestId != State->RequestId || Metadata.PinnedRuntimeRevision != State->Revision)
 { Diagnostics.Append(State->Reject(TEXT("MutationComposition.BindingMismatch"), TEXT("Live transaction does not match frozen request/revision/capture.")).Diagnostics); return nullptr; }
 if (!State->bInstalled)
 {
  if (State->Finalizer)
  {
   auto R = Transaction.EnlistPostRollbackFinalizer(MakeShared<FState::FFinalizer>(State));
   if (!R.bValid) { State->bRejected = true; Diagnostics.Append(R.Diagnostics); return nullptr; }
  }
  auto R = Transaction.EnlistFootprint(State->Footprint);
  if (!R.bValid) { State->bRejected = true; Diagnostics.Append(R.Diagnostics); return nullptr; }
  State->bInstalled = true; Adapter = MakeUnique<FState::FAdapter>(State, Transaction);
 }
 return Adapter.Get();
}
FAssetDocumentPrimitiveOutcome FAssetDocumentMutationComposition::Invoke(const FString& ProviderId,
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction, TFunctionRef<FAssetDocumentPrimitiveOutcome()> Forward)
{
 if (State->bRejected || !State->bInstalled || State->bInvoking || Transaction != Adapter.Get()
  || Invocation.RequestId != State->RequestId || Invocation.RuntimeRevision != State->Revision
  || Invocation.TransactionId != State->TransactionId || (!State->bSnapshotCovered && !State->Steps.IsValidIndex(State->Next)))
  return FailedOutcome(State->Reject(TEXT("MutationComposition.InvocationMismatch"), TEXT("Mutation invocation is not bound to the next frozen step.")));
 if (State->bSnapshotCovered)
 {
  State->bInvoking = true; State->bEnlisted = false;
  auto Outcome = Forward();
  State->bInvoking = false;
  if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) State->bRejected = true;
  else if (State->bRejected) return FailedOutcome(CompositionFailure(TEXT("MutationComposition.UncoveredResource"), TEXT("A snapshot-covered request may only mutate its target package.")));
  return Outcome;
 }
 auto& Step = State->Steps[State->Next];
 if (ProviderId != Step.ProviderId || Invocation.PrimitiveId != Step.Intent.Requirement.PrimitiveId
  || Invocation.Version != Step.Intent.Requirement.Version || Invocation.Phase != Step.Intent.Phase)
  return FailedOutcome(State->Reject(TEXT("MutationComposition.InvocationMismatch"), TEXT("Provider, primitive, version and phase must match the exact next intent.")));
 auto Validation = Step.Contribution.ValidateInvocation(Invocation, Handles);
 if (!Validation.bValid) { State->bRejected = true; return FailedOutcome(Validation); }
 State->bInvoking = true; State->bEnlisted = false;
 if (!State->bCreate) State->Armed.Add(State->Next);
 auto Outcome = Forward();
 State->bInvoking = false;
 if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) { State->bRejected = true; return Outcome; }
 if (State->bRejected || (!State->bEnlisted && !Step.Contribution.bCoveredByPackageSnapshot && !Outcome.ChangedSet.IsEmpty()))
  return FailedOutcome(State->Reject(TEXT("MutationComposition.UncoveredEffect"), TEXT("A changed outcome requires the exact projected raw enlistment.")));
 ++State->Next;
 return Outcome;
}
FAssetDocumentContractValidationResult FAssetDocumentMutationComposition::ValidateComplete() const { return State->Complete(); }
