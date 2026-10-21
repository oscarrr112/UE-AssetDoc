// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/AssetDocumentSkeletonTestHarness.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
using namespace AssetDocumentSkeletonTests;
namespace
{
// Core package snapshot rollback (AssetDocument.CorePackageSnapshot) changes
// both contracts below; each test covers it on and off.
IConsoleVariable* FindPackageSnapshotSwitch(FAutomationTestBase& Test)
{
 IConsoleVariable* Switch = IConsoleManager::Get().FindConsoleVariable(TEXT("AssetDocument.CorePackageSnapshot"));
 Test.TestNotNull(TEXT("Core package snapshot switch exists"), Switch);
 return Switch;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonCompositionTest,
 "UEAssetDoc.SkeletonS1.Composition", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSkeletonCompositionTest::RunTest(const FString&)
{
 IConsoleVariable* Snapshot = FindPackageSnapshotSwitch(*this); if (!Snapshot) return false;
 const bool bSnapshotWasEnabled = Snapshot->GetBool();
 ON_SCOPE_EXIT { Snapshot->Set(bSnapshotWasEnabled, ECVF_SetByCode); };
 for (const bool Missing : {true, false})
 for (const bool bSnapshot : {false, true})
 {
  if (!Missing && !bSnapshot) continue;
  Snapshot->Set(bSnapshot, ECVF_SetByCode);
  FAssets F; if (!TestTrue(TEXT("Body-only baseline"), F.Setup() && F.Document(F.SetBody(), TEXT("Update"), false))) return false;
  FOptions O; O.bMissingAnimationPreparation = Missing; O.FailAfter = TEXT("skeleton");
  FHarness H(O); if (!H.Initialize(*this)) return false;
  auto Result = H.Apply(F.Sidecar);
  for (const auto& D : Result.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  TestFalse(TEXT("injected or missing preparation failure"), Result.IsSuccess());
  TestEqual(TEXT("Body-only original Skeleton restored"), F.Montage->GetSkeleton(), F.Old.Get());
  TestFalse(TEXT("Body-only Editor dirty finalizer restored"), F.Montage->GetOutermost()->IsDirty());
  if (Missing && bSnapshot)
  {
   // The package snapshot covers a provider without preparation: it runs and is rolled back.
   TestFalse(TEXT("snapshot-covered missing callback invokes mutators"), H.Invocations.IsEmpty());
   TestFalse(TEXT("snapshot-covered missing callback is not PreparationUnavailable"), Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("MutationComposition.PreparationUnavailable"); }));
  }
  else if (Missing)
  {
   TestTrue(TEXT("missing callback invokes zero mutators"), H.Invocations.IsEmpty());
   TestTrue(TEXT("missing callback stable diagnostic"), Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("MutationComposition.PreparationUnavailable"); }));
  }
  else
  {
   TSet<FString> Objects, Packages;
   for (const auto& Row : H.RawRows)
   {
    if (Row.Kind == EAssetDocumentMutationFootprintKind::UObjectState) Objects.Add(Row.StableId);
    if (Row.Kind == EAssetDocumentMutationFootprintKind::Package) Packages.Add(Row.StableId);
   }
   TestEqual(TEXT("all real contributions share one object identity"), Objects.Num(), 1);
   TestEqual(TEXT("all real contributions share one package identity"), Packages.Num(), 1);
   TestTrue(TEXT("object is canonical real target"), Objects.Contains(TEXT("Object:") + F.Target));
   TestTrue(TEXT("package is canonical real package"), Packages.Contains(TEXT("Package:") + FPackageName::ObjectPathToPackageName(F.Target)));
   TestTrue(TEXT("real Body inverse and Editor package finalizer"), H.Inverses.Contains(TEXT("animation.asset.skeleton.set.v1")) && H.Inverses.Last() == TEXT("dirty-last"));
  }
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonCompositionPoisonTest,
 "UEAssetDoc.SkeletonS1.InversePoison", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSkeletonCompositionPoisonTest::RunTest(const FString&)
{
 IConsoleVariable* Snapshot = FindPackageSnapshotSwitch(*this); if (!Snapshot) return false;
 const bool bSnapshotWasEnabled = Snapshot->GetBool();
 ON_SCOPE_EXIT { Snapshot->Set(bSnapshotWasEnabled, ECVF_SetByCode); };
 for (const bool bSnapshot : {false, true})
 {
 Snapshot->Set(bSnapshot, ECVF_SetByCode);
 FAssets F; if (!TestTrue(TEXT("poison fixture saved baseline"), F.Setup() && F.Document(F.SetBody()))) return false;
 const auto Package = Bytes(PackageFile(F.Target)); const auto Sidecar = Bytes(F.Sidecar);
 FOptions O; O.FailAfter = TEXT("skeleton"); O.SabotageOldPath = F.Old->GetPathName();
 FHarness H(O); if (!H.Initialize(*this)) return false;
 auto Result = H.Apply(F.Sidecar);
 for (const auto& D : Result.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
 TestFalse(TEXT("real inverse failure cannot succeed"), Result.IsSuccess());
 TestTrue(TEXT("domain rollback failure is preserved"), Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("AnimationProvider.Skeleton.RollbackFailed"); }));
 TestTrue(TEXT("Atomic poison is preserved"), Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("Atomic.Poisoned.RollbackFailed"); }));
 TestEqual(TEXT("other real field inverse still ran"), F.Montage->RateScale, 1.0f);
 TestFalse(TEXT("Editor finalizer still ran last"), F.Montage->GetOutermost()->IsDirty());
 if (bSnapshot)
  // The typed inverse still fails and poisons, but Core's snapshot restores last.
  TestEqual(TEXT("package snapshot restores the Skeleton after the failed inverse"), F.Montage->GetSkeleton(), F.Old.Get());
 else
  TestEqual(TEXT("failed Skeleton inverse did not pretend to restore"), F.Montage->GetSkeleton(), F.Desired.Get());
 // With Core's package snapshot the property batch is restored by the snapshot,
 // not by replaying its typed preimage.
 TestEqual(TEXT("inverse attempts followed by dirty-last"), H.Inverses, bSnapshot
  ? TArray<FString>{TEXT("animation.asset.skeleton.set.v1"), TEXT("dirty-last")}
  : TArray<FString>{TEXT("animation.asset.skeleton.set.v1"), TEXT("editor.property.write_admitted_batch.v1"), TEXT("dirty-last")});
 TestTrue(TEXT("poison leaves disk baseline unchanged"), Bytes(PackageFile(F.Target)) == Package && Bytes(F.Sidecar) == Sidecar);
 }
 return !HasAnyErrors();
}
#endif
