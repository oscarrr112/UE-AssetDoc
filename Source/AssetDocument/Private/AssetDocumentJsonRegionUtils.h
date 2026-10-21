// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentProfile.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct ASSETDOCUMENT_API FAssetDocumentJsonRegionUtils
{
	static FAssetDocumentCapabilityResult Failure(
		const FString& Path,
		const FString& Code,
		const FString& Message);

	static FAssetDocumentCapabilityResult RequireObjectValue(
		const TSharedPtr<FJsonValue>& Value,
		const FString& Path,
		TSharedPtr<FJsonObject>& OutObject);

	static FAssetDocumentCapabilityResult RequireArrayValue(
		const TSharedPtr<FJsonValue>& Value,
		const FString& Path,
		TArray<TSharedPtr<FJsonValue>>& OutArray);

	static FAssetDocumentCapabilityResult RequireStringField(
		const TSharedPtr<FJsonObject>& Object,
		const FString& FieldName,
		const FString& Path,
		FString& OutString,
		const FString& Code = TEXT("InvalidStringField"));

	static FAssetDocumentCapabilityResult RequireNumberField(
		const TSharedPtr<FJsonObject>& Object,
		const FString& FieldName,
		const FString& Path,
		double& OutNumber,
		const FString& Code = TEXT("InvalidNumericField"));

	static FAssetDocumentCapabilityResult RequireBoolField(
		const TSharedPtr<FJsonObject>& Object,
		const FString& FieldName,
		const FString& Path,
		bool& OutBool,
		const FString& Code = TEXT("InvalidBooleanField"));

	static FString EscapeJsonPointerToken(const FString& Token);
	static FString AppendJsonPointerToken(const FString& BasePath, const FString& Token);
	static FString AppendJsonPointerIndex(const FString& BasePath, int32 Index);
	static FString MakeBodyPath(const FString& BodyKey);
	static FString MakeBodyArrayItemPath(const FString& BodyKey, int32 Index);
	static TSharedPtr<FJsonValue> DeepCloneJsonValue(const TSharedPtr<FJsonValue>& Value);
	static TSharedPtr<FJsonObject> DeepCloneJsonObject(const TSharedPtr<FJsonObject>& Object);
	static FString JsonValueToComparableString(const TSharedPtr<FJsonValue>& Value);

	static void AddDiffEntry(
		TArray<TSharedPtr<FJsonValue>>& Entries,
		const FString& Path,
		const FString& Status,
		TSharedPtr<FJsonValue> Current,
		TSharedPtr<FJsonValue> Desired);
};
