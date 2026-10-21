// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentEditorSequenceRetargetSourceNameMutation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

// This exercises the fixed Editor participant with actual Editor-issued native
// payloads. The recording transaction is not evidence of genuine Core rollback.
namespace AssetDocumentEditorSequenceRetargetSourceNameTests
{
constexpr const TCHAR* Primitive = TEXT("animation.sequence.retarget_source_name.set.v1");
constexpr const TCHAR* Schema = TEXT("animation.sequence.retarget_source_name.set.v1.input.v1");
FAssetDocumentRuntimeValueDto String(const FString& Text)
{ FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::String; V.StringValue = Text; return V; }
FAssetDocumentRuntimeValueDto NameValue(const FString& Text)
{
 FAssetDocumentRuntimeValueDto V; V.Kind = EAssetDocumentRuntimeValueKind::Object;
 V.ObjectKeys = {TEXT("retarget_source_name")}; V.Elements = {String(Text)}; return V;
}
bool ExactName(FName A, FName B) { return A.IsEqual(B, ENameCase::CaseSensitive, true); }
bool ExactPose(const TArray<FTransform>& A, const TArray<FTransform>& B)
{
 if (A.Num() != B.Num()) return false;
 for (int32 I = 0; I < A.Num(); ++I)
  if (A[I].GetTranslation() != B[I].GetTranslation() || A[I].GetRotation() != B[I].GetRotation()
   || A[I].GetScale3D() != B[I].GetScale3D()) return false;
 return true;
}
struct FFixture
{
 TStrongObjectPtr<USkeleton> Skeleton;
 TStrongObjectPtr<USkeletalMesh> SourceMesh;
 TStrongObjectPtr<UAnimSequence> Sequence;
 bool Setup()
 {
  const FString Root = TEXT("/Game/AssetDocumentSequenceRetargetNameEditor/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  // Construction and authored assignments below belong to this test fixture.
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  { FReferenceSkeletonModifier Bones(Skeleton.Get()); Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); }
  SourceMesh.Reset(NewObject<USkeletalMesh>(CreatePackage(*(Root + TEXT("/Source"))), TEXT("Source"), RF_Public | RF_Standalone));
  SourceMesh->SetSkeleton(Skeleton.Get());
  { FReferenceSkeletonModifier Bones(SourceMesh->GetRefSkeleton(), Skeleton.Get());
   Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform(FVector(7., 0., 0.))); }
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  Sequence->SetSkeleton(Skeleton.Get()); Sequence->RateScale = 1.25f;
  auto& C = Sequence->GetController();
  C.OpenBracket(FText::FromString(TEXT("Retarget name Editor fixture")), false);
  C.InitializeModel(); C.SetFrameRate(FFrameRate(24, 1), false); C.SetNumberOfFrames(FFrameNumber(12), false);
  const bool bTrack = C.AddBoneCurve(TEXT("root"), false);
  TArray<FVector3f> P, S; TArray<FQuat4f> Q;
  P.Init(FVector3f::ZeroVector, 13); S.Init(FVector3f::OneVector, 13); Q.Init(FQuat4f::Identity, 13);
  const bool bKeys = bTrack && C.SetBoneTrackKeys(TEXT("root"), P, Q, S, false);
  C.NotifyPopulated(); C.CloseBracket(false); Sequence->WaitOnExistingCompression();
  Sequence->SetRetargetSourceAsset(SourceMesh.Get()); Sequence->UpdateRetargetSourceAssetData();
  Sequence->RetargetSource = FName(TEXT("RetainedSource_9"));
  Sequence->ClearAllCachedCookedPlatformData(); Sequence->CacheDerivedDataForCurrentPlatform(); Sequence->WaitOnExistingCompression();
  for (UObject* O : {static_cast<UObject*>(Sequence.Get()), static_cast<UObject*>(Skeleton.Get()), static_cast<UObject*>(SourceMesh.Get())})
   O->GetOutermost()->SetDirtyFlag(false);
  return bKeys;
 }
 ~FFixture()
 {
  if (Sequence.IsValid()) Sequence->WaitOnExistingCompression();
  for (UObject* O : {static_cast<UObject*>(Sequence.Get()), static_cast<UObject*>(SourceMesh.Get()), static_cast<UObject*>(Skeleton.Get())})
   if (IsValid(O)) { O->ClearFlags(RF_Public | RF_Standalone); O->GetOutermost()->SetDirtyFlag(false); }
 }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 1;
  R.Operation = EAssetDocumentRuntimeOperation::Apply; R.TargetAssetPath = Sequence->GetPathName();
  R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence"); return R;
 }
 FAssetDocumentMutationIntent Intent(const FString& Text) const
 {
  FAssetDocumentMutationIntent I; I.StepIndex = 0; I.Requirement = {Primitive, 1, TEXT("animation_profile")};
  I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.InputSchemaId = Schema; I.Intent = NameValue(Text); return I;
 }
};
struct FRetainedState
{
 FSoftObjectPath Asset; TArray<FTransform> Pose; FGuid Model, SkeletonGuid;
 USkeleton* Skeleton; float Rate; bool bTargetDirty, bSkeletonDirty, bMeshDirty;
 explicit FRetainedState(const FFixture& F)
  : Asset(F.Sequence->GetRetargetSourceAsset().ToSoftObjectPath()), Pose(F.Sequence->RetargetSourceAssetReferencePose),
    Model(F.Sequence->GetDataModel()->GenerateGuid()), SkeletonGuid(F.Sequence->GetSkeletonGuid()), Skeleton(F.Sequence->GetSkeleton()),
    Rate(F.Sequence->RateScale), bTargetDirty(F.Sequence->GetOutermost()->IsDirty()),
    bSkeletonDirty(F.Skeleton->GetOutermost()->IsDirty()), bMeshDirty(F.SourceMesh->GetOutermost()->IsDirty()) {}
 void Check(FAutomationTestBase& T, const FFixture& F) const
 {
  T.TestEqual(TEXT("fixed setter preserves soft asset identity"), F.Sequence->GetRetargetSourceAsset().ToSoftObjectPath(), Asset);
  T.TestTrue(TEXT("fixed setter preserves the entire hidden stored pose"), ExactPose(F.Sequence->RetargetSourceAssetReferencePose, Pose));
  T.TestEqual(TEXT("fixed setter preserves model"), F.Sequence->GetDataModel()->GenerateGuid(), Model);
  T.TestEqual(TEXT("fixed setter preserves Skeleton identity"), F.Sequence->GetSkeleton(), Skeleton);
  T.TestEqual(TEXT("fixed setter preserves Skeleton GUID"), F.Sequence->GetSkeletonGuid(), SkeletonGuid);
  T.TestEqual(TEXT("fixed setter preserves unrelated scalar"), F.Sequence->RateScale, Rate);
  T.TestEqual(TEXT("Never setter preserves target dirty state"), F.Sequence->GetOutermost()->IsDirty(), bTargetDirty);
  T.TestEqual(TEXT("Skeleton package is unchanged"), F.Skeleton->GetOutermost()->IsDirty(), bSkeletonDirty);
  T.TestEqual(TEXT("source mesh package is unchanged"), F.SourceMesh->GetOutermost()->IsDirty(), bMeshDirty);
 }
};
class FResolver final : public IAssetDocumentHandleResolver
{
 struct FEntry { FAssetDocumentOpaqueHandle Handle; TUniquePtr<IAssetDocumentNativeHandlePayload> Payload; };
 class FLease final : public IAssetDocumentVerifiedHandleLease
 {
  FAssetDocumentOpaqueHandle Handle; IAssetDocumentNativeHandlePayload& Payload;
 public:
  FLease(const FAssetDocumentOpaqueHandle& H, IAssetDocumentNativeHandlePayload& P) : Handle(H), Payload(P) {}
  const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
  IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return Payload; }
 };
 TArray<FEntry> Entries;
public:
 FAssetDocumentMutationToken Token;
 FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access,
  TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
 {
  auto& E = Entries.AddDefaulted_GetRef(); E.Handle.RequestId = Token.RequestId; E.Handle.TransactionId = Token.TransactionId;
  E.Handle.RuntimeRevision = Token.RuntimeRevision; E.Handle.ProviderId = TEXT("editor"); E.Handle.Slot = Entries.Num();
  E.Handle.Generation = 1; E.Handle.Kind = Kind; E.Handle.Access = Access; E.Payload = MoveTemp(Payload);
  FAssetDocumentIssuedHandleResult R; R.Handle = E.Handle; return R;
 }
 FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& H,
  const FAssetDocumentHandleValidationContext& C) override
 {
  FAssetDocumentVerifiedLeaseResult R; auto V = ValidateAssetDocumentHandleMetadata(H, C);
  if (!V.bValid) { R.Diagnostics = MoveTemp(V.Diagnostics); return R; }
  const int32 Index = int32(H.Slot) - 1;
  if (!Entries.IsValidIndex(Index) || Entries[Index].Handle.Generation != H.Generation || Entries[Index].Handle.Kind != H.Kind
   || Entries[Index].Handle.Access != H.Access || !Entries[Index].Payload || !Entries[Index].Payload->IsAlive()) return R;
  R.Lease = MakeUnique<FLease>(H, *Entries[Index].Payload); return R;
 }
 FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override
 { FAssetDocumentHandleInvalidationResult R; R.InvalidatedHandleCount = Entries.Num(); Entries.Reset(); return R; }
};
class FRetargetSourceNameTransaction final : public IAssetDocumentAtomicTransaction
{
public:
 FAssetDocumentMutationToken Token; UAnimSequence* ObservedTarget = nullptr; FName Original;
 int32 Enlistments = 0; bool bArmedBeforeWrite = false, bRejectEnlistment = false;
 FGuid GetTransactionId() const override { return Token.TransactionId; }
 EAssetDocumentAtomicState GetState() const override { return EAssetDocumentAtomicState::Mutating; }
 const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
 FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Rows) override
 {
  ++Enlistments;
  bArmedBeforeWrite = ObservedTarget && ExactName(ObservedTarget->RetargetSource, Original) && Rows.Num() == 2
   && Rows[0].Kind == EAssetDocumentMutationFootprintKind::UObjectState
   && Rows[0].StableId == TEXT("Object:") + ObservedTarget->GetPathName()
   && Rows[1].Kind == EAssetDocumentMutationFootprintKind::Package
   && Rows[1].StableId == TEXT("Package:") + ObservedTarget->GetOutermost()->GetName();
  auto R = Valid(); R.bValid = !bRejectEnlistment; return R;
 }
 FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Valid(); }
 FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return Valid(); }
 FAssetDocumentContractValidationResult Rollback(FStringView) override { return Valid(); }
 FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction>) override { return Valid(); }
 FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override { return Valid(); }
 static FAssetDocumentContractValidationResult Valid() { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
};
TOptional<FAssetDocumentOpaqueHandle> Resolve(UObject& Object, EAssetDocumentHandleAccess Access, FResolver& Handles, FRetargetSourceNameTransaction& Tx)
{
 FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = TEXT("editor.asset.resolve_existing.v1"); I.Version = 1;
 I.RequestId = Tx.Token.RequestId; I.TransactionId = Tx.Token.TransactionId; I.RuntimeRevision = Tx.Token.RuntimeRevision;
 I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
 I.ValueArguments.ObjectKeys = {TEXT("path"), TEXT("expected_class")};
 I.ValueArguments.Elements = {String(Object.GetPathName()), String(Object.GetClass()->GetPathName())};
 const auto O = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(I, Handles, &Tx);
 if (O.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) return {};
 const FString Name = Access == EAssetDocumentHandleAccess::Mutable ? TEXT("asset_mutable") : TEXT("asset_read");
 for (const auto& Row : O.HandleResults) if (Row.Name == Name && Row.Handles.Num() == 1) return Row.Handles[0];
 return {};
}
FAssetDocumentPrimitiveInvocation Invocation(const FAssetDocumentMutationIntent& Intent, const FRetargetSourceNameTransaction& Tx,
 const FAssetDocumentOpaqueHandle& Handle, const FString& RuntimeName)
{
 FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = Primitive; I.Version = 1; I.Phase = Intent.Phase;
 I.RequestId = Tx.Token.RequestId; I.TransactionId = Tx.Token.TransactionId; I.RuntimeRevision = Tx.Token.RuntimeRevision;
 I.ValueArguments = NameValue(RuntimeName); I.HandleArguments = {{TEXT("sequence"), Handle}}; return I;
}
struct FObservedLifecycle
{
 int32 Modified = 0, PropertyChanged = 0, DirtyChanged = 0, MarkedDirty = 0;
 FDelegateHandle ModifiedHandle, PropertyHandle, DirtyHandle, MarkedHandle;
 explicit FObservedLifecycle(UAnimSequence& Target)
 {
  ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([this, &Target](UObject* O) { if (O == &Target) ++Modified; });
  PropertyHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this, &Target](UObject* O, FPropertyChangedEvent&)
   { if (O == &Target) ++PropertyChanged; });
  DirtyHandle = UPackage::PackageDirtyStateChangedEvent.AddLambda([this, &Target](UPackage* P) { if (P == Target.GetOutermost()) ++DirtyChanged; });
  MarkedHandle = UPackage::PackageMarkedDirtyEvent.AddLambda([this, &Target](UPackage* P, bool) { if (P == Target.GetOutermost()) ++MarkedDirty; });
 }
 ~FObservedLifecycle()
 {
  FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle); FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle);
  UPackage::PackageDirtyStateChangedEvent.Remove(DirtyHandle); UPackage::PackageMarkedDirtyEvent.Remove(MarkedHandle);
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEditorSequenceRetargetSourceNameFixedSetterTest,
 "UEAssetDoc.AnimationProvider.SequenceRetargetSourceName.EditorFixedSetter",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentEditorSequenceRetargetSourceNameFixedSetterTest::RunTest(const FString&)
{
 using namespace AssetDocumentEditorSequenceRetargetSourceNameTests;
 FFixture F; if (!TestTrue(TEXT("valid existing test-owned Sequence"), F.Setup())) return false;
 const FName Original = F.Sequence->RetargetSource;
 const auto Request = F.Request();
 for (const bool bInitiallyDirty : {false, true})
  for (const TCHAR* Name : {TEXT("NewSource_7"), TEXT("None"), TEXT("Default"), TEXT("retainedsource_9"), TEXT("Name_01"), TEXT("None_7")})
  {
   F.Sequence->GetOutermost()->SetDirtyFlag(bInitiallyDirty); const FRetainedState Before(F);
   auto Intent = F.Intent(Name); if (bInitiallyDirty) Intent.Phase = EAssetDocumentRuntimeRequestPhase::Repair;
   TSharedPtr<IAssetDocumentEditorSequenceRetargetSourceNameMutation> Group;
   if (!TestTrue(TEXT("fixed native name intent prepares"), PrepareAssetDocumentEditorSequenceRetargetSourceNameMutation(Request, Intent, Group).bValid)) return false;
   FRetargetSourceNameTransaction Tx; Tx.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision};
   Tx.ObservedTarget = F.Sequence.Get(); Tx.Original = Original; FResolver Handles; Handles.Token = Tx.Token;
   const auto Mutable = Resolve(*F.Sequence, EAssetDocumentHandleAccess::Mutable, Handles, Tx);
   const auto ReadOnly = Resolve(*F.Sequence, EAssetDocumentHandleAccess::ReadOnly, Handles, Tx);
   if (!TestTrue(TEXT("actual Editor-issued mutable/read leases"), Mutable.IsSet() && ReadOnly.IsSet())) return false;
   TestTrue(TEXT("capture binds exact transaction"), Group->OpenCapture(Tx.Token.TransactionId).bValid);
   auto I = Invocation(Intent, Tx, Mutable.GetValue(), FString(Name).ToUpper());
   TestTrue(TEXT("runtime case alias matches the bound native identity"), Group->ValidateInvocation(I, Handles).bValid);
   for (int32 Case = 0; Case < 10; ++Case)
   {
    auto Bad = I;
    if (Case == 0) Bad.HandleArguments[0].Handle.ProviderId = TEXT("animation");
    if (Case == 1) Bad.HandleArguments[0].Handle.Generation += 1;
    if (Case == 2) Bad.HandleArguments[0].Handle = ReadOnly.GetValue();
    if (Case == 3) Bad.HandleArguments[0].Name = TEXT("asset");
    if (Case == 4) { const auto Duplicate = Bad.HandleArguments[0]; Bad.HandleArguments.Add(Duplicate); }
    if (Case == 5) Bad.TransactionId = FGuid::NewGuid();
    if (Case == 6) Bad.RequestId = FGuid::NewGuid();
    if (Case == 7) Bad.RuntimeRevision += 1;
    if (Case == 8) Bad.ValueArguments = NameValue(TEXT("DifferentBoundName"));
    if (Case == 9) { Bad.ValueArguments.ObjectKeys.Add(TEXT("property")); Bad.ValueArguments.Elements.Add(String(TEXT("RetargetSource"))); }
    TestFalse(TEXT("authority/value drift is rejected before enlistment"), Group->Apply(Bad, Handles, Tx).bValid);
    TestEqual(TEXT("rejected call did not enlist"), Tx.Enlistments, 0);
    TestTrue(TEXT("rejected call preserves exact original name"), Group->IsOriginalCurrent());
    TestFalse(TEXT("rejected call never wrote"), Group->DidWrite()); Before.Check(*this, F);
   }
   FObservedLifecycle Events(*F.Sequence);
   if (!TestTrue(TEXT("fixed public helper applies bound name"), Group->Apply(I, Handles, Tx).bValid)) return false;
   const bool bNoop = FString(Name) == TEXT("retainedsource_9");
   TestEqual(TEXT("case-only identity is a true no-op"), Group->HasChanges(), !bNoop);
   TestEqual(TEXT("actual writer state"), Group->DidWrite(), !bNoop);
   TestEqual(TEXT("one parent enlistment only for a changed identity"), Tx.Enlistments, bNoop ? 0 : 1);
   if (!bNoop) TestTrue(TEXT("object/package preimage was armed before the writer"), Tx.bArmedBeforeWrite);
   const FName Expected = bNoop ? Original : FName(Name);
   TestTrue(TEXT("bound spelling and number read back exactly"), ExactName(F.Sequence->RetargetSource, Expected));
   TestTrue(TEXT("desired name reader preserves original display on no-op"), ExactName(Group->GetDesiredName(), Expected));
   TestTrue(TEXT("desired native state reads back"), Group->IsDesiredCurrent()); Before.Check(*this, F);
   TestFalse(TEXT("consumed forward cannot repeat"), Group->Apply(I, Handles, Tx).bValid);
   TestFalse(TEXT("inverse requires capture closure"), Group->Restore().bValid);
   Group->CloseCapture(); TestTrue(TEXT("same public helper inverses the typed original"), Group->Restore().bValid);
   TestTrue(TEXT("exact old display and number restored"), Group->IsOriginalCurrent()); Before.Check(*this, F);
   TestFalse(TEXT("inverse is terminal"), Group->Restore().bValid);
   TestEqual(TEXT("Never emitted no property lifecycle callback"), Events.PropertyChanged, 0);
   TestEqual(TEXT("fixed helper emitted no object-modified event"), Events.Modified, 0);
   TestEqual(TEXT("fixed helper emitted no package-dirty event"), Events.DirtyChanged + Events.MarkedDirty, 0);
  }
 F.Sequence->GetOutermost()->SetDirtyFlag(false);
 const auto Valid = F.Intent(TEXT("NextSource"));
 FString EmbeddedNull = TEXT("Source_Suffix"); EmbeddedNull[6] = 0;
 TestEqual(TEXT("embedded NUL fixture retains its full length"), EmbeddedNull.Len(), 13);
 TestEqual(TEXT("embedded NUL fixture contains the rejected character"), EmbeddedNull[6], TCHAR(0));
 FString TooLong; for (int32 I = 0; I < NAME_SIZE; ++I) TooLong.AppendChar(TEXT('x'));
 for (int32 Case = 0; Case < 12; ++Case)
 {
  auto Bad = Valid; auto R = Request;
  if (Case == 0) Bad.Intent = NameValue(TEXT(""));
  if (Case == 1) Bad.Intent = NameValue(EmbeddedNull);
  if (Case == 2) Bad.Intent = NameValue(TooLong);
  if (Case == 3) Bad.Intent.Elements[0] = FAssetDocumentRuntimeValueDto();
  if (Case == 4) { Bad.Intent.ObjectKeys.Add(TEXT("fields")); Bad.Intent.Elements.Add(FAssetDocumentRuntimeValueDto()); }
  if (Case == 5) Bad.Intent.ObjectKeys[0] = TEXT("RetargetSource");
  if (Case == 6) Bad.InputSchemaId += TEXT(".drift");
  if (Case == 7) Bad.Requirement.RequiredByRouteId = TEXT("foreign_route");
  if (Case == 8) Bad.Requirement.Version = 2;
  if (Case == 9) Bad.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  if (Case == 10) R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
  if (Case == 11) R.TargetAssetPath = F.Skeleton->GetPathName();
  const FRetainedState Before(F); TSharedPtr<IAssetDocumentEditorSequenceRetargetSourceNameMutation> Rejected;
  TestFalse(TEXT("closed input/context rejects invalid preparation"), PrepareAssetDocumentEditorSequenceRetargetSourceNameMutation(R, Bad, Rejected).bValid);
  TestFalse(TEXT("invalid preparation publishes no participant"), Rejected.IsValid());
  TestTrue(TEXT("invalid preparation preserves exact name"), ExactName(F.Sequence->RetargetSource, Original)); Before.Check(*this, F);
 }
 for (EObjectFlags Flag : {RF_ArchetypeObject, RF_ClassDefaultObject, RF_Transient, RF_NeedPostLoad})
 {
  F.Sequence->SetFlags(Flag); TSharedPtr<IAssetDocumentEditorSequenceRetargetSourceNameMutation> Rejected;
  TestFalse(TEXT("archetype/template/loading target cannot enter fixed writer"), PrepareAssetDocumentEditorSequenceRetargetSourceNameMutation(Request, Valid, Rejected).bValid);
  F.Sequence->ClearFlags(Flag);
  TestFalse(TEXT("unsafe target publishes no participant"), Rejected.IsValid());
  TestTrue(TEXT("unsafe target rejection preserves original name"), ExactName(F.Sequence->RetargetSource, Original));
 }
 {
  TSharedPtr<IAssetDocumentEditorSequenceRetargetSourceNameMutation> Group;
  if (!TestTrue(TEXT("parent-failure participant prepares"), PrepareAssetDocumentEditorSequenceRetargetSourceNameMutation(Request, Valid, Group).bValid)) return false;
  FRetargetSourceNameTransaction Tx; Tx.Token = {Request.RequestId, FGuid::NewGuid(), Request.ExpectedRuntimeRevision};
  Tx.ObservedTarget = F.Sequence.Get(); Tx.Original = Original; Tx.bRejectEnlistment = true;
  FResolver Handles; Handles.Token = Tx.Token; const auto Handle = Resolve(*F.Sequence, EAssetDocumentHandleAccess::Mutable, Handles, Tx);
  if (!TestTrue(TEXT("parent-failure actual mutable handle"), Handle.IsSet())) return false;
  TestTrue(TEXT("parent-failure capture opens"), Group->OpenCapture(Tx.Token.TransactionId).bValid);
  const FRetainedState Before(F); const auto I = Invocation(Valid, Tx, Handle.GetValue(), TEXT("NextSource"));
  TestFalse(TEXT("failed parent footprint blocks the writer"), Group->Apply(I, Handles, Tx).bValid);
  TestTrue(TEXT("parent failure observed full original before write"), Tx.bArmedBeforeWrite);
  TestFalse(TEXT("parent failure did not publish a writer"), Group->DidWrite());
  TestTrue(TEXT("parent failure preserves exact name"), Group->IsOriginalCurrent()); Before.Check(*this, F);
  Group->CloseCapture(); TestTrue(TEXT("zero-write inverse is harmless"), Group->Restore().bValid);
 }
 return !HasAnyErrors();
}
#endif
