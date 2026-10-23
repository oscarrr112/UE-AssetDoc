// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BehaviorTree.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardValueOrKeyCacheInvalidation.h"
#include "Templates/ValueOrError.h"
#include "UObject/StrongObjectPtr.h"

class FProperty;
struct FBlackboardKeySelector;
class IAssetRegistry;
struct FAssetData;
class UBlackboardData;
class UBehaviorTree;
class UScriptStruct;

namespace AssetDocumentAIBlackboardLoadedConsumerCompletion
{
inline constexpr FStringView AuthorityId =
	TEXTVIEW("AD.AI.BLACKBOARD.LOADED-BT-CONSUMER-COMPLETION-v1");
enum class ELoadedConsumerReceiptCaptureFailure : uint8
{
	None,
	AuthorityUnavailable,
};

enum class ELoadedConsumerCompletionPhase : uint8
{
	ForwardPublished,
	RollbackRestored,
};

enum class ELoadedConsumerCompletionResult : uint8
{
	Succeeded,
	DerivedRefreshFailed,
};

namespace Private
{
struct FCodecBudget
{
	int32 Fields = 0;
	int32 Elements = 0;
	int32 Bytes = 0;
};

ASSETDOCUMENTAI_API int32 LocatorMaximumDepth();
ASSETDOCUMENTAI_API int32 LocatorMaximumFields();
ASSETDOCUMENTAI_API int32 LocatorMaximumElements();
ASSETDOCUMENTAI_API int32 LocatorMaximumBytes();

enum class ETypedPathSegmentKind : uint8
{
	Field,
	InstancedStruct,
	ArrayElementDigest,
	MapValueKeyDigest,
};

struct FTypedPathSegment
{
	ETypedPathSegmentKind Kind = ETypedPathSegmentKind::Field;
	FString DeclaringOwnerPath;
	FName PropertyName;
	FString PropertyClassPath;
	int32 StaticArrayOrdinal = 0;
	FString ContainedStructPath;
	FString Digest;
};

enum class ELeafKind : uint8
{
	Selector,
	ExactValueOr,
};

struct FLeafLocator
{
	FGuid GraphGuid;
	FGuid NodeGuid;
	TArray<FTypedPathSegment> Segments;
	ELeafKind Kind = ELeafKind::Selector;
	AssetDocumentAIBlackboardValueOrKeyCacheInvalidation::EExactValueOrBlackboardKeyFamily Family =
		AssetDocumentAIBlackboardValueOrKeyCacheInvalidation::EExactValueOrBlackboardKeyFamily::Bool;
};

struct FCurrentLeaf
{
	FLeafLocator Locator;
	FString AuthoredFingerprint;
	FName AuthoredKeyName;
	FBlackboardKeySelector* Selector = nullptr;
	AssetDocumentAIBlackboardValueOrKeyCacheInvalidation::EExactValueOrBlackboardKeyFamily Family =
		AssetDocumentAIBlackboardValueOrKeyCacheInvalidation::EExactValueOrBlackboardKeyFamily::Bool;
	void* ValueOr = nullptr;
	UScriptStruct* DeclaredStruct = nullptr;
};

struct FStoredLeaf
{
	FLeafLocator Locator;
	FString AuthoredFingerprint;
	FName AuthoredKeyName;
};

struct FConsumer
{
	FString AssetPath;
	TStrongObjectPtr<UBehaviorTree> Tree;
	bool bPackageDirty = false;
	TArray<FGuid> GraphGuids;
	TArray<FGuid> NodeGuids;
	TArray<FStoredLeaf> Leaves;
};

FString PropertyClassIdentity(const FProperty& Property);
bool IsExcludedProperty(const FProperty& Property);
bool ExcludedValueContainsAuthoredLeaf(
	const FProperty& Property, const void* Value, int32 Depth, FCodecBudget& Budget);
bool SameLeafLocator(const FLeafLocator& Left, const FLeafLocator& Right);
FString LeafLocatorSortKey(const FLeafLocator& Locator);
bool HashUtf8(FStringView Text, FString& OutHash);
UScriptStruct* ExactValueOrStruct(
	const UScriptStruct& Struct,
	AssetDocumentAIBlackboardValueOrKeyCacheInvalidation::EExactValueOrBlackboardKeyFamily& OutFamily);
FName ValueOrKey(void* Value,
	AssetDocumentAIBlackboardValueOrKeyCacheInvalidation::EExactValueOrBlackboardKeyFamily Family);
ASSETDOCUMENTAI_API bool EncodeBlackboardBTLeafLocatorDigestV1(
	const FProperty& Property, const void* Value, FString& OutDigest,
	FCodecBudget* ConsumerPreimageBudget = nullptr);
ASSETDOCUMENTAI_API bool EncodeLeafAuthoredFingerprint(
	const FProperty& Property, const void* Value, FString& OutFingerprint,
	FCodecBudget* ConsumerPreimageBudget = nullptr);

bool InventoryTree(UBehaviorTree& Tree, TArray<FCurrentLeaf>& OutLeaves,
	TArray<FGuid>& OutGraphGuids, TArray<FGuid>& OutNodeGuids);
bool EffectiveBlackboardContains(const UBehaviorTree& Tree, const UBlackboardData& Target);
ASSETDOCUMENTAI_API bool QueryExactBehaviorTrees(IAssetRegistry& Registry, TArray<FAssetData>& OutAssets);
ASSETDOCUMENTAI_API bool RegistryIdentityDigest(TConstArrayView<FAssetData> Assets, FString& OutDigest);
ASSETDOCUMENTAI_API bool BuildCandidateUnion(IAssetRegistry& Registry, UBlackboardData& Target,
	TConstArrayView<FAssetData> BehaviorTreeAudit, TArray<FAssetData>& OutCandidates);
bool ResolveLoadedExactAsset(const FAssetData& Asset, const UClass& ExactClass,
	UObject*& OutObject);
ASSETDOCUMENTAI_API bool CaptureConsumers(UBlackboardData& Target, TConstArrayView<FAssetData> Assets,
	TArray<FConsumer>& OutConsumers, TArray<FString>& OutSkippedUnloaded);
} // namespace Private

/** Move-only, private receipt. Its implementation owns the strong BT lifetime. */
class ASSETDOCUMENTAI_API FLoadedBehaviorTreeConsumerReceipt final
{
public:
	FLoadedBehaviorTreeConsumerReceipt(FLoadedBehaviorTreeConsumerReceipt&&);
	FLoadedBehaviorTreeConsumerReceipt& operator=(FLoadedBehaviorTreeConsumerReceipt&&);
	~FLoadedBehaviorTreeConsumerReceipt();

	FLoadedBehaviorTreeConsumerReceipt(const FLoadedBehaviorTreeConsumerReceipt&) = delete;
	FLoadedBehaviorTreeConsumerReceipt& operator=(const FLoadedBehaviorTreeConsumerReceipt&) = delete;

private:
	struct FImpl;
	explicit FLoadedBehaviorTreeConsumerReceipt(TUniquePtr<FImpl> InImpl);
	TUniquePtr<FImpl> Impl;

	friend ASSETDOCUMENTAI_API TValueOrError<FLoadedBehaviorTreeConsumerReceipt,
		ELoadedConsumerReceiptCaptureFailure>
	CaptureLoadedBehaviorTreeConsumerReceipt(UBlackboardData& ExactTarget);
	friend ASSETDOCUMENTAI_API ELoadedConsumerCompletionResult CompleteLoadedBehaviorTreeConsumerReceipt(
		FLoadedBehaviorTreeConsumerReceipt& Receipt,
		UBlackboardData& ExactTarget,
		ELoadedConsumerCompletionPhase Phase);
};

ASSETDOCUMENTAI_API TValueOrError<FLoadedBehaviorTreeConsumerReceipt, ELoadedConsumerReceiptCaptureFailure>
CaptureLoadedBehaviorTreeConsumerReceipt(UBlackboardData& ExactTarget);

ASSETDOCUMENTAI_API ELoadedConsumerCompletionResult CompleteLoadedBehaviorTreeConsumerReceipt(
	FLoadedBehaviorTreeConsumerReceipt& Receipt,
	UBlackboardData& ExactTarget,
	ELoadedConsumerCompletionPhase Phase);

enum class ELoadedConsumerCompletionTestFailurePoint : uint8
{
	None,
	RegistryBeforeReadiness,
	RegistrySnapshotA,
	RegistryReferencers,
	RegistrySnapshotB,
	RegistryAfterReadiness,
	BeforeLoadedConsumerForwardCompletion,
	AfterLoadedConsumerForwardCompletion,
	BeforeGraph,
	AfterGraph,
	BeforeSelector,
	AfterSelector,
	BeforeBool, AfterBool,
	BeforeClass, AfterClass,
	BeforeEnum, AfterEnum,
	BeforeFloat, AfterFloat,
	BeforeInt32, AfterInt32,
	BeforeName, AfterName,
	BeforeString, AfterString,
	BeforeObject, AfterObject,
	BeforeRotator, AfterRotator,
	BeforeStruct, AfterStruct,
	BeforeVector, AfterVector,
	BeforeVerification,
	AfterVerification,
	RollbackVerification,
};

#if WITH_DEV_AUTOMATION_TESTS
ASSETDOCUMENTAI_API void SetLoadedConsumerCompletionTestFailurePoint(
	ELoadedConsumerCompletionTestFailurePoint FailurePoint);
bool ConsumeLoadedConsumerCompletionTestFailurePoint(
	ELoadedConsumerCompletionTestFailurePoint FailurePoint);
#endif
} // namespace AssetDocumentAIBlackboardLoadedConsumerCompletion
