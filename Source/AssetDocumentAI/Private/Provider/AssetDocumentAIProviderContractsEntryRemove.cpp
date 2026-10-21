// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/AssetDocumentAIProviderContracts.h"

#include "UObject/SoftObjectPath.h"

bool ParseAssetDocumentAIBlackboardEntryRemoveRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardEntryRemoveRequest& OutRequest)
{
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| Value.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Value.ObjectKeys != TArray<FString>{TEXT("blackboard_path"), TEXT("key_name")}
		|| Value.Elements.Num() != 2)
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto& BlackboardPath = Value.Elements[0];
	const FAssetDocumentRuntimeValueDto& KeyName = Value.Elements[1];
	if (BlackboardPath.Kind != EAssetDocumentRuntimeValueKind::String
		|| KeyName.Kind != EAssetDocumentRuntimeValueKind::String
		|| KeyName.StringValue.IsEmpty())
	{
		return false;
	}
	const FSoftObjectPath CanonicalPath(BlackboardPath.StringValue);
	if (!CanonicalPath.IsValid() || !CanonicalPath.GetSubPathString().IsEmpty()
		|| CanonicalPath.ToString() != BlackboardPath.StringValue)
	{
		return false;
	}
	OutRequest.BlackboardPath = BlackboardPath.StringValue;
	OutRequest.KeyName = KeyName.StringValue;
	return true;
}
