// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompressionDocumentInputs.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompressionDefaultInputs.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompressionState.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentSequenceCompressionDocumentInputs
{
namespace
{
namespace Defaults = AssetDocumentSequenceCompressionDefaultInputs;
namespace Compression = AssetDocumentSequenceCompression;
namespace Retained = Compression::Retained;
constexpr const TCHAR* ReferenceFields[] = {
 TEXT("BoneCompressionSettings"), TEXT("CurveCompressionSettings"), TEXT("VariableFrameStrippingSettings")};
FAssetDocumentContractValidationResult Reject(const TCHAR* Reason)
{
 auto R = Compression::Result(false, TEXT("AnimationProvider.SequenceCompression.DocumentInputInvalid"));
 R.Diagnostics[0].Path = TEXT("/Compression/DocumentInputs"); R.Diagnostics[0].Message = Reason; return R;
}
const FAssetDocumentRuntimeValueDto* Member(const FAssetDocumentRuntimeValueDto& V, const TCHAR* Name)
{
 const int32 Index = V.ObjectKeys.IndexOfByPredicate([&](const FString& Key) { return Key.Equals(Name, ESearchCase::CaseSensitive); });
 return Index == INDEX_NONE ? nullptr : &V.Elements[Index];
}
int32 ReferenceIndex(const FString& Name)
{
 for (int32 I = 0; I < 3; ++I) if (Name.Equals(ReferenceFields[I], ESearchCase::CaseSensitive)) return I;
 return INDEX_NONE;
}
struct FProjection
{
 const FAssetDocumentRuntimeValueDto* Supplied = nullptr;
 const FAssetDocumentRuntimeValueDto* References = nullptr;
 TArray<Defaults::EKind> Clear;
};
bool Parse(const FAssetDocumentRuntimeValueDto& V, FProjection& Out)
{
 if (!ValidateAssetDocumentRuntimeValue(V).bValid || V.Kind != EAssetDocumentRuntimeValueKind::Object || V.ObjectKeys.Num() != 3) return false;
 const auto* Supplied = Member(V, TEXT("supplied_values")); const auto* References = Member(V, TEXT("reference_fragments"));
 const auto* Clear = Member(V, TEXT("native_default_fields"));
 if (!Supplied || !References || !Clear || Supplied->Kind != EAssetDocumentRuntimeValueKind::Object
  || References->Kind != EAssetDocumentRuntimeValueKind::Object || Clear->Kind != EAssetDocumentRuntimeValueKind::Array) return false;
 for (int32 I = 0; I < Supplied->ObjectKeys.Num(); ++I)
 {
  const auto& Key = Supplied->ObjectKeys[I]; const auto& Value = Supplied->Elements[I];
  if (Key.Equals(TEXT("CompressionErrorThresholdScale"), ESearchCase::CaseSensitive))
  { if (Value.Kind != EAssetDocumentRuntimeValueKind::Number || !FMath::IsFinite(Value.NumberValue) || Value.NumberValue < 0.) return false; }
  else if (!Key.Equals(TEXT("bDoNotOverrideCompression"), ESearchCase::CaseSensitive) || Value.Kind != EAssetDocumentRuntimeValueKind::Boolean) return false;
 }
 bool Seen[3] = {};
 for (int32 I = 0; I < References->ObjectKeys.Num(); ++I)
 {
  const int32 Index = ReferenceIndex(References->ObjectKeys[I]); const auto& Fragment = References->Elements[I];
  if (Index == INDEX_NONE || Seen[Index] || Fragment.Kind != EAssetDocumentRuntimeValueKind::Object
   || Fragment.ObjectKeys.Num() < 2 || Fragment.ObjectKeys.Num() > 3) return false;
  Seen[Index] = true;
  const auto* Kind = Member(Fragment, TEXT("Kind")); const auto* Path = Member(Fragment, TEXT("Path"));
  if (!Kind || Kind->Kind != EAssetDocumentRuntimeValueKind::String || !Kind->StringValue.Equals(TEXT("AssetRef"), ESearchCase::CaseSensitive)
   || !Path || Path->Kind != EAssetDocumentRuntimeValueKind::String || Path->StringValue.TrimStartAndEnd().IsEmpty()) return false;
  for (const auto& Key : Fragment.ObjectKeys)
   if (!Key.Equals(TEXT("Kind"), ESearchCase::CaseSensitive) && !Key.Equals(TEXT("Path"), ESearchCase::CaseSensitive)
    && !Key.Equals(TEXT("Class"), ESearchCase::CaseSensitive)) return false;
  if (const auto* Hint = Member(Fragment, TEXT("Class")); Hint && Hint->Kind != EAssetDocumentRuntimeValueKind::Null
   && Hint->Kind != EAssetDocumentRuntimeValueKind::String) return false;
 }
 for (const auto& Value : Clear->Elements)
 {
  if (Value.Kind != EAssetDocumentRuntimeValueKind::String) return false;
  const int32 Index = ReferenceIndex(Value.StringValue); if (Index == INDEX_NONE || Seen[Index]) return false;
  Seen[Index] = true; Out.Clear.Add(static_cast<Defaults::EKind>(Index));
 }
 if (Supplied->ObjectKeys.IsEmpty() && References->ObjectKeys.IsEmpty() && Out.Clear.IsEmpty()) return false;
 Out.Supplied = Supplied; Out.References = References; return true;
}
bool ValidOtherIntents(TConstArrayView<FAssetDocumentMutationIntent> Others, int32 StepIndex)
{
 TSet<FString> Surfaces; int32 Previous = INDEX_NONE;
 for (const auto& I : Others)
 {
  const auto& Id = I.Requirement.PrimitiveId;
  const bool bV2 = Id == TEXT("animation.sequence.complete_edit.v2") || Id == TEXT("animation.sequence.retarget_source_asset.set.v2");
  if ((!bV2 && Id != TEXT("animation.asset.preview_mesh.set.v1") && Id != TEXT("animation.sequence.retarget_source_name.set.v1"))
   || I.Requirement.Version != (bV2 ? 2u : 1u) || I.Requirement.RequiredByRouteId != TEXT("animation_profile")
   || I.StepIndex <= Previous || I.StepIndex == StepIndex || Surfaces.Contains(Id)
   || (I.Phase != EAssetDocumentRuntimeRequestPhase::Apply && I.Phase != EAssetDocumentRuntimeRequestPhase::Repair)
   || !ValidateAssetDocumentRuntimeValue(I.Intent).bValid) return false;
  Previous = I.StepIndex; Surfaces.Add(Id);
 }
 return true;
}
UObject* ResolveReference(const FAssetDocumentRuntimeValueDto& Fragment, UClass*& OutClass)
{
 OutClass = nullptr;
 // Selection uses only exported, non-loading CoreUObject helpers. Authored
 // fragments are never rewritten; actual loaded paths become supplied-v1.
 FString Selector = Member(Fragment, TEXT("Path"))->StringValue.TrimStartAndEnd();
 // FindChar also sees the storage terminator; only an earlier NUL is authored.
 int32 NulIndex;
 if (Selector.FindChar(TEXT('\0'), NulIndex) && NulIndex < Selector.Len()) return nullptr;
 if (FPackageName::IsValidLongPackageName(FStringView(Selector), true))
 {
  const FString Name = FPackageName::GetLongPackageAssetName(Selector);
  Selector = FPackageName::ObjectPathCombine(FStringView(Selector), FStringView(Name));
 }
 if (!FPackageName::IsValidObjectPath(FStringView(Selector))) return nullptr;
 const FSoftObjectPath Path(Selector);
 if (Path.GetAssetName().IsEmpty() || !Path.GetSubPathUtf8String().IsEmpty() || !Path.ToString().Equals(Selector, ESearchCase::CaseSensitive)) return nullptr;
 UObject* Object = Path.ResolveObject();
 if (!Retained::Loaded(Object) || !Object->GetPathName().Equals(Selector, ESearchCase::CaseSensitive)) return nullptr;
 if (const auto* Class = Member(Fragment, TEXT("Class")); Class && Class->Kind == EAssetDocumentRuntimeValueKind::String)
 {
  const FString HintPath = Class->StringValue.TrimStartAndEnd();
  if (!HintPath.IsEmpty())
  {
   if ((HintPath.FindChar(TEXT('\0'), NulIndex) && NulIndex < HintPath.Len()) || !FPackageName::IsValidObjectPath(FStringView(HintPath))) return nullptr;
   const FSoftClassPath Hint(HintPath);
   if (Hint.GetAssetName().IsEmpty() || !Hint.GetSubPathUtf8String().IsEmpty() || !Hint.ToString().Equals(HintPath, ESearchCase::CaseSensitive)) return nullptr;
   UClass* Type = Hint.ResolveClass();
   if (!Retained::Loaded(Type) || !Retained::Loaded(Type->GetOutermost()) || !Type->GetPathName().Equals(HintPath, ESearchCase::CaseSensitive) || !Object->IsA(Type)) return nullptr;
   OutClass = Type;
  }
 }
 return Object;
}
}
FAssetDocumentContractValidationResult Prepare(
 const FAssetDocumentRuntimeRequestDto& Request, IAssetDocumentAnimationRequestProvider& Owner,
 const FAssetDocumentRuntimeValueDto& DocumentInputs, int32 StepIndex, EAssetDocumentRuntimeRequestPhase Phase,
 TConstArrayView<FAssetDocumentMutationIntent> OtherIntents,
 TArray<FAssetDocumentMutationIntent>& OutActualIntents, FAssetDocumentPreparedMutationBatch& OutBatch)
{
 OutActualIntents.Reset(); OutBatch = {};
 FProjection Projection;
 if (StepIndex < 0 || Owner.GetProviderId() != TEXT("animation") || !Parse(DocumentInputs, Projection) || !ValidOtherIntents(OtherIntents, StepIndex))
  return Reject(TEXT("Closed nonempty document snapshot and ordered distinct admitted Sequence occurrences required."));
 FAssetDocumentRuntimeValueDto Fields; TSharedPtr<Defaults::FInputs> Inputs;
 const auto Resolved = Defaults::Resolve(Request, Projection.Clear, StepIndex, Phase, Fields, Inputs); if (!Resolved.bValid) return Resolved;
 for (int32 I = 0; I < Projection.References->ObjectKeys.Num(); ++I)
 {
  const auto& Name = Projection.References->ObjectKeys[I]; UClass* Class = nullptr;
  UObject* Object = ResolveReference(Projection.References->Elements[I], Class);
  const auto RetainedInput = Defaults::RetainReference(Inputs, static_cast<Defaults::EKind>(ReferenceIndex(Name)), Object, Class);
  if (!RetainedInput.bValid) return RetainedInput;
  Fields.ObjectKeys.Add(Name); auto& Literal = Fields.Elements.AddDefaulted_GetRef(); Literal.Kind = EAssetDocumentRuntimeValueKind::String; Literal.StringValue = Object->GetPathName();
 }
 Fields.ObjectKeys.Append(Projection.Supplied->ObjectKeys); Fields.Elements.Append(Projection.Supplied->Elements);
 FAssetDocumentMutationIntent CompressionIntent; CompressionIntent.StepIndex = StepIndex;
 CompressionIntent.Requirement = {Compression::Primitive, 1, TEXT("animation_profile")}; CompressionIntent.Phase = Phase; CompressionIntent.InputSchemaId = Compression::Schema;
 CompressionIntent.Intent.Kind = EAssetDocumentRuntimeValueKind::Object; CompressionIntent.Intent.ObjectKeys = {TEXT("fields")}; CompressionIntent.Intent.Elements.Add(MoveTemp(Fields));
 TArray<FAssetDocumentMutationIntent> Actual; bool bInserted = false;
 for (const auto& Other : OtherIntents)
 {
  if (!bInserted && Other.StepIndex > StepIndex) { Actual.Add(CompressionIntent); bInserted = true; }
  Actual.Add(Other);
 }
 if (!bInserted) Actual.Add(MoveTemp(CompressionIntent));
 FAssetDocumentPreparedMutationBatch Batch;
 // Exactly one preparation through the caller's already-bound production
 // owner. Guard the original batch; never prepare twice or patch its Intents.
 const auto Prepared = Owner.Prepare(Request, Actual, Batch); if (!Prepared.bValid) return Prepared;
 const auto Guarded = Defaults::GuardPreparedBatch(Inputs, StepIndex, Batch); if (!Guarded.bValid) return Guarded;
 OutActualIntents = MoveTemp(Actual); OutBatch = MoveTemp(Batch); return Prepared;
}
}
