// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Misc/PackageName.h"

inline constexpr uint32 AssetDocumentRuntimeContractVersion = 1u;

enum class EAssetDocumentRuntimeValueKind : uint8
{
	Null = 0,
	Boolean = 1,
	Integer = 2,
	Number = 3,
	String = 4,
	Bytes = 5,
	Array = 6,
	Object = 7,
};

enum class EAssetDocumentRuntimeOperation : uint8
{
	Inspect = 0,
	Schema = 1,
	Template = 2,
	Validate = 3,
	Preflight = 4,
	Apply = 5,
	Extract = 6,
	Diff = 7,
	RegenerateSidecar = 8,
	Sync = 9,
};

enum class EAssetDocumentRuntimeRequestPhase : uint8
{
	Validate = 0,
	Preflight = 1,
	Apply = 2,
	Repair = 3,
	Persist = 4,
	Verify = 5,
	Complete = 6,
	Failed = 7,
};

enum class EAssetDocumentRuntimeTerminalStatus : uint8
{
	Succeeded = 0,
	Failed = 1,
	Partial = 2,
	Cancelled = 3,
	DeadlineExceeded = 4,
};

enum class EAssetDocumentDiagnosticSeverity : uint8
{
	Info = 0,
	Warning = 1,
	Error = 2,
};

enum class EAssetDocumentExecutionThread : uint8
{
	GameThread = 0,
};

enum class EAssetDocumentPrimitiveAccess : uint8
{
	ReadOnly = 0,
	Scratch = 1,
	Mutating = 2,
};

enum class EAssetDocumentHandleKind : uint8
{
	Object = 0,
	Class = 1,
	Struct = 2,
	Property = 3,
	Package = 4,
	Asset = 5,
	Graph = 6,
	Node = 7,
	Pin = 8,
	Subobject = 9,
	Action = 10,
	Schema = 11,
	Track = 12,
	Section = 13,
	Channel = 14,
	Curve = 15,
	Scratch = 16,
};

enum class EAssetDocumentHandleAccess : uint8
{
	ReadOnly = 0,
	Mutable = 1,
};

enum class EAssetDocumentEvidenceKind : uint8
{
	Present = 0,
	ExplicitEmpty = 1,
	AbsentByPolicy = 2,
	EvidenceUnavailable = 3,
};

enum class EAssetDocumentRuntimeSyncDirection : uint8
{
	NoChange = 0,
	AssetChanged = 1,
	SidecarChanged = 2,
	Conflict = 3,
	EvidenceUnavailable = 4,
};

struct ASSETDOCUMENT_API FAssetDocumentOpaqueHandle
{
	FGuid RequestId;
	FGuid TransactionId;
	uint64 RuntimeRevision = 0;
	FString ProviderId;
	uint32 Slot = 0;
	uint32 Generation = 0;
	EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Object;
	EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::ReadOnly;
};

struct ASSETDOCUMENT_API FAssetDocumentRuntimeValueDto
{
	EAssetDocumentRuntimeValueKind Kind = EAssetDocumentRuntimeValueKind::Null;
	bool BooleanValue = false;
	int64 IntegerValue = 0;
	double NumberValue = 0.0;
	FString StringValue;
	TArray<uint8> BytesValue;
	TArray<FAssetDocumentRuntimeValueDto> Elements;
	TArray<FString> ObjectKeys;
};

struct ASSETDOCUMENT_API FAssetDocumentRuntimeDiagnosticContext
{
	FString Key;
	FAssetDocumentRuntimeValueDto Value;
};

struct ASSETDOCUMENT_API FAssetDocumentRuntimeDiagnostic
{
	FString Path;
	FString Code;
	FString Message;
	EAssetDocumentDiagnosticSeverity Severity = EAssetDocumentDiagnosticSeverity::Error;
	TArray<FAssetDocumentRuntimeDiagnosticContext> Context;
};

struct ASSETDOCUMENT_API FAssetDocumentCancellationEvidence
{
	uint64 Sequence = 0;
	bool bRequested = false;
};

struct ASSETDOCUMENT_API FAssetDocumentChangedRecord
{
	FString AuthorityId;
	FString StableObjectId;
	FString Path;
	FString ChangeKind;
};

struct ASSETDOCUMENT_API FAssetDocumentRuntimeRequestDto
{
	uint32 AbiVersion = AssetDocumentRuntimeContractVersion;
	FGuid RequestId;
	EAssetDocumentRuntimeOperation Operation = EAssetDocumentRuntimeOperation::Validate;
	FString TargetAssetPath;
	FString RequestedProfileClassPath;
	FString RequestedAssetClassPath;
	FAssetDocumentRuntimeValueDto CanonicalDocument;
	int64 DeadlineUnixMilliseconds = 0;
	FAssetDocumentCancellationEvidence Cancellation;
	uint64 ExpectedRuntimeRevision = 0;
};

struct ASSETDOCUMENT_API FAssetDocumentRuntimeResultDto
{
	uint32 AbiVersion = AssetDocumentRuntimeContractVersion;
	FGuid RequestId;
	EAssetDocumentRuntimeTerminalStatus TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
	EAssetDocumentRuntimeRequestPhase FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
	uint64 RuntimeRevision = 0;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TOptional<FAssetDocumentRuntimeValueDto> CanonicalValue;
	TArray<uint8> CanonicalBytes;
	FString SemanticHash;
	uint32 CodecVersion = 0;
	TArray<FAssetDocumentChangedRecord> ChangedSet;
};

struct ASSETDOCUMENT_API FAssetDocumentRegionEvidence
{
	EAssetDocumentEvidenceKind Kind = EAssetDocumentEvidenceKind::EvidenceUnavailable;
	FString SemanticHash;
	uint32 CodecVersion = 0;
	TOptional<FAssetDocumentRuntimeDiagnostic> Diagnostic;
};

struct ASSETDOCUMENT_API FAssetDocumentContractValidationResult
{
	bool bValid = false;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

namespace UE::AssetDocument::RuntimeTypes::Private
{
constexpr int32 RuntimeValueMaximumDepth = 64;
// Per-call safety budget: primitive invocation values and outcomes, diagnostic context values.
// Providers page and chunk their calls to stay under it.
constexpr int32 RuntimeValueMaximumNodes = 4096;
// Whole-document budget: a request's CanonicalDocument and a result's CanonicalValue (an
// extracted or canonical document). A document is not a per-call payload; this bounds memory
// (about 2^20 nodes, e.g. 40,000 rows x 20 fields), not what one primitive call may carry.
constexpr int32 RuntimeDocumentMaximumNodes = 1 << 20;

inline FAssetDocumentContractValidationResult MakeValidationResult(
	bool bValid,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!bValid)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

inline bool IsUnicodeScalar(uint32 CodePoint)
{
	const bool bOutsideUnicode = CodePoint > 0x10ffffu;
	const bool bSurrogate = CodePoint >= 0xd800u && CodePoint <= 0xdfffu;
	return !bOutsideUnicode && !bSurrogate;
}

inline bool ConsumeUtf16Trail(const FString& Value, int32& Index)
{
	if (Index + 1 >= Value.Len())
	{
		return false;
	}
	const uint32 Trail = static_cast<uint32>(Value[Index + 1]);
	if (Trail < 0xdc00u || Trail > 0xdfffu)
	{
		return false;
	}
	++Index;
	return true;
}

inline bool IsUtf16LeadSurrogate(uint32 CodeUnit)
{
	return CodeUnit >= 0xd800u && CodeUnit <= 0xdbffu;
}

inline bool IsUtf16TrailSurrogate(uint32 CodeUnit)
{
	return CodeUnit >= 0xdc00u && CodeUnit <= 0xdfffu;
}

inline bool IsValidUtf16(const FString& Value)
{
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		const uint32 CodeUnit = static_cast<uint32>(Value[Index]);
		if (IsUtf16LeadSurrogate(CodeUnit) && !ConsumeUtf16Trail(Value, Index))
		{
			return false;
		}
		if (IsUtf16TrailSurrogate(CodeUnit))
		{
			return false;
		}
	}
	return true;
}

inline bool IsValidUtf32(const FString& Value)
{
	for (TCHAR Character : Value)
	{
		if (!IsUnicodeScalar(static_cast<uint32>(Character)))
		{
			return false;
		}
	}
	return true;
}

inline bool IsValidUtf(const FString& Value)
{
	if constexpr (sizeof(TCHAR) == 2)
	{
		return IsValidUtf16(Value);
	}
	else
	{
		return IsValidUtf32(Value);
	}
}

inline bool HasDefaultScalarFields(const FAssetDocumentRuntimeValueDto& Value)
{
	return !Value.BooleanValue && Value.IntegerValue == 0 && Value.NumberValue == 0.0;
}

inline bool HasDefaultTextFields(const FAssetDocumentRuntimeValueDto& Value)
{
	return Value.StringValue.IsEmpty() && Value.BytesValue.IsEmpty();
}

inline bool HasDefaultContainerFields(const FAssetDocumentRuntimeValueDto& Value)
{
	return Value.Elements.IsEmpty() && Value.ObjectKeys.IsEmpty();
}

inline bool IsNullShape(const FAssetDocumentRuntimeValueDto& Value)
{
	return HasDefaultScalarFields(Value)
		&& HasDefaultTextFields(Value)
		&& HasDefaultContainerFields(Value);
}

inline bool IsBooleanShape(const FAssetDocumentRuntimeValueDto& Value)
{
	const bool bOtherScalarsDefault = Value.IntegerValue == 0 && Value.NumberValue == 0.0;
	return bOtherScalarsDefault
		&& HasDefaultTextFields(Value)
		&& HasDefaultContainerFields(Value);
}

inline bool IsIntegerShape(const FAssetDocumentRuntimeValueDto& Value)
{
	const bool bOtherScalarsDefault = !Value.BooleanValue && Value.NumberValue == 0.0;
	return bOtherScalarsDefault
		&& HasDefaultTextFields(Value)
		&& HasDefaultContainerFields(Value);
}

inline bool IsNumberShape(const FAssetDocumentRuntimeValueDto& Value)
{
	const bool bOtherScalarsDefault = !Value.BooleanValue && Value.IntegerValue == 0;
	return bOtherScalarsDefault
		&& FMath::IsFinite(Value.NumberValue)
		&& HasDefaultTextFields(Value)
		&& HasDefaultContainerFields(Value);
}

inline bool IsStringShape(const FAssetDocumentRuntimeValueDto& Value)
{
	return HasDefaultScalarFields(Value)
		&& Value.BytesValue.IsEmpty()
		&& HasDefaultContainerFields(Value)
		&& IsValidUtf(Value.StringValue);
}

inline bool IsBytesShape(const FAssetDocumentRuntimeValueDto& Value)
{
	return HasDefaultScalarFields(Value)
		&& Value.StringValue.IsEmpty()
		&& HasDefaultContainerFields(Value);
}

inline bool IsRuntimeValueShapeValid(
	const FAssetDocumentRuntimeValueDto& Value,
	int32 Depth,
	int32& RemainingNodes);

inline bool AreElementsValid(
	const TArray<FAssetDocumentRuntimeValueDto>& Elements,
	int32 Depth,
	int32& RemainingNodes);

inline bool CanValidateChildren(int32 ChildCount, int32 RemainingNodes)
{
	return ChildCount >= 0 && ChildCount <= RemainingNodes;
}

inline bool IsArrayShape(
	const FAssetDocumentRuntimeValueDto& Value,
	int32 Depth,
	int32& RemainingNodes)
{
	const bool bInactiveFieldsDefault = HasDefaultScalarFields(Value)
		&& HasDefaultTextFields(Value)
		&& Value.ObjectKeys.IsEmpty();
	return bInactiveFieldsDefault
		&& CanValidateChildren(Value.Elements.Num(), RemainingNodes)
		&& AreElementsValid(Value.Elements, Depth, RemainingNodes);
}

// JSON object keys are exact; FString's default set equality ignores case.
struct FExactObjectKeyFuncs : DefaultKeyFuncs<FString>
{
	static bool Matches(KeyInitType A, KeyInitType B)
	{
		return A.Len() == B.Len()
			&& FMemory::Memcmp(*A, *B, SIZE_T(A.Len()) * sizeof(TCHAR)) == 0;
	}
};

inline bool AreObjectKeysValid(const TArray<FString>& ObjectKeys)
{
	TSet<FString, FExactObjectKeyFuncs> SeenKeys;
	for (const FString& Key : ObjectKeys)
	{
		if (!IsValidUtf(Key))
		{
			return false;
		}
		if (SeenKeys.Contains(Key))
		{
			return false;
		}
		SeenKeys.Add(Key);
	}
	return true;
}

inline bool CanValidateObjectStorage(
	const FAssetDocumentRuntimeValueDto& Value,
	int32 RemainingNodes)
{
	return Value.ObjectKeys.Num() == Value.Elements.Num()
		&& CanValidateChildren(Value.Elements.Num(), RemainingNodes);
}

inline bool IsObjectShape(
	const FAssetDocumentRuntimeValueDto& Value,
	int32 Depth,
	int32& RemainingNodes)
{
	if (!CanValidateObjectStorage(Value, RemainingNodes))
	{
		return false;
	}
	const bool bInactiveFieldsDefault = HasDefaultScalarFields(Value) && HasDefaultTextFields(Value);
	return bInactiveFieldsDefault
		&& AreObjectKeysValid(Value.ObjectKeys)
		&& AreElementsValid(Value.Elements, Depth, RemainingNodes);
}

inline bool IsScalarRuntimeValueShapeValid(
	const FAssetDocumentRuntimeValueDto& Value,
	int32 Depth,
	int32& RemainingNodes)
{
	using FShapeValidator = bool (*)(const FAssetDocumentRuntimeValueDto&);
	static constexpr FShapeValidator Validators[] = {
		&IsNullShape,
		&IsBooleanShape,
		&IsIntegerShape,
		&IsNumberShape,
		&IsStringShape,
		&IsBytesShape,
	};
	const uint8 KindIndex = static_cast<uint8>(Value.Kind);
	(void)Depth;
	(void)RemainingNodes;
	if (KindIndex >= UE_ARRAY_COUNT(Validators))
	{
		return false;
	}
	return Validators[KindIndex](Value);
}

inline bool ConsumeRuntimeValueBudget(int32 Depth, int32& RemainingNodes)
{
	if (Depth > RuntimeValueMaximumDepth || RemainingNodes <= 0)
	{
		return false;
	}
	--RemainingNodes;
	return true;
}

inline bool IsRuntimeValueShapeValid(
	const FAssetDocumentRuntimeValueDto& Value,
	int32 Depth,
	int32& RemainingNodes)
{
	using FTreeValidator = bool (*)(const FAssetDocumentRuntimeValueDto&, int32, int32&);
	static constexpr FTreeValidator Validators[] = {
		&IsScalarRuntimeValueShapeValid,
		&IsScalarRuntimeValueShapeValid,
		&IsScalarRuntimeValueShapeValid,
		&IsScalarRuntimeValueShapeValid,
		&IsScalarRuntimeValueShapeValid,
		&IsScalarRuntimeValueShapeValid,
		&IsArrayShape,
		&IsObjectShape,
	};
	if (!ConsumeRuntimeValueBudget(Depth, RemainingNodes))
	{
		return false;
	}
	const uint8 KindIndex = static_cast<uint8>(Value.Kind);
	if (KindIndex >= UE_ARRAY_COUNT(Validators))
	{
		return false;
	}
	return Validators[KindIndex](Value, Depth, RemainingNodes);
}

inline bool AreElementsValid(
	const TArray<FAssetDocumentRuntimeValueDto>& Elements,
	int32 Depth,
	int32& RemainingNodes)
{
	for (const FAssetDocumentRuntimeValueDto& Element : Elements)
	{
		if (!IsRuntimeValueShapeValid(Element, Depth + 1, RemainingNodes))
		{
			return false;
		}
	}
	return true;
}

inline bool IsRuntimeValueTreeValid(const FAssetDocumentRuntimeValueDto& Value, int32 MaximumNodes = RuntimeValueMaximumNodes)
{
	int32 RemainingNodes = MaximumNodes;
	return IsRuntimeValueShapeValid(Value, 0, RemainingNodes);
}

inline bool IsRuntimeDocumentTreeValid(const FAssetDocumentRuntimeValueDto& Value)
{
	return IsRuntimeValueTreeValid(Value, RuntimeDocumentMaximumNodes);
}

inline bool IsValidObjectPath(const FString& Path)
{
	return FPackageName::IsValidObjectPath(FStringView(Path));
}

inline bool IsValidOptionalObjectPath(const FString& Path)
{
	return Path.IsEmpty() || IsValidObjectPath(Path);
}

inline bool IsValidRequestIdentity(const FAssetDocumentRuntimeRequestDto& Request)
{
	return Request.AbiVersion == AssetDocumentRuntimeContractVersion && Request.RequestId.IsValid();
}

inline bool IsValidRequestPaths(const FAssetDocumentRuntimeRequestDto& Request)
{
	return IsValidObjectPath(Request.TargetAssetPath)
		&& IsValidOptionalObjectPath(Request.RequestedProfileClassPath)
		&& IsValidOptionalObjectPath(Request.RequestedAssetClassPath);
}

inline bool IsValidRequestClock(const FAssetDocumentRuntimeRequestDto& Request)
{
	const bool bCancellationEvidenceValid = !Request.Cancellation.bRequested || Request.Cancellation.Sequence > 0;
	return Request.DeadlineUnixMilliseconds >= 0 && bCancellationEvidenceValid;
}

inline bool IsValidOperation(EAssetDocumentRuntimeOperation Operation)
{
	return static_cast<uint8>(Operation) <= static_cast<uint8>(EAssetDocumentRuntimeOperation::Sync);
}

inline bool IsValidRequestRevision(const FAssetDocumentRuntimeRequestDto& Request)
{
	return Request.Operation != EAssetDocumentRuntimeOperation::Apply || Request.ExpectedRuntimeRevision > 0;
}

inline bool IsValidRequestEnvelope(const FAssetDocumentRuntimeRequestDto& Request)
{
	return IsValidRequestIdentity(Request) && IsValidRequestPaths(Request);
}

inline bool IsValidRequestExecution(const FAssetDocumentRuntimeRequestDto& Request)
{
	return IsValidOperation(Request.Operation)
		&& IsValidRequestClock(Request)
		&& IsValidRequestRevision(Request);
}

inline bool IsValidResultRevision(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentRuntimeResultDto& Result)
{
	const bool bRevisionMatches = Request.ExpectedRuntimeRevision == 0
		|| Result.RuntimeRevision == Request.ExpectedRuntimeRevision;
	return Result.RuntimeRevision > 0 && bRevisionMatches;
}

inline bool IsValidResultIdentity(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentRuntimeResultDto& Result)
{
	const bool bAbiAndRequestMatch = Result.AbiVersion == AssetDocumentRuntimeContractVersion
		&& Result.RequestId.IsValid()
		&& Result.RequestId == Request.RequestId;
	return bAbiAndRequestMatch && IsValidResultRevision(Request, Result);
}

inline bool IsTerminalPhaseValid(const FAssetDocumentRuntimeResultDto& Result)
{
	const bool bCompleted = Result.TerminalStatus == EAssetDocumentRuntimeTerminalStatus::Succeeded
		|| Result.TerminalStatus == EAssetDocumentRuntimeTerminalStatus::Partial;
	const EAssetDocumentRuntimeRequestPhase ExpectedPhase = bCompleted
		? EAssetDocumentRuntimeRequestPhase::Complete
		: EAssetDocumentRuntimeRequestPhase::Failed;
	return Result.FinalPhase == ExpectedPhase;
}

inline bool IsTerminalStatusValid(EAssetDocumentRuntimeTerminalStatus Status)
{
	return static_cast<uint8>(Status) <= static_cast<uint8>(EAssetDocumentRuntimeTerminalStatus::DeadlineExceeded);
}

inline bool IsPartialMutation(const FAssetDocumentRuntimeRequestDto& Request, const FAssetDocumentRuntimeResultDto& Result)
{
	return Request.Operation == EAssetDocumentRuntimeOperation::Apply
		&& Result.TerminalStatus == EAssetDocumentRuntimeTerminalStatus::Partial;
}

inline bool IsFailedMutationWithChanges(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentRuntimeResultDto& Result)
{
	const bool bFailed = Result.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded;
	return Request.Operation == EAssetDocumentRuntimeOperation::Apply
		&& bFailed
		&& !Result.ChangedSet.IsEmpty();
}

inline bool HasAnyByteEvidence(const FAssetDocumentRuntimeResultDto& Result)
{
	return !Result.CanonicalBytes.IsEmpty() || !Result.SemanticHash.IsEmpty() || Result.CodecVersion > 0;
}

inline bool HasCompleteByteEvidence(const FAssetDocumentRuntimeResultDto& Result)
{
	return !Result.CanonicalBytes.IsEmpty() && !Result.SemanticHash.IsEmpty() && Result.CodecVersion > 0;
}

inline bool IsByteEvidenceValid(const FAssetDocumentRuntimeResultDto& Result)
{
	return !HasAnyByteEvidence(Result) || HasCompleteByteEvidence(Result);
}

inline bool IsDiagnosticContextValid(const FAssetDocumentRuntimeDiagnosticContext& Context)
{
	return !Context.Key.IsEmpty()
		&& IsValidUtf(Context.Key)
		&& IsRuntimeValueTreeValid(Context.Value);
}

inline bool AreDiagnosticContextsValid(const FAssetDocumentRuntimeDiagnostic& Diagnostic)
{
	for (const FAssetDocumentRuntimeDiagnosticContext& Context : Diagnostic.Context)
	{
		if (!IsDiagnosticContextValid(Context))
		{
			return false;
		}
	}
	return true;
}

inline bool HasRequiredDiagnosticText(const FAssetDocumentRuntimeDiagnostic& Diagnostic)
{
	return !Diagnostic.Code.IsEmpty() && !Diagnostic.Message.IsEmpty();
}

inline bool HasValidDiagnosticText(const FAssetDocumentRuntimeDiagnostic& Diagnostic)
{
	return IsValidUtf(Diagnostic.Path)
		&& IsValidUtf(Diagnostic.Code)
		&& IsValidUtf(Diagnostic.Message);
}

inline bool IsDiagnosticSeverityValid(EAssetDocumentDiagnosticSeverity Severity)
{
	return static_cast<uint8>(Severity) <= static_cast<uint8>(EAssetDocumentDiagnosticSeverity::Error);
}

inline bool IsDiagnosticValid(const FAssetDocumentRuntimeDiagnostic& Diagnostic)
{
	return HasRequiredDiagnosticText(Diagnostic)
		&& HasValidDiagnosticText(Diagnostic)
		&& IsDiagnosticSeverityValid(Diagnostic.Severity)
		&& AreDiagnosticContextsValid(Diagnostic);
}

inline bool AreDiagnosticsValid(const FAssetDocumentRuntimeResultDto& Result)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Result.Diagnostics)
	{
		if (!IsDiagnosticValid(Diagnostic))
		{
			return false;
		}
	}
	return true;
}

inline bool HasRequiredChangedRecordText(const FAssetDocumentChangedRecord& Changed)
{
	return !Changed.AuthorityId.IsEmpty()
		&& !Changed.StableObjectId.IsEmpty()
		&& !Changed.Path.IsEmpty()
		&& !Changed.ChangeKind.IsEmpty();
}

inline bool HasValidChangedRecordText(const FAssetDocumentChangedRecord& Changed)
{
	return IsValidUtf(Changed.AuthorityId)
		&& IsValidUtf(Changed.StableObjectId)
		&& IsValidUtf(Changed.Path)
		&& IsValidUtf(Changed.ChangeKind);
}

inline bool IsChangedRecordValid(const FAssetDocumentChangedRecord& Changed)
{
	return HasRequiredChangedRecordText(Changed) && HasValidChangedRecordText(Changed);
}

inline bool AreChangedRecordsValid(const FAssetDocumentRuntimeResultDto& Result)
{
	for (const FAssetDocumentChangedRecord& Changed : Result.ChangedSet)
	{
		if (!IsChangedRecordValid(Changed))
		{
			return false;
		}
	}
	return true;
}

inline bool IsCanonicalValueValid(const FAssetDocumentRuntimeResultDto& Result)
{
	return !Result.CanonicalValue.IsSet() || IsRuntimeDocumentTreeValid(Result.CanonicalValue.GetValue());
}

inline bool IsResultTerminalValid(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentRuntimeResultDto& Result)
{
	return IsTerminalStatusValid(Result.TerminalStatus)
		&& IsTerminalPhaseValid(Result)
		&& !IsPartialMutation(Request, Result)
		&& !IsFailedMutationWithChanges(Request, Result);
}

inline bool IsResultPayloadValid(const FAssetDocumentRuntimeResultDto& Result)
{
	return IsCanonicalValueValid(Result)
		&& IsByteEvidenceValid(Result)
		&& AreDiagnosticsValid(Result)
		&& AreChangedRecordsValid(Result);
}
}

inline FAssetDocumentContractValidationResult ValidateAssetDocumentRuntimeValue(
	const FAssetDocumentRuntimeValueDto& Value)
{
	const bool bValid = UE::AssetDocument::RuntimeTypes::Private::IsRuntimeValueTreeValid(Value);
	return UE::AssetDocument::RuntimeTypes::Private::MakeValidationResult(
		bValid,
		TEXT("RuntimeValue.Invalid"),
		TEXT("Runtime value kind, active fields, or nested value is invalid."));
}

// A whole document (request CanonicalDocument, result CanonicalValue): the document budget.
inline FAssetDocumentContractValidationResult ValidateAssetDocumentRuntimeDocument(
	const FAssetDocumentRuntimeValueDto& Value)
{
	const bool bValid = UE::AssetDocument::RuntimeTypes::Private::IsRuntimeDocumentTreeValid(Value);
	return UE::AssetDocument::RuntimeTypes::Private::MakeValidationResult(
		bValid,
		TEXT("RuntimeValue.Invalid"),
		TEXT("Runtime document kind, active fields, or nested value is invalid."));
}

inline FAssetDocumentContractValidationResult ValidateAssetDocumentRuntimeRequest(
	const FAssetDocumentRuntimeRequestDto& Request)
{
	using namespace UE::AssetDocument::RuntimeTypes::Private;
	if (!IsValidRequestEnvelope(Request))
	{
		return MakeValidationResult(false, TEXT("RuntimeRequest.InvalidEnvelope"), TEXT("Request identity or path is invalid."));
	}
	if (!IsValidRequestExecution(Request))
	{
		return MakeValidationResult(false, TEXT("RuntimeRequest.InvalidExecution"), TEXT("Request operation, deadline, cancellation, or revision is invalid."));
	}
	return MakeValidationResult(
		IsRuntimeDocumentTreeValid(Request.CanonicalDocument),
		TEXT("RuntimeRequest.InvalidDocument"),
		TEXT("Canonical document is invalid."));
}

inline FAssetDocumentContractValidationResult ValidateAssetDocumentRuntimeResult(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentRuntimeResultDto& Result)
{
	using namespace UE::AssetDocument::RuntimeTypes::Private;
	if (!IsValidResultIdentity(Request, Result))
	{
		return MakeValidationResult(false, TEXT("RuntimeResult.InvalidIdentity"), TEXT("Result ABI, request identity, or runtime revision is invalid."));
	}
	if (!IsResultTerminalValid(Request, Result))
	{
		return MakeValidationResult(false, TEXT("RuntimeResult.InvalidTerminal"), TEXT("Terminal status, phase, or mutation outcome is invalid."));
	}
	return MakeValidationResult(
		IsResultPayloadValid(Result),
		TEXT("RuntimeResult.InvalidPayload"),
		TEXT("Result payload or evidence is invalid."));
}
