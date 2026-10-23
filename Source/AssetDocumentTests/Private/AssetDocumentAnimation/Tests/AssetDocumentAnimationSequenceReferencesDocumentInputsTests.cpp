// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "Provider/Sequence/AssetDocumentAnimationSequenceReferencesDocumentInputs.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceAssetState.h"
#include "Tests/AssetDocumentAnimationSequenceReferencesDocumentTestTypes.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace AssetDocumentSequenceReferencesDocumentTests
{
using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;
FValue String(const FString& S) { FValue V; V.Kind = EKind::String; V.StringValue = S; return V; }
FValue Object(TArray<FString> Keys = {}, TArray<FValue> Values = {})
{ FValue V; V.Kind = EKind::Object; V.ObjectKeys = MoveTemp(Keys); V.Elements = MoveTemp(Values); return V; }
FValue Array(TArray<FValue> Values = {}) { FValue V; V.Kind = EKind::Array; V.Elements = MoveTemp(Values); return V; }
FValue Boolean(bool B) { FValue V; V.Kind = EKind::Boolean; V.BooleanValue = B; return V; }
FValue Snapshot(bool Present, FValue Name, FValue References, bool ExplicitClear, TArray<FValue> Defaults, FValue Definitions = Object())
{
 return Object({TEXT("region_present"), TEXT("supplied_name"), TEXT("reference_fragments"), TEXT("explicit_asset_clear"), TEXT("native_default_fields"), TEXT("definitions")},
  {Boolean(Present), MoveTemp(Name), MoveTemp(References), Boolean(ExplicitClear), Array(MoveTemp(Defaults)), MoveTemp(Definitions)});
}
FValue Reference(const FString& Path, FValue Class = FValue())
{ return Object({TEXT("Kind"), TEXT("Path"), TEXT("Class")}, {String(TEXT("aSsEtReF")), String(Path), MoveTemp(Class)}); }
struct FAssets
{
 FString Root = TEXT("/Game/AssetDocumentSequenceReferencesInputs/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<UAssetDocumentSequenceReferencesTestSkeleton> Skeleton;
 TStrongObjectPtr<UAssetDocumentSequenceReferencesTestMesh> Mesh;
 TStrongObjectPtr<UAnimSequence> Sequence;
 bool Setup()
 {
  // Raw construction is confined to new test-owned authored fixtures.
  Skeleton.Reset(NewObject<UAssetDocumentSequenceReferencesTestSkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  Mesh.Reset(NewObject<UAssetDocumentSequenceReferencesTestMesh>(CreatePackage(*(Root + TEXT("/Mesh"))), TEXT("Mesh"), RF_Public | RF_Standalone));
  Mesh->SetSkeleton(Skeleton.Get());
  { FReferenceSkeletonModifier Bones(Mesh->GetRefSkeleton(), Skeleton.Get());
  Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
  Bones.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(20., 0., 0.))); }
  if (!Skeleton->MergeAllBonesToBoneTree(Mesh.Get(), false)) return false;
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone));
  Sequence->SetSkeleton(Skeleton.Get()); Sequence->RetargetSource = TEXT("RetainedOriginal");
  auto& C = Sequence->GetController(); C.InitializeModel(); C.SetFrameRate(FFrameRate(24, 1), false); C.SetNumberOfFrames(FFrameNumber(12), false); C.NotifyPopulated();
  return AssetDocumentSequenceRetargetSource::CanonicalSequence(Sequence.Get());
 }
 FAssetDocumentRuntimeRequestDto Request() const
 {
  FAssetDocumentRuntimeRequestDto R; R.RequestId = FGuid::NewGuid(); R.ExpectedRuntimeRevision = 93;
  R.Operation = EAssetDocumentRuntimeOperation::Apply; R.TargetAssetPath = Sequence->GetPathName(); R.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence"); return R;
 }
 ~FAssets()
 {
  if (Sequence.IsValid()) Sequence->WaitOnExistingCompression();
  for (UObject* O : {static_cast<UObject*>(Sequence.Get()), static_cast<UObject*>(Mesh.Get()), static_cast<UObject*>(Skeleton.Get())})
   if (IsValid(O)) { auto* P = O->GetOutermost(); O->ClearFlags(RF_Public | RF_Standalone); O->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional); O->MarkAsGarbage(); P->SetDirtyFlag(false); P->MarkAsGarbage(); }
 }
};
class FHandles final : public IAssetDocumentHandleResolver
{
 struct FEntry { FAssetDocumentOpaqueHandle H; TUniquePtr<IAssetDocumentNativeHandlePayload> P; };
 TArray<FEntry> Entries;
 class FLease final : public IAssetDocumentVerifiedHandleLease
 {
  FAssetDocumentOpaqueHandle H; IAssetDocumentNativeHandlePayload& P;
 public:
  FLease(FAssetDocumentOpaqueHandle In, IAssetDocumentNativeHandlePayload& Payload) : H(In), P(Payload) {}
  const FAssetDocumentOpaqueHandle& GetHandle() const override { return H; }
  IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return P; }
 };
public:
 FAssetDocumentHandleValidationContext Context;
 FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access, TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
 {
  auto& E = Entries.AddDefaulted_GetRef(); E.H.RequestId = Context.RequestId; E.H.TransactionId = Context.TransactionId;
  E.H.RuntimeRevision = Context.RuntimeRevision; E.H.ProviderId = TEXT("editor"); E.H.Slot = Entries.Num(); E.H.Generation = 1; E.H.Kind = Kind; E.H.Access = Access; E.P = MoveTemp(Payload);
  FAssetDocumentIssuedHandleResult R; R.Handle = E.H; return R;
 }
 FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& H, const FAssetDocumentHandleValidationContext& C) override
 {
  FAssetDocumentVerifiedLeaseResult R; if (!ValidateAssetDocumentHandleMetadata(H, C).bValid) return R;
  const int32 I = int32(H.Slot) - 1; if (!Entries.IsValidIndex(I) || Entries[I].H.Generation != H.Generation || !Entries[I].P->IsAlive()) return R;
  R.Lease = MakeUnique<FLease>(H, *Entries[I].P); return R;
 }
 FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override
 { FAssetDocumentHandleInvalidationResult R; R.InvalidatedHandleCount = Entries.Num(); Entries.Reset(); return R; }
};
class FReadonlyContext final : public IAssetDocumentNativeRequestContext
{
 FAssetDocumentRequestContextMetadata Metadata;
 TSharedRef<IAssetDocumentPrimitiveProvider> Editor = CreateAssetDocumentEditorPrimitiveProvider();
public:
 FHandles Handles; int32 Invoked = 0; bool ForeignReturnedRevision = false;
 explicit FReadonlyContext(const FAssetDocumentRuntimeRequestDto& R)
 {
  Metadata.RequestId = R.RequestId; Metadata.PinnedRuntimeRevision = R.ExpectedRuntimeRevision; Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
  Handles.Context.RequestId = R.RequestId; Handles.Context.TransactionId = FGuid::NewGuid(); Handles.Context.RuntimeRevision = R.ExpectedRuntimeRevision;
  Handles.Context.ProviderId = TEXT("editor"); Handles.Context.Phase = Metadata.Phase;
 }
 const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
 FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override
 { FAssetDocumentPrimitiveRegistrySnapshot S; S.Descriptors.Append(Editor->DescribePrimitives()); return S; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& I) override
 {
  ++Invoked; auto R = Editor->Invoke(I, Handles, nullptr);
  if (ForeignReturnedRevision) for (auto& Group : R.HandleResults) for (auto& H : Group.Handles) ++H.RuntimeRevision;
  return R;
 }
};
class FInverse final : public IAssetDocumentRollbackAction
{
 int32& Calls;
public:
 explicit FInverse(int32& In) : Calls(In) {}
 FStringView GetRollbackComponentId() const override { return TEXT("SequenceReferencesInputFixture.OriginalInverse"); }
 FAssetDocumentContractValidationResult Rollback(FStringView) override
 { ++Calls; FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
};
class FRecordingOwner final : public IAssetDocumentAnimationRequestProvider
{
public:
 int32 Prepared = 0, Opened = 0, Closed = 0, Inverted = 0; TArray<FAssetDocumentMutationIntent> Recorded;
 FAssetDocumentContractValidationResult BindRequest(const FAssetDocumentRuntimeRequestDto&) override { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
 void CloseRequest() override {}
 FString GetProviderId() const override { return TEXT("animation"); }
 TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override { return {}; }
 FAssetDocumentProviderAvailability InspectAvailability() const override { return {TEXT("animation"), EAssetDocumentProviderAvailabilityState::Available}; }
 TUniquePtr<FAssetDocumentAnimationStagedReadLease> BindStagedRead(UAnimMontage&, const FString&, uint64, TArray<FAssetDocumentRuntimeDiagnostic>&) override { return {}; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&, IAssetDocumentAtomicTransaction*) override { return {}; }
 FAssetDocumentContractValidationResult Prepare(const FAssetDocumentRuntimeRequestDto&, TConstArrayView<FAssetDocumentMutationIntent> Intents, FAssetDocumentPreparedMutationBatch& B) override
 {
  ++Prepared; Recorded.Reset(); Recorded.Append(Intents);
  for (const auto& I : Intents)
  {
   auto& C = B.Contributions.AddDefaulted_GetRef(); C.StepIndex = I.StepIndex; C.InitialAction = MakeShared<FInverse>(Inverted);
   C.ValidateInvocation = [](const auto&, auto&) { FAssetDocumentContractValidationResult R; R.bValid = true; return R; };
  }
  B.OpenMutationCapture = [this](const FGuid&, TArray<FAssetDocumentRuntimeDiagnostic>&) -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
  { ++Opened; return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([this] { ++Closed; }); };
  FAssetDocumentContractValidationResult R; R.bValid = true; return R;
 }
};
FAssetDocumentContractValidationResult Prepare(const FAssetDocumentRuntimeRequestDto& R, FRecordingOwner& Owner, FReadonlyContext& Context,
 const FValue& V, TArray<FAssetDocumentMutationIntent>& Actual, FAssetDocumentPreparedMutationBatch& Batch)
{
 return AssetDocumentSequenceReferencesDocumentInputs::Prepare(R, Owner, Context, Context.Handles.Context.TransactionId,
  V, 4, 2, EAssetDocumentRuntimeRequestPhase::Apply, {}, Actual, Batch);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceReferencesDocumentMaterializationTest,
 "UEAssetDoc.AnimationProvider.SequenceReferencesDocumentInputs.Materialization", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceReferencesDocumentMaterializationTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceReferencesDocumentTests;
 FAssets A; if (!TestTrue(TEXT("test-owned canonical target with legitimate Skeleton/Mesh subtypes"), A.Setup())) return false;
 const auto Request = A.Request(); FReadonlyContext Context(Request); const auto* CDO = Cast<UAnimSequence>(UAnimSequence::StaticClass()->GetDefaultObject(false));
 if (!TestNotNull(TEXT("actual already-existing exact AnimSequence CDO"), CDO)) return false;
 TestTrue(TEXT("actual CDO selects native None and empty soft asset"), CDO->RetargetSource.IsNone() && CDO->GetRetargetSourceAsset().IsNull());
 for (bool Present : {false, true})
 {
  FRecordingOwner Owner; TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch;
  const auto V = Snapshot(Present, FValue(), Object(), false, {String(TEXT("RetargetSource")), String(TEXT("RetargetSourceAsset"))});
  TestTrue(TEXT("missing and empty region materialize actual CDO selections"), Prepare(Request, Owner, Context, V, Actual, Batch).bValid);
  if (!TestEqual(TEXT("exact original two Intents"), Actual.Num(), 2)) return false;
  TestEqual(TEXT("caller's asset-first StepIndex retained"), Actual[0].StepIndex, 2); TestEqual(TEXT("asset v2 unchanged"), Actual[0].Requirement.Version, 2u);
  TestEqual(TEXT("actual default asset clear"), Actual[0].Intent.Elements[0].StringValue, FString(TEXT("clear")));
  TestEqual(TEXT("actual default asset path null"), Actual[0].Intent.Elements[1].Kind, EKind::Null);
  TestEqual(TEXT("actual default v1 nonempty None"), Actual[1].Intent.Elements[0].StringValue, FString(TEXT("None")));
  TestEqual(TEXT("same owner prepared exactly once"), Owner.Prepared, 1); TestEqual(TEXT("defaults need no loader"), Context.Invoked, 0);
  TestEqual(TEXT("projection never sets the target name"), A.Sequence->RetargetSource, FName(TEXT("RetainedOriginal")));
 }
 for (bool NameDefault : {false, true})
 {
  FRecordingOwner Owner; TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch;
  const auto V = NameDefault
   ? Snapshot(true, FValue(), Object({TEXT("RetargetSourceAsset")}, {Reference(A.Mesh->GetPathName())}), false, {String(TEXT("RetargetSource"))})
   : Snapshot(true, String(TEXT("Default")), Object(), false, {String(TEXT("RetargetSourceAsset"))});
  TestTrue(TEXT("individual omission reads selected actual defaults"), Prepare(Request, Owner, Context, V, Actual, Batch).bValid);
  if (!TestEqual(TEXT("individual default still prepares complete pair"), Actual.Num(), 2)) return false;
  TestEqual(TEXT("only omitted name materializes None"), Actual[1].Intent.Elements[0].StringValue, FString(NameDefault ? TEXT("None") : TEXT("Default")));
  TestEqual(TEXT("only omitted asset materializes clear"), Actual[0].Intent.Elements[0].StringValue, FString(NameDefault ? TEXT("set") : TEXT("clear")));
 }
 for (const FString Name : {FString(), FString(TEXT("None")), FString(TEXT("Default")), FString(TEXT("未知名称"))})
 {
  FRecordingOwner Owner; TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch;
  TestTrue(TEXT("literal/empty name with explicit null clear accepted"), Prepare(Request, Owner, Context, Snapshot(true, String(Name), Object(), true, {}), Actual, Batch).bValid);
  if (!TestEqual(TEXT("literal produces two native Intents"), Actual.Num(), 2)) return false;
  TestEqual(TEXT("only native None spellings become required v1 literal"), Actual[1].Intent.Elements[0].StringValue, Name.IsEmpty() || Name == TEXT("None") ? FString(TEXT("None")) : Name);
 }
 FValue Number; Number.Kind = EKind::Number; Number.NumberValue = 7.;
 for (const FValue Class : {FValue(), String(TEXT(" ")), Number, String(TEXT("USkeletalMesh")), String(TEXT("/Script/Engine.SkeletalMesh")), String(TEXT("AssetDocumentSequenceReferencesTestMesh"))})
 {
  FRecordingOwner Owner; TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch;
  const auto Definitions = Object({TEXT(" exact "), TEXT("terminal")}, {
   Object({TEXT("Kind"), TEXT("Id"), TEXT("extra")}, {String(TEXT("dEfInItIoNrEf")), String(TEXT("terminal")), String(TEXT("retained"))}),
   Reference(A.Root + TEXT("/Mesh"), Class)});
  const auto References = Object({TEXT("Skeleton"), TEXT("RetargetSourceAsset")}, {
   Reference(A.Skeleton->GetPathName(), String(TEXT("Skeleton"))), Object({TEXT("Kind"), TEXT("Id")}, {String(TEXT("DefinitionRef")), String(TEXT(" exact "))})});
  TestTrue(TEXT("original package/Class aliases, inactive Class and exact Definition chain"), Prepare(Request, Owner, Context, Snapshot(true, String(TEXT("Default")), References, false, {}, Definitions), Actual, Batch).bValid);
  if (!TestEqual(TEXT("resolved pair is complete"), Actual.Num(), 2)) return false;
  TestEqual(TEXT("actual canonical Mesh path reaches unchanged v2"), Actual[0].Intent.Elements[1].StringValue, A.Mesh->GetPathName());
  TestEqual(TEXT("Default remains literal"), Actual[1].Intent.Elements[0].StringValue, FString(TEXT("Default")));
  TestEqual(TEXT("only same owner prepares once"), Owner.Prepared, 1); TestFalse(TEXT("no second package finalizer"), Batch.PackageFinalizer.IsValid());
 }
 {
  FRecordingOwner Owner; TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch;
  const auto Ref = [](const TCHAR* Id) { return Object({TEXT("Kind"), TEXT("Id")}, {String(TEXT("DefinitionRef")), String(Id)}); };
  auto Mesh = Reference(A.Mesh->GetPathName());
  Mesh.ObjectKeys.Append({TEXT("extra"), TEXT("Extra")}); Mesh.Elements.Append({String(TEXT("first")), String(TEXT("second"))});
  const auto Definitions = Object({TEXT("A"), TEXT("a")}, {Ref(TEXT("a")), Mesh});
  const auto V = Snapshot(true, String(TEXT("Default")), Object({TEXT("RetargetSourceAsset")}, {Ref(TEXT("A"))}), false, {}, Definitions);
  TestTrue(TEXT("original JSON validator preserves case-distinct keys and arbitrary extras"), ValidateAssetDocumentRuntimeValue(V).bValid);
  TestTrue(TEXT("exact A to a Definition chain is not a cycle"), Prepare(Request, Owner, Context, V, Actual, Batch).bValid);
  if (!TestEqual(TEXT("case-distinct Id chain produces original pair"), Actual.Num(), 2)) return false;
  TestEqual(TEXT("case-distinct chain resolves actual Mesh"), Actual[0].Intent.Elements[1].StringValue, A.Mesh->GetPathName());
  TestEqual(TEXT("case-distinct chain prepares same owner once"), Owner.Prepared, 1);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceReferencesDocumentRejectionTest,
 "UEAssetDoc.AnimationProvider.SequenceReferencesDocumentInputs.Rejection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceReferencesDocumentRejectionTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceReferencesDocumentTests;
 FAssets A; if (!TestTrue(TEXT("new rejection inputs"), A.Setup())) return false;
 const auto Request = A.Request(); FReadonlyContext Context(Request);
 const auto Good = Snapshot(true, String(TEXT("Default")), Object({TEXT("RetargetSourceAsset")}, {Reference(A.Mesh->GetPathName())}), false, {});
 TArray<FValue> Bad = {FValue(), Array(), Object(), Snapshot(false, String(TEXT("Default")), Object(), true, {}),
  Snapshot(true, FValue(), Object(), true, {}), Snapshot(true, String(TEXT("Default")), Object(), true, {String(TEXT("RetargetSourceAsset"))}),
  Snapshot(true, String(TEXT("Default")), Object({TEXT("RetargetSourceAsset")}, {Reference(A.Mesh->GetPathName())}), true, {}),
  Snapshot(true, String(TEXT("Default")), Object({TEXT("Skeleton")}, {FValue()}), true, {}),
  Snapshot(true, String(TEXT("Default")), Object({TEXT("RetargetSourceAsset")}, {FValue()}), false, {}),
  Snapshot(true, String(TEXT("Default")), Object({TEXT("RetargetSourceAsset")}, {Reference(TEXT(" "))}), false, {}),
  Snapshot(true, String(TEXT("Default")), Object({TEXT("Skeleton")}, {Object({TEXT("Kind")}, {String(TEXT("EmbeddedObject"))})}), true, {}),
  Snapshot(true, String(TEXT("Default")), Object({TEXT("Skeleton")}, {Object({TEXT("Kind"), TEXT("Id")}, {String(TEXT("DefinitionRef")), String(TEXT("missing"))})}), true, {}),
  Snapshot(true, String(TEXT("Default")), Object({TEXT("Skeleton")}, {Object({TEXT("Kind"), TEXT("Id")}, {String(TEXT("DefinitionRef")), String(TEXT(" exact "))})}), true, {},
   Object({TEXT(" exact ")}, {Object({TEXT("Kind"), TEXT("Id")}, {String(TEXT("DefinitionRef")), String(TEXT(" exact "))})})),
  Snapshot(true, String(TEXT("Default")), Object({TEXT("Skeleton")}, {Object({TEXT("Kind"), TEXT("Id")}, {String(TEXT("DefinitionRef")), String(TEXT(" EXACT "))})}), true, {},
   Object({TEXT(" exact ")}, {Reference(A.Skeleton->GetPathName())})),
  Snapshot(true, String(TEXT("Default")), Object({TEXT("RetargetSourceAsset")}, {Object({TEXT("Kind"), TEXT("Id")}, {String(TEXT("DefinitionRef")), String(TEXT("A"))})}), false, {},
   Object({TEXT("A"), TEXT("A")}, {Reference(A.Mesh->GetPathName()), Reference(A.Mesh->GetPathName())})),
  Snapshot(true, String(TEXT("Default")), Object({TEXT("RetargetSourceAsset")}, {Object({TEXT("Kind"), TEXT("Id")}, {String(TEXT("DefinitionRef")), String(TEXT("A"))})}), false, {},
   Object({TEXT("A"), TEXT("a")}, {
    Object({TEXT("Kind"), TEXT("Id")}, {String(TEXT("DefinitionRef")), String(TEXT("a"))}),
    Object({TEXT("Kind"), TEXT("Id")}, {String(TEXT("DefinitionRef")), String(TEXT("A"))})}))};
 TestFalse(TEXT("original JSON validator still rejects truly repeated keys"), ValidateAssetDocumentRuntimeValue(Bad[Bad.Num() - 2]).bValid);
 TestTrue(TEXT("case-distinct cycle is a valid JSON shape before exact graph rejection"), ValidateAssetDocumentRuntimeValue(Bad.Last()).bValid);
 for (const auto& V : Bad)
 {
  FRecordingOwner Owner; TArray<FAssetDocumentMutationIntent> Actual; Actual.AddDefaulted(); FAssetDocumentPreparedMutationBatch Batch;
  const int32 Before = Context.Invoked; TestFalse(TEXT("invalid closed shape or reference chain refuses"), Prepare(Request, Owner, Context, V, Actual, Batch).bValid);
  TestEqual(TEXT("invalid grammar never loads"), Context.Invoked, Before); TestEqual(TEXT("invalid grammar never prepares owner"), Owner.Prepared, 0);
  TestTrue(TEXT("failure clears output Intents and batch"), Actual.IsEmpty() && Batch.Contributions.IsEmpty() && !Batch.OpenMutationCapture);
 }
 for (bool ForeignRequest : {true, false})
 {
  auto Foreign = Request; if (ForeignRequest) Foreign.RequestId = FGuid::NewGuid(); else ++Foreign.ExpectedRuntimeRevision;
  FRecordingOwner Owner; TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch;
  const int32 Before = Context.Invoked; TestFalse(TEXT("original context identity must match"), Prepare(Foreign, Owner, Context, Good, Actual, Batch).bValid);
  TestEqual(TEXT("request/revision mismatch performs zero resolver calls"), Context.Invoked, Before); TestEqual(TEXT("foreign context never prepares"), Owner.Prepared, 0);
 }
 {
  TStrongObjectPtr<UAssetDocumentSequenceReferencesTestSequence> Derived(NewObject<UAssetDocumentSequenceReferencesTestSequence>(
   CreatePackage(*(A.Root + TEXT("/DerivedSequence"))), TEXT("DerivedSequence"), RF_Public | RF_Standalone));
  auto Foreign = Request; Foreign.TargetAssetPath = Derived->GetPathName(); FRecordingOwner Owner;
  TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch; const int32 Before = Context.Invoked;
  TestFalse(TEXT("reference subtype admission never broadens exact UAnimSequence target"), Prepare(Foreign, Owner, Context, Good, Actual, Batch).bValid);
  TestEqual(TEXT("target subtype refuses before loaders"), Context.Invoked, Before); TestEqual(TEXT("target subtype never prepares owner"), Owner.Prepared, 0);
  auto* Package = Derived->GetOutermost(); Derived->ClearFlags(RF_Public | RF_Standalone); Derived->MarkAsGarbage(); Package->SetDirtyFlag(false); Package->MarkAsGarbage();
 }
 {
  FRecordingOwner Owner; TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch; Context.ForeignReturnedRevision = true;
  TestFalse(TEXT("opaque readonly result with foreign revision rejected"), Prepare(Request, Owner, Context, Good, Actual, Batch).bValid);
  TestEqual(TEXT("invalid opaque result cannot reach owner"), Owner.Prepared, 0); Context.ForeignReturnedRevision = false;
 }
 {
  FRecordingOwner Owner; TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch;
  const auto V = Snapshot(true, String(TEXT("Default")), Object({TEXT("RetargetSourceAsset")}, {Reference(A.Mesh->GetPathName(), String(TEXT("/Script/Engine.AnimSequence")))}), false, {});
  TestFalse(TEXT("a real canonical but mismatched active Class is not ignored"), Prepare(Request, Owner, Context, V, Actual, Batch).bValid); TestEqual(TEXT("Class mismatch never prepares"), Owner.Prepared, 0);
 }
 {
  auto* Other = NewObject<UAssetDocumentSequenceReferencesTestSkeleton>(CreatePackage(*(A.Root + TEXT("/OtherSkeleton"))), TEXT("OtherSkeleton"), RF_Public | RF_Standalone);
  FRecordingOwner Owner; TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch;
  TestFalse(TEXT("explicit readonly Skeleton mismatch refuses, no replacement"), Prepare(Request, Owner, Context, Snapshot(true, String(TEXT("Default")), Object({TEXT("Skeleton")}, {Reference(Other->GetPathName())}), true, {}), Actual, Batch).bValid);
  TestEqual(TEXT("Skeleton mismatch never prepares owner"), Owner.Prepared, 0); TestEqual(TEXT("Skeleton identity remains original"), A.Sequence->GetSkeleton(), static_cast<USkeleton*>(A.Skeleton.Get()));
  Other->ClearFlags(RF_Public | RF_Standalone); Other->GetOutermost()->SetDirtyFlag(false); Other->MarkAsGarbage();
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceReferencesDocumentReadonlyGuardsTest,
 "UEAssetDoc.AnimationProvider.SequenceReferencesDocumentInputs.ReadonlyGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceReferencesDocumentReadonlyGuardsTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceReferencesDocumentTests;
 for (bool AfterCapture : {false, true})
 {
  FAssets A; if (!TestTrue(TEXT("new readonly guard inputs"), A.Setup())) return false;
  const auto Request = A.Request(); FReadonlyContext Context(Request); FRecordingOwner Owner; TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch;
  const auto V = Snapshot(true, String(TEXT("Default")), Object({TEXT("Skeleton"), TEXT("RetargetSourceAsset")}, {
   Reference(A.Skeleton->GetPathName()), Reference(A.Mesh->GetPathName(), String(UAssetDocumentSequenceReferencesTestMesh::StaticClass()->GetPathName()))}), false, {});
  if (!TestTrue(TEXT("original guard batch prepares once"), Prepare(Request, Owner, Context, V, Actual, Batch).bValid)) return false;
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics; TUniquePtr<FAssetDocumentCoreMutationCaptureLease> Lease;
  if (AfterCapture) { Lease = Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics); if (!TestTrue(TEXT("original opener preserved"), Lease && Lease->IsValid())) return false; Lease->Close(); }
  // External drift is confined to the test-owned reflected class. Restore its
  // test configuration at scope end; no fixture supplies a product inverse.
  auto* Class = UAssetDocumentSequenceReferencesTestMesh::StaticClass(); TGuardValue<EClassFlags> Flags(Class->ClassFlags, Class->ClassFlags | CLASS_Deprecated);
  if (!AfterCapture) TestFalse(TEXT("Class drift refuses before original capture"), bool(Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics)));
  FAssetDocumentPrimitiveInvocation I;
  TestFalse(TEXT("Class drift refuses contribution validation"), Batch.Contributions[0].ValidateInvocation(I, Context.Handles).bValid);
  const auto Reverted = Batch.Contributions[0].InitialAction->Rollback(TEXT("fixture-journal"));
  TestEqual(TEXT("zero-write opener abort keeps original inverse semantics"), Reverted.bValid, !AfterCapture);
  TestEqual(TEXT("readonly guard delegates original inverse only for safe zero-write abort"), Owner.Inverted, AfterCapture ? 0 : 1);
  TestEqual(TEXT("capture not replaced or duplicated"), Owner.Opened, AfterCapture ? 1 : 0);
 }
 for (bool MeshDrift : {false, true})
 {
  FAssets A; if (!TestTrue(TEXT("new reference content drift inputs"), A.Setup())) return false;
  const auto Request = A.Request(); FReadonlyContext Context(Request); FRecordingOwner Owner; TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch;
  const auto V = Snapshot(true, String(TEXT("Default")), Object({TEXT("RetargetSourceAsset")}, {Reference(A.Mesh->GetPathName())}), false, {});
  if (!TestTrue(TEXT("readonly reference snapshot prepares"), Prepare(Request, Owner, Context, V, Actual, Batch).bValid)) return false;
  if (MeshDrift) { FReferenceSkeletonModifier Bones(A.Mesh->GetRefSkeleton(), A.Skeleton.Get()); Bones.UpdateRefPoseTransform(1, FTransform(FVector(21., 0., 0.))); }
  else A.Skeleton->GetOutermost()->SetDirtyFlag(!A.Skeleton->GetOutermost()->IsDirty());
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  TestFalse(TEXT("readonly Skeleton/Mesh drift refuses original opener"), bool(Batch.OpenMutationCapture(FGuid::NewGuid(), Diagnostics))); TestEqual(TEXT("no original capture after drift"), Owner.Opened, 0);
 }
 return !HasAnyErrors();
}
#endif
