// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorPropertyValueCodec.h"

#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::EditorPropertyValueCodec
{
namespace AssetDocumentEditorPropertyValueCodecLocal
{
using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;
using FEncodeValue = TFunctionRef<bool(FProperty&, const void*, FValue&)>;

constexpr int32 MaximumPruneDepth = 64;

bool IsNumeric(const FValue& Value)
{
	return Value.Kind == EKind::Integer || Value.Kind == EKind::Number;
}

double AsNumber(const FValue& Value)
{
	return Value.Kind == EKind::Integer ? static_cast<double>(Value.IntegerValue) : Value.NumberValue;
}

const FValue* FindMember(const FValue& Object, const FString& Key)
{
	const int32 Index = Object.ObjectKeys.IndexOfByKey(Key);
	return Index != INDEX_NONE && Object.Elements.IsValidIndex(Index) ? &Object.Elements[Index] : nullptr;
}

bool AreEqual(const FValue& Left, const FValue& Right, int32 Depth = 0)
{
	if (Depth > MaximumPruneDepth)
	{
		return false;
	}
	if (Left.Kind != Right.Kind)
	{
		return IsNumeric(Left) && IsNumeric(Right) && AsNumber(Left) == AsNumber(Right);
	}
	switch (Left.Kind)
	{
	case EKind::Null: return true;
	case EKind::Boolean: return Left.BooleanValue == Right.BooleanValue;
	case EKind::Integer: return Left.IntegerValue == Right.IntegerValue;
	case EKind::Number: return Left.NumberValue == Right.NumberValue;
	case EKind::String: return Left.StringValue.Equals(Right.StringValue, ESearchCase::CaseSensitive);
	case EKind::Array:
		if (Left.Elements.Num() != Right.Elements.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.Elements.Num(); ++Index)
		{
			if (!AreEqual(Left.Elements[Index], Right.Elements[Index], Depth + 1))
			{
				return false;
			}
		}
		return true;
	case EKind::Object:
		if (Left.ObjectKeys.Num() != Right.ObjectKeys.Num() || Left.Elements.Num() != Left.ObjectKeys.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.ObjectKeys.Num(); ++Index)
		{
			const FValue* Other = FindMember(Right, Left.ObjectKeys[Index]);
			if (Other == nullptr || !AreEqual(Left.Elements[Index], *Other, Depth + 1))
			{
				return false;
			}
		}
		return true;
	default:
		return Left.BytesValue == Right.BytesValue;
	}
}

// A struct written as an object of its members, which an edit may list in part.
bool IsMemberStruct(const FProperty& Property, const FValue& Value)
{
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	return Struct != nullptr && Struct->Struct != nullptr && Property.ArrayDim == 1 && Value.Kind == EKind::Object
		&& Struct->Struct != TBaseStructure<FInstancedStruct>::Get()
		&& (Struct->Struct->StructFlags & (STRUCT_Atomic | STRUCT_Immutable)) == 0;
}

FProperty* FindStructMember(const UScriptStruct& Struct, const FString& Name)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (It->GetAuthoredName() == Name)
		{
			return *It;
		}
	}
	return nullptr;
}

void Prune(FProperty& Property, FValue& Value, const FValue& Base, FEncodeValue EncodeValue, int32 Depth)
{
	if (Depth > MaximumPruneDepth)
	{
		return;
	}
	if (IsMemberStruct(Property, Value))
	{
		if (Base.Kind != EKind::Object)
		{
			return;
		}
		const UScriptStruct& Struct = *CastFieldChecked<FStructProperty>(&Property)->Struct;
		for (int32 Index = FMath::Min(Value.ObjectKeys.Num(), Value.Elements.Num()) - 1; Index >= 0; --Index)
		{
			const FValue* BaseMember = FindMember(Base, Value.ObjectKeys[Index]);
			FProperty* Member = BaseMember != nullptr ? FindStructMember(Struct, Value.ObjectKeys[Index]) : nullptr;
			if (Member == nullptr)
			{
				continue;
			}
			if (AreEqual(Value.Elements[Index], *BaseMember))
			{
				Value.ObjectKeys.RemoveAt(Index);
				Value.Elements.RemoveAt(Index);
				continue;
			}
			Prune(*Member, Value.Elements[Index], *BaseMember, EncodeValue, Depth + 1);
		}
		return;
	}
	const FArrayProperty* Array = CastField<FArrayProperty>(&Property);
	if (Array == nullptr || Property.ArrayDim != 1 || Value.Kind != EKind::Array)
	{
		return;
	}
	// Past Base's end an edit adds default elements: the rest is compared with one.
	TOptional<FValue> DefaultElement;
	bool bEncodedDefault = false;
	for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
	{
		const FValue* BaseElement = Base.Kind == EKind::Array && Base.Elements.IsValidIndex(Index) ? &Base.Elements[Index] : nullptr;
		if (BaseElement == nullptr)
		{
			if (!bEncodedDefault)
			{
				bEncodedDefault = true;
				FAssetDocumentEditorScopedPropertyValue Default(*Array->Inner);
				FValue Encoded;
				if (Default.IsValid() && EncodeValue(*Array->Inner, Default.GetMemory(), Encoded))
				{
					DefaultElement = MoveTemp(Encoded);
				}
			}
			BaseElement = DefaultElement.GetPtrOrNull();
		}
		if (BaseElement != nullptr)
		{
			Prune(*Array->Inner, Value.Elements[Index], *BaseElement, EncodeValue, Depth + 1);
		}
	}
}
} // namespace AssetDocumentEditorPropertyValueCodecLocal

bool PruneToChanged(FProperty& Property, FAssetDocumentRuntimeValueDto& Value, const FAssetDocumentRuntimeValueDto& Base,
	TFunctionRef<bool(FProperty& Property, const void* ValuePtr, FAssetDocumentRuntimeValueDto& OutValue)> EncodeValue)
{
	using namespace AssetDocumentEditorPropertyValueCodecLocal;
	if (AreEqual(Value, Base))
	{
		return false;
	}
	Prune(Property, Value, Base, EncodeValue, 0);
	return true;
}

bool PruneToChanged(FProperty& Property, FAssetDocumentRuntimeValueDto& Value, const FAssetDocumentRuntimeValueDto& Base)
{
	return PruneToChanged(Property, Value, Base, [](FProperty& Element, const void* ValuePtr, FAssetDocumentRuntimeValueDto& OutValue)
	{
		FString Code;
		return Encode(Element, ValuePtr, OutValue, Code);
	});
}

int32 FindEnumIndexBySpelling(const UEnum& Enum, const FString& Spelling)
{
	const FString Wanted = Spelling.TrimStartAndEnd();
	if (Wanted.IsEmpty())
	{
		return INDEX_NONE;
	}
	// The enumerator's own name first ("NewEnumerator2", "E_State::NewEnumerator2"), exactly.
	const int32 Exact = Enum.GetIndexByNameString(Wanted);
	if (Exact != INDEX_NONE)
	{
		return Exact;
	}
	int32 Found = INDEX_NONE;
	const int32 Count = Enum.NumEnums() - (Enum.ContainsExistingMax() ? 1 : 0);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const bool bNames = Enum.GetNameStringByIndex(Index).Equals(Wanted, ESearchCase::IgnoreCase)
			|| Enum.GetAuthoredNameStringByIndex(Index).Equals(Wanted, ESearchCase::IgnoreCase)
			|| Enum.GetDisplayNameTextByIndex(Index).ToString().Equals(Wanted, ESearchCase::IgnoreCase);
		if (bNames)
		{
			if (Found != INDEX_NONE && Found != Index)
			{
				return INDEX_NONE;  // ambiguous (two display names alike)
			}
			Found = Index;
		}
	}
	return Found;
}

FString DescribeEnumSpellings(const UEnum& Enum)
{
	TArray<FString> Names;
	const int32 Count = Enum.NumEnums() - (Enum.ContainsExistingMax() ? 1 : 0);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (!Enum.HasMetaData(TEXT("Hidden"), Index))
		{
			Names.Add(Enum.GetAuthoredNameStringByIndex(Index));
		}
	}
	return FString::Join(Names, TEXT(", "));
}

bool Inspect(FProperty& Property, FString& OutCodecId)
{
	if (DoesAssetDocumentEditorPropertyHoldInstancedObjects(Property, /*bIncludeInstancedStructs*/ false, /*bIncludeEditInline*/ false))
	{
		OutCodecId.Reset();
		return false;
	}
	const FAssetDocumentEditorTypedValueCodecResult Result = InspectAssetDocumentEditorTypedValueProperty(Property);
	OutCodecId = Result.bSucceeded ? Result.CodecId : FString();
	return Result.bSucceeded;
}

bool Encode(FProperty& Property, const void* ValuePtr, FAssetDocumentRuntimeValueDto& OutValue, FString& OutDiagnosticCode)
{
	OutValue = {};
	if (ValuePtr == nullptr)
	{
		OutDiagnosticCode = TEXT("TypedValueCodec.InvalidNativeValue");
		return false;
	}
	FAssetDocumentEditorScopedPropertyValue Copy(Property);
	if (!Copy.IsValid())
	{
		OutDiagnosticCode = TEXT("TypedValueCodec.InvalidNativeValue");
		return false;
	}
	Copy.ReadFrom(ValuePtr);
	const FAssetDocumentEditorTypedValueCodecResult Result = EncodeAssetDocumentEditorTypedValue(Copy, OutValue);
	OutDiagnosticCode = Result.bSucceeded ? FString() : Result.DiagnosticCode;
	return Result.bSucceeded;
}

bool Decode(FProperty& Property, const FAssetDocumentRuntimeValueDto& Value, void* ValuePtr, FString& OutDiagnosticCode)
{
	return Decode(Property, Value, ValuePtr, OutDiagnosticCode, nullptr);
}

bool Decode(FProperty& Property, const FAssetDocumentRuntimeValueDto& Value, void* ValuePtr, FString& OutDiagnosticCode, UObject* Owner)
{
	if (ValuePtr == nullptr)
	{
		OutDiagnosticCode = TEXT("TypedValueCodec.InvalidNativeValue");
		return false;
	}
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> Decoded;
	FAssetDocumentEditorDecodeBase Base;
	Base.Value = ValuePtr;
	Base.Owner = Owner;
	const FAssetDocumentEditorTypedValueCodecResult Result = DecodeAssetDocumentEditorTypedValue(Property, Value, Decoded, Base);
	if (!Result.bSucceeded || !Decoded.IsValid() || !Decoded->IsValid())
	{
		OutDiagnosticCode = Result.bSucceeded ? FString(TEXT("TypedValueCodec.ConversionFailed")) : Result.DiagnosticCode;
		return false;
	}
	if (Decoded->HasStagedObjects())
	{
		OutDiagnosticCode = TEXT("TypedValueCodec.InstancedObjectNeedsOwner");
		return false;
	}
	Property.CopyCompleteValue(ValuePtr, Decoded->GetMemory());
	OutDiagnosticCode.Reset();
	return true;
}
}
