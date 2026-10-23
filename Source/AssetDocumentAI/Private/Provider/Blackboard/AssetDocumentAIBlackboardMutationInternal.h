// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentEditorNativeState.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Provider/Blackboard/AssetDocumentAIBlackboardPrimitives.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "TypedPreimage/AssetDocumentAIBlackboardTypedPreimage.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BlackboardData.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "PropertyHandle.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentAIBlackboardMutation
{
struct FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest
{
	FString BlackboardPath;
	FString KeyName;
	FString ExpectedClassPath;
	FAssetDocumentRuntimeValueDto CanonicalProperties;
	FString Description;
	FString Category;
	bool bInstanceSynced = false;
};

FAssetDocumentPrimitiveOutcome Failure(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics = {});
FAssetDocumentPrimitiveOutcome FailureFromPreimage(
	const FAssetDocumentEditorTypedPreimageResult& Result);
FAssetDocumentPrimitiveOutcome FailureFromValidation(
	const FAssetDocumentContractValidationResult& Result);
FAssetDocumentPrimitiveOutcome NewKeyAuthorityFailure(const TCHAR* Message);

FAssetDocumentRuntimeValueDto StringValue(FString Value);
FAssetDocumentRuntimeValueDto ArrayValue(TArray<FAssetDocumentRuntimeValueDto> Values);
FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Values);
bool RuntimeValueEquals(
	const FAssetDocumentRuntimeValueDto& Left,
	const FAssetDocumentRuntimeValueDto& Right);

UBlackboardData* FindBlackboard(const FString& Path);
UBlackboardKeyType* FindOwnedKeyType(UBlackboardData& Blackboard, const FString& Name);
bool IsEditorPropertyContext(const UObject* Object = nullptr);
ASSETDOCUMENTAI_API bool TryMakeRows(
	UObject& Object,
	TSharedPtr<IPropertyRowGenerator>& OutRows,
	const TSet<FString>* PropertyAllowList = nullptr);
ASSETDOCUMENTAI_API TSharedPtr<IPropertyHandle> FindUniqueOwnedPropertyHandle(
	IPropertyRowGenerator& Rows,
	UObject& Owner,
	FName PropertyName);
bool BuildPropertyWrites(
	UBlackboardKeyType& KeyType,
	const FAssetDocumentRuntimeValueDto& Properties,
	TSharedPtr<IPropertyRowGenerator>& OutRows,
	TArray<TSharedRef<IPropertyHandle>>& OutHandles,
	TArray<const FAssetDocumentRuntimeValueDto*>& OutValues);
bool BuildCompletePropertyWrites(
	UBlackboardKeyType& KeyType,
	const FAssetDocumentRuntimeValueDto& Properties,
	TSharedPtr<IPropertyRowGenerator>& OutRows,
	TArray<TSharedRef<IPropertyHandle>>& OutHandles,
	TArray<const FAssetDocumentRuntimeValueDto*>& OutValues,
	UEnum*& OutCanonicalEnumType);
bool BuildCanonicalProperties(
	TConstArrayView<TSharedRef<IPropertyHandle>> Handles,
	FAssetDocumentRuntimeValueDto& OutProperties);
bool BuildAssetDocumentAIBlackboardEntryMetadataReadback(
	IPropertyRowGenerator& BlackboardRows,
	UBlackboardData& Blackboard,
	const FString& KeyName,
	UBlackboardKeyType& KeyType,
	const FAssetDocumentRuntimeValueDto& CanonicalProperties,
	FAssetDocumentRuntimeValueDto& OutEntry,
	TArray<FString>& OutLocalKeyNames);
ASSETDOCUMENTAI_API bool BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(
	UBlackboardKeyType& KeyType,
	FAssetDocumentRuntimeValueDto& OutProperties);
// Vector/Rotator keys: direct exact codec (their default sits behind an inline edit
// condition that has no PropertyEditor row). Property changes replace the key type.
bool IsAssetDocumentAIBlackboardDirectValueKeyType(const UClass* KeyTypeClass);
bool WriteAssetDocumentAIBlackboardDirectValueKeyType(
	UBlackboardKeyType& KeyType,
	const FAssetDocumentRuntimeValueDto& Properties);
bool BuildAssetDocumentAIBlackboardCanonicalKeyTypePropertiesFromRows(
	UBlackboardKeyType& KeyType,
	IPropertyRowGenerator& Rows,
	FAssetDocumentRuntimeValueDto& OutProperties);
bool WriteLeaf(
	IPropertyHandle& Handle,
	const FAssetDocumentRuntimeValueDto& Value,
	const UBlackboardKeyType* KeyType = nullptr,
	const UEnum* CanonicalEnumType = nullptr);

FAssetDocumentMutationFootprint MakeFootprint(
	const FString& StableId,
	const TCHAR* ComponentId);
FAssetDocumentPrimitiveOutcome MakeSuccess(
	FAssetDocumentRuntimeValueDto Value,
	const FString& AuthorityId,
	const FString& StableObjectId,
	const FString& Path);

bool IsParentCycle(const UBlackboardData& Blackboard, const UBlackboardData& Candidate);
bool HasInheritedKey(const UBlackboardData& Blackboard, FName Name);
bool IsVisibleInheritedBlackboardKey(const UBlackboardData& Blackboard, FName Name);
ASSETDOCUMENTAI_API bool IsEngineDerivedSelfKey(const FBlackboardEntry& Entry);

ASSETDOCUMENTAI_API FAssetDocumentPrimitiveOutcome ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest& Request,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome ApplyAssetDocumentAIBlackboardEntryAddWithIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardEntryAddRequest& Request,
	IAssetDocumentAtomicTransaction& ActiveTransaction);

FAssetDocumentPrimitiveOutcome ApplyAssetDocumentAIBlackboardEntryReplaceClassWithIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardEntryReplaceClassRequest& Request,
	IAssetDocumentAtomicTransaction& ActiveTransaction);

#if WITH_DEV_AUTOMATION_TESTS
bool ConsumeBlackboardTestFailurePoint(EAssetDocumentAIBlackboardTestFailurePoint FailurePoint);

enum class EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint : uint8
{
	None,
	AfterKeyTypeLeaf,
	AfterDescription,
	AfterCategory,
	AfterInstanceSynced,
	AfterPostReadback,
};

bool ConsumeBlackboardMetadataWriteTestFailurePoint(
	EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint FailurePoint,
	int32 KeyTypeLeafIndex = INDEX_NONE);
ASSETDOCUMENTAI_API void SetBlackboardMetadataWriteTestFailurePointInternal(
	EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint FailurePoint,
	int32 KeyTypeLeafIndex = INDEX_NONE);
ASSETDOCUMENTAI_API void BeginBlackboardMetadataGeneratorAudit();
void RecordBlackboardMetadataGenerator(
	const UObject& Object,
	const TSet<FString>* PropertyAllowList);
ASSETDOCUMENTAI_API TArray<FString> EndBlackboardMetadataGeneratorAudit();
#endif
} // namespace AssetDocumentAIBlackboardMutation
