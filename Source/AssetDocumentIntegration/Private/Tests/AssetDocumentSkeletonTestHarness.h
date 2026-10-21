// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentGenericRuntimeRoute.h"
#include "../../../AssetDocument/Private/Tests/AssetDocumentSidecarWriteTestSupport.h"
#include "AssetDocumentRuntimeTargetActivation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentEditorMutationPreparation.h"
#include "AssetDocumentAnimationPrimitiveProvider.h"
#include "AssetDocumentAnimationMutationPreparation.h"
#include "AssetDocumentPythonRuntime.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "AssetDocumentPluginPaths.h"

namespace AssetDocumentSkeletonTests
{
constexpr uint64 Revision = 61;
inline void Diagnostics(FAutomationTestBase& Test, TConstArrayView<FAssetDocumentRuntimeDiagnostic> Items)
{ for (const auto& D : Items) Test.AddInfo(D.Code + TEXT(": ") + D.Message); }
inline TArray<uint8> Bytes(const FString& Path)
{ TArray<uint8> Result; FFileHelper::LoadFileToArray(Result, *Path); return Result; }
inline FString PackageFile(const FString& Path)
{ return FPackageName::LongPackageNameToFilename(FPackageName::ObjectPathToPackageName(Path), FPackageName::GetAssetPackageExtension()); }
inline bool Save(UObject* Asset)
{
 FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
 const FString Path = PackageFile(Asset->GetPathName());
 IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
 return UPackage::SavePackage(Asset->GetOutermost(), Asset, *Path, Args);
}
struct FAssets
{
 FString Root = TEXT("/Game/AssetDocumentS1/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 FString Target = Root + TEXT("/Montage.Montage");
 FString Sidecar = FPackageName::LongPackageNameToFilename(Root + TEXT("/Montage"), TEXT(".assetdoc.json"));
 TStrongObjectPtr<USkeleton> Old;
 TStrongObjectPtr<USkeleton> Desired;
 TStrongObjectPtr<UAnimMontage> Montage;
 bool bKeep = false;
 bool Setup(bool bExisting = true)
 {
  // Test data construction precedes all production preparation and snapshots.
  auto Skeleton = [&](const TCHAR* Name)
  {
   auto* S = NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone | RF_Transactional);
   S->RegisterSlotNode(TEXT("DefaultSlot")); FAssetRegistryModule::AssetCreated(S); return S;
  };
  Old.Reset(Skeleton(TEXT("Old"))); Desired.Reset(Skeleton(TEXT("Desired")));
  if (!Save(Old.Get()) || !Save(Desired.Get())) return false;
  if (bExisting)
  {
   Montage.Reset(NewObject<UAnimMontage>(CreatePackage(*(Root + TEXT("/Montage"))), TEXT("Montage"), RF_Public | RF_Standalone | RF_Transactional));
   Montage->SetSkeleton(Old.Get()); Montage->RateScale = 1.0f;
   FAssetRegistryModule::AssetCreated(Montage.Get());
   if (!Save(Montage.Get())) return false;
  }
  return true;
 }
 bool Document(const FString& Body, const TCHAR* Action = TEXT("Update"), bool bProperties = true)
 {
  IFileManager::Get().MakeDirectory(*FPaths::GetPath(Sidecar), true);
  return FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Class\":\"/Script/Engine.AnimMontage\",\"Target\":\"%s\",\"Action\":\"%s\",%s\"Body\":%s}"),
   *FPackageName::ObjectPathToPackageName(Target), Action, bProperties ? TEXT("\"Properties\":{\"RateScale\":2.0},") : TEXT(""), *Body), *Sidecar);
 }
 FString SetBody() const { return FString::Printf(TEXT("{\"Skeleton\":\"%s\"}"), *Desired->GetPathName()); }
 ~FAssets()
 {
  if (bKeep) return;
  IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
  TArray<UObject*> Assets{FindObject<UObject>(nullptr, *Target), Desired.Get(), Old.Get()};
  Montage.Reset(); Desired.Reset(); Old.Reset();
  for (UObject* Asset : Assets) if (IsValid(Asset))
  {
   auto* Package = Asset->GetOutermost(); FAssetRegistryModule::AssetDeleted(Asset);
   Asset->ClearFlags(RF_Public | RF_Standalone); Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional); Asset->MarkAsGarbage();
   Package->SetDirtyFlag(false); Package->MarkAsGarbage();
  }
  IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
 }
};
class FCancellation final : public IAssetDocumentCancellationView
{ bool IsCancellationRequested() const override { return false; } };
class FObserver final : public IAssetDocumentRequestObserver
{
public:
 TArray<EAssetDocumentRuntimeRequestPhase> Phases;
 void OnPhaseChanged(FGuid, EAssetDocumentRuntimeRequestPhase Phase) override { Phases.Add(Phase); }
};
class FRegistry final : public IAssetDocumentPrimitiveRegistry
{
 const IAssetDocumentPrimitiveRegistry& Inner;
public:
 explicit FRegistry(const IAssetDocumentPrimitiveRegistry& R) : Inner(R) {}
 FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const override { return Inner.Snapshot(); }
 FAssetDocumentPrimitiveResolution Resolve(const FAssetDocumentPrimitiveRequirement& R) const override { return Inner.Resolve(R); }
};
class FObservedInverse final : public IAssetDocumentRollbackAction
{
 TSharedPtr<IAssetDocumentRollbackAction> Inner; TArray<FString>& Trace; FString Primitive;
public:
 FObservedInverse(TSharedPtr<IAssetDocumentRollbackAction> A, TArray<FString>& T, FString P) : Inner(MoveTemp(A)), Trace(T), Primitive(MoveTemp(P)) {}
 FStringView GetRollbackComponentId() const override { return Inner->GetRollbackComponentId(); }
 FAssetDocumentContractValidationResult Rollback(FStringView Code) override { Trace.Add(Primitive); return Inner->Rollback(Code); }
};
class FObservedFinalizer final : public IAssetDocumentPostRollbackFinalizer
{
 TSharedPtr<IAssetDocumentPostRollbackFinalizer> Inner; TArray<FString>& Trace;
public:
 FObservedFinalizer(TSharedPtr<IAssetDocumentPostRollbackFinalizer> A, TArray<FString>& T) : Inner(MoveTemp(A)), Trace(T) {}
 FStringView GetRollbackComponentId() const override { return Inner->GetRollbackComponentId(); }
 FAssetDocumentContractValidationResult FinalizePostRollback(FStringView Code) override { Trace.Add(TEXT("dirty-last")); return Inner->FinalizePostRollback(Code); }
};
struct FOptions
{
 FString Order = TEXT("properties_first");
 FString FactorySkeleton;
 FString FailAfter;
 bool bMissingAnimationPreparation = false;
 bool bMontageNotifyRequestProvider = false;
 FString SabotageOldPath;
 // Optional test-owned caller extension; empty defaults preserve S1 exactly.
 TArray<FAssetDocumentPrimitiveRequirement> ExtraRequirements;
 FString EntrySuffix;
};
class FHarness
{
public:
 FString Temp = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentS1"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
 FOptions Options;
 TUniquePtr<IAssetDocumentPythonRuntime> Runtime;
 TSharedPtr<FAssetDocumentGenericRuntimeRoute> Route;
 TUniquePtr<FAssetDocumentCoreRuntimeComposition> Core;
 TUniquePtr<FRegistry> Registry;
 TUniquePtr<FAssetDocumentService> Service;
 TSharedPtr<const FAssetDocumentRequiredPrimitiveProjection> Projection;
 FCancellation Cancellation;
 FObserver Observer;
 TArray<FString> Invocations;
 TArray<FString> Inverses;
 TArray<FAssetDocumentMutationFootprint> RawRows;
 TArray<FAssetDocumentChangedRecord> Changes;
 TArray<FAssetDocumentHandleInvalidation> Invalidations;
 TArray<FString> SourcePaths;
 explicit FHarness(FOptions InOptions = {}) : Options(MoveTemp(InOptions)) {}
 ~FHarness()
 {
  if (Service && Route) Service->UnbindGenericRuntimeRoute(Route.ToSharedRef());
  if (Route) Route->UnbindComposition();
  Core.Reset(); Registry.Reset(); Service.Reset(); Route.Reset(); Projection.Reset();
  if (Runtime) Runtime->ShutdownPythonBinding(); Runtime.Reset();
  IFileManager::Get().DeleteDirectory(*Temp, false, true);
 }
 bool Initialize(FAutomationTestBase& Test)
 {
  IFileManager::Get().MakeDirectory(*Temp, true);
  // Each test revision has its own entry module; the repository fixture is
  // first-party Python, and all native providers remain the real production ones.
  const FString Plugin = FPaths::ConvertRelativePathToFull(GetAssetDocumentPluginBaseDir());
  const FString PythonRoot = FPaths::Combine(Temp, TEXT("Python"));
  if (!FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(*PythonRoot, *FPaths::Combine(Plugin, TEXT("Content/Python")), false)) return false;
  FString Entry = FString::Printf(TEXT("import importlib.util, sys\nspec = importlib.util.spec_from_file_location('_s1caller', __file__.rsplit('/', 1)[0] + '/asset_document/tests/fixtures/skeleton_s1_caller.py')\ncaller = importlib.util.module_from_spec(spec)\nsys.modules['_s1caller'] = caller\nspec.loader.exec_module(caller)\ndispatch = caller.configure(order='%s', factory_skeleton_path=%s, fail_after=%s)\n"),
   *Options.Order, Options.FactorySkeleton.IsEmpty() ? TEXT("None") : *(TEXT("'") + Options.FactorySkeleton + TEXT("'")), Options.FailAfter.IsEmpty() ? TEXT("None") : *(TEXT("'") + Options.FailAfter + TEXT("'")));
  Entry += Options.EntrySuffix;
  if (!FFileHelper::SaveStringToFile(Entry, *FPaths::Combine(PythonRoot, TEXT("s1_entry.py")))) return false;
  FAssetDocumentPythonRuntimeConfiguration Config; Config.RepositoryRoot = Temp; Config.PackageRootRelativePath = TEXT("Python");
  Runtime = CreateAssetDocumentPythonRuntime(Config);
  auto Binding = Runtime->InitializePythonBinding(); Diagnostics(Test, Binding.Diagnostics);
  if (!Test.TestTrue(TEXT("S1 real Python initialized"), Binding.bValid)) return false;
  Route = MakeShared<FAssetDocumentGenericRuntimeRoute>(*Runtime);
  TSharedPtr<IAssetDocumentAnimationRequestProvider> NotifyOwner;
  if (Options.bMontageNotifyRequestProvider) NotifyOwner = CreateAssetDocumentAnimationRequestProvider();
  TSharedRef<IAssetDocumentPrimitiveProvider> Animation = NotifyOwner.IsValid()
   ? StaticCastSharedRef<IAssetDocumentPrimitiveProvider>(NotifyOwner.ToSharedRef()) : CreateAssetDocumentAnimationPrimitiveProvider();
  TArray<TSharedRef<IAssetDocumentPrimitiveProvider>> Providers{CreateAssetDocumentEditorPrimitiveProvider(), Animation};
  TArray<FAssetDocumentPrimitiveRequirement> Seed;
  for (const auto& Id : {TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"), TEXT("editor.property.enumerate_admitted.v1"), TEXT("editor.property.enumerate_admitted_batch.v1"), TEXT("editor.property.preflight_admitted.v1"), TEXT("editor.property.read_admitted.v1"), TEXT("editor.property.write_admitted_batch.v1"), TEXT("editor.property.inspect_evidence.v1")})
   Seed.Add({Id, 1, TEXT("route.generic.v1")});
  Seed.Add({TEXT("animation.asset.skeleton.set.v1"), 1, TEXT("animation_profile")});
  Seed.Append(Options.ExtraRequirements);
  // AnimMontage shares the class-neutral generic factory lifecycle, owned by the caller's route.
  if (const auto* L = FindAssetDocumentEditorTargetLifecycle(TEXT("/Script/Engine.AnimMontage")))
   for (auto Requirement : L->RequiredPrimitives) { Requirement.RequiredByRouteId = TEXT("animation_profile"); Seed.Add(Requirement); }
  auto Capture = [this, NotifyOwner](const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents, FAssetDocumentPreparedMutationBatch& Batch, bool bEditor)
  {

   auto R = bEditor ? PrepareAssetDocumentEditorMutationBatch(Request, Intents, Batch)
    : NotifyOwner.IsValid() ? NotifyOwner->Prepare(Request, Intents, Batch) : PrepareAssetDocumentAnimationMutationBatch(Request, Intents, Batch);
   if (R.bValid)
   {
    for (auto& C : Batch.Contributions)
    {
     RawRows.Append(C.RawFootprint);
     FString Id; for (const auto& I : Intents) if (I.StepIndex == C.StepIndex) Id = I.Requirement.PrimitiveId;
     if (C.InitialAction) C.InitialAction = MakeShared<FObservedInverse>(C.InitialAction, Inverses, Id);
    }
    if (Batch.PackageFinalizer) Batch.PackageFinalizer = MakeShared<FObservedFinalizer>(Batch.PackageFinalizer, Inverses);
   }
   return R;
  };
  TArray<FAssetDocumentMutationPreparationBinding> Bindings;
  Bindings.Add({Providers[0], [Capture](const auto& R, auto I, auto& B) { return Capture(R, I, B, true); }});
  if (!Options.bMissingAnimationPreparation) Bindings.Add({Providers[1], [Capture](const auto& R, auto I, auto& B) { return Capture(R, I, B, false); }});
  auto Built = BuildAssetDocumentRequiredPrimitiveProjection(Providers, Seed, GetG1FrozenAuthorityExceptionRecords(), Bindings,
   [this](const FString& Provider, const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& H, IAssetDocumentAtomicTransaction* T, TFunctionRef<FAssetDocumentPrimitiveOutcome()> Forward)
   {
    return Route->InvokePreparedMutation(Provider, I, H, T, [&]()
    {
     Invocations.Add(I.PrimitiveId); auto O = Forward(); Changes.Append(O.ChangedSet);
     if (!Options.SabotageOldPath.IsEmpty() && I.PrimitiveId == TEXT("animation.asset.skeleton.set.v1"))
     {
      // Deliberate test-only drift makes the real domain inverse fail its old
      // identity guard; no fake action or diagnostic substitutes for it.
      if (auto* Old = FindObject<USkeleton>(nullptr, *Options.SabotageOldPath)) Old->Rename(TEXT("InverseIdentityDrift"), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
     }
     return O;
    });
   });
  Diagnostics(Test, Built.Diagnostics);
  if (!Test.TestTrue(TEXT("S1 exact real provider projection"), Built.Projection.IsValid())) return false;
  Projection = Built.Projection;
  FAssetDocumentPythonRevisionCandidate Candidate; Candidate.RuntimeRevision = Revision;
  Candidate.EntryModuleRelativePath = TEXT("s1_entry.py");
  Candidate.Providers = Projection->Providers; Candidate.RequiredPrimitives = Projection->Requirements; Candidate.AuthorityCatalog.Append(GetG1FrozenAuthorityExceptionRecords());
  auto Activated = Runtime->TryActivateRevision(Candidate); Diagnostics(Test, Activated.Diagnostics);
  if (!Test.TestTrue(TEXT("S1 test-owned exact revision activates"), Activated.bActivated)) return false;
  Route->BindMutationPreparationProjection(Projection);
  TArray<FAssetDocumentRuntimeTargetActivation> Activations; TArray<FAssetDocumentRuntimeDiagnostic> D;
  for (const auto& Row : Runtime->GetActiveRuntimeRegistrationProjection().Elements)
  {
   auto A = ParseAndValidateRuntimeTargetActivation(Row, GetAssetDocumentEditorTargetLifecycleCatalog(), D);
   if (!A) { Diagnostics(Test, D); return false; } Activations.Add(MoveTemp(A.GetValue()));
  }
  auto Bound = Route->BindRuntimeTargetActivations(MoveTemp(Activations)); Diagnostics(Test, Bound.Diagnostics);
  if (!Test.TestTrue(TEXT("S1 L1 target activation bound"), Bound.bValid)) return false;
  Registry = MakeUnique<FRegistry>(*Runtime->GetActivePrimitiveRegistry());
  FAssetDocumentCoreRuntimeDependencies Dependencies{*Route, *Registry, Cancellation, Observer,
   [this](const auto& M, const FGuid& Id, IAssetDocumentAtomicTransaction* T, TArray<FAssetDocumentRuntimeDiagnostic>& D)
   {
    auto* Routed = Route->ResolveTransactionForPhase(M, T, D);
    if (T && !Routed) return TUniquePtr<IAssetDocumentNativeRequestContext>();
    auto C = Runtime->CreateRequestContext(M, Id.IsValid() ? Id : FGuid::NewGuid(), Routed); D.Append(C.Diagnostics); return MoveTemp(C.Context);
   },
   [this](const auto& Event) { Invalidations.Add(Event); return Runtime->InvalidateHandles(Event); }};
  Core = MakeUnique<FAssetDocumentCoreRuntimeComposition>(MoveTemp(Dependencies));
  if (!Test.TestTrue(TEXT("S1 real Core composition valid"), Core->IsValid())) return false;
  Route->BindComposition(*Core); Service = MakeUnique<FAssetDocumentService>(); Service->BindGenericRuntimeRoute(Route.ToSharedRef());
  return true;
 }
 FAssetDocumentResult Apply(const FString& Sidecar)
 {
  SourcePaths.Add(Sidecar);
  FAssetDocumentApplyFileRequest Request; Request.FilePath = Sidecar; Request.bSaveAsset = true;
  // The legacy exact Montage Service profile remains registered. S1 exercises
  // the approved private route with the real sidecar adapter and persistence.
  auto Result = WithAssetDocumentSidecarWriteForTest(Sidecar, [&] { return Route->DispatchApplyFile(Request); });
  UE_LOG(LogTemp, Display, TEXT("SkeletonS1 ApplyFile terminal: %s"), *Result.Message);
  return Result;
 }
};
}
#endif
