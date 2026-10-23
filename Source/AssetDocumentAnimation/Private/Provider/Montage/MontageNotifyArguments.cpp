// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageNotifyArguments.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentMontage
{
namespace
{
bool HasKeys(const FAssetDocumentRuntimeValueDto& Value, const TArray<FString>& Keys)
{
 return Value.Kind == EAssetDocumentRuntimeValueKind::Object
  && Value.ObjectKeys == Keys && Value.Elements.Num() == Keys.Num();
}

bool ReadNonnegativeFloat(const FAssetDocumentRuntimeValueDto& Value, float& Out)
{
 if (Value.Kind != EAssetDocumentRuntimeValueKind::Number && Value.Kind != EAssetDocumentRuntimeValueKind::Integer) return false;
 const double Number = Value.Kind == EAssetDocumentRuntimeValueKind::Integer ? double(Value.IntegerValue) : Value.NumberValue;
 if (!FMath::IsFinite(Number) || Number < 0.0 || Number > double(MAX_flt)) return false;
 Out = float(Number);
 return FMath::IsFinite(Out);
}

bool ReadTrackIndex(const FAssetDocumentRuntimeValueDto& Value, int32& Out)
{
 if (Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
 {
  if (Value.IntegerValue < 0 || Value.IntegerValue > MAX_int32) return false;
  Out = int32(Value.IntegerValue);
  return true;
 }
 if (Value.Kind != EAssetDocumentRuntimeValueKind::Number || !FMath::IsFinite(Value.NumberValue)
  || Value.NumberValue < 0.0 || Value.NumberValue > double(MAX_int32)) return false;
 Out = int32(Value.NumberValue);
 return double(Out) == Value.NumberValue;
}

UClass* ResolveNotifyClass(const FAssetDocumentRuntimeValueDto& Value, bool bStates)
{
 if (Value.Kind != EAssetDocumentRuntimeValueKind::String || !FPackageName::IsValidObjectPath(Value.StringValue)) return nullptr;
 const FSoftObjectPath Path(Value.StringValue);
 if (!Path.IsValid() || !Path.GetSubPathString().IsEmpty() || Path.ToString() != Value.StringValue) return nullptr;
 UClass* Class = Cast<UClass>(Path.ResolveObject());
 if (!IsValid(Class) || Class->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad)
  || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
  || Class->GetPathName() != Value.StringValue
  || !Class->IsChildOf(bStates ? UAnimNotifyState::StaticClass() : UAnimNotify::StaticClass())) return nullptr;
 return Class;
}
}

FAssetDocumentContractValidationResult ParseMontageNotifyArguments(
 const FAssetDocumentRuntimeValueDto& Value, bool bStates, TArray<FNotifyEdit>& OutEdits)
{
 OutEdits.Reset();
 if (!IsStructuralContext()) return Result(false, TEXT("AnimationProvider.Montage.InvalidContext"));
 if (!ValidateAssetDocumentRuntimeValue(Value).bValid
  || (bStates ? !HasKeys(Value, {TEXT("notify_states")})
   : (!HasKeys(Value, {TEXT("notifies")}) && !HasKeys(Value, {TEXT("notifies"), TEXT("notify_states")}))))
  return Result(false, TEXT("AnimationProvider.InvalidValueShape"));

 TArray<FNotifyEdit> Parsed;
 for (int32 Family = 0; Family < Value.Elements.Num(); ++Family)
 {
  const bool bRowStates = bStates || Family == 1;
  if (Value.Elements[Family].Kind != EAssetDocumentRuntimeValueKind::Array)
   return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
  const TArray<FString> RowKeys = bRowStates
   ? TArray<FString>{TEXT("time"), TEXT("duration"), TEXT("track_index"), TEXT("object")}
   : TArray<FString>{TEXT("time"), TEXT("track_index"), TEXT("object")};
  for (const auto& Row : Value.Elements[Family].Elements)
  {
   if (!HasKeys(Row, RowKeys)) return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
   const auto& Object = Row.Elements.Last();
   if (!HasKeys(Object, {TEXT("class"), TEXT("properties")})
    || Object.Elements[1].Kind != EAssetDocumentRuntimeValueKind::Object)
    return Result(false, TEXT("AnimationProvider.InvalidValueShape"));

   FNotifyEdit Edit;
   Edit.bState = bRowStates;
   if (!ReadNonnegativeFloat(Row.Elements[0], Edit.Time)
    || !ReadTrackIndex(Row.Elements[bRowStates ? 2 : 1], Edit.TrackIndex))
    return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
   if (bRowStates)
   {
    if (!ReadNonnegativeFloat(Row.Elements[1], Edit.Duration) || Edit.Duration <= 0.f)
     return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
    const float End = Edit.Time + Edit.Duration;
    if (!FMath::IsFinite(End) || End <= Edit.Time)
     return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
   }
   Edit.Class = ResolveNotifyClass(Object.Elements[0], bRowStates);
   if (!Edit.Class)
    return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"),
     TEXT("Notify class must be an already-loaded, canonical concrete class of the requested notify family."));
   Edit.Properties = Object.Elements[1];
   Parsed.Add(MoveTemp(Edit));
  }
 }
 OutEdits = MoveTemp(Parsed);
 return Result(true);
}
}
