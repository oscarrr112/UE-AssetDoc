// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class FAutomationTestBase;
class IBehaviorTreeEditor;
class IPropertyHandle;
class IPropertyRowGenerator;
class FAssetDocumentAITestFixture final
{
public:
	bool CreateFactoryOwnedScratchBundle(FAutomationTestBase& Test);
	bool RequireUniqueRootPropertyHandle(FAutomationTestBase& Test);
	bool BootstrapBehaviorTreeEditorCache(FAutomationTestBase& Test);
	bool CloseBehaviorTreeEditorToolkit(FAutomationTestBase& Test);

	UBehaviorTree* GetScratchTree() const { return ScratchTree.Get(); }
	UBehaviorTreeGraph* GetScratchGraph() const { return ScratchGraph.Get(); }
	UBehaviorTreeGraphNode_Root* GetRootNode() const { return RootNode.Get(); }
	TSharedPtr<IPropertyHandle> GetBlackboardAssetHandle() const { return BlackboardAssetHandle; }

private:
	TStrongObjectPtr<UBehaviorTree> BootstrapTree;
	TStrongObjectPtr<UBehaviorTree> ScratchTree;
	TStrongObjectPtr<UBehaviorTreeGraph> ScratchGraph;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Root> RootNode;
	TSharedPtr<IBehaviorTreeEditor> Toolkit;
	TSharedPtr<IPropertyRowGenerator> PropertyRows;
	TSharedPtr<IPropertyHandle> BlackboardAssetHandle;
	TArray<UObject*> EditedAssetsBefore;
	int32 OpenEditorCountBefore = INDEX_NONE;
};

#endif // WITH_DEV_AUTOMATION_TESTS
