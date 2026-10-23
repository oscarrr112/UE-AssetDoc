// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/AssetDocumentSkeletonTestHarness.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "HAL/PlatformProcess.h"

namespace AssetDocumentPreviewMeshIntegrationTests
{
using namespace AssetDocumentSkeletonTests;
constexpr const TCHAR* Primitive = TEXT("animation.asset.preview_mesh.set.v1");

FOptions Options(const FString& Failure = {})
{
 FOptions O;
 O.FailAfter = Failure;
 O.ExtraRequirements.Add({Primitive, 1, TEXT("animation_profile")});
 // Test-owned caller extends the Skeleton caller's phase/host/evidence machinery. No production
 // registration or Python file is changed. Preview has no read-only primitive:
 // its provider checks completion, and native assertions below check live/durable
 // state. This is not a claim of production canonical Preview extraction.
 O.EntrySuffix = TEXT(R"PY(
PREVIEW = 'animation.asset.preview_mesh.set.v1'
row = caller.PrimitiveRequirement(PREVIEW, 1, 'animation_profile')
caller.REQUIREMENTS += (row,)
caller._BY_ID[PREVIEW] = row
_original_interpret = caller.interpret

def interpret_preview(document):
    body = document.get('Body', {})
    if type(body) is not dict or set(body) != {'PreviewMesh'}:
        raise caller._error('Preview test requires one explicit PreviewMesh leaf.')
    properties, _ = _original_interpret(dict(document, Body={}))
    value = body['PreviewMesh']
    return properties, {'operation': 'clear' if value is None else 'set',
                        'preview_mesh_path': None if value is None else caller.canonical_reference(value)}
caller.interpret = interpret_preview

class PreviewDispatch(caller.SkeletonS1Dispatch):
    def registration_projection(self):
        result = super().registration_projection()
        result['BodySections'] = ['PreviewMesh']
        return result

    def _plan(self, request, context, properties, preview):
        if request.canonical_document.get('Action') != 'Update':
            raise caller._error('Preview fixture only supports an existing target.')
        steps, exists, read, mutable = super()._plan(request, context, properties, None)
        steps.append(caller._step(len(steps), PREVIEW, preview))
        return steps, exists, read, mutable

    def _mesh(self, context, path):
        exists, read, _ = caller._resolve(context, path, '/Script/Engine.SkeletalMesh')
        if not exists:
            raise caller._error('Preview fixture mesh is missing.')
        return context.handoff(source_handle=read, target_provider_id='animation', access='read_only')

    def _preflight(self, request, context, properties, preview, steps, exists, read):
        super()._preflight(request, context, properties, None, steps[:-1], exists, read)
        if preview['operation'] == 'set':
            self._mesh(context, preview['preview_mesh_path'])

    def _apply(self, request, context, properties, steps, read, mutable):
        changed = list(super()._apply(request, context, properties, steps[:-1], read, mutable))
        intent = steps[-1]['Intent']
        handles = {'asset': context.handoff(source_handle=mutable, target_provider_id='animation', access='mutable')}
        if intent['operation'] == 'set':
            handles['preview_mesh'] = self._mesh(context, intent['preview_mesh_path'])
        changed.extend(caller._invoke(context, PREVIEW, {'operation': intent['operation']}, **handles).changed_set)
        return tuple(changed)

    def _verify(self, request, context, properties, preview):
        # The Skeleton caller verifies the admitted scalar. Native const getter checks Preview
        # after terminal; FreshRead checks its ordinary same-path deserialization.
        super()._verify(request, context, properties, None)

dispatch = PreviewDispatch(dispatch.options)
)PY");
 return O;
}

USkeletalMesh* Preview(const UAnimMontage& Montage) { return Montage.GetPreviewMesh(); }
TArray<FRichCurveKey> CurveKeys()
{
 FRichCurveKey A(0.0f, 1.25f), B(0.5f, -2.0f);
 for (auto* Key : {&A, &B})
 {
  Key->InterpMode = RCIM_Cubic; Key->TangentMode = RCTM_Break;
  Key->TangentWeightMode = RCTWM_WeightedBoth;
  Key->ArriveTangent = 1.5f; Key->LeaveTangent = -0.25f;
  Key->ArriveTangentWeight = 0.2f; Key->LeaveTangentWeight = 0.4f;
 }
 return {A, B};
}
bool Retained(FAutomationTestBase& Test, const UAnimMontage& Montage, const FString& SkeletonPath, const FGuid& Guid)
{
 bool Ok = Test.TestEqual(TEXT("Skeleton reference preserved"), Montage.GetSkeleton() ? Montage.GetSkeleton()->GetPathName() : FString(), SkeletonPath);
 Ok &= Test.TestEqual(TEXT("Skeleton GUID preserved"), Montage.GetSkeletonGuid(), Guid);
 const IAnimationDataModel* Model = Montage.GetDataModel();
 if (!Test.TestNotNull(TEXT("nonempty curve data model retained"), Model)) return false;
 const auto& Curves = Model->GetFloatCurves();
 if (!Test.TestEqual(TEXT("float curve count retained"), Curves.Num(), 1)) return false;
 Ok &= Test.TestEqual(TEXT("curve name retained"), Curves[0].GetName(), FName(TEXT("PreviewUntouched")));
 Ok &= Test.TestEqual(TEXT("curve flags retained"), Curves[0].GetCurveTypeFlags(), static_cast<int32>(AACF_Editable));
 const auto& ActualKeys = Curves[0].FloatCurve.GetConstRefOfKeys();
 const auto ExpectedKeys = CurveKeys();
 Ok &= Test.TestTrue(TEXT("curve key values, modes and tangents retained"), ActualKeys == ExpectedKeys);
 // FRichCurveKey::operator== omits tangent weights in this Engine version.
 if (ActualKeys.Num() == ExpectedKeys.Num()) for (int32 I = 0; I < ActualKeys.Num(); ++I)
 {
  Ok &= Test.TestEqual(TEXT("arrive weight retained"), ActualKeys[I].ArriveTangentWeight, ExpectedKeys[I].ArriveTangentWeight);
  Ok &= Test.TestEqual(TEXT("leave weight retained"), ActualKeys[I].LeaveTangentWeight, ExpectedKeys[I].LeaveTangentWeight);
 }
 Ok &= Test.TestEqual(TEXT("curve default retained"), Curves[0].FloatCurve.GetDefaultValue(), MAX_flt);
 Ok &= Test.TestEqual(TEXT("curve pre extrapolation retained"), Curves[0].FloatCurve.PreInfinityExtrap.GetValue(), RCCE_Constant);
 Ok &= Test.TestEqual(TEXT("curve post extrapolation retained"), Curves[0].FloatCurve.PostInfinityExtrap.GetValue(), RCCE_Constant);
 return Ok;
}
struct FPreviewAssets
{
 FAssets Base;
 TStrongObjectPtr<USkeletalMesh> A, B;
 bool Setup(FAutomationTestBase& Test)
 {
  if (!Base.Setup()) return false;
  {
   FReferenceSkeletonModifier SkeletonModifier(Base.Old.Get());
   SkeletonModifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
  }
  if (!Save(Base.Old.Get())) return false;
  auto Mesh = [&](const TCHAR* Name)
  {
   USkeletalMesh* M = NewObject<USkeletalMesh>(CreatePackage(*(Base.Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone | RF_Transactional);
   M->SetSkeleton(Base.Old.Get());
   FReferenceSkeletonModifier Modifier(M->GetRefSkeleton(), Base.Old.Get());
   Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
   FAssetRegistryModule::AssetCreated(M);
   return M;
  };
  A.Reset(Mesh(TEXT("PreviewA"))); B.Reset(Mesh(TEXT("PreviewB")));
  if (!Save(A.Get()) || !Save(B.Get())) return false;
  IAnimationDataController& Controller = Base.Montage->GetController();
  Controller.InitializeModel();
  Controller.SetFrameRate(FFrameRate(30, 1), false);
  Controller.SetNumberOfFrames(FFrameNumber(30), false);
  const FAnimationCurveIdentifier Id(FName(TEXT("PreviewUntouched")), ERawCurveTrackTypes::RCT_Float);
  if (!Controller.AddCurve(Id, AACF_Editable, false) || !Controller.SetCurveKeys(Id, CurveKeys(), false)) return false;
  Controller.NotifyPopulated();
  return Save(Base.Montage.Get()) && Retained(Test, *Base.Montage, Base.Old->GetPathName(), Base.Old->GetGuid());
 }
 bool Document(USkeletalMesh* Mesh)
 {
  return Base.Document(Mesh ? FString::Printf(TEXT("{\"PreviewMesh\":\"%s\"}"), *Mesh->GetPathName()) : TEXT("{\"PreviewMesh\":null}"));
 }
 ~FPreviewAssets()
 {
  if (Base.bKeep) return;
  for (auto* Mesh : {A.Get(), B.Get()}) if (IsValid(Mesh))
  {
   auto* Package = Mesh->GetOutermost(); FAssetRegistryModule::AssetDeleted(Mesh);
   Mesh->ClearFlags(RF_Public | RF_Standalone);
   Mesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
   Mesh->MarkAsGarbage(); Package->SetDirtyFlag(false); Package->MarkAsGarbage();
  }
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewMeshApplyFileTest,
 "UEAssetDoc.PreviewMeshIntegration.ApplyFile.Success", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPreviewMeshApplyFileTest::RunTest(const FString&)
{
 using namespace AssetDocumentPreviewMeshIntegrationTests;
 FPreviewAssets F; if (!TestTrue(TEXT("saved mesh and nonempty curve fixtures"), F.Setup(*this))) return false;
 const auto ABytes = Bytes(PackageFile(F.A->GetPathName())), BBytes = Bytes(PackageFile(F.B->GetPathName()));
 const auto SkeletonBytes = Bytes(PackageFile(F.Base.Old->GetPathName()));
 FHarness H(Options()); if (!H.Initialize(*this)) return false;
 for (USkeletalMesh* Desired : {F.A.Get(), F.B.Get(), static_cast<USkeletalMesh*>(nullptr)})
 {
  if (!TestTrue(TEXT("real authored Preview sidecar"), F.Document(Desired))) return false;
  const int32 Before = H.Changes.Num();
  const auto Result = H.Apply(F.Base.Sidecar); for (const auto& D : Result.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  if (!TestTrue(TEXT("Preview ApplyFile persisted"), Result.IsSuccess())) return false;
  TestEqual(TEXT("const live Preview readback"), Preview(*F.Base.Montage), Desired);
  TestTrue(TEXT("Preview exact ChangedSet"), H.Changes.ContainsByPredicate([&](const auto& C)
   { return C.AuthorityId == Primitive && C.StableObjectId == F.Base.Target && C.Path == TEXT("/PreviewMesh") && C.ChangeKind == (Desired ? TEXT("Set") : TEXT("Clear")); }));
  TestTrue(TEXT("Preview mutation recorded"), H.Changes.Num() > Before);
  Retained(*this, *F.Base.Montage, F.Base.Old->GetPathName(), F.Base.Old->GetGuid());
  TestFalse(TEXT("persisted target clean"), F.Base.Montage->GetOutermost()->IsDirty());
 }
 TestTrue(TEXT("A package preserved"), Bytes(PackageFile(F.A->GetPathName())) == ABytes);
 TestTrue(TEXT("B package preserved"), Bytes(PackageFile(F.B->GetPathName())) == BBytes);
 TestTrue(TEXT("Skeleton package preserved"), Bytes(PackageFile(F.Base.Old->GetPathName())) == SkeletonBytes);
 TestEqual(TEXT("all calls enter real sidecar route"), H.SourcePaths.Num(), 3);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewMeshApplyFileRollbackTest,
 "UEAssetDoc.PreviewMeshIntegration.ApplyFile.Rollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPreviewMeshApplyFileRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentPreviewMeshIntegrationTests;
 for (const FString Failure : {TEXT("repair"), TEXT("verify")})
 for (const bool bClear : {false, true})
 {
  FPreviewAssets F; if (!TestTrue(TEXT("rollback fixtures saved"), F.Setup(*this))) return false;
  F.Base.Montage->SetPreviewMesh(F.A.Get(), false);
  if (!TestTrue(TEXT("saved nonempty inverse baseline"), Save(F.Base.Montage.Get()) && F.Document(bClear ? nullptr : F.B.Get()))) return false;
  const auto Package = Bytes(PackageFile(F.Base.Target)), Sidecar = Bytes(F.Base.Sidecar);
  FHarness H(Options(Failure)); if (!H.Initialize(*this)) return false;
  const auto Result = H.Apply(F.Base.Sidecar); for (const auto& D : Result.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  TestFalse(TEXT("later injected failure rejects ApplyFile"), Result.IsSuccess());
  const bool bStagedVerify = Failure == TEXT("verify");
  // Fresh verification intentionally maps a failed runtime Verify to a
  // canonical mismatch; its invalid receipt then stops the coordinator.
  // Repair fails directly before persistence. Assert both concrete mappings
  // and the observed phase boundary, rather than accepting arbitrary failure.
  TestEqual(TEXT("only verify injection reaches persistence"), H.Observer.Phases.Contains(EAssetDocumentRuntimeRequestPhase::Persist), bStagedVerify);
  if (bStagedVerify)
  {
   TestTrue(TEXT("staged verify reports canonical mismatch"), Result.Diagnostics.ContainsByPredicate([](const auto& D)
    { return D.Code == TEXT("Persistence.CanonicalMismatch") && D.Message == TEXT("Freshly loaded staged package did not match canonical document semantics."); }));
   TestTrue(TEXT("failed staged verify rejects durable receipt"), Result.Diagnostics.ContainsByPredicate([](const auto& D)
    { return D.Code == TEXT("RequestCoordinator.InvalidDurableReceipt"); }));
  }
  else
  {
   TestTrue(TEXT("repair injection remains the direct failure"), Result.Diagnostics.ContainsByPredicate([](const auto& D)
    { return D.Code == TEXT("S1Fixture.InjectedFailure") && D.Message == TEXT("Injected failure after repair"); }));
  }
  TestTrue(TEXT("real Preview forward ran before failure"), H.Changes.ContainsByPredicate([](const auto& C) { return C.AuthorityId == Primitive; }));
  TestTrue(TEXT("real Preview inverse ran"), H.Inverses.Contains(Primitive));
  TestEqual(TEXT("old nonempty Preview restored"), Preview(*F.Base.Montage), F.A.Get());
  TestEqual(TEXT("property inverse restored"), F.Base.Montage->RateScale, 1.0f);
  Retained(*this, *F.Base.Montage, F.Base.Old->GetPathName(), F.Base.Old->GetGuid());
  TestFalse(TEXT("dirty restored"), F.Base.Montage->GetOutermost()->IsDirty());
  TestTrue(TEXT("target package bytes symmetric"), Bytes(PackageFile(F.Base.Target)) == Package);
  TestTrue(TEXT("sidecar bytes symmetric"), Bytes(F.Base.Sidecar) == Sidecar);
  TestTrue(TEXT("rollback invalidation observed"), H.Invalidations.ContainsByPredicate([](const auto& I) { return I.Reason == EAssetDocumentHandleInvalidationReason::Rollback; }));
  TestFalse(TEXT("failure cannot commit"), H.Invalidations.ContainsByPredicate([](const auto& I) { return I.Reason == EAssetDocumentHandleInvalidationReason::Commit; }));
 }
 return !HasAnyErrors();
}

#endif
