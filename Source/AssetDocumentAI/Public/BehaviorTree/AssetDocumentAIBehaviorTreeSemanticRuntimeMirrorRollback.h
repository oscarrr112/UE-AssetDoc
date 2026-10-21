// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

class IAssetDocumentVerifiedHandleLease;
class UBehaviorTreeGraph;

struct ASSETDOCUMENTAI_API FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult
{
	bool bSucceeded = false;
	FString RootNodeClassPath;
	int32 NodeCount = 0;
	FString ExecutionOrderHash;
	FString BlackboardEvidenceHash;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

ASSETDOCUMENTAI_API FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult
RebuildAssetDocumentBehaviorTreeRuntimeMirror(
	const IAssetDocumentVerifiedHandleLease& MutableAssetLease,
	UBehaviorTreeGraph& ExpectedLiveGraph,
	int32 UpdateFlags,
	const FAssetDocumentMutationFootprint& RuntimeMirrorFootprint,
	IAssetDocumentAtomicTransaction& ActiveTransaction);
