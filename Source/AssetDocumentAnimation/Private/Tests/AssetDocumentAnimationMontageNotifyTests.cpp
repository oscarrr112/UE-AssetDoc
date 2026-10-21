// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "Provider/Montage/MontageNotifyStaging.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Animation/AnimNotifies/AnimNotify_PlaySound.h"
#include "Animation/AnimNotifies/AnimNotifyState_TimedParticleEffect.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "Utils/AssetDocumentEngineCompat.h"

namespace AssetDocumentMontageNotifyTests
{
using namespace AssetDocumentMontage;
class FStagingTransaction final : public IAssetDocumentAtomicTransaction
{
public:
 FAssetDocumentMutationToken Token;
 TSharedPtr<IAssetDocumentRollbackAction> InitialAction;
 TUniquePtr<FAssetDocumentCoreMutationCaptureLease> Capture;
 TArray<FAssetDocumentMutationFootprint> Expected;
 bool bEnlisted = false;
 int32 LateCalls = 0;
 FStagingTransaction() { Token.RequestId = FGuid::NewGuid(); Token.TransactionId = FGuid::NewGuid(); Token.RuntimeRevision = 1; }
 bool Bind(FAssetDocumentPreparedMutationBatch& Batch)
 {
  if (Batch.Contributions.Num() != 1 || !Batch.Contributions[0].InitialAction || !Batch.OpenMutationCapture) return false;
  InitialAction = Batch.Contributions[0].InitialAction; Expected = Batch.Contributions[0].RawFootprint;
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  Capture = Batch.OpenMutationCapture(Token.TransactionId, Diagnostics);
  return Capture && Capture->IsValid();
 }
 FGuid GetTransactionId() const override { return Token.TransactionId; }
 EAssetDocumentAtomicState GetState() const override { return EAssetDocumentAtomicState::Mutating; }
 const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
 FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Rows) override
 {
  if (bEnlisted || Rows.Num() != Expected.Num()) return Result(false);
  for (int32 I = 0; I < Rows.Num(); ++I)
   if (Rows[I].Kind != Expected[I].Kind || Rows[I].StableId != Expected[I].StableId || Rows[I].RollbackComponentId != Expected[I].RollbackComponentId) return Result(false);
  bEnlisted = true; return Result(true);
 }
 FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction>) override { ++LateCalls; return Result(false); }
 FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override { ++LateCalls; return Result(false); }
 FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Result(false); }
 FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return Result(false); }
 FAssetDocumentContractValidationResult Rollback(FStringView Failure) override
 { Capture.Reset(); return InitialAction ? InitialAction->Rollback(Failure) : Result(false); }
};
FAssetDocumentRuntimeRequestDto MakeRequest(const UAnimMontage& Target, const FStagingTransaction& Transaction)
{
 FAssetDocumentRuntimeRequestDto R; R.RequestId = Transaction.Token.RequestId; R.ExpectedRuntimeRevision = Transaction.Token.RuntimeRevision;
 R.Operation = EAssetDocumentRuntimeOperation::Apply; R.TargetAssetPath = Target.GetPathName(); R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage"); return R;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageNotifyStaging,
 "UEAssetDoc.AnimationProvider.MontageStructural.NotifyStaging",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageNotifyStaging::RunTest(const FString&)
{
 using namespace AssetDocumentMontageNotifyTests;
 for (bool bStates : {false, true}) for (bool bInject : {false, true})
 {
  TStrongObjectPtr<UAnimMontage> Target(NewObject<UAnimMontage>(GetTransientPackage(), NAME_None, RF_Transactional));
  FStagingTransaction Transaction; FAssetDocumentPreparedMutationBatch Batch; TSharedPtr<FPreparedNotifyStaging> State;
  UClass* Class = bStates ? UAnimNotifyState_TimedParticleEffect::StaticClass() : UAnimNotify_PlaySound::StaticClass();
  TArray<UClass*> Classes = {Class, Class};
  if (!TestTrue(TEXT("typed family staging prepares"), PrepareMontageNotifyStaging(*Target, Classes, bStates,
   MakeRequest(*Target, Transaction), 0, Batch, State).bValid)) continue;
  const int32 OldNotifies = Target->Notifies.Num();
  TArray<UObject*> Objects;
  TestFalse(TEXT("Mutating without actual capture cannot construct"), Testing::StageNative(State.ToSharedRef(), Transaction, Objects).bValid);
  if (!TestTrue(TEXT("prepared inverse and capture are installed"), Transaction.Bind(Batch))) continue;
  const auto R = Testing::StageNative(State.ToSharedRef(), Transaction, Objects, bInject ? 0 : INDEX_NONE);
  TestEqual(TEXT("only explicit injected case fails"), R.bValid, !bInject);
  TestEqual(TEXT("no event attached by staging"), Target->Notifies.Num(), OldNotifies);
  TestEqual(TEXT("no handler late action/finalizer registration"), Transaction.LateCalls, 0);
  TArray<TWeakObjectPtr<UObject>> Created;
  if (bInject)
  {
   TestTrue(TEXT("partial staged set is not returned"), Objects.IsEmpty());
   TArray<UObject*> Children; GetObjectsWithOuter(Target.Get(), Children, AssetDocumentObjects::DirectOnly);
   for (auto* Child : Children)
    if (Child->IsA(bStates ? UAnimNotifyState::StaticClass() : UAnimNotify::StaticClass())) Created.Emplace(Child);
   TestEqual(TEXT("failure occurred after one actual construction"), Created.Num(), 1);
  }
  else
  {
   TestEqual(TEXT("complete ordered staging result"), Objects.Num(), 2);
   for (auto* Object : Objects)
   {
    Created.Emplace(Object);
    TestTrue(TEXT("correct direct outer"), Object->GetOuter() == Target.Get());
    TestTrue(TEXT("transactional object"), Object->HasAnyFlags(RF_Transactional));
    TestTrue(TEXT("legacy-compatible managed prefix"), Object->GetName().StartsWith(bStates ? TEXT("AssetDocumentManaged_NotifyState_") : TEXT("AssetDocumentManaged_Notify_")));
   }
   TestFalse(TEXT("prepared staging cannot execute twice"), Testing::StageNative(State.ToSharedRef(), Transaction, Objects).bValid);
  }
  TestTrue(TEXT("rollback retires only newly staged objects"), Transaction.Rollback(TEXT("fixture-staging-rollback")).bValid);
  for (const auto& Object : Created) TestFalse(TEXT("staged object is no longer live"), Object.IsValid());
  TestEqual(TEXT("rollback does not remove prior events"), Target->Notifies.Num(), OldNotifies);
 }
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageNotifyStagingWrongClass,
 "UEAssetDoc.AnimationProvider.MontageStructural.NotifyStagingWrongClass",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageNotifyStagingWrongClass::RunTest(const FString&)
{
 using namespace AssetDocumentMontageNotifyTests;
 TStrongObjectPtr<UAnimMontage> Target(NewObject<UAnimMontage>(GetTransientPackage(), NAME_None, RF_Transactional));
 for (bool bStates : {false, true})
 {
  FStagingTransaction Transaction; FAssetDocumentPreparedMutationBatch Batch; TSharedPtr<FPreparedNotifyStaging> State;
  TArray<UClass*> Classes = {bStates ? UAnimNotify_PlaySound::StaticClass() : UAnimNotifyState_TimedParticleEffect::StaticClass()};
  TestFalse(TEXT("wrong family rejected before capture/allocation"), PrepareMontageNotifyStaging(*Target, Classes, bStates,
   MakeRequest(*Target, Transaction), 0, Batch, State).bValid);
  TestFalse(TEXT("invalid family has no prepared state"), State.IsValid());
  TestTrue(TEXT("invalid family installs no inverse"), Batch.Contributions.IsEmpty());
 }
 return true;
}
#endif
