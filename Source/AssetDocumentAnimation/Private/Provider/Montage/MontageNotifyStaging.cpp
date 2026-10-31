// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageNotifyStaging.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Provider/Montage/MontageNotifyArguments.h"
#include "Provider/Montage/MontageStructuralProvider.h"
#include "Provider/Montage/MontageStructuralPreparation.h"
#include "Provider/Sequence/AnimationAssetAuthoringRawEdits.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentEditorNativeState.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
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
 // Replacement: the edit each staged class belongs to (rows without an existing notify to reuse), and for every
 // edit the index of the existing notify it reuses (INDEX_NONE: a new notify).
 TArray<int32> StagedEdits, Pairs;
 // Existing notify objects whose properties this replacement wrote, with their serialized state from before.
 TArray<TStrongObjectPtr<UObject>> Rewritten;
 TArray<TArray<uint8>> RewrittenPreimages;
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
   || (bReplacement && StagedEdits.Num() != Classes.Num())) return false;
  for (int32 I = 0; I < Classes.Num(); ++I)
  {
   auto* Class = Classes[I].Get();
   if (!IsValid(Class) || Class->GetPathName() != ClassPaths[I]
    || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
    || !Class->IsChildOf(ClassStates[I] ? UAnimNotifyState::StaticClass() : UAnimNotify::StaticClass())
    || (ClassStates[I] ? !bReplaceStates : !bReplaceNotifies)
    || (bReplacement && (!Edits.IsValidIndex(StagedEdits[I]) || Edits[StagedEdits[I]].Class != Class
     || Edits[StagedEdits[I]].bState != ClassStates[I]))) return false;
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
  // Existing notify objects get back the property values they had before this replacement wrote them.
  for (int32 I = 0; I < State->Rewritten.Num(); ++I)
  {
   UObject* Object = State->Rewritten[I].Get();
   if (!IsValid(Object) || Object->GetOuter() != State->Target.Get())
    return Result(false, TEXT("AnimationProvider.Montage.NotifyStagingRollbackFailed"));
   FObjectReader Restore(Object, State->RewrittenPreimages[I]);
   Object->PostEditChange();
  }
  State->Rewritten.Reset(); State->RewrittenPreimages.Reset();
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
 // As the notify panel's Add Notify makes it: owned by the Montage, transactional, with an engine-generated name.
 return NewObject<UObject>(State.Target.Get(), &Class, NAME_None, RF_Transactional);
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
// Exact equality: strings and keys compare case-sensitively (an enumerator spelled in another case is a different value).
bool EqualNotifyArguments(const FAssetDocumentRuntimeValueDto& A, const FAssetDocumentRuntimeValueDto& B)
{
 if (A.Kind != B.Kind || A.BooleanValue != B.BooleanValue || A.IntegerValue != B.IntegerValue
  || A.NumberValue != B.NumberValue || !A.StringValue.Equals(B.StringValue, ESearchCase::CaseSensitive)
  || A.BytesValue != B.BytesValue || A.ObjectKeys.Num() != B.ObjectKeys.Num() || A.Elements.Num() != B.Elements.Num()) return false;
 for (int32 I = 0; I < A.ObjectKeys.Num(); ++I)
  if (!A.ObjectKeys[I].Equals(B.ObjectKeys[I], ESearchCase::CaseSensitive)) return false;
 for (int32 I = 0; I < A.Elements.Num(); ++I)
  if (!EqualNotifyArguments(A.Elements[I], B.Elements[I])) return false;
 return true;
}
bool IsReplacedFamily(const FAnimNotifyEvent& Event, const FPreparedNotifyStaging& State)
{
 return Event.NotifyStateClass ? State.bReplaceStates : State.bReplaceNotifies;
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
bool MontageNotifyPlanReplaces(const FAnimNotifyEvent& Event, bool bReplaceNotifies, bool bReplaceStates)
{
 return Event.NotifyStateClass ? bReplaceStates : bReplaceNotifies;
}
const TCHAR* MontageNotifyFamilyLabel(bool bState) { return bState ? TEXT("NotifyStates") : TEXT("Notifies"); }
FString MontageNotifyRowPath(const FNotifyEdit& Edit, const TCHAR* Field)
{
 return FString::Printf(TEXT("/Body/%s/%d/%s"), MontageNotifyFamilyLabel(Edit.bState), Edit.Row, Field);
}
FString MontageExistingNotifyPath(int32 Index, const TCHAR* Field)
{
 return FString::Printf(TEXT("/Montage/Notifies/%d/%s"), Index, Field);
}
// A notify link is kept on its slot track only in memory: loading the Montage relinks every notify to the first slot
// track (UAnimMontage::PostLoad), so a document names no other.
FAssetDocumentContractValidationResult CheckMontageNotifySlot(FName Slot, const TArray<FSlotAnimationTrack>& Slots,
 const FNotifyEdit& Edit, const TCHAR* Field)
{
 if (Slot.IsNone()) return Result(true);
 const int32 Index = Slots.IndexOfByPredicate([Slot](const FSlotAnimationTrack& Track) { return Track.SlotName == Slot; });
 if (Index == 0) return Result(true);
 const FString First = Slots.IsEmpty() ? FString(TEXT("none")) : Slots[0].SlotName.ToString();
 return MontageNotifyPlanFailure(TEXT("AnimationProvider.Montage.InvalidIntent"), MontageNotifyRowPath(Edit, Field),
  Index == INDEX_NONE
   ? FString::Printf(TEXT("%s[%d] %s '%s' is not a slot track of the Montage (its first is '%s')."),
    MontageNotifyFamilyLabel(Edit.bState), Edit.Row, Field, *Slot.ToString(), *First)
   : FString::Printf(TEXT("%s[%d] %s '%s': UE relinks every notify to the first slot track ('%s') when the Montage is loaded, so a "
    "notify cannot stay on another; omit %s or name '%s'."), MontageNotifyFamilyLabel(Edit.bState), Edit.Row, Field, *Slot.ToString(),
    *First, Field, *First));
}
}
FAssetDocumentContractValidationResult CheckMontageNotifyPlan(const UAnimMontage& Target, TConstArrayView<FNotifyEdit> Edits,
 const FPreparedStructuralEdit* Structure, int32 TrackCount, bool bTracksAuthored, bool bReplaceNotifies, bool bReplaceStates)
{
 const float DesiredLength = Structure ? Structure->DesiredLength : Target.GetPlayLength();
 const TArray<FSlotAnimationTrack>& FinalSlots = Structure ? Structure->DesiredSlots : Target.SlotAnimTracks;
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
 for (const auto& Edit : Edits)
 {
  const TCHAR* Label = MontageNotifyFamilyLabel(Edit.bState);
  if (Edit.TrackIndex < 0 || Edit.TrackIndex >= TrackCount)
   return MontageNotifyPlanFailure(TEXT("AnimationProvider.Montage.NotifyTrackMissing"), MontageNotifyRowPath(Edit, TEXT("TrackIndex")),
    FString::Printf(TEXT("%s[%d] TrackIndex %d does not exist: the Montage has %d notify track(s) (0..%d)%s."),
     Label, Edit.Row, Edit.TrackIndex, TrackCount, TrackCount - 1,
     bTracksAuthored ? TEXT(", as NotifyTracks lists them") : TEXT("; add tracks with NotifyTracks")));
  if (Edit.Time > DesiredLength || (Edit.bState && Edit.Time + Edit.Duration > DesiredLength))
   return MontageNotifyPlanFailure(TEXT("AnimationProvider.Montage.NotifyBeyondLength"), MontageNotifyRowPath(Edit, TEXT("Time")),
    FString::Printf(TEXT("%s[%d] at %g s%s ends after the Montage length %g s."), Label, Edit.Row, Edit.Time,
     Edit.bState ? *FString::Printf(TEXT(" + %g s"), Edit.Duration) : TEXT(""), DesiredLength));
  auto Valid = CheckMontageNotifySlot(Edit.Slot, FinalSlots, Edit, TEXT("SlotName"));
  if (Valid.bValid && Edit.bState) Valid = CheckMontageNotifySlot(Edit.EndSlot, FinalSlots, Edit, TEXT("EndSlotName"));
  if (!Valid.bValid) return Valid;
 }
 // Notifies the document keeps (a family it omits) stay as they are; they must already be reloadable as they are.
 for (int32 Index = 0; Index < Target.Notifies.Num(); ++Index)
 {
  const auto& Event = Target.Notifies[Index];
  if (MontageNotifyPlanReplaces(Event, bReplaceNotifies, bReplaceStates)) continue;
  const TCHAR* Kind = Event.NotifyStateClass ? TEXT("notify state") : TEXT("notify");
  const TCHAR* List = MontageNotifyFamilyLabel(Event.NotifyStateClass != nullptr);
  if (Event.GetSlotIndex() != 0 || (Event.NotifyStateClass && Event.EndLink.GetSlotIndex() != 0))
   return MontageNotifyPlanFailure(TEXT("AnimationProvider.Montage.DependencyUnavailable"),
    MontageExistingNotifyPath(Index, Event.GetSlotIndex() != 0 ? TEXT("SlotName") : TEXT("EndSlotName")),
    FString::Printf(TEXT("The Montage keeps a %s at %g s linked to a slot track other than the first, which UE relinks to the first when "
     "the Montage is loaded; list %s in the document to place it."), Kind, Event.GetTime(), List));
  if (!Target.AnimNotifyTracks.IsValidIndex(Event.TrackIndex))
   return MontageNotifyPlanFailure(TEXT("AnimationProvider.Montage.NotifyTrackMissing"), MontageExistingNotifyPath(Index, TEXT("TrackIndex")),
    FString::Printf(TEXT("The Montage keeps a %s at %g s on notify track %d, which it does not have; list %s in the document."),
     Kind, Event.GetTime(), Event.TrackIndex, List));
  if (!FMath::IsFinite(Event.GetTime()) || Event.GetTime() < 0.f || Event.GetTime() > Target.GetPlayLength()
   || (Event.NotifyStateClass && (!FMath::IsFinite(Event.GetDuration()) || Event.GetDuration() <= 0.f
    || Event.EndLink.GetTime() > Target.GetPlayLength())))
   return MontageNotifyPlanFailure(TEXT("AnimationProvider.Montage.NotifyBeyondLength"), MontageExistingNotifyPath(Index, TEXT("Time")),
    FString::Printf(TEXT("The Montage keeps a %s at %g s that is not within its length %g s; list %s in the document."),
     Kind, Event.GetTime(), Target.GetPlayLength(), List));
 }
 // Both forward completion and rollback must preserve the admitted baseline offsets. A notify that is not a
 // state has no end, so its end offset has no effect (the Montage editor and stock completion disagree on it).
 for (int32 Index = 0; Index < Target.Notifies.Num(); ++Index)
 {
  const auto& Event = Target.Notifies[Index];
  auto Predicted = Event;
  Predicted.RefreshTriggerOffset(Target.CalculateOffsetForNotify(Event.GetTime()));
  Predicted.RefreshEndTriggerOffset(Target.CalculateOffsetForNotify(Event.EndLink.GetTime()));
  if (Predicted.TriggerTimeOffset != Event.TriggerTimeOffset
   || (Event.NotifyStateClass && Predicted.EndTriggerTimeOffset != Event.EndTriggerTimeOffset))
   return MontageNotifyPlanFailure(TEXT("AnimationProvider.Montage.DependencyUnavailable"), MontageExistingNotifyPath(Index, TEXT("Time")),
    FString::Printf(TEXT("The Montage's notify at %g s has a trigger offset that UE would recompute on this edit; open the Montage in the "
     "editor and save it to refresh its notify offsets."), Event.GetTime()));
 }
 // Retained notifies (a family the document omits) keep their TrackIndex.
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
  if (!Structure && !MontageNotifyPlanReplaces(Old, bReplaceNotifies, bReplaceStates))
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
namespace
{
UClass* MontageNotifyEventClass(const FAnimNotifyEvent& Event)
{
 if (Event.NotifyStateClass) return Event.NotifyStateClass->GetClass();
 if (Event.Notify) return Event.Notify->GetClass();
 return nullptr;
}
// The existing notify an edit row stands for: the same family and identity (its object's class, or a skeleton
// notify's name), owned by this Montage. First the one at the row's exact time and track, then the nearest in time
// (the same track first, then the earliest in the array). Rows are matched in document order.
TArray<int32> PairMontageNotifyEdits(const UAnimMontage& Target, TConstArrayView<FNotifyEdit> Edits, bool bReplaceNotifies, bool bReplaceStates)
{
 TArray<int32> Pairs;
 Pairs.Init(INDEX_NONE, Edits.Num());
 TArray<bool> Used;
 Used.Init(false, Target.Notifies.Num());
 auto Same = [&Target](const FAnimNotifyEvent& Event, const FNotifyEdit& Edit)
 {
  const UObject* Object = Edit.bState ? static_cast<const UObject*>(Event.NotifyStateClass.Get()) : static_cast<const UObject*>(Event.Notify.Get());
  if ((Event.NotifyStateClass != nullptr) != Edit.bState || (Edit.bState && Event.Notify)) return false;
  if (!Edit.Class) return !Object && !Event.Notify && !Event.NotifyStateClass && Event.NotifyName == Edit.Name;
  return Object && Object->GetOuter() == &Target && Object->GetClass() == Edit.Class;
 };
 for (int32 E = 0; E < Edits.Num(); ++E)
  for (int32 I = 0; I < Target.Notifies.Num(); ++I)
  {
   const auto& Event = Target.Notifies[I];
   if (!Used[I] && MontageNotifyPlanReplaces(Event, bReplaceNotifies, bReplaceStates) && Same(Event, Edits[E])
    && Event.TrackIndex == Edits[E].TrackIndex && Event.GetTime() == Edits[E].Time)
   { Pairs[E] = I; Used[I] = true; break; }
  }
 for (int32 E = 0; E < Edits.Num(); ++E)
 {
  if (Pairs[E] != INDEX_NONE) continue;
  int32 Best = INDEX_NONE;
  for (int32 I = 0; I < Target.Notifies.Num(); ++I)
  {
   const auto& Event = Target.Notifies[I];
   if (Used[I] || !MontageNotifyPlanReplaces(Event, bReplaceNotifies, bReplaceStates) || !Same(Event, Edits[E])) continue;
   if (Best == INDEX_NONE) { Best = I; continue; }
   const auto& Current = Target.Notifies[Best];
   const float Distance = FMath::Abs(Event.GetTime() - Edits[E].Time), BestDistance = FMath::Abs(Current.GetTime() - Edits[E].Time);
   const bool bTrack = Event.TrackIndex == Edits[E].TrackIndex, bBestTrack = Current.TrackIndex == Edits[E].TrackIndex;
   if (Distance < BestDistance || (Distance == BestDistance && bTrack && !bBestTrack)) Best = I;
  }
  if (Best != INDEX_NONE) { Pairs[E] = Best; Used[Best] = true; }
 }
 return Pairs;
}
void ApplyMontageNotifyTrigger(FAnimNotifyEvent& Event, const FNotifyTrigger& Trigger)
{
 if (Trigger.WeightThreshold) Event.TriggerWeightThreshold = *Trigger.WeightThreshold;
 if (Trigger.Chance) Event.NotifyTriggerChance = *Trigger.Chance;
 if (Trigger.FilterType) Event.NotifyFilterType = *Trigger.FilterType;
 if (Trigger.FilterLOD) Event.NotifyFilterLOD = *Trigger.FilterLOD;
 if (Trigger.FilterableByRequest) Event.bCanBeFilteredViaRequest = *Trigger.FilterableByRequest;
 if (Trigger.DedicatedServer) Event.bTriggerOnDedicatedServer = *Trigger.DedicatedServer;
 if (Trigger.Follower) Event.bTriggerOnFollower = *Trigger.Follower;
 if (Trigger.TickType) Event.MontageTickType = *Trigger.TickType;
}
bool SameMontageNotifyTrigger(const FAnimNotifyEvent& A, const FAnimNotifyEvent& B)
{
 return A.TriggerWeightThreshold == B.TriggerWeightThreshold && A.NotifyTriggerChance == B.NotifyTriggerChance
  && A.NotifyFilterType == B.NotifyFilterType && A.NotifyFilterLOD == B.NotifyFilterLOD
  && A.bCanBeFilteredViaRequest == B.bCanBeFilteredViaRequest && A.bTriggerOnDedicatedServer == B.bTriggerOnDedicatedServer
  && A.bTriggerOnFollower == B.bTriggerOnFollower && A.MontageTickType == B.MontageTickType;
}
// The authored properties of an existing notify object whose current value differs (written in place, as the
// details panel edits a selected notify).
FAssetDocumentContractValidationResult ChangedMontageNotifyProperties(UObject& Object, const FNotifyEdit& Edit,
 FAssetDocumentRuntimeValueDto& Out)
{
 Out = FAssetDocumentRuntimeValueDto();
 Out.Kind = EAssetDocumentRuntimeValueKind::Object;
 FAssetDocumentRuntimeValueDto Current;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 if (!ReadAssetDocumentEditorSubobjectProperties(Object, Current, Diagnostics))
 {
  auto Failed = Result(false, TEXT("PropertyRead.AccessFailed"), TEXT("The notify's properties could not be read."));
  if (!Diagnostics.IsEmpty()) Failed.Diagnostics = MoveTemp(Diagnostics);
  for (auto& Diagnostic : Failed.Diagnostics) Diagnostic.Path = MontageNotifyRowPath(Edit, TEXT("Object/Properties"));
  return Failed;
 }
 for (int32 I = 0; I < Edit.Properties.ObjectKeys.Num(); ++I)
 {
  const int32 Found = Current.ObjectKeys.IndexOfByKey(Edit.Properties.ObjectKeys[I]);
  if (Current.Elements.IsValidIndex(Found) && EqualNotifyArguments(Current.Elements[Found], Edit.Properties.Elements[I])) continue;
  Out.ObjectKeys.Add(Edit.Properties.ObjectKeys[I]);
  Out.Elements.Add(Edit.Properties.Elements[I]);
 }
 return Result(true);
}
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
  StructuralIntent.bReplaceNotifies = !bStates;
  StructuralIntent.bReplaceNotifyStates = bReplaceStates;
  Valid = PrepareMontageStructuralEdit(Target, StructuralIntent, Request.RequestId, Request.ExpectedRuntimeRevision, Structure);
  if (!Valid.bValid) return Valid;
 }
 const int32 TrackCount = TrackNames ? TrackNames->Num() : Target.AnimNotifyTracks.Num();
 Valid = CheckMontageNotifyPlan(Target, Edits, Structure.Get(), TrackCount, TrackNames.IsSet(), !bStates, bReplaceStates);
 if (!Valid.bValid) return Valid;
 // Rows that stand for an existing notify reuse it; the others get a new notify object (skeleton notifies have none).
 const TArray<int32> Pairs = PairMontageNotifyEdits(Target, Edits, !bStates, bReplaceStates);
 TArray<UClass*> Classes;
 TArray<int32> StagedEdits;
 for (int32 I = 0; I < Edits.Num(); ++I)
  if (Pairs[I] == INDEX_NONE && Edits[I].Class) { Classes.Add(Edits[I].Class); StagedEdits.Add(I); }
 TSharedPtr<FPreparedNotifyStaging> State;
 Valid = PrepareMontageNotifyStagingImpl(Target, Classes, bStates, Request, StepIndex, OutBatch, State,
  !bStates, bReplaceStates);
 if (!Valid.bValid) return Valid;
 State->Structure = Structure;
 State->bReplacement = true; State->FrozenArguments = FrozenArguments; State->Edits = MoveTemp(Edits);
 State->StagedEdits = MoveTemp(StagedEdits); State->Pairs = Pairs;
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
 // An empty replacement still removes the family's notifies: enlist once before any allocation or publication.
 Valid = Transaction->EnlistFootprint(State->Footprint());
 if (!Valid.bValid) return NotifyFailure(Valid);
 TArray<UObject*> Objects;
 Valid = StageImpl(State, *Transaction, Objects, INDEX_NONE);
 if (!Valid.bValid) return NotifyFailure(Valid);
 auto RowDiagnostics = [](FAssetDocumentContractValidationResult Failed, const FNotifyEdit& Edit)
 {
  for (auto& Diagnostic : Failed.Diagnostics)
   if (!Diagnostic.Path.StartsWith(TEXT("/Body/"))) Diagnostic.Path = MontageNotifyRowPath(Edit, TEXT("Object/Properties"));
  return Failed;
 };
 for (int32 I = 0; I < Objects.Num(); ++I)
 {
  // Provenance is the actual ordered set populated immediately by the sole allocation leaf.
  if (State->Objects[I].Get() != Objects[I] || Objects[I]->GetClass() != State->Classes[I].Get()) return NotifyFailure(Result(false));
  const auto& Edit = State->Edits[State->StagedEdits[I]];
  auto Configured = ConfigureAssetDocumentEditorStagedMontageNotifyProperties(Invocation, *Acquired.Lease,
   *Objects[I], Edit.Properties, *Transaction);
  if (Configured.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
  {
   FAssetDocumentContractValidationResult Failed;
   Failed.Diagnostics = MoveTemp(Configured.Diagnostics);
   Configured.Diagnostics = RowDiagnostics(MoveTemp(Failed), Edit).Diagnostics;
   return Configured;
  }
 }
 // A row that stands for an existing notify edits its object in place, as the details panel edits a selected notify.
 for (int32 E = 0; E < State->Edits.Num(); ++E)
 {
  const auto& Edit = State->Edits[E];
  if (State->Pairs[E] == INDEX_NONE || !Edit.Class) continue;
  const auto& Previous = State->OldNotifies[State->Pairs[E]];
  UObject* Object = Edit.bState ? static_cast<UObject*>(Previous.NotifyStateClass.Get()) : static_cast<UObject*>(Previous.Notify.Get());
  if (!IsValid(Object) || Object->GetOuter() != State->Target.Get() || Object->GetClass() != Edit.Class)
   return NotifyFailure(Result(false, TEXT("AnimationProvider.Montage.PreparedStateDrift")));
  FAssetDocumentRuntimeValueDto Changed;
  Valid = ChangedMontageNotifyProperties(*Object, Edit, Changed);
  if (!Valid.bValid) return NotifyFailure(Valid);
  if (Changed.ObjectKeys.IsEmpty()) continue;
  FObjectWriter Capture(Object, State->RewrittenPreimages.AddDefaulted_GetRef(), false, false, /*bDoDelta*/ false);
  State->Rewritten.Emplace(Object);
  Object->Modify();
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  if (!WriteAssetDocumentEditorSubobjectProperties(*Object, Changed, Invocation.PrimitiveId, Diagnostics))
  {
   auto Failed = Result(false, TEXT("PropertyWrite.AccessFailed"), TEXT("A notify property could not be written."));
   if (!Diagnostics.IsEmpty()) Failed.Diagnostics = MoveTemp(Diagnostics);
   return NotifyFailure(RowDiagnostics(MoveTemp(Failed), Edit));
  }
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
  const bool bCanPlace = State->Edits[State->StagedEdits[I]].bState ? static_cast<UAnimNotifyState*>(Objects[I])->CanBePlaced(State->Target.Get())
   : static_cast<UAnimNotify*>(Objects[I])->CanBePlaced(State->Target.Get());
  if (!bCanPlace)
   return NotifyFailure(MontageNotifyPlanFailure(TEXT("AnimationProvider.Montage.NotifyPlacementRejected"),
    MontageNotifyRowPath(State->Edits[State->StagedEdits[I]], TEXT("Object/Class")),
    FString::Printf(TEXT("%s cannot be placed on this Montage (its CanBePlaced refuses it)."), *Objects[I]->GetClass()->GetPathName())));
 }
 if (BeforePlacement.Num() != State->Target->Notifies.Num()) return NotifyFailure(Result(false));
 for (int32 I = 0; I < BeforePlacement.Num(); ++I)
  if (!FAnimNotifyEvent::StaticStruct()->CompareScriptStruct(&BeforePlacement[I], &State->Target->Notifies[I], 0)) return NotifyFailure(Result(false));
 Valid = State->Structure ? ReadbackPreparedStructure(*State->Structure) : Result(State->BaselineCurrent());
 if (!Valid.bValid) return NotifyFailure(Valid);
 TArray<FAnimNotifyEvent> Events;
 TArray<FLinkPlacement> Starts, Ends;
 // Relink[I]: Events[I] is placed from Starts/Ends; otherwise it keeps its link as it is.
 TArray<bool> Relink;
 if (State->Structure)
 {
  Events = State->Structure->DesiredNotifies;
  Starts = State->Structure->NotifyStarts; Ends = State->Structure->NotifyEnds;
  Relink.Init(true, Events.Num());
 }
 else
  for (const auto& Old : State->OldNotifies)
  {
   if (IsReplacedFamily(Old, *State)) continue;
   Events.Add(Old); Starts.Add({Old.GetTime(), Old.GetSlotIndex(), Old.GetLinkMethod()});
   Ends.Add({Old.EndLink.GetTime(), Old.EndLink.GetSlotIndex(), Old.EndLink.GetLinkMethod()});
   Relink.Add(false);
  }
 const int32 RetainedCount = Events.Num();
 int32 Staged = 0;
 for (int32 E = 0; E < State->Edits.Num(); ++E)
 {
  const auto& Edit = State->Edits[E];
  const float End = Edit.bState ? Edit.Time + Edit.Duration : Edit.Time;
  FAnimNotifyEvent Event;
  bool bKeepLink = false;
  if (State->Pairs[E] != INDEX_NONE)
  {
   // The existing notify keeps its guid, name, colour and the settings the row omits.
   const auto& Previous = State->OldNotifies[State->Pairs[E]];
   Event = Previous;
   bKeepLink = !State->Structure && Previous.GetTime() == Edit.Time && Previous.GetSlotIndex() == 0 && Previous.GetLinkMethod() == Edit.Method
    && (!Edit.bState || (Previous.EndLink.GetTime() == End && Previous.EndLink.GetSlotIndex() == 0 && Previous.EndLink.GetLinkMethod() == Edit.EndMethod));
  }
  else
  {
   // SAnimNotifyTrack::CreateNewNotify: a new guid, the panel's name for the class, its default weight threshold.
   Event.Guid = FGuid::NewGuid();
   if (Edit.Class)
   {
    if (!State->StagedEdits.IsValidIndex(Staged) || State->StagedEdits[Staged] != E) return NotifyFailure(Result(false));
    UObject* Object = Objects[Staged++];
    Event.NotifyName = AssetDocumentAnimationAuthoringRaw::MakeNotifyName(*Edit.Class);
    if (Edit.bState)
    {
     Event.NotifyStateClass = CastChecked<UAnimNotifyState>(Object);
     Event.TriggerWeightThreshold = Event.NotifyStateClass->GetDefaultTriggerWeightThreshold();
    }
    else
    {
     Event.Notify = CastChecked<UAnimNotify>(Object);
     Event.TriggerWeightThreshold = Event.Notify->GetDefaultTriggerWeightThreshold();
    }
   }
   else Event.NotifyName = Edit.Name;
  }
  Event.TrackIndex = Edit.TrackIndex;
  ApplyMontageNotifyTrigger(Event, Edit.Trigger);
  Events.Add(MoveTemp(Event));
  Starts.Add({Edit.Time, 0, Edit.Method});
  Ends.Add({End, 0, Edit.EndMethod});
  Relink.Add(!bKeepLink);
 }
 if (Staged != Objects.Num()) return NotifyFailure(Result(false));
 State->bPublished = true;
 if (State->bReplaceTracks)
 {
  State->bTracksPublished = true;
  PublishMontageNotifyTracks(*State->Target, State->DesiredTracks);
 }
 PublishMontageNotifyEvents(*State->Target, Events, Starts, Ends, Relink);
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
 TArray<bool> Matched;
 Matched.Init(false, Events.Num());
 for (int32 I = 0; I < Events.Num(); ++I)
 {
  auto Expected = Events[I];
  if (State->Structure)
  {
   Expected.RefreshTriggerOffset(State->Target->CalculateOffsetForNotify(Starts[I].Time));
   Expected.RefreshEndTriggerOffset(State->Target->CalculateOffsetForNotify(Expected.NotifyStateClass ? Ends[I].Time : Expected.EndLink.GetTime()));
  }
  const int32 Found = State->Target->Notifies.IndexOfByPredicate([&](const FAnimNotifyEvent& Candidate)
  {
   const int32 Index = UE_PTRDIFF_TO_INT32(&Candidate - State->Target->Notifies.GetData());
   return !Matched[Index] && Candidate.Notify == Expected.Notify && Candidate.NotifyStateClass == Expected.NotifyStateClass
    && Candidate.NotifyName == Expected.NotifyName && Candidate.Guid == Expected.Guid && Candidate.TrackIndex == Expected.TrackIndex
    && FMath::IsNearlyEqual(Candidate.GetTime(), Starts[I].Time);
  });
  if (Found == INDEX_NONE) return NotifyFailure(Result(false, TEXT("AnimationProvider.Montage.ReadbackMismatch")));
  Matched[Found] = true;
  const auto& Actual = State->Target->Notifies[Found];
  const bool bCompareOffsets = I < RetainedCount || !Relink[I];
  if ((bCompareOffsets && (Actual.TriggerTimeOffset != Expected.TriggerTimeOffset
    || (Expected.NotifyStateClass && Actual.EndTriggerTimeOffset != Expected.EndTriggerTimeOffset)))
   || !SameMontageNotifyTrigger(Actual, Expected) || Actual.GetLinkMethod() != Starts[I].Method
   || (Actual.NotifyStateClass && (!FMath::IsNearlyEqual(Actual.GetDuration(), Ends[I].Time - Starts[I].Time)
    || Actual.EndLink.GetLinkMethod() != Ends[I].Method)))
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
