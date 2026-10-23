// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Struct/AssetDocumentEditorStructCodec.h"

#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace
{
FAssetDocumentEditorStructCodecResult MakeStructCodecFailure(
	FString DiagnosticCode)
{
	FAssetDocumentEditorStructCodecResult Result;
	Result.DiagnosticCode = MoveTemp(DiagnosticCode);
	return Result;
}

bool CollectAdmittedProperties(
	UScriptStruct& Schema,
	TArray<FProperty*>& OutProperties)
{
	for (TFieldIterator<FProperty> It(&Schema); It; ++It)
	{
		if (!InspectAssetDocumentEditorTypedValueProperty(**It).bSucceeded)
		{
			return false;
		}
		OutProperties.Add(*It);
	}
	return !OutProperties.IsEmpty();
}

bool HasExactStructObjectShape(
	const FAssetDocumentRuntimeValueDto& Value,
	TConstArrayView<FProperty*> Properties)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::Object
		&& Value.ObjectKeys.Num() == Properties.Num()
		&& Value.Elements.Num() == Properties.Num();
}

bool TryAppendExactInputField(
	const FAssetDocumentRuntimeValueDto& Value,
	FProperty& Property,
	TSet<FString>& Seen,
	TArray<const FAssetDocumentRuntimeValueDto*>& OutFields)
{
	const int32 Index = Value.ObjectKeys.Find(Property.GetAuthoredName());
	if (!Value.Elements.IsValidIndex(Index)
		|| Seen.Contains(Value.ObjectKeys[Index]))
	{
		return false;
	}
	Seen.Add(Value.ObjectKeys[Index]);
	OutFields.Add(&Value.Elements[Index]);
	return true;
}

bool FindExactInputFields(
	const FAssetDocumentRuntimeValueDto& Value,
	TConstArrayView<FProperty*> Properties,
	TArray<const FAssetDocumentRuntimeValueDto*>& OutFields)
{
	if (!HasExactStructObjectShape(Value, Properties))
	{
		return false;
	}
	TSet<FString> Seen;
	for (FProperty* Property : Properties)
	{
		if (!TryAppendExactInputField(
			Value, *Property, Seen, OutFields))
		{
			return false;
		}
	}
	return Seen.Num() == Value.ObjectKeys.Num();
}

bool DecodeFieldIntoScope(
	FProperty& Property,
	const FAssetDocumentRuntimeValueDto& Value,
	FStructOnScope& Scope,
	FString& OutDiagnosticCode)
{
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> NativeValue;
	const FAssetDocumentEditorTypedValueCodecResult Decode =
		DecodeAssetDocumentEditorTypedValue(Property, Value, NativeValue);
	if (!Decode.bSucceeded || !NativeValue)
	{
		OutDiagnosticCode = Decode.DiagnosticCode;
		return false;
	}
	Property.CopyCompleteValueFromScriptVM_InContainer(
		Scope.GetStructMemory(), NativeValue->GetMemory());
	return true;
}

bool PopulateScope(
	TConstArrayView<FProperty*> Properties,
	TConstArrayView<const FAssetDocumentRuntimeValueDto*> Fields,
	FStructOnScope& Scope,
	FString& OutDiagnosticCode)
{
	for (int32 Index = 0; Index < Properties.Num(); ++Index)
	{
		if (!DecodeFieldIntoScope(
			*Properties[Index], *Fields[Index], Scope, OutDiagnosticCode))
		{
			return false;
		}
	}
	return true;
}

bool EncodeFieldFromScope(
	FProperty& Property,
	const FStructOnScope& Scope,
	FAssetDocumentEditorValueField& OutField,
	FString& OutDiagnosticCode)
{
	FAssetDocumentEditorScopedPropertyValue NativeValue(Property);
	Property.CopyCompleteValueToScriptVM_InContainer(
		NativeValue.GetMemory(), Scope.GetStructMemory());
	const FAssetDocumentEditorTypedValueCodecResult Encode =
		EncodeAssetDocumentEditorTypedValue(NativeValue, OutField.Value);
	OutField.Name = Property.GetAuthoredName();
	OutDiagnosticCode = Encode.DiagnosticCode;
	return Encode.bSucceeded;
}

bool BuildCanonicalValue(
	TConstArrayView<FProperty*> Properties,
	const FStructOnScope& Scope,
	FAssetDocumentRuntimeValueDto& OutValue,
	FString& OutDiagnosticCode)
{
	TArray<FAssetDocumentEditorValueField> Fields;
	Fields.Reserve(Properties.Num());
	for (FProperty* Property : Properties)
	{
		FAssetDocumentEditorValueField& Field = Fields.AddDefaulted_GetRef();
		if (!EncodeFieldFromScope(
			*Property, Scope, Field, OutDiagnosticCode))
		{
			return false;
		}
	}
	return TryBuildAssetDocumentEditorValueObject(
		Fields, OutValue, OutDiagnosticCode);
}

bool PrepareStructRoundTrip(
	UScriptStruct& Schema,
	const FAssetDocumentRuntimeValueDto& Value,
	TArray<FProperty*>& OutProperties,
	TArray<const FAssetDocumentRuntimeValueDto*>& OutFields,
	FString& OutDiagnosticCode)
{
	if (!CollectAdmittedProperties(Schema, OutProperties))
	{
		OutDiagnosticCode = TEXT("StructCodec.UnsupportedSchema");
		return false;
	}
	if (!FindExactInputFields(Value, OutProperties, OutFields))
	{
		OutDiagnosticCode = TEXT("StructCodec.ValueShapeMismatch");
		return false;
	}
	return true;
}
}

bool IsAssetDocumentEditorStructSchemaAdmitted(UScriptStruct& Schema)
{
	TArray<FProperty*> Properties;
	return CollectAdmittedProperties(Schema, Properties);
}

FAssetDocumentEditorStructCodecResult
RoundTripAssetDocumentEditorStructValue(
	UScriptStruct& Schema,
	const FAssetDocumentRuntimeValueDto& Value)
{
	TArray<FProperty*> Properties;
	TArray<const FAssetDocumentRuntimeValueDto*> Fields;
	FString DiagnosticCode;
	if (!PrepareStructRoundTrip(
		Schema, Value, Properties, Fields, DiagnosticCode))
	{
		return MakeStructCodecFailure(MoveTemp(DiagnosticCode));
	}
	FStructOnScope Scope(&Schema);
	if (!Scope.IsValid())
	{
		return MakeStructCodecFailure(TEXT("StructCodec.NativeScopeFailed"));
	}
	if (!PopulateScope(Properties, Fields, Scope, DiagnosticCode))
	{
		return MakeStructCodecFailure(MoveTemp(DiagnosticCode));
	}
	FAssetDocumentEditorStructCodecResult Result;
	Result.bSucceeded = BuildCanonicalValue(
		Properties, Scope, Result.CanonicalValue, Result.DiagnosticCode);
	return Result;
}
