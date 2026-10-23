// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentRuntimeTypes.h"

#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"

#include <limits>
#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentRuntimeTypesContractTests
{
// S: Compile-time wire values are part of the v1 ABI.
static_assert(AssetDocumentRuntimeContractVersion == 1u);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeValueKind::Null) == 0);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeValueKind::Boolean) == 1);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeValueKind::Integer) == 2);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeValueKind::Number) == 3);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeValueKind::String) == 4);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeValueKind::Bytes) == 5);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeValueKind::Array) == 6);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeValueKind::Object) == 7);

static_assert(static_cast<uint8>(EAssetDocumentRuntimeOperation::Inspect) == 0);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeOperation::Schema) == 1);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeOperation::Template) == 2);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeOperation::Validate) == 3);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeOperation::Preflight) == 4);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeOperation::Apply) == 5);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeOperation::Extract) == 6);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeOperation::Diff) == 7);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeOperation::RegenerateSidecar) == 8);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeOperation::Sync) == 9);

static_assert(static_cast<uint8>(EAssetDocumentRuntimeRequestPhase::Validate) == 0);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeRequestPhase::Preflight) == 1);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeRequestPhase::Apply) == 2);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeRequestPhase::Repair) == 3);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeRequestPhase::Persist) == 4);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeRequestPhase::Verify) == 5);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeRequestPhase::Complete) == 6);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeRequestPhase::Failed) == 7);

static_assert(static_cast<uint8>(EAssetDocumentRuntimeTerminalStatus::Succeeded) == 0);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeTerminalStatus::Failed) == 1);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeTerminalStatus::Partial) == 2);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeTerminalStatus::Cancelled) == 3);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeTerminalStatus::DeadlineExceeded) == 4);
static_assert(static_cast<uint8>(EAssetDocumentDiagnosticSeverity::Info) == 0);
static_assert(static_cast<uint8>(EAssetDocumentDiagnosticSeverity::Warning) == 1);
static_assert(static_cast<uint8>(EAssetDocumentDiagnosticSeverity::Error) == 2);
static_assert(static_cast<uint8>(EAssetDocumentExecutionThread::GameThread) == 0);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveAccess::ReadOnly) == 0);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveAccess::Scratch) == 1);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveAccess::Mutating) == 2);

static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Object) == 0);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Class) == 1);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Struct) == 2);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Property) == 3);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Package) == 4);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Asset) == 5);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Graph) == 6);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Node) == 7);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Pin) == 8);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Subobject) == 9);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Action) == 10);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Schema) == 11);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Track) == 12);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Section) == 13);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Channel) == 14);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Curve) == 15);
static_assert(static_cast<uint8>(EAssetDocumentHandleKind::Scratch) == 16);
static_assert(static_cast<uint8>(EAssetDocumentHandleAccess::ReadOnly) == 0);
static_assert(static_cast<uint8>(EAssetDocumentHandleAccess::Mutable) == 1);

static_assert(static_cast<uint8>(EAssetDocumentEvidenceKind::Present) == 0);
static_assert(static_cast<uint8>(EAssetDocumentEvidenceKind::ExplicitEmpty) == 1);
static_assert(static_cast<uint8>(EAssetDocumentEvidenceKind::AbsentByPolicy) == 2);
static_assert(static_cast<uint8>(EAssetDocumentEvidenceKind::EvidenceUnavailable) == 3);
static_assert(std::is_enum_v<EAssetDocumentRuntimeSyncDirection>);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeSyncDirection::NoChange) == 0);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeSyncDirection::AssetChanged) == 1);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeSyncDirection::SidecarChanged) == 2);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeSyncDirection::Conflict) == 3);
static_assert(static_cast<uint8>(EAssetDocumentRuntimeSyncDirection::EvidenceUnavailable) == 4);

// S: Every member type is frozen independently from its runtime default.
#define ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(Type, Member, Expected) \
	static_assert(std::is_same_v<decltype(Type::Member), Expected>)
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentOpaqueHandle, RequestId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentOpaqueHandle, TransactionId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentOpaqueHandle, RuntimeRevision, uint64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentOpaqueHandle, ProviderId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentOpaqueHandle, Slot, uint32);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentOpaqueHandle, Generation, uint32);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentOpaqueHandle, Kind, EAssetDocumentHandleKind);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentOpaqueHandle, Access, EAssetDocumentHandleAccess);

ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeValueDto, Kind, EAssetDocumentRuntimeValueKind);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeValueDto, BooleanValue, bool);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeValueDto, IntegerValue, int64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeValueDto, NumberValue, double);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeValueDto, StringValue, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeValueDto, BytesValue, TArray<uint8>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeValueDto, Elements, TArray<FAssetDocumentRuntimeValueDto>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeValueDto, ObjectKeys, TArray<FString>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeDiagnosticContext, Key, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeDiagnosticContext, Value, FAssetDocumentRuntimeValueDto);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeDiagnostic, Path, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeDiagnostic, Code, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeDiagnostic, Message, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeDiagnostic, Severity, EAssetDocumentDiagnosticSeverity);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeDiagnostic, Context, TArray<FAssetDocumentRuntimeDiagnosticContext>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentCancellationEvidence, Sequence, uint64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentCancellationEvidence, bRequested, bool);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentChangedRecord, AuthorityId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentChangedRecord, StableObjectId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentChangedRecord, Path, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentChangedRecord, ChangeKind, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeRequestDto, AbiVersion, uint32);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeRequestDto, RequestId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeRequestDto, Operation, EAssetDocumentRuntimeOperation);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeRequestDto, TargetAssetPath, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeRequestDto, RequestedProfileClassPath, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeRequestDto, RequestedAssetClassPath, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeRequestDto, CanonicalDocument, FAssetDocumentRuntimeValueDto);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeRequestDto, DeadlineUnixMilliseconds, int64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeRequestDto, Cancellation, FAssetDocumentCancellationEvidence);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeRequestDto, ExpectedRuntimeRevision, uint64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeResultDto, AbiVersion, uint32);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeResultDto, RequestId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeResultDto, TerminalStatus, EAssetDocumentRuntimeTerminalStatus);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeResultDto, FinalPhase, EAssetDocumentRuntimeRequestPhase);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeResultDto, RuntimeRevision, uint64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeResultDto, Diagnostics, TArray<FAssetDocumentRuntimeDiagnostic>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeResultDto, CanonicalValue, TOptional<FAssetDocumentRuntimeValueDto>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeResultDto, CanonicalBytes, TArray<uint8>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeResultDto, SemanticHash, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeResultDto, CodecVersion, uint32);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRuntimeResultDto, ChangedSet, TArray<FAssetDocumentChangedRecord>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRegionEvidence, Kind, EAssetDocumentEvidenceKind);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRegionEvidence, SemanticHash, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRegionEvidence, CodecVersion, uint32);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRegionEvidence, Diagnostic, TOptional<FAssetDocumentRuntimeDiagnostic>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentContractValidationResult, bValid, bool);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentContractValidationResult, Diagnostics, TArray<FAssetDocumentRuntimeDiagnostic>);
#undef ASSET_DOCUMENT_ASSERT_MEMBER_TYPE

using FValidateValueSignature = FAssetDocumentContractValidationResult (*)(const FAssetDocumentRuntimeValueDto&);
using FValidateRequestSignature = FAssetDocumentContractValidationResult (*)(const FAssetDocumentRuntimeRequestDto&);
using FValidateResultSignature = FAssetDocumentContractValidationResult (*)(const FAssetDocumentRuntimeRequestDto&, const FAssetDocumentRuntimeResultDto&);
static_assert(std::is_same_v<decltype(&ValidateAssetDocumentRuntimeValue), FValidateValueSignature>);
static_assert(std::is_same_v<decltype(&ValidateAssetDocumentRuntimeRequest), FValidateRequestSignature>);
static_assert(std::is_same_v<decltype(&ValidateAssetDocumentRuntimeResult), FValidateResultSignature>);
static_assert(!std::is_constructible_v<FAssetDocumentOpaqueHandle, FAssetDocumentRuntimeValueDto>);
static_assert(!std::is_constructible_v<FAssetDocumentRuntimeValueDto, FAssetDocumentOpaqueHandle>);
static_assert(!std::is_convertible_v<FAssetDocumentOpaqueHandle, FAssetDocumentRuntimeValueDto>);
static_assert(!std::is_convertible_v<FAssetDocumentRuntimeValueDto, FAssetDocumentOpaqueHandle>);

FAssetDocumentRuntimeValueDto MakeStringValue(const FString& Text)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Value.StringValue = Text;
	return Value;
}

FAssetDocumentRuntimeValueDto MakeValidObject()
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Value.ObjectKeys = {TEXT("enabled"), TEXT("name")};
	FAssetDocumentRuntimeValueDto Boolean;
	Boolean.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Boolean.BooleanValue = true;
	Value.Elements = {Boolean, MakeStringValue(TEXT("Runtime"))};
	return Value;
}

FAssetDocumentRuntimeRequestDto MakeValidRequest(EAssetDocumentRuntimeOperation Operation)
{
	FAssetDocumentRuntimeRequestDto Request;
	Request.RequestId = FGuid::NewGuid();
	Request.Operation = Operation;
	Request.TargetAssetPath = TEXT("/Game/AssetDocument/T_Test.T_Test");
	Request.RequestedProfileClassPath = TEXT("/Script/AssetDocument.AssetDocumentProfile");
	Request.RequestedAssetClassPath = TEXT("/Script/Engine.Blueprint");
	Request.CanonicalDocument = MakeValidObject();
	Request.DeadlineUnixMilliseconds = 4102444800000;
	Request.ExpectedRuntimeRevision = Operation == EAssetDocumentRuntimeOperation::Apply ? 7 : 0;
	return Request;
}

FAssetDocumentRuntimeResultDto MakeValidResult(const FAssetDocumentRuntimeRequestDto& Request)
{
	FAssetDocumentRuntimeResultDto Result;
	Result.RequestId = Request.RequestId;
	Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded;
	Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
	Result.RuntimeRevision = Request.ExpectedRuntimeRevision > 0 ? Request.ExpectedRuntimeRevision : 7;
	Result.CanonicalValue = MakeValidObject();
	Result.CanonicalBytes = {0x7b, 0x7d};
	Result.SemanticHash = TEXT("sha256:runtime-contract");
	Result.CodecVersion = 1;
	return Result;
}

FString MakeInvalidUnicodeString()
{
	FString Invalid;
	Invalid.AppendChar(static_cast<TCHAR>(0xd800));
	return Invalid;
}

FAssetDocumentRuntimeValueDto MakeNestedArray(int32 Depth)
{
	FAssetDocumentRuntimeValueDto Value;
	for (int32 Index = 0; Index < Depth; ++Index)
	{
		FAssetDocumentRuntimeValueDto Parent;
		Parent.Kind = EAssetDocumentRuntimeValueKind::Array;
		Parent.Elements.Add(MoveTemp(Value));
		Value = MoveTemp(Parent);
	}
	return Value;
}

FAssetDocumentRuntimeValueDto MakeWideArray(int32 ChildCount)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Array;
	Value.Elements.SetNum(ChildCount);
	return Value;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRuntimeTypesContractTest,
	"UEAssetDoc.RuntimeContracts.RuntimeTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRuntimeTypesContractTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRuntimeTypesContractTests;
	constexpr int32 TestedMaximumRuntimeValueDepth = 64;
	constexpr int32 TestedMaximumRuntimeValueNodes = 4096;
	const auto TestInvalidContract = [this](
		const TCHAR* Label,
		const FAssetDocumentContractValidationResult& Validation,
		const TCHAR* ExpectedCode,
		const TCHAR* ExpectedMessage)
	{
		TestFalse(*FString::Printf(TEXT("P: %s is rejected"), Label), Validation.bValid);
		TestEqual(*FString::Printf(TEXT("P: %s has one stable diagnostic"), Label), Validation.Diagnostics.Num(), 1);
		if (Validation.Diagnostics.Num() == 1)
		{
			const FAssetDocumentRuntimeDiagnostic& Diagnostic = Validation.Diagnostics[0];
			TestEqual(*FString::Printf(TEXT("P: %s diagnostic path"), Label), Diagnostic.Path, FString(TEXT("/")));
			TestEqual(*FString::Printf(TEXT("P: %s diagnostic code"), Label), Diagnostic.Code, FString(ExpectedCode));
			TestEqual(*FString::Printf(TEXT("P: %s diagnostic message"), Label), Diagnostic.Message, FString(ExpectedMessage));
		}
	};

	// S: Defaults freeze the zero-state ABI independently from validation.
	const FAssetDocumentOpaqueHandle Handle;
	TestFalse(TEXT("S: handle request id defaults invalid"), Handle.RequestId.IsValid());
	TestFalse(TEXT("S: handle transaction id defaults invalid"), Handle.TransactionId.IsValid());
	TestEqual(TEXT("S: handle runtime revision defaults zero"), Handle.RuntimeRevision, uint64{0});
	TestTrue(TEXT("S: handle provider defaults empty"), Handle.ProviderId.IsEmpty());
	TestEqual(TEXT("S: handle slot defaults zero"), Handle.Slot, uint32{0});
	TestEqual(TEXT("S: handle generation defaults zero"), Handle.Generation, uint32{0});
	TestEqual(TEXT("S: handle kind defaults object"), Handle.Kind, EAssetDocumentHandleKind::Object);
	TestEqual(TEXT("S: handle access defaults read-only"), Handle.Access, EAssetDocumentHandleAccess::ReadOnly);

	const FAssetDocumentRuntimeValueDto DefaultValue;
	TestEqual(TEXT("S: value kind defaults null"), DefaultValue.Kind, EAssetDocumentRuntimeValueKind::Null);
	TestFalse(TEXT("S: boolean defaults false"), DefaultValue.BooleanValue);
	TestEqual(TEXT("S: integer defaults zero"), DefaultValue.IntegerValue, int64{0});
	TestEqual(TEXT("S: number defaults zero"), DefaultValue.NumberValue, 0.0);
	TestTrue(TEXT("S: string defaults empty"), DefaultValue.StringValue.IsEmpty());
	TestTrue(TEXT("S: bytes default empty"), DefaultValue.BytesValue.IsEmpty());
	TestTrue(TEXT("S: elements default empty"), DefaultValue.Elements.IsEmpty());
	TestTrue(TEXT("S: object keys default empty"), DefaultValue.ObjectKeys.IsEmpty());

	const FAssetDocumentRuntimeDiagnosticContext DefaultContext;
	TestTrue(TEXT("S: diagnostic-context key defaults empty"), DefaultContext.Key.IsEmpty());
	TestEqual(TEXT("S: diagnostic-context value kind defaults null"), DefaultContext.Value.Kind, EAssetDocumentRuntimeValueKind::Null);
	TestFalse(TEXT("S: diagnostic-context value boolean defaults false"), DefaultContext.Value.BooleanValue);
	TestEqual(TEXT("S: diagnostic-context value integer defaults zero"), DefaultContext.Value.IntegerValue, int64{0});
	TestEqual(TEXT("S: diagnostic-context value number defaults zero"), DefaultContext.Value.NumberValue, 0.0);
	TestTrue(TEXT("S: diagnostic-context value string defaults empty"), DefaultContext.Value.StringValue.IsEmpty());
	TestTrue(TEXT("S: diagnostic-context value bytes defaults empty"), DefaultContext.Value.BytesValue.IsEmpty());
	TestTrue(TEXT("S: diagnostic-context value elements default empty"), DefaultContext.Value.Elements.IsEmpty());
	TestTrue(TEXT("S: diagnostic-context value object keys default empty"), DefaultContext.Value.ObjectKeys.IsEmpty());
	const FAssetDocumentRuntimeDiagnostic DefaultDiagnostic;
	TestTrue(TEXT("S: diagnostic path defaults empty"), DefaultDiagnostic.Path.IsEmpty());
	TestTrue(TEXT("S: diagnostic code defaults empty"), DefaultDiagnostic.Code.IsEmpty());
	TestTrue(TEXT("S: diagnostic message defaults empty"), DefaultDiagnostic.Message.IsEmpty());
	TestEqual(TEXT("S: diagnostic severity defaults error"), DefaultDiagnostic.Severity, EAssetDocumentDiagnosticSeverity::Error);
	TestTrue(TEXT("S: diagnostic context defaults empty"), DefaultDiagnostic.Context.IsEmpty());
	const FAssetDocumentCancellationEvidence DefaultCancellation;
	TestEqual(TEXT("S: cancellation sequence defaults zero"), DefaultCancellation.Sequence, uint64{0});
	TestFalse(TEXT("S: cancellation request defaults false"), DefaultCancellation.bRequested);
	const FAssetDocumentChangedRecord DefaultChangedRecord;
	TestTrue(TEXT("S: changed authority defaults empty"), DefaultChangedRecord.AuthorityId.IsEmpty());
	TestTrue(TEXT("S: changed stable object id defaults empty"), DefaultChangedRecord.StableObjectId.IsEmpty());
	TestTrue(TEXT("S: changed path defaults empty"), DefaultChangedRecord.Path.IsEmpty());
	TestTrue(TEXT("S: changed kind defaults empty"), DefaultChangedRecord.ChangeKind.IsEmpty());

	const FAssetDocumentRuntimeRequestDto DefaultRequest;
	TestEqual(TEXT("S: request ABI defaults v1"), DefaultRequest.AbiVersion, AssetDocumentRuntimeContractVersion);
	TestFalse(TEXT("S: request id defaults invalid"), DefaultRequest.RequestId.IsValid());
	TestEqual(TEXT("S: request operation defaults validate"), DefaultRequest.Operation, EAssetDocumentRuntimeOperation::Validate);
	TestTrue(TEXT("S: request target path defaults empty"), DefaultRequest.TargetAssetPath.IsEmpty());
	TestTrue(TEXT("S: request profile path defaults empty"), DefaultRequest.RequestedProfileClassPath.IsEmpty());
	TestTrue(TEXT("S: request asset-class path defaults empty"), DefaultRequest.RequestedAssetClassPath.IsEmpty());
	TestEqual(TEXT("S: request document kind defaults null"), DefaultRequest.CanonicalDocument.Kind, EAssetDocumentRuntimeValueKind::Null);
	TestFalse(TEXT("S: request document boolean defaults false"), DefaultRequest.CanonicalDocument.BooleanValue);
	TestEqual(TEXT("S: request document integer defaults zero"), DefaultRequest.CanonicalDocument.IntegerValue, int64{0});
	TestEqual(TEXT("S: request document number defaults zero"), DefaultRequest.CanonicalDocument.NumberValue, 0.0);
	TestTrue(TEXT("S: request document string defaults empty"), DefaultRequest.CanonicalDocument.StringValue.IsEmpty());
	TestTrue(TEXT("S: request document bytes defaults empty"), DefaultRequest.CanonicalDocument.BytesValue.IsEmpty());
	TestTrue(TEXT("S: request document elements default empty"), DefaultRequest.CanonicalDocument.Elements.IsEmpty());
	TestTrue(TEXT("S: request document object keys default empty"), DefaultRequest.CanonicalDocument.ObjectKeys.IsEmpty());
	TestEqual(TEXT("S: request deadline defaults zero"), DefaultRequest.DeadlineUnixMilliseconds, int64{0});
	TestEqual(TEXT("S: request cancellation sequence defaults zero"), DefaultRequest.Cancellation.Sequence, uint64{0});
	TestFalse(TEXT("S: request cancellation requested defaults false"), DefaultRequest.Cancellation.bRequested);
	TestEqual(TEXT("S: request revision defaults zero"), DefaultRequest.ExpectedRuntimeRevision, uint64{0});
	const FAssetDocumentRuntimeResultDto DefaultResult;
	TestEqual(TEXT("S: result ABI defaults v1"), DefaultResult.AbiVersion, AssetDocumentRuntimeContractVersion);
	TestFalse(TEXT("S: result request id defaults invalid"), DefaultResult.RequestId.IsValid());
	TestEqual(TEXT("S: result terminal status defaults failed"), DefaultResult.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
	TestEqual(TEXT("S: result final phase defaults failed"), DefaultResult.FinalPhase, EAssetDocumentRuntimeRequestPhase::Failed);
	TestEqual(TEXT("S: result runtime revision defaults zero"), DefaultResult.RuntimeRevision, uint64{0});
	TestTrue(TEXT("S: result diagnostics default empty"), DefaultResult.Diagnostics.IsEmpty());
	TestFalse(TEXT("S: result canonical value defaults unset"), DefaultResult.CanonicalValue.IsSet());
	TestTrue(TEXT("S: result canonical bytes default empty"), DefaultResult.CanonicalBytes.IsEmpty());
	TestTrue(TEXT("S: result semantic hash defaults empty"), DefaultResult.SemanticHash.IsEmpty());
	TestEqual(TEXT("S: result codec version defaults zero"), DefaultResult.CodecVersion, uint32{0});
	TestTrue(TEXT("S: result changed set default empty"), DefaultResult.ChangedSet.IsEmpty());
	const FAssetDocumentRegionEvidence DefaultEvidence;
	TestEqual(TEXT("S: region evidence defaults unavailable"), DefaultEvidence.Kind, EAssetDocumentEvidenceKind::EvidenceUnavailable);
	TestTrue(TEXT("S: region semantic hash defaults empty"), DefaultEvidence.SemanticHash.IsEmpty());
	TestEqual(TEXT("S: region codec version defaults zero"), DefaultEvidence.CodecVersion, uint32{0});
	TestFalse(TEXT("S: region diagnostic defaults unset"), DefaultEvidence.Diagnostic.IsSet());
	const FAssetDocumentContractValidationResult DefaultValidation;
	TestFalse(TEXT("S: validation result defaults invalid"), DefaultValidation.bValid);
	TestTrue(TEXT("S: validation diagnostics default empty"), DefaultValidation.Diagnostics.IsEmpty());

	// P: Every runtime value kind reaches the production validator.
	TestTrue(TEXT("P: null value is valid"), ValidateAssetDocumentRuntimeValue(DefaultValue).bValid);
	FAssetDocumentRuntimeValueDto BooleanValue;
	BooleanValue.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	BooleanValue.BooleanValue = true;
	TestTrue(TEXT("P: boolean value is valid"), ValidateAssetDocumentRuntimeValue(BooleanValue).bValid);
	FAssetDocumentRuntimeValueDto IntegerValue;
	IntegerValue.Kind = EAssetDocumentRuntimeValueKind::Integer;
	IntegerValue.IntegerValue = TNumericLimits<int64>::Lowest();
	TestTrue(TEXT("P: integer value is valid"), ValidateAssetDocumentRuntimeValue(IntegerValue).bValid);
	FAssetDocumentRuntimeValueDto NumberValue;
	NumberValue.Kind = EAssetDocumentRuntimeValueKind::Number;
	NumberValue.NumberValue = -42.5;
	TestTrue(TEXT("P: finite number is valid"), ValidateAssetDocumentRuntimeValue(NumberValue).bValid);
	TestTrue(TEXT("P: unicode string is valid"), ValidateAssetDocumentRuntimeValue(MakeStringValue(TEXT("Asset文档"))).bValid);
	FAssetDocumentRuntimeValueDto BytesValue;
	BytesValue.Kind = EAssetDocumentRuntimeValueKind::Bytes;
	BytesValue.BytesValue = {0x00, 0xff};
	TestTrue(TEXT("P: bytes are valid"), ValidateAssetDocumentRuntimeValue(BytesValue).bValid);
	FAssetDocumentRuntimeValueDto ArrayValue;
	ArrayValue.Kind = EAssetDocumentRuntimeValueKind::Array;
	ArrayValue.Elements = {MakeStringValue(TEXT("entry")), IntegerValue};
	TestTrue(TEXT("P: nested array is valid"), ValidateAssetDocumentRuntimeValue(ArrayValue).bValid);
	TestTrue(TEXT("P: ordered object is valid"), ValidateAssetDocumentRuntimeValue(MakeValidObject()).bValid);

	FAssetDocumentRuntimeValueDto MismatchedObject = MakeValidObject();
	MismatchedObject.ObjectKeys.Pop();
	TestFalse(TEXT("P: unequal object key and element counts are rejected"), ValidateAssetDocumentRuntimeValue(MismatchedObject).bValid);
	FAssetDocumentRuntimeValueDto DuplicateObject = MakeValidObject();
	DuplicateObject.ObjectKeys[1] = DuplicateObject.ObjectKeys[0];
	TestFalse(TEXT("P: duplicate object keys are rejected"), ValidateAssetDocumentRuntimeValue(DuplicateObject).bValid);
	FAssetDocumentRuntimeValueDto InactiveValue;
	InactiveValue.Kind = EAssetDocumentRuntimeValueKind::Null;
	InactiveValue.StringValue = TEXT("inactive");
	TestFalse(TEXT("P: inactive fields carrying values are rejected"), ValidateAssetDocumentRuntimeValue(InactiveValue).bValid);
	FAssetDocumentRuntimeValueDto InvalidString = MakeStringValue(MakeInvalidUnicodeString());
	TestFalse(TEXT("P: invalid unicode strings are rejected"), ValidateAssetDocumentRuntimeValue(InvalidString).bValid);
	FAssetDocumentRuntimeValueDto InvalidKeyObject = MakeValidObject();
	InvalidKeyObject.ObjectKeys[0] = MakeInvalidUnicodeString();
	TestFalse(TEXT("P: invalid unicode object keys are rejected"), ValidateAssetDocumentRuntimeValue(InvalidKeyObject).bValid);
	FAssetDocumentRuntimeValueDto NonFinite;
	NonFinite.Kind = EAssetDocumentRuntimeValueKind::Number;
	NonFinite.NumberValue = std::numeric_limits<double>::infinity();
	TestFalse(TEXT("P: non-finite numbers are rejected"), ValidateAssetDocumentRuntimeValue(NonFinite).bValid);
	FAssetDocumentRuntimeValueDto InvalidNestedArray = ArrayValue;
	InvalidNestedArray.Elements.Add(NonFinite);
	TestFalse(TEXT("P: invalid nested values are rejected"), ValidateAssetDocumentRuntimeValue(InvalidNestedArray).bValid);
	FAssetDocumentRuntimeValueDto InvalidKind;
	InvalidKind.Kind = static_cast<EAssetDocumentRuntimeValueKind>(255);
	TestInvalidContract(
		TEXT("unknown value kind"),
		ValidateAssetDocumentRuntimeValue(InvalidKind),
		TEXT("RuntimeValue.Invalid"),
		TEXT("Runtime value kind, active fields, or nested value is invalid."));
	TestTrue(
		TEXT("P: maximum-depth nested value is accepted"),
		ValidateAssetDocumentRuntimeValue(MakeNestedArray(TestedMaximumRuntimeValueDepth)).bValid);
	TestFalse(
		TEXT("P: over-depth nested value is rejected"),
		ValidateAssetDocumentRuntimeValue(MakeNestedArray(TestedMaximumRuntimeValueDepth + 1)).bValid);
	TestTrue(
		TEXT("P: maximum-node container is accepted"),
		ValidateAssetDocumentRuntimeValue(MakeWideArray(TestedMaximumRuntimeValueNodes - 1)).bValid);
	TestFalse(
		TEXT("P: over-node container is rejected"),
		ValidateAssetDocumentRuntimeValue(MakeWideArray(TestedMaximumRuntimeValueNodes)).bValid);

	FAssetDocumentRuntimeRequestDto InspectRequest = MakeValidRequest(EAssetDocumentRuntimeOperation::Inspect);
	TestTrue(TEXT("P: valid inspect request is accepted"), ValidateAssetDocumentRuntimeRequest(InspectRequest).bValid);
	FAssetDocumentRuntimeRequestDto ApplyRequest = MakeValidRequest(EAssetDocumentRuntimeOperation::Apply);
	TestTrue(TEXT("P: mutating request with pinned revision is accepted"), ValidateAssetDocumentRuntimeRequest(ApplyRequest).bValid);
	FAssetDocumentRuntimeRequestDto InvalidRequest = InspectRequest;
	InvalidRequest.AbiVersion = 99;
	TestInvalidContract(
		TEXT("request ABI mismatch"),
		ValidateAssetDocumentRuntimeRequest(InvalidRequest),
		TEXT("RuntimeRequest.InvalidEnvelope"),
		TEXT("Request identity or path is invalid."));
	InvalidRequest = InspectRequest;
	InvalidRequest.RequestId.Invalidate();
	TestFalse(TEXT("P: invalid request id is rejected"), ValidateAssetDocumentRuntimeRequest(InvalidRequest).bValid);
	InvalidRequest = InspectRequest;
	InvalidRequest.TargetAssetPath = TEXT("Game/NotAnObjectPath");
	TestFalse(TEXT("P: invalid target asset path is rejected"), ValidateAssetDocumentRuntimeRequest(InvalidRequest).bValid);
	InvalidRequest = InspectRequest;
	InvalidRequest.TargetAssetPath = TEXT("/Game/Asset:Bad.Asset");
	TestFalse(
		TEXT("P: Engine rejects the colon object path fixture"),
		FPackageName::IsValidObjectPath(InvalidRequest.TargetAssetPath));
	TestFalse(
		TEXT("P: request validator rejects the Engine-invalid colon object path"),
		ValidateAssetDocumentRuntimeRequest(InvalidRequest).bValid);
	InvalidRequest = InspectRequest;
	InvalidRequest.RequestedProfileClassPath = TEXT("Profile Class");
	TestFalse(TEXT("P: invalid profile class path is rejected"), ValidateAssetDocumentRuntimeRequest(InvalidRequest).bValid);
	InvalidRequest = InspectRequest;
	InvalidRequest.DeadlineUnixMilliseconds = -1;
	TestInvalidContract(
		TEXT("negative request deadline"),
		ValidateAssetDocumentRuntimeRequest(InvalidRequest),
		TEXT("RuntimeRequest.InvalidExecution"),
		TEXT("Request operation, deadline, cancellation, or revision is invalid."));
	InvalidRequest = ApplyRequest;
	InvalidRequest.ExpectedRuntimeRevision = 0;
	TestFalse(TEXT("P: mutating request without revision is rejected"), ValidateAssetDocumentRuntimeRequest(InvalidRequest).bValid);
	InvalidRequest = InspectRequest;
	InvalidRequest.Cancellation.bRequested = true;
	TestFalse(TEXT("P: requested cancellation without a sequence is rejected"), ValidateAssetDocumentRuntimeRequest(InvalidRequest).bValid);
	InvalidRequest = InspectRequest;
	InvalidRequest.CanonicalDocument = NonFinite;
	TestInvalidContract(
		TEXT("invalid canonical request document"),
		ValidateAssetDocumentRuntimeRequest(InvalidRequest),
		TEXT("RuntimeRequest.InvalidDocument"),
		TEXT("Canonical document is invalid."));
	InvalidRequest = InspectRequest;
	InvalidRequest.CanonicalDocument = MakeNestedArray(TestedMaximumRuntimeValueDepth + 1);
	TestFalse(TEXT("P: request document uses the depth budget"), ValidateAssetDocumentRuntimeRequest(InvalidRequest).bValid);

	FAssetDocumentRuntimeResultDto InspectResult = MakeValidResult(InspectRequest);
	TestTrue(TEXT("P: valid inspect result is accepted"), ValidateAssetDocumentRuntimeResult(InspectRequest, InspectResult).bValid);
	FAssetDocumentRuntimeResultDto InvalidResult = InspectResult;
	InvalidResult.AbiVersion = 0;
	TestInvalidContract(
		TEXT("result ABI mismatch"),
		ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult),
		TEXT("RuntimeResult.InvalidIdentity"),
		TEXT("Result ABI, request identity, or runtime revision is invalid."));
	InvalidResult = InspectResult;
	InvalidResult.RequestId = FGuid::NewGuid();
	TestFalse(TEXT("P: result request id mismatch is rejected"), ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult).bValid);
	InvalidResult = InspectResult;
	InvalidResult.FinalPhase = EAssetDocumentRuntimeRequestPhase::Apply;
	TestInvalidContract(
		TEXT("terminal and final phase mismatch"),
		ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult),
		TEXT("RuntimeResult.InvalidTerminal"),
		TEXT("Terminal status, phase, or mutation outcome is invalid."));
	InvalidResult = InspectResult;
	InvalidResult.CanonicalValue = NonFinite;
	TestFalse(TEXT("P: invalid canonical result payload is rejected"), ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult).bValid);
	InvalidResult = InspectResult;
	InvalidResult.SemanticHash.Reset();
	TestInvalidContract(
		TEXT("incomplete byte evidence"),
		ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult),
		TEXT("RuntimeResult.InvalidPayload"),
		TEXT("Result payload or evidence is invalid."));
	InvalidResult = InspectResult;
	InvalidResult.RuntimeRevision = 0;
	TestFalse(TEXT("P: terminal result without a runtime revision is rejected"), ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult).bValid);

	FAssetDocumentRuntimeResultDto StructuredResult = InspectResult;
	FAssetDocumentRuntimeDiagnostic& StructuredDiagnostic = StructuredResult.Diagnostics.AddDefaulted_GetRef();
	StructuredDiagnostic.Path = TEXT("/Body/Value");
	StructuredDiagnostic.Code = TEXT("Runtime.Test");
	StructuredDiagnostic.Message = TEXT("Structured diagnostic");
	StructuredDiagnostic.Severity = EAssetDocumentDiagnosticSeverity::Warning;
	FAssetDocumentRuntimeDiagnosticContext& StructuredContext = StructuredDiagnostic.Context.AddDefaulted_GetRef();
	StructuredContext.Key = TEXT("actual");
	StructuredContext.Value = MakeStringValue(TEXT("value"));
	StructuredResult.ChangedSet.Add({TEXT("asset"), TEXT("object"), TEXT("/Body/Value"), TEXT("modified")});
	TestTrue(TEXT("P: valid diagnostic context and changed record are accepted"), ValidateAssetDocumentRuntimeResult(InspectRequest, StructuredResult).bValid);
	InvalidResult = StructuredResult;
	InvalidResult.Diagnostics[0].Severity = static_cast<EAssetDocumentDiagnosticSeverity>(255);
	TestFalse(TEXT("P: invalid diagnostic severity is rejected"), ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult).bValid);
	InvalidResult = StructuredResult;
	InvalidResult.Diagnostics[0].Context[0].Value = MakeNestedArray(TestedMaximumRuntimeValueDepth + 1);
	TestFalse(TEXT("P: diagnostic context uses the depth budget"), ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult).bValid);
	InvalidResult = InspectResult;
	InvalidResult.CanonicalValue = MakeNestedArray(TestedMaximumRuntimeValueDepth + 1);
	TestFalse(TEXT("P: canonical result value uses the depth budget"), ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult).bValid);
	InvalidResult = StructuredResult;
	InvalidResult.ChangedSet[0].AuthorityId = MakeInvalidUnicodeString();
	TestFalse(TEXT("P: changed authority invalid UTF is rejected"), ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult).bValid);
	InvalidResult = StructuredResult;
	InvalidResult.ChangedSet[0].StableObjectId = MakeInvalidUnicodeString();
	TestFalse(TEXT("P: changed stable object invalid UTF is rejected"), ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult).bValid);
	InvalidResult = StructuredResult;
	InvalidResult.ChangedSet[0].Path = MakeInvalidUnicodeString();
	TestFalse(TEXT("P: changed path invalid UTF is rejected"), ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult).bValid);
	InvalidResult = StructuredResult;
	InvalidResult.ChangedSet[0].ChangeKind = MakeInvalidUnicodeString();
	TestFalse(TEXT("P: changed kind invalid UTF is rejected"), ValidateAssetDocumentRuntimeResult(InspectRequest, InvalidResult).bValid);

	FAssetDocumentRuntimeResultDto FailedApply = MakeValidResult(ApplyRequest);
	FailedApply.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
	FailedApply.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
	FailedApply.CanonicalValue.Reset();
	FailedApply.CanonicalBytes.Reset();
	FailedApply.SemanticHash.Reset();
	FailedApply.CodecVersion = 0;
	TestTrue(TEXT("P: failed apply without partial payload is accepted"), ValidateAssetDocumentRuntimeResult(ApplyRequest, FailedApply).bValid);
	FailedApply.ChangedSet.Add({TEXT("asset"), TEXT("object"), TEXT("/Body"), TEXT("modified")});
	TestFalse(TEXT("P: failed mutation cannot report committed changes"), ValidateAssetDocumentRuntimeResult(ApplyRequest, FailedApply).bValid);

	FAssetDocumentRuntimeResultDto PartialApply = MakeValidResult(ApplyRequest);
	PartialApply.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Partial;
	TestFalse(TEXT("P: mutating Apply plus Partial is rejected"), ValidateAssetDocumentRuntimeResult(ApplyRequest, PartialApply).bValid);
	FAssetDocumentRuntimeResultDto PartialInspect = MakeValidResult(InspectRequest);
	PartialInspect.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Partial;
	TestTrue(TEXT("P: non-mutating Inspect plus Partial is accepted"), ValidateAssetDocumentRuntimeResult(InspectRequest, PartialInspect).bValid);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
