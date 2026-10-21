// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentJsonRegionUtils.h"

#include "AssetDocumentCanonicalJson.h"
#include "Utils/AssetDocumentJsonKeys.h"

FAssetDocumentCapabilityResult FAssetDocumentJsonRegionUtils::Failure(
	const FString& Path,
	const FString& Code,
	const FString& Message)
{
	return FAssetDocumentCapabilityResult::Failure(Message, Path, Code);
}

FAssetDocumentCapabilityResult FAssetDocumentJsonRegionUtils::RequireObjectValue(
	const TSharedPtr<FJsonValue>& Value,
	const FString& Path,
	TSharedPtr<FJsonObject>& OutObject)
{
	if (!Value.IsValid() || Value->Type != EJson::Object)
	{
		return Failure(Path, TEXT("InvalidBodySectionType"), TEXT("Expected a JSON object"));
	}

	OutObject = Value->AsObject();
	if (!OutObject.IsValid())
	{
		return Failure(Path, TEXT("InvalidBodySectionType"), TEXT("Expected a JSON object"));
	}

	return FAssetDocumentCapabilityResult::Success();
}

FAssetDocumentCapabilityResult FAssetDocumentJsonRegionUtils::RequireArrayValue(
	const TSharedPtr<FJsonValue>& Value,
	const FString& Path,
	TArray<TSharedPtr<FJsonValue>>& OutArray)
{
	if (!Value.IsValid() || Value->Type != EJson::Array)
	{
		return Failure(Path, TEXT("InvalidBodySectionType"), TEXT("Expected a JSON array"));
	}

	OutArray = Value->AsArray();
	return FAssetDocumentCapabilityResult::Success();
}

FAssetDocumentCapabilityResult FAssetDocumentJsonRegionUtils::RequireStringField(
	const TSharedPtr<FJsonObject>& Object,
	const FString& FieldName,
	const FString& Path,
	FString& OutString,
	const FString& Code)
{
	const TSharedPtr<FJsonValue> FieldValue = Object.IsValid() ? Object->TryGetField(FieldName) : nullptr;
	if (!FieldValue.IsValid() || FieldValue->Type != EJson::String)
	{
		return Failure(Path, Code, FString::Printf(TEXT("%s must be a non-empty string"), *FieldName));
	}

	OutString = FieldValue->AsString().TrimStartAndEnd();
	if (OutString.IsEmpty())
	{
		return Failure(Path, Code, FString::Printf(TEXT("%s must be a non-empty string"), *FieldName));
	}
	return FAssetDocumentCapabilityResult::Success();
}

FAssetDocumentCapabilityResult FAssetDocumentJsonRegionUtils::RequireNumberField(
	const TSharedPtr<FJsonObject>& Object,
	const FString& FieldName,
	const FString& Path,
	double& OutNumber,
	const FString& Code)
{
	const TSharedPtr<FJsonValue> FieldValue = Object.IsValid() ? Object->TryGetField(FieldName) : nullptr;
	if (!FieldValue.IsValid() || FieldValue->Type != EJson::Number)
	{
		return Failure(Path, Code, FString::Printf(TEXT("%s must be a number"), *FieldName));
	}
	OutNumber = FieldValue->AsNumber();
	return FAssetDocumentCapabilityResult::Success();
}

FAssetDocumentCapabilityResult FAssetDocumentJsonRegionUtils::RequireBoolField(
	const TSharedPtr<FJsonObject>& Object,
	const FString& FieldName,
	const FString& Path,
	bool& OutBool,
	const FString& Code)
{
	const TSharedPtr<FJsonValue> FieldValue = Object.IsValid() ? Object->TryGetField(FieldName) : nullptr;
	if (!FieldValue.IsValid() || FieldValue->Type != EJson::Boolean)
	{
		return Failure(Path, Code, FString::Printf(TEXT("%s must be a boolean"), *FieldName));
	}
	OutBool = FieldValue->AsBool();
	return FAssetDocumentCapabilityResult::Success();
}

FString FAssetDocumentJsonRegionUtils::EscapeJsonPointerToken(const FString& Token)
{
	return Token.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
}

FString FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(const FString& BasePath, const FString& Token)
{
	const FString EscapedToken = EscapeJsonPointerToken(Token);
	return BasePath.IsEmpty()
		? FString::Printf(TEXT("/%s"), *EscapedToken)
		: FString::Printf(TEXT("%s/%s"), *BasePath, *EscapedToken);
}

FString FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(const FString& BasePath, int32 Index)
{
	return AppendJsonPointerToken(BasePath, FString::FromInt(Index));
}

FString FAssetDocumentJsonRegionUtils::MakeBodyPath(const FString& BodyKey)
{
	return AppendJsonPointerToken(TEXT("/Body"), BodyKey);
}

FString FAssetDocumentJsonRegionUtils::MakeBodyArrayItemPath(const FString& BodyKey, int32 Index)
{
	return AppendJsonPointerIndex(MakeBodyPath(BodyKey), Index);
}

TSharedPtr<FJsonValue> FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return nullptr;
	}

	switch (Value->Type)
	{
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		return MakeShared<FJsonValueObject>(
			Object.IsValid() ? DeepCloneJsonObject(Object) : MakeShared<FJsonObject>());
	}
	case EJson::Array:
	{
		TArray<TSharedPtr<FJsonValue>> CloneArray;
		CloneArray.Reserve(Value->AsArray().Num());
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			CloneArray.Add(DeepCloneJsonValue(Item));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(CloneArray));
	}
	case EJson::String:
		return MakeShared<FJsonValueString>(Value->AsString());
	case EJson::Number:
		return MakeShared<FJsonValueNumber>(Value->AsNumber());
	case EJson::Boolean:
		return MakeShared<FJsonValueBoolean>(Value->AsBool());
	case EJson::Null:
	default:
		return MakeShared<FJsonValueNull>();
	}
}

TSharedPtr<FJsonObject> FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
	{
		return nullptr;
	}

	TSharedRef<FJsonObject> Clone = MakeShared<FJsonObject>();
	TArray<FString> Keys;
	AssetDocumentServerJson::GetKeys(*Object, Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		Clone->SetField(Key, DeepCloneJsonValue(AssetDocumentServerJson::FindFieldRef(*Object, Key)));
	}
	return Clone;
}

FString FAssetDocumentJsonRegionUtils::JsonValueToComparableString(const TSharedPtr<FJsonValue>& Value)
{
	return FAssetDocumentCanonicalJson::WriteCanonicalJson(Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
}

void FAssetDocumentJsonRegionUtils::AddDiffEntry(
	TArray<TSharedPtr<FJsonValue>>& Entries,
	const FString& Path,
	const FString& Status,
	TSharedPtr<FJsonValue> Current,
	TSharedPtr<FJsonValue> Desired)
{
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("path"), Path);
	Entry->SetStringField(TEXT("status"), Status);
	Entry->SetField(TEXT("current"), Current.IsValid() ? Current : MakeShared<FJsonValueNull>());
	Entry->SetField(TEXT("desired"), Desired.IsValid() ? Desired : MakeShared<FJsonValueNull>());
	Entries.Add(MakeShared<FJsonValueObject>(Entry));
}
