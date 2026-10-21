// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Runtime/AssetDocumentRuntimeTypes.h"

struct FInstancedStruct;
class FProperty;
class UBlackboardKeyType_Struct;
class UScriptStruct;

namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalRead
{
inline constexpr FStringView GovernanceId =
	TEXTVIEW("AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-READ-v1");

enum class ESource : uint8
{
	ExactLiveDefaultValue,
	DetachedScratch,
};

enum class EFailure : uint8
{
	None,
	UnsupportedProperty,
	UnstableReference,
	UnstableText,
	NonFiniteNumber,
	InvalidEnum,
	AmbiguousName,
	CanonicalCollision,
	BudgetExceeded,
	SchemaDrift,
	InvalidContext,
	DtoInvalid,
};

#if WITH_DEV_AUTOMATION_TESTS
struct FReadTestControl
{
	int32 PrechargeSchemaFields = 0;
	int32 PrechargeFields = 0;
	int32 PrechargeContainerEntries = 0;
	int32 PrechargeNodes = 0;
	int64 PrechargeComparisonBytes = 0;
	bool bForceSchemaDrift = false;
};
#endif

namespace Private
{
struct FReadBudgetPrecharge
{
	int32 SchemaFields = 0;
	int32 Fields = 0;
	int32 ContainerEntries = 0;
	int32 Nodes = 0;
	int64 ComparisonBytes = 0;
};

FReadBudgetPrecharge GetReadBudgetPrecharge();
bool ShouldForceReadSchemaDrift();

enum class ESchemaTokenKind : uint8
{
	Struct,
	Inheritance,
	Property,
	Container,
	DynamicStruct,
};

struct FSchemaToken
{
	ESchemaTokenKind Kind = ESchemaTokenKind::Struct;
	UPTRINT Identity = 0;
	UPTRINT TypeA = 0;
	UPTRINT TypeB = 0;
	UPTRINT TypeC = 0;
	uint64 Flags = 0;
	int32 ArrayDim = 0;
	FString Name;
	FString OwnerPath;
	FString TypePathA;
	FString TypePathB;

	bool operator==(const FSchemaToken& Other) const;
};

struct FSchemaSnapshot
{
	TArray<FSchemaToken> Tokens;

	bool operator==(const FSchemaSnapshot& Other) const;
};

bool CollectStoredFields(
	const UScriptStruct& ScriptStruct,
	int32& RemainingFields,
	TArray<TPair<FString, const FProperty*>>& OutFields,
	EFailure& OutFailure);

bool CaptureSchemaSnapshot(
	const UScriptStruct& ScriptStruct,
	const void* Memory,
	FSchemaSnapshot& OutSnapshot,
	EFailure& OutFailure);
#if WITH_DEV_AUTOMATION_TESTS
class ASSETDOCUMENTAI_API FScopedReadTestControl final
{
public:
	explicit FScopedReadTestControl(const FReadTestControl& Control);
	~FScopedReadTestControl();

	FScopedReadTestControl(const FScopedReadTestControl&) = delete;
	FScopedReadTestControl& operator=(const FScopedReadTestControl&) = delete;

private:
	const FReadTestControl* Previous = nullptr;
};
#endif
} // namespace Private

struct FReadResult
{
	bool bSucceeded = false;
	EFailure Failure = EFailure::InvalidContext;
	FAssetDocumentRuntimeValueDto Value;
};

ASSETDOCUMENTAI_API FReadResult ReadCanonicalValue(
	const FInstancedStruct& SourceValue,
	ESource Source,
	const UBlackboardKeyType_Struct* ExactLiveOwner);
} // namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalRead
