// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Montage/MontageNotifyStaging.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Provider/Montage/MontageNotifyArguments.h"
#include "Provider/Montage/MontageStructuralProvider.h"
#include "Provider/Montage/MontageStructuralPreparation.h"
#include "Provider/Montage/MontageManagedNotifyIdentity.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentEditorNativeState.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace AssetDocumentMontage
{
struct FPreparedNotifyStaging
{
 TStrongObjectPtr<UAnimMontage> Target;
 TArray<TStrongObjectPtr<UClass>> Classes;
 TArray<FString> ClassPaths;
 TArray<bool> ClassStates;
 TArray<TStrongObjectPtr<UObject>> Objects;
 FString TargetPath, PackagePath;
 FGuid RequestId, TransactionId;
 uint64 Revision = 0;
 uint32 TargetRevision = 0;
 FAssetDocumentRuntimeValueDto FrozenArguments;
 TArray<FNotifyEdit> Edits;
 TSharedPtr<FPreparedStructuralEdit> Structure;
 TSharedPtr<IAssetDocumentRollbackAction> StructureInverse;
 TArray<FAnimNotifyEvent> OldNotifies;
 TArray<FSlotAnimationTrack> OldSlots;
 TArray<FCompositeSection> OldSections;
 TArray<TStrongObjectPtr<UObject>> Anchors;
 // NotifyTracks (bReplaceTracks): the authored tracks, published before the events; OldTracks is the rollback preimage.
 TArray<FAnimNotifyTrack> OldTracks, DesiredTracks;
 bool bReplaceTracks = false, bTracksPublished = false;
 float OldLength = 0.f;
 int32 OldTrackCount = 0;
 bool bReplacement = false, bPublished = false;
 bool bReplaceNotifies = false, bReplaceStates = false;
 bool BaselineCurrent() const
 {
  if (Structure && !AssetDocumentMontage::BaselineCurrent(*Structure)) return false;
  if (!Current() || Target->GetPlayLength() != OldLength || Target->AnimNotifyTracks.Num() != OldTrackCount
   || Target->Notifies.Num() != OldNotifies.Num() || Target->SlotAnimTracks.Num() != OldSlots.Num()
   || Target->CompositeSections.Num() != OldSections.Num()) return false;
  for (int32 I = 0; I < OldTracks.Num() && OldTracks.Num() == OldTrackCount; ++I)
   if (Target->AnimNotifyTracks[I].TrackName != OldTracks[I].TrackName) return false;
  for (int32 I = 0; I < OldNotifies.Num(); ++I)
   if (!FAnimNotifyEvent::StaticStruct()->CompareScriptStruct(&Target->Notifies[I], &OldNotifies[I], 0)) return false;
  for (int32 I = 0; I < OldSlots.Num(); ++I)
   if (!FSlotAnimationTrack::StaticStruct()->CompareScriptStruct(&Target->SlotAnimTracks[I], &OldSlots[I], 0)) return false;
  for (int32 I = 0; I < OldSections.Num(); ++I)
   if (!FCompositeSection::StaticStruct()->CompareScriptStruct(&Target->CompositeSections[I], &OldSections[I], 0)) return false;
  return true;
 }
 bool bStates = false, bOpened = false, bClosed = false, bConsumed = false, bRolledBack = false;
 bool Current() const
 {
  if (!IsStructuralContext() || !IsValid(Target.Get()) || Target->GetClass() != UAnimMontage::StaticClass()
   || Target->GetPathName() != TargetPath || Target->GetUniqueID() != TargetRevision
   || Target->GetOutermost()->GetName() != PackagePath || Target->ParentAsset || !Target->ChildrenAssets.IsEmpty()
   || ClassPaths.Num() != Classes.Num() || ClassStates.Num() != Classes.Num()
   || (bReplacement && Edits.Num() != Classes.Num())) return false;
  for (int32 I = 0; I < Classes.Num(); ++I)
  {
   auto* Class = Classes[I].Get();
   if (!IsValid(Class) || Class->GetPathName() != ClassPaths[I]
    || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
    || !Class->IsChildOf(ClassStates[I] ? UAnimNotifyState::StaticClass() : UAnimNotify::StaticClass())
    || (ClassStates[I] ? !bReplaceStates : !bReplaceNotifies)
    || (bReplacement && (Edits[I].Class != Class || Edits[I].bState != ClassStates[I]))) return false;
  }
  return true;
 }
 bool OpenFor(const FAssetDocumentMutationToken& Token) const
 {
  return bOpened && !bClosed && TransactionId.IsValid() && Token.RequestId == RequestId
   && Token.TransactionId == TransactionId && Token.RuntimeRevision == Revision;
 }
 TArray<FAssetDocumentMutationFootprint> Footprint() const
 {
  return {{EAssetDocumentMutationFootprintKind::UObjectState, TEXT("Object:") + TargetPath, TEXT("rollback.uobject")},
   {EAssetDocumentMutationFootprintKind::Package, TEXT("Package:") + PackagePath, TEXT("rollback.package")}};
 }
};
TSharedPtr<const FPreparedStructuralEdit> GetMontageNotifyPreparedStructure(const FPreparedNotifyStaging& State)
{ return State.Structure; }

namespace
{
constexpr const TCHAR* NotifyPrefix = TEXT("AssetDocumentManaged_Notify_");
constexpr const TCHAR* StatePrefix = TEXT("AssetDocumentManaged_NotifyState_");
class FNotifyStagingInverse final : public IAssetDocumentRollbackAction
{
 TSharedRef<FPreparedNotifyStaging> State;
public:
 explicit FNotifyStagingInverse(TSharedRef<FPreparedNotifyStaging> In) : State(MoveTemp(In)) {}
 FStringView GetRollbackComponentId() const override { return TEXT("animation.montage.notify-staging.rollback.v1"); }
 FAssetDocumentContractValidationResult Rollback(FStringView) override
 {
  if (State->bRolledBack || !State->bOpened || !State->bClosed || !State->Current())
   return Result(false, TEXT("AnimationProvider.Montage.NotifyStagingRollbackFailed"));
  State->bRolledBack = true;
  // Authored notify tracks go back first, so the restored events' TrackIndex values name their old tracks.
  const bool bRestoreTracks = State->bTracksPublished;
  if (bRestoreTracks) { State->Target->AnimNotifyTracks = State->OldTracks; State->bTracksPublished = false; }
  // A composite restores the old mixed timeline before retiring its actual allocation set.
  if (State->StructureInverse && State->bConsumed)
  {
   auto Restored = State->StructureInverse->Rollback(TEXT("montage-combined-notify"));
   if (!Restored.bValid) return Restored;
  }
  else if (State->bReplacement && State->bConsumed)
  {
   State->Target->Notifies = State->OldNotifies; // rollback-only typed preimage restoration
   auto Restored = CompleteMontageStructure(*State->Target);
   if (!Restored.bValid) return Restored;
  }
  if (bRestoreTracks)
  {
   // The track caches (FAnimNotifyTrack::Notifies) follow the restored tracks.
   auto Restored = CompleteMontageStructure(*State->Target);
   if (!Restored.bValid) return Restored;
  }
  // Standalone staging still requires any separate publication to have been undone first.
  for (const auto& Object : State->Objects)
  {
   if (!IsValid(Object.Get()) || Object->GetOuter() != State->Target.Get()) return Result(false);
   for (const auto& Event : State->Target->Notifies)
    if (Event.Notify == Object.Get() || Event.NotifyStateClass == Object.Get())
     return Result(false, TEXT("AnimationProvider.Montage.NotifyStillPublished"));
  }
  for (auto& Object : State->Objects) Object->MarkAsGarbage();
  State->Objects.Reset();
  return Result(true);
 }
};
UObject* StageMontageNotifyObject(FPreparedNotifyStaging& State, UClass& Class)
{
 // AD.ANIMATION.MONTAGE.NOTIFY-STAGING.v1: sole allocation site; exact family validated twice.
 const bool bState = Class.IsChildOf(UAnimNotifyState::StaticClass());
 if (!Class.IsChildOf(bState ? UAnimNotifyState::StaticClass() : UAnimNotify::StaticClass())
  || (bState ? !State.bReplaceStates : !State.bReplaceNotifies)) return nullptr;
 const FName Name = MakeUniqueObjectName(State.Target.Get(), &Class, bState ? StatePrefix : NotifyPrefix);
 return NewObject<UObject>(State.Target.Get(), &Class, Name, RF_Transactional);
}
FAssetDocumentContractValidationResult StageImpl(TSharedRef<FPreparedNotifyStaging> State,
 IAssetDocumentAtomicTransaction& Transaction, TArray<UObject*>& OutObjects, int32 FailAfterObject)
{
 OutObjects.Reset();
 if (!State->Current() || State->bConsumed || Transaction.GetState() != EAssetDocumentAtomicState::Mutating
  || !State->OpenFor(Transaction.GetMutationToken()) || Transaction.GetTransactionId() != State->TransactionId)
  return Result(false, TEXT("AnimationProvider.Montage.InvalidContext"));
 State->bConsumed = true;

 for (int32 I = 0; I < State->Classes.Num(); ++I)
 {
  UObject* Object = StageMontageNotifyObject(*State, *State->Classes[I].Get());
  if (!Object) return Result(false, TEXT("AnimationProvider.Montage.NotifyStagingFailed"));
  State->Objects.Emplace(Object);
  if (I == FailAfterObject) return Result(false, TEXT("AnimationProvider.Montage.InjectedFailure"));

 }
 // Return only the complete staged set; safe properties and event publication are separate owned stages.
 for (const auto& Object : State->Objects) OutObjects.Add(Object.Get());
 return Result(true);
}
}
static FAssetDocumentContractValidationResult PrepareMontageNotifyStagingImpl(UAnimMontage& Target,
 TConstArrayView<UClass*> Classes, bool bStates, const FAssetDocumentRuntimeRequestDto& Request, int32 StepIndex,
 FAssetDocumentPreparedMutationBatch& OutBatch, TSharedPtr<FPreparedNotifyStaging>& OutState,
 bool bReplaceNotifies, bool bReplaceStates)
{
 OutBatch = FAssetDocumentPreparedMutationBatch(); OutState.Reset();
 if (!IsStructuralContext() || !IsValid(&Target) || Target.GetClass() != UAnimMontage::StaticClass()
  || Target.IsTemplate() || Target.HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad)
  || !Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0 || StepIndex < 0
  || Request.Operation != EAssetDocumentRuntimeOperation::Apply || Request.TargetAssetPath != Target.GetPathName()
  || Request.RequestedAssetClassPath != TEXT("/Script/Engine.AnimMontage")) return Result(false);
 auto State = MakeShared<FPreparedNotifyStaging>(); State->Target.Reset(&Target);
 State->TargetPath = Target.GetPathName(); State->PackagePath = Target.GetOutermost()->GetName();
 State->TargetRevision = Target.GetUniqueID(); State->RequestId = Request.RequestId; State->Revision = Request.ExpectedRuntimeRevision;
 State->bStates = bStates; State->bReplaceNotifies = bReplaceNotifies; State->bReplaceStates = bReplaceStates;
 for (auto* Class : Classes)
 {
  if (!IsValid(Class) || Class->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad)) return Result(false);
  State->Classes.Emplace(Class); State->ClassPaths.Add(Class->GetPathName());
  State->ClassStates.Add(Class->IsChildOf(UAnimNotifyState::StaticClass()));
 }
 if (!State->Current()) return Result(false);
 OutBatch.OpenMutationCapture = [State](const FGuid& TransactionId, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!TransactionId.IsValid() || State->bOpened || State->bClosed || !State->Current())
  { Diagnostics.Append(Result(false, TEXT("AnimationProvider.Montage.PreparedStateDrift")).Diagnostics); return {}; }
  State->TransactionId = TransactionId; State->bOpened = true;
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([State]() { State->bClosed = true; });
 };
 FAssetDocumentPreparedMutationContribution Contribution; Contribution.StepIndex = StepIndex;
 Contribution.RawFootprint = State->Footprint(); Contribution.OwnedSurfaceIds = {State->TargetPath + TEXT("::NotifyStaging")};
 Contribution.InitialAction = MakeShared<FNotifyStagingInverse>(State);
 Contribution.ValidateInvocation = [State](const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles)
 {
  FAssetDocumentMutationToken Token; Token.RequestId = Invocation.RequestId; Token.TransactionId = Invocation.TransactionId; Token.RuntimeRevision = Invocation.RuntimeRevision;
  const FString Primitive = State->bStates ? TEXT("animation.montage.notify_states.replace.v1") : TEXT("animation.montage.notifies.replace.v1");
  if (!State->Current() || !State->OpenFor(Token) || State->bConsumed || Invocation.PrimitiveId != Primitive || Invocation.Version != 1
   || Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Apply || Invocation.HandleArguments.Num() != 1
   || !ValidateAssetDocumentRuntimeValue(Invocation.ValueArguments).bValid
   || Invocation.ValueArguments.Kind != EAssetDocumentRuntimeValueKind::Array || Invocation.ValueArguments.Elements.Num() != State->ClassPaths.Num()) return Result(false);
  for (int32 I = 0; I < State->ClassPaths.Num(); ++I)
   if (Invocation.ValueArguments.Elements[I].Kind != EAssetDocumentRuntimeValueKind::String
    || Invocation.ValueArguments.Elements[I].StringValue != State->ClassPaths[I]) return Result(false);
  auto Lease = AcquireAnimationInputLease(Handles, Invocation, TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
  if (!Lease.Lease) return Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
  return WithAssetDocumentEditorAssetIdentity(*Lease.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
  { return Result(&View.GetObject() == State->Target.Get() && View.GetCanonicalObjectPath() == State->TargetPath && View.GetTypeRevision() == State->TargetRevision); });
 };
 OutBatch.Contributions.Add(MoveTemp(Contribution)); OutState = State;
 return Result(true);
}
FAssetDocumentContractValidationResult PrepareMontageNotifyStaging(UAnimMontage& Target,
 TConstArrayView<UClass*> Classes, bool bStates, const FAssetDocumentRuntimeRequestDto& Request, int32 StepIndex,
 FAssetDocumentPreparedMutationBatch& OutBatch, TSharedPtr<FPreparedNotifyStaging>& OutState)
{
 return PrepareMontageNotifyStagingImpl(Target, Classes, bStates, Request, StepIndex, OutBatch, OutState, !bStates, bStates);
}
FAssetDocumentContractValidationResult StageMontageNotifyObjects(TSharedRef<FPreparedNotifyStaging> State,
 const IAssetDocumentVerifiedHandleLease& TargetLease, IAssetDocumentAtomicTransaction& Transaction, TArray<UObject*>& OutObjects)
{
 OutObjects.Reset(); const auto& H = TargetLease.GetHandle();
 if (H.Kind != EAssetDocumentHandleKind::Asset || H.Access != EAssetDocumentHandleAccess::Mutable
  || H.RequestId != State->RequestId || H.TransactionId != Transaction.GetTransactionId() || H.RuntimeRevision != State->Revision) return Result(false);
 auto Valid = WithAssetDocumentEditorAssetIdentity(TargetLease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 { return Result(&View.GetObject() == State->Target.Get() && View.GetCanonicalObjectPath() == State->TargetPath && View.GetTypeRevision() == State->TargetRevision); });
 if (!Valid.bValid) return Valid;
 if (!State->Current() || State->bConsumed || Transaction.GetState() != EAssetDocumentAtomicState::Mutating
  || !State->OpenFor(Transaction.GetMutationToken())) return Result(false, TEXT("AnimationProvider.Montage.InvalidContext"));
 if (!State->Classes.IsEmpty()) { Valid = Transaction.EnlistFootprint(State->Footprint()); if (!Valid.bValid) return Valid; }
 return StageImpl(State, Transaction, OutObjects, INDEX_NONE);
}
#if WITH_DEV_AUTOMATION_TESTS
FAssetDocumentContractValidationResult Testing::StageNative(TSharedRef<FPreparedNotifyStaging> State,
 IAssetDocumentAtomicTransaction& Transaction, TArray<UObject*>& OutObjects, int32 FailAfterObject)
{
 if (!State->Current() || State->bConsumed || Transaction.GetState() != EAssetDocumentAtomicState::Mutating
  || !State->OpenFor(Transaction.GetMutationToken())) return Result(false, TEXT("AnimationProvider.Montage.InvalidContext"));
 if (!State->Classes.IsEmpty())
 { auto Valid = Transaction.EnlistFootprint(State->Footprint()); if (!Valid.bValid) return Valid; }
 return StageImpl(State, Transaction, OutObjects, FailAfterObject);
}
#endif
}

namespace AssetDocumentMontage
{
namespace
{
bool EqualNotifyArguments(const FAssetDocumentRuntimeValueDto& A, const FAssetDocumentRuntimeValueDto& B)
{
 if (A.Kind != B.Kind || A.BooleanValue != B.BooleanValue || A.IntegerValue != B.IntegerValue
  || A.NumberValue != B.NumberValue || A.StringValue != B.StringValue || A.BytesValue != B.BytesValue
  || A.ObjectKeys != B.ObjectKeys || A.Elements.Num() != B.Elements.Num()) return false;
 for (int32 I = 0; I < A.Elements.Num(); ++I)
  if (!EqualNotifyArguments(A.Elements[I], B.Elements[I])) return false;
 return true;
}
bool IsReplacedFamily(const FAnimNotifyEvent& Event, const FPreparedNotifyStaging& State)
{
 return (State.bReplaceNotifies && IsManagedMontageNotifyEvent(*State.Target, Event, false))
  || (State.bReplaceStates && IsManagedMontageNotifyEvent(*State.Target, Event, true));
}
FAssetDocumentContractValidationResult ValidateNotifyInvocation(const FPreparedNotifyStaging& State,
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles)
{
 FAssetDocumentMutationToken Token;
 Token.RequestId = Invocation.RequestId; Token.TransactionId = Invocation.TransactionId; Token.RuntimeRevision = Invocation.RuntimeRevision;
 if (!State.bReplacement || !State.BaselineCurrent() || !State.OpenFor(Token) || State.bConsumed
  || Invocation.Version != 1 || Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Apply
  || Invocation.PrimitiveId != (State.bStates ? TEXT("animation.montage.notify_states.replace.v1") : TEXT("animation.montage.notifies.replace.v1"))
  || Invocation.HandleArguments.Num() != 1 || Invocation.HandleArguments[0].Name != TEXT("asset")
  || !ValidateAssetDocumentRuntimeValue(Invocation.ValueArguments).bValid
  || !EqualNotifyArguments(Invocation.ValueArguments, State.FrozenArguments))
  return Result(false, TEXT("AnimationProvider.Montage.PreparedStateDrift"));
 auto Acquired = AcquireAnimationInputLease(Handles, Invocation, TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
 if (!Acquired.Lease) return Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
 return WithAssetDocumentEditorAssetIdentity(*Acquired.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 { return Result(&View.GetObject() == State.Target.Get() && View.GetCanonicalObjectPath() == State.TargetPath
   && View.GetTypeRevision() == State.TargetRevision); });
}
FAssetDocumentPrimitiveOutcome NotifyFailure(const FAssetDocumentContractValidationResult& Valid)
{
 auto Out = MakeAnimationProviderFailure(TEXT("AnimationProvider.Montage.InvalidIntent"), TEXT("The prepared montage notify replacement failed."));
 if (!Valid.Diagnostics.IsEmpty()) Out.Diagnostics = Valid.Diagnostics;
 return Out;
}
}
FAssetDocumentContractValidationResult ParseMontageNotifyTrackNames(const FAssetDocumentRuntimeValueDto& Value, TArray<FName>& OutNames)
{
 // Names are labels (the editor's Rename Notify Track takes any text); TrackIndex refers to a track by position.
 OutNames.Reset();
 if (Value.Kind != EAssetDocumentRuntimeValueKind::Array) return Result(false);
 for (const auto& Row : Value.Elements)
 {
  if (Row.Kind != EAssetDocumentRuntimeValueKind::String) { OutNames.Reset(); return Result(false); }
  OutNames.Add(FName(*Row.StringValue));
 }
 if (OutNames.IsEmpty())
 {
  auto Failed = Result(false, TEXT("AnimationProvider.Montage.InvalidIntent"),
   TEXT("List at least one notify track: the editor always keeps one (a Montage without tracks gets track \"1\")."));
  Failed.Diagnostics[0].Path = TEXT("/Body/NotifyTracks");
  return Failed;
 }
 return Result(true);
}
namespace
{
FAssetDocumentContractValidationResult MontageNotifyPlanFailure(const TCHAR* Code, const FString& Path, const FString& Message)
{
 auto Failed = Result(false, Code, Message);
 Failed.Diagnostics[0].Path = Path;
 return Failed;
}
bool MontageNotifyPlanReplaces(const UAnimMontage& Target, const FAnimNotifyEvent& Event, bool bReplaceNotifies, bool bReplaceStates)
{
 return (bReplaceNotifies && IsManagedMontageNotifyEvent(Target, Event, false))
  || (bReplaceStates && IsManagedMontageNotifyEvent(Target, Event, true));
}
}
FAssetDocumentContractValidationResult CheckMontageNotifyPlan(const UAnimMontage& Target, TConstArrayView<FNotifyEdit> Edits,
 const FPreparedStructuralEdit* Structure, int32 TrackCount, bool bTracksAuthored, bool bReplaceNotifies, bool bReplaceStates)
{
 const float DesiredLength = Structure ? Structure->DesiredLength : Target.GetPlayLength();
 // A new (zero-length) Montage gets its length from the frozen structure that
 // travels with the notify replacement; without one the current length must be positive.
 const TCHAR* Family = bReplaceNotifies ? TEXT("Notifies") : TEXT("NotifyStates");
 if (Target.SlotAnimTracks.IsEmpty())
  return Result(false, TEXT("AnimationProvider.Montage.NotifyNeedsSlot"),
   FString::Printf(TEXT("%s need a Montage with a slot track: list SlotAnimTracks with a segment."), Family));
 if (!FMath::IsFinite(DesiredLength) || DesiredLength <= 0.f
  || !FMath::IsFinite(Target.GetPlayLength()) || (!Structure && Target.GetPlayLength() <= 0.f))
  return Result(false, TEXT("AnimationProvider.Montage.NotifyNeedsLength"),
   FString::Printf(TEXT("%s need a Montage with a positive length (%g s): its SlotAnimTracks segments give the length."), Family, DesiredLength));
 int32 FamilyIndex[2] = {0, 0};
 for (const auto& Edit : Edits)
 {
  const TCHAR* Label = Edit.bState ? TEXT("NotifyStates") : TEXT("Notifies");
  const int32 Index = FamilyIndex[Edit.bState ? 1 : 0]++;
  if (Edit.TrackIndex < 0 || Edit.TrackIndex >= TrackCount)
   return MontageNotifyPlanFailure(TEXT("AnimationProvider.Montage.NotifyTrackMissing"),
    FString::Printf(TEXT("/Body/%s/%d/TrackIndex"), Label, Index),
    FString::Printf(TEXT("%s[%d] TrackIndex %d does not exist: the Montage has %d notify track(s) (0..%d)%s."),
     Label, Index, Edit.TrackIndex, TrackCount, TrackCount - 1,
     bTracksAuthored ? TEXT(", as NotifyTracks lists them") : TEXT("; add tracks with NotifyTracks")));
  if (Edit.Time > DesiredLength || (Edit.bState && Edit.Time + Edit.Duration > DesiredLength))
   return MontageNotifyPlanFailure(TEXT("AnimationProvider.Montage.NotifyBeyondLength"),
    FString::Printf(TEXT("/Body/%s/%d/Time"), Label, Index),
    FString::Printf(TEXT("%s[%d] at %g s%s ends after the Montage length %g s."), Label, Index, Edit.Time,
     Edit.bState ? *FString::Printf(TEXT(" + %g s"), Edit.Duration) : TEXT(""), DesiredLength));
 }
 // Stock reload relinks endpoints to slot zero. Reject source links that cannot round-trip.
 for (const auto& Event : Target.Notifies)
  if (Event.GetSlotIndex() != 0 || !Target.AnimNotifyTracks.IsValidIndex(Event.TrackIndex)
   || !FMath::IsFinite(Event.GetTime()) || Event.GetTime() < 0.f || Event.GetTime() > Target.GetPlayLength()
   || (Event.NotifyStateClass && (Event.EndLink.GetSlotIndex() != 0 || !FMath::IsFinite(Event.GetDuration())
    || Event.GetDuration() <= 0.f || Event.EndLink.GetTime() > Target.GetPlayLength()))) return Result(false);
 // Both forward completion and rollback must preserve the admitted baseline offsets.
 for (const auto& Event : Target.Notifies)
 {
  auto Predicted = Event;
  Predicted.RefreshTriggerOffset(Target.CalculateOffsetForNotify(Event.GetTime()));
  Predicted.RefreshEndTriggerOffset(Target.CalculateOffsetForNotify(Event.EndLink.GetTime()));
  if (Predicted.TriggerTimeOffset != Event.TriggerTimeOffset || Predicted.EndTriggerTimeOffset != Event.EndTriggerTimeOffset)
   return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), TEXT("Stock completion would normalize existing trigger offsets."));
 }
 // Retained notifies (a family the document omits, or notifies not made by AssetDocument) keep their TrackIndex.
 TArray<TPair<float, int32>> Placements;
 TArray<FAnimNotifyEvent> Retained;
 if (Structure)
 {
  Retained = Structure->DesiredNotifies;
  for (int32 I = 0; I < Structure->DesiredNotifies.Num(); ++I)
   Placements.Emplace(Structure->NotifyStarts[I].Time, Structure->DesiredNotifies[I].TrackIndex);
 }
 for (const auto& Old : Target.Notifies)
 {
  if (!Structure && !MontageNotifyPlanReplaces(Target, Old, bReplaceNotifies, bReplaceStates))
  {
   Retained.Add(Old);
   Placements.Emplace(Old.GetTime(), Old.TrackIndex);
  }
  // The original timeline must also be restorable without stock track relocation.
  for (const auto& Other : Target.Notifies)
   if (&Old != &Other && Old.TrackIndex == Other.TrackIndex && FMath::IsNearlyEqual(Old.GetTime(), Other.GetTime()))
    return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), FString::Printf(
     TEXT("The Montage has two notifies starting at Time %g on notify track %d; it cannot be edited here until one "
      "of them is moved in the editor."), Old.GetTime(), Old.TrackIndex));
 }
 for (const auto& Event : Retained)
  if (Event.TrackIndex >= TrackCount)
   return MontageNotifyPlanFailure(TEXT("AnimationProvider.Montage.NotifyTrackMissing"), TEXT("/Body/NotifyTracks"),
    FString::Printf(TEXT("The Montage keeps a %s at %g s on notify track %d that this document does not replace, but NotifyTracks "
     "lists %d track(s): keep that track, or list the notify's family (%s) in the same document."),
     Event.NotifyStateClass ? TEXT("notify state") : TEXT("notify"), Event.GetTime(), Event.TrackIndex, TrackCount,
     Event.NotifyStateClass ? TEXT("NotifyStates") : TEXT("Notifies")));
 for (const auto& Edit : Edits) Placements.Emplace(Edit.Time, Edit.TrackIndex);
 for (int32 I = 0; I < Placements.Num(); ++I)
  for (int32 J = 0; J < I; ++J)
   if (Placements[I].Value == Placements[J].Value && FMath::IsNearlyEqual(Placements[I].Key, Placements[J].Key))
    return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"), FString::Printf(
     TEXT("Two notifies start at Time %g on notify track %d; the editor keeps one notify start per track and time "
      "(it would move one to another track). Give one of them another TrackIndex or Time."),
     Placements[I].Key, Placements[I].Value));
 return Result(true);
}
FAssetDocumentContractValidationResult PrepareMontageNotifyReplacement(UAnimMontage& Target,
 const FAssetDocumentRuntimeValueDto& FrozenArguments, bool bStates,
 const FAssetDocumentRuntimeRequestDto& Request, int32 StepIndex,
 FAssetDocumentPreparedMutationBatch& OutBatch, TSharedPtr<FPreparedNotifyStaging>& OutState)
{
 OutBatch = {}; OutState.Reset();
 auto NotifyArguments = FrozenArguments;
 TSharedPtr<FPreparedStructuralEdit> Structure;
 const bool bStructure = NotifyArguments.Kind == EAssetDocumentRuntimeValueKind::Object
  && !NotifyArguments.ObjectKeys.IsEmpty() && NotifyArguments.ObjectKeys.Last() == TEXT("structure");
 FAssetDocumentRuntimeValueDto StructureArguments;
 if (bStructure)
 {
  if (NotifyArguments.Elements.Num() != NotifyArguments.ObjectKeys.Num()) return Result(false);
  StructureArguments = NotifyArguments.Elements.Pop(); NotifyArguments.ObjectKeys.Pop();
 }
 // NotifyTracks (notify_tracks, after the families): the whole track list, published with the events.
 TOptional<TArray<FName>> TrackNames;
 if (NotifyArguments.Kind == EAssetDocumentRuntimeValueKind::Object && !NotifyArguments.ObjectKeys.IsEmpty()
  && NotifyArguments.ObjectKeys.Last() == TEXT("notify_tracks"))
 {
  if (NotifyArguments.Elements.Num() != NotifyArguments.ObjectKeys.Num()) return Result(false);
  auto Valid = ParseMontageNotifyTrackNames(NotifyArguments.Elements.Last(), TrackNames.Emplace());
  if (!Valid.bValid) return Valid;
  NotifyArguments.Elements.Pop(); NotifyArguments.ObjectKeys.Pop();
 }
 TArray<FNotifyEdit> Edits;
 auto Valid = ParseMontageNotifyArguments(NotifyArguments, bStates, Edits);
 if (!Valid.bValid) return Valid;
 // A Montage made by this Create has no notify track yet; the editor adds track "1" when the asset
 // loads or its timeline is first edited (UAnimSequenceBase::InitializeNotifyTrack). Do the same, so a
 // one-shot Create with Notifies places them on TrackIndex 0 as Create-then-Update does.
 if (!TrackNames && Target.AnimNotifyTracks.IsEmpty() && !Edits.IsEmpty()) Target.InitializeNotifyTrack();
 const bool bReplaceStates = FrozenArguments.ObjectKeys.Contains(TEXT("notify_states"));
 if (bStructure)
 {
  FStructuralIntent StructuralIntent;
  const bool bSlots = StructureArguments.ObjectKeys.Contains(TEXT("slots"));
  Valid = ParseMontageStructuralArguments(StructureArguments, bSlots ? SlotsPrimitive : SectionsPrimitive, StructuralIntent);
  if (!Valid.bValid) return Valid;
  StructuralIntent.ReplaceManagedFamilies = bStates ? EManagedNotifyReplacement::States
   : bReplaceStates ? EManagedNotifyReplacement::Both : EManagedNotifyReplacement::Events;
  Valid = PrepareMontageStructuralEdit(Target, StructuralIntent, Request.RequestId, Request.ExpectedRuntimeRevision, Structure);
  if (!Valid.bValid) return Valid;
 }
 const int32 TrackCount = TrackNames ? TrackNames->Num() : Target.AnimNotifyTracks.Num();
 Valid = CheckMontageNotifyPlan(Target, Edits, Structure.Get(), TrackCount, TrackNames.IsSet(), !bStates, bReplaceStates);
 if (!Valid.bValid) return Valid;
 TArray<UClass*> Classes;
 for (const auto& Edit : Edits) Classes.Add(Edit.Class);
 TSharedPtr<FPreparedNotifyStaging> State;
 Valid = PrepareMontageNotifyStagingImpl(Target, Classes, bStates, Request, StepIndex, OutBatch, State,
  !bStates, bReplaceStates);
 if (!Valid.bValid) return Valid;
 State->Structure = Structure;
 State->bReplacement = true; State->FrozenArguments = FrozenArguments; State->Edits = MoveTemp(Edits);
 State->OldNotifies = Target.Notifies; State->OldSlots = Target.SlotAnimTracks; State->OldSections = Target.CompositeSections;
 State->OldLength = Target.GetPlayLength(); State->OldTrackCount = Target.AnimNotifyTracks.Num();
 State->OldTracks = Target.AnimNotifyTracks;
 if (TrackNames)
 {
  // A kept track keeps its colour; a new one is White, as the editor's Add/Insert Notify Track makes it.
  State->bReplaceTracks = true;
  for (int32 I = 0; I < TrackNames->Num(); ++I)
   State->DesiredTracks.Emplace((*TrackNames)[I], State->OldTracks.IsValidIndex(I) ? State->OldTracks[I].TrackColor : FLinearColor::White);
 }
 for (const auto& Event : State->OldNotifies)
 {
  if (Event.Notify) State->Anchors.Emplace(Event.Notify);
  if (Event.NotifyStateClass) State->Anchors.Emplace(Event.NotifyStateClass);
 }
 // Replace the staging-only projection with the entire immutable authoring intent.
 OutBatch.Contributions[0].OwnedSurfaceIds = {State->TargetPath + TEXT("::Notifies")};
 OutBatch.Contributions[0].ValidateInvocation = [State](const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles)
 { return ValidateNotifyInvocation(*State, Invocation, Handles); };
 OutBatch.OpenMutationCapture = [State](const FGuid& TransactionId, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!TransactionId.IsValid() || State->bOpened || State->bClosed || !State->BaselineCurrent())
  { Diagnostics.Append(Result(false, TEXT("AnimationProvider.Montage.PreparedStateDrift")).Diagnostics); return {}; }
  State->TransactionId = TransactionId; State->bOpened = true;
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([State]() { State->bClosed = true; });
 };
 if (Structure)
 {
  FAssetDocumentPreparedMutationBatch StructuralBatch;
  FAssetDocumentMutationIntent StructuralFrozen;
  Valid = MakeMontageStructuralPreparedBatch(Structure.ToSharedRef(), StepIndex, EAssetDocumentRuntimeRequestPhase::Apply, StructuralBatch, StructuralFrozen);
  if (!Valid.bValid) { OutBatch = {}; return Valid; }
  State->StructureInverse = StructuralBatch.Contributions[0].InitialAction;
  OutBatch.Contributions[0].OwnedSurfaceIds = StructuralBatch.Contributions[0].OwnedSurfaceIds;
  auto NotifyOpen = MoveTemp(OutBatch.OpenMutationCapture); auto StructureOpen = MoveTemp(StructuralBatch.OpenMutationCapture);
  OutBatch.OpenMutationCapture = [NotifyOpen = MoveTemp(NotifyOpen), StructureOpen = MoveTemp(StructureOpen)](
   const FGuid& TransactionId, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics) mutable
   -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
  {
   auto First = NotifyOpen(TransactionId, Diagnostics);
   if (!First || !First->IsValid()) return {};
   auto Last = StructureOpen(TransactionId, Diagnostics);
   if (!Last || !Last->IsValid()) { First->Close(); return {}; }
   return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([First = MoveTemp(First), Last = MoveTemp(Last)]() mutable
   { Last->Close(); First->Close(); Last.Reset(); First.Reset(); });
  };
 }
 OutState = MoveTemp(State);
 return Result(true);
}
FAssetDocumentPrimitiveOutcome InvokeMontageNotifyReplacement(TSharedRef<FPreparedNotifyStaging> State,
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction)
{
 if (!Transaction || Transaction->GetState() != EAssetDocumentAtomicState::Mutating
  || Transaction->GetTransactionId() != Invocation.TransactionId || !State->OpenFor(Transaction->GetMutationToken()))
  return NotifyFailure(Result(false, TEXT("AnimationProvider.TransactionUnavailable")));
 auto Valid = ValidateNotifyInvocation(*State, Invocation, Handles);
 if (!Valid.bValid) return NotifyFailure(Valid);
 auto Acquired = AcquireAnimationInputLease(Handles, Invocation, TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
 if (!Acquired.Lease) return NotifyFailure(Result(false, TEXT("AnimationProvider.InvalidHandlePayload")));
 // Empty replacement still removes old managed rows: enlist once before any allocation or publication.
 Valid = Transaction->EnlistFootprint(State->Footprint());
 if (!Valid.bValid) return NotifyFailure(Valid);
 TArray<UObject*> Objects;
 Valid = StageImpl(State, *Transaction, Objects, INDEX_NONE);
 if (!Valid.bValid) return NotifyFailure(Valid);
 for (int32 I = 0; I < Objects.Num(); ++I)
 {
  // Provenance is the actual ordered set populated immediately by the sole allocation leaf.
  if (State->Objects[I].Get() != Objects[I] || Objects[I]->GetClass() != State->Classes[I].Get()) return NotifyFailure(Result(false));
  auto Configured = ConfigureAssetDocumentEditorStagedMontageNotifyProperties(Invocation, *Acquired.Lease,
   *Objects[I], State->Edits[I].Properties, *Transaction);
  if (Configured.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) return Configured;

 }
 if (!State->BaselineCurrent()) return NotifyFailure(Result(false, TEXT("AnimationProvider.Montage.PreparedStateDrift")));
 if (State->Structure)
 {
  State->Structure->bApplied = true;
  Valid = ApplyPreparedStructurePrefixNoEnlist(State->Structure.ToSharedRef(), *Transaction);
  if (!Valid.bValid) return NotifyFailure(Valid);
 }
 // Observe the completed prefix before any placement callback; this is only a
 // forward side-effect guard, never a replacement for the immutable preimage.
 const auto BeforePlacement = State->Target->Notifies;
 for (int32 I = 0; I < Objects.Num(); ++I)
 {
  const bool bCanPlace = State->Edits[I].bState ? static_cast<UAnimNotifyState*>(Objects[I])->CanBePlaced(State->Target.Get())
   : static_cast<UAnimNotify*>(Objects[I])->CanBePlaced(State->Target.Get());
  if (!bCanPlace) return NotifyFailure(Result(false, TEXT("AnimationProvider.Montage.NotifyPlacementRejected")));
 }
 if (BeforePlacement.Num() != State->Target->Notifies.Num()) return NotifyFailure(Result(false));
 for (int32 I = 0; I < BeforePlacement.Num(); ++I)
  if (!FAnimNotifyEvent::StaticStruct()->CompareScriptStruct(&BeforePlacement[I], &State->Target->Notifies[I], 0)) return NotifyFailure(Result(false));
 Valid = State->Structure ? ReadbackPreparedStructure(*State->Structure) : Result(State->BaselineCurrent());
 if (!Valid.bValid) return NotifyFailure(Valid);
 TArray<FAnimNotifyEvent> Events;
 TArray<FLinkPlacement> Starts, Ends;
 for (const auto& Old : State->OldNotifies)
 {
  if (IsReplacedFamily(Old, *State)) continue;
  Events.Add(Old); Starts.Add({Old.GetTime(), Old.GetSlotIndex(), Old.GetLinkMethod()});
  Ends.Add({Old.EndLink.GetTime(), Old.EndLink.GetSlotIndex(), Old.EndLink.GetLinkMethod()});
 }
 if (State->Structure)
 {
  Events = State->Structure->DesiredNotifies;
  Starts = State->Structure->NotifyStarts; Ends = State->Structure->NotifyEnds;
 }
 const int32 RetainedCount = Events.Num();
 for (int32 I = 0; I < Objects.Num(); ++I)
 {
  const auto& Edit = State->Edits[I];
  auto& Event = Events.AddDefaulted_GetRef();
  Event.Guid = FGuid::NewGuid();
  Event.TrackIndex = Edit.TrackIndex;
  Event.NotifyName = Edit.bState ? FName(TEXT("AssetDocument.NotifyState")) : FName(TEXT("AssetDocument.Notify"));
  if (Edit.bState) Event.NotifyStateClass = CastChecked<UAnimNotifyState>(Objects[I]);
  else Event.Notify = CastChecked<UAnimNotify>(Objects[I]);
  Starts.Add({Edit.Time, 0, EAnimLinkMethod::Absolute});
  Ends.Add({Edit.Time + Edit.Duration, 0, EAnimLinkMethod::Absolute});
 }
 State->bPublished = true;
 if (State->bReplaceTracks)
 {
  State->bTracksPublished = true;
  PublishMontageNotifyTracks(*State->Target, State->DesiredTracks);
 }
 const int32 UnchangedPrefix = State->Structure ? 0 : RetainedCount;
 PublishMontageNotifyEvents(*State->Target, Events, Starts, Ends, UnchangedPrefix);
 Valid = CompleteMontageStructure(*State->Target);
 if (!Valid.bValid) return NotifyFailure(Valid);
 if (State->bReplaceTracks)
 {
  if (State->Target->AnimNotifyTracks.Num() != State->DesiredTracks.Num()) return NotifyFailure(Result(false, TEXT("AnimationProvider.Montage.ReadbackMismatch")));
  for (int32 I = 0; I < State->DesiredTracks.Num(); ++I)
   if (State->Target->AnimNotifyTracks[I].TrackName != State->DesiredTracks[I].TrackName)
    return NotifyFailure(Result(false, TEXT("AnimationProvider.Montage.ReadbackMismatch")));
 }
 if (State->Structure)
 {
  Valid = ReadbackPreparedStructure(*State->Structure);
  if (!Valid.bValid) return NotifyFailure(Valid);
 }
 if (State->Target->Notifies.Num() != Events.Num()) return NotifyFailure(Result(false, TEXT("AnimationProvider.Montage.ReadbackMismatch")));
 for (int32 I = 0; I < Events.Num(); ++I)
 {
  auto Expected = Events[I];
  if (State->Structure)
  {
   Expected.RefreshTriggerOffset(State->Target->CalculateOffsetForNotify(Starts[I].Time));
   Expected.RefreshEndTriggerOffset(State->Target->CalculateOffsetForNotify(Expected.NotifyStateClass ? Ends[I].Time : Expected.EndLink.GetTime()));
  }
  const auto* Found = State->Target->Notifies.FindByPredicate([&](const FAnimNotifyEvent& Candidate)
  { return Candidate.Notify == Expected.Notify && Candidate.NotifyStateClass == Expected.NotifyStateClass
    && Candidate.TrackIndex == Expected.TrackIndex && FMath::IsNearlyEqual(Candidate.GetTime(), Starts[I].Time); });
  if (!Found) return NotifyFailure(Result(false, TEXT("AnimationProvider.Montage.ReadbackMismatch")));
  const auto& Actual = *Found;
  if (Actual.Notify != Expected.Notify || Actual.NotifyStateClass != Expected.NotifyStateClass
   || Actual.NotifyName != Expected.NotifyName || Actual.Guid != Expected.Guid || Actual.TrackIndex != Expected.TrackIndex
   || (I < RetainedCount && (Actual.TriggerTimeOffset != Expected.TriggerTimeOffset || Actual.EndTriggerTimeOffset != Expected.EndTriggerTimeOffset))
   || !FMath::IsNearlyEqual(Actual.GetTime(), Starts[I].Time)
   || (Actual.NotifyStateClass && !FMath::IsNearlyEqual(Actual.GetDuration(), Ends[I].Time - Starts[I].Time)))
   return NotifyFailure(Result(false, TEXT("AnimationProvider.Montage.ReadbackMismatch")));
 }
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 Out.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 Out.Value.ObjectKeys = {TEXT("changed")};
 auto& Changed = Out.Value.Elements.AddDefaulted_GetRef();
 Changed.Kind = EAssetDocumentRuntimeValueKind::Boolean; Changed.BooleanValue = true;
 auto& Change = Out.ChangedSet.AddDefaulted_GetRef();
 Change.AuthorityId = Invocation.PrimitiveId; Change.StableObjectId = State->TargetPath;
 Change.Path = TEXT("/Notifies"); Change.ChangeKind = TEXT("Replace");
 return Out;
}
}
