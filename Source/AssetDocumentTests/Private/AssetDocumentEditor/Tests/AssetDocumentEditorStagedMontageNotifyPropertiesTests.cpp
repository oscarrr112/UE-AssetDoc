// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentEditorNativeState.h"
#include "AssetDocumentEditorStagedMontageNotifyTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "Animation/AnimMontage.h"
#include "Misc/AutomationTest.h"
#include "Provider/ClassReference/AssetDocumentEditorAssetPayload.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyEdit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_EDITOR
void UAssetDocumentEditorStagedNotifyTest::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
 Super::PostEditChangeProperty(Event);
 if (bInvalidateAfterFirstWrite && Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UAssetDocumentEditorStagedNotifyTest, First))
  ClearFlags(RF_Transactional);
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
namespace EditorStagedMontageNotifyPropertiesTestsLocal
{
FAssetDocumentRuntimeValueDto Integer(int32 Value)
{
 FAssetDocumentRuntimeValueDto Out; Out.Kind = EAssetDocumentRuntimeValueKind::Integer; Out.IntegerValue = Value; return Out;
}
FAssetDocumentRuntimeValueDto Properties(bool bTwo = true)
{
 FAssetDocumentRuntimeValueDto Out; Out.Kind = EAssetDocumentRuntimeValueKind::Object;
 Out.ObjectKeys.Add(TEXT("First")); Out.Elements.Add(Integer(17));
 if (bTwo) { Out.ObjectKeys.Add(TEXT("Second")); Out.Elements.Add(Integer(29)); }
 return Out;
}
bool HasCode(const FAssetDocumentPrimitiveOutcome& Out, const TCHAR* Code)
{
 return Out.Diagnostics.ContainsByPredicate([&](const auto& D) { return D.Code == Code; });
}
struct FFixture
{
 FAssetDocumentEditorProviderTestContext Context;
 TUniquePtr<FAssetDocumentEditorProviderTestHandleResolver> Resolver;
 TUniquePtr<FAssetDocumentEditorProviderTestTransaction> Transaction;
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UAnimMontage> Montage;
 TUniquePtr<IAssetDocumentVerifiedHandleLease> Lease;
 FAssetDocumentPrimitiveInvocation Invocation;
 FFixture()
 {
  Context.ProviderId = TEXT("animation");
  Resolver = MakeUnique<FAssetDocumentEditorProviderTestHandleResolver>(Context);
  Transaction = MakeUnique<FAssetDocumentEditorProviderTestTransaction>(Context);
  const FString Path = TEXT("/Temp/AssetDocumentStagedNotify_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  Package.Reset(CreatePackage(*Path));
  Montage.Reset(NewObject<UAnimMontage>(Package.Get(), TEXT("Montage"), RF_Public | RF_Standalone | RF_Transactional));
  auto Issued = Resolver->Issue(EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, MakeAssetDocumentEditorAssetPayload(*Montage.Get()));
  if (!Issued.Handle.IsSet()) return;
  FAssetDocumentHandleValidationContext Validation;
  Validation.RequestId = Context.RequestId; Validation.TransactionId = Context.TransactionId; Validation.RuntimeRevision = Context.RuntimeRevision;
  Validation.ProviderId = Context.ProviderId; Validation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  Validation.RequiredKind = EAssetDocumentHandleKind::Asset; Validation.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
  auto Acquired = Resolver->AcquireVerifiedLease(*Issued.Handle, Validation);
  Lease = MoveTemp(Acquired.Lease);
  Invocation.PrimitiveId = TEXT("animation.montage.notifies.replace.v1"); Invocation.Version = 1;
  Invocation.RequestId = Context.RequestId; Invocation.TransactionId = Context.TransactionId; Invocation.RuntimeRevision = Context.RuntimeRevision;
  Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply; Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
  Invocation.HandleArguments.Add({TEXT("asset"), *Issued.Handle});
  const TArray<FAssetDocumentMutationFootprint> ParentFootprint = {
   {EAssetDocumentMutationFootprintKind::UObjectState, TEXT("Object:") + Montage->GetPathName(), TEXT("rollback.uobject")},
   {EAssetDocumentMutationFootprintKind::Package, TEXT("Package:") + Package->GetName(), TEXT("rollback.package")}};
  Transaction->EnlistFootprint(ParentFootprint);
 }
 FAssetDocumentPrimitiveOutcome Configure(UObject& Object, const FAssetDocumentRuntimeValueDto& Values)
 {
  const bool bStates = Invocation.PrimitiveId == TEXT("animation.montage.notify_states.replace.v1");
  FAssetDocumentRuntimeValueDto Class; Class.Kind = EAssetDocumentRuntimeValueKind::String;
  Class.StringValue = Object.GetClass()->GetPathName();
  FAssetDocumentRuntimeValueDto Requested; Requested.Kind = EAssetDocumentRuntimeValueKind::Object;
  Requested.ObjectKeys = {TEXT("class"), TEXT("properties")}; Requested.Elements = {Class, Values};
  FAssetDocumentRuntimeValueDto Row; Row.Kind = EAssetDocumentRuntimeValueKind::Object;
  Row.ObjectKeys = bStates ? TArray<FString>{TEXT("time"), TEXT("duration"), TEXT("track_index"), TEXT("object")}
   : TArray<FString>{TEXT("time"), TEXT("track_index"), TEXT("object")};
  Row.Elements.Add(Integer(0));
  if (bStates) Row.Elements.Add(Integer(1));
  Row.Elements.Add(Integer(0)); Row.Elements.Add(MoveTemp(Requested));
  FAssetDocumentRuntimeValueDto Rows; Rows.Kind = EAssetDocumentRuntimeValueKind::Array; Rows.Elements.Add(MoveTemp(Row));
  Invocation.ValueArguments.ObjectKeys = {bStates ? TEXT("notify_states") : TEXT("notifies")};
  Invocation.ValueArguments.Elements = {MoveTemp(Rows)};
  return ConfigureAssetDocumentEditorStagedMontageNotifyProperties(Invocation, *Lease, Object, Values, *Transaction);
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedNotifyPropertiesSuccess,
 "UEAssetDoc.Editor.StagedMontageNotify.PropertiesSuccess",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStagedNotifyPropertiesSuccess::RunTest(const FString&)
{
 using namespace EditorStagedMontageNotifyPropertiesTestsLocal;
 FFixture F;
 if (!TestTrue(TEXT("verified target lease"), F.Lease.IsValid())) return false;
 TStrongObjectPtr<UAssetDocumentEditorStagedNotifyTest> Notify(NewObject<UAssetDocumentEditorStagedNotifyTest>(F.Montage.Get(), TEXT("AssetDocumentManaged_Notify_Test"), RF_Transactional));
 auto Out = F.Configure(*Notify.Get(), Properties());
 TestTrue(TEXT("Notify configuration succeeds"), Out.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 TestEqual(TEXT("first authored value"), Notify->First, 17);
 TestEqual(TEXT("second authored value"), Notify->Second, 29);
 TestEqual(TEXT("readback includes both requested fields"), Out.Value.Elements.Num(), 2);
 TestEqual(TEXT("one parent footprint remains two rows"), F.Transaction->GetEnlistedFootprint().Num(), 2);
 TestTrue(TEXT("Configure does not enlist inverses"), F.Transaction->EnlistedRollbackActionComponentIds.IsEmpty());
 F.Invocation.PrimitiveId = TEXT("animation.montage.notify_states.replace.v1");
 TStrongObjectPtr<UAssetDocumentEditorStagedNotifyStateTest> State(NewObject<UAssetDocumentEditorStagedNotifyStateTest>(F.Montage.Get(), TEXT("AssetDocumentManaged_NotifyState_Test"), RF_Transactional));
 Out = F.Configure(*State.Get(), Properties(false));
 TestTrue(TEXT("State configuration succeeds"), Out.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 TestEqual(TEXT("state authored value"), State->First, 17);
 TestEqual(TEXT("State does not add another footprint"), F.Transaction->GetEnlistedFootprint().Num(), 2);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedNotifyPropertiesAdmission,
 "UEAssetDoc.Editor.StagedMontageNotify.PredecodeAndContext",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStagedNotifyPropertiesAdmission::RunTest(const FString&)
{
 using namespace EditorStagedMontageNotifyPropertiesTestsLocal;
 FFixture F;
 if (!TestTrue(TEXT("verified target lease"), F.Lease.IsValid())) return false;
 TStrongObjectPtr<UAssetDocumentEditorStagedNotifyTest> Notify(NewObject<UAssetDocumentEditorStagedNotifyTest>(F.Montage.Get(), TEXT("AssetDocumentManaged_Notify_Test"), RF_Transactional));
 auto Invalid = Properties(); Invalid.Elements[1].Kind = EAssetDocumentRuntimeValueKind::String; Invalid.Elements[1].IntegerValue = 0; Invalid.Elements[1].StringValue = TEXT("invalid integer");
 auto Out = F.Configure(*Notify.Get(), Invalid);
 TestTrue(TEXT("second decode fails with existing diagnostic"), HasCode(Out, TEXT("PropertyWrite.DecodeFailed")));
 TestEqual(TEXT("all predecode completes before first setter"), Notify->First, 0);
 Invalid = Properties(); Invalid.ObjectKeys[1] = TEXT("UnknownProperty");
 Out = F.Configure(*Notify.Get(), Invalid);
 TestTrue(TEXT("unknown property is rejected"), HasCode(Out, TEXT("PropertyBatchWrite.UnknownProperty")));
 TestEqual(TEXT("unknown trailing property leaves first untouched"), Notify->First, 0);
 F.Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Repair;
 TestTrue(TEXT("Repair is not admitted"), HasCode(F.Configure(*Notify.Get(), Properties()), TEXT("EditorNativeState.StagedNotifyInvalidContext")));
 F.Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 ++F.Invocation.HandleArguments[0].Handle.Generation;
 TestTrue(TEXT("invocation must contain the exact leased handle"), HasCode(F.Configure(*Notify.Get(), Properties()), TEXT("EditorNativeState.StagedNotifyInvalidContext")));
 --F.Invocation.HandleArguments[0].Handle.Generation;
 F.Invocation.PrimitiveId = TEXT("animation.montage.notify_states.replace.v1");
 TestTrue(TEXT("wrong notify family is rejected"), HasCode(F.Configure(*Notify.Get(), Properties()), TEXT("EditorNativeState.StagedNotifyInvalidTarget")));
 F.Invocation.PrimitiveId = TEXT("animation.montage.notifies.replace.v1");
 FAnimNotifyEvent Event; Event.Notify = Notify.Get(); F.Montage->Notifies.Add(Event);
 TestTrue(TEXT("already published object is rejected"), HasCode(F.Configure(*Notify.Get(), Properties()), TEXT("EditorNativeState.StagedNotifyInvalidTarget")));
 TestEqual(TEXT("all rejected requests preserve properties"), Notify->First, 0);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedNotifyPropertiesPartialFailure,
 "UEAssetDoc.Editor.StagedMontageNotify.PartialFailureAndLeaf",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStagedNotifyPropertiesPartialFailure::RunTest(const FString&)
{
 using namespace EditorStagedMontageNotifyPropertiesTestsLocal;
 FFixture F;
 if (!TestTrue(TEXT("verified target lease"), F.Lease.IsValid())) return false;
 TStrongObjectPtr<UAssetDocumentEditorStagedNotifyTest> Notify(NewObject<UAssetDocumentEditorStagedNotifyTest>(F.Montage.Get(), TEXT("AssetDocumentManaged_Notify_Test"), RF_Transactional));
 const auto Fields = GetAssetDocumentEditorAdmittedPropertyFields(*Notify->GetClass());
 const auto* Field = Fields.FindByPredicate([](const auto& Item) { return Item.Property->GetAuthoredName() == TEXT("First"); });
 if (!TestNotNull(TEXT("existing safe admission finds field"), Field)) return false;
 FAssetDocumentEditorPropertyTokenPayload Payload(*Field->DeclaringClass, *Field->Property, Field->CodecId, Field->bReadable, Field->bWritable, Field->TypeRevision);
 FAssetDocumentEditorPropertyTokenView Token;
 if (!TestTrue(TEXT("current token"), Payload.TryGetView(Token))) return false;
 FAssetDocumentEditorPreparedPropertyWrite Prepared;
 FAssetDocumentEditorPropertyEditResult Failure;
 TestTrue(TEXT("value-only leaf accepts staged value"), TryPrepareAssetDocumentEditorAdmittedPropertyValue(Token, Integer(17), Prepared, Failure));
 TestTrue(TEXT("value-only leaf creates no footprint"), Prepared.GetFootprint().IsEmpty());
 TestFalse(TEXT("old top-level path still rejects subobject identity"), TryPrepareAssetDocumentEditorAdmittedPropertyWrite(*Notify.Get(), Token, Integer(17), Prepared, Failure));
 TestTrue(TEXT("old target diagnostic preserved"), Failure.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("PropertyWrite.InvalidTarget"); }));
 Notify->bInvalidateAfterFirstWrite = true;
 auto Out = F.Configure(*Notify.Get(), Properties());
 TestTrue(TEXT("post-edit identity change fails configuration"), HasCode(Out, TEXT("EditorNativeState.StagedNotifyIdentityDrift")));
 TestEqual(TEXT("first setter ran"), Notify->First, 17);
 TestEqual(TEXT("next setter did not run after identity failure"), Notify->Second, 0);
 TestEqual(TEXT("partial failure leaves parent footprint owner unchanged"), F.Transaction->GetEnlistedFootprint().Num(), 2);
 return true;
}
#endif
