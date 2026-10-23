// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGraphValue.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Misc/Base64.h"

void FAssetDocumentGraphError::Set(FStringView InCode, FStringView InMessage, FStringView InPath)
{
	Code = FString(InCode);
	Message = FString(InMessage);
	Path = FString(InPath);
}

void FAssetDocumentGraphError::AddContext(FStringView Key, FAssetDocumentRuntimeValueDto Value)
{
	FAssetDocumentRuntimeDiagnosticContext& Entry = Context.AddDefaulted_GetRef();
	Entry.Key = FString(Key);
	Entry.Value = MoveTemp(Value);
}

void FAssetDocumentGraphError::AddContextString(FStringView Key, FStringView Value)
{
	AddContext(Key, UE::AssetDocument::Graph::MakeString(FString(Value)));
}

FString FormatAssetDocumentGraphGuid(const FGuid& Guid)
{
	return Guid.ToString(EGuidFormats::Digits);
}

namespace UE::AssetDocument::Graph
{
FValue MakeNull()
{
	return FValue();
}

FValue MakeBool(bool bValue)
{
	FValue V;
	V.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	V.BooleanValue = bValue;
	return V;
}

FValue MakeInt(int64 Value)
{
	FValue V;
	V.Kind = EAssetDocumentRuntimeValueKind::Integer;
	V.IntegerValue = Value;
	return V;
}

FValue MakeNumber(double Value)
{
	FValue V;
	V.Kind = EAssetDocumentRuntimeValueKind::Number;
	V.NumberValue = FMath::IsFinite(Value) ? Value : 0.0;
	return V;
}

FValue MakeString(FString Value)
{
	FValue V;
	V.Kind = EAssetDocumentRuntimeValueKind::String;
	V.StringValue = MoveTemp(Value);
	return V;
}

FValue MakeArray(TArray<FValue> Elements)
{
	FValue V;
	V.Kind = EAssetDocumentRuntimeValueKind::Array;
	V.Elements = MoveTemp(Elements);
	return V;
}

FValue MakeStringArray(TConstArrayView<FString> Values)
{
	TArray<FValue> Elements;
	for (const FString& Value : Values)
	{
		Elements.Add(MakeString(Value));
	}
	return MakeArray(MoveTemp(Elements));
}

FValue MakeEmptyObject()
{
	FValue V;
	V.Kind = EAssetDocumentRuntimeValueKind::Object;
	return V;
}

FObjectBuilder& FObjectBuilder::Add(FString Key, FValue Value)
{
	const int32 Existing = Object.ObjectKeys.IndexOfByKey(Key);
	if (Existing != INDEX_NONE)
	{
		Object.Elements[Existing] = MoveTemp(Value);
		return *this;
	}
	Object.ObjectKeys.Add(MoveTemp(Key));
	Object.Elements.Add(MoveTemp(Value));
	return *this;
}

FValue FObjectBuilder::Build()
{
	FValue Result = MoveTemp(Object);
	Object = MakeEmptyObject();
	return Result;
}

const FValue* FindField(const FValue& Object, FStringView Key)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return nullptr;
	}
	for (int32 Index = 0; Index < Object.ObjectKeys.Num() && Index < Object.Elements.Num(); ++Index)
	{
		if (FStringView(Object.ObjectKeys[Index]).Equals(Key, ESearchCase::CaseSensitive))
		{
			return &Object.Elements[Index];
		}
	}
	return nullptr;
}

int32 CountValueNodes(const FValue& Value)
{
	int32 Count = 1;
	for (const FValue& Element : Value.Elements)
	{
		Count += CountValueNodes(Element);
	}
	return Count;
}

bool TryParseGuid(FStringView Text, FGuid& OutGuid)
{
	FString Trimmed(Text);
	if (Trimmed.Len() != 32)
	{
		return false;
	}
	for (TCHAR C : Trimmed)
	{
		if (!FChar::IsHexDigit(C))
		{
			return false;
		}
	}
	return FGuid::ParseExact(Trimmed.ToUpper(), EGuidFormats::Digits, OutGuid);
}

FReader::FReader(const FValue& InObject, FString InPath, FAssetDocumentGraphError& InError)
	: Object_(InObject)
	, Path(MoveTemp(InPath))
	, Error(InError)
{
}

bool FReader::IsObject() const
{
	return Object_.Kind == EAssetDocumentRuntimeValueKind::Object;
}

FString FReader::FieldPath(FStringView Key) const
{
	if (Key.IsEmpty())
	{
		return Path.IsEmpty() ? FString(TEXT("/")) : Path;
	}
	return FString::Printf(TEXT("%s/%.*s"), *Path, Key.Len(), Key.GetData());
}

bool FReader::Fail(FStringView Code, FStringView Message, FStringView Key)
{
	if (Error.Code.IsEmpty())
	{
		Error.Set(Code, Message, FieldPath(Key));
	}
	return false;
}

bool FReader::OnlyFields(std::initializer_list<const TCHAR*> Allowed)
{
	if (!IsObject())
	{
		return Fail(TEXT("Graph.InvalidArgument"), TEXT("Expected a JSON object."));
	}
	for (const FString& Key : Object_.ObjectKeys)
	{
		if (Key == TEXT("schema_version"))
		{
			const FValue* Version = Find(Key);
			const bool bOne = Version != nullptr
				&& ((Version->Kind == EAssetDocumentRuntimeValueKind::Integer && Version->IntegerValue == 1)
					|| (Version->Kind == EAssetDocumentRuntimeValueKind::Number && Version->NumberValue == 1.0));
			if (!bOne)
			{
				return Fail(TEXT("Graph.UnsupportedSchemaVersion"), TEXT("schema_version must be 1."), Key);
			}
			continue;
		}
		bool bKnown = false;
		for (const TCHAR* Name : Allowed)
		{
			bKnown |= Key.Equals(Name, ESearchCase::CaseSensitive);
		}
		if (!bKnown)
		{
			TArray<FString> AllowedNames;
			for (const TCHAR* Name : Allowed)
			{
				AllowedNames.Add(Name);
			}
			Fail(TEXT("Graph.UnknownField"),
				FString::Printf(TEXT("Unknown field '%s'. Allowed fields: %s."), *Key,
					*FString::Join(AllowedNames, TEXT(", "))), Key);
			Error.AddContext(TEXT("allowed_fields"), MakeStringArray(AllowedNames));
			return false;
		}
	}
	return true;
}

bool FReader::Has(FStringView Key) const
{
	const FValue* Value = Find(Key);
	return Value != nullptr && Value->Kind != EAssetDocumentRuntimeValueKind::Null;
}

const FValue* FReader::Find(FStringView Key) const
{
	return FindField(Object_, Key);
}

bool FReader::RefuseNull(FStringView Key, FStringView Code, FStringView Message)
{
	const FValue* Value = Find(Key);
	if (Value == nullptr || Value->Kind != EAssetDocumentRuntimeValueKind::Null)
	{
		return true;
	}
	return Fail(Code, FString::Printf(TEXT("%.*s is null: %.*s"), Key.Len(), Key.GetData(), Message.Len(), Message.GetData()), Key);
}

bool FReader::String(FStringView Key, FString& Out, bool bRequired)
{
	const FValue* Value = Find(Key);
	if (Value == nullptr || Value->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		return bRequired ? Fail(TEXT("Graph.InvalidArgument"), TEXT("Missing required string field."), Key) : true;
	}
	if (Value->Kind != EAssetDocumentRuntimeValueKind::String)
	{
		return Fail(TEXT("Graph.InvalidArgument"), TEXT("Field must be a string."), Key);
	}
	Out = Value->StringValue;
	return true;
}

bool FReader::Bool(FStringView Key, bool& Out, bool bRequired)
{
	const FValue* Value = Find(Key);
	if (Value == nullptr || Value->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		return bRequired ? Fail(TEXT("Graph.InvalidArgument"), TEXT("Missing required boolean field."), Key) : true;
	}
	if (Value->Kind != EAssetDocumentRuntimeValueKind::Boolean)
	{
		return Fail(TEXT("Graph.InvalidArgument"), TEXT("Field must be a boolean."), Key);
	}
	Out = Value->BooleanValue;
	return true;
}

bool FReader::Number(FStringView Key, double& Out, bool bRequired)
{
	const FValue* Value = Find(Key);
	if (Value == nullptr || Value->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		return bRequired ? Fail(TEXT("Graph.InvalidArgument"), TEXT("Missing required number field."), Key) : true;
	}
	if (Value->Kind == EAssetDocumentRuntimeValueKind::Integer)
	{
		Out = static_cast<double>(Value->IntegerValue);
		return true;
	}
	if (Value->Kind == EAssetDocumentRuntimeValueKind::Number && FMath::IsFinite(Value->NumberValue))
	{
		Out = Value->NumberValue;
		return true;
	}
	return Fail(TEXT("Graph.InvalidArgument"), TEXT("Field must be a finite number."), Key);
}

bool FReader::Int(FStringView Key, int64& Out, bool bRequired)
{
	double Number = 0.0;
	const FValue* Value = Find(Key);
	if (Value == nullptr || Value->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		return bRequired ? Fail(TEXT("Graph.InvalidArgument"), TEXT("Missing required integer field."), Key) : true;
	}
	if (!this->Number(Key, Number, true) || FMath::FloorToDouble(Number) != Number)
	{
		return Fail(TEXT("Graph.InvalidArgument"), TEXT("Field must be an integer."), Key);
	}
	Out = static_cast<int64>(Number);
	return true;
}

bool FReader::Guid(FStringView Key, FGuid& Out, bool bRequired)
{
	FString Text;
	if (!String(Key, Text, bRequired))
	{
		return false;
	}
	if (Text.IsEmpty() && !bRequired)
	{
		return true;
	}
	if (!TryParseGuid(Text, Out))
	{
		return Fail(TEXT("Graph.InvalidArgument"),
			TEXT("GUIDs are 32 hexadecimal digits without hyphens."), Key);
	}
	return true;
}

bool FReader::Object(FStringView Key, const FValue*& Out, bool bRequired)
{
	Out = nullptr;
	const FValue* Value = Find(Key);
	if (Value == nullptr || Value->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		return bRequired ? Fail(TEXT("Graph.InvalidArgument"), TEXT("Missing required object field."), Key) : true;
	}
	if (Value->Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Fail(TEXT("Graph.InvalidArgument"), TEXT("Field must be an object."), Key);
	}
	Out = Value;
	return true;
}

bool FReader::Array(FStringView Key, const FValue*& Out, bool bRequired)
{
	Out = nullptr;
	const FValue* Value = Find(Key);
	if (Value == nullptr || Value->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		return bRequired ? Fail(TEXT("Graph.InvalidArgument"), TEXT("Missing required array field."), Key) : true;
	}
	if (Value->Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return Fail(TEXT("Graph.InvalidArgument"), TEXT("Field must be an array."), Key);
	}
	Out = Value;
	return true;
}

FReader FReader::Child(FStringView Key, const FValue& Value) const
{
	return FReader(Value, FieldPath(Key), Error);
}

TSharedPtr<FJsonValue> ValueToJson(const FValue& Value)
{
	switch (Value.Kind)
	{
	case EAssetDocumentRuntimeValueKind::Boolean:
		return MakeShared<FJsonValueBoolean>(Value.BooleanValue);
	case EAssetDocumentRuntimeValueKind::Integer:
		return MakeShared<FJsonValueNumberString>(LexToString(Value.IntegerValue));
	case EAssetDocumentRuntimeValueKind::Number:
		return MakeShared<FJsonValueNumber>(Value.NumberValue);
	case EAssetDocumentRuntimeValueKind::String:
		return MakeShared<FJsonValueString>(Value.StringValue);
	case EAssetDocumentRuntimeValueKind::Bytes:
		return MakeShared<FJsonValueString>(FBase64::Encode(Value.BytesValue));
	case EAssetDocumentRuntimeValueKind::Array:
	{
		TArray<TSharedPtr<FJsonValue>> Elements;
		for (const FValue& Element : Value.Elements)
		{
			Elements.Add(ValueToJson(Element));
		}
		return MakeShared<FJsonValueArray>(Elements);
	}
	case EAssetDocumentRuntimeValueKind::Object:
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		for (int32 Index = 0; Index < Value.ObjectKeys.Num() && Index < Value.Elements.Num(); ++Index)
		{
			Object->SetField(Value.ObjectKeys[Index], ValueToJson(Value.Elements[Index]));
		}
		return MakeShared<FJsonValueObject>(Object);
	}
	default:
		return MakeShared<FJsonValueNull>();
	}
}

FValue JsonToValue(const TSharedPtr<FJsonValue>& Json)
{
	if (!Json.IsValid())
	{
		return MakeNull();
	}
	switch (Json->Type)
	{
	case EJson::Boolean:
		return MakeBool(Json->AsBool());
	case EJson::Number:
	{
		const double Number = Json->AsNumber();
		if (FMath::FloorToDouble(Number) == Number && FMath::Abs(Number) < 9.0e15)
		{
			return MakeInt(static_cast<int64>(Number));
		}
		return MakeNumber(Number);
	}
	case EJson::String:
		return MakeString(Json->AsString());
	case EJson::Array:
	{
		TArray<FValue> Elements;
		for (const TSharedPtr<FJsonValue>& Element : Json->AsArray())
		{
			Elements.Add(JsonToValue(Element));
		}
		return MakeArray(MoveTemp(Elements));
	}
	case EJson::Object:
	{
		FObjectBuilder Builder;
		for (const auto& Pair : Json->AsObject()->Values)
		{
			Builder.Add(FString(*Pair.Key), JsonToValue(Pair.Value));
		}
		return Builder.Build();
	}
	default:
		return MakeNull();
	}
}

bool TryParseJsonValue(const FString& Text, FValue& OutValue)
{
	TSharedPtr<FJsonValue> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return false;
	}
	OutValue = JsonToValue(Json);
	return true;
}

FString ValueToJsonString(const FValue& Value, bool bPretty)
{
	FString Output;
	const TSharedPtr<FJsonValue> Json = ValueToJson(Value);
	if (bPretty)
	{
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(Json, FString(), Writer);
	}
	else
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(Json, FString(), Writer);
	}
	return Output;
}
} // namespace UE::AssetDocument::Graph
