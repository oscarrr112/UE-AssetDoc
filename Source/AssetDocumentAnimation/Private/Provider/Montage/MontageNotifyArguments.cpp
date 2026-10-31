// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageNotifyArguments.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Algo/Find.h"
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

const FAssetDocumentRuntimeValueDto* NotifyArgumentField(const FAssetDocumentRuntimeValueDto& Value, const TCHAR* Key)
{
 const int32 Index = Value.ObjectKeys.IndexOfByKey(Key);
 return Value.Elements.IsValidIndex(Index) ? &Value.Elements[Index] : nullptr;
}

template <typename TEnum>
bool ReadMontageNotifyEnum(const FAssetDocumentRuntimeValueDto& Value, TEnum& Out)
{
 if (Value.Kind != EAssetDocumentRuntimeValueKind::String) return false;
 const UEnum* Enum = StaticEnum<TEnum>();
 const int64 Raw = Enum->GetValueByNameString(Value.StringValue);
 if (Raw == INDEX_NONE || Enum->GetNameStringByValue(Raw) != Value.StringValue) return false;
 Out = TEnum(Raw);
 return true;
}

FAssetDocumentContractValidationResult NotifyArgumentFailure(const FString& Path, const FString& Message)
{
 auto Failed = Result(false, TEXT("AnimationProvider.InvalidValueShape"), Message);
 Failed.Diagnostics[0].Path = Path;
 return Failed;
}

FAssetDocumentContractValidationResult ReadNotifyTrigger(const FAssetDocumentRuntimeValueDto& Value, const FString& Path, FNotifyTrigger& Out)
{
 static const TCHAR* const Keys[] = {TEXT("TriggerWeightThreshold"), TEXT("NotifyTriggerChance"), TEXT("NotifyFilterType"),
  TEXT("NotifyFilterLOD"), TEXT("bCanBeFilteredViaRequest"), TEXT("bTriggerOnDedicatedServer"), TEXT("bTriggerOnFollower"), TEXT("MontageTickType")};
 if (Value.Kind != EAssetDocumentRuntimeValueKind::Object || Value.ObjectKeys.Num() != Value.Elements.Num())
  return NotifyArgumentFailure(Path, TEXT("A notify's trigger settings must be an object."));
 for (int32 I = 0; I < Value.ObjectKeys.Num(); ++I)
 {
  const FString& Key = Value.ObjectKeys[I];
  const FAssetDocumentRuntimeValueDto& Field = Value.Elements[I];
  const FString At = Path + TEXT("/") + Key;
  const bool bKnown = Algo::FindByPredicate(Keys, [&Key](const TCHAR* Candidate) { return Key == Candidate; }) != nullptr;
  if (!bKnown || Value.ObjectKeys.IndexOfByKey(Key) != I)
   return NotifyArgumentFailure(At, TEXT("Unknown or repeated notify trigger setting."));
  float Number = 0.f;
  if (Key == TEXT("TriggerWeightThreshold") || Key == TEXT("NotifyTriggerChance"))
  {
   const bool bNumber = Field.Kind == EAssetDocumentRuntimeValueKind::Number || Field.Kind == EAssetDocumentRuntimeValueKind::Integer;
   const double Raw = Field.Kind == EAssetDocumentRuntimeValueKind::Integer ? double(Field.IntegerValue) : Field.NumberValue;
   Number = float(Raw);
   if (!bNumber || !FMath::IsFinite(Raw) || FMath::Abs(Raw) > double(MAX_flt)
    || (Key == TEXT("NotifyTriggerChance") && (Number < 0.f || Number > 1.f)))
    return NotifyArgumentFailure(At, Key == TEXT("NotifyTriggerChance") ? TEXT("NotifyTriggerChance must be a number in [0, 1].")
     : TEXT("TriggerWeightThreshold must be a finite number."));
   (Key == TEXT("TriggerWeightThreshold") ? Out.WeightThreshold : Out.Chance) = Number;
  }
  else if (Key == TEXT("NotifyFilterLOD"))
  {
   if (Field.Kind != EAssetDocumentRuntimeValueKind::Integer || Field.IntegerValue < 0 || Field.IntegerValue > MAX_int32)
    return NotifyArgumentFailure(At, TEXT("NotifyFilterLOD must be a nonnegative integer."));
   Out.FilterLOD = int32(Field.IntegerValue);
  }
  else if (Key == TEXT("NotifyFilterType"))
  {
   ENotifyFilterType::Type Type = ENotifyFilterType::NoFiltering;
   if (!ReadMontageNotifyEnum(Field, Type)) return NotifyArgumentFailure(At, TEXT("NotifyFilterType must be NoFiltering or LOD."));
   Out.FilterType = Type;
  }
  else if (Key == TEXT("MontageTickType"))
  {
   EMontageNotifyTickType::Type Type = EMontageNotifyTickType::Queued;
   if (!ReadMontageNotifyEnum(Field, Type)) return NotifyArgumentFailure(At, TEXT("MontageTickType must be Queued or BranchingPoint."));
   Out.TickType = Type;
  }
  else
  {
   if (Field.Kind != EAssetDocumentRuntimeValueKind::Boolean) return NotifyArgumentFailure(At, Key + TEXT(" must be a boolean."));
   (Key == TEXT("bCanBeFilteredViaRequest") ? Out.FilterableByRequest
    : Key == TEXT("bTriggerOnDedicatedServer") ? Out.DedicatedServer : Out.Follower) = Field.BooleanValue;
  }
 }
 return Result(true);
}

bool ReadNotifyLinkMethod(const FAssetDocumentRuntimeValueDto* Value, EAnimLinkMethod::Type& Out)
{
 if (!Value || Value->Kind != EAssetDocumentRuntimeValueKind::Integer || Value->IntegerValue < 0 || Value->IntegerValue > 2) return false;
 Out = static_cast<EAnimLinkMethod::Type>(Value->IntegerValue);
 return true;
}

bool ReadNotifyLinkSlot(const FAssetDocumentRuntimeValueDto* Value, FName& Out)
{
 if (!Value) return false;
 if (Value->Kind == EAssetDocumentRuntimeValueKind::Null) { Out = NAME_None; return true; }
 if (Value->Kind != EAssetDocumentRuntimeValueKind::String || Value->StringValue.IsEmpty() || Value->StringValue == TEXT("None")) return false;
 Out = FName(*Value->StringValue);
 return true;
}

FAssetDocumentContractValidationResult ReadNotifyLink(const FAssetDocumentRuntimeValueDto& Value, bool bState, const FString& Path, FNotifyEdit& Out)
{
 const TArray<FString> Keys = bState ? TArray<FString>{TEXT("method"), TEXT("slot_name"), TEXT("end_method"), TEXT("end_slot_name")}
  : TArray<FString>{TEXT("method"), TEXT("slot_name")};
 if (!HasKeys(Value, Keys)) return NotifyArgumentFailure(Path, TEXT("A notify link is {method, slot} (a notify state's adds end_method, end_slot)."));
 if (!ReadNotifyLinkMethod(NotifyArgumentField(Value, TEXT("method")), Out.Method))
  return NotifyArgumentFailure(Path + TEXT("/LinkMethod"), TEXT("LinkMethod must be 0 (Absolute), 1 (Relative) or 2 (Proportional)."));
 if (!ReadNotifyLinkSlot(NotifyArgumentField(Value, TEXT("slot_name")), Out.Slot))
  return NotifyArgumentFailure(Path + TEXT("/SlotName"), TEXT("SlotName must be a slot track name or null."));
 Out.EndMethod = Out.Method; Out.EndSlot = Out.Slot;
 if (bState)
 {
  if (!ReadNotifyLinkMethod(NotifyArgumentField(Value, TEXT("end_method")), Out.EndMethod))
   return NotifyArgumentFailure(Path + TEXT("/EndLinkMethod"), TEXT("EndLinkMethod must be 0 (Absolute), 1 (Relative) or 2 (Proportional)."));
  if (!ReadNotifyLinkSlot(NotifyArgumentField(Value, TEXT("end_slot_name")), Out.EndSlot))
   return NotifyArgumentFailure(Path + TEXT("/EndSlotName"), TEXT("EndSlotName must be a slot track name or null."));
 }
 return Result(true);
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
  const TCHAR* Label = bRowStates ? TEXT("NotifyStates") : TEXT("Notifies");
  if (Value.Elements[Family].Kind != EAssetDocumentRuntimeValueKind::Array)
   return NotifyArgumentFailure(FString::Printf(TEXT("/Body/%s"), Label), TEXT("Notify rows must be an array."));
  const auto& Rows = Value.Elements[Family].Elements;
  for (int32 RowIndex = 0; RowIndex < Rows.Num(); ++RowIndex)
  {
   const auto& Row = Rows[RowIndex];
   const FString Path = FString::Printf(TEXT("/Body/%s/%d"), Label, RowIndex);
   const FAssetDocumentRuntimeValueDto* Object = Row.Kind == EAssetDocumentRuntimeValueKind::Object ? NotifyArgumentField(Row, TEXT("object")) : nullptr;
   const bool bNamed = Object && Object->Kind == EAssetDocumentRuntimeValueKind::Null;
   TArray<FString> RowKeys{TEXT("time")};
   if (bRowStates) RowKeys.Add(TEXT("duration"));
   RowKeys.Append({TEXT("track_index"), TEXT("object")});
   if (bNamed) RowKeys.Add(TEXT("name"));
   RowKeys.Append({TEXT("trigger"), TEXT("link")});
   if (!HasKeys(Row, RowKeys))
    return NotifyArgumentFailure(Path, FString::Printf(TEXT("A native notify row has the keys %s."), *FString::Join(RowKeys, TEXT(", "))));

   FNotifyEdit Edit;
   Edit.bState = bRowStates;
   Edit.Row = RowIndex;
   if (!ReadNonnegativeFloat(*NotifyArgumentField(Row, TEXT("time")), Edit.Time))
    return NotifyArgumentFailure(Path + TEXT("/Time"), TEXT("Time must be a finite nonnegative number."));
   if (!ReadTrackIndex(*NotifyArgumentField(Row, TEXT("track_index")), Edit.TrackIndex))
    return NotifyArgumentFailure(Path + TEXT("/TrackIndex"), TEXT("TrackIndex must be a nonnegative integer."));
   if (bRowStates)
   {
    if (!ReadNonnegativeFloat(*NotifyArgumentField(Row, TEXT("duration")), Edit.Duration) || Edit.Duration <= 0.f)
     return NotifyArgumentFailure(Path + TEXT("/Duration"), TEXT("Duration must be a finite positive number."));
    const float End = Edit.Time + Edit.Duration;
    if (!FMath::IsFinite(End) || End <= Edit.Time)
     return NotifyArgumentFailure(Path + TEXT("/Duration"), TEXT("Duration must end after the notify state's Time."));
   }
   if (bNamed)
   {
    const auto* Name = NotifyArgumentField(Row, TEXT("name"));
    if (bRowStates || Name->Kind != EAssetDocumentRuntimeValueKind::String || Name->StringValue.IsEmpty() || Name->StringValue == TEXT("None"))
     return NotifyArgumentFailure(Path + TEXT("/Name"), bRowStates ? TEXT("A notify state requires its Object {Class, Properties}.")
      : TEXT("A skeleton notify's Name must be a nonempty name."));
    Edit.Name = FName(*Name->StringValue);
   }
   else
   {
    if (!HasKeys(*Object, {TEXT("class"), TEXT("properties")}) || Object->Elements[1].Kind != EAssetDocumentRuntimeValueKind::Object)
     return NotifyArgumentFailure(Path + TEXT("/Object"), TEXT("A notify Object is {Class, Properties}."));
    Edit.Class = ResolveNotifyClass(Object->Elements[0], bRowStates);
    if (!Edit.Class)
    {
     auto Failed = Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"),
      TEXT("Notify class must be an already-loaded, canonical concrete class of the requested notify family."));
     Failed.Diagnostics[0].Path = Path + TEXT("/Object/Class");
     return Failed;
    }
    Edit.Properties = Object->Elements[1];
   }
   auto Valid = ReadNotifyTrigger(*NotifyArgumentField(Row, TEXT("trigger")), Path, Edit.Trigger);
   if (!Valid.bValid) return Valid;
   Valid = ReadNotifyLink(*NotifyArgumentField(Row, TEXT("link")), bRowStates, Path, Edit);
   if (!Valid.bValid) return Valid;
   Parsed.Add(MoveTemp(Edit));
  }
 }
 OutEdits = MoveTemp(Parsed);
 return Result(true);
}
}
