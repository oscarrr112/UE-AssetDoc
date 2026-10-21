// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Graphs/AssetDocumentGraphEncodingUtils.h"

#include "EdGraph/EdGraphNode.h"
#include "UObject/Class.h"

FString FAssetDocumentGraphEncodingUtils::GetClassPath(const UClass* Class)
{
	return Class ? Class->GetPathName() : FString();
}

TSharedRef<FJsonObject> FAssetDocumentGraphEncodingUtils::MakeNodePositionObject(const UEdGraphNode* Node)
{
	TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("X"), Node ? Node->NodePosX : 0);
	Position->SetNumberField(TEXT("Y"), Node ? Node->NodePosY : 0);
	return Position;
}

FString FAssetDocumentGraphEncodingUtils::GetNodeTitle(const UEdGraphNode* Node)
{
	return Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString();
}

FString FAssetDocumentGraphEncodingUtils::GetNodeGuid(const UEdGraphNode* Node)
{
	return Node ? Node->NodeGuid.ToString(EGuidFormats::Digits) : FString();
}
