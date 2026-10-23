// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentAnimationMutationPreparation.h"
#include "AssetDocumentAnimationPrimitiveProvider.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonOperations.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/Skeleton.h"
#include "Misc/AutomationTest.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentAnimationSkeletonTests
{
constexpr const TCHAR* Primitive = TEXT("animation.asset.skeleton.set.v1");
TStrongObjectPtr<UAnimSequence> CreateSequenceFixture(FAutomationTestBase& Test, USkeleton& Skeleton)
{
 // Stock AnimSequenceFactory.cpp: skeleton precedes model initialization;
 // NotifyPopulated completes the public controller lifecycle before observation.
 TStrongObjectPtr<UAnimSequence> Sequence(NewObject<UAnimSequence>(GetTransientPackage()));
 Sequence->SetSkeleton(&Skeleton);
 Sequence->GetController().InitializeModel();
 Sequence->GetController().SetFrameRate(FFrameRate(30, 1), false);
 Sequence->GetController().SetNumberOfFrames(FFrameNumber(30), false);
 if (!Test.TestTrue(TEXT("fixture has a real reference root"), Skeleton.GetReferenceSkeleton().GetNum() > 0)) return {};
 if (!Test.TestTrue(TEXT("fixture adds root animation curve"), Sequence->GetController().AddBoneCurve(TEXT("root"), false))) return {};
 TArray<FVector3f> Positions, Scales; TArray<FQuat4f> Rotations;
 Positions.Init(FVector3f::ZeroVector, 31); Scales.Init(FVector3f::OneVector, 31); Rotations.Init(FQuat4f::Identity, 31);
 if (!Test.TestTrue(TEXT("fixture root keys cover every sample"), Sequence->GetController().SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false))) return {};
 Sequence->GetController().NotifyPopulated();
 Sequence->WaitOnExistingCompression();
 return Sequence;
}

struct FFixture
{
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UAnimMontage> Target;
 TStrongObjectPtr<USkeleton> Old;
 TStrongObjectPtr<USkeleton> Desired;
 FFixture()
  : Package(CreatePackage(*FString::Printf(TEXT("/Game/AssetDocumentTests/Skeleton_%s/Target"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))),
    Target(NewObject<UAnimMontage>(Package.Get(), TEXT("Target"), RF_Public | RF_Transactional)),
    Old(NewObject<USkeleton>(CreatePackage(*(Package->GetName() + TEXT("_Old"))), *FString(TEXT("Target_Old")), RF_Public)),
    Desired(NewObject<USkeleton>(CreatePackage(*(Package->GetName() + TEXT("_Desired"))), *FString(TEXT("Target_Desired")), RF_Public))
 {
  for (USkeleton* Skeleton : {Old.Get(), Desired.Get()})
  {
   FReferenceSkeletonModifier Modifier(Skeleton);
   Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
  }
  for (const auto& Slot : Target->SlotAnimTracks)
  {
   Old->RegisterSlotNode(Slot.SlotName); Desired->RegisterSlotNode(Slot.SlotName);
  }
  Target->SetSkeleton(Old.Get());
  Package->SetDirtyFlag(false);
  FAssetRegistryModule::AssetCreated(Target.Get());
  FAssetRegistryModule::AssetCreated(Old.Get());
  FAssetRegistryModule::AssetCreated(Desired.Get());
 }
 ~FFixture()
 {
  if (Target.IsValid()) FAssetRegistryModule::AssetDeleted(Target.Get());
  if (Old.IsValid()) FAssetRegistryModule::AssetDeleted(Old.Get());
  if (Desired.IsValid()) FAssetRegistryModule::AssetDeleted(Desired.Get());
  if (Package.IsValid()) Package->SetDirtyFlag(false);
  if (Old.IsValid()) Old->GetOutermost()->SetDirtyFlag(false);
  if (Desired.IsValid()) Desired->GetOutermost()->SetDirtyFlag(false);
 }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  FAssetDocumentRuntimeRequestDto Result;
  Result.RequestId = FGuid::NewGuid(); Result.ExpectedRuntimeRevision = 1;
  Result.Operation = EAssetDocumentRuntimeOperation::Apply;
  Result.TargetAssetPath = Target->GetPathName();
  Result.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
  return Result;
 }
 FAssetDocumentMutationIntent Intent(bool bClear = false) const
 {
  FAssetDocumentMutationIntent Result;
  Result.StepIndex = 0;
  Result.Requirement = {Primitive, 1, TEXT("animation_profile")};
  Result.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  Result.InputSchemaId = FString(Primitive) + TEXT(".input.v1");
  Result.Intent.Kind = EAssetDocumentRuntimeValueKind::Object;
  Result.Intent.ObjectKeys = {TEXT("operation"), TEXT("skeleton_path")};
  Result.Intent.Elements.SetNum(2);
  Result.Intent.Elements[0].Kind = EAssetDocumentRuntimeValueKind::String;
  Result.Intent.Elements[0].StringValue = bClear ? TEXT("clear") : TEXT("set");
  if (!bClear)
  {
   Result.Intent.Elements[1].Kind = EAssetDocumentRuntimeValueKind::String;
   Result.Intent.Elements[1].StringValue = Desired->GetPathName();
  }
  return Result;
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSkeletonDescriptorTest,
 "UEAssetDoc.AnimationProvider.Skeleton.Descriptor",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSkeletonDescriptorTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationSkeletonTests;
 const auto Provider = CreateAssetDocumentAnimationPrimitiveProvider();
 const FAssetDocumentPrimitiveDescriptor* Found = nullptr;
 for (const auto& Row : Provider->DescribePrimitives()) if (Row.PrimitiveId == Primitive) Found = &Row;
 if (!TestNotNull(TEXT("Skeleton is callable"), Found)) return false;
 const auto& D = *Found;
 TestTrue(TEXT("descriptor validates"), ValidateAssetDocumentPrimitiveDescriptor(D).bValid);
 TestTrue(TEXT("exact Animation admission validates"), ValidateAnimationAdmissionEvidence(D).bValid);
 TestEqual(TEXT("mutating access"), D.Access, EAssetDocumentPrimitiveAccess::Mutating);
 TestEqual(TEXT("only Apply and existing Repair"), D.AllowedPhases,
  TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair});
 TestTrue(TEXT("transaction required"), D.bRequiresActiveTransaction);
 TestEqual(TEXT("only Asset handle family"), D.AllowedHandleKinds, TArray<EAssetDocumentHandleKind>{EAssetDocumentHandleKind::Asset});
 TestEqual(TEXT("input schema"), D.InputSchemaId, FString(Primitive) + TEXT(".input.v1"));
 TestEqual(TEXT("output schema"), D.OutputSchemaId, FString(Primitive) + TEXT(".output.v1"));
 TestEqual(TEXT("footprint schema"), D.MutationFootprintSchemaId, FString(Primitive) + TEXT(".footprint.v1"));
 TestTrue(TEXT("no handle results"), D.HandleResultSchemas.IsEmpty());
 if (TestEqual(TEXT("two handle schemas"), D.HandleArgumentSchemas.Num(), 2))
 {
  TestEqual(TEXT("target name"), D.HandleArgumentSchemas[0].Name, FString(TEXT("asset")));
  TestTrue(TEXT("target required"), D.HandleArgumentSchemas[0].bRequired);
  TestEqual(TEXT("target mutable"), D.HandleArgumentSchemas[0].RequiredAccess, EAssetDocumentHandleAccess::Mutable);
  TestEqual(TEXT("reference name"), D.HandleArgumentSchemas[1].Name, FString(TEXT("skeleton")));
  TestFalse(TEXT("reference optional for clear"), D.HandleArgumentSchemas[1].bRequired);
  TestEqual(TEXT("reference read-only"), D.HandleArgumentSchemas[1].RequiredAccess, EAssetDocumentHandleAccess::ReadOnly);
 }
 TestEqual(TEXT("stock authority"), D.Admission.ExportedAuthoritativeApi, FString(TEXT("UAnimationAsset::SetSkeleton;GetSkeleton;GetSkeletonGuid")));
 TestEqual(TEXT("inverse contract"), D.Admission.RollbackContractId, FString(Primitive) + TEXT(".rollback-subaction.v1"));
 for (int32 Case = 0; Case < 10; ++Case)
 {
  auto Drift = D;
  if (Case == 0) Drift.Admission.ExportedAuthoritativeApi = TEXT("UAnimationAsset::ReplaceSkeleton");
  if (Case == 1) Drift.InputSchemaId += TEXT(".drift");
  if (Case == 2) Drift.Admission.RollbackContractId += TEXT(".drift");
  if (Case == 3) Drift.Admission.ExactEngineVersion = TEXT("5.8");
  if (Case == 4) Drift.RequiredConsumerIds.Add(TEXT("unapproved_consumer"));
  if (Case == 5) Drift.Admission.FailureInjectionTestIds.Reset();
  if (Case == 6) Drift.OutputSchemaId += TEXT(".drift");
  if (Case == 7) Drift.MutationFootprintSchemaId += TEXT(".drift");
  if (Case == 8) Drift.HandleArgumentSchemas[1].RequiredAccess = EAssetDocumentHandleAccess::Mutable;
  if (Case == 9) Drift.AllowedPhases.Add(EAssetDocumentRuntimeRequestPhase::Verify);
  TestFalse(FString::Printf(TEXT("exact admission rejects drift %d"), Case), ValidateAnimationAdmissionEvidence(Drift).bValid);
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSkeletonPreparationTest,
 "UEAssetDoc.AnimationProvider.Skeleton.Preparation",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSkeletonPreparationTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationSkeletonTests;
 FFixture F;
 const auto Request = F.Request();
 const TArray<FAssetDocumentMutationIntent> Intents = {F.Intent()};
 FAssetDocumentPreparedMutationBatch Batch;
 if (!TestTrue(TEXT("prepare exact set"), PrepareAssetDocumentAnimationMutationBatch(Request, Intents, Batch).bValid)) return false;
 if (!TestEqual(TEXT("one contribution"), Batch.Contributions.Num(), 1)) return false;
 TestFalse(TEXT("Animation does not own dirty finalizer"), Batch.PackageFinalizer.IsValid());
 const auto& C = Batch.Contributions[0];
 TestEqual(TEXT("one physical Skeleton surface"), C.OwnedSurfaceIds, TArray<FString>{Request.TargetAssetPath + TEXT("::Skeleton")});
 if (TestEqual(TEXT("object/package footprint only"), C.RawFootprint.Num(), 2))
 {
  TestEqual(TEXT("object identity"), C.RawFootprint[0].StableId, FString(TEXT("Object:")) + Request.TargetAssetPath);
  TestEqual(TEXT("object component"), C.RawFootprint[0].RollbackComponentId, FString(TEXT("rollback.uobject")));
  TestEqual(TEXT("package component"), C.RawFootprint[1].RollbackComponentId, FString(TEXT("rollback.package")));
 }
 TestTrue(TEXT("existing target inverse captured"), C.InitialAction.IsValid());
 TestEqual(TEXT("prepare leaves old pointer"), F.Target->GetSkeleton(), F.Old.Get());
 TestFalse(TEXT("prepare leaves package clean"), F.Package->IsDirty());
 TWeakObjectPtr<UAnimMontage> Target(F.Target.Get());
 TWeakObjectPtr<USkeleton> Old(F.Old.Get()), Desired(F.Desired.Get());
 TWeakObjectPtr<UPackage> Package(F.Package.Get());
 F.Target.Reset(); F.Old.Reset(); F.Desired.Reset(); F.Package.Reset();
 CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
 TestTrue(TEXT("prepared state owns target/package/old/desired across GC"), Target.IsValid() && Package.IsValid() && Old.IsValid() && Desired.IsValid());
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 auto Capture = Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
 TestTrue(TEXT("capture opens after strong retention"), Capture.IsValid());
 if (Capture.IsValid()) Capture->Close();
 TestTrue(TEXT("unexecuted inverse is harmless"), C.InitialAction->Rollback(TEXT("test-before-forward")).bValid);
 if (Target.IsValid()) TestEqual(TEXT("unexecuted inverse leaves baseline"), Target->GetSkeleton(), Old.Get());
 F.Target.Reset(Target.Get()); F.Old.Reset(Old.Get()); F.Desired.Reset(Desired.Get()); F.Package.Reset(Package.Get());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSkeletonPreparationRejectionTest,
 "UEAssetDoc.AnimationProvider.Skeleton.PreparationRejections",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSkeletonPreparationRejectionTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationSkeletonTests;
 FFixture F;
 for (int32 Case = 0; Case < 11; ++Case)
 {
  auto Request = F.Request(); auto Intent = F.Intent();
  if (Case == 0) Intent.Intent.ObjectKeys.Add(TEXT("extra")), Intent.Intent.Elements.AddDefaulted();
  if (Case == 1) Intent.Intent.Elements[0].StringValue = TEXT("replace");
  if (Case == 2) Intent.Intent.Elements[1].StringValue = TEXT("/Game/Missing.Missing");
  if (Case == 3) Intent.Intent.Elements[1].StringValue = F.Target->GetPathName();
  if (Case == 4) Intent.Intent.Elements[0].StringValue = TEXT("clear");
  if (Case == 5) Intent.InputSchemaId += TEXT(".wrong");
  if (Case == 6) Intent.Requirement.PrimitiveId = TEXT("animation.asset.preview_mesh.set.v1");
  if (Case == 7) Intent.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == 8) Intent.Intent.ObjectKeys[1] = TEXT("operation");
  if (Case == 9) Intent.Intent.Elements[0].Kind = EAssetDocumentRuntimeValueKind::Boolean;
  if (Case == 10) Intent.Requirement.RequiredByRouteId = TEXT("unapproved_consumer");
  FAssetDocumentPreparedMutationBatch Batch;
  const TArray<FAssetDocumentMutationIntent> Intents = {Intent};
  TestFalse(FString::Printf(TEXT("malformed preparation %d rejected"), Case), PrepareAssetDocumentAnimationMutationBatch(Request, Intents, Batch).bValid);
  TestTrue(TEXT("failed prepare clears contributions"), Batch.Contributions.IsEmpty());
  TestEqual(TEXT("reject leaves pointer"), F.Target->GetSkeleton(), F.Old.Get());
  TestEqual(TEXT("reject leaves GUID"), F.Target->GetSkeletonGuid(), F.Old->GetGuid());
  TestFalse(TEXT("reject leaves dirty unchanged"), F.Package->IsDirty());
 }
 return true;
}
// This resolver owns real Editor-issued payloads. It supplies Animation metadata
// solely for direct-provider tests; Integration tests own actual handle evidence.
namespace AssetDocumentAnimationSkeletonTests
{
class FResolver final : public IAssetDocumentHandleResolver
{
 struct FEntry { FAssetDocumentOpaqueHandle Handle; TUniquePtr<IAssetDocumentNativeHandlePayload> Payload; };
 class FLease final : public IAssetDocumentVerifiedHandleLease
 {
 public:
  FLease(const FAssetDocumentOpaqueHandle& H, IAssetDocumentNativeHandlePayload& P) : Handle(H), Payload(P) {}
  const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
  IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return Payload; }
 private:
  FAssetDocumentOpaqueHandle Handle; IAssetDocumentNativeHandlePayload& Payload;
 };
 TArray<FEntry> Entries;
public:
 FAssetDocumentMutationToken Token;
 FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access,
  TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
 {
  auto& Entry = Entries.AddDefaulted_GetRef();
  Entry.Handle.RequestId = Token.RequestId; Entry.Handle.TransactionId = Token.TransactionId;
  Entry.Handle.RuntimeRevision = Token.RuntimeRevision; Entry.Handle.ProviderId = TEXT("editor");
  Entry.Handle.Slot = Entries.Num(); Entry.Handle.Generation = 1; Entry.Handle.Kind = Kind; Entry.Handle.Access = Access;
  Entry.Payload = MoveTemp(Payload);
  FAssetDocumentIssuedHandleResult Result; Result.Handle = Entry.Handle; return Result;
 }
 FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& Handle,
  const FAssetDocumentHandleValidationContext& Context) override
 {
  FAssetDocumentVerifiedLeaseResult Result;
  auto Validation = ValidateAssetDocumentHandleMetadata(Handle, Context);
  if (!Validation.bValid) { Result.Diagnostics = MoveTemp(Validation.Diagnostics); return Result; }
  const int32 Index = int32(Handle.Slot) - 1;
  if (!Entries.IsValidIndex(Index) || Entries[Index].Handle.Generation != Handle.Generation
   || Entries[Index].Handle.Kind != Handle.Kind || Entries[Index].Handle.Access != Handle.Access
   || !Entries[Index].Payload || !Entries[Index].Payload->IsAlive()) return Result;
  Result.Lease = MakeUnique<FLease>(Handle, *Entries[Index].Payload); return Result;
 }
 FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override { return {}; }
};

class FSkeletonTestTransaction final : public IAssetDocumentAtomicTransaction
{
public:
 FAssetDocumentMutationToken Token;
 int32 FootprintCalls = 0;
 FGuid GetTransactionId() const override { return Token.TransactionId; }
 EAssetDocumentAtomicState GetState() const override { return EAssetDocumentAtomicState::Mutating; }
 const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
 FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint>) override
 { ++FootprintCalls; return Valid(); }
 FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Valid(); }
 FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return Valid(); }
 FAssetDocumentContractValidationResult Rollback(FStringView) override { return Valid(); }
 FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction>) override { return Valid(); }
 FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override { return Valid(); }
 static FAssetDocumentContractValidationResult Valid() { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
};

TOptional<FAssetDocumentOpaqueHandle> Resolve(FAutomationTestBase& Test, UObject& Object,
 EAssetDocumentHandleAccess Access, FResolver& Resolver, FSkeletonTestTransaction& Transaction)
{
 FAssetDocumentPrimitiveInvocation I;
 I.PrimitiveId = TEXT("editor.asset.resolve_existing.v1"); I.Version = 1;
 I.RequestId = Resolver.Token.RequestId; I.TransactionId = Resolver.Token.TransactionId;
 I.RuntimeRevision = Resolver.Token.RuntimeRevision; I.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 I.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
 I.ValueArguments.ObjectKeys = {TEXT("path"), TEXT("expected_class")};
 I.ValueArguments.Elements.SetNum(2);
 I.ValueArguments.Elements[0].Kind = I.ValueArguments.Elements[1].Kind = EAssetDocumentRuntimeValueKind::String;
 I.ValueArguments.Elements[0].StringValue = Object.GetPathName();
 I.ValueArguments.Elements[1].StringValue = Object.GetClass()->GetPathName();
 const auto Outcome = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(I, Resolver, &Transaction);
 if (!Test.TestEqual(TEXT("real Editor resolves fixture"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return {};
 const FString Name = Access == EAssetDocumentHandleAccess::Mutable ? TEXT("asset_mutable") : TEXT("asset_read");
 for (const auto& Row : Outcome.HandleResults) if (Row.Name == Name && Row.Handles.Num() == 1) { auto Handle = Row.Handles[0]; Handle.ProviderId = TEXT("animation"); return Handle; }
 Test.AddError(TEXT("real Editor resolve omitted expected Asset handle")); return {};
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSkeletonInvocationRejectionTest,
 "UEAssetDoc.AnimationProvider.Skeleton.InvocationRejections",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSkeletonInvocationRejectionTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationSkeletonTests;
 for (int32 Case = 0; Case < 21; ++Case)
 {
  FFixture F; const auto Request = F.Request();
  FResolver Resolver; Resolver.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision};
  FSkeletonTestTransaction Transaction; Transaction.Token = Resolver.Token;
  const auto Target = Resolve(*this, *F.Target, EAssetDocumentHandleAccess::Mutable, Resolver, Transaction);
  const auto Reference = Resolve(*this, *F.Desired, EAssetDocumentHandleAccess::ReadOnly, Resolver, Transaction);
  if (!Target.IsSet() || !Reference.IsSet()) return false;
  FAssetDocumentPreparedMutationBatch Batch;
  const TArray<FAssetDocumentMutationIntent> Intents = {F.Intent()};
  if (!TestTrue(TEXT("prepare direct provider rejection fixture"), PrepareAssetDocumentAnimationMutationBatch(Request, Intents, Batch).bValid)) return false;
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  auto Capture = Batch.OpenMutationCapture(Transaction.Token.TransactionId, Diagnostics);
  if (!TestTrue(TEXT("open fixture capture"), Capture.IsValid())) return false;
  FAssetDocumentPrimitiveInvocation I;
  I.PrimitiveId = Primitive; I.Version = 1; I.RequestId = Request.RequestId;
  I.TransactionId = Transaction.Token.TransactionId; I.RuntimeRevision = Request.ExpectedRuntimeRevision;
  I.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  I.HandleArguments = {{TEXT("asset"), Target.GetValue()}, {TEXT("skeleton"), Reference.GetValue()}};
  I.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
  I.ValueArguments.ObjectKeys = {TEXT("operation")}; I.ValueArguments.Elements.SetNum(1);
  I.ValueArguments.Elements[0].Kind = EAssetDocumentRuntimeValueKind::String; I.ValueArguments.Elements[0].StringValue = TEXT("set");
  if (!TestTrue(TEXT("real payloads satisfy unmodified prepared validator"), Batch.Contributions[0].ValidateInvocation(I, Resolver).bValid)) return false;
  if (Case == 0) I.HandleArguments.RemoveAt(1);
  if (Case == 1) { const auto Duplicate = I.HandleArguments[0]; I.HandleArguments.Add(Duplicate); }
  if (Case == 2) I.HandleArguments[1].Name = TEXT("extra");
  if (Case == 3) I.HandleArguments[0].Handle.Kind = EAssetDocumentHandleKind::Object;
  if (Case == 4) I.HandleArguments[0].Handle.Access = EAssetDocumentHandleAccess::ReadOnly;
  if (Case == 5) I.HandleArguments[0].Handle.ProviderId = TEXT("editor");
  if (Case == 6) ++I.HandleArguments[0].Handle.Generation;
  if (Case == 7) I.RequestId = FGuid::NewGuid();
  if (Case == 8) I.TransactionId = FGuid::NewGuid();
  if (Case == 9) ++I.RuntimeRevision;
  if (Case == 10) I.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == 11) I.ValueArguments.ObjectKeys.Add(TEXT("extra")), I.ValueArguments.Elements.AddDefaulted();
  if (Case == 12) I.ValueArguments.Elements[0].StringValue = TEXT("clear");
  if (Case == 13) I.ValueArguments.Elements[0].StringValue = TEXT("replace");
  if (Case == 14) I.HandleArguments[0].Handle.Slot += 100;
  if (Case == 16) I.HandleArguments[1].Handle.Kind = EAssetDocumentHandleKind::Object;
  if (Case == 17) I.HandleArguments[1].Handle.ProviderId = TEXT("editor");
  if (Case == 18) ++I.HandleArguments[1].Handle.Generation;
  if (Case == 19) I.HandleArguments[1].Handle.Access = EAssetDocumentHandleAccess::Mutable;
  if (Case == 20) I.HandleArguments[1].Handle.RequestId = FGuid::NewGuid();
  AssetDocumentAnimationSkeletonTesting::FHooks Hooks;
  AssetDocumentAnimationSkeletonTesting::SetHooks(&Hooks);
  const auto Outcome = CreateAssetDocumentAnimationPrimitiveProvider()->Invoke(I, Resolver, Case == 15 ? nullptr : &Transaction);
  AssetDocumentAnimationSkeletonTesting::SetHooks(nullptr);
  TestEqual(FString::Printf(TEXT("direct malformed invocation %d fails"), Case), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  TestEqual(TEXT("zero native setters on rejection"), Hooks.ForwardSetterCalls, 0);
  TestTrue(TEXT("rejection emits no changed set"), Outcome.ChangedSet.IsEmpty());
  TestEqual(TEXT("rejection retains target"), F.Target->GetSkeleton(), F.Old.Get());
  TestFalse(TEXT("rejection retains clean package"), F.Package->IsDirty());
  Capture->Close();
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSkeletonCompatibilityTest,
 "UEAssetDoc.AnimationProvider.Skeleton.RetainedCompatibilityAndDrift",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSkeletonCompatibilityTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationSkeletonTests;
 FFixture F;
 const FName SlotName(TEXT("S1RetainedSlot"));
 FSlotAnimationTrack& Slot = F.Target->SlotAnimTracks.AddDefaulted_GetRef();
 Slot.SlotName = SlotName;
 const auto Request = F.Request();
 const TArray<FAssetDocumentMutationIntent> Set = {F.Intent()};
 FAssetDocumentPreparedMutationBatch Batch;
 TestFalse(TEXT("missing desired slot rejected without registering it"), PrepareAssetDocumentAnimationMutationBatch(Request, Set, Batch).bValid);
 TestFalse(TEXT("provider did not register missing slot"), F.Desired->ContainsSlotName(SlotName));
 F.Desired->RegisterSlotNode(SlotName); // Test-only authored compatibility fixture.
 TestTrue(TEXT("existing compatible slot accepted"), PrepareAssetDocumentAnimationMutationBatch(Request, Set, Batch).bValid);
 Slot.AnimTrack.AnimSegments.AddDefaulted();
 TestFalse(TEXT("invalid retained segment rejected"), PrepareAssetDocumentAnimationMutationBatch(Request, Set, Batch).bValid);
 TStrongObjectPtr<UAnimSequence> Sequence = CreateSequenceFixture(*this, *F.Desired);
 if (!TestTrue(TEXT("public sequence lifecycle creates compatible segment fixture"), Sequence.IsValid())) return false;
 Slot.AnimTrack.AnimSegments[0].SetAnimReference(Sequence.Get(), true);
 TestTrue(TEXT("fixture segment is valid"), Slot.AnimTrack.AnimSegments[0].IsValid());
 TestTrue(TEXT("segment playable on desired Skeleton accepted"), PrepareAssetDocumentAnimationMutationBatch(Request, Set, Batch).bValid);
 Sequence->SetSkeleton(F.Old.Get());
 Sequence->WaitOnExistingCompression();
 TestFalse(TEXT("incompatible retained segment rejected"), PrepareAssetDocumentAnimationMutationBatch(Request, Set, Batch).bValid);
 const TArray<FAssetDocumentMutationIntent> Clear = {F.Intent(true)};
 TestTrue(TEXT("clear does not demand desired compatibility"), PrepareAssetDocumentAnimationMutationBatch(Request, Clear, Batch).bValid);
 Slot.AnimTrack.AnimSegments.Reset();
 TestTrue(TEXT("prepare stable baseline for retained drift"), PrepareAssetDocumentAnimationMutationBatch(Request, Set, Batch).bValid);
 Slot.SlotName = TEXT("ChangedAfterPrepare");
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 auto Capture = Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics);
 TestFalse(TEXT("capture rejects retained drift before writes"), Capture.IsValid());
 TestEqual(TEXT("drift rejection leaves original reference"), F.Target->GetSkeleton(), F.Old.Get());
 TestFalse(TEXT("compatibility checks leave target clean"), F.Package->IsDirty());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSkeletonNoncanonicalBaselineTest,
 "UEAssetDoc.AnimationProvider.Skeleton.NoncanonicalBaseline",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSkeletonNoncanonicalBaselineTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationSkeletonTests;
 FFixture F;
 // Test-only corruption of the reflected pointer keeps the private GUID intact.
 // Production must reject this state and must never repair the private GUID.
 auto* PointerProperty = FindFProperty<FObjectPropertyBase>(UAnimationAsset::StaticClass(), TEXT("Skeleton"));
 if (!TestNotNull(TEXT("fixture can represent persisted pointer/GUID inconsistency"), PointerProperty)) return false;
 const FGuid Guid = F.Target->GetSkeletonGuid();
 if (!TestTrue(TEXT("fixture old Skeleton GUID valid"), Guid.IsValid())) return false;
 for (int32 Case = 0; Case < 2; ++Case)
 {
  PointerProperty->SetObjectPropertyValue_InContainer(F.Target.Get(), Case == 0 ? nullptr : F.Desired.Get());
  FAssetDocumentPreparedMutationBatch Batch;
  const TArray<FAssetDocumentMutationIntent> Intents = {F.Intent(true)};
  TestFalse(TEXT("noncanonical pointer/GUID baseline rejected before BeginLive"), PrepareAssetDocumentAnimationMutationBatch(F.Request(), Intents, Batch).bValid);
  TestEqual(TEXT("no implicit GUID repair"), F.Target->GetSkeletonGuid(), Guid);
  TestFalse(TEXT("rejection does not dirty package"), F.Package->IsDirty());
  PointerProperty->SetObjectPropertyValue_InContainer(F.Target.Get(), F.Old.Get());
 }
 return true;
}

namespace AssetDocumentAnimationSkeletonTests
{
// Direct provider harness uses real Editor payloads and real Animation capture;
// its transaction is a focused test double. Integration owns A0/Atomic evidence.
struct FLiveFixture
{
 FFixture Objects;
 FAssetDocumentRuntimeRequestDto Request = Objects.Request();
 FResolver Resolver;
 FSkeletonTestTransaction Transaction;
 FAssetDocumentPreparedMutationBatch Batch;
 TUniquePtr<FAssetDocumentCoreMutationCaptureLease> Capture;
 FAssetDocumentPrimitiveInvocation Invocation;
 TStrongObjectPtr<UAnimSequence> Segment;
 bool Initialize(FAutomationTestBase& Test, bool bClear = false, bool bNoOp = false, bool bRetained = false)
 {
  if (bClear && bNoOp) Objects.Target->SetSkeleton(nullptr);
  if (bRetained)
  {
   const FName SlotName(TEXT("RetainedForSetClearInverse"));
   Objects.Old->RegisterSlotNode(SlotName); Objects.Desired->RegisterSlotNode(SlotName);
   auto& Slot = Objects.Target->SlotAnimTracks.AddDefaulted_GetRef(); Slot.SlotName = SlotName;
   Segment = CreateSequenceFixture(Test, *Objects.Desired);
   if (!Test.TestTrue(TEXT("public sequence lifecycle creates retained segment"), Segment.IsValid())) return false;
   Slot.AnimTrack.AnimSegments.AddDefaulted_GetRef().SetAnimReference(Segment.Get(), true);
  }
  Resolver.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision}; Transaction.Token = Resolver.Token;
  const auto Target = Resolve(Test, *Objects.Target, EAssetDocumentHandleAccess::Mutable, Resolver, Transaction);
  if (!Target.IsSet()) return false;
  auto Intent = Objects.Intent(bClear);
  USkeleton* ReferenceObject = bNoOp ? Objects.Old.Get() : Objects.Desired.Get();
  if (!bClear) Intent.Intent.Elements[1].StringValue = ReferenceObject->GetPathName();
  const TArray<FAssetDocumentMutationIntent> Intents = {Intent};
  const auto Prepared = PrepareAssetDocumentAnimationMutationBatch(Request, Intents, Batch);
  if (!Test.TestTrue(TEXT("prepare live focused fixture"), Prepared.bValid))
  {
   for (const auto& Diagnostic : Prepared.Diagnostics) Test.AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
   return false;
  }
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  Capture = Batch.OpenMutationCapture(Transaction.Token.TransactionId, Diagnostics);
  if (!Test.TestTrue(TEXT("open live focused capture"), Capture.IsValid())) return false;
  Invocation.PrimitiveId = Primitive; Invocation.Version = 1; Invocation.RequestId = Request.RequestId;
  Invocation.TransactionId = Transaction.Token.TransactionId; Invocation.RuntimeRevision = Request.ExpectedRuntimeRevision;
  Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
  Invocation.HandleArguments.Add({TEXT("asset"), Target.GetValue()});
  if (!bClear)
  {
   const auto Reference = Resolve(Test, *ReferenceObject, EAssetDocumentHandleAccess::ReadOnly, Resolver, Transaction);
   if (!Reference.IsSet()) return false;
   Invocation.HandleArguments.Add({TEXT("skeleton"), Reference.GetValue()});
  }
  Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
  Invocation.ValueArguments.ObjectKeys = {TEXT("operation")}; Invocation.ValueArguments.Elements.SetNum(1);
  Invocation.ValueArguments.Elements[0].Kind = EAssetDocumentRuntimeValueKind::String;
  Invocation.ValueArguments.Elements[0].StringValue = bClear ? TEXT("clear") : TEXT("set");
  return Test.TestTrue(TEXT("real Editor payloads satisfy prepared validator"), Batch.Contributions[0].ValidateInvocation(Invocation, Resolver).bValid);
 }
 FAssetDocumentPrimitiveOutcome Invoke() { return CreateAssetDocumentAnimationPrimitiveProvider()->Invoke(Invocation, Resolver, &Transaction); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSkeletonForwardInverseTest,
 "UEAssetDoc.AnimationProvider.Skeleton.ForwardInverse",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSkeletonForwardInverseTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationSkeletonTests;
 for (int32 Case = 0; Case < 4; ++Case)
 {
  const bool bClear = (Case & 1) != 0, bNoOp = Case >= 2;
  FLiveFixture F;
  if (!F.Initialize(*this, bClear, bNoOp, !bNoOp)) return false;
  USkeleton* Baseline = F.Objects.Target->GetSkeleton();
  const FGuid BaselineGuid = F.Objects.Target->GetSkeletonGuid();
  const int32 TrackCount = F.Objects.Target->SlotAnimTracks.Num();
  const bool OldDirty = F.Objects.Old->GetOutermost()->IsDirty(), DesiredDirty = F.Objects.Desired->GetOutermost()->IsDirty();
  AssetDocumentAnimationSkeletonTesting::FHooks Hooks;
  AssetDocumentAnimationSkeletonTesting::SetHooks(&Hooks);
  const auto Outcome = F.Invoke();
  TestEqual(TEXT("set/clear/no-op succeeds"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
  TestEqual(TEXT("exact setter count"), Hooks.ForwardSetterCalls, bNoOp ? 0 : 1);
  TestEqual(TEXT("exact changed set size"), Outcome.ChangedSet.Num(), bNoOp ? 0 : 1);
  TestEqual(TEXT("exact output keys"), Outcome.Value.ObjectKeys, TArray<FString>{TEXT("skeleton_path"), TEXT("skeleton_guid"), TEXT("changed")});
  if (Outcome.Value.Elements.Num() == 3)
  {
   TestEqual(TEXT("output changed"), Outcome.Value.Elements[2].BooleanValue, !bNoOp);
   if (bClear)
   {
    TestEqual(TEXT("clear output path null"), Outcome.Value.Elements[0].Kind, EAssetDocumentRuntimeValueKind::Null);
    TestEqual(TEXT("clear output guid null"), Outcome.Value.Elements[1].Kind, EAssetDocumentRuntimeValueKind::Null);
   }
   else
   {
    USkeleton* Expected = bNoOp ? F.Objects.Old.Get() : F.Objects.Desired.Get();
    TestEqual(TEXT("output exact path"), Outcome.Value.Elements[0].StringValue, Expected->GetPathName());
    TestEqual(TEXT("output exact GUID format"), Outcome.Value.Elements[1].StringValue, Expected->GetGuid().ToString(EGuidFormats::DigitsWithHyphens));
   }
  }
  if (!Outcome.ChangedSet.IsEmpty())
  {
   TestEqual(TEXT("changed authority"), Outcome.ChangedSet[0].AuthorityId, FString(Primitive));
   TestEqual(TEXT("changed target"), Outcome.ChangedSet[0].StableObjectId, F.Request.TargetAssetPath);
   TestEqual(TEXT("changed path"), Outcome.ChangedSet[0].Path, FString(TEXT("/Skeleton")));
   TestEqual(TEXT("changed kind"), Outcome.ChangedSet[0].ChangeKind, FString(bClear ? TEXT("Clear") : TEXT("Set")));
  }
  TestEqual(TEXT("retained tracks remain"), F.Objects.Target->SlotAnimTracks.Num(), TrackCount);
  if (!bNoOp) TestEqual(TEXT("retained segment remains"), F.Objects.Target->SlotAnimTracks.Last().AnimTrack.AnimSegments[0].GetAnimReference().Get(), static_cast<UAnimSequenceBase*>(F.Segment.Get()));
  F.Capture->Close();
  TestTrue(TEXT("real prepared inverse succeeds"), F.Batch.Contributions[0].InitialAction->Rollback(TEXT("focused")).bValid);
  TestEqual(TEXT("inverse restores pointer"), F.Objects.Target->GetSkeleton(), Baseline);
  TestEqual(TEXT("inverse restores GUID"), F.Objects.Target->GetSkeletonGuid(), BaselineGuid);
  TestEqual(TEXT("inverse retains tracks"), F.Objects.Target->SlotAnimTracks.Num(), TrackCount);
  if (!bNoOp) TestEqual(TEXT("inverse retains segment"), F.Objects.Target->SlotAnimTracks.Last().AnimTrack.AnimSegments[0].GetAnimReference().Get(), static_cast<UAnimSequenceBase*>(F.Segment.Get()));
  TestEqual(TEXT("old Skeleton dirty unchanged"), F.Objects.Old->GetOutermost()->IsDirty(), OldDirty);
  TestEqual(TEXT("desired Skeleton dirty unchanged"), F.Objects.Desired->GetOutermost()->IsDirty(), DesiredDirty);
  AssetDocumentAnimationSkeletonTesting::SetHooks(nullptr);
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSkeletonFailureInverseTest,
 "UEAssetDoc.AnimationProvider.Skeleton.FailureInverse",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSkeletonFailureInverseTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationSkeletonTests;
 for (int32 Case = 0; Case < 2; ++Case)
 {
  FLiveFixture F;
  if (!F.Initialize(*this)) return false;
  TWeakObjectPtr<USkeleton> Old(F.Objects.Old.Get()); const FGuid OldGuid = Old->GetGuid();
  F.Objects.Old.Reset(); // Only the real prepared strong preimage keeps old alive.
  AssetDocumentAnimationSkeletonTesting::FHooks Hooks;
  Hooks.bFailForwardReadback = true;
  Hooks.AfterSetter = [&]() { CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS); };
  AssetDocumentAnimationSkeletonTesting::SetHooks(&Hooks);
  const auto Outcome = F.Invoke();
  TestEqual(TEXT("after-set failure is reported"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  TestEqual(TEXT("forward setter did execute"), Hooks.ForwardSetterCalls, 1);
  TestTrue(TEXT("failure changed set is empty"), Outcome.ChangedSet.IsEmpty());
  TestTrue(TEXT("old preimage survived forced GC"), Old.IsValid());
  TestEqual(TEXT("partial write remains until inverse"), F.Objects.Target->GetSkeleton(), F.Objects.Desired.Get());
  F.Capture->Close(); Hooks.bFailRollbackReadback = Case == 1;
  const auto Inverse = F.Batch.Contributions[0].InitialAction->Rollback(TEXT("after-set-failure"));
  TestEqual(TEXT("inverse readback failure remains visible"), Inverse.bValid, Case == 0);
  TestEqual(TEXT("real inverse setter executed once"), Hooks.InverseSetterCalls, 1);
  TestEqual(TEXT("inverse restores strong old pointer"), F.Objects.Target->GetSkeleton(), Old.Get());
  TestEqual(TEXT("inverse restores old GUID"), F.Objects.Target->GetSkeletonGuid(), OldGuid);
  if (Case == 1) TestTrue(TEXT("domain rollback failure retained"), Inverse.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("AnimationProvider.Skeleton.RollbackFailed"); }));
  AssetDocumentAnimationSkeletonTesting::SetHooks(nullptr);
  F.Objects.Old.Reset(Old.Get());
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSkeletonNativeIdentityRejectionTest,
 "UEAssetDoc.AnimationProvider.Skeleton.NativeIdentityRejections",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentSkeletonNativeIdentityRejectionTest::RunTest(const FString&)
{
 using namespace AssetDocumentAnimationSkeletonTests;
 for (int32 Case = 0; Case < 7; ++Case)
 {
  FLiveFixture F;
  if (!F.Initialize(*this)) return false;
  const FString OldName = F.Objects.Old->GetName(), DesiredName = F.Objects.Desired->GetName();
  const FString TargetName = F.Objects.Target->GetName(), PackageName = F.Objects.Package->GetName();
  if (Case == 0) F.Objects.Old->Rename(TEXT("OldIdentityDrift"), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
  if (Case == 1) F.Objects.Desired->Rename(TEXT("DesiredIdentityDrift"), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
  if (Case == 2) F.Objects.Target->SetSkeleton(F.Objects.Desired.Get());
  if (Case == 3 || Case == 4)
  {
   USkeleton* Changed = Case == 3 ? F.Objects.Desired.Get() : F.Objects.Old.Get();
   const FGuid Previous = Changed->GetGuid();
   Changed->PostDuplicate(false); // Public lifecycle hook, test-owned GUID drift.
   TestTrue(TEXT("fixture lifecycle changes GUID"), Changed->GetGuid() != Previous);
  }
  if (Case == 5) F.Objects.Target->Rename(TEXT("TargetIdentityDrift"), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
  if (Case == 6) F.Objects.Package->Rename(*(PackageName + TEXT("_Drift")), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
  TestFalse(TEXT("native identity drift rejected by prepared validator"), F.Batch.Contributions[0].ValidateInvocation(F.Invocation, F.Resolver).bValid);
  F.Capture->Close();
  if (Case == 0) F.Objects.Old->Rename(*OldName, nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
  if (Case == 1) F.Objects.Desired->Rename(*DesiredName, nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
  if (Case == 2 || Case == 4) F.Objects.Target->SetSkeleton(F.Objects.Old.Get());
  if (Case == 5) F.Objects.Target->Rename(*TargetName, nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
  if (Case == 6) F.Objects.Package->Rename(*PackageName, nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
 }
 {
  FLiveFixture Live;
  if (!Live.Initialize(*this)) return false;
  const FString Name = Live.Objects.Target->GetName();
  FAssetRegistryModule::AssetDeleted(Live.Objects.Target.Get());
  Live.Objects.Target->Rename(TEXT("PreservedOriginal"), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
  TStrongObjectPtr<UAnimMontage> Replacement(NewObject<UAnimMontage>(Live.Objects.Package.Get(), *Name, RF_Public | RF_Transactional));
  Replacement->SetSkeleton(Live.Objects.Old.Get());
  FAssetRegistryModule::AssetCreated(Replacement.Get());
  const auto ReplacedHandle = Resolve(*this, *Replacement, EAssetDocumentHandleAccess::Mutable, Live.Resolver, Live.Transaction);
  if (ReplacedHandle.IsSet())
  {
   Live.Invocation.HandleArguments[0].Handle = ReplacedHandle.GetValue();
   TestTrue(TEXT("same canonical path resolves a distinct native type revision"), Replacement->GetUniqueID() != Live.Objects.Target->GetUniqueID());
   TestEqual(TEXT("replacement uses frozen target path"), Replacement->GetPathName(), Live.Request.TargetAssetPath);
   TestFalse(TEXT("prepared identity rejects same-path replacement"), Live.Batch.Contributions[0].ValidateInvocation(Live.Invocation, Live.Resolver).bValid);
  }
  Live.Capture->Close();
  FAssetRegistryModule::AssetDeleted(Replacement.Get());
  Replacement->Rename(TEXT("RetiredReplacement"), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
  Live.Objects.Target->Rename(*Name, nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
  FAssetRegistryModule::AssetCreated(Live.Objects.Target.Get());
 }
 FFixture F;
 TStrongObjectPtr<UAnimSequence> WrongTarget(NewObject<UAnimSequence>(F.Package.Get(), TEXT("WrongTarget")));
 TStrongObjectPtr<UAnimMontage> WrongReference(NewObject<UAnimMontage>(F.Package.Get(), TEXT("WrongReference")));
 FAssetDocumentPreparedMutationBatch Batch;
 auto Request = F.Request(); Request.TargetAssetPath = WrongTarget->GetPathName();
 TArray<FAssetDocumentMutationIntent> Intents = {F.Intent()};
 TestFalse(TEXT("non-Montage animation target rejected"), PrepareAssetDocumentAnimationMutationBatch(Request, Intents, Batch).bValid);
 Request = F.Request(); Intents[0].Intent.Elements[1].StringValue = WrongReference->GetPathName();
 TestFalse(TEXT("non-Skeleton reference rejected"), PrepareAssetDocumentAnimationMutationBatch(Request, Intents, Batch).bValid);
 return true;
}
#endif
