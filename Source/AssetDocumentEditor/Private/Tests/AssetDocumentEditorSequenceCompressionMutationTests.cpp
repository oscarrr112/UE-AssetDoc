// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentEditorSequenceCompressionMutation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "Provider/Sequence/AssetDocumentEditorSequenceCompressionMutationTestHooks.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimCompress_BitwiseCompressOnly.h"
#include "Animation/AnimCurveCompressionCodec_UniformlySampled.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "Animation/Skeleton.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include <limits>

// Actual Editor-issued payloads and a recording parent isolate the fixed
// setter contract. This recording parent does not demonstrate Core rollback.
namespace AssetDocumentEditorSequenceCompressionTests
{
constexpr const TCHAR* Primitive = TEXT("animation.sequence.compression.settings.set.v1");
constexpr const TCHAR* Schema = TEXT("animation.sequence.compression.settings.set.v1.input.v1");
FAssetDocumentRuntimeValueDto String(const FString& S)
{ FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::String; V.StringValue = S; return V; }
FAssetDocumentRuntimeValueDto Number(double N)
{ FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Number; V.NumberValue = N; return V; }
FAssetDocumentRuntimeValueDto BooleanDto(bool B)
{ FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Boolean; V.BooleanValue = B; return V; }
FAssetDocumentRuntimeValueDto Projection(TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values)
{
 FAssetDocumentRuntimeValueDto Inner, Outer; Inner.Kind = Outer.Kind = EAssetDocumentRuntimeValueKind::Object;
 Inner.ObjectKeys = MoveTemp(Keys); Inner.Elements = MoveTemp(Values); Outer.ObjectKeys = {TEXT("fields")}; Outer.Elements.Add(MoveTemp(Inner)); return Outer;
}
struct FTuple
{
 float Scale; UAnimBoneCompressionSettings* Bone; UAnimCurveCompressionSettings* Curve;
 UVariableFrameStrippingSettings* Variable; bool Keep;
 explicit FTuple(const UAnimSequence& S) : Scale(S.CompressionErrorThresholdScale), Bone(S.BoneCompressionSettings.Get()),
  Curve(S.CurveCompressionSettings.Get()), Variable(S.VariableFrameStrippingSettings.Get()), Keep(S.bDoNotOverrideCompression) {}
 bool Matches(const UAnimSequence& S) const
 { return FMemory::Memcmp(&Scale, &S.CompressionErrorThresholdScale, sizeof(float)) == 0 && Bone == S.BoneCompressionSettings.Get()
   && Curve == S.CurveCompressionSettings.Get() && Variable == S.VariableFrameStrippingSettings.Get() && Keep == bool(S.bDoNotOverrideCompression); }
};
struct FFixture
{
 FString Root = TEXT("/Game/AssetDocumentSequenceCompressionEditor/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<USkeleton> Skeleton; TStrongObjectPtr<UAnimSequence> Sequence;
 TStrongObjectPtr<UAnimBoneCompressionSettings> Bone[2]; TStrongObjectPtr<UAnimCurveCompressionSettings> Curve[2];
 TStrongObjectPtr<UVariableFrameStrippingSettings> Variable[2];
 bool Setup()
 {
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  { FReferenceSkeletonModifier Bones(Skeleton.Get()); Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); }
  // All direct authoring below constructs this test's own assets/codecs.
  for (int32 I = 0; I < 2; ++I)
  {
   Bone[I].Reset(NewObject<UAnimBoneCompressionSettings>(CreatePackage(*FString::Printf(TEXT("%s/Bone%d"), *Root, I)), TEXT("Settings"), RF_Public | RF_Standalone));
   Bone[I]->Codecs.Add(NewObject<UAnimCompress_BitwiseCompressOnly>(Bone[I].Get(), TEXT("Codec"))); Bone[I]->ErrorThreshold = 0.1f + I;
   Curve[I].Reset(NewObject<UAnimCurveCompressionSettings>(CreatePackage(*FString::Printf(TEXT("%s/Curve%d"), *Root, I)), TEXT("Settings"), RF_Public | RF_Standalone));
   Curve[I]->Codec = NewObject<UAnimCurveCompressionCodec_UniformlySampled>(Curve[I].Get(), TEXT("Codec"));
   Variable[I].Reset(NewObject<UVariableFrameStrippingSettings>(CreatePackage(*FString::Printf(TEXT("%s/Variable%d"), *Root, I)), TEXT("Settings"), RF_Public | RF_Standalone));
   Variable[I]->UseVariableFrameStripping.Default = false; Variable[I]->FrameStrippingRate.Default = 2 + I;
  }
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  Sequence->SetSkeleton(Skeleton.Get()); Sequence->BoneCompressionSettings = Bone[0].Get(); Sequence->CurveCompressionSettings = Curve[0].Get();
  Sequence->VariableFrameStrippingSettings = Variable[0].Get(); Sequence->CompressionErrorThresholdScale = 1.f; Sequence->bDoNotOverrideCompression = false;
  Sequence->RateScale = 1.25f; Sequence->RetargetSource = TEXT("PreservedName_3");
  auto& C = Sequence->GetController(); C.OpenBracket(FText::FromString(TEXT("Compression Editor fixture")), false);
  C.InitializeModel(); C.SetFrameRate(FFrameRate(24, 1), false); C.SetNumberOfFrames(FFrameNumber(12), false);
  const bool Track = C.AddBoneCurve(TEXT("root"), false); TArray<FVector3f> P, S; TArray<FQuat4f> Q;
  P.Init(FVector3f::ZeroVector, 13); S.Init(FVector3f::OneVector, 13); Q.Init(FQuat4f::Identity, 13);
  const bool Keys = Track && C.SetBoneTrackKeys(TEXT("root"), P, Q, S, false); C.NotifyPopulated(); C.CloseBracket(false);
  Sequence->WaitOnExistingCompression();
  for (UObject* O : Assets()) O->GetOutermost()->SetDirtyFlag(false);
  return Keys && Bone[0]->AreSettingsValid() && Bone[1]->AreSettingsValid() && Curve[0]->AreSettingsValid() && Curve[1]->AreSettingsValid();
 }
 TArray<UObject*> Assets() const
 {
  TArray<UObject*> A{Skeleton.Get(), Sequence.Get()};
  for (int32 I = 0; I < 2; ++I) { A.Add(Bone[I].Get()); A.Add(Curve[I].Get()); A.Add(Variable[I].Get()); } return A;
 }
 ~FFixture()
 { if (Sequence.IsValid()) Sequence->WaitOnExistingCompression(); for (UObject* O : Assets()) if (IsValid(O)) { O->ClearFlags(RF_Public | RF_Standalone); O->GetOutermost()->SetDirtyFlag(false); } }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 1; R.Operation = EAssetDocumentRuntimeOperation::Apply;
  R.TargetAssetPath = Sequence->GetPathName(); R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence"); return R;
 }
 FAssetDocumentMutationIntent Intent(FAssetDocumentRuntimeValueDto V) const
 {
  FAssetDocumentMutationIntent I; I.StepIndex = 0; I.Requirement = {Primitive, 1, TEXT("animation_profile")};
  I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.InputSchemaId = Schema; I.Intent = MoveTemp(V); return I;
 }
 FAssetDocumentRuntimeValueDto All(bool Original = false) const
 {
  const int32 I = Original ? 0 : 1;
  return Projection({TEXT("CompressionErrorThresholdScale"), TEXT("BoneCompressionSettings"), TEXT("CurveCompressionSettings"),
   TEXT("VariableFrameStrippingSettings"), TEXT("bDoNotOverrideCompression")},
   {Number(Original ? 1. : 2.5), String(Bone[I]->GetPathName()), String(Curve[I]->GetPathName()), String(Variable[I]->GetPathName()), BooleanDto(!Original)});
 }
};
struct FRetained
{
 FGuid Model, SkeletonGuid; USkeleton* Skeleton; FName Name; float Rate; bool Dirty;
 FSoftObjectPath RetargetAsset; TArray<FTransform> StoredPose;
 TArray<bool> SettingsDirty;
 explicit FRetained(const FFixture& F) : Model(F.Sequence->GetDataModel()->GenerateGuid()), SkeletonGuid(F.Sequence->GetSkeletonGuid()),
  Skeleton(F.Sequence->GetSkeleton()), Name(F.Sequence->RetargetSource), Rate(F.Sequence->RateScale), Dirty(F.Sequence->GetOutermost()->IsDirty()),
  RetargetAsset(F.Sequence->GetRetargetSourceAsset().ToSoftObjectPath()), StoredPose(F.Sequence->RetargetSourceAssetReferencePose)
 { for (int32 I = 0; I < 2; ++I) { SettingsDirty.Add(F.Bone[I]->GetOutermost()->IsDirty()); SettingsDirty.Add(F.Curve[I]->GetOutermost()->IsDirty()); SettingsDirty.Add(F.Variable[I]->GetOutermost()->IsDirty()); } }
 void Check(FAutomationTestBase& T, const FFixture& F) const
 {
  T.TestEqual(TEXT("fixed setter preserves authored model"), F.Sequence->GetDataModel()->GenerateGuid(), Model);
  T.TestEqual(TEXT("fixed setter preserves skeleton identity"), F.Sequence->GetSkeleton(), Skeleton);
  T.TestEqual(TEXT("fixed setter preserves skeleton GUID"), F.Sequence->GetSkeletonGuid(), SkeletonGuid);
  T.TestTrue(TEXT("fixed setter preserves exact retained name"), F.Sequence->RetargetSource.IsEqual(Name, ENameCase::CaseSensitive, true));
  T.TestEqual(TEXT("fixed setter preserves retained source asset"), F.Sequence->GetRetargetSourceAsset().ToSoftObjectPath(), RetargetAsset);
  bool ExactStored = StoredPose.Num() == F.Sequence->RetargetSourceAssetReferencePose.Num();
  for (int32 I = 0; ExactStored && I < StoredPose.Num(); ++I)
   ExactStored = StoredPose[I].GetTranslation() == F.Sequence->RetargetSourceAssetReferencePose[I].GetTranslation()
    && StoredPose[I].GetRotation() == F.Sequence->RetargetSourceAssetReferencePose[I].GetRotation()
    && StoredPose[I].GetScale3D() == F.Sequence->RetargetSourceAssetReferencePose[I].GetScale3D();
  T.TestTrue(TEXT("fixed setter preserves complete retained stored pose"), ExactStored);
  T.TestEqual(TEXT("fixed setter preserves unrelated scalar"), F.Sequence->RateScale, Rate);
  T.TestEqual(TEXT("Never preserves target dirty preimage"), F.Sequence->GetOutermost()->IsDirty(), Dirty);
  for (int32 I = 0; I < 2; ++I)
  { T.TestEqual(TEXT("readonly bone package unchanged"), F.Bone[I]->GetOutermost()->IsDirty(), SettingsDirty[I * 3]);
    T.TestEqual(TEXT("readonly curve package unchanged"), F.Curve[I]->GetOutermost()->IsDirty(), SettingsDirty[I * 3 + 1]);
    T.TestEqual(TEXT("readonly variable package unchanged"), F.Variable[I]->GetOutermost()->IsDirty(), SettingsDirty[I * 3 + 2]); }
 }
};
class FResolver final : public IAssetDocumentHandleResolver
{
 struct FEntry { FAssetDocumentOpaqueHandle Handle; TUniquePtr<IAssetDocumentNativeHandlePayload> Payload; };
 class FLease final : public IAssetDocumentVerifiedHandleLease
 {
  FAssetDocumentOpaqueHandle Handle; IAssetDocumentNativeHandlePayload& Payload; int32& Count;
 public:
  FLease(const FAssetDocumentOpaqueHandle& H, IAssetDocumentNativeHandlePayload& P, int32& C) : Handle(H), Payload(P), Count(C) { ++Count; }
  ~FLease() override { --Count; }
  const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
  IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return Payload; }
 };
 TArray<FEntry> Entries;
public:
 FAssetDocumentMutationToken Token; int32 ActiveLeases = 0;
 FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access, TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
 {
  auto& E = Entries.AddDefaulted_GetRef(); E.Handle.RequestId = Token.RequestId; E.Handle.TransactionId = Token.TransactionId;
  E.Handle.RuntimeRevision = Token.RuntimeRevision; E.Handle.ProviderId = TEXT("editor"); E.Handle.Slot = Entries.Num();
  E.Handle.Generation = 1; E.Handle.Kind = Kind; E.Handle.Access = Access; E.Payload = MoveTemp(Payload);
  FAssetDocumentIssuedHandleResult R; R.Handle = E.Handle; return R;
 }
 FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& H, const FAssetDocumentHandleValidationContext& C) override
 {
  FAssetDocumentVerifiedLeaseResult R; auto V = ValidateAssetDocumentHandleMetadata(H, C);
  if (!V.bValid) { R.Diagnostics = MoveTemp(V.Diagnostics); return R; }
  const int32 I = int32(H.Slot) - 1;
  if (!Entries.IsValidIndex(I) || Entries[I].Handle.Generation != H.Generation || Entries[I].Handle.Kind != H.Kind
   || Entries[I].Handle.Access != H.Access || !Entries[I].Payload || !Entries[I].Payload->IsAlive()) return R;
  R.Lease = MakeUnique<FLease>(H, *Entries[I].Payload, ActiveLeases); return R;
 }
 FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override
 { FAssetDocumentHandleInvalidationResult R; R.InvalidatedHandleCount = Entries.Num(); Entries.Reset(); return R; }
};
class FCompressionTransaction final : public IAssetDocumentAtomicTransaction
{
public:
 FAssetDocumentMutationToken Token; UAnimSequence* Observed = nullptr; TUniquePtr<FTuple> Old;
 int32 Enlistments = 0; bool ArmedBeforeWrite = false, RejectEnlistment = false; TFunction<void()> OnEnlist;
 FGuid GetTransactionId() const override { return Token.TransactionId; }
 EAssetDocumentAtomicState GetState() const override { return EAssetDocumentAtomicState::Mutating; }
 const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
 FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Rows) override
 {
  ++Enlistments; ArmedBeforeWrite = Observed && Old && Old->Matches(*Observed) && Rows.Num() == 2
   && Rows[0].Kind == EAssetDocumentMutationFootprintKind::UObjectState && Rows[0].StableId.Equals(TEXT("Object:") + Observed->GetPathName(), ESearchCase::CaseSensitive)
   && Rows[1].Kind == EAssetDocumentMutationFootprintKind::Package && Rows[1].StableId.Equals(TEXT("Package:") + Observed->GetOutermost()->GetName(), ESearchCase::CaseSensitive);
  if (OnEnlist) OnEnlist(); auto R = Valid(); R.bValid = !RejectEnlistment; return R;
 }
 FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Valid(); }
 FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return Valid(); }
 FAssetDocumentContractValidationResult Rollback(FStringView) override { return Valid(); }
 FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction>) override { return Valid(); }
 FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override { return Valid(); }
 static FAssetDocumentContractValidationResult Valid() { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
};
TOptional<FAssetDocumentOpaqueHandle> Resolve(UObject& O, EAssetDocumentHandleAccess Access, FResolver& H, FCompressionTransaction& T)
{
 FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = TEXT("editor.asset.resolve_existing.v1"); I.Version = 1;
 I.RequestId = T.Token.RequestId; I.TransactionId = T.Token.TransactionId; I.RuntimeRevision = T.Token.RuntimeRevision;
 I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
 I.ValueArguments.ObjectKeys = {TEXT("path"), TEXT("expected_class")}; I.ValueArguments.Elements = {String(O.GetPathName()), String(O.GetClass()->GetPathName())};
 const auto Out = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(I, H, &T);
 if (Out.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) return {};
 const TCHAR* Role = Access == EAssetDocumentHandleAccess::Mutable ? TEXT("asset_mutable") : TEXT("asset_read");
 for (const auto& R : Out.HandleResults) if (R.Name.Equals(Role, ESearchCase::CaseSensitive) && R.Handles.Num() == 1) return R.Handles[0]; return {};
}
bool MakeInvocation(const FFixture& F, const FAssetDocumentMutationIntent& Intent, FResolver& H, FCompressionTransaction& T, FAssetDocumentPrimitiveInvocation& I)
{
 I.PrimitiveId = Primitive; I.Version = 1; I.Phase = Intent.Phase; I.RequestId = T.Token.RequestId;
 I.TransactionId = T.Token.TransactionId; I.RuntimeRevision = T.Token.RuntimeRevision; I.ValueArguments = Intent.Intent;
 const auto Target = Resolve(*F.Sequence, EAssetDocumentHandleAccess::Mutable, H, T); if (!Target) return false;
 I.HandleArguments.Add({TEXT("sequence"), Target.GetValue()});
 const auto& Fields = Intent.Intent.Elements[0];
 for (int32 N = 0; N < Fields.ObjectKeys.Num(); ++N)
 {
  const TCHAR* Role = nullptr;
  if (Fields.ObjectKeys[N].Equals(TEXT("BoneCompressionSettings"), ESearchCase::CaseSensitive)) Role = TEXT("bone_compression_settings");
  if (Fields.ObjectKeys[N].Equals(TEXT("CurveCompressionSettings"), ESearchCase::CaseSensitive)) Role = TEXT("curve_compression_settings");
  if (Fields.ObjectKeys[N].Equals(TEXT("VariableFrameStrippingSettings"), ESearchCase::CaseSensitive)) Role = TEXT("frame_stripping_settings");
  if (!Role) continue; UObject* Asset = FSoftObjectPath(Fields.Elements[N].StringValue).ResolveObject(); if (!Asset) return false;
  const auto Ref = Resolve(*Asset, EAssetDocumentHandleAccess::ReadOnly, H, T); if (!Ref) return false; I.HandleArguments.Add({Role, Ref.GetValue()});
 }
 return true;
}
struct FLifecycle
{
 int32 Changed = 0, Dirty = 0, Modified = 0; FDelegateHandle A, B, C;
 explicit FLifecycle(UAnimSequence& S)
 {
  A = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this, &S](UObject* O, FPropertyChangedEvent&) { if (O == &S) ++Changed; });
  B = UPackage::PackageDirtyStateChangedEvent.AddLambda([this, &S](UPackage* P) { if (P == S.GetOutermost()) ++Dirty; });
  C = FCoreUObjectDelegates::OnObjectModified.AddLambda([this, &S](UObject* O) { if (O == &S) ++Modified; });
 }
 ~FLifecycle() { FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(A); UPackage::PackageDirtyStateChangedEvent.Remove(B); FCoreUObjectDelegates::OnObjectModified.Remove(C); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEditorSequenceCompressionFixedSetterTest,
 "UEAssetDoc.AnimationProvider.SequenceCompression.EditorFixedSetter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentEditorSequenceCompressionFixedSetterTest::RunTest(const FString&)
{
 using namespace AssetDocumentEditorSequenceCompressionTests;
 FFixture F; if (!TestTrue(TEXT("test-owned valid settings and model"), F.Setup())) return false;
 const auto Request = F.Request(); const FTuple Old(*F.Sequence);
 for (const bool Dirty : {false, true})
 {
  F.Sequence->GetOutermost()->SetDirtyFlag(Dirty); const FRetained Before(F); auto Intent = F.Intent(F.All());
  if (Dirty) Intent.Phase = EAssetDocumentRuntimeRequestPhase::Repair;
  TSharedPtr<IAssetDocumentEditorSequenceCompressionMutation> Group;
  if (!TestTrue(TEXT("five-field tuple prepares"), PrepareAssetDocumentEditorSequenceCompressionMutation(Request, Intent, Group).bValid)) return false;
  TestTrue(TEXT("preparation preserves complete original tuple"), Old.Matches(*F.Sequence)); Before.Check(*this, F);
  FCompressionTransaction T; T.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision}; T.Observed = F.Sequence.Get(); T.Old = MakeUnique<FTuple>(*F.Sequence);
  FResolver H; H.Token = T.Token; FAssetDocumentPrimitiveInvocation I;
  if (!TestTrue(TEXT("actual Editor resolves all four native handles"), MakeInvocation(F, Intent, H, T, I))) return false;
  TestTrue(TEXT("capture binds exact parent"), Group->OpenCapture(T.Token.TransactionId).bValid);
  const auto WrongRead = Resolve(*F.Sequence, EAssetDocumentHandleAccess::ReadOnly, H, T);
  const auto WrongSettings = Resolve(*F.Bone[0], EAssetDocumentHandleAccess::ReadOnly, H, T);
  const auto MutableSettings = Resolve(*F.Bone[1], EAssetDocumentHandleAccess::Mutable, H, T);
  if (!WrongRead || !WrongSettings || !MutableSettings) return false;
  for (int32 Case = 0; Case < 18; ++Case)
  {
   auto Bad = I;
   if (Case == 0) Bad.HandleArguments[0].Handle.ProviderId = TEXT("animation");
   if (Case == 1) Bad.HandleArguments[0].Handle.Generation += 1;
   if (Case == 2) Bad.HandleArguments[0].Handle = WrongRead.GetValue();
   if (Case == 3) Bad.HandleArguments[0].Name = TEXT("Sequence");
   if (Case == 4) { const auto Copy = Bad.HandleArguments[0]; Bad.HandleArguments.Add(Copy); }
   if (Case == 5) Bad.TransactionId = FGuid::NewGuid();
   if (Case == 6) Bad.RequestId = FGuid::NewGuid();
   if (Case == 7) Bad.RuntimeRevision += 1;
   if (Case == 8) Bad.ValueArguments.Elements[0].Elements[0] = Number(2.5000000000000004);
   if (Case == 9) Bad.ValueArguments.Elements[0].Elements[1] = String(F.Bone[1]->GetPathName().ToLower());
   if (Case == 10) Bad.ValueArguments.Elements[0].ObjectKeys[1] = TEXT("bonecompressionsettings");
   if (Case == 11) Bad.ValueArguments.ObjectKeys[0] = TEXT("Fields");
   if (Case == 12) Bad.HandleArguments.RemoveAt(1);
   if (Case == 13) Bad.HandleArguments[1].Handle = WrongSettings.GetValue();
   if (Case == 14) Bad.HandleArguments[1].Handle = MutableSettings.GetValue();
   if (Case == 15) Bad.HandleArguments[1].Handle.ProviderId = TEXT("Editor");
   if (Case == 16) { Bad.ValueArguments.Elements[0].ObjectKeys.Add(TEXT("null_policy")); Bad.ValueArguments.Elements[0].Elements.Add(String(TEXT("default"))); }
   if (Case == 17) Bad.ValueArguments.Elements[0].Elements[4] = BooleanDto(false);
   TestFalse(FString::Printf(TEXT("literal/identity drift rejects before enlist (case %d)"), Case), Group->Apply(Bad, H, T).bValid);
   TestEqual(TEXT("rejected invocation has no footprint"), T.Enlistments, 0); TestFalse(TEXT("rejected invocation did not write"), Group->DidWrite());
   TestTrue(TEXT("rejected invocation preserves original tuple"), Group->IsOriginalCurrent()); Before.Check(*this, F);
  }
  // Key order is irrelevant while key spelling, kind and literal stay bound.
  Swap(I.ValueArguments.Elements[0].ObjectKeys[0], I.ValueArguments.Elements[0].ObjectKeys[4]);
  Swap(I.ValueArguments.Elements[0].Elements[0], I.ValueArguments.Elements[0].Elements[4]);
  FLifecycle Events(*F.Sequence);
  TestTrue(TEXT("identity callbacks hold target and supplied refs across body"), Group->WithVerifiedAssets(I, H, [&]
  { TestEqual(TEXT("all four actual leases are held"), H.ActiveLeases, 4); return Group->Apply(I, H, T); }).bValid);
  TestEqual(TEXT("callbacks release after body"), H.ActiveLeases, 0); TestEqual(TEXT("one whole-batch Editor enlistment"), T.Enlistments, 1);
  TestTrue(TEXT("whole preimage was armed before first setter"), T.ArmedBeforeWrite); TestTrue(TEXT("full desired tuple readback"), Group->IsDesiredCurrent());
  TestTrue(TEXT("bitfield boolean is set correctly"), bool(F.Sequence->bDoNotOverrideCompression)); TestTrue(TEXT("written tuple recorded"), Group->DidWrite());
  TestFalse(TEXT("participant consumes invocation once"), Group->Apply(I, H, T).bValid);
  Group->CloseCapture(); TestTrue(TEXT("public fixed setter restores complete original tuple"), Group->Restore().bValid);
  TestTrue(TEXT("inverse exact original readback"), Group->IsOriginalCurrent()); TestFalse(TEXT("inverse is terminal"), Group->Restore().bValid);
  TestEqual(TEXT("Never emits no property notification"), Events.Changed, 0); TestEqual(TEXT("Never emits no dirty event"), Events.Dirty, 0);
  TestEqual(TEXT("fixed participant adds no Modify notification"), Events.Modified, 0); Before.Check(*this, F);
 }
 F.Sequence->GetOutermost()->SetDirtyFlag(false);
 for (int32 Fail = 0; Fail < 5; ++Fail)
 {
  const FRetained Before(F); const auto Intent = F.Intent(F.All()); TSharedPtr<IAssetDocumentEditorSequenceCompressionMutation> Group;
  if (!PrepareAssetDocumentEditorSequenceCompressionMutation(Request, Intent, Group).bValid) return false;
  FCompressionTransaction T; T.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision}; T.Observed = F.Sequence.Get(); T.Old = MakeUnique<FTuple>(*F.Sequence);
  FResolver H; H.Token = T.Token; FAssetDocumentPrimitiveInvocation I; if (!MakeInvocation(F, Intent, H, T, I)) return false;
  TestTrue(TEXT("partial-failure capture opens"), Group->OpenCapture(T.Token.TransactionId).bValid);
  AssetDocumentEditorSequenceCompressionTesting::FHooks Hook; Hook.FailAfterField = Fail; AssetDocumentEditorSequenceCompressionTesting::SetHooks(&Hook);
  const auto Applied = Group->Apply(I, H, T); AssetDocumentEditorSequenceCompressionTesting::SetHooks(nullptr);
  TestFalse(FString::Printf(TEXT("forward injection fails at zero-based field %d"), Fail), Applied.bValid);
  TestEqual(TEXT("hook sees only completed forward setters"), Hook.FieldWrites, Fail + 1); TestTrue(TEXT("partial touch is recorded for inverse"), Group->DidWrite());
  TestEqual(TEXT("partial write has one enlisted preimage"), T.Enlistments, 1); TestTrue(TEXT("partial write armed before first setter"), T.ArmedBeforeWrite);
  Group->CloseCapture(); TestTrue(TEXT("full tuple inverse succeeds after partial failure"), Group->Restore().bValid);
  TestTrue(TEXT("partial failure restores all five values"), Old.Matches(*F.Sequence)); Before.Check(*this, F);
 }
 for (int32 Case = 0; Case < 2; ++Case)
 {
  auto Intent = F.Intent(F.All()); TSharedPtr<IAssetDocumentEditorSequenceCompressionMutation> Group;
  if (!PrepareAssetDocumentEditorSequenceCompressionMutation(Request, Intent, Group).bValid) return false;
  FCompressionTransaction T; T.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision}; T.Observed = F.Sequence.Get(); T.Old = MakeUnique<FTuple>(*F.Sequence);
  FResolver H; H.Token = T.Token; FAssetDocumentPrimitiveInvocation I; if (!MakeInvocation(F, Intent, H, T, I)) return false;
  Group->OpenCapture(T.Token.TransactionId); T.RejectEnlistment = Case == 0;
  if (Case == 1) T.OnEnlist = [&] { F.Sequence->CompressionErrorThresholdScale = 11.f; };
  TestFalse(TEXT("enlist failure or changed preimage blocks first setter"), Group->Apply(I, H, T).bValid);
  TestFalse(TEXT("post-enlist rejection did not touch a setter"), Group->DidWrite());
  TestEqual(TEXT("no supplied reference was published"), F.Sequence->BoneCompressionSettings.Get(), F.Bone[0].Get());
  Group->CloseCapture(); TestTrue(TEXT("untouched participant inverse is harmless"), Group->Restore().bValid);
  F.Sequence->CompressionErrorThresholdScale = Old.Scale;
 }
 {
  const auto Intent = F.Intent(F.All(true)); TSharedPtr<IAssetDocumentEditorSequenceCompressionMutation> Group;
  if (!PrepareAssetDocumentEditorSequenceCompressionMutation(Request, Intent, Group).bValid) return false;
  FCompressionTransaction T; T.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision}; FResolver H; H.Token = T.Token;
  FAssetDocumentPrimitiveInvocation I; if (!MakeInvocation(F, Intent, H, T, I)) return false; Group->OpenCapture(T.Token.TransactionId);
  auto Missing = I; Missing.HandleArguments.RemoveAt(1);
  TestFalse(TEXT("same-ref noop still requires supplied readonly handle"), Group->Apply(Missing, H, T).bValid);
  TestTrue(TEXT("verified full same-tuple noop succeeds"), Group->Apply(I, H, T).bValid); TestEqual(TEXT("true noop has zero footprint"), T.Enlistments, 0);
  TestFalse(TEXT("true noop never writes"), Group->DidWrite()); Group->CloseCapture(); TestTrue(TEXT("noop inverse succeeds"), Group->Restore().bValid);
 }
 for (const double Scale : {0., -2., double(std::numeric_limits<float>::max()), double(std::numeric_limits<float>::min())})
 {
  const auto Intent = F.Intent(Projection({TEXT("CompressionErrorThresholdScale")}, {Number(Scale)}));
  TSharedPtr<IAssetDocumentEditorSequenceCompressionMutation> Group;
  TestTrue(TEXT("representation accepts finite zero/negative/extreme floats without invented range"), PrepareAssetDocumentEditorSequenceCompressionMutation(Request, Intent, Group).bValid);
  TestTrue(TEXT("scale preparation leaves original untouched"), Old.Matches(*F.Sequence));
 }
 for (const bool NegativeOriginal : {false, true})
 {
  // Test-owned direct construction of each original zero representation. The
  // helper's native-identical path must retain that exact original byte value.
  F.Sequence->CompressionErrorThresholdScale = NegativeOriginal ? -0.f : 0.f;
  const float OriginalZero = F.Sequence->CompressionErrorThresholdScale;
  const double Literal = NegativeOriginal ? 0. : -0.;
  const auto Intent = F.Intent(Projection({TEXT("CompressionErrorThresholdScale")}, {Number(Literal)}));
  TSharedPtr<IAssetDocumentEditorSequenceCompressionMutation> Group;
  if (!PrepareAssetDocumentEditorSequenceCompressionMutation(Request, Intent, Group).bValid) return false;
  TestFalse(TEXT("native-identical signed zero is a true noop"), Group->HasChanges());
  FCompressionTransaction T; T.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision}; FResolver H; H.Token = T.Token;
  FAssetDocumentPrimitiveInvocation I; if (!MakeInvocation(F, Intent, H, T, I)) return false; Group->OpenCapture(T.Token.TransactionId);
  auto WrongLiteral = I; WrongLiteral.ValueArguments.Elements[0].Elements[0] = Number(-Literal);
  TestFalse(TEXT("zero native equivalence does not loosen frozen double literal"), Group->Apply(WrongLiteral, H, T).bValid);
  TestTrue(TEXT("bound signed-zero noop succeeds"), Group->Apply(I, H, T).bValid);
  TestEqual(TEXT("signed-zero noop has zero footprint"), T.Enlistments, 0); TestFalse(TEXT("signed-zero noop never writes"), Group->DidWrite());
  TestTrue(TEXT("desired readback retains original zero bytes"), Group->IsDesiredCurrent()
   && FMemory::Memcmp(&OriginalZero, &F.Sequence->CompressionErrorThresholdScale, sizeof(float)) == 0);
  Group->CloseCapture(); TestTrue(TEXT("signed-zero noop inverse succeeds"), Group->Restore().bValid);
  TestTrue(TEXT("inverse retains original zero bytes"), FMemory::Memcmp(&OriginalZero, &F.Sequence->CompressionErrorThresholdScale, sizeof(float)) == 0);
  F.Sequence->CompressionErrorThresholdScale = Old.Scale;
 }
 FString Embedded = F.Bone[1]->GetPathName(); Embedded[5] = 0;
 for (int32 Case = 0; Case < 12; ++Case)
 {
  auto Intent = F.Intent(F.All()); auto R = Request;
  if (Case == 0) Intent.Intent = Projection({}, {});
  if (Case == 1) Intent.Intent = Projection({TEXT("CompressionErrorThresholdScale")}, {Number(1.e100)});
  if (Case == 2) Intent.Intent = Projection({TEXT("CompressionErrorThresholdScale")}, {Number(1.e-100)});
  if (Case == 3) Intent.Intent = Projection({TEXT("CompressionErrorThresholdScale")}, {Number(std::numeric_limits<double>::infinity())});
  if (Case == 4) Intent.Intent = Projection({TEXT("BoneCompressionSettings")}, {FAssetDocumentRuntimeValueDto()});
  if (Case == 5) Intent.Intent = Projection({TEXT("BoneCompressionSettings")}, {String(Embedded)});
  if (Case == 6) Intent.Intent = Projection({TEXT("BoneCompressionSettings")}, {String(F.Root + TEXT("/Unloaded.Unloaded"))});
  if (Case == 7) Intent.Intent = Projection({TEXT("CompressionErrorThresholdScale")}, {BooleanDto(true)});
  if (Case == 8) Intent.Intent = Projection({TEXT("bDoNotOverrideCompression")}, {String(TEXT("true"))});
  if (Case == 9) Intent.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == 10) R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
  if (Case == 11) Intent.Intent.Elements[0].ObjectKeys[0] = TEXT("compressionerrorthresholdscale");
  TSharedPtr<IAssetDocumentEditorSequenceCompressionMutation> Rejected;
  TestFalse(FString::Printf(TEXT("closed fixed input rejects invalid projection (case %d)"), Case), PrepareAssetDocumentEditorSequenceCompressionMutation(R, Intent, Rejected).bValid);
  TestFalse(TEXT("rejection publishes no native participant"), Rejected.IsValid()); TestTrue(TEXT("rejection preserves entire original tuple"), Old.Matches(*F.Sequence));
 }
 return true;
}
#endif
