// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalRead.h"

class IAssetDocumentAtomicTransaction;
class UBlackboardData;
class UBlackboardKeyType_Struct;
struct FInstancedStruct;

namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite
{
inline constexpr FStringView GovernanceId =
	TEXTVIEW("AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-WRITE-v1");

enum class ECaller : uint8 { UpdateExistingV2, AddV2, ReplaceClassV2 };
enum class EFailure : uint8 { None, InvalidContext, InvalidDto, DependencyUnavailable, SchemaDrift, DecodeFailed, ReadbackFailed };

#if WITH_DEV_AUTOMATION_TESTS
enum class EPrepareAuditTerminal : uint8
{
	NotEntered,
	InvalidContext,
	RootDependencyUnavailable,
	ScratchDecodeFailed,
	SchemaCaptureFailed,
	DetachedReadFailed,
	DetachedReadMismatch,
	Prepared,
};

enum class EOuterPreflightAuditTerminal : uint8
{
	NotEntered,
	CanonicalPrepareFailed,
	TypedPreimageHandleUnavailable,
	CurrentReadUnavailable,
	CurrentKeysMismatch,
	MetadataReadUnavailable,
	Prepared,
};

struct FDependencyAttemptAudit
{
	FString LiteralPath;
	bool bResolveObjectSucceeded = false;
	bool bTriedLoad = false;
	bool bLoadSucceeded = false;
};

struct FPrepareTestControl
{
	int32 FieldsPrecharge = 0;
	int32 ContainerPrecharge = 0;
	int32 NodesPrecharge = 0;
	int32 DependencyAttemptPrecharge = 0;
};

enum class ELifecycleTestFailurePoint : uint8 { None, BeforeLiveLifecycle, AfterAssignment };
struct FLifecycleAuditEvent
{
	ECaller Caller = ECaller::UpdateExistingV2;
	bool bDefaultValueProperty = false;
	bool bDefaultValueMember = false;
	bool bEnteredPreEdit = false;
	bool bPostEditDispatched = false;
};
struct FLifecycleTestControl { ELifecycleTestFailurePoint FailurePoint = ELifecycleTestFailurePoint::None; };

class ASSETDOCUMENTAI_API FScopedPrepareTestControl final
{
public:
	explicit FScopedPrepareTestControl(const FPrepareTestControl& Control);
	~FScopedPrepareTestControl();
	FScopedPrepareTestControl(const FScopedPrepareTestControl&) = delete;
	FScopedPrepareTestControl& operator=(const FScopedPrepareTestControl&) = delete;
private:
	FPrepareTestControl Previous;
};

class ASSETDOCUMENTAI_API FScopedLifecycleTestControl final
{
public:
	explicit FScopedLifecycleTestControl(const FLifecycleTestControl& Control);
	~FScopedLifecycleTestControl();
	FScopedLifecycleTestControl(const FScopedLifecycleTestControl&) = delete;
	FScopedLifecycleTestControl& operator=(const FScopedLifecycleTestControl&) = delete;
private:
	FLifecycleTestControl Previous;
};

ASSETDOCUMENTAI_API void ResetPrepareAudit();
ASSETDOCUMENTAI_API EPrepareAuditTerminal GetPrepareAuditTerminal();
ASSETDOCUMENTAI_API const TCHAR* GetPrepareAuditTerminalName(EPrepareAuditTerminal Terminal);
ASSETDOCUMENTAI_API void ResetOuterPreflightAudit();
ASSETDOCUMENTAI_API EOuterPreflightAuditTerminal GetOuterPreflightAuditTerminal();
ASSETDOCUMENTAI_API const TCHAR* GetOuterPreflightAuditTerminalName(EOuterPreflightAuditTerminal Terminal);
ASSETDOCUMENTAI_API void ResetDependencyAttemptAudit();
ASSETDOCUMENTAI_API TArray<FDependencyAttemptAudit> GetDependencyAttemptAudit();
ASSETDOCUMENTAI_API void ResetScratchInitializeAudit();
ASSETDOCUMENTAI_API int32 GetScratchInitializeAuditCount();
ASSETDOCUMENTAI_API void ResetLifecycleAudit();
ASSETDOCUMENTAI_API TArray<FLifecycleAuditEvent> GetLifecycleAudit();
#endif

class FPreparedWriteState;
struct FPrepareResult;
class ASSETDOCUMENTAI_API FPreparedWrite final
{
public:
	FPreparedWrite();
	~FPreparedWrite();
	FPreparedWrite(FPreparedWrite&&) noexcept;
	FPreparedWrite& operator=(FPreparedWrite&&) noexcept;
	FPreparedWrite(const FPreparedWrite&) = delete;
	FPreparedWrite& operator=(const FPreparedWrite&) = delete;
	bool IsValid() const;
	ECaller GetCaller() const;
	UBlackboardData* GetBlackboard() const;
	UBlackboardKeyType_Struct* GetExistingTarget() const;
	IAssetDocumentAtomicTransaction* GetTransaction() const;
	FGuid GetTransactionId() const;
	FGuid GetRequestId() const;
	uint64 GetRuntimeRevision() const;
	const FInstancedStruct& GetScratch() const;
	const FAssetDocumentRuntimeValueDto& GetDesired() const;
	const AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::Private::FSchemaSnapshot& GetSchema() const;
private:
	friend ASSETDOCUMENTAI_API FPrepareResult PrepareCanonicalWrite(ECaller, UBlackboardData&, const UClass&, UBlackboardKeyType_Struct*, IAssetDocumentAtomicTransaction&, const FAssetDocumentRuntimeValueDto&);
	TUniquePtr<FPreparedWriteState> State;
};

struct FPrepareResult { bool bSucceeded = false; EFailure Failure = EFailure::InvalidContext; FPreparedWrite Prepared; };
struct FApplyResult { bool bSucceeded = false; EFailure Failure = EFailure::InvalidContext; };

ASSETDOCUMENTAI_API FPrepareResult PrepareCanonicalWrite(ECaller Caller, UBlackboardData& ExactBlackboard,
	const UClass& RequestedExactKeyTypeClass, UBlackboardKeyType_Struct* ExistingExactTarget,
	IAssetDocumentAtomicTransaction& ActiveTransaction, const FAssetDocumentRuntimeValueDto& CanonicalProperties);
ASSETDOCUMENTAI_API FApplyResult ApplyPreparedCanonicalWrite(const FPreparedWrite& Prepared, ECaller Caller,
	UBlackboardKeyType_Struct& ExactTarget, IAssetDocumentAtomicTransaction& ActiveTransaction);
} // namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite
