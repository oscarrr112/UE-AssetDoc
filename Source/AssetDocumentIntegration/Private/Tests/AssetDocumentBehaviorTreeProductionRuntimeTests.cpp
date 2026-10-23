// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentGenericRuntimeRoute.h"
#include "AssetDocumentTestDirectoryBlock.h"
#include "AssetDocumentGenericRuntimeBootstrap.h"
#include "AssetDocumentStagedBehaviorTreeReadBinding.h"
#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentEditorMutationPreparation.h"
#include "AssetDocumentRequiredPrimitiveProviderView.h"
#include "AssetDocumentPythonRuntime.h"
#include "AssetDocumentService.h"
#include "Engine/Engine.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "../../../AssetDocument/Private/Tests/AssetDocumentSidecarWriteTestSupport.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
// Same private fixture seam as the legacy BT tests; no runtime/provider contract.
ASSETDOCUMENT_API void FlushBehaviorTreeFixtureFileEvents();
ASSETDOCUMENT_API void DeleteBehaviorTreeFixtureLoadedAsset(UObject* Asset);
FAssetDocumentGenericRuntimeBootstrap* GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
TArray<FAssetDocumentPrimitiveRequirement> GetAssetDocumentLegacyGenericRequirementsForTests();

namespace AssetDocumentBTProductionRuntimeTests
{
class FRegistry final : public IAssetDocumentPrimitiveRegistry
{
public:
 explicit FRegistry(const IAssetDocumentPrimitiveRegistry& In) : Inner(In) {}
 FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const override { return Inner.Snapshot(); }
 FAssetDocumentPrimitiveResolution Resolve(const FAssetDocumentPrimitiveRequirement& R) const override { return Inner.Resolve(R); }
private:
 const IAssetDocumentPrimitiveRegistry& Inner;
};
class FCancellation final : public IAssetDocumentCancellationView
{
public:
 bool IsCancellationRequested() const override { return bRequested; }
 bool bRequested = false;
};
class FObserver final : public IAssetDocumentRequestObserver
{
public:
 void OnPhaseChanged(FGuid, EAssetDocumentRuntimeRequestPhase Phase) override
 {
  Phases.Add(Phase);
  if (OnPhase) OnPhase(Phase);
 }
 TFunction<void(EAssetDocumentRuntimeRequestPhase)> OnPhase;
 TArray<EAssetDocumentRuntimeRequestPhase> Phases;
};
struct FInvalidationObservation
{
 FAssetDocumentHandleInvalidation Event;
 uint32 InvalidatedHandleCount = 0;
};

struct FFileSnapshot
{
 TArray<FString> Paths;
 TArray<TArray<uint8>> Bytes;
 bool operator==(const FFileSnapshot& Other) const { return Paths == Other.Paths && Bytes == Other.Bytes; }
};
bool SnapshotFiles(FAutomationTestBase& Test, const FString& Root, FFileSnapshot& Out)
{
 IFileManager::Get().FindFilesRecursive(Out.Paths, *Root, TEXT("*"), true, false);
 Out.Paths.Sort();
 for (const FString& Path : Out.Paths)
  if (!Test.TestTrue(TEXT("file preimage is readable: ") + Path,
   FFileHelper::LoadFileToArray(Out.Bytes.AddDefaulted_GetRef(), *Path))) return false;
 return true;
}

// Only observe the real slot and the native terminal callback. Every business
// invocation and outcome still comes from the unchanged production dispatch.
constexpr const TCHAR* Entry = TEXT(
 "from asset_document.profiles.behavior_tree_production import BEHAVIOR_TREE_PROFILE_META, create_behavior_tree_profile\n"
 "from asset_document import runtime as rt\n"
 "from asset_document import bootstrap\n"
 "from asset_document.registry import ProfileRegistryBuilder\n"
 "from asset_document.registered_runtime import build_dispatch\n"
 // Bootstrap initially freezes BB. Replace only the isolated fixture's
 // registry configuration; retain its actual exported dispatch function.
 "rt._profile_registry = None\n"
 "rt.configure_profile_registry(((BEHAVIOR_TREE_PROFILE_META, create_behavior_tree_profile),))\n"
 "builder = ProfileRegistryBuilder()\n"
 "builder.register(BEHAVIOR_TREE_PROFILE_META)\n"
 "bootstrap._registered_dispatch = build_dispatch(builder.freeze())\n"
 "bootstrap.dispatch.describe_runtime_registrations = bootstrap._registered_dispatch.describe_runtime_registrations\n"
 "previous_request = None\n"
 "left_slot = False\n"
 "terminal_request = None\n"
 "request_number = 0\n"
 "expected_failure_request_number = 0\n"
 "failure_cells = ()\n"
 "def terminal(request_id, revision):\n"
 "    global terminal_request\n"
 "    had_slot = rt._behavior_tree_expected_slot is not None\n"
 "    bootstrap.dispatch._clear_behavior_tree_expected_for_terminal(request_id, revision)\n"
 "    assert rt._behavior_tree_expected_slot is None, 'native terminal left BT expected slot alive'\n"
 "    if had_slot: terminal_request = request_id\n"
 "def dispatch(request, context):\n"
 "    global previous_request, left_slot, request_number\n"
 "    request_id = request['request_id']\n"
 "    if previous_request is not None and previous_request != request_id:\n"
 "        assert rt._behavior_tree_expected_slot is None, 'terminal slot leaked before next real request'\n"
 "        if left_slot: assert terminal_request == previous_request, 'native terminal did not consume live slot'\n"
 "        if request_number in failure_cells or request_number == expected_failure_request_number:\n"
 "            assert left_slot and terminal_request == previous_request, 'failure cell never produced and terminally cleared its real Apply slot'\n"
 "    if previous_request != request_id: request_number += 1\n"
 "    result = bootstrap.dispatch(request, context)\n"
 "    if context.phase == 2 and result['terminal_status'] == 0:\n"
 "        assert rt._behavior_tree_expected_slot is not None, 'successful actual Apply omitted expected slot'\n"
 "    previous_request = request_id\n"
 "    left_slot = rt._behavior_tree_expected_slot is not None\n"
 "    return result\n"
 "dispatch._clear_behavior_tree_expected_for_terminal = terminal\n"
 "dispatch.describe_runtime_registrations = bootstrap.dispatch.describe_runtime_registrations\n");

constexpr const TCHAR* ServiceBody = TEXT(R"JSON({"BlackboardAsset":null,"Tree":{"GraphGuid":"11111111111111111111111111111111","Comments":[],"Root":{"Id":"22222222222222222222222222222222","Class":"/Script/AIModule.BTComposite_Sequence","Properties":{},"Children":[{"Id":"33333333333333333333333333333333","Class":"/Script/AssetDocumentTests.AssetDocumentAILegacyEmptyKeyProbeTask","Properties":{"Map":{"A":11,"_":12,"\u00df":13,"SS":14}},"Children":[],"Decorators":[],"Services":[]}],"Decorators":[],"Services":[]}}})JSON");

struct FHarness
{
 FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
 FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentBTProductionRuntime"), Id);
 FString Target = TEXT("/Game/AssetDocumentTests/BTProduction_") + Id + TEXT("/Tree");
 FString Sidecar = FPackageName::LongPackageNameToFilename(Target, TEXT(".assetdoc.json"));
 TUniquePtr<IAssetDocumentPythonRuntime> Runtime;
 TSharedPtr<FAssetDocumentGenericRuntimeRoute> Route;
 TSharedPtr<FAssetDocumentStagedBehaviorTreeReadBinding> Staged;
 TSharedPtr<const FAssetDocumentRequiredPrimitiveProjection> Projection;
 TUniquePtr<FRegistry> Registry;
 TUniquePtr<FAssetDocumentCoreRuntimeComposition> Composition;
 FCancellation Cancellation;
 FObserver Observer;
 TArray<FInvalidationObservation> Invalidations;

 ~FHarness()
 {
  if (Route) Route->UnbindComposition();
  Composition.Reset(); Registry.Reset(); Route.Reset(); Projection.Reset(); Staged.Reset();
  if (Runtime) Runtime->ShutdownPythonBinding();
  Runtime.Reset();
  WithAssetDocumentSidecarWriteForTest(Sidecar, [&]
  {
   FlushBehaviorTreeFixtureFileEvents();
   const FString ObjectPath = Target + TEXT(".") + FPackageName::GetShortName(Target);
   UObject* ExistingAsset = FindObject<UObject>(nullptr, *ObjectPath);
   // This GUID directory belongs to the fixture. Remove its files before the
   // stock loaded-asset API marks the package empty during its slower GC work.
   IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Sidecar), false, true);
   FlushBehaviorTreeFixtureFileEvents();
   DeleteBehaviorTreeFixtureLoadedAsset(ExistingAsset);
   FlushBehaviorTreeFixtureFileEvents();
   return FAssetDocumentResult::Success(TEXT(""));
  });
  IFileManager::Get().DeleteDirectory(*Root, false, true);
 }
 // FailureCells: the request numbers that must follow a failed Apply. Only the direct-route
 // test counts its own requests; Service applies add a live extract after each success.
 bool Initialize(FAutomationTestBase& Test, int32 ExpectedFailureRequestNumber = 0, const TCHAR* FailureCells = TEXT("()"))
 {
  IFileManager::Get().MakeDirectory(*Root, true);
  const FString Package = FPaths::Combine(Root, TEXT("Python"));
  if (!Test.TestTrue(TEXT("copy unchanged production Python package"),
   FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(*Package,
    *FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Content/Python")), false))) return false;
  const FString ObservedEntry = FString(Entry) + FString::Printf(
   TEXT("expected_failure_request_number = %d\nfailure_cells = %s\n"), ExpectedFailureRequestNumber, FailureCells);
  if (!FFileHelper::SaveStringToFile(ObservedEntry, *FPaths::Combine(Package, TEXT("bt_production_entry.py")))) return false;
  FAssetDocumentPythonRuntimeConfiguration Config;
  Config.RepositoryRoot = Root; Config.PackageRootRelativePath = TEXT("Python");
  Runtime = CreateAssetDocumentPythonRuntime(Config);
  if (!Test.TestTrue(TEXT("real Python binding initializes"), Runtime->InitializePythonBinding().bValid)) return false;
  Route = MakeShared<FAssetDocumentGenericRuntimeRoute>(*Runtime);
  Staged = MakeShared<FAssetDocumentStagedBehaviorTreeReadBinding>(CreateAssetDocumentEditorPrimitiveProvider());
  TArray<TSharedRef<IAssetDocumentPrimitiveProvider>> Providers{
   Staged.ToSharedRef(), WrapAssetDocumentBehaviorTreeStagedAIReadProvider(CreateAssetDocumentAIPrimitiveProvider(), Staged.ToSharedRef())};
  // This temporary registry selects Editor + AI only, using the existing
  // finite requirement helper that excludes the unrelated Montage consumer.
  auto Requirements = GetAssetDocumentLegacyGenericRequirementsForTests();
  for (const TCHAR* Name : {TEXT("ai.behavior_tree.blackboard.readback.v1"), TEXT("ai.behavior_tree.selector.inventory.v1"),
   TEXT("ai.behavior_tree.binding.inspect.v1"),
   TEXT("ai.behavior_tree.graph.create.v1"), TEXT("ai.behavior_tree.graph.author_scratch.v1"),
   TEXT("ai.behavior_tree.graph.install.v1"), TEXT("ai.behavior_tree.blackboard.set.v1")})
   Requirements.Add({Name, 1, TEXT("ai_profile")});
  Requirements.Add({TEXT("ai.behavior_tree.authored_graph.readback.v2"), 2, TEXT("ai_profile")});
  Requirements.Add({TEXT("ai.behavior_tree.node_class.inspect.v2"), 2, TEXT("ai_profile")});
  Requirements.Add({TEXT("ai.behavior_tree.value_or_key.refresh.v3"), 3, TEXT("ai_profile")});
  Requirements.Add({TEXT("ai.behavior_tree.selector.resolve_batch.v2"), 2, TEXT("ai_profile")});
  const TArray<FAssetDocumentMutationPreparationBinding> PreparationBindings{
   {Providers[0], PrepareAssetDocumentEditorMutationBatch}};
  const auto Selected = BuildAssetDocumentRequiredPrimitiveProjection(Providers, Requirements,
   GetFrozenAuthorityExceptionRecords(), PreparationBindings,
   [this](const FString& Provider, const FAssetDocumentPrimitiveInvocation& Invocation,
    IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction,
    TFunctionRef<FAssetDocumentPrimitiveOutcome()> Forward)
   { return Route->InvokePreparedMutation(Provider, Invocation, Handles, Transaction, Forward); });
  for (const auto& D : Selected.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
  if (!Test.TestTrue(TEXT("actual provider closure freezes"), Selected.Projection.IsValid())) return false;
  Projection = Selected.Projection;
  FAssetDocumentPythonRevisionCandidate Candidate;
  Candidate.RuntimeRevision = 1; Candidate.EntryModuleRelativePath = TEXT("bt_production_entry.py");
  Candidate.Providers = Projection->Providers; Candidate.RequiredPrimitives = Projection->Requirements;
  Candidate.AuthorityCatalog.Append(GetFrozenAuthorityExceptionRecords());
  const auto Activated = Runtime->TryActivateRevision(Candidate);
  for (const auto& D : Activated.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
  if (!Test.TestTrue(TEXT("real BT Profile runtime activates"), Activated.bActivated)) return false;
  Registry = MakeUnique<FRegistry>(*Runtime->GetActivePrimitiveRegistry());
  Route->BindMutationPreparationProjection(Projection);
  Route->BindBehaviorTreeStagedReadProvider(Staged);
  FAssetDocumentCoreRuntimeDependencies Dependencies{*Route, *Registry, Cancellation, Observer,
   [this](const FAssetDocumentRequestContextMetadata& Metadata, const FGuid& TransactionId,
    IAssetDocumentAtomicTransaction* ActiveTransaction, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
   {
    auto* ActualTransaction = Route->ResolveTransactionForPhase(Metadata, ActiveTransaction, Diagnostics);
    if (ActiveTransaction && !ActualTransaction) return TUniquePtr<IAssetDocumentNativeRequestContext>();
    auto Created = Runtime->CreateRequestContext(Metadata,
     TransactionId.IsValid() ? TransactionId : FGuid::NewGuid(), ActualTransaction);
    Diagnostics.Append(Created.Diagnostics); return MoveTemp(Created.Context);
   }, [this](const FAssetDocumentHandleInvalidation& Event)
   {
    const auto Result = Runtime->InvalidateHandles(Event);
    Invalidations.Add({Event, Result.InvalidatedHandleCount});
    return Result;
   }};
  Composition = MakeUnique<FAssetDocumentCoreRuntimeComposition>(MoveTemp(Dependencies));
  if (!Test.TestTrue(TEXT("real Atomic composition valid"), Composition->IsValid())) return false;
  Route->BindComposition(*Composition);
  return true;
 }
 FAssetDocumentResult Apply(const FString& Action, const FString& Body)
 {
  IFileManager::Get().MakeDirectory(*FPaths::GetPath(Sidecar), true);
  const FString Document = FString::Printf(TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Class\":\"/Script/AIModule.BehaviorTree\",\"Target\":\"%s\",\"Action\":\"%s\",\"Properties\":{},\"Body\":%s}"), *Target, *Action, *Body);
  FFileHelper::SaveStringToFile(Document, *Sidecar);
  FAssetDocumentApplyFileRequest Request;
  Request.FilePath = Sidecar; Request.bSaveAsset = true;
  // Same ambient guard as Service; this intentionally exercises Generic's
  // real ApplyFile coordinator before the global exact BT Service cutover.
  return WithAssetDocumentSidecarWriteForTest(Sidecar, [&] { return Route->DispatchApplyFile(Request); });
 }
 bool WritePublicDocument(const FString& Action, const FString& Body, const FString& Properties = TEXT("{}"))
 {
  if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Sidecar), true)) return false;
  const FString Document = FString::Printf(TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Class\":\"/Script/AIModule.BehaviorTree\",\"Target\":\"%s\",\"Action\":\"%s\",\"Properties\":%s,\"Body\":%s}"), *Target, *Action, *Properties, *Body);
  return FFileHelper::SaveStringToFile(Document, *Sidecar, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
 }
 FAssetDocumentResult ApplyPublic()
 {
  FAssetDocumentService Service;
  Service.BindGenericRuntimeRoute(Route.ToSharedRef());
  FAssetDocumentApplyFileRequest Request;
  Request.FilePath = Sidecar; Request.bSaveAsset = true;
  return Service.ApplyFile(Request);
 }
};
void Report(FAutomationTestBase& Test, const FAssetDocumentResult& Result)
{
 for (const auto& D : Result.Diagnostics) Test.AddInfo(D.Code + TEXT(" ") + D.Path + TEXT(": ") + D.Message);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBTProductionRuntimeTest,
 "UEAssetDoc.Integration.BehaviorTree.ProductionRuntime",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBTProductionRuntimeTest::RunTest(const FString&)
{
 using namespace AssetDocumentBTProductionRuntimeTests;
 auto* Bootstrap = GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
 if (!TestNotNull(TEXT("bootstrap available for runtime isolation"), Bootstrap)) return false;
 Bootstrap->Deactivate();
 ON_SCOPE_EXIT { TestTrue(TEXT("production bootstrap restored"), Bootstrap->Activate()); };
 FHarness Harness;
 if (!Harness.Initialize(*this, 0, TEXT("(3, 5)"))) return false;
 const FString Body = TEXT(R"JSON({"BlackboardAsset":null,"Tree":{"GraphGuid":"11111111111111111111111111111111","Comments":[],"Root":{"Id":"22222222222222222222222222222222","Class":"/Script/AIModule.BTComposite_Sequence","Properties":{},"Children":[{"Id":"33333333333333333333333333333333","Class":"/Script/AssetDocumentTests.AssetDocumentAILegacyEmptyKeyProbeTask","Properties":{"Map":{"A":11,"_":12,"\u00df":13,"SS":14}},"Children":[],"Decorators":[],"Services":[]}],"Decorators":[],"Services":[]}}})JSON");
 auto Created = Harness.Apply(TEXT("Create"), Body); Report(*this, Created);
 if (!TestTrue(TEXT("real full-tree factory Apply/save/fresh Verify succeeds"), Created.IsSuccess())) return false;
 UObject* Asset = FSoftObjectPath(Harness.Target + TEXT(".Tree")).ResolveObject();
 if (!TestNotNull(TEXT("factory published target"), Asset)) return false;
 auto* GraphProperty = FindFProperty<FObjectPropertyBase>(Asset->GetClass(), TEXT("BTGraph"));
 if (!TestNotNull(TEXT("public authored graph property"), GraphProperty)) return false;
 auto* Graph = Cast<UEdGraph>(GraphProperty->GetObjectPropertyValue_InContainer(Asset));
 if (!TestNotNull(TEXT("installed graph"), Graph)) return false;
 const FGuid BeforeGuid = Graph->GraphGuid;
 const auto BeforeNodes = Graph->Nodes;
 TestTrue(TEXT("saved package exists"), IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(Harness.Target, FPackageName::GetAssetPackageExtension())));
 TestTrue(TEXT("real coordinator reaches Verify"), Harness.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Verify));
 auto Overlay = Harness.Apply(TEXT("Update"), TEXT("{\"BlackboardAsset\":null}")); Report(*this, Overlay);
 TestTrue(TEXT("omitted Tree overlay saves and freshly verifies"), Overlay.IsSuccess());
 TestEqual(TEXT("overlay retains authored graph identity"), Graph->GraphGuid, BeforeGuid);
 TestTrue(TEXT("overlay retains exact ordered graph nodes"), Graph->Nodes == BeforeNodes);
 TestFalse(TEXT("fresh verification binding is closed after request"), Harness.Staged->IsBound());
 const FString SavedPackage = FPackageName::LongPackageNameToFilename(Harness.Target, FPackageName::GetAssetPackageExtension());
 TArray<uint8> BeforeSavedBytes, AfterSavedBytes;
 if (!TestTrue(TEXT("saved baseline package is readable"), FFileHelper::LoadFileToArray(BeforeSavedBytes, *SavedPackage))) return false;
 const FString ChangedBody = Body.Replace(TEXT("\"A\":11"), TEXT("\"A\":21"));
 if (!TestNotEqual(TEXT("failure requests author a real tree property change"), ChangedBody, Body)) return false;
 // Cancellation is raised when Apply starts, but Core checks it only after
 // the real host returns from Apply. No host result or primitive is replaced.
 Harness.Observer.Phases.Reset();
 Harness.Observer.OnPhase = [&Harness](EAssetDocumentRuntimeRequestPhase Phase)
 {
  if (Phase == EAssetDocumentRuntimeRequestPhase::Apply) Harness.Cancellation.bRequested = true;
 };
 auto Cancelled = Harness.Apply(TEXT("Update"), ChangedBody); Report(*this, Cancelled);
 TestFalse(TEXT("native cancellation after actual Apply rejects request"), Cancelled.IsSuccess());
 TestTrue(TEXT("cancelled request reached actual Apply"), Harness.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Apply));
 TestFalse(TEXT("cancellation happens before Repair/Verify"), Harness.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Repair));
 TestTrue(TEXT("cancellation is the actual terminal cause"), Cancelled.Diagnostics.ContainsByPredicate([](const auto& D)
 { return D.Code == TEXT("RequestCoordinator.InvalidPhaseResult") && D.Message.Contains(TEXT("cancelled")); }));
 TestTrue(TEXT("cancellation restores the exact original graph"), GraphProperty->GetObjectPropertyValue_InContainer(Asset) == Graph);
 TestTrue(TEXT("cancellation restores the exact original graph nodes"), Graph->Nodes == BeforeNodes);
 TestTrue(TEXT("package remains readable after cancellation"), FFileHelper::LoadFileToArray(AfterSavedBytes, *SavedPackage));
 TestEqual(TEXT("cancellation leaves saved package bytes unchanged"), AfterSavedBytes, BeforeSavedBytes);
 Harness.Observer.OnPhase = {}; Harness.Cancellation.bRequested = false;
 auto AfterCancellation = Harness.Apply(TEXT("Update"), TEXT("{\"BlackboardAsset\":null}")); Report(*this, AfterCancellation);
 TestTrue(TEXT("next real request proves native cancellation consumed the live expected slot"), AfterCancellation.IsSuccess());
 // Reuse the existing D5 persistence failure technique: make the existing
 // staging directory readonly, restore its exact previous mode afterwards.
 const FString StagingRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocument/Runtime/Staging"));
 auto& Files = FPlatformFileManager::Get().GetPlatformFile();
 const bool Existed = Files.DirectoryExists(*StagingRoot);
 if (!Existed) Files.CreateDirectoryTree(*StagingRoot);
 const bool WasReadOnly = Files.IsReadOnly(*StagingRoot);
 if (!TestTrue(TEXT("staging failure fixture becomes readonly"), WasReadOnly || AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *StagingRoot, true))) return false;
 ON_SCOPE_EXIT
 {
  AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *StagingRoot, WasReadOnly);
  if (!Existed) IFileManager::Get().DeleteDirectory(*StagingRoot, false, false);
 };
 Harness.Observer.Phases.Reset();
 BeforeSavedBytes.Reset(); AfterSavedBytes.Reset();
 if (!TestTrue(TEXT("package preimage is readable before persistence failure"), FFileHelper::LoadFileToArray(BeforeSavedBytes, *SavedPackage))) return false;
 bool SawInstalledChangedGraph = false;
 Harness.Observer.OnPhase = [&](EAssetDocumentRuntimeRequestPhase Phase)
 {
  if (Phase == EAssetDocumentRuntimeRequestPhase::Persist)
   SawInstalledChangedGraph = GraphProperty->GetObjectPropertyValue_InContainer(Asset) != Graph;
 };
 auto PersistenceFailure = Harness.Apply(TEXT("Update"), ChangedBody); Report(*this, PersistenceFailure);
 TestFalse(TEXT("real persistence failure rejects after Repair"), PersistenceFailure.IsSuccess());
 TestTrue(TEXT("actual Repair completed before persistence failure"), Harness.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Persist));
 TestFalse(TEXT("persistence failure precedes Verify"), Harness.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Verify));
 TestTrue(TEXT("persistence failure follows a real installed graph change"), SawInstalledChangedGraph);
 TestTrue(TEXT("persistence rollback restores the exact original graph"), GraphProperty->GetObjectPropertyValue_InContainer(Asset) == Graph);
 TestTrue(TEXT("persistence rollback restores the exact original graph nodes"), Graph->Nodes == BeforeNodes);
 TestTrue(TEXT("package remains readable after persistence rollback"), FFileHelper::LoadFileToArray(AfterSavedBytes, *SavedPackage));
 TestEqual(TEXT("persistence rollback leaves saved package bytes unchanged"), AfterSavedBytes, BeforeSavedBytes);
 Harness.Observer.OnPhase = {};
 AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *StagingRoot, WasReadOnly);
 auto AfterPersistenceFailure = Harness.Apply(TEXT("Update"), TEXT("{\"BlackboardAsset\":null}")); Report(*this, AfterPersistenceFailure);
 TestTrue(TEXT("next real request proves persistence rollback consumed expected slot"), AfterPersistenceFailure.IsSuccess());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBTServiceFactoryPersistRollbackTest,
 "UEAssetDoc.Integration.BehaviorTree.ServiceFactoryPersistRollback",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBTServiceFactoryPersistRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentBTProductionRuntimeTests;
 auto* Bootstrap = GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
 if (!TestNotNull(TEXT("bootstrap available for runtime isolation"), Bootstrap)) return false;
 Bootstrap->Deactivate();
 ON_SCOPE_EXIT { TestTrue(TEXT("production bootstrap restored"), Bootstrap->Activate()); };
 FHarness Harness;
 if (!Harness.Initialize(*this, 1)) return false;
 const FString Body = ServiceBody;
 if (!TestTrue(TEXT("public factory document is written"), Harness.WritePublicDocument(TEXT("Create"), Body))) return false;
 const FString ObjectPath = Harness.Target + TEXT(".Tree");
 const FString PackageFile = FPackageName::LongPackageNameToFilename(Harness.Target, FPackageName::GetAssetPackageExtension());
 TestNull(TEXT("factory object is originally absent"), FSoftObjectPath(ObjectPath).ResolveObject());
 TestNull(TEXT("factory package is originally absent"), FindPackage(nullptr, *Harness.Target));
 TestFalse(TEXT("factory package file is originally absent"), IFileManager::Get().FileExists(*PackageFile));
 const FString SyncRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocument/Runtime/Sync"));
 FFileSnapshot BeforeFiles, BeforeSync, AfterFiles, AfterSync;
 if (!SnapshotFiles(*this, FPaths::GetPath(Harness.Sidecar), BeforeFiles)
  || !SnapshotFiles(*this, SyncRoot, BeforeSync)) return false;

 const FString StagingRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocument/Runtime/Staging"));
 auto& Files = FPlatformFileManager::Get().GetPlatformFile();
 const bool Existed = Files.DirectoryExists(*StagingRoot);
 if (!Existed && !TestTrue(TEXT("staging fixture directory is created"), Files.CreateDirectoryTree(*StagingRoot))) return false;
 const bool WasReadOnly = Files.IsReadOnly(*StagingRoot);
 ON_SCOPE_EXIT
 {
  AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *StagingRoot, WasReadOnly);
  if (!Existed) IFileManager::Get().DeleteDirectory(*StagingRoot, false, false);
 };
 if (!TestFalse(TEXT("staging fixture was writable before injection"), WasReadOnly)
  || !TestTrue(TEXT("staging failure fixture becomes readonly"), AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *StagingRoot, true))) return false;
 TWeakObjectPtr<UObject> FactoryAsset;
 TWeakObjectPtr<UEdGraph> InstalledGraph;
 TArray<TWeakObjectPtr<UObject>> OwnedObjects;
 bool SawInstalledTreeAtPersist = false;
 Harness.Observer.OnPhase = [&](EAssetDocumentRuntimeRequestPhase Phase)
 {
  if (Phase != EAssetDocumentRuntimeRequestPhase::Persist) return;
  UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject();
  FactoryAsset = Asset;
  if (!Asset) return;
  auto* GraphProperty = FindFProperty<FObjectPropertyBase>(Asset->GetClass(), TEXT("BTGraph"));
  auto* Graph = GraphProperty ? Cast<UEdGraph>(GraphProperty->GetObjectPropertyValue_InContainer(Asset)) : nullptr;
  InstalledGraph = Graph;
  SawInstalledTreeAtPersist = Graph && Graph->GraphGuid == FGuid(0x11111111, 0x11111111, 0x11111111, 0x11111111)
   && Graph->Nodes.Num() > 0;
  TArray<UObject*> Owned;
  GetObjectsWithOuter(Asset, Owned);
  for (UObject* Object : Owned) OwnedObjects.Add(Object);
 };
 const auto Failed = Harness.ApplyPublic(); Report(*this, Failed);
 TestFalse(TEXT("real Service factory Persist failure rejects request"), Failed.IsSuccess());
 TestTrue(TEXT("real factory Apply and Repair ran before Persist"),
  Harness.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Apply)
  && Harness.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Repair)
  && Harness.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Persist));
 TestFalse(TEXT("late failure occurs before fresh Verify"), Harness.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Verify));
 TestTrue(TEXT("Persist observes the factory-published authored graph"), SawInstalledTreeAtPersist);
 TestTrue(TEXT("original persistence diagnostic survives rollback"), Failed.Diagnostics.ContainsByPredicate([](const auto& D)
 { return D.Code == TEXT("Persistence.StagePackageFailed"); }));
 TestFalse(TEXT("factory inverse completes without Poisoned state"), Failed.Diagnostics.ContainsByPredicate([](const auto& D)
 { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 TestNull(TEXT("factory rollback removes target object"), FSoftObjectPath(ObjectPath).ResolveObject());
 TestNull(TEXT("factory rollback removes target package"), FindPackage(nullptr, *Harness.Target));
 TestFalse(TEXT("factory asset owner is released"), FactoryAsset.IsValid());
 TestFalse(TEXT("installed scratch graph owner is released"), InstalledGraph.IsValid());
 TestTrue(TEXT("factory ownership observation covered real subobjects"), OwnedObjects.Num() > 0);
 TestFalse(TEXT("factory rollback leaves no live owned subobjects"), OwnedObjects.ContainsByPredicate([](const auto& Object) { return Object.IsValid(); }));
 const auto Rollbacks = Harness.Invalidations.FilterByPredicate([](const auto& Observation)
 { return Observation.Event.Reason == EAssetDocumentHandleInvalidationReason::Rollback; });
 TestEqual(TEXT("actual Atomic emits one rollback invalidation"), Rollbacks.Num(), 1);
 if (Rollbacks.Num() == 1)
 {
  TestTrue(TEXT("actual rollback retires live runtime handles"), Rollbacks[0].InvalidatedHandleCount > 0);
  TestTrue(TEXT("rollback invalidation carries native request and transaction identities"),
   Rollbacks[0].Event.RequestId.IsValid() && Rollbacks[0].Event.TransactionId.IsValid() && Rollbacks[0].Event.RuntimeRevision == 1);
 }
 TestFalse(TEXT("staged fresh-read binding is closed after rollback"), Harness.Staged->IsBound());
 if (!SnapshotFiles(*this, FPaths::GetPath(Harness.Sidecar), AfterFiles)
  || !SnapshotFiles(*this, SyncRoot, AfterSync)) return false;
 TestTrue(TEXT("factory rollback preserves absent package and exact input sidecar bytes"), AfterFiles == BeforeFiles);
 TestTrue(TEXT("factory rollback preserves the sync state preimage"), AfterSync == BeforeSync);

 Harness.Observer.OnPhase = {}; Harness.Observer.Phases.Reset();
 if (!TestTrue(TEXT("staging fixture mode is restored before retry"), AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *StagingRoot, WasReadOnly))) return false;
 if (!TestTrue(TEXT("next public factory document is written"), Harness.WritePublicDocument(TEXT("Create"), Body))) return false;
 const auto Retried = Harness.ApplyPublic(); Report(*this, Retried);
 TestTrue(TEXT("next real Service Create proves native terminal consumed the live failure slot"), Retried.IsSuccess());
 TestTrue(TEXT("next real Service Create durably saves and freshly verifies"),
  IFileManager::Get().FileExists(*PackageFile) && Harness.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Verify));
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBTServiceTopPropertiesOwnershipTest,
 "UEAssetDoc.Integration.BehaviorTree.ServiceTopPropertiesOwnership",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBTServiceTopPropertiesOwnershipTest::RunTest(const FString&)
{
 using namespace AssetDocumentBTProductionRuntimeTests;
 auto* Bootstrap = GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
 if (!TestNotNull(TEXT("bootstrap available for runtime isolation"), Bootstrap)) return false;
 Bootstrap->Deactivate();
 ON_SCOPE_EXIT { TestTrue(TEXT("production bootstrap restored"), Bootstrap->Activate()); };
 FHarness Harness;
 if (!Harness.Initialize(*this)) return false;
 if (!TestTrue(TEXT("public baseline factory document is written"), Harness.WritePublicDocument(TEXT("Create"), ServiceBody))) return false;
 const auto Created = Harness.ApplyPublic(); Report(*this, Created);
 if (!TestTrue(TEXT("real Service baseline Create durably verifies"), Created.IsSuccess())) return false;
 UObject* Asset = FSoftObjectPath(Harness.Target + TEXT(".Tree")).ResolveObject();
 if (!TestNotNull(TEXT("public baseline target exists"), Asset)) return false;
 auto* GraphProperty = FindFProperty<FObjectPropertyBase>(Asset->GetClass(), TEXT("BTGraph"));
 auto* RootProperty = FindFProperty<FObjectPropertyBase>(Asset->GetClass(), TEXT("RootNode"));
 if (!TestNotNull(TEXT("public baseline graph property exists"), GraphProperty)
  || !TestNotNull(TEXT("public baseline runtime root property exists"), RootProperty)) return false;
 auto* Graph = Cast<UEdGraph>(GraphProperty->GetObjectPropertyValue_InContainer(Asset));
 if (!TestNotNull(TEXT("public baseline authored graph exists"), Graph)) return false;
 const auto BeforeNodes = Graph->Nodes;
 UObject* BeforeRoot = RootProperty->GetObjectPropertyValue_InContainer(Asset);
 const FString Overlay = TEXT("{\"BlackboardAsset\":null}");
 if (!TestTrue(TEXT("nonempty top-level Body-owned property request is written"),
  Harness.WritePublicDocument(TEXT("Update"), Overlay, TEXT("{\"RootNode\":null}")))) return false;
 // The submitted sidecar is the preimage. Do not compare it with the prior
 // successful request's sidecar, which the caller intentionally replaced.
 FFileSnapshot BeforeFiles, BeforeSync, AfterFiles, AfterSync;
 const FString SyncRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocument/Runtime/Sync"));
 if (!SnapshotFiles(*this, FPaths::GetPath(Harness.Sidecar), BeforeFiles)
  || !SnapshotFiles(*this, SyncRoot, BeforeSync)) return false;
 Harness.Observer.Phases.Reset(); Harness.Invalidations.Reset();
 const auto Rejected = Harness.ApplyPublic(); Report(*this, Rejected);
 TestFalse(TEXT("real Service rejects nonempty top-level Body-owned Properties"), Rejected.IsSuccess());
 TestTrue(TEXT("original public Body-owned diagnostic and path are preserved"), Rejected.Diagnostics.ContainsByPredicate([](const auto& D)
 { return D.Code == TEXT("BodyOwnedProperty") && D.Path == TEXT("/Properties/RootNode")
  && D.Message == TEXT("Property 'RootNode' is owned by Body.Tree and cannot be authored through top-level Properties"); }));
 TestFalse(TEXT("top-level ownership rejection precedes actual Apply"), Harness.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Apply));
 TestFalse(TEXT("top-level ownership rejection precedes Persist"), Harness.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Persist));
 TestTrue(TEXT("rejected public request never enlists an Atomic terminal invalidation"), Harness.Invalidations.IsEmpty());
 TestTrue(TEXT("rejected top-level write retains exact graph ownership"), GraphProperty->GetObjectPropertyValue_InContainer(Asset) == Graph);
 TestTrue(TEXT("rejected top-level write retains ordered graph nodes"), Graph->Nodes == BeforeNodes);
 TestTrue(TEXT("rejected top-level write retains runtime root identity"), RootProperty->GetObjectPropertyValue_InContainer(Asset) == BeforeRoot);
 if (!SnapshotFiles(*this, FPaths::GetPath(Harness.Sidecar), AfterFiles)
  || !SnapshotFiles(*this, SyncRoot, AfterSync)) return false;
 TestTrue(TEXT("ownership rejection preserves package and submitted sidecar bytes"), AfterFiles == BeforeFiles);
 TestTrue(TEXT("ownership rejection preserves durable sync state"), AfterSync == BeforeSync);
 TestFalse(TEXT("ownership rejection leaves no staged fresh-read binding"), Harness.Staged->IsBound());
 TestFalse(TEXT("ownership rejection leaves no pinned Python request"), Harness.Runtime->HasActiveRequests());
 if (!TestTrue(TEXT("next valid public overlay document is written"), Harness.WritePublicDocument(TEXT("Update"), Overlay))) return false;
 const auto Next = Harness.ApplyPublic(); Report(*this, Next);
 TestTrue(TEXT("next real Service request succeeds after ownership rejection"), Next.IsSuccess());
 return !HasAnyErrors();
}
#endif
