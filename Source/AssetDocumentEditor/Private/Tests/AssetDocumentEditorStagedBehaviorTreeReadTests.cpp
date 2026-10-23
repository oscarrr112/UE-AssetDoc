// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentEditorStagedBehaviorTreeRead.h"
#include "AssetDocumentEditorNativeState.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentStagedBTReadTests
{
class FRecordingResolver final : public IAssetDocumentHandleResolver
{
public:
 explicit FRecordingResolver(const FAssetDocumentEditorProviderTestContext& Context) : Inner(Context) {}
 TArray<FAssetDocumentOpaqueHandle> Issued;
 FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access,
  TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
 {
  auto Result = Inner.Issue(Kind, Access, MoveTemp(Payload));
  if (Result.Handle.IsSet()) Issued.Add(Result.Handle.GetValue());
  return Result;
 }
 FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& Handle,
  const FAssetDocumentHandleValidationContext& Context) override { return Inner.AcquireVerifiedLease(Handle, Context); }
 FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation& Event) override { return Inner.Invalidate(Event); }
private:
 FAssetDocumentEditorProviderTestHandleResolver Inner;
};
struct FFixture
{
 FAssetDocumentEditorProviderTestContext Context;
 FAssetDocumentRuntimeRequestDto Request;
 FAssetDocumentHandleValidationContext Verify;
 TStrongObjectPtr<UPackage> OriginalPackage, StagedPackage;
 TStrongObjectPtr<UObject> Original, Staged;
 FObjectPropertyBase* BlackboardProperty = nullptr;
 FString Directory, Filename;
 bool Initialize(FAutomationTestBase& Test)
 {
  UClass* TreeClass = FSoftClassPath(TEXT("/Script/AIModule.BehaviorTree")).TryLoadClass<UObject>();
  UClass* BlackboardClass = FSoftClassPath(TEXT("/Script/AIModule.BlackboardData")).TryLoadClass<UObject>();
  if (!Test.TestNotNull(TEXT("exact loaded BT class"), TreeClass) || !Test.TestNotNull(TEXT("exact loaded Blackboard class"), BlackboardClass)) return false;
  BlackboardProperty = FindFProperty<FObjectPropertyBase>(TreeClass, TEXT("BlackboardAsset"));
  if (!Test.TestNotNull(TEXT("public UObject property distinguishes live and saved content"), BlackboardProperty)) return false;
  const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
  OriginalPackage.Reset(CreatePackage(*(TEXT("/Temp/StagedBTOriginal_") + Id)));
  Original.Reset(NewObject<UObject>(OriginalPackage.Get(), TreeClass, TEXT("Target"), RF_Public | RF_Standalone));
  UObject* SavedBlackboard = NewObject<UObject>(OriginalPackage.Get(), BlackboardClass, TEXT("SavedBlackboard"), RF_Public);
  BlackboardProperty->SetObjectPropertyValue_InContainer(Original.Get(), SavedBlackboard);
  Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/StagedBehaviorTreeRead") / Id);
  IFileManager::Get().MakeDirectory(*Directory, true); Filename = Directory / TEXT("Target.uasset");
  FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
  if (!Test.TestTrue(TEXT("real BT package bytes saved"), UPackage::SavePackage(OriginalPackage.Get(), Original.Get(), *Filename, Args))) return false;
  // Change live after save. The staged object must retain the saved reference.
  BlackboardProperty->SetObjectPropertyValue_InContainer(Original.Get(), nullptr);
  StagedPackage.Reset(CreatePackage(*(TEXT("/Temp/StagedBTLoaded_") + Id)));
  UPackage* Loaded = LoadPackage(StagedPackage.Get(), FPackagePath::FromLocalPath(Filename), LOAD_ForDiff);
  if (!Test.TestTrue(TEXT("actual distinct package loads for diff"), Loaded == StagedPackage.Get())) return false;
  Staged.Reset(FindObject<UObject>(Loaded, TEXT("Target")));
  if (!Test.TestTrue(TEXT("staged exact BT differs from live"), Staged.IsValid() && Staged.Get() != Original.Get() && Staged->GetClass() == TreeClass)) return false;
  if (!Test.TestTrue(TEXT("saved staged Blackboard differs from changed live"), BlackboardProperty->GetObjectPropertyValue_InContainer(Staged.Get()) != nullptr
   && BlackboardProperty->GetObjectPropertyValue_InContainer(Original.Get()) == nullptr)) return false;
  Context.RuntimeRevision = 7;
  Request.RequestId = Context.RequestId; Request.Operation = EAssetDocumentRuntimeOperation::Apply;
  Request.ExpectedRuntimeRevision = 3; Request.TargetAssetPath = Original->GetPathName();
  Request.RequestedAssetClassPath = TEXT("/Script/AIModule.BehaviorTree");
  Verify.RequestId = Context.RequestId; Verify.TransactionId = Context.TransactionId; Verify.RuntimeRevision = Context.RuntimeRevision;
  Verify.ProviderId = Context.ProviderId; Verify.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  Verify.RequiredKind = EAssetDocumentHandleKind::Asset; Verify.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
  return true;
 }
 ~FFixture()
 {
  if (Original.IsValid()) Original->ClearFlags(RF_Standalone);
  if (Staged.IsValid()) Staged->ClearFlags(RF_Standalone);
  if (OriginalPackage.IsValid()) OriginalPackage->SetDirtyFlag(false);
  if (StagedPackage.IsValid()) StagedPackage->SetDirtyFlag(false);
  if (!Directory.IsEmpty()) IFileManager::Get().DeleteDirectory(*Directory, false, true);
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedBehaviorTreeReadLifetime,
 "UEAssetDoc.Editor.StagedBehaviorTreeRead.ActualSavedContentAndLifetime",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStagedBehaviorTreeReadLifetime::RunTest(const FString&)
{
 using namespace AssetDocumentStagedBTReadTests;
 FFixture F; if (!F.Initialize(*this)) return false;
 FRecordingResolver Handles(F.Context); TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 auto Scope = ResolveAssetDocumentEditorStagedBehaviorTreeRead(F.Request, F.Verify, F.Staged->GetPathName(), F.Filename, 7, Handles, Diagnostics);
 if (!TestTrue(TEXT("actual staged BT issues readonly lease"), Scope.IsValid())) return false;
 TestEqual(TEXT("one readonly asset issuance"), Handles.Issued.Num(), 1);
 TestEqual(TEXT("Verify uses its bound fresh revision"), Scope->GetAssetReadHandle().RuntimeRevision, uint64(7));
 TestTrue(TEXT("scope current"), Scope->IsCurrent());
 auto Acquired = Handles.AcquireVerifiedLease(Scope->GetAssetReadHandle(), F.Verify);
 if (!TestTrue(TEXT("real Editor identity lease acquired"), Acquired.Lease.IsValid())) return false;
 const auto Read = WithAssetDocumentEditorAssetIdentity(*Acquired.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  FAssetDocumentContractValidationResult R;
  R.bValid = &View.GetObject() == F.Staged.Get() && &View.GetObject() != F.Original.Get()
   && F.BlackboardProperty->GetObjectPropertyValue_InContainer(&View.GetObject()) != nullptr;
  return R;
 });
 TestTrue(TEXT("identity reader sees persisted staged value, not differing live value"), Read.bValid);
 auto Mutable = F.Verify; Mutable.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
 TestFalse(TEXT("readonly payload cannot grant mutation"), Handles.AcquireVerifiedLease(Scope->GetAssetReadHandle(), Mutable).Lease.IsValid());
 const auto Handle = Scope->GetAssetReadHandle(); Scope->Close(); Scope->Close();
 TestFalse(TEXT("closed query is false"), Scope->IsCurrent());
 TestFalse(TEXT("previous acquired payload expires"), Acquired.Lease->GetPayloadChecked().IsAlive());
 TestFalse(TEXT("closed issuance cannot reacquire"), Handles.AcquireVerifiedLease(Handle, F.Verify).Lease.IsValid());
 TestFalse(TEXT("previous identity callback fails closed"), WithAssetDocumentEditorAssetIdentity(*Acquired.Lease,
  [](const FAssetDocumentEditorAssetIdentityView&) { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }).bValid);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedBehaviorTreeReadRejections,
 "UEAssetDoc.Editor.StagedBehaviorTreeRead.Rejections",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStagedBehaviorTreeReadRejections::RunTest(const FString&)
{
 using namespace AssetDocumentStagedBTReadTests;
 FFixture F; if (!F.Initialize(*this)) return false;
 for (int32 Case = 0; Case < 10; ++Case)
 {
  auto Request = F.Request; auto Verify = F.Verify;
  FString Path = F.Staged->GetPathName(), File = F.Filename;
  if (Case == 0) Verify.RequestId = FGuid::NewGuid();
  if (Case == 1) Verify.RuntimeRevision = 8;
  if (Case == 2) Verify.ProviderId = TEXT("ai");
  if (Case == 3) Verify.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  if (Case == 4) Verify.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
  if (Case == 5) Path = Request.TargetAssetPath;
  if (Case == 6) Path += TEXT("_Missing");
  if (Case == 7) File = F.Directory / TEXT("Other.uasset");
  if (Case == 8) Request.RequestedAssetClassPath = TEXT("/Script/AIModule.BlackboardData");
  if (Case == 9) F.StagedPackage->ClearPackageFlags(PKG_ForDiffing);
  FRecordingResolver Handles(F.Context); TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  auto Scope = ResolveAssetDocumentEditorStagedBehaviorTreeRead(Request, Verify, Path, File, 7, Handles, Diagnostics);
  TestFalse(TEXT("wrong request/revision/provider/phase/access/live/path/file/class/load lane rejects"), Scope.IsValid());
  TestTrue(TEXT("invalid inputs issue no payload"), Handles.Issued.IsEmpty());
  TestFalse(TEXT("rejection has diagnostic"), Diagnostics.IsEmpty());
  if (Case == 9) F.StagedPackage->SetPackageFlags(PKG_ForDiffing);
 }
 return true;
}
#endif
