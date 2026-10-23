// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Sequence/AssetDocumentAnimationSequenceReferencesDocumentInputs.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceNameState.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Misc/PackageName.h"
#include "Utils/ClassFinderUtils.h"

namespace AssetDocumentSequenceReferencesDocumentInputs
{
namespace
{
namespace Retained = AssetDocumentSequenceRetargetSource;
namespace Named = AssetDocumentSequenceRetargetSourceName;
using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;
FAssetDocumentContractValidationResult Reject(const TCHAR* Message,
 const TCHAR* Code = TEXT("AnimationProvider.SequenceReferences.DocumentInputInvalid"))
{
 auto R = Retained::Result(false, Code);
 R.Diagnostics[0].Path = TEXT("/References/DocumentInputs"); R.Diagnostics[0].Message = Message; return R;
}
const FValue* Member(const FValue& V, const TCHAR* Name)
{
 const int32 I = V.ObjectKeys.IndexOfByPredicate([&](const FString& K) { return K.Equals(Name, ESearchCase::CaseSensitive); });
 return I == INDEX_NONE ? nullptr : &V.Elements[I];
}
FValue String(const FString& S)
{ FValue V; V.Kind = EKind::String; V.StringValue = S; return V; }
bool HasNul(const FString& S)
{ int32 I; return S.FindChar(TEXT('\0'), I) && I < S.Len(); }
bool Ready()
{
 return Retained::ContextValid() && GIsRunning && !IsEngineExitRequested() && !IsRunningCommandlet()
  && IsValid(GEngine) && GEngine->IsInitialized();
}
struct FProjection
{
 const FValue* Name = nullptr;
 const FValue* References = nullptr;
 const FValue* Definitions = nullptr;
 bool DefaultName = false, DefaultAsset = false, ExplicitClear = false;
};
struct FExactDefinitionIdKeyFuncs : DefaultKeyFuncs<FString>
{
 static bool Matches(KeyInitType A, KeyInitType B)
 { return A.Len() == B.Len() && FMemory::Memcmp(*A, *B, SIZE_T(A.Len()) * sizeof(TCHAR)) == 0; }
};
bool Parse(const FValue& V, FProjection& P)
{
 if (!ValidateAssetDocumentRuntimeValue(V).bValid || V.Kind != EKind::Object || V.ObjectKeys.Num() != 6) return false;
 const auto* Present = Member(V, TEXT("region_present")); P.Name = Member(V, TEXT("supplied_name"));
 P.References = Member(V, TEXT("reference_fragments")); P.Definitions = Member(V, TEXT("definitions"));
 const auto* Clear = Member(V, TEXT("explicit_asset_clear")); const auto* Defaults = Member(V, TEXT("native_default_fields"));
 if (!Present || !P.Name || !P.References || !P.Definitions || !Clear || !Defaults
  || Present->Kind != EKind::Boolean || Clear->Kind != EKind::Boolean || P.References->Kind != EKind::Object
  || P.Definitions->Kind != EKind::Object || Defaults->Kind != EKind::Array
  || (P.Name->Kind != EKind::Null && P.Name->Kind != EKind::String)) return false;
 if (P.Name->Kind == EKind::String && (HasNul(P.Name->StringValue) || P.Name->StringValue.Len() >= NAME_SIZE)) return false;
 for (const auto& K : P.References->ObjectKeys)
  if (!K.Equals(TEXT("Skeleton"), ESearchCase::CaseSensitive) && !K.Equals(TEXT("RetargetSourceAsset"), ESearchCase::CaseSensitive)) return false;
 P.DefaultName = P.Name->Kind == EKind::Null; P.ExplicitClear = Clear->BooleanValue;
 const bool Asset = Member(*P.References, TEXT("RetargetSourceAsset")) != nullptr;
 if (Asset && P.ExplicitClear) return false;
 P.DefaultAsset = !Asset && !P.ExplicitClear;
 TArray<FString> Expected;
 if (P.DefaultName) Expected.Add(TEXT("RetargetSource"));
 if (P.DefaultAsset) Expected.Add(TEXT("RetargetSourceAsset"));
 if (Defaults->Elements.Num() != Expected.Num()) return false;
 for (int32 I = 0; I < Expected.Num(); ++I)
  if (Defaults->Elements[I].Kind != EKind::String || !Defaults->Elements[I].StringValue.Equals(Expected[I], ESearchCase::CaseSensitive)) return false;
 return Present->BooleanValue || (P.DefaultName && P.DefaultAsset && P.References->ObjectKeys.IsEmpty());
}
FAssetDocumentContractValidationResult Terminal(const FValue& Start, const FValue& Definitions, const FValue*& Out)
{
 Out = nullptr; const FValue* F = &Start; TSet<FString, FExactDefinitionIdKeyFuncs> Seen;
 while (F->Kind == EKind::Object)
 {
  const auto* Kind = Member(*F, TEXT("Kind")); if (!Kind || Kind->Kind != EKind::String) break;
  if (Kind->StringValue.Equals(TEXT("AssetRef"), ESearchCase::IgnoreCase))
  {
   const auto* Path = Member(*F, TEXT("Path"));
   if (!Path || Path->Kind != EKind::String || Path->StringValue.TrimStartAndEnd().IsEmpty()) break;
   Out = F; return Retained::Result(true);
  }
  if (!Kind->StringValue.Equals(TEXT("DefinitionRef"), ESearchCase::IgnoreCase)) break;
  const auto* Id = Member(*F, TEXT("Id"));
  if (!Id || Id->Kind != EKind::String || Id->StringValue.TrimStartAndEnd().IsEmpty())
   return Reject(TEXT("DefinitionRef requires a nonempty original Id."), TEXT("AnimationProvider.SequenceReferences.DefinitionIdMissing"));
  if (Seen.Contains(Id->StringValue)) return Reject(TEXT("The exact DefinitionRef chain is cyclic."), TEXT("AnimationProvider.SequenceReferences.DefinitionCycle"));
  Seen.Add(Id->StringValue);
  const int32 I = Definitions.ObjectKeys.IndexOfByPredicate([&](const FString& K) { return FExactDefinitionIdKeyFuncs::Matches(K, Id->StringValue); });
  if (I == INDEX_NONE) return Reject(TEXT("The original exact Definition Id is absent."), TEXT("AnimationProvider.SequenceReferences.DefinitionMissing"));
  F = &Definitions.Elements[I];
 }
 return Reject(TEXT("A reference must terminate in an AssetRef with nonempty Path."), TEXT("AnimationProvider.SequenceReferences.InvalidReferenceTerminal"));
}
bool ValidOthers(TConstArrayView<FAssetDocumentMutationIntent> Others, int32 NameStep, int32 AssetStep)
{
 int32 Previous = INDEX_NONE; TSet<FString> Surfaces;
 for (const auto& I : Others)
 {
  const auto& Id = I.Requirement.PrimitiveId; const bool V2 = Id == TEXT("animation.sequence.complete_edit.v2");
  if ((!V2 && Id != TEXT("animation.asset.preview_mesh.set.v1") && Id != TEXT("animation.sequence.compression.settings.set.v1"))
   || I.Requirement.Version != (V2 ? 2u : 1u) || I.Requirement.RequiredByRouteId != TEXT("animation_profile")
   || I.StepIndex <= Previous || I.StepIndex == NameStep || I.StepIndex == AssetStep || Surfaces.Contains(Id)
   || (I.Phase != EAssetDocumentRuntimeRequestPhase::Apply && I.Phase != EAssetDocumentRuntimeRequestPhase::Repair)
   || !ValidateAssetDocumentRuntimeValue(I.Intent).bValid) return false;
  Previous = I.StepIndex; Surfaces.Add(Id);
 }
 return true;
}
bool ValidClass(const UClass* C)
{
 return Retained::Loaded(C) && Retained::Loaded(C->GetOutermost()) && C->IsChildOf(UObject::StaticClass())
  && !C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
}
FAssetDocumentContractValidationResult ResolveReadonly(IAssetDocumentNativeRequestContext& Context,
 const FGuid& Transaction, const TCHAR* Primitive, const TCHAR* FirstKey, FValue First,
 const TCHAR* SecondKey, FValue Second, FValue& Out)
{
 FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = Primitive; I.Version = 1;
 const auto& M = Context.GetMetadata(); I.RequestId = M.RequestId; I.TransactionId = Transaction;
 I.RuntimeRevision = M.PinnedRuntimeRevision; I.Phase = M.Phase;
 I.ValueArguments.Kind = EKind::Object; I.ValueArguments.ObjectKeys = {FirstKey, SecondKey}; I.ValueArguments.Elements = {MoveTemp(First), MoveTemp(Second)};
 const auto Catalog = Context.InspectCatalog();
 const auto* D = Catalog.Descriptors.FindByPredicate([&](const auto& V) { return V.PrimitiveId == Primitive && V.Version == 1; });
 if (!D || D->ProviderId != TEXT("editor") || D->Access != EAssetDocumentPrimitiveAccess::ReadOnly)
  return Reject(TEXT("The original readonly resolver is absent from this request catalog."));
 const auto R = Context.Invoke(I);
 if (R.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
 {
  auto Failure = Reject(TEXT("The original readonly resolver refused this reference."));
  if (!R.Diagnostics.IsEmpty()) Failure.Diagnostics = R.Diagnostics;
  return Failure;
 }
 // This is the same original registry/issuance path, not a fabricated handle
 // or a second loader. Validate its returned request/transaction/revision/type.
 if (!ValidateAssetDocumentPrimitiveOutcome(I, *D, R).bValid || !R.ChangedSet.IsEmpty())
  return Reject(TEXT("Readonly resolution returned an invalid identity or a changed set."));
 Out = R.Value; return Retained::Result(true);
}
FAssetDocumentContractValidationResult ResolveReference(const FValue& Fragment, UClass* Expected,
 IAssetDocumentNativeRequestContext& Context, const FGuid& Transaction, UObject*& OutObject, UClass*& OutClass)
{
 OutObject = nullptr; OutClass = nullptr;
 FString Selector = Member(Fragment, TEXT("Path"))->StringValue.TrimStartAndEnd();
 if (HasNul(Selector)) return Reject(TEXT("An authored reference contains NUL."));
 Selector = FPackageName::ExportTextPathToObjectPath(Selector);
 if (Selector.StartsWith(TEXT("/")) && !Selector.Contains(TEXT(".")))
 {
  const FString Name = FPackageName::GetLongPackageAssetName(Selector);
  Selector = FPackageName::ObjectPathCombine(FStringView(Selector), FStringView(Name));
 }
 const auto* Hint = Member(Fragment, TEXT("Class"));
 if (Hint && Hint->Kind == EKind::String && !Hint->StringValue.TrimStartAndEnd().IsEmpty())
 {
  FString ClassPath = Hint->StringValue.TrimStartAndEnd();
  if (HasNul(ClassPath)) return Reject(TEXT("An active Class assertion contains NUL."));
  if (!ClassPath.StartsWith(TEXT("/")))
  {
   // Preserve the legacy public alias helper (U/A prefixes, project/native and
   // Blueprint lookup). This readonly lookup is confined to Preflight; its
   // canonical result still goes through the original class resolver below.
   UClass* Alias = FClassFinderUtils::FindClassByName(ClassPath, UObject::StaticClass(), true);
   if (!ValidClass(Alias)) return Reject(TEXT("The active Class alias did not resolve to a valid class."));
   ClassPath = Alias->GetPathName();
  }
  FValue Value;
  auto R = ResolveReadonly(Context, Transaction, TEXT("editor.class.resolve_canonical.v1"),
   TEXT("class"), String(ClassPath), TEXT("expected_base"), FValue(), Value);
  if (!R.bValid) return R;
  const auto* Canonical = Member(Value, TEXT("canonical_class"));
  if (!Canonical || Canonical->Kind != EKind::String || Canonical->StringValue != ClassPath) return Reject(TEXT("Class identity readback mismatched."));
  OutClass = FSoftClassPath(ClassPath).ResolveClass();
  if (!ValidClass(OutClass) || OutClass->GetPathName() != ClassPath) return Reject(TEXT("Resolved Class is unavailable or invalid."));
 }
 FValue Value;
 auto R = ResolveReadonly(Context, Transaction, TEXT("editor.reference.resolve_asset.v1"),
  TEXT("expected_class"), String(Expected->GetPathName()), TEXT("path"), String(Selector), Value);
 if (!R.bValid) return R;
 const auto* Path = Member(Value, TEXT("canonical_path")); const auto* Class = Member(Value, TEXT("canonical_class"));
 if (!Path || !Class || Path->Kind != EKind::String || Class->Kind != EKind::String || Path->StringValue != Selector)
  return Reject(TEXT("Resolved asset identity readback mismatched."));
 OutObject = FSoftObjectPath(Path->StringValue).ResolveObject();
 if (!Retained::CanonicalAsset(OutObject, Expected, true) || OutObject->GetPathName() != Selector
  || OutObject->GetClass()->GetPathName() != Class->StringValue || (OutClass && !OutObject->IsA(OutClass)))
  return Reject(TEXT("The loaded reference does not match its expected base or active Class assertion."));
 return Retained::Result(true);
}
struct FClassInput
{
 Retained::FIdentity Identity;
 EClassFlags Flags;
 UClass* Super;
 explicit FClassInput(UClass& C) : Identity(&C), Flags(C.GetClassFlags()), Super(C.GetSuperClass()) {}
 bool Current() const
 {
  const auto* C = Cast<UClass>(Identity.Object.Get());
  return Identity.Current() && ValidClass(C) && C->GetClassFlags() == Flags && C->GetSuperClass() == Super
   && FSoftClassPath(Identity.Path).ResolveClass() == C;
 }
};
struct FDefaultInput
{
 Retained::FIdentity Identity;
 FName Name;
 FSoftObjectPath Asset;
 bool Dirty;
 explicit FDefaultInput(UAnimSequence& C) : Identity(&C), Name(C.RetargetSource), Asset(C.GetRetargetSourceAsset().ToSoftObjectPath()), Dirty(C.GetOutermost()->IsDirty()) {}
 bool Current() const
 {
  const auto* C = Cast<UAnimSequence>(Identity.Object.Get());
  return Identity.Current() && C->GetClass() == UAnimSequence::StaticClass() && C->HasAnyFlags(RF_ClassDefaultObject)
   && UAnimSequence::StaticClass()->GetDefaultObject(false) == C && Name.IsNone() && Asset.IsNull()
   && Named::ExactName(C->RetargetSource, Name) && C->GetRetargetSourceAsset().ToSoftObjectPath().ToString() == Asset.ToString()
   && C->GetOutermost()->IsDirty() == Dirty;
 }
};
struct FInputs
{
 Retained::FIdentity Target;
 Retained::FSkeletonInput Skeleton;
 TUniquePtr<Retained::FMeshInput> Mesh;
 TUniquePtr<FDefaultInput> Defaults;
 TArray<FClassInput> Classes;
 bool Opened = false;
 explicit FInputs(UAnimSequence& S) : Target(&S), Skeleton(*S.GetSkeleton(), S.RetargetSource) {}
 bool Current() const
 {
  const auto* S = Cast<UAnimSequence>(Target.Object.Get()); const auto* K = Cast<USkeleton>(Skeleton.Identity.Object.Get());
  if (!Ready() || !Target.Current() || !Retained::CanonicalSequence(S) || S->GetSkeleton() != K || !Skeleton.Current()
   || (Defaults && !Defaults->Current()) || (Mesh && !Mesh->Current(*K))) return false;
  for (const auto& C : Classes) if (!C.Current()) return false;
  return true;
 }
};
class FGuardedInverse final : public IAssetDocumentRollbackAction
{
 TSharedRef<FInputs> Inputs;
 TSharedPtr<IAssetDocumentRollbackAction> Original;
 bool Consumed = false;
public:
 FGuardedInverse(TSharedRef<FInputs> In, TSharedPtr<IAssetDocumentRollbackAction> Action) : Inputs(MoveTemp(In)), Original(MoveTemp(Action)) {}
 FStringView GetRollbackComponentId() const override { return Original->GetRollbackComponentId(); }
 FAssetDocumentContractValidationResult Rollback(FStringView Journal) override
 {
  if (Consumed) return Reject(TEXT("The original guarded inverse is already consumed."));
  Consumed = true;
  if (Inputs->Opened && !Inputs->Current()) return Reject(TEXT("Readonly document inputs drifted before the original inverse."), TEXT("AnimationProvider.SequenceReferences.ReadonlyInputDrift"));
  // A refused opener has no owned write; let the original action retain its
  // existing zero-write refusal semantics rather than poison a safe abort.
  const auto R = Original->Rollback(Journal);
  return !Inputs->Opened || Inputs->Current() ? R : Reject(TEXT("Readonly inputs drifted during the original inverse."), TEXT("AnimationProvider.SequenceReferences.ReadonlyInputDrift"));
 }
};
FAssetDocumentContractValidationResult Guard(const TSharedRef<FInputs>& Inputs, int32 NameStep, int32 AssetStep, FAssetDocumentPreparedMutationBatch& Batch)
{
 if (!Inputs->Current() || !Batch.OpenMutationCapture || Batch.PackageFinalizer.IsValid()) return Reject(TEXT("Original prepared batch or readonly inputs are unavailable."));
 for (int32 Step : {NameStep, AssetStep})
 {
  int32 Count = 0; for (const auto& C : Batch.Contributions) if (C.StepIndex == Step) ++Count;
  auto* C = Batch.Contributions.FindByPredicate([&](const auto& V) { return V.StepIndex == Step; });
  if (Count != 1 || !C || !C->InitialAction || !C->ValidateInvocation) return Reject(TEXT("Both original native contributions are required."));
 }
 auto Open = MoveTemp(Batch.OpenMutationCapture);
 Batch.OpenMutationCapture = [Inputs, Open = MoveTemp(Open)](const FGuid& Id, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics) mutable -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!Inputs->Current()) { Diagnostics.Append(Reject(TEXT("Readonly document inputs drifted before capture."), TEXT("AnimationProvider.SequenceReferences.ReadonlyInputDrift")).Diagnostics); return {}; }
  auto Lease = Open(Id, Diagnostics);
  if (Lease && !Inputs->Current()) { Lease->Close(); Diagnostics.Append(Reject(TEXT("Readonly document inputs drifted while opening capture.")).Diagnostics); return {}; }
  if (Lease && Lease->IsValid()) Inputs->Opened = true;
  return Lease;
 };
 for (int32 Step : {NameStep, AssetStep})
 {
  auto& C = *Batch.Contributions.FindByPredicate([&](const auto& V) { return V.StepIndex == Step; });
  C.InitialAction = MakeShared<FGuardedInverse>(Inputs, MoveTemp(C.InitialAction));
  auto Validate = MoveTemp(C.ValidateInvocation);
  C.ValidateInvocation = [Inputs, Validate = MoveTemp(Validate)](const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& H) mutable
  {
   if (!Inputs->Current()) return Reject(TEXT("Readonly document inputs drifted before invocation."), TEXT("AnimationProvider.SequenceReferences.ReadonlyInputDrift"));
   const auto R = Validate(I, H);
   return Inputs->Current() ? R : Reject(TEXT("Readonly document inputs drifted during invocation validation."), TEXT("AnimationProvider.SequenceReferences.ReadonlyInputDrift"));
  };
 }
 return Retained::Result(true);
}
}

FAssetDocumentContractValidationResult Prepare(
 const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentAnimationRequestProvider& Owner,
 IAssetDocumentNativeRequestContext& Context, const FGuid& ReadonlyTransaction, const FValue& DocumentInputs,
 int32 NameStep, int32 AssetStep, EAssetDocumentRuntimeRequestPhase Phase,
 TConstArrayView<FAssetDocumentMutationIntent> OtherIntents,
 TArray<FAssetDocumentMutationIntent>& OutActualIntents, FAssetDocumentPreparedMutationBatch& OutBatch)
{
 OutActualIntents.Reset(); OutBatch = {}; FProjection P;
 const auto& M = Context.GetMetadata();
 if (!Ready() || !Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0 || Request.Operation != EAssetDocumentRuntimeOperation::Apply
  || Request.RequestedAssetClassPath != TEXT("/Script/Engine.AnimSequence") || !Retained::CanonicalPath(Request.TargetAssetPath)
  || Owner.GetProviderId() != TEXT("animation") || M.RequestId != Request.RequestId || M.PinnedRuntimeRevision != Request.ExpectedRuntimeRevision
  || M.Phase != EAssetDocumentRuntimeRequestPhase::Preflight || !ReadonlyTransaction.IsValid()
  || NameStep < 0 || AssetStep < 0 || NameStep == AssetStep || (Phase != EAssetDocumentRuntimeRequestPhase::Apply && Phase != EAssetDocumentRuntimeRequestPhase::Repair)
  || !Parse(DocumentInputs, P) || !ValidOthers(OtherIntents, NameStep, AssetStep)) return Reject(TEXT("Exact six-field snapshot and original request/Preflight context required."));
 auto* Target = Cast<UAnimSequence>(FSoftObjectPath(Request.TargetAssetPath).ResolveObject());
 if (!Retained::CanonicalSequence(Target) || Target->GetPathName() != Request.TargetAssetPath) return Reject(TEXT("The exact already-loaded UAnimSequence target is unavailable."));
 // Validate every selected chain before any loader or owner preparation.
 TArray<const FValue*> Fragments;
 for (const auto& F : P.References->Elements)
 { const FValue* T = nullptr; const auto Valid = Terminal(F, *P.Definitions, T); if (!Valid.bValid) return Valid; Fragments.Add(T); }
 auto Inputs = MakeShared<FInputs>(*Target);
 if (P.DefaultName || P.DefaultAsset)
 {
  auto* C = Cast<UAnimSequence>(UAnimSequence::StaticClass()->GetDefaultObject(false));
  if (!Retained::Loaded(C) || C->GetClass() != UAnimSequence::StaticClass() || !C->HasAnyFlags(RF_ClassDefaultObject))
   return Reject(TEXT("The already-existing exact AnimSequence CDO is unavailable."), TEXT("AnimationProvider.SequenceReferences.DefaultUnavailable"));
  Inputs->Defaults = MakeUnique<FDefaultInput>(*C);
  if (!Inputs->Defaults->Current()) return Reject(TEXT("The actual CDO does not select NAME_None and an empty asset."), TEXT("AnimationProvider.SequenceReferences.DefaultUnavailable"));
 }
 USkeletalMesh* Mesh = nullptr;
 for (int32 I = 0; I < Fragments.Num(); ++I)
 {
  const bool Skeleton = P.References->ObjectKeys[I].Equals(TEXT("Skeleton"), ESearchCase::CaseSensitive);
  UObject* Object = nullptr; UClass* Declared = nullptr;
  auto R = ResolveReference(*Fragments[I], Skeleton ? USkeleton::StaticClass() : USkeletalMesh::StaticClass(), Context, ReadonlyTransaction, Object, Declared);
  if (!R.bValid) return R;
  if (Skeleton && Object != Target->GetSkeleton()) return Reject(TEXT("Explicit Skeleton must assert the target's original identity."), TEXT("AnimationProvider.SequenceReferences.SkeletonMismatch"));
  if (!Skeleton)
  {
   Mesh = Cast<USkeletalMesh>(Object);
   if (!Retained::CanonicalMesh(Mesh, *Target->GetSkeleton()) || Object->GetOutermost() == Target->GetOutermost()) return Reject(TEXT("Resolved Mesh is not a compatible readonly input."));
   Inputs->Mesh = MakeUnique<Retained::FMeshInput>(*Mesh, *Target->GetSkeleton());
  }
  if (Declared) Inputs->Classes.Emplace(*Declared);
 }
 if (!Inputs->Current()) return Reject(TEXT("Readonly document inputs changed during resolution."));
 FString Name = P.DefaultName ? Inputs->Defaults->Name.ToString() : P.Name->StringValue;
 if (FName(*Name).IsNone()) Name = TEXT("None");
 FAssetDocumentMutationIntent NamedIntent, AssetIntent;
 NamedIntent.StepIndex = NameStep; NamedIntent.Requirement = {Named::Primitive, 1, TEXT("animation_profile")}; NamedIntent.Phase = Phase; NamedIntent.InputSchemaId = Named::Schema;
 NamedIntent.Intent.Kind = EKind::Object; NamedIntent.Intent.ObjectKeys = {TEXT("retarget_source_name")}; NamedIntent.Intent.Elements = {String(Name)};
 AssetIntent.StepIndex = AssetStep; AssetIntent.Requirement = {Retained::Primitive, 2, TEXT("animation_profile")}; AssetIntent.Phase = Phase; AssetIntent.InputSchemaId = Retained::Schema;
 AssetIntent.Intent.Kind = EKind::Object; AssetIntent.Intent.ObjectKeys = {TEXT("operation"), TEXT("retarget_source_asset_path")};
 AssetIntent.Intent.Elements = {String(Mesh ? TEXT("set") : TEXT("clear")), Mesh ? String(Mesh->GetPathName()) : FValue()};
 TArray<FAssetDocumentMutationIntent> Actual; Actual.Append(OtherIntents); Actual.Add(MoveTemp(NamedIntent)); Actual.Add(MoveTemp(AssetIntent));
 Actual.Sort([](const auto& A, const auto& B) { return A.StepIndex < B.StepIndex; });
 FAssetDocumentPreparedMutationBatch Batch;
 const auto Prepared = Owner.Prepare(Request, Actual, Batch); if (!Prepared.bValid) return Prepared;
 const auto Guarded = Guard(Inputs, NameStep, AssetStep, Batch); if (!Guarded.bValid) return Guarded;
 OutActualIntents = MoveTemp(Actual); OutBatch = MoveTemp(Batch); return Prepared;
}
}

FAssetDocumentContractValidationResult PrepareAssetDocumentAnimationSequenceReferencesDocumentBatch(
 const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentAnimationRequestProvider& Owner,
 IAssetDocumentNativeRequestContext& ReadonlyContext, const FGuid& ReadonlyTransaction,
 const FAssetDocumentRuntimeValueDto& DocumentInputs, int32 NameStepIndex, int32 AssetStepIndex,
 EAssetDocumentRuntimeRequestPhase Phase, TConstArrayView<FAssetDocumentMutationIntent> OtherIntents,
 TArray<FAssetDocumentMutationIntent>& OutActualIntents, FAssetDocumentPreparedMutationBatch& OutBatch)
{
 return AssetDocumentSequenceReferencesDocumentInputs::Prepare(Request, Owner, ReadonlyContext, ReadonlyTransaction,
  DocumentInputs, NameStepIndex, AssetStepIndex, Phase, OtherIntents, OutActualIntents, OutBatch);
}
