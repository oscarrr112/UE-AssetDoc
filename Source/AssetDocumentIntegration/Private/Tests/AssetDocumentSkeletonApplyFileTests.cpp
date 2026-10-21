// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/AssetDocumentSkeletonTestHarness.h"
#include "AssetDocumentTestDirectoryBlock.h"
#include "AssetDocumentGenericRequestAdapter.h"
#include "AssetDocumentGenericReadOnlyDispatcher.h"
#include "HAL/PlatformProcess.h"
#include "UObject/ObjectSaveContext.h"
#include "Misc/ScopeExit.h"
using namespace AssetDocumentSkeletonTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonApplyFileTest,
 "UEAssetDoc.SkeletonS1.ApplyFile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSkeletonApplyFileTest::RunTest(const FString&)
{
 for (const bool bClear : {false, true})
 {
  FAssets F; if (!TestTrue(TEXT("saved original package and both Skeleton packages"), F.Setup())) return false;
  const auto OldBytes = Bytes(PackageFile(F.Old->GetPathName()));
  const auto DesiredBytes = Bytes(PackageFile(F.Desired->GetPathName()));
  const FString AliasBody = FString::Printf(TEXT("{\"Skeleton\":null,\"References\":{\"Skeleton\":\"%s\"}}"), *F.Desired->GetPathName());
  if (!TestTrue(TEXT("actual sidecar exists"), F.Document(bClear ? TEXT("{\"Skeleton\":null}") : AliasBody))) return false;
  FHarness H; if (!H.Initialize(*this)) return false;
  FAssetDocumentApplyFileRequest CanonicalRequest; CanonicalRequest.FilePath = F.Sidecar;
  TArray<FAssetDocumentRuntimeDiagnostic> CanonicalDiagnostics;
  const auto BaselineDto = FAssetDocumentGenericRequestAdapter::AdaptApplyFile(CanonicalRequest, H.Runtime->GetActiveRuntimeRevision(), CanonicalDiagnostics);
  if (!TestTrue(TEXT("authored sidecar adapts"), BaselineDto.IsSet())) return false;
  const auto CanonicalBaseline = FAssetDocumentGenericReadOnlyDispatcher::DispatchValidate(*H.Runtime, BaselineDto.GetValue());
  if (!TestEqual(TEXT("authored canonical validation"), CanonicalBaseline.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded)) return false;
  auto Result = H.Apply(F.Sidecar);
  for (const auto& D : Result.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  if (!TestTrue(TEXT("real sidecar ApplyFile completes"), Result.IsSuccess())) return false;
  TestEqual(TEXT("both aliases yield exactly one native Skeleton invocation"), H.Invocations.FilterByPredicate([](const auto& Id) { return Id == TEXT("animation.asset.skeleton.set.v1"); }).Num(), 1);
  TestEqual(TEXT("real Skeleton pointer readback"), F.Montage->GetSkeleton(), bClear ? nullptr : F.Desired.Get());
  TestEqual(TEXT("real Skeleton GUID readback"), F.Montage->GetSkeletonGuid(), bClear ? FGuid() : F.Desired->GetGuid());
  TestEqual(TEXT("real admitted RateScale scalar"), F.Montage->RateScale, 2.0f);
  TestFalse(TEXT("successful persisted target clean"), F.Montage->GetOutermost()->IsDirty());
  const auto CanonicalDto = FAssetDocumentGenericRequestAdapter::AdaptApplyFile(CanonicalRequest, H.Runtime->GetActiveRuntimeRevision(), CanonicalDiagnostics);
  if (!TestTrue(TEXT("persisted canonical sidecar adapts"), CanonicalDto.IsSet())) return false;
  const auto VerifiedSidecar = FAssetDocumentGenericReadOnlyDispatcher::DispatchValidate(*H.Runtime, CanonicalDto.GetValue());
  TestEqual(TEXT("persisted canonical sidecar validates"), VerifiedSidecar.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
  TestEqual(TEXT("persisted canonical document equals authored intent"), VerifiedSidecar.CanonicalBytes, CanonicalBaseline.CanonicalBytes);
  TestTrue(TEXT("old Skeleton package unchanged"), Bytes(PackageFile(F.Old->GetPathName())) == OldBytes);
  TestTrue(TEXT("desired Skeleton package unchanged"), Bytes(PackageFile(F.Desired->GetPathName())) == DesiredBytes);
  TestFalse(TEXT("old Skeleton package remains clean"), F.Old->GetOutermost()->IsDirty());
  TestFalse(TEXT("desired Skeleton package remains clean"), F.Desired->GetOutermost()->IsDirty());
  const auto* SkeletonChange = H.Changes.FindByPredicate([](const auto& C) { return C.AuthorityId == TEXT("animation.asset.skeleton.set.v1"); });
  if (TestNotNull(TEXT("one Skeleton change"), SkeletonChange))
  {
   TestEqual(TEXT("change exact object"), SkeletonChange->StableObjectId, F.Target);
   TestEqual(TEXT("change exact path"), SkeletonChange->Path, FString(TEXT("/Skeleton")));
   TestEqual(TEXT("change exact kind"), SkeletonChange->ChangeKind, FString(bClear ? TEXT("Clear") : TEXT("Set")));
  }
  TestTrue(TEXT("actual nonempty SourceDocumentPath ingress"), H.SourcePaths == TArray<FString>{F.Sidecar});
  const auto SkeletonChangeCount = [&] { return H.Changes.FilterByPredicate([](const auto& C) { return C.AuthorityId == TEXT("animation.asset.skeleton.set.v1"); }).Num(); };
  const int32 Before = SkeletonChangeCount();
  auto Again = H.Apply(F.Sidecar);
  for (const auto& D : Again.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  TestTrue(TEXT("both property and Skeleton no-op consumed and persisted"), Again.IsSuccess());
  TestEqual(TEXT("Skeleton no-op produces no additional Skeleton ChangedSet"), SkeletonChangeCount(), Before);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonApplyFileRollbackTest,
 "UEAssetDoc.SkeletonS1.ApplyFileRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSkeletonApplyFileRollbackTest::RunTest(const FString&)
{
 for (const FString Order : {TEXT("properties_first"), TEXT("skeleton_first")})
 for (const FString Failure : {TEXT("properties"), TEXT("skeleton"), TEXT("repair"), TEXT("verify")})
 {
  FAssets F; if (!TestTrue(TEXT("rollback saved baseline"), F.Setup() && F.Document(F.SetBody()))) return false;
  const auto Package = Bytes(PackageFile(F.Target)); const auto Sidecar = Bytes(F.Sidecar);
  const auto OldGuid = F.Montage->GetSkeletonGuid();
  FOptions O; O.Order = Order; O.FailAfter = Failure;
  FHarness H(O); if (!H.Initialize(*this)) return false;
  auto Result = H.Apply(F.Sidecar);
  for (const auto& D : Result.Diagnostics) AddInfo(Order + TEXT("/") + Failure + TEXT(": ") + D.Code + TEXT(": ") + D.Message);
  TestFalse(TEXT("injected failure fails ApplyFile"), Result.IsSuccess());
  TestEqual(TEXT("strong target identity restored"), FindObject<UAnimMontage>(nullptr, *F.Target), F.Montage.Get());
  TestEqual(TEXT("strong old Skeleton restored after GC"), F.Montage->GetSkeleton(), F.Old.Get());
  TestEqual(TEXT("old GUID restored"), F.Montage->GetSkeletonGuid(), OldGuid);
  TestEqual(TEXT("real property inverse restored"), F.Montage->RateScale, 1.0f);
  TestFalse(TEXT("dirty restored last"), F.Montage->GetOutermost()->IsDirty());
  // The generic property batch has no typed replay under Core's package snapshot
  // (the snapshot restores those fields); domain inverses still run in reverse order.
  TArray<FString> ExpectedInverse;
  for (int32 I = H.Invocations.Num() - 1; I >= 0; --I)
   if (H.Invocations[I] != TEXT("editor.property.write_admitted_batch.v1")) ExpectedInverse.Add(H.Invocations[I]);
  ExpectedInverse.Add(TEXT("dirty-last"));
  TestEqual(TEXT("actual inverse order is reverse arm order with Editor dirty-last"), H.Inverses, ExpectedInverse);
  TestTrue(TEXT("package bytes restored symmetrically"), Bytes(PackageFile(F.Target)) == Package);
  TestTrue(TEXT("sidecar bytes restored symmetrically"), Bytes(F.Sidecar) == Sidecar);
  TestTrue(TEXT("real mutation occurred before injected failure"), !H.Changes.IsEmpty());
  TestTrue(TEXT("rollback invalidates handles"), H.Invalidations.ContainsByPredicate([](const auto& E) { return E.Reason == EAssetDocumentHandleInvalidationReason::Rollback; }));
  TestFalse(TEXT("failed request cannot commit"), H.Invalidations.ContainsByPredicate([](const auto& E) { return E.Reason == EAssetDocumentHandleInvalidationReason::Commit; }));
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonCreateApplyFileTest,
 "UEAssetDoc.SkeletonS1.CreateApplyFile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSkeletonCreateApplyFileTest::RunTest(const FString&)
{
 for (const bool bClear : {false, true})
 for (const FString Failure : {TEXT("factory"), TEXT("resolve"), TEXT("properties"), TEXT("handoff"), TEXT("skeleton"), TEXT("repair"), TEXT("verify"), TEXT("created_payload"), TEXT("")})
 {
  if (bClear && !Failure.IsEmpty()) continue;
  FAssets F; if (!TestTrue(TEXT("create references and sidecar prepared"), F.Setup(false) && F.Document(bClear ? TEXT("{\"Skeleton\":null}") : F.SetBody(), TEXT("CreateOrUpdate"), Failure != TEXT("created_payload")))) return false;
  const auto Sidecar = Bytes(F.Sidecar);
  FOptions O; O.FactorySkeleton = F.Old->GetPathName(); O.FailAfter = Failure;
  FHarness H(O); if (!H.Initialize(*this)) return false;
  auto Result = H.Apply(F.Sidecar);
  for (const auto& D : Result.Diagnostics) AddInfo(TEXT("Create/") + Failure + TEXT(": ") + D.Code + TEXT(": ") + D.Message);
  const bool Success = Failure.IsEmpty();
  TestEqual(TEXT("Create terminal"), Result.IsSuccess(), Success);
  UAnimMontage* Created = FindObject<UAnimMontage>(nullptr, *F.Target);
  if (Success)
  {
   if (TestNotNull(TEXT("Create real target"), Created))
   {
    TestEqual(TEXT("factory then real Skeleton setter"), Created->GetSkeleton(), bClear ? nullptr : F.Desired.Get());
    TestEqual(TEXT("created GUID"), Created->GetSkeletonGuid(), bClear ? FGuid() : F.Desired->GetGuid());
    TestEqual(TEXT("created property"), Created->RateScale, 2.0f);
    TestFalse(TEXT("created persisted package clean"), Created->GetOutermost()->IsDirty());
   }
   TestTrue(TEXT("created package durable"), IFileManager::Get().FileExists(*PackageFile(F.Target)));
  }
  else
  {
   if (Failure == TEXT("created_payload"))
   {
    TestTrue(TEXT("real Created payload rejected by native identity view"), Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("EditorNativeState.AssetPayloadTypeMismatch"); }));
    TestFalse(TEXT("Created payload cannot produce Skeleton ChangedSet"), H.Changes.ContainsByPredicate([](const auto& C) { return C.AuthorityId == TEXT("animation.asset.skeleton.set.v1"); }));
   }
   TestNull(TEXT("no object ghost"), Created);
   TestNull(TEXT("no package ghost"), FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(F.Target)));
   TestFalse(TEXT("no registry ghost"), IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(F.Target)).IsValid());
   TestFalse(TEXT("no package file ghost"), IFileManager::Get().FileExists(*PackageFile(F.Target)));
   TestTrue(TEXT("preexisting input sidecar preserved"), Bytes(F.Sidecar) == Sidecar);
   TestTrue(TEXT("factory failure invalidates request handles"), H.Invalidations.ContainsByPredicate([](const auto& E) { return E.Reason == EAssetDocumentHandleInvalidationReason::Rollback; }));
  }
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonPersistenceFailureTest,
 "UEAssetDoc.SkeletonS1.PersistenceFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSkeletonPersistenceFailureTest::RunTest(const FString&)
{
 for (const bool bCreate : {false, true}) for (const bool bInstall : {false, true})
 {
  FAssets F;
  if (!TestTrue(TEXT("persistence failure baseline"), F.Setup(!bCreate) && F.Document(F.SetBody(), bCreate ? TEXT("Create") : TEXT("Update")))) return false;
  const auto Package = Bytes(PackageFile(F.Target)); const auto Sidecar = Bytes(F.Sidecar);
  FOptions O; O.FactorySkeleton = F.Old->GetPathName();
  FHarness H(O); if (!H.Initialize(*this)) return false;
  const FString BlockedRoot = bInstall ? FPaths::GetPath(F.Sidecar) : FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocument/Runtime/Staging"));
  IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
  Files.CreateDirectoryTree(*BlockedRoot);
  const bool WasReadOnly = Files.IsReadOnly(*BlockedRoot);
  bool Injected = false;
  FDelegateHandle SavedHook;
  ON_SCOPE_EXIT
  {
   if (SavedHook.IsValid()) UPackage::PackageSavedWithContextEvent.Remove(SavedHook);
   if (!WasReadOnly) AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *BlockedRoot, false);
  };
  if (bInstall)
  {
   SavedHook = UPackage::PackageSavedWithContextEvent.AddLambda([&](const FString& Filename, UPackage* PackageObject, FObjectPostSaveContext)
   {
    if (PackageObject && PackageObject->GetName() == FPackageName::ObjectPathToPackageName(F.Target) && Filename.Contains(TEXT("Staging")))
     Injected = AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *BlockedRoot, true);
   });
  }
  else Injected = AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *BlockedRoot, true);
  const auto Result = H.Apply(F.Sidecar);
  // Restore permissions immediately after the real persistence/rollback terminal.
  if (SavedHook.IsValid()) { UPackage::PackageSavedWithContextEvent.Remove(SavedHook); SavedHook.Reset(); }
  if (!WasReadOnly) AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *BlockedRoot, false);
  for (const auto& D : Result.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  TestTrue(TEXT("filesystem failure injection occurred"), Injected);
  TestFalse(TEXT("real stage/install failure fails ApplyFile"), Result.IsSuccess());
  TestTrue(TEXT("mutation preceded persistence failure"), !H.Changes.IsEmpty());
  TestFalse(TEXT("persistence failure cannot Commit"), H.Invalidations.ContainsByPredicate([](const auto& E) { return E.Reason == EAssetDocumentHandleInvalidationReason::Commit; }));
  if (bInstall && !bCreate)
   TestTrue(TEXT("persistent filesystem obstruction preserves recovery Poisoned"), Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("Atomic.Poisoned.RecoveryFailed"); }));
  TestTrue(TEXT("package bytes symmetric after persistence failure"), Bytes(PackageFile(F.Target)) == Package);
  TestTrue(TEXT("sidecar bytes symmetric after persistence failure"), Bytes(F.Sidecar) == Sidecar);
  if (bCreate)
  {
   TestNull(TEXT("failed create leaves no live target"), FindObject<UObject>(nullptr, *F.Target));
   TestFalse(TEXT("failed create leaves no registry ghost"), IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(F.Target)).IsValid());
  }
  else
  {
   TestEqual(TEXT("existing Skeleton inverse restored"), F.Montage->GetSkeleton(), F.Old.Get());
   TestEqual(TEXT("existing property inverse restored"), F.Montage->RateScale, 1.0f);
   TestFalse(TEXT("existing dirty restored"), F.Montage->GetOutermost()->IsDirty());
  }
 }
 return !HasAnyErrors();
}
#endif
