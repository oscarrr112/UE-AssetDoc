// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class FAssetDocumentSidecarPathUtils
{
public:
	static FString NormalizeFilePath(const FString& FilePath);
	static FString NormalizeWatchedFilePath(const FString& FilePath, const FString& WatchDirectory);
	static FString NormalizeContentDirectory();
	static FString NormalizeObjectPath(const FString& ObjectPath);

private:
	FAssetDocumentSidecarPathUtils() = delete;
};
