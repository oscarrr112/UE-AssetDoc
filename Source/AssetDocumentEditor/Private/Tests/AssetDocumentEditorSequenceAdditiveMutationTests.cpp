// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentEditorSequenceAdditiveMutation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "Provider/Sequence/AssetDocumentEditorSequenceAdditiveMutationTestHooks.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/Skeleton.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
namespace AssetDocumentEditorSequenceAdditiveTests
{
constexpr const TCHAR* Primitive = TEXT("animation.sequence.complete_edit.v2");
constexpr const TCHAR* Schema = TEXT("animation.sequence.complete_edit.input.v2");
FAssetDocumentRuntimeValueDto String(const TCHAR* V)
{ FAssetDocumentRuntimeValueDto R; R.Kind=EAssetDocumentRuntimeValueKind::String; R.StringValue=V; return R; }
FAssetDocumentRuntimeValueDto Object(TArray<FString> K, TArray<FAssetDocumentRuntimeValueDto> V)
{ FAssetDocumentRuntimeValueDto R; R.Kind=EAssetDocumentRuntimeValueKind::Object; R.ObjectKeys=MoveTemp(K); R.Elements=MoveTemp(V); return R; }
struct FFixture
{
 TStrongObjectPtr<USkeleton> Skeleton;
 TStrongObjectPtr<UAnimSequence> Sequence;
 FFixture()
 {
  const FString Root = TEXT("/Game/AssetDocumentSequenceAdditiveContract/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  { FReferenceSkeletonModifier Modifier(Skeleton.Get()); Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); }
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  Sequence->SetSkeleton(Skeleton.Get());
  auto& C = Sequence->GetController();
  C.OpenBracket(FText::FromString(TEXT("Sequence additive contract fixture")), false);
  C.InitializeModel(); C.SetFrameRate(FFrameRate(24, 1), false); C.SetNumberOfFrames(FFrameNumber(12), false);
  C.AddBoneCurve(TEXT("root"), false);
  TArray<FVector3f> P, S; TArray<FQuat4f> Q;
  P.Init(FVector3f::ZeroVector, 13); S.Init(FVector3f::OneVector, 13); Q.Init(FQuat4f::Identity, 13);
  C.SetBoneTrackKeys(TEXT("root"), P, Q, S, false); C.NotifyPopulated(); C.CloseBracket(false);
  Sequence->WaitOnExistingCompression();
  Sequence->GetOutermost()->SetDirtyFlag(false); Skeleton->GetOutermost()->SetDirtyFlag(false);
 }
 ~FFixture()
 {
  Sequence->WaitOnExistingCompression();
  Sequence->ClearFlags(RF_Public | RF_Standalone); Sequence->GetOutermost()->SetDirtyFlag(false);
  Skeleton->ClearFlags(RF_Public | RF_Standalone); Skeleton->GetOutermost()->SetDirtyFlag(false);
 }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 1;
  R.Operation = EAssetDocumentRuntimeOperation::Apply; R.TargetAssetPath = Sequence->GetPathName();
  R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence"); return R;
 }
 FAssetDocumentMutationIntent Intent(FAssetDocumentRuntimeValueDto Fields) const
 {
  FAssetDocumentMutationIntent I; I.StepIndex = 0; I.Requirement = {Primitive, 2, TEXT("animation_profile")};
  I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.InputSchemaId = Schema;
  I.Intent = Object({TEXT("completion_kind"), TEXT("additive")}, {String(TEXT("additive_root")), MoveTemp(Fields)}); return I;
 }
};

class FResolver final : public IAssetDocumentHandleResolver
{
 struct FEntry { FAssetDocumentOpaqueHandle Handle; TUniquePtr<IAssetDocumentNativeHandlePayload> Payload; };
 class FLease final : public IAssetDocumentVerifiedHandleLease
 {
  FAssetDocumentOpaqueHandle Handle; IAssetDocumentNativeHandlePayload& Payload;
 public:
  FLease(const FAssetDocumentOpaqueHandle& InHandle, IAssetDocumentNativeHandlePayload& InPayload) : Handle(InHandle), Payload(InPayload) {}
  const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
  IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return Payload; }
 };
 TArray<FEntry> Entries;
public:
 FAssetDocumentMutationToken Token;
 FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access, TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
 {
  auto& Entry = Entries.AddDefaulted_GetRef(); Entry.Handle.RequestId = Token.RequestId; Entry.Handle.TransactionId = Token.TransactionId;
  Entry.Handle.RuntimeRevision = Token.RuntimeRevision; Entry.Handle.ProviderId = TEXT("editor"); Entry.Handle.Slot = Entries.Num();
  Entry.Handle.Generation = 1; Entry.Handle.Kind = Kind; Entry.Handle.Access = Access; Entry.Payload = MoveTemp(Payload);
  FAssetDocumentIssuedHandleResult Result; Result.Handle = Entry.Handle; return Result;
 }
 FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& Handle, const FAssetDocumentHandleValidationContext& Context) override
 {
  FAssetDocumentVerifiedLeaseResult Result; auto Validation = ValidateAssetDocumentHandleMetadata(Handle, Context);
  if (!Validation.bValid) { Result.Diagnostics = MoveTemp(Validation.Diagnostics); return Result; }
  const int32 Index = int32(Handle.Slot) - 1;
  if (!Entries.IsValidIndex(Index) || Entries[Index].Handle.Generation != Handle.Generation || Entries[Index].Handle.Kind != Handle.Kind
   || Entries[Index].Handle.Access != Handle.Access || !Entries[Index].Payload || !Entries[Index].Payload->IsAlive()) return Result;
  Result.Lease = MakeUnique<FLease>(Handle, *Entries[Index].Payload); return Result;
 }
 FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override
 { FAssetDocumentHandleInvalidationResult Result; Result.InvalidatedHandleCount = Entries.Num(); Entries.Reset(); return Result; }
};
TOptional<FAssetDocumentOpaqueHandle> Resolve(UObject& Object, EAssetDocumentHandleAccess Access, FResolver& Resolver, IAssetDocumentAtomicTransaction& Transaction)
{
 FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = TEXT("editor.asset.resolve_existing.v1"); I.Version = 1;
 I.RequestId = Resolver.Token.RequestId; I.TransactionId = Resolver.Token.TransactionId; I.RuntimeRevision = Resolver.Token.RuntimeRevision; I.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 I.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object; I.ValueArguments.ObjectKeys = {TEXT("path"), TEXT("expected_class")};
 I.ValueArguments.Elements.SetNum(2); for (auto& Value : I.ValueArguments.Elements) Value.Kind = EAssetDocumentRuntimeValueKind::String;
 I.ValueArguments.Elements[0].StringValue = Object.GetPathName(); I.ValueArguments.Elements[1].StringValue = Object.GetClass()->GetPathName();
 const auto Outcome = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(I, Resolver, &Transaction);
 if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) return {};
 const FString Name = Access == EAssetDocumentHandleAccess::Mutable ? TEXT("asset_mutable") : TEXT("asset_read");
 for (const auto& Row : Outcome.HandleResults) if (Row.Name == Name && Row.Handles.Num() == 1) { return Row.Handles[0]; }
 return {};
}

class FAdditiveTransaction final : public IAssetDocumentAtomicTransaction
{
public:
 FAssetDocumentMutationToken Token; int32 Footprints=0; bool bArmedBeforeFirstWrite=false; AssetDocumentEditorSequenceAdditiveTesting::FHooks* ObservedHooks=nullptr;
 FGuid GetTransactionId() const override { return Token.TransactionId; }
 EAssetDocumentAtomicState GetState() const override { return EAssetDocumentAtomicState::Mutating; }
 const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
 FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Rows) override
 { ++Footprints; bArmedBeforeFirstWrite=Rows.Num()==2 && ObservedHooks && ObservedHooks->FieldWrites==0; return Valid(); }
 FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Valid(); }
 FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return Valid(); }
 FAssetDocumentContractValidationResult Rollback(FStringView) override { return Valid(); }
 FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction>) override { return Valid(); }
 FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override { return Valid(); }
 static FAssetDocumentContractValidationResult Valid() { FAssetDocumentContractValidationResult R; R.bValid=true; return R; }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEditorSequenceAdditiveAtomicTest,
 "UEAssetDoc.AnimationProvider.SequenceAdditive.EditorAtomic",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentEditorSequenceAdditiveAtomicTest::RunTest(const FString&)
{
 using namespace AssetDocumentEditorSequenceAdditiveTests;
 // Unit proof of the actual Editor group. Genuine Core ordering/dirty/save
 // evidence is in SequenceAdditiveCore; this transaction only records enlistment.
 for (int32 FailAfter=0; FailAfter<4; ++FailAfter)
 {
  FFixture F; auto Request=F.Request(); auto Intent=F.Intent(Object({TEXT("AdditiveAnimType"),TEXT("RefPoseType")},
   {String(TEXT("AAT_LocalSpaceBase")),String(TEXT("ABPT_LocalAnimFrame"))}));
  TSharedPtr<IAssetDocumentEditorSequenceAdditiveMutation> Group;
  if (!TestTrue(TEXT("real owned typed buffers prepare"),PrepareAssetDocumentEditorSequenceAdditiveMutation(Request,Intent,Group).bValid)) return false;
  FAdditiveTransaction Tx; Tx.Token.RequestId=Request.RequestId; Tx.Token.RuntimeRevision=Request.ExpectedRuntimeRevision; Tx.Token.TransactionId=FGuid::NewGuid();
  FResolver Handles; Handles.Token=Tx.Token;
  const auto Handle=Resolve(*F.Sequence,EAssetDocumentHandleAccess::Mutable,Handles,Tx); if (!TestTrue(TEXT("real Editor-issued mutable lease"),Handle.IsSet())) return false;
  TestTrue(TEXT("open exact native capture"),Group->OpenCapture(Tx.Token.TransactionId).bValid);
  FAssetDocumentPrimitiveInvocation I; I.PrimitiveId=Primitive; I.Version=2; I.RequestId=Tx.Token.RequestId;
  I.TransactionId=Tx.Token.TransactionId; I.RuntimeRevision=Tx.Token.RuntimeRevision; I.Phase=EAssetDocumentRuntimeRequestPhase::Apply;
  I.ValueArguments=Object({TEXT("completion_kind")},{String(TEXT("additive_root"))}); I.HandleArguments={{TEXT("sequence"),Handle.GetValue()}};
  TestTrue(TEXT("unchanged real Editor-issued capability validates"),Group->ValidateInvocation(I,Handles).bValid);
  auto WrongOwner=I; WrongOwner.HandleArguments[0].Handle.ProviderId=TEXT("animation");
  TestFalse(TEXT("consumer cannot relabel the Editor capability owner"),Group->ValidateInvocation(WrongOwner,Handles).bValid);
  AssetDocumentEditorSequenceAdditiveTesting::FHooks Hooks; Hooks.FailAfterField=FailAfter; Tx.ObservedHooks=&Hooks;
  AssetDocumentEditorSequenceAdditiveTesting::SetHooks(&Hooks);
  const auto Applied=Group->Apply(I,Handles,Tx); AssetDocumentEditorSequenceAdditiveTesting::SetHooks(nullptr);
  TestFalse(TEXT("actual partial helper group fails at the selected step"),Applied.bValid);
  TestEqual(TEXT("one parent footprint before any field writer"),Tx.Footprints,1);
  TestTrue(TEXT("complete object/package parent is armed"),Tx.bArmedBeforeFirstWrite);
  TestEqual(TEXT("exact partial writer count"),Hooks.FieldWrites,FailAfter+1);
  TestEqual(TEXT("first actual write occurred"),F.Sequence->AdditiveAnimType.GetValue(),AAT_LocalSpaceBase);
  TestFalse(TEXT("inverse refuses an open capture"),Group->Restore().bValid);
  Group->CloseCapture(); TestTrue(TEXT("real owned four-value inverse restores"),Group->Restore().bValid);
  TestTrue(TEXT("full typed preimage readback"),Group->IsOriginalCurrent());
  TestFalse(TEXT("inverse consumption is terminal"),Group->Restore().bValid);
 }
 return !HasAnyErrors();
}
#endif
