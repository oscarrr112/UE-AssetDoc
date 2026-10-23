// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorFloatPrecision.h"

#include "Misc/PackageName.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::EditorFloatPrecision
{
namespace
{
using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;

constexpr int32 MaximumDepth = 64;

FValue* FindField(FValue& Object, FStringView Key)
{
	if (Object.Kind != EKind::Object || Object.ObjectKeys.Num() != Object.Elements.Num())
	{
		return nullptr;
	}
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (FStringView(Object.ObjectKeys[Index]) == Key)
		{
			return &Object.Elements[Index];
		}
	}
	return nullptr;
}

// Documents name members by their authored name (the name the Details panel shows
// for user-defined structs), native members by their name.
const FProperty* FindMember(const UStruct& Struct, const FString& Name)
{
	const FProperty* ByName = nullptr;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (It->GetAuthoredName() == Name)
		{
			return *It;
		}
		if (ByName == nullptr && It->GetName() == Name)
		{
			ByName = *It;
		}
	}
	return ByName;
}

template <typename T>
T* ResolveType(const FValue* Path)
{
	if (Path == nullptr || Path->Kind != EKind::String || !FPackageName::IsValidObjectPath(Path->StringValue))
	{
		return nullptr;
	}
	if (T* Found = Cast<T>(FSoftObjectPath(Path->StringValue).ResolveObject()))
	{
		return Found;
	}
	// Compiled-in types are always loaded; asset types (Blueprint classes, user
	// structs) load as the codec would load them.
	return Path->StringValue.StartsWith(TEXT("/Script/"))
		? nullptr
		: LoadObject<T>(nullptr, *Path->StringValue, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

int32 NormalizeWithDepth(const FProperty& Property, FValue& Value, int32 Depth);

int32 NormalizeMembers(const UStruct& Struct, FValue& Object, int32 Depth)
{
	if (Object.Kind != EKind::Object || Object.ObjectKeys.Num() != Object.Elements.Num())
	{
		return 0;
	}
	int32 Changed = 0;
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (const FProperty* Member = FindMember(Struct, Object.ObjectKeys[Index]))
		{
			Changed += NormalizeWithDepth(*Member, Object.Elements[Index], Depth + 1);
		}
	}
	return Changed;
}

int32 NormalizeFloat(FValue& Value)
{
	if (Value.Kind == EKind::Number)
	{
		const float Stored = static_cast<float>(Value.NumberValue);
		if (!FMath::IsFinite(Value.NumberValue) || !FMath::IsFinite(Stored))
		{
			return 0; // out of float range: the codec reports it
		}
		const double Shortest = ShortestFloatDecimal(Stored);
		if (Shortest == Value.NumberValue)
		{
			return 0;
		}
		Value.NumberValue = Shortest;
		return 1;
	}
	if (Value.Kind == EKind::Integer)
	{
		// An integral literal stays an integer when the float holds it (16777217 -> 16777216).
		const float Stored = static_cast<float>(Value.IntegerValue);
		constexpr float Int64Bound = 9223372036854775808.0f; // 2^63
		if (Stored >= Int64Bound || Stored < -Int64Bound)
		{
			Value.Kind = EKind::Number;
			Value.NumberValue = ShortestFloatDecimal(Stored);
			Value.IntegerValue = 0;
			return 1;
		}
		const int64 Rounded = static_cast<int64>(Stored);
		if (Rounded == Value.IntegerValue)
		{
			return 0;
		}
		Value.IntegerValue = Rounded;
		return 1;
	}
	return 0;
}

int32 NormalizeElement(const FProperty& Property, FValue& Value, int32 Depth)
{
	if (Depth > MaximumDepth)
	{
		return 0;
	}
	if (Property.IsA<FFloatProperty>())
	{
		return NormalizeFloat(Value);
	}
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
	{
		if (StructProperty->Struct == nullptr)
		{
			return 0;
		}
		if (StructProperty->Struct == FInstancedStruct::StaticStruct())
		{
			// {"Struct": "<path>", "Properties": {...}}
			const UScriptStruct* Struct = ResolveType<UScriptStruct>(FindField(Value, TEXT("Struct")));
			FValue* Properties = FindField(Value, TEXT("Properties"));
			return Struct != nullptr && Properties != nullptr ? NormalizeMembers(*Struct, *Properties, Depth) : 0;
		}
		return NormalizeMembers(*StructProperty->Struct, Value, Depth);
	}
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
	{
		// Instanced subobject: {"Class": "<path>", "Properties": {...}}; references are strings.
		const UClass* Class = ResolveType<UClass>(FindField(Value, TEXT("Class")));
		FValue* Properties = FindField(Value, TEXT("Properties"));
		return Class != nullptr && Properties != nullptr
				&& (ObjectProperty->PropertyClass == nullptr || Class->IsChildOf(ObjectProperty->PropertyClass))
			? NormalizeMembers(*Class, *Properties, Depth)
			: 0;
	}
	const FProperty* Inner = nullptr;
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		Inner = Array->Inner;
	}
	else if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		Inner = Set->ElementProp;
	}
	else if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		// Keys are JSON object keys (strings); values are walked.
		if (Value.Kind != EKind::Object)
		{
			return 0;
		}
		int32 Changed = 0;
		for (FValue& Entry : Value.Elements)
		{
			Changed += NormalizeWithDepth(*Map->ValueProp, Entry, Depth + 1);
		}
		return Changed;
	}
	if (Inner == nullptr || Value.Kind != EKind::Array)
	{
		return 0;
	}
	int32 Changed = 0;
	for (FValue& Element : Value.Elements)
	{
		Changed += NormalizeWithDepth(*Inner, Element, Depth + 1);
	}
	return Changed;
}

int32 NormalizeWithDepth(const FProperty& Property, FValue& Value, int32 Depth)
{
	if (Property.ArrayDim > 1)
	{
		// A fixed-size C array is a JSON array of exactly ArrayDim elements.
		if (Value.Kind != EKind::Array)
		{
			return 0;
		}
		int32 Changed = 0;
		for (FValue& Element : Value.Elements)
		{
			Changed += NormalizeElement(Property, Element, Depth + 1);
		}
		return Changed;
	}
	return NormalizeElement(Property, Value, Depth);
}
} // namespace

double ShortestFloatDecimal(float Value)
{
	if (!FMath::IsFinite(Value))
	{
		return Value;
	}
	for (int32 Digits = 1; Digits <= 9; ++Digits)
	{
		const FString Text = FString::Printf(TEXT("%.*g"), Digits, static_cast<double>(Value));
		const double Parsed = FCString::Atod(*Text);
		if (static_cast<float>(Parsed) == Value)
		{
			return Parsed;
		}
	}
	return Value;
}

int32 NormalizeValue(const FProperty& Property, FAssetDocumentRuntimeValueDto& Value)
{
	return NormalizeWithDepth(Property, Value, 0);
}

int32 NormalizeDocumentProperties(FAssetDocumentRuntimeValueDto& Document)
{
	const UClass* Class = ResolveType<UClass>(FindField(Document, TEXT("Class")));
	FValue* Properties = FindField(Document, TEXT("Properties"));
	return Class != nullptr && Properties != nullptr ? NormalizeMembers(*Class, *Properties, 0) : 0;
}
}
