// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Cutover/AssetDocumentEditorFactoryMutationPreparation.h"
#include "AssetDocumentEditorTargetLifecycleCatalog.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Cutover/AssetDocumentEditorMutationFootprintBuilders.h"
#include "Provider/Cutover/AssetDocumentEditorRollbackSeed.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
FAssetDocumentContractValidationResult FactoryFailure(const TCHAR* Code, const TCHAR* Message)
{
 FAssetDocumentContractValidationResult Result;
 auto& D = Result.Diagnostics.AddDefaulted_GetRef();
 D.Path = TEXT("/MutationPreparation/Editor/Factory"); D.Code = Code; D.Message = Message;
 return Result;
}

struct FFactoryIntentState
{
 FGuid RequestId;
 uint64 Revision = 0;
 FAssetDocumentEditorMutationIdentity Target;
 FString PackagePath;
 FString AssetName;
 FString ClassPath;
 // The class-neutral generic factory row creates from the class alone or with an
 // optional factory context asset (factory_context_path / factory_context, e.g. a Skeleton).
 FString SkeletonPath;
 FAssetDocumentPrimitiveRequirement Create;
 TStrongObjectPtr<UClass> Class;
 TStrongObjectPtr<UObject> Skeleton;
 TStrongObjectPtr<UClass> SkeletonClass;
};

FAssetDocumentContractValidationResult ValidateFactoryInvocation(
 const FFactoryIntentState& State, const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles)
{
 if (Invocation.PrimitiveId != State.Create.PrimitiveId || Invocation.Version != State.Create.Version
  || Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Apply || Invocation.RequestId != State.RequestId
  || Invocation.RuntimeRevision != State.Revision || !Invocation.TransactionId.IsValid()
  || Invocation.HandleArguments.Num() != (State.Skeleton.IsValid() ? 2 : 1))
  return FactoryFailure(TEXT("MutationPreparation.InvocationMismatch"), TEXT("Factory invocation differs from the prepared L1 step."));
 auto Validation = ValidateAssetDocumentRuntimeValue(Invocation.ValueArguments);
 if (!Validation.bValid) return Validation;
 const FAssetDocumentEditorValueFieldSpec Shape[] = {
  {TEXT("package_path"), EAssetDocumentRuntimeValueKind::String}, {TEXT("asset_name"), EAssetDocumentRuntimeValueKind::String}};
 TArray<const FAssetDocumentRuntimeValueDto*> Values; FString Code;
 if (!TryReadAssetDocumentEditorExactObject(Invocation.ValueArguments, Shape, Values, Code)
  || Values[0]->StringValue != State.PackagePath || Values[1]->StringValue != State.AssetName)
  return FactoryFailure(TEXT("MutationPreparation.IntentMismatch"), TEXT("Factory live target differs from the frozen intent."));
 const FAssetDocumentPrimitiveHandleArgument* ClassArg = nullptr;
 const FAssetDocumentPrimitiveHandleArgument* SkeletonArg = nullptr;
 for (const auto& Arg : Invocation.HandleArguments)
 {
  if (Arg.Name == TEXT("asset_class")) ClassArg = &Arg;
  // The generic row names its optional context handle factory_context.
  if (Arg.Name == TEXT("factory_context")) SkeletonArg = &Arg;
 }
 if (!ClassArg || (SkeletonArg != nullptr) != State.Skeleton.IsValid())
  return FactoryFailure(TEXT("MutationPreparation.InvocationMismatch"), TEXT("Factory requires exact asset_class and context handles."));
 FAssetDocumentHandleValidationContext Context;
 Context.RequestId = Invocation.RequestId; Context.TransactionId = Invocation.TransactionId;
 Context.RuntimeRevision = Invocation.RuntimeRevision; Context.ProviderId = TEXT("editor");
 Context.Phase = Invocation.Phase; Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
 Context.RequiredKind = EAssetDocumentHandleKind::Class;
 auto ClassLease = Handles.AcquireVerifiedLease(ClassArg->Handle, Context);
 if (!ClassLease.Lease || !ClassLease.Diagnostics.IsEmpty())
 {
  FAssetDocumentContractValidationResult Result; Result.Diagnostics = MoveTemp(ClassLease.Diagnostics);
  if (Result.Diagnostics.IsEmpty()) return FactoryFailure(TEXT("MutationPreparation.HandleRejected"), TEXT("Factory class lease is missing."));
  return Result;
 }
 using namespace UE::AssetDocument::EditorPrimitive;
 auto* ClassPayload = TryGetNativeIdentityPayload(ClassLease.Lease->GetPayloadChecked(), ClassPayloadTypeId);
 FClassIdentityView ClassView;
 if (!ClassPayload || !ClassPayload->TryGetClassIdentity(ClassView)
  || ClassView.Class != State.Class.Get() || ClassView.CanonicalClassPath != State.ClassPath
  || !IsValid(State.Class.Get()) || State.Class->GetPathName() != State.ClassPath)
  return FactoryFailure(TEXT("MutationPreparation.FactoryIdentityChanged"), TEXT("Factory class differs from its exact strong prepared identity."));
 if (!IsAssetDocumentEditorCreatedTargetFullyAbsent(CaptureAssetDocumentEditorCreatedTargetAbsence(State.Target)))
  return FactoryFailure(TEXT("MutationPreparation.FactoryTargetExists"), TEXT("The factory target is no longer fully absent."));
 if (!State.Skeleton.IsValid()) { FAssetDocumentContractValidationResult Result; Result.bValid = true; return Result; }
 Context.RequiredKind = EAssetDocumentHandleKind::Asset;
 auto ContextLease = Handles.AcquireVerifiedLease(SkeletonArg->Handle, Context);
 if (!ContextLease.Lease || !ContextLease.Diagnostics.IsEmpty())
 {
  FAssetDocumentContractValidationResult Result; Result.Diagnostics = MoveTemp(ContextLease.Diagnostics);
  if (Result.Diagnostics.IsEmpty()) return FactoryFailure(TEXT("MutationPreparation.HandleRejected"), TEXT("Factory context lease is missing."));
  return Result;
 }
 auto& ContextPayload = ContextLease.Lease->GetPayloadChecked();
 auto* NativeContext = TryGetNativeIdentityPayload(ContextPayload, AssetPayloadTypeId);
 if (!NativeContext) NativeContext = TryGetNativeIdentityPayload(ContextPayload, CreatedAssetPayloadTypeId);
 FObjectIdentityView ContextView;
 if (!NativeContext || !NativeContext->TryGetObjectIdentity(ContextView)
  || ContextView.Object != State.Skeleton.Get() || ContextView.CanonicalObjectPath != State.SkeletonPath
  || !IsValid(State.Skeleton.Get()) || State.Skeleton->GetPathName() != State.SkeletonPath
  || State.Skeleton->GetClass() != State.SkeletonClass.Get())
  return FactoryFailure(TEXT("MutationPreparation.FactoryIdentityChanged"), TEXT("Factory context reference differs from its exact strong prepared identity."));
 FAssetDocumentContractValidationResult Result; Result.bValid = true; return Result;
}
}

FAssetDocumentContractValidationResult PrepareAssetDocumentEditorFactoryMutationContribution(
 const FAssetDocumentRuntimeRequestDto& Request, const FAssetDocumentMutationIntent& Intent,
 FAssetDocumentPreparedMutationContribution& OutContribution)
{
 OutContribution = FAssetDocumentPreparedMutationContribution();
 // The class's catalog lifecycle owns the create step. Only the class-neutral generic
 // factory row is prepared here; class-specific rows (Blueprint) create through the
 // snapshot-covered path (FreezeSnapshotCovered + PrepareAssetDocumentEditorCutoverTransaction).
 const FAssetDocumentEditorTargetLifecycleCatalogEntry* Factory =
  FindAssetDocumentEditorTargetLifecycle(Request.RequestedAssetClassPath);
 if (Factory && !Factory->IsClassNeutral())
  return FactoryFailure(TEXT("MutationPreparation.UnsupportedIntent"), TEXT("Class-specific lifecycle rows do not prepare factory steps here; only the class-neutral generic factory row does."));
 if (!Factory || Factory->RequiredPrimitives.Num() != 4
  || Factory->RequiredPrimitives[2].PrimitiveId != Intent.Requirement.PrimitiveId
  || Factory->RequiredPrimitives[2].Version != Intent.Requirement.Version
  || Intent.Requirement.RequiredByRouteId.IsEmpty() || Intent.StepIndex < 0
  || Intent.InputSchemaId != Intent.Requirement.PrimitiveId + TEXT(".input.v1")
  || Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply
  || !CanAssetDocumentEditorTargetLifecycleCreate(*Factory, Request.RequestedAssetClassPath))
  return FactoryFailure(TEXT("MutationPreparation.UnsupportedIntent"), TEXT("Factory preparation requires the class's catalog lifecycle create contract."));
 auto Validation = ValidateAssetDocumentRuntimeValue(Intent.Intent);
 if (!Validation.bValid) return Validation;
 const FAssetDocumentEditorValueFieldSpec Shape[] = {
  {TEXT("asset_class_path"), EAssetDocumentRuntimeValueKind::String},
  {TEXT("package_path"), EAssetDocumentRuntimeValueKind::String}, {TEXT("asset_name"), EAssetDocumentRuntimeValueKind::String}};
 // The generic row optionally carries one factory context asset (AnimMontage: Skeleton).
 const FAssetDocumentEditorValueFieldSpec ContextShape[] = {
  {TEXT("asset_class_path"), EAssetDocumentRuntimeValueKind::String}, {TEXT("factory_context_path"), EAssetDocumentRuntimeValueKind::String},
  {TEXT("package_path"), EAssetDocumentRuntimeValueKind::String}, {TEXT("asset_name"), EAssetDocumentRuntimeValueKind::String}};
 TArray<const FAssetDocumentRuntimeValueDto*> Values; FString Code;
 const bool bContext = Intent.Intent.ObjectKeys.Contains(TEXT("factory_context_path"));
 const bool bShape = bContext
  ? TryReadAssetDocumentEditorExactObject(Intent.Intent, ContextShape, Values, Code)
  : TryReadAssetDocumentEditorExactObject(Intent.Intent, Shape, Values, Code);
 if (!bShape)
  return FactoryFailure(TEXT("MutationPreparation.InvalidIntent"), TEXT("Factory preparation requires exact class, context reference and target value intent."));
 if (!bContext) Values.Insert(nullptr, 1);
 auto State = MakeShared<FFactoryIntentState>();
 State->RequestId = Request.RequestId; State->Revision = Request.ExpectedRuntimeRevision;
 State->Create = Intent.Requirement;
 State->ClassPath = Values[0]->StringValue; State->SkeletonPath = Values[1] ? Values[1]->StringValue : FString();
 State->PackagePath = Values[2]->StringValue; State->AssetName = Values[3]->StringValue;
 if (State->ClassPath != Request.RequestedAssetClassPath
  || !TryBuildAssetDocumentEditorMutationIdentity(State->PackagePath + TEXT("/") + State->AssetName + TEXT(".") + State->AssetName, State->Target)
  || State->Target.CanonicalObjectPath != Request.TargetAssetPath)
  return FactoryFailure(TEXT("MutationPreparation.InvalidTarget"), TEXT("The typed factory target/class differs from the exact request."));
 State->Class.Reset(FSoftClassPath(State->ClassPath).ResolveClass());
 if (!IsValid(State->Class.Get()) || State->Class->GetPathName() != State->ClassPath)
  return FactoryFailure(TEXT("MutationPreparation.InvalidReference"), TEXT("The exact factory class must already be live."));
 if (bContext)
 {
  FAssetDocumentEditorMutationIdentity ContextIdentity;
  State->Skeleton.Reset(FSoftObjectPath(State->SkeletonPath).ResolveObject());
  if (!TryBuildAssetDocumentEditorMutationIdentity(State->SkeletonPath, ContextIdentity)
   || !IsValid(State->Skeleton.Get()) || State->Skeleton->IsTemplate() || State->Skeleton->GetPathName() != State->SkeletonPath)
   return FactoryFailure(TEXT("MutationPreparation.InvalidReference"), TEXT("The factory context must be one canonical live asset."));
  State->SkeletonClass.Reset(State->Skeleton->GetClass());
 }
 if (!IsAssetDocumentEditorCreatedTargetFullyAbsent(CaptureAssetDocumentEditorCreatedTargetAbsence(State->Target)))
  return FactoryFailure(TEXT("MutationPreparation.FactoryTargetExists"), TEXT("Factory preparation requires the exact target to be fully absent."));
 OutContribution.StepIndex = Intent.StepIndex;
 OutContribution.RawFootprint = BuildAssetDocumentEditorFactoryMutationFootprint(State->Target);
 OutContribution.OwnedSurfaceIds = {Request.TargetAssetPath + TEXT("::@lifecycle.create")};
 OutContribution.ValidateInvocation = [State](const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles)
 { return ValidateFactoryInvocation(*State, Invocation, Handles); };
 FAssetDocumentContractValidationResult Result; Result.bValid = true; return Result;
}
