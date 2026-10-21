// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Sequence/AnimationAssetAuthoring.h"
#include "Provider/Sequence/AnimationAssetAuthoringRawEdits.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentEditorNativeState.h"
#include "AssetDocumentEditorTrialScope.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/AssetUserData.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentAnimationAuthoring
{
bool IsPrimitive(FStringView PrimitiveId)
{
 return PrimitiveId.Equals(SnapshotPrimitive, ESearchCase::CaseSensitive) || PrimitiveId.Equals(EditPrimitive, ESearchCase::CaseSensitive);
}
bool IsAdmittedClassPath(FStringView ClassPath)
{
 return ClassPath.Equals(UAnimSequence::StaticClass()->GetPathName(), ESearchCase::CaseSensitive);
}
}

namespace AnimationAssetAuthoringLocal
{
using namespace AssetDocumentAnimationAuthoring;
using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;
using FResult = FAssetDocumentContractValidationResult;
constexpr const TCHAR* InvalidIntent = TEXT("AnimationProvider.Authoring.InvalidIntent");
constexpr const TCHAR* Unresolved = TEXT("AnimationProvider.Authoring.ReferenceUnavailable");
constexpr const TCHAR* SkeletonMismatch = TEXT("AnimationProvider.Authoring.SkeletonMismatch");
constexpr const TCHAR* TrackInUse = TEXT("AnimationProvider.Authoring.NotifyTrackInUse");
constexpr const TCHAR* UnknownTrack = TEXT("AnimationProvider.Authoring.UnknownNotifyTrack");
constexpr const TCHAR* Overlap = TEXT("AnimationProvider.Authoring.OverlappingNotifies");
constexpr const TCHAR* OutOfRange = TEXT("AnimationProvider.Authoring.TimeOutOfRange");
constexpr const TCHAR* ControllerFailed = TEXT("AnimationProvider.Authoring.ControllerFailed");
constexpr const TCHAR* SnapshotFailed = TEXT("AnimationProvider.Authoring.SnapshotUnsupported");
constexpr float TimeTolerance = 1.e-4f;

FResult Invalid(const TCHAR* Code, const FString& Message, const FString& Path = TEXT("/"))
{
 FResult R;
 auto& D = R.Diagnostics.AddDefaulted_GetRef();
 D.Code = Code; D.Path = Path; D.Message = Message;
 return R;
}
FResult Valid() { FResult R; R.bValid = true; return R; }
FAssetDocumentPrimitiveOutcome Failed(FResult&& Result)
{
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
 Out.Value.Kind = EKind::Object;
 Out.Diagnostics = MoveTemp(Result.Diagnostics);
 if (Out.Diagnostics.IsEmpty()) Out.Diagnostics = Invalid(InvalidIntent, TEXT("Animation authoring failed.")).Diagnostics;
 return Out;
}

FValue String(const FString& Value) { FValue Out; Out.Kind = EKind::String; Out.StringValue = Value; return Out; }
FValue Number(double Value) { FValue Out; Out.Kind = EKind::Number; Out.NumberValue = Value; return Out; }
FValue Integer(int64 Value) { FValue Out; Out.Kind = EKind::Integer; Out.IntegerValue = Value; return Out; }
FValue Bool(bool Value) { FValue Out; Out.Kind = EKind::Boolean; Out.BooleanValue = Value; return Out; }
FValue Null() { return FValue(); }
FValue OptionalPath(const UObject* Object) { return Object ? String(Object->GetPathName()) : Null(); }
FValue MakeObject(TArray<FString> Keys, TArray<FValue> Values)
{ FValue Out; Out.Kind = EKind::Object; Out.ObjectKeys = MoveTemp(Keys); Out.Elements = MoveTemp(Values); return Out; }
FValue Array(TArray<FValue> Values = {}) { FValue Out; Out.Kind = EKind::Array; Out.Elements = MoveTemp(Values); return Out; }
void Put(FValue& Target, const FString& Key, FValue Value) { Target.ObjectKeys.Add(Key); Target.Elements.Add(MoveTemp(Value)); }

const FValue* Field(const FValue& Value, const TCHAR* Key)
{
 if (Value.Kind != EKind::Object) return nullptr;
 const int32 Index = Value.ObjectKeys.IndexOfByKey(Key);
 return Value.Elements.IsValidIndex(Index) ? &Value.Elements[Index] : nullptr;
}
bool OnlyKeys(const FValue& Value, std::initializer_list<const TCHAR*> Allowed)
{
 if (Value.Kind != EKind::Object || Value.ObjectKeys.Num() != Value.Elements.Num()) return false;
 for (const FString& Key : Value.ObjectKeys)
 {
  bool bFound = false;
  for (const TCHAR* Candidate : Allowed) bFound |= Key == Candidate;
  if (!bFound) return false;
 }
 return true;
}
bool ReadFloat(const FValue* Value, float& Out)
{
 if (!Value) return false;
 const double Raw = Value->Kind == EKind::Number ? Value->NumberValue : Value->Kind == EKind::Integer ? double(Value->IntegerValue) : NAN;
 Out = float(Raw);
 return FMath::IsFinite(Raw) && FMath::IsFinite(Out);
}
bool ReadInt(const FValue* Value, int64& Out)
{
 if (!Value || Value->Kind != EKind::Integer) return false;
 Out = Value->IntegerValue; return true;
}
bool ReadText(const FValue* Value, FString& Out)
{
 if (!Value || Value->Kind != EKind::String) return false;
 Out = Value->StringValue; return true;
}
// Null clears; a string names the referenced object.
bool ReadOptionalPath(const FValue* Value, FString& Out)
{
 if (!Value) return false;
 if (Value->Kind == EKind::Null) { Out.Reset(); return true; }
 return Value->Kind == EKind::String && !Value->StringValue.IsEmpty() && (Out = Value->StringValue, true);
}
template <typename TEnum>
bool ReadEnum(const FValue* Value, TEnum& Out)
{
 if (!Value || Value->Kind != EKind::String) return false;
 const UEnum* Enum = StaticEnum<TEnum>();
 const int64 Raw = Enum->GetValueByNameString(Value->StringValue);
 if (Raw == INDEX_NONE || Enum->GetNameStringByValue(Raw) != Value->StringValue) return false;
 Out = TEnum(Raw);
 return true;
}
template <typename TEnum>
FString EnumName(TEnum Value) { return StaticEnum<TEnum>()->GetNameStringByValue(int64(Value)); }
FString Sub(const FString& Path, const FString& Key) { return Path + TEXT("/") + Key; }
FString Sub(const FString& Path, int32 Index) { return FString::Printf(TEXT("%s/%d"), *Path, Index); }

UObject* LoadExact(const FString& Path, UClass& Expected)
{
 const FSoftObjectPath Soft(Path);
 if (!Soft.IsValid() || !Soft.GetSubPathString().IsEmpty() || Soft.ToString() != Path) return nullptr;
 UObject* Loaded = Soft.TryLoad();
 return IsValid(Loaded) && Loaded->IsA(&Expected) ? Loaded : nullptr;
}
UClass* LoadConcreteClass(const FString& Path, UClass& Base)
{
 UClass* Class = Cast<UClass>(LoadExact(Path, *UClass::StaticClass()));
 return Class && Class->IsChildOf(&Base) && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
  ? Class : nullptr;
}

// ---------------------------------------------------------------------------
// Object properties: admitted Editor codecs plus hard (non-instanced) object or
// class references. The Editor codec admits these too; the explicit path keeps
// the exact-class load (LoadExact) and Unresolved diagnostics of this profile.
// ---------------------------------------------------------------------------
bool IsAuthorableReference(const FProperty& Property)
{
 return CastField<FObjectProperty>(&Property) && Property.ArrayDim == 1 && Property.HasAnyPropertyFlags(CPF_Edit)
  && !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnInstance | CPF_Deprecated | CPF_Transient
   | CPF_InstancedReference | CPF_PersistentInstance | CPF_ContainsInstancedReference);
}
FObjectProperty* FindReference(UClass& Class, const FString& Name)
{
 for (TFieldIterator<FProperty> It(&Class); It; ++It)
  if (It->GetAuthoredName() == Name) return IsAuthorableReference(**It) ? CastField<FObjectProperty>(*It) : nullptr;
 return nullptr;
}
UClass* ReferenceTarget(const FObjectProperty& Property)
{
 if (const FClassProperty* ClassProperty = CastField<FClassProperty>(&Property)) return ClassProperty->MetaClass;
 return Property.PropertyClass;
}

struct FObjectProperties
{
 FValue Admitted;
 TArray<TPair<FObjectProperty*, UObject*>> References;
};

FResult ResolveProperties(UClass& Class, const FValue& Properties, const FString& Path, FObjectProperties& Out)
{
 if (Properties.Kind != EKind::Object) return Invalid(InvalidIntent, TEXT("Properties must be an object."), Path);
 Out.Admitted = MakeObject({}, {});
 for (int32 Index = 0; Index < Properties.ObjectKeys.Num(); ++Index)
 {
  const FString& Name = Properties.ObjectKeys[Index];
  const FValue& Value = Properties.Elements[Index];
  if (FObjectProperty* Reference = FindReference(Class, Name))
  {
   FString Target;
   if (!ReadOptionalPath(&Value, Target))
    return Invalid(InvalidIntent, FString::Printf(TEXT("'%s' is an object reference: give an object path or null."), *Name), Sub(Path, Name));
   UObject* Loaded = nullptr;
   if (!Target.IsEmpty())
   {
    UClass* Expected = ReferenceTarget(*Reference);
    Loaded = CastField<FClassProperty>(Reference) ? LoadConcreteClass(Target, *Expected) : LoadExact(Target, *Expected);
    if (!Loaded && CastField<FClassProperty>(Reference))
    {
     UClass* AnyClass = Cast<UClass>(LoadExact(Target, *UClass::StaticClass()));
     Loaded = AnyClass && AnyClass->IsChildOf(Expected) ? AnyClass : nullptr;
    }
    if (!Loaded)
     return Invalid(Unresolved, FString::Printf(TEXT("'%s' does not resolve to a %s."), *Target, *Expected->GetName()), Sub(Path, Name));
   }
   Out.References.Emplace(Reference, Loaded);
   continue;
  }
  Put(Out.Admitted, Name, Value);
 }
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 if (!ValidateAssetDocumentEditorSubobjectProperties(Class, Out.Admitted, Diagnostics))
 {
  FResult R; R.Diagnostics = MoveTemp(Diagnostics);
  for (auto& D : R.Diagnostics) D.Path = D.Path == TEXT("/Subobject/Properties") ? Path : Path + D.Path;
  return R;
 }
 return Valid();
}

FResult WriteProperties(UObject& Target, const FObjectProperties& Properties, const FString& Path)
{
 if (!Properties.Admitted.ObjectKeys.IsEmpty())
 {
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  if (!WriteAssetDocumentEditorSubobjectProperties(Target, Properties.Admitted, EditPrimitive, Diagnostics))
  {
   FResult R; R.Diagnostics = MoveTemp(Diagnostics);
   for (auto& D : R.Diagnostics) D.Path = D.Path == TEXT("/Subobject/Properties") ? Path : Path + D.Path;
   return R;
  }
 }
 for (const auto& Reference : Properties.References)
  AssetDocumentAnimationAuthoringRaw::SetObjectReference(Target, *Reference.Key, Reference.Value);
 return Valid();
}

bool ReadProperties(UObject& Object, FValue& Out, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
 Out = MakeObject({}, {});
 if (!ReadAssetDocumentEditorSubobjectProperties(Object, Out, Diagnostics)) return false;
 // The Editor codec now admits hard references itself (same path-or-null form);
 // add only the ones it did not already read.
 for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
  if (IsAuthorableReference(**It) && !Out.ObjectKeys.Contains(It->GetAuthoredName()))
   Put(Out, It->GetAuthoredName(), OptionalPath(CastField<FObjectProperty>(*It)->GetObjectPropertyValue_InContainer(&Object)));
 return true;
}

FValue ReadReferences(UObject& Object)
{
 FValue Out = MakeObject({}, {});
 for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
  if (IsAuthorableReference(**It))
   Put(Out, It->GetAuthoredName(), OptionalPath(CastField<FObjectProperty>(*It)->GetObjectPropertyValue_InContainer(&Object)));
 return Out;
}

// ---------------------------------------------------------------------------
// Intent
// ---------------------------------------------------------------------------
struct FObjectRow
{
 UClass* Class = nullptr;
 FObjectProperties Properties;
 FString Path;
};
struct FTrigger
{
 TOptional<float> WeightThreshold, Chance;
 TOptional<ENotifyFilterType::Type> FilterType;
 TOptional<int32> FilterLOD;
 TOptional<bool> FilterableByRequest, DedicatedServer, Follower;
 TOptional<EMontageNotifyTickType::Type> TickType;
};
struct FNotifyRow
{
 float Time = 0.f, Duration = 0.f;
 FName Track, Name;
 FObjectRow Object;
 FTrigger Trigger;
 FString Path;
};
struct FCurveRow
{
 FName Name;
 TArray<FRichCurveKey> Keys;
 TOptional<int32> Flags;
 TOptional<FLinearColor> Color;
 TOptional<FString> Comment;
};
struct FMarkerRow { FName Name, Track; float Time = 0.f; FString Path; };
struct FIntent
{
 TOptional<FString> Skeleton;
 TOptional<FString> PreviewMesh;
 UObject* PreviewMeshObject = nullptr;
 TOptional<FObjectProperties> References;
 TOptional<FFrameRate> FrameRate;
 TOptional<int32> NumberOfFrames;
 TOptional<TArray<FCurveRow>> Curves;
 TOptional<TArray<FName>> Tracks;
 TOptional<TArray<FNotifyRow>> Notifies, States;
 TOptional<TArray<FMarkerRow>> Markers;
 TOptional<TArray<FObjectRow>> Metadata, UserData;
};

FResult ParseObjectRows(const FValue* Value, UClass& Base, const FString& Path, TArray<FObjectRow>& Out)
{
 if (!Value || Value->Kind != EKind::Array) return Invalid(InvalidIntent, TEXT("Object rows must be an array."), Path);
 TSet<UClass*> Seen;
 for (int32 Index = 0; Index < Value->Elements.Num(); ++Index)
 {
  const FValue& Row = Value->Elements[Index];
  const FString RowPath = Sub(Path, Index);
  FString ClassPath;
  if (!OnlyKeys(Row, {TEXT("class"), TEXT("properties")}) || !ReadText(Field(Row, TEXT("class")), ClassPath))
   return Invalid(InvalidIntent, TEXT("Object rows require class and optional properties."), RowPath);
  FObjectRow& Out1 = Out.AddDefaulted_GetRef();
  Out1.Path = RowPath;
  Out1.Class = LoadConcreteClass(ClassPath, Base);
  if (!Out1.Class)
   return Invalid(Unresolved, FString::Printf(TEXT("'%s' is not a concrete %s class."), *ClassPath, *Base.GetName()), Sub(RowPath, TEXT("class")));
  if (Base.IsChildOf(UAssetUserData::StaticClass()) && Seen.Contains(Out1.Class))
   return Invalid(InvalidIntent, TEXT("Asset user data holds one object per class."), Sub(RowPath, TEXT("class")));
  Seen.Add(Out1.Class);
  const FValue* Properties = Field(Row, TEXT("properties"));
  auto R = ResolveProperties(*Out1.Class, Properties ? *Properties : MakeObject({}, {}), Sub(RowPath, TEXT("properties")), Out1.Properties);
  if (!R.bValid) return R;
 }
 return Valid();
}

FResult ParseTrigger(const FValue* Value, const FString& Path, FTrigger& Out)
{
 if (!Value) return Valid();
 if (!OnlyKeys(*Value, {TEXT("TriggerWeightThreshold"), TEXT("NotifyTriggerChance"), TEXT("NotifyFilterType"), TEXT("NotifyFilterLOD"),
  TEXT("bCanBeFilteredViaRequest"), TEXT("bTriggerOnDedicatedServer"), TEXT("bTriggerOnFollower"), TEXT("MontageTickType")}))
  return Invalid(InvalidIntent, TEXT("Unknown notify trigger setting."), Path);
 float Number = 0.f; int64 Int = 0;
 if (const FValue* V = Field(*Value, TEXT("TriggerWeightThreshold"))) { if (!ReadFloat(V, Number)) return Invalid(InvalidIntent, TEXT("TriggerWeightThreshold must be a number."), Path); Out.WeightThreshold = Number; }
 if (const FValue* V = Field(*Value, TEXT("NotifyTriggerChance")))
 {
  if (!ReadFloat(V, Number) || Number < 0.f || Number > 1.f) return Invalid(InvalidIntent, TEXT("NotifyTriggerChance must be in [0, 1]."), Path);
  Out.Chance = Number;
 }
 if (const FValue* V = Field(*Value, TEXT("NotifyFilterType")))
 {
  ENotifyFilterType::Type Type;
  if (!ReadEnum(V, Type)) return Invalid(InvalidIntent, TEXT("NotifyFilterType must name an ENotifyFilterType value."), Path);
  Out.FilterType = Type;
 }
 if (const FValue* V = Field(*Value, TEXT("NotifyFilterLOD")))
 {
  if (!ReadInt(V, Int) || Int < 0 || Int > MAX_int32) return Invalid(InvalidIntent, TEXT("NotifyFilterLOD must be a nonnegative integer."), Path);
  Out.FilterLOD = int32(Int);
 }
 auto Flag = [&](const TCHAR* Key, TOptional<bool>& Target)
 {
  const FValue* V = Field(*Value, Key);
  if (!V) return true;
  if (V->Kind != EKind::Boolean) return false;
  Target = V->BooleanValue; return true;
 };
 if (!Flag(TEXT("bCanBeFilteredViaRequest"), Out.FilterableByRequest) || !Flag(TEXT("bTriggerOnDedicatedServer"), Out.DedicatedServer)
  || !Flag(TEXT("bTriggerOnFollower"), Out.Follower)) return Invalid(InvalidIntent, TEXT("Notify trigger flags must be booleans."), Path);
 if (const FValue* V = Field(*Value, TEXT("MontageTickType")))
 {
  EMontageNotifyTickType::Type Type;
  if (!ReadEnum(V, Type)) return Invalid(InvalidIntent, TEXT("MontageTickType must name an EMontageNotifyTickType value."), Path);
  Out.TickType = Type;
 }
 return Valid();
}

FResult ParseNotifies(const FValue* Value, bool bStates, const FString& Path, TArray<FNotifyRow>& Out)
{
 if (!Value || Value->Kind != EKind::Array) return Invalid(InvalidIntent, TEXT("Notify rows must be an array."), Path);
 for (int32 Index = 0; Index < Value->Elements.Num(); ++Index)
 {
  const FValue& Row = Value->Elements[Index];
  FNotifyRow& Parsed = Out.AddDefaulted_GetRef();
  Parsed.Path = Sub(Path, Index);
  if (!OnlyKeys(Row, {TEXT("time"), TEXT("duration"), TEXT("track"), TEXT("class"), TEXT("name"), TEXT("properties"), TEXT("trigger")})
   || (!bStates && Field(Row, TEXT("duration"))))
   return Invalid(InvalidIntent, TEXT("Unknown notify field."), Parsed.Path);
  if (!ReadFloat(Field(Row, TEXT("time")), Parsed.Time) || Parsed.Time < 0.f)
   return Invalid(InvalidIntent, TEXT("Notify time must be a nonnegative number."), Sub(Parsed.Path, TEXT("time")));
  if (bStates && (!ReadFloat(Field(Row, TEXT("duration")), Parsed.Duration) || Parsed.Duration <= 0.f))
   return Invalid(InvalidIntent, TEXT("Notify state duration must be a positive number."), Sub(Parsed.Path, TEXT("duration")));
  FString Text;
  if (const FValue* Track = Field(Row, TEXT("track")))
  {
   if (!ReadText(Track, Text) || Text.IsEmpty())
    return Invalid(InvalidIntent, TEXT("Notify track must be a nonempty name."), Sub(Parsed.Path, TEXT("track")));
   Parsed.Track = FName(*Text);
  }
  const FValue* Class = Field(Row, TEXT("class"));
  if (Class && Class->Kind != EKind::Null)
  {
   if (!ReadText(Class, Text)) return Invalid(InvalidIntent, TEXT("Notify class must be a class path or null."), Sub(Parsed.Path, TEXT("class")));
   UClass& Base = bStates ? *UAnimNotifyState::StaticClass() : *UAnimNotify::StaticClass();
   Parsed.Object.Class = LoadConcreteClass(Text, Base);
   if (!Parsed.Object.Class)
    return Invalid(Unresolved, FString::Printf(TEXT("'%s' is not a concrete %s class."), *Text, *Base.GetName()), Sub(Parsed.Path, TEXT("class")));
   const FValue* Properties = Field(Row, TEXT("properties"));
   auto R = ResolveProperties(*Parsed.Object.Class, Properties ? *Properties : MakeObject({}, {}), Sub(Parsed.Path, TEXT("properties")), Parsed.Object.Properties);
   if (!R.bValid) return R;
  }
  else
  {
   if (bStates) return Invalid(InvalidIntent, TEXT("A notify state requires its class."), Sub(Parsed.Path, TEXT("class")));
   if (Field(Row, TEXT("properties"))) return Invalid(InvalidIntent, TEXT("A named notify has no object properties."), Sub(Parsed.Path, TEXT("properties")));
   if (!ReadText(Field(Row, TEXT("name")), Text) || Text.IsEmpty())
    return Invalid(InvalidIntent, TEXT("A notify without a class requires a name."), Sub(Parsed.Path, TEXT("name")));
   Parsed.Name = FName(*Text);
  }
  auto R = ParseTrigger(Field(Row, TEXT("trigger")), Sub(Parsed.Path, TEXT("trigger")), Parsed.Trigger);
  if (!R.bValid) return R;
 }
 return Valid();
}

FResult ParseKey(const FValue& Value, const FString& Path, FRichCurveKey& Out)
{
 if (!OnlyKeys(Value, {TEXT("Time"), TEXT("Value"), TEXT("InterpMode"), TEXT("TangentMode"), TEXT("TangentWeightMode"),
  TEXT("ArriveTangent"), TEXT("ArriveTangentWeight"), TEXT("LeaveTangent"), TEXT("LeaveTangentWeight")}))
  return Invalid(InvalidIntent, TEXT("Unknown curve key field."), Path);
 if (!ReadFloat(Field(Value, TEXT("Time")), Out.Time) || !ReadFloat(Field(Value, TEXT("Value")), Out.Value))
  return Invalid(InvalidIntent, TEXT("Curve keys require numeric Time and Value."), Path);
 auto Optional = [&](const TCHAR* Key, float& Target) { const FValue* V = Field(Value, Key); return !V || ReadFloat(V, Target); };
 if (!Optional(TEXT("ArriveTangent"), Out.ArriveTangent) || !Optional(TEXT("ArriveTangentWeight"), Out.ArriveTangentWeight)
  || !Optional(TEXT("LeaveTangent"), Out.LeaveTangent) || !Optional(TEXT("LeaveTangentWeight"), Out.LeaveTangentWeight))
  return Invalid(InvalidIntent, TEXT("Curve tangents must be numbers."), Path);
 if (const FValue* V = Field(Value, TEXT("InterpMode")))
 { ERichCurveInterpMode Mode; if (!ReadEnum(V, Mode)) return Invalid(InvalidIntent, TEXT("Unknown InterpMode."), Path); Out.InterpMode = Mode; }
 if (const FValue* V = Field(Value, TEXT("TangentMode")))
 { ERichCurveTangentMode Mode; if (!ReadEnum(V, Mode)) return Invalid(InvalidIntent, TEXT("Unknown TangentMode."), Path); Out.TangentMode = Mode; }
 if (const FValue* V = Field(Value, TEXT("TangentWeightMode")))
 { ERichCurveTangentWeightMode Mode; if (!ReadEnum(V, Mode)) return Invalid(InvalidIntent, TEXT("Unknown TangentWeightMode."), Path); Out.TangentWeightMode = Mode; }
 return Valid();
}

FResult ParseCurves(const FValue* Value, const FString& Path, TArray<FCurveRow>& Out)
{
 if (!Value || Value->Kind != EKind::Array) return Invalid(InvalidIntent, TEXT("Curves must be an array."), Path);
 TSet<FName> Names;
 for (int32 Index = 0; Index < Value->Elements.Num(); ++Index)
 {
  const FValue& Row = Value->Elements[Index];
  const FString RowPath = Sub(Path, Index);
  FCurveRow& Curve = Out.AddDefaulted_GetRef();
  FString Name;
  if (!OnlyKeys(Row, {TEXT("name"), TEXT("keys"), TEXT("flags"), TEXT("color"), TEXT("comment")}) || !ReadText(Field(Row, TEXT("name")), Name) || Name.IsEmpty())
   return Invalid(InvalidIntent, TEXT("Curve rows require a nonempty name."), RowPath);
  Curve.Name = FName(*Name);
  if (Names.Contains(Curve.Name)) return Invalid(InvalidIntent, TEXT("A curve name is listed twice."), Sub(RowPath, TEXT("name")));
  Names.Add(Curve.Name);
  const FValue* Keys = Field(Row, TEXT("keys"));
  if (!Keys || Keys->Kind != EKind::Array) return Invalid(InvalidIntent, TEXT("Curve keys must be an array."), Sub(RowPath, TEXT("keys")));
  for (int32 KeyIndex = 0; KeyIndex < Keys->Elements.Num(); ++KeyIndex)
  {
   FRichCurveKey& Key = Curve.Keys.AddDefaulted_GetRef();
   auto R = ParseKey(Keys->Elements[KeyIndex], Sub(Sub(RowPath, TEXT("keys")), KeyIndex), Key);
   if (!R.bValid) return R;
   if (KeyIndex > 0 && Key.Time <= Curve.Keys[KeyIndex - 1].Time)
    return Invalid(InvalidIntent, TEXT("Curve key times must be strictly ascending."), Sub(Sub(RowPath, TEXT("keys")), KeyIndex));
  }
  if (const FValue* V = Field(Row, TEXT("flags")))
  {
   int64 Flags = 0;
   constexpr int64 Known = AACF_Editable | AACF_Metadata | AACF_DriveTrack | AACF_Disabled;
   if (!ReadInt(V, Flags) || (Flags & ~Known) != 0) return Invalid(InvalidIntent, TEXT("Curve flags hold unknown bits."), Sub(RowPath, TEXT("flags")));
   Curve.Flags = int32(Flags);
  }
  if (const FValue* Color = Field(Row, TEXT("color")); Color && Color->Kind != EKind::Null)
  {
   FLinearColor Parsed;
   if (!OnlyKeys(*Color, {TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")}) || !ReadFloat(Field(*Color, TEXT("R")), Parsed.R)
    || !ReadFloat(Field(*Color, TEXT("G")), Parsed.G) || !ReadFloat(Field(*Color, TEXT("B")), Parsed.B) || !ReadFloat(Field(*Color, TEXT("A")), Parsed.A))
    return Invalid(InvalidIntent, TEXT("Curve color requires numeric R, G, B and A."), Sub(RowPath, TEXT("color")));
   Curve.Color = Parsed;
  }
  if (const FValue* Comment = Field(Row, TEXT("comment")))
  {
   FString Text;
   if (!ReadText(Comment, Text)) return Invalid(InvalidIntent, TEXT("Curve comment must be a string."), Sub(RowPath, TEXT("comment")));
   Curve.Comment = Text;
  }
 }
 return Valid();
}

FResult ParseIntent(const FValue& Value, FIntent& Out)
{
 if (!ValidateAssetDocumentRuntimeValue(Value).bValid || !OnlyKeys(Value, {TEXT("skeleton"), TEXT("preview_mesh"), TEXT("object_properties"),
  TEXT("frame_rate"), TEXT("number_of_frames"), TEXT("curves"), TEXT("notify_tracks"), TEXT("notifies"), TEXT("notify_states"),
  TEXT("sync_markers"), TEXT("metadata"), TEXT("asset_user_data")}))
  return Invalid(InvalidIntent, TEXT("Animation authoring takes a sparse object of known sections."));
 FString Text;
 if (const FValue* V = Field(Value, TEXT("skeleton")))
 {
  if (!ReadText(V, Text) || Text.IsEmpty()) return Invalid(InvalidIntent, TEXT("Skeleton must be an object path."), TEXT("/skeleton"));
  Out.Skeleton = Text;
 }
 if (const FValue* V = Field(Value, TEXT("preview_mesh")))
 {
  if (!ReadOptionalPath(V, Text)) return Invalid(InvalidIntent, TEXT("Preview mesh must be a SkeletalMesh path or null."), TEXT("/preview_mesh"));
  if (!Text.IsEmpty() && !(Out.PreviewMeshObject = LoadExact(Text, *USkeletalMesh::StaticClass())))
   return Invalid(Unresolved, FString::Printf(TEXT("'%s' is not a SkeletalMesh."), *Text), TEXT("/preview_mesh"));
  Out.PreviewMesh = Text;
 }
 if (const FValue* V = Field(Value, TEXT("object_properties")))
 {
  // Hard object/class references only: plain fields go through the Editor batch.
  if (V->Kind != EKind::Object) return Invalid(InvalidIntent, TEXT("Object properties must be an object."), TEXT("/object_properties"));
  for (const FString& Name : V->ObjectKeys)
   if (!FindReference(*UAnimSequence::StaticClass(), Name))
    return Invalid(InvalidIntent, FString::Printf(TEXT("'%s' is not an editable object reference of AnimSequence."), *Name), Sub(TEXT("/object_properties"), Name));
  auto R = ResolveProperties(*UAnimSequence::StaticClass(), *V, TEXT("/object_properties"), Out.References.Emplace());
  if (!R.bValid) return R;
 }
 if (const FValue* V = Field(Value, TEXT("frame_rate")))
 {
  int64 Numerator = 0, Denominator = 0;
  if (!OnlyKeys(*V, {TEXT("numerator"), TEXT("denominator")}) || !ReadInt(Field(*V, TEXT("numerator")), Numerator)
   || !ReadInt(Field(*V, TEXT("denominator")), Denominator) || Numerator <= 0 || Denominator <= 0 || Numerator > MAX_int32 || Denominator > MAX_int32)
   return Invalid(InvalidIntent, TEXT("Frame rate requires positive integer numerator and denominator."), TEXT("/frame_rate"));
  Out.FrameRate = FFrameRate(int32(Numerator), int32(Denominator));
 }
 if (const FValue* V = Field(Value, TEXT("number_of_frames")))
 {
  int64 Frames = 0;
  if (!ReadInt(V, Frames) || Frames < 1 || Frames > MAX_int32) return Invalid(InvalidIntent, TEXT("Number of frames must be a positive integer."), TEXT("/number_of_frames"));
  Out.NumberOfFrames = int32(Frames);
 }
 if (const FValue* V = Field(Value, TEXT("curves")))
 {
  auto R = ParseCurves(V, TEXT("/curves"), Out.Curves.Emplace());
  if (!R.bValid) return R;
 }
 if (const FValue* V = Field(Value, TEXT("notify_tracks")))
 {
  if (V->Kind != EKind::Array || V->Elements.IsEmpty())
   return Invalid(InvalidIntent, TEXT("Notify tracks must list at least one track (UE always keeps one)."), TEXT("/notify_tracks"));
  auto& Tracks = Out.Tracks.Emplace();
  for (int32 Index = 0; Index < V->Elements.Num(); ++Index)
  {
   if (!ReadText(&V->Elements[Index], Text) || Text.IsEmpty() || Tracks.Contains(FName(*Text)))
    return Invalid(InvalidIntent, TEXT("Notify track names must be unique and nonempty."), Sub(TEXT("/notify_tracks"), Index));
   Tracks.Add(FName(*Text));
  }
 }
 for (const bool bStates : {false, true})
  if (const FValue* V = Field(Value, bStates ? TEXT("notify_states") : TEXT("notifies")))
  {
   auto R = ParseNotifies(V, bStates, bStates ? TEXT("/notify_states") : TEXT("/notifies"), (bStates ? Out.States : Out.Notifies).Emplace());
   if (!R.bValid) return R;
  }
 if (const FValue* V = Field(Value, TEXT("sync_markers")))
 {
  if (V->Kind != EKind::Array) return Invalid(InvalidIntent, TEXT("Sync markers must be an array."), TEXT("/sync_markers"));
  auto& Markers = Out.Markers.Emplace();
  for (int32 Index = 0; Index < V->Elements.Num(); ++Index)
  {
   const FValue& Row = V->Elements[Index];
   FMarkerRow& Marker = Markers.AddDefaulted_GetRef();
   Marker.Path = Sub(TEXT("/sync_markers"), Index);
   FString Track;
   const FValue* TrackValue = Field(Row, TEXT("track"));
   if (!OnlyKeys(Row, {TEXT("name"), TEXT("time"), TEXT("track")}) || !ReadText(Field(Row, TEXT("name")), Text) || Text.IsEmpty()
    || !ReadFloat(Field(Row, TEXT("time")), Marker.Time) || Marker.Time < 0.f || (TrackValue && (!ReadText(TrackValue, Track) || Track.IsEmpty())))
    return Invalid(InvalidIntent, TEXT("Sync markers require name, nonnegative time and an optional track name."), Marker.Path);
   Marker.Name = FName(*Text); Marker.Track = Track.IsEmpty() ? NAME_None : FName(*Track);
  }
 }
 if (const FValue* V = Field(Value, TEXT("metadata")))
 {
  auto R = ParseObjectRows(V, *UAnimMetaData::StaticClass(), TEXT("/metadata"), Out.Metadata.Emplace());
  if (!R.bValid) return R;
 }
 if (const FValue* V = Field(Value, TEXT("asset_user_data")))
 {
  auto R = ParseObjectRows(V, *UAssetUserData::StaticClass(), TEXT("/asset_user_data"), Out.UserData.Emplace());
  if (!R.bValid) return R;
 }
 return Valid();
}

// ---------------------------------------------------------------------------
// Apply
// ---------------------------------------------------------------------------
TArray<FName> CurrentTracks(const UAnimSequenceBase& Sequence)
{
 TArray<FName> Names;
#if WITH_EDITORONLY_DATA
 for (const FAnimNotifyTrack& Track : Sequence.AnimNotifyTracks) Names.Add(Track.TrackName);
#endif
 return Names;
}
// An omitted track means the first notify track.
FName EffectiveTrack(FName Track, const TArray<FName>& Tracks) { return Track.IsNone() && !Tracks.IsEmpty() ? Tracks[0] : Track; }
FName TrackNameAt(const TArray<FName>& Tracks, int32 Index)
{
 return Tracks.IsValidIndex(Index) ? Tracks[Index] : (Tracks.IsEmpty() ? FName(TEXT("1")) : Tracks[0]);
}

void ApplyTrigger(FAnimNotifyEvent& Event, const FTrigger& Trigger)
{
 if (Trigger.WeightThreshold) Event.TriggerWeightThreshold = *Trigger.WeightThreshold;
 if (Trigger.Chance) Event.NotifyTriggerChance = *Trigger.Chance;
 if (Trigger.FilterType) Event.NotifyFilterType = *Trigger.FilterType;
 if (Trigger.FilterLOD) Event.NotifyFilterLOD = *Trigger.FilterLOD;
 if (Trigger.FilterableByRequest) Event.bCanBeFilteredViaRequest = *Trigger.FilterableByRequest;
 if (Trigger.DedicatedServer) Event.bTriggerOnDedicatedServer = *Trigger.DedicatedServer;
 if (Trigger.Follower) Event.bTriggerOnFollower = *Trigger.Follower;
 if (Trigger.TickType) Event.MontageTickType = *Trigger.TickType;
}

UClass* EventClass(const FAnimNotifyEvent& Event)
{
 if (Event.Notify) return Event.Notify->GetClass();
 if (Event.NotifyStateClass) return Event.NotifyStateClass->GetClass();
 return nullptr;
}

// Builds the replacement rows of one notify family. The existing row at the
// same position is reused (object, guid, unspecified settings) when its class
// matches, like editing that notify in place; otherwise a fresh event is made.
FResult BuildNotifyRows(UAnimSequenceBase& Sequence, const TArray<FNotifyRow>& Rows, const TArray<FAnimNotifyEvent>& Previous,
 const TArray<FName>& Tracks, bool bStates, TArray<FAnimNotifyEvent>& Out)
{
 for (int32 Index = 0; Index < Rows.Num(); ++Index)
 {
  const FNotifyRow& Row = Rows[Index];
  const FName Track = EffectiveTrack(Row.Track, Tracks);
  const int32 TrackIndex = Tracks.IndexOfByKey(Track);
  if (TrackIndex == INDEX_NONE)
   return Invalid(UnknownTrack, FString::Printf(TEXT("Notify track '%s' does not exist."), *Track.ToString()), Sub(Row.Path, TEXT("track")));
  const FAnimNotifyEvent* Reused = Previous.IsValidIndex(Index) && EventClass(Previous[Index]) == Row.Object.Class
   && (!Row.Object.Class || (bStates ? Previous[Index].NotifyStateClass->IsIn(&Sequence) : Previous[Index].Notify->IsIn(&Sequence)))
   ? &Previous[Index] : nullptr;
  FAnimNotifyEvent Event = Reused ? *Reused : FAnimNotifyEvent();
  FGuid Guid = FGuid::NewGuid();
#if WITH_EDITORONLY_DATA
  if (Reused && Reused->Guid.IsValid()) Guid = Reused->Guid;
#endif
  if (Row.Object.Class)
  {
   UObject* Object = Reused ? (bStates ? static_cast<UObject*>(Reused->NotifyStateClass) : static_cast<UObject*>(Reused->Notify))
    : AssetDocumentAnimationAuthoringRaw::NewInstancedSubobject(Sequence, *Row.Object.Class);
   if (!Object) return Invalid(InvalidIntent, TEXT("A notify object could not be allocated."), Row.Path);
   if (Reused) Object->Modify();
   auto R = WriteProperties(*Object, Row.Object.Properties, Sub(Row.Path, TEXT("properties")));
   if (!R.bValid) return R;
   Event.Notify = bStates ? nullptr : Cast<UAnimNotify>(Object);
   Event.NotifyStateClass = bStates ? Cast<UAnimNotifyState>(Object) : nullptr;
   Event.NotifyName = AssetDocumentAnimationAuthoringRaw::MakeNotifyName(*Row.Object.Class);
   if (!Reused)
    Event.TriggerWeightThreshold = bStates ? Event.NotifyStateClass->GetDefaultTriggerWeightThreshold() : Event.Notify->GetDefaultTriggerWeightThreshold();
  }
  else
  {
   Event.Notify = nullptr; Event.NotifyStateClass = nullptr; Event.NotifyName = Row.Name;
  }
  AssetDocumentAnimationAuthoringRaw::PlaceNotify(Sequence, Event, Row.Time, TrackIndex, Guid);
  if (bStates) AssetDocumentAnimationAuthoringRaw::PlaceNotifyStateEnd(Sequence, Event, Row.Duration);
  else { Event.Duration = 0.f; Event.EndTriggerTimeOffset = 0.f; }
  ApplyTrigger(Event, Row.Trigger);
  Out.Add(MoveTemp(Event));
 }
 return Valid();
}

FResult ValidateTimeline(const UAnimSequenceBase& Sequence, const FIntent& Intent, float PlayLength, const TArray<FName>& FinalTracks)
{
 const TArray<FName> Tracks = CurrentTracks(Sequence);
 // Unauthored families keep their rows; they must still find their track by name.
 auto CheckKept = [&](bool bStates) -> FResult
 {
  for (const FAnimNotifyEvent& Event : Sequence.Notifies)
  {
   if ((Event.NotifyStateClass != nullptr) != bStates) continue;
   const FName Name = TrackNameAt(Tracks, Event.TrackIndex);
   if (!FinalTracks.Contains(Name))
    return Invalid(TrackInUse, FString::Printf(TEXT("Notify track '%s' is still used by an unlisted notify."), *Name.ToString()), TEXT("/notify_tracks"));
  }
  return Valid();
 };
 if (Intent.Tracks)
 {
  if (!Intent.Notifies) { auto R = CheckKept(false); if (!R.bValid) return R; }
  if (!Intent.States) { auto R = CheckKept(true); if (!R.bValid) return R; }
  if (!Intent.Markers)
   if (const UAnimSequence* AsSequence = Cast<UAnimSequence>(&Sequence))
    for (const FAnimSyncMarker& Marker : AsSequence->AuthoredSyncMarkers)
    {
#if WITH_EDITORONLY_DATA
     const FName Name = TrackNameAt(Tracks, Marker.TrackIndex);
     if (!FinalTracks.Contains(Name))
      return Invalid(TrackInUse, FString::Printf(TEXT("Notify track '%s' is still used by a sync marker."), *Name.ToString()), TEXT("/notify_tracks"));
#endif
    }
 }
 const float Limit = PlayLength + TimeTolerance;
 for (const auto* Rows : {Intent.Notifies.GetPtrOrNull(), Intent.States.GetPtrOrNull()})
  if (Rows)
   for (const FNotifyRow& Row : *Rows)
   {
    if (Row.Time > Limit || Row.Time + Row.Duration > Limit)
     return Invalid(OutOfRange, FString::Printf(TEXT("Notify time %g (+ duration %g) exceeds the play length %g."), Row.Time, Row.Duration, PlayLength), Sub(Row.Path, TEXT("time")));
    if (!FinalTracks.Contains(EffectiveTrack(Row.Track, FinalTracks)))
     return Invalid(UnknownTrack, FString::Printf(TEXT("Notify track '%s' does not exist."), *Row.Track.ToString()), Sub(Row.Path, TEXT("track")));
   }
 if (Intent.Markers)
  for (const FMarkerRow& Marker : *Intent.Markers)
  {
   if (Marker.Time > Limit)
    return Invalid(OutOfRange, FString::Printf(TEXT("Sync marker time %g exceeds the play length %g."), Marker.Time, PlayLength), Sub(Marker.Path, TEXT("time")));
   if (!FinalTracks.Contains(EffectiveTrack(Marker.Track, FinalTracks)))
    return Invalid(UnknownTrack, FString::Printf(TEXT("Notify track '%s' does not exist."), *Marker.Track.ToString()), Sub(Marker.Path, TEXT("track")));
  }
 return Valid();
}

FResult CheckOverlaps(const TArray<FAnimNotifyEvent>& Events, const TArray<FName>& Tracks)
{
 for (int32 A = 0; A < Events.Num(); ++A)
  for (int32 B = A + 1; B < Events.Num(); ++B)
   if (Events[A].TrackIndex == Events[B].TrackIndex && FMath::IsNearlyEqual(Events[A].GetTime(), Events[B].GetTime()))
    return Invalid(Overlap, FString::Printf(TEXT("Two notifies share track '%s' at time %g; UE would move one to another track."),
     *TrackNameAt(Tracks, Events[A].TrackIndex).ToString(), Events[A].GetTime()), TEXT("/notifies"));
 return Valid();
}

FResult ApplyCurves(UAnimSequenceBase& Sequence, const TArray<FCurveRow>& Curves)
{
 IAnimationDataController& Controller = Sequence.GetController();
 const IAnimationDataModel* Model = Sequence.GetDataModel();
 if (!Model) return Invalid(ControllerFailed, TEXT("The sequence has no data model."), TEXT("/curves"));
 TArray<FName> Existing;
 for (const FFloatCurve& Curve : Model->GetFloatCurves()) Existing.Add(Curve.GetName());
 Controller.OpenBracket(FText::FromString(TEXT("AssetDocument curves")), false);
 FResult Result = Valid();
 for (const FName& Name : Existing)
  if (!Curves.ContainsByPredicate([&](const FCurveRow& Row) { return Row.Name == Name; }))
   if (!Controller.RemoveCurve(FAnimationCurveIdentifier(Name, ERawCurveTrackTypes::RCT_Float), false))
   { Result = Invalid(ControllerFailed, FString::Printf(TEXT("Curve '%s' could not be removed."), *Name.ToString()), TEXT("/curves")); break; }
 for (int32 Index = 0; Result.bValid && Index < Curves.Num(); ++Index)
 {
  const FCurveRow& Row = Curves[Index];
  const FAnimationCurveIdentifier Id(Row.Name, ERawCurveTrackTypes::RCT_Float);
  // Omitted flags / color / comment keep the curve's value (new curve: UE default).
  const bool bOk = (Existing.Contains(Row.Name) || Controller.AddCurve(Id, Row.Flags.Get(AACF_DefaultCurve), false))
   && Controller.SetCurveKeys(Id, Row.Keys, false)
   && (!Row.Flags || Controller.SetCurveFlags(Id, *Row.Flags, false))
   && (!Row.Color || Controller.SetCurveColor(Id, *Row.Color, false))
   && (!Row.Comment || Controller.SetCurveComment(Id, *Row.Comment, false));
  if (!bOk) Result = Invalid(ControllerFailed, FString::Printf(TEXT("Curve '%s' could not be written."), *Row.Name.ToString()), Sub(TEXT("/curves"), Index));
 }
 Controller.CloseBracket(false);
 return Result;
}

FResult BuildObjects(UObject& Outer, const TArray<UObject*>& Current, const TArray<FObjectRow>& Rows, TArray<UObject*>& OutNext)
{
 for (int32 Index = 0; Index < Rows.Num(); ++Index)
 {
  UObject* Existing = Current.IsValidIndex(Index) ? Current[Index] : nullptr;
  UObject* Target = IsValid(Existing) && Existing->GetClass() == Rows[Index].Class && Existing->GetOuter() == &Outer
   ? Existing : AssetDocumentAnimationAuthoringRaw::NewInstancedSubobject(Outer, *Rows[Index].Class);
  if (!Target) return Invalid(InvalidIntent, TEXT("An instanced object could not be allocated."), Rows[Index].Path);
  Target->Modify();
  auto R = WriteProperties(*Target, Rows[Index].Properties, Sub(Rows[Index].Path, TEXT("properties")));
  if (!R.bValid) return R;
  OutNext.Add(Target);
 }
 return Valid();
}

FResult Apply(UAnimSequence& Sequence, const FIntent& Intent)
{
 if (Intent.Skeleton && (!Sequence.GetSkeleton() || Sequence.GetSkeleton()->GetPathName() != *Intent.Skeleton))
  return Invalid(SkeletonMismatch, FString::Printf(TEXT("The sequence uses skeleton '%s'; changing it (Replace Skeleton) is not an authoring edit."),
   *GetPathNameSafe(Sequence.GetSkeleton())), TEXT("/skeleton"));
 const IAnimationDataModel* Model = Sequence.GetDataModel();
 if (!Model) return Invalid(ControllerFailed, TEXT("The sequence has no data model."));
 const FFrameRate Rate = Intent.FrameRate.Get(Model->GetFrameRate());
 const int32 Frames = Intent.NumberOfFrames.Get(Model->GetNumberOfFrames());
 const float PlayLength = Intent.FrameRate || Intent.NumberOfFrames ? float(Rate.AsSeconds(FFrameNumber(Frames))) : Sequence.GetPlayLength();
 const TArray<FName> PreviousTracks = CurrentTracks(Sequence);
 const TArray<FName> FinalTracks = Intent.Tracks ? *Intent.Tracks : (PreviousTracks.IsEmpty() ? TArray<FName>{FName(TEXT("1"))} : PreviousTracks);
 auto R = ValidateTimeline(Sequence, Intent, PlayLength, FinalTracks);
 if (!R.bValid) return R;
 // The preview mesh picker offers only meshes whose skeleton is compatible.
 if (const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Intent.PreviewMeshObject))
  if (!Mesh->GetSkeleton() || !Mesh->GetSkeleton()->IsCompatibleForEditor(Sequence.GetSkeleton()))
   return Invalid(SkeletonMismatch, FString::Printf(TEXT("Preview mesh '%s' does not use this sequence's skeleton."), *Mesh->GetPathName()), TEXT("/preview_mesh"));
 FObjectProperties References;
 if (Intent.References) References = *Intent.References;

 Sequence.Modify();
 if (Intent.FrameRate || Intent.NumberOfFrames)
 {
  IAnimationDataController& Controller = Sequence.GetController();
  Controller.OpenBracket(FText::FromString(TEXT("AssetDocument frame data")), false);
  if (Intent.FrameRate && Model->GetFrameRate() != *Intent.FrameRate) Controller.SetFrameRate(*Intent.FrameRate, false);
  if (Model->GetNumberOfFrames() != Frames) Controller.SetNumberOfFrames(FFrameNumber(Frames), false);
  Controller.CloseBracket(false);
  if (Model->GetFrameRate() != Rate || Model->GetNumberOfFrames() != Frames)
   return Invalid(ControllerFailed, TEXT("The frame rate / number of frames did not apply."), TEXT("/number_of_frames"));
 }
 if (Intent.Curves) { R = ApplyCurves(Sequence, *Intent.Curves); if (!R.bValid) return R; }

 if (Intent.Tracks || Intent.Notifies || Intent.States)
 {
  TArray<FAnimNotifyEvent> PreviousEvents, PreviousStates, Next;
  for (const FAnimNotifyEvent& Event : Sequence.Notifies) (Event.NotifyStateClass ? PreviousStates : PreviousEvents).Add(Event);
  // Kept rows follow their track by name.
  auto Keep = [&](const TArray<FAnimNotifyEvent>& Rows)
  {
   for (FAnimNotifyEvent Event : Rows)
   {
    Event.TrackIndex = FinalTracks.IndexOfByKey(TrackNameAt(PreviousTracks, Event.TrackIndex));
    Next.Add(MoveTemp(Event));
   }
  };
  if (Intent.Notifies) { R = BuildNotifyRows(Sequence, *Intent.Notifies, PreviousEvents, FinalTracks, false, Next); if (!R.bValid) return R; }
  else Keep(PreviousEvents);
  if (Intent.States) { R = BuildNotifyRows(Sequence, *Intent.States, PreviousStates, FinalTracks, true, Next); if (!R.bValid) return R; }
  else Keep(PreviousStates);
  R = CheckOverlaps(Next, FinalTracks);
  if (!R.bValid) return R;
  AssetDocumentAnimationAuthoringRaw::SetNotifyTracks(Sequence, FinalTracks);
  AssetDocumentAnimationAuthoringRaw::SetNotifies(Sequence, MoveTemp(Next));
 }
 if (Intent.Markers || Intent.Tracks)
 {
  TArray<AssetDocumentAnimationAuthoringRaw::FSyncMarkerRow> Rows;
  if (Intent.Markers)
   for (int32 Index = 0; Index < Intent.Markers->Num(); ++Index)
   {
    const FMarkerRow& Marker = (*Intent.Markers)[Index];
    FGuid Guid;
#if WITH_EDITORONLY_DATA
    if (Sequence.AuthoredSyncMarkers.IsValidIndex(Index) && Sequence.AuthoredSyncMarkers[Index].MarkerName == Marker.Name)
     Guid = Sequence.AuthoredSyncMarkers[Index].Guid;
#endif
    Rows.Add({Marker.Name, Marker.Time, FinalTracks.IndexOfByKey(EffectiveTrack(Marker.Track, FinalTracks)), Guid});
   }
  else
   for (const FAnimSyncMarker& Marker : Sequence.AuthoredSyncMarkers)
   {
#if WITH_EDITORONLY_DATA
    Rows.Add({Marker.MarkerName, Marker.Time, FinalTracks.IndexOfByKey(TrackNameAt(PreviousTracks, Marker.TrackIndex)), Marker.Guid});
#else
    Rows.Add({Marker.MarkerName, Marker.Time, 0, FGuid()});
#endif
   }
  AssetDocumentAnimationAuthoringRaw::SetAuthoredSyncMarkers(Sequence, Rows);
 }
 if (Intent.PreviewMesh) Sequence.SetPreviewMesh(Cast<USkeletalMesh>(Intent.PreviewMeshObject), true);
 if (Intent.References) { R = WriteProperties(Sequence, References, TEXT("/object_properties")); if (!R.bValid) return R; }
 if (Intent.Metadata)
 {
  TArray<UObject*> Current(Sequence.GetMetaData()), Next;
  R = BuildObjects(Sequence, Current, *Intent.Metadata, Next);
  if (!R.bValid) return R;
  Sequence.EmptyMetaData();
  for (UObject* Meta : Next) Sequence.AddMetaData(CastChecked<UAnimMetaData>(Meta));
 }
 if (Intent.UserData)
 {
  TArray<UObject*> Current, Next;
  if (const TArray<UAssetUserData*>* Existing = Sequence.GetAssetUserDataArray()) for (UAssetUserData* Data : *Existing) Current.Add(Data);
  R = BuildObjects(Sequence, Current, *Intent.UserData, Next);
  if (!R.bValid) return R;
  for (UObject* Data : Current) if (Data) Sequence.RemoveUserDataOfClass(Data->GetClass());
  for (UObject* Data : Next) Sequence.AddAssetUserData(CastChecked<UAssetUserData>(Data));
 }
 // The notify panel's completion: cache refresh (sorted notifies, track
 // bookkeeping), PostEditChange (compression / derived data), dirty package.
 Sequence.RefreshCacheData();
 Sequence.PostEditChange();
 Sequence.MarkPackageDirty();
 return Valid();
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------
FValue CaptureTrigger(const FAnimNotifyEvent& Event)
{
 return MakeObject({TEXT("TriggerWeightThreshold"), TEXT("NotifyTriggerChance"), TEXT("NotifyFilterType"), TEXT("NotifyFilterLOD"),
  TEXT("bCanBeFilteredViaRequest"), TEXT("bTriggerOnDedicatedServer"), TEXT("bTriggerOnFollower"), TEXT("MontageTickType")},
  {Number(Event.TriggerWeightThreshold), Number(Event.NotifyTriggerChance), String(EnumName(Event.NotifyFilterType.GetValue())),
   Integer(Event.NotifyFilterLOD), Bool(Event.bCanBeFilteredViaRequest), Bool(Event.bTriggerOnDedicatedServer),
   Bool(Event.bTriggerOnFollower), String(EnumName(Event.MontageTickType.GetValue()))});
}

bool CaptureObjectRows(UObject& Owner, TConstArrayView<UObject*> Objects, FValue& Out, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
 Out = Array();
 for (UObject* Item : Objects)
 {
  if (!IsValid(Item)) continue;
  FValue Properties = MakeObject({}, {});
  // Only objects instanced in this asset have authorable properties.
  if (!Item->IsTemplate() && Item->IsIn(&Owner) && !ReadProperties(*Item, Properties, Diagnostics)) return false;
  Out.Elements.Add(MakeObject({TEXT("class"), TEXT("properties")}, {String(Item->GetClass()->GetPathName()), MoveTemp(Properties)}));
 }
 return true;
}

FValue CaptureKey(const FRichCurveKey& Key)
{
 return MakeObject({TEXT("Time"), TEXT("Value"), TEXT("InterpMode"), TEXT("TangentMode"), TEXT("TangentWeightMode"),
  TEXT("ArriveTangent"), TEXT("ArriveTangentWeight"), TEXT("LeaveTangent"), TEXT("LeaveTangentWeight")},
  {Number(Key.Time), Number(Key.Value), String(EnumName(Key.InterpMode.GetValue())), String(EnumName(Key.TangentMode.GetValue())),
   String(EnumName(Key.TangentWeightMode.GetValue())), Number(Key.ArriveTangent), Number(Key.ArriveTangentWeight),
   Number(Key.LeaveTangent), Number(Key.LeaveTangentWeight)});
}
FValue CaptureColor(const FLinearColor& Color)
{
 return MakeObject({TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")}, {Number(Color.R), Number(Color.G), Number(Color.B), Number(Color.A)});
}

bool Capture(UAnimSequence& Sequence, FValue& Out, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
 const IAnimationDataModel* Model = Sequence.GetDataModel();
 if (!Model)
 {
  Diagnostics.Append(Invalid(SnapshotFailed, TEXT("The sequence has no data model.")).Diagnostics);
  return false;
 }
 Out = MakeObject({}, {});
 Put(Out, TEXT("skeleton"), OptionalPath(Sequence.GetSkeleton()));
 // The const accessor: the mutable one clears an incompatible mesh (a write).
 Put(Out, TEXT("preview_mesh"), OptionalPath(static_cast<const UAnimationAsset&>(Sequence).GetPreviewMesh()));
 FValue Properties = MakeObject({}, {});
 if (!ReadAssetDocumentEditorSubobjectProperties(Sequence, Properties, Diagnostics)) return false;
 Put(Out, TEXT("properties"), MoveTemp(Properties));
 Put(Out, TEXT("object_properties"), ReadReferences(Sequence));
 const FFrameRate Rate = Model->GetFrameRate();
 Put(Out, TEXT("frame_rate"), MakeObject({TEXT("numerator"), TEXT("denominator")}, {Integer(Rate.Numerator), Integer(Rate.Denominator)}));
 Put(Out, TEXT("number_of_frames"), Integer(Model->GetNumberOfFrames()));
 Put(Out, TEXT("play_length"), Number(Sequence.GetPlayLength()));
 TArray<FName> Bones;
 Model->GetBoneTrackNames(Bones);
 FValue BoneRows = Array();
 for (const FName& Bone : Bones) BoneRows.Elements.Add(String(Bone.ToString()));
 Put(Out, TEXT("bone_tracks"), MoveTemp(BoneRows));
 FValue Curves = Array();
 for (const FFloatCurve& Curve : Model->GetFloatCurves())
 {
  FValue Keys = Array();
  for (const FRichCurveKey& Key : Curve.FloatCurve.GetConstRefOfKeys()) Keys.Elements.Add(CaptureKey(Key));
  Curves.Elements.Add(MakeObject({TEXT("name"), TEXT("keys"), TEXT("flags"), TEXT("color"), TEXT("default_color"), TEXT("comment")},
   {String(Curve.GetName().ToString()), MoveTemp(Keys), Integer(Curve.GetCurveTypeFlags()), CaptureColor(Curve.GetColor()),
    CaptureColor(FAnimCurveBase::MakeColor(Curve.GetName())), String(Curve.Comment)}));
 }
 Put(Out, TEXT("curves"), MoveTemp(Curves));
 const TArray<FName> Tracks = CurrentTracks(Sequence);
 FValue TrackRows = Array();
 for (const FName& Track : Tracks) TrackRows.Elements.Add(String(Track.ToString()));
 Put(Out, TEXT("notify_tracks"), MoveTemp(TrackRows));
 FValue Events = Array(), States = Array();
 for (const FAnimNotifyEvent& Event : Sequence.Notifies)
 {
  UObject* Item = Event.NotifyStateClass ? static_cast<UObject*>(Event.NotifyStateClass) : static_cast<UObject*>(Event.Notify);
  FValue Row = MakeObject({}, {});
  Put(Row, TEXT("time"), Number(Event.GetTime()));
  if (Event.NotifyStateClass) Put(Row, TEXT("duration"), Number(Event.GetDuration()));
  Put(Row, TEXT("track"), String(TrackNameAt(Tracks, Event.TrackIndex).ToString()));
  Put(Row, TEXT("class"), Item ? String(Item->GetClass()->GetPathName()) : Null());
  Put(Row, TEXT("name"), String(Event.NotifyName.ToString()));
  FValue ItemProperties = MakeObject({}, {});
  if (Item && !Item->IsTemplate() && Item->IsIn(&Sequence) && !ReadProperties(*Item, ItemProperties, Diagnostics)) return false;
  Put(Row, TEXT("properties"), MoveTemp(ItemProperties));
  Put(Row, TEXT("trigger"), CaptureTrigger(Event));
  (Event.NotifyStateClass ? States : Events).Elements.Add(MoveTemp(Row));
 }
 Put(Out, TEXT("notifies"), MoveTemp(Events));
 Put(Out, TEXT("notify_states"), MoveTemp(States));
 FValue Markers = Array();
 for (const FAnimSyncMarker& Marker : Sequence.AuthoredSyncMarkers)
 {
  int32 TrackIndex = 0;
#if WITH_EDITORONLY_DATA
  TrackIndex = Marker.TrackIndex;
#endif
  Markers.Elements.Add(MakeObject({TEXT("name"), TEXT("time"), TEXT("track")},
   {String(Marker.MarkerName.ToString()), Number(Marker.Time), String(TrackNameAt(Tracks, TrackIndex).ToString())}));
 }
 Put(Out, TEXT("sync_markers"), MoveTemp(Markers));
 FValue Metadata, UserData;
 TArray<UObject*> MetaObjects(Sequence.GetMetaData());
 if (!CaptureObjectRows(Sequence, MetaObjects, Metadata, Diagnostics)) return false;
 TArray<UObject*> UserObjects;
 if (const TArray<UAssetUserData*>* Existing = Sequence.GetAssetUserDataArray()) for (UAssetUserData* Data : *Existing) UserObjects.Add(Data);
 if (!CaptureObjectRows(Sequence, UserObjects, UserData, Diagnostics)) return false;
 Put(Out, TEXT("metadata"), MoveTemp(Metadata));
 Put(Out, TEXT("asset_user_data"), MoveTemp(UserData));
 return ValidateAssetDocumentRuntimeValue(Out).bValid;
}

FAssetDocumentPrimitiveDescriptor MakeDescriptor(bool bEdit)
{
 FAssetDocumentPrimitiveDescriptor D;
 D.PrimitiveId = bEdit ? EditPrimitive : SnapshotPrimitive; D.Version = 1; D.ProviderId = TEXT("animation");
 D.Owner = EAssetDocumentPrimitiveOwner::Animation; D.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
 D.Access = bEdit ? EAssetDocumentPrimitiveAccess::Mutating : EAssetDocumentPrimitiveAccess::ReadOnly;
 D.AllowedPhases = bEdit ? TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Apply}
  : TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Verify};
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleArgumentSchemas = {{TEXT("asset"), EAssetDocumentHandleKind::Asset,
  bEdit ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true}};
 D.bRequiresActiveTransaction = bEdit; D.RequiredConsumerIds = {TEXT("animation_profile")};
 D.InputSchemaId = D.PrimitiveId + TEXT(".input.v1"); D.OutputSchemaId = D.PrimitiveId + TEXT(".output.v1");
 if (bEdit) D.MutationFootprintSchemaId = D.PrimitiveId + TEXT(".footprint.v1");
 D.StableDiagnosticCodes = {InvalidIntent, Unresolved, SkeletonMismatch, TrackInUse, UnknownTrack, Overlap, OutOfRange,
  ControllerFailed, SnapshotFailed, TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("AnimationProvider.TransactionUnavailable"),
  TEXT("AnimationProvider.TargetClassMismatch"), TEXT("PrimitiveInvocation.InvalidValueShape"), TEXT("PropertyBatchWrite.DuplicateProperty"),
  TEXT("PropertyBatchWrite.UnknownProperty"), TEXT("PropertyToken.InvalidPayload"), TEXT("PropertyWrite.DecodeFailed"),
  TEXT("PropertyWrite.AccessFailed"), TEXT("PropertyRead.AccessFailed"), TEXT("PropertyRead.EncodeFailed")};
 auto& A = D.Admission;
 A.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
 A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
 A.ExactEngineVersion = TEXT("5.7.4"); A.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
 A.PublicModule = TEXT("Engine;AssetDocumentEditor");
 A.PublicHeader = TEXT("Animation/AnimSequence.h;Animation/AnimData/IAnimationDataController.h;Animation/AnimData/IAnimationDataModel.h;AssetDocumentEditorNativeState.h");
 A.ExportedAuthoritativeApi = bEdit
  ? TEXT("IAnimationDataController::SetFrameRate;IAnimationDataController::SetNumberOfFrames;IAnimationDataController::AddCurve;IAnimationDataController::RemoveCurve;IAnimationDataController::SetCurveKeys;IAnimationDataController::SetCurveFlags;IAnimationDataController::SetCurveColor;IAnimationDataController::SetCurveComment;UAnimationAsset::SetPreviewMesh;UAnimationAsset::AddMetaData;UAnimationAsset::AddAssetUserData;UAnimSequence::RefreshSyncMarkerDataFromAuthored;UAnimSequenceBase::RefreshCacheData;WriteAssetDocumentEditorSubobjectProperties")
  : TEXT("IAnimationDataModel::GetFloatCurves;IAnimationDataModel::GetBoneTrackNames;UAnimationAsset::GetSkeleton;ReadAssetDocumentEditorSubobjectProperties");
 A.AuthorityContextId = TEXT("animation.asset.authoring.v1");
 A.PrimaryOperationId = D.PrimitiveId;
 A.IdentityLifetimeContractId = bEdit ? TEXT("animation.provider-lease.mutable.v1") : TEXT("animation.provider-lease.read-only.v1");
 A.TransactionContractId = bEdit ? TEXT("animation.core-active-transaction.v1") : TEXT("animation.read-only.no-active-transaction.v1");
 A.RollbackContractId = bEdit ? TEXT("core.package-snapshot.v1") : TEXT("animation.read-only.no-rollback.v1");
 A.CompletionContractId = bEdit ? TEXT("animation.asset.authoring.post-edit-refresh.v1") : TEXT("animation.authoritative-readback.v1");
 A.SaveReloadEvidenceId = D.PrimitiveId + TEXT(".save-reload.v1");
 A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimSequence.Python.UpdateRollback")};
 A.VerificationTestIds = {TEXT("UEAssetDoc.AnimSequence.Python.CreatePopulated"),
  TEXT("UEAssetDoc.AnimationProvider.AssetAuthoring.RawEdits")};
 A.AuthorityFingerprint = TEXT("ue-5.7.4|animation|asset-authoring|snapshot-covered|") + D.PrimitiveId;
 return D;
}
}

FAssetDocumentPrimitiveDescriptor MakeAnimationAssetAuthoringSnapshotDescriptor() { using namespace AnimationAssetAuthoringLocal; return MakeDescriptor(false); }
FAssetDocumentPrimitiveDescriptor MakeAnimationAssetAuthoringEditDescriptor() { using namespace AnimationAssetAuthoringLocal; return MakeDescriptor(true); }

FAssetDocumentPrimitiveOutcome InvokeAnimationAssetAuthoring(const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction, const FString& ExpectedTargetPath)
{
 using namespace AnimationAssetAuthoringLocal;
 const bool bEdit = Invocation.PrimitiveId.Equals(EditPrimitive, ESearchCase::CaseSensitive);
 if (!ValidateAssetDocumentPrimitiveInvocation(Invocation, MakeDescriptor(bEdit)).bValid)
  return Failed(Invalid(InvalidIntent, TEXT("The invocation does not match the animation authoring descriptor.")));
 if (bEdit && (!Transaction || Transaction->GetState() != EAssetDocumentAtomicState::Mutating
  || Transaction->GetTransactionId() != Invocation.TransactionId))
  return Failed(Invalid(TEXT("AnimationProvider.TransactionUnavailable"), TEXT("Animation authoring requires the matching active transaction.")));
 FIntent Intent;
 if (bEdit)
 {
  auto R = ParseIntent(Invocation.ValueArguments, Intent);
  if (!R.bValid) return Failed(MoveTemp(R));
 }
 else if (Invocation.ValueArguments.Kind != EKind::Object || !Invocation.ValueArguments.ObjectKeys.IsEmpty())
  return Failed(Invalid(InvalidIntent, TEXT("The authoring snapshot takes an empty object.")));
 auto Acquired = AcquireAnimationInputLease(Handles, Invocation, TEXT("asset"), EAssetDocumentHandleKind::Asset,
  bEdit ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly);
 if (!Acquired.Lease) return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"), TEXT("Animation authoring requires a verified asset lease."));
 FResult Applied = Invalid(TEXT("AnimationProvider.TargetClassMismatch"), TEXT("The verified asset is not this request's exact AnimSequence."));
 FValue Snapshot;
 const auto Identity = WithAssetDocumentEditorAssetIdentity(*Acquired.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  UObject& Target = View.GetObject();
  // Validate's trial target (a transient copy, or a new transient sequence) stands in for the request's target.
  if (IsValid(&Target) && IsAdmittedClassPath(Target.GetClass()->GetPathName()) && View.GetCanonicalObjectPath() == Target.GetPathName()
   && (ExpectedTargetPath.IsEmpty() || Target.GetPathName() == ExpectedTargetPath || FAssetDocumentEditorTrialScope::IsTrialTarget(Target)))
  {
   UAnimSequence& Sequence = static_cast<UAnimSequence&>(Target);
   if (bEdit) Applied = Apply(Sequence, Intent);
   else
   {
    TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
    Applied = Capture(Sequence, Snapshot, Diagnostics) ? Valid() : FResult();
    Applied.Diagnostics = MoveTemp(Diagnostics);
   }
  }
  return Valid();
 });
 if (!Identity.bValid) return Failed(FResult(Identity));
 if (!Applied.bValid) return Failed(MoveTemp(Applied));
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 if (bEdit)
 {
  Out.Value.Kind = EKind::Object;
  auto& Change = Out.ChangedSet.AddDefaulted_GetRef();
  Change.AuthorityId = EditPrimitive; Change.StableObjectId = ExpectedTargetPath;
  Change.Path = TEXT("/Authoring"); Change.ChangeKind = TEXT("Replace");
 }
 else Out.Value = MoveTemp(Snapshot);
 return Out;
}
