// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class ASSETDOCUMENT_API FAssetDocumentSidecarReader final
{
public:
	static bool LoadValidatedDocument(
		const FString& FilePath,
		TSharedPtr<FJsonObject>& OutDocument,
		FString& OutError);
};
