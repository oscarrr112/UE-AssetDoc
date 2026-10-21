// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentEditorNativeState.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

namespace
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
struct FStagedReadFixture
{
 FAssetDocumentEditorProviderTestContext Context;
 FAssetDocumentRuntimeRequestDto Request;
 FAssetDocumentHandleValidationContext Verify;
 TStrongObjectPtr<UPackage> OriginalPackage, StagedPackage;
 TStrongObjectPtr<UAnimMontage> Original, Staged;
 FString Directory, Filename;
 bool Initialize(FAutomationTestBase& Test)
 {
  const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
  OriginalPackage.Reset(CreatePackage(*(TEXT("/Temp/StagedReadOriginal_") + Id)));
  Original.Reset(NewObject<UAnimMontage>(OriginalPackage.Get(), TEXT("Target"), RF_Public | RF_Standalone));
  Original->GetController().InitializeModel(); Original->GetController().NotifyPopulated();
  Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/StagedMontageRead") / Id);
  IFileManager::Get().MakeDirectory(*Directory, true); Filename = Directory / TEXT("Target.uasset");
  FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
  if (!Test.TestTrue(TEXT("fixture saves real staged input bytes"), UPackage::SavePackage(OriginalPackage.Get(), Original.Get(), *Filename, Args))) return false;
  StagedPackage.Reset(CreatePackage(*(TEXT("/Temp/StagedReadLoaded_") + Id)));
  UPackage* Loaded = LoadPackage(StagedPackage.Get(), FPackagePath::FromLocalPath(Filename), LOAD_None);
  if (!Test.TestTrue(TEXT("fixture loads file into actual distinct stage package"), Loaded == StagedPackage.Get())) return false;
  Staged.Reset(FindObject<UAnimMontage>(Loaded, TEXT("Target")));
  if (!Test.TestTrue(TEXT("fixture has a real loaded staged Montage"), Staged.IsValid() && Staged.Get() != Original.Get())) return false;
  // The default registry query can synthesize data from this live UObject.
  // This fixture never calls AssetCreated; only disk-gathered absence is asserted.
  Test.TestFalse(TEXT("staged object has no disk-gathered registry entry"),
   FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Staged->GetPathName()), true).IsValid());
  Context.RuntimeRevision = 1;
  Request.RequestId = Context.RequestId; Request.Operation = EAssetDocumentRuntimeOperation::Apply;
  Request.ExpectedRuntimeRevision = 1; Request.TargetAssetPath = Original->GetPathName();
  Request.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
  Verify.RequestId = Context.RequestId; Verify.TransactionId = Context.TransactionId; Verify.RuntimeRevision = Context.RuntimeRevision;
  Verify.ProviderId = Context.ProviderId; Verify.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  Verify.RequiredKind = EAssetDocumentHandleKind::Asset; Verify.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
  return true;
 }
 ~FStagedReadFixture()
 {
  if (OriginalPackage.IsValid()) OriginalPackage->SetDirtyFlag(false);
  if (StagedPackage.IsValid()) StagedPackage->SetDirtyFlag(false);
  if (!Directory.IsEmpty()) IFileManager::Get().DeleteDirectory(*Directory, false, true);
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedMontageReadLifetime,
 "UEAssetDoc.Editor.StagedMontageRead.Lifetime",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStagedMontageReadLifetime::RunTest(const FString&)
{
 FStagedReadFixture F; if (!F.Initialize(*this)) return false;
 FRecordingResolver Handles(F.Context);
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 auto Scope = ResolveAssetDocumentEditorStagedMontageRead(F.Request, F.Verify, F.Staged->GetPathName(), F.Filename, Handles, Diagnostics);
 if (!TestTrue(TEXT("actual staged target without disk-gathered entry resolves readonly"), Scope.IsValid())) return false;
 TestEqual(TEXT("bootstrap issues only one readonly handle"), Handles.Issued.Num(), 1);
 const auto Handle = Scope->GetAssetReadHandle();
 TestEqual(TEXT("issued only Asset kind"), Handle.Kind, EAssetDocumentHandleKind::Asset);
 TestEqual(TEXT("issued only readonly access"), Handle.Access, EAssetDocumentHandleAccess::ReadOnly);
 TestEqual(TEXT("staged verification accepts current activated request revision"), Handle.RuntimeRevision, F.Request.ExpectedRuntimeRevision);
 const auto& Value = Scope->GetResolveValue();
 TestEqual(TEXT("staged resolve DTO is exact"), Value.ObjectKeys,
  TArray<FString>{TEXT("canonical_class"), TEXT("canonical_path"), TEXT("exists"), TEXT("package_dirty"), TEXT("package_name")});
 for (const auto& Expected : TArray<TPair<FString, FString>>{
  {TEXT("canonical_path"), F.Staged->GetPathName()}, {TEXT("canonical_class"), F.Staged->GetClass()->GetPathName()},
  {TEXT("package_name"), F.StagedPackage->GetName()}})
 {
  const int32 Index = Value.ObjectKeys.IndexOfByKey(Expected.Key);
  if (TestTrue(*FString::Printf(TEXT("actual %s field exists"), *Expected.Key), Value.Elements.IsValidIndex(Index)))
  {
   TestEqual(TEXT("identity fields retain string encoding"), Value.Elements[Index].Kind, EAssetDocumentRuntimeValueKind::String);
   TestEqual(*FString::Printf(TEXT("%s is read from actual staged target"), *Expected.Key), Value.Elements[Index].StringValue, Expected.Value);
  }
 }
 for (const auto& Expected : TArray<TPair<FString, bool>>{
  {TEXT("exists"), true}, {TEXT("package_dirty"), F.StagedPackage->IsDirty()}})
 {
  const int32 Index = Value.ObjectKeys.IndexOfByKey(Expected.Key);
  if (TestTrue(*FString::Printf(TEXT("actual %s field exists"), *Expected.Key), Value.Elements.IsValidIndex(Index)))
  {
   TestEqual(TEXT("state fields retain boolean encoding"), Value.Elements[Index].Kind, EAssetDocumentRuntimeValueKind::Boolean);
   TestEqual(*FString::Printf(TEXT("%s is read from actual staged target"), *Expected.Key), Value.Elements[Index].BooleanValue, Expected.Value);
  }
 }
 auto Acquired = Handles.AcquireVerifiedLease(Handle, F.Verify);
 if (!TestTrue(TEXT("issued readonly handle acquires real lease"), Acquired.Lease.IsValid())) return false;
 bool bObservedStaged = false;
 const auto Identity = WithAssetDocumentEditorAssetIdentity(*Acquired.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  FAssetDocumentContractValidationResult R; R.bValid = &View.GetObject() == F.Staged.Get(); bObservedStaged = R.bValid; return R;
 });
 TestTrue(TEXT("existing native identity reader reaches actual staged object"), Identity.bValid && bObservedStaged);
 auto Mutable = F.Verify; Mutable.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
 TestFalse(TEXT("readonly issuance cannot acquire mutable authority"), Handles.AcquireVerifiedLease(Handle, Mutable).Lease.IsValid());
 Scope->Close(); Scope->Close();
 TestFalse(TEXT("previously acquired payload is dead after scope closes"), Acquired.Lease->GetPayloadChecked().IsAlive());
 TestFalse(TEXT("closed scope cannot acquire another lease"), Handles.AcquireVerifiedLease(Handle, F.Verify).Lease.IsValid());
 TestFalse(TEXT("existing native reader rejects previously acquired closed scope"), WithAssetDocumentEditorAssetIdentity(*Acquired.Lease,
  [](const FAssetDocumentEditorAssetIdentityView&) { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }).bValid);
 auto Second = ResolveAssetDocumentEditorStagedMontageRead(F.Request, F.Verify, F.Staged->GetPathName(), F.Filename, Handles, Diagnostics);
 if (!TestTrue(TEXT("independent new scope opens"), Second.IsValid())) return false;
 const auto SecondHandle = Second->GetAssetReadHandle(); Second.Reset();
 TestFalse(TEXT("destruction closes scope automatically"), Handles.AcquireVerifiedLease(SecondHandle, F.Verify).Lease.IsValid());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedMontageReadRejections,
 "UEAssetDoc.Editor.StagedMontageRead.Rejections",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStagedMontageReadRejections::RunTest(const FString&)
{
 FStagedReadFixture F; if (!F.Initialize(*this)) return false;
 for (int32 Case = 0; Case < 10; ++Case)
 {
  auto Request = F.Request; auto Context = F.Verify;
  FString Path = F.Staged->GetPathName(), File = F.Filename;
  if (Case == 0) Context.RequestId = FGuid::NewGuid();
  if (Case == 1) Context.RuntimeRevision = 0;
  if (Case == 2) Context.ProviderId = TEXT("unrelated_provider");
  if (Case == 3) Context.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  if (Case == 4) Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
  if (Case == 5) Path = Request.TargetAssetPath;
  if (Case == 6) Path += TEXT("_Missing");
  if (Case == 7) File = F.Directory / TEXT("Other.uasset");
  if (Case == 8) Request.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence");
  if (Case == 9) Context.RuntimeRevision = 3; // nonzero number, wrong actual issuer revision.
  FRecordingResolver Handles(F.Context);
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  auto Rejected = ResolveAssetDocumentEditorStagedMontageRead(Request, Context, Path, File, Handles, Diagnostics);
  TestFalse(TEXT("wrong request/revision/phase/access/path/file/class rejects"), Rejected.IsValid());
  TestTrue(TEXT("rejection has an explicit diagnostic"), !Diagnostics.IsEmpty());
  if (Case == 9)
  {
   TestEqual(TEXT("mismatching issuer actually issued one handle"), Handles.Issued.Num(), 1);
   if (!Handles.Issued.IsEmpty())
    TestFalse(TEXT("failed issuance leaves no usable staged handle"), Handles.AcquireVerifiedLease(Handles.Issued[0], F.Verify).Lease.IsValid());
  }
  else TestTrue(TEXT("invalid inputs issue no handles"), Handles.Issued.IsEmpty());
 }
 return true;
}
#endif
