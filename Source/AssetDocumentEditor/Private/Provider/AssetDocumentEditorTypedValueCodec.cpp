// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "AssetDocumentEditorFloatPrecision.h"
#include "AssetDocumentEditorMarkerText.h"
#include "AssetDocumentEditorPropertyValueCodec.h"
#include "Internationalization/Text.h"
#include "Internationalization/TextPackageNamespaceUtil.h"
#include "JsonObjectConverter.h"
#include "Misc/OutputDeviceNull.h"
#include "PropertyEditorUtils.h"
#include "Runtime/AssetDocumentTextDisplays.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Class.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/PropertyOptional.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

// Document forms (codec ids in parentheses):
//   bool/integer/number/string/name/enum, soft object/class paths  - unchanged
//   FText (text.v1)                  "source string"; INVTEXT(...)/LOCTABLE(...)/NSLOCTEXT(...)
//                                    text forms for invariant, string-table or macro-looking texts
//   hard object ref (object_path.v1) "/Game/Dir/Asset.Asset" (subobject: "...Asset:Sub") | null
//   class ref (class_path.v1)        "/Script/Module.Class" | "/Game/Dir/BP.BP_C" | null
//   instanced subobject (instanced_object.v1)
//                                    {"Class": "<class path>", "Properties": {<non-default editable fields>}} | null
//   edit-inline reference (edit_inline_object.v1): a plain object reference with EditInline metadata
//                                    a subobject: the instanced form; another asset: its object path | null
//   FInstancedStruct (instanced_struct.v1)
//                                    {"Struct": "<struct path>", "Properties": {<non-default members>}} | null
//   struct with native text export (struct_text.v1)
//                                    the struct's ExportText string (FGuid, FSoftObjectPath, ...)
//   TOptional (optional.v1)          null when unset, else the value's own form
//   other structs (struct.v1)        object with every member; a member of a type with no document form
//                                    (delegate, field path, a map keyed by float, ...) is the marker
//                                    {"Unsupported": "<reason>", "Text": "<export text>"}, which a write keeps
//   arrays/sets (array.v1/set.v1), maps keyed by string/name/enum/object/soft reference/text-struct/integer/other
//   struct (string_key_map.v1): a gameplay tag key is its tag name, another struct key its export text
namespace
{
constexpr int32 AssetDocumentEditorTypedValueMaximumDepth = 64;

using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;

FAssetDocumentEditorTypedValueCodecResult
MakeAssetDocumentEditorTypedValueFailure(const TCHAR* DiagnosticCode)
{
	FAssetDocumentEditorTypedValueCodecResult Result;
	Result.DiagnosticCode = DiagnosticCode;
	return Result;
}
FAssetDocumentEditorTypedValueCodecResult
MakeAssetDocumentEditorTypedValueSuccess(FString CodecId)
{
	FAssetDocumentEditorTypedValueCodecResult Result;
	Result.bSucceeded = true;
	Result.CodecId = MoveTemp(CodecId);
	return Result;
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

bool IsAssetDocumentEditorSoftReference(const FProperty& Property)
{
	return CastField<FSoftObjectProperty>(&Property) != nullptr;
}

bool HasAssetDocumentEditorInstancedFlags(const FProperty& Property)
{
	constexpr uint64 InstancedFlags = CPF_InstancedReference | CPF_PersistentInstance;
	if (Property.HasAnyPropertyFlags(InstancedFlags))
	{
		return true;
	}
	// Container inners: the Instanced specifier is on the owning container.
	const FProperty* Owner = Property.GetOwner<FProperty>();
	return Owner != nullptr
		&& (CastField<FArrayProperty>(Owner) || CastField<FSetProperty>(Owner) || CastField<FMapProperty>(Owner))
		&& Owner->HasAnyPropertyFlags(CPF_PersistentInstance);
}

const FObjectProperty* AsAssetDocumentEditorInstancedObject(const FProperty& Property)
{
	const FObjectProperty* Object = CastField<FObjectProperty>(&Property);
	if (Object == nullptr || CastField<FClassProperty>(&Property) != nullptr || Object->PropertyClass == nullptr)
	{
		return nullptr;
	}
	return HasAssetDocumentEditorInstancedFlags(Property)
			|| Object->PropertyClass->HasAnyClassFlags(CLASS_DefaultToInstanced)
		? Object
		: nullptr;
}

// Hard (non-soft, non-instanced) object/class/weak/lazy references.
const FObjectPropertyBase* AsAssetDocumentEditorHardReference(const FProperty& Property)
{
	const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property);
	if (Object == nullptr || IsAssetDocumentEditorSoftReference(Property)
		|| AsAssetDocumentEditorInstancedObject(Property) != nullptr || Object->PropertyClass == nullptr)
	{
		return nullptr;
	}
	return Object;
}

bool IsAssetDocumentEditorEditInlineReference(const FProperty& Property);
bool IsAssetDocumentEditorObjectInsideAsset(const UObject* Object, const UObject& Asset);

// A plain (non-instanced) object reference with EditInline metadata, e.g. UAnimCurveCompressionSettings::Codec.
// The Details panel shows it as an edit-inline node (SPropertyEditorEditInline): no asset picker, a class
// picker that creates a new object under the edited object (NewObject(Owner, Class), EditInlineNew classes),
// and the referenced object's own fields inline, which edit that object wherever it lives.
const FObjectProperty* AsAssetDocumentEditorEditInlineObject(const FProperty& Property)
{
	const FObjectProperty* Object = CastField<FObjectProperty>(&Property);
	if (Object == nullptr || CastField<FClassProperty>(&Property) != nullptr || Object->PropertyClass == nullptr
		|| AsAssetDocumentEditorInstancedObject(Property) != nullptr || IsAssetDocumentEditorSoftReference(Property))
	{
		return nullptr;
	}
	return IsAssetDocumentEditorEditInlineReference(Property) ? Object : nullptr;
}

// The document form of an edit-inline reference: a subobject (an object whose outer is another object, as the
// class picker and asset factories create them) is written like an instanced subobject; an asset (outer is its
// package) by its object path, since editing its fields inline would edit that asset.
bool IsAssetDocumentEditorEditInlineSubobject(const UObject* Object)
{
	return Object != nullptr && Object->GetOuter() != nullptr && !Object->GetOuter()->IsA<UPackage>();
}

bool IsAssetDocumentEditorInstancedStruct(const FProperty& Property)
{
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	return Struct != nullptr && Struct->Struct == FInstancedStruct::StaticStruct();
}

// An FInstancedPropertyBag is coded as the struct it is (its Value: an instanced struct of the bag's layout), whatever
// the engine's JSON converter does with property bags.
bool IsAssetDocumentEditorPropertyBag(const FProperty& Property)
{
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	return Struct != nullptr && Struct->Struct == FInstancedPropertyBag::StaticStruct();
}

// The JSON converter exports structs with a native ExportTextItem as that text.
bool IsAssetDocumentEditorTextFormStruct(const FProperty& Property)
{
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	if (Struct == nullptr || IsAssetDocumentEditorInstancedStruct(Property))
	{
		return false;
	}
	const UScriptStruct::ICppStructOps* Ops = Struct->Struct->GetCppStructOps();
	return Ops != nullptr && Ops->HasExportTextItem() && Ops->HasImportTextItem();
}

const FProperty& GetAssetDocumentEditorMetadataProperty(const FProperty& Property)
{
	const FProperty* Owner = Property.GetOwnerProperty();
	return Owner != nullptr ? *Owner : Property;
}

// ---------------------------------------------------------------------------
// Admission
// ---------------------------------------------------------------------------

bool TryGetAssetDocumentEditorNumericCodecId(
	const FNumericProperty* Numeric,
	FString& OutCodecId)
{
	if (Numeric == nullptr || Numeric->IsEnum())
	{
		return false;
	}
	if (Numeric->IsFloatingPoint())
	{
		OutCodecId = TEXT("number.v1");
		return true;
	}
	if (Numeric->IsInteger())
	{
		OutCodecId = TEXT("integer.v1");
		return true;
	}
	return false;
}

bool TryGetAssetDocumentEditorFundamentalCodecId(
	FProperty& Property,
	FString& OutCodecId)
{
	if (CastField<FBoolProperty>(&Property) != nullptr)
	{
		OutCodecId = TEXT("bool.v1");
		return true;
	}
	return TryGetAssetDocumentEditorNumericCodecId(
		CastField<FNumericProperty>(&Property), OutCodecId);
}

bool TryGetAssetDocumentEditorNumericEnumCodecId(
	FProperty& Property,
	FString& OutCodecId)
{
	const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
	if (Numeric == nullptr)
	{
		return false;
	}
	if (!Numeric->IsEnum())
	{
		return false;
	}
	OutCodecId = TEXT("enum.v1");
	return true;
}

bool TryGetAssetDocumentEditorTextCodecId(
	FProperty& Property,
	FString& OutCodecId)
{
	if (CastField<FStrProperty>(&Property) != nullptr)
	{
		OutCodecId = TEXT("string.v1");
		return true;
	}
	if (CastField<FNameProperty>(&Property) != nullptr)
	{
		OutCodecId = TEXT("name.v1");
		return true;
	}
	if (CastField<FEnumProperty>(&Property) != nullptr)
	{
		OutCodecId = TEXT("enum.v1");
		return true;
	}
	if (CastField<FTextProperty>(&Property) != nullptr)
	{
		OutCodecId = TEXT("text.v1");
		return true;
	}
	return TryGetAssetDocumentEditorNumericEnumCodecId(
		Property, OutCodecId);
}
bool TryGetAssetDocumentEditorSoftPathCodecId(
	FProperty& Property,
	FString& OutCodecId)
{
	if (CastField<FSoftClassProperty>(&Property) != nullptr)
	{
		OutCodecId = TEXT("soft_class_path.v1");
		return true;
	}
	if (CastField<FSoftObjectProperty>(&Property) != nullptr)
	{
		OutCodecId = TEXT("soft_object_path.v1");
		return true;
	}
	return false;
}

bool TryGetAssetDocumentEditorReferenceCodecId(
	FProperty& Property,
	FString& OutCodecId)
{
	if (AsAssetDocumentEditorInstancedObject(Property) != nullptr)
	{
		OutCodecId = TEXT("instanced_object.v1");
		return true;
	}
	if (AsAssetDocumentEditorEditInlineObject(Property) != nullptr)
	{
		OutCodecId = TEXT("edit_inline_object.v1");
		return true;
	}
	if (const FObjectPropertyBase* Hard = AsAssetDocumentEditorHardReference(Property))
	{
		OutCodecId = CastField<FClassProperty>(Hard) != nullptr
			? TEXT("class_path.v1")
			: TEXT("object_path.v1");
		return true;
	}
	const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(&Property);
	if (Interface != nullptr && Interface->InterfaceClass != nullptr)
	{
		OutCodecId = TEXT("object_path.v1");
		return true;
	}
	if (IsAssetDocumentEditorInstancedStruct(Property))
	{
		OutCodecId = TEXT("instanced_struct.v1");
		return true;
	}
	if (IsAssetDocumentEditorTextFormStruct(Property))
	{
		OutCodecId = TEXT("struct_text.v1");
		return true;
	}
	return false;
}

// Plain scalars whose document form is a bool, number or string.
bool TryGetAssetDocumentEditorScalarCodecId(
	FProperty& Property,
	FString& OutCodecId)
{
	return TryGetAssetDocumentEditorFundamentalCodecId(Property, OutCodecId)
		|| TryGetAssetDocumentEditorTextCodecId(Property, OutCodecId)
		|| TryGetAssetDocumentEditorSoftPathCodecId(Property, OutCodecId);
}

// Every kind with a dedicated (non-member-wise) document form.
bool TryGetAssetDocumentEditorLeafCodecId(
	FProperty& Property,
	FString& OutCodecId)
{
	return TryGetAssetDocumentEditorReferenceCodecId(Property, OutCodecId)
		|| TryGetAssetDocumentEditorScalarCodecId(Property, OutCodecId);
}

bool IsAssetDocumentEditorPropertyAdmitted(
	FProperty& Property,
	bool bIgnoreStaticArray,
	int32 Depth);

// The JSON converter skips transient and deprecated struct members on export,
// so they are not part of a struct value's codec shape in either direction.
bool IsAssetDocumentEditorStructCodecField(const FProperty& Field)
{
	return !Field.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated);
}

// A struct is admitted whatever its members are: a member the codec has no form for reads as a marker (see
// ExportAssetDocumentEditorMarker) and keeps its value on write. Only the nesting depth bounds it.
bool IsAssetDocumentEditorStructAdmitted(
	const FStructProperty& StructProperty,
	int32 Depth)
{
	return Depth <= AssetDocumentEditorTypedValueMaximumDepth;
}

// An integer map key (not an enum): TMap<int32, ...>, e.g. FPerQualityLevelInt::PerQuality (keyed by quality level).
const FNumericProperty* AsAssetDocumentEditorIntegerMapKey(const FProperty& Key)
{
	const FNumericProperty* Numeric = CastField<FNumericProperty>(&Key);
	return Numeric != nullptr && Numeric->IsInteger() && !Numeric->IsEnum() ? Numeric : nullptr;
}

// An integer map key travels as its decimal text ("0", "-3"), which is what UE exports for it. Only that canonical
// spelling is accepted (no sign on positives, no leading zeros, inside the key type's range), so one key has one name.
bool TryParseAssetDocumentEditorIntegerMapKey(const FNumericProperty& Key, const FString& Text, int64& OutValue)
{
	const int32 Start = Text.StartsWith(TEXT("-")) ? 1 : 0;
	if (Text.Len() <= Start || Text.Len() > 20)
	{
		return false;
	}
	for (int32 Index = Start; Index < Text.Len(); ++Index)
	{
		if (!FChar::IsDigit(Text[Index]))
		{
			return false;
		}
	}
	OutValue = FCString::Strtoi64(*Text, nullptr, 10);
	if (LexToString(OutValue) != Text)
	{
		return false;
	}
	alignas(8) uint8 Buffer[8] = {};
	check(Key.GetElementSize() <= static_cast<int32>(sizeof(Buffer)));
	Key.SetIntPropertyValue(Buffer, OutValue);
	const bool bUnsigned = CastField<FByteProperty>(&Key) != nullptr || CastField<FUInt16Property>(&Key) != nullptr
		|| CastField<FUInt32Property>(&Key) != nullptr || CastField<FUInt64Property>(&Key) != nullptr;
	return bUnsigned
		? OutValue >= 0 && Key.GetUnsignedIntPropertyValue(Buffer) == static_cast<uint64>(OutValue)
		: Key.GetSignedIntPropertyValue(Buffer) == OutValue;
}

bool IsAssetDocumentEditorGameplayTag(const FProperty& Property)
{
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	return Struct != nullptr && Struct->Struct != nullptr
		&& Struct->Struct->GetPathName() == TEXT("/Script/GameplayTags.GameplayTag");
}

// A struct map key with no native text export: a gameplay tag is written as its tag name ("Ability.Cooldown"), which is
// how the tag picker shows it; any other struct as its export text ("(Type=Bone,Name=\"root\")").
bool IsAssetDocumentEditorStructMapKey(const FProperty& Key)
{
	return CastField<FStructProperty>(&Key) != nullptr && Key.ArrayDim == 1
		&& !IsAssetDocumentEditorInstancedStruct(Key) && !IsAssetDocumentEditorPropertyBag(Key)
		&& !IsAssetDocumentEditorTextFormStruct(Key);
}

bool IsAssetDocumentEditorMapKeyProperty(const FProperty& Property)
{
	const FMapProperty* Map = Property.GetOwner<FMapProperty>();
	return Map != nullptr && Map->KeyProp == &Property;
}

FString ExportAssetDocumentEditorStructMapKey(const FProperty& Key, const void* Value)
{
	if (IsAssetDocumentEditorGameplayTag(Key))
	{
		const FNameProperty* TagName = FindFProperty<FNameProperty>(CastFieldChecked<FStructProperty>(&Key)->Struct, TEXT("TagName"));
		if (TagName != nullptr)
		{
			return TagName->GetPropertyValue_InContainer(Value).ToString();
		}
	}
	FString Text;
	Key.ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);
	return Text;
}

// Reads a struct map key from its document form; the key must be written exactly as it reads back (one name per key).
// A gameplay tag must be registered (or a redirected name), as the tag picker requires.
bool ParseAssetDocumentEditorStructMapKey(const FProperty& Key, const FString& Text, void* Out)
{
	FString Buffer = Text;
	if (IsAssetDocumentEditorGameplayTag(Key))
	{
		Buffer = FString::Printf(TEXT("\"%s\""), *Text.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\"")));
	}
	FOutputDeviceNull Errors;
	const TCHAR* End = Key.ImportText_Direct(*Buffer, Out, nullptr, PPF_None, &Errors);
	if (End == nullptr)
	{
		return false;
	}
	while (FChar::IsWhitespace(*End))
	{
		++End;
	}
	return *End == TCHAR('\0');
}

// The entry of the map at Base whose key reads as Text (INDEX_NONE: none). A key already in the map is kept as it is
// stored, whether or not it parses today (an unregistered or redirected gameplay tag).
int32 FindAssetDocumentEditorStructMapKeyIndex(const FMapProperty& Map, const void* Base, const FString& Text)
{
	if (Base == nullptr)
	{
		return INDEX_NONE;
	}
	FScriptMapHelper Helper(&Map, Base);
	for (FScriptMapHelper::FIterator It(Helper); It; ++It)
	{
		if (ExportAssetDocumentEditorStructMapKey(*Map.KeyProp, Helper.GetKeyPtr(It)).Equals(Text, ESearchCase::CaseSensitive))
		{
			return It.GetInternalIndex();
		}
	}
	return INDEX_NONE;
}

// Writes the key named Text into Out: the stored key of that name in the map at Current, else the parsed key.
bool ImportAssetDocumentEditorStructMapKey(const FMapProperty& Map, const void* Current, const FString& Text, void* Out)
{
	const int32 CurrentIndex = FindAssetDocumentEditorStructMapKeyIndex(Map, Current, Text);
	if (CurrentIndex != INDEX_NONE)
	{
		FScriptMapHelper CurrentHelper(&Map, Current);
		Map.KeyProp->CopyCompleteValue(Out, CurrentHelper.GetKeyPtr(CurrentIndex));
		return true;
	}
	return ParseAssetDocumentEditorStructMapKey(*Map.KeyProp, Text, Out)
		&& ExportAssetDocumentEditorStructMapKey(*Map.KeyProp, Out).Equals(Text, ESearchCase::CaseSensitive);
}

// Map keys travel as JSON object member names.
bool IsAssetDocumentEditorMapKeyAdmitted(FProperty& Key)
{
	return CastField<FStrProperty>(&Key) != nullptr
		|| AsAssetDocumentEditorIntegerMapKey(Key) != nullptr
		|| CastField<FNameProperty>(&Key) != nullptr
		|| CastField<FEnumProperty>(&Key) != nullptr
		|| (CastField<FNumericProperty>(&Key) != nullptr && CastField<FNumericProperty>(&Key)->IsEnum())
		|| (AsAssetDocumentEditorHardReference(Key) != nullptr && Key.ArrayDim == 1)
		|| (IsAssetDocumentEditorSoftReference(Key) && Key.ArrayDim == 1)
		|| IsAssetDocumentEditorTextFormStruct(Key)
		|| IsAssetDocumentEditorStructMapKey(Key);
}

bool IsAssetDocumentEditorMapAdmitted(
	const FMapProperty& MapProperty,
	int32 Depth)
{
	return IsAssetDocumentEditorMapKeyAdmitted(*MapProperty.KeyProp)
		&& IsAssetDocumentEditorPropertyAdmitted(
			*MapProperty.ValueProp, false, Depth + 1);
}

bool IsAssetDocumentEditorContainerAdmitted(
	FProperty& Property,
	int32 Depth)
{
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		return IsAssetDocumentEditorPropertyAdmitted(
			*Array->Inner, false, Depth + 1);
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		return IsAssetDocumentEditorPropertyAdmitted(
			*Set->ElementProp, false, Depth + 1);
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		return IsAssetDocumentEditorMapAdmitted(*Map, Depth);
	}
	if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(&Property))
	{
		return IsAssetDocumentEditorPropertyAdmitted(
			*Optional->GetValueProperty(), false, Depth + 1);
	}
	return false;
}

bool HasAssetDocumentEditorStaticArrayShape(
	const FProperty& Property,
	bool bIgnoreStaticArray)
{
	return !bIgnoreStaticArray && Property.ArrayDim > 1;
}

bool IsAssetDocumentEditorPropertyAdmitted(
	FProperty& Property,
	bool bIgnoreStaticArray,
	int32 Depth)
{
	if (Depth > AssetDocumentEditorTypedValueMaximumDepth)
	{
		return false;
	}
	if (HasAssetDocumentEditorStaticArrayShape(Property, bIgnoreStaticArray))
	{
		return IsAssetDocumentEditorPropertyAdmitted(Property, true, Depth + 1);
	}
	FString LeafCodecId;
	if (TryGetAssetDocumentEditorLeafCodecId(Property, LeafCodecId))
	{
		return true;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		return IsAssetDocumentEditorStructAdmitted(*Struct, Depth);
	}
	return IsAssetDocumentEditorContainerAdmitted(Property, Depth);
}

FString GetAssetDocumentEditorContainerCodecId(FProperty& Property)
{
	if (CastField<FArrayProperty>(&Property) != nullptr)
	{
		return TEXT("array.v1");
	}
	if (CastField<FSetProperty>(&Property) != nullptr)
	{
		return TEXT("set.v1");
	}
	if (CastField<FMapProperty>(&Property) != nullptr)
	{
		return TEXT("string_key_map.v1");
	}
	if (CastField<FOptionalProperty>(&Property) != nullptr)
	{
		return TEXT("optional.v1");
	}
	return TEXT("struct.v1");
}

FString GetAssetDocumentEditorCodecId(FProperty& Property)
{
	if (Property.ArrayDim > 1)
	{
		return TEXT("fixed_array.v1");
	}
	FString LeafCodecId;
	if (TryGetAssetDocumentEditorLeafCodecId(Property, LeafCodecId))
	{
		return LeafCodecId;
	}
	return GetAssetDocumentEditorContainerCodecId(Property);
}

// Editable fields of an instanced subobject class: the same rule as the top-level
// writable admission (Details-visible, serialized, not EditConst, typed codec). A
// DuplicateTransient field is saved with the subobject and edited in Details, as at
// the top level.
bool IsAssetDocumentEditorSubobjectField(FProperty& Field, int32 Depth)
{
	constexpr uint64 RejectedFlags =
		CPF_Transient | CPF_Deprecated | CPF_SkipSerialization;
	return Field.HasAnyPropertyFlags(CPF_Edit)
		&& !Field.HasAnyPropertyFlags(RejectedFlags)
		&& PropertyAccessUtil::CanGetPropertyValue(&Field) == EPropertyAccessResultFlags::Success
		&& PropertyAccessUtil::CanSetPropertyValue(&Field, PropertyAccessUtil::EditorReadOnlyFlags, false)
			== EPropertyAccessResultFlags::Success
		&& IsAssetDocumentEditorPropertyAdmitted(Field, false, Depth + 1);
}

FProperty* FindAssetDocumentEditorSubobjectField(UClass& Class, FStringView Name, int32 Depth)
{
	for (TFieldIterator<FProperty> It(&Class); It; ++It)
	{
		if (It->GetAuthoredName() == Name)
		{
			return IsAssetDocumentEditorSubobjectField(**It, Depth) ? *It : nullptr;
		}
	}
	return nullptr;
}

FProperty* FindAssetDocumentEditorInstancedStructField(const UScriptStruct& Struct, FStringView Name, int32 Depth)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (It->GetAuthoredName() == Name)
		{
			return IsAssetDocumentEditorStructCodecField(**It) ? *It : nullptr;
		}
	}
	return nullptr;
}

FString DescribeAssetDocumentEditorSubobjectFields(UClass& Class, int32 Depth)
{
	TArray<FString> Names;
	for (TFieldIterator<FProperty> It(&Class); It; ++It)
	{
		if (IsAssetDocumentEditorSubobjectField(**It, Depth))
		{
			Names.Add(It->GetAuthoredName());
		}
	}
	Names.Sort();
	return Names.IsEmpty() ? FString(TEXT("(none)")) : FString::Join(Names, TEXT(", "));
}

// ---------------------------------------------------------------------------
// Value check: shape (and, when decoding, reference resolution) with an
// AI-readable failure location.
// ---------------------------------------------------------------------------

enum class EAssetDocumentEditorCheckMode : uint8
{
	Decode,
	Encode,
};

struct FAssetDocumentEditorValueCheck
{
	EAssetDocumentEditorCheckMode Mode = EAssetDocumentEditorCheckMode::Decode;
	// The value edits a current value: omitted struct members keep theirs (a value
	// without a current one, e.g. a new array element, starts from its default).
	bool bPartial = false;
	// The object holding the property being checked (picker filter functions).
	UObject* Owner = nullptr;
	FString Path;
	FString Message;
};

// The current value of the map entry whose document key is Key (nullptr: none).
const void* FindAssetDocumentEditorMapBaseValue(const FMapProperty& Map, const void* Base, const FString& Key);

bool FailAssetDocumentEditorCheck(FAssetDocumentEditorValueCheck& Check, const FString& Path, FString Message)
{
	if (Check.Message.IsEmpty())
	{
		Check.Path = Path;
		Check.Message = MoveTemp(Message);
	}
	return false;
}

FString EscapeAssetDocumentEditorPointer(const FString& Segment)
{
	return Segment.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
}

FString AppendAssetDocumentEditorPath(const FString& Path, const FString& Segment)
{
	return Path + TEXT("/") + EscapeAssetDocumentEditorPointer(Segment);
}

FString AppendAssetDocumentEditorPath(const FString& Path, int32 Index)
{
	return FString::Printf(TEXT("%s/%d"), *Path, Index);
}

const TCHAR* DescribeAssetDocumentEditorKind(EKind Kind)
{
	switch (Kind)
	{
	case EKind::Null: return TEXT("null");
	case EKind::Boolean: return TEXT("a boolean");
	case EKind::Integer: return TEXT("an integral number");
	case EKind::Number: return TEXT("a number");
	case EKind::String: return TEXT("a string");
	case EKind::Bytes: return TEXT("bytes");
	case EKind::Array: return TEXT("an array");
	case EKind::Object: return TEXT("an object");
	}
	return TEXT("a value");
}

const FValue* FindAssetDocumentEditorTypedField(
	const FValue& Value,
	FStringView Name)
{
	for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
	{
		if (Value.ObjectKeys[Index] == Name)
		{
			return &Value.Elements[Index];
		}
	}
	return nullptr;
}

// Exact canonical object path (no export-text class prefix, no quotes).
UObject* ResolveAssetDocumentEditorObjectPath(const FString& Path, bool bLoad)
{
	const FSoftObjectPath SoftPath(Path);
	if (!SoftPath.IsValid() || SoftPath.ToString() != Path)
	{
		return nullptr;
	}
	UObject* Object = SoftPath.ResolveObject();
	if (Object == nullptr && bLoad && FPackageName::DoesPackageExist(SoftPath.GetLongPackageName()))
	{
		Object = SoftPath.TryLoad();
	}
	// A package-only path loads the package's main asset; packages themselves
	// are never picker values. Only the object's own exact path is accepted.
	return IsValid(Object) && !Object->IsA<UPackage>() && Object->GetPathName() == Path ? Object : nullptr;
}

FString DescribeAssetDocumentEditorClasses(TConstArrayView<const UClass*> Classes)
{
	TArray<FString> Paths;
	for (const UClass* Class : Classes)
	{
		if (Class != nullptr)
		{
			Paths.Add(Class->GetPathName());
		}
	}
	return FString::Join(Paths, TEXT(", "));
}

// The class filters the Details panel picker applies (AllowedClasses,
// DisallowedClasses, ExactClass metadata of the owning UPROPERTY).
bool CheckAssetDocumentEditorClassFilters(
	const FProperty& Property,
	const UClass& Candidate,
	const UClass& PropertyClass,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	const FProperty& Metadata = GetAssetDocumentEditorMetadataProperty(Property);
	const bool bExactClass = Metadata.HasMetaData(TEXT("ExactClass"));
	TArray<const UClass*> Allowed;
	TArray<const UClass*> Disallowed;
	// GetAllowedClasses/GetDisallowedClasses functions run on the edited object,
	// as the picker does for its outer objects.
	TArray<UObject*> Owners;
	if (Check.Owner != nullptr)
	{
		Owners.Add(Check.Owner);
	}
	PropertyEditorUtils::GetAllowedAndDisallowedClasses(Owners, Metadata, Allowed, Disallowed, bExactClass, &PropertyClass);
	if (!Allowed.IsEmpty() && !Allowed.ContainsByPredicate([&](const UClass* Class)
		{
			return Class != nullptr && (bExactClass ? &Candidate == Class : Candidate.IsChildOf(Class));
		}))
	{
		return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
			TEXT("%s is not allowed here; the property only accepts %s."),
			*Candidate.GetPathName(), *DescribeAssetDocumentEditorClasses(Allowed)));
	}
	for (const UClass* Class : Disallowed)
	{
		if (Class != nullptr && Candidate.IsChildOf(Class))
		{
			return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
				TEXT("%s is disallowed for this property (DisallowedClasses: %s)."),
				*Candidate.GetPathName(), *DescribeAssetDocumentEditorClasses(Disallowed)));
		}
	}
	return true;
}

UObject* GetAssetDocumentEditorReferenceValue(const FProperty& Property, const void* Value)
{
	if (Value == nullptr)
	{
		return nullptr;
	}
	if (CastField<FInterfaceProperty>(&Property) != nullptr)
	{
		return static_cast<const FScriptInterface*>(Value)->GetObject();
	}
	const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property);
	return Object != nullptr ? Object->GetObjectPropertyValue(Value) : nullptr;
}

bool HasAssetDocumentEditorNoClear(const FProperty& Property)
{
	// The pickers read the NoClear/NonNullable property flags (UPROPERTY(NoClear)); the
	// metadata spelling is honoured too.
	const FProperty& Metadata = GetAssetDocumentEditorMetadataProperty(Property);
	return Property.HasAnyPropertyFlags(CPF_NoClear | CPF_NonNullable)
		|| Metadata.HasAnyPropertyFlags(CPF_NoClear | CPF_NonNullable)
		|| Metadata.HasMetaData(TEXT("NoClear"));
}

bool CheckAssetDocumentEditorReference(
	FProperty& Property,
	const FValue& Value,
	const void* Base,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	UObject* const Current = GetAssetDocumentEditorReferenceValue(Property, Base);
	// NoClear: the picker offers no "Clear"; a stored null stays re-appliable and
	// a value without a current one (restore, create) may still be null.
	if (Value.Kind == EKind::Null)
	{
		if (Check.Mode == EAssetDocumentEditorCheckMode::Decode && Current != nullptr
			&& HasAssetDocumentEditorNoClear(Property))
		{
			return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
				TEXT("the property is NoClear: it cannot be cleared, only set to another object (it holds '%s')."),
				*Current->GetPathName()));
		}
		return true;
	}
	if (Value.Kind != EKind::String || Value.StringValue.IsEmpty())
	{
		return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
			TEXT("expected an object path string (e.g. \"/Game/Folder/Asset.Asset\") or null, got %s."),
			DescribeAssetDocumentEditorKind(Value.Kind)));
	}
	if (Check.Mode == EAssetDocumentEditorCheckMode::Encode)
	{
		return true;
	}
	UObject* Object = ResolveAssetDocumentEditorObjectPath(Value.StringValue, true);
	if (Object == nullptr)
	{
		return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
			TEXT("'%s' does not resolve to an existing object; use the exact object path \"/Game/Folder/Asset.Asset\"."),
			*Value.StringValue));
	}
	// The picker constraints apply to a new choice; the current value re-applies.
	if (Object == Current)
	{
		return true;
	}
	if (const FClassProperty* ClassProperty = CastField<FClassProperty>(&Property))
	{
		UClass* Class = Cast<UClass>(Object);
		UClass* MetaClass = ClassProperty->MetaClass != nullptr ? ClassProperty->MetaClass.Get() : UObject::StaticClass();
		if (Class == nullptr || !Class->IsChildOf(MetaClass))
		{
			return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
				TEXT("'%s' is not a subclass of %s."), *Value.StringValue, *MetaClass->GetPathName()));
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract)
			&& !GetAssetDocumentEditorMetadataProperty(Property).HasMetaData(TEXT("AllowAbstract")))
		{
			return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
				TEXT("%s is abstract; the class picker offers abstract classes only for AllowAbstract properties."),
				*Class->GetPathName()));
		}
		const FString MustImplement = GetAssetDocumentEditorMetadataProperty(Property).GetMetaData(TEXT("MustImplement"));
		if (!MustImplement.IsEmpty())
		{
			const UClass* Interface = FindFirstObject<UClass>(*MustImplement, EFindFirstObjectOptions::None);
			if (Interface == nullptr)
			{
				Interface = LoadObject<UClass>(nullptr, *MustImplement);
			}
			if (Interface != nullptr && !Class->ImplementsInterface(Interface))
			{
				return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
					TEXT("'%s' must implement %s."), *Value.StringValue, *Interface->GetPathName()));
			}
		}
		return CheckAssetDocumentEditorClassFilters(Property, *Class, *MetaClass, Path, Check);
	}
	if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(&Property))
	{
		if (!Object->GetClass()->ImplementsInterface(Interface->InterfaceClass))
		{
			return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
				TEXT("'%s' (%s) does not implement %s."), *Value.StringValue,
				*Object->GetClass()->GetName(), *Interface->InterfaceClass->GetPathName()));
		}
		return true;
	}
	const FObjectPropertyBase* Reference = CastFieldChecked<FObjectPropertyBase>(&Property);
	if (!Object->IsA(Reference->PropertyClass))
	{
		return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
			TEXT("'%s' is a %s, but the property requires a %s."), *Value.StringValue,
			*Object->GetClass()->GetName(), *Reference->PropertyClass->GetName()));
	}
	return CheckAssetDocumentEditorClassFilters(Property, *Object->GetClass(), *Reference->PropertyClass, Path, Check);
}

bool IsAssetDocumentEditorValueChecked(
	FProperty& Property,
	const FValue& Value,
	const void* Base,
	bool bIgnoreStaticArray,
	int32 Depth,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check);

bool CheckAssetDocumentEditorMarkedMember(
	FProperty& Field,
	const FValue& Value,
	const void* Current,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check);

bool CheckAssetDocumentEditorExactKeys(
	const FValue& Value,
	TConstArrayView<const TCHAR*> Required,
	TConstArrayView<const TCHAR*> Optional,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	for (const FString& Key : Value.ObjectKeys)
	{
		if (!Required.ContainsByPredicate([&](const TCHAR* Name) { return Key == Name; })
			&& !Optional.ContainsByPredicate([&](const TCHAR* Name) { return Key == Name; }))
		{
			return FailAssetDocumentEditorCheck(Check, AppendAssetDocumentEditorPath(Path, Key),
				FString::Printf(TEXT("unknown member '%s'."), *Key));
		}
	}
	for (const TCHAR* Name : Required)
	{
		if (FindAssetDocumentEditorTypedField(Value, Name) == nullptr)
		{
			return FailAssetDocumentEditorCheck(Check, Path,
				FString::Printf(TEXT("missing required member '%s'."), Name));
		}
	}
	return true;
}

bool CheckAssetDocumentEditorInstancedObject(
	const FObjectProperty& Property,
	const FValue& Value,
	const void* Base,
	int32 Depth,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	UObject* const Current = Base != nullptr ? Property.GetObjectPropertyValue(Base) : nullptr;
	if (Check.Mode == EAssetDocumentEditorCheckMode::Decode && Base != nullptr && Depth == 0
		&& IsAssetDocumentEditorInPlaceInstancedProperty(Property))
	{
		// VisibleAnywhere: only the subobject's own fields are editable.
		if (Current == nullptr)
		{
			return FailAssetDocumentEditorCheck(Check, Path,
				TEXT("the property is VisibleAnywhere and holds no subobject: there is nothing to edit (the owner creates it)."));
		}
		const FValue* ClassValue = Value.Kind == EKind::Object ? FindAssetDocumentEditorTypedField(Value, TEXT("Class")) : nullptr;
		if (ClassValue == nullptr || ClassValue->Kind != EKind::String
			|| ClassValue->StringValue != Current->GetClass()->GetPathName())
		{
			return FailAssetDocumentEditorCheck(Check, ClassValue != nullptr ? AppendAssetDocumentEditorPath(Path, TEXT("Class")) : Path,
				FString::Printf(TEXT("the property is VisibleAnywhere: its subobject cannot be cleared or replaced, only edited in place; ")
					TEXT("give {\"Class\": \"%s\", \"Properties\": {...}}."), *Current->GetClass()->GetPathName()));
		}
	}
	if (Value.Kind == EKind::Null)
	{
		if (Check.Mode == EAssetDocumentEditorCheckMode::Decode && Current != nullptr
			&& HasAssetDocumentEditorNoClear(Property))
		{
			return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
				TEXT("the property is NoClear: the %s subobject cannot be cleared; give a {\"Class\", \"Properties\"} value."),
				*Current->GetClass()->GetName()));
		}
		return true;
	}
	if (Value.Kind != EKind::Object)
	{
		return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
			TEXT("an instanced subobject is {\"Class\": \"<class path>\", \"Properties\": {...}} or null, got %s."),
			DescribeAssetDocumentEditorKind(Value.Kind)));
	}
	if (!CheckAssetDocumentEditorExactKeys(Value, {TEXT("Class")}, {TEXT("Properties")}, Path, Check))
	{
		return false;
	}
	const FValue& ClassValue = *FindAssetDocumentEditorTypedField(Value, TEXT("Class"));
	const FString ClassPath = AppendAssetDocumentEditorPath(Path, TEXT("Class"));
	if (ClassValue.Kind != EKind::String || ClassValue.StringValue.IsEmpty())
	{
		return FailAssetDocumentEditorCheck(Check, ClassPath, TEXT("Class must be a class path string."));
	}
	UClass* Class = Cast<UClass>(ResolveAssetDocumentEditorObjectPath(
		ClassValue.StringValue, Check.Mode == EAssetDocumentEditorCheckMode::Decode));
	if (Class == nullptr)
	{
		return FailAssetDocumentEditorCheck(Check, ClassPath, FString::Printf(
			TEXT("'%s' does not resolve to a class (e.g. \"/Script/Module.ClassName\")."), *ClassValue.StringValue));
	}
	if (!Class->IsChildOf(Property.PropertyClass))
	{
		return FailAssetDocumentEditorCheck(Check, ClassPath, FString::Printf(
			TEXT("%s is not a subclass of %s."), *Class->GetPathName(), *Property.PropertyClass->GetPathName()));
	}
	const bool bSameClass = Current != nullptr && Current->GetClass() == Class;
	if (Check.Mode == EAssetDocumentEditorCheckMode::Decode && !bSameClass)
	{
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			return FailAssetDocumentEditorCheck(Check, ClassPath, FString::Printf(
				TEXT("%s is abstract or deprecated; choose a concrete class."), *Class->GetPathName()));
		}
		if (Class->ClassWithin != nullptr && Class->ClassWithin != UObject::StaticClass())
		{
			return FailAssetDocumentEditorCheck(Check, ClassPath, FString::Printf(
				TEXT("%s requires an outer of class %s and cannot be authored as a generic instanced subobject."),
				*Class->GetPathName(), *Class->ClassWithin->GetPathName()));
		}
		if (!CheckAssetDocumentEditorClassFilters(Property, *Class, *Property.PropertyClass, ClassPath, Check))
		{
			return false;
		}
	}
	const FValue* Properties = FindAssetDocumentEditorTypedField(Value, TEXT("Properties"));
	if (Properties == nullptr)
	{
		return true;
	}
	const FString PropertiesPath = AppendAssetDocumentEditorPath(Path, TEXT("Properties"));
	if (Properties->Kind != EKind::Object)
	{
		return FailAssetDocumentEditorCheck(Check, PropertiesPath, TEXT("Properties must be an object."));
	}
	// Omitted fields keep the current subobject's values (same class) or start
	// from the class default object (a new subobject, as the class picker makes).
	UObject* const FieldOwner = bSameClass ? Current : Class->GetDefaultObject();
	TGuardValue<UObject*> OwnerGuard(Check.Owner, FieldOwner);
	for (int32 Index = 0; Index < Properties->ObjectKeys.Num(); ++Index)
	{
		const FString& Name = Properties->ObjectKeys[Index];
		const FString FieldPath = AppendAssetDocumentEditorPath(PropertiesPath, Name);
		FProperty* Field = FindAssetDocumentEditorSubobjectField(*Class, Name, Depth);
		if (Field == nullptr)
		{
			return FailAssetDocumentEditorCheck(Check, FieldPath, FString::Printf(
				TEXT("'%s' is not an editable property of %s. Editable: %s."), *Name, *Class->GetName(),
				*DescribeAssetDocumentEditorSubobjectFields(*Class, Depth)));
		}
		const void* FieldBase = FieldOwner != nullptr ? Field->ContainerPtrToValuePtr<void>(FieldOwner) : nullptr;
		if (!IsAssetDocumentEditorValueChecked(*Field, Properties->Elements[Index], FieldBase, false, Depth + 1, FieldPath, Check))
		{
			return false;
		}
	}
	return true;
}

TSharedPtr<FJsonValue> ExportAssetDocumentEditorJson(FProperty& Property, const void* Value);
bool AreAssetDocumentEditorJsonValuesEqual(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right);

// The instanced form of an edit-inline reference. The class picker lists EditInlineNew classes and creates the
// object under the edited object; the inline fields edit the referenced object itself. When that object is not
// inside the edited asset, an edit of its fields would change another asset: refused, pointing at that asset.
bool CheckAssetDocumentEditorEditInlineObject(
	const FObjectProperty& Property,
	const FValue& Value,
	const void* Base,
	int32 Depth,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	UObject* const Current = Base != nullptr ? Property.GetObjectPropertyValue(Base) : nullptr;
	const FValue* ClassValue = FindAssetDocumentEditorTypedField(Value, TEXT("Class"));
	if (Check.Mode != EAssetDocumentEditorCheckMode::Decode || ClassValue == nullptr || ClassValue->Kind != EKind::String)
	{
		return CheckAssetDocumentEditorInstancedObject(Property, Value, Base, Depth, Path, Check);
	}
	const bool bSameClass = Current != nullptr && ClassValue->StringValue == Current->GetClass()->GetPathName();
	if (!bSameClass)
	{
		const UClass* Class = Cast<UClass>(ResolveAssetDocumentEditorObjectPath(ClassValue->StringValue, true));
		if (Class != nullptr && Class->IsChildOf(Property.PropertyClass) && !Class->HasAnyClassFlags(CLASS_EditInlineNew))
		{
			return FailAssetDocumentEditorCheck(Check, AppendAssetDocumentEditorPath(Path, TEXT("Class")), FString::Printf(
				TEXT("%s is not EditInlineNew; the property's class picker lists only EditInlineNew subclasses of %s."),
				*Class->GetPathName(), *Property.PropertyClass->GetPathName()));
		}
		return CheckAssetDocumentEditorInstancedObject(Property, Value, Base, Depth, Path, Check);
	}
	const UObject* Asset = Check.Owner != nullptr && !Check.Owner->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
		? Check.Owner->GetOutermostObject() : nullptr;
	if (Asset != nullptr && !IsAssetDocumentEditorObjectInsideAsset(Current, *Asset))
	{
		// Listed fields that keep the referenced object's values change nothing (an extracted value re-applies).
		const FValue* Properties = FindAssetDocumentEditorTypedField(Value, TEXT("Properties"));
		bool bChanges = false;
		for (int32 Index = 0; Properties != nullptr && Properties->Kind == EKind::Object && !bChanges && Index < Properties->ObjectKeys.Num(); ++Index)
		{
			FProperty* Field = FindAssetDocumentEditorSubobjectField(*Current->GetClass(), Properties->ObjectKeys[Index], Depth);
			TSharedPtr<FJsonValue> Listed;
			FString Code;
			bChanges = Field == nullptr || !TryConvertAssetDocumentEditorValueToJson(Properties->Elements[Index], Listed, Code)
				|| !AreAssetDocumentEditorJsonValuesEqual(ExportAssetDocumentEditorJson(*Field, Field->ContainerPtrToValuePtr<void>(Current)), Listed);
		}
		if (bChanges)
		{
			const FString Owning = Current->GetOutermostObject() != nullptr ? Current->GetOutermostObject()->GetPathName() : Current->GetPathName();
			return FailAssetDocumentEditorCheck(Check, AppendAssetDocumentEditorPath(Path, TEXT("Properties")), FString::Printf(
				TEXT("the property references %s, which belongs to %s, not to this asset: the Details panel edits that object's ")
				TEXT("fields in place, which changes %s. Author it in that asset's own document, or give another class here to ")
				TEXT("create this asset's own object (the class picker)."), *Current->GetPathName(), *Owning, *Owning));
		}
	}
	return CheckAssetDocumentEditorInstancedObject(Property, Value, Base, Depth, Path, Check);
}

bool CheckAssetDocumentEditorInstancedStruct(
	const FProperty& Property,
	const FValue& Value,
	const void* Base,
	int32 Depth,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	if (Value.Kind == EKind::Null)
	{
		return true;
	}
	if (Value.Kind != EKind::Object)
	{
		return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
			TEXT("an instanced struct is {\"Struct\": \"<struct path>\", \"Properties\": {...}} or null, got %s."),
			DescribeAssetDocumentEditorKind(Value.Kind)));
	}
	if (!CheckAssetDocumentEditorExactKeys(Value, {TEXT("Struct")}, {TEXT("Properties")}, Path, Check))
	{
		return false;
	}
	const FValue& StructValue = *FindAssetDocumentEditorTypedField(Value, TEXT("Struct"));
	const FString StructPath = AppendAssetDocumentEditorPath(Path, TEXT("Struct"));
	const UScriptStruct* Struct = StructValue.Kind == EKind::String
		? Cast<UScriptStruct>(ResolveAssetDocumentEditorObjectPath(
			StructValue.StringValue, Check.Mode == EAssetDocumentEditorCheckMode::Decode))
		: nullptr;
	if (Struct == nullptr)
	{
		return FailAssetDocumentEditorCheck(Check, StructPath,
			TEXT("Struct must be the path of a script struct (e.g. \"/Script/Module.StructName\")."));
	}
	const FString BaseStructName = GetAssetDocumentEditorMetadataProperty(Property).GetMetaData(TEXT("BaseStruct"));
	if (Check.Mode == EAssetDocumentEditorCheckMode::Decode && !BaseStructName.IsEmpty())
	{
		const UScriptStruct* BaseStruct = FindFirstObject<UScriptStruct>(*BaseStructName, EFindFirstObjectOptions::None);
		if (BaseStruct == nullptr)
		{
			BaseStruct = LoadObject<UScriptStruct>(nullptr, *BaseStructName);
		}
		if (BaseStruct != nullptr && !Struct->IsChildOf(BaseStruct))
		{
			return FailAssetDocumentEditorCheck(Check, StructPath, FString::Printf(
				TEXT("%s is not derived from %s (BaseStruct)."), *Struct->GetPathName(), *BaseStruct->GetPathName()));
		}
	}
	const FValue* Properties = FindAssetDocumentEditorTypedField(Value, TEXT("Properties"));
	if (Properties == nullptr)
	{
		return true;
	}
	const FString PropertiesPath = AppendAssetDocumentEditorPath(Path, TEXT("Properties"));
	if (Properties->Kind != EKind::Object)
	{
		return FailAssetDocumentEditorCheck(Check, PropertiesPath, TEXT("Properties must be an object."));
	}
	// Omitted members keep the current value (same struct) or the struct defaults.
	const FInstancedStruct* Current = static_cast<const FInstancedStruct*>(Base);
	FStructOnScope Defaults;
	const void* MemberBase = nullptr;
	if (Current != nullptr && Current->GetScriptStruct() == Struct && Current->GetMemory() != nullptr)
	{
		MemberBase = Current->GetMemory();
	}
	else
	{
		Defaults.Initialize(Struct);
		MemberBase = Defaults.GetStructMemory();
	}
	for (int32 Index = 0; Index < Properties->ObjectKeys.Num(); ++Index)
	{
		const FString& Name = Properties->ObjectKeys[Index];
		const FString FieldPath = AppendAssetDocumentEditorPath(PropertiesPath, Name);
		FProperty* Field = FindAssetDocumentEditorInstancedStructField(*Struct, Name, Depth);
		if (Field == nullptr)
		{
			return FailAssetDocumentEditorCheck(Check, FieldPath, FString::Printf(
				TEXT("'%s' is not a member of %s."), *Name, *Struct->GetName()));
		}
		if (!IsAssetDocumentEditorPropertyAdmitted(*Field, false, Depth + 1))
		{
			if (!CheckAssetDocumentEditorMarkedMember(*Field, Properties->Elements[Index],
				MemberBase != nullptr ? Field->ContainerPtrToValuePtr<void>(MemberBase) : nullptr, FieldPath, Check))
			{
				return false;
			}
			continue;
		}
		if (!IsAssetDocumentEditorValueChecked(*Field, Properties->Elements[Index],
			MemberBase != nullptr ? Field->ContainerPtrToValuePtr<void>(MemberBase) : nullptr,
			false, Depth + 1, FieldPath, Check))
		{
			return false;
		}
	}
	return true;
}

bool CheckAssetDocumentEditorText(
	const FValue& Value,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	if (Value.Kind != EKind::String)
	{
		return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
			TEXT("expected a string (text source string), got %s."), DescribeAssetDocumentEditorKind(Value.Kind)));
	}
	if (Check.Mode == EAssetDocumentEditorCheckMode::Decode && FTextStringHelper::IsComplexText(*Value.StringValue))
	{
		FText Parsed;
		if (FTextStringHelper::ReadFromBuffer(*Value.StringValue, Parsed) == nullptr)
		{
			return FailAssetDocumentEditorCheck(Check, Path, TEXT(
				"the text macro (NSLOCTEXT/LOCTEXT/INVTEXT/LOCTABLE) could not be parsed."));
		}
	}
	return true;
}

bool CheckAssetDocumentEditorEnumName(
	FProperty& Property,
	const FValue& Value,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	const UEnum* Enum = nullptr;
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
	{
		Enum = EnumProperty->GetEnum();
	}
	else if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
	{
		Enum = Numeric->GetIntPropertyEnum();
	}
	if (Enum == nullptr || Check.Mode == EAssetDocumentEditorCheckMode::Encode)
	{
		return true;
	}
	// The value is the enumerator's authored name, the spelling Extract reads back (a user enum's display name). Another
	// spelling of an enumerator (a user enum's internal NewEnumeratorN, other letter case) is refused here with the
	// name to write, not after the write when the read-back differs.
	const int32 Index = UE::AssetDocument::EditorPropertyValueCodec::FindEnumIndexBySpelling(*Enum, Value.StringValue);
	if (Index != INDEX_NONE && Enum->GetAuthoredNameStringByIndex(Index).Equals(Value.StringValue, ESearchCase::CaseSensitive))
	{
		return true;
	}
	if (Index != INDEX_NONE)
	{
		return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(TEXT("'%s' is the enumerator '%s' of %s: write '%s' (the name Extract reads back). Values: %s."),
			*Value.StringValue, *Enum->GetAuthoredNameStringByIndex(Index), *Enum->GetName(), *Enum->GetAuthoredNameStringByIndex(Index),
			*UE::AssetDocument::EditorPropertyValueCodec::DescribeEnumSpellings(*Enum)));
	}
	return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(TEXT("'%s' is not a value of %s. Values: %s."),
		*Value.StringValue, *Enum->GetName(), *UE::AssetDocument::EditorPropertyValueCodec::DescribeEnumSpellings(*Enum)));
}

// A JSON number is a number: UE's JSON writer prints the double 1.0 as "1", so a
// float property accepts an integer literal (the exact-value readback check still
// rejects integers the float cannot hold). An integer property accepts a number
// only when it is integral and inside int64; a fraction is never truncated.
bool IsAssetDocumentEditorFloatingValueCompatible(
	const FAssetDocumentRuntimeValueDto& Value)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::Number
		|| Value.Kind == EAssetDocumentRuntimeValueKind::Integer;
}

bool IsAssetDocumentEditorIntegerValueCompatible(
	const FAssetDocumentRuntimeValueDto& Value)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
	{
		return true;
	}
	constexpr double Int64Bound = 9223372036854775808.0; // 2^63
	return Value.Kind == EAssetDocumentRuntimeValueKind::Number
		&& FMath::IsFinite(Value.NumberValue)
		&& FMath::Frac(Value.NumberValue) == 0.0
		&& Value.NumberValue >= -Int64Bound
		&& Value.NumberValue < Int64Bound;
}

bool IsAssetDocumentEditorScalarValueCompatible(
	FProperty& Property,
	const FValue& Value)
{
	if (CastField<FBoolProperty>(&Property) != nullptr)
	{
		return Value.Kind == EAssetDocumentRuntimeValueKind::Boolean;
	}
	const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
	if (Numeric != nullptr && !Numeric->IsEnum())
	{
		return Numeric->IsFloatingPoint()
			? IsAssetDocumentEditorFloatingValueCompatible(Value)
			: IsAssetDocumentEditorIntegerValueCompatible(Value);
	}
	return Value.Kind == EAssetDocumentRuntimeValueKind::String;
}

EKind GetAssetDocumentEditorScalarExpectedKind(FProperty& Property)
{
	if (CastField<FBoolProperty>(&Property) != nullptr)
	{
		return EKind::Boolean;
	}
	const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
	if (Numeric != nullptr && !Numeric->IsEnum())
	{
		return Numeric->IsFloatingPoint() ? EKind::Number : EKind::Integer;
	}
	return EKind::String;
}

TSharedPtr<FJsonValue> ExportAssetDocumentEditorJson(FProperty& Property, const void* Value);
bool AreAssetDocumentEditorJsonValuesEqual(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right);

// Whether an authored marker is the marker the current value (held by Within) reads as. Text naming objects inside
// the asset compares as the asset's own path names them (EditorMarkerText): the same content reads alike in the
// asset, in Validate's copy of it, and after a move; any other path compares as it is. Without Within the text
// compares exactly.
bool IsAssetDocumentEditorSameMarker(const TSharedPtr<FJsonValue>& Authored, const TSharedPtr<FJsonValue>& Current,
	const UObject* Within = nullptr)
{
	const TSharedPtr<FJsonObject>* AuthoredObject = nullptr;
	const TSharedPtr<FJsonObject>* CurrentObject = nullptr;
	if (!Authored.IsValid() || !Current.IsValid() || !Authored->TryGetObject(AuthoredObject) || !Current->TryGetObject(CurrentObject))
	{
		return false;
	}
	FString AuthoredReason, CurrentReason, AuthoredText, CurrentText;
	if (!(*AuthoredObject)->TryGetStringField(TEXT("Unsupported"), AuthoredReason)
		|| !(*CurrentObject)->TryGetStringField(TEXT("Unsupported"), CurrentReason) || AuthoredReason != CurrentReason
		|| (*AuthoredObject)->Values.Num() != (*CurrentObject)->Values.Num())
	{
		return false;
	}
	(*AuthoredObject)->TryGetStringField(TEXT("Text"), AuthoredText);
	(*CurrentObject)->TryGetStringField(TEXT("Text"), CurrentText);
	return UE::AssetDocument::EditorMarkerText::IsSameText(AuthoredText, CurrentText, Within);
}

// A struct member the codec has no form for (it reads as a marker). A value given for it must be what the current
// value reads as, which keeps that value; there is no form to write another one.
bool CheckAssetDocumentEditorMarkedMember(
	FProperty& Field,
	const FValue& Value,
	const void* Current,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	if (Check.Mode == EAssetDocumentEditorCheckMode::Encode)
	{
		return true;
	}
	TSharedPtr<FJsonValue> Authored;
	FString Code;
	if (Current != nullptr && TryConvertAssetDocumentEditorValueToJson(Value, Authored, Code)
		&& IsAssetDocumentEditorSameMarker(Authored, ExportAssetDocumentEditorJson(Field, Current), Check.Owner))
	{
		return true;
	}
	return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
		TEXT("%s has no document form, so it cannot be written: leave its \"Unsupported\" marker as read, or omit the member to keep its value."),
		*Field.GetCPPType()));
}

bool CheckAssetDocumentEditorStruct(
	const FStructProperty& StructProperty,
	const FValue& Value,
	const void* Base,
	int32 Depth,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	if (Value.Kind != EKind::Object)
	{
		return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
			TEXT("expected an object with the members of %s, got %s."),
			*StructProperty.Struct->GetName(), DescribeAssetDocumentEditorKind(Value.Kind)));
	}
	TArray<FString> Expected;
	for (TFieldIterator<FProperty> It(StructProperty.Struct); It; ++It)
	{
		if (!IsAssetDocumentEditorStructCodecField(**It))
		{
			continue;
		}
		const FString Name = It->GetAuthoredName();
		Expected.Add(Name);
		const FValue* FieldValue = FindAssetDocumentEditorTypedField(Value, Name);
		if (!IsAssetDocumentEditorPropertyAdmitted(**It, false, Depth + 1))
		{
			// Omitted: keeps its value (or the member default).
			if (FieldValue != nullptr && !CheckAssetDocumentEditorMarkedMember(**It, *FieldValue,
				Base != nullptr ? It->ContainerPtrToValuePtr<void>(Base) : nullptr, AppendAssetDocumentEditorPath(Path, Name), Check))
			{
				return false;
			}
			continue;
		}
		if (FieldValue == nullptr)
		{
			// An omitted member keeps its current value; with nothing to keep
			// (a restore or a value outside an edit) every member is required.
			if (Check.bPartial || Base != nullptr)
			{
				continue;
			}
			return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
				TEXT("missing member '%s' of %s (this value has no current value to keep, so it lists every member)."),
				*Name, *StructProperty.Struct->GetName()));
		}
		if (!IsAssetDocumentEditorValueChecked(**It, *FieldValue,
			Base != nullptr ? It->ContainerPtrToValuePtr<void>(Base) : nullptr, false, Depth + 1,
			AppendAssetDocumentEditorPath(Path, Name), Check))
		{
			return false;
		}
	}
	for (const FString& Key : Value.ObjectKeys)
	{
		if (!Expected.Contains(Key))
		{
			return FailAssetDocumentEditorCheck(Check, AppendAssetDocumentEditorPath(Path, Key), FString::Printf(
				TEXT("'%s' is not a member of %s. Members: %s."), *Key, *StructProperty.Struct->GetName(),
				*FString::Join(Expected, TEXT(", "))));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Array elements identified by their marked content
// ---------------------------------------------------------------------------
// An array element edit starts from a current element. Where elements hold content with no document form (read as
// markers), that content belongs to one element only, so an element is matched to the current element whose markers
// it carries, wherever that element stands now (a removed, inserted or moved element keeps its own content).

bool IsAssetDocumentEditorMarkerJson(const TSharedPtr<FJsonValue>& Json)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	FString Reason;
	return Json.IsValid() && Json->TryGetObject(Object) && Object != nullptr && Object->IsValid()
		&& (*Object)->TryGetStringField(TEXT("Unsupported"), Reason);
}

bool HoldsAssetDocumentEditorMarkerJson(const TSharedPtr<FJsonValue>& Json, int32 Depth = 0)
{
	if (!Json.IsValid() || Depth > AssetDocumentEditorTypedValueMaximumDepth)
	{
		return false;
	}
	if (IsAssetDocumentEditorMarkerJson(Json))
	{
		return true;
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (Json->TryGetObject(Object) && Object != nullptr && Object->IsValid())
	{
		for (const auto& Entry : (*Object)->Values)
		{
			if (HoldsAssetDocumentEditorMarkerJson(Entry.Value, Depth + 1))
			{
				return true;
			}
		}
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
	if (Json->TryGetArray(Elements) && Elements != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Element : *Elements)
		{
			if (HoldsAssetDocumentEditorMarkerJson(Element, Depth + 1))
			{
				return true;
			}
		}
	}
	return false;
}

// The marker an earlier read gave a whole optional, map, array or set whose type had no document form then. The codec
// now reads such a value, but a document written before still holds the marker there: it keeps the current value, as
// it did when it was read, and is never decoded as the value (a map would take its two members for keys).
const TCHAR* const AssetDocumentEditorFormerMarkerSuffix = TEXT(" has no document form; AssetDoc keeps the value.");

bool IsAssetDocumentEditorFormerMarkerReason(const FString& Reason)
{
	for (const TCHAR* Type : { TEXT("TOptional"), TEXT("TMap"), TEXT("TArray"), TEXT("TSet") })
	{
		if (Reason.Equals(FString(Type) + AssetDocumentEditorFormerMarkerSuffix, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

bool IsAssetDocumentEditorFormerMarkerProperty(const FProperty& Property, const FString& Reason)
{
	const bool bContainer = CastField<FOptionalProperty>(&Property) != nullptr || CastField<FMapProperty>(&Property) != nullptr
		|| CastField<FArrayProperty>(&Property) != nullptr || CastField<FSetProperty>(&Property) != nullptr;
	return bContainer && Property.ArrayDim == 1
		&& Reason.Equals(Property.GetCPPType() + AssetDocumentEditorFormerMarkerSuffix, ESearchCase::CaseSensitive);
}

// Only the marker's own members: "Unsupported" and the read-only "Text".
bool IsAssetDocumentEditorFormerMarkerValue(const FProperty& Property, const FValue& Value)
{
	if (Value.Kind != EKind::Object || Value.ObjectKeys.Num() > 2)
	{
		return false;
	}
	const FValue* Reason = nullptr;
	for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
	{
		if (Value.Elements[Index].Kind != EKind::String)
		{
			return false;
		}
		if (Value.ObjectKeys[Index] == TEXT("Unsupported"))
		{
			Reason = &Value.Elements[Index];
		}
		else if (Value.ObjectKeys[Index] != TEXT("Text"))
		{
			return false;
		}
	}
	return Reason != nullptr && IsAssetDocumentEditorFormerMarkerProperty(Property, Reason->StringValue);
}

bool GetAssetDocumentEditorFormerMarkerJsonReason(const TSharedPtr<FJsonValue>& Json, FString& OutReason)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Json.IsValid() || !Json->TryGetObject(Object) || Object == nullptr || !Object->IsValid() || (*Object)->Values.Num() > 2)
	{
		return false;
	}
	for (const auto& Entry : (*Object)->Values)
	{
		const FString Name(*Entry.Key);
		if (!Entry.Value.IsValid() || Entry.Value->Type != EJson::String || (Name != TEXT("Unsupported") && Name != TEXT("Text")))
		{
			return false;
		}
	}
	return (*Object)->TryGetStringField(TEXT("Unsupported"), OutReason);
}

bool IsAssetDocumentEditorFormerMarkerJson(const FProperty& Property, const TSharedPtr<FJsonValue>& Json)
{
	FString Reason;
	return GetAssetDocumentEditorFormerMarkerJsonReason(Json, Reason) && IsAssetDocumentEditorFormerMarkerProperty(Property, Reason);
}

// Whether a value of Property can hold members with no document form (they read as markers): a struct member-wise
// value with such a member, at any depth, or a container of such values.
bool HasAssetDocumentEditorUnformedMembers(FProperty& Property, int32 Depth)
{
	FString LeafCodecId;
	if (Depth > AssetDocumentEditorTypedValueMaximumDepth || IsAssetDocumentEditorPropertyBag(Property)
		|| TryGetAssetDocumentEditorLeafCodecId(Property, LeafCodecId))
	{
		return false;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			if (IsAssetDocumentEditorStructCodecField(**It)
				&& (!IsAssetDocumentEditorPropertyAdmitted(**It, false, Depth + 1) || HasAssetDocumentEditorUnformedMembers(**It, Depth + 1)))
			{
				return true;
			}
		}
		return false;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		return HasAssetDocumentEditorUnformedMembers(*Array->Inner, Depth + 1);
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		return HasAssetDocumentEditorUnformedMembers(*Set->ElementProp, Depth + 1);
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		return HasAssetDocumentEditorUnformedMembers(*Map->ValueProp, Depth + 1);
	}
	return false;
}

// How an authored value relates to a current value's export, ordered from no match to an exact one:
// Different: a marker it carries is not the current value's at the same place;
// Nested: its own markers are the current value's, but a marked element of an array inside it is not found there
//         (that array matches its elements itself);
// Kept: every marker it carries is the current value's;
// Equal: Kept, and every other value it gives is the current one too.
enum class EAssetDocumentEditorMarkedMatch : uint8
{
	Different,
	Nested,
	Kept,
	Equal,
};

// bStrictArrays: a marked element of an inner array must be found in the current array (Different otherwise).
EAssetDocumentEditorMarkedMatch MatchAssetDocumentEditorMarkedJson(
	const TSharedPtr<FJsonValue>& Authored,
	const TSharedPtr<FJsonValue>& Current,
	const UObject* Within,
	bool bStrictArrays,
	int32 Depth)
{
	using EMatch = EAssetDocumentEditorMarkedMatch;
	if (!Authored.IsValid() || Depth > AssetDocumentEditorTypedValueMaximumDepth)
	{
		return EMatch::Different;
	}
	if (IsAssetDocumentEditorMarkerJson(Authored))
	{
		if (IsAssetDocumentEditorSameMarker(Authored, Current, Within))
		{
			return EMatch::Equal;
		}
		// The marker of a value the codec now reads keeps that value wherever it stands.
		FString Reason;
		return GetAssetDocumentEditorFormerMarkerJsonReason(Authored, Reason) && IsAssetDocumentEditorFormerMarkerReason(Reason)
			&& Current.IsValid() && !IsAssetDocumentEditorMarkerJson(Current) ? EMatch::Kept : EMatch::Different;
	}
	const TSharedPtr<FJsonObject>* AuthoredObject = nullptr;
	if (Authored->TryGetObject(AuthoredObject) && AuthoredObject != nullptr && AuthoredObject->IsValid())
	{
		const TSharedPtr<FJsonObject>* CurrentObject = nullptr;
		const bool bObject = Current.IsValid() && Current->TryGetObject(CurrentObject) && CurrentObject != nullptr && CurrentObject->IsValid();
		EMatch Out = bObject ? EMatch::Equal : EMatch::Kept;
		for (const auto& Entry : (*AuthoredObject)->Values)
		{
			const TSharedPtr<FJsonValue> Inner = bObject ? (*CurrentObject)->TryGetField(Entry.Key) : nullptr;
			const EMatch Member = MatchAssetDocumentEditorMarkedJson(Entry.Value, Inner, Within, bStrictArrays, Depth + 1);
			if (Member == EMatch::Different)
			{
				return EMatch::Different;
			}
			Out = Member < Out ? Member : Out;
		}
		return Out;
	}
	const TArray<TSharedPtr<FJsonValue>>* AuthoredElements = nullptr;
	if (Authored->TryGetArray(AuthoredElements) && AuthoredElements != nullptr)
	{
		const TArray<TSharedPtr<FJsonValue>>* CurrentElements = nullptr;
		const bool bArray = Current.IsValid() && Current->TryGetArray(CurrentElements) && CurrentElements != nullptr;
		bool bEqual = bArray && CurrentElements->Num() == AuthoredElements->Num();
		bool bFoundAll = true;
		for (int32 Index = 0; Index < AuthoredElements->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Element = (*AuthoredElements)[Index];
			if (bEqual && MatchAssetDocumentEditorMarkedJson(Element, (*CurrentElements)[Index], Within, bStrictArrays, Depth + 1) == EMatch::Equal)
			{
				continue;
			}
			bEqual = false;
			if (!HoldsAssetDocumentEditorMarkerJson(Element))
			{
				continue;
			}
			bool bFound = false;
			for (int32 Candidate = 0; bArray && !bFound && Candidate < CurrentElements->Num(); ++Candidate)
			{
				bFound = MatchAssetDocumentEditorMarkedJson(Element, (*CurrentElements)[Candidate], Within, bStrictArrays, Depth + 1) != EMatch::Different;
			}
			bFoundAll = bFoundAll && bFound;
		}
		if (!bFoundAll)
		{
			return bStrictArrays ? EMatch::Different : EMatch::Nested;
		}
		return bEqual ? EMatch::Equal : EMatch::Kept;
	}
	return Current.IsValid() && FJsonValue::CompareEqual(*Authored, *Current) ? EMatch::Equal : EMatch::Kept;
}

// Whether an element without markers, decoded as an edit of Current (the current element at its index), would keep
// marked content of Current that it leaves out and that the default element does not hold: the element cannot be
// told to be that one, so that content could belong to another element.
bool LeavesOutAssetDocumentEditorMarkedContent(
	const TSharedPtr<FJsonValue>& Authored,
	const TSharedPtr<FJsonValue>& Current,
	const TSharedPtr<FJsonValue>& Default,
	const UObject* Within,
	int32 Depth)
{
	if (!Current.IsValid() || Depth > AssetDocumentEditorTypedValueMaximumDepth)
	{
		return false;
	}
	if (!Authored.IsValid())
	{
		return HoldsAssetDocumentEditorMarkerJson(Current)
			&& MatchAssetDocumentEditorMarkedJson(Current, Default, Within, true, Depth) == EAssetDocumentEditorMarkedMatch::Different;
	}
	const TSharedPtr<FJsonObject>* AuthoredObject = nullptr;
	const TSharedPtr<FJsonObject>* CurrentObject = nullptr;
	if (IsAssetDocumentEditorMarkerJson(Current) || !Authored->TryGetObject(AuthoredObject) || AuthoredObject == nullptr
		|| !AuthoredObject->IsValid() || !Current->TryGetObject(CurrentObject) || CurrentObject == nullptr || !CurrentObject->IsValid())
	{
		// A whole value given (an array given matches its own elements).
		return false;
	}
	const TSharedPtr<FJsonObject>* DefaultObject = nullptr;
	const bool bDefaultObject = Default.IsValid() && Default->TryGetObject(DefaultObject) && DefaultObject != nullptr && DefaultObject->IsValid();
	for (const auto& Entry : (*CurrentObject)->Values)
	{
		if (LeavesOutAssetDocumentEditorMarkedContent((*AuthoredObject)->TryGetField(Entry.Key), Entry.Value,
			bDefaultObject ? (*DefaultObject)->TryGetField(Entry.Key) : nullptr, Within, Depth + 1))
		{
			return true;
		}
	}
	return false;
}

// Whether the elements of an array value are matched by identity (MatchAssetDocumentEditorArrayElements) rather than
// by index: its element type has members with no document form, or the value carries markers.
bool IsAssetDocumentEditorArrayMatchedByIdentity(const FArrayProperty& Array, const TArray<TSharedPtr<FJsonValue>>& Elements)
{
	if (HasAssetDocumentEditorUnformedMembers(*Array.Inner, 0))
	{
		return true;
	}
	for (const TSharedPtr<FJsonValue>& Element : Elements)
	{
		if (HoldsAssetDocumentEditorMarkerJson(Element))
		{
			return true;
		}
	}
	return false;
}

enum class EAssetDocumentEditorArrayMatchFailure : uint8
{
	None,
	// An element carries markers no current element left and the default element do not have.
	MarkerNotFound,
	// An element without markers would take marked content of the current element at its index (see
	// LeavesOutAssetDocumentEditorMarkedContent).
	MarkedContentLeftOut,
};

// The current element each authored element continues (OutMatch: its index in Current, or INDEX_NONE for one that
// starts from DefaultElement). An element carrying markers continues the current element (not continued by another)
// whose markers it carries: an equal one first, the one at its own index next, else the first. One whose markers are
// the default element's and that no current element has starts from the default. An element without markers continues
// the current element at its index when no element carrying markers took it, else starts from the default.
// False (OutFailedIndex, OutFailure) when an element cannot be matched that way.
bool MatchAssetDocumentEditorArrayElements(
	const FArrayProperty& Array,
	const TArray<TSharedPtr<FJsonValue>>& Authored,
	const void* Current,
	const void* DefaultElement,
	const UObject* Within,
	TArray<int32>& OutMatch,
	int32& OutFailedIndex,
	EAssetDocumentEditorArrayMatchFailure& OutFailure)
{
	using EMatch = EAssetDocumentEditorMarkedMatch;
	FProperty& Inner = *Array.Inner;
	FScriptArrayHelper CurrentHelper(&Array, Current);
	const int32 CurrentNum = Current != nullptr ? CurrentHelper.Num() : 0;
	TArray<TSharedPtr<FJsonValue>> CurrentJson;
	CurrentJson.SetNum(CurrentNum);
	auto CurrentAt = [&](int32 Index) -> const TSharedPtr<FJsonValue>&
	{
		if (!CurrentJson[Index].IsValid())
		{
			CurrentJson[Index] = ExportAssetDocumentEditorJson(Inner, CurrentHelper.GetRawPtr(Index));
		}
		return CurrentJson[Index];
	};
	const TSharedPtr<FJsonValue> DefaultJson = ExportAssetDocumentEditorJson(Inner, DefaultElement);
	TArray<bool> Taken;
	Taken.Init(false, CurrentNum);
	OutMatch.Init(INDEX_NONE, Authored.Num());
	OutFailedIndex = INDEX_NONE;
	OutFailure = EAssetDocumentEditorArrayMatchFailure::None;
	for (int32 Index = 0; Index < Authored.Num(); ++Index)
	{
		if (!HoldsAssetDocumentEditorMarkerJson(Authored[Index]))
		{
			continue;
		}
		int32 Best = INDEX_NONE;
		EMatch BestMatch = EMatch::Different;
		for (int32 Step = -1; Step < CurrentNum && BestMatch != EMatch::Equal; ++Step)
		{
			const int32 Candidate = Step < 0 ? Index : Step;
			if (Candidate >= CurrentNum || (Step >= 0 && Candidate == Index) || Taken[Candidate])
			{
				continue;
			}
			const EMatch Match = MatchAssetDocumentEditorMarkedJson(Authored[Index], CurrentAt(Candidate), Within, false, 0);
			if (Match > BestMatch)
			{
				Best = Candidate;
				BestMatch = Match;
			}
		}
		if (Best != INDEX_NONE)
		{
			OutMatch[Index] = Best;
			Taken[Best] = true;
		}
		else if (MatchAssetDocumentEditorMarkedJson(Authored[Index], DefaultJson, Within, false, 0) == EMatch::Different)
		{
			OutFailedIndex = Index;
			OutFailure = EAssetDocumentEditorArrayMatchFailure::MarkerNotFound;
			return false;
		}
	}
	for (int32 Index = 0; Index < FMath::Min(Authored.Num(), CurrentNum); ++Index)
	{
		if (HoldsAssetDocumentEditorMarkerJson(Authored[Index]) || Taken[Index])
		{
			continue;
		}
		if (LeavesOutAssetDocumentEditorMarkedContent(Authored[Index], CurrentAt(Index), DefaultJson, Within, 0))
		{
			OutFailedIndex = Index;
			OutFailure = EAssetDocumentEditorArrayMatchFailure::MarkedContentLeftOut;
			return false;
		}
		OutMatch[Index] = Index;
	}
	return true;
}

FString DescribeAssetDocumentEditorArrayMatchFailure(const FProperty& Inner, EAssetDocumentEditorArrayMatchFailure Failure)
{
	if (Failure == EAssetDocumentEditorArrayMatchFailure::MarkerNotFound)
	{
		return FString::Printf(TEXT("this %s element carries an \"Unsupported\" marker that no element of the current array has "
			"(each current element's marked content can be kept by one element only). Elements whose content has no "
			"document form are matched by their markers: leave each element's markers as read (an element may move), "
			"and do not edit or copy them."), *Inner.GetCPPType());
	}
	return FString::Printf(TEXT("this %s element leaves out members with no document form, and the current element at "
		"this index holds content for them: AssetDoc cannot tell whether it is that element. Give the element's "
		"\"Unsupported\" markers as read to keep that content with it, or remove the element (in a separate apply) "
		"before adding a new one here."), *Inner.GetCPPType());
}

bool CheckAssetDocumentEditorElements(
	FProperty& Element,
	TConstArrayView<FValue> Elements,
	TFunctionRef<const void*(int32)> BaseAt,
	bool bIgnoreStaticArray,
	int32 Depth,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	for (int32 Index = 0; Index < Elements.Num(); ++Index)
	{
		if (!IsAssetDocumentEditorValueChecked(Element, Elements[Index], BaseAt(Index), bIgnoreStaticArray, Depth + 1,
			AppendAssetDocumentEditorPath(Path, Index), Check))
		{
			return false;
		}
	}
	return true;
}

// A struct map key names a key the map holds (kept as stored) or parses to a key that reads back as the same text.
bool IsAssetDocumentEditorStructMapKeyChecked(
	const FMapProperty& Map,
	const void* Base,
	const FString& Text,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	// An encoded value is the asset's own key (only its shape is checked).
	if (Check.Mode == EAssetDocumentEditorCheckMode::Encode || FindAssetDocumentEditorStructMapKeyIndex(Map, Base, Text) != INDEX_NONE)
	{
		return true;
	}
	FAssetDocumentEditorScopedPropertyValue Parsed(*Map.KeyProp);
	const bool bGameplayTag = IsAssetDocumentEditorGameplayTag(*Map.KeyProp);
	if (!Parsed.IsValid() || !ParseAssetDocumentEditorStructMapKey(*Map.KeyProp, Text, Parsed.GetMemory()))
	{
		return FailAssetDocumentEditorCheck(Check, Path, bGameplayTag
			? FString::Printf(TEXT("'%s' is not a registered gameplay tag: write the key as a tag name the tag picker lists ('Ability.Cooldown')."), *Text)
			: FString::Printf(TEXT("'%s' is not a %s map key: write the key as the struct's export text, as extract shows it."),
				*Text, *CastFieldChecked<FStructProperty>(Map.KeyProp)->Struct->GetName()));
	}
	const FString Canonical = ExportAssetDocumentEditorStructMapKey(*Map.KeyProp, Parsed.GetMemory());
	if (!Canonical.Equals(Text, ESearchCase::CaseSensitive))
	{
		return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
			TEXT("the map key '%s' reads back as '%s': write the key as '%s' (one name per key)."), *Text, *Canonical, *Canonical));
	}
	return true;
}

bool CheckAssetDocumentEditorContainer(
	FProperty& Property,
	const FValue& Value,
	const void* Base,
	int32 Depth,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		if (Value.Kind != EKind::Object)
		{
			return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
				TEXT("expected an object (map keyed by string), got %s."), DescribeAssetDocumentEditorKind(Value.Kind)));
		}
		// Names, strings, paths and the names inside struct keys compare ignoring case, so keys that differ only in
		// case are one key: the map would keep only one of the entries.
		const bool bFoldedKeys = AsAssetDocumentEditorIntegerMapKey(*Map->KeyProp) == nullptr;
		TMap<FString, int32> FoldedKeys;
		for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
		{
			const FString EntryPath = AppendAssetDocumentEditorPath(Path, Value.ObjectKeys[Index]);
			if (bFoldedKeys && Check.Mode == EAssetDocumentEditorCheckMode::Decode)
			{
				const FString Folded = Value.ObjectKeys[Index].ToLower();
				if (const int32* Earlier = FoldedKeys.Find(Folded))
				{
					return FailAssetDocumentEditorCheck(Check, EntryPath, FString::Printf(
						TEXT("the map keys '%s' and '%s' are one %s key (keys compare ignoring case): give one entry for it."),
						*Value.ObjectKeys[*Earlier], *Value.ObjectKeys[Index], *Map->KeyProp->GetCPPType()));
				}
				FoldedKeys.Add(Folded, Index);
			}
			if (IsAssetDocumentEditorSoftReference(*Map->KeyProp) && Check.Mode == EAssetDocumentEditorCheckMode::Decode
				&& !Value.ObjectKeys[Index].IsEmpty())
			{
				const FSoftObjectPath KeyPath(Value.ObjectKeys[Index]);
				if (!KeyPath.IsValid() || KeyPath.ToString() != Value.ObjectKeys[Index])
				{
					return FailAssetDocumentEditorCheck(Check, EntryPath, FString::Printf(
						TEXT("'%s' is not an object path: write the key as the object's path ('/Game/Folder/Asset.Asset'), as extract shows it."),
						*Value.ObjectKeys[Index]));
				}
			}
			FValue Key;
			Key.Kind = EKind::String;
			Key.StringValue = Value.ObjectKeys[Index];
			if (const FNumericProperty* IntegerKey = AsAssetDocumentEditorIntegerMapKey(*Map->KeyProp))
			{
				if (!TryParseAssetDocumentEditorIntegerMapKey(*IntegerKey, Value.ObjectKeys[Index], Key.IntegerValue))
				{
					return FailAssetDocumentEditorCheck(Check, EntryPath, FString::Printf(
						TEXT("'%s' is not a %s map key: write the key as a decimal integer ('0', '3', '-1') inside the type's range."),
						*Value.ObjectKeys[Index], *IntegerKey->GetCPPType()));
				}
				Key.Kind = EKind::Integer;
				Key.StringValue.Reset();
			}
			if (Check.Mode == EAssetDocumentEditorCheckMode::Decode && Value.Elements[Index].Kind == EKind::Null
				&& AsAssetDocumentEditorInstancedObject(*Map->ValueProp) != nullptr)
			{
				return FailAssetDocumentEditorCheck(Check, EntryPath,
					TEXT("an instanced subobject map value cannot be null; give it a {\"Class\", \"Properties\"} value or remove the entry."));
			}
			if (IsAssetDocumentEditorStructMapKey(*Map->KeyProp))
			{
				if (!IsAssetDocumentEditorStructMapKeyChecked(*Map, Base, Value.ObjectKeys[Index], EntryPath, Check))
				{
					return false;
				}
			}
			else if (!IsAssetDocumentEditorValueChecked(*Map->KeyProp, Key, nullptr, false, Depth + 1, EntryPath, Check))
			{
				return false;
			}
			// A value keeps the current value of the entry with the same key.
			const void* ValueBase = Check.Mode == EAssetDocumentEditorCheckMode::Decode
				? FindAssetDocumentEditorMapBaseValue(*Map, Base, Value.ObjectKeys[Index]) : nullptr;
			if (!IsAssetDocumentEditorValueChecked(*Map->ValueProp, Value.Elements[Index], ValueBase, false, Depth + 1, EntryPath, Check))
			{
				return false;
			}
		}
		return true;
	}
	FProperty* Element = nullptr;
	const FArrayProperty* Array = CastField<FArrayProperty>(&Property);
	if (Array != nullptr)
	{
		Element = Array->Inner;
	}
	else if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		Element = Set->ElementProp;
	}
	if (Element == nullptr)
	{
		return FailAssetDocumentEditorCheck(Check, Path, TEXT("the property type has no document form."));
	}
	if (Value.Kind != EKind::Array)
	{
		return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
			TEXT("expected an array, got %s."), DescribeAssetDocumentEditorKind(Value.Kind)));
	}
	// Owners commonly iterate instanced arrays in PostEditChangeProperty without
	// null checks (UPaperTileMap::TileLayers crashes the editor), so a write may
	// not add empty instanced elements; existing values still encode.
	if (Check.Mode == EAssetDocumentEditorCheckMode::Decode && AsAssetDocumentEditorInstancedObject(*Element) != nullptr)
	{
		for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
		{
			if (Value.Elements[Index].Kind == EKind::Null)
			{
				return FailAssetDocumentEditorCheck(Check, AppendAssetDocumentEditorPath(Path, Index),
					TEXT("an instanced subobject element cannot be null; give it a {\"Class\", \"Properties\"} value or remove it."));
			}
		}
	}
	// Array elements keep the current element at the same index; set elements
	// are whole values. Elements with content that has no document form keep
	// the current element they are identified with (see
	// MatchAssetDocumentEditorArrayElements; the import matches them alike).
	if (Array != nullptr && Base != nullptr && Check.Mode == EAssetDocumentEditorCheckMode::Decode)
	{
		TSharedPtr<FJsonValue> Json;
		FString Code;
		const TArray<TSharedPtr<FJsonValue>>* JsonElements = nullptr;
		if (TryConvertAssetDocumentEditorValueToJson(Value, Json, Code) && Json.IsValid() && Json->TryGetArray(JsonElements)
			&& JsonElements != nullptr && IsAssetDocumentEditorArrayMatchedByIdentity(*Array, *JsonElements))
		{
			FAssetDocumentEditorScopedPropertyValue DefaultElement(*Element);
			TArray<int32> Match;
			int32 FailedIndex = INDEX_NONE;
			EAssetDocumentEditorArrayMatchFailure Failure = EAssetDocumentEditorArrayMatchFailure::None;
			if (!DefaultElement.IsValid())
			{
				return FailAssetDocumentEditorCheck(Check, Path, TEXT("the array's element type could not be initialized."));
			}
			if (!MatchAssetDocumentEditorArrayElements(*Array, *JsonElements, Base, DefaultElement.GetMemory(), Check.Owner, Match, FailedIndex, Failure))
			{
				return FailAssetDocumentEditorCheck(Check, AppendAssetDocumentEditorPath(Path, FailedIndex),
					DescribeAssetDocumentEditorArrayMatchFailure(*Element, Failure));
			}
			return CheckAssetDocumentEditorElements(*Element, Value.Elements,
				[Array, Base, &Match, &DefaultElement](int32 Index) -> const void*
				{
					return Match.IsValidIndex(Index) && Match[Index] != INDEX_NONE
						? FScriptArrayHelper(Array, Base).GetRawPtr(Match[Index]) : DefaultElement.GetMemory();
				},
				false, Depth, Path, Check);
		}
	}
	const int32 BaseNum = Array != nullptr && Base != nullptr ? FScriptArrayHelper(Array, Base).Num() : 0;
	return CheckAssetDocumentEditorElements(*Element, Value.Elements,
		[Array, Base, BaseNum](int32 Index) -> const void*
		{
			return Index < BaseNum ? FScriptArrayHelper(Array, Base).GetRawPtr(Index) : nullptr;
		},
		false, Depth, Path, Check);
}

// null is a value only where the Details panel can show "None": references,
// instanced subobjects and instanced structs.
bool IsAssetDocumentEditorNullable(FProperty& Property, bool bIgnoreStaticArray)
{
	if (HasAssetDocumentEditorStaticArrayShape(Property, bIgnoreStaticArray))
	{
		return false;
	}
	return AsAssetDocumentEditorInstancedObject(Property) != nullptr
		|| AsAssetDocumentEditorHardReference(Property) != nullptr
		|| CastField<FInterfaceProperty>(&Property) != nullptr
		|| IsAssetDocumentEditorInstancedStruct(Property)
		|| CastField<FOptionalProperty>(&Property) != nullptr;
}

bool IsAssetDocumentEditorValueChecked(
	FProperty& Property,
	const FValue& Value,
	const void* Base,
	bool bIgnoreStaticArray,
	int32 Depth,
	const FString& Path,
	FAssetDocumentEditorValueCheck& Check)
{
	if (Depth > AssetDocumentEditorTypedValueMaximumDepth)
	{
		return FailAssetDocumentEditorCheck(Check, Path, TEXT("the value nests too deeply."));
	}
	if (Value.Kind == EKind::Null && !IsAssetDocumentEditorNullable(Property, bIgnoreStaticArray))
	{
		return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
			TEXT("null is not a %s value (only references, instanced subobjects and instanced structs take null); give a value, or omit a struct member to keep its current value."),
			*Property.GetCPPType()));
	}
	if (IsAssetDocumentEditorFormerMarkerValue(Property, Value))
	{
		// Kept, never decoded; with no current value there is nothing to keep.
		return Check.Mode == EAssetDocumentEditorCheckMode::Encode || Base != nullptr
			|| FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
				TEXT("this %s has a document form now and no current value to keep: write the value as extract shows it."),
				*Property.GetCPPType()));
	}
	if (HasAssetDocumentEditorStaticArrayShape(Property, bIgnoreStaticArray))
	{
		if (Value.Kind != EKind::Array || Value.Elements.Num() != Property.ArrayDim)
		{
			return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
				TEXT("expected an array of exactly %d elements (fixed-size array)."), Property.ArrayDim));
		}
		const int32 ElementSize = Property.GetElementSize();
		return CheckAssetDocumentEditorElements(Property, Value.Elements,
			[Base, ElementSize](int32 Index) -> const void*
			{
				return Base != nullptr ? static_cast<const uint8*>(Base) + Index * ElementSize : nullptr;
			},
			true, Depth, Path, Check);
	}
	if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(&Property))
	{
		// null is the unset optional; any other value is the set value, an edit of the current one when it is set.
		return Value.Kind == EKind::Null
			|| IsAssetDocumentEditorValueChecked(*Optional->GetValueProperty(), Value,
				Base != nullptr ? Optional->GetValuePointerForReadIfSet(Base) : nullptr, false, Depth + 1, Path, Check);
	}
	if (CastField<FTextProperty>(&Property) != nullptr)
	{
		return CheckAssetDocumentEditorText(Value, Path, Check);
	}
	if (const FObjectProperty* Instanced = AsAssetDocumentEditorInstancedObject(Property))
	{
		return CheckAssetDocumentEditorInstancedObject(*Instanced, Value, Base, Depth, Path, Check);
	}
	if (const FObjectProperty* EditInline = AsAssetDocumentEditorEditInlineObject(Property))
	{
		// The instanced form edits or creates the object; a path string still names an object, as before.
		return Value.Kind == EKind::Object
			? CheckAssetDocumentEditorEditInlineObject(*EditInline, Value, Base, Depth, Path, Check)
			: CheckAssetDocumentEditorReference(Property, Value, Base, Path, Check);
	}
	if (AsAssetDocumentEditorHardReference(Property) != nullptr || CastField<FInterfaceProperty>(&Property) != nullptr)
	{
		return CheckAssetDocumentEditorReference(Property, Value, Base, Path, Check);
	}
	if (IsAssetDocumentEditorInstancedStruct(Property))
	{
		return CheckAssetDocumentEditorInstancedStruct(Property, Value, Base, Depth, Path, Check);
	}
	if (IsAssetDocumentEditorTextFormStruct(Property))
	{
		return Value.Kind == EKind::String
			|| FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
				TEXT("expected the %s text form as a string, got %s."),
				*CastFieldChecked<FStructProperty>(&Property)->Struct->GetName(), DescribeAssetDocumentEditorKind(Value.Kind)));
	}
	FString ScalarCodecId;
	if (TryGetAssetDocumentEditorScalarCodecId(Property, ScalarCodecId))
	{
		if (CastField<FNumericProperty>(&Property) != nullptr && Value.Kind == EKind::Object
			&& FindAssetDocumentEditorTypedField(Value, TEXT("Unsupported")) != nullptr)
		{
			// The marker of a number with no JSON form (NaN, an infinity): kept, never written.
			return CheckAssetDocumentEditorMarkedMember(Property, Value, Base, Path, Check);
		}
		if (!IsAssetDocumentEditorScalarValueCompatible(Property, Value))
		{
			return FailAssetDocumentEditorCheck(Check, Path, FString::Printf(
				TEXT("expected %s, got %s."),
				DescribeAssetDocumentEditorKind(GetAssetDocumentEditorScalarExpectedKind(Property)),
				DescribeAssetDocumentEditorKind(Value.Kind)));
		}
		return CheckAssetDocumentEditorEnumName(Property, Value, Path, Check);
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		return CheckAssetDocumentEditorStruct(*Struct, Value, Base, Depth, Path, Check);
	}
	return CheckAssetDocumentEditorContainer(Property, Value, Base, Depth, Path, Check);
}

bool IsAssetDocumentEditorValueCompatible(
	FProperty& Property,
	const FValue& Value,
	bool bIgnoreStaticArray,
	int32 Depth)
{
	FAssetDocumentEditorValueCheck Check;
	Check.Mode = EAssetDocumentEditorCheckMode::Encode;
	return IsAssetDocumentEditorValueChecked(Property, Value, nullptr, bIgnoreStaticArray, Depth, FString(), Check);
}

// ---------------------------------------------------------------------------
// Text form
// ---------------------------------------------------------------------------

FString EncodeAssetDocumentEditorText(const FText& Text)
{
	if (Text.IsEmpty())
	{
		return FString();
	}
	FString Complex;
	if (Text.IsFromStringTable() || Text.IsCultureInvariant())
	{
		FTextStringHelper::WriteToBuffer(Complex, Text);
		UE::AssetDocument::TextDisplays::Record(Complex, Text.ToString());
		return Complex;
	}
	const FString* Source = FTextInspector::GetSourceString(Text);
	const FString Plain = Source != nullptr ? *Source : Text.ToString();
	if (FTextStringHelper::IsComplexText(*Plain))
	{
		FTextStringHelper::WriteToBuffer(Complex, Text);
		UE::AssetDocument::TextDisplays::Record(Complex, Text.ToString());
		return Complex;
	}
	return Plain;
}

bool DecodeAssetDocumentEditorText(const FString& String, FText& OutText)
{
	if (String.IsEmpty())
	{
		OutText = FText::GetEmpty();
		return true;
	}
	if (FTextStringHelper::IsComplexText(*String))
	{
		return FTextStringHelper::ReadFromBuffer(*String, OutText) != nullptr;
	}
	// A plain string is localizable source text, as typed into the Details panel.
	OutText = FText::FromString(String);
	return true;
}

// ---------------------------------------------------------------------------
// Export (native -> JSON)
// ---------------------------------------------------------------------------

TSharedPtr<FJsonValue> ExportAssetDocumentEditorCustom(FProperty* Property, const void* Value);

TSharedPtr<FJsonValue> ExportAssetDocumentEditorJson(FProperty& Property, const void* Value)
{
	const FJsonObjectConverter::CustomExportCallback ExportCallback =
		FJsonObjectConverter::CustomExportCallback::CreateStatic(&ExportAssetDocumentEditorCustom);
	return FJsonObjectConverter::UPropertyToJsonValue(
		&Property, Value, 0, 0, &ExportCallback, nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
}

bool AreAssetDocumentEditorJsonValuesEqual(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
{
	return Left.IsValid() && Right.IsValid() && FJsonValue::CompareEqual(*Left, *Right);
}

TSharedPtr<FJsonValue> ExportAssetDocumentEditorInstancedObject(UObject* Object)
{
	if (Object == nullptr)
	{
		return MakeShared<FJsonValueNull>();
	}
	UClass* Class = Object->GetClass();
	const UObject* Defaults = Class->GetDefaultObject();
	TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		if (!IsAssetDocumentEditorSubobjectField(**It, 0))
		{
			continue;
		}
		TSharedPtr<FJsonValue> Current = ExportAssetDocumentEditorJson(**It, It->ContainerPtrToValuePtr<void>(Object));
		if (!Current.IsValid())
		{
			return nullptr;
		}
		if (Defaults != nullptr && Defaults != Object)
		{
			const TSharedPtr<FJsonValue> Default =
				ExportAssetDocumentEditorJson(**It, It->ContainerPtrToValuePtr<void>(Defaults));
			if (AreAssetDocumentEditorJsonValuesEqual(Current, Default))
			{
				continue;
			}
		}
		Properties->SetField(It->GetAuthoredName(), Current);
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("Class"), Class->GetPathName());
	Out->SetObjectField(TEXT("Properties"), Properties);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> ExportAssetDocumentEditorInstancedStruct(const FInstancedStruct& Value)
{
	const UScriptStruct* Struct = Value.GetScriptStruct();
	if (Struct == nullptr || Value.GetMemory() == nullptr)
	{
		return MakeShared<FJsonValueNull>();
	}
	FStructOnScope Defaults(Struct);
	TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		if (!IsAssetDocumentEditorStructCodecField(**It))
		{
			continue;
		}
		TSharedPtr<FJsonValue> Current = ExportAssetDocumentEditorJson(**It, It->ContainerPtrToValuePtr<void>(Value.GetMemory()));
		if (!Current.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonValue> Default =
			ExportAssetDocumentEditorJson(**It, It->ContainerPtrToValuePtr<void>(Defaults.GetStructMemory()));
		if (!AreAssetDocumentEditorJsonValuesEqual(Current, Default))
		{
			Properties->SetField(It->GetAuthoredName(), Current);
		}
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("Struct"), Struct->GetPathName());
	Out->SetObjectField(TEXT("Properties"), Properties);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> ExportAssetDocumentEditorObjectPath(const UObject* Object)
{
	if (Object == nullptr)
	{
		return MakeShared<FJsonValueNull>();
	}
	return MakeShared<FJsonValueString>(Object->GetPathName());
}

// The marker of a value the codec has no form for: the reason and the engine's export text (read only).
TSharedPtr<FJsonValue> ExportAssetDocumentEditorMarker(const FProperty& Property, const void* Value)
{
	FString Text;
	Property.ExportText_Direct(Text, Value, nullptr, nullptr, PPF_None);
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("Unsupported"), FString::Printf(TEXT("%s has no document form; AssetDoc keeps the value."),
		*Property.GetCPPType()));
	Out->SetStringField(TEXT("Text"), Text);
	return MakeShared<FJsonValueObject>(Out);
}

bool HasAssetDocumentEditorOptionalMember(const UStruct& Struct)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (CastField<FOptionalProperty>(*It) != nullptr && IsAssetDocumentEditorStructCodecField(**It))
		{
			return true;
		}
	}
	return false;
}

// Every codec member of a struct, each in its document form.
TSharedPtr<FJsonValue> ExportAssetDocumentEditorStructMembers(const UStruct& Struct, const void* Value)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (!IsAssetDocumentEditorStructCodecField(**It))
		{
			continue;
		}
		const TSharedPtr<FJsonValue> Member = ExportAssetDocumentEditorJson(**It, It->ContainerPtrToValuePtr<void>(Value));
		if (!Member.IsValid())
		{
			return nullptr;
		}
		Out->SetField(It->GetAuthoredName(), Member);
	}
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> ExportAssetDocumentEditorCustom(FProperty* Property, const void* Value)
{
	if (Property == nullptr || Value == nullptr)
	{
		return nullptr;
	}
	if (IsAssetDocumentEditorStructMapKey(*Property) && IsAssetDocumentEditorMapKeyProperty(*Property))
	{
		return MakeShared<FJsonValueString>(ExportAssetDocumentEditorStructMapKey(*Property, Value));
	}
	// A struct member (or one element of a fixed-size member) of a type the codec has no form for.
	if (!IsAssetDocumentEditorPropertyAdmitted(*Property, true, 1))
	{
		return ExportAssetDocumentEditorMarker(*Property, Value);
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		if (!Numeric->IsEnum() && Numeric->IsInteger())
		{
			return MakeShared<FJsonValueNumberString>(
				LexToString(Numeric->GetSignedIntPropertyValue(Value)));
		}
		// A stored float extracts as the shortest decimal that reads back as it (0.3,
		// not 0.30000001192092896); doubles keep their exact value. NaN and infinities have no JSON form.
		if (const FFloatProperty* Float = CastField<FFloatProperty>(Numeric))
		{
			const float Stored = Float->GetPropertyValue(Value);
			if (FMath::IsFinite(Stored))
			{
				return MakeShared<FJsonValueNumber>(UE::AssetDocument::EditorFloatPrecision::ShortestFloatDecimal(Stored));
			}
			return ExportAssetDocumentEditorMarker(*Property, Value);
		}
		if (const FDoubleProperty* Double = CastField<FDoubleProperty>(Numeric); Double != nullptr
			&& !FMath::IsFinite(Double->GetPropertyValue(Value)))
		{
			return ExportAssetDocumentEditorMarker(*Property, Value);
		}
		return nullptr;
	}
	if (const FTextProperty* Text = CastField<FTextProperty>(Property))
	{
		return MakeShared<FJsonValueString>(EncodeAssetDocumentEditorText(Text->GetPropertyValue(Value)));
	}
	if (const FObjectProperty* Instanced = AsAssetDocumentEditorInstancedObject(*Property))
	{
		return ExportAssetDocumentEditorInstancedObject(Instanced->GetObjectPropertyValue(Value));
	}
	if (const FObjectProperty* EditInline = AsAssetDocumentEditorEditInlineObject(*Property))
	{
		UObject* Object = EditInline->GetObjectPropertyValue(Value);
		return IsAssetDocumentEditorEditInlineSubobject(Object)
			? ExportAssetDocumentEditorInstancedObject(Object)
			: ExportAssetDocumentEditorObjectPath(Object);
	}
	if (const FObjectPropertyBase* Hard = AsAssetDocumentEditorHardReference(*Property))
	{
		return ExportAssetDocumentEditorObjectPath(Hard->GetObjectPropertyValue(Value));
	}
	if (CastField<FInterfaceProperty>(Property) != nullptr)
	{
		return ExportAssetDocumentEditorObjectPath(static_cast<const FScriptInterface*>(Value)->GetObject());
	}
	if (IsAssetDocumentEditorInstancedStruct(*Property))
	{
		return ExportAssetDocumentEditorInstancedStruct(*static_cast<const FInstancedStruct*>(Value));
	}
	if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
	{
		const void* Inner = Optional->GetValuePointerForReadIfSet(Value);
		return Inner != nullptr ? ExportAssetDocumentEditorJson(*Optional->GetValueProperty(), Inner) : MakeShared<FJsonValueNull>();
	}
	if (IsAssetDocumentEditorPropertyBag(*Property))
	{
		return ExportAssetDocumentEditorStructMembers(*FInstancedPropertyBag::StaticStruct(), Value);
	}
	// The engine's JSON export leaves out an unset optional member; it reads as null.
	if (const FStructProperty* Struct = CastField<FStructProperty>(Property); Struct != nullptr
		&& !IsAssetDocumentEditorTextFormStruct(*Property) && !IsAssetDocumentEditorInstancedStruct(*Property)
		&& HasAssetDocumentEditorOptionalMember(*Struct->Struct))
	{
		return ExportAssetDocumentEditorStructMembers(*Struct->Struct, Value);
	}
	return nullptr;
}

FProperty* FindAssetDocumentEditorStructCodecField(const UStruct& Struct, FStringView Name)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (IsAssetDocumentEditorStructCodecField(**It) && It->GetAuthoredName() == Name)
		{
			return *It;
		}
	}
	return nullptr;
}

// The export of Value in the shape of Authored (see
// EncodeAssetDocumentEditorTypedValueAsAuthored).
TSharedPtr<FJsonValue> ExportAssetDocumentEditorJsonAsAuthored(
	FProperty& Property,
	const void* Value,
	const TSharedPtr<FJsonValue>& Authored,
	bool bIgnoreStaticArray,
	int32 Depth)
{
	if (!Authored.IsValid() || Depth > AssetDocumentEditorTypedValueMaximumDepth
		|| HasAssetDocumentEditorStaticArrayShape(Property, bIgnoreStaticArray) || Property.ArrayDim > 1
		|| !IsAssetDocumentEditorPropertyAdmitted(Property, true, Depth))
	{
		return ExportAssetDocumentEditorJson(Property, Value);
	}
	const TSharedPtr<FJsonObject>* AuthoredObject = nullptr;
	const bool bAuthoredObject = Authored->TryGetObject(AuthoredObject) && AuthoredObject != nullptr && AuthoredObject->IsValid();
	auto ExportMembers = [Depth](
		const TSharedPtr<FJsonObject>& AuthoredMembers,
		TFunctionRef<FProperty*(const FString&)> FindMember,
		const void* Container) -> TSharedPtr<FJsonObject>
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const auto& Entry : AuthoredMembers->Values)
		{
			FProperty* Member = FindMember(FString(*Entry.Key));
			if (Member == nullptr)
			{
				continue;
			}
			const TSharedPtr<FJsonValue> Exported = ExportAssetDocumentEditorJsonAsAuthored(
				*Member, Member->ContainerPtrToValuePtr<void>(Container), Entry.Value, false, Depth + 1);
			if (!Exported.IsValid())
			{
				return nullptr;
			}
			Out->SetField(Entry.Key, Exported);
		}
		return Out;
	};
	const FObjectProperty* EditInline = AsAssetDocumentEditorEditInlineObject(Property);
	if (const FObjectProperty* Instanced = EditInline != nullptr
		? (IsAssetDocumentEditorEditInlineSubobject(EditInline->GetObjectPropertyValue(Value)) ? EditInline : nullptr)
		: AsAssetDocumentEditorInstancedObject(Property))
	{
		UObject* Object = Instanced->GetObjectPropertyValue(Value);
		FString ClassPath;
		if (Object == nullptr || !bAuthoredObject || !(*AuthoredObject)->TryGetStringField(TEXT("Class"), ClassPath)
			|| ClassPath != Object->GetClass()->GetPathName())
		{
			return ExportAssetDocumentEditorJson(Property, Value);
		}
		UClass* Class = Object->GetClass();
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("Class"), ClassPath);
		const TSharedPtr<FJsonObject>* AuthoredProperties = nullptr;
		if ((*AuthoredObject)->TryGetObjectField(TEXT("Properties"), AuthoredProperties))
		{
			const TSharedPtr<FJsonObject> Properties = ExportMembers(*AuthoredProperties,
				[Class, Depth](const FString& Name) { return FindAssetDocumentEditorSubobjectField(*Class, Name, Depth); }, Object);
			if (!Properties.IsValid())
			{
				return nullptr;
			}
			Out->SetObjectField(TEXT("Properties"), Properties);
		}
		return MakeShared<FJsonValueObject>(Out);
	}
	if (IsAssetDocumentEditorInstancedStruct(Property))
	{
		const FInstancedStruct& Instance = *static_cast<const FInstancedStruct*>(Value);
		const UScriptStruct* Struct = Instance.GetScriptStruct();
		FString StructPath;
		if (Struct == nullptr || Instance.GetMemory() == nullptr || !bAuthoredObject
			|| !(*AuthoredObject)->TryGetStringField(TEXT("Struct"), StructPath) || StructPath != Struct->GetPathName())
		{
			return ExportAssetDocumentEditorJson(Property, Value);
		}
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("Struct"), StructPath);
		const TSharedPtr<FJsonObject>* AuthoredProperties = nullptr;
		if ((*AuthoredObject)->TryGetObjectField(TEXT("Properties"), AuthoredProperties))
		{
			const TSharedPtr<FJsonObject> Properties = ExportMembers(*AuthoredProperties,
				[Struct, Depth](const FString& Name) { return FindAssetDocumentEditorInstancedStructField(*Struct, Name, Depth); },
				Instance.GetMemory());
			if (!Properties.IsValid())
			{
				return nullptr;
			}
			Out->SetObjectField(TEXT("Properties"), Properties);
		}
		return MakeShared<FJsonValueObject>(Out);
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property);
		Struct != nullptr && !IsAssetDocumentEditorTextFormStruct(Property))
	{
		if (!bAuthoredObject)
		{
			return ExportAssetDocumentEditorJson(Property, Value);
		}
		const TSharedPtr<FJsonObject> Members = ExportMembers(*AuthoredObject,
			[Struct](const FString& Name) { return FindAssetDocumentEditorStructCodecField(*Struct->Struct, Name); }, Value);
		if (!Members.IsValid())
		{
			return nullptr;
		}
		return MakeShared<FJsonValueObject>(Members);
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		const TArray<TSharedPtr<FJsonValue>>* AuthoredElements = nullptr;
		FScriptArrayHelper Helper(Array, Value);
		if (!Authored->TryGetArray(AuthoredElements))
		{
			return ExportAssetDocumentEditorJson(Property, Value);
		}
		// The elements the authored array shares with the live one by index are read in the authored shape; live
		// elements past its end are read in full (the authored array removes them).
		TArray<TSharedPtr<FJsonValue>> Elements;
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			const TSharedPtr<FJsonValue> Exported = Index < AuthoredElements->Num()
				? ExportAssetDocumentEditorJsonAsAuthored(*Array->Inner, Helper.GetRawPtr(Index), (*AuthoredElements)[Index], false, Depth + 1)
				: ExportAssetDocumentEditorJson(*Array->Inner, Helper.GetRawPtr(Index));
			if (!Exported.IsValid())
			{
				return nullptr;
			}
			Elements.Add(Exported);
		}
		return MakeShared<FJsonValueArray>(Elements);
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		const TSharedPtr<FJsonValue> Full = ExportAssetDocumentEditorJson(Property, Value);
		const TSharedPtr<FJsonObject>* FullObject = nullptr;
		if (!bAuthoredObject || !Full.IsValid() || !Full->TryGetObject(FullObject))
		{
			return Full;
		}
		for (auto& Entry : (*FullObject)->Values)
		{
			const TSharedPtr<FJsonValue>* AuthoredEntry = (*AuthoredObject)->Values.Find(Entry.Key);
			const void* EntryValue = AuthoredEntry != nullptr ? FindAssetDocumentEditorMapBaseValue(*Map, Value, FString(*Entry.Key)) : nullptr;
			if (EntryValue != nullptr)
			{
				Entry.Value = ExportAssetDocumentEditorJsonAsAuthored(*Map->ValueProp, EntryValue, *AuthoredEntry, false, Depth + 1);
				if (!Entry.Value.IsValid())
				{
					return nullptr;
				}
			}
		}
		return Full;
	}
	return ExportAssetDocumentEditorJson(Property, Value);
}

// ---------------------------------------------------------------------------
// Import (JSON -> native)
// ---------------------------------------------------------------------------

struct FAssetDocumentEditorImportContext
{
	FAssetDocumentEditorScopedPropertyValue* Value = nullptr;
	UObject* Outer = nullptr;
	// The memory being imported into holds the current value (an edit): an
	// instanced subobject of the same class owned by BaseOwner is the starting
	// point of its staged replacement.
	bool bPartial = false;
	UObject* BaseOwner = nullptr;
	// The package whose text ids an edited FText keeps (nullptr: none known).
	UPackage* TextPackage = nullptr;
	bool bFailed = false;
};

bool ImportAssetDocumentEditorJson(
	const TSharedPtr<FJsonValue>& Json,
	FProperty& Property,
	void* Memory,
	FAssetDocumentEditorImportContext& Context);

bool ImportAssetDocumentEditorFields(
	const TSharedPtr<FJsonObject>& Properties,
	TFunctionRef<FProperty*(const FString&)> FindField,
	void* Container,
	FAssetDocumentEditorImportContext& Context)
{
	if (!Properties.IsValid())
	{
		return true;
	}
	for (const auto& Entry : Properties->Values)
	{
		FProperty* Field = FindField(FString(*Entry.Key));
		if (Field == nullptr
			|| !ImportAssetDocumentEditorJson(Entry.Value, *Field, Field->ContainerPtrToValuePtr<void>(Container), Context))
		{
			return false;
		}
	}
	return true;
}

bool ImportAssetDocumentEditorInstancedObject(
	const TSharedPtr<FJsonValue>& Json,
	const FObjectProperty& Property,
	void* Out,
	FAssetDocumentEditorImportContext& Context)
{
	if (Json->Type == EJson::Null)
	{
		Property.SetObjectPropertyValue(Out, nullptr);
		return true;
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	FString ClassPath;
	if (!Json->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("Class"), ClassPath))
	{
		return false;
	}
	UClass* Class = Cast<UClass>(ResolveAssetDocumentEditorObjectPath(ClassPath, true));
	// The current subobject (an edit): the staged object starts from its field
	// values, so omitted Properties keep them; another class starts from the CDO.
	UObject* const Existing = Context.bPartial ? Property.GetObjectPropertyValue(Out) : nullptr;
	const bool bFromExisting = IsValid(Existing) && Existing->GetClass() == Class
		&& (Context.BaseOwner == nullptr || Existing->GetOuter() == Context.BaseOwner);
	if (Class == nullptr || !Class->IsChildOf(Property.PropertyClass)
		|| (!bFromExisting && Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)))
	{
		return false;
	}
	UObject* Outer = Context.Outer != nullptr ? Context.Outer : GetTransientPackage();
	UObject* Created = NewObject<UObject>(Outer, Class, NAME_None, RF_NoFlags);
	Context.Value->AddStagedObject(*Created);
	Property.SetObjectPropertyValue(Out, Created);
	if (bFromExisting)
	{
		// The fields a document writes, and the saved fields it has no form for (not editable, read-only, no codec):
		// a replacement keeps those too. A field holding subobjects of the current object is not shared with the
		// replacement; it starts from the class default.
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			if (IsAssetDocumentEditorSubobjectField(**It, 0)
				|| (!It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) && !DoesAssetDocumentEditorPropertyHoldInstancedObjects(**It, true, true)))
			{
				It->CopyCompleteValue_InContainer(Created, Existing);
			}
		}
	}
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (!(*Object)->TryGetObjectField(TEXT("Properties"), Properties))
	{
		return true;
	}
	FAssetDocumentEditorImportContext Nested;
	Nested.Value = Context.Value;
	Nested.Outer = Created;
	Nested.bPartial = true;
	Nested.BaseOwner = bFromExisting ? Existing : Created;
	Nested.TextPackage = Context.TextPackage;
	const bool bImported = ImportAssetDocumentEditorFields(*Properties,
		[Class](const FString& Name) { return FindAssetDocumentEditorSubobjectField(*Class, Name, 0); },
		Created, Nested);
	return bImported && !Nested.bFailed;
}

bool ImportAssetDocumentEditorInstancedStruct(
	const TSharedPtr<FJsonValue>& Json,
	void* Out,
	FAssetDocumentEditorImportContext& Context)
{
	FInstancedStruct& Value = *static_cast<FInstancedStruct*>(Out);
	if (Json->Type == EJson::Null)
	{
		Value.Reset();
		return true;
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	FString StructPath;
	if (!Json->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("Struct"), StructPath))
	{
		return false;
	}
	const UScriptStruct* Struct = Cast<UScriptStruct>(ResolveAssetDocumentEditorObjectPath(StructPath, true));
	if (Struct == nullptr)
	{
		return false;
	}
	// The same struct keeps its current members (omitted ones stay); another
	// struct starts from its defaults.
	if (!Context.bPartial || Value.GetScriptStruct() != Struct || Value.GetMemory() == nullptr)
	{
		Value.InitializeAs(Struct);
	}
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (!(*Object)->TryGetObjectField(TEXT("Properties"), Properties))
	{
		return true;
	}
	FAssetDocumentEditorImportContext Nested = Context;
	Nested.bPartial = true;
	const bool bImported = ImportAssetDocumentEditorFields(*Properties,
		[Struct](const FString& Name) { return FindAssetDocumentEditorInstancedStructField(*Struct, Name, 0); },
		Value.GetMutableMemory(), Nested);
	Context.bFailed |= Nested.bFailed;
	return bImported;
}

bool ImportAssetDocumentEditorReference(
	const TSharedPtr<FJsonValue>& Json,
	FProperty& Property,
	void* Out)
{
	UObject* Object = nullptr;
	if (Json->Type != EJson::Null)
	{
		FString Path;
		if (!Json->TryGetString(Path))
		{
			return false;
		}
		Object = ResolveAssetDocumentEditorObjectPath(Path, true);
		if (Object == nullptr)
		{
			return false;
		}
	}
	if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(&Property))
	{
		FScriptInterface& Value = *static_cast<FScriptInterface*>(Out);
		Value.SetObject(Object);
		Value.SetInterface(Object != nullptr ? Object->GetInterfaceAddress(Interface->InterfaceClass) : nullptr);
		return true;
	}
	CastFieldChecked<FObjectPropertyBase>(&Property)->SetObjectPropertyValue(Out, Object);
	return true;
}

// Maps import entry by entry: the engine's JSON map import drops entries whose
// value is null (a map of references keeps them; the Details panel shows
// "None"), and an edited map keeps the current value of each kept key so a
// partial struct or instanced value edits it.
bool ImportAssetDocumentEditorMap(
	const TSharedPtr<FJsonValue>& Json,
	const FMapProperty& Map,
	void* Out,
	FAssetDocumentEditorImportContext& Context)
{
	if (Json->Type != EJson::Object)
	{
		return false;
	}
	FScriptMapHelper Helper(&Map, Out);
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> Current;
	if (Context.bPartial && Helper.Num() > 0)
	{
		Current = MakeUnique<FAssetDocumentEditorScopedPropertyValue>(const_cast<FMapProperty&>(Map));
		Map.CopyCompleteValue(Current->GetMemory(), Out);
	}
	Helper.EmptyValues(Json->AsObject()->Values.Num());
	bool bImported = true;
	for (const auto& Entry : Json->AsObject()->Values)
	{
		const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
		if (IsAssetDocumentEditorStructMapKey(*Map.KeyProp)
			? !ImportAssetDocumentEditorStructMapKey(Map, Current.IsValid() ? Current->GetMemory() : nullptr, FString(*Entry.Key), Helper.GetKeyPtr(Index))
			: !ImportAssetDocumentEditorJson(MakeShared<FJsonValueString>(FString(*Entry.Key)), *Map.KeyProp, Helper.GetKeyPtr(Index), Context))
		{
			bImported = false;
			break;
		}
		if (!Entry.Value.IsValid() || Entry.Value->IsNull())
		{
			continue;
		}
		if (Current.IsValid())
		{
			FScriptMapHelper CurrentHelper(&Map, Current->GetMemory());
			const int32 CurrentIndex = CurrentHelper.FindMapIndexWithKey(Helper.GetKeyPtr(Index));
			if (CurrentIndex != INDEX_NONE)
			{
				Map.ValueProp->CopyCompleteValue(Helper.GetValuePtr(Index), CurrentHelper.GetValuePtr(CurrentIndex));
			}
		}
		if (!ImportAssetDocumentEditorJson(Entry.Value, *Map.ValueProp, Helper.GetValuePtr(Index), Context))
		{
			bImported = false;
			break;
		}
	}
	Helper.Rehash();
	return bImported;
}

// The id TextNamespaceUtil::GetTextIdForEdit gives a text that has no key yet: the
// package's namespace and a new random key. GetTextIdForEdit also serializes the
// whole package to look for another text already using that key; a new GUID key
// cannot be in use, and that scan per text made writing many texts into one package
// (every row of a large table) quadratic in the package size.
void MakeAssetDocumentEditorNewTextId(UPackage& Package, const FString& ProposedNamespace, FString& OutNamespace, FString& OutKey)
{
#if USE_STABLE_LOCALIZATION_KEYS
	if (const FString PackageNamespace = TextNamespaceUtil::EnsurePackageNamespace(&Package); !PackageNamespace.IsEmpty())
	{
		const FString FullNamespace = TextNamespaceUtil::BuildFullNamespace(ProposedNamespace, PackageNamespace, /*bAlwaysApplyPackageNamespace*/ true);
		OutNamespace = ProposedNamespace.Equals(FullNamespace, ESearchCase::CaseSensitive)
			? FullNamespace
			: TextNamespaceUtil::BuildFullNamespace(FString(), PackageNamespace, /*bAlwaysApplyPackageNamespace*/ true);
	}
#endif
	OutKey = TextNamespaceUtil::GenerateRandomTextKey();
}

// An FText source string edited like the Details text box does
// (STextPropertyEditableTextBox::OnTextCommitted): an unchanged string keeps the
// text; a changed one keeps its namespace and key when it is the only text using
// them (TextNamespaceUtil::GetTextIdForEdit), else gets a new key. The string a
// String Table or culture-invariant text displays (what documents held before such
// texts were read in their text form) also keeps that text.
bool ImportAssetDocumentEditorText(
	const FString& String,
	FText& InOutText,
	const FAssetDocumentEditorImportContext& Context)
{
	if (FTextStringHelper::IsComplexText(*String) || !Context.bPartial)
	{
		return DecodeAssetDocumentEditorText(String, InOutText);
	}
	const FText Current = InOutText;
	if (!Current.IsEmpty() && Current.ToString().Equals(String, ESearchCase::CaseSensitive))
	{
		return true;
	}
	if (String.IsEmpty())
	{
		InOutText = FText::GetEmpty();
		return true;
	}
	const FString* Source = FTextInspector::GetSourceString(Current);
	const bool bLocalizable = !Current.IsEmpty() && !Current.IsCultureInvariant() && !Current.IsFromStringTable();
	const FString CurrentNamespace = bLocalizable ? FTextInspector::GetNamespace(Current).Get(FString()) : FString();
	const FString CurrentKey = bLocalizable ? FTextInspector::GetKey(Current).Get(FString()) : FString();
	FString Namespace;
	FString Key;
	if (Context.TextPackage != nullptr && CurrentKey.IsEmpty())
	{
		MakeAssetDocumentEditorNewTextId(*Context.TextPackage, CurrentNamespace, Namespace, Key);
	}
	else if (Context.TextPackage != nullptr)
	{
		TextNamespaceUtil::GetTextIdForEdit(Context.TextPackage, TextNamespaceUtil::ETextEditAction::SourceString,
			Source != nullptr ? *Source : FString(), CurrentNamespace, CurrentKey, Namespace, Key);
	}
	else if (!CurrentKey.IsEmpty())
	{
		Namespace = CurrentNamespace;
		Key = CurrentKey;
	}
	else
	{
		InOutText = FText::FromString(String);
		return true;
	}
	InOutText = FText::ChangeKey(Namespace, Key, FText::FromString(String));
	return true;
}

// An array whose elements are matched by identity (IsAssetDocumentEditorArrayMatchedByIdentity): each element is
// imported over a copy of the current element it continues, or over the default element, never over the memory of
// whichever element happened to stand at its index.
bool ImportAssetDocumentEditorMatchedArray(
	const TArray<TSharedPtr<FJsonValue>>& Elements,
	FArrayProperty& Array,
	void* Out,
	FAssetDocumentEditorImportContext& Context)
{
	FAssetDocumentEditorScopedPropertyValue DefaultElement(*Array.Inner);
	FAssetDocumentEditorScopedPropertyValue Staged(Array);
	TArray<int32> Match;
	int32 FailedIndex = INDEX_NONE;
	EAssetDocumentEditorArrayMatchFailure Failure = EAssetDocumentEditorArrayMatchFailure::None;
	if (!DefaultElement.IsValid() || !Staged.IsValid()
		|| !MatchAssetDocumentEditorArrayElements(Array, Elements, Out, DefaultElement.GetMemory(), Context.BaseOwner, Match, FailedIndex, Failure))
	{
		return false;
	}
	FScriptArrayHelper Current(&Array, Out);
	FScriptArrayHelper Result(&Array, Staged.GetMemory());
	Result.AddValues(Elements.Num());
	for (int32 Index = 0; Index < Elements.Num(); ++Index)
	{
		Array.Inner->CopySingleValue(Result.GetRawPtr(Index),
			Match[Index] != INDEX_NONE ? Current.GetRawPtr(Match[Index]) : DefaultElement.GetMemory());
	}
	for (int32 Index = 0; Index < Elements.Num(); ++Index)
	{
		if (!ImportAssetDocumentEditorJson(Elements[Index], *Array.Inner, Result.GetRawPtr(Index), Context))
		{
			return false;
		}
	}
	Array.CopySingleValue(Out, Staged.GetMemory());
	return true;
}

bool ImportAssetDocumentEditorCustom(
	const TSharedPtr<FJsonValue>& Json,
	FProperty* Property,
	void* Out,
	FAssetDocumentEditorImportContext& Context)
{
	if (Property == nullptr || Out == nullptr || !Json.IsValid())
	{
		return false;
	}
	if (!IsAssetDocumentEditorPropertyAdmitted(*Property, true, 1)
		|| (CastField<FNumericProperty>(Property) != nullptr && Json->Type == EJson::Object)
		|| IsAssetDocumentEditorFormerMarkerJson(*Property, Json))
	{
		// A marked value: the value check matched it to the current value, which the memory already holds.
		return true;
	}
	bool bHandled = true;
	bool bImported = false;
	if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
	{
		if (Json->IsNull())
		{
			Optional->MarkUnset(Out);
			bImported = true;
		}
		else
		{
			// A set optional is edited in place (a struct keeps the members the value omits); an unset one starts from
			// the value type's default.
			void* Inner = Optional->IsSet(Out)
				? Optional->GetValuePointerForReplace(Out) : Optional->MarkSetAndGetInitializedValuePointerToReplace(Out);
			bImported = ImportAssetDocumentEditorJson(Json, *Optional->GetValueProperty(), Inner, Context);
		}
	}
	else if (const FTextProperty* Text = CastField<FTextProperty>(Property))
	{
		FText Decoded = Text->GetPropertyValue(Out);
		FString String;
		bImported = Json->TryGetString(String) && ImportAssetDocumentEditorText(String, Decoded, Context);
		if (bImported)
		{
			Text->SetPropertyValue(Out, Decoded);
		}
	}
	else if (const FObjectProperty* Instanced = AsAssetDocumentEditorInstancedObject(*Property))
	{
		bImported = ImportAssetDocumentEditorInstancedObject(Json, *Instanced, Out, Context);
	}
	else if (const FObjectProperty* EditInline = AsAssetDocumentEditorEditInlineObject(*Property))
	{
		bImported = Json->Type == EJson::Object
			? ImportAssetDocumentEditorInstancedObject(Json, *EditInline, Out, Context)
			: ImportAssetDocumentEditorReference(Json, *Property, Out);
	}
	else if (AsAssetDocumentEditorHardReference(*Property) != nullptr || CastField<FInterfaceProperty>(Property) != nullptr)
	{
		bImported = ImportAssetDocumentEditorReference(Json, *Property, Out);
	}
	else if (IsAssetDocumentEditorInstancedStruct(*Property))
	{
		bImported = ImportAssetDocumentEditorInstancedStruct(Json, Out, Context);
	}
	else if (IsAssetDocumentEditorPropertyBag(*Property))
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		bImported = Json->TryGetObject(Object) && ImportAssetDocumentEditorFields(*Object,
			[](const FString& Name) { return FindAssetDocumentEditorStructCodecField(*FInstancedPropertyBag::StaticStruct(), Name); },
			Out, Context);
	}
	else if (const FMapProperty* Map = CastField<FMapProperty>(Property))
	{
		bImported = ImportAssetDocumentEditorMap(Json, *Map, Out, Context);
	}
	else if (FArrayProperty* Array = CastField<FArrayProperty>(Property); Array != nullptr && Json->Type == EJson::Array
		&& IsAssetDocumentEditorArrayMatchedByIdentity(*Array, Json->AsArray()))
	{
		bImported = ImportAssetDocumentEditorMatchedArray(Json->AsArray(), *Array, Out, Context);
	}
	else
	{
		bHandled = false;
	}
	if (bHandled && !bImported)
	{
		Context.bFailed = true;
	}
	return bHandled;
}

// The engine's JSON import skips null struct members and array elements, which
// would keep the value already in Memory (the current value of an edit, or a
// member default). null is a value (None) for the kinds that take it, so those
// are cleared first; the value check rejected null everywhere else.
void ClearAssetDocumentEditorNullValues(const TSharedPtr<FJsonValue>& Json, FProperty& Property, void* Memory,
	bool bIgnoreStaticArray, int32 Depth)
{
	if (!Json.IsValid() || Memory == nullptr || Depth > AssetDocumentEditorTypedValueMaximumDepth)
	{
		return;
	}
	if (!bIgnoreStaticArray && Property.ArrayDim > 1)
	{
		const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
		if (Json->TryGetArray(Elements))
		{
			for (int32 Index = 0; Index < FMath::Min(Elements->Num(), Property.ArrayDim); ++Index)
			{
				ClearAssetDocumentEditorNullValues((*Elements)[Index], Property,
					static_cast<uint8*>(Memory) + Index * Property.GetElementSize(), true, Depth + 1);
			}
		}
		return;
	}
	if (Json->IsNull())
	{
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property);
			Object != nullptr && CastField<FSoftObjectProperty>(&Property) == nullptr)
		{
			Object->SetObjectPropertyValue(Memory, nullptr);
		}
		else if (CastField<FInterfaceProperty>(&Property) != nullptr)
		{
			*static_cast<FScriptInterface*>(Memory) = FScriptInterface();
		}
		else if (IsAssetDocumentEditorInstancedStruct(Property))
		{
			static_cast<FInstancedStruct*>(Memory)->Reset();
		}
		else if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(&Property))
		{
			Optional->MarkUnset(Memory);
		}
		return;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
		// Elements matched by identity are cleared one by one as they are imported (ImportAssetDocumentEditorMatchedArray),
		// not at their index here.
		if (Json->TryGetArray(Elements) && Elements != nullptr && !IsAssetDocumentEditorArrayMatchedByIdentity(*Array, *Elements))
		{
			FScriptArrayHelper Helper(Array, Memory);
			for (int32 Index = 0; Index < FMath::Min(Elements->Num(), Helper.Num()); ++Index)
			{
				ClearAssetDocumentEditorNullValues((*Elements)[Index], *Array->Inner, Helper.GetRawPtr(Index), true, Depth + 1);
			}
		}
		return;
	}
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	const TSharedPtr<FJsonObject>* Members = nullptr;
	if (Struct != nullptr && !IsAssetDocumentEditorTextFormStruct(Property) && !IsAssetDocumentEditorInstancedStruct(Property)
		&& Json->TryGetObject(Members) && Members != nullptr && Members->IsValid())
	{
		for (const auto& Entry : (*Members)->Values)
		{
			if (FProperty* Member = FindAssetDocumentEditorStructCodecField(*Struct->Struct, Entry.Key))
			{
				ClearAssetDocumentEditorNullValues(Entry.Value, *Member, Member->ContainerPtrToValuePtr<void>(Memory), false, Depth + 1);
			}
		}
	}
}

bool ImportAssetDocumentEditorJson(
	const TSharedPtr<FJsonValue>& Json,
	FProperty& Property,
	void* Memory,
	FAssetDocumentEditorImportContext& Context)
{
	ClearAssetDocumentEditorNullValues(Json, Property, Memory, false, 0);
	const FJsonObjectConverter::CustomImportCallback ImportCallback =
		FJsonObjectConverter::CustomImportCallback::CreateLambda(
			[&Context](const TSharedPtr<FJsonValue>& Value, FProperty* Field, void* Out)
			{
				return ImportAssetDocumentEditorCustom(Value, Field, Out, Context);
			});
	FText FailureReason;
	// Same member set as the export (which skips transient and deprecated struct
	// members by default). Not strict: the value check already required every
	// member where there is no current value, and an omitted member of an edit
	// keeps the value already in Memory.
	return FJsonObjectConverter::JsonValueToUProperty(
			Json, &Property, Memory, 0, CPF_Deprecated | CPF_Transient, false, &FailureReason, &ImportCallback)
		&& !Context.bFailed;
}

const void* FindAssetDocumentEditorMapBaseValue(const FMapProperty& Map, const void* Base, const FString& Key)
{
	if (Base == nullptr || FScriptMapHelper(&Map, Base).Num() == 0)
	{
		return nullptr;
	}
	if (IsAssetDocumentEditorStructMapKey(*Map.KeyProp))
	{
		const int32 StructKeyIndex = FindAssetDocumentEditorStructMapKeyIndex(Map, Base, Key);
		return StructKeyIndex != INDEX_NONE ? FScriptMapHelper(&Map, Base).GetValuePtr(StructKeyIndex) : nullptr;
	}
	FAssetDocumentEditorScopedPropertyValue KeyValue(*Map.KeyProp);
	FAssetDocumentEditorImportContext Context;
	Context.Value = &KeyValue;
	if (!KeyValue.IsValid()
		|| !ImportAssetDocumentEditorJson(MakeShared<FJsonValueString>(Key), *Map.KeyProp, KeyValue.GetMemory(), Context))
	{
		return nullptr;
	}
	FScriptMapHelper Helper(&Map, Base);
	const int32 Index = Helper.FindMapIndexWithKey(KeyValue.GetMemory());
	return Index != INDEX_NONE ? Helper.GetValuePtr(Index) : nullptr;
}

bool TryConvertAssetDocumentEditorEncodedValue(
	FProperty& Property,
	const TSharedPtr<FJsonValue>& JsonValue,
	FValue& OutValue)
{
	FString DiagnosticCode;
	return JsonValue.IsValid()
		&& TryConvertAssetDocumentEditorJsonToValue(
			JsonValue, OutValue, DiagnosticCode)
		&& IsAssetDocumentEditorValueCompatible(
			Property, OutValue, false, 0);
}

// ---------------------------------------------------------------------------
// Instanced value adoption
// ---------------------------------------------------------------------------

void ForEachAssetDocumentEditorInstancedObject(
	FProperty& Property,
	const void* Value,
	bool bIgnoreStaticArray,
	int32 Depth,
	TFunctionRef<void(UObject*)> Visit)
{
	if (Value == nullptr || Depth > AssetDocumentEditorTypedValueMaximumDepth
		|| !DoesAssetDocumentEditorPropertyHoldInstancedObjects(Property))
	{
		return;
	}
	if (!bIgnoreStaticArray && Property.ArrayDim > 1)
	{
		for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
		{
			ForEachAssetDocumentEditorInstancedObject(Property,
				static_cast<const uint8*>(Value) + Index * Property.GetElementSize(), true, Depth + 1, Visit);
		}
		return;
	}
	if (const FObjectProperty* Instanced = AsAssetDocumentEditorInstancedObject(Property) != nullptr
		? AsAssetDocumentEditorInstancedObject(Property) : AsAssetDocumentEditorEditInlineObject(Property))
	{
		if (UObject* Object = Instanced->GetObjectPropertyValue(Value))
		{
			Visit(Object);
		}
		return;
	}
	if (IsAssetDocumentEditorInstancedStruct(Property))
	{
		const FInstancedStruct& Struct = *static_cast<const FInstancedStruct*>(Value);
		if (Struct.GetScriptStruct() != nullptr && Struct.GetMemory() != nullptr)
		{
			for (TFieldIterator<FProperty> It(Struct.GetScriptStruct()); It; ++It)
			{
				ForEachAssetDocumentEditorInstancedObject(**It, It->ContainerPtrToValuePtr<void>(Struct.GetMemory()), false, Depth + 1, Visit);
			}
		}
		return;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			ForEachAssetDocumentEditorInstancedObject(**It, It->ContainerPtrToValuePtr<void>(Value), false, Depth + 1, Visit);
		}
		return;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(Array, Value);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			ForEachAssetDocumentEditorInstancedObject(*Array->Inner, Helper.GetRawPtr(Index), false, Depth + 1, Visit);
		}
		return;
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		FScriptSetHelper Helper(Set, Value);
		for (FScriptSetHelper::FIterator It(Helper); It; ++It)
		{
			ForEachAssetDocumentEditorInstancedObject(*Set->ElementProp, Helper.GetElementPtr(It), false, Depth + 1, Visit);
		}
		return;
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		FScriptMapHelper Helper(Map, Value);
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			ForEachAssetDocumentEditorInstancedObject(*Map->KeyProp, Helper.GetKeyPtr(It), false, Depth + 1, Visit);
			ForEachAssetDocumentEditorInstancedObject(*Map->ValueProp, Helper.GetValuePtr(It), false, Depth + 1, Visit);
		}
	}
}

void ApplyAssetDocumentEditorSubobjectFlags(UObject& Object, UObject& Owner)
{
	EObjectFlags Flags = Owner.GetMaskedFlags(RF_PropagateToSubObjects);
	if (Owner.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		Flags |= RF_ArchetypeObject;
	}
	Object.SetFlags(Flags);
}

void AdoptAssetDocumentEditorStagedObject(
	UObject& Staged,
	UObject& Owner,
	const FAssetDocumentEditorScopedPropertyValue& Desired)
{
	if (Staged.GetOuter() != &Owner)
	{
		Staged.Rename(nullptr, &Owner, REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
	}
	ApplyAssetDocumentEditorSubobjectFlags(Staged, Owner);
	TArray<UObject*> Inner;
	GetObjectsWithOuter(&Staged, Inner);
	for (UObject* Object : Inner)
	{
		if (Desired.IsStagedObject(Object))
		{
			ApplyAssetDocumentEditorSubobjectFlags(*Object, *Object->GetOuter());
		}
	}
}

void ReconcileAssetDocumentEditorInstancedValue(
	UObject& Owner,
	FProperty& Property,
	void* Desired,
	const void* Current,
	const FAssetDocumentEditorScopedPropertyValue& Staged,
	bool bNotify,
	bool bIgnoreStaticArray,
	int32 Depth);

bool AreAssetDocumentEditorValuesIdentical(FProperty& Property, const void* Left, const void* Right)
{
	for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
	{
		const int32 Offset = Index * Property.GetElementSize();
		if (!Property.Identical(static_cast<const uint8*>(Left) + Offset, static_cast<const uint8*>(Right) + Offset, PPF_None))
		{
			return false;
		}
	}
	return true;
}

// Copies the desired state of a staged subobject into an existing subobject of
// the same class, field by field, the way the Details panel edits a nested field.
void CopyAssetDocumentEditorStagedIntoExisting(
	UObject& Existing,
	UObject& Staged,
	const FAssetDocumentEditorScopedPropertyValue& Desired,
	bool bNotify,
	int32 Depth)
{
	for (TFieldIterator<FProperty> It(Existing.GetClass()); It; ++It)
	{
		FProperty& Field = **It;
		if (!IsAssetDocumentEditorSubobjectField(Field, Depth))
		{
			continue;
		}
		FAssetDocumentEditorScopedPropertyValue Value(Field);
		if (!Value.IsValid())
		{
			continue;
		}
		Field.CopyCompleteValue(Value.GetMemory(), Field.ContainerPtrToValuePtr<void>(&Staged));
		void* Target = Field.ContainerPtrToValuePtr<void>(&Existing);
		ReconcileAssetDocumentEditorInstancedValue(Existing, Field, Value.GetMemory(), Target, Desired, bNotify, false, Depth + 1);
		if (AreAssetDocumentEditorValuesIdentical(Field, Target, Value.GetMemory()))
		{
			continue;
		}
		FAssetDocumentEditorScopedPropertyValue Before(Field);
		Field.CopyCompleteValue(Before.GetMemory(), Target);
		PropertyAccessUtil::SetPropertyValue_Object(&Field, &Existing, &Field, Value.GetMemory(), INDEX_NONE,
			PropertyAccessUtil::EditorReadOnlyFlags,
			bNotify ? EPropertyAccessChangeNotifyMode::Default : EPropertyAccessChangeNotifyMode::Never);
		ReleaseAssetDocumentEditorReplacedInstancedObjects(Existing, Field, Before.GetMemory(), Target);
	}
}

void ReconcileAssetDocumentEditorInstancedValue(
	UObject& Owner,
	FProperty& Property,
	void* Desired,
	const void* Current,
	const FAssetDocumentEditorScopedPropertyValue& Staged,
	bool bNotify,
	bool bIgnoreStaticArray,
	int32 Depth)
{
	if (Desired == nullptr || Depth > AssetDocumentEditorTypedValueMaximumDepth
		|| !DoesAssetDocumentEditorPropertyHoldInstancedObjects(Property))
	{
		return;
	}
	if (!bIgnoreStaticArray && Property.ArrayDim > 1)
	{
		for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
		{
			const int32 Offset = Index * Property.GetElementSize();
			ReconcileAssetDocumentEditorInstancedValue(Owner, Property, static_cast<uint8*>(Desired) + Offset,
				Current != nullptr ? static_cast<const uint8*>(Current) + Offset : nullptr, Staged, bNotify, true, Depth + 1);
		}
		return;
	}
	if (const FObjectProperty* Instanced = AsAssetDocumentEditorInstancedObject(Property) != nullptr
		? AsAssetDocumentEditorInstancedObject(Property) : AsAssetDocumentEditorEditInlineObject(Property))
	{
		UObject* Object = Instanced->GetObjectPropertyValue(Desired);
		if (Object == nullptr || !Staged.IsStagedObject(Object))
		{
			return;
		}
		UObject* Existing = Current != nullptr ? Instanced->GetObjectPropertyValue(Current) : nullptr;
		if (Existing != nullptr && Existing != Object && Existing->GetClass() == Object->GetClass()
			&& Existing->GetOuter() == &Owner && !Staged.IsStagedObject(Existing))
		{
			CopyAssetDocumentEditorStagedIntoExisting(*Existing, *Object, Staged, bNotify, Depth + 1);
			Instanced->SetObjectPropertyValue(Desired, Existing);
			return;
		}
		AdoptAssetDocumentEditorStagedObject(*Object, Owner, Staged);
		return;
	}
	if (IsAssetDocumentEditorInstancedStruct(Property))
	{
		FInstancedStruct& Value = *static_cast<FInstancedStruct*>(Desired);
		const FInstancedStruct* Before = static_cast<const FInstancedStruct*>(Current);
		if (Value.GetScriptStruct() == nullptr || Value.GetMutableMemory() == nullptr)
		{
			return;
		}
		const bool bSameStruct = Before != nullptr && Before->GetScriptStruct() == Value.GetScriptStruct()
			&& Before->GetMemory() != nullptr;
		for (TFieldIterator<FProperty> It(Value.GetScriptStruct()); It; ++It)
		{
			ReconcileAssetDocumentEditorInstancedValue(Owner, **It, It->ContainerPtrToValuePtr<void>(Value.GetMutableMemory()),
				bSameStruct ? It->ContainerPtrToValuePtr<void>(Before->GetMemory()) : nullptr, Staged, bNotify, false, Depth + 1);
		}
		return;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			ReconcileAssetDocumentEditorInstancedValue(Owner, **It, It->ContainerPtrToValuePtr<void>(Desired),
				Current != nullptr ? It->ContainerPtrToValuePtr<void>(Current) : nullptr, Staged, bNotify, false, Depth + 1);
		}
		return;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper DesiredHelper(Array, Desired);
		const int32 CurrentNum = Current != nullptr ? FScriptArrayHelper(Array, Current).Num() : 0;
		for (int32 Index = 0; Index < DesiredHelper.Num(); ++Index)
		{
			const void* CurrentElement = Index < CurrentNum
				? FScriptArrayHelper(Array, Current).GetRawPtr(Index)
				: nullptr;
			ReconcileAssetDocumentEditorInstancedValue(Owner, *Array->Inner, DesiredHelper.GetRawPtr(Index),
				CurrentElement, Staged, bNotify, false, Depth + 1);
		}
		return;
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		FScriptMapHelper DesiredHelper(Map, Desired);
		for (FScriptMapHelper::FIterator It(DesiredHelper); It; ++It)
		{
			const void* CurrentValue = nullptr;
			if (Current != nullptr)
			{
				FScriptMapHelper CurrentHelper(Map, Current);
				for (FScriptMapHelper::FIterator CurrentIt(CurrentHelper); CurrentIt; ++CurrentIt)
				{
					if (Map->KeyProp->Identical(CurrentHelper.GetKeyPtr(CurrentIt), DesiredHelper.GetKeyPtr(It), PPF_None))
					{
						CurrentValue = CurrentHelper.GetValuePtr(CurrentIt);
						break;
					}
				}
			}
			ReconcileAssetDocumentEditorInstancedValue(Owner, *Map->ValueProp, DesiredHelper.GetValuePtr(It),
				CurrentValue, Staged, bNotify, false, Depth + 1);
		}
		return;
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		FScriptSetHelper DesiredHelper(Set, Desired);
		for (FScriptSetHelper::FIterator It(DesiredHelper); It; ++It)
		{
			ReconcileAssetDocumentEditorInstancedValue(Owner, *Set->ElementProp, DesiredHelper.GetElementPtr(It),
				nullptr, Staged, bNotify, false, Depth + 1);
		}
	}
}

bool DoesAssetDocumentEditorPropertyHoldInstancedObjectsImpl(const FProperty& Property, int32 Depth, bool bIncludeInstancedStructs,
	bool bIncludeEditInline)
{
	if (Depth > AssetDocumentEditorTypedValueMaximumDepth)
	{
		return false;
	}
	if (AsAssetDocumentEditorInstancedObject(Property) != nullptr
		|| (bIncludeEditInline && AsAssetDocumentEditorEditInlineObject(Property) != nullptr)
		|| (bIncludeInstancedStructs && IsAssetDocumentEditorInstancedStruct(Property)))
	{
		return true;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (IsAssetDocumentEditorTextFormStruct(Property))
		{
			return false;
		}
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			if (DoesAssetDocumentEditorPropertyHoldInstancedObjectsImpl(**It, Depth + 1, bIncludeInstancedStructs, bIncludeEditInline))
			{
				return true;
			}
		}
		return false;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		return DoesAssetDocumentEditorPropertyHoldInstancedObjectsImpl(*Array->Inner, Depth + 1, bIncludeInstancedStructs, bIncludeEditInline);
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		return DoesAssetDocumentEditorPropertyHoldInstancedObjectsImpl(*Set->ElementProp, Depth + 1, bIncludeInstancedStructs, bIncludeEditInline);
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		return DoesAssetDocumentEditorPropertyHoldInstancedObjectsImpl(*Map->KeyProp, Depth + 1, bIncludeInstancedStructs, bIncludeEditInline)
			|| DoesAssetDocumentEditorPropertyHoldInstancedObjectsImpl(*Map->ValueProp, Depth + 1, bIncludeInstancedStructs, bIncludeEditInline);
	}
	return false;
}

// Editor-derived references (see FindAssetDocumentEditorDerivedReference).
// The Details panel shows an edit-inline node (the referenced object's own fields)
// for EditInline / ShowInnerProperties metadata on the property or on its owning
// container (PropertyNode.cpp), so such a reference is edited through the object,
// not picked as an asset.
bool IsAssetDocumentEditorEditInlineReference(const FProperty& Property)
{
	if (Property.HasMetaData(TEXT("NoEditInline")))
	{
		return false;
	}
	if (Property.HasMetaData(TEXT("EditInline")) || Property.HasMetaData(TEXT("ShowInnerProperties")))
	{
		return true;
	}
	const FProperty* Owner = Property.GetOwner<FProperty>();
	return Owner != nullptr
		&& (CastField<FArrayProperty>(Owner) || CastField<FSetProperty>(Owner) || CastField<FMapProperty>(Owner))
		&& Owner->HasMetaData(TEXT("EditInline"));
}

bool IsAssetDocumentEditorObjectInsideAsset(const UObject* Object, const UObject& Asset)
{
	return Object != nullptr && Object != &Asset && Object->IsIn(&Asset);
}

// Visits every editor-derived reference in Value with its JSON pointer; Visit
// returns true to stop. Returns true when stopped.
bool VisitAssetDocumentEditorDerivedReferences(
	const FProperty& Property,
	const void* Value,
	const UObject& Asset,
	bool bIgnoreStaticArray,
	bool bIntoInstanced,
	int32 Depth,
	const FString& Path,
	TFunctionRef<bool(const UObject&, const FString&)> Visit)
{
	if (Value == nullptr || Depth > AssetDocumentEditorTypedValueMaximumDepth)
	{
		return false;
	}
	if (!bIgnoreStaticArray && Property.ArrayDim > 1)
	{
		for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
		{
			if (VisitAssetDocumentEditorDerivedReferences(Property, static_cast<const uint8*>(Value) + Index * Property.GetElementSize(),
				Asset, true, bIntoInstanced, Depth + 1, AppendAssetDocumentEditorPath(Path, Index), Visit))
			{
				return true;
			}
		}
		return false;
	}
	const auto VisitFields = [&](const UStruct& Struct, const void* Container, bool bSubobjectFields)
	{
		for (TFieldIterator<FProperty> It(&Struct); It; ++It)
		{
			if (bSubobjectFields ? !IsAssetDocumentEditorSubobjectField(**It, Depth) : !IsAssetDocumentEditorStructCodecField(**It))
			{
				continue;
			}
			if (VisitAssetDocumentEditorDerivedReferences(**It, It->ContainerPtrToValuePtr<void>(Container), Asset, false,
				bIntoInstanced, Depth + 1, AppendAssetDocumentEditorPath(Path, It->GetAuthoredName()), Visit))
			{
				return true;
			}
		}
		return false;
	};
	if (const FObjectProperty* Instanced = AsAssetDocumentEditorInstancedObject(Property))
	{
		const UObject* Object = Instanced->GetObjectPropertyValue(Value);
		return bIntoInstanced && Object != nullptr && VisitFields(*Object->GetClass(), Object, true);
	}
	if (IsAssetDocumentEditorInstancedStruct(Property))
	{
		const FInstancedStruct& Struct = *static_cast<const FInstancedStruct*>(Value);
		return bIntoInstanced && Struct.GetScriptStruct() != nullptr && Struct.GetMemory() != nullptr
			&& VisitFields(*Struct.GetScriptStruct(), Struct.GetMemory(), false);
	}
	if (AsAssetDocumentEditorHardReference(Property) != nullptr || CastField<FInterfaceProperty>(&Property) != nullptr)
	{
		if (IsAssetDocumentEditorEditInlineReference(Property))
		{
			return false;
		}
		const UObject* Object = GetAssetDocumentEditorReferenceValue(Property, Value);
		return IsAssetDocumentEditorObjectInsideAsset(Object, Asset) && Visit(*Object, Path);
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		return !IsAssetDocumentEditorTextFormStruct(Property) && VisitFields(*Struct->Struct, Value, false);
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(Array, Value);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			if (VisitAssetDocumentEditorDerivedReferences(*Array->Inner, Helper.GetRawPtr(Index), Asset, false,
				bIntoInstanced, Depth + 1, AppendAssetDocumentEditorPath(Path, Index), Visit))
			{
				return true;
			}
		}
		return false;
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		FScriptSetHelper Helper(Set, Value);
		int32 Index = 0;
		for (FScriptSetHelper::FIterator It(Helper); It; ++It, ++Index)
		{
			if (VisitAssetDocumentEditorDerivedReferences(*Set->ElementProp, Helper.GetElementPtr(It), Asset, false,
				bIntoInstanced, Depth + 1, AppendAssetDocumentEditorPath(Path, Index), Visit))
			{
				return true;
			}
		}
		return false;
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		FScriptMapHelper Helper(Map, Value);
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			FString Key;
			Map->KeyProp->ExportTextItem_Direct(Key, Helper.GetKeyPtr(It), nullptr, nullptr, PPF_None);
			const FString EntryPath = AppendAssetDocumentEditorPath(Path, Key);
			if (VisitAssetDocumentEditorDerivedReferences(*Map->KeyProp, Helper.GetKeyPtr(It), Asset, false,
					bIntoInstanced, Depth + 1, EntryPath, Visit)
				|| VisitAssetDocumentEditorDerivedReferences(*Map->ValueProp, Helper.GetValuePtr(It), Asset, false,
					bIntoInstanced, Depth + 1, EntryPath, Visit))
			{
				return true;
			}
		}
	}
	return false;
}
} // namespace

FAssetDocumentEditorScopedPropertyValue::FAssetDocumentEditorScopedPropertyValue(
	FProperty& InProperty)
	: Property(&InProperty)
	, Memory(InProperty.AllocateAndInitializeValue())
{
}

FAssetDocumentEditorScopedPropertyValue::~FAssetDocumentEditorScopedPropertyValue()
{
	if (Property != nullptr && Memory != nullptr)
	{
		Property->DestroyAndFreeValue(Memory);
		Memory = nullptr;
	}
}

FProperty& FAssetDocumentEditorScopedPropertyValue::GetProperty() const
{
	check(Property != nullptr);
	return *Property;
}

void* FAssetDocumentEditorScopedPropertyValue::GetMemory() const
{
	return View != nullptr ? const_cast<void*>(View) : Memory;
}

void FAssetDocumentEditorScopedPropertyValue::ReadFrom(const void* Source)
{
	View = nullptr;
	if (!IsValid() || Source == nullptr)
	{
		return;
	}
	Property->CopyCompleteValue(Memory, Source);
	for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
	{
		const int32 Offset = Index * Property->GetElementSize();
		if (!Property->Identical(
			static_cast<const uint8*>(Memory) + Offset,
			static_cast<const uint8*>(Source) + Offset,
			PPF_None))
		{
			View = Source;
			return;
		}
	}
}

bool ReadAssetDocumentEditorObjectPropertyValue(
	const UObject& Object,
	FProperty& Property,
	FAssetDocumentEditorScopedPropertyValue& OutValue)
{
	if (!OutValue.IsValid() || &OutValue.GetProperty() != &Property
		|| !Object.IsA(Property.GetOwnerClass())
		|| PropertyAccessUtil::CanGetPropertyValue(&Property) != EPropertyAccessResultFlags::Success)
	{
		return false;
	}
	OutValue.ReadFrom(Property.ContainerPtrToValuePtr<void>(&Object));
	return true;
}

bool FAssetDocumentEditorScopedPropertyValue::IsValid() const
{
	return Property != nullptr && Memory != nullptr;
}

void FAssetDocumentEditorScopedPropertyValue::AddStagedObject(UObject& Object)
{
	StagedObjects.Emplace(&Object);
}

bool FAssetDocumentEditorScopedPropertyValue::IsStagedObject(const UObject* Object) const
{
	return Object != nullptr && StagedObjects.ContainsByPredicate(
		[Object](const TStrongObjectPtr<UObject>& Staged) { return Staged.Get() == Object; });
}

bool FAssetDocumentEditorScopedPropertyValue::HasStagedObjects() const
{
	return !StagedObjects.IsEmpty();
}

FAssetDocumentEditorTypedValueCodecResult
InspectAssetDocumentEditorTypedValueProperty(FProperty& Property)
{
	if (!IsAssetDocumentEditorPropertyAdmitted(Property, false, 0))
	{
		return MakeAssetDocumentEditorTypedValueFailure(
			TEXT("TypedValueCodec.UnsupportedProperty"));
	}
	return MakeAssetDocumentEditorTypedValueSuccess(
		GetAssetDocumentEditorCodecId(Property));
}

FAssetDocumentEditorTypedValueCodecResult
DecodeAssetDocumentEditorTypedValue(
	FProperty& Property,
	const FValue& Value,
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue>& OutNativeValue)
{
	return DecodeAssetDocumentEditorTypedValue(Property, Value, OutNativeValue, FAssetDocumentEditorDecodeBase());
}

FAssetDocumentEditorTypedValueCodecResult
DecodeAssetDocumentEditorTypedValue(
	FProperty& Property,
	const FValue& Value,
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue>& OutNativeValue,
	const FAssetDocumentEditorDecodeBase& Base)
{
	OutNativeValue.Reset();
	const FAssetDocumentEditorTypedValueCodecResult Admission =
		InspectAssetDocumentEditorTypedValueProperty(Property);
	if (!Admission.bSucceeded)
	{
		FAssetDocumentEditorTypedValueCodecResult Failure =
			MakeAssetDocumentEditorTypedValueFailure(TEXT("TypedValueCodec.ValueTypeMismatch"));
		Failure.Message = TEXT("the property type has no supported document form.");
		return Failure;
	}
	FAssetDocumentEditorValueCheck Check;
	Check.Mode = EAssetDocumentEditorCheckMode::Decode;
	Check.bPartial = Base.Value != nullptr;
	Check.Owner = Base.Owner;
	if (!IsAssetDocumentEditorValueChecked(Property, Value, Base.Value, false, 0, FString(), Check))
	{
		FAssetDocumentEditorTypedValueCodecResult Failure =
			MakeAssetDocumentEditorTypedValueFailure(TEXT("TypedValueCodec.ValueTypeMismatch"));
		Failure.ValuePath = Check.Path;
		Failure.Message = Check.Message;
		return Failure;
	}
	TSharedPtr<FJsonValue> JsonValue;
	FString DiagnosticCode;
	if (!TryConvertAssetDocumentEditorValueToJson(
		Value, JsonValue, DiagnosticCode))
	{
		FAssetDocumentEditorTypedValueCodecResult Failure =
			MakeAssetDocumentEditorTypedValueFailure(TEXT("TypedValueCodec.InvalidValue"));
		Failure.Message = TEXT("the value is not exact JSON.");
		return Failure;
	}
	OutNativeValue = MakeUnique<FAssetDocumentEditorScopedPropertyValue>(Property);
	FAssetDocumentEditorImportContext Context;
	Context.Value = OutNativeValue.Get();
	if (Base.Value != nullptr && OutNativeValue->IsValid())
	{
		// The edit starts from the current value (omitted = keep).
		Property.CopyCompleteValue(OutNativeValue->GetMemory(), Base.Value);
		Context.bPartial = true;
		Context.BaseOwner = Base.Owner;
	}
	Context.TextPackage = Base.Owner != nullptr ? Base.Owner->GetPackage() : nullptr;
	if (!OutNativeValue->IsValid()
		|| !ImportAssetDocumentEditorJson(JsonValue, Property, OutNativeValue->GetMemory(), Context))
	{
		OutNativeValue.Reset();
		FAssetDocumentEditorTypedValueCodecResult Failure =
			MakeAssetDocumentEditorTypedValueFailure(TEXT("TypedValueCodec.ConversionFailed"));
		Failure.Message = TEXT("the value has the right shape but Unreal could not import it (check enum names and text forms).");
		return Failure;
	}
	return Admission;
}

FAssetDocumentEditorTypedValueCodecResult
EncodeAssetDocumentEditorTypedValue(
	const FAssetDocumentEditorScopedPropertyValue& NativeValue,
	FValue& OutValue)
{
	OutValue = {};
	if (!NativeValue.IsValid())
	{
		return MakeAssetDocumentEditorTypedValueFailure(
			TEXT("TypedValueCodec.InvalidNativeValue"));
	}
	FProperty& Property = NativeValue.GetProperty();
	const FAssetDocumentEditorTypedValueCodecResult Admission =
		InspectAssetDocumentEditorTypedValueProperty(Property);
	if (!Admission.bSucceeded)
	{
		return MakeAssetDocumentEditorTypedValueFailure(
			TEXT("TypedValueCodec.InvalidNativeValue"));
	}
	const TSharedPtr<FJsonValue> JsonValue =
		ExportAssetDocumentEditorJson(Property, NativeValue.GetMemory());
	if (!TryConvertAssetDocumentEditorEncodedValue(
		Property, JsonValue, OutValue))
	{
		OutValue = {};
		return MakeAssetDocumentEditorTypedValueFailure(
			TEXT("TypedValueCodec.ConversionFailed"));
	}
	return Admission;
}

FAssetDocumentEditorTypedValueCodecResult
EncodeAssetDocumentEditorTypedValueAsAuthored(
	const FAssetDocumentEditorScopedPropertyValue& NativeValue,
	const FValue& Authored,
	FValue& OutValue)
{
	OutValue = {};
	if (!NativeValue.IsValid())
	{
		return MakeAssetDocumentEditorTypedValueFailure(TEXT("TypedValueCodec.InvalidNativeValue"));
	}
	FProperty& Property = NativeValue.GetProperty();
	const FAssetDocumentEditorTypedValueCodecResult Admission = InspectAssetDocumentEditorTypedValueProperty(Property);
	if (!Admission.bSucceeded)
	{
		return MakeAssetDocumentEditorTypedValueFailure(TEXT("TypedValueCodec.InvalidNativeValue"));
	}
	TSharedPtr<FJsonValue> AuthoredJson;
	FString DiagnosticCode;
	if (!TryConvertAssetDocumentEditorValueToJson(Authored, AuthoredJson, DiagnosticCode))
	{
		AuthoredJson.Reset();
	}
	const TSharedPtr<FJsonValue> JsonValue = ExportAssetDocumentEditorJsonAsAuthored(
		Property, NativeValue.GetMemory(), AuthoredJson, false, 0);
	if (!JsonValue.IsValid() || !TryConvertAssetDocumentEditorJsonToValue(JsonValue, OutValue, DiagnosticCode))
	{
		OutValue = {};
		return MakeAssetDocumentEditorTypedValueFailure(TEXT("TypedValueCodec.ConversionFailed"));
	}
	return Admission;
}

bool AreAssetDocumentEditorTypedValuesEquivalent(
	FProperty& Property,
	const void* Left,
	const void* Right)
{
	if (Left == nullptr || Right == nullptr)
	{
		return false;
	}
	if (Property.Identical(Left, Right, PPF_None))
	{
		return true;
	}
	return AreAssetDocumentEditorJsonValuesEqual(
		ExportAssetDocumentEditorJson(Property, Left), ExportAssetDocumentEditorJson(Property, Right));
}

bool DoesAssetDocumentEditorPropertyHoldInstancedObjects(const FProperty& Property, bool bIncludeInstancedStructs, bool bIncludeEditInline)
{
	return DoesAssetDocumentEditorPropertyHoldInstancedObjectsImpl(Property, 0, bIncludeInstancedStructs, bIncludeEditInline);
}

const UObject* FindAssetDocumentEditorDerivedReference(
	const FProperty& Property,
	const void* Value,
	const UObject& Owner,
	FString& OutValuePath,
	bool bIntoInstanced,
	const void* KeptValue)
{
	OutValuePath.Reset();
	const UObject* Asset = Owner.GetOutermostObject();
	if (Asset == nullptr)
	{
		return nullptr;
	}
	TSet<const UObject*> Kept;
	if (KeptValue != nullptr)
	{
		VisitAssetDocumentEditorDerivedReferences(Property, KeptValue, *Asset, false, bIntoInstanced, 0, FString(),
			[&Kept](const UObject& Object, const FString&) { Kept.Add(&Object); return false; });
	}
	const UObject* Found = nullptr;
	VisitAssetDocumentEditorDerivedReferences(Property, Value, *Asset, false, bIntoInstanced, 0, FString(),
		[&](const UObject& Object, const FString& Path)
		{
			if (Kept.Contains(&Object))
			{
				return false;
			}
			Found = &Object;
			OutValuePath = Path;
			return true;
		});
	return Found;
}

bool IsAssetDocumentEditorInPlaceInstancedProperty(const FProperty& Property)
{
	return Property.HasAllPropertyFlags(CPF_Edit | CPF_EditConst) && Property.ArrayDim == 1
		&& AsAssetDocumentEditorInstancedObject(Property) != nullptr;
}

void AdoptAssetDocumentEditorInstancedValue(
	UObject& Owner,
	const FAssetDocumentEditorScopedPropertyValue& Desired,
	const void* CurrentValue,
	bool bNotify)
{
	if (!Desired.IsValid() || !Desired.HasStagedObjects())
	{
		return;
	}
	ReconcileAssetDocumentEditorInstancedValue(Owner, Desired.GetProperty(), Desired.GetMemory(),
		CurrentValue, Desired, bNotify, false, 0);
}

void ReleaseAssetDocumentEditorReplacedInstancedObjects(
	UObject& Owner,
	FProperty& Property,
	const void* BeforeValue,
	const void* AfterValue)
{
	if (!DoesAssetDocumentEditorPropertyHoldInstancedObjects(Property))
	{
		return;
	}
	TSet<UObject*> Before;
	ForEachAssetDocumentEditorInstancedObject(Property, BeforeValue, false, 0,
		[&Before, &Owner](UObject* Object) { if (Object->GetOuter() == &Owner) Before.Add(Object); });
	if (Before.IsEmpty())
	{
		return;
	}
	TSet<UObject*> After;
	ForEachAssetDocumentEditorInstancedObject(Property, AfterValue, false, 0,
		[&After](UObject* Object) { After.Add(Object); });
	for (UObject* Object : Before)
	{
		if (!After.Contains(Object) && IsValid(Object))
		{
			// As SPropertyEditorEditInline::OnClassPicked does for a replaced subobject.
			Object->Rename(nullptr, GetTransientOuterForRename(Object->GetClass()),
				REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
		}
	}
}

namespace UE::AssetDocument::EditorPropertyValueCodec
{
FString EncodeText(const FText& Text)
{
	return EncodeAssetDocumentEditorText(Text);
}

bool DecodeText(const FString& Value, FText& InOutText, UObject* Owner)
{
	FAssetDocumentEditorImportContext Context;
	Context.bPartial = true;
	Context.TextPackage = Owner != nullptr ? Owner->GetPackage() : nullptr;
	FText Decoded = InOutText;
	if (!ImportAssetDocumentEditorText(Value, Decoded, Context))
	{
		return false;
	}
	InOutText = Decoded;
	return true;
}

bool IsSameText(const FString& Value, const FText& Text)
{
	return Value.Equals(EncodeAssetDocumentEditorText(Text), ESearchCase::CaseSensitive)
		|| (!FTextStringHelper::IsComplexText(*Value) && !Text.IsEmpty()
			&& Value.Equals(Text.ToString(), ESearchCase::CaseSensitive));
}
}
