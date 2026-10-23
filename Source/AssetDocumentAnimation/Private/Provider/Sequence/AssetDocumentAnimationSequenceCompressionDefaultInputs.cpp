// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompressionDefaultInputs.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompressionDefaultInputsTestHooks.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompression.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompressionState.h"
#include "AnimationUtils.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"

#if WITH_DEV_AUTOMATION_TESTS
AssetDocumentSequenceCompressionDefaultInputsTesting::FHooks*
 AssetDocumentSequenceCompressionDefaultInputsTesting::ActiveHooks = nullptr;
#endif

namespace AssetDocumentSequenceCompressionDefaultInputs
{
namespace
{
namespace Compression = AssetDocumentSequenceCompression;
namespace Retained = Compression::Retained;
constexpr const TCHAR* Fields[] = {
 TEXT("BoneCompressionSettings"), TEXT("CurveCompressionSettings"), TEXT("VariableFrameStrippingSettings")};

FAssetDocumentContractValidationResult Reject(const TCHAR* Reason)
{
 auto R = Compression::Result(false);
 R.Diagnostics[0].Path = TEXT("/Compression/DefaultInputs"); R.Diagnostics[0].Message = Reason;
 return R;
}
bool Ready()
{
 // Stock LaunchEngineLoop preloads defaults before startup modules (3836-3842),
 // then sets GIsRunning during Init (4858). No provider warmup or cold resolver.
 if (!Retained::ContextValid() || !GIsRunning || IsEngineExitRequested() || IsRunningCommandlet()
  || !IsValid(GEngine) || !GEngine->IsInitialized()) return false;
#if WITH_DEV_AUTOMATION_TESTS
 if (AssetDocumentSequenceCompressionDefaultInputsTesting::ActiveHooks
  && AssetDocumentSequenceCompressionDefaultInputsTesting::ActiveHooks->bForceNotReady) return false;
#endif
 return true;
}
UClass* SettingsClass(EKind Kind)
{
 switch (Kind)
 {
 case EKind::Bone: return UAnimBoneCompressionSettings::StaticClass();
 case EKind::Curve: return UAnimCurveCompressionSettings::StaticClass();
 case EKind::FrameStripping: return UVariableFrameStrippingSettings::StaticClass();
 default: return nullptr;
 }
}
UObject* Query(EKind Kind)
{
#if WITH_DEV_AUTOMATION_TESTS
 if (AssetDocumentSequenceCompressionDefaultInputsTesting::ActiveHooks)
  AssetDocumentSequenceCompressionDefaultInputsTesting::ActiveHooks->Queries.Add(Kind);
#endif
 switch (Kind)
 {
 case EKind::Bone: return FAnimationUtils::GetDefaultAnimationBoneCompressionSettings();
 case EKind::Curve: return FAnimationUtils::GetDefaultAnimationCurveCompressionSettings();
 case EKind::FrameStripping: return FAnimationUtils::GetDefaultVariableFrameStrippingSettings();
 default: return nullptr;
 }
}
}
struct FInputs
{
 Retained::FIdentity Target;
 TArray<Compression::FSettingsInput> Defaults;
 TArray<bool> RequiresRoot;
 TArray<Retained::FIdentity> ClassHints;
 explicit FInputs(UAnimSequence& S) : Target(&S) {}
 bool Current() const
 {
  const auto* S = Cast<UAnimSequence>(Target.Object.Get());
  if (!Ready() || !Target.Current() || !Retained::CanonicalSequence(S)) return false;
  for (const auto& Hint : ClassHints) if (!Hint.Current()) return false;
  for (int32 I = 0; I < Defaults.Num(); ++I)
   if ((RequiresRoot[I] && !Defaults[I].Identity.Object->IsRooted()) || !Defaults[I].Current(*S)) return false;
  return true;
 }
};

FAssetDocumentContractValidationResult Resolve(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<EKind> Selection,
 int32 StepIndex, EAssetDocumentRuntimeRequestPhase Phase,
 FAssetDocumentRuntimeValueDto& OutFields, TSharedPtr<FInputs>& OutInputs)
{
 OutFields = {}; OutInputs.Reset();
 bool Selected[3] = {};
 if (Selection.Num() > 3) return Reject(TEXT("Select distinct fixed settings kinds."));
 for (const auto Kind : Selection)
 {
  const int32 Index = static_cast<int32>(Kind);
  if (Index < 0 || Index >= 3 || Selected[Index]) return Reject(TEXT("Unknown or duplicate default settings kind."));
  Selected[Index] = true;
 }
 if (!Ready() || !Request.RequestId.IsValid() || !Request.ExpectedRuntimeRevision
  || Request.Operation != EAssetDocumentRuntimeOperation::Apply
  || !Request.RequestedAssetClassPath.Equals(TEXT("/Script/Engine.AnimSequence"), ESearchCase::CaseSensitive)
  || StepIndex < 0 || (Phase != EAssetDocumentRuntimeRequestPhase::Apply && Phase != EAssetDocumentRuntimeRequestPhase::Repair)
  || !Retained::CanonicalPath(Request.TargetAssetPath)) return Reject(TEXT("Normal initialized stock Editor and exact native preparation context required before default queries."));
 auto* Target = Cast<UAnimSequence>(FSoftObjectPath(Request.TargetAssetPath).ResolveObject());
 if (!Retained::CanonicalSequence(Target) || !Target->GetPathName().Equals(Request.TargetAssetPath, ESearchCase::CaseSensitive))
  return Reject(TEXT("An exact already-loaded target is required before default queries."));
 auto Inputs = MakeShared<FInputs>(*Target);
 FAssetDocumentRuntimeValueDto Values; Values.Kind = EAssetDocumentRuntimeValueKind::Object;
 for (int32 Index = 0; Index < 3; ++Index)
 {
  if (!Selected[Index]) continue;
  if (!Ready()) return Reject(TEXT("Editor readiness changed before a selected default query."));
  const auto Kind = static_cast<EKind>(Index); UObject* Object = Query(Kind);
  if (!Retained::CanonicalAsset(Object, SettingsClass(Kind)) || !Object->IsRooted()
   || Object->GetOutermost() == Target->GetOutermost()) return Reject(TEXT("The actual stock default must be a rooted valid loaded settings asset."));
  Inputs->Defaults.Emplace(Object, *Target);
  Inputs->RequiresRoot.Add(true);
  if (!Inputs->Current()) return Reject(TEXT("Actual default identity or readonly effective content is not ready."));
  Values.ObjectKeys.Add(Fields[Index]); auto& Value = Values.Elements.AddDefaulted_GetRef();
  Value.Kind = EAssetDocumentRuntimeValueKind::String; Value.StringValue = Object->GetPathName();
 }
 OutFields = MoveTemp(Values); OutInputs = MoveTemp(Inputs); return Compression::Result(true);
}
FAssetDocumentContractValidationResult RetainReference(const TSharedPtr<FInputs>& Inputs, EKind Kind, UObject* Object, UClass* DeclaredClass)
{
 if (!Inputs || !Inputs->Current() || !Retained::CanonicalAsset(Object, SettingsClass(Kind))
  || Object->GetOutermost() == Inputs->Target.Package.Get()
  || (DeclaredClass && (!Retained::Loaded(DeclaredClass) || !Retained::Loaded(DeclaredClass->GetOutermost()) || !Object->IsA(DeclaredClass))))
  return Reject(TEXT("Declared reference must be an exact loaded readonly settings asset."));
 Inputs->Defaults.Emplace(Object, *CastChecked<UAnimSequence>(Inputs->Target.Object.Get()));
 Inputs->RequiresRoot.Add(false);
 if (DeclaredClass) Inputs->ClassHints.Emplace(DeclaredClass);
 return Inputs->Current() ? Compression::Result(true) : Reject(TEXT("Declared settings content or identity is not ready."));
}
FAssetDocumentContractValidationResult GuardPreparedBatch(const TSharedPtr<FInputs>& Inputs, int32 StepIndex, FAssetDocumentPreparedMutationBatch& Batch)
{
 const int32 Index = Batch.Contributions.IndexOfByPredicate([&](const auto& C) { return C.StepIndex == StepIndex; });
 int32 Count = 0; for (const auto& C : Batch.Contributions) if (C.StepIndex == StepIndex) ++Count;
 if (!Inputs || !Inputs->Current() || Count != 1 || Index == INDEX_NONE || !Batch.OpenMutationCapture
  || !Batch.Contributions[Index].InitialAction || !Batch.Contributions[Index].ValidateInvocation)
  return Reject(TEXT("Readonly inputs drifted or the original compression batch is absent."));
 auto Open = MoveTemp(Batch.OpenMutationCapture);
 Batch.OpenMutationCapture = [Inputs, Open = MoveTemp(Open)](const FGuid& Transaction, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics) mutable
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!Inputs->Current()) { Diagnostics.Append(Reject(TEXT("Default inputs changed before capture.")).Diagnostics); return {}; }
  auto Capture = Open(Transaction, Diagnostics);
  if (Capture && !Inputs->Current())
  { Capture->Close(); Diagnostics.Append(Reject(TEXT("Default inputs changed while opening capture.")).Diagnostics); return {}; }
  return Capture;
 };
 auto Validate = MoveTemp(Batch.Contributions[Index].ValidateInvocation);
 Batch.Contributions[Index].ValidateInvocation = [Inputs, Validate = MoveTemp(Validate)](
  const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles) mutable
 {
  if (!Inputs->Current()) return Reject(TEXT("Actual default inputs drifted before supplied invocation."));
  const auto R = Validate(Invocation, Handles);
  return Inputs->Current() ? R : Reject(TEXT("Actual default inputs drifted during supplied invocation validation."));
 };
 return Compression::Result(true);
}
FAssetDocumentContractValidationResult Prepare(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<EKind> Selection,
 int32 StepIndex, EAssetDocumentRuntimeRequestPhase Phase,
 FAssetDocumentMutationIntent& OutSuppliedIntent, FAssetDocumentPreparedMutationBatch& OutBatch)
{
 OutSuppliedIntent = {}; OutBatch = {};
 if (Selection.IsEmpty()) return Reject(TEXT("Select one or more distinct fixed settings kinds."));
 FAssetDocumentRuntimeValueDto Values; TSharedPtr<FInputs> Inputs;
 const auto Resolved = Resolve(Request, Selection, StepIndex, Phase, Values, Inputs);
 if (!Resolved.bValid) return Resolved;
 FAssetDocumentMutationIntent Supplied; Supplied.StepIndex = StepIndex;
 Supplied.Requirement = {Compression::Primitive, 1, TEXT("animation_profile")}; Supplied.Phase = Phase; Supplied.InputSchemaId = Compression::Schema;
 Supplied.Intent.Kind = EAssetDocumentRuntimeValueKind::Object; Supplied.Intent.ObjectKeys = {TEXT("fields")}; Supplied.Intent.Elements.Add(MoveTemp(Values));
 FAssetDocumentPreparedMutationBatch Batch;
 const auto Prepared = PrepareAnimationSequenceCompressionBatch(Request, TConstArrayView<FAssetDocumentMutationIntent>(&Supplied, 1), Batch);
 if (!Prepared.bValid) return Prepared;
 if (Batch.Contributions.Num() != 1) return Reject(TEXT("The unchanged supplied-v1 batch must own one compression contribution."));
 const auto Guarded = GuardPreparedBatch(Inputs, StepIndex, Batch); if (!Guarded.bValid) return Guarded;
 // Original default-only behavior and dev-only export remain unchanged.
 OutSuppliedIntent = MoveTemp(Supplied); OutBatch = MoveTemp(Batch); return Prepared;
}
}
