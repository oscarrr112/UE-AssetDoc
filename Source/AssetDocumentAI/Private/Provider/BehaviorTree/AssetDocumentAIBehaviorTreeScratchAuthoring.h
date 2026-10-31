// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeGraphBundleInternal.h"

class UClass;
class UBehaviorTree;
class FStructOnScope;
namespace UE::AssetDocumentAI::BehaviorTreeScratchAuthoring
{
// These functions select the existing stock menu actions. They never construct
// graph objects or publish an authored/native property through raw memory.
UEdGraphNode* CreateMain(UBehaviorTreeGraph& Graph, const FString& ClassPath, int32 X, int32 Y);
UAIGraphNode* CreateSubnode(UBehaviorTreeGraph& Graph, UAIGraphNode& Parent,
 const FString& ClassPath, bool bDecorator);
UAIGraphNode* CreateComposite(UBehaviorTreeGraph& Graph, UAIGraphNode& Parent);
// A marker value (an object whose Unsupported is a string) is not written: the property takes the value Kept (the
// replaced tree's instance of the same node) holds when Kept is of Object's class, copied as stored since it has no
// form the property editor reads back; else the property keeps its current value.
ASSETDOCUMENTAI_API bool WriteProperties(UObject& Object, const FAssetDocumentRuntimeValueDto& Properties, FString& Error,
 const UObject* Kept = nullptr);
// Only independently initialized exact ValueOr storage is admitted here.
bool WriteIsolatedValueOrProperties(const TSharedRef<FStructOnScope>& Storage,
 const FAssetDocumentRuntimeValueDto& Properties, FString& Error);
bool WriteExactHiddenField(UObject& Object, FName Field, const FString& Formatted, FString& Error);

// The texts of the tree a scratch build replaces, while one is alive: a node property written with the source string
// (or the displayed string) of the same property's text in that tree gets that text, with its key or String Table
// reference, instead of a new text. A rebuilt tree so keeps the localization identity of every unchanged text.
// It also names that tree's node instances by node GUID, the values a marked property keeps, and its nodes whose class
// is missing, the nodes a marked node is recreated from.
class FScopedReplacedTexts
{
public:
 explicit FScopedReplacedTexts(const UBehaviorTree* Replaced);
 ~FScopedReplacedTexts();
 FScopedReplacedTexts(const FScopedReplacedTexts&) = delete;
 FScopedReplacedTexts& operator=(const FScopedReplacedTexts&) = delete;
 // The kept text for Property written as Value, or nullptr.
 static const FText* Find(const FProperty& Property, const FString& Value);
 // The replaced tree's instance of the node NodeGuid (main node, subnode or bound graph test), or nullptr.
 static const UObject* FindInstance(const FGuid& NodeGuid);
 // The replaced tree's main node or subnode NodeGuid when its class is missing, or nullptr.
 static const UAIGraphNode* FindMissingClassNode(const FGuid& NodeGuid);
private:
 TMap<FString, FText> Texts;
 TMap<FGuid, const UObject*> Instances;
 TMap<FGuid, const UAIGraphNode*> MissingClassNodes;
 FScopedReplacedTexts* Previous = nullptr;
};
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeAuthorScratch(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);
