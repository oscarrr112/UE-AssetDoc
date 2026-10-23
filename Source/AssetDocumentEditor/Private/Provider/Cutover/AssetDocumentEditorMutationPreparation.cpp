// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentEditorMutationPreparation.h"

#include "AssetDocumentEditorCutoverTransactionPreparation.h"
#include "AssetDocumentEditorTargetLifecycleCatalog.h"
#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Cutover/AssetDocumentEditorMutationFootprintBuilders.h"
#include "Provider/Cutover/AssetDocumentEditorFactoryMutationPreparation.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyHandlers.h"
#include "UObject/Class.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectThreadContext.h"
#include "UObject/UnrealType.h"

namespace EditorMutationPreparationLocal
{
constexpr const TCHAR* BatchPrimitive = TEXT("editor.property.write_admitted_batch.v1");
constexpr const TCHAR* BatchSchema = TEXT("editor.property.write_admitted_batch.v1.input.v1");

FAssetDocumentContractValidationResult Failure(const TCHAR* Code, const TCHAR* Message)
{
 FAssetDocumentContractValidationResult Result;
 auto& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
 Diagnostic.Path = TEXT("/MutationPreparation/Editor");
 Diagnostic.Code = Code;
 Diagnostic.Message = Message;
 return Result;
}
FAssetDocumentContractValidationResult Success()
{
 FAssetDocumentContractValidationResult Result;
 Result.bValid = true;
 return Result;
}
bool IsContextValid()
{
 return IsInGameThread() && GIsEditor && !IsGarbageCollecting()
  && !UE::IsSavingPackage(nullptr) && !FUObjectThreadContext::Get().IsRoutingPostLoad;
}

// A lease binds only the frozen state. No live capture or token issuance.
struct FCaptureState
{
 FGuid Transaction;
 bool bOpened = false;
 bool bClosed = false;
};

struct FTargetIdentity
{
 TStrongObjectPtr<UObject> Target;
 TStrongObjectPtr<UClass> Class;
 TStrongObjectPtr<UPackage> Package;
 FString ObjectPath;
 FString PackagePath;
 bool bCreate = false;
 bool IsCurrent(UObject* Object) const
 {
  return IsValid(Object) && !Object->IsTemplate()
   && Object->GetPathName() == ObjectPath && Object->GetClass() == Class.Get()
   && Object->GetOutermost()->GetName() == PackagePath
   && (bCreate || (Object == Target.Get() && Object->GetOutermost() == Package.Get()));
 }
};

class FFieldsAction final : public IAssetDocumentRollbackAction
{
public:
 FFieldsAction(FAssetDocumentEditorPropertyPreimage&& InPreimage, TSharedRef<FCaptureState> InCapture)
  : Preimage(MoveTemp(InPreimage)), Capture(MoveTemp(InCapture)) {}
 virtual FStringView GetRollbackComponentId() const override { return TEXT("rollback.uobject"); }
 virtual FAssetDocumentContractValidationResult Rollback(FStringView) override
 {
  if (!IsContextValid() || !Capture->bOpened || !Capture->bClosed)
   return Failure(TEXT("MutationPreparation.InvalidCapture"), TEXT("Fields restore requires a closed prepared capture."));
  return Preimage.RestoreFieldsOnly();
 }
private:
 FAssetDocumentEditorPropertyPreimage Preimage;
 TSharedRef<FCaptureState> Capture;
};

class FPackageFinalizer final : public IAssetDocumentPostRollbackFinalizer
{
public:
 explicit FPackageFinalizer(UPackage& InPackage, TSharedRef<FCaptureState> InCapture)
  : Package(&InPackage), PackagePath(InPackage.GetName()), bOriginalDirty(InPackage.IsDirty()), Capture(MoveTemp(InCapture)) {}
 virtual FStringView GetRollbackComponentId() const override { return TEXT("rollback.package"); }
 virtual FAssetDocumentContractValidationResult FinalizePostRollback(FStringView) override
 {
  if (bConsumed) return Failure(TEXT("MutationPreparation.AlreadyConsumed"), TEXT("The package finalizer was already attempted."));
  bConsumed = true;
  if (!IsContextValid() || !Capture->bOpened || !Capture->bClosed
   || !IsValid(Package.Get()) || Package->GetName() != PackagePath)
   return Failure(TEXT("MutationPreparation.PackageIdentityChanged"), TEXT("The exact prepared package or capture is no longer valid."));
  Package->SetDirtyFlag(bOriginalDirty);
  return Package->IsDirty() == bOriginalDirty ? Success()
   : Failure(TEXT("MutationPreparation.DirtyRestoreFailed"), TEXT("The original package dirty state did not read back."));
 }
private:
 TStrongObjectPtr<UPackage> Package;
 FString PackagePath;
 bool bOriginalDirty;
 bool bConsumed = false;
 TSharedRef<FCaptureState> Capture;
};

bool ValuesEqual(const FAssetDocumentRuntimeValueDto& A, const FAssetDocumentRuntimeValueDto& B)
{
 if (A.Kind != B.Kind || A.BooleanValue != B.BooleanValue || A.IntegerValue != B.IntegerValue
  || A.NumberValue != B.NumberValue || A.StringValue != B.StringValue || A.BytesValue != B.BytesValue
  || A.ObjectKeys != B.ObjectKeys || A.Elements.Num() != B.Elements.Num()) return false;
 for (int32 Index = 0; Index < A.Elements.Num(); ++Index)
  if (!ValuesEqual(A.Elements[Index], B.Elements[Index])) return false;
 return true;
}

FAssetDocumentContractValidationResult ParseFields(
 const FAssetDocumentRuntimeValueDto& Intent, UClass& Class,
 TArray<FAssetDocumentEditorPropertyBatchTokenFieldView>& OutFields)
{
 const auto Validation = ValidateAssetDocumentRuntimeValue(Intent);
 if (!Validation.bValid) return Validation;
 TArray<const FAssetDocumentRuntimeValueDto*> Values;
 FString Code;
 const TArray<FAssetDocumentEditorValueFieldSpec> Shape = {{TEXT("properties"), EAssetDocumentRuntimeValueKind::Array}};
 FAssetDocumentRuntimeValueDto Rows;
 TArray<FString> DeclaredNames;
 if (!TrySplitAssetDocumentEditorDeclaredPropertyArguments(Intent, Rows, DeclaredNames)
  || !TryReadAssetDocumentEditorExactObject(Rows, Shape, Values, Code))
  return Failure(TEXT("MutationPreparation.InvalidIntent"), TEXT("Expected the exact admitted properties value intent."));
 TArray<FAssetDocumentEditorPropertyBatchTokenFieldView> Admitted;
 TArray<FAssetDocumentRuntimeDiagnostic> AdmissionDiagnostics;
 if (!TryGetAssetDocumentEditorAdmittedPropertyFields(Class, DeclaredNames, Admitted, AdmissionDiagnostics))
 {
  FAssetDocumentContractValidationResult Result;
  Result.Diagnostics = MoveTemp(AdmissionDiagnostics);
  return Result;
 }
 int32 Cursor = 0;
 for (const auto& Row : Values[0]->Elements)
 {
  const int32 NameIndex = Row.ObjectKeys.IndexOfByKey(TEXT("name"));
  const int32 ValueIndex = Row.ObjectKeys.IndexOfByKey(TEXT("value"));
  if (Row.Kind != EAssetDocumentRuntimeValueKind::Object || Row.ObjectKeys.Num() != 2
   || Row.Elements.Num() != 2 || !Row.Elements.IsValidIndex(NameIndex) || !Row.Elements.IsValidIndex(ValueIndex)
   || Row.Elements[NameIndex].Kind != EAssetDocumentRuntimeValueKind::String)
   return Failure(TEXT("MutationPreparation.InvalidIntent"), TEXT("Expected exact name/value property rows."));
  const FString& Name = Row.Elements[NameIndex].StringValue;
  int32 Match = INDEX_NONE;
  for (int32 Index = Cursor; Index < Admitted.Num(); ++Index)
   if (Admitted[Index].Property->GetAuthoredName() == Name) { Match = Index; break; }
  if (Match == INDEX_NONE || !Admitted[Match].bWritable)
   return Failure(TEXT("MutationPreparation.PropertyNotAdmitted"), TEXT("Property rows must be unique, ordered and writable under the existing admission."));
  const auto& Field = Admitted[Match];
  // Whole aggregates have no disjoint leaf ownership proof in A0, except a
  // Profile-declared field: the declaring Profile owns that whole surface.
  if (Field.Property->ArrayDim != 1 || (!Field.bDeclared && (CastField<FStructProperty>(Field.Property)
   || CastField<FArrayProperty>(Field.Property) || CastField<FSetProperty>(Field.Property) || CastField<FMapProperty>(Field.Property))))
   return Failure(TEXT("MutationPreparation.OverlappingSurface"), TEXT("Whole aggregate writes require an independently admitted disjoint ownership contract."));
  TUniquePtr<FAssetDocumentEditorScopedPropertyValue> Native;
  const auto Decoded = DecodeAssetDocumentEditorTypedValue(*Field.Property, Row.Elements[ValueIndex], Native);
  if (!Decoded.bSucceeded || Decoded.CodecId != Field.CodecId)
   return Failure(TEXT("MutationPreparation.InvalidTypedValue"), TEXT("The property desired value does not match its admitted codec."));
  OutFields.Add(Field);
  Cursor = Match + 1;
 }
 return Success();
}

FAssetDocumentContractValidationResult ValidateLive(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 const FAssetDocumentMutationIntent& Intent, const FGuid& RequestId, uint64 Revision,
 const FTargetIdentity& Identity, const FCaptureState& Capture,
 const FAssetDocumentEditorPropertyBatchTokenPayload& FrozenFields)
{
 if (!IsContextValid() || !Capture.bOpened || Capture.bClosed || Invocation.TransactionId != Capture.Transaction
  || Invocation.RequestId != RequestId || Invocation.RuntimeRevision != Revision
  || Invocation.PrimitiveId != Intent.Requirement.PrimitiveId || Invocation.Version != Intent.Requirement.Version
  || Invocation.Phase != Intent.Phase || Invocation.HandleArguments.Num() != 2)
  return Failure(TEXT("MutationPreparation.InvocationMismatch"), TEXT("The invocation does not match the bound prepared step."));
 const auto ValidValue = ValidateAssetDocumentRuntimeValue(Invocation.ValueArguments);
 if (!ValidValue.bValid) return ValidValue;
 if (!ValuesEqual(Invocation.ValueArguments, Intent.Intent))
  return Failure(TEXT("MutationPreparation.IntentMismatch"), TEXT("Live desired values differ from the frozen typed intent."));
 TUniquePtr<IAssetDocumentVerifiedHandleLease> AssetLease, BatchLease;
 UObject* Target = nullptr;
 FAssetDocumentEditorPropertyBatchTokenView Batch;
 FAssetDocumentPrimitiveOutcome Outcome;
 if (!TryAcquireAssetDocumentEditorPropertyBatchWrite(Invocation, Handles, AssetLease, BatchLease, Target, Batch, Outcome))
 {
  FAssetDocumentContractValidationResult Result;
  Result.Diagnostics = MoveTemp(Outcome.Diagnostics);
  return Result;
 }
 if (!Identity.IsCurrent(Target))
  return Failure(TEXT("MutationPreparation.TargetIdentityChanged"), TEXT("The live asset handle is not the exact prepared target."));
 FAssetDocumentEditorPropertyBatchTokenView Frozen;
 if (!FrozenFields.TryGetView(Frozen))
  return Failure(TEXT("MutationPreparation.FieldIdentityChanged"), TEXT("Prepared property identity or type revision changed."));
 for (const auto& Field : Frozen.Fields)
 {
  const auto* Match = Batch.Fields.FindByPredicate([&](const auto& Current) { return Current.Property == Field.Property; });
  if (!Match || Match->DeclaringClass != Field.DeclaringClass || Match->CodecId != Field.CodecId
   || Match->TypeRevision != Field.TypeRevision || !Match->bReadable || !Match->bWritable)
   return Failure(TEXT("MutationPreparation.FieldIdentityChanged"), TEXT("Live admission differs from the frozen property field contract."));
 }
 return Success();
}
}

FAssetDocumentContractValidationResult PrepareAssetDocumentEditorMutationBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 using namespace EditorMutationPreparationLocal;
 OutBatch = FAssetDocumentPreparedMutationBatch();
 static bool bPreparing = false;
 if (!IsContextValid() || bPreparing || !Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0
  || Request.Operation != EAssetDocumentRuntimeOperation::Apply)
  return Failure(TEXT("MutationPreparation.InvalidContext"), TEXT("Preparation requires a non-reentrant Editor Apply request before BeginLive."));
 TGuardValue<bool> PreparingGuard(bPreparing, true);
 FAssetDocumentEditorMutationIdentity Canonical;
 if (!TryBuildAssetDocumentEditorMutationIdentity(Request.TargetAssetPath, Canonical))
  return Failure(TEXT("MutationPreparation.InvalidTarget"), TEXT("The target path must be canonical."));
 auto Identity = MakeShared<FTargetIdentity>();
 Identity->ObjectPath = Request.TargetAssetPath;
 Identity->PackagePath = Canonical.PackageName;
 UObject* Target = FSoftObjectPath(Request.TargetAssetPath).ResolveObject();
 UClass* Class = FSoftClassPath(Request.RequestedAssetClassPath).ResolveClass();
 if (!IsValid(Class) || Class->GetPathName() != Request.RequestedAssetClassPath)
  return Failure(TEXT("MutationPreparation.InvalidClass"), TEXT("The exact asset class must already be resolved."));
 Identity->Class.Reset(Class);
 if (!Target)
 {
  const FAssetDocumentEditorTargetLifecycleCatalogEntry* Factory =
   FindAssetDocumentEditorTargetLifecycle(Request.RequestedAssetClassPath);
  if (!Factory || !CanAssetDocumentEditorTargetLifecycleCreate(*Factory, Request.RequestedAssetClassPath))
   return Failure(TEXT("MutationPreparation.FactoryCoverageMissing"), TEXT("The absent target has no lifecycle that can create its class."));
  FAssetDocumentEditorCutoverMutationIntent Create;
  Create.RequestId = Request.RequestId;
  Create.RuntimeRevision = Request.ExpectedRuntimeRevision;
  Create.CanonicalObjectPath = Request.TargetAssetPath;
  Create.ExactAssetClassPath = Request.RequestedAssetClassPath;
  Create.FactoryId = Factory->FactoryId;
  Create.bCreateByFactory = true;
  auto Coverage = PrepareAssetDocumentEditorCutoverTransaction(Create);
  if (!Coverage.bPrepared)
  {
   FAssetDocumentContractValidationResult Result;
   Result.Diagnostics = MoveTemp(Coverage.Diagnostics);
   return Result;
  }
  if (Coverage.RollbackSeeds.Num() != 1)
   return Failure(TEXT("MutationPreparation.FactoryCoverageMissing"), TEXT("Create coverage requires exactly one factory inverse."));
  Identity->bCreate = true;
 }
 else
 {
  Identity->Target.Reset(Target);
  Identity->Package.Reset(Target->GetOutermost());
  if (!Identity->IsCurrent(Target))
   return Failure(TEXT("MutationPreparation.InvalidTarget"), TEXT("The target class or package identity is not exact."));
 }
 auto Capture = MakeShared<FCaptureState>();
 FAssetDocumentPreparedMutationBatch Prepared;
 if (!Identity->bCreate) Prepared.PackageFinalizer = MakeShared<FPackageFinalizer>(*Identity->Package.Get(), Capture);
 // The live transaction only binds this already captured batch.
 Prepared.OpenMutationCapture = [Capture, Identity](const FGuid& Transaction, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!IsContextValid() || !Transaction.IsValid() || Capture->bOpened || Capture->bClosed
   || (!Identity->bCreate && !Identity->IsCurrent(Identity->Target.Get())))
  {
   Diagnostics.Append(Failure(TEXT("MutationPreparation.InvalidCapture"), TEXT("The prepared capture can bind one exact live transaction.")).Diagnostics);
   return nullptr;
  }
  Capture->Transaction = Transaction;
  Capture->bOpened = true;
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([Capture]() { Capture->bClosed = true; });
 };
 TSet<FString> Surfaces;
 TSet<int32> Steps;
 for (const auto& Intent : Intents)
 {
  if (Identity->bCreate && Intent.Requirement.PrimitiveId != BatchPrimitive)
  {
   if (Steps.Contains(Intent.StepIndex))
    return Failure(TEXT("MutationPreparation.UnsupportedIntent"), TEXT("Duplicate factory preparation step."));
   FAssetDocumentPreparedMutationContribution FactoryContribution;
   auto FactoryResult = PrepareAssetDocumentEditorFactoryMutationContribution(Request, Intent, FactoryContribution);
   if (!FactoryResult.bValid) return FactoryResult;
   for (const auto& Surface : FactoryContribution.OwnedSurfaceIds)
   {
    if (Surfaces.Contains(Surface)) return Failure(TEXT("MutationPreparation.OverlappingSurface"), TEXT("The factory lifecycle has multiple prepared writers."));
    Surfaces.Add(Surface);
   }
   auto ValidateFactory = MoveTemp(FactoryContribution.ValidateInvocation);
   FactoryContribution.ValidateInvocation = [Capture, ValidateFactory = MoveTemp(ValidateFactory)]
    (const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles) mutable
   {
    if (!IsContextValid() || !Capture->bOpened || Capture->bClosed || Invocation.TransactionId != Capture->Transaction)
     return Failure(TEXT("MutationPreparation.InvalidCapture"), TEXT("Factory invocation requires the prepared live capture binding."));
    return ValidateFactory(Invocation, Handles);
   };
   Steps.Add(Intent.StepIndex);
   Prepared.Contributions.Add(MoveTemp(FactoryContribution));
   continue;
  }
  if (Intent.StepIndex < 0 || Steps.Contains(Intent.StepIndex) || Intent.Requirement.PrimitiveId != BatchPrimitive)
   return Failure(TEXT("MutationPreparation.UnsupportedIntent"), TEXT("Preparation only accepts exact admitted property batch steps."));
  if (Intent.Requirement.PrimitiveId != BatchPrimitive
   || Intent.Requirement.Version != 1 || Intent.Requirement.RequiredByRouteId.IsEmpty() || Intent.InputSchemaId != BatchSchema
   || (Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply && Intent.Phase != EAssetDocumentRuntimeRequestPhase::Repair))
   return Failure(TEXT("MutationPreparation.UnsupportedIntent"), TEXT("Preparation only accepts exact admitted property batch steps."));
  Steps.Add(Intent.StepIndex);
  TArray<FAssetDocumentEditorPropertyBatchTokenFieldView> Fields;
  auto Parsed = ParseFields(Intent.Intent, *Class, Fields);
  if (!Parsed.bValid) return Parsed;
  FAssetDocumentPreparedMutationContribution Contribution;
  Contribution.StepIndex = Intent.StepIndex;
  for (const auto& Field : Fields)
  {
   const FString Surface = Request.TargetAssetPath + TEXT("::") + Field.Property->GetName();
   if (Surfaces.Contains(Surface)) return Failure(TEXT("MutationPreparation.OverlappingSurface"), TEXT("A physical property surface has multiple prepared writers."));
   Surfaces.Add(Surface);
   Contribution.OwnedSurfaceIds.Add(Surface);
  }
  if (!Fields.IsEmpty()) Contribution.RawFootprint = BuildAssetDocumentEditorPropertyMutationFootprint(Canonical);
  if (!Identity->bCreate)
  {
   // Declared (non-edit) fields are restored by the Core package snapshot of
   // the existing target, not by a typed property preimage.
   TArray<FAssetDocumentEditorPropertyBatchTokenFieldView> PreimageFields = Fields.FilterByPredicate(
    [](const FAssetDocumentEditorPropertyBatchTokenFieldView& Field) { return !Field.bDeclared; });
   auto Preimage = CaptureAssetDocumentEditorAdmittedPropertyPreimage(*Target, PreimageFields);
   if (!Preimage.bSucceeded)
   {
    FAssetDocumentContractValidationResult Result;
    Result.Diagnostics = MoveTemp(Preimage.Diagnostics);
    return Result;
   }
   Contribution.InitialAction = MakeShared<FFieldsAction>(MoveTemp(Preimage.Preimage), Capture);
  }
  // Create has no target yet: the CDO carries only the admitted field/type
  // identity for validation, never a rollback preimage of the created object.
  UObject* TokenTarget = Target ? Target : Class->GetDefaultObject(false);
  if (!TokenTarget) return Failure(TEXT("MutationPreparation.InvalidClass"), TEXT("The admitted class must have an existing default object."));
  auto Frozen = MakeShared<FAssetDocumentEditorPropertyBatchTokenPayload>(*TokenTarget, Fields);
  Contribution.ValidateInvocation = [Identity, Capture, Frozen, Intent, RequestId = Request.RequestId, Revision = Request.ExpectedRuntimeRevision]
   (const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles)
  {
   return ValidateLive(Invocation, Handles, Intent, RequestId, Revision, *Identity, *Capture, *Frozen);
  };
  Prepared.Contributions.Add(MoveTemp(Contribution));
 }
 OutBatch = MoveTemp(Prepared);
 return Success();
}
