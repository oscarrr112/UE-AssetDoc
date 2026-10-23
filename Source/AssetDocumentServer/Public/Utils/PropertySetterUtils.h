// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "UObject/UnrealType.h"
#include <type_traits>
#include "Layout/Margin.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateBrush.h"
#include "Widgets/Layout/Anchors.h"

/**
 * Parsed type information from type string like "Object:UStaticMesh" or "Array:Float"
 */
struct ASSETDOCUMENTSERVER_API FParsedTypeInfo
{
	/** Base type category: Bool, Int, Float, String, FVector, Object, Array, Map, Enum, Struct, etc. */
	FString BaseType;

	/** Sub type for Object/SoftObject/Class/Enum/Struct (e.g., "UStaticMesh", "ECollisionChannel", "FMyStruct") */
	FString SubType;

	/** Element type for Array (e.g., "Float", "Object:UStaticMesh") */
	FString ElementType;

	/** Key type for Map */
	FString KeyType;

	/** Value type for Map */
	FString ValueType;

	/** Whether parsing was successful */
	bool bIsValid = false;

	/** Error message if parsing failed */
	FString ErrorMessage;
};

/**
 * Result of property validation
 */
struct ASSETDOCUMENTSERVER_API FPropertyValidationResult
{
	bool bIsValid = false;
	FString ErrorMessage;

	static FPropertyValidationResult Success() { return { true, TEXT("") }; }
	static FPropertyValidationResult Failure(const FString& Error) { return { false, Error }; }
};

/**
 * Utility class for setting UObject properties from JSON values (AssetDocument's property adapter uses it).
 *
 * Supported property types:
 * - Numeric (int8-64, uint8-64, float, double)
 * - Bool
 * - String / FName / FText
 * - Enum / ByteEnum
 * - Struct (FLinearColor, FColor, FVector, FVector2D, FMargin, FSlateColor, FSlateFontInfo, FSlateBrush, FAnchors + generic)
 * - Object Reference / SoftObject / Class
 * - Array / Map
 */
class ASSETDOCUMENTSERVER_API FPropertySetterUtils
{
public:
	//~ New Typed Property System (with explicit { "type": "...", "value": ... } format)

	/**
	 * Parse a type string into structured type info
	 * Examples: "Float", "FVector", "Object:UStaticMesh", "Array:Float", "Map:String:Int", "Enum:ECollisionChannel"
	 */
	static FParsedTypeInfo ParseTypeString(const FString& TypeString);

	/**
	 * Validate that a JSON value matches the expected type
	 * @param TypeInfo - Parsed type information
	 * @param Value - The JSON value to validate
	 * @return Validation result with success/failure and error message
	 */
	static FPropertyValidationResult ValidateTypedValue(const FParsedTypeInfo& TypeInfo, TSharedPtr<FJsonValue> Value);

	/**
	 * Validate all properties in a typed properties JSON object
	 * Each property should be { "type": "...", "value": ... }
	 * @param Properties - JSON object containing typed property definitions
	 * @param OutErrors - Array to collect validation errors
	 * @return true if all properties are valid
	 */
	static bool ValidateTypedProperties(TSharedPtr<FJsonObject> Properties, TArray<FString>& OutErrors);

	/**
	 * Set a single property using typed format { "type": "...", "value": ... }
	 * @param Object - The UObject containing the property
	 * @param PropertyName - Name of the property to set
	 * @param TypedValue - JSON object with "type" and "value" fields
	 * @return true if the property was set successfully
	 */
	static bool SetTypedPropertyFromJson(UObject* Object, const FString& PropertyName, TSharedPtr<FJsonObject> TypedValue);

	/**
	 * Set multiple properties using typed format
	 * @param Object - The UObject to set properties on
	 * @param Properties - JSON object where each value is { "type": "...", "value": ... }
	 * @return true if all properties were set successfully
	 */
	static bool SetTypedPropertiesFromJson(UObject* Object, TSharedPtr<FJsonObject> Properties);

	/**
	 * Set a value directly using parsed type info (for nested/recursive calls)
	 * @param ValuePtr - Pointer to the memory location to set
	 * @param TypeInfo - Parsed type information
	 * @param JsonValue - The JSON value containing the data
	 * @return true if successful
	 */
	static bool SetValueFromTypedJson(void* ValuePtr, const FParsedTypeInfo& TypeInfo, TSharedPtr<FJsonValue> JsonValue);

	//~ Legacy Property System (auto-detect types from property reflection)

	/**
	 * Set a single property from a JSON value
	 * @param Object - The UObject containing the property
	 * @param Property - The property to set
	 * @param JsonValue - The JSON value to use
	 * @return true if the property was set successfully
	 */
	static bool SetPropertyFromJson(UObject* Object, FProperty* Property, TSharedPtr<FJsonValue> JsonValue);

	/**
	 * Set multiple properties from a JSON object
	 * @param Object - The UObject to set properties on
	 * @param Properties - JSON object containing property name/value pairs
	 * @return true if all properties were set successfully
	 */
	static bool SetPropertiesFromJson(UObject* Object, TSharedPtr<FJsonObject> Properties);

	/**
	 * Set a struct property from a JSON value
	 * @param StructProp - The struct property
	 * @param ValuePtr - Pointer to the struct value
	 * @param JsonValue - The JSON value to use
	 * @return true if the property was set successfully
	 */
	static bool SetStructPropertyFromJson(FStructProperty* StructProp, void* ValuePtr, TSharedPtr<FJsonValue> JsonValue);

	/**
	 * Set an array property from JSON array values
	 * @param Object - The UObject containing the property
	 * @param ArrayProp - The array property
	 * @param ArrayValues - Array of JSON values
	 * @return true if the property was set successfully
	 */
	static bool SetArrayProperty(UObject* Object, FArrayProperty* ArrayProp, const TArray<TSharedPtr<FJsonValue>>& ArrayValues);

	/**
	 * Set a map property from a JSON object
	 * @param Object - The UObject containing the property
	 * @param MapProp - The map property
	 * @param MapConfig - JSON object representing the map
	 * @return true if the property was set successfully
	 */
	static bool SetMapProperty(UObject* Object, FMapProperty* MapProp, TSharedPtr<FJsonObject> MapConfig);

	/**
	 * Set an object reference property from an asset path
	 * @param ObjProp - The object property
	 * @param ValuePtr - Pointer to the property value
	 * @param ObjectPath - Asset path string
	 * @return true if the property was set successfully
	 */
	static bool SetObjectReferenceFromPath(FObjectPropertyBase* ObjProp, void* ValuePtr, const FString& ObjectPath);

	/**
	 * Set a soft object property from an asset path
	 * @param Object - The UObject containing the property
	 * @param Property - The soft object property
	 * @param AssetPath - Asset path string
	 * @return true if the property was set successfully
	 */
	static bool SetSoftObjectProperty(UObject* Object, FSoftObjectProperty* Property, const FString& AssetPath);

	/**
	 * Set a class property from a class path
	 * @param Object - The UObject containing the property
	 * @param Property - The class property
	 * @param ClassPath - Class path string
	 * @return true if the property was set successfully
	 */
	static bool SetClassProperty(UObject* Object, FClassProperty* Property, const FString& ClassPath);

	//~ Extract Properties (Property → JSON, reverse of Set)

	/**
	 * Extract a single property value to JSON
	 * @param Property - The property to extract
	 * @param ValuePtr - Pointer to the property value
	 * @return JSON value representing the property, or nullptr if unsupported
	 */
	static TSharedPtr<FJsonValue> ExtractPropertyToJson(FProperty* Property, const void* ValuePtr);

	/**
	 * Extract all editable properties from a UObject to JSON
	 * @param Object - The UObject to extract properties from
	 * @param bEditableOnly - If true, only extract properties with CPF_Edit flag
	 * @param bSkipDefaults - If true, skip properties that match CDO defaults
	 * @return JSON object containing property name/value pairs
	 */
	static TSharedPtr<FJsonObject> ExtractPropertiesToJson(UObject* Object, bool bEditableOnly = true, bool bSkipDefaults = false);

	/**
	 * Extract a struct value to JSON
	 * @param StructProp - The struct property
	 * @param ValuePtr - Pointer to the struct value
	 * @return JSON value representing the struct
	 */
	static TSharedPtr<FJsonValue> ExtractStructToJson(FStructProperty* StructProp, const void* ValuePtr);

	/**
	 * Extract an array property to JSON
	 * @param ArrayProp - The array property
	 * @param ValuePtr - Pointer to the array value
	 * @return JSON array value
	 */
	static TSharedPtr<FJsonValue> ExtractArrayToJson(FArrayProperty* ArrayProp, const void* ValuePtr);

	/**
	 * Extract a map property to JSON
	 * @param MapProp - The map property
	 * @param ValuePtr - Pointer to the map value
	 * @return JSON object representing the map
	 */
	static TSharedPtr<FJsonValue> ExtractMapToJson(FMapProperty* MapProp, const void* ValuePtr);

	/**
	 * Core unified entry point for setting struct values from JSON.
	 * Uses 3-layer strategy: special format registry -> generic array -> generic object fallback.
	 * Shared by SetStructPropertyFromJson and SetValueFromTypedJson.
	 */
	static bool SetStructFromJson(UScriptStruct* Struct, void* ValuePtr, TSharedPtr<FJsonValue> JsonValue);

	/**
	 * Set a detached property value at an explicit address.
	 * Use this for prepared container elements or struct fields that do not have a UObject container address.
	 */
	static bool SetDetachedPropertyValueFromJson(
		FProperty* Property,
		void* ValuePtr,
		TSharedPtr<FJsonValue> JsonValue,
		UObject* OwnerObject = nullptr,
		const FString& PropertyPath = FString());

	//~ Generic struct parser template — replaces all explicit Parse* helpers

	/** Parse any struct from a JSON value */
	template<typename T>
	static T ParseStruct(TSharedPtr<FJsonValue> JsonValue)
	{
		T Result{};
		SetStructFromJson(TGetScriptStruct<T>::Get(), &Result, JsonValue);
		return Result;
	}

	/** Convenience: parse struct from a JSON object */
	template<typename T>
	static T ParseStruct(TSharedPtr<FJsonObject> JsonObject)
	{
		return ParseStruct<T>(MakeShared<FJsonValueObject>(JsonObject));
	}

	/** Convenience: parse struct from a JSON array */
	template<typename T>
	static T ParseStruct(const TArray<TSharedPtr<FJsonValue>>& Array)
	{
		return ParseStruct<T>(MakeShared<FJsonValueArray>(Array));
	}

private:
	// SFINAE helper: use T::StaticStruct() if available (USTRUCTs), else TBaseStructure<T>::Get() (core math types)
	template<typename T, typename = void>
	struct TGetScriptStruct
	{
		static UScriptStruct* Get() { return TBaseStructure<T>::Get(); }
	};

	template<typename T>
	struct TGetScriptStruct<T, std::void_t<decltype(T::StaticStruct())>>
	{
		static UScriptStruct* Get() { return T::StaticStruct(); }
	};

	//~ Parse Helpers (used internally by the special format registry)
	static FLinearColor ParseColor(TSharedPtr<FJsonValue> Value);
	static FVector2D ParseVector2D(const TArray<TSharedPtr<FJsonValue>>& Array);
	static FVector ParseVector(const TArray<TSharedPtr<FJsonValue>>& Array);
	static FMargin ParseMargin(TSharedPtr<FJsonValue> Value);
	static FSlateFontInfo ParseFont(TSharedPtr<FJsonObject> FontConfig);
	static FSlateBrush ParseBrush(TSharedPtr<FJsonObject> BrushConfig);
	static FAnchors ParseAnchors(TSharedPtr<FJsonObject> AnchorsConfig);

	// Special format registry types
	using FStructDeserializer = TFunction<bool(void* ValuePtr, TSharedPtr<FJsonValue> JsonValue)>;
	using FStructSerializer = TFunction<TSharedPtr<FJsonValue>(const void* ValuePtr)>;

	/** Get lazily-initialized special deserializers registry */
	static TMap<UScriptStruct*, FStructDeserializer>& GetSpecialDeserializers();

	/** Get lazily-initialized special serializers registry */
	static TMap<UScriptStruct*, FStructSerializer>& GetSpecialSerializers();

	/**
	 * Unified recursive entry point for setting any FProperty value from JSON.
	 * Symmetric with ExtractPropertyToJson (extraction side).
	 * Supports arbitrary nesting of Array/Map/Set containers.
	 * @param Property - The FProperty describing the type
	 * @param ValuePtr - Pointer to the memory location to write
	 * @param JsonValue - The JSON value containing the data
	 * @param OwnerObject - Optional UObject used as Outer for NewObject (instanced subobjects)
	 * @param PropertyPath - Full reflected property path for diagnostics
	 * @return true if the value was set successfully
	 */
	static bool SetPropertyValueFromJson(FProperty* Property, void* ValuePtr, TSharedPtr<FJsonValue> JsonValue, UObject* OwnerObject = nullptr, const FString& PropertyPath = FString());

	/**
	 * Internal object property setter with a base path for nested diagnostics.
	 */
	static bool SetPropertiesFromJsonInternal(UObject* Object, TSharedPtr<FJsonObject> Properties, const FString& BasePath);

	/**
	 * Internal struct setter with owner and path context for recursive diagnostics.
	 */
	static bool SetStructFromJsonInternal(UScriptStruct* Struct, void* ValuePtr, TSharedPtr<FJsonValue> JsonValue, UObject* OwnerObject, const FString& PropertyPath);

	/**
	 * Internal property value setter with void* pointer
	 */
	static bool SetPropertyValueInternal(UObject* Object, FProperty* Property, void* ValuePtr, TSharedPtr<FJsonValue> JsonValue, const FString& PropertyPath = FString());

	/**
	 * Find a UScriptStruct by name (e.g., "FVector", "FLinearColor", "FMyCustomStruct")
	 */
	static UScriptStruct* FindStructByName(const FString& StructName);

	/**
	 * Find a UEnum by name (e.g., "ECollisionChannel")
	 */
	static UEnum* FindEnumByName(const FString& EnumName);

	/**
	 * Set a struct value from typed JSON
	 */
	static bool SetStructValueFromTypedJson(void* ValuePtr, UScriptStruct* Struct, TSharedPtr<FJsonValue> JsonValue);
};
