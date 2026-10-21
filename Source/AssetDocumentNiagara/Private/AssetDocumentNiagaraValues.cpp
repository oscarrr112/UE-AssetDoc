// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Niagara type tokens and value forms (design §3.2): plain-data values as JSON, the scalar wrapper
// structs unwrapped, enums by authored name (user-defined Niagara enums: the names the editor shows),
// structs and reflected properties through the Editor typed value codec with float32 rendered shortest.
// Writes are the exact inverses (DecodeValue, DecodeProperty).

#include "AssetDocumentNiagaraPrimitives.h"

#include "AssetDocumentEditorFloatPrecision.h"
#include "AssetDocumentEditorPropertyValueCodec.h"
#include "AssetDocumentGraphValue.h"

#include "NiagaraCommon.h"
#include "NiagaraDataInterface.h"
#include "NiagaraTypeRegistry.h"
#include "NiagaraTypes.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Niagara
{
namespace
{
bool IsSameType(const FNiagaraTypeDefinition& Left, const FNiagaraTypeDefinition& Right)
{
	return Left.GetStruct() == Right.GetStruct() && Left.GetEnum() == Right.GetEnum();
}

FNiagaraTypeDefinition NonStatic(const FNiagaraTypeDefinition& Type)
{
	return Type.IsStatic() ? Type.RemoveStaticDef() : Type;
}

bool EncodeProperty(FProperty& Property, const void* ValuePtr, FValue& OutValue)
{
	FString Code;
	if (!EditorPropertyValueCodec::Encode(Property, ValuePtr, OutValue, Code))
	{
		return false;
	}
	EditorFloatPrecision::NormalizeValue(Property, OutValue);
	return true;
}

// FNiagaraTypeDefinitionHandle stores an index into the process-wide type registry, which depends on
// registration order: the codec's {"RegisteredTypeIndex": N} is replaced by the type's token.
void ResolveTypeHandles(FValue& Value);
} // namespace

FString TypeToken(const FNiagaraTypeDefinition& InType)
{
	const FNiagaraTypeDefinition Type = NonStatic(InType);
	struct FNamed
	{
		const FNiagaraTypeDefinition& Def;
		const TCHAR* Token;
	};
	const FNamed Known[] = {
		{FNiagaraTypeDefinition::GetFloatDef(), TEXT("float")},
		{FNiagaraTypeDefinition::GetIntDef(), TEXT("int32")},
		{FNiagaraTypeDefinition::GetBoolDef(), TEXT("bool")},
		{FNiagaraTypeDefinition::GetVec2Def(), TEXT("Vector2")},
		{FNiagaraTypeDefinition::GetVec3Def(), TEXT("Vector")},
		{FNiagaraTypeDefinition::GetVec4Def(), TEXT("Vector4")},
		{FNiagaraTypeDefinition::GetColorDef(), TEXT("LinearColor")},
		{FNiagaraTypeDefinition::GetQuatDef(), TEXT("Quat")},
		{FNiagaraTypeDefinition::GetPositionDef(), TEXT("Position")},
		{FNiagaraTypeDefinition::GetMatrix4Def(), TEXT("Matrix")},
		{FNiagaraTypeDefinition::GetIDDef(), TEXT("ID")},
		{FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("ParameterMap")},
		{FNiagaraTypeDefinition::GetGenericNumericDef(), TEXT("Numeric")},
	};
	if (const UEnum* Enum = Type.GetEnum())
	{
		return Enum->GetPathName();
	}
	for (const FNamed& Item : Known)
	{
		if (IsSameType(Type, Item.Def))
		{
			return Item.Token;
		}
	}
	if (Type.IsDataInterface())
	{
		return GetPathNameSafe(Type.GetClass());
	}
	if (Type.IsUObject())
	{
		return FString(TEXT("Object:")) + GetPathNameSafe(Type.GetClass());
	}
	return GetPathNameSafe(Type.GetStruct());
}

bool EncodeValue(const FNiagaraTypeDefinition& InType, const uint8* Data, FValue& OutValue)
{
	const FNiagaraTypeDefinition Type = NonStatic(InType);
	if (Data == nullptr || Type.IsDataInterface() || Type.IsUObject())
	{
		return false;
	}
	if (IsSameType(Type, FNiagaraTypeDefinition::GetFloatDef()))
	{
		float Value = 0.0f;
		FMemory::Memcpy(&Value, Data, sizeof(Value));
		OutValue = Graph::MakeNumber(EditorFloatPrecision::ShortestFloatDecimal(Value));
		return true;
	}
	if (IsSameType(Type, FNiagaraTypeDefinition::GetBoolDef()))
	{
		int32 Raw = 0;
		FMemory::Memcpy(&Raw, Data, sizeof(Raw));
		OutValue = Graph::MakeBool(Raw != 0);
		return true;
	}
	if (const UEnum* Enum = Type.GetEnum())
	{
		int32 Value = 0;
		FMemory::Memcpy(&Value, Data, sizeof(Value));
		const int32 Index = Enum->GetIndexByValue(Value);
		OutValue = Index != INDEX_NONE ? Graph::MakeString(Enum->GetAuthoredNameStringByIndex(Index)) : Graph::MakeInt(Value);
		return true;
	}
	if (IsSameType(Type, FNiagaraTypeDefinition::GetIntDef()))
	{
		int32 Value = 0;
		FMemory::Memcpy(&Value, Data, sizeof(Value));
		OutValue = Graph::MakeInt(Value);
		return true;
	}
	const UScriptStruct* Struct = Type.GetScriptStruct();
	if (Struct == nullptr)
	{
		return false;
	}
	FObjectBuilder Object;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FValue Member;
		if (!EncodeProperty(**It, It->ContainerPtrToValuePtr<void>(Data), Member))
		{
			return false;
		}
		Object.Add(It->GetAuthoredName(), MoveTemp(Member));
	}
	OutValue = Object.Build();
	return true;
}

FString ClassLabel(const UClass* Class, const TCHAR* Prefix, const TCHAR* Suffix)
{
	if (Class == nullptr)
	{
		return FString();
	}
	const FString Name = Class->GetName();
	const FString Package = Class->GetOutermost()->GetName();
	if (Package == TEXT("/Script/Niagara") && Name.StartsWith(Prefix, ESearchCase::CaseSensitive)
		// (FString::EndsWith is false for an empty suffix.)
		&& (*Suffix == TEXT('\0') || Name.EndsWith(Suffix, ESearchCase::CaseSensitive))
		&& Name.Len() > FCString::Strlen(Prefix) + FCString::Strlen(Suffix))
	{
		return Name.Mid(FCString::Strlen(Prefix), Name.Len() - FCString::Strlen(Prefix) - FCString::Strlen(Suffix));
	}
	return Class->GetPathName();
}


namespace
{
void ResolveTypeHandles(FValue& Value)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Array)
	{
		for (FValue& Element : Value.Elements)
		{
			ResolveTypeHandles(Element);
		}
		return;
	}
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return;
	}
	for (int32 Index = 0; Index < Value.ObjectKeys.Num() && Index < Value.Elements.Num(); ++Index)
	{
		FValue& Element = Value.Elements[Index];
		if (Element.Kind == EAssetDocumentRuntimeValueKind::Object && Element.ObjectKeys.Num() == 1
			&& Element.ObjectKeys[0] == TEXT("RegisteredTypeIndex") && Element.Elements.Num() == 1
			&& Element.Elements[0].Kind == EAssetDocumentRuntimeValueKind::Integer)
		{
			FNiagaraTypeDefinitionHandle Handle;
			const FIntProperty* IndexProperty = CastField<FIntProperty>(
				FNiagaraTypeDefinitionHandle::StaticStruct()->FindPropertyByName(TEXT("RegisteredTypeIndex")));
			const int32 Registered = static_cast<int32>(Element.Elements[0].IntegerValue);
			if (IndexProperty != nullptr && Registered != INDEX_NONE)
			{
				IndexProperty->SetPropertyValue_InContainer(&Handle, Registered);
				Element = FValue();
				Element.Kind = EAssetDocumentRuntimeValueKind::String;
				Element.StringValue = TypeToken(*Handle);
			}
			else
			{
				Element = FValue();
			}
			continue;
		}
		ResolveTypeHandles(Element);
	}
}
void RemoveKeys(FValue& Value, TConstArrayView<const TCHAR*> Keys)
{
	for (int32 Index = Value.ObjectKeys.Num() - 1; Index >= 0; --Index)
	{
		if (Keys.ContainsByPredicate([&Value, Index](const TCHAR* Key) { return Value.ObjectKeys[Index] == Key; }))
		{
			Value.ObjectKeys.RemoveAt(Index);
			Value.Elements.RemoveAt(Index);
		}
	}
}

// Drops the members a renderer's attribute bindings derive from the emitter (UNiagaraRendererProperties::
// UpdateSourceModeDerivates -> CacheValues recomputes them after each edit and on load): the bound name, its
// source mode and the parameter map variable are the binding; the attribute table name, display name and the
// "exists on source" / "particle value" flags (and the pre-5.0 BoundVariable) follow from the emitter.
void StripDerived(const FProperty& Property, FValue& Value)
{
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Array)
		{
			for (FValue& Element : Value.Elements)
			{
				StripDerived(*Array->Inner, Element);
			}
		}
		return;
	}
	const FStructProperty* StructProperty = CastField<FStructProperty>(&Property);
	if (StructProperty == nullptr || Value.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return;
	}
	if (StructProperty->Struct == FNiagaraVariableAttributeBinding::StaticStruct())
	{
		RemoveKeys(Value, {TEXT("DataSetName"), TEXT("CachedDisplayName"), TEXT("bBindingExistsOnSource"), TEXT("bIsCachedParticleValue"),
			TEXT("BoundVariable")});
		// Unbound (no root name): the parameter map variable only carries the type the renderer's Setup or
		// CacheValues gave it, which differs between a new renderer and a loaded one.
		const int32 Root = Value.ObjectKeys.IndexOfByKey(FString(TEXT("RootName")));
		const FValue* RootName = Root != INDEX_NONE && Root < Value.Elements.Num() ? &Value.Elements[Root] : nullptr;
		if (RootName != nullptr && RootName->Kind == EAssetDocumentRuntimeValueKind::String
			&& (RootName->StringValue.IsEmpty() || RootName->StringValue == TEXT("None")))
		{
			RemoveKeys(Value, {TEXT("ParamMapVariable")});
		}
		return;
	}
	if (StructProperty->Struct == FNiagaraMaterialAttributeBinding::StaticStruct())
	{
		RemoveKeys(Value, {TEXT("ResolvedNiagaraVariable")});
		return;
	}
	// A lightweight module's distribution in a curve mode: Values / ValuesTimeRange are the lookup table the editor
	// bakes from ChannelCurves on every change (FNiagaraDistributionBase::UpdateValuesFromDistribution), whose last
	// bits differ between platforms; the curves are the authored value.
	static const UScriptStruct* const DistributionBase = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Niagara.NiagaraDistributionBase"));
	if (DistributionBase != nullptr && StructProperty->Struct->IsChildOf(DistributionBase))
	{
		const int32 ModeIndex = Value.ObjectKeys.IndexOfByKey(FString(TEXT("Mode")));
		const FValue* Mode = ModeIndex != INDEX_NONE && ModeIndex < Value.Elements.Num() ? &Value.Elements[ModeIndex] : nullptr;
		if (Mode != nullptr && Mode->Kind == EAssetDocumentRuntimeValueKind::String
			&& (Mode->StringValue == TEXT("UniformCurve") || Mode->StringValue == TEXT("NonUniformCurve") || Mode->StringValue == TEXT("ColorGradient")))
		{
			RemoveKeys(Value, {TEXT("Values"), TEXT("ValuesTimeRange")});
		}
	}
	// A lightweight emitter's spawn info (engine-internal struct, matched by name): its SourceId is the stack
	// entry's identity (not edit-visible; the spawn group's "+" gives a new one), kept by position on write.
	if (StructProperty->Struct->GetFName() == TEXT("NiagaraStatelessSpawnInfo"))
	{
		RemoveKeys(Value, {TEXT("SourceId")});
	}
	for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
	{
		const FString Key = It->GetAuthoredName();
		const int32 Index = Value.ObjectKeys.IndexOfByKey(Key);
		if (Index != INDEX_NONE && Index < Value.Elements.Num())
		{
			StripDerived(**It, Value.Elements[Index]);
		}
	}
}

// The document form of one property (type tokens resolved, derived binding members dropped).
bool EncodeDocumentProperty(FProperty& Property, const void* ValuePtr, FValue& OutValue)
{
	if (!EncodeProperty(Property, ValuePtr, OutValue))
	{
		return false;
	}
	ResolveTypeHandles(OutValue);
	StripDerived(Property, OutValue);
	return true;
}
} // namespace

FValue SparseProperties(const UStruct& Struct, const void* Container, const void* Defaults, TConstArrayView<FName> Skip,
	TArray<FString>* OutUnsupported)
{
	FObjectBuilder Object;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		FProperty& Property = **It;
		if (!Property.HasAnyPropertyFlags(CPF_Edit) || Property.HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)
			|| Skip.Contains(Property.GetFName()))
		{
			continue;
		}
		if (Defaults != nullptr && Property.Identical_InContainer(Container, Defaults))
		{
			continue;
		}
		FValue Value;
		if (!EncodeDocumentProperty(Property, Property.ContainerPtrToValuePtr<void>(Container), Value))
		{
			if (OutUnsupported != nullptr)
			{
				OutUnsupported->Add(Property.GetAuthoredName());
			}
			continue;
		}
		// Equal to the default once derived members are dropped: left out as well.
		FValue Default;
		if (Defaults != nullptr && EncodeDocumentProperty(Property, Property.ContainerPtrToValuePtr<void>(Defaults), Default)
			&& Graph::ValueToJsonString(Default, false) == Graph::ValueToJsonString(Value, false))
		{
			continue;
		}
		Object.Add(Property.GetAuthoredName(), MoveTemp(Value));
	}
	return Object.Build();
}

FString ObjectPathOrEmpty(const UObject* Object)
{
	return Object != nullptr ? Object->GetPathName() : FString();
}

bool PropertyMatchesDefault(FProperty& Property, const void* ValuePtr, const void* DefaultPtr)
{
	if (Property.Identical(ValuePtr, DefaultPtr))
	{
		return true;
	}
	FValue Value;
	FValue Default;
	return EncodeDocumentProperty(Property, ValuePtr, Value) && EncodeDocumentProperty(Property, DefaultPtr, Default)
		&& Graph::ValueToJsonString(Default, false) == Graph::ValueToJsonString(Value, false);
}

FValue SparseObjectProperties(const UObject& Object, TConstArrayView<FName> Skip, TArray<FString>* OutUnsupported)
{
	const UObject* Defaults = Object.GetClass()->GetDefaultObject();
	return SparseProperties(*Object.GetClass(), &Object, Defaults, Skip, OutUnsupported);
}

FValue DescribeDataInterface(const UNiagaraDataInterface& DataInterface)
{
	FObjectBuilder Result;
	Result.AddString(TEXT("Class"), ClassLabel(DataInterface.GetClass(), TEXT("NiagaraDataInterface"), TEXT("")));
	FValue Properties = SparseObjectProperties(DataInterface);
	if (Properties.ObjectKeys.Num() > 0)
	{
		// For extract (diff_only): the values with struct members equal to the class default's left out, also in
		// arrays (a curve's keys); an apply decodes them as edits of the class default (DecodeProperty's BasePtr).
		// Listed only when shorter; the comparisons of an apply read Properties whole.
		FValue Sparse = Properties;
		const UObject* Defaults = DataInterface.GetClass()->GetDefaultObject();
		bool bPruned = false;
		for (int32 Index = 0; Index < Sparse.ObjectKeys.Num() && Index < Sparse.Elements.Num(); ++Index)
		{
			FProperty* Property = FindEditableProperty(*DataInterface.GetClass(), Sparse.ObjectKeys[Index]);
			FValue Default;
			if (Property == nullptr || !EncodeDocumentProperty(*Property, Property->ContainerPtrToValuePtr<void>(Defaults), Default))
			{
				continue;
			}
			const FString Before = Graph::ValueToJsonString(Sparse.Elements[Index], false);
			EditorPropertyValueCodec::PruneToChanged(*Property, Sparse.Elements[Index], Default,
				[](FProperty& Element, const void* ElementPtr, FValue& OutElement)
				{
					return EncodeDocumentProperty(Element, ElementPtr, OutElement);
				});
			bPruned |= Graph::ValueToJsonString(Sparse.Elements[Index], false) != Before;
		}
		Result.Add(TEXT("Properties"), MoveTemp(Properties));
		if (bPruned)
		{
			Result.Add(TEXT("SparseProperties"), MoveTemp(Sparse));
		}
	}
	return Result.Build();
}

// ---- writes ---------------------------------------------------------------------------------------------

bool TypeFromToken(const FString& Token, FNiagaraTypeDefinition& OutType)
{
	for (const FNiagaraTypeDefinition& Type : FNiagaraTypeRegistry::GetRegisteredTypes())
	{
		if (Type.IsValid() && !Type.IsStatic() && TypeToken(Type) == Token)
		{
			OutType = Type;
			return true;
		}
	}
	return false;
}

namespace
{
const TCHAR* KindName(EAssetDocumentRuntimeValueKind Kind)
{
	switch (Kind)
	{
	case EAssetDocumentRuntimeValueKind::Boolean: return TEXT("a boolean");
	case EAssetDocumentRuntimeValueKind::Integer:
	case EAssetDocumentRuntimeValueKind::Number: return TEXT("a number");
	case EAssetDocumentRuntimeValueKind::String: return TEXT("a string");
	case EAssetDocumentRuntimeValueKind::Array: return TEXT("an array");
	case EAssetDocumentRuntimeValueKind::Object: return TEXT("an object");
	default: return TEXT("null");
	}
}

bool IsNumber(const FValue& Value)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::Integer || Value.Kind == EAssetDocumentRuntimeValueKind::Number;
}

double AsNumber(const FValue& Value)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::Integer ? static_cast<double>(Value.IntegerValue) : Value.NumberValue;
}

// The read writes each FNiagaraTypeDefinitionHandle as its type token; the codec reads the registered index.
void ResolveTypeTokens(const FProperty& Property, FValue& Value)
{
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Array)
		{
			for (FValue& Element : Value.Elements)
			{
				ResolveTypeTokens(*Array->Inner, Element);
			}
		}
		return;
	}
	const FStructProperty* StructProperty = CastField<FStructProperty>(&Property);
	if (StructProperty == nullptr)
	{
		return;
	}
	if (StructProperty->Struct == FNiagaraTypeDefinitionHandle::StaticStruct())
	{
		int64 Registered = INDEX_NONE;
		FNiagaraTypeDefinition Type;
		if (Value.Kind == EAssetDocumentRuntimeValueKind::String && TypeFromToken(Value.StringValue, Type))
		{
			const FNiagaraTypeDefinitionHandle Handle(Type);
			const FIntProperty* IndexProperty = CastField<FIntProperty>(
				FNiagaraTypeDefinitionHandle::StaticStruct()->FindPropertyByName(TEXT("RegisteredTypeIndex")));
			if (IndexProperty != nullptr)
			{
				Registered = IndexProperty->GetPropertyValue_InContainer(&Handle);
			}
		}
		else if (Value.Kind != EAssetDocumentRuntimeValueKind::Null)
		{
			return;
		}
		FObjectBuilder Object;
		Object.AddInt(TEXT("RegisteredTypeIndex"), Registered);
		Value = Object.Build();
		return;
	}
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return;
	}
	for (int32 Index = 0; Index < Value.ObjectKeys.Num() && Index < Value.Elements.Num(); ++Index)
	{
		if (const FProperty* Member = FindEditableProperty(*StructProperty->Struct, Value.ObjectKeys[Index]))
		{
			ResolveTypeTokens(*Member, Value.Elements[Index]);
		}
		else
		{
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				if (It->GetAuthoredName() == Value.ObjectKeys[Index])
				{
					ResolveTypeTokens(**It, Value.Elements[Index]);
					break;
				}
			}
		}
	}
}
} // namespace

FProperty* FindEditableProperty(const UStruct& Struct, const FString& Name)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		FProperty* Property = *It;
		if (Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)
			&& Property->GetAuthoredName() == Name)
		{
			return Property;
		}
	}
	return nullptr;
}

FString EnumShortName(const FString& Name)
{
	const int32 Open = Name.Find(TEXT(" ("));
	return Open > 0 && Name.EndsWith(TEXT(")")) ? Name.Left(Open).TrimEnd() : Name;
}

bool DecodeValue(const FNiagaraTypeDefinition& InType, const FValue& Value, uint8* Data, FString& OutMessage)
{
	const FNiagaraTypeDefinition Type = NonStatic(InType);
	if (Data == nullptr || Type.IsDataInterface() || Type.IsUObject())
	{
		OutMessage = TEXT("The type has no plain value.");
		return false;
	}
	if (IsSameType(Type, FNiagaraTypeDefinition::GetFloatDef()))
	{
		if (!IsNumber(Value))
		{
			OutMessage = FString::Printf(TEXT("A float value is a number (got %s)."), KindName(Value.Kind));
			return false;
		}
		const float Number = static_cast<float>(AsNumber(Value));
		FMemory::Memcpy(Data, &Number, sizeof(Number));
		return true;
	}
	if (IsSameType(Type, FNiagaraTypeDefinition::GetBoolDef()))
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Boolean)
		{
			OutMessage = FString::Printf(TEXT("A bool value is true or false (got %s)."), KindName(Value.Kind));
			return false;
		}
		const FNiagaraBool Bool(Value.BooleanValue);
		FMemory::Memcpy(Data, &Bool, sizeof(Bool));
		return true;
	}
	if (const UEnum* Enum = Type.GetEnum())
	{
		int32 Number = 0;
		if (Value.Kind == EAssetDocumentRuntimeValueKind::String)
		{
			int32 Found = INDEX_NONE;
			for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
			{
				if (Enum->GetAuthoredNameStringByIndex(Index) == Value.StringValue)
				{
					Found = Index;
					break;
				}
			}
			// The short name of a value whose name explains itself in parentheses ("Kill" for "Kill (Emitter and Particles
			// Die Immediately)"), when exactly one value has it.
			TArray<FString> Names;
			TArray<int32> Short;
			for (int32 Index = 0; Index + 1 < Enum->NumEnums(); ++Index)
			{
				Names.Add(Enum->GetAuthoredNameStringByIndex(Index));
				if (Found == INDEX_NONE && EnumShortName(Names.Last()) == Value.StringValue)
				{
					Short.Add(Index);
				}
			}
			if (Found == INDEX_NONE && Short.Num() == 1)
			{
				Found = Short[0];
			}
			if (Found == INDEX_NONE)
			{
				TArray<FString> Matches;
				for (const int32 Index : Short)
				{
					Matches.Add(Names[Index]);
				}
				OutMessage = Short.Num() > 1
					? FString::Printf(TEXT("'%s' is the short name of more than one value of %s (%s): write the full name."),
						*Value.StringValue, *Enum->GetName(), *FString::Join(Matches, TEXT(", ")))
					: FString::Printf(TEXT("'%s' is not a value of %s (values: %s; a value whose name ends in a parenthesized "
						"explanation can be written without it)."), *Value.StringValue, *Enum->GetName(), *FString::Join(Names, TEXT(", ")));
				return false;
			}
			Number = static_cast<int32>(Enum->GetValueByIndex(Found));
		}
		else if (Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
		{
			Number = static_cast<int32>(Value.IntegerValue);
		}
		else
		{
			OutMessage = FString::Printf(TEXT("An enum value is a value name (got %s)."), KindName(Value.Kind));
			return false;
		}
		FMemory::Memcpy(Data, &Number, sizeof(Number));
		return true;
	}
	if (IsSameType(Type, FNiagaraTypeDefinition::GetIntDef()))
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Integer
			&& !(Value.Kind == EAssetDocumentRuntimeValueKind::Number && FMath::IsNearlyEqual(Value.NumberValue, FMath::RoundToDouble(Value.NumberValue), 0.0)))
		{
			OutMessage = FString::Printf(TEXT("An int32 value is an integer (got %s)."), KindName(Value.Kind));
			return false;
		}
		const int32 Number = static_cast<int32>(AsNumber(Value));
		FMemory::Memcpy(Data, &Number, sizeof(Number));
		return true;
	}
	const UScriptStruct* Struct = Type.GetScriptStruct();
	if (Struct == nullptr || Value.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		OutMessage = Struct == nullptr ? FString(TEXT("The type has no plain value."))
			: FString::Printf(TEXT("A %s value is an object of its members (got %s)."), *Struct->GetName(), KindName(Value.Kind));
		return false;
	}
	// Decode into a copy, so a failure leaves Data unchanged.
	TArray<uint8> Copy;
	Copy.SetNumUninitialized(Struct->GetStructureSize());
	FMemory::Memcpy(Copy.GetData(), Data, Copy.Num());
	for (int32 Index = 0; Index < Value.ObjectKeys.Num() && Index < Value.Elements.Num(); ++Index)
	{
		FProperty* Member = nullptr;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (It->GetAuthoredName() == Value.ObjectKeys[Index])
			{
				Member = *It;
				break;
			}
		}
		FString Code;
		if (Member == nullptr)
		{
			OutMessage = FString::Printf(TEXT("%s has no member '%s'."), *Struct->GetName(), *Value.ObjectKeys[Index]);
			return false;
		}
		if (!EditorPropertyValueCodec::Decode(*Member, Value.Elements[Index], Member->ContainerPtrToValuePtr<void>(Copy.GetData()), Code))
		{
			OutMessage = FString::Printf(TEXT("%s.%s: the value does not fit the member (%s)."), *Struct->GetName(), *Value.ObjectKeys[Index], *Code);
			return false;
		}
	}
	FMemory::Memcpy(Data, Copy.GetData(), Copy.Num());
	return true;
}

bool DecodeProperty(FProperty& Property, const FValue& Value, void* ValuePtr, UObject* Owner, FString& OutCode, const void* BasePtr)
{
	FValue Resolved = Value;
	ResolveTypeTokens(Property, Resolved);
	if (BasePtr == nullptr || BasePtr == ValuePtr)
	{
		return EditorPropertyValueCodec::Decode(Property, Resolved, ValuePtr, OutCode, Owner);
	}
	void* Copy = FMemory::Malloc(Property.GetSize(), Property.GetMinAlignment());
	Property.InitializeValue(Copy);
	Property.CopyCompleteValue(Copy, BasePtr);
	const bool bDecoded = EditorPropertyValueCodec::Decode(Property, Resolved, Copy, OutCode, Owner);
	if (bDecoded)
	{
		Property.CopyCompleteValue(ValuePtr, Copy);
	}
	Property.DestroyValue(Copy);
	FMemory::Free(Copy);
	return bDecoded;
}

bool DecodePropertyIsSame(FProperty& Property, const FValue& Value, const void* ValuePtr, UObject* Owner, bool& OutSame, FString& OutCode,
	const void* BasePtr)
{
	void* Copy = FMemory::Malloc(Property.GetSize(), Property.GetMinAlignment());
	Property.InitializeValue(Copy);
	Property.CopyCompleteValue(Copy, BasePtr != nullptr ? BasePtr : ValuePtr);
	const bool bDecoded = DecodeProperty(Property, Value, Copy, Owner, OutCode);
	OutSame = bDecoded && Property.Identical(Copy, ValuePtr, PPF_None);
	Property.DestroyValue(Copy);
	FMemory::Free(Copy);
	return bDecoded;
}
} // namespace UE::AssetDocument::Niagara

namespace UE::AssetDocument::Niagara
{
TArray<UObject*> ReflectedObjectList(const UObject* Owner, const TCHAR* PropertyName)
{
	TArray<UObject*> Out;
	const FArrayProperty* Array = Owner != nullptr ? CastField<FArrayProperty>(Owner->GetClass()->FindPropertyByName(PropertyName)) : nullptr;
	const FObjectPropertyBase* Inner = Array != nullptr ? CastField<FObjectPropertyBase>(Array->Inner) : nullptr;
	if (Inner == nullptr)
	{
		return Out;
	}
	FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Owner));
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
	{
		Out.Add(Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)));
	}
	return Out;
}
} // namespace UE::AssetDocument::Niagara
