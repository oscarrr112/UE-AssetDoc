// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentPolicy.h"
#include "AssetDocumentTypes.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct ASSETDOCUMENT_API FAssetDocumentCapabilityContext
{
	UObject* Asset = nullptr;
	UClass* AssetClass = nullptr;
	FString TargetAssetPath;
	FString SourceDocumentPath;
	const TSharedPtr<FJsonObject>* Definitions = nullptr;
	bool bIsDryRun = false;
	FAssetDocumentResult* Result = nullptr;
};

struct ASSETDOCUMENT_API FAssetDocumentCapabilityResult
{
	bool bSuccess = false;
	FString Message;
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	TSharedPtr<FJsonObject> Payload;

	static FAssetDocumentCapabilityResult Success(const FString& Message = TEXT(""))
	{
		FAssetDocumentCapabilityResult Result;
		Result.bSuccess = true;
		Result.Message = Message;
		return Result;
	}

	static FAssetDocumentCapabilityResult Failure(const FString& Message, const FString& Path = TEXT(""), const FString& Code = TEXT("ValidationFailed"))
	{
		FAssetDocumentCapabilityResult Result;
		Result.bSuccess = false;
		Result.Message = Message;
		if (!Path.IsEmpty() || !Code.IsEmpty())
		{
			FAssetDocumentDiagnostic Diagnostic;
			Diagnostic.Path = Path;
			Diagnostic.Code = Code;
			Diagnostic.Message = Message;
			Result.Diagnostics.Add(MoveTemp(Diagnostic));
		}
		return Result;
	}
};

class ASSETDOCUMENT_API IAssetDocumentCapability
{
public:
	virtual ~IAssetDocumentCapability() = default;

	virtual FName GetName() const = 0;
	virtual TArray<FName> GetInternalAdapterNames() const
	{
		return {GetName()};
	}
	virtual int32 GetApplyOrder() const = 0;
	virtual bool SupportsAsset(const UObject* Asset) const = 0;
	virtual bool SupportsClass(const UClass* AssetClass) const = 0;
	virtual TSharedRef<FJsonObject> GetSchemaHint() const = 0;
	virtual FAssetDocumentCapabilityResult Validate(const FAssetDocumentCapabilityContext& Context, const TSharedRef<FJsonValue>& BodyJson) const = 0;
	virtual FAssetDocumentCapabilityResult Preflight(FAssetDocumentCapabilityContext& Context, const TSharedRef<FJsonValue>& BodyJson) const
	{
		return Validate(Context, BodyJson);
	}
	virtual FAssetDocumentCapabilityResult Apply(FAssetDocumentCapabilityContext& Context, const TSharedRef<FJsonValue>& BodyJson) = 0;
	virtual FAssetDocumentCapabilityResult Extract(const FAssetDocumentCapabilityContext& Context, TSharedRef<FJsonObject>& OutBodyJson) const = 0;
	virtual FAssetDocumentCapabilityResult Diff(const FAssetDocumentCapabilityContext& Context, const TSharedRef<FJsonValue>& DesiredJson, TArray<TSharedPtr<FJsonValue>>& OutDiffEntries) const = 0;
};
