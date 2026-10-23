// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeGraphBundleInternal.h"

class UClass;
class FStructOnScope;
namespace UE::AssetDocumentAI::BehaviorTreeScratchAuthoring
{
// These functions select the existing stock menu actions. They never construct
// graph objects or publish an authored/native property through raw memory.
UEdGraphNode* CreateMain(UBehaviorTreeGraph& Graph, const FString& ClassPath, int32 X, int32 Y);
UAIGraphNode* CreateSubnode(UBehaviorTreeGraph& Graph, UAIGraphNode& Parent,
 const FString& ClassPath, bool bDecorator);
UAIGraphNode* CreateComposite(UBehaviorTreeGraph& Graph, UAIGraphNode& Parent);
ASSETDOCUMENTAI_API bool WriteProperties(UObject& Object, const FAssetDocumentRuntimeValueDto& Properties, FString& Error);
// Only independently initialized exact ValueOr storage is admitted here.
bool WriteIsolatedValueOrProperties(const TSharedRef<FStructOnScope>& Storage,
 const FAssetDocumentRuntimeValueDto& Properties, FString& Error);
bool WriteExactHiddenField(UObject& Object, FName Field, const FString& Formatted, FString& Error);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeAuthorScratch(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);
