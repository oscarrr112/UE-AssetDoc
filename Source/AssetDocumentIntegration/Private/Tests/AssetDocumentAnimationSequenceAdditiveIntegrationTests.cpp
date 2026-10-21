// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentMutationComposition.h"
#include "AssetDocumentAnimationRequestBinding.h"
#include "AssetDocumentEditorMutationPreparation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentAnimationPrimitiveProvider.h"
#include "AssetDocumentAnimationMutationPreparation.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "Engine/AssetUserData.h"
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
#include "UObject/UnrealType.h"

// Existing-target fixture caller only. The actual exact Editor atomic group,
// Animation participant, production composition and Core own writes/inverse.
// This does not register a Sequence Profile/factory or admit an ApplyFile route.
namespace AssetDocumentSequenceAdditiveIntegrationTests
{
constexpr const TCHAR* Primitive = TEXT("animation.sequence.complete_edit.v2");
enum class EBoundCase { Disabled, Forward, MissingTarget, ReadOnlyTarget, WrongTargetClass, ForeignTarget, ForeignTransaction,
 ForeignRequest, ForeignRevision, WrongPhase, ExtraValue, Closed, StagedRevision };
struct FScopedBoundRequest
{
 EBoundCase Case = EBoundCase::Disabled; FAutomationTestBase* Test = nullptr;
 bool bRefusalObserved = false, bStagedReadRejected = false, bCompletionObserved = false;
 TSharedPtr<FAssetDocumentAnimationRequestBinding> Binding;
 ~FScopedBoundRequest() { if (Binding) Binding->CloseRequest(); }
};
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
 EAdditiveAnimationType Type=AAT_LocalSpaceBase; EAdditiveBasePoseType Base=ABPT_LocalAnimFrame; int32 Frame=2; FString Reference;
 static FValues Read(const UAnimSequence& S) { return {S.AdditiveAnimType.GetValue(),S.RefPoseType.GetValue(),S.RefFrameIndex,S.RefPoseSeq ? S.RefPoseSeq->GetPathName() : FString()}; }
 bool Matches(const UAnimSequence& S) const { const auto V=Read(S); return V.Type==Type && V.Base==Base && V.Frame==Frame && V.Reference==Reference; }
 bool Check(FAutomationTestBase& T,const UAnimSequence& S) const
 {
  bool Ok=T.TestTrue(TEXT("exact four authored values including preserved omissions"),Matches(S));
  Ok &= T.TestEqual(TEXT("native additive validity follows active/none mode"),S.IsValidAdditive(),Type!=AAT_None);
  Ok &= T.TestTrue(TEXT("actual compressed native data valid"),S.IsCompressedDataValid()); return Ok;
 }
};
enum class ECase { Local, ScaledReference, FrameReference, ExplicitReset, SparseFrame, SparseReferenceFrame, Equal, Empty, Omitted,
 MissingReferenceHandle, MutableReferenceHandle, ForeignToken, ForeignProvider, StaleReferenceHandle, WrongReferenceIdentity, ExtraReferenceHandle };
FValues Desired(ECase Case,const FString& Reference)
{
 FValues V;
 if (Case==ECase::Local) { V.Type=AAT_RotationOffsetMeshSpace; V.Frame=4; }
 if (Case==ECase::ScaledReference || Case==ECase::MissingReferenceHandle || Case==ECase::MutableReferenceHandle || Case==ECase::ForeignToken || Case==ECase::ForeignProvider
  || Case==ECase::StaleReferenceHandle || Case==ECase::WrongReferenceIdentity) { V.Base=ABPT_AnimScaled; V.Reference=Reference; }
 if (Case==ECase::FrameReference) { V.Type=AAT_RotationOffsetMeshSpace; V.Base=ABPT_AnimFrame; V.Frame=3; V.Reference=Reference; }
 if (Case==ECase::ExplicitReset) { V.Type=AAT_None; V.Base=ABPT_None; V.Frame=0; }
 if (Case==ECase::SparseFrame) V.Frame=4;
 if (Case==ECase::SparseReferenceFrame) { V.Base=ABPT_AnimFrame; V.Frame=4; V.Reference=Reference; }
 return V;
}
FAssetDocumentRuntimeValueDto String(const FString& V)
{ FAssetDocumentRuntimeValueDto R; R.Kind=EAssetDocumentRuntimeValueKind::String; R.StringValue=V; return R; }
FAssetDocumentRuntimeValueDto RuntimeValue()
{ FAssetDocumentRuntimeValueDto R; R.Kind=EAssetDocumentRuntimeValueKind::Object; R.ObjectKeys={TEXT("completion_kind")}; R.Elements={String(TEXT("additive_root"))}; return R; }
FAssetDocumentRuntimeValueDto Batch(ECase Case,const FValues& V)
{
 auto R=RuntimeValue(); R.ObjectKeys.Add(TEXT("additive")); auto& A=R.Elements.AddDefaulted_GetRef(); A.Kind=EAssetDocumentRuntimeValueKind::Object;
 auto Add=[&](const TCHAR* Name,FAssetDocumentRuntimeValueDto Value) { A.ObjectKeys.Add(Name); A.Elements.Add(MoveTemp(Value)); };
 if (Case==ECase::Empty || Case==ECase::Omitted) return R;
 if (Case!=ECase::SparseFrame && Case!=ECase::SparseReferenceFrame)
 {
  Add(TEXT("AdditiveAnimType"),String(V.Type==AAT_None ? TEXT("AAT_None") : (V.Type==AAT_LocalSpaceBase ? TEXT("AAT_LocalSpaceBase") : TEXT("AAT_RotationOffsetMeshSpace"))));
  Add(TEXT("RefPoseType"),String(V.Base==ABPT_None ? TEXT("ABPT_None") : (V.Base==ABPT_LocalAnimFrame ? TEXT("ABPT_LocalAnimFrame") : (V.Base==ABPT_AnimScaled ? TEXT("ABPT_AnimScaled") : TEXT("ABPT_AnimFrame")))));
  FAssetDocumentRuntimeValueDto Ref; if (!V.Reference.IsEmpty()) Ref=String(V.Reference); Add(TEXT("RefPoseSeq"),MoveTemp(Ref));
 }
 FAssetDocumentRuntimeValueDto Frame; Frame.Kind=EAssetDocumentRuntimeValueKind::Integer; Frame.IntegerValue=V.Frame; Add(TEXT("RefFrameIndex"),Frame); return R;
}
FTransform Usage(const UAnimSequence& S)
{ FTransform R; S.GetBoneTransform(R,FSkeletonPoseBoneIndex(0),FAnimExtractContext(0.25),false); return R; }
FTransform BoundUsage(const UAnimSequence& S)
{
 FTransform R; FAnimExtractContext Context(0.25); Context.bEnforceCompressedDataSampling = true;
 Context.TargetPlatformHash = S.GetDerivedDataKeyHash(nullptr);
 S.GetBoneTransform(R, FSkeletonPoseBoneIndex(0), Context, false); return R;
}
double BoundExpectedUsage(const FValues& Expected)
{
 // Independent authored fixture: root at .25 is 6, with 12 frames/13 keys.
 // Stock frame references normalize by key count, so the base root is
 // 12 * RefFrameIndex / 13, plus 10 for the supplied reference sequence.
 return 6. - (12. * Expected.Frame / 13. + (Expected.Reference.IsEmpty() ? 0. : 10.));
}
struct FAssets
{
 FString Root = TEXT("/Game/AssetDocumentSequenceAdditive/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 FString DiskRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AssetDocumentSequenceAdditive") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
 TStrongObjectPtr<USkeleton> Skeleton; TStrongObjectPtr<UAnimSequence> Sequence, Reference; bool bKeep = false;
 bool Setup(bool bReferencePreimage=false)
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
  const FAnimationCurveIdentifier Curve(FName(TEXT("RetainedAdditiveCurve")), ERawCurveTrackTypes::RCT_Float);
  FRichCurveKey A(0.0f, 1.0f), B(0.5f, -3.0f); A.InterpMode = RCIM_Cubic; A.TangentMode = RCTM_Break; A.TangentWeightMode = RCTWM_WeightedBoth;
  A.ArriveTangentWeight = 0.2f; A.LeaveTangentWeight = 0.4f;
  if (!Controller.AddCurve(Curve, AACF_Editable, false) || !Controller.SetCurveKeys(Curve, {A, B}, false)) return false;
  Sequence->AdditiveAnimType=AAT_LocalSpaceBase; Sequence->RefPoseType=ABPT_LocalAnimFrame; Sequence->RefFrameIndex=2;
  Controller.NotifyPopulated(); Sequence->WaitOnExistingCompression();
  Reference.Reset(NewObject<UAnimSequence>(CreatePackage(*(Root+TEXT("/Reference"))),TEXT("Reference"),RF_Public|RF_Standalone|RF_Transactional));
  Reference->SetSkeleton(Skeleton.Get()); auto& RC=Reference->GetController(); RC.InitializeModel(); RC.SetFrameRate(FFrameRate(24,1),false); RC.SetNumberOfFrames(FFrameNumber(12),false);
  if (!RC.AddBoneCurve(TEXT("root"),false)) return false;
  for (auto& P : Positions) P.X += 10.0f;
  if (!RC.SetBoneTrackKeys(TEXT("root"),Positions,Rotations,Scales,false)) return false;
  RC.NotifyPopulated(); Reference->WaitOnExistingCompression();
  if (bReferencePreimage)
  {
   // Initial fixture author state before its first save, never product forward.
   Sequence->RefPoseType=ABPT_AnimFrame; Sequence->RefPoseSeq=Reference.Get();
   FPropertyChangedEvent Event(FindFProperty<FByteProperty>(UAnimSequence::StaticClass(),GET_MEMBER_NAME_CHECKED(UAnimSequence,AdditiveAnimType)),EPropertyChangeType::ValueSet);
   Sequence->PostEditChangeProperty(Event); Sequence->WaitOnExistingCompression();
  }
  Sequence->AnimNotifyTracks.SetNum(1); Sequence->AnimNotifyTracks[0].TrackName = TEXT("RetainedTrack");
  auto& Notify = Sequence->Notifies.AddDefaulted_GetRef(); Notify.NotifyName = TEXT("RetainedPoint"); Notify.Guid = FGuid(1, 2, 3, 4); Notify.TrackIndex = 0;
  auto& Marker = Sequence->AuthoredSyncMarkers.AddDefaulted_GetRef(); Marker.MarkerName = TEXT("RetainedMarker"); Marker.Time = 0.25f; Marker.TrackIndex = 0; Marker.Guid = FGuid(5, 6, 7, 8);
  for (UObject* Object : {static_cast<UObject*>(Skeleton.Get()), static_cast<UObject*>(Sequence.Get()), static_cast<UObject*>(Reference.Get())})
  {
   // This existing-target fixture is created directly and saved natively. Stock
   // AnimationModifiers attaches callback UserData on an in-memory creation
   // broadcast; that separate creation route is outside this frozen leaf.
   // Never remove attached UserData or relax the product callback guard.
   if (Object!=Sequence.Get()) FAssetRegistryModule::AssetCreated(Object);
   if (!Save(Object)) return false;
  }
  if (const auto* Data=Sequence->GetAssetUserDataArray())
   if (Data->ContainsByPredicate([](const auto* D){return D!=nullptr;})) return false;
  return true;
 }
 ~FAssets()
 {
  if (Sequence.IsValid()) Sequence->WaitOnExistingCompression();
  if (Reference.IsValid()) Reference->WaitOnExistingCompression();
  if (bKeep) return;
  for (UObject* Object : {static_cast<UObject*>(Sequence.Get()), static_cast<UObject*>(Reference.Get()), static_cast<UObject*>(Skeleton.Get())})
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
  Ok &= Test.TestEqual(TEXT("retained curve name"), Curve.GetName(), FName(TEXT("RetainedAdditiveCurve")));
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
 TSharedRef<IAssetDocumentPrimitiveProvider> Provider; int32& BatchCalls; FScopedBoundRequest& Bound;
public:
 FContext(FAssetDocumentRequestContextMetadata M, FAssets& A, ECase C, FAssetDocumentRuntimeValueDto V, FResolver& H,
  IAssetDocumentAtomicTransaction& T, TSharedRef<FAssetDocumentMutationComposition> P, TSharedRef<IAssetDocumentPrimitiveProvider> Editor, int32& Calls, FScopedBoundRequest& B)
  : Metadata(M), Assets(A), Case(C), Value(MoveTemp(V)), Handles(H), Transaction(T), Composition(P), Provider(Editor), BatchCalls(Calls), Bound(B) {}
 const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
 FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return {}; }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& I) override
 { return Composition->Invoke(TEXT("animation"), I, Handles, &Transaction, [&]() { return Provider->Invoke(I, Handles, &Transaction); }); }
 FAssetDocumentPrimitiveOutcome Apply()
 {
  FAssetDocumentPrimitiveOutcome Failed; Failed.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
  if (Case == ECase::Omitted) { Failed.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded; return Failed; }
  Handles.Token = Transaction.GetMutationToken();
  UObject& TargetObject = Bound.Case == EBoundCase::WrongTargetClass ? static_cast<UObject&>(*Assets.Skeleton)
   : Bound.Case == EBoundCase::ForeignTarget ? static_cast<UObject&>(*Assets.Reference) : static_cast<UObject&>(*Assets.Sequence);
  auto Target = Resolve(TargetObject, Bound.Case == EBoundCase::ReadOnlyTarget ? EAssetDocumentHandleAccess::ReadOnly : EAssetDocumentHandleAccess::Mutable, Handles, Transaction);
  if (!Target.IsSet()) return Failed;
  FAssetDocumentPrimitiveInvocation I; I.Version=2; I.RequestId=Metadata.RequestId; I.TransactionId=Transaction.GetTransactionId();
  I.RuntimeRevision=Metadata.PinnedRuntimeRevision; I.Phase=Metadata.Phase; I.PrimitiveId=Primitive; I.ValueArguments=RuntimeValue();
  if (Bound.Case == EBoundCase::ForeignTransaction) Target->TransactionId = FGuid::NewGuid();
  if (Bound.Case != EBoundCase::MissingTarget) I.HandleArguments={{TEXT("sequence"),Target.GetValue()}};
  const auto Expected=Desired(Case,Assets.Reference->GetPathName());
  if ((!Expected.Reference.IsEmpty() && Case!=ECase::MissingReferenceHandle && Case!=ECase::SparseReferenceFrame) || Case==ECase::ExtraReferenceHandle)
  {
   UObject& RefObject=Case==ECase::WrongReferenceIdentity ? static_cast<UObject&>(*Assets.Sequence) : static_cast<UObject&>(*Assets.Reference);
   auto Ref=Resolve(RefObject,Case==ECase::MutableReferenceHandle ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly,Handles,Transaction);
   if (!Ref.IsSet()) return Failed; auto H=Ref.GetValue(); if (Case==ECase::ForeignToken) H.TransactionId=FGuid::NewGuid();
   if (Case==ECase::ForeignProvider) H.ProviderId=TEXT("animation");
   if (Case==ECase::StaleReferenceHandle) ++H.Generation;
   I.HandleArguments.Add({TEXT("ref_pose_seq"),H});
  }
  if (Bound.Binding && Bound.Case != EBoundCase::Forward)
  {
   auto& Test = *Bound.Test;
   const auto Before = FValues::Read(*Assets.Sequence), ReferenceBefore = FValues::Read(*Assets.Reference);
   const FTransform BeforeUsage = BoundUsage(*Assets.Sequence), ReferenceUsage = BoundUsage(*Assets.Reference);
   UAnimSequence* Original = Assets.Sequence.Get(); UPackage* Package = Original->GetOutermost();
   UObject* Model = Original->GetDataModelInterface().GetObject(); USkeleton* Skeleton = Assets.Skeleton.Get(); UAnimSequence* Reference = Assets.Reference.Get();
   const FString Guid = Original->GetDataModel()->GenerateGuid().ToString(), ReferenceGuid = Reference->GetDataModel()->GenerateGuid().ToString();
   const auto Disk = Bytes(PackageFile(Original->GetPathName())), SkeletonDisk = Bytes(PackageFile(Skeleton->GetPathName())), ReferenceDisk = Bytes(PackageFile(Reference->GetPathName()));
   const bool Dirty = Package->IsDirty(), SkeletonDirty = Skeleton->GetOutermost()->IsDirty(), ReferenceDirty = Reference->GetOutermost()->IsDirty();
   if (Bound.Case == EBoundCase::ForeignRequest) I.RequestId = FGuid::NewGuid();
   if (Bound.Case == EBoundCase::ForeignRevision) ++I.RuntimeRevision;
   if (Bound.Case == EBoundCase::WrongPhase) I.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
   if (Bound.Case == EBoundCase::ExtraValue) { I.ValueArguments.ObjectKeys.Add(TEXT("unexpected")); I.ValueArguments.Elements.Add(String(TEXT("closed-shape refusal"))); }
   if (Bound.Case == EBoundCase::Closed) Bound.Binding->CloseRequest();
   if (Bound.Case == EBoundCase::StagedRevision)
   {
    // The real binding's staged API accepts Montage only; Sequence has no
    // staged read lease. Exercise that boundary without inventing staged state.
    TStrongObjectPtr<UAnimMontage> Probe(NewObject<UAnimMontage>(GetTransientPackage())); TArray<FAssetDocumentRuntimeDiagnostic> D;
    auto Lease = Bound.Binding->BindStagedRead(*Probe, PackageFile(Original->GetPathName()), I.RuntimeRevision + 1, D);
    Bound.bStagedReadRejected = !Lease && !D.IsEmpty();
    Test.TestTrue(TEXT("actual Montage-only staged API rejects this Sequence owner"), Bound.bStagedReadRejected);
    if (Lease) Lease->Close(); ++I.RuntimeRevision;
   }
   // A direct negative probe inside the actual Core transaction reaches the
   // bound owner even when Composition would reject the invocation first.
   const auto Rejected = Bound.Binding->Invoke(I, Handles, &Transaction);
   Bound.bRefusalObserved = Rejected.Status == EAssetDocumentPrimitiveOutcomeStatus::Failed && Rejected.ChangedSet.IsEmpty() && !Rejected.Diagnostics.IsEmpty();
   Test.TestEqual(TEXT("actual bound negative probe returns failed Outcome"), Rejected.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
   Test.TestTrue(TEXT("actual bound Invoke refuses with diagnostics and no changed set"), Bound.bRefusalObserved);
   // Check now, before the original Composition refusal/Core inverse can hide
   // a mistaken write by the probe. The fixture supplies no inverse.
   Before.Check(Test, *Original); Retained(Test, *Original, Skeleton->GetPathName(), Guid);
   Test.TestTrue(TEXT("immediate bound refusal preserves native target usage"), BoundUsage(*Original).Equals(BeforeUsage, 1e-5));
   Test.TestTrue(TEXT("immediate bound refusal preserves current public platform cache"), Original->HasCompressedDataForPlatform(static_cast<const ITargetPlatform*>(nullptr)) && !Original->IsCompressedDataOutOfDate());
   Test.TestEqual(TEXT("immediate bound refusal preserves target identity"), Assets.Sequence.Get(), Original);
   Test.TestEqual(TEXT("immediate bound refusal preserves package identity"), Original->GetOutermost(), Package);
   Test.TestEqual(TEXT("immediate bound refusal preserves model identity"), Original->GetDataModelInterface().GetObject(), Model);
   Test.TestEqual(TEXT("immediate bound refusal preserves Skeleton identity"), Original->GetSkeleton(), Skeleton);
   Test.TestEqual(TEXT("immediate bound refusal preserves reference identity"), Assets.Reference.Get(), Reference);
   Test.TestTrue(TEXT("immediate bound refusal preserves readonly reference tuple"), ReferenceBefore.Matches(*Reference));
   Test.TestEqual(TEXT("immediate bound refusal preserves reference model"), Reference->GetDataModel()->GenerateGuid().ToString(), ReferenceGuid);
   Test.TestTrue(TEXT("immediate bound refusal preserves reference native usage"), BoundUsage(*Reference).Equals(ReferenceUsage, 1e-5));
   Test.TestEqual(TEXT("immediate bound refusal preserves original dirty"), Package->IsDirty(), Dirty);
   Test.TestEqual(TEXT("immediate bound refusal preserves Skeleton dirty"), Skeleton->GetOutermost()->IsDirty(), SkeletonDirty);
   Test.TestEqual(TEXT("immediate bound refusal preserves reference dirty"), Reference->GetOutermost()->IsDirty(), ReferenceDirty);
   Test.TestTrue(TEXT("immediate bound refusal preserves installed target"), Bytes(PackageFile(Original->GetPathName())) == Disk);
   Test.TestTrue(TEXT("immediate bound refusal preserves installed Skeleton"), Bytes(PackageFile(Skeleton->GetPathName())) == SkeletonDisk);
   Test.TestTrue(TEXT("immediate bound refusal preserves installed reference"), Bytes(PackageFile(Reference->GetPathName())) == ReferenceDisk);
  }
  ++BatchCalls; return Invoke(I);
 }
};
class FHost final : public IAssetDocumentPythonRuntimeHost
{
public:
 FAssets& Assets; FValues Expected; bool bFailRepair; bool& CaptureOpen; FScopedBoundRequest& Bound;
 bool bForwardObserved = false, bOpenAtRepair = false; int32 ApplyCount = 0, RepairCount = 0, ChangedCount = 0;
 FHost(FAssets& A, FValues V, bool Fail, bool& Open, FScopedBoundRequest& B) : Assets(A), Expected(V), bFailRepair(Fail), CaptureOpen(Open), Bound(B) {}
 FAssetDocumentRuntimeResultDto Dispatch(const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentNativeRequestContext& Context) override
 {
  FAssetDocumentRuntimeResultDto R; R.RequestId = Request.RequestId; R.RuntimeRevision = Request.ExpectedRuntimeRevision;
  R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
  if (Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Apply)
  {
   ++ApplyCount; const auto Outcome = static_cast<FContext&>(Context).Apply(); R.ChangedSet = Outcome.ChangedSet; R.Diagnostics = Outcome.Diagnostics;
   ChangedCount = Outcome.ChangedSet.Num(); if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) { R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; R.FinalPhase=EAssetDocumentRuntimeRequestPhase::Failed; }
  }
  else if (Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Repair)
  {
   ++RepairCount; bOpenAtRepair = CaptureOpen;
   if (Bound.Binding)
   {
    auto& S = *Assets.Sequence;
    Bound.bCompletionObserved = Expected.Matches(S) && S.IsCompressedDataValid() && !S.IsCompressedDataOutOfDate()
     && S.HasCompressedDataForPlatform(static_cast<const ITargetPlatform*>(nullptr)) && !S.HasResidency(static_cast<const ITargetPlatform*>(nullptr));
    Bound.Test->TestTrue(TEXT("bound leaf completes its current native cache before fixture Wait"), Bound.bCompletionObserved);
    Bound.Test->TestTrue(TEXT("bound additive native sample uses the desired base before fixture Wait"),
     FMath::IsNearlyEqual(BoundUsage(S).GetTranslation().X, BoundExpectedUsage(Expected), 1e-4));
   }
   Assets.Sequence->WaitOnExistingCompression();
   bForwardObserved = Expected.Matches(*Assets.Sequence); CollectGarbage(RF_NoFlags);
   if (bFailRepair)
   {
    R.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed; R.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
    auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/SequenceAdditiveFixture/Repair"); D.Code = TEXT("SequenceAdditiveFixture.LateFailure");
    D.Message = TEXT("Test-only late failure after the production additive completion."); D.Severity = EAssetDocumentDiagnosticSeverity::Error;
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
 FAutomationTestBase& Test; FAssets& Assets; FValues Expected; FString ModelGuid; FTransform ExpectedUsage;
public:
 int32 Calls = 0;
 FVerifier(FAutomationTestBase& T, FAssets& A, FValues V) : Test(T), Assets(A), Expected(V), ModelGuid(A.Sequence->GetDataModelInterface()->GenerateGuid().ToString()), ExpectedUsage(Usage(*A.Sequence)) {}
 FAssetDocumentFreshVerificationResult VerifyFresh(const FAssetDocumentFreshVerificationInput& Input) override
 {
  ++Calls; FAssetDocumentFreshVerificationResult Result; ExpectedUsage=Usage(*Assets.Sequence);
  // Fixture-specific native staged observation, not a public Sequence extractor.
  const FString InstancedName = TEXT("/Temp/SequenceAdditiveVerify_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  FLinkerInstancingContext Instancing; Instancing.AddPackageMapping(FName(*Assets.Sequence->GetOutermost()->GetName()), FName(*InstancedName));
  TStrongObjectPtr<UPackage> Package(LoadPackage(nullptr, *Input.StagedPackagePath, LOAD_ForDiff | LOAD_NoWarn, nullptr, &Instancing));
  auto* Sequence = Package.IsValid() ? FindObject<UAnimSequence>(Package.Get(), TEXT("Sequence")) : nullptr;
  if (!Test.TestNotNull(TEXT("staged native Sequence ordinarily loads"), Sequence)) return Result;
  Sequence->WaitOnExistingCompression();
  Result.bFreshLoadProven = Sequence != Assets.Sequence.Get() && Sequence->GetOutermost() != Assets.Sequence->GetOutermost()
   && FPaths::IsSamePath(Package->GetLoadedPath().GetLocalFullPath(), Input.StagedPackagePath);
  Result.bCanonicalMatch = Result.bFreshLoadProven && Expected.Check(Test, *Sequence)
   && Test.TestTrue(TEXT("staged compressed usage equals live authored mode"),Usage(*Sequence).Equals(ExpectedUsage,1e-5))
   && Retained(Test, *Sequence, Assets.Skeleton->GetPathName(), ModelGuid) && Bytes(Input.StagedSidecarPath) == TArray<uint8>{0x7b, 0x7d};
  if (Result.bCanonicalMatch) { Result.ActualSemanticHash = Input.ExpectedSemanticHash; Result.ActualCodecVersion = Input.ExpectedCodecVersion; }
  Sequence->ClearFlags(RF_Public | RF_Standalone); Package->SetDirtyFlag(false); Package->ClearFlags(RF_Standalone); return Result;
 }
};
FAssetDocumentRegionEvidence Evidence()
{ FAssetDocumentRegionEvidence E; E.Kind = EAssetDocumentEvidenceKind::Present; E.SemanticHash = TEXT("sequence-additive-fixture:v1"); E.CodecVersion = 1; return E; }
struct FRun
{
 FAssetDocumentRuntimeResultDto Result; bool bCaptureOpen = false, bClosedBeforeRollback = false, bForwardObserved = false, bOpenAtRepair = false;
 bool bBoundRefusalObserved = false, bStagedReadRejected = false, bBoundCompletionObserved = false;
 int32 Closes = 0, Invalidations = 0, ApplyCount = 0, RepairCount = 0, BatchCalls = 0, ChangedCount = 0, VerifierCalls = 0;
};
FRun Execute(FAutomationTestBase& Test, FAssets& Assets, ECase Case, bool bFailRepair, EBoundCase BoundCase = EBoundCase::Disabled)
{
 FRun Run; FAssetDocumentRuntimeRequestDto Request; Request.RequestId = FGuid::NewGuid(); Request.ExpectedRuntimeRevision = 84;
 Request.Operation = EAssetDocumentRuntimeOperation::Apply; Request.TargetAssetPath = Assets.Sequence->GetPathName(); Request.RequestedAssetClassPath = TEXT("/Script/Engine.AnimSequence");
 const auto Expected = Desired(Case, Assets.Reference->GetPathName()); const auto Value = Batch(Case, Expected); TArray<FAssetDocumentMutationIntent> Intents;
 if (Case != ECase::Omitted)
 {
  auto& Intent = Intents.AddDefaulted_GetRef(); Intent.StepIndex = 0; Intent.Requirement = {Primitive, 2, TEXT("animation_profile")};
  Intent.Phase = EAssetDocumentRuntimeRequestPhase::Apply; Intent.InputSchemaId = TEXT("animation.sequence.complete_edit.input.v2"); Intent.Intent = Value;
 }
 FScopedBoundRequest Bound; Bound.Case = BoundCase; Bound.Test = &Test;
 if (BoundCase != EBoundCase::Disabled)
 {
  Bound.Binding = MakeShared<FAssetDocumentAnimationRequestBinding>();
  if (!Test.TestTrue(TEXT("one production request binding binds the actual Core request"), Bound.Binding->BindRequest(Request).bValid)) return Run;
 }
 TSharedRef<IAssetDocumentPrimitiveProvider> Provider = Bound.Binding
  ? StaticCastSharedRef<IAssetDocumentPrimitiveProvider>(Bound.Binding.ToSharedRef()) : CreateAssetDocumentAnimationPrimitiveProvider();
 auto EditorProvider = CreateAssetDocumentEditorPrimitiveProvider();
 TArray<FAssetDocumentMutationComposition::FProviderBatch> Batches; auto& Editor = Batches.AddDefaulted_GetRef(); Editor.ProviderId = TEXT("editor"); Editor.ActualProvider = EditorProvider;
 const auto Prepared = PrepareAssetDocumentEditorMutationBatch(Request, {}, Editor.Batch);
 for (const auto& D : Prepared.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("real Editor batch/preimage prepares before Core"), Prepared.bValid)) return Run;
 if (Case!=ECase::Omitted)
 {
  auto& Animation=Batches.AddDefaulted_GetRef(); Animation.ProviderId=TEXT("animation"); Animation.ActualProvider=Provider;
  const auto PreparedAnimation = Bound.Binding ? Bound.Binding->Prepare(Request, Intents, Animation.Batch)
   : PrepareAssetDocumentAnimationMutationBatch(Request, Intents, Animation.Batch);
  for (const auto& D : PreparedAnimation.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
  if (!PreparedAnimation.bValid)
   if (const auto* Data=Assets.Sequence->GetAssetUserDataArray())
    for (const auto* D : *Data) if (D) Test.AddInfo(TEXT("Attached callback identity: ")+D->GetPathName()+TEXT(" class=")+D->GetClass()->GetPathName());
  if (!Test.TestTrue(TEXT("real complete four-field parent prepares"),PreparedAnimation.bValid)) return Run;
  Test.TestEqual(TEXT("one genuine Animation inverse contribution"),Animation.Batch.Contributions.Num(),1);
  Test.TestEqual(TEXT("one parent owns exactly all four physical fields"),Animation.Batch.Contributions[0].OwnedSurfaceIds.Num(),4);
 }
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 const auto Composition = FAssetDocumentMutationComposition::Freeze(Request, MoveTemp(Intents), MoveTemp(Batches), {}, {}, {}, Diagnostics);
 for (const auto& D : Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 if (!Test.TestTrue(TEXT("freeze genuine production composition"), Composition.IsValid())) return Run;
 FAssetDocumentPreflightReceipt Preflight; Preflight.RequestId = Request.RequestId; Preflight.RuntimeRevision = Request.ExpectedRuntimeRevision;
 Preflight.CompleteFootprint.Append(Composition->GetFootprint()); Preflight.ReceiptHash = TEXT("sha256:sequence-additive-fixture"); Preflight.bSucceeded = true;
 FAssetDocumentPersistencePlan Plan; Plan.RequestId = Request.RequestId; Plan.TargetPackagePath = Assets.Sequence->GetOutermost()->GetName();
 Plan.TargetSidecarPath = Assets.DiskRoot / TEXT("Installed/Additive.assetdoc"); Plan.StagingRoot = Assets.DiskRoot / TEXT("Staging");
 Plan.JournalId = FGuid::NewGuid().ToString(EGuidFormats::Digits); Plan.Sidecar.Utf8Bytes = {0x7b, 0x7d}; Plan.Sidecar.SemanticHash = Evidence().SemanticHash; Plan.Sidecar.CodecVersion = 1;
 Plan.Footprint.Append(Composition->GetFootprint());
 FResolver Handles; FRegistry Registry; FCancellation Cancellation; FObserver Observer;
 FHost Host(Assets, Expected, bFailRepair, Run.bCaptureOpen, Bound); FVerifier Verifier(Test, Assets, Expected);
 FAssetDocumentCoreRuntimeComposition Core({Host, Registry, Cancellation, Observer,
  [&](const FAssetDocumentRequestContextMetadata& Metadata, const FGuid&, IAssetDocumentAtomicTransaction* Transaction, TArray<FAssetDocumentRuntimeDiagnostic>& Out) -> TUniquePtr<IAssetDocumentNativeRequestContext>
  {
   if (!Transaction) return MakeUnique<FReadContext>(Metadata);
   auto* Adapter = Composition->BindTransaction(*Transaction, Metadata, Out); if (!Adapter) return {};
   return MakeUnique<FContext>(Metadata, Assets, Case, Value, Handles, *Adapter, Composition.ToSharedRef(), Provider, Run.BatchCalls, Bound);
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
  {Evidence(), Evidence()}, Assets.DiskRoot / TEXT("Journal"), Assets.DiskRoot / TEXT("Journal/Backup"), Assets.DiskRoot / TEXT("Sync"), TEXT("SequenceAdditiveFixture"), Request.ExpectedRuntimeRevision,
  Composition->MakeInitialActions(),
  [&, Opener = MoveTemp(Opener)](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Out) mutable -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
  {
   auto Lease = Opener(Id, Out); if (!Lease || !Lease->IsValid()) return {}; Run.bCaptureOpen = true;
   return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([&, Lease = MoveTemp(Lease)]() mutable { Lease->Close(); Lease.Reset(); Run.bCaptureOpen = false; ++Run.Closes; });
  }};
 Run.Result = Core.ExecuteMutation(Request, MoveTemp(Mutation));
 for (const auto& D : Run.Result.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
 Run.bForwardObserved = Host.bForwardObserved; Run.bOpenAtRepair = Host.bOpenAtRepair;
 Run.bBoundRefusalObserved = Bound.bRefusalObserved; Run.bStagedReadRejected = Bound.bStagedReadRejected; Run.bBoundCompletionObserved = Bound.bCompletionObserved;
 Run.ApplyCount = Host.ApplyCount; Run.RepairCount = Host.RepairCount; Run.ChangedCount = Host.ChangedCount; Run.VerifierCalls = Verifier.Calls;
 return Run;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAdditiveCoreRollbackTest,
 "UEAssetDoc.AnimationProvider.SequenceAdditiveCore.LateFailureRollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceAdditiveCoreRollbackTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceAdditiveIntegrationTests;
 for (const ECase Case : {ECase::Local, ECase::ScaledReference, ECase::FrameReference, ECase::ExplicitReset, ECase::SparseFrame, ECase::SparseReferenceFrame, ECase::Equal, ECase::Empty, ECase::Omitted})
 for (const bool bDirty : {false, true})
 {
  FAssets Assets; if (!TestTrue(TEXT("existing native Sequence fixture"), Assets.Setup(Case==ECase::ExplicitReset || Case==ECase::SparseReferenceFrame))) return false;
  UAnimSequence* Original = Assets.Sequence.Get(); UPackage* Package = Original->GetOutermost(); UObject* Model = Original->GetDataModelInterface().GetObject();
  const FTransform BeforeUsage=Usage(*Original); const FValues Before = FValues::Read(*Original); const FString Guid = Original->GetDataModelInterface()->GenerateGuid().ToString();
  const auto DiskBefore = Bytes(PackageFile(Original->GetPathName())); const auto SkeletonBefore = Bytes(PackageFile(Assets.Skeleton->GetPathName()));
  const auto ReferenceBefore=Bytes(PackageFile(Assets.Reference->GetPathName())); const auto ReferenceTuple=FValues::Read(*Assets.Reference);
  Package->SetDirtyFlag(bDirty); const auto Run = Execute(*this, Assets, Case, true);
  TestEqual(TEXT("actual Core terminal failure"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestTrue(TEXT("injected failure reached Repair after production batch"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceAdditiveFixture.LateFailure"); }));
  TestEqual(TEXT("one Apply"), Run.ApplyCount, 1); TestEqual(TEXT("one late Repair"), Run.RepairCount, 1);
  TestEqual(TEXT("omission never invokes a property batch"), Run.BatchCalls, Case == ECase::Omitted ? 0 : 1);
  TestEqual(TEXT("one changed region only for actual changed author tuple"),Run.ChangedCount,(Case==ECase::Equal || Case==ECase::Empty || Case==ECase::Omitted) ? 0 : 1);
  TestTrue(TEXT("all supplied and omitted values observed before rollback"), Run.bForwardObserved); TestTrue(TEXT("capture open during real late Repair"), Run.bOpenAtRepair);
  TestEqual(TEXT("Core closes capture once"), Run.Closes, 1); TestTrue(TEXT("Core closes capture before rollback invalidation/inverse"), Run.bClosedBeforeRollback);
  TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen); TestEqual(TEXT("one actual rollback invalidation"), Run.Invalidations, 1);
  Before.Check(*this, *Original); TestTrue(TEXT("restored compressed usage equals preimage"),Usage(*Original).Equals(BeforeUsage,1e-5)); TestEqual(TEXT("original package dirty restored last"), Package->IsDirty(), bDirty);
  TestEqual(TEXT("exact target native identity retained"), Assets.Sequence.Get(), Original); TestEqual(TEXT("exact package identity retained"), Original->GetOutermost(), Package);
  TestEqual(TEXT("exact authored model native identity retained"), Original->GetDataModelInterface().GetObject(), Model); TestEqual(TEXT("exact Skeleton native identity retained"), Original->GetSkeleton(), Assets.Skeleton.Get());
  Retained(*this, *Original, Assets.Skeleton->GetPathName(), Guid);
  TestTrue(TEXT("installed Sequence bytes unchanged on rollback"), Bytes(PackageFile(Original->GetPathName())) == DiskBefore);
  TestTrue(TEXT("retained Skeleton bytes unchanged"), Bytes(PackageFile(Assets.Skeleton->GetPathName())) == SkeletonBefore);
  TestTrue(TEXT("reference tuple retained on rollback"),ReferenceTuple.Matches(*Assets.Reference));
  TestTrue(TEXT("reference bytes retained on rollback"),Bytes(PackageFile(Assets.Reference->GetPathName()))==ReferenceBefore);
  TestFalse(TEXT("readonly reference remains clean"),Assets.Reference->GetOutermost()->IsDirty());
  TestEqual(TEXT("late failure precedes staged verifier"), Run.VerifierCalls, 0);
  TestFalse(TEXT("no poisoned inverse"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceAdditiveCoreHandlePreflightTest,
 "UEAssetDoc.AnimationProvider.SequenceAdditiveCore.HandlePreflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceAdditiveCoreHandlePreflightTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceAdditiveIntegrationTests;
 for (const ECase Case : {ECase::MissingReferenceHandle,ECase::MutableReferenceHandle,ECase::ForeignToken,ECase::ForeignProvider,
  ECase::StaleReferenceHandle,ECase::WrongReferenceIdentity,ECase::ExtraReferenceHandle})
 for (const bool bDirty : {false,true})
 {
  FAssets Assets; if (!TestTrue(TEXT("native handle-preflight fixture"),Assets.Setup())) return false;
  const auto Before=FValues::Read(*Assets.Sequence), RefBefore=FValues::Read(*Assets.Reference);
  const auto DiskBefore=Bytes(PackageFile(Assets.Sequence->GetPathName())), RefDiskBefore=Bytes(PackageFile(Assets.Reference->GetPathName()));
  const auto UsageBefore=Usage(*Assets.Sequence); const FString Guid=Assets.Sequence->GetDataModelInterface()->GenerateGuid().ToString();
  Assets.Sequence->GetOutermost()->SetDirtyFlag(bDirty);
  const auto Run=Execute(*this,Assets,Case,false);
  TestEqual(TEXT("invalid reference handle fails real Core Apply"),Run.Result.TerminalStatus,EAssetDocumentRuntimeTerminalStatus::Failed);
  TestEqual(TEXT("actual additive invocation attempted once"),Run.BatchCalls,1);
  TestEqual(TEXT("no Repair after rejected preflight"),Run.RepairCount,0);
  TestEqual(TEXT("no changed region before write"),Run.ChangedCount,0);
  TestEqual(TEXT("no staged verifier after rejected preflight"),Run.VerifierCalls,0);
  TestEqual(TEXT("Core closes actual capture once"),Run.Closes,1); TestFalse(TEXT("terminal capture closed"),Run.bCaptureOpen);
  Before.Check(*this,*Assets.Sequence); Retained(*this,*Assets.Sequence,Assets.Skeleton->GetPathName(),Guid);
  TestTrue(TEXT("cached native use unchanged before any mutation"),Usage(*Assets.Sequence).Equals(UsageBefore,1e-5));
  TestEqual(TEXT("original dirty state preserved"),Assets.Sequence->GetOutermost()->IsDirty(),bDirty);
  TestTrue(TEXT("reference tuple unchanged"),RefBefore.Matches(*Assets.Reference)); TestFalse(TEXT("reference remains clean"),Assets.Reference->GetOutermost()->IsDirty());
  TestTrue(TEXT("installed target unchanged"),Bytes(PackageFile(Assets.Sequence->GetPathName()))==DiskBefore);
  TestTrue(TEXT("installed reference unchanged"),Bytes(PackageFile(Assets.Reference->GetPathName()))==RefDiskBefore);
  TestFalse(TEXT("preflight failure does not poison rollback"),Run.Result.Diagnostics.ContainsByPredicate([](const auto& D){return D.Code.StartsWith(TEXT("Atomic.Poisoned"));}));
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequenceBoundCompleteEditCoreTest,
 "UEAssetDoc.AnimationProvider.SequenceBoundCompletionCore.CompleteEdit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSequenceBoundCompleteEditCoreTest::RunTest(const FString&)
{
 using namespace AssetDocumentSequenceAdditiveIntegrationTests;
 // Only the already-integrated four-field additive completion. All writes,
 // capture and inverse remain the actual bound provider/Editor/Core chain.
 for (const ECase Case : {ECase::FrameReference, ECase::Equal})
 {
  FAssets A; if (!TestTrue(TEXT("existing saved bound additive fixture"), A.Setup())) return false;
  UAnimSequence* Target = A.Sequence.Get(); UPackage* Package = Target->GetOutermost(); UObject* Model = Target->GetDataModelInterface().GetObject();
  USkeleton* Skeleton = A.Skeleton.Get(); UAnimSequence* Reference = A.Reference.Get(); const FGuid SkeletonGuid = Skeleton->GetGuid();
  const FString Guid = Target->GetDataModel()->GenerateGuid().ToString(), ReferenceGuid = Reference->GetDataModel()->GenerateGuid().ToString();
  const auto Before = FValues::Read(*Target), ReferenceBefore = FValues::Read(*Reference); const FTransform ReferenceUsage = BoundUsage(*Reference);
  const auto ReferenceDisk = Bytes(PackageFile(Reference->GetPathName())), SkeletonDisk = Bytes(PackageFile(Skeleton->GetPathName()));
  const auto Run = Execute(*this, A, Case, false, EBoundCase::Forward);
  TestEqual(TEXT("same request binding forward/noop commits through Core"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
  TestTrue(TEXT("bound native completion proven before any fixture Wait"), Run.bBoundCompletionObserved);
  TestTrue(TEXT("real Repair observes forward authored tuple"), Run.bForwardObserved);
  Desired(Case, Reference->GetPathName()).Check(*this, *Target);
  TestEqual(TEXT("independent bound additive native sample"), BoundUsage(*Target).GetTranslation().X, BoundExpectedUsage(Desired(Case, Reference->GetPathName())), 1e-4);
  TestEqual(TEXT("true noop has no changed region"), Run.ChangedCount, Case == ECase::Equal ? 0 : 1);
  if (Case == ECase::Equal) TestTrue(TEXT("true noop keeps exact authored tuple"), Before.Matches(*Target));
  else TestEqual(TEXT("real supplied reference is used by native leaf"), Target->RefPoseSeq.Get(), Reference);
  TestEqual(TEXT("one original Composition invocation"), Run.BatchCalls, 1); TestEqual(TEXT("existing real staged verifier runs"), Run.VerifierCalls, 1);
  TestEqual(TEXT("actual capture closes once"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  TestEqual(TEXT("target identity retained"), A.Sequence.Get(), Target); TestEqual(TEXT("package identity retained"), Target->GetOutermost(), Package);
  TestEqual(TEXT("authored model identity retained"), Target->GetDataModelInterface().GetObject(), Model);
  TestEqual(TEXT("Skeleton identity retained"), Target->GetSkeleton(), Skeleton); TestEqual(TEXT("Skeleton GUID retained"), Skeleton->GetGuid(), SkeletonGuid);
  TestEqual(TEXT("readonly reference identity retained"), A.Reference.Get(), Reference); TestTrue(TEXT("readonly reference tuple retained"), ReferenceBefore.Matches(*Reference));
  TestEqual(TEXT("readonly reference model retained"), Reference->GetDataModel()->GenerateGuid().ToString(), ReferenceGuid);
  TestTrue(TEXT("readonly reference native use retained"), BoundUsage(*Reference).Equals(ReferenceUsage, 1e-5));
  TestTrue(TEXT("readonly reference bytes retained"), Bytes(PackageFile(Reference->GetPathName())) == ReferenceDisk);
  TestTrue(TEXT("readonly Skeleton bytes retained"), Bytes(PackageFile(Skeleton->GetPathName())) == SkeletonDisk);
  TestFalse(TEXT("readonly reference remains clean"), Reference->GetOutermost()->IsDirty()); TestFalse(TEXT("readonly Skeleton remains clean"), Skeleton->GetOutermost()->IsDirty());
  Retained(*this, *Target, Skeleton->GetPathName(), Guid); TestFalse(TEXT("Core saved target is clean"), Package->IsDirty());
 }
 for (const bool Dirty : {false, true})
 {
  FAssets A; if (!TestTrue(TEXT("existing saved bound inverse fixture"), A.Setup())) return false;
  UAnimSequence* Target = A.Sequence.Get(); UPackage* Package = Target->GetOutermost(); UObject* Model = Target->GetDataModelInterface().GetObject();
  USkeleton* Skeleton = A.Skeleton.Get(); UAnimSequence* Reference = A.Reference.Get(); const FGuid SkeletonGuid = Skeleton->GetGuid();
  const auto Before = FValues::Read(*Target), ReferenceBefore = FValues::Read(*Reference); const FTransform BeforeUsage = BoundUsage(*Target), ReferenceUsage = BoundUsage(*Reference);
  const FString Guid = Target->GetDataModel()->GenerateGuid().ToString(), ReferenceGuid = Reference->GetDataModel()->GenerateGuid().ToString();
  const auto Disk = Bytes(PackageFile(Target->GetPathName())), ReferenceDisk = Bytes(PackageFile(Reference->GetPathName())), SkeletonDisk = Bytes(PackageFile(Skeleton->GetPathName()));
  Package->SetDirtyFlag(Dirty); const auto Run = Execute(*this, A, ECase::FrameReference, true, EBoundCase::Forward);
  TestEqual(TEXT("genuine bound later failure reaches Core"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestTrue(TEXT("specific existing late failure reached"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("SequenceAdditiveFixture.LateFailure"); }));
  TestTrue(TEXT("forward native completion precedes later failure"), Run.bBoundCompletionObserved); TestTrue(TEXT("forward tuple was actually observed"), Run.bForwardObserved);
  TestEqual(TEXT("one actual Apply"), Run.ApplyCount, 1); TestEqual(TEXT("one later Repair"), Run.RepairCount, 1); TestEqual(TEXT("real forward changed one region"), Run.ChangedCount, 1);
  TestTrue(TEXT("capture open at later failure"), Run.bOpenAtRepair); TestTrue(TEXT("capture closes before actual Core inverse"), Run.bClosedBeforeRollback);
  TestEqual(TEXT("capture closes once"), Run.Closes, 1); TestEqual(TEXT("one real handle invalidation"), Run.Invalidations, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  Before.Check(*this, *Target); TestTrue(TEXT("Core inverse restores complete native usage"), BoundUsage(*Target).Equals(BeforeUsage, 1e-5));
  TestEqual(TEXT("Core inverse restores original dirty last"), Package->IsDirty(), Dirty);
  TestEqual(TEXT("inverse target identity"), A.Sequence.Get(), Target); TestEqual(TEXT("inverse package identity"), Target->GetOutermost(), Package);
  TestEqual(TEXT("inverse model identity"), Target->GetDataModelInterface().GetObject(), Model); TestEqual(TEXT("inverse Skeleton identity"), Target->GetSkeleton(), Skeleton);
  TestEqual(TEXT("inverse Skeleton GUID"), Skeleton->GetGuid(), SkeletonGuid); TestEqual(TEXT("inverse reference identity"), A.Reference.Get(), Reference);
  Retained(*this, *Target, Skeleton->GetPathName(), Guid); TestTrue(TEXT("inverse preserves readonly reference tuple"), ReferenceBefore.Matches(*Reference));
  TestEqual(TEXT("inverse preserves readonly reference model"), Reference->GetDataModel()->GenerateGuid().ToString(), ReferenceGuid);
  TestTrue(TEXT("inverse preserves readonly reference usage"), BoundUsage(*Reference).Equals(ReferenceUsage, 1e-5));
  TestTrue(TEXT("later failure leaves installed target"), Bytes(PackageFile(Target->GetPathName())) == Disk);
  TestTrue(TEXT("later failure leaves readonly reference"), Bytes(PackageFile(Reference->GetPathName())) == ReferenceDisk);
  TestTrue(TEXT("later failure leaves readonly Skeleton"), Bytes(PackageFile(Skeleton->GetPathName())) == SkeletonDisk);
  TestFalse(TEXT("readonly reference remains clean"), Reference->GetOutermost()->IsDirty()); TestFalse(TEXT("readonly Skeleton remains clean"), Skeleton->GetOutermost()->IsDirty());
  TestEqual(TEXT("later failure precedes save verification"), Run.VerifierCalls, 0);
  TestFalse(TEXT("genuine Core inverse is not poisoned"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 for (const EBoundCase Case : {EBoundCase::MissingTarget, EBoundCase::ReadOnlyTarget, EBoundCase::WrongTargetClass, EBoundCase::ForeignTarget,
  EBoundCase::ForeignTransaction, EBoundCase::ForeignRequest, EBoundCase::ForeignRevision, EBoundCase::WrongPhase, EBoundCase::ExtraValue, EBoundCase::Closed, EBoundCase::StagedRevision})
 {
  FAssets A; if (!TestTrue(TEXT("existing saved bound refusal fixture"), A.Setup())) return false;
  const auto Before = FValues::Read(*A.Sequence); const FTransform BeforeUsage = BoundUsage(*A.Sequence); const auto Disk = Bytes(PackageFile(A.Sequence->GetPathName()));
  A.Sequence->GetOutermost()->SetDirtyFlag(true); const auto Run = Execute(*this, A, ECase::FrameReference, false, Case);
  TestTrue(TEXT("actual bound refusal observed before Composition/Core recovery"), Run.bBoundRefusalObserved);
  if (Case == EBoundCase::StagedRevision) TestTrue(TEXT("actual staged API refusal observed"), Run.bStagedReadRejected);
  TestEqual(TEXT("original Core invocation fails"), Run.Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
  TestEqual(TEXT("one original Composition invocation"), Run.BatchCalls, 1); TestEqual(TEXT("no changed set"), Run.ChangedCount, 0);
  TestEqual(TEXT("refusal precedes Repair"), Run.RepairCount, 0); TestEqual(TEXT("refusal precedes save verifier"), Run.VerifierCalls, 0);
  TestEqual(TEXT("actual capture closes once"), Run.Closes, 1); TestFalse(TEXT("terminal capture closed"), Run.bCaptureOpen);
  Before.Check(*this, *A.Sequence); TestTrue(TEXT("terminal refusal preserves native use"), BoundUsage(*A.Sequence).Equals(BeforeUsage, 1e-5));
  TestTrue(TEXT("terminal refusal preserves original dirty"), A.Sequence->GetOutermost()->IsDirty());
  TestTrue(TEXT("terminal refusal leaves installed bytes"), Bytes(PackageFile(A.Sequence->GetPathName())) == Disk);
  TestFalse(TEXT("refusal does not poison inverse"), Run.Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
 }
 return !HasAnyErrors();
}
#endif
