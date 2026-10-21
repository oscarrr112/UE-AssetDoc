// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentEditorMutationPreparation.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "Provider/ClassReference/AssetDocumentEditorClassPayload.h"
#include "Provider/ClassReference/AssetDocumentEditorAssetPayload.h"
#include "AssetDocumentEditorGenericProviderTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "Misc/AutomationTest.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorMutationPreparationTests
{
struct FFixture
{
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> Asset;
 FProperty* Boolean = FindFProperty<FProperty>(UAssetDocumentEditorCodecTestObject::StaticClass(), TEXT("BooleanValue"));
 FProperty* Integer = FindFProperty<FProperty>(UAssetDocumentEditorCodecTestObject::StaticClass(), TEXT("IntegerValue"));
 EPropertyFlags BooleanFlags = Boolean->GetPropertyFlags();
 EPropertyFlags IntegerFlags = Integer->GetPropertyFlags();
 FFixture()
  : Package(CreatePackage(*FString::Printf(TEXT("/Game/AssetDocumentTests/MutationPreparation_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))),
    Asset(NewObject<UAssetDocumentEditorCodecTestObject>(Package.Get(), TEXT("Target"), RF_Transactional))
 {
  Boolean->SetPropertyFlags(CPF_Edit);
  Integer->SetPropertyFlags(CPF_Edit);
  Package->SetDirtyFlag(false);
 }
 ~FFixture()
 {
  Boolean->ClearPropertyFlags(CPF_AllFlags); Boolean->SetPropertyFlags(BooleanFlags);
  Integer->ClearPropertyFlags(CPF_AllFlags); Integer->SetPropertyFlags(IntegerFlags);
  if (Package.IsValid()) Package->SetDirtyFlag(false);
 }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  FAssetDocumentRuntimeRequestDto R;
  R.RequestId = FGuid::NewGuid();
  R.ExpectedRuntimeRevision = 1;
  R.Operation = EAssetDocumentRuntimeOperation::Apply;
  R.TargetAssetPath = Asset->GetPathName();
  R.RequestedAssetClassPath = Asset->GetClass()->GetPathName();
  return R;
 }
};

FAssetDocumentMutationIntent MakeIntent()
{
 FAssetDocumentMutationIntent Intent;
 Intent.StepIndex = 0;
 Intent.Requirement = {TEXT("editor.property.write_admitted_batch.v1"), 1, TEXT("animation_profile")};
 Intent.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 Intent.InputSchemaId = TEXT("editor.property.write_admitted_batch.v1.input.v1");
 Intent.Intent.Kind = EAssetDocumentRuntimeValueKind::Object;
 Intent.Intent.ObjectKeys = {TEXT("properties")};
 auto& Rows = Intent.Intent.Elements.AddDefaulted_GetRef(); Rows.Kind = EAssetDocumentRuntimeValueKind::Array;
 auto& Row = Rows.Elements.AddDefaulted_GetRef(); Row.Kind = EAssetDocumentRuntimeValueKind::Object;
 Row.ObjectKeys = {TEXT("name"), TEXT("value")};
 Row.Elements.SetNum(2);
 Row.Elements[0].Kind = EAssetDocumentRuntimeValueKind::String; Row.Elements[0].StringValue = TEXT("BooleanValue");
 Row.Elements[1].Kind = EAssetDocumentRuntimeValueKind::Boolean; Row.Elements[1].BooleanValue = true;
 return Intent;
}

class FAssetPayload final : public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
 explicit FAssetPayload(UObject& InObject) : Object(&InObject), Path(InObject.GetPathName()) {}
 virtual FName GetPayloadTypeId() const override { return UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId; }
 virtual bool IsAlive() const override { return Object.IsValid(); }
 virtual bool TryGetObjectIdentity(UE::AssetDocument::EditorPrimitive::FObjectIdentityView& View) const override
 {
  if (!Object.IsValid()) return false;
  View.Object = Object.Get(); View.CanonicalObjectPath = Path; View.TypeRevision = Object->GetUniqueID(); return true;
 }
private:
 TWeakObjectPtr<UObject> Object;
 FString Path;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMutationPreparationFieldsTest,
 "UEAssetDoc.Editor.MutationPreparation.FieldsOnlyAndLegacy",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentMutationPreparationFieldsTest::RunTest(const FString&)
{
 using namespace AssetDocumentEditorMutationPreparationTests;
 FFixture Fixture;
 const auto Fields = GetAssetDocumentEditorAdmittedPropertyFields(*Fixture.Asset->GetClass());
 auto Capture = CaptureAssetDocumentEditorAdmittedPropertyPreimage(*Fixture.Asset.Get(), Fields);
 if (!TestTrue(TEXT("real admitted capture"), Capture.bSucceeded)) return false;
 Fixture.Asset->BooleanValue = true; Fixture.Package->SetDirtyFlag(true);
 TestTrue(TEXT("fields-only restores typed baseline"), Capture.Preimage.RestoreFieldsOnly().bValid);
 TestFalse(TEXT("boolean baseline restored"), Fixture.Asset->BooleanValue);
 TestTrue(TEXT("fields-only leaves dirty for finalizer"), Fixture.Package->IsDirty());
 Fixture.Package->SetDirtyFlag(false);
 auto Legacy = CaptureAssetDocumentEditorAdmittedPropertyPreimage(*Fixture.Asset.Get(), Fields);
 Fixture.Asset->BooleanValue = true; Fixture.Package->SetDirtyFlag(true);
 TestTrue(TEXT("legacy restore succeeds"), Legacy.Preimage.Restore().bValid);
 TestFalse(TEXT("legacy restore still restores dirty"), Fixture.Package->IsDirty());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMutationPreparationLifetimeTest,
 "UEAssetDoc.Editor.MutationPreparation.StrongLifetimeAndSubset",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentMutationPreparationLifetimeTest::RunTest(const FString&)
{
 using namespace AssetDocumentEditorMutationPreparationTests;
 FFixture Fixture;
 const auto Request = Fixture.Request();
 const TArray<FAssetDocumentMutationIntent> Intents = {MakeIntent()};
 FAssetDocumentPreparedMutationBatch Batch;
 if (!TestTrue(TEXT("prepare real exact subset"), PrepareAssetDocumentEditorMutationBatch(Request, Intents, Batch).bValid)) return false;
 TestEqual(TEXT("one contribution"), Batch.Contributions.Num(), 1);
 TestEqual(TEXT("one physical field"), Batch.Contributions[0].OwnedSurfaceIds.Num(), 1);
 TestEqual(TEXT("canonical field surface"), Batch.Contributions[0].OwnedSurfaceIds[0], Request.TargetAssetPath + TEXT("::BooleanValue"));
 TWeakObjectPtr<UAssetDocumentEditorCodecTestObject> WeakAsset(Fixture.Asset.Get());
 TWeakObjectPtr<UPackage> WeakPackage(Fixture.Package.Get());
 Fixture.Asset.Reset(); Fixture.Package.Reset();
 CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
 if (!TestTrue(TEXT("prepared actions retain exact target and package through GC"), WeakAsset.IsValid() && WeakPackage.IsValid())) return false;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 auto Lease = Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
 if (!TestTrue(TEXT("bind capture once"), Lease.IsValid())) return false;
 WeakAsset->BooleanValue = true; WeakAsset->IntegerValue = 42; WeakPackage->SetDirtyFlag(true);
 Lease->Close();
 TestTrue(TEXT("restore selected field"), Batch.Contributions[0].InitialAction->Rollback(TEXT("test")).bValid);
 TestFalse(TEXT("selected field restored"), WeakAsset->BooleanValue);
 TestEqual(TEXT("unowned field untouched"), WeakAsset->IntegerValue, int64(42));
 TestTrue(TEXT("dirty remains until finalizer"), WeakPackage->IsDirty());
 TestTrue(TEXT("strong package finalizer succeeds"), Batch.PackageFinalizer->FinalizePostRollback(TEXT("test")).bValid);
 TestFalse(TEXT("dirty baseline restored last"), WeakPackage->IsDirty());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMutationPreparationBodyOnlyTest,
 "UEAssetDoc.Editor.MutationPreparation.BodyOnlyAndInverseFailure",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentMutationPreparationBodyOnlyTest::RunTest(const FString&)
{
 using namespace AssetDocumentEditorMutationPreparationTests;
 FFixture Fixture;
 auto Request = Fixture.Request();
 FAssetDocumentPreparedMutationBatch Batch;
 TestTrue(TEXT("empty intents prepare exact package"), PrepareAssetDocumentEditorMutationBatch(Request, {}, Batch).bValid);
 TestTrue(TEXT("body-only has no property step"), Batch.Contributions.IsEmpty());
 if (!TestTrue(TEXT("body-only has finalizer"), Batch.PackageFinalizer.IsValid())) return false;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 auto Lease = Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
 Fixture.Package->SetDirtyFlag(true); Lease->Close();
 TestTrue(TEXT("body-only restores package"), Batch.PackageFinalizer->FinalizePostRollback(TEXT("test")).bValid);
 TestFalse(TEXT("body-only dirty restored"), Fixture.Package->IsDirty());
 const TArray<FAssetDocumentMutationIntent> Intents = {MakeIntent()};
 TestTrue(TEXT("prepare failing inverse case"), PrepareAssetDocumentEditorMutationBatch(Request, Intents, Batch).bValid);
 Lease = Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
 Fixture.Asset->BooleanValue = true; Fixture.Package->SetDirtyFlag(true); Lease->Close();
 Fixture.Boolean->SetPropertyFlags(CPF_EditConst);
 TestFalse(TEXT("inverse fails on lost property write access"), Batch.Contributions[0].InitialAction->Rollback(TEXT("test")).bValid);
 TestTrue(TEXT("package finalizer remains callable after field failure"), Batch.PackageFinalizer->FinalizePostRollback(TEXT("test")).bValid);
 TestFalse(TEXT("package dirtiness restored even when field failed"), Fixture.Package->IsDirty());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMutationPreparationValidationTest,
 "UEAssetDoc.Editor.MutationPreparation.ExactInvocation",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentMutationPreparationValidationTest::RunTest(const FString&)
{
 using namespace AssetDocumentEditorMutationPreparationTests;
 FFixture Fixture;
 auto Request = Fixture.Request();
 const TArray<FAssetDocumentMutationIntent> Intents = {MakeIntent()};
 FAssetDocumentPreparedMutationBatch Batch;
 if (!TestTrue(TEXT("prepare validator"), PrepareAssetDocumentEditorMutationBatch(Request, Intents, Batch).bValid)) return false;
 FAssetDocumentEditorProviderTestContext Context;
 Context.RequestId = Request.RequestId;
 FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
 auto AssetHandle = Handles.Issue(EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, MakeUnique<FAssetPayload>(*Fixture.Asset.Get()));
 auto Admission = AdmitAssetDocumentEditorPropertyBatch(*Fixture.Asset.Get(), Handles);
 if (!TestTrue(TEXT("live handle admission"), AssetHandle.Handle.IsSet() && Admission.bSucceeded && Admission.PropertyBatchHandle.IsSet())) return false;
 FAssetDocumentPrimitiveInvocation Invocation;
 Invocation.RequestId = Request.RequestId; Invocation.TransactionId = Context.TransactionId;
 Invocation.RuntimeRevision = 1; Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 Invocation.PrimitiveId = Intents[0].Requirement.PrimitiveId; Invocation.Version = 1;
 Invocation.ValueArguments = Intents[0].Intent;
 Invocation.HandleArguments = {{TEXT("asset"), AssetHandle.Handle.GetValue()}, {TEXT("property_batch"), Admission.PropertyBatchHandle.GetValue()}};
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 auto Lease = Batch.OpenMutationCapture(Context.TransactionId, Diagnostics);
 auto& Validate = Batch.Contributions[0].ValidateInvocation;
 TestTrue(TEXT("exact typed invocation and real admitted tokens accepted"), Validate(Invocation, Handles).bValid);
 Invocation.ValueArguments.Elements[0].Elements[0].Elements[1].BooleanValue = false;
 TestFalse(TEXT("desired intent drift rejected"), Validate(Invocation, Handles).bValid);
 Invocation.ValueArguments = Intents[0].Intent;
 ++Invocation.HandleArguments[0].Handle.Generation;
 TestFalse(TEXT("generation drift rejected"), Validate(Invocation, Handles).bValid);
 --Invocation.HandleArguments[0].Handle.Generation;
 Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Repair;
 TestFalse(TEXT("phase drift rejected"), Validate(Invocation, Handles).bValid);
 TestFalse(TEXT("validation remains read-only"), Fixture.Asset->BooleanValue);
 TestFalse(TEXT("validation does not dirty package"), Fixture.Package->IsDirty());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMutationPreparationRejectionTest,
 "UEAssetDoc.Editor.MutationPreparation.RejectIntent",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentMutationPreparationRejectionTest::RunTest(const FString&)
{
 using namespace AssetDocumentEditorMutationPreparationTests;
 FFixture Fixture;
 auto Request = Fixture.Request();
 FAssetDocumentPreparedMutationBatch Batch;
 auto Intent = MakeIntent();
 TArray<FAssetDocumentMutationIntent> Intents = {Intent, Intent}; Intents[1].StepIndex = 1;
 TestFalse(TEXT("same physical surface in two steps rejected"), PrepareAssetDocumentEditorMutationBatch(Request, Intents, Batch).bValid);
 TestTrue(TEXT("failed batch is empty"), Batch.Contributions.IsEmpty() && !Batch.PackageFinalizer.IsValid() && !Batch.OpenMutationCapture);
 Intents.SetNum(1);
 Intents[0].Intent.Elements[0].Elements[0].Elements[0].StringValue = TEXT("Skeleton");
 TestFalse(TEXT("arbitrary field string is not native authority"), PrepareAssetDocumentEditorMutationBatch(Request, Intents, Batch).bValid);
 Intents[0] = Intent; Intents[0].InputSchemaId += TEXT(".unknown");
 TestFalse(TEXT("schema drift rejected"), PrepareAssetDocumentEditorMutationBatch(Request, Intents, Batch).bValid);
 Intents[0] = Intent; Intents[0].Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
 TestFalse(TEXT("preflight cannot be a live mutation step"), PrepareAssetDocumentEditorMutationBatch(Request, Intents, Batch).bValid);
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMutationPreparationFactoryTest,
 "UEAssetDoc.Editor.MutationPreparation.FactoryCoverage",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentMutationPreparationFactoryTest::RunTest(const FString&)
{
 using namespace AssetDocumentEditorMutationPreparationTests;
 TStrongObjectPtr<UPackage> SkeletonPackage(CreatePackage(*FString::Printf(TEXT("/Game/AssetDocumentTests/PreparationSkeleton_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
 TStrongObjectPtr<USkeleton> Skeleton(NewObject<USkeleton>(SkeletonPackage.Get(), TEXT("Skeleton"), RF_Transactional));
 FAssetDocumentRuntimeRequestDto Request;
 Request.RequestId = FGuid::NewGuid(); Request.ExpectedRuntimeRevision = 1; Request.Operation = EAssetDocumentRuntimeOperation::Apply;
 const FString AssetName = FString::Printf(TEXT("PreparedMontage_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
 Request.TargetAssetPath = TEXT("/Game/AssetDocumentTests/") + AssetName + TEXT(".") + AssetName;
 Request.RequestedAssetClassPath = UAnimMontage::StaticClass()->GetPathName();
 FAssetDocumentMutationIntent Factory;
 Factory.StepIndex = 0;
 Factory.Requirement = {TEXT("editor.asset.create_by_factory.v2"), 2, TEXT("animation_profile")};
 Factory.InputSchemaId = TEXT("editor.asset.create_by_factory.v2.input.v1"); Factory.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 Factory.Intent.Kind = EAssetDocumentRuntimeValueKind::Object;
 Factory.Intent.ObjectKeys = {TEXT("asset_class_path"), TEXT("factory_context_path"), TEXT("package_path"), TEXT("asset_name")};
 const TArray<FString> Values = {Request.RequestedAssetClassPath, Skeleton->GetPathName(), TEXT("/Game/AssetDocumentTests"), AssetName};
 for (const FString& Value : Values)
 {
  auto& V = Factory.Intent.Elements.AddDefaulted_GetRef(); V.Kind = EAssetDocumentRuntimeValueKind::String; V.StringValue = Value;
 }
 // The factory and a property batch share factory rollback coverage, but
 // raw contributions must still match each unmodified provider's enlist.
 FProperty* RateScale = FindFProperty<FProperty>(UAnimMontage::StaticClass(), TEXT("RateScale"));
 if (!TestNotNull(TEXT("real admitted Montage scalar"), RateScale)) return false;
 FAssetDocumentMutationIntent Property = MakeIntent();
 Property.StepIndex = 1;
 Property.Intent.Elements[0].Elements[0].Elements[0].StringValue = TEXT("RateScale");
 auto& DesiredRate = Property.Intent.Elements[0].Elements[0].Elements[1];
 DesiredRate = {}; DesiredRate.Kind = EAssetDocumentRuntimeValueKind::Number; DesiredRate.NumberValue = 1.5;
 FAssetDocumentPreparedMutationBatch Batch;
 const TArray<FAssetDocumentMutationIntent> Intents = {Factory, Property};
 if (!TestTrue(TEXT("exact L1 factory and property intents prepare"), PrepareAssetDocumentEditorMutationBatch(Request, Intents, Batch).bValid)) return false;
 TestEqual(TEXT("factory plus property has two contributions"), Batch.Contributions.Num(), 2);
 TestEqual(TEXT("create property raw object component remains provider exact"), Batch.Contributions[1].RawFootprint[0].RollbackComponentId, FString(TEXT("rollback.uobject")));
 TestEqual(TEXT("create property raw package component remains provider exact"), Batch.Contributions[1].RawFootprint[1].RollbackComponentId, FString(TEXT("rollback.package")));
 TestFalse(TEXT("create property adds no late inverse"), Batch.Contributions[1].InitialAction.IsValid());
 TestFalse(TEXT("create has no dirty preimage"), Batch.PackageFinalizer.IsValid());
 TestFalse(TEXT("create uses Integration factory inverse, never existing preimage"), Batch.Contributions[0].InitialAction.IsValid());
 TestEqual(TEXT("factory raw batch preserves all lifecycle rows"), Batch.Contributions[0].RawFootprint.Num(), 5);
 for (const auto& Row : Batch.Contributions[0].RawFootprint)
  TestEqual(TEXT("factory rollback component unchanged"), Row.RollbackComponentId, FString(TEXT("editor.asset.rollback_created.v1")));
 FAssetDocumentEditorProviderTestContext Context; Context.RequestId = Request.RequestId;
 FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
 auto ClassHandle = Handles.Issue(EAssetDocumentHandleKind::Class, EAssetDocumentHandleAccess::ReadOnly, MakeAssetDocumentEditorClassPayload(*UAnimMontage::StaticClass()));
 auto SkeletonHandle = Handles.Issue(EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, MakeAssetDocumentEditorAssetPayload(*Skeleton.Get()));
 if (!TestTrue(TEXT("factory read-only class and skeleton handles"), ClassHandle.Handle.IsSet() && SkeletonHandle.Handle.IsSet())) return false;
 FAssetDocumentPrimitiveInvocation Invocation;
 Invocation.PrimitiveId = Factory.Requirement.PrimitiveId; Invocation.Version = 2;
 Invocation.RequestId = Request.RequestId; Invocation.RuntimeRevision = 1; Invocation.TransactionId = Context.TransactionId;
 Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
 Invocation.ValueArguments.ObjectKeys = {TEXT("package_path"), TEXT("asset_name")};
 Invocation.ValueArguments.Elements = {Factory.Intent.Elements[2], Factory.Intent.Elements[3]};
 Invocation.HandleArguments = {{TEXT("asset_class"), ClassHandle.Handle.GetValue()}, {TEXT("factory_context"), SkeletonHandle.Handle.GetValue()}};
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 auto Lease = Batch.OpenMutationCapture(Context.TransactionId, Diagnostics);
 TWeakObjectPtr<USkeleton> WeakSkeleton(Skeleton.Get());
 Skeleton.Reset(); SkeletonPackage.Reset(); CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
 TestTrue(TEXT("factory context reference remains strongly anchored"), WeakSkeleton.IsValid());
 TestTrue(TEXT("exact factory invocation accepted without creating target"), Batch.Contributions[0].ValidateInvocation(Invocation, Handles).bValid);
 Invocation.ValueArguments.Elements[1].StringValue += TEXT("Other");
 TestFalse(TEXT("different live target rejected"), Batch.Contributions[0].ValidateInvocation(Invocation, Handles).bValid);
 Invocation.ValueArguments.Elements[1] = Factory.Intent.Elements[3];
 TStrongObjectPtr<USkeleton> OtherSkeleton(NewObject<USkeleton>(WeakSkeleton->GetOutermost(), TEXT("OtherSkeleton"), RF_Transactional));
 auto OtherHandle = Handles.Issue(EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, MakeAssetDocumentEditorAssetPayload(*OtherSkeleton.Get()));
 if (!TestTrue(TEXT("second valid reference handle"), OtherHandle.Handle.IsSet())) return false;
 Invocation.HandleArguments[1].Handle = OtherHandle.Handle.GetValue();
 TestFalse(TEXT("same-class different skeleton reference rejected"), Batch.Contributions[0].ValidateInvocation(Invocation, Handles).bValid);
 TestNull(TEXT("factory preparation and validation create no asset"), FSoftObjectPath(Request.TargetAssetPath).ResolveObject());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMutationPreparationEmptyBatchTest,
 "UEAssetDoc.Editor.MutationPreparation.EmptyPropertyStep",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentMutationPreparationEmptyBatchTest::RunTest(const FString&)
{
 using namespace AssetDocumentEditorMutationPreparationTests;
 FFixture Fixture;
 auto Intent = MakeIntent(); Intent.Intent.Elements[0].Elements.Reset();
 const TArray<FAssetDocumentMutationIntent> Intents = {Intent};
 FAssetDocumentPreparedMutationBatch Batch;
 if (!TestTrue(TEXT("empty admitted batch prepares one no-op step"), PrepareAssetDocumentEditorMutationBatch(Fixture.Request(), Intents, Batch).bValid)) return false;
 TestEqual(TEXT("expected no-op step retained"), Batch.Contributions.Num(), 1);
 TestTrue(TEXT("no-op footprint empty"), Batch.Contributions[0].RawFootprint.IsEmpty());
 TestTrue(TEXT("no-op owns no authored surface"), Batch.Contributions[0].OwnedSurfaceIds.IsEmpty());
 TestTrue(TEXT("no-op retains safe initial action"), Batch.Contributions[0].InitialAction.IsValid());
 TestTrue(TEXT("empty batch still prepares package"), Batch.PackageFinalizer.IsValid());
 return true;
}
#endif
