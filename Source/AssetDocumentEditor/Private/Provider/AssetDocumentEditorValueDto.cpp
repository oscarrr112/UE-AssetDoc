// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/AssetDocumentEditorValueDto.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utils/AssetDocumentJsonKeys.h"

namespace
{
constexpr int32 AssetDocumentEditorValueMaximumDepth = 64;
constexpr int32 AssetDocumentEditorValueMaximumNodes = 4096;

bool FailAssetDocumentEditorValueDto(
	FString& OutDiagnosticCode,
	const TCHAR* DiagnosticCode)
{
	OutDiagnosticCode = DiagnosticCode;
	return false;
}

bool IsAssetDocumentEditorT01Value(
	const FAssetDocumentRuntimeValueDto& Value)
{
	if (!ValidateAssetDocumentRuntimeValue(Value).bValid
		|| Value.Kind == EAssetDocumentRuntimeValueKind::Bytes)
	{
		return false;
	}
	for (const FAssetDocumentRuntimeValueDto& Element : Value.Elements)
	{
		if (!IsAssetDocumentEditorT01Value(Element))
		{
			return false;
		}
	}
	return true;
}

bool AreAssetDocumentEditorFieldNamesUnique(
	TConstArrayView<FAssetDocumentEditorValueField> Fields)
{
	TSet<FString> Names;
	for (const FAssetDocumentEditorValueField& Field : Fields)
	{
		if (Field.Name.IsEmpty() || Names.Contains(Field.Name))
		{
			return false;
		}
		Names.Add(Field.Name);
	}
	return true;
}

bool AreAssetDocumentEditorSpecsUnique(
	TConstArrayView<FAssetDocumentEditorValueFieldSpec> Fields)
{
	TSet<FString> Names;
	for (const FAssetDocumentEditorValueFieldSpec& Field : Fields)
	{
		if (Field.Name.IsEmpty() || Names.Contains(Field.Name))
		{
			return false;
		}
		Names.Add(Field.Name);
	}
	return true;
}

const FAssetDocumentRuntimeValueDto* FindAssetDocumentEditorObjectField(
	const FAssetDocumentRuntimeValueDto& Value,
	const FString& Name)
{
	const int32 Index = Value.ObjectKeys.IndexOfByKey(Name);
	return Value.Elements.IsValidIndex(Index) ? &Value.Elements[Index] : nullptr;
}

bool TryConvertAssetDocumentEditorValueToJsonRecursive(
	const FAssetDocumentRuntimeValueDto& Value,
	TSharedPtr<FJsonValue>& OutJsonValue,
	FString& OutDiagnosticCode);

bool TryConvertAssetDocumentEditorElementsToJson(
	TConstArrayView<FAssetDocumentRuntimeValueDto> Elements,
	TArray<TSharedPtr<FJsonValue>>& OutElements,
	FString& OutDiagnosticCode)
{
	for (const FAssetDocumentRuntimeValueDto& Element : Elements)
	{
		TSharedPtr<FJsonValue> JsonElement;
		if (!TryConvertAssetDocumentEditorValueToJsonRecursive(
			Element, JsonElement, OutDiagnosticCode))
		{
			return false;
		}
		OutElements.Add(MoveTemp(JsonElement));
	}
	return true;
}

bool TryConvertAssetDocumentEditorObjectToJson(
	const FAssetDocumentRuntimeValueDto& Value,
	TSharedPtr<FJsonValue>& OutJsonValue,
	FString& OutDiagnosticCode)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
	{
		TSharedPtr<FJsonValue> JsonElement;
		if (!TryConvertAssetDocumentEditorValueToJsonRecursive(
			Value.Elements[Index], JsonElement, OutDiagnosticCode))
		{
			return false;
		}
		Object->SetField(Value.ObjectKeys[Index], JsonElement);
	}
	OutJsonValue = MakeShared<FJsonValueObject>(Object);
	return true;
}

bool TryConvertAssetDocumentEditorPrimitiveToJson(
	const FAssetDocumentRuntimeValueDto& Value,
	TSharedPtr<FJsonValue>& OutJsonValue)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		OutJsonValue = MakeShared<FJsonValueNull>();
		return true;
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Boolean)
	{
		OutJsonValue = MakeShared<FJsonValueBoolean>(Value.BooleanValue);
		return true;
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
	{
		OutJsonValue = MakeShared<FJsonValueNumberString>(
			LexToString(Value.IntegerValue));
		return true;
	}
	OutJsonValue = MakeShared<FJsonValueNumber>(Value.NumberValue);
	return true;
}

bool TryConvertAssetDocumentEditorArrayToJson(
	const FAssetDocumentRuntimeValueDto& Value,
	TSharedPtr<FJsonValue>& OutJsonValue,
	FString& OutDiagnosticCode)
{
	TArray<TSharedPtr<FJsonValue>> Elements;
	if (!TryConvertAssetDocumentEditorElementsToJson(
		Value.Elements, Elements, OutDiagnosticCode))
	{
		return false;
	}
	OutJsonValue = MakeShared<FJsonValueArray>(MoveTemp(Elements));
	return true;
}

bool TryConvertAssetDocumentEditorValueToJsonRecursive(
	const FAssetDocumentRuntimeValueDto& Value,
	TSharedPtr<FJsonValue>& OutJsonValue,
	FString& OutDiagnosticCode)
{
	if (Value.Kind <= EAssetDocumentRuntimeValueKind::Number)
	{
		return TryConvertAssetDocumentEditorPrimitiveToJson(
			Value, OutJsonValue);
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::String)
	{
		OutJsonValue = MakeShared<FJsonValueString>(Value.StringValue);
		return true;
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Array)
	{
		return TryConvertAssetDocumentEditorArrayToJson(
			Value, OutJsonValue, OutDiagnosticCode);
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Object)
	{
		return TryConvertAssetDocumentEditorObjectToJson(
			Value, OutJsonValue, OutDiagnosticCode);
	}
	return FailAssetDocumentEditorValueDto(
		OutDiagnosticCode, TEXT("ValueDto.UnsupportedKind"));
}

bool ConsumeAssetDocumentEditorJsonBudget(
	int32 Depth,
	int32& RemainingNodes,
	FString& OutDiagnosticCode)
{
	if (Depth > AssetDocumentEditorValueMaximumDepth || RemainingNodes <= 0)
	{
		return FailAssetDocumentEditorValueDto(
			OutDiagnosticCode, TEXT("ValueDto.TreeLimitExceeded"));
	}
	--RemainingNodes;
	return true;
}

bool TryConvertAssetDocumentEditorJsonToValueRecursive(
	const TSharedPtr<FJsonValue>& JsonValue,
	FAssetDocumentRuntimeValueDto& OutValue,
	FString& OutDiagnosticCode,
	int32 Depth,
	int32& RemainingNodes);

bool TryConvertAssetDocumentEditorJsonArray(
	const TSharedPtr<FJsonValue>& JsonValue,
	FAssetDocumentRuntimeValueDto& OutValue,
	FString& OutDiagnosticCode,
	int32 Depth,
	int32& RemainingNodes)
{
	const TArray<TSharedPtr<FJsonValue>>* JsonElements = nullptr;
	if (!JsonValue->TryGetArray(JsonElements) || JsonElements == nullptr)
	{
		return FailAssetDocumentEditorValueDto(
			OutDiagnosticCode, TEXT("ValueDto.InvalidJsonArray"));
	}
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Array;
	for (const TSharedPtr<FJsonValue>& JsonElement : *JsonElements)
	{
		FAssetDocumentRuntimeValueDto& Element = OutValue.Elements.AddDefaulted_GetRef();
		if (!TryConvertAssetDocumentEditorJsonToValueRecursive(
			JsonElement, Element, OutDiagnosticCode, Depth + 1, RemainingNodes))
		{
			return false;
		}
	}
	return true;
}

bool IsAssetDocumentEditorValidJsonObject(
	const TSharedPtr<FJsonObject>* JsonObject)
{
	return JsonObject != nullptr && JsonObject->IsValid();
}

bool TryConvertAssetDocumentEditorJsonObject(
	const TSharedPtr<FJsonValue>& JsonValue,
	FAssetDocumentRuntimeValueDto& OutValue,
	FString& OutDiagnosticCode,
	int32 Depth,
	int32& RemainingNodes)
{
	const TSharedPtr<FJsonObject>* JsonObject = nullptr;
	if (!JsonValue->TryGetObject(JsonObject))
	{
		return FailAssetDocumentEditorValueDto(
			OutDiagnosticCode, TEXT("ValueDto.InvalidJsonObject"));
	}
	if (!IsAssetDocumentEditorValidJsonObject(JsonObject))
	{
		return FailAssetDocumentEditorValueDto(
			OutDiagnosticCode, TEXT("ValueDto.InvalidJsonObject"));
	}
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Object;
	AssetDocumentServerJson::GetKeys(**JsonObject, OutValue.ObjectKeys);
	OutValue.ObjectKeys.Sort();
	for (const FString& Key : OutValue.ObjectKeys)
	{
		FAssetDocumentRuntimeValueDto& Element = OutValue.Elements.AddDefaulted_GetRef();
		if (!TryConvertAssetDocumentEditorJsonToValueRecursive(
			*AssetDocumentServerJson::FindField(**JsonObject, Key),
			Element,
			OutDiagnosticCode,
			Depth + 1,
			RemainingNodes))
		{
			return false;
		}
	}
	return true;
}

bool TryConvertAssetDocumentEditorJsonNumber(
	const TSharedPtr<FJsonValue>& JsonValue,
	FAssetDocumentRuntimeValueDto& OutValue,
	FString& OutDiagnosticCode)
{
	double Number = 0.0;
	if (!JsonValue->TryGetNumber(Number) || !FMath::IsFinite(Number))
	{
		return FailAssetDocumentEditorValueDto(
			OutDiagnosticCode, TEXT("ValueDto.InvalidJsonNumber"));
	}
	int64 Integer = 0;
	if (JsonValue->PreferStringRepresentation()
		&& JsonValue->TryGetNumber(Integer))
	{
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Integer;
		OutValue.IntegerValue = Integer;
		return true;
	}
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Number;
	OutValue.NumberValue = Number;
	return true;
}

bool IsAssetDocumentEditorSimpleJsonType(EJson JsonType)
{
	return JsonType == EJson::Null || JsonType == EJson::Boolean;
}

bool TryConvertAssetDocumentEditorSimpleJsonValue(
	const TSharedPtr<FJsonValue>& JsonValue,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	if (JsonValue->Type == EJson::Null)
	{
		return true;
	}
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	return JsonValue->TryGetBool(OutValue.BooleanValue);
}

bool TryConvertAssetDocumentEditorStructuredJsonValue(
	const TSharedPtr<FJsonValue>& JsonValue,
	FAssetDocumentRuntimeValueDto& OutValue,
	FString& OutDiagnosticCode,
	int32 Depth,
	int32& RemainingNodes)
{
	switch (JsonValue->Type)
	{
	case EJson::String:
		OutValue.Kind = EAssetDocumentRuntimeValueKind::String;
		return JsonValue->TryGetString(OutValue.StringValue);
	case EJson::Array:
		return TryConvertAssetDocumentEditorJsonArray(
			JsonValue, OutValue, OutDiagnosticCode, Depth, RemainingNodes);
	case EJson::Object:
		return TryConvertAssetDocumentEditorJsonObject(
			JsonValue, OutValue, OutDiagnosticCode, Depth, RemainingNodes);
	default:
		return FailAssetDocumentEditorValueDto(
			OutDiagnosticCode, TEXT("ValueDto.UnsupportedJsonKind"));
	}
}

bool TryConvertAssetDocumentEditorJsonToValueRecursive(
	const TSharedPtr<FJsonValue>& JsonValue,
	FAssetDocumentRuntimeValueDto& OutValue,
	FString& OutDiagnosticCode,
	int32 Depth,
	int32& RemainingNodes)
{
	OutValue = {};
	if (!JsonValue.IsValid()
		|| !ConsumeAssetDocumentEditorJsonBudget(
			Depth, RemainingNodes, OutDiagnosticCode))
	{
		return false;
	}
	if (IsAssetDocumentEditorSimpleJsonType(JsonValue->Type))
	{
		return TryConvertAssetDocumentEditorSimpleJsonValue(
			JsonValue, OutValue);
	}
	if (JsonValue->Type == EJson::Number)
	{
		return TryConvertAssetDocumentEditorJsonNumber(
			JsonValue, OutValue, OutDiagnosticCode);
	}
	return TryConvertAssetDocumentEditorStructuredJsonValue(
		JsonValue, OutValue, OutDiagnosticCode, Depth, RemainingNodes);
}

bool HasAssetDocumentEditorExactObjectShape(
	const FAssetDocumentRuntimeValueDto& Value,
	TConstArrayView<FAssetDocumentEditorValueFieldSpec> Fields)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::Object
		&& IsAssetDocumentEditorT01Value(Value)
		&& Value.ObjectKeys.Num() == Fields.Num()
		&& AreAssetDocumentEditorSpecsUnique(Fields);
}

bool DoesAssetDocumentEditorFieldMatch(
	const FAssetDocumentRuntimeValueDto* Value,
	EAssetDocumentRuntimeValueKind ExpectedKind)
{
	return Value != nullptr && Value->Kind == ExpectedKind;
}
}

bool TryBuildAssetDocumentEditorValueObject(
	TConstArrayView<FAssetDocumentEditorValueField> Fields,
	FAssetDocumentRuntimeValueDto& OutValue,
	FString& OutDiagnosticCode)
{
	OutValue = {};
	if (!AreAssetDocumentEditorFieldNamesUnique(Fields))
	{
		return FailAssetDocumentEditorValueDto(
			OutDiagnosticCode, TEXT("ValueDto.InvalidFieldNames"));
	}
	TArray<FAssetDocumentEditorValueField> SortedFields(Fields);
	SortedFields.Sort([](
		const FAssetDocumentEditorValueField& Left,
		const FAssetDocumentEditorValueField& Right)
	{
		return Left.Name < Right.Name;
	});
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Object;
	for (FAssetDocumentEditorValueField& Field : SortedFields)
	{
		if (!IsAssetDocumentEditorT01Value(Field.Value))
		{
			OutValue = {};
			return FailAssetDocumentEditorValueDto(
				OutDiagnosticCode, TEXT("ValueDto.InvalidFieldValue"));
		}
		OutValue.ObjectKeys.Add(MoveTemp(Field.Name));
		OutValue.Elements.Add(MoveTemp(Field.Value));
	}
	OutDiagnosticCode.Reset();
	return true;
}

bool TryReadAssetDocumentEditorExactObject(
	const FAssetDocumentRuntimeValueDto& Value,
	TConstArrayView<FAssetDocumentEditorValueFieldSpec> Fields,
	TArray<const FAssetDocumentRuntimeValueDto*>& OutValues,
	FString& OutDiagnosticCode)
{
	OutValues.Reset();
	if (!HasAssetDocumentEditorExactObjectShape(Value, Fields))
	{
		return FailAssetDocumentEditorValueDto(
			OutDiagnosticCode, TEXT("ValueDto.InvalidObjectShape"));
	}
	for (const FAssetDocumentEditorValueFieldSpec& Field : Fields)
	{
		const FAssetDocumentRuntimeValueDto* Found =
			FindAssetDocumentEditorObjectField(Value, Field.Name);
		if (!DoesAssetDocumentEditorFieldMatch(Found, Field.Kind))
		{
			OutValues.Reset();
			return FailAssetDocumentEditorValueDto(
				OutDiagnosticCode, TEXT("ValueDto.FieldMismatch"));
		}
		OutValues.Add(Found);
	}
	OutDiagnosticCode.Reset();
	return true;
}

bool TryConvertAssetDocumentEditorValueToJson(
	const FAssetDocumentRuntimeValueDto& Value,
	TSharedPtr<FJsonValue>& OutJsonValue,
	FString& OutDiagnosticCode)
{
	OutJsonValue.Reset();
	if (!IsAssetDocumentEditorT01Value(Value))
	{
		return FailAssetDocumentEditorValueDto(
			OutDiagnosticCode, TEXT("ValueDto.InvalidRuntimeValue"));
	}
	if (!TryConvertAssetDocumentEditorValueToJsonRecursive(
		Value, OutJsonValue, OutDiagnosticCode))
	{
		OutJsonValue.Reset();
		return false;
	}
	OutDiagnosticCode.Reset();
	return true;
}

bool TryConvertAssetDocumentEditorJsonToValue(
	const TSharedPtr<FJsonValue>& JsonValue,
	FAssetDocumentRuntimeValueDto& OutValue,
	FString& OutDiagnosticCode)
{
	int32 RemainingNodes = AssetDocumentEditorValueMaximumNodes;
	if (!TryConvertAssetDocumentEditorJsonToValueRecursive(
		JsonValue, OutValue, OutDiagnosticCode, 0, RemainingNodes)
		|| !IsAssetDocumentEditorT01Value(OutValue))
	{
		OutValue = {};
		if (OutDiagnosticCode.IsEmpty())
		{
			OutDiagnosticCode = TEXT("ValueDto.InvalidJsonValue");
		}
		return false;
	}
	OutDiagnosticCode.Reset();
	return true;
}
