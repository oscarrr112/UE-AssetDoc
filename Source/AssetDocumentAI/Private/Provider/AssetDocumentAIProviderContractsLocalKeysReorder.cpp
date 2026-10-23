// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/AssetDocumentAIProviderContracts.h"

#include "UObject/SoftObjectPath.h"

bool ParseAssetDocumentAIBlackboardLocalKeysReorderRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardLocalKeysReorderRequest& OutRequest)
{
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| Value.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Value.ObjectKeys != TArray<FString>{TEXT("blackboard_path"), TEXT("ordered_local_key_names")}
		|| Value.Elements.Num() != 2)
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto& BlackboardPath = Value.Elements[0];
	const FAssetDocumentRuntimeValueDto& OrderedNames = Value.Elements[1];
	if (BlackboardPath.Kind != EAssetDocumentRuntimeValueKind::String
		|| OrderedNames.Kind != EAssetDocumentRuntimeValueKind::Array)
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
	OutRequest.OrderedLocalKeyNames.Reset();
	OutRequest.OrderedLocalKeyNames.Reserve(OrderedNames.Elements.Num());
	for (const FAssetDocumentRuntimeValueDto& Name : OrderedNames.Elements)
	{
		if (Name.Kind != EAssetDocumentRuntimeValueKind::String || Name.StringValue.IsEmpty()) return false;
		OutRequest.OrderedLocalKeyNames.Add(Name.StringValue);
	}
	return true;
}
