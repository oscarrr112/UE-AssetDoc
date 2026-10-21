// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentEditorNativeState.h"
#include "AssetDocumentEditorStagedMontageNotifyTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "Provider/ClassReference/AssetDocumentEditorAssetPayload.h"
#include "Animation/AnimMontage.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMontageNotifyPropertySnapshot,
 "UEAssetDoc.Editor.MontageNotifyPropertySnapshot",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMontageNotifyPropertySnapshot::RunTest(const FString&)
{
 FAssetDocumentEditorProviderTestContext Context; Context.ProviderId = TEXT("animation");
 FAssetDocumentEditorProviderTestHandleResolver Resolver(Context);
 TStrongObjectPtr<UPackage> Package(CreatePackage(*(TEXT("/Temp/NotifySnapshot_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
 TStrongObjectPtr<UAnimMontage> Target(NewObject<UAnimMontage>(Package.Get(), TEXT("Montage"), RF_Transactional));
 TStrongObjectPtr<UAssetDocumentEditorStagedNotifyTest> Notify(NewObject<UAssetDocumentEditorStagedNotifyTest>(Target.Get()));
 Notify->First = 73; Notify->Second = 91;
 const auto Issued = Resolver.Issue(EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, MakeAssetDocumentEditorAssetPayload(*Target));
 if (!TestTrue(TEXT("readonly asset issued"), Issued.Handle.IsSet())) return false;
 FAssetDocumentHandleValidationContext Validation;
 Validation.RequestId = Context.RequestId; Validation.TransactionId = Context.TransactionId; Validation.RuntimeRevision = Context.RuntimeRevision;
 Validation.ProviderId = Context.ProviderId; Validation.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
 Validation.RequiredKind = EAssetDocumentHandleKind::Asset; Validation.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
 auto Acquired = Resolver.AcquireVerifiedLease(*Issued.Handle, Validation);
 if (!TestTrue(TEXT("readonly verified lease acquired"), Acquired.Lease.IsValid())) return false;
 FAssetDocumentPrimitiveInvocation Invocation;
 Invocation.PrimitiveId = TEXT("animation.montage.profile.snapshot.v1"); Invocation.Version = 1;
 Invocation.RequestId = Context.RequestId; Invocation.TransactionId = Context.TransactionId; Invocation.RuntimeRevision = Context.RuntimeRevision;
 Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Verify; Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
 Invocation.HandleArguments.Add({TEXT("asset"), *Issued.Handle});
 auto Out = ReadAssetDocumentEditorMontageNotifyProperties(Invocation, *Acquired.Lease, *Notify);
 TestTrue(TEXT("unpublished object cannot be observed through live contract"), Out.Status == EAssetDocumentPrimitiveOutcomeStatus::Failed);
 Target->Notifies.AddDefaulted_GetRef().Notify = Notify.Get();
 Out = ReadAssetDocumentEditorMontageNotifyProperties(Invocation, *Acquired.Lease, *Notify);
 if (!TestTrue(TEXT("installed live Notify is readable without creation-prefix authority"), Out.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
 TestTrue(TEXT("observation produces no mutation record"), Out.ChangedSet.IsEmpty());
 const int32 First = Out.Value.ObjectKeys.IndexOfByKey(TEXT("First"));
 const int32 Second = Out.Value.ObjectKeys.IndexOfByKey(TEXT("Second"));
 if (TestTrue(TEXT("all writable/readable fields included"), Out.Value.Elements.IsValidIndex(First) && Out.Value.Elements.IsValidIndex(Second)))
 {
  TestEqual(TEXT("first comes from actual object"), Out.Value.Elements[First].IntegerValue, int64(73));
  TestEqual(TEXT("second comes from actual object"), Out.Value.Elements[Second].IntegerValue, int64(91));
 }
 Notify->First = 105;
 Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
 Out = ReadAssetDocumentEditorMontageNotifyProperties(Invocation, *Acquired.Lease, *Notify);
 if (TestTrue(TEXT("Preflight reads same real object"), Out.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
 {
  const int32 Changed = Out.Value.ObjectKeys.IndexOfByKey(TEXT("First"));
  if (TestTrue(TEXT("changed field present"), Out.Value.Elements.IsValidIndex(Changed)))
   TestEqual(TEXT("fresh observation does not echo earlier snapshot"), Out.Value.Elements[Changed].IntegerValue, int64(105));
 }
 Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 TestTrue(TEXT("Apply phase is not admitted for observation"),
  ReadAssetDocumentEditorMontageNotifyProperties(Invocation, *Acquired.Lease, *Notify).Status == EAssetDocumentPrimitiveOutcomeStatus::Failed);
 TestEqual(TEXT("reader leaves actual fields unchanged"), Notify->First, 105);
 return true;
}
#endif
