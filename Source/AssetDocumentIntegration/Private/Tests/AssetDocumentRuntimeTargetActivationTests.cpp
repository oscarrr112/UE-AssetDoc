// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentRuntimeTargetActivation.h"
#include "Misc/AutomationTest.h"
#include "AssetDocumentGenericRuntimeRoute.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FAssetDocumentRuntimeValueDto ActivationString(const FString& Value)
{ FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::String; R.StringValue = Value; return R; }
FAssetDocumentRuntimeValueDto ActivationProjection()
{
 FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::Object;
 R.ObjectKeys = {TEXT("Class"), TEXT("BodySections"), TEXT("RuntimeRegistration")};
 FAssetDocumentRuntimeValueDto Sections; Sections.Kind = EAssetDocumentRuntimeValueKind::Array; Sections.Elements = {ActivationString(TEXT("Skeleton"))};
 FAssetDocumentRuntimeValueDto Requirements; Requirements.Kind = EAssetDocumentRuntimeValueKind::Array;
 const TCHAR* Ids[] = {TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"), TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.scratch.create_by_factory.v1"), TEXT("editor.asset.create_by_factory.v2"), TEXT("editor.asset.rollback_created.v1")};
 for (const TCHAR* Id : Ids)
 {
  FAssetDocumentRuntimeValueDto Row, Version; Row.Kind = EAssetDocumentRuntimeValueKind::Object; Version.Kind = EAssetDocumentRuntimeValueKind::Integer; Version.IntegerValue = FString(Id).EndsWith(TEXT(".v2")) ? 2 : 1;
  Row.ObjectKeys = {TEXT("PrimitiveId"), TEXT("Version"), TEXT("RequiredByRouteId")};
  Row.Elements = {ActivationString(Id), Version, ActivationString(TEXT("animation_profile"))}; Requirements.Elements.Add(Row);
 }
 FAssetDocumentRuntimeValueDto Registration; Registration.Kind = EAssetDocumentRuntimeValueKind::Object;
 Registration.ObjectKeys = {TEXT("LifecycleId"), TEXT("TargetPolicy"), TEXT("RequiredPrimitives")};
 Registration.Elements = {ActivationString(TEXT("factory_generic.v1")), ActivationString(TEXT("CreateOrUpdate")), Requirements};
 R.Elements = {ActivationString(TEXT("/Script/Engine.AnimMontage")), Sections, Registration}; return R;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRuntimeTargetActivationDtoTest,
 "UEAssetDoc.GenericRuntime.TargetLifecycle.ActivationDto",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentRuntimeTargetActivationDtoTest::RunTest(const FString& Parameters)
{
 (void)Parameters;
 const auto Catalog = GetAssetDocumentEditorTargetLifecycleCatalog();
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 const auto Good = ParseAndValidateRuntimeTargetActivation(ActivationProjection(), Catalog, Diagnostics);
 if (!TestTrue(TEXT("exact test-owned projection accepted"), Good.IsSet())) return false;
 TestEqual(TEXT("full ordered tuple retained"), Good->RequiredPrimitives.Num(), 6);
 TestEqual(TEXT("caller first"), Good->RequiredPrimitives[0].PrimitiveId, FString(TEXT("editor.class.resolve_canonical.v1")));
 TestEqual(TEXT("rollback last"), Good->RequiredPrimitives[5].PrimitiveId, FString(TEXT("editor.asset.rollback_created.v1")));
 auto Reject = [&](const TCHAR* Label, FAssetDocumentRuntimeValueDto P)
 { Diagnostics.Reset(); TestFalse(Label, ParseAndValidateRuntimeTargetActivation(P, Catalog, Diagnostics).IsSet()); TestFalse(TEXT("rejection has diagnostic"), Diagnostics.IsEmpty()); };
 auto P = ActivationProjection(); P.ObjectKeys.Add(TEXT("Unknown")); P.Elements.Add(ActivationString(TEXT("x"))); Reject(TEXT("unknown top-level field rejected"), P);
 P = ActivationProjection(); P.Elements[2].ObjectKeys.RemoveAt(1); P.Elements[2].Elements.RemoveAt(1); Reject(TEXT("missing policy rejected"), P);
 P = ActivationProjection(); P.Elements[2].Elements[1] = ActivationString(TEXT("create_or_update")); Reject(TEXT("unknown policy rejected"), P);
 P = ActivationProjection(); { const auto Duplicate = P.Elements[2].Elements[2].Elements[0]; P.Elements[2].Elements[2].Elements.Add(Duplicate); } Reject(TEXT("duplicate requirement rejected"), P);
 P = ActivationProjection(); P.Elements[2].Elements[2].Elements.RemoveAt(5); Reject(TEXT("missing lifecycle requirement rejected"), P);
 P = ActivationProjection(); P.Elements[2].Elements[2].Elements[2].Elements[1].IntegerValue = 0; Reject(TEXT("zero requirement version rejected"), P);
 P = ActivationProjection(); P.Elements[2].Elements[2].Elements[2].Elements[2].StringValue = TEXT("other"); Reject(TEXT("lifecycle consumer drift rejected"), P);
 P = ActivationProjection(); P.Elements[2].Elements[0].StringValue = TEXT("unknown.v1"); Reject(TEXT("unknown lifecycle rejected"), P);
 P = ActivationProjection(); P.Elements[2].Elements[0].StringValue = TEXT("animation_montage_exact.v1"); Reject(TEXT("retired Montage-specific lifecycle rejected"), P);
 P = ActivationProjection(); P.ObjectKeys[1] = TEXT("Class"); Reject(TEXT("duplicate DTO key rejected"), P);
 P = ActivationProjection(); P.Elements[2].Elements[1].StringValue = TEXT("UpdateOnly"); Diagnostics.Reset();
 const auto UpdateOnly = ParseAndValidateRuntimeTargetActivation(P, Catalog, Diagnostics);
 TestTrue(TEXT("UpdateOnly wire accepted"), UpdateOnly.IsSet() && UpdateOnly->TargetPolicy == EAssetDocumentRuntimeTargetPolicy::UpdateOnly);
 P.Elements[2].ObjectKeys.RemoveAt(0); P.Elements[2].Elements.RemoveAt(0);
 P.Elements[0] = ActivationString(TEXT("/Script/Engine.Blueprint"));
 Diagnostics.Reset(); const auto Existing = ParseAndValidateRuntimeTargetActivation(P, Catalog, Diagnostics);
 TestTrue(TEXT("exact two-field existing-only has no creation lifecycle"), Existing.IsSet() && Existing->bExistingOnly && Existing->LifecycleId.IsEmpty());

 // Any other class can opt into the class-neutral generic factory lifecycle under its own route.
 auto Generic = [](const TCHAR* Class, const TCHAR* Lifecycle, const TCHAR* LastRoute)
 {
  auto G = ActivationProjection(); G.Elements[0] = ActivationString(Class); G.Elements[1].Elements.Reset();
  G.Elements[2].Elements[0] = ActivationString(Lifecycle);
  const TCHAR* GenericIds[] = {TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"), TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.scratch.create_by_factory.v1"), TEXT("editor.asset.create_by_factory.v2"), TEXT("editor.asset.rollback_created.v1")};
  auto& Rows = G.Elements[2].Elements[2].Elements;
  for (int32 Index = 0; Index < 6; ++Index)
  {
   Rows[Index].Elements[0] = ActivationString(GenericIds[Index]);
   Rows[Index].Elements[1].IntegerValue = FString(GenericIds[Index]).EndsWith(TEXT(".v2")) ? 2 : 1;
   Rows[Index].Elements[2] = ActivationString(Index == 5 ? LastRoute : TEXT("route.curve.v1"));
  }
  return G;
 };
 Diagnostics.Reset();
 const auto Curve = ParseAndValidateRuntimeTargetActivation(Generic(TEXT("/Script/Engine.CurveFloat"), TEXT("factory_generic.v1"), TEXT("route.curve.v1")), Catalog, Diagnostics);
 TestTrue(TEXT("generic factory lifecycle accepted for a class without a specific row"), Curve.IsSet() && Curve->LifecycleId == TEXT("factory_generic.v1"));
 Reject(TEXT("generic lifecycle rows must share the Profile route"), Generic(TEXT("/Script/Engine.CurveFloat"), TEXT("factory_generic.v1"), TEXT("other.route")));
 Diagnostics.Reset();
 TestTrue(TEXT("Montage shares the generic lifecycle under any one route"), ParseAndValidateRuntimeTargetActivation(Generic(TEXT("/Script/Engine.AnimMontage"), TEXT("factory_generic.v1"), TEXT("route.curve.v1")), Catalog, Diagnostics).IsSet());
 auto BadExisting = P; BadExisting.Elements[2].Elements[0] = ActivationString(TEXT("CreateOrUpdate"));
 Reject(TEXT("missing lifecycle cannot authorize CreateOrUpdate"), BadExisting);
 BadExisting = P; BadExisting.Elements[2].ObjectKeys.Add(TEXT("Unknown")); BadExisting.Elements[2].Elements.Add(ActivationString(TEXT("x")));
 Reject(TEXT("existing-only extra field rejected"), BadExisting);
 auto Reordered = Good.GetValue(); Reordered.RequiredPrimitives.Swap(0, 1);
 TestFalse(TEXT("ordered tuple drift is unequal"), AreAssetDocumentRuntimeTargetActivationsEqual(Good.GetValue(), Reordered));
 TestTrue(TEXT("identical tuple equal"), AreAssetDocumentRuntimeTargetActivationsEqual(Good.GetValue(), Good.GetValue()));
 for (int32 Case = 0; Case != 5; ++Case)
 {
  TArray<FAssetDocumentEditorTargetLifecycleCatalogEntry> Changed;
  Changed.Append(Catalog.GetData(), Catalog.Num());
  // Drift the class-neutral row the Montage projection resolves to.
  const int32 Row = Changed.IndexOfByPredicate([](const FAssetDocumentEditorTargetLifecycleCatalogEntry& Entry) { return Entry.IsClassNeutral(); });
  if (!TestTrue(TEXT("catalog has the generic row"), Row != INDEX_NONE)) return false;
  if (Case == 0) Changed[Row].FactoryId = TEXT("other_factory.v1");
  if (Case == 1) Changed[Row].ScratchCategoryId = TEXT("other.v1");
  if (Case == 2) Changed[Row].CreateContextExactClassPath = TEXT("/Script/Engine.AnimSequence");
  if (Case == 3) Changed[Row].CreateContextHandles.AddDefaulted();
  if (Case == 4) { const auto Duplicate = Changed[Row]; Changed.Add(Duplicate); }
  Diagnostics.Reset(); TestFalse(*FString::Printf(TEXT("catalog drift %d rejected"), Case), ParseAndValidateRuntimeTargetActivation(ActivationProjection(), Changed, Diagnostics).IsSet());
 }
 return true;
}

namespace
{
class FActivationRegistry final : public IAssetDocumentPrimitiveRegistry
{
public:
 TArray<FAssetDocumentPrimitiveRequirement> Admitted;
 FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const override { return {}; }
 FAssetDocumentPrimitiveResolution Resolve(const FAssetDocumentPrimitiveRequirement& R) const override
 {
  FAssetDocumentPrimitiveResolution Out;
  Out.bResolved = Admitted.ContainsByPredicate([&](const auto& A) { return A.PrimitiveId == R.PrimitiveId && A.Version == R.Version && A.RequiredByRouteId == R.RequiredByRouteId; }); return Out;
 }
};
class FActivationHost final : public IAssetDocumentPythonRuntimeHost
{
public:
 FAssetDocumentRuntimeResultDto Dispatch(const FAssetDocumentRuntimeRequestDto&, IAssetDocumentNativeRequestContext&) override { return {}; }
};
class FActivationRuntime final : public IAssetDocumentPythonRuntime
{
public:
 FActivationHost Host;
 FActivationRegistry Registry;
 FAssetDocumentRuntimeValueDto Projection;
 mutable int32 ProjectionReads = 0;
 FAssetDocumentContractValidationResult InitializePythonBinding() override { return {}; }
 FAssetDocumentPythonRevisionActivationResult TryActivateRevision(const FAssetDocumentPythonRevisionCandidate&) override { return {}; }
 uint64 GetActiveRuntimeRevision() const override { return 17; }
 FAssetDocumentRuntimeValueDto GetActiveRuntimeRegistrationProjection() const override { ++ProjectionReads; return Projection; }
 bool HasActiveRequests() const override { return false; }
 IAssetDocumentPythonRuntimeHost& GetHost() override { return Host; }
 const IAssetDocumentPrimitiveRegistry* GetActivePrimitiveRegistry() const override { return &Registry; }
 FAssetDocumentPythonRequestContextCreateResult CreateRequestContext(const FAssetDocumentRequestContextMetadata&, const FGuid&, IAssetDocumentAtomicTransaction*) override { return {}; }
 FAssetDocumentHandleInvalidationResult InvalidateHandles(const FAssetDocumentHandleInvalidation&) override { return {}; }
 void ShutdownPythonBinding() override {}
};
class FActivationCancellation final : public IAssetDocumentCancellationView
{ public: bool IsCancellationRequested() const override { return false; } };
class FActivationObserver final : public IAssetDocumentRequestObserver
{ public: void OnPhaseChanged(FGuid, EAssetDocumentRuntimeRequestPhase) override {} };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRuntimeTargetActivationBindTest,
 "UEAssetDoc.GenericRuntime.TargetLifecycle.ActivationBind",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentRuntimeTargetActivationBindTest::RunTest(const FString& Parameters)
{
 (void)Parameters;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 const auto Good = ParseAndValidateRuntimeTargetActivation(ActivationProjection(), GetAssetDocumentEditorTargetLifecycleCatalog(), Diagnostics);
 if (!TestTrue(TEXT("binding fixture parses"), Good.IsSet())) return false;
 for (int32 Case = 0; Case != 8; ++Case)
 {
  FActivationRuntime Runtime;
  Runtime.Projection.Kind = EAssetDocumentRuntimeValueKind::Array;
  Runtime.Projection.Elements = {ActivationProjection()};
  Runtime.Registry.Admitted = Good->RequiredPrimitives;
  TArray<FAssetDocumentRuntimeTargetActivation> Rows = {Good.GetValue()};
  if (Case == 1) Rows[0].RequiredPrimitives.Swap(0, 1);
  if (Case == 2) Rows[0].ExactAssetClassPath.Reset();
  if (Case == 3) { Rows.Add(Good.GetValue()); Runtime.Projection.Elements.Add(ActivationProjection()); }
  if (Case == 4) Rows[0].TargetPolicy = EAssetDocumentRuntimeTargetPolicy::UpdateOnly;
  if (Case == 5) Runtime.Registry.Admitted.RemoveAt(0);
  if (Case == 6) Runtime.Projection.Elements.Reset();
  FAssetDocumentGenericRuntimeRoute Route(Runtime);
  FActivationCancellation Cancellation; FActivationObserver Observer;
  FAssetDocumentCoreRuntimeComposition Composition({Runtime.Host, Runtime.Registry, Cancellation, Observer, {}, {}});
  if (Case == 7) { Route.BindComposition(Composition); Route.UnbindComposition(); }
  const auto Bound = Route.BindRuntimeTargetActivations(MoveTemp(Rows));
  TestEqual(*FString::Printf(TEXT("bind fixture case %d"), Case), Bound.bValid, Case == 0);
  if (Case == 0)
  {
   const int32 Reads = Runtime.ProjectionReads;
   Runtime.Projection.Elements.Reset();
   const auto Cached = Route.DispatchDescribeRegisteredProfiles();
   TestEqual(TEXT("describe retains bound registry projection"), Cached.Elements.Num(), 1);
   TestEqual(TEXT("describe never re-enters runtime getter"), Runtime.ProjectionReads, Reads);
   TArray<FAssetDocumentRuntimeTargetActivation> Again = {Good.GetValue()};
   TestFalse(TEXT("second bind rejected"), Route.BindRuntimeTargetActivations(MoveTemp(Again)).bValid);
  }
  else TestTrue(TEXT("rejected binding publishes no rows"), Route.DispatchDescribeRegisteredProfiles().Elements.IsEmpty());
 }
 return true;
}
#endif
