// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageSettings.h"
#include "Provider/Montage/MontageSettingsRawEdits.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentEditorNativeState.h"
#include "AlphaBlend.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimMontage.h"
#include "UObject/UnrealType.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendProfile.h"
#include "Animation/Skeleton.h"
#include "Curves/CurveFloat.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

namespace MontageSettingsLocal
{
using namespace AssetDocumentMontageSettings;
using FValue = FAssetDocumentRuntimeValueDto;
constexpr const TCHAR* Schema = TEXT("animation.montage.settings.set.v1.input.v1");
constexpr const TCHAR* InvalidIntent = TEXT("AnimationProvider.Montage.InvalidSettings");
constexpr const TCHAR* Unresolved = TEXT("AnimationProvider.Montage.SettingsReferenceUnavailable");

FAssetDocumentContractValidationResult Invalid(const TCHAR* Code, const FString& Message, const FString& Path = TEXT("/Montage/Settings"))
{
 FAssetDocumentContractValidationResult R;
 auto& D = R.Diagnostics.AddDefaulted_GetRef();
 D.Code = Code; D.Path = Path; D.Message = Message;
 return R;
}
FAssetDocumentContractValidationResult Valid() { FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
FAssetDocumentPrimitiveOutcome Failed(FAssetDocumentContractValidationResult&& Result)
{
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
 Out.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 Out.Diagnostics = MoveTemp(Result.Diagnostics);
 if (Out.Diagnostics.IsEmpty()) Out.Diagnostics = Invalid(InvalidIntent, TEXT("Montage settings failed.")).Diagnostics;
 return Out;
}

FValue String(const FString& Value) { FValue Out; Out.Kind = EAssetDocumentRuntimeValueKind::String; Out.StringValue = Value; return Out; }
FValue Number(float Value) { FValue Out; Out.Kind = EAssetDocumentRuntimeValueKind::Number; Out.NumberValue = double(Value); return Out; }
FValue Integer(int64 Value) { FValue Out; Out.Kind = EAssetDocumentRuntimeValueKind::Integer; Out.IntegerValue = Value; return Out; }
FValue MakeBool(bool Value) { FValue Out; Out.Kind = EAssetDocumentRuntimeValueKind::Boolean; Out.BooleanValue = Value; return Out; }
FValue Null() { return FValue(); }
FValue OptionalPath(const UObject* Object) { return Object ? String(Object->GetPathName()) : Null(); }
FValue Object(TArray<FString> Keys, TArray<FValue> Values)
{ FValue Out; Out.Kind = EAssetDocumentRuntimeValueKind::Object; Out.ObjectKeys = MoveTemp(Keys); Out.Elements = MoveTemp(Values); return Out; }
FValue Array(TArray<FValue> Values = {}) { FValue Out; Out.Kind = EAssetDocumentRuntimeValueKind::Array; Out.Elements = MoveTemp(Values); return Out; }

const FValue* Field(const FValue& Value, const TCHAR* Key)
{
 const int32 Index = Value.ObjectKeys.IndexOfByKey(Key);
 return Value.Elements.IsValidIndex(Index) ? &Value.Elements[Index] : nullptr;
}
bool OnlyKeys(const FValue& Value, std::initializer_list<const TCHAR*> Allowed)
{
 if (Value.Kind != EAssetDocumentRuntimeValueKind::Object || Value.ObjectKeys.Num() != Value.Elements.Num()) return false;
 for (const FString& Key : Value.ObjectKeys)
 {
  bool bFound = false;
  for (const TCHAR* Candidate : Allowed) bFound |= Key == Candidate;
  if (!bFound) return false;
 }
 return true;
}
bool ReadFloat(const FValue& Value, float& Out)
{
 const double Raw = Value.Kind == EAssetDocumentRuntimeValueKind::Number ? Value.NumberValue
  : Value.Kind == EAssetDocumentRuntimeValueKind::Integer ? double(Value.IntegerValue) : NAN;
 Out = float(Raw);
 return FMath::IsFinite(Raw) && FMath::IsFinite(Out);
}
template <typename TEnum>
bool ReadEnum(const FValue& Value, TEnum& Out)
{
 if (Value.Kind != EAssetDocumentRuntimeValueKind::String) return false;
 const UEnum* Enum = StaticEnum<TEnum>();
 const int64 Raw = Enum->GetValueByNameString(Value.StringValue);
 if (Raw == INDEX_NONE || Enum->GetNameStringByValue(Raw) != Value.StringValue) return false;
 Out = TEnum(Raw);
 return true;
}
template <typename TEnum>
FString EnumName(TEnum Value) { return StaticEnum<TEnum>()->GetNameStringByValue(int64(Value)); }
// Null clears; a string names the referenced object.
bool ReadOptionalText(const FValue& Value, FString& Out)
{
 if (Value.Kind == EAssetDocumentRuntimeValueKind::Null) { Out.Reset(); return true; }
 if (Value.Kind != EAssetDocumentRuntimeValueKind::String || Value.StringValue.IsEmpty()) return false;
 Out = Value.StringValue;
 return true;
}

struct FBlendIntent
{
 TOptional<float> Time;
 TOptional<EAlphaBlendOption> Option;
 bool bHasCurve = false;
 FString CurvePath;
};
struct FMetaIntent
{
 FString ClassPath;
 FValue Properties;
};
struct FIntent
{
 TOptional<FBlendIntent> BlendIn, BlendOut;
 TOptional<FString> ProfileIn, ProfileOut;
 TOptional<bool> Translation, Rotation;
 TOptional<ERootMotionRootLock::Type> RootLock;
 TOptional<FString> BasePose;
 TOptional<float> SamplingRate, Precision;
 TOptional<TArray<FMetaIntent>> Metadata;
 TArray<TPair<FName, TArray<FMetaIntent>>> SectionMetadata;
};

FAssetDocumentContractValidationResult ParseBlend(const FValue& Value, const TCHAR* Path, FBlendIntent& Out)
{
 if (!OnlyKeys(Value, {TEXT("time"), TEXT("option"), TEXT("custom_curve")}))
  return Invalid(InvalidIntent, TEXT("Blend settings accept only time, option and custom_curve."), Path);
 if (const FValue* Time = Field(Value, TEXT("time")))
 {
  float Parsed = 0.f;
  if (!ReadFloat(*Time, Parsed) || Parsed < 0.f) return Invalid(InvalidIntent, TEXT("Blend time must be a finite nonnegative number."), Path);
  Out.Time = Parsed;
 }
 if (const FValue* Option = Field(Value, TEXT("option")))
 {
  EAlphaBlendOption Parsed;
  if (!ReadEnum(*Option, Parsed)) return Invalid(InvalidIntent, TEXT("Blend option must name an EAlphaBlendOption value."), Path);
  Out.Option = Parsed;
 }
 if (const FValue* Curve = Field(Value, TEXT("custom_curve")))
 {
  if (!ReadOptionalText(*Curve, Out.CurvePath)) return Invalid(InvalidIntent, TEXT("Custom curve must be a curve asset path or null."), Path);
  Out.bHasCurve = true;
 }
 return Valid();
}

FAssetDocumentContractValidationResult ParseMetadata(const FValue& Value, const FString& Path, TArray<FMetaIntent>& Out)
{
 if (Value.Kind != EAssetDocumentRuntimeValueKind::Array) return Invalid(InvalidIntent, TEXT("Metadata must be an array."), Path);
 for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
 {
  const FValue& Row = Value.Elements[Index];
  const FValue* Class = Field(Row, TEXT("class"));
  const FValue* Properties = Field(Row, TEXT("properties"));
  if (!OnlyKeys(Row, {TEXT("class"), TEXT("properties")}) || !Class || !Properties
   || Class->Kind != EAssetDocumentRuntimeValueKind::String || Properties->Kind != EAssetDocumentRuntimeValueKind::Object)
   return Invalid(InvalidIntent, TEXT("Metadata rows require exactly class and properties."), FString::Printf(TEXT("%s/%d"), *Path, Index));
  Out.Add({Class->StringValue, *Properties});
 }
 return Valid();
}

FAssetDocumentContractValidationResult ParseIntent(const FValue& Value, FIntent& Out)
{
 if (!ValidateAssetDocumentRuntimeValue(Value).bValid || !OnlyKeys(Value, {TEXT("blend_in"), TEXT("blend_out"),
  TEXT("blend_profile_in"), TEXT("blend_profile_out"), TEXT("root_motion"), TEXT("preview_base_pose"),
  TEXT("time_stretch"), TEXT("metadata"), TEXT("section_metadata")}))
  return Invalid(InvalidIntent, TEXT("Montage settings require a sparse object of known settings."));
 for (const bool bIn : {true, false})
  if (const FValue* Blend = Field(Value, bIn ? TEXT("blend_in") : TEXT("blend_out")))
  {
   FBlendIntent Parsed;
   auto R = ParseBlend(*Blend, bIn ? TEXT("/Montage/Settings/blend_in") : TEXT("/Montage/Settings/blend_out"), Parsed);
   if (!R.bValid) return R;
   (bIn ? Out.BlendIn : Out.BlendOut) = MoveTemp(Parsed);
  }
 for (const bool bIn : {true, false})
  if (const FValue* Profile = Field(Value, bIn ? TEXT("blend_profile_in") : TEXT("blend_profile_out")))
  {
   FString Name;
   if (!ReadOptionalText(*Profile, Name)) return Invalid(InvalidIntent, TEXT("Blend profile must be a profile name or null."));
   (bIn ? Out.ProfileIn : Out.ProfileOut) = Name;
  }
 if (const FValue* Root = Field(Value, TEXT("root_motion")))
 {
  if (!OnlyKeys(*Root, {TEXT("translation"), TEXT("rotation"), TEXT("root_lock")}))
   return Invalid(InvalidIntent, TEXT("Root motion accepts only translation, rotation and root_lock."));
  for (const bool bTranslation : {true, false})
   if (const FValue* Flag = Field(*Root, bTranslation ? TEXT("translation") : TEXT("rotation")))
   {
    if (Flag->Kind != EAssetDocumentRuntimeValueKind::Boolean) return Invalid(InvalidIntent, TEXT("Root motion flags must be booleans."));
    (bTranslation ? Out.Translation : Out.Rotation) = Flag->BooleanValue;
   }
  if (const FValue* Lock = Field(*Root, TEXT("root_lock")))
  {
   ERootMotionRootLock::Type Parsed;
   if (!ReadEnum(*Lock, Parsed)) return Invalid(InvalidIntent, TEXT("Root lock must be RefPose, AnimFirstFrame or Zero."));
   Out.RootLock = Parsed;
  }
 }
 if (const FValue* Pose = Field(Value, TEXT("preview_base_pose")))
 {
  FString Path;
  if (!ReadOptionalText(*Pose, Path)) return Invalid(InvalidIntent, TEXT("Preview base pose must be an AnimSequence path or null."));
  Out.BasePose = Path;
 }
 if (const FValue* Stretch = Field(Value, TEXT("time_stretch")))
 {
  if (!OnlyKeys(*Stretch, {TEXT("sampling_rate"), TEXT("precision")}))
   return Invalid(InvalidIntent, TEXT("Time stretch accepts only sampling_rate and precision."));
  float Parsed = 0.f;
  if (const FValue* Rate = Field(*Stretch, TEXT("sampling_rate")))
  {
   if (!ReadFloat(*Rate, Parsed) || Parsed <= 0.f) return Invalid(InvalidIntent, TEXT("Time stretch sampling rate must be positive."));
   Out.SamplingRate = Parsed;
  }
  if (const FValue* Precision = Field(*Stretch, TEXT("precision")))
  {
   if (!ReadFloat(*Precision, Parsed) || Parsed < 0.f) return Invalid(InvalidIntent, TEXT("Time stretch precision must be nonnegative."));
   Out.Precision = Parsed;
  }
 }
 if (const FValue* Metadata = Field(Value, TEXT("metadata")))
 {
  TArray<FMetaIntent> Rows;
  auto R = ParseMetadata(*Metadata, TEXT("/Montage/Settings/metadata"), Rows);
  if (!R.bValid) return R;
  Out.Metadata = MoveTemp(Rows);
 }
 if (const FValue* Sections = Field(Value, TEXT("section_metadata")))
 {
  if (Sections->Kind != EAssetDocumentRuntimeValueKind::Array) return Invalid(InvalidIntent, TEXT("Section metadata must be an array."));
  for (int32 Index = 0; Index < Sections->Elements.Num(); ++Index)
  {
   const FValue& Row = Sections->Elements[Index];
   const FValue* Section = Field(Row, TEXT("section"));
   const FValue* Metadata = Field(Row, TEXT("metadata"));
   const FString Path = FString::Printf(TEXT("/Montage/Settings/section_metadata/%d"), Index);
   if (!OnlyKeys(Row, {TEXT("section"), TEXT("metadata")}) || !Section || !Metadata
    || Section->Kind != EAssetDocumentRuntimeValueKind::String || Section->StringValue.IsEmpty())
    return Invalid(InvalidIntent, TEXT("Section metadata rows require section and metadata."), Path);
   const FName Name(*Section->StringValue);
   if (Out.SectionMetadata.ContainsByPredicate([&](const auto& Existing) { return Existing.Key == Name; }))
    return Invalid(InvalidIntent, TEXT("A section is listed twice."), Path);
   TArray<FMetaIntent> Rows;
   auto R = ParseMetadata(*Metadata, Path + TEXT("/metadata"), Rows);
   if (!R.bValid) return R;
   Out.SectionMetadata.Emplace(Name, MoveTemp(Rows));
  }
 }
 return Valid();
}

bool ValuesEqual(const FValue& A, const FValue& B)
{
 if (A.Kind != B.Kind || A.BooleanValue != B.BooleanValue || A.IntegerValue != B.IntegerValue
  || A.StringValue != B.StringValue || A.BytesValue != B.BytesValue || A.ObjectKeys != B.ObjectKeys
  || A.Elements.Num() != B.Elements.Num()) return false;
 if (A.Kind == EAssetDocumentRuntimeValueKind::Number && A.NumberValue != B.NumberValue) return false;
 for (int32 Index = 0; Index < A.Elements.Num(); ++Index) if (!ValuesEqual(A.Elements[Index], B.Elements[Index])) return false;
 return true;
}

template <typename T>
T* LoadExact(const FString& Path)
{
 const FSoftObjectPath Soft(Path);
 if (!Soft.IsValid() || !Soft.GetSubPathString().IsEmpty()) return nullptr;
 UObject* Loaded = Soft.TryLoad();
 return IsValid(Loaded) && Loaded->IsA<T>() ? static_cast<T*>(Loaded) : nullptr;
}

// Resolved, validated native inputs; no state is changed until every row resolves.
struct FResolvedMeta { UClass* Class = nullptr; const FValue* Properties = nullptr; };
struct FResolved
{
 TOptional<UCurveFloat*> CurveIn, CurveOut;
 TOptional<UBlendProfile*> ProfileIn, ProfileOut;
 TOptional<UAnimSequence*> BasePose;
 TArray<FResolvedMeta> Metadata;
 TArray<TPair<int32, TArray<FResolvedMeta>>> Sections;
};

FAssetDocumentContractValidationResult ResolveMetadata(const TArray<FMetaIntent>& Rows, const FString& Path, TArray<FResolvedMeta>& Out)
{
 for (int32 Index = 0; Index < Rows.Num(); ++Index)
 {
  const FString RowPath = FString::Printf(TEXT("%s/%d"), *Path, Index);
  UClass* Class = LoadExact<UClass>(Rows[Index].ClassPath);
  if (!Class || !Class->IsChildOf(UAnimMetaData::StaticClass())
   || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
   return Invalid(Unresolved, FString::Printf(TEXT("'%s' is not a concrete UAnimMetaData class."), *Rows[Index].ClassPath), RowPath + TEXT("/class"));
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  if (!ValidateAssetDocumentEditorSubobjectProperties(*Class, Rows[Index].Properties, Diagnostics))
  {
   FAssetDocumentContractValidationResult R; R.Diagnostics = MoveTemp(Diagnostics);
   for (auto& D : R.Diagnostics) D.Path = RowPath + TEXT("/properties");
   return R;
  }
  Out.Add({Class, &Rows[Index].Properties});
 }
 return Valid();
}

FAssetDocumentContractValidationResult Resolve(UAnimMontage& Montage, const FIntent& Intent, FResolved& Out)
{
 for (const bool bIn : {true, false})
 {
  const auto& Blend = bIn ? Intent.BlendIn : Intent.BlendOut;
  if (Blend && Blend->bHasCurve)
  {
   UCurveFloat* Curve = nullptr;
   if (!Blend->CurvePath.IsEmpty() && !(Curve = LoadExact<UCurveFloat>(Blend->CurvePath)))
    return Invalid(Unresolved, FString::Printf(TEXT("Custom blend curve '%s' is not a CurveFloat asset."), *Blend->CurvePath));
   (bIn ? Out.CurveIn : Out.CurveOut) = Curve;
  }
  const auto& Profile = bIn ? Intent.ProfileIn : Intent.ProfileOut;
  if (Profile)
  {
   UBlendProfile* Found = nullptr;
   if (!Profile->IsEmpty())
   {
    USkeleton* Skeleton = Montage.GetSkeleton();
    Found = Skeleton ? Skeleton->GetBlendProfile(FName(**Profile)) : nullptr;
    if (!Found)
     return Invalid(Unresolved, FString::Printf(TEXT("Blend profile '%s' does not exist on the Montage skeleton."), **Profile),
      bIn ? TEXT("/Montage/Settings/blend_profile_in") : TEXT("/Montage/Settings/blend_profile_out"));
   }
   (bIn ? Out.ProfileIn : Out.ProfileOut) = Found;
  }
 }
 if (Intent.BasePose)
 {
  UAnimSequence* Pose = nullptr;
  if (!Intent.BasePose->IsEmpty() && !(Pose = LoadExact<UAnimSequence>(*Intent.BasePose)))
   return Invalid(Unresolved, FString::Printf(TEXT("Preview base pose '%s' is not an AnimSequence."), **Intent.BasePose));
  Out.BasePose = Pose;
 }
 if (Intent.Metadata)
 {
  auto R = ResolveMetadata(*Intent.Metadata, TEXT("/Montage/Settings/metadata"), Out.Metadata);
  if (!R.bValid) return R;
 }
 for (const auto& Section : Intent.SectionMetadata)
 {
  const int32 Index = Montage.GetSectionIndex(Section.Key);
  if (!Montage.CompositeSections.IsValidIndex(Index))
   return Invalid(Unresolved, FString::Printf(TEXT("Section '%s' does not exist on the Montage."), *Section.Key.ToString()),
    TEXT("/Montage/Settings/section_metadata"));
  auto& Row = Out.Sections.Emplace_GetRef(Index, TArray<FResolvedMeta>());
  auto R = ResolveMetadata(Section.Value, TEXT("/Montage/Settings/section_metadata/") + Section.Key.ToString(), Row.Value);
  if (!R.bValid) return R;
 }
 return Valid();
}

// Reuses an existing instance of the same class at the same index (details
// panel edits in place); otherwise allocates a fresh instanced object.
FAssetDocumentContractValidationResult BuildMetadata(UAnimMontage& Montage, const TArray<UAnimMetaData*>& Current,
 const TArray<FResolvedMeta>& Rows, TArray<UAnimMetaData*>& OutNext)
{
 OutNext.Reset();
 for (int32 Index = 0; Index < Rows.Num(); ++Index)
 {
  UAnimMetaData* Existing = Current.IsValidIndex(Index) ? Current[Index] : nullptr;
  UAnimMetaData* Target = IsValid(Existing) && Existing->GetClass() == Rows[Index].Class && Existing->GetOuter() == &Montage
   ? Existing : AssetDocumentMontageSettingsRaw::NewInstancedMetaData(Montage, *Rows[Index].Class);
  if (!Target) return Invalid(InvalidIntent, TEXT("A metadata object could not be allocated."));
  Target->Modify();
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  if (!WriteAssetDocumentEditorSubobjectProperties(*Target, *Rows[Index].Properties, Primitive, Diagnostics))
  {
   FAssetDocumentContractValidationResult R; R.Diagnostics = MoveTemp(Diagnostics);
   return R;
  }
  OutNext.Add(Target);
 }
 return Valid();
}

FAssetDocumentContractValidationResult Apply(UAnimMontage& Montage, const FIntent& Intent)
{
 FResolved Resolved;
 auto R = Resolve(Montage, Intent, Resolved);
 if (!R.bValid) return R;
 Montage.Modify();
 for (const bool bIn : {true, false})
 {
  FAlphaBlend& Blend = bIn ? Montage.BlendIn : Montage.BlendOut;
  if (const auto& Desired = bIn ? Intent.BlendIn : Intent.BlendOut)
  {
   if (Desired->Time) Blend.SetBlendTime(*Desired->Time);
   if (Desired->Option) Blend.SetBlendOption(*Desired->Option);
  }
  if (const auto& Curve = bIn ? Resolved.CurveIn : Resolved.CurveOut) Blend.SetCustomCurve(*Curve);
  if (const auto& Profile = bIn ? Resolved.ProfileIn : Resolved.ProfileOut) (bIn ? Montage.BlendProfileIn : Montage.BlendProfileOut) = *Profile;
 }
 if (Intent.Translation) Montage.bEnableRootMotionTranslation = *Intent.Translation;
 if (Intent.Rotation) Montage.bEnableRootMotionRotation = *Intent.Rotation;
 if (Intent.RootLock) Montage.RootMotionRootLock = *Intent.RootLock;
#if WITH_EDITORONLY_DATA
 if (Resolved.BasePose) Montage.PreviewBasePose = *Resolved.BasePose;
#endif
 if ((Intent.SamplingRate || Intent.Precision) && !AssetDocumentMontageSettingsRaw::SetTimeStretchConfig(
  Montage, Intent.SamplingRate.GetPtrOrNull(), Intent.Precision.GetPtrOrNull()))
  return Invalid(InvalidIntent, TEXT("Time stretch configuration fields are unavailable."));
 if (Intent.Metadata)
 {
  TArray<UAnimMetaData*> Next;
  R = BuildMetadata(Montage, Montage.GetMetaData(), Resolved.Metadata, Next);
  if (!R.bValid) return R;
  // Public UAnimationAsset API; removed instances stay unreferenced like the details panel "Clear".
  Montage.EmptyMetaData();
  for (UAnimMetaData* Meta : Next) Montage.AddMetaData(Meta);
 }
 for (const auto& Section : Resolved.Sections)
 {
  TArray<UAnimMetaData*> Next;
  R = BuildMetadata(Montage, Montage.CompositeSections[Section.Key].GetMetaData(), Section.Value, Next);
  if (!R.bValid) return R;
  Montage.CompositeSections[Section.Key].MetaData = TArray<TObjectPtr<UAnimMetaData>>(Next);
 }
 // Derived state the details panel refreshes after any of these edits.
 AssetDocumentMontageSettingsRaw::RefreshDerivedSyncState(Montage);
 AssetDocumentMontageSettingsRaw::BakeTimeStretchCurve(Montage);
 Montage.MarkPackageDirty();
 return Valid();
}

bool CaptureMetadata(const UAnimMontage& Montage, const TArray<UAnimMetaData*>& Objects, FValue& Out,
 TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
 Out = Array();
 for (UAnimMetaData* Meta : Objects)
 {
  if (!IsValid(Meta)) continue;
  FValue Properties = Object({}, {});
  // Only instanced metadata owned by this Montage has authorable properties.
  // A shared pointer (e.g. a class default object) is observed by class only.
  if (!Meta->IsTemplate() && Meta->IsIn(&Montage)
   && !ReadAssetDocumentEditorSubobjectProperties(*Meta, Properties, Diagnostics)) return false;
  Out.Elements.Add(Object({TEXT("class"), TEXT("properties")}, {String(Meta->GetClass()->GetPathName()), MoveTemp(Properties)}));
 }
 return true;
}
FValue CaptureBlend(const FAlphaBlend& Blend)
{
 return Object({TEXT("time"), TEXT("option"), TEXT("custom_curve")},
  {Number(Blend.GetBlendTime()), String(EnumName(Blend.GetBlendOption())), OptionalPath(Blend.GetCustomCurve())});
}
}

bool CaptureAnimationMontageSettings(UAnimMontage& Montage, FAssetDocumentRuntimeValueDto& OutSettings,
 TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
 using namespace MontageSettingsLocal;
 float SamplingRate = 0.f, Precision = 0.f;
 if (!AssetDocumentMontageSettingsRaw::GetTimeStretchConfig(Montage, SamplingRate, Precision))
 {
  OutDiagnostics.Append(Invalid(TEXT("AnimationProvider.Montage.SnapshotUnsupported"), TEXT("Time stretch configuration is unavailable.")).Diagnostics);
  return false;
 }
 FValue Metadata, Sections = Array();
 if (!CaptureMetadata(Montage, Montage.GetMetaData(), Metadata, OutDiagnostics)) return false;
 for (const FCompositeSection& Section : Montage.CompositeSections)
 {
  FValue Rows;
  if (!CaptureMetadata(Montage, Section.GetMetaData(), Rows, OutDiagnostics)) return false;
  Sections.Elements.Add(Object({TEXT("section"), TEXT("metadata")}, {String(Section.SectionName.ToString()), MoveTemp(Rows)}));
 }
 // Every other plain admitted reflected field (e.g. RateScale), for the
 // document Properties region. Aggregates are owned by dedicated regions; hard
 // references (blend profiles, preview base pose, ...) by the settings regions,
 // and text / text-form structs are not part of this profile's Properties.
 FValue AllProperties, Properties = Object({}, {});
 if (!ReadAssetDocumentEditorSubobjectProperties(Montage, AllProperties, OutDiagnostics)) return false;
 for (int32 Index = 0; Index < AllProperties.ObjectKeys.Num(); ++Index)
 {
  const auto Kind = AllProperties.Elements[Index].Kind;
  if (Kind == EAssetDocumentRuntimeValueKind::Array || Kind == EAssetDocumentRuntimeValueKind::Object
   || Kind == EAssetDocumentRuntimeValueKind::Bytes) continue;
  const FProperty* Field = FindFProperty<FProperty>(Montage.GetClass(), FName(*AllProperties.ObjectKeys[Index]));
  if (Field == nullptr || (CastField<FObjectPropertyBase>(Field) != nullptr && CastField<FSoftObjectProperty>(Field) == nullptr)
   || CastField<FInterfaceProperty>(Field) != nullptr || CastField<FTextProperty>(Field) != nullptr
   || CastField<FStructProperty>(Field) != nullptr) continue;
  Properties.ObjectKeys.Add(AllProperties.ObjectKeys[Index]);
  Properties.Elements.Add(MoveTemp(AllProperties.Elements[Index]));
 }
 const UAnimSequence* BasePose = nullptr;
#if WITH_EDITORONLY_DATA
 BasePose = Montage.PreviewBasePose;
#endif
 auto ProfileName = [](const UBlendProfile* Profile) { return Profile ? String(Profile->GetName()) : Null(); };
 OutSettings = Object(
  {TEXT("blend_in"), TEXT("blend_out"), TEXT("blend_mode_in"), TEXT("blend_mode_out"), TEXT("blend_out_trigger_time"),
   TEXT("enable_auto_blend_out"), TEXT("blend_profile_in"), TEXT("blend_profile_out"), TEXT("sync_group"),
   TEXT("sync_slot_index"), TEXT("root_motion"), TEXT("preview_base_pose"), TEXT("time_stretch"),
   TEXT("metadata"), TEXT("section_metadata"), TEXT("properties")},
  {CaptureBlend(Montage.BlendIn), CaptureBlend(Montage.BlendOut), String(EnumName(Montage.BlendModeIn)),
   String(EnumName(Montage.BlendModeOut)), Number(Montage.BlendOutTriggerTime), MakeBool(Montage.bEnableAutoBlendOut),
   ProfileName(Montage.BlendProfileIn), ProfileName(Montage.BlendProfileOut), String(Montage.SyncGroup.ToString()),
   Integer(Montage.SyncSlotIndex),
   Object({TEXT("translation"), TEXT("rotation"), TEXT("root_lock")},
    {MakeBool(Montage.bEnableRootMotionTranslation), MakeBool(Montage.bEnableRootMotionRotation),
     String(EnumName(Montage.RootMotionRootLock.GetValue()))}),
   OptionalPath(BasePose),
   Object({TEXT("curve_name"), TEXT("sampling_rate"), TEXT("precision")},
    {String(Montage.TimeStretchCurveName.ToString()), Number(SamplingRate), Number(Precision)}),
   MoveTemp(Metadata), MoveTemp(Sections), MoveTemp(Properties)});
 return true;
}

FAssetDocumentPrimitiveDescriptor MakeAnimationMontageSettingsDescriptor()
{
 using namespace MontageSettingsLocal;
 FAssetDocumentPrimitiveDescriptor D;
 D.PrimitiveId = Primitive; D.Version = 1; D.ProviderId = TEXT("animation"); D.Owner = EAssetDocumentPrimitiveOwner::Animation;
 D.ExecutionThread = EAssetDocumentExecutionThread::GameThread; D.Access = EAssetDocumentPrimitiveAccess::Mutating;
 D.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {{TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, true}};
 D.bRequiresActiveTransaction = true; D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.InputSchemaId = Schema; D.OutputSchemaId = FString(Primitive) + TEXT(".output.v1");
 D.MutationFootprintSchemaId = FString(Primitive) + TEXT(".footprint.v1");
 D.StableDiagnosticCodes = {InvalidIntent, Unresolved, TEXT("AnimationProvider.InvalidHandlePayload"),
  TEXT("AnimationProvider.TransactionUnavailable"), TEXT("PrimitiveInvocation.InvalidValueShape"),
  TEXT("PropertyBatchWrite.DuplicateProperty"), TEXT("PropertyBatchWrite.UnknownProperty"), TEXT("PropertyToken.InvalidPayload"),
  TEXT("PropertyWrite.DecodeFailed"), TEXT("PropertyWrite.AccessFailed")};
 auto& A = D.Admission;
 A.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
 A.ExactEngineVersion = TEXT("5.7.4"); A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 A.PublicModule = TEXT("Engine;AssetDocumentEditor");
 A.PublicHeader = TEXT("AlphaBlend.h;Animation/AnimMontage.h;Animation/Skeleton.h;AssetDocumentEditorNativeState.h");
 A.ExportedAuthoritativeApi = TEXT("FAlphaBlend::SetBlendTime;FAlphaBlend::SetBlendOption;FAlphaBlend::SetCustomCurve;USkeleton::GetBlendProfile;FTimeStretchCurve::BakeFromFloatCurve;WriteAssetDocumentEditorSubobjectProperties");
 A.AuthorityContextId = TEXT("animation.montage.settings.v1");
 A.PrimaryOperationId = Primitive;
 A.IdentityLifetimeContractId = TEXT("animation.provider-lease.mutable.v1");
 A.TransactionContractId = TEXT("animation.core-active-transaction.v1");
 A.RollbackContractId = TEXT("core.package-snapshot.v1");
 A.CompletionContractId = TEXT("animation.montage.settings.post-edit-refresh.v1");
 A.SaveReloadEvidenceId = FString(Primitive) + TEXT(".save-reload.v1");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimMontage.Settings.InvalidValueRollsBack")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimMontage.Settings.RoundTrip"),
  TEXT("UEAssetDoc.AnimationProvider.MontageSettings.RawEdits")};
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|montage-settings|snapshot-covered|") + D.PrimitiveId;
 return D;
}

FAssetDocumentContractValidationResult PrepareAnimationMontageSettingsBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 using namespace MontageSettingsLocal;
 OutBatch = FAssetDocumentPreparedMutationBatch();
 if (Intents.Num() != 1 || Request.Operation != EAssetDocumentRuntimeOperation::Apply
  || Request.RequestedAssetClassPath != UAnimMontage::StaticClass()->GetPathName())
  return Invalid(InvalidIntent, TEXT("Montage settings prepare one exact Montage Apply intent."));
 const auto& Intent = Intents[0];
 if (Intent.StepIndex < 0 || Intent.Requirement.PrimitiveId != Primitive || Intent.Requirement.Version != 1
  || Intent.Requirement.RequiredByRouteId != TEXT("animation_profile") || Intent.InputSchemaId != Schema
  || Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply)
  return Invalid(InvalidIntent, TEXT("The intent is not the exact Montage settings step."));
 FIntent Parsed;
 auto R = ParseIntent(Intent.Intent, Parsed);
 if (!R.bValid) return R;
 auto* Target = Cast<UAnimMontage>(FSoftObjectPath(Request.TargetAssetPath).ResolveObject());
 if (!IsValid(Target) || Target->GetClass() != UAnimMontage::StaticClass() || Target->GetPathName() != Request.TargetAssetPath)
  return Invalid(InvalidIntent, TEXT("Montage settings require the existing loaded Montage target."));
 const FString Package = Target->GetOutermost()->GetName();
 FAssetDocumentPreparedMutationContribution Contribution;
 Contribution.StepIndex = Intent.StepIndex;
 Contribution.RawFootprint = {
  {EAssetDocumentMutationFootprintKind::UObjectState, TEXT("Object:") + Request.TargetAssetPath, TEXT("rollback.uobject")},
  {EAssetDocumentMutationFootprintKind::Package, TEXT("Package:") + Package, TEXT("rollback.package")}};
 Contribution.OwnedSurfaceIds = {Request.TargetAssetPath + TEXT("::MontageSettings")};
 Contribution.bCoveredByPackageSnapshot = true;
 Contribution.ValidateInvocation = [Intent, RequestId = Request.RequestId, Revision = Request.ExpectedRuntimeRevision](
  const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver&)
 {
  if (Invocation.RequestId != RequestId || Invocation.RuntimeRevision != Revision || Invocation.PrimitiveId != Primitive
   || Invocation.Version != 1 || Invocation.Phase != Intent.Phase || !ValuesEqual(Invocation.ValueArguments, Intent.Intent))
   return Invalid(InvalidIntent, TEXT("The invocation differs from the prepared Montage settings intent."));
  return Valid();
 };
 FAssetDocumentPreparedMutationBatch Batch;
 Batch.Contributions.Add(MoveTemp(Contribution));
 Batch.OpenMutationCapture = [](const FGuid& Transaction, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!Transaction.IsValid())
  {
   Diagnostics.Append(Invalid(InvalidIntent, TEXT("A live transaction is required.")).Diagnostics);
   return nullptr;
  }
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([]() {});
 };
 OutBatch = MoveTemp(Batch);
 return Valid();
}

FAssetDocumentPrimitiveOutcome InvokeAnimationMontageSettings(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction)
{
 using namespace MontageSettingsLocal;
 if (!ValidateAssetDocumentPrimitiveInvocation(Invocation, MakeAnimationMontageSettingsDescriptor()).bValid)
  return Failed(Invalid(InvalidIntent, TEXT("The invocation does not match the Montage settings descriptor.")));
 if (!Transaction || Transaction->GetState() != EAssetDocumentAtomicState::Mutating
  || Transaction->GetTransactionId() != Invocation.TransactionId)
  return Failed(Invalid(TEXT("AnimationProvider.TransactionUnavailable"), TEXT("Montage settings require the matching active transaction.")));
 FIntent Intent;
 auto R = ParseIntent(Invocation.ValueArguments, Intent);
 if (!R.bValid) return Failed(MoveTemp(R));
 auto Acquired = AcquireAnimationInputLease(Handles, Invocation, TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
 if (!Acquired.Lease) return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("Montage settings require a verified mutable asset lease."));
 FAssetDocumentContractValidationResult Applied = Invalid(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("The verified asset is not an exact Montage."));
 FString TargetPath;
 const auto Identity = WithAssetDocumentEditorAssetIdentity(*Acquired.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  UObject& Target = View.GetObject();
  if (IsValid(&Target) && Target.GetClass() == UAnimMontage::StaticClass() && View.GetCanonicalObjectPath() == Target.GetPathName())
  {
   TargetPath = Target.GetPathName();
   Applied = Apply(static_cast<UAnimMontage&>(Target), Intent);
  }
  return Valid();
 });
 if (!Identity.bValid) return Failed(FAssetDocumentContractValidationResult(Identity));
 if (!Applied.bValid) return Failed(MoveTemp(Applied));
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 Out.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 auto& Change = Out.ChangedSet.AddDefaulted_GetRef();
 Change.AuthorityId = Primitive; Change.StableObjectId = TargetPath;
 Change.Path = TEXT("/Settings"); Change.ChangeKind = TEXT("Replace");
 return Out;
}
