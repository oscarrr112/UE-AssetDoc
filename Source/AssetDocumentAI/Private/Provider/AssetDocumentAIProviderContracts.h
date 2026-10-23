// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

struct FAssetDocumentAIBlackboardKeyTypeRequest
{
	FString BlackboardPath;
	FString KeyName;
	FString ExpectedClassPath;
	const FAssetDocumentRuntimeValueDto* CanonicalProperties = nullptr;
};

struct FAssetDocumentAIBlackboardEntryReadbackRequest
{
	FString BlackboardPath;
	FString KeyName;
};

struct FAssetDocumentAIBlackboardParentReadbackRequest { FString BlackboardPath; };
struct FAssetDocumentAIBlackboardLocalKeysReadbackRequest { FString BlackboardPath; };

enum class EAssetDocumentAIBlackboardEntryUpdateParseResult : uint8
{
	Succeeded,
	InvalidEnvelope,
	InvalidEntry,
};

struct FAssetDocumentAIBlackboardEntryUpdateExistingRequest
{
	FString BlackboardPath;
	FString KeyName;
	FString ExpectedClassPath;
	FAssetDocumentRuntimeValueDto CanonicalProperties;
	FString Description;
	FString Category;
	bool bInstanceSynced = false;
};

enum class EAssetDocumentAIBlackboardEntryAddParseResult : uint8
{
	Succeeded,
	InvalidEnvelope,
	InvalidEntry,
};

struct FAssetDocumentAIBlackboardEntryAddRequest
{
	FString BlackboardPath;
	FString KeyName;
	FString ExpectedClassPath;
	FAssetDocumentRuntimeValueDto CanonicalProperties;
	FString Description;
	FString Category;
	bool bInstanceSynced = false;
	int32 InsertionIndex = INDEX_NONE;
};

enum class EAssetDocumentAIBlackboardEntryReplaceClassParseResult : uint8
{
	Succeeded,
	InvalidEnvelope,
	InvalidEntry,
};

struct FAssetDocumentAIBlackboardEntryReplaceClassRequest
{
	FString BlackboardPath;
	FString KeyName;
	FString ExpectedClassPath;
	FAssetDocumentRuntimeValueDto CanonicalProperties;
	FString Description;
	FString Category;
	bool bInstanceSynced = false;
};

struct FAssetDocumentAIBlackboardEntryRemoveRequest
{
	FString BlackboardPath;
	FString KeyName;
};

struct FAssetDocumentAIBlackboardLocalKeysReorderRequest
{
	FString BlackboardPath;
	TArray<FString> OrderedLocalKeyNames;
};

struct FAssetDocumentAIBlackboardParentRequest
{
	FString BlackboardPath;
	bool bClearParent = false;
	FString ParentPath;
};

struct FAssetDocumentAIBlackboardLocalKeyRequest
{
	FString KeyName;
	FString KeyTypeClassPath;
	const FAssetDocumentRuntimeValueDto* CanonicalProperties = nullptr;
};

struct FAssetDocumentAIBlackboardLocalKeysRequest
{
	FString BlackboardPath;
	TArray<FAssetDocumentAIBlackboardLocalKeyRequest> OrderedKeys;
};

FAssetDocumentPrimitiveOutcome MakeAssetDocumentAIPrimitiveFailure(
	EAssetDocumentPrimitiveOutcomeStatus Status,
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics = {});

FAssetDocumentPrimitiveOutcome NormalizeAssetDocumentAIPrimitiveOutcome(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FAssetDocumentPrimitiveOutcome Outcome);

bool IsAssetDocumentAITransactionValid(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

ASSETDOCUMENTAI_API bool IsAssetDocumentAICanonicalRuntimeValue(
	const FAssetDocumentRuntimeValueDto& Value);

bool ParseAssetDocumentAIBlackboardKeyTypeRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	bool bRequireCanonicalProperties,
	FAssetDocumentAIBlackboardKeyTypeRequest& OutRequest);

bool ParseAssetDocumentAIBlackboardEntryReadbackRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardEntryReadbackRequest& OutRequest);
bool ParseAssetDocumentAIBlackboardParentReadbackRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardParentReadbackRequest& OutRequest);
bool ParseAssetDocumentAIBlackboardLocalKeysReadbackRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardLocalKeysReadbackRequest& OutRequest);

EAssetDocumentAIBlackboardEntryUpdateParseResult
ParseAssetDocumentAIBlackboardEntryUpdateExistingRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardEntryUpdateExistingRequest& OutRequest);

EAssetDocumentAIBlackboardEntryAddParseResult
ParseAssetDocumentAIBlackboardEntryAddRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardEntryAddRequest& OutRequest);

EAssetDocumentAIBlackboardEntryReplaceClassParseResult
ParseAssetDocumentAIBlackboardEntryReplaceClassRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardEntryReplaceClassRequest& OutRequest);

bool ParseAssetDocumentAIBlackboardEntryRemoveRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardEntryRemoveRequest& OutRequest);

bool ParseAssetDocumentAIBlackboardLocalKeysReorderRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardLocalKeysReorderRequest& OutRequest);

bool ParseAssetDocumentAIBlackboardParentRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardParentRequest& OutRequest);

bool ParseAssetDocumentAIBlackboardLocalKeysRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentAIBlackboardLocalKeysRequest& OutRequest);

FAssetDocumentVerifiedLeaseResult AcquireAssetDocumentAIMutableAssetLease(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentPrimitiveInvocation& Invocation);
