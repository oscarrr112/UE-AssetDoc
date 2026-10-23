// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentBehaviorTreeOperationClassifier.h"

namespace
{
const FAssetDocumentRuntimeValueDto* FindObjectMember(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return nullptr;
	}
	const int32 Index = Object.ObjectKeys.IndexOfByKey(FString(Name));
	return Object.Elements.IsValidIndex(Index) ? &Object.Elements[Index] : nullptr;
}

bool IsExactEmptyObject(const FAssetDocumentRuntimeValueDto* Value)
{
	return Value != nullptr
		&& Value->Kind == EAssetDocumentRuntimeValueKind::Object
		&& Value->ObjectKeys.IsEmpty()
		&& Value->Elements.IsEmpty();
}

bool IsRetainedOverlayBody(const FAssetDocumentRuntimeValueDto* Body)
{
	if (IsExactEmptyObject(Body)) return true;
	if (Body == nullptr || Body->Kind != EAssetDocumentRuntimeValueKind::Object
		|| Body->ObjectKeys.Num() != 1 || Body->Elements.Num() != 1
		|| Body->ObjectKeys[0] != TEXT("BlackboardAsset")) return false;
	const FAssetDocumentRuntimeValueDto& Blackboard = Body->Elements[0];
	if (Blackboard.Kind == EAssetDocumentRuntimeValueKind::String
		|| Blackboard.Kind == EAssetDocumentRuntimeValueKind::Null) return true;
	// The existing public header also preserves AssetRef in canonical JSON.
	// Classify that exact shape without changing its bytes or reference validation.
	if (Blackboard.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Blackboard.ObjectKeys.Num() != 2 || Blackboard.Elements.Num() != 2) return false;
	const auto* Kind = FindObjectMember(Blackboard, TEXT("Kind"));
	const auto* Path = FindObjectMember(Blackboard, TEXT("Path"));
	return Kind && Kind->Kind == EAssetDocumentRuntimeValueKind::String
		&& Kind->StringValue == TEXT("AssetRef")
		&& Path && Path->Kind == EAssetDocumentRuntimeValueKind::String
		&& !Path->StringValue.IsEmpty();
}
}

FAssetDocumentBehaviorTreeOperationClassification
ClassifyAssetDocumentBehaviorTreeOperation(
	FStringView RequestedAssetClassPath,
	const FAssetDocumentRuntimeValueDto& CanonicalDocument)
{
	FAssetDocumentBehaviorTreeOperationClassification Result;
	const FAssetDocumentRuntimeValueDto* Properties =
		FindObjectMember(CanonicalDocument, TEXT("Properties"));
	Result.PropertyMode = Properties == nullptr || IsExactEmptyObject(Properties)
		? EAssetDocumentBehaviorTreeTopLevelPropertyMode::NoTopLevelPropertyWrite
		: EAssetDocumentBehaviorTreeTopLevelPropertyMode::GenericTopLevelPropertyWrite;
	if (RequestedAssetClassPath != TEXT("/Script/AIModule.BehaviorTree"))
	{
		return Result;
	}
	const FAssetDocumentRuntimeValueDto* Body =
		FindObjectMember(CanonicalDocument, TEXT("Body"));
	if (Body == nullptr || Body->Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		Result.Shape = EAssetDocumentBehaviorTreeOperationShape::InvalidBehaviorTreeShape;
		return Result;
	}
	if (FindObjectMember(*Body, TEXT("Tree")) != nullptr)
	{
		Result.Shape = EAssetDocumentBehaviorTreeOperationShape::FullTreeLifecycle;
		return Result;
	}
	Result.Shape = IsRetainedOverlayBody(Body)
		? EAssetDocumentBehaviorTreeOperationShape::RetainedTreeOverlay
		: EAssetDocumentBehaviorTreeOperationShape::InvalidBehaviorTreeShape;
	return Result;
}
