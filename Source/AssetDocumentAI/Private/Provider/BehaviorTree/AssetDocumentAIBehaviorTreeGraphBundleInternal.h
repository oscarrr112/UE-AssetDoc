// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"
#include "UObject/StrongObjectPtr.h"

#include <initializer_list>

class UBehaviorTree;
class UBehaviorTreeGraph;
class UAIGraphNode;
class UEdGraphNode;
class UEdGraphNode_Comment;

struct FAssetDocumentAIBehaviorTreeScratchBundleState
{
	TStrongObjectPtr<UBehaviorTree> Tree;
	TStrongObjectPtr<UBehaviorTreeGraph> Graph;
	bool bConsumed = false;
};

struct FAssetDocumentAIBehaviorTreeLiveGraphState
{
	TWeakObjectPtr<UBehaviorTree> Tree;
	TWeakObjectPtr<UBehaviorTreeGraph> Graph;
	FGuid GraphGuid;
	bool bAlive = true;
};

bool IssueAssetDocumentAIBehaviorTreeLiveGraphHandle(
	IAssetDocumentHandleResolver& Handles,
	TSharedRef<FAssetDocumentAIBehaviorTreeLiveGraphState> State,
	FAssetDocumentPrimitiveOutcome& Outcome);

struct FAssetDocumentAIBehaviorTreeGraphLease
{
	TUniquePtr<IAssetDocumentVerifiedHandleLease> Lease;
	TSharedPtr<FAssetDocumentAIBehaviorTreeScratchBundleState> State;
	UBehaviorTreeGraph* Graph = nullptr;
	FString LiveBehaviorTreePath;
};

struct FAssetDocumentAIBehaviorTreeNodeLease
{
	TUniquePtr<IAssetDocumentVerifiedHandleLease> Lease;
	UEdGraphNode* Node = nullptr;
};

FAssetDocumentPrimitiveOutcome MakeAssetDocumentAIBehaviorTreeGraphFailure(
	const TCHAR* Code, const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics = {});
FAssetDocumentRuntimeValueDto MakeAssetDocumentAIBehaviorTreeStringValue(FString Value);
FAssetDocumentRuntimeValueDto MakeAssetDocumentAIBehaviorTreeIntegerValue(int64 Value);
FAssetDocumentRuntimeValueDto MakeAssetDocumentAIBehaviorTreeObjectValue(
	TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values);
ASSETDOCUMENTAI_API const FAssetDocumentRuntimeValueDto* FindAssetDocumentAIBehaviorTreeField(
	const FAssetDocumentRuntimeValueDto& Object, const TCHAR* Name);
bool HasAssetDocumentAIBehaviorTreeExactFields(
	const FAssetDocumentRuntimeValueDto& Object,
	std::initializer_list<const TCHAR*> Names);
bool IsAssetDocumentAIBehaviorTreeCanonicalObjectPath(
	const FAssetDocumentRuntimeValueDto* Value);
bool ReadAssetDocumentAIBehaviorTreeFiniteCoordinate(
	const FAssetDocumentRuntimeValueDto* Value, double& OutValue);
bool IsAssetDocumentAIBehaviorTreeScratchContext();
UBehaviorTree* ResolveAssetDocumentAIBehaviorTreeLiveTree(const FString& Path);
bool EnlistAssetDocumentAIBehaviorTreeScratchBundle(
	IAssetDocumentAtomicTransaction& Transaction,
	TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> State,
	const FGuid& RequestId,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

ASSETDOCUMENTAI_API bool AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, FStringView Name,
	FAssetDocumentAIBehaviorTreeGraphLease& OutLease,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
bool AcquireAssetDocumentAIBehaviorTreeNodeLeaseInternal(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, FStringView Name,
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	FAssetDocumentAIBehaviorTreeNodeLease& OutLease,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
bool IssueAssetDocumentAIBehaviorTreeGraphHandle(
	IAssetDocumentHandleResolver& Handles,
	TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> State,
	const FString& LivePath, FAssetDocumentPrimitiveOutcome& Outcome);
bool IssueAssetDocumentAIBehaviorTreeNodeHandle(
	IAssetDocumentHandleResolver& Handles, FStringView ResultName,
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease, UEdGraphNode& Node,
	FAssetDocumentPrimitiveOutcome& Outcome);
FAssetDocumentChangedRecord MakeAssetDocumentAIBehaviorTreeScratchChange(
	const FString& LivePath, const UBehaviorTreeGraph& Graph,
	const UEdGraphNode* Node, FString Kind);
FAssetDocumentChangedRecord MakeAssetDocumentAIBehaviorTreeScratchConnectionChange(
	const FString& LivePath, const UBehaviorTreeGraph& Graph,
	const UEdGraphNode& FromNode, const UEdGraphNode& ToNode);
FAssetDocumentPrimitiveOutcome MakeAssetDocumentAIBehaviorTreeNodeActionSuccess(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	const FString& ClassPath, UEdGraphNode& Node);
FAssetDocumentPrimitiveOutcome MakeAssetDocumentAIBehaviorTreeSubnodeActionSuccess(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	const FString& ClassPath, const UAIGraphNode& Parent,
	UAIGraphNode& Child, bool bDecorator);
FAssetDocumentPrimitiveOutcome MakeAssetDocumentAIBehaviorTreeCommentActionSuccess(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	UEdGraphNode_Comment& Comment);
FAssetDocumentPrimitiveOutcome MakeAssetDocumentAIBehaviorTreeConnectionSuccess(
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	const FAssetDocumentAIBehaviorTreeNodeLease& FromLease,
	const FAssetDocumentAIBehaviorTreeNodeLease& ToLease);
