// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentTypes.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// The level tools' response: the AssetDocument result shape
// {success, message, diagnostics[{path, code, message, severity, details}], payload}
// with HTTP 200 / 207 / 400, so the MCP formats failures as it does for documents.
namespace UE::AssetDocumentLevel::Private
{
struct FLevelToolResult
{
	int32 HttpStatus = 200;
	bool bSuccess = true;
	FString Message;
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	TSharedPtr<FJsonObject> Payload;

	static FLevelToolResult Success(FString Message, TSharedPtr<FJsonObject> Payload);
	// Root cause first: Message is "<Code>: <Message> (at <Path>)" of the first diagnostic.
	static FLevelToolResult Failure(TArray<FAssetDocumentDiagnostic> Diagnostics, TSharedPtr<FJsonObject> Payload = nullptr);

	TSharedRef<FJsonObject> ToJson() const;
	const FAssetDocumentDiagnostic* FindDiagnostic(const FString& Code) const;
};

FAssetDocumentDiagnostic MakeLevelDiagnostic(const FString& Path, const FString& Code, const FString& Message,
	TSharedPtr<FJsonObject> Details = nullptr);
}
