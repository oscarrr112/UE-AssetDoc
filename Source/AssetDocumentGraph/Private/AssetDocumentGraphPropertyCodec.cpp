// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentGraphPropertyCodec.h"
#include "AssetDocumentEditorPropertyValueCodec.h"

#include "UObject/EnumProperty.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph
{
namespace
{
FValue ExportAsText(const FProperty& Property, const void* ValuePtr)
{
	FString Text;
	Property.ExportTextItem_Direct(Text, ValuePtr, nullptr, nullptr, PPF_None);
	return MakeString(MoveTemp(Text));
}

bool ImportFromText(const FProperty& Property, void* ValuePtr, const FValue& Value, FString& OutError)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::String)
	{
		OutError = FString::Printf(TEXT("%s expects UE text export syntax as a string."), *Property.GetName());
		return false;
	}
	if (Property.ImportText_Direct(*Value.StringValue, ValuePtr, nullptr, PPF_None) == nullptr)
	{
		OutError = FString::Printf(TEXT("'%s' is not valid text for %s."), *Value.StringValue, *Property.GetName());
		return false;
	}
	return true;
}

UObject* ResolveObjectPath(const FString& Path, UClass* RequiredClass)
{
	UObject* Object = FindObject<UObject>(nullptr, *Path);
	if (Object == nullptr)
	{
		Object = FSoftObjectPath(Path).TryLoad();
	}
	return Object != nullptr && (RequiredClass == nullptr || Object->IsA(RequiredClass)) ? Object : nullptr;
}

bool ReadNumber(const FValue& Value, double& Out)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
	{
		Out = static_cast<double>(Value.IntegerValue);
		return true;
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Number)
	{
		Out = Value.NumberValue;
		return true;
	}
	return false;
}

FValue EncodeEnumValue(const UEnum* Enum, int64 Raw)
{
	if (Enum == nullptr)
	{
		return MakeInt(Raw);
	}
	// The authored name: a User Defined Enum's value reads as its display name (as DataTable rows and
	// the editor show it); a native enum's is its enumerator name.
	const FString Name = Enum->GetIndexByValue(Raw) != INDEX_NONE ? Enum->GetAuthoredNameStringByValue(Raw) : FString();
	return Name.IsEmpty() ? MakeInt(Raw) : MakeString(Name);
}

bool DecodeEnumValue(const UEnum* Enum, const FValue& Value, int64& OutRaw, FString& OutError)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
	{
		OutRaw = Value.IntegerValue;
		return true;
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::String && Enum != nullptr)
	{
		// The enumerator's name, or the authored / display name the editor shows (a User Defined Enum's "Ground").
		const int32 Index = EditorPropertyValueCodec::FindEnumIndexBySpelling(*Enum, Value.StringValue);
		if (Index != INDEX_NONE)
		{
			OutRaw = Enum->GetValueByIndex(Index);
			return true;
		}
		OutError = FString::Printf(TEXT("'%s' is not a %s value. Valid: %s."), *Value.StringValue,
			*Enum->GetName(), *EditorPropertyValueCodec::DescribeEnumSpellings(*Enum));
		return false;
	}
	OutError = TEXT("Enum values are enumerator name strings.");
	return false;
}
} // namespace

bool IsPropertyEditable(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_Edit)
		&& !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated);
}

FValue EncodePropertyValue(const FProperty& Property, const void* ValuePtr)
{
	if (Property.ArrayDim > 1)
	{
		return ExportAsText(Property, ValuePtr);
	}
	if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
	{
		return MakeBool(Bool->GetPropertyValue(ValuePtr));
	}
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(&Property))
	{
		return EncodeEnumValue(EnumProp->GetEnum(),
			EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr));
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
	{
		if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
		{
			return EncodeEnumValue(Enum, Numeric->GetSignedIntPropertyValue(ValuePtr));
		}
		if (Numeric->IsFloatingPoint())
		{
			return MakeNumber(Numeric->GetFloatingPointPropertyValue(ValuePtr));
		}
		return MakeInt(Numeric->GetSignedIntPropertyValue(ValuePtr));
	}
	if (const FStrProperty* Str = CastField<FStrProperty>(&Property))
	{
		return MakeString(Str->GetPropertyValue(ValuePtr));
	}
	if (const FNameProperty* Name = CastField<FNameProperty>(&Property))
	{
		return MakeString(Name->GetPropertyValue(ValuePtr).ToString());
	}
	if (const FTextProperty* Text = CastField<FTextProperty>(&Property))
	{
		return MakeString(Text->GetPropertyValue(ValuePtr).ToString());
	}
	if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(&Property))
	{
		const FSoftObjectPtr& Ptr = *static_cast<const FSoftObjectPtr*>(ValuePtr);
		return Ptr.IsNull() ? MakeNull() : MakeString(Ptr.ToSoftObjectPath().ToString());
	}
	if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
	{
		const UObject* Value = Object->GetObjectPropertyValue(ValuePtr);
		return Value == nullptr ? MakeNull() : MakeString(Value->GetPathName());
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Struct->Struct == TBaseStructure<FSoftObjectPath>::Get())
		{
			return MakeString(static_cast<const FSoftObjectPath*>(ValuePtr)->ToString());
		}
		FObjectBuilder Builder;
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			Builder.Add(It->GetName(), EncodePropertyValue(**It, It->ContainerPtrToValuePtr<void>(ValuePtr)));
		}
		return Builder.Build();
	}
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(ArrayProp, ValuePtr);
		TArray<FValue> Elements;
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			Elements.Add(EncodePropertyValue(*ArrayProp->Inner, Helper.GetRawPtr(Index)));
		}
		return MakeArray(MoveTemp(Elements));
	}
	return ExportAsText(Property, ValuePtr);
}

namespace
{
// A map key written as JSON object key text: a number key reads the number the text states.
bool AssetDocumentGraphPropertyCodec_DecodeKey(const FProperty& KeyProperty, void* KeyPtr, const FValue& Key, FString& OutError)
{
	const FNumericProperty* Numeric = CastField<FNumericProperty>(&KeyProperty);
	if (Key.Kind == EAssetDocumentRuntimeValueKind::String && Numeric != nullptr && Numeric->GetIntPropertyEnum() == nullptr)
	{
		const FString Trimmed = Key.StringValue.TrimStartAndEnd();
		if (!Trimmed.IsNumeric())
		{
			OutError = FString::Printf(TEXT("'%s' is not a number key."), *Key.StringValue);
			return false;
		}
		return DecodePropertyValue(KeyProperty, KeyPtr, MakeNumber(FCString::Atod(*Trimmed)), OutError);
	}
	return DecodePropertyValue(KeyProperty, KeyPtr, Key, OutError);
}

bool AssetDocumentGraphPropertyCodec_DecodeMap(const FMapProperty& MapProperty, void* ValuePtr, const FValue& Value, bool bKeyed,
	FString& OutError)
{
	FScriptMapHelper Helper(&MapProperty, ValuePtr);
	const int32 Count = bKeyed ? Value.ObjectKeys.Num() : Value.Elements.Num();
	Helper.EmptyValues(Count);
	for (int32 Pair = 0; Pair < Count; ++Pair)
	{
		const FValue KeyText = bKeyed ? MakeString(Value.ObjectKeys[Pair]) : FValue();
		const FValue* Key = bKeyed ? &KeyText : FindField(Value.Elements[Pair], TEXT("Key"));
		const FValue* Item = bKeyed ? &Value.Elements[Pair] : FindField(Value.Elements[Pair], TEXT("Value"));
		if (Key == nullptr || Item == nullptr)
		{
			Helper.Rehash();
			OutError = FString::Printf(TEXT("%s expects an object keyed by the map's keys or an array of {\"Key\", \"Value\"}."),
				*MapProperty.GetName());
			return false;
		}
		const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
		if (!AssetDocumentGraphPropertyCodec_DecodeKey(*MapProperty.KeyProp, Helper.GetKeyPtr(Index), *Key, OutError)
			|| !DecodePropertyValue(*MapProperty.ValueProp, Helper.GetValuePtr(Index), *Item, OutError))
		{
			Helper.Rehash();
			return false;
		}
	}
	Helper.Rehash();
	for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
	{
		if (Helper.IsValidIndex(Index) && Helper.FindMapIndexWithKey(Helper.GetPairPtr(Index)) != Index)
		{
			FString KeyText;
			MapProperty.KeyProp->ExportTextItem_Direct(KeyText, Helper.GetKeyPtr(Index), nullptr, nullptr, PPF_None);
			OutError = FString::Printf(TEXT("%s names the key %s twice."), *MapProperty.GetName(), *KeyText);
			return false;
		}
	}
	return true;
}

bool AssetDocumentGraphPropertyCodec_DecodeSet(const FSetProperty& SetProperty, void* ValuePtr, const FValue& Value, FString& OutError)
{
	FScriptSetHelper Helper(&SetProperty, ValuePtr);
	Helper.EmptyElements(Value.Elements.Num());
	for (const FValue& Element : Value.Elements)
	{
		const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
		if (!AssetDocumentGraphPropertyCodec_DecodeKey(*SetProperty.ElementProp, Helper.GetElementPtr(Index), Element, OutError))
		{
			Helper.Rehash();
			return false;
		}
	}
	Helper.Rehash();
	for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
	{
		if (Helper.IsValidIndex(Index) && Helper.FindElementIndex(Helper.GetElementPtr(Index)) != Index)
		{
			OutError = FString::Printf(TEXT("%s lists one element twice."), *SetProperty.GetName());
			return false;
		}
	}
	return true;
}
} // namespace

bool DecodePropertyValue(const FProperty& Property, void* ValuePtr, const FValue& Value, FString& OutError)
{
	if (Property.ArrayDim > 1)
	{
		return ImportFromText(Property, ValuePtr, Value, OutError);
	}
	if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Boolean)
		{
			OutError = FString::Printf(TEXT("%s expects a boolean."), *Property.GetName());
			return false;
		}
		Bool->SetPropertyValue(ValuePtr, Value.BooleanValue);
		return true;
	}
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(&Property))
	{
		int64 Raw = 0;
		if (!DecodeEnumValue(EnumProp->GetEnum(), Value, Raw, OutError))
		{
			return false;
		}
		EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, Raw);
		return true;
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
	{
		if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
		{
			int64 Raw = 0;
			if (!DecodeEnumValue(Enum, Value, Raw, OutError))
			{
				return false;
			}
			Numeric->SetIntPropertyValue(ValuePtr, Raw);
			return true;
		}
		double Number = 0.0;
		if (!ReadNumber(Value, Number) || !FMath::IsFinite(Number))
		{
			OutError = FString::Printf(TEXT("%s expects a number."), *Property.GetName());
			return false;
		}
		if (Numeric->IsFloatingPoint())
		{
			Numeric->SetFloatingPointPropertyValue(ValuePtr, Number);
		}
		else
		{
			if (FMath::FloorToDouble(Number) != Number)
			{
				OutError = FString::Printf(TEXT("%s expects an integer."), *Property.GetName());
				return false;
			}
			Numeric->SetIntPropertyValue(ValuePtr, static_cast<int64>(Number));
		}
		return true;
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::String)
	{
		if (const FStrProperty* Str = CastField<FStrProperty>(&Property))
		{
			Str->SetPropertyValue(ValuePtr, Value.StringValue);
			return true;
		}
		if (const FNameProperty* Name = CastField<FNameProperty>(&Property))
		{
			Name->SetPropertyValue(ValuePtr, FName(*Value.StringValue));
			return true;
		}
		if (const FTextProperty* Text = CastField<FTextProperty>(&Property))
		{
			Text->SetPropertyValue(ValuePtr, FText::FromString(Value.StringValue));
			return true;
		}
	}
	if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(&Property))
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Null)
		{
			*static_cast<FSoftObjectPtr*>(ValuePtr) = FSoftObjectPtr();
			return true;
		}
		if (Value.Kind != EAssetDocumentRuntimeValueKind::String)
		{
			OutError = FString::Printf(TEXT("%s expects an object path string or null."), *Property.GetName());
			return false;
		}
		*static_cast<FSoftObjectPtr*>(ValuePtr) = FSoftObjectPtr(FSoftObjectPath(Value.StringValue));
		return true;
	}
	if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Null)
		{
			Object->SetObjectPropertyValue(ValuePtr, nullptr);
			return true;
		}
		if (Value.Kind != EAssetDocumentRuntimeValueKind::String)
		{
			OutError = FString::Printf(TEXT("%s expects an object path string or null."), *Property.GetName());
			return false;
		}
		UClass* Required = Object->PropertyClass;
		if (const FClassProperty* ClassProp = CastField<FClassProperty>(&Property))
		{
			Required = UClass::StaticClass();
			UClass* Resolved = Cast<UClass>(ResolveObjectPath(Value.StringValue, Required));
			if (Resolved == nullptr || (ClassProp->MetaClass != nullptr && !Resolved->IsChildOf(ClassProp->MetaClass)))
			{
				OutError = FString::Printf(TEXT("'%s' is not a %s subclass."), *Value.StringValue,
					ClassProp->MetaClass != nullptr ? *ClassProp->MetaClass->GetName() : TEXT("UObject"));
				return false;
			}
			Object->SetObjectPropertyValue(ValuePtr, Resolved);
			return true;
		}
		UObject* Resolved = ResolveObjectPath(Value.StringValue, Required);
		if (Resolved == nullptr)
		{
			OutError = FString::Printf(TEXT("'%s' does not resolve to a %s."), *Value.StringValue,
				Required != nullptr ? *Required->GetName() : TEXT("UObject"));
			return false;
		}
		Object->SetObjectPropertyValue(ValuePtr, Resolved);
		return true;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Struct->Struct == TBaseStructure<FSoftObjectPath>::Get() && Value.Kind == EAssetDocumentRuntimeValueKind::String)
		{
			*static_cast<FSoftObjectPath*>(ValuePtr) = FSoftObjectPath(Value.StringValue);
			return true;
		}
		if (Value.Kind == EAssetDocumentRuntimeValueKind::String)
		{
			return ImportFromText(Property, ValuePtr, Value, OutError);
		}
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			OutError = FString::Printf(TEXT("%s expects an object of struct fields."), *Property.GetName());
			return false;
		}
		// Partial update: only the provided fields change.
		for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
		{
			FProperty* Field = Struct->Struct->FindPropertyByName(FName(*Value.ObjectKeys[Index]));
			if (Field == nullptr)
			{
				OutError = FString::Printf(TEXT("Struct %s has no field '%s'."), *Struct->Struct->GetName(),
					*Value.ObjectKeys[Index]);
				return false;
			}
			if (!DecodePropertyValue(*Field, Field->ContainerPtrToValuePtr<void>(ValuePtr), Value.Elements[Index], OutError))
			{
				return false;
			}
		}
		return true;
	}
	if (const FMapProperty* MapProp = CastField<FMapProperty>(&Property))
	{
		// A map is its UE text, a JSON object keyed by its keys' text, or an array of {"Key", "Value"}.
		const bool bKeyed = Value.Kind == EAssetDocumentRuntimeValueKind::Object;
		if (!bKeyed && Value.Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			return ImportFromText(Property, ValuePtr, Value, OutError);
		}
		return AssetDocumentGraphPropertyCodec_DecodeMap(*MapProp, ValuePtr, Value, bKeyed, OutError);
	}
	if (const FSetProperty* SetProp = CastField<FSetProperty>(&Property))
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			return ImportFromText(Property, ValuePtr, Value, OutError);
		}
		return AssetDocumentGraphPropertyCodec_DecodeSet(*SetProp, ValuePtr, Value, OutError);
	}
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(&Property))
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			return ImportFromText(Property, ValuePtr, Value, OutError);
		}
		FScriptArrayHelper Helper(ArrayProp, ValuePtr);
		Helper.Resize(Value.Elements.Num());
		for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
		{
			if (!DecodePropertyValue(*ArrayProp->Inner, Helper.GetRawPtr(Index), Value.Elements[Index], OutError))
			{
				return false;
			}
		}
		return true;
	}
	return ImportFromText(Property, ValuePtr, Value, OutError);
}

FValue EncodeEditableProperties(UObject& Object)
{
	FObjectBuilder Builder;
	for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Edit) && !It->HasAnyPropertyFlags(CPF_Deprecated))
		{
			Builder.Add(It->GetName(), EncodePropertyValue(**It, It->ContainerPtrToValuePtr<void>(&Object)));
		}
	}
	return Builder.Build();
}

bool ResolvePropertyPath(UObject& Object, FStringView Path, FResolvedPropertyPath& Out, FString& OutError)
{
	TArray<FString> Segments;
	FString(Path).ParseIntoArray(Segments, TEXT("."));
	if (Segments.IsEmpty())
	{
		OutError = TEXT("property_path is empty.");
		return false;
	}
	UStruct* Struct = Object.GetClass();
	void* Container = &Object;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		FProperty* Property = Struct->FindPropertyByName(FName(*Segments[Index]));
		if (Property == nullptr)
		{
			TArray<FString> Names;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (Index > 0 || It->HasAnyPropertyFlags(CPF_Edit))
				{
					Names.Add(It->GetName());
				}
			}
			OutError = FString::Printf(TEXT("%s has no property '%s'. Candidates: %s."), *Struct->GetName(),
				*Segments[Index], *FString::Join(Names, TEXT(", ")));
			return false;
		}
		if (Index == 0)
		{
			Out.TopLevel = Property;
		}
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
		if (Index == Segments.Num() - 1)
		{
			Out.Leaf = Property;
			Out.LeafPtr = ValuePtr;
			return true;
		}
		const FStructProperty* StructProp = CastField<FStructProperty>(Property);
		if (StructProp == nullptr)
		{
			OutError = FString::Printf(TEXT("'%s' is not a struct; cannot descend into '%s'."),
				*Segments[Index], *Segments[Index + 1]);
			return false;
		}
		Struct = StructProp->Struct;
		Container = ValuePtr;
	}
	return false;
}
} // namespace UE::AssetDocument::Graph
