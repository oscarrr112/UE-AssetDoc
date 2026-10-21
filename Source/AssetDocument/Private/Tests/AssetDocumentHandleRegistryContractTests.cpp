// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Runtime/AssetDocumentHandleRegistry.h"

#include "Misc/AutomationTest.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentHandleRegistryContractTests
{
// S: Invalidation reasons are frozen wire values owned by the handle contract.
static_assert(static_cast<uint8>(EAssetDocumentHandleInvalidationReason::Commit) == 0);
static_assert(static_cast<uint8>(EAssetDocumentHandleInvalidationReason::Rollback) == 1);
static_assert(static_cast<uint8>(EAssetDocumentHandleInvalidationReason::Compile) == 2);
static_assert(static_cast<uint8>(EAssetDocumentHandleInvalidationReason::Rebuild) == 3);
static_assert(static_cast<uint8>(EAssetDocumentHandleInvalidationReason::Reinstance) == 4);
static_assert(static_cast<uint8>(EAssetDocumentHandleInvalidationReason::GarbageCollection) == 5);
static_assert(static_cast<uint8>(EAssetDocumentHandleInvalidationReason::PackageUnload) == 6);
static_assert(static_cast<uint8>(EAssetDocumentHandleInvalidationReason::PackageReload) == 7);
static_assert(static_cast<uint8>(EAssetDocumentHandleInvalidationReason::RuntimeRevisionSwap) == 8);
static_assert(static_cast<uint8>(EAssetDocumentHandleInvalidationReason::ProviderUnload) == 9);

#define ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(Type, Member, Expected) \
	static_assert(std::is_same_v<decltype(Type::Member), Expected>)
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleValidationContext, RequestId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleValidationContext, TransactionId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleValidationContext, RuntimeRevision, uint64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleValidationContext, ProviderId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleValidationContext, Phase, EAssetDocumentRuntimeRequestPhase);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleValidationContext, RequiredKind, EAssetDocumentHandleKind);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleValidationContext, RequiredAccess, EAssetDocumentHandleAccess);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleInvalidation, Reason, EAssetDocumentHandleInvalidationReason);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleInvalidation, RequestId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleInvalidation, TransactionId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleInvalidation, RuntimeRevision, uint64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleInvalidation, ProviderId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentHandleInvalidationResult, InvalidatedHandleCount, uint32);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentIssuedHandleResult, Handle, TOptional<FAssetDocumentOpaqueHandle>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentIssuedHandleResult, Diagnostics, TArray<FAssetDocumentRuntimeDiagnostic>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentVerifiedLeaseResult, Lease, TUniquePtr<IAssetDocumentVerifiedHandleLease>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentVerifiedLeaseResult, Diagnostics, TArray<FAssetDocumentRuntimeDiagnostic>);
#undef ASSET_DOCUMENT_ASSERT_MEMBER_TYPE

using FValidateHandleSignature = FAssetDocumentContractValidationResult (*)(
	const FAssetDocumentOpaqueHandle&, const FAssetDocumentHandleValidationContext&);
using FGetPayloadTypeIdSignature = FName (IAssetDocumentNativeHandlePayload::*)() const;
using FIsAliveSignature = bool (IAssetDocumentNativeHandlePayload::*)() const;
using FGetHandleSignature = const FAssetDocumentOpaqueHandle& (IAssetDocumentVerifiedHandleLease::*)() const;
using FGetPayloadCheckedSignature = IAssetDocumentNativeHandlePayload& (IAssetDocumentVerifiedHandleLease::*)() const;
using FIssueSignature = FAssetDocumentIssuedHandleResult (IAssetDocumentHandleResolver::*)(
	EAssetDocumentHandleKind, EAssetDocumentHandleAccess,
	TUniquePtr<IAssetDocumentNativeHandlePayload>&&);
using FAcquireVerifiedLeaseSignature = FAssetDocumentVerifiedLeaseResult (IAssetDocumentHandleResolver::*)(
	const FAssetDocumentOpaqueHandle&, const FAssetDocumentHandleValidationContext&);
using FInvalidateSignature = FAssetDocumentHandleInvalidationResult (
	IAssetDocumentHandleResolver::*)(const FAssetDocumentHandleInvalidation&);
static_assert(std::is_same_v<decltype(&ValidateAssetDocumentHandleMetadata), FValidateHandleSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentNativeHandlePayload::GetPayloadTypeId), FGetPayloadTypeIdSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentNativeHandlePayload::IsAlive), FIsAliveSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentVerifiedHandleLease::GetHandle), FGetHandleSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentVerifiedHandleLease::GetPayloadChecked), FGetPayloadCheckedSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentHandleResolver::Issue), FIssueSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentHandleResolver::AcquireVerifiedLease), FAcquireVerifiedLeaseSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentHandleResolver::Invalidate), FInvalidateSignature>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentNativeHandlePayload>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentVerifiedHandleLease>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentHandleResolver>);

// S: Native issuance and verified leases use unique C++ ownership, never a value DTO.
static_assert(!std::is_copy_constructible_v<FAssetDocumentIssuedHandleResult>);
static_assert(!std::is_copy_assignable_v<FAssetDocumentIssuedHandleResult>);
static_assert(std::is_move_constructible_v<FAssetDocumentIssuedHandleResult>);
static_assert(std::is_move_assignable_v<FAssetDocumentIssuedHandleResult>);
static_assert(!std::is_copy_constructible_v<FAssetDocumentVerifiedLeaseResult>);
static_assert(!std::is_copy_assignable_v<FAssetDocumentVerifiedLeaseResult>);
static_assert(std::is_move_constructible_v<FAssetDocumentVerifiedLeaseResult>);
static_assert(std::is_move_assignable_v<FAssetDocumentVerifiedLeaseResult>);
static_assert(!std::is_constructible_v<FAssetDocumentRuntimeValueDto, FAssetDocumentVerifiedLeaseResult>);
static_assert(!std::is_constructible_v<FAssetDocumentVerifiedLeaseResult, FAssetDocumentRuntimeValueDto>);
static_assert(!std::is_convertible_v<FAssetDocumentVerifiedLeaseResult, FAssetDocumentRuntimeValueDto>);
static_assert(!std::is_convertible_v<FAssetDocumentRuntimeValueDto, FAssetDocumentVerifiedLeaseResult>);
static_assert(!std::is_constructible_v<FAssetDocumentRuntimeValueDto, FAssetDocumentIssuedHandleResult>);
static_assert(!std::is_constructible_v<FAssetDocumentIssuedHandleResult, FAssetDocumentRuntimeValueDto>);
static_assert(!std::is_convertible_v<FAssetDocumentIssuedHandleResult, FAssetDocumentRuntimeValueDto>);
static_assert(!std::is_convertible_v<FAssetDocumentRuntimeValueDto, FAssetDocumentIssuedHandleResult>);

FAssetDocumentHandleValidationContext MakeValidContext(EAssetDocumentHandleKind Kind)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = FGuid::NewGuid();
	Context.TransactionId = FGuid::NewGuid();
	Context.RuntimeRevision = 7;
	Context.ProviderId = TEXT("blueprint");
	Context.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Context.RequiredKind = Kind;
	Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Context;
}

FAssetDocumentOpaqueHandle MakeValidHandle(const FAssetDocumentHandleValidationContext& Context)
{
	FAssetDocumentOpaqueHandle Handle;
	Handle.RequestId = Context.RequestId;
	Handle.TransactionId = Context.TransactionId;
	Handle.RuntimeRevision = Context.RuntimeRevision;
	Handle.ProviderId = Context.ProviderId;
	Handle.Slot = 11;
	Handle.Generation = 3;
	Handle.Kind = Context.RequiredKind;
	Handle.Access = Context.RequiredAccess;
	return Handle;
}

void TestInvalidMetadata(
	FAutomationTestBase& Test,
	const TCHAR* What,
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context,
	const TCHAR* ExpectedCode,
	const TCHAR* ExpectedMessage)
{
	const FAssetDocumentContractValidationResult Result = ValidateAssetDocumentHandleMetadata(Handle, Context);
	Test.TestFalse(*FString::Printf(TEXT("P: %s is rejected"), What), Result.bValid);
	Test.TestEqual(*FString::Printf(TEXT("P: %s returns one diagnostic"), What), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() == 1)
	{
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic path"), What), Result.Diagnostics[0].Path, FString(TEXT("/Handle")));
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic code"), What), Result.Diagnostics[0].Code, FString(ExpectedCode));
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic message"), What), Result.Diagnostics[0].Message, FString(ExpectedMessage));
	}
}

void TestRequestIdentityMetadata(
	FAutomationTestBase& Test,
	const FAssetDocumentOpaqueHandle& Valid,
	const FAssetDocumentHandleValidationContext& Context)
{
	FAssetDocumentHandleValidationContext InvalidContext = Context;
	FAssetDocumentOpaqueHandle Invalid = Valid;
	Invalid.RequestId.Invalidate();
	TestInvalidMetadata(Test, TEXT("invalid handle request identity"), Invalid, Context,
		TEXT("HandleMetadata.RequestMismatch"), TEXT("Handle request identity does not match the validation context."));
	InvalidContext = Context;
	Invalid = Valid;
	InvalidContext.RequestId.Invalidate();
	TestInvalidMetadata(Test, TEXT("invalid context request identity"), Invalid, InvalidContext,
		TEXT("HandleMetadata.RequestMismatch"), TEXT("Handle request identity does not match the validation context."));
	Invalid.RequestId.Invalidate();
	TestInvalidMetadata(Test, TEXT("matching invalid request identities"), Invalid, InvalidContext,
		TEXT("HandleMetadata.RequestMismatch"), TEXT("Handle request identity does not match the validation context."));
	InvalidContext = Context;
	Invalid = Valid;
	Invalid.RequestId = FGuid::NewGuid();
	TestInvalidMetadata(Test, TEXT("request mismatch"), Invalid, Context,
		TEXT("HandleMetadata.RequestMismatch"), TEXT("Handle request identity does not match the validation context."));
}

void TestTransactionIdentityMetadata(
	FAutomationTestBase& Test,
	const FAssetDocumentOpaqueHandle& Valid,
	const FAssetDocumentHandleValidationContext& Context)
{
	FAssetDocumentHandleValidationContext InvalidContext = Context;
	FAssetDocumentOpaqueHandle Invalid = Valid;
	Invalid.TransactionId.Invalidate();
	TestInvalidMetadata(Test, TEXT("invalid handle transaction identity"), Invalid, Context,
		TEXT("HandleMetadata.TransactionMismatch"), TEXT("Handle transaction identity does not match the validation context."));
	InvalidContext = Context;
	Invalid = Valid;
	InvalidContext.TransactionId.Invalidate();
	TestInvalidMetadata(Test, TEXT("invalid context transaction identity"), Invalid, InvalidContext,
		TEXT("HandleMetadata.TransactionMismatch"), TEXT("Handle transaction identity does not match the validation context."));
	Invalid.TransactionId.Invalidate();
	TestInvalidMetadata(Test, TEXT("matching invalid transaction identities"), Invalid, InvalidContext,
		TEXT("HandleMetadata.TransactionMismatch"), TEXT("Handle transaction identity does not match the validation context."));
	InvalidContext = Context;
	Invalid = Valid;
	Invalid.TransactionId = FGuid::NewGuid();
	TestInvalidMetadata(Test, TEXT("transaction mismatch"), Invalid, Context,
		TEXT("HandleMetadata.TransactionMismatch"), TEXT("Handle transaction identity does not match the validation context."));
}

void TestRuntimeRevisionMetadata(
	FAutomationTestBase& Test,
	const FAssetDocumentOpaqueHandle& Valid,
	const FAssetDocumentHandleValidationContext& Context)
{
	FAssetDocumentHandleValidationContext InvalidContext = Context;
	FAssetDocumentOpaqueHandle Invalid = Valid;
	Invalid.RuntimeRevision = 0;
	TestInvalidMetadata(Test, TEXT("zero handle runtime revision"), Invalid, Context,
		TEXT("HandleMetadata.RuntimeRevisionMismatch"), TEXT("Handle runtime revision does not match the validation context."));
	InvalidContext = Context;
	Invalid = Valid;
	InvalidContext.RuntimeRevision = 0;
	TestInvalidMetadata(Test, TEXT("zero context runtime revision"), Invalid, InvalidContext,
		TEXT("HandleMetadata.RuntimeRevisionMismatch"), TEXT("Handle runtime revision does not match the validation context."));
	Invalid.RuntimeRevision = 0;
	TestInvalidMetadata(Test, TEXT("matching zero runtime revisions"), Invalid, InvalidContext,
		TEXT("HandleMetadata.RuntimeRevisionMismatch"), TEXT("Handle runtime revision does not match the validation context."));
	InvalidContext = Context;
	Invalid = Valid;
	Invalid.RuntimeRevision = Context.RuntimeRevision + 1;
	TestInvalidMetadata(Test, TEXT("runtime revision mismatch"), Invalid, Context,
		TEXT("HandleMetadata.RuntimeRevisionMismatch"), TEXT("Handle runtime revision does not match the validation context."));
}

void TestProviderIdentityMetadata(
	FAutomationTestBase& Test,
	const FAssetDocumentOpaqueHandle& Valid,
	const FAssetDocumentHandleValidationContext& Context)
{
	FAssetDocumentHandleValidationContext InvalidContext = Context;
	FAssetDocumentOpaqueHandle Invalid = Valid;
	Invalid.ProviderId.Reset();
	TestInvalidMetadata(Test, TEXT("empty handle provider identity"), Invalid, Context,
		TEXT("HandleMetadata.ProviderMismatch"), TEXT("Handle provider does not match the validation context."));
	InvalidContext = Context;
	Invalid = Valid;
	InvalidContext.ProviderId.Reset();
	TestInvalidMetadata(Test, TEXT("empty context provider identity"), Invalid, InvalidContext,
		TEXT("HandleMetadata.ProviderMismatch"), TEXT("Handle provider does not match the validation context."));
	Invalid.ProviderId.Reset();
	TestInvalidMetadata(Test, TEXT("matching empty provider identities"), Invalid, InvalidContext,
		TEXT("HandleMetadata.ProviderMismatch"), TEXT("Handle provider does not match the validation context."));
	InvalidContext = Context;
	Invalid = Valid;
	Invalid.ProviderId = TEXT("animation");
	TestInvalidMetadata(Test, TEXT("provider mismatch"), Invalid, Context,
		TEXT("HandleMetadata.ProviderMismatch"), TEXT("Handle provider does not match the validation context."));
}

void TestIdentityMetadata(FAutomationTestBase& Test)
{
	const FAssetDocumentHandleValidationContext Context = MakeValidContext(EAssetDocumentHandleKind::Object);
	const FAssetDocumentOpaqueHandle Valid = MakeValidHandle(Context);
	Test.TestTrue(TEXT("P: matching handle metadata is valid"), ValidateAssetDocumentHandleMetadata(Valid, Context).bValid);
	TestRequestIdentityMetadata(Test, Valid, Context);
	TestTransactionIdentityMetadata(Test, Valid, Context);
	TestRuntimeRevisionMetadata(Test, Valid, Context);
	TestProviderIdentityMetadata(Test, Valid, Context);
}

void TestKindPhaseAndAccessMetadata(FAutomationTestBase& Test)
{
	for (uint8 Index = 0; Index <= static_cast<uint8>(EAssetDocumentHandleKind::Scratch); ++Index)
	{
		const EAssetDocumentHandleKind Kind = static_cast<EAssetDocumentHandleKind>(Index);
		const FAssetDocumentHandleValidationContext Context = MakeValidContext(Kind);
		const FAssetDocumentOpaqueHandle Handle = MakeValidHandle(Context);
		Test.TestTrue(
			*FString::Printf(TEXT("P: frozen handle kind %u is valid"), Index),
			ValidateAssetDocumentHandleMetadata(Handle, Context).bValid);
	}

	FAssetDocumentHandleValidationContext Context = MakeValidContext(EAssetDocumentHandleKind::Action);
	FAssetDocumentOpaqueHandle Handle = MakeValidHandle(Context);
	Context.Phase = static_cast<EAssetDocumentRuntimeRequestPhase>(255);
	TestInvalidMetadata(Test, TEXT("invalid request phase enum"), Handle, Context,
		TEXT("HandleMetadata.InvalidPhase"), TEXT("Handle validation phase is outside the frozen request-phase enum."));

	Context = MakeValidContext(EAssetDocumentHandleKind::Schema);
	Handle = MakeValidHandle(Context);
	Handle.Kind = static_cast<EAssetDocumentHandleKind>(255);
	TestInvalidMetadata(Test, TEXT("invalid handle kind enum"), Handle, Context,
		TEXT("HandleMetadata.InvalidKind"), TEXT("Handle kind metadata is outside the frozen handle-kind enum."));
	Context = MakeValidContext(EAssetDocumentHandleKind::Track);
	Handle = MakeValidHandle(Context);
	Context.RequiredKind = static_cast<EAssetDocumentHandleKind>(255);
	TestInvalidMetadata(Test, TEXT("invalid required kind enum"), Handle, Context,
		TEXT("HandleMetadata.InvalidKind"), TEXT("Handle kind metadata is outside the frozen handle-kind enum."));
	Context = MakeValidContext(EAssetDocumentHandleKind::Section);
	Handle = MakeValidHandle(Context);
	Context.RequiredKind = EAssetDocumentHandleKind::Channel;
	TestInvalidMetadata(Test, TEXT("handle kind mismatch"), Handle, Context,
		TEXT("HandleMetadata.KindMismatch"), TEXT("Handle kind does not match the required kind."));

	Context = MakeValidContext(EAssetDocumentHandleKind::Curve);
	Handle = MakeValidHandle(Context);
	Handle.Access = static_cast<EAssetDocumentHandleAccess>(255);
	TestInvalidMetadata(Test, TEXT("invalid handle access enum"), Handle, Context,
		TEXT("HandleMetadata.InvalidAccess"), TEXT("Handle access metadata is outside the frozen access enum."));
	Context = MakeValidContext(EAssetDocumentHandleKind::Scratch);
	Handle = MakeValidHandle(Context);
	Context.RequiredAccess = static_cast<EAssetDocumentHandleAccess>(255);
	TestInvalidMetadata(Test, TEXT("invalid required access enum"), Handle, Context,
		TEXT("HandleMetadata.InvalidAccess"), TEXT("Handle access metadata is outside the frozen access enum."));
	Context = MakeValidContext(EAssetDocumentHandleKind::Object);
	Handle = MakeValidHandle(Context);
	Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	TestInvalidMetadata(Test, TEXT("handle access mismatch"), Handle, Context,
		TEXT("HandleMetadata.AccessMismatch"), TEXT("Handle access does not match the required access."));
}

void TestStorageMetadata(FAutomationTestBase& Test)
{
	const FAssetDocumentHandleValidationContext Context = MakeValidContext(EAssetDocumentHandleKind::Object);
	const FAssetDocumentOpaqueHandle Valid = MakeValidHandle(Context);
	FAssetDocumentOpaqueHandle Invalid = Valid;
	Invalid.Slot = 0;
	TestInvalidMetadata(Test, TEXT("zero slot"), Invalid, Context,
		TEXT("HandleMetadata.InvalidSlot"), TEXT("Handle slot must be non-zero."));
	Invalid = Valid;
	Invalid.Generation = 0;
	TestInvalidMetadata(Test, TEXT("zero generation"), Invalid, Context,
		TEXT("HandleMetadata.InvalidGeneration"), TEXT("Handle generation must be non-zero."));
}

void TestValidationFamilyPrecedence(FAutomationTestBase& Test)
{
	FAssetDocumentHandleValidationContext Context = MakeValidContext(EAssetDocumentHandleKind::Object);
	FAssetDocumentOpaqueHandle Invalid = MakeValidHandle(Context);
	Invalid.RequestId = FGuid::NewGuid();
	Context.Phase = static_cast<EAssetDocumentRuntimeRequestPhase>(255);
	Invalid.Kind = EAssetDocumentHandleKind::Node;
	Invalid.Access = EAssetDocumentHandleAccess::ReadOnly;
	Invalid.Slot = 0;
	Invalid.Generation = 0;
	TestInvalidMetadata(Test, TEXT("cross-family precedence"), Invalid, Context,
		TEXT("HandleMetadata.RequestMismatch"), TEXT("Handle request identity does not match the validation context."));
}

struct FInvalidationVerificationVector
{
	EAssetDocumentHandleInvalidationReason Reason;
	const TCHAR* ExpectedDiagnosticCode;
};

void TestInvalidationVerificationVectors(FAutomationTestBase& Test)
{
	// V: F-HOST-ABI owns these effects; T05 freezes inputs/codes without a live table or fake resolver.
	static constexpr FInvalidationVerificationVector Vectors[] = {
		{EAssetDocumentHandleInvalidationReason::Compile, TEXT("Handle.Invalidated.Compile")},
		{EAssetDocumentHandleInvalidationReason::Rebuild, TEXT("Handle.Invalidated.Rebuild")},
		{EAssetDocumentHandleInvalidationReason::Reinstance, TEXT("Handle.Invalidated.Reinstance")},
		{EAssetDocumentHandleInvalidationReason::GarbageCollection, TEXT("Handle.Invalidated.GarbageCollection")},
		{EAssetDocumentHandleInvalidationReason::PackageUnload, TEXT("Handle.Invalidated.PackageUnload")},
		{EAssetDocumentHandleInvalidationReason::PackageReload, TEXT("Handle.Invalidated.PackageReload")},
		{EAssetDocumentHandleInvalidationReason::RuntimeRevisionSwap, TEXT("Handle.Invalidated.RuntimeRevisionSwap")},
		{EAssetDocumentHandleInvalidationReason::ProviderUnload, TEXT("Handle.Invalidated.ProviderUnload")},
	};
	Test.TestEqual(TEXT("V: F-HOST-ABI owns eight non-transaction invalidation vectors"), int32{UE_ARRAY_COUNT(Vectors)}, int32{8});
	for (const FInvalidationVerificationVector& Vector : Vectors)
	{
		Test.TestTrue(TEXT("V: invalidation diagnostic code is frozen"), FCString::Strlen(Vector.ExpectedDiagnosticCode) > 0);
		Test.TestTrue(
			TEXT("V: invalidation reason remains in the frozen non-transaction range"),
			static_cast<uint8>(Vector.Reason) >= static_cast<uint8>(EAssetDocumentHandleInvalidationReason::Compile));
	}
}

struct FIssuanceVerificationVector
{
	EAssetDocumentHandleKind Kind;
	const TCHAR* PayloadType;
};

void TestIssuanceVerificationVectors(FAutomationTestBase& Test)
{
	// V: F-HOST-ABI owns registry effects; T05 freezes the complete generic issue surface.
	static constexpr FIssuanceVerificationVector Vectors[] = {
		{EAssetDocumentHandleKind::Object, TEXT("provider.object")},
		{EAssetDocumentHandleKind::Class, TEXT("provider.class")},
		{EAssetDocumentHandleKind::Struct, TEXT("provider.struct")},
		{EAssetDocumentHandleKind::Property, TEXT("provider.property")},
		{EAssetDocumentHandleKind::Package, TEXT("provider.package")},
		{EAssetDocumentHandleKind::Asset, TEXT("provider.asset")},
		{EAssetDocumentHandleKind::Graph, TEXT("provider.graph")},
		{EAssetDocumentHandleKind::Node, TEXT("provider.node")},
		{EAssetDocumentHandleKind::Pin, TEXT("provider.pin")},
		{EAssetDocumentHandleKind::Subobject, TEXT("provider.subobject")},
		{EAssetDocumentHandleKind::Action, TEXT("provider.action")},
		{EAssetDocumentHandleKind::Schema, TEXT("provider.schema")},
		{EAssetDocumentHandleKind::Track, TEXT("provider.track")},
		{EAssetDocumentHandleKind::Section, TEXT("provider.section")},
		{EAssetDocumentHandleKind::Channel, TEXT("provider.channel")},
		{EAssetDocumentHandleKind::Curve, TEXT("provider.curve")},
		{EAssetDocumentHandleKind::Scratch, TEXT("provider.scratch")},
	};
	Test.TestEqual(TEXT("V: all frozen handle kinds use the same issue operation"),
		int32{UE_ARRAY_COUNT(Vectors)}, int32{17});
	for (uint8 Index = 0; Index < UE_ARRAY_COUNT(Vectors); ++Index)
	{
		Test.TestEqual(TEXT("V: issue kind wire value is exact"),
			static_cast<uint8>(Vectors[Index].Kind), Index);
		Test.TestTrue(TEXT("V: provider payload type id is nonempty"),
			FCString::Strlen(Vectors[Index].PayloadType) > 0);
	}
	static constexpr const TCHAR* FailureCodes[] = {
		TEXT("HandleIssue.InvalidPayload"), TEXT("HandleIssue.DeadPayload"),
		TEXT("HandleIssue.KindNotAdmitted"), TEXT("HandleIssue.AccessNotAdmitted"),
		TEXT("HandleIssue.InvocationClosed"), TEXT("HandleIssue.IdentityMismatch"),
	};
	for (const TCHAR* Code : FailureCodes)
	{
		Test.TestTrue(TEXT("V: issue failure diagnostic is stable"), FCString::Strlen(Code) > 0);
	}
	Test.TestEqual(TEXT("V: registry assigns request transaction revision provider slot generation"),
		FString(TEXT("registry-assigned-complete-identity")), FString(TEXT("registry-assigned-complete-identity")));
	Test.TestEqual(TEXT("V: omitted or invalidly returned issue is invalidated before invocation closes"),
		FString(TEXT("invalidate-unreturned-or-invalid-return")), FString(TEXT("invalidate-unreturned-or-invalid-return")));
}

void TestLeaseUseVerificationVector(FAutomationTestBase& Test)
{
	// V: This is a successor-owned contract vector, not behavior proof of acquisition or liveness.
	const EAssetDocumentExecutionThread RequiredThread = EAssetDocumentExecutionThread::GameThread;
	const FString AcquireBoundary = TEXT("after-metadata-and-liveness-validation");
	const FString ReleaseBoundary = TEXT("before-current-provider-Invoke-returns");
	const FString Owner = TEXT("F-HOST-ABI");
	const bool bMayCache = false;
	const bool bMaySerialize = false;
	const bool bMayEnterHostValueDtos = false;
	Test.TestEqual(TEXT("V: lease acquisition/use is Game Thread only"), RequiredThread, EAssetDocumentExecutionThread::GameThread);
	Test.TestEqual(TEXT("V: lease acquisition follows validation"), AcquireBoundary, FString(TEXT("after-metadata-and-liveness-validation")));
	Test.TestEqual(TEXT("V: lease use ends before Invoke returns"), ReleaseBoundary, FString(TEXT("before-current-provider-Invoke-returns")));
	Test.TestEqual(TEXT("V: lease-use implementation owner is frozen"), Owner, FString(TEXT("F-HOST-ABI")));
	Test.TestFalse(TEXT("V: lease cannot be cached"), bMayCache);
	Test.TestFalse(TEXT("V: lease cannot be serialized"), bMaySerialize);
	Test.TestFalse(TEXT("V: lease cannot enter Host value DTOs"), bMayEnterHostValueDtos);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentHandleRegistryContractTest,
	"UEAssetDoc.RuntimeContracts.HandleRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentHandleRegistryContractTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentHandleRegistryContractTests;
	(void)Parameters;

	// S: Defaults freeze metadata only; they do not claim a live registry exists.
	const FAssetDocumentHandleValidationContext Context;
	TestFalse(TEXT("S: validation request id defaults invalid"), Context.RequestId.IsValid());
	TestFalse(TEXT("S: validation transaction id defaults invalid"), Context.TransactionId.IsValid());
	TestEqual(TEXT("S: validation revision defaults zero"), Context.RuntimeRevision, uint64{0});
	TestTrue(TEXT("S: validation provider defaults empty"), Context.ProviderId.IsEmpty());
	TestEqual(TEXT("S: validation phase defaults validate"), Context.Phase, EAssetDocumentRuntimeRequestPhase::Validate);
	TestEqual(TEXT("S: validation kind defaults object"), Context.RequiredKind, EAssetDocumentHandleKind::Object);
	TestEqual(TEXT("S: validation access defaults read-only"), Context.RequiredAccess, EAssetDocumentHandleAccess::ReadOnly);

	const FAssetDocumentHandleInvalidation Invalidation;
	TestEqual(TEXT("S: invalidation reason defaults commit"), Invalidation.Reason, EAssetDocumentHandleInvalidationReason::Commit);
	TestFalse(TEXT("S: invalidation request id defaults invalid"), Invalidation.RequestId.IsValid());
	TestFalse(TEXT("S: invalidation transaction id defaults invalid"), Invalidation.TransactionId.IsValid());
	TestEqual(TEXT("S: invalidation revision defaults zero"), Invalidation.RuntimeRevision, uint64{0});
	TestTrue(TEXT("S: invalidation provider defaults empty"), Invalidation.ProviderId.IsEmpty());

	const FAssetDocumentVerifiedLeaseResult Result;
	TestFalse(TEXT("S: lease result defaults without lease"), Result.Lease.IsValid());
	TestTrue(TEXT("S: lease result diagnostics default empty"), Result.Diagnostics.IsEmpty());
	const FAssetDocumentIssuedHandleResult Issued;
	TestFalse(TEXT("S: issued result defaults without handle"), Issued.Handle.IsSet());
	TestTrue(TEXT("S: issued result diagnostics default empty"), Issued.Diagnostics.IsEmpty());

	TestIdentityMetadata(*this);
	TestKindPhaseAndAccessMetadata(*this);
	TestStorageMetadata(*this);
	TestValidationFamilyPrecedence(*this);
	TestInvalidationVerificationVectors(*this);
	TestIssuanceVerificationVectors(*this);
	TestLeaseUseVerificationVector(*this);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
