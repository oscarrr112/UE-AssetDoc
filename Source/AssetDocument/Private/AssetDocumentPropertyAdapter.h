// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentTypes.h"

class FJsonObject;
class FJsonValue;
class FProperty;

struct FAssetDocumentPropertyApplyResult
{
	bool bSuccess = false;
	FString Message;
	TArray<FAssetDocumentDiagnostic> Diagnostics;
};

class ASSETDOCUMENT_API FAssetDocumentPropertyAdapter
{
public:
	static FAssetDocumentPropertyApplyResult ApplyProperties(UObject* Asset, TSharedPtr<FJsonObject> Properties);
	static FAssetDocumentPropertyApplyResult PreflightProperties(UClass* Class, TSharedPtr<FJsonObject> Properties);
	static FString GetTypeToken(FProperty* Property);
	static bool IsWritableProperty(FProperty* Property);
	static FString GetNonWritableReason(FProperty* Property);
	static TSharedPtr<FJsonValue> ExtractPropertyValue(FProperty* Property, const void* ValuePtr);
	static TSharedPtr<FJsonObject> ExtractWritablePropertiesToJson(
		UObject* Object,
		bool bSkipDefaults,
		const TSet<FName>* ExcludedPropertyNames = nullptr);
	static TSharedPtr<FJsonObject> InspectProperties(
		UClass* Class,
		UObject* CurrentObject = nullptr,
		const TSet<FName>* ExcludedPropertyNames = nullptr);

private:
	static bool ApplyPropertiesDirect(UObject* Asset, TSharedPtr<FJsonObject> Properties, TArray<FAssetDocumentDiagnostic>& OutDiagnostics);
	static bool ApplySingleProperty(UObject* Asset, const FString& PropertyName, TSharedPtr<FJsonValue> JsonValue, TArray<FAssetDocumentDiagnostic>& OutDiagnostics);
	static bool TryGetTypedValue(TSharedPtr<FJsonValue> JsonValue, FString& OutType, TSharedPtr<FJsonValue>& OutValue, bool& bOutIsTyped, FString& OutError);
	static bool ValidateTypedValueForProperty(FProperty* Property, const FString& TypeName, TSharedPtr<FJsonValue> Value, FString& OutError);
	static void AddDiagnostic(TArray<FAssetDocumentDiagnostic>& Diagnostics, const FString& PropertyName, const FString& Code, const FString& Message);
};
