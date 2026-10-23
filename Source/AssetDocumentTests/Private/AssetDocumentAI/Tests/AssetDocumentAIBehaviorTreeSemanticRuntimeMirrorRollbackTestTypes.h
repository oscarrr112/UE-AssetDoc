// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Tasks/BTTask_BlackboardBase.h"
#include "BehaviorTreeGraph.h"

#include "AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestTypes.generated.h"

class UBehaviorTree;

/** Exact derived graph used to prove that Node B rejects non-stock dynamic classes. */
UCLASS()
class UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestGraph final
	: public UBehaviorTreeGraph
{
	GENERATED_BODY()
};

/**
 * A deliberately small Blackboard node used only to make the supported
 * InitializeFromAsset lifecycle observable.  It owns no graph state and does
 * not attempt to model a production node's caches.
 */
UCLASS()
class UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestTask final
	: public UBTTask_BlackboardBase
{
	GENERATED_BODY()

public:
	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;

	void ConfigureSelectedKey(FName InKeyName, TSubclassOf<UBlackboardKeyType> InKeyType);
	void SetExplicitAuthoredValue(int32 InValue) { ExplicitAuthoredValue = InValue; }
	int32 GetExplicitAuthoredValue() const { return ExplicitAuthoredValue; }
	int32 GetInitializeInvocationCount() const { return InitializeInvocationCount; }
	FString GetObservedBlackboardAssetSignature() const { return ObservedBlackboardAssetSignature; }
	FBlackboard::FKey GetObservedSelectedKeyId() const { return BlackboardKey.GetSelectedKeyID(); }
	UClass* GetObservedSelectedKeyType() const { return BlackboardKey.SelectedKeyType.Get(); }
	FName GetObservedSelectedKeyName() const { return BlackboardKey.SelectedKeyName; }

private:
	/** A representative declared authored property; rollback actions own its preimage. */
	UPROPERTY(EditAnywhere, Category = "AssetDocument Test")
	int32 ExplicitAuthoredValue = 17;

	UPROPERTY(Transient)
	int32 InitializeInvocationCount = 0;

	UPROPERTY(Transient)
	FString ObservedBlackboardAssetSignature;
};

/** Test-only service that makes post-UpdateAsset public runtime-link corruption observable. */
UCLASS()
class UAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackTestService final
	: public UBTService
{
	GENERATED_BODY()

public:
	virtual void InitializeFromAsset(UBehaviorTree& Asset) override;

	void ConfigureRuntimeLinkCorruption(bool bInCorruptParent, bool bInCorruptChildIndex)
	{
		bCorruptParent = bInCorruptParent;
		bCorruptChildIndex = bInCorruptChildIndex;
	}
	int32 GetInitializeInvocationCount() const { return InitializeInvocationCount; }

private:
	UPROPERTY(Transient)
	bool bCorruptParent = false;

	UPROPERTY(Transient)
	bool bCorruptChildIndex = false;

	UPROPERTY(Transient)
	int32 InitializeInvocationCount = 0;
};
