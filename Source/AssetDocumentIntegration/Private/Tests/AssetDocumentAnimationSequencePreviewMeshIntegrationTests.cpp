// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentMutationComposition.h"
#include "AssetDocumentAnimationMutationPreparation.h"
#include "AssetDocumentEditorMutationPreparation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

// Test-only native caller of the existing leaf. It does not register a Sequence
// Profile, bind Montage staged reads to Sequence, or bypass the Python apply guard.
// Transaction, capture, physical footprint projection and inverse all come from
// the actual Core and production FAssetDocumentMutationComposition.
namespace AssetDocumentSequencePreviewIntegrationTests
{
constexpr const TCHAR* Primitive = TEXT("animation.asset.preview_mesh.set.v1");
USkeletalMesh* Read(const UAnimationAsset& Asset) { return Asset.GetPreviewMesh(); }
FString PackageFile(const FString& Path)
{ return FPackageName::LongPackageNameToFilename(FPackageName::ObjectPathToPackageName(Path), FPackageName::GetAssetPackageExtension()); }
TArray<uint8> Bytes(const FString& Path)
{ TArray<uint8> Result; FFileHelper::LoadFileToArray(Result, *Path); return Result; }
bool Save(UObject* Asset)
{
 FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
 const FString Path = PackageFile(Asset->GetPathName());
 IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
 return UPackage::SavePackage(Asset->GetOutermost(), Asset, *Path, Args);
}
struct FAssets
{
 FString Root = TEXT("/Game/AssetDocumentSequencePreview/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 FString DiskRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AssetDocumentSequencePreview") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
 TStrongObjectPtr<USkeleton> Skeleton;
 TStrongObjectPtr<USkeletalMesh> Old, Desired;
 TStrongObjectPtr<UAnimSequence> Sequence;
 bool bKeep = false;
 bool Setup()
 {
  if (!IFileManager::Get().MakeDirectory(*(DiskRoot / TEXT("Installed")), true)) return false;
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  {
   FReferenceSkeletonModifier Modifier(Skeleton.Get());
   Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
  }
  auto Mesh = [&](const TCHAR* Name)
  {
   auto* Object = NewObject<USkeletalMesh>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone);
   Object->SetSkeleton(Skeleton.Get());
   FReferenceSkeletonModifier Bones(Object->GetRefSkeleton(), Skeleton.Get());
   Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
   return Object;
  };
  Old.Reset(Mesh(TEXT("Old"))); Desired.Reset(Mesh(TEXT("Desired")));
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  Sequence->SetSkeleton(Skeleton.Get()); Sequence->SetPreviewMesh(Old.Get(), false);
  Sequence->RateScale = 1.25f; Sequence->bLoop = true; Sequence->bForceRootLock = true;
  auto& Controller = Sequence->GetController();
  Controller.InitializeModel(); Controller.SetFrameRate(FFrameRate(24, 1), false); Controller.SetNumberOfFrames(FFrameNumber(12), false);
  if (!Controller.AddBoneCurve(TEXT("root"), false)) return false;
  TArray<FVector3f> Positions, Scales; TArray<FQuat4f> Rotations;
  Positions.Init(FVector3f::ZeroVector, 13); Scales.Init(FVector3f::OneVector, 13); Rotations.Init(FQuat4f::Identity, 13);
  if (!Controller.SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false)) return false;
  const FAnimationCurveIdentifier Curve(FName(TEXT("RetainedPreviewCurve")), ERawCurveTrackTypes::RCT_Float);
  FRichCurveKey A(0.0f, 1.0f), B(0.5f, -3.0f);
  A.InterpMode = RCIM_Cubic; A.TangentMode = RCTM_Break; A.TangentWeightMode = RCTWM_WeightedBoth;
  A.ArriveTangentWeight = 0.2f; A.LeaveTangentWeight = 0.4f;
  Controller.AddCurve(Curve, AACF_Editable, false); Controller.SetCurveKeys(Curve, {A, B}, false); Controller.NotifyPopulated();
  Sequence->WaitOnExistingCompression(); // Finish only this authored fixture before freezing the mutation preimage.
  Sequence->AnimNotifyTracks.SetNum(1); Sequence->AnimNotifyTracks[0].TrackName = TEXT("RetainedTrack");
  auto& Notify = Sequence->Notifies.AddDefaulted_GetRef(); Notify.NotifyName = TEXT("RetainedPoint"); Notify.Guid = FGuid::NewGuid(); Notify.TrackIndex = 0;
  auto& Marker = Sequence->AuthoredSyncMarkers.AddDefaulted_GetRef(); Marker.MarkerName = TEXT("RetainedMarker"); Marker.Time = 0.25f; Marker.TrackIndex = 0; Marker.Guid = FGuid::NewGuid();
  for (UObject* Object : {static_cast<UObject*>(Skeleton.Get()), static_cast<UObject*>(Old.Get()), static_cast<UObject*>(Desired.Get()), static_cast<UObject*>(Sequence.Get())})
  { FAssetRegistryModule::AssetCreated(Object); if (!Save(Object)) return false; }
  return true;
 }
 ~FAssets()
 {
  if (Sequence.IsValid()) Sequence->WaitOnExistingCompression(); // Do not garbage-mark an asset still owned by a compiling job.
  if (bKeep) return;
  for (UObject* Object : {static_cast<UObject*>(Sequence.Get()), static_cast<UObject*>(Old.Get()), static_cast<UObject*>(Desired.Get()), static_cast<UObject*>(Skeleton.Get())})
   if (IsValid(Object))
   {
    UPackage* Package = Object->GetOutermost(); FAssetRegistryModule::AssetDeleted(Object);
    Object->ClearFlags(RF_Public | RF_Standalone); Object->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
    Object->MarkAsGarbage(); Package->SetDirtyFlag(false); Package->MarkAsGarbage();
   }
  IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
  IFileManager::Get().DeleteDirectory(*DiskRoot, false, true);
 }
};
bool Retained(FAutomationTestBase& Test, const UAnimSequence& Sequence, const FString& SkeletonPath, const FString& ModelGuid)
{
 bool Ok = Test.TestEqual(TEXT("Skeleton reference retained"), Sequence.GetSkeleton() ? Sequence.GetSkeleton()->GetPathName() : FString(), SkeletonPath);
 if (!Test.TestNotNull(TEXT("retained Skeleton loaded"), Sequence.GetSkeleton())) return false;
 Ok &= Test.TestEqual(TEXT("Skeleton GUID retained"), Sequence.GetSkeletonGuid(), Sequence.GetSkeleton()->GetGuid());
 const IAnimationDataModel* Model = Sequence.GetDataModel();
 if (!Test.TestNotNull(TEXT("authored model retained"), Model)) return false;
 Ok &= Test.TestEqual(TEXT("authored model content retained"), Model->GenerateGuid().ToString(), ModelGuid);
 TArray<FName> BoneNames; Model->GetBoneTrackNames(BoneNames);
 Ok &= Test.TestTrue(TEXT("ordered bone track names retained"), BoneNames == TArray<FName>{TEXT("root")});
 Ok &= Test.TestEqual(TEXT("frame rate retained"), Model->GetFrameRate(), FFrameRate(24, 1));
 Ok &= Test.TestEqual(TEXT("frame count retained"), Model->GetNumberOfFrames(), 12);
 Ok &= Test.TestEqual(TEXT("curve count retained"), Model->GetFloatCurves().Num(), 1);
 if (Model->GetFloatCurves().Num() == 1)
 {
  const auto& Curve = Model->GetFloatCurves()[0]; const auto& Keys = Curve.FloatCurve.GetConstRefOfKeys();
  Ok &= Test.TestEqual(TEXT("curve name retained"), Curve.GetName(), FName(TEXT("RetainedPreviewCurve")));
  Ok &= Test.TestEqual(TEXT("curve flags retained"), Curve.GetCurveTypeFlags(), int32(AACF_Editable));
  Ok &= Test.TestEqual(TEXT("curve keys retained"), Keys.Num(), 2);
  if (Keys.Num() == 2)
  {
   Ok &= Test.TestEqual(TEXT("curve first value retained"), Keys[0].Value, 1.0f);
   Ok &= Test.TestEqual(TEXT("curve second value retained"), Keys[1].Value, -3.0f);
   Ok &= Test.TestEqual(TEXT("weighted arrive tangent retained"), Keys[0].ArriveTangentWeight, 0.2f);
   Ok &= Test.TestEqual(TEXT("weighted leave tangent retained"), Keys[0].LeaveTangentWeight, 0.4f);
  }
 }
 Ok &= Test.TestEqual(TEXT("RateScale retained"), Sequence.RateScale, 1.25f);
 Ok &= Test.TestTrue(TEXT("playback loop retained"), Sequence.bLoop);
 Ok &= Test.TestTrue(TEXT("root motion setting retained"), Sequence.bForceRootLock);
 Ok &= Test.TestEqual(TEXT("notify count retained"), Sequence.Notifies.Num(), 1);
 if (Sequence.Notifies.Num() == 1) Ok &= Test.TestEqual(TEXT("notify retained"), Sequence.Notifies[0].NotifyName, FName(TEXT("RetainedPoint")));
 Ok &= Test.TestEqual(TEXT("marker count retained"), Sequence.AuthoredSyncMarkers.Num(), 1);
 if (Sequence.AuthoredSyncMarkers.Num() == 1) Ok &= Test.TestEqual(TEXT("marker retained"), Sequence.AuthoredSyncMarkers[0].MarkerName, FName(TEXT("RetainedMarker")));
 return Ok;
}

// Only payload storage is a fixture. Payloads are issued by the actual Editor
// resolver; the production mutation composition supplies the transaction adapter.
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
 for (const auto& Row : Outcome.HandleResults) if (Row.Name == Name && Row.Handles.Num() == 1) { auto Handle = Row.Handles[0]; Handle.ProviderId = TEXT("animation"); return Handle; }
 return {};
}
class FReadContext final : public IAssetDocumentNativeRequestContext
{
 FAssetDocumentRequestContextMetadata Metadata;
public:
 explicit FReadContext(FAssetDocumentRequestContextMetadata InMetadata) : Metadata(InMetadata) {}
 const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
 FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return {}; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation&) override { return {}; }
};
class FContext final : public IAssetDocumentNativeRequestContext
{
 FAssetDocumentRequestContextMetadata Metadata; FAssets& Assets; USkeletalMesh* Desired;
 FResolver& Handles; IAssetDocumentAtomicTransaction& Transaction;
 TSharedRef<FAssetDocumentMutationComposition> Composition; TSharedRef<IAssetDocumentAnimationRequestProvider> Provider;
public:
 FContext(FAssetDocumentRequestContextMetadata InMetadata, FAssets& InAssets, USkeletalMesh* InDesired, FResolver& InHandles,
  IAssetDocumentAtomicTransaction& InTransaction, TSharedRef<FAssetDocumentMutationComposition> InComposition, TSharedRef<IAssetDocumentAnimationRequestProvider> InProvider)
  : Metadata(InMetadata), Assets(InAssets), Desired(InDesired), Handles(InHandles), Transaction(InTransaction), Composition(InComposition), Provider(InProvider) {}
 const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
 FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return {}; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& Invocation) override
 { return Composition->Invoke(TEXT("animation"), Invocation, Handles, &Transaction, [&]() { return Provider->Invoke(Invocation, Handles, &Transaction); }); }
 FAssetDocumentPrimitiveOutcome Apply()
 {
  Handles.Token = Transaction.GetMutationToken();
  FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = Primitive; I.Version = 1; I.RequestId = Metadata.RequestId;
  I.TransactionId = Transaction.GetTransactionId(); I.RuntimeRevision = Metadata.PinnedRuntimeRevision; I.Phase = Metadata.Phase;
  const auto Target = Resolve(*Assets.Sequence, EAssetDocumentHandleAccess::Mutable, Handles, Transaction);
  FAssetDocumentPrimitiveOutcome Failed; Failed.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
  if (!Target.IsSet()) return Failed;
  I.HandleArguments.Add({TEXT("asset"), Target.GetValue()});
  if (Desired)
  {
   const auto Mesh = Resolve(*Desired, EAssetDocumentHandleAccess::ReadOnly, Handles, Transaction); if (!Mesh.IsSet()) return Failed;
   I.HandleArguments.Add({TEXT("preview_mesh"), Mesh.GetValue()});
  }
  I.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object; I.ValueArguments.ObjectKeys = {TEXT("operation")}; I.ValueArguments.Elements.SetNum(1);
  I.ValueArguments.Elements[0].Kind = EAssetDocumentRuntimeValueKind::String; I.ValueArguments.Elements[0].StringValue = Desired ? TEXT("set") : TEXT("clear");
  return Invoke(I);
 }
};
class FHost final : public IAssetDocumentPythonRuntimeHost
{
public:
 FAssets& Assets; USkeletalMesh* Desired; bool bFailRepair; bool& bCaptureOpen;
 bool bActualWriteObserved = false, bOpenAtRepair = false; int32 ApplyCount = 0, RepairCount = 0;
 FHost(FAssets& InAssets, USkeletalMesh* InDesired, bool InFail, bool& InCapture) : Assets(InAssets), Desired(InDesired), bFailRepair(InFail), bCaptureOpen(InCapture) {}
 FAssetDocumentRuntimeResultDto Dispatch(const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentNativeRequestContext& Context) override
 {
  FAssetDocumentRuntimeResultDto Result; Result.RequestId = Request.RequestId; Result.RuntimeRevision = Request.ExpectedRuntimeRevision;
  Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded; Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
  if (Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Apply)
  {
   ++ApplyCount; const auto Outcome = static_cast<FContext&>(Context).Apply(); Result.ChangedSet = Outcome.ChangedSet; Result.Diagnostics = Outcome.Diagnostics;
   if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
  }
  else if (Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Repair)
  {
   ++RepairCount; bOpenAtRepair = bCaptureOpen; bActualWriteObserved = Read(*Assets.Sequence) == Desired;
   CollectGarbage(RF_NoFlags);
   if (bFailRepair)
   {
    Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
    auto& D = Result.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/SequencePreviewFixture/Repair"); D.Code = TEXT("SequencePreviewFixture.LateFailure");
    D.Message = TEXT("Test-only failure after the actual production Preview write."); D.Severity = EAssetDocumentDiagnosticSeverity::Error;
   }
  }
  return Result;
 }
};
class FRegistry final : public IAssetDocumentPrimitiveRegistry
{
 FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const override { return {}; }
 FAssetDocumentPrimitiveResolution Resolve(const FAssetDocumentPrimitiveRequirement&) const override { return {}; }
};
class FCancellation final : public IAssetDocumentCancellationView { bool IsCancellationRequested() const override { return false; } };
class FObserver final : public IAssetDocumentRequestObserver
{
public:
 TArray<EAssetDocumentRuntimeRequestPhase> Phases;
 void OnPhaseChanged(FGuid, EAssetDocumentRuntimeRequestPhase Phase) override { Phases.Add(Phase); }
};
class FVerifier final : public IAssetDocumentStagedArtifactVerifier
{
 FAutomationTestBase& Test; FAssets& Assets; FString ExpectedPreview, ModelGuid;
public:
 int32 Calls = 0;
 FVerifier(FAutomationTestBase& InTest, FAssets& InAssets, USkeletalMesh* Desired)
  : Test(InTest), Assets(InAssets), ExpectedPreview(Desired ? Desired->GetPathName() : FString()), ModelGuid(InAssets.Sequence->GetDataModelInterface()->GenerateGuid().ToString()) {}
 FAssetDocumentFreshVerificationResult VerifyFresh(const FAssetDocumentFreshVerificationInput& Input) override
 {
  ++Calls; FAssetDocumentFreshVerificationResult Result;
  // This is a fixture-specific const native leaf verifier, not a production
  // Sequence canonical extractor or Montage BindStagedRead extension.
  const FString InstancedName = TEXT("/Temp/SequencePreviewVerify_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  FLinkerInstancingContext Instancing; Instancing.AddPackageMapping(FName(*Assets.Sequence->GetOutermost()->GetName()), FName(*InstancedName));
  TStrongObjectPtr<UPackage> Package(LoadPackage(nullptr, *Input.StagedPackagePath, LOAD_ForDiff | LOAD_NoWarn, nullptr, &Instancing));
  auto* Sequence = Package.IsValid() ? FindObject<UAnimSequence>(Package.Get(), TEXT("Sequence")) : nullptr;
  if (!Test.TestNotNull(TEXT("staged leaf deserializes ordinarily"), Sequence)) return Result;
  Result.bFreshLoadProven = Sequence != Assets.Sequence.Get() && Sequence->GetOutermost() != Assets.Sequence->GetOutermost()
   && FPaths::IsSamePath(Package->GetLoadedPath().GetLocalFullPath(), Input.StagedPackagePath);
  auto* Mesh = Read(*Sequence);
  Result.bCanonicalMatch = Result.bFreshLoadProven && (Mesh ? Mesh->GetPathName() : FString()) == ExpectedPreview
   && Retained(Test, *Sequence, Assets.Skeleton->GetPathName(), ModelGuid) && Bytes(Input.StagedSidecarPath) == TArray<uint8>{0x7b, 0x7d};
  if (Result.bCanonicalMatch) { Result.ActualSemanticHash = Input.ExpectedSemanticHash; Result.ActualCodecVersion = Input.ExpectedCodecVersion; }
  Sequence->ClearFlags(RF_Public | RF_Standalone); Package->SetDirtyFlag(false); Package->ClearFlags(RF_Standalone);
  return Result;
 }
};
FAssetDocumentRegionEvidence Evidence()
{ FAssetDocumentRegionEvidence E; E.Kind = EAssetDocumentEvidenceKind::Present; E.SemanticHash = TEXT("sequence-preview-fixture:v1"); E.CodecVersion = 1; return E; }
struct FRun
{
 FAssetDocumentRuntimeResultDto Result; bool bCaptureOpen = false, bClosedBeforeRollback = false, bActualWriteObserved = false, bOpenAtRepair = false;
 int32 Closes = 0, Invalidations = 0, ApplyCount = 0, RepairCount = 0, VerifierCalls = 0;
};
FRun Execute(FAutomationTestBase& Test, FAssets& Assets, USkeletalMesh* Desired, bool bFailRepair)
{
 FRun Run; FAssetDocumentRuntimeRequestDto Request; Request.RequestId = FGuid::NewGuid(); Request.ExpectedRuntimeRevision = 83;
 Request.Operation = EAssetDocumentRuntimeOperation::Apply; Request.TargetAssetPath = Assets.Sequence->GetPathName(); Request.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence");
 FAssetDocumentMutationIntent Intent; Intent.StepIndex = 0; Intent.Requirement = {Primitive, 1, TEXT("animation_profile")}; Intent.InputSchemaId = FString(Primitive) + TEXT(".input.v1");
 Intent.Intent.Kind = EAssetDocumentRuntimeValueKind::Object; Intent.Intent.ObjectKeys = {TEXT("operation"), TEXT("preview_mesh_path")}; Intent.Intent.Elements.SetNum(2);
 Intent.Intent.Elements[0].Kind = EAssetDocumentRuntimeValueKind::String; Intent.Intent.Elements[0].StringValue = Desired ? TEXT("set") : TEXT("clear");
 if (Desired) { Intent.Intent.Elements[1].Kind = EAssetDocumentRuntimeValueKind::String; Intent.Intent.Elements[1].StringValue = Desired->GetPathName(); }
 TArray<FAssetDocumentMutationIntent> Intents = {Intent}; auto Provider = CreateAssetDocumentAnimationRequestProvider();
 if (!Test.TestTrue(TEXT("bind exact Sequence request owner"), Provider->BindRequest(Request).bValid)) return Run;
 TArray<FAssetDocumentMutationComposition::FProviderBatch> Batches; auto& Editor = Batches.AddDefaulted_GetRef(); Editor.ProviderId = TEXT("editor");
 if (!Test.TestTrue(TEXT("prepare actual Editor package finalizer"), PrepareAssetDocumentEditorMutationBatch(Request, {}, Editor.Batch).bValid)) return Run;
 auto& Animation = Batches.AddDefaulted_GetRef(); Animation.ProviderId = TEXT("animation"); Animation.ActualProvider = Provider;
 if (!Test.TestTrue(TEXT("prepare actual bound Preview contribution"), Provider->Prepare(Request, Intents, Animation.Batch).bValid)) return Run;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 const auto Composition = FAssetDocumentMutationComposition::Freeze(Request, MoveTemp(Intents), MoveTemp(Batches), {}, {}, {}, Diagnostics);
 for (const auto& D : Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("freeze unchanged production composition"), Composition.IsValid())) return Run;
 FAssetDocumentPreflightReceipt Preflight; Preflight.RequestId = Request.RequestId; Preflight.RuntimeRevision = Request.ExpectedRuntimeRevision;
 Preflight.CompleteFootprint.Append(Composition->GetFootprint()); Preflight.ReceiptHash = TEXT("sha256:sequence-preview-fixture"); Preflight.bSucceeded = true;
 FAssetDocumentPersistencePlan Plan; Plan.RequestId = Request.RequestId; Plan.TargetPackagePath = Assets.Sequence->GetOutermost()->GetName();
 Plan.TargetSidecarPath = Assets.DiskRoot / TEXT("Installed/Preview.assetdoc"); Plan.StagingRoot = Assets.DiskRoot / TEXT("Staging");
 Plan.JournalId = FGuid::NewGuid().ToString(EGuidFormats::Digits); Plan.Sidecar.Utf8Bytes = {0x7b, 0x7d}; Plan.Sidecar.SemanticHash = Evidence().SemanticHash; Plan.Sidecar.CodecVersion = 1;
 Plan.Footprint.Append(Composition->GetFootprint());
 FResolver Handles; FRegistry Registry; FCancellation Cancellation; FObserver Observer; FHost Host(Assets, Desired, bFailRepair, Run.bCaptureOpen); FVerifier Verifier(Test, Assets, Desired);
 FAssetDocumentCoreRuntimeComposition Core({Host, Registry, Cancellation, Observer,
  [&](const FAssetDocumentRequestContextMetadata& Metadata, const FGuid&, IAssetDocumentAtomicTransaction* Transaction, TArray<FAssetDocumentRuntimeDiagnostic>& Out) -> TUniquePtr<IAssetDocumentNativeRequestContext>
  {
   if (!Transaction) return MakeUnique<FReadContext>(Metadata);
   auto* Adapter = Composition->BindTransaction(*Transaction, Metadata, Out); if (!Adapter) return {};
   return MakeUnique<FContext>(Metadata, Assets, Desired, Handles, *Adapter, Composition.ToSharedRef(), Provider);
  },
  [&](const FAssetDocumentHandleInvalidation& Event)
  {
   ++Run.Invalidations; if (Event.Reason == EAssetDocumentHandleInvalidationReason::Rollback) Run.bClosedBeforeRollback = !Run.bCaptureOpen;
   return Handles.Invalidate(Event);
  }});
 auto Opener = Composition->MakeCaptureOpener();
 FAssetDocumentCoreMutationInputs Mutation{Preflight, Plan, Verifier,
  [&](const FAssetDocumentCorePersistenceTargetRequest& Input, TArray<FAssetDocumentRuntimeDiagnostic>&) -> TOptional<FAssetDocumentCorePersistenceTargetBinding>
  {
   if (!Composition->ValidateComplete().bValid || Input.CanonicalObjectPath != Request.TargetAssetPath || Input.ExactAssetClassPath != Request.RequestedAssetClassPath || Input.TargetPackagePath != Plan.TargetPackagePath) return {};
   return FAssetDocumentCorePersistenceTargetBinding{Input.RequestId, Input.TransactionId, Input.RuntimeRevision, Assets.Sequence.Get(), Assets.Sequence->GetOutermost()};
  },
  {Evidence(), Evidence()}, Assets.DiskRoot / TEXT("Journal"), Assets.DiskRoot / TEXT("Journal/Backup"), Assets.DiskRoot / TEXT("Sync"), TEXT("SequencePreviewFixture"), Request.ExpectedRuntimeRevision,
  Composition->MakeInitialActions(),
  [&, Opener = MoveTemp(Opener)](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Out) mutable -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
  {
   auto Lease = Opener(Id, Out); if (!Lease || !Lease->IsValid()) return {}; Run.bCaptureOpen = true;
   return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([&, Lease = MoveTemp(Lease)]() mutable { Lease->Close(); Lease.Reset(); Run.bCaptureOpen = false; ++Run.Closes; });
  }};
 Run.Result = Core.ExecuteMutation(Request, MoveTemp(Mutation));
 for (const auto& D : Run.Result.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 Run.bActualWriteObserved = Host.bActualWriteObserved; Run.bOpenAtRepair = Host.bOpenAtRepair; Run.ApplyCount = Host.ApplyCount; Run.RepairCount = Host.RepairCount; Run.VerifierCalls = Verifier.Calls;
 Provider->CloseRequest(); return Run;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencePreviewCoreRollbackTest,
 "UEAssetDoc.SequencePreviewCore.LateFailureRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequencePreviewCoreRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequencePreviewIntegrationTests;
 for (const bool bDirty : {false, true}) for (int32 Case = 0; Case < 3; ++Case)
 {
  FAssets Assets; if (!TestTrue(TEXT("saved nonempty Sequence fixture"), Assets.Setup())) return false;
  Assets.Sequence->GetOutermost()->SetDirtyFlag(bDirty);
  const FString Guid = Assets.Sequence->GetDataModelInterface()->GenerateGuid().ToString(); const auto DiskBefore = Bytes(PackageFile(Assets.Sequence->GetPathName()));
  USkeletalMesh* Desired = Case == 0 ? Assets.Desired.Get() : Case == 1 ? nullptr : Assets.Old.Get();
  const auto Run = Execute(*this, Assets, Desired, true);
  TestEqual(TEXT("late failure is terminal Failed"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestTrue(TEXT("failure reached Repair after actual production write"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequencePreviewFixture.LateFailure"); }));
  TestEqual(TEXT("one actual Apply"), Run.ApplyCount, 1); TestEqual(TEXT("one late Repair"), Run.RepairCount, 1);
  TestTrue(TEXT("desired preview observed at late failure"), Run.bActualWriteObserved); TestTrue(TEXT("capture still open during Repair"), Run.bOpenAtRepair);
  TestEqual(TEXT("Core closes capture once"), Run.Closes, 1); TestTrue(TEXT("Core closes capture before rollback invalidation/inverse"), Run.bClosedBeforeRollback);
  TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen); TestEqual(TEXT("one Core rollback invalidation"), Run.Invalidations, 1);
  TestEqual(TEXT("production inverse restores exact original mesh"), Read(*Assets.Sequence), Assets.Old.Get());
  TestEqual(TEXT("production Editor finalizer restores original dirty flag"), Assets.Sequence->GetOutermost()->IsDirty(), bDirty);
  TestFalse(TEXT("no poisoned rollback"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
  TestEqual(TEXT("failure precedes persistence verifier"), Run.VerifierCalls, 0); TestTrue(TEXT("installed package untouched"), Bytes(PackageFile(Assets.Sequence->GetPathName())) == DiskBefore);
  Retained(*this, *Assets.Sequence, Assets.Skeleton->GetPathName(), Guid);
 }
 return !HasAnyErrors();
}

#endif
