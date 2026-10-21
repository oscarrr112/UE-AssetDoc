// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentMutationComposition.h"
#include "AssetDocumentEditorMutationPreparation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
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

// Existing-target test caller only. The production Editor prepared batch and
// FAssetDocumentMutationComposition own all field writes, footprints and inverse.
// This does not register a Sequence Profile/factory or admit an ApplyFile route.
namespace AssetDocumentSequenceFieldsIntegrationTests
{
constexpr const TCHAR* Primitive = TEXT("editor.property.write_admitted_batch.v1");
FString PackageFile(const FString& Path)
{ return FPackageName::LongPackageNameToFilename(FPackageName::ObjectPathToPackageName(Path), FPackageName::GetAssetPackageExtension()); }
TArray<uint8> Bytes(const FString& Path)
{ TArray<uint8> Result; FFileHelper::LoadFileToArray(Result, *Path); return Result; }
bool Save(UObject* Asset)
{
 FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
 const FString Path = PackageFile(Asset->GetPathName()); IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
 return UPackage::SavePackage(Asset->GetOutermost(), Asset, *Path, Args);
}
struct FValues
{
 float Rate = 1.25f; bool bEnabled = false; ERootMotionRootLock::Type Lock = ERootMotionRootLock::RefPose;
 bool bForce = false, bNormalized = false;
 static FValues Read(const UAnimSequence& Sequence)
 { return {Sequence.RateScale, Sequence.bEnableRootMotion, Sequence.RootMotionRootLock, Sequence.bForceRootLock, Sequence.bUseNormalizedRootMotionScale}; }
 bool Matches(const UAnimSequence& Sequence) const
 {
  const auto Actual = Read(Sequence);
  return Actual.Rate == Rate && Actual.bEnabled == bEnabled && Actual.Lock == Lock && Actual.bForce == bForce && Actual.bNormalized == bNormalized;
 }
 bool Check(FAutomationTestBase& Test, const UAnimSequence& Sequence) const
 {
  const auto Actual = Read(Sequence);
  bool Ok = Test.TestTrue(TEXT("exact supplied/preserved RateScale"), Actual.Rate == Rate);
  Ok &= Test.TestEqual(TEXT("supplied/preserved bEnableRootMotion"), Actual.bEnabled, bEnabled);
  Ok &= Test.TestEqual(TEXT("supplied/preserved RootMotionRootLock"), Actual.Lock, Lock);
  Ok &= Test.TestEqual(TEXT("supplied/preserved bForceRootLock"), Actual.bForce, bForce);
  Ok &= Test.TestEqual(TEXT("supplied/preserved bUseNormalizedRootMotionScale"), Actual.bNormalized, bNormalized);
  return Ok;
 }
};
enum class ECase { RefPose, AnimFirstFrame, Zero, PlaybackOnly, RootMotionOnly, Equal, Empty, Omitted };
FValues Desired(ECase Case)
{
 FValues Value;
 if (Case == ECase::Equal || Case == ECase::Empty || Case == ECase::Omitted) return Value;
 Value.Rate = Case == ECase::RootMotionOnly ? 1.25f : (Case == ECase::PlaybackOnly ? 2.5f : 1.75f);
 if (Case != ECase::PlaybackOnly)
 {
  Value.bEnabled = Value.bForce = Value.bNormalized = true;
  Value.Lock = Case == ECase::RefPose ? ERootMotionRootLock::RefPose : (Case == ECase::AnimFirstFrame ? ERootMotionRootLock::AnimFirstFrame : ERootMotionRootLock::Zero);
 }
 return Value;
}
FAssetDocumentRuntimeValueDto String(const FString& Value)
{ FAssetDocumentRuntimeValueDto D; D.Kind = EAssetDocumentRuntimeValueKind::String; D.StringValue = Value; return D; }
FAssetDocumentRuntimeValueDto Boolean(bool Value)
{ FAssetDocumentRuntimeValueDto D; D.Kind = EAssetDocumentRuntimeValueKind::Boolean; D.BooleanValue = Value; return D; }
FAssetDocumentRuntimeValueDto Batch(ECase Case, const FValues& Value)
{
 FAssetDocumentRuntimeValueDto D; D.Kind = EAssetDocumentRuntimeValueKind::Object; D.ObjectKeys = {TEXT("properties")};
 auto& Rows = D.Elements.AddDefaulted_GetRef(); Rows.Kind = EAssetDocumentRuntimeValueKind::Array;
 auto Add = [&](const TCHAR* Name, FAssetDocumentRuntimeValueDto Field)
 {
  auto& Row = Rows.Elements.AddDefaulted_GetRef(); Row.Kind = EAssetDocumentRuntimeValueKind::Object;
  Row.ObjectKeys = {TEXT("name"), TEXT("value")}; Row.Elements = {String(Name), MoveTemp(Field)};
 };
 if (Case != ECase::Empty && Case != ECase::Omitted)
 {
  // Match existing case-insensitive FString admission and the Python field adapter.
  if (Case != ECase::PlaybackOnly)
  {
   Add(TEXT("bEnableRootMotion"), Boolean(Value.bEnabled));
   Add(TEXT("bForceRootLock"), Boolean(Value.bForce)); Add(TEXT("bUseNormalizedRootMotionScale"), Boolean(Value.bNormalized));
  }
  if (Case != ECase::RootMotionOnly)
  { FAssetDocumentRuntimeValueDto Rate; Rate.Kind = EAssetDocumentRuntimeValueKind::Number; Rate.NumberValue = Value.Rate; Add(TEXT("RateScale"), MoveTemp(Rate)); }
  if (Case != ECase::PlaybackOnly)
  {
   const TCHAR* Lock = Value.Lock == ERootMotionRootLock::RefPose ? TEXT("RefPose") : (Value.Lock == ERootMotionRootLock::AnimFirstFrame ? TEXT("AnimFirstFrame") : TEXT("Zero"));
   Add(TEXT("RootMotionRootLock"), String(Lock));
  }
 }
 return D;
}
struct FAssets
{
 FString Root = TEXT("/Game/AssetDocumentSequenceFields/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 FString DiskRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AssetDocumentSequenceFields") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
 TStrongObjectPtr<USkeleton> Skeleton; TStrongObjectPtr<UAnimSequence> Sequence; bool bKeep = false;
 bool Setup()
 {
  if (!IFileManager::Get().MakeDirectory(*(DiskRoot / TEXT("Installed")), true)) return false;
  Skeleton.Reset(NewObject<USkeleton>(CreatePackage(*(Root + TEXT("/Skeleton"))), TEXT("Skeleton"), RF_Public | RF_Standalone));
  { FReferenceSkeletonModifier Modifier(Skeleton.Get()); Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); }
  Sequence.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root + TEXT("/Sequence"))), TEXT("Sequence"), RF_Public | RF_Standalone | RF_Transactional));
  Sequence->SetSkeleton(Skeleton.Get()); Sequence->RateScale = 1.25f; Sequence->bLoop = true;
  Sequence->bEnableRootMotion = false; Sequence->RootMotionRootLock = ERootMotionRootLock::RefPose;
  Sequence->bForceRootLock = false; Sequence->bUseNormalizedRootMotionScale = false;
  auto& Controller = Sequence->GetController(); Controller.InitializeModel();
  Controller.SetFrameRate(FFrameRate(24, 1), false); Controller.SetNumberOfFrames(FFrameNumber(12), false);
  if (!Controller.AddBoneCurve(TEXT("root"), false)) return false;
  TArray<FVector3f> Positions, Scales; TArray<FQuat4f> Rotations;
  for (int32 I = 0; I < 13; ++I) Positions.Add(FVector3f(float(I), 0.0f, 0.0f));
  Scales.Init(FVector3f::OneVector, 13); Rotations.Init(FQuat4f::Identity, 13);
  if (!Controller.SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false)) return false;
  const FAnimationCurveIdentifier Curve(FName(TEXT("RetainedFieldsCurve")), ERawCurveTrackTypes::RCT_Float);
  FRichCurveKey A(0.0f, 1.0f), B(0.5f, -3.0f); A.InterpMode = RCIM_Cubic; A.TangentMode = RCTM_Break; A.TangentWeightMode = RCTWM_WeightedBoth;
  A.ArriveTangentWeight = 0.2f; A.LeaveTangentWeight = 0.4f;
  if (!Controller.AddCurve(Curve, AACF_Editable, false) || !Controller.SetCurveKeys(Curve, {A, B}, false)) return false;
  Controller.NotifyPopulated(); Sequence->WaitOnExistingCompression();
  Sequence->AnimNotifyTracks.SetNum(1); Sequence->AnimNotifyTracks[0].TrackName = TEXT("RetainedTrack");
  auto& Notify = Sequence->Notifies.AddDefaulted_GetRef(); Notify.NotifyName = TEXT("RetainedPoint"); Notify.Guid = FGuid(1, 2, 3, 4); Notify.TrackIndex = 0;
  auto& Marker = Sequence->AuthoredSyncMarkers.AddDefaulted_GetRef(); Marker.MarkerName = TEXT("RetainedMarker"); Marker.Time = 0.25f; Marker.TrackIndex = 0; Marker.Guid = FGuid(5, 6, 7, 8);
  for (UObject* Object : {static_cast<UObject*>(Skeleton.Get()), static_cast<UObject*>(Sequence.Get())})
  { FAssetRegistryModule::AssetCreated(Object); if (!Save(Object)) return false; }
  return true;
 }
 ~FAssets()
 {
  if (Sequence.IsValid()) Sequence->WaitOnExistingCompression();
  if (bKeep) return;
  for (UObject* Object : {static_cast<UObject*>(Sequence.Get()), static_cast<UObject*>(Skeleton.Get())})
   if (IsValid(Object))
   {
    // Keep the owned disk assets valid until Editor exit; delayed file-added
    // notifications must not race an editor-deleted package tombstone. The
    // node's external fixture manifest removes only its files after exit.
    UPackage* Package = Object->GetOutermost();
    Object->ClearFlags(RF_Public | RF_Standalone); Object->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
    Object->MarkAsGarbage(); Package->SetDirtyFlag(false); Package->MarkAsGarbage();
   }
  IFileManager::Get().DeleteDirectory(*DiskRoot, false, true);
 }
};
bool Retained(FAutomationTestBase& Test, const UAnimSequence& Sequence, const FString& SkeletonPath, const FString& Guid)
{
 bool Ok = Test.TestEqual(TEXT("retained Skeleton path"), Sequence.GetSkeleton() ? Sequence.GetSkeleton()->GetPathName() : FString(), SkeletonPath);
 if (!Test.TestNotNull(TEXT("retained Skeleton loaded"), Sequence.GetSkeleton())) return false;
 Ok &= Test.TestEqual(TEXT("retained Skeleton GUID"), Sequence.GetSkeletonGuid(), Sequence.GetSkeleton()->GetGuid());
 const IAnimationDataModel* Model = Sequence.GetDataModel(); if (!Test.TestNotNull(TEXT("retained authored model"), Model)) return false;
 Ok &= Test.TestEqual(TEXT("retained model content GUID"), Model->GenerateGuid().ToString(), Guid);
 TArray<FName> Names; Model->GetBoneTrackNames(Names); Ok &= Test.TestTrue(TEXT("retained ordered bone names"), Names == TArray<FName>{TEXT("root")});
 Ok &= Test.TestEqual(TEXT("retained frame rate"), Model->GetFrameRate(), FFrameRate(24, 1));
 Ok &= Test.TestEqual(TEXT("retained frames"), Model->GetNumberOfFrames(), 12); Ok &= Test.TestEqual(TEXT("retained keys"), Model->GetNumberOfKeys(), 13);
 Ok &= Test.TestEqual(TEXT("retained curve count"), Model->GetFloatCurves().Num(), 1);
 if (Model->GetFloatCurves().Num() == 1)
 {
  const auto& Curve = Model->GetFloatCurves()[0]; const auto& Keys = Curve.FloatCurve.GetConstRefOfKeys();
  Ok &= Test.TestEqual(TEXT("retained curve name"), Curve.GetName(), FName(TEXT("RetainedFieldsCurve")));
  Ok &= Test.TestEqual(TEXT("retained curve flags"), Curve.GetCurveTypeFlags(), int32(AACF_Editable));
  Ok &= Test.TestEqual(TEXT("retained curve keys"), Keys.Num(), 2);
  if (Keys.Num() == 2)
  {
   Ok &= Test.TestEqual(TEXT("retained curve first value"), Keys[0].Value, 1.0f); Ok &= Test.TestEqual(TEXT("retained curve second value"), Keys[1].Value, -3.0f);
   Ok &= Test.TestEqual(TEXT("retained arrive tangent weight"), Keys[0].ArriveTangentWeight, 0.2f); Ok &= Test.TestEqual(TEXT("retained leave tangent weight"), Keys[0].LeaveTangentWeight, 0.4f);
  }
 }
 Ok &= Test.TestTrue(TEXT("retained loop"), Sequence.bLoop); Ok &= Test.TestNull(TEXT("retained empty PreviewMesh"), static_cast<const UAnimationAsset&>(Sequence).GetPreviewMesh());
 Ok &= Test.TestEqual(TEXT("retained retarget name"), Sequence.RetargetSource, NAME_None);
 Ok &= Test.TestEqual(TEXT("retained additive mode"), static_cast<EAdditiveAnimationType>(Sequence.AdditiveAnimType), AAT_None);
 Ok &= Test.TestEqual(TEXT("retained notify count"), Sequence.Notifies.Num(), 1);
 if (Sequence.Notifies.Num() == 1)
 {
  const auto& N = Sequence.Notifies[0]; Ok &= Test.TestEqual(TEXT("retained notify name"), N.NotifyName, FName(TEXT("RetainedPoint")));
  Ok &= Test.TestEqual(TEXT("retained notify GUID"), N.Guid, FGuid(1, 2, 3, 4)); Ok &= Test.TestEqual(TEXT("retained notify track"), N.TrackIndex, 0);
 }
 Ok &= Test.TestEqual(TEXT("retained track count"), Sequence.AnimNotifyTracks.Num(), 1);
 if (Sequence.AnimNotifyTracks.Num() == 1) Ok &= Test.TestEqual(TEXT("retained track name"), Sequence.AnimNotifyTracks[0].TrackName, FName(TEXT("RetainedTrack")));
 Ok &= Test.TestEqual(TEXT("retained marker count"), Sequence.AuthoredSyncMarkers.Num(), 1);
 if (Sequence.AuthoredSyncMarkers.Num() == 1)
 {
  const auto& M = Sequence.AuthoredSyncMarkers[0]; Ok &= Test.TestEqual(TEXT("retained marker name"), M.MarkerName, FName(TEXT("RetainedMarker")));
  Ok &= Test.TestEqual(TEXT("retained marker time"), M.Time, 0.25f); Ok &= Test.TestEqual(TEXT("retained marker GUID"), M.Guid, FGuid(5, 6, 7, 8));
 }
 return Ok;
}
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
 FAssetDocumentRequestContextMetadata Metadata; FAssets& Assets; ECase Case; FAssetDocumentRuntimeValueDto Value;
 FResolver& Handles; IAssetDocumentAtomicTransaction& Transaction; TSharedRef<FAssetDocumentMutationComposition> Composition;
 TSharedRef<IAssetDocumentPrimitiveProvider> Provider; int32& BatchCalls;
public:
 FContext(FAssetDocumentRequestContextMetadata M, FAssets& A, ECase C, FAssetDocumentRuntimeValueDto V, FResolver& H,
  IAssetDocumentAtomicTransaction& T, TSharedRef<FAssetDocumentMutationComposition> P, TSharedRef<IAssetDocumentPrimitiveProvider> Editor, int32& Calls)
  : Metadata(M), Assets(A), Case(C), Value(MoveTemp(V)), Handles(H), Transaction(T), Composition(P), Provider(Editor), BatchCalls(Calls) {}
 const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
 FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return {}; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& I) override
 { return Composition->Invoke(TEXT("editor"), I, Handles, &Transaction, [&]() { return Provider->Invoke(I, Handles, &Transaction); }); }
 FAssetDocumentPrimitiveOutcome Apply()
 {
  FAssetDocumentPrimitiveOutcome Failed; Failed.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
  if (Case == ECase::Omitted) { Failed.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded; return Failed; }
  Handles.Token = Transaction.GetMutationToken();
  const auto Target = Resolve(*Assets.Sequence, EAssetDocumentHandleAccess::Mutable, Handles, Transaction);
  const auto ReadTarget = Resolve(*Assets.Sequence, EAssetDocumentHandleAccess::ReadOnly, Handles, Transaction);
  if (!Target.IsSet() || !ReadTarget.IsSet()) return Failed;
  FAssetDocumentPrimitiveInvocation I; I.Version = 1; I.RequestId = Metadata.RequestId; I.TransactionId = Transaction.GetTransactionId();
  I.RuntimeRevision = Metadata.PinnedRuntimeRevision; I.Phase = Metadata.Phase; I.PrimitiveId = TEXT("editor.property.enumerate_admitted_batch.v1");
  I.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object; I.HandleArguments = {{TEXT("asset"), ReadTarget.GetValue()}};
  const auto Admission = Provider->Invoke(I, Handles, &Transaction);
  if (Admission.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) return Admission;
  const auto* Group = Admission.HandleResults.FindByPredicate([](const auto& Row) { return Row.Name == TEXT("property_batch"); });
  if (!Group || Group->Handles.Num() != 1) return Failed;
  I.PrimitiveId = Primitive; I.ValueArguments = Value;
  I.HandleArguments = {{TEXT("asset"), Target.GetValue()}, {TEXT("property_batch"), Group->Handles[0]}};
  ++BatchCalls; return Invoke(I);
 }
};
class FHost final : public IAssetDocumentPythonRuntimeHost
{
public:
 FAssets& Assets; FValues Expected; bool bFailRepair; bool& CaptureOpen;
 bool bForwardObserved = false, bOpenAtRepair = false; int32 ApplyCount = 0, RepairCount = 0, ChangedCount = 0;
 FHost(FAssets& A, FValues V, bool Fail, bool& Open) : Assets(A), Expected(V), bFailRepair(Fail), CaptureOpen(Open) {}
 FAssetDocumentRuntimeResultDto Dispatch(const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentNativeRequestContext& Context) override
 {
  FAssetDocumentRuntimeResultDto R; R.RequestId = Request.RequestId; R.RuntimeRevision = Request.ExpectedRuntimeRevision;
  R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
  if (Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Apply)
  {
   ++ApplyCount; const auto Outcome = static_cast<FContext&>(Context).Apply(); R.ChangedSet = Outcome.ChangedSet; R.Diagnostics = Outcome.Diagnostics;
   ChangedCount = Outcome.ChangedSet.Num(); if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
  }
  else if (Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Repair)
  {
   ++RepairCount; bOpenAtRepair = CaptureOpen; Assets.Sequence->WaitOnExistingCompression();
   bForwardObserved = Expected.Matches(*Assets.Sequence); CollectGarbage(RF_NoFlags);
   if (bFailRepair)
   {
    R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
    auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/SequenceFieldsFixture/Repair"); D.Code = TEXT("SequenceFieldsFixture.LateFailure");
    D.Message = TEXT("Test-only late failure after the actual production property batch."); D.Severity = EAssetDocumentDiagnosticSeverity::Error;
   }
  }
  return R;
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
 FAutomationTestBase& Test; FAssets& Assets; FValues Expected; FString ModelGuid;
public:
 int32 Calls = 0;
 FVerifier(FAutomationTestBase& T, FAssets& A, FValues V) : Test(T), Assets(A), Expected(V), ModelGuid(A.Sequence->GetDataModelInterface()->GenerateGuid().ToString()) {}
 FAssetDocumentFreshVerificationResult VerifyFresh(const FAssetDocumentFreshVerificationInput& Input) override
 {
  ++Calls; FAssetDocumentFreshVerificationResult Result;
  // Fixture-specific native staged observation, not a public Sequence extractor.
  const FString InstancedName = TEXT("/Temp/SequenceFieldsVerify_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  FLinkerInstancingContext Instancing; Instancing.AddPackageMapping(FName(*Assets.Sequence->GetOutermost()->GetName()), FName(*InstancedName));
  TStrongObjectPtr<UPackage> Package(LoadPackage(nullptr, *Input.StagedPackagePath, LOAD_ForDiff | LOAD_NoWarn, nullptr, &Instancing));
  auto* Sequence = Package.IsValid() ? FindObject<UAnimSequence>(Package.Get(), TEXT("Sequence")) : nullptr;
  if (!Test.TestNotNull(TEXT("staged native Sequence ordinarily loads"), Sequence)) return Result;
  Sequence->WaitOnExistingCompression();
  Result.bFreshLoadProven = Sequence != Assets.Sequence.Get() && Sequence->GetOutermost() != Assets.Sequence->GetOutermost()
   && FPaths::IsSamePath(Package->GetLoadedPath().GetLocalFullPath(), Input.StagedPackagePath);
  Result.bCanonicalMatch = Result.bFreshLoadProven && Expected.Check(Test, *Sequence)
   && Retained(Test, *Sequence, Assets.Skeleton->GetPathName(), ModelGuid) && Bytes(Input.StagedSidecarPath) == TArray<uint8>{0x7b, 0x7d};
  if (Result.bCanonicalMatch) { Result.ActualSemanticHash = Input.ExpectedSemanticHash; Result.ActualCodecVersion = Input.ExpectedCodecVersion; }
  Sequence->ClearFlags(RF_Public | RF_Standalone); Package->SetDirtyFlag(false); Package->ClearFlags(RF_Standalone); return Result;
 }
};
FAssetDocumentRegionEvidence Evidence()
{ FAssetDocumentRegionEvidence E; E.Kind = EAssetDocumentEvidenceKind::Present; E.SemanticHash = TEXT("sequence-fields-fixture:v1"); E.CodecVersion = 1; return E; }
struct FRun
{
 FAssetDocumentRuntimeResultDto Result; bool bCaptureOpen = false, bClosedBeforeRollback = false, bForwardObserved = false, bOpenAtRepair = false;
 int32 Closes = 0, Invalidations = 0, ApplyCount = 0, RepairCount = 0, BatchCalls = 0, ChangedCount = 0, VerifierCalls = 0;
};
FRun Execute(FAutomationTestBase& Test, FAssets& Assets, ECase Case, bool bFailRepair)
{
 FRun Run; FAssetDocumentRuntimeRequestDto Request; Request.RequestId = FGuid::NewGuid(); Request.ExpectedRuntimeRevision = 84;
 Request.Operation = EAssetDocumentRuntimeOperation::Apply; Request.TargetAssetPath = Assets.Sequence->GetPathName(); Request.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence");
 const auto Expected = Desired(Case); const auto Value = Batch(Case, Expected); TArray<FAssetDocumentMutationIntent> Intents;
 if (Case != ECase::Omitted)
 {
  auto& Intent = Intents.AddDefaulted_GetRef(); Intent.StepIndex = 0; Intent.Requirement = {Primitive, 1, TEXT("animation_profile")};
  Intent.Phase = EAssetDocumentRuntimeRequestPhase::Apply; Intent.InputSchemaId = FString(Primitive) + TEXT(".input.v1"); Intent.Intent = Value;
 }
 auto Provider = CreateAssetDocumentEditorPrimitiveProvider();
 TArray<FAssetDocumentMutationComposition::FProviderBatch> Batches; auto& Editor = Batches.AddDefaulted_GetRef(); Editor.ProviderId = TEXT("editor"); Editor.ActualProvider = Provider;
 const auto Prepared = PrepareAssetDocumentEditorMutationBatch(Request, Intents, Editor.Batch);
 for (const auto& D : Prepared.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("real Editor batch/preimage prepares before Core"), Prepared.bValid)) return Run;
 Test.TestEqual(TEXT("production contributes exactly the supplied step"), Editor.Batch.Contributions.Num(), Case == ECase::Omitted ? 0 : 1);
 if (!Editor.Batch.Contributions.IsEmpty()) Test.TestEqual(TEXT("production owns only supplied physical fields"), Editor.Batch.Contributions[0].OwnedSurfaceIds.Num(), Value.Elements[0].Elements.Num());
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 const auto Composition = FAssetDocumentMutationComposition::Freeze(Request, MoveTemp(Intents), MoveTemp(Batches), {}, {}, {}, Diagnostics);
 for (const auto& D : Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("freeze genuine production composition"), Composition.IsValid())) return Run;
 FAssetDocumentPreflightReceipt Preflight; Preflight.RequestId = Request.RequestId; Preflight.RuntimeRevision = Request.ExpectedRuntimeRevision;
 Preflight.CompleteFootprint.Append(Composition->GetFootprint()); Preflight.ReceiptHash = TEXT("sha256:sequence-fields-fixture"); Preflight.bSucceeded = true;
 FAssetDocumentPersistencePlan Plan; Plan.RequestId = Request.RequestId; Plan.TargetPackagePath = Assets.Sequence->GetOutermost()->GetName();
 Plan.TargetSidecarPath = Assets.DiskRoot / TEXT("Installed/Fields.assetdoc"); Plan.StagingRoot = Assets.DiskRoot / TEXT("Staging");
 Plan.JournalId = FGuid::NewGuid().ToString(EGuidFormats::Digits); Plan.Sidecar.Utf8Bytes = {0x7b, 0x7d}; Plan.Sidecar.SemanticHash = Evidence().SemanticHash; Plan.Sidecar.CodecVersion = 1;
 Plan.Footprint.Append(Composition->GetFootprint());
 FResolver Handles; FRegistry Registry; FCancellation Cancellation; FObserver Observer;
 FHost Host(Assets, Expected, bFailRepair, Run.bCaptureOpen); FVerifier Verifier(Test, Assets, Expected);
 FAssetDocumentCoreRuntimeComposition Core({Host, Registry, Cancellation, Observer,
  [&](const FAssetDocumentRequestContextMetadata& Metadata, const FGuid&, IAssetDocumentAtomicTransaction* Transaction, TArray<FAssetDocumentRuntimeDiagnostic>& Out) -> TUniquePtr<IAssetDocumentNativeRequestContext>
  {
   if (!Transaction) return MakeUnique<FReadContext>(Metadata);
   auto* Adapter = Composition->BindTransaction(*Transaction, Metadata, Out); if (!Adapter) return {};
   return MakeUnique<FContext>(Metadata, Assets, Case, Value, Handles, *Adapter, Composition.ToSharedRef(), Provider, Run.BatchCalls);
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
  {Evidence(), Evidence()}, Assets.DiskRoot / TEXT("Journal"), Assets.DiskRoot / TEXT("Journal/Backup"), Assets.DiskRoot / TEXT("Sync"), TEXT("SequenceFieldsFixture"), Request.ExpectedRuntimeRevision,
  Composition->MakeInitialActions(),
  [&, Opener = MoveTemp(Opener)](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Out) mutable -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
  {
   auto Lease = Opener(Id, Out); if (!Lease || !Lease->IsValid()) return {}; Run.bCaptureOpen = true;
   return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([&, Lease = MoveTemp(Lease)]() mutable { Lease->Close(); Lease.Reset(); Run.bCaptureOpen = false; ++Run.Closes; });
  }};
 Run.Result = Core.ExecuteMutation(Request, MoveTemp(Mutation));
 for (const auto& D : Run.Result.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 Run.bForwardObserved = Host.bForwardObserved; Run.bOpenAtRepair = Host.bOpenAtRepair;
 Run.ApplyCount = Host.ApplyCount; Run.RepairCount = Host.RepairCount; Run.ChangedCount = Host.ChangedCount; Run.VerifierCalls = Verifier.Calls;
 return Run;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceFieldsCoreRollbackTest,
 "UEAssetDoc.SequenceFieldsCore.LateFailureRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceFieldsCoreRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceFieldsIntegrationTests;
 for (const ECase Case : {ECase::RefPose, ECase::AnimFirstFrame, ECase::Zero, ECase::PlaybackOnly, ECase::RootMotionOnly, ECase::Equal, ECase::Empty, ECase::Omitted})
 for (const bool bDirty : {false, true})
 {
  FAssets Assets; if (!TestTrue(TEXT("existing native Sequence fixture"), Assets.Setup())) return false;
  UAnimSequence* Original = Assets.Sequence.Get(); UPackage* Package = Original->GetOutermost(); UObject* Model = Original->GetDataModelInterface().GetObject();
  const FValues Before = FValues::Read(*Original); const FString Guid = Original->GetDataModelInterface()->GenerateGuid().ToString();
  const auto DiskBefore = Bytes(PackageFile(Original->GetPathName())); const auto SkeletonBefore = Bytes(PackageFile(Assets.Skeleton->GetPathName()));
  Package->SetDirtyFlag(bDirty); const auto Run = Execute(*this, Assets, Case, true);
  TestEqual(TEXT("actual Core terminal failure"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestTrue(TEXT("injected failure reached Repair after production batch"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceFieldsFixture.LateFailure"); }));
  TestEqual(TEXT("one Apply"), Run.ApplyCount, 1); TestEqual(TEXT("one late Repair"), Run.RepairCount, 1);
  TestEqual(TEXT("omission never invokes a property batch"), Run.BatchCalls, Case == ECase::Omitted ? 0 : 1);
  // The existing batch reports one record per supplied row even for equal values.
  TestEqual(TEXT("exact existing equal/empty/sparse changed-record semantics"), Run.ChangedCount, Batch(Case, Desired(Case)).Elements[0].Elements.Num());
  TestTrue(TEXT("all supplied and omitted values observed before rollback"), Run.bForwardObserved); TestTrue(TEXT("capture open during real late Repair"), Run.bOpenAtRepair);
  TestEqual(TEXT("Core closes capture once"), Run.Closes, 1); TestTrue(TEXT("Core closes capture before rollback invalidation/inverse"), Run.bClosedBeforeRollback);
  TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen); TestEqual(TEXT("one actual rollback invalidation"), Run.Invalidations, 1);
  Before.Check(*this, *Original); TestEqual(TEXT("original package dirty restored last"), Package->IsDirty(), bDirty);
  TestEqual(TEXT("exact target native identity retained"), Assets.Sequence.Get(), Original); TestEqual(TEXT("exact package identity retained"), Original->GetOutermost(), Package);
  TestEqual(TEXT("exact authored model native identity retained"), Original->GetDataModelInterface().GetObject(), Model); TestEqual(TEXT("exact Skeleton native identity retained"), Original->GetSkeleton(), Assets.Skeleton.Get());
  Retained(*this, *Original, Assets.Skeleton->GetPathName(), Guid);
  TestTrue(TEXT("installed Sequence bytes unchanged on rollback"), Bytes(PackageFile(Original->GetPathName())) == DiskBefore);
  TestTrue(TEXT("retained Skeleton bytes unchanged"), Bytes(PackageFile(Assets.Skeleton->GetPathName())) == SkeletonBefore);
  TestEqual(TEXT("late failure precedes staged verifier"), Run.VerifierCalls, 0);
  TestFalse(TEXT("no poisoned inverse"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 return !HasAnyErrors();
}

#endif
