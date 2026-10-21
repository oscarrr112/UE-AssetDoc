// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UClass;
class UEdGraphNode;

class FAssetDocumentGraphEncodingUtils final
{
public:
	FAssetDocumentGraphEncodingUtils() = delete;

	static FString GetClassPath(const UClass* Class);
	static TSharedRef<FJsonObject> MakeNodePositionObject(const UEdGraphNode* Node);
	static FString GetNodeTitle(const UEdGraphNode* Node);
	static FString GetNodeGuid(const UEdGraphNode* Node);
};
