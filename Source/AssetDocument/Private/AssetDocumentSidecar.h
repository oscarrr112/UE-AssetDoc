// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class ASSETDOCUMENT_API FAssetDocumentSidecar
{
public:
	static FString ResolveObjectPathFromSidecar(const FString& FilePath);
	static FString ResolveSidecarPathFromObjectPath(const FString& ObjectPath);
	static bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
	static bool WriteJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Json, FString& OutError);
	static bool ValidateTargetMatchesSidecar(const FString& FilePath, const TSharedPtr<FJsonObject>& Json, FString& OutError);
};
