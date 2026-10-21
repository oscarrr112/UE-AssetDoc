// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentRuntimeTargetActivation.h"
#include "AssetDocumentGenericMutationPreparation.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/PrimaryAssetLabel.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace
{
// The guard is class-neutral: this test-owned object only supplies registry existence.
// Montage factory/rollback authority (generic factory row with a Skeleton
// factory_context) is exercised by MontageLifecycle.GenericFactoryContext.
struct FTargetExistenceFixture
{
 FString PackageName = TEXT("/Game/AssetDocumentTargetGuard/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 FString Path = PackageName + TEXT(".Target");
 UPackage* Package = nullptr;
 UPrimaryAssetLabel* Asset = nullptr;
 bool Registered = false;
 void Add()
 {
  if (!Asset) { Package = CreatePackage(*PackageName); Asset = NewObject<UPrimaryAssetLabel>(Package, TEXT("Target"), RF_Public | RF_Standalone); }
  FAssetRegistryModule::AssetCreated(Asset); Registered = true;
 }
 void Remove()
 {
  if (Registered) { FAssetRegistryModule::AssetDeleted(Asset); Registered = false; }
  // AssetRegistry also enumerates loaded public assets: a notification alone is not deletion.
  if (Asset)
  {
   Asset->ClearFlags(RF_Public | RF_Standalone);
   Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
   Asset->MarkAsGarbage(); Asset = nullptr;
  }
  if (Package) { Package->SetDirtyFlag(false); Package->MarkAsGarbage(); Package = nullptr; }
 }
 ~FTargetExistenceFixture() { Remove(); }
};
FAssetDocumentRuntimeRequestDto GuardRequest(const FString& Path)
{
 FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 17;
 R.TargetAssetPath = Path; R.RequestedAssetClassPath = UPrimaryAssetLabel::StaticClass()->GetPathName();
 R.CanonicalDocument.Kind = EAssetDocumentRuntimeValueKind::Object; R.CanonicalDocument.ObjectKeys = {TEXT("Action")};
 FAssetDocumentRuntimeValueDto Action; Action.Kind = EAssetDocumentRuntimeValueKind::String; Action.StringValue = TEXT("CreateOrUpdate"); R.CanonicalDocument.Elements = {Action}; return R;
}
class FGuardCountingContext final : public IAssetDocumentNativeRequestContext
{
public:
 FAssetDocumentRequestContextMetadata Metadata;
 int32 Forwarded = 0;
 int32 ForwardedMutations = 0;
 const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& Invocation) override
 { ++Forwarded; if (Invocation.PrimitiveId == TEXT("test.target.mutate.v1")) ++ForwardedMutations; FAssetDocumentPrimitiveOutcome O; O.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded; return O; }
 FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override
 {
  FAssetDocumentPrimitiveRegistrySnapshot S;
  FAssetDocumentPrimitiveDescriptor D; D.PrimitiveId = TEXT("test.target.mutate.v1"); D.Version = 1; D.Access = EAssetDocumentPrimitiveAccess::Mutating; S.Descriptors.Add(D); return S;
 }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPreparedTargetGuardTest,
 "UEAssetDoc.GenericRuntime.TargetLifecycle.PreparedDecisionGuard",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPreparedTargetGuardTest::RunTest(const FString& Parameters)
{
 (void)Parameters;
 FAssetDocumentRuntimeTargetActivation Activation;
 Activation.ExactAssetClassPath = UPrimaryAssetLabel::StaticClass()->GetPathName(); Activation.LifecycleId = TEXT("test.existence.v1"); Activation.TargetPolicy = EAssetDocumentRuntimeTargetPolicy::CreateOrUpdate;
 FAssetDocumentPrimitiveInvocation Mutation; Mutation.PrimitiveId = TEXT("test.target.mutate.v1"); Mutation.Version = 1;
 for (const bool InitiallyExists : {false, true})
 {
  for (const bool Drift : {false, true})
  {
   FTargetExistenceFixture Fixture; if (InitiallyExists) Fixture.Add();
   auto Request = GuardRequest(Fixture.Path); TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
   const auto Prepared = PrepareAssetDocumentRuntimeTargetDecision(Activation, Request, Diagnostics);
   if (!TestTrue(TEXT("preparation resolves test-owned initial existence"), Prepared.IsSet())) continue;
   TestEqual(TEXT("prepared create decision"), Prepared->bCreate, !InitiallyExists);
   FGuardCountingContext Inner; Inner.Metadata.RequestId = Request.RequestId; Inner.Metadata.PinnedRuntimeRevision = Request.ExpectedRuntimeRevision; Inner.Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
   FAssetDocumentPreparedTargetContext Guard(Inner, Prepared.GetValue(), Request);
   if (Drift) { if (InitiallyExists) Fixture.Remove(); else Fixture.Add(); }
   const auto Outcome = Guard.Invoke(Mutation);
   TestEqual(TEXT("drift rejected before forwarding first mutator"), Inner.Forwarded, Drift ? 0 : 1);
   TestEqual(TEXT("stable decision alone permits mutation"), Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded, !Drift);
   if (Drift) TestEqual(TEXT("stable drift diagnostic"), Outcome.Diagnostics.IsEmpty() ? FString() : Outcome.Diagnostics[0].Code, FString(TEXT("GenericRuntime.TargetActivationDrift")));
   else if (!InitiallyExists)
   {
    // Once first mutation starts, its own creation must not be mistaken for external drift.
    Fixture.Add(); Guard.Invoke(Mutation); TestEqual(TEXT("request's own create permits subsequent mutation"), Inner.Forwarded, 2);
   }
   Fixture.Remove(); TestNull(TEXT("fixture object identity removed"), FSoftObjectPath(Fixture.Path).ResolveObject()); TestFalse(TEXT("fixture registry absence restored"), IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Fixture.Path)).IsValid());
  }
 }
 // A read-only invocation must not consume the first-mutation freshness check.
 {
  FTargetExistenceFixture ReadFixture; auto ReadRequest = GuardRequest(ReadFixture.Path);
  TArray<FAssetDocumentRuntimeDiagnostic> ReadDiagnostics;
  const auto ReadPrepared = PrepareAssetDocumentRuntimeTargetDecision(Activation, ReadRequest, ReadDiagnostics);
  if (!TestTrue(TEXT("read-before-mutation preparation succeeds"), ReadPrepared.IsSet())) return false;
  FGuardCountingContext Inner; Inner.Metadata.RequestId = ReadRequest.RequestId; Inner.Metadata.PinnedRuntimeRevision = 17; Inner.Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  FAssetDocumentPreparedTargetContext Guard(Inner, ReadPrepared.GetValue(), ReadRequest);
  FAssetDocumentPrimitiveInvocation Read; Read.PrimitiveId = TEXT("test.target.read.v1"); Read.Version = 1;
  Guard.Invoke(Read); TestEqual(TEXT("initial read forwarded"), Inner.Forwarded, 1);
  ReadFixture.Add(); Guard.Invoke(Mutation);
  TestEqual(TEXT("drift after read forwards zero mutators"), Inner.ForwardedMutations, 0);
  TestEqual(TEXT("drift does not forward second invocation"), Inner.Forwarded, 1);
 }
 FTargetExistenceFixture Fixture; auto Request = GuardRequest(Fixture.Path); TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 const auto Prepared = PrepareAssetDocumentRuntimeTargetDecision(Activation, Request, Diagnostics);
 if (!TestTrue(TEXT("identity test preparation succeeds"), Prepared.IsSet())) return false;
 for (int32 Case = 0; Case != 5; ++Case)
 {
  auto Changed = Request;
  FGuardCountingContext Inner; Inner.Metadata.RequestId = Request.RequestId; Inner.Metadata.PinnedRuntimeRevision = 17; Inner.Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  if (Case == 0) Changed.RequestId = FGuid::NewGuid();
  if (Case == 1) Changed.ExpectedRuntimeRevision = 18;
  if (Case == 2) Changed.CanonicalDocument.Elements[0].StringValue = TEXT("Create");
  if (Case == 3) Inner.Metadata.RequestId = FGuid::NewGuid();
  if (Case == 4) Inner.Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
  FAssetDocumentPreparedTargetContext Guard(Inner, Prepared.GetValue(), Changed); const auto Outcome = Guard.Invoke(Mutation);
  TestEqual(TEXT("request/action/context drift forwards zero"), Inner.Forwarded, 0);
  TestFalse(TEXT("identity drift fails"), Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 }
 return true;
}

namespace
{
class FMontagePreparationVerifier final : public IAssetDocumentStagedArtifactVerifier
{
public:
 int32 Calls = 0;
 FAssetDocumentFreshVerificationResult VerifyFresh(const FAssetDocumentFreshVerificationInput&) override
 { ++Calls; return {}; }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMontageNativePreparationTest,
 "UEAssetDoc.GenericRuntime.TargetLifecycle.MontageNativePreparation",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentMontageNativePreparationTest::RunTest(const FString& Parameters)
{
 (void)Parameters;
 // AnimMontage has no class-specific row: it shares the class-neutral generic
 // factory lifecycle, owned by the registering Profile's route.
 const FAssetDocumentEditorTargetLifecycleCatalogEntry* Lifecycle =
  FindAssetDocumentEditorTargetLifecycle(TEXT("/Script/Engine.AnimMontage"));
 if (!TestNotNull(TEXT("production Montage lifecycle exists"), Lifecycle)) return false;
 TestTrue(TEXT("Montage uses the class-neutral row"), Lifecycle->IsClassNeutral());
 FAssetDocumentRuntimeTargetActivation Activation;
 Activation.ExactAssetClassPath = TEXT("/Script/Engine.AnimMontage");
 Activation.LifecycleId = Lifecycle->LifecycleId;
 Activation.RequiredPrimitives = Lifecycle->RequiredPrimitives;
 for (auto& Requirement : Activation.RequiredPrimitives) Requirement.RequiredByRouteId = TEXT("animation_profile");
 FMontagePreparationVerifier Verifier;
 const TCHAR* Actions[] = {TEXT("Create"), TEXT("CreateOrUpdate"), TEXT("Update")};
 for (const auto Policy : {EAssetDocumentRuntimeTargetPolicy::CreateOrUpdate, EAssetDocumentRuntimeTargetPolicy::UpdateOnly})
 {
  Activation.TargetPolicy = Policy;
  for (int32 ActionIndex = 0; ActionIndex != 3; ++ActionIndex)
  {
   const FString Name = TEXT("Montage_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
   const FString Package = TEXT("/Game/AssetDocumentMontagePreparation/") + Name;
   const FString Target = Package + TEXT(".") + Name;
   const FString Filename = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
   auto Request = GuardRequest(Target);
   Request.RequestedAssetClassPath = Activation.ExactAssetClassPath;
   Request.Operation = EAssetDocumentRuntimeOperation::Apply;
   Request.CanonicalDocument.Elements[0].StringValue = Actions[ActionIndex];
   FAssetDocumentRuntimeResultDto Validated;
   Validated.RequestId = Request.RequestId; Validated.RuntimeRevision = Request.ExpectedRuntimeRevision;
   Validated.SemanticHash = TEXT("sha256:montage-native-preparation"); Validated.CodecVersion = 1; Validated.CanonicalBytes = {0x7b, 0x7d};
   FAssetDocumentApplyFileRequest Service; Service.FilePath = TEXT("/tmp/MontageNativePreparation.assetdoc.json"); Service.bSaveAsset = false;
   TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
   const auto Prepared = FAssetDocumentGenericMutationPreparation::Prepare(Service, Request, Validated, Validated, Verifier, Diagnostics, &Activation);
   const bool Accepted = Policy == EAssetDocumentRuntimeTargetPolicy::CreateOrUpdate && ActionIndex != 2;
   const FString Label = FString::Printf(TEXT("missing Montage policy=%d action=%s"), static_cast<int32>(Policy), Actions[ActionIndex]);
   FString DiagnosticText;
   for (const auto& D : Diagnostics) DiagnosticText += D.Code + TEXT(" @ ") + D.Path + TEXT(": ") + D.Message + TEXT("; ");
   TestEqual(*(Label + TEXT(" mutation prepared; diagnostics: ") + DiagnosticText), Prepared.Mutation.IsValid(), Accepted);
   if (Accepted && Prepared.Mutation)
   {
    TestTrue(TEXT("native decision records create"), Prepared.TargetDecision.IsSet() && Prepared.TargetDecision->bCreate);
    TestEqual(TEXT("native decision retains lifecycle"), Prepared.TargetDecision->Activation.LifecycleId, FString(TEXT("factory_generic.v1")));
    TestTrue(TEXT("successful preparation has no diagnostics"), Diagnostics.IsEmpty());
    const auto& Footprint = Prepared.Mutation->PreflightReceipt.CompleteFootprint;
    TestEqual(TEXT("creation has exactly five rows"), Footprint.Num(), 5);
    const EAssetDocumentMutationFootprintKind ExpectedKinds[] = {EAssetDocumentMutationFootprintKind::UObjectState, EAssetDocumentMutationFootprintKind::Package, EAssetDocumentMutationFootprintKind::AssetRegistry, EAssetDocumentMutationFootprintKind::File, EAssetDocumentMutationFootprintKind::Handle};
    TSet<FString> StableIds;
    for (const auto Kind : ExpectedKinds)
    {
     int32 Count = 0;
     for (const auto& Row : Footprint) if (Row.Kind == Kind) ++Count;
     TestEqual(TEXT("each required creation footprint kind appears once"), Count, 1);
    }
    for (const auto& Row : Footprint)
    {
     TestEqual(TEXT("every row uses created-asset rollback"), Row.RollbackComponentId, FString(TEXT("editor.asset.rollback_created.v1")));
     TestFalse(TEXT("stable id is nonempty"), Row.StableId.IsEmpty()); StableIds.Add(Row.StableId);
    }
    TestEqual(TEXT("five independent stable identities"), StableIds.Num(), 5);
    TestEqual(TEXT("single lifecycle rollback adapter"), Prepared.Mutation->RollbackActions.Num(), 1);
   }
   else if (!Accepted)
   {
    const FString Expected = ActionIndex == 2 ? TEXT("GenericRuntime.UpdateTargetMissing") : TEXT("GenericRuntime.UpdateOnlyActionRequired");
    TestTrue(TEXT("missing/policy rejection reports exact diagnostic"), Diagnostics.ContainsByPredicate([&](const auto& D) { return D.Code == Expected; }));
   }
   TestNull(TEXT("preparation never creates object"), FindObject<UObject>(nullptr, *Target));
   TestNull(TEXT("preparation never creates package"), FindPackage(nullptr, *Package));
   TestFalse(TEXT("preparation leaves registry absent"), IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Target)).IsValid());
   TestFalse(TEXT("preparation never writes package file"), IFileManager::Get().FileExists(*Filename));
  }
 }
 TestEqual(TEXT("preparation does not execute fresh persistence verifier"), Verifier.Calls, 0);
 return true;
}
#endif
