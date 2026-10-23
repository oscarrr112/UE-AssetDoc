// Copyright 2026 Oscar Peng and Houyu. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Utils/PropertySetterUtils.h"
#include "AssetDocumentServerModule.h"
#include "Utils/ClassFinderUtils.h"

#include "UObject/EnumProperty.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateColor.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Fonts/SlateFontInfo.h"
#include "Layout/Margin.h"
#include "Widgets/Layout/Anchors.h"
#include "GameplayTagsManager.h"
#include "StateTree.h"
#include "StateTreeReference.h"
#if WITH_GAMEPLAY_ABILITIES
#include "AttributeSet.h"
#endif

namespace
{
FString MakeChildPropertyPath(const FString& BasePath, const FString& ChildName)
{
	return BasePath.IsEmpty() ? ChildName : FString::Printf(TEXT("%s.%s"), *BasePath, *ChildName);
}

FString MakeArrayPropertyPath(const FString& BasePath, int32 Index)
{
	return FString::Printf(TEXT("%s[%d]"), *BasePath, Index);
}

FString MakeMapPropertyPath(const FString& BasePath, const FString& Key)
{
	return FString::Printf(TEXT("%s[%s]"), *BasePath, *Key);
}

UClass* FindOrLoadClassAtExactPath(const FString& ClassPath)
{
	if (UClass* FoundClass = FindObject<UClass>(nullptr, *ClassPath))
	{
		return FoundClass;
	}
	return LoadClass<UObject>(nullptr, *ClassPath, {}, LOAD_NoWarn);
}

UClass* ResolveClassPropertyPath(const FString& ClassPath)
{
	if (UClass* ExactClass = FindOrLoadClassAtExactPath(ClassPath))
	{
		return ExactClass;
	}

	const bool bIsContentPath = ClassPath.StartsWith(TEXT("/")) && !ClassPath.StartsWith(TEXT("/Script/"));
	if (bIsContentPath && !ClassPath.EndsWith(TEXT("_C")))
	{
		return FindOrLoadClassAtExactPath(ClassPath + TEXT("_C"));
	}
	return nullptr;
}
}

//////////////////////////////////////////////////////////////////////////
// New Typed Property System
//////////////////////////////////////////////////////////////////////////

FParsedTypeInfo FPropertySetterUtils::ParseTypeString(const FString& TypeString)
{
	FParsedTypeInfo Result;

	if (TypeString.IsEmpty())
	{
		Result.ErrorMessage = TEXT("Type string is empty");
		return Result;
	}

	// Check for compound types with colon separator
	// Examples: "Object:UStaticMesh", "Array:Float", "Map:String:Int", "Enum:ECollisionChannel", "Struct:FMyStruct"
	TArray<FString> Parts;
	TypeString.ParseIntoArray(Parts, TEXT(":"), true);

	if (Parts.Num() == 0)
	{
		Result.ErrorMessage = TEXT("Failed to parse type string");
		return Result;
	}

	Result.BaseType = Parts[0];

	// Handle different base types
	if (Result.BaseType == TEXT("Object") || Result.BaseType == TEXT("SoftObject") || Result.BaseType == TEXT("Class"))
	{
		if (Parts.Num() < 2)
		{
			Result.ErrorMessage = FString::Printf(TEXT("%s type requires a class name (e.g., %s:UStaticMesh)"), *Result.BaseType, *Result.BaseType);
			return Result;
		}
		Result.SubType = Parts[1];
	}
	else if (Result.BaseType == TEXT("Array"))
	{
		if (Parts.Num() < 2)
		{
			Result.ErrorMessage = TEXT("Array type requires element type (e.g., Array:Float or Array:Object:UStaticMesh)");
			return Result;
		}
		// Element type could be compound like "Object:UStaticMesh"
		if (Parts.Num() >= 3)
		{
			Result.ElementType = Parts[1] + TEXT(":") + Parts[2];
		}
		else
		{
			Result.ElementType = Parts[1];
		}
	}
	else if (Result.BaseType == TEXT("Map"))
	{
		if (Parts.Num() < 3)
		{
			Result.ErrorMessage = TEXT("Map type requires key and value types (e.g., Map:String:Float)");
			return Result;
		}
		Result.KeyType = Parts[1];
		// Value type could be compound
		if (Parts.Num() >= 4)
		{
			Result.ValueType = Parts[2] + TEXT(":") + Parts[3];
		}
		else
		{
			Result.ValueType = Parts[2];
		}
	}
	else if (Result.BaseType == TEXT("Enum"))
	{
		if (Parts.Num() < 2)
		{
			Result.ErrorMessage = TEXT("Enum type requires enum name (e.g., Enum:ECollisionChannel)");
			return Result;
		}
		Result.SubType = Parts[1];
	}
	else if (Result.BaseType == TEXT("Struct"))
	{
		if (Parts.Num() < 2)
		{
			Result.ErrorMessage = TEXT("Struct type requires struct name (e.g., Struct:FMyStruct)");
			return Result;
		}
		Result.SubType = Parts[1];
	}
	// Basic types and built-in structs don't need additional parsing
	// Bool, Byte, Int, Int64, Float, Double, String, Name, Text
	// FVector, FVector2D, FRotator, FTransform, FLinearColor, FColor, FMargin

	Result.bIsValid = true;
	return Result;
}

FPropertyValidationResult FPropertySetterUtils::ValidateTypedValue(const FParsedTypeInfo& TypeInfo, TSharedPtr<FJsonValue> Value)
{
	if (!TypeInfo.bIsValid)
	{
		return FPropertyValidationResult::Failure(TypeInfo.ErrorMessage);
	}

	if (!Value.IsValid())
	{
		return FPropertyValidationResult::Failure(TEXT("Value is null"));
	}

	const FString& Type = TypeInfo.BaseType;

	// Boolean
	if (Type == TEXT("Bool"))
	{
		bool Temp;
		if (!Value->TryGetBool(Temp))
		{
			return FPropertyValidationResult::Failure(TEXT("Expected boolean value"));
		}
	}
	// Numeric types
	else if (Type == TEXT("Byte") || Type == TEXT("Int") || Type == TEXT("Int64") ||
			 Type == TEXT("Float") || Type == TEXT("Double"))
	{
		double Temp;
		if (!Value->TryGetNumber(Temp))
		{
			return FPropertyValidationResult::Failure(FString::Printf(TEXT("Expected numeric value for type %s"), *Type));
		}
	}
	// String types
	else if (Type == TEXT("String") || Type == TEXT("Name") || Type == TEXT("Text"))
	{
		FString Temp;
		if (!Value->TryGetString(Temp))
		{
			return FPropertyValidationResult::Failure(FString::Printf(TEXT("Expected string value for type %s"), *Type));
		}
	}
	// Vector types - expect array
	else if (Type == TEXT("FVector") || Type == TEXT("FRotator"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!Value->TryGetArray(Arr) || Arr->Num() < 3)
		{
			return FPropertyValidationResult::Failure(FString::Printf(TEXT("Expected array with 3 elements for %s"), *Type));
		}
	}
	else if (Type == TEXT("FVector2D"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!Value->TryGetArray(Arr) || Arr->Num() < 2)
		{
			return FPropertyValidationResult::Failure(TEXT("Expected array with 2 elements for FVector2D"));
		}
	}
	else if (Type == TEXT("FTransform"))
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!Value->TryGetObject(Obj))
		{
			return FPropertyValidationResult::Failure(TEXT("Expected object with Location/Rotation/Scale for FTransform"));
		}
	}
	// Color types - accept array, string, or object
	else if (Type == TEXT("FLinearColor") || Type == TEXT("FColor"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		FString Str;
		const TSharedPtr<FJsonObject>* Obj;
		if (!Value->TryGetArray(Arr) && !Value->TryGetString(Str) && !Value->TryGetObject(Obj))
		{
			return FPropertyValidationResult::Failure(FString::Printf(TEXT("Expected array [R,G,B,A], hex string, or object for %s"), *Type));
		}
	}
	// Object/SoftObject/Class references - expect string path
	else if (Type == TEXT("Object") || Type == TEXT("SoftObject") || Type == TEXT("Class"))
	{
		FString Temp;
		if (!Value->TryGetString(Temp))
		{
			return FPropertyValidationResult::Failure(FString::Printf(TEXT("Expected asset path string for %s"), *Type));
		}
	}
	// Array - expect JSON array
	else if (Type == TEXT("Array"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!Value->TryGetArray(Arr))
		{
			return FPropertyValidationResult::Failure(TEXT("Expected array value for Array type"));
		}
		// Optionally validate each element
		FParsedTypeInfo ElementType = ParseTypeString(TypeInfo.ElementType);
		if (ElementType.bIsValid)
		{
			for (int32 i = 0; i < Arr->Num(); ++i)
			{
				auto ElementResult = ValidateTypedValue(ElementType, (*Arr)[i]);
				if (!ElementResult.bIsValid)
				{
					return FPropertyValidationResult::Failure(FString::Printf(TEXT("Array element %d: %s"), i, *ElementResult.ErrorMessage));
				}
			}
		}
	}
	// Map - expect JSON object
	else if (Type == TEXT("Map"))
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!Value->TryGetObject(Obj))
		{
			return FPropertyValidationResult::Failure(TEXT("Expected object value for Map type"));
		}
		// Recursively validate values (symmetric with Array validation above)
		FParsedTypeInfo ValueTypeInfo = ParseTypeString(TypeInfo.ValueType);
		if (ValueTypeInfo.bIsValid)
		{
			for (const auto& Pair : (*Obj)->Values)
			{
				auto ValResult = ValidateTypedValue(ValueTypeInfo, Pair.Value);
				if (!ValResult.bIsValid)
				{
					return FPropertyValidationResult::Failure(FString::Printf(
						TEXT("Map key '%s': %s"), *Pair.Key, *ValResult.ErrorMessage));
				}
			}
		}
	}
	// Enum - expect string
	else if (Type == TEXT("Enum"))
	{
		FString Temp;
		if (!Value->TryGetString(Temp))
		{
			return FPropertyValidationResult::Failure(TEXT("Expected string value for Enum type"));
		}
	}
	// Struct - expect object with nested properties
	else if (Type == TEXT("Struct"))
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!Value->TryGetObject(Obj))
		{
			return FPropertyValidationResult::Failure(TEXT("Expected object value for Struct type"));
		}
	}
	// FMargin - accept number, array, or object
	else if (Type == TEXT("FMargin"))
	{
		double Num;
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		const TSharedPtr<FJsonObject>* Obj;
		if (!Value->TryGetNumber(Num) && !Value->TryGetArray(Arr) && !Value->TryGetObject(Obj))
		{
			return FPropertyValidationResult::Failure(TEXT("Expected number, array, or object for FMargin"));
		}
	}

	return FPropertyValidationResult::Success();
}

bool FPropertySetterUtils::ValidateTypedProperties(TSharedPtr<FJsonObject> Properties, TArray<FString>& OutErrors)
{
	if (!Properties.IsValid())
	{
		OutErrors.Add(TEXT("Properties object is null"));
		return false;
	}

	bool bAllValid = true;

	for (const auto& Pair : Properties->Values)
	{
		const FString PropName(*Pair.Key);
		const TSharedPtr<FJsonValue>& PropValue = Pair.Value;

		const TSharedPtr<FJsonObject>* TypedObj;
		if (!PropValue->TryGetObject(TypedObj))
		{
			OutErrors.Add(FString::Printf(TEXT("Property '%s': expected object with 'type' and 'value' fields"), *PropName));
			bAllValid = false;
			continue;
		}

		FString TypeStr;
		if (!(*TypedObj)->TryGetStringField(TEXT("type"), TypeStr))
		{
			OutErrors.Add(FString::Printf(TEXT("Property '%s': missing 'type' field"), *PropName));
			bAllValid = false;
			continue;
		}

		TSharedPtr<FJsonValue> ValueField = (*TypedObj)->TryGetField(TEXT("value"));
		if (!ValueField.IsValid())
		{
			OutErrors.Add(FString::Printf(TEXT("Property '%s': missing 'value' field"), *PropName));
			bAllValid = false;
			continue;
		}

		FParsedTypeInfo TypeInfo = ParseTypeString(TypeStr);
		if (!TypeInfo.bIsValid)
		{
			OutErrors.Add(FString::Printf(TEXT("Property '%s': invalid type '%s' - %s"), *PropName, *TypeStr, *TypeInfo.ErrorMessage));
			bAllValid = false;
			continue;
		}

		auto ValidationResult = ValidateTypedValue(TypeInfo, ValueField);
		if (!ValidationResult.bIsValid)
		{
			OutErrors.Add(FString::Printf(TEXT("Property '%s': %s"), *PropName, *ValidationResult.ErrorMessage));
			bAllValid = false;
		}
	}

	return bAllValid;
}

UScriptStruct* FPropertySetterUtils::FindStructByName(const FString& StructName)
{
	static TMap<FString, UScriptStruct*> Cache;
	static bool bInit = false;
	if (!bInit)
	{
		bInit = true;

		// Pre-populate common types (both with and without F prefix)
		auto Add = [](UScriptStruct* S, const FString& Name)
		{
			Cache.Add(Name, S);
			Cache.Add(TEXT("F") + Name, S);
		};
		Add(TBaseStructure<FVector>::Get(), TEXT("Vector"));
		Add(TBaseStructure<FVector2D>::Get(), TEXT("Vector2D"));
		Add(TBaseStructure<FRotator>::Get(), TEXT("Rotator"));
		Add(TBaseStructure<FTransform>::Get(), TEXT("Transform"));
		Add(TBaseStructure<FLinearColor>::Get(), TEXT("LinearColor"));
		Add(TBaseStructure<FColor>::Get(), TEXT("Color"));
		Add(TBaseStructure<FMargin>::Get(), TEXT("Margin"));
		Add(TBaseStructure<FSoftObjectPath>::Get(), TEXT("SoftObjectPath"));
	}

	if (auto* Found = Cache.Find(StructName))
	{
		return *Found;
	}

	// Dynamic lookup + cache result
	FString SearchName = StructName.StartsWith(TEXT("F")) ? StructName : TEXT("F") + StructName;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		if (It->GetName() == SearchName || It->GetName() == StructName)
		{
			Cache.Add(StructName, *It);
			return *It;
		}
	}

	UE_LOG(LogAssetDocumentServer, Warning, TEXT("Struct '%s' not found"), *StructName);
	return nullptr;
}

UEnum* FPropertySetterUtils::FindEnumByName(const FString& EnumName)
{
	FString SearchName = EnumName;
	if (!SearchName.StartsWith(TEXT("E")))
	{
		SearchName = TEXT("E") + SearchName;
	}

	for (TObjectIterator<UEnum> It; It; ++It)
	{
		if (It->GetName() == SearchName || It->GetName() == EnumName)
		{
			return *It;
		}
	}

	UE_LOG(LogAssetDocumentServer, Warning, TEXT("Enum '%s' not found"), *EnumName);
	return nullptr;
}

bool FPropertySetterUtils::SetValueFromTypedJson(void* ValuePtr, const FParsedTypeInfo& TypeInfo, TSharedPtr<FJsonValue> JsonValue)
{
	if (!ValuePtr || !TypeInfo.bIsValid || !JsonValue.IsValid())
	{
		return false;
	}

	const FString& Type = TypeInfo.BaseType;

	// Boolean
	if (Type == TEXT("Bool"))
	{
		bool Value = false;
		if (JsonValue->TryGetBool(Value))
		{
			*static_cast<bool*>(ValuePtr) = Value;
			return true;
		}
	}
	// Byte
	else if (Type == TEXT("Byte"))
	{
		double Value = 0;
		if (JsonValue->TryGetNumber(Value))
		{
			*static_cast<uint8*>(ValuePtr) = static_cast<uint8>(Value);
			return true;
		}
	}
	// Int
	else if (Type == TEXT("Int"))
	{
		double Value = 0;
		if (JsonValue->TryGetNumber(Value))
		{
			*static_cast<int32*>(ValuePtr) = static_cast<int32>(Value);
			return true;
		}
	}
	// Int64
	else if (Type == TEXT("Int64"))
	{
		double Value = 0;
		if (JsonValue->TryGetNumber(Value))
		{
			*static_cast<int64*>(ValuePtr) = static_cast<int64>(Value);
			return true;
		}
	}
	// Float
	else if (Type == TEXT("Float"))
	{
		double Value = 0;
		if (JsonValue->TryGetNumber(Value))
		{
			*static_cast<float*>(ValuePtr) = static_cast<float>(Value);
			return true;
		}
	}
	// Double
	else if (Type == TEXT("Double"))
	{
		double Value = 0;
		if (JsonValue->TryGetNumber(Value))
		{
			*static_cast<double*>(ValuePtr) = Value;
			return true;
		}
	}
	// String
	else if (Type == TEXT("String"))
	{
		FString Value;
		if (JsonValue->TryGetString(Value))
		{
			*static_cast<FString*>(ValuePtr) = Value;
			return true;
		}
	}
	// Name
	else if (Type == TEXT("Name"))
	{
		FString Value;
		if (JsonValue->TryGetString(Value))
		{
			*static_cast<FName*>(ValuePtr) = FName(*Value);
			return true;
		}
	}
	// Text
	else if (Type == TEXT("Text"))
	{
		FString Value;
		if (JsonValue->TryGetString(Value))
		{
			*static_cast<FText*>(ValuePtr) = FText::FromString(Value);
			return true;
		}
	}
	// F-prefixed struct types (FVector, FLinearColor, FTransform, etc.)
	else if (Type.StartsWith(TEXT("F")))
	{
		UScriptStruct* Struct = FindStructByName(Type);
		if (Struct)
		{
			return SetStructFromJson(Struct, ValuePtr, JsonValue);
		}
	}
	// Object reference
	else if (Type == TEXT("Object"))
	{
		FString ObjectPath;
		if (JsonValue->TryGetString(ObjectPath))
		{
			UClass* ExpectedClass = FClassFinderUtils::FindClassByName(TypeInfo.SubType, UObject::StaticClass(), false);
			if (!ExpectedClass)
			{
				ExpectedClass = UObject::StaticClass();
			}

			UObject* LoadedObject = StaticLoadObject(ExpectedClass, nullptr, *ObjectPath);
			if (LoadedObject)
			{
				*static_cast<UObject**>(ValuePtr) = LoadedObject;
				return true;
			}
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Failed to load object: %s"), *ObjectPath);
		}
	}
	// Soft object reference
	else if (Type == TEXT("SoftObject"))
	{
		FString ObjectPath;
		if (JsonValue->TryGetString(ObjectPath))
		{
			if (!ObjectPath.Contains(TEXT(".")))
			{
				FString AssetName = FPaths::GetBaseFilename(ObjectPath);
				ObjectPath = ObjectPath + TEXT(".") + AssetName;
			}
			FSoftObjectPath SoftPath(ObjectPath);
			*static_cast<FSoftObjectPath*>(ValuePtr) = SoftPath;
			return true;
		}
	}
	// Enum
	else if (Type == TEXT("Enum"))
	{
		FString EnumValueStr;
		if (JsonValue->TryGetString(EnumValueStr))
		{
			UEnum* Enum = FindEnumByName(TypeInfo.SubType);
			if (Enum)
			{
				int64 EnumValue = Enum->GetValueByNameString(EnumValueStr);
				if (EnumValue == INDEX_NONE)
				{
					EnumValue = Enum->GetValueByNameString(Enum->GetName() + TEXT("::") + EnumValueStr);
				}
				if (EnumValue != INDEX_NONE)
				{
					*static_cast<uint8*>(ValuePtr) = static_cast<uint8>(EnumValue);
					return true;
				}
			}
		}
	}
	// Struct - recursive handling
	else if (Type == TEXT("Struct"))
	{
		UScriptStruct* Struct = FindStructByName(TypeInfo.SubType);
		if (Struct)
		{
			return SetStructValueFromTypedJson(ValuePtr, Struct, JsonValue);
		}
	}

	return false;
}

bool FPropertySetterUtils::SetStructValueFromTypedJson(void* ValuePtr, UScriptStruct* Struct, TSharedPtr<FJsonValue> JsonValue)
{
	if (!ValuePtr || !Struct || !JsonValue.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Obj;
	if (!JsonValue->TryGetObject(Obj))
	{
		return false;
	}

	bool bAllSucceeded = true;
	for (const auto& Pair : (*Obj)->Values)
	{
		const FString FieldName(*Pair.Key);
		const TSharedPtr<FJsonValue>& FieldValue = Pair.Value;
		const FString FieldPath = MakeChildPropertyPath(Struct->GetName(), FieldName);

		// Each field should be { "type": "...", "value": ... }
		const TSharedPtr<FJsonObject>* TypedFieldObj;
		if (!FieldValue->TryGetObject(TypedFieldObj))
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Typed struct field '%s': expected object with 'type' and 'value' fields"), *FieldPath);
			bAllSucceeded = false;
			continue;
		}

		FString TypeStr;
		if (!(*TypedFieldObj)->TryGetStringField(TEXT("type"), TypeStr))
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Typed struct field '%s': missing 'type' field"), *FieldPath);
			bAllSucceeded = false;
			continue;
		}

		TSharedPtr<FJsonValue> ValueField = (*TypedFieldObj)->TryGetField(TEXT("value"));
		if (!ValueField.IsValid())
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Typed struct field '%s': missing 'value' field"), *FieldPath);
			bAllSucceeded = false;
			continue;
		}

		FProperty* FieldProp = Struct->FindPropertyByName(*FieldName);
		if (!FieldProp)
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Struct field path '%s' not found in %s"), *FieldPath, *Struct->GetName());
			bAllSucceeded = false;
			continue;
		}

		void* FieldPtr = FieldProp->ContainerPtrToValuePtr<void>(ValuePtr);
		FParsedTypeInfo TypeInfo = ParseTypeString(TypeStr);

		if (!TypeInfo.bIsValid)
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Typed struct field '%s': invalid type '%s' - %s"), *FieldPath, *TypeStr, *TypeInfo.ErrorMessage);
			bAllSucceeded = false;
			continue;
		}

		if (!SetValueFromTypedJson(FieldPtr, TypeInfo, ValueField))
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Failed to set typed struct field '%s'"), *FieldPath);
			bAllSucceeded = false;
		}
	}

	return bAllSucceeded;
}

bool FPropertySetterUtils::SetTypedPropertyFromJson(UObject* Object, const FString& PropertyName, TSharedPtr<FJsonObject> TypedValue)
{
	if (!Object || !TypedValue.IsValid())
	{
		return false;
	}

	FString TypeStr;
	if (!TypedValue->TryGetStringField(TEXT("type"), TypeStr))
	{
		UE_LOG(LogAssetDocumentServer, Warning, TEXT("Property '%s': missing 'type' field"), *PropertyName);
		return false;
	}

	TSharedPtr<FJsonValue> ValueField = TypedValue->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		UE_LOG(LogAssetDocumentServer, Warning, TEXT("Property '%s': missing 'value' field"), *PropertyName);
		return false;
	}

	FParsedTypeInfo TypeInfo = ParseTypeString(TypeStr);
	if (!TypeInfo.bIsValid)
	{
		UE_LOG(LogAssetDocumentServer, Warning, TEXT("Property '%s': invalid type '%s' - %s"), *PropertyName, *TypeStr, *TypeInfo.ErrorMessage);
		return false;
	}

	UClass* ObjectClass = Object->GetClass();
	FProperty* Property = ObjectClass->FindPropertyByName(*PropertyName);
	if (!Property)
	{
		UE_LOG(LogAssetDocumentServer, Warning, TEXT("Property '%s' not found on class '%s'"), *PropertyName, *ObjectClass->GetName());
		return false;
	}

	// Special handling for arrays - use reflection-based array handling
	if (TypeInfo.BaseType == TEXT("Array"))
	{
		FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property);
		if (ArrayProp)
		{
			const TArray<TSharedPtr<FJsonValue>>* ArrayValues;
			if (ValueField->TryGetArray(ArrayValues))
			{
				return SetArrayProperty(Object, ArrayProp, *ArrayValues);
			}
		}
		return false;
	}

	// Special handling for maps - use reflection-based map handling
	if (TypeInfo.BaseType == TEXT("Map"))
	{
		FMapProperty* MapProp = CastField<FMapProperty>(Property);
		if (MapProp)
		{
			const TSharedPtr<FJsonObject>* MapObj;
			if (ValueField->TryGetObject(MapObj))
			{
				return SetMapProperty(Object, MapProp, *MapObj);
			}
		}
		return false;
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
	return SetValueFromTypedJson(ValuePtr, TypeInfo, ValueField);
}

bool FPropertySetterUtils::SetTypedPropertiesFromJson(UObject* Object, TSharedPtr<FJsonObject> Properties)
{
	if (!Object || !Properties.IsValid())
	{
		return false;
	}

	bool bAllSucceeded = true;
	for (const auto& Pair : Properties->Values)
	{
		const FString PropertyName(*Pair.Key);
		const TSharedPtr<FJsonValue>& PropertyValue = Pair.Value;

		const TSharedPtr<FJsonObject>* TypedObj;
		if (!PropertyValue->TryGetObject(TypedObj))
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Property '%s': expected object with 'type' and 'value' fields"), *PropertyName);
			bAllSucceeded = false;
			continue;
		}

		if (!SetTypedPropertyFromJson(Object, PropertyName, *TypedObj))
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Failed to set typed property '%s'"), *PropertyName);
			bAllSucceeded = false;
		}
	}
	return bAllSucceeded;
}

//////////////////////////////////////////////////////////////////////////
// Legacy Property System
//////////////////////////////////////////////////////////////////////////

bool FPropertySetterUtils::SetPropertyFromJson(UObject* Object, FProperty* Property, TSharedPtr<FJsonValue> JsonValue)
{
	if (!Object || !Property || !JsonValue.IsValid())
	{
		return false;
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
	return SetPropertyValueInternal(Object, Property, ValuePtr, JsonValue);
}

bool FPropertySetterUtils::SetPropertiesFromJson(UObject* Object, TSharedPtr<FJsonObject> Properties)
{
	return SetPropertiesFromJsonInternal(Object, Properties, FString());
}

bool FPropertySetterUtils::SetPropertiesFromJsonInternal(UObject* Object, TSharedPtr<FJsonObject> Properties, const FString& BasePath)
{
	if (!Object || !Properties.IsValid())
	{
		return false;
	}

	bool bAllSucceeded = true;
	UClass* ObjectClass = Object->GetClass();

	for (const auto& Pair : Properties->Values)
	{
		const FString PropertyName(*Pair.Key);
		const TSharedPtr<FJsonValue>& JsonValue = Pair.Value;
		const FString PropertyPath = MakeChildPropertyPath(BasePath, PropertyName);

		FProperty* Property = ObjectClass->FindPropertyByName(*PropertyName);
		if (!Property)
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Property path '%s' not found on class '%s'"),
				*PropertyPath, *ObjectClass->GetName());
			bAllSucceeded = false;
			continue;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		if (!SetPropertyValueInternal(Object, Property, ValuePtr, JsonValue, PropertyPath))
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Failed to set property path '%s'"), *PropertyPath);
			bAllSucceeded = false;
		}
	}
	return bAllSucceeded;
}

bool FPropertySetterUtils::SetPropertyValueFromJson(FProperty* Property, void* ValuePtr, TSharedPtr<FJsonValue> JsonValue, UObject* OwnerObject, const FString& PropertyPath)
{
	if (!Property || !ValuePtr || !JsonValue.IsValid())
	{
		return false;
	}

	const FString EffectivePath = PropertyPath.IsEmpty() ? Property->GetName() : PropertyPath;

	if (JsonValue->Type == EJson::Null)
	{
		if (FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Property))
		{
			SoftObjProp->SetPropertyValue(ValuePtr, FSoftObjectPtr());
			return true;
		}

		if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Property))
		{
			SoftClassProp->SetPropertyValue(ValuePtr, FSoftObjectPtr());
			return true;
		}

		if (FClassProperty* ClassProp = CastField<FClassProperty>(Property))
		{
			ClassProp->SetPropertyValue(ValuePtr, nullptr);
			return true;
		}

		if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
		{
			ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
			return true;
		}
	}

	// --- Enum types (must come before FNumericProperty since FByteProperty inherits from it) ---

	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		FString EnumValueStr;
		if (!JsonValue->TryGetString(EnumValueStr))
		{
			return false;
		}
		UEnum* Enum = EnumProp->GetEnum();
		int64 EnumValue = Enum->GetValueByNameString(EnumValueStr);
		if (EnumValue == INDEX_NONE)
		{
			EnumValue = Enum->GetValueByNameString(Enum->GetName() + TEXT("::") + EnumValueStr);
		}
		if (EnumValue != INDEX_NONE)
		{
			EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumValue);
			return true;
		}
		UE_LOG(LogAssetDocumentServer, Warning, TEXT("SetPropertyValueFromJson: unknown enum value '%s' for '%s' at '%s'"), *EnumValueStr, *Enum->GetName(), *EffectivePath);
		return false;
	}

	if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		if (UEnum* Enum = ByteProp->Enum)
		{
			FString EnumValueStr;
			if (JsonValue->TryGetString(EnumValueStr))
			{
				int64 EnumValue = Enum->GetValueByNameString(EnumValueStr);
				if (EnumValue == INDEX_NONE)
				{
					EnumValue = Enum->GetValueByNameString(Enum->GetName() + TEXT("::") + EnumValueStr);
				}
				if (EnumValue != INDEX_NONE)
				{
					ByteProp->SetIntPropertyValue(ValuePtr, EnumValue);
					return true;
				}
			}
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("SetPropertyValueFromJson: unknown byte enum value '%s' for '%s' at '%s'"), *EnumValueStr, *Enum->GetName(), *EffectivePath);
			return false;
		}
		double Value = 0.0;
		if (JsonValue->TryGetNumber(Value))
		{
			ByteProp->SetIntPropertyValue(ValuePtr, static_cast<int64>(Value));
			return true;
		}
		return false;
	}

	// --- Primitive types ---

	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		bool Value = false;
		if (JsonValue->TryGetBool(Value))
		{
			BoolProp->SetPropertyValue(ValuePtr, Value);
			return true;
		}
		return false;
	}

	if (FNumericProperty* NumProp = CastField<FNumericProperty>(Property))
	{
		double Value = 0.0;
		if (JsonValue->TryGetNumber(Value))
		{
			if (NumProp->IsFloatingPoint())
			{
				NumProp->SetFloatingPointPropertyValue(ValuePtr, Value);
			}
			else
			{
				NumProp->SetIntPropertyValue(ValuePtr, static_cast<int64>(Value));
			}
			return true;
		}
		return false;
	}

	if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		FString Value;
		if (JsonValue->TryGetString(Value))
		{
			StrProp->SetPropertyValue(ValuePtr, Value);
			return true;
		}
		return false;
	}

	if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
	{
		FString Value;
		if (JsonValue->TryGetString(Value))
		{
			NameProp->SetPropertyValue(ValuePtr, FName(*Value));
			return true;
		}
		return false;
	}

	if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
	{
		FString Value;
		if (JsonValue->TryGetString(Value))
		{
			TextProp->SetPropertyValue(ValuePtr, FText::FromString(Value));
			return true;
		}
		return false;
	}

	// --- Reference types ---

	if (FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Property))
	{
		FString AssetPath;
		if (JsonValue->TryGetString(AssetPath))
		{
			if (!AssetPath.Contains(TEXT(".")))
			{
				FString AssetName = FPaths::GetBaseFilename(AssetPath);
				AssetPath = AssetPath + TEXT(".") + AssetName;
			}
			FSoftObjectPath SoftPath(AssetPath);
			FSoftObjectPtr SoftPtr(SoftPath);
			SoftObjProp->SetPropertyValue(ValuePtr, SoftPtr);
			return true;
		}
		return false;
	}

	if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Property))
	{
		FString ClassPath;
		if (JsonValue->TryGetString(ClassPath))
		{
			FSoftObjectPath SoftPath(ClassPath);
			FSoftObjectPtr SoftPtr(SoftPath);
			SoftClassProp->SetPropertyValue(ValuePtr, SoftPtr);
			return true;
		}
		return false;
	}

	if (FClassProperty* ClassProp = CastField<FClassProperty>(Property))
	{
		FString ClassPath;
		if (JsonValue->TryGetString(ClassPath))
		{
			UClass* LoadedClass = ResolveClassPropertyPath(ClassPath);
			if (LoadedClass)
			{
				UClass* MetaClass = ClassProp->MetaClass;
				if (MetaClass && !LoadedClass->IsChildOf(MetaClass))
				{
					UE_LOG(LogAssetDocumentServer, Warning, TEXT("SetPropertyValueFromJson: class '%s' is not a subclass of '%s' at '%s'"),
						*LoadedClass->GetName(), *MetaClass->GetName(), *EffectivePath);
					return false;
				}
				ClassProp->SetPropertyValue(ValuePtr, LoadedClass);
				return true;
			}
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("SetPropertyValueFromJson: failed to load class '%s' at '%s'"), *ClassPath, *EffectivePath);
		}
		return false;
	}

	if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
	{
		FString ObjectPath;
		if (JsonValue->TryGetString(ObjectPath))
		{
			const bool bSetObject = SetObjectReferenceFromPath(ObjProp, ValuePtr, ObjectPath);
			if (!bSetObject)
			{
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("SetPropertyValueFromJson: failed to set object reference '%s' at '%s'"), *ObjectPath, *EffectivePath);
			}
			return bSetObject;
		}
		// Instanced subobject: JSON object with "Class" field
		const TSharedPtr<FJsonObject>* ObjJson;
		if (JsonValue->TryGetObject(ObjJson) && OwnerObject)
		{
			FString ClassName;
			if ((*ObjJson)->TryGetStringField(TEXT("Class"), ClassName))
			{
				UClass* ElemClass = FClassFinderUtils::FindClassByName(ClassName, ObjProp->PropertyClass, false);
				if (ElemClass)
				{
					UObject* NewObj = NewObject<UObject>(OwnerObject, ElemClass);
					if (NewObj)
					{
						TSharedPtr<FJsonObject> PropsJson;
						if ((*ObjJson)->HasField(TEXT("Properties")))
						{
							PropsJson = (*ObjJson)->GetObjectField(TEXT("Properties"));
						}
						if (PropsJson.IsValid())
						{
							if (!SetPropertiesFromJsonInternal(NewObj, PropsJson, EffectivePath))
							{
								return false;
							}
						}
						ObjProp->SetObjectPropertyValue(ValuePtr, NewObj);
						return true;
					}
				}
			}
		}
		return false;
	}

	// --- Struct ---

	if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		return SetStructFromJsonInternal(StructProp->Struct, ValuePtr, JsonValue, OwnerObject, EffectivePath);
	}

	// --- Containers (recursive) ---

	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		const TArray<TSharedPtr<FJsonValue>>* ArrayValues;
		if (!JsonValue->TryGetArray(ArrayValues))
		{
			return false;
		}

		FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
		FProperty* InnerProp = ArrayProp->Inner;
		ArrayHelper.EmptyValues();

		bool bAllSucceeded = true;
		for (int32 i = 0; i < ArrayValues->Num(); ++i)
		{
			int32 Index = ArrayHelper.AddValue();
			void* ElemPtr = ArrayHelper.GetRawPtr(Index);
			const FString ElementPath = MakeArrayPropertyPath(EffectivePath, i);
			if (!SetPropertyValueFromJson(InnerProp, ElemPtr, (*ArrayValues)[i], OwnerObject, ElementPath))
			{
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("SetPropertyValueFromJson: failed to set '%s'"), *ElementPath);
				bAllSucceeded = false;
			}
		}
		return bAllSucceeded;
	}

	if (FMapProperty* MapProp = CastField<FMapProperty>(Property))
	{
		const TSharedPtr<FJsonObject>* MapObj;
		if (!JsonValue->TryGetObject(MapObj))
		{
			return false;
		}

		FScriptMapHelper MapHelper(MapProp, ValuePtr);
		FProperty* KeyProp = MapProp->KeyProp;
		FProperty* ValuePropInner = MapProp->ValueProp;
		MapHelper.EmptyValues();

		bool bAllSucceeded = true;
		for (const auto& Pair : (*MapObj)->Values)
		{
			const FString KeyStr(*Pair.Key);
			const TSharedPtr<FJsonValue>& PairValue = Pair.Value;

			int32 Index = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
			void* KeyPtr = MapHelper.GetKeyPtr(Index);

			// Parse key (same logic as SetMapProperty)
			bool bKeySet = false;
			if (FEnumProperty* EnumKeyProp = CastField<FEnumProperty>(KeyProp))
			{
				UEnum* Enum = EnumKeyProp->GetEnum();
				int64 EnumValue = Enum->GetValueByNameString(KeyStr);
				if (EnumValue == INDEX_NONE)
				{
					EnumValue = Enum->GetValueByNameString(Enum->GetName() + TEXT("::") + KeyStr);
				}
				if (EnumValue != INDEX_NONE)
				{
					EnumKeyProp->GetUnderlyingProperty()->SetIntPropertyValue(KeyPtr, EnumValue);
					bKeySet = true;
				}
			}
			else if (FByteProperty* ByteKeyProp = CastField<FByteProperty>(KeyProp))
			{
				if (UEnum* Enum = ByteKeyProp->Enum)
				{
					int64 EnumValue = Enum->GetValueByNameString(KeyStr);
					if (EnumValue != INDEX_NONE)
					{
						ByteKeyProp->SetIntPropertyValue(KeyPtr, EnumValue);
						bKeySet = true;
					}
				}
				else
				{
					ByteKeyProp->SetIntPropertyValue(KeyPtr, static_cast<int64>(FCString::Atoi(*KeyStr)));
					bKeySet = true;
				}
			}
			else if (FStrProperty* StrKeyProp = CastField<FStrProperty>(KeyProp))
			{
				StrKeyProp->SetPropertyValue(KeyPtr, KeyStr);
				bKeySet = true;
			}
			else if (FNameProperty* NameKeyProp = CastField<FNameProperty>(KeyProp))
			{
				NameKeyProp->SetPropertyValue(KeyPtr, FName(*KeyStr));
				bKeySet = true;
			}

			if (!bKeySet)
			{
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("SetPropertyValueFromJson: unsupported map key type '%s' at '%s'"), *KeyProp->GetClass()->GetName(), *EffectivePath);
				MapHelper.RemoveAt(Index);
				bAllSucceeded = false;
				continue;
			}

			// Set value recursively
			void* ValPtr = MapHelper.GetValuePtr(Index);
			const FString ValuePath = MakeMapPropertyPath(EffectivePath, KeyStr);
			if (!SetPropertyValueFromJson(ValuePropInner, ValPtr, PairValue, OwnerObject, ValuePath))
			{
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("SetPropertyValueFromJson: failed to set '%s'"), *ValuePath);
				bAllSucceeded = false;
			}
		}

		MapHelper.Rehash();
		return bAllSucceeded;
	}

	if (FSetProperty* SetProp = CastField<FSetProperty>(Property))
	{
		const TArray<TSharedPtr<FJsonValue>>* ArrayValues;
		if (!JsonValue->TryGetArray(ArrayValues))
		{
			return false;
		}

		FScriptSetHelper SetHelper(SetProp, ValuePtr);
		FProperty* ElemProp = SetProp->ElementProp;
		SetHelper.EmptyElements();

		bool bAllSucceeded = true;
		for (int32 i = 0; i < ArrayValues->Num(); ++i)
		{
			int32 Index = SetHelper.AddDefaultValue_Invalid_NeedsRehash();
			void* ElemPtr = SetHelper.GetElementPtr(Index);
			const FString ElementPath = MakeArrayPropertyPath(EffectivePath, i);
			if (!SetPropertyValueFromJson(ElemProp, ElemPtr, (*ArrayValues)[i], OwnerObject, ElementPath))
			{
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("SetPropertyValueFromJson: failed to set '%s'"), *ElementPath);
				bAllSucceeded = false;
			}
		}

		SetHelper.Rehash();
		return bAllSucceeded;
	}

	UE_LOG(LogAssetDocumentServer, Warning, TEXT("SetPropertyValueFromJson: unsupported property type '%s' (%s) at '%s'"),
		*Property->GetName(), *Property->GetClass()->GetName(), *EffectivePath);
	return false;
}

bool FPropertySetterUtils::SetDetachedPropertyValueFromJson(
	FProperty* Property,
	void* ValuePtr,
	TSharedPtr<FJsonValue> JsonValue,
	UObject* OwnerObject,
	const FString& PropertyPath)
{
	return SetPropertyValueFromJson(Property, ValuePtr, MoveTemp(JsonValue), OwnerObject, PropertyPath);
}

bool FPropertySetterUtils::SetPropertyValueInternal(UObject* Object, FProperty* Property, void* ValuePtr, TSharedPtr<FJsonValue> JsonValue, const FString& PropertyPath)
{
	if (!Property || !ValuePtr || !JsonValue.IsValid())
	{
		return false;
	}
	return SetPropertyValueFromJson(Property, ValuePtr, JsonValue, Object, PropertyPath);
}

// Check if a struct only contains numeric fields (float, double, int variants)
static bool IsNumericOnlyStruct(UScriptStruct* Struct)
{
	if (!Struct)
	{
		return false;
	}

	int32 FieldCount = 0;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		if (!CastField<FNumericProperty>(*It))
		{
			return false;
		}
		++FieldCount;
	}
	return FieldCount > 0;
}

// JSON array -> struct: map array elements to fields by declaration order
static bool SetStructFromArray(UScriptStruct* Struct, void* ValuePtr, const TArray<TSharedPtr<FJsonValue>>& Array)
{
	int32 Index = 0;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		if (Index >= Array.Num())
		{
			break;
		}

		FNumericProperty* NumProp = CastField<FNumericProperty>(*It);
		if (!NumProp)
		{
			return false;
		}

		double Value = 0;
		if (!Array[Index]->TryGetNumber(Value))
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Struct '%s' field '%s' expected numeric array value at index %d"),
				*Struct->GetName(), *(*It)->GetName(), Index);
			return false;
		}

		void* FieldPtr = NumProp->ContainerPtrToValuePtr<void>(ValuePtr);
		if (NumProp->IsFloatingPoint())
		{
			NumProp->SetFloatingPointPropertyValue(FieldPtr, Value);
		}
		else
		{
			NumProp->SetIntPropertyValue(FieldPtr, static_cast<int64>(Value));
		}
		++Index;
	}
	return true;
}

// struct -> JSON array: extract all numeric fields as a compact array
static TSharedPtr<FJsonValue> ExtractStructToArray(UScriptStruct* Struct, const void* ValuePtr)
{
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FNumericProperty* NumProp = CastField<FNumericProperty>(*It);
		if (!NumProp)
		{
			return nullptr;
		}

		const void* FieldPtr = NumProp->ContainerPtrToValuePtr<void>(ValuePtr);
		if (NumProp->IsFloatingPoint())
		{
			JsonArray.Add(MakeShared<FJsonValueNumber>(NumProp->GetFloatingPointPropertyValue(FieldPtr)));
		}
		else
		{
			JsonArray.Add(MakeShared<FJsonValueNumber>(static_cast<double>(NumProp->GetSignedIntPropertyValue(FieldPtr))));
		}
	}
	return MakeShared<FJsonValueArray>(JsonArray);
}

TMap<UScriptStruct*, FPropertySetterUtils::FStructDeserializer>& FPropertySetterUtils::GetSpecialDeserializers()
{
	static TMap<UScriptStruct*, FStructDeserializer> Map;
	static bool bFullyInit = false;
	if (!bFullyInit)
	{
		bool bAllFound = true;

		// Helper: register if struct found, track if any missing
		auto SafeAdd = [&bAllFound](UScriptStruct* Struct, FStructDeserializer Handler)
		{
			if (Struct)
			{
				Map.FindOrAdd(Struct) = MoveTemp(Handler);
			}
			else
			{
				bAllFound = false;
			}
		};

		// FLinearColor - supports hex string, named color, array, object
		Map.Add(TBaseStructure<FLinearColor>::Get(), [](void* ValuePtr, TSharedPtr<FJsonValue> JsonValue) -> bool
		{
			*static_cast<FLinearColor*>(ValuePtr) = ParseColor(JsonValue);
			return true;
		});

		// FColor - same as FLinearColor but converted
		Map.Add(TBaseStructure<FColor>::Get(), [](void* ValuePtr, TSharedPtr<FJsonValue> JsonValue) -> bool
		{
			*static_cast<FColor*>(ValuePtr) = ParseColor(JsonValue).ToFColor(true);
			return true;
		});

		// FMargin - supports single number, [H,V], [L,T,R,B], object
		Map.Add(TBaseStructure<FMargin>::Get(), [](void* ValuePtr, TSharedPtr<FJsonValue> JsonValue) -> bool
		{
			*static_cast<FMargin*>(ValuePtr) = ParseMargin(JsonValue);
			return true;
		});

		// FSoftObjectPath - string shorthand
		Map.Add(TBaseStructure<FSoftObjectPath>::Get(), [](void* ValuePtr, TSharedPtr<FJsonValue> JsonValue) -> bool
		{
			FString PathStr;
			if (JsonValue->TryGetString(PathStr))
			{
				*static_cast<FSoftObjectPath*>(ValuePtr) = FSoftObjectPath(PathStr);
				return true;
			}
			return false;
		});

		// FSlateColor - wraps FLinearColor
		SafeAdd(FindStructByName(TEXT("SlateColor")), [](void* ValuePtr, TSharedPtr<FJsonValue> JsonValue) -> bool
		{
			*static_cast<FSlateColor*>(ValuePtr) = FSlateColor(ParseColor(JsonValue));
			return true;
		});

		// FSlateFontInfo - handle known fields, then fallback to reflection for the rest
		SafeAdd(FindStructByName(TEXT("SlateFontInfo")), [](void* ValuePtr, TSharedPtr<FJsonValue> JsonValue) -> bool
		{
			const TSharedPtr<FJsonObject>* FontObj;
			if (JsonValue->TryGetObject(FontObj))
			{
				FSlateFontInfo* Font = static_cast<FSlateFontInfo*>(ValuePtr);

				// Handle Size with convenient numeric format
				double Size = 0;
				if ((*FontObj)->TryGetNumberField(TEXT("Size"), Size))
				{
					Font->Size = static_cast<float>(Size);
				}

				// Remaining fields: fallback to generic struct reflection
				static UScriptStruct* Struct = FSlateFontInfo::StaticStruct();
				static const FName SizeName = TEXT("Size");
				bool bAllSucceeded = true;
				for (const auto& Pair : (*FontObj)->Values)
				{
					if (Pair.Key == SizeName) continue; // already handled
					if (FProperty* FieldProp = Struct->FindPropertyByName(*Pair.Key))
					{
						void* FieldPtr = FieldProp->ContainerPtrToValuePtr<void>(ValuePtr);
						if (!SetPropertyValueInternal(nullptr, FieldProp, FieldPtr, Pair.Value, MakeChildPropertyPath(TEXT("SlateFontInfo"), FString(*Pair.Key))))
						{
							bAllSucceeded = false;
						}
					}
					else
					{
						UE_LOG(LogAssetDocumentServer, Warning, TEXT("Struct field path 'SlateFontInfo.%s' not found in struct 'SlateFontInfo'"), *Pair.Key);
						bAllSucceeded = false;
					}
				}

				return bAllSucceeded;
			}
			return false;
		});

		// FSlateBrush - handle known fields, then fallback to reflection for the rest
		SafeAdd(FindStructByName(TEXT("SlateBrush")), [](void* ValuePtr, TSharedPtr<FJsonValue> JsonValue) -> bool
		{
			const TSharedPtr<FJsonObject>* BrushObj;
			if (JsonValue->TryGetObject(BrushObj))
			{
				FSlateBrush* Brush = static_cast<FSlateBrush*>(ValuePtr);

				// Handle Image/ResourceObject with convenient string path format
				FString ImagePath;
				if ((*BrushObj)->TryGetStringField(TEXT("Image"), ImagePath) ||
					(*BrushObj)->TryGetStringField(TEXT("ResourceObject"), ImagePath))
				{
					UObject* Resource = LoadObject<UTexture2D>(nullptr, *ImagePath);
					if (!Resource) Resource = LoadObject<UMaterialInterface>(nullptr, *ImagePath);
					if (!Resource) Resource = LoadObject<UObject>(nullptr, *ImagePath);
					if (Resource) Brush->SetResourceObject(Resource);
				}

				// Handle Tint with convenient color format (hex, named, array)
				if ((*BrushObj)->HasField(TEXT("Tint")))
				{
					Brush->TintColor = FSlateColor(ParseColor((*BrushObj)->TryGetField(TEXT("Tint"))));
				}

				// Handle ImageSize with convenient array format
				const TArray<TSharedPtr<FJsonValue>>* ImageSizeArray = nullptr;
				if ((*BrushObj)->TryGetArrayField(TEXT("ImageSize"), ImageSizeArray) && ImageSizeArray->Num() >= 2)
				{
					Brush->ImageSize = ParseVector2D(*ImageSizeArray);
				}

				// Handle DrawAs with convenient string format
				FString DrawAs;
				if ((*BrushObj)->TryGetStringField(TEXT("DrawAs"), DrawAs))
				{
					if (DrawAs == TEXT("Box")) Brush->DrawAs = ESlateBrushDrawType::Box;
					else if (DrawAs == TEXT("Image")) Brush->DrawAs = ESlateBrushDrawType::Image;
					else if (DrawAs == TEXT("Border")) Brush->DrawAs = ESlateBrushDrawType::Border;
					else if (DrawAs == TEXT("NoDrawType")) Brush->DrawAs = ESlateBrushDrawType::NoDrawType;
				}

				// Remaining fields: fallback to generic struct reflection
				static UScriptStruct* Struct = FSlateBrush::StaticStruct();
				static const TSet<FName> HandledFields = {
					TEXT("Image"), TEXT("ResourceObject"), TEXT("Tint"),
					TEXT("ImageSize"), TEXT("DrawAs")
				};
				bool bAllSucceeded = true;
				for (const auto& Pair : (*BrushObj)->Values)
				{
					if (HandledFields.Contains(FName(*Pair.Key))) continue;
					if (FProperty* FieldProp = Struct->FindPropertyByName(*Pair.Key))
					{
						void* FieldPtr = FieldProp->ContainerPtrToValuePtr<void>(ValuePtr);
						if (!SetPropertyValueInternal(nullptr, FieldProp, FieldPtr, Pair.Value, MakeChildPropertyPath(TEXT("SlateBrush"), FString(*Pair.Key))))
						{
							bAllSucceeded = false;
						}
					}
					else
					{
						UE_LOG(LogAssetDocumentServer, Warning, TEXT("Struct field path 'SlateBrush.%s' not found in struct 'SlateBrush'"), *Pair.Key);
						bAllSucceeded = false;
					}
				}

				return bAllSucceeded;
			}
			return false;
		});

		// FAnchors
		SafeAdd(FindStructByName(TEXT("Anchors")), [](void* ValuePtr, TSharedPtr<FJsonValue> JsonValue) -> bool
		{
			const TSharedPtr<FJsonObject>* AnchorsObj;
			if (JsonValue->TryGetObject(AnchorsObj))
			{
				*static_cast<FAnchors*>(ValuePtr) = ParseAnchors(*AnchorsObj);
				return true;
			}
			return false;
		});

		// FGameplayTag - string → RequestGameplayTag (strict: tag must exist)
		SafeAdd(FindStructByName(TEXT("GameplayTag")), [](void* ValuePtr, TSharedPtr<FJsonValue> JsonValue) -> bool
		{
			FGameplayTag* GameplayTag = static_cast<FGameplayTag*>(ValuePtr);
			if (JsonValue->Type == EJson::Null)
			{
				*GameplayTag = FGameplayTag();
				return true;
			}

			FString TagStr;
			if (JsonValue->TryGetString(TagStr))
			{
				if (TagStr.IsEmpty() || TagStr == TEXT("None"))
				{
					*GameplayTag = FGameplayTag();
					return true;
				}

				FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
				if (!Tag.IsValid())
				{
					UE_LOG(LogAssetDocumentServer, Warning, TEXT("FGameplayTag: tag '%s' not found. Register it first (Project Settings > GameplayTags)."), *TagStr);
					return false;
				}
				*GameplayTag = Tag;
				return true;
			}
			return false;
		});

		// FGameplayTagContainer - string array → AddTag each (strict: all tags must exist)
		SafeAdd(FindStructByName(TEXT("GameplayTagContainer")), [](void* ValuePtr, TSharedPtr<FJsonValue> JsonValue) -> bool
		{
			FGameplayTagContainer* Container = static_cast<FGameplayTagContainer*>(ValuePtr);
			Container->Reset();

			const TArray<TSharedPtr<FJsonValue>>* Arr;
			if (JsonValue->TryGetArray(Arr))
			{
				for (const auto& Elem : *Arr)
				{
					FString TagStr;
					if (Elem->TryGetString(TagStr))
					{
						if (TagStr.IsEmpty() || TagStr == TEXT("None"))
						{
							continue;
						}

						FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
						if (!Tag.IsValid())
						{
							UE_LOG(LogAssetDocumentServer, Warning, TEXT("FGameplayTagContainer: tag '%s' not found. Register it first (Project Settings > GameplayTags)."), *TagStr);
							return false;
						}
						Container->AddTag(Tag);
					}
				}
				return true;
			}
			return false;
		});

		// FStateTreeReference - string or { "StateTree": "/Game/Path.Asset" }.
		SafeAdd(FindStructByName(TEXT("StateTreeReference")), [](void* ValuePtr, TSharedPtr<FJsonValue> JsonValue) -> bool
		{
			FStateTreeReference* Reference = static_cast<FStateTreeReference*>(ValuePtr);
			if (JsonValue->Type == EJson::Null)
			{
				Reference->SetStateTree(nullptr);
				return true;
			}

			FString StateTreePath;
			if (!JsonValue->TryGetString(StateTreePath))
			{
				const TSharedPtr<FJsonObject>* Obj;
				if (JsonValue->TryGetObject(Obj))
				{
					(*Obj)->TryGetStringField(TEXT("StateTree"), StateTreePath);
				}
			}

			if (StateTreePath.IsEmpty())
			{
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("FStateTreeReference: expected StateTree asset path"));
				return false;
			}

			UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *StateTreePath);
			if (!StateTree)
			{
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("FStateTreeReference: failed to load StateTree '%s'"), *StateTreePath);
				return false;
			}

			Reference->SetStateTree(StateTree);
			return true;
		});

		// FGameplayAttribute - "ClassName.PropertyName" → SetUProperty
#if WITH_GAMEPLAY_ABILITIES
		SafeAdd(FindStructByName(TEXT("GameplayAttribute")), [](void* ValuePtr, TSharedPtr<FJsonValue> JsonValue) -> bool
		{
			FGameplayAttribute* Attr = static_cast<FGameplayAttribute*>(ValuePtr);
			if (JsonValue->Type == EJson::Null)
			{
				*Attr = FGameplayAttribute();
				return true;
			}

			FString AttrStr;
			if (!JsonValue->TryGetString(AttrStr))
			{
				return false;
			}

			if (AttrStr.IsEmpty() || AttrStr == TEXT("None"))
			{
				*Attr = FGameplayAttribute();
				return true;
			}

			FString ClassName, PropName;
			if (!AttrStr.Split(TEXT("."), &ClassName, &PropName))
			{
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("FGameplayAttribute: expected 'ClassName.PropertyName', got '%s'"), *AttrStr);
				return false;
			}

			UClass* AttrSetClass = FClassFinderUtils::FindClassByName(ClassName, UAttributeSet::StaticClass(), false);
			if (!AttrSetClass)
			{
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("FGameplayAttribute: AttributeSet class '%s' not found"), *ClassName);
				return false;
			}

			FProperty* AttrProp = AttrSetClass->FindPropertyByName(*PropName);
			if (!AttrProp)
			{
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("FGameplayAttribute: property '%s' not found on '%s'"), *PropName, *ClassName);
				return false;
			}

			Attr->SetUProperty(AttrProp);
			return true;
		});
#endif
		bFullyInit = bAllFound;
	}
	return Map;
}

TMap<UScriptStruct*, FPropertySetterUtils::FStructSerializer>& FPropertySetterUtils::GetSpecialSerializers()
{
	static TMap<UScriptStruct*, FStructSerializer> Map;
	static bool bFullyInit = false;
	if (!bFullyInit)
	{
		bool bAllFound = true;

		auto SafeAdd = [&bAllFound](UScriptStruct* Struct, FStructSerializer Handler)
		{
			if (Struct)
			{
				Map.FindOrAdd(Struct) = MoveTemp(Handler);
			}
			else
			{
				bAllFound = false;
			}
		};

		// FTransform - compound nested object {Location:[], Rotation:[], Scale:[]}
		Map.Add(TBaseStructure<FTransform>::Get(), [](const void* ValuePtr) -> TSharedPtr<FJsonValue>
		{
			const FTransform* Trans = static_cast<const FTransform*>(ValuePtr);
			TSharedPtr<FJsonObject> TransObj = MakeShared<FJsonObject>();

			TArray<TSharedPtr<FJsonValue>> LocArray;
			LocArray.Add(MakeShared<FJsonValueNumber>(Trans->GetLocation().X));
			LocArray.Add(MakeShared<FJsonValueNumber>(Trans->GetLocation().Y));
			LocArray.Add(MakeShared<FJsonValueNumber>(Trans->GetLocation().Z));
			TransObj->SetArrayField(TEXT("Location"), LocArray);

			FRotator Rot = Trans->Rotator();
			TArray<TSharedPtr<FJsonValue>> RotArray;
			RotArray.Add(MakeShared<FJsonValueNumber>(Rot.Pitch));
			RotArray.Add(MakeShared<FJsonValueNumber>(Rot.Yaw));
			RotArray.Add(MakeShared<FJsonValueNumber>(Rot.Roll));
			TransObj->SetArrayField(TEXT("Rotation"), RotArray);

			TArray<TSharedPtr<FJsonValue>> ScaleArray;
			ScaleArray.Add(MakeShared<FJsonValueNumber>(Trans->GetScale3D().X));
			ScaleArray.Add(MakeShared<FJsonValueNumber>(Trans->GetScale3D().Y));
			ScaleArray.Add(MakeShared<FJsonValueNumber>(Trans->GetScale3D().Z));
			TransObj->SetArrayField(TEXT("Scale"), ScaleArray);

			return MakeShared<FJsonValueObject>(TransObj);
		});

		// FAnchors - nested object {Min:[], Max:[]}
		SafeAdd(FindStructByName(TEXT("Anchors")), [](const void* ValuePtr) -> TSharedPtr<FJsonValue>
		{
			const FAnchors* Anchors = static_cast<const FAnchors*>(ValuePtr);
			TSharedPtr<FJsonObject> AnchorsObj = MakeShared<FJsonObject>();

			TArray<TSharedPtr<FJsonValue>> MinArray;
			MinArray.Add(MakeShared<FJsonValueNumber>(Anchors->Minimum.X));
			MinArray.Add(MakeShared<FJsonValueNumber>(Anchors->Minimum.Y));
			AnchorsObj->SetArrayField(TEXT("Min"), MinArray);

			TArray<TSharedPtr<FJsonValue>> MaxArray;
			MaxArray.Add(MakeShared<FJsonValueNumber>(Anchors->Maximum.X));
			MaxArray.Add(MakeShared<FJsonValueNumber>(Anchors->Maximum.Y));
			AnchorsObj->SetArrayField(TEXT("Max"), MaxArray);

			return MakeShared<FJsonValueObject>(AnchorsObj);
		});

		// FSoftObjectPath - string
		Map.Add(TBaseStructure<FSoftObjectPath>::Get(), [](const void* ValuePtr) -> TSharedPtr<FJsonValue>
		{
			const FSoftObjectPath* SoftPath = static_cast<const FSoftObjectPath*>(ValuePtr);
			return MakeShared<FJsonValueString>(SoftPath->ToString());
		});

		// FGameplayTag → tag name string
		SafeAdd(FindStructByName(TEXT("GameplayTag")), [](const void* ValuePtr) -> TSharedPtr<FJsonValue>
		{
			const FGameplayTag* Tag = static_cast<const FGameplayTag*>(ValuePtr);
			if (!Tag->IsValid())
			{
				return MakeShared<FJsonValueNull>();
			}
			return MakeShared<FJsonValueString>(Tag->GetTagName().ToString());
		});

		// FGameplayTagContainer → array of tag name strings
		SafeAdd(FindStructByName(TEXT("GameplayTagContainer")), [](const void* ValuePtr) -> TSharedPtr<FJsonValue>
		{
			const FGameplayTagContainer* Container = static_cast<const FGameplayTagContainer*>(ValuePtr);
			TArray<TSharedPtr<FJsonValue>> JsonArray;
			for (const FGameplayTag& Tag : *Container)
			{
				JsonArray.Add(MakeShared<FJsonValueString>(Tag.GetTagName().ToString()));
			}
			return MakeShared<FJsonValueArray>(JsonArray);
		});

		// FGameplayAttribute → "ClassName.PropertyName"
#if WITH_GAMEPLAY_ABILITIES
		SafeAdd(FindStructByName(TEXT("GameplayAttribute")), [](const void* ValuePtr) -> TSharedPtr<FJsonValue>
		{
			const FGameplayAttribute* Attr = static_cast<const FGameplayAttribute*>(ValuePtr);
			if (Attr->IsValid())
			{
				FString Result = Attr->GetAttributeSetClass()->GetName() + TEXT(".") + Attr->GetName();
				return MakeShared<FJsonValueString>(Result);
			}
			return MakeShared<FJsonValueNull>();
		});
#endif
		bFullyInit = bAllFound;
	}
	return Map;
}

bool FPropertySetterUtils::SetStructFromJson(UScriptStruct* Struct, void* ValuePtr, TSharedPtr<FJsonValue> JsonValue)
{
	return SetStructFromJsonInternal(Struct, ValuePtr, JsonValue, nullptr, Struct ? Struct->GetName() : FString());
}

bool FPropertySetterUtils::SetStructFromJsonInternal(UScriptStruct* Struct, void* ValuePtr, TSharedPtr<FJsonValue> JsonValue, UObject* OwnerObject, const FString& PropertyPath)
{
	if (!Struct || !ValuePtr || !JsonValue.IsValid())
	{
		return false;
	}

	// 1. Special format registry
	if (auto* Handler = GetSpecialDeserializers().Find(Struct))
	{
		if ((*Handler)(ValuePtr, JsonValue))
		{
			return true;
		}
	}

	// 2. Generic array mapping (pure numeric structs)
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (JsonValue->TryGetArray(Arr) && IsNumericOnlyStruct(Struct))
	{
		return SetStructFromArray(Struct, ValuePtr, *Arr);
	}

	// 3. Generic object fallback (match fields by name)
	const TSharedPtr<FJsonObject>* Obj;
	if (JsonValue->TryGetObject(Obj))
	{
		bool bAllSucceeded = true;
		for (const auto& Pair : (*Obj)->Values)
		{
			if (FProperty* FieldProp = Struct->FindPropertyByName(*Pair.Key))
			{
				void* FieldPtr = FieldProp->ContainerPtrToValuePtr<void>(ValuePtr);
				const FString FieldPath = MakeChildPropertyPath(PropertyPath, FString(*Pair.Key));
				if (!SetPropertyValueInternal(OwnerObject, FieldProp, FieldPtr, Pair.Value, FieldPath))
				{
					UE_LOG(LogAssetDocumentServer, Warning, TEXT("Failed to set struct property path '%s'"), *FieldPath);
					bAllSucceeded = false;
				}
			}
			else
			{
				const FString FieldPath = MakeChildPropertyPath(PropertyPath, FString(*Pair.Key));
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("Struct field path '%s' not found in struct '%s'"), *FieldPath, *Struct->GetName());
				bAllSucceeded = false;
			}
		}
		return bAllSucceeded;
	}

	return false;
}

bool FPropertySetterUtils::SetStructPropertyFromJson(FStructProperty* StructProp, void* ValuePtr, TSharedPtr<FJsonValue> JsonValue)
{
	if (!StructProp || !ValuePtr || !JsonValue.IsValid())
	{
		return false;
	}

	return SetStructFromJsonInternal(StructProp->Struct, ValuePtr, JsonValue, nullptr, StructProp->GetName());
}

bool FPropertySetterUtils::SetArrayProperty(UObject* Object, FArrayProperty* Property, const TArray<TSharedPtr<FJsonValue>>& ArrayValues)
{
	if (!Object || !Property)
	{
		return false;
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
	FScriptArrayHelper ArrayHelper(Property, ValuePtr);
	FProperty* InnerProp = Property->Inner;

	// Clear existing entries
	ArrayHelper.EmptyValues();

	bool bAllSucceeded = true;
	for (int32 i = 0; i < ArrayValues.Num(); ++i)
	{
		const TSharedPtr<FJsonValue>& JsonValue = ArrayValues[i];

		int32 Index = ArrayHelper.AddValue();
		void* ElementPtr = ArrayHelper.GetRawPtr(Index);

		const FString ElementPath = MakeArrayPropertyPath(Property->GetName(), i);
		if (!SetPropertyValueFromJson(InnerProp, ElementPtr, JsonValue, Object, ElementPath))
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Failed to set array property path '%s'"), *ElementPath);
			bAllSucceeded = false;
		}
	}

	UE_LOG(LogAssetDocumentServer, Verbose, TEXT("Set TArray: %s with %d elements"), *Property->GetName(), ArrayHelper.Num());
	return bAllSucceeded;
}

bool FPropertySetterUtils::SetMapProperty(UObject* Object, FMapProperty* Property, TSharedPtr<FJsonObject> MapConfig)
{
	if (!Object || !Property || !MapConfig.IsValid())
	{
		return false;
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
	FScriptMapHelper MapHelper(Property, ValuePtr);

	FProperty* KeyProp = Property->KeyProp;
	FProperty* ValueProp = Property->ValueProp;

	// Clear existing entries
	MapHelper.EmptyValues();

	bool bAllSucceeded = true;
	for (const auto& Pair : MapConfig->Values)
	{
		const FString KeyStr(*Pair.Key);
		const TSharedPtr<FJsonValue>& JsonValue = Pair.Value;

		// Add a new entry
		int32 Index = MapHelper.AddDefaultValue_Invalid_NeedsRehash();

		// Set the key
		void* KeyPtr = MapHelper.GetKeyPtr(Index);

		// Handle enum keys
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(KeyProp))
		{
			UEnum* Enum = EnumProp->GetEnum();
			int64 EnumValue = Enum->GetValueByNameString(KeyStr);
			if (EnumValue == INDEX_NONE)
			{
				FString FullName = Enum->GetName() + TEXT("::") + KeyStr;
				EnumValue = Enum->GetValueByNameString(FullName);
			}
			if (EnumValue != INDEX_NONE)
			{
				EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(KeyPtr, EnumValue);
			}
			else
			{
				UE_LOG(LogAssetDocumentServer, Warning, TEXT("Unknown enum value '%s' for enum '%s'"), *KeyStr, *Enum->GetName());
				MapHelper.RemoveAt(Index);
				bAllSucceeded = false;
				continue;
			}
		}
		else if (FByteProperty* ByteProp = CastField<FByteProperty>(KeyProp))
		{
			if (UEnum* Enum = ByteProp->Enum)
			{
				int64 EnumValue = Enum->GetValueByNameString(KeyStr);
				if (EnumValue != INDEX_NONE)
				{
					ByteProp->SetIntPropertyValue(KeyPtr, EnumValue);
				}
				else
				{
					MapHelper.RemoveAt(Index);
					bAllSucceeded = false;
					continue;
				}
			}
			else
			{
				ByteProp->SetIntPropertyValue(KeyPtr, static_cast<int64>(FCString::Atoi(*KeyStr)));
			}
		}
		else if (FStrProperty* StrProp = CastField<FStrProperty>(KeyProp))
		{
			StrProp->SetPropertyValue(KeyPtr, KeyStr);
		}
		else if (FNameProperty* NameProp = CastField<FNameProperty>(KeyProp))
		{
			NameProp->SetPropertyValue(KeyPtr, FName(*KeyStr));
		}
		else
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Unsupported map key type: %s"), *KeyProp->GetClass()->GetName());
			MapHelper.RemoveAt(Index);
			bAllSucceeded = false;
			continue;
		}

		// Set the value (recursive — supports nested containers)
		void* ValPtr = MapHelper.GetValuePtr(Index);
		const FString ValuePath = MakeMapPropertyPath(Property->GetName(), KeyStr);
		if (!SetPropertyValueFromJson(ValueProp, ValPtr, JsonValue, Object, ValuePath))
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Failed to set map property path '%s'"), *ValuePath);
			bAllSucceeded = false;
		}
	}

	MapHelper.Rehash();

	UE_LOG(LogAssetDocumentServer, Verbose, TEXT("Set TMap: %s with %d entries"), *Property->GetName(), MapHelper.Num());
	return bAllSucceeded;
}

bool FPropertySetterUtils::SetObjectReferenceFromPath(FObjectPropertyBase* ObjProp, void* ValuePtr, const FString& ObjectPath)
{
	if (!ObjProp || !ValuePtr)
	{
		return false;
	}

	if (ObjectPath.IsEmpty())
	{
		ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
		return true;
	}

	UObject* LoadedObject = StaticLoadObject(ObjProp->PropertyClass, nullptr, *ObjectPath);
	if (LoadedObject)
	{
		ObjProp->SetObjectPropertyValue(ValuePtr, LoadedObject);
		return true;
	}

	UE_LOG(LogAssetDocumentServer, Warning, TEXT("Failed to load object: %s"), *ObjectPath);
	return false;
}

bool FPropertySetterUtils::SetSoftObjectProperty(UObject* Object, FSoftObjectProperty* Property, const FString& AssetPath)
{
	if (!Object || !Property || AssetPath.IsEmpty())
	{
		return false;
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);

	// Normalize the asset path
	FString NormalizedPath = AssetPath;
	if (!NormalizedPath.Contains(TEXT(".")))
	{
		// Add asset name if not present (e.g., "/Game/UI/Mat" -> "/Game/UI/Mat.Mat")
		FString AssetName = FPaths::GetBaseFilename(NormalizedPath);
		NormalizedPath = NormalizedPath + TEXT(".") + AssetName;
	}

	FSoftObjectPath SoftPath(NormalizedPath);
	FSoftObjectPtr SoftPtr(SoftPath);
	Property->SetPropertyValue(ValuePtr, SoftPtr);

	UE_LOG(LogAssetDocumentServer, Verbose, TEXT("Set TSoftObjectPtr: %s = %s"), *Property->GetName(), *NormalizedPath);
	return true;
}

bool FPropertySetterUtils::SetClassProperty(UObject* Object, FClassProperty* Property, const FString& ClassPath)
{
	if (!Object || !Property || ClassPath.IsEmpty())
	{
		return false;
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);

	UClass* LoadedClass = ResolveClassPropertyPath(ClassPath);

	if (LoadedClass)
	{
		// Verify the class is compatible with the property's meta class
		UClass* MetaClass = Property->MetaClass;
		if (MetaClass && !LoadedClass->IsChildOf(MetaClass))
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Class '%s' is not a subclass of '%s'"),
				*LoadedClass->GetName(), *MetaClass->GetName());
			return false;
		}

		Property->SetPropertyValue(ValuePtr, LoadedClass);
		UE_LOG(LogAssetDocumentServer, Verbose, TEXT("Set TSubclassOf: %s = %s"), *Property->GetName(), *LoadedClass->GetName());
		return true;
	}

	UE_LOG(LogAssetDocumentServer, Warning, TEXT("Failed to load class: %s"), *ClassPath);
	return false;
}

//~ Parse Helpers

FLinearColor FPropertySetterUtils::ParseColor(TSharedPtr<FJsonValue> ColorValue)
{
	FLinearColor Result = FLinearColor::White;

	if (!ColorValue.IsValid())
	{
		return Result;
	}

	// String format - hex or named
	FString ColorStr;
	if (ColorValue->TryGetString(ColorStr))
	{
		// Hex format
		if (ColorStr.StartsWith(TEXT("#")))
		{
			FColor ParsedColor = FColor::FromHex(ColorStr);
			return FLinearColor(ParsedColor);
		}

		// Named colors
		if (ColorStr == TEXT("White")) return FLinearColor::White;
		if (ColorStr == TEXT("Black")) return FLinearColor::Black;
		if (ColorStr == TEXT("Red")) return FLinearColor::Red;
		if (ColorStr == TEXT("Green")) return FLinearColor::Green;
		if (ColorStr == TEXT("Blue")) return FLinearColor::Blue;
		if (ColorStr == TEXT("Yellow")) return FLinearColor::Yellow;
		if (ColorStr == TEXT("Transparent")) return FLinearColor::Transparent;
	}

	// Array format [R, G, B, A]
	const TArray<TSharedPtr<FJsonValue>>* ColorArray = nullptr;
	if (ColorValue->TryGetArray(ColorArray))
	{
		if (ColorArray->Num() >= 3)
		{
			double R = 1, G = 1, B = 1, A = 1;
			(*ColorArray)[0]->TryGetNumber(R);
			(*ColorArray)[1]->TryGetNumber(G);
			(*ColorArray)[2]->TryGetNumber(B);
			if (ColorArray->Num() >= 4)
			{
				(*ColorArray)[3]->TryGetNumber(A);
			}
			return FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
		}
	}

	// Object format {R, G, B, A}
	const TSharedPtr<FJsonObject>* ColorObject = nullptr;
	if (ColorValue->TryGetObject(ColorObject))
	{
		double R = 1, G = 1, B = 1, A = 1;
		(*ColorObject)->TryGetNumberField(TEXT("R"), R);
		(*ColorObject)->TryGetNumberField(TEXT("G"), G);
		(*ColorObject)->TryGetNumberField(TEXT("B"), B);
		(*ColorObject)->TryGetNumberField(TEXT("A"), A);
		return FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
	}

	return Result;
}

FVector2D FPropertySetterUtils::ParseVector2D(const TArray<TSharedPtr<FJsonValue>>& Array)
{
	FVector2D Result(0.0f, 0.0f);

	if (Array.Num() >= 2)
	{
		double X = 0, Y = 0;
		Array[0]->TryGetNumber(X);
		Array[1]->TryGetNumber(Y);
		Result = FVector2D(static_cast<float>(X), static_cast<float>(Y));
	}

	return Result;
}

FVector FPropertySetterUtils::ParseVector(const TArray<TSharedPtr<FJsonValue>>& Array)
{
	FVector Result(0.0f, 0.0f, 0.0f);

	if (Array.Num() >= 3)
	{
		double X = 0, Y = 0, Z = 0;
		Array[0]->TryGetNumber(X);
		Array[1]->TryGetNumber(Y);
		Array[2]->TryGetNumber(Z);
		Result = FVector(X, Y, Z);
	}

	return Result;
}

FMargin FPropertySetterUtils::ParseMargin(TSharedPtr<FJsonValue> MarginsValue)
{
	FMargin Result(0.0f);

	if (!MarginsValue.IsValid())
	{
		return Result;
	}

	// Single number - uniform margin
	double UniformValue = 0;
	if (MarginsValue->TryGetNumber(UniformValue))
	{
		return FMargin(static_cast<float>(UniformValue));
	}

	// Array format [Left, Top, Right, Bottom] or [H, V]
	const TArray<TSharedPtr<FJsonValue>>* MarginsArray = nullptr;
	if (MarginsValue->TryGetArray(MarginsArray))
	{
		if (MarginsArray->Num() >= 4)
		{
			double Left = 0, Top = 0, Right = 0, Bottom = 0;
			(*MarginsArray)[0]->TryGetNumber(Left);
			(*MarginsArray)[1]->TryGetNumber(Top);
			(*MarginsArray)[2]->TryGetNumber(Right);
			(*MarginsArray)[3]->TryGetNumber(Bottom);
			return FMargin(static_cast<float>(Left), static_cast<float>(Top), static_cast<float>(Right), static_cast<float>(Bottom));
		}
		else if (MarginsArray->Num() >= 2)
		{
			double H = 0, V = 0;
			(*MarginsArray)[0]->TryGetNumber(H);
			(*MarginsArray)[1]->TryGetNumber(V);
			return FMargin(static_cast<float>(H), static_cast<float>(V));
		}
	}

	// Object format {Left, Top, Right, Bottom}
	const TSharedPtr<FJsonObject>* MarginsObject = nullptr;
	if (MarginsValue->TryGetObject(MarginsObject))
	{
		double Left = 0, Top = 0, Right = 0, Bottom = 0;
		(*MarginsObject)->TryGetNumberField(TEXT("Left"), Left);
		(*MarginsObject)->TryGetNumberField(TEXT("Top"), Top);
		(*MarginsObject)->TryGetNumberField(TEXT("Right"), Right);
		(*MarginsObject)->TryGetNumberField(TEXT("Bottom"), Bottom);
		return FMargin(static_cast<float>(Left), static_cast<float>(Top), static_cast<float>(Right), static_cast<float>(Bottom));
	}

	return Result;
}

FSlateFontInfo FPropertySetterUtils::ParseFont(TSharedPtr<FJsonObject> FontConfig)
{
	FSlateFontInfo Font;

	if (FontConfig.IsValid())
	{
		double Size = 12;
		if (FontConfig->TryGetNumberField(TEXT("Size"), Size))
		{
			Font.Size = static_cast<int32>(Size);
		}

		// Font family handling would need font asset loading
		// For now, just use the default font with specified size
	}

	return Font;
}

FSlateBrush FPropertySetterUtils::ParseBrush(TSharedPtr<FJsonObject> BrushConfig)
{
	FSlateBrush Brush;

	if (!BrushConfig.IsValid())
	{
		return Brush;
	}

	// Load image resource (Texture2D, Material, or other UObject)
	FString ImagePath;
	if (BrushConfig->TryGetStringField(TEXT("Image"), ImagePath) ||
		BrushConfig->TryGetStringField(TEXT("ResourceObject"), ImagePath))
	{
		UObject* Resource = LoadObject<UTexture2D>(nullptr, *ImagePath);
		if (!Resource)
		{
			Resource = LoadObject<UMaterialInterface>(nullptr, *ImagePath);
		}
		if (!Resource)
		{
			Resource = LoadObject<UObject>(nullptr, *ImagePath);
		}

		if (Resource)
		{
			Brush.SetResourceObject(Resource);
		}
		else
		{
			UE_LOG(LogAssetDocumentServer, Warning, TEXT("Failed to load brush resource: %s"), *ImagePath);
		}
	}

	// Tint color
	if (BrushConfig->HasField(TEXT("Tint")))
	{
		FLinearColor Tint = ParseColor(BrushConfig->TryGetField(TEXT("Tint")));
		Brush.TintColor = FSlateColor(Tint);
	}

	// Image size
	const TArray<TSharedPtr<FJsonValue>>* ImageSizeArray = nullptr;
	if (BrushConfig->TryGetArrayField(TEXT("ImageSize"), ImageSizeArray) && ImageSizeArray->Num() >= 2)
	{
		Brush.ImageSize = ParseVector2D(*ImageSizeArray);
	}

	// Draw type
	FString DrawAs;
	if (BrushConfig->TryGetStringField(TEXT("DrawAs"), DrawAs))
	{
		if (DrawAs == TEXT("Box")) Brush.DrawAs = ESlateBrushDrawType::Box;
		else if (DrawAs == TEXT("Image")) Brush.DrawAs = ESlateBrushDrawType::Image;
		else if (DrawAs == TEXT("Border")) Brush.DrawAs = ESlateBrushDrawType::Border;
		else if (DrawAs == TEXT("NoDrawType")) Brush.DrawAs = ESlateBrushDrawType::NoDrawType;
	}

	return Brush;
}

FAnchors FPropertySetterUtils::ParseAnchors(TSharedPtr<FJsonObject> AnchorsConfig)
{
	FAnchors Result;

	if (!AnchorsConfig.IsValid())
	{
		return Result;
	}

	// Array format: {"Min": [0.5, 0.5], "Max": [0.5, 0.5]}
	const TArray<TSharedPtr<FJsonValue>>* MinArray = nullptr;
	if (AnchorsConfig->TryGetArrayField(TEXT("Min"), MinArray) && MinArray->Num() >= 2)
	{
		Result.Minimum = ParseVector2D(*MinArray);
	}
	else if (AnchorsConfig->TryGetArrayField(TEXT("Minimum"), MinArray) && MinArray->Num() >= 2)
	{
		Result.Minimum = ParseVector2D(*MinArray);
	}

	const TArray<TSharedPtr<FJsonValue>>* MaxArray = nullptr;
	if (AnchorsConfig->TryGetArrayField(TEXT("Max"), MaxArray) && MaxArray->Num() >= 2)
	{
		Result.Maximum = ParseVector2D(*MaxArray);
	}
	else if (AnchorsConfig->TryGetArrayField(TEXT("Maximum"), MaxArray) && MaxArray->Num() >= 2)
	{
		Result.Maximum = ParseVector2D(*MaxArray);
	}

	// Object format: {"Minimum": {"X": 0.5, "Y": 0.5}, "Maximum": {"X": 0.5, "Y": 0.5}}
	TSharedPtr<FJsonObject> MinObj;
	if (AnchorsConfig->HasTypedField<EJson::Object>(TEXT("Minimum")))
	{
		MinObj = AnchorsConfig->GetObjectField(TEXT("Minimum"));
	}
	if (!MinObj.IsValid() && AnchorsConfig->HasTypedField<EJson::Object>(TEXT("Min")))
	{
		MinObj = AnchorsConfig->GetObjectField(TEXT("Min"));
	}
	if (MinObj.IsValid())
	{
		double X = 0, Y = 0;
		MinObj->TryGetNumberField(TEXT("X"), X);
		MinObj->TryGetNumberField(TEXT("Y"), Y);
		Result.Minimum = FVector2D(X, Y);
	}

	TSharedPtr<FJsonObject> MaxObj;
	if (AnchorsConfig->HasTypedField<EJson::Object>(TEXT("Maximum")))
	{
		MaxObj = AnchorsConfig->GetObjectField(TEXT("Maximum"));
	}
	if (!MaxObj.IsValid() && AnchorsConfig->HasTypedField<EJson::Object>(TEXT("Max")))
	{
		MaxObj = AnchorsConfig->GetObjectField(TEXT("Max"));
	}
	if (MaxObj.IsValid())
	{
		double X = 0, Y = 0;
		MaxObj->TryGetNumberField(TEXT("X"), X);
		MaxObj->TryGetNumberField(TEXT("Y"), Y);
		Result.Maximum = FVector2D(X, Y);
	}

	return Result;
}

//////////////////////////////////////////////////////////////////////////
// Extract Properties (Property → JSON)
//////////////////////////////////////////////////////////////////////////

TSharedPtr<FJsonValue> FPropertySetterUtils::ExtractPropertyToJson(FProperty* Property, const void* ValuePtr)
{
	if (!Property || !ValuePtr)
	{
		return nullptr;
	}

	// Enum (FEnumProperty — UE5 native enum)
	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		UEnum* Enum = EnumProp->GetEnum();
		FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
		int64 EnumValue = UnderlyingProp->GetSignedIntPropertyValue(ValuePtr);
		FString EnumName = Enum->GetNameStringByValue(EnumValue);
		return MakeShared<FJsonValueString>(EnumName);
	}

	// Byte enum (TEnumAsByte — must check BEFORE FNumericProperty since FByteProperty is a subclass)
	if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		if (UEnum* Enum = ByteProp->Enum)
		{
			uint8 ByteValue = ByteProp->GetPropertyValue(ValuePtr);
			FString EnumName = Enum->GetNameStringByValue(ByteValue);
			return MakeShared<FJsonValueString>(EnumName);
		}
		else
		{
			return MakeShared<FJsonValueNumber>(ByteProp->GetPropertyValue(ValuePtr));
		}
	}

	// Numeric properties
	if (FNumericProperty* NumProp = CastField<FNumericProperty>(Property))
	{
		if (NumProp->IsFloatingPoint())
		{
			return MakeShared<FJsonValueNumber>(NumProp->GetFloatingPointPropertyValue(ValuePtr));
		}
		else
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtr)));
		}
	}

	// Bool
	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
	}

	// String
	if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
	}

	// Name
	if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
	{
		return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(ValuePtr).ToString());
	}

	// Text
	if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
	{
		return MakeShared<FJsonValueString>(TextProp->GetPropertyValue(ValuePtr).ToString());
	}

	// Object reference
	if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
	{
		UObject* ObjValue = ObjProp->GetObjectPropertyValue(ValuePtr);
		if (ObjValue)
		{
			// Instanced subobject: Outer is not a UPackage → extract Class + Properties recursively
			if (ObjValue->GetOuter() && !ObjValue->GetOuter()->IsA<UPackage>())
			{
				TSharedPtr<FJsonObject> SubObj = MakeShared<FJsonObject>();
				SubObj->SetStringField(TEXT("Class"), ObjValue->GetClass()->GetName());
				TSharedPtr<FJsonObject> Props = ExtractPropertiesToJson(ObjValue, true, true);
				if (Props.IsValid() && Props->Values.Num() > 0)
				{
					SubObj->SetObjectField(TEXT("Properties"), Props);
				}
				return MakeShared<FJsonValueObject>(SubObj);
			}
			return MakeShared<FJsonValueString>(ObjValue->GetPathName());
		}
		return MakeShared<FJsonValueNull>();
	}

	// Soft object reference
	if (FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Property))
	{
		const FSoftObjectPtr* SoftPtr = static_cast<const FSoftObjectPtr*>(ValuePtr);
		if (!SoftPtr->IsNull())
		{
			return MakeShared<FJsonValueString>(SoftPtr->ToSoftObjectPath().ToString());
		}
		return MakeShared<FJsonValueNull>();
	}

	// Class reference
	if (FClassProperty* ClassProp = CastField<FClassProperty>(Property))
	{
		UClass* ClassValue = Cast<UClass>(ClassProp->GetObjectPropertyValue(ValuePtr));
		if (ClassValue)
		{
			return MakeShared<FJsonValueString>(ClassValue->GetPathName());
		}
		return MakeShared<FJsonValueNull>();
	}

	// Soft class reference
	if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Property))
	{
		const FSoftObjectPtr* SoftPtr = static_cast<const FSoftObjectPtr*>(ValuePtr);
		if (!SoftPtr->IsNull())
		{
			return MakeShared<FJsonValueString>(SoftPtr->ToSoftObjectPath().ToString());
		}
		return MakeShared<FJsonValueNull>();
	}

	// Struct
	if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		return ExtractStructToJson(StructProp, ValuePtr);
	}

	// Array
	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		return ExtractArrayToJson(ArrayProp, ValuePtr);
	}

	// Map
	if (FMapProperty* MapProp = CastField<FMapProperty>(Property))
	{
		return ExtractMapToJson(MapProp, ValuePtr);
	}

	// Set - treat as array
	if (FSetProperty* SetProp = CastField<FSetProperty>(Property))
	{
		FScriptSetHelper SetHelper(SetProp, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> JsonArray;

		for (int32 i = 0; i < SetHelper.Num(); ++i)
		{
			if (SetHelper.IsValidIndex(i))
			{
				const void* ElemPtr = SetHelper.GetElementPtr(i);
				TSharedPtr<FJsonValue> ElemJson = ExtractPropertyToJson(SetProp->ElementProp, ElemPtr);
				if (ElemJson.IsValid())
				{
					JsonArray.Add(ElemJson);
				}
			}
		}

		return MakeShared<FJsonValueArray>(JsonArray);
	}

	UE_LOG(LogAssetDocumentServer, Verbose, TEXT("Unsupported property type for extraction: %s (%s)"),
		*Property->GetName(), *Property->GetClass()->GetName());
	return nullptr;
}

TSharedPtr<FJsonObject> FPropertySetterUtils::ExtractPropertiesToJson(UObject* Object, bool bEditableOnly, bool bSkipDefaults)
{
	if (!Object)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> PropertiesJson = MakeShared<FJsonObject>();
	UClass* ObjectClass = Object->GetClass();
	UObject* CDO = bSkipDefaults ? ObjectClass->GetDefaultObject() : nullptr;

	for (TFieldIterator<FProperty> PropIt(ObjectClass); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;

		// Skip non-editable if requested
		if (bEditableOnly && !Property->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		// Skip deprecated properties
		if (Property->HasAnyPropertyFlags(CPF_Deprecated))
		{
			continue;
		}

		FString PropertyName = Property->GetName();
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);

		// Skip if same as default
		if (bSkipDefaults && CDO)
		{
			const void* DefaultPtr = Property->ContainerPtrToValuePtr<void>(CDO);
			if (Property->Identical(ValuePtr, DefaultPtr))
			{
				continue;
			}
		}

		TSharedPtr<FJsonValue> JsonValue = ExtractPropertyToJson(Property, ValuePtr);
		if (JsonValue.IsValid())
		{
			PropertiesJson->SetField(PropertyName, JsonValue);
		}
	}

	return PropertiesJson;
}

TSharedPtr<FJsonValue> FPropertySetterUtils::ExtractStructToJson(FStructProperty* StructProp, const void* ValuePtr)
{
	if (!StructProp || !ValuePtr)
	{
		return nullptr;
	}

	UScriptStruct* Struct = StructProp->Struct;

	// 1. Special format registry
	if (auto* Handler = GetSpecialSerializers().Find(Struct))
	{
		return (*Handler)(ValuePtr);
	}

	// 2. Generic array (pure numeric structs)
	if (IsNumericOnlyStruct(Struct))
	{
		return ExtractStructToArray(Struct, ValuePtr);
	}

	// 3. Generic object fallback
	TSharedPtr<FJsonObject> StructObj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const void* FieldPtr = (*It)->ContainerPtrToValuePtr<void>(ValuePtr);
		if (auto FieldJson = ExtractPropertyToJson(*It, FieldPtr))
		{
			StructObj->SetField((*It)->GetName(), FieldJson);
		}
	}
	return MakeShared<FJsonValueObject>(StructObj);
}

TSharedPtr<FJsonValue> FPropertySetterUtils::ExtractArrayToJson(FArrayProperty* ArrayProp, const void* ValuePtr)
{
	if (!ArrayProp || !ValuePtr)
	{
		return nullptr;
	}

	FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
	TArray<TSharedPtr<FJsonValue>> JsonArray;

	FProperty* InnerProp = ArrayProp->Inner;

	for (int32 i = 0; i < ArrayHelper.Num(); ++i)
	{
		const void* ElemPtr = ArrayHelper.GetRawPtr(i);
		TSharedPtr<FJsonValue> ElemJson = ExtractPropertyToJson(InnerProp, ElemPtr);
		if (ElemJson.IsValid())
		{
			JsonArray.Add(ElemJson);
		}
	}

	return MakeShared<FJsonValueArray>(JsonArray);
}

TSharedPtr<FJsonValue> FPropertySetterUtils::ExtractMapToJson(FMapProperty* MapProp, const void* ValuePtr)
{
	if (!MapProp || !ValuePtr)
	{
		return nullptr;
	}

	FScriptMapHelper MapHelper(MapProp, ValuePtr);
	TSharedPtr<FJsonObject> MapObj = MakeShared<FJsonObject>();

	for (int32 i = 0; i < MapHelper.Num(); ++i)
	{
		if (MapHelper.IsValidIndex(i))
		{
			const void* KeyPtr = MapHelper.GetKeyPtr(i);
			const void* ValPtr = MapHelper.GetValuePtr(i);

			// Get key as string
			FString KeyStr;
			FProperty* KeyProp = MapProp->KeyProp;

			if (FEnumProperty* EnumProp = CastField<FEnumProperty>(KeyProp))
			{
				UEnum* Enum = EnumProp->GetEnum();
				int64 Val = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(KeyPtr);
				KeyStr = Enum->GetNameStringByValue(Val);
			}
			else if (FByteProperty* ByteProp = CastField<FByteProperty>(KeyProp))
			{
				if (UEnum* Enum = ByteProp->Enum)
				{
					KeyStr = Enum->GetNameStringByValue(static_cast<int64>(ByteProp->GetPropertyValue(KeyPtr)));
				}
				else
				{
					KeyStr = FString::FromInt(static_cast<int32>(ByteProp->GetPropertyValue(KeyPtr)));
				}
			}
			else if (FStrProperty* StrProp = CastField<FStrProperty>(KeyProp))
			{
				KeyStr = StrProp->GetPropertyValue(KeyPtr);
			}
			else if (FNameProperty* NameProp = CastField<FNameProperty>(KeyProp))
			{
				KeyStr = NameProp->GetPropertyValue(KeyPtr).ToString();
			}
			else if (FNumericProperty* NumProp = CastField<FNumericProperty>(KeyProp))
			{
				if (NumProp->IsFloatingPoint())
				{
					KeyStr = FString::SanitizeFloat(NumProp->GetFloatingPointPropertyValue(KeyPtr));
				}
				else
				{
					KeyStr = FString::FromInt(static_cast<int32>(NumProp->GetSignedIntPropertyValue(KeyPtr)));
				}
			}
			else
			{
				// Fallback: use index
				KeyStr = FString::FromInt(i);
			}

			TSharedPtr<FJsonValue> ValJson = ExtractPropertyToJson(MapProp->ValueProp, ValPtr);
			if (ValJson.IsValid())
			{
				MapObj->SetField(KeyStr, ValJson);
			}
		}
	}

	return MakeShared<FJsonValueObject>(MapObj);
}
