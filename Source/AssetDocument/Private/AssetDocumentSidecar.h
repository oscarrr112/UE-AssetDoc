// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class ASSETDOCUMENT_API FAssetDocumentSidecar
{
public:
	static FString ResolveObjectPathFromSidecar(const FString& FilePath);
	static FString ResolveSidecarPathFromObjectPath(const FString& ObjectPath);
	// The check before every read or write of a sidecar: the file the access reaches (links followed, the leaf
	// included) is a <AssetName>.assetdoc.json inside the project's Content directory. OutRealPath is that file;
	// the access uses it. On refusal OutError says why, and nothing has been opened.
	static bool ResolveSidecarFile(const FString& FilePath, FString& OutRealPath, FString& OutError);
	// Reads and writes of a sidecar, each after ResolveSidecarFile.
	static bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson, FString& OutError);
	static bool LoadText(const FString& FilePath, FString& OutText, FString& OutError);
	static bool LoadBytes(const FString& FilePath, TArray<uint8>& OutBytes, FString& OutError);
	static bool WriteJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Json, FString& OutError);
	static bool ValidateTargetMatchesSidecar(const FString& FilePath, const TSharedPtr<FJsonObject>& Json, FString& OutError);
};
