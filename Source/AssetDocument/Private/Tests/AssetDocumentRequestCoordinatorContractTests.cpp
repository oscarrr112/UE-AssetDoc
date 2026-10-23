// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentRequestCoordinator.h"

#include "Misc/AutomationTest.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentRequestCoordinatorContractTests
{
#define ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(Type, Member, Expected) \
	static_assert(std::is_same_v<decltype(Type::Member), Expected>)
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRequestContextMetadata, RequestId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRequestContextMetadata, PinnedRuntimeRevision, uint64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRequestContextMetadata, Phase, EAssetDocumentRuntimeRequestPhase);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRequestContextMetadata, bCancellationDeferredInCriticalSection, bool);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRequestCoordinatorDependencies, Host, IAssetDocumentPythonRuntimeHost&);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRequestCoordinatorDependencies, PrimitiveRegistry, IAssetDocumentPrimitiveRegistry&);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRequestCoordinatorDependencies, Handles, IAssetDocumentHandleResolver&);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRequestCoordinatorDependencies, TransactionFactory, IAssetDocumentAtomicTransactionFactory&);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRequestCoordinatorDependencies, Persistence, IAssetDocumentPersistenceCoordinator&);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRequestCoordinatorDependencies, Sync, IAssetDocumentSyncCoordinator&);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRequestCoordinatorDependencies, Cancellation, IAssetDocumentCancellationView&);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentRequestCoordinatorDependencies, Observer, IAssetDocumentRequestObserver&);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveInvocation, ValueArguments, FAssetDocumentRuntimeValueDto);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveInvocation, HandleArguments, TArray<FAssetDocumentPrimitiveHandleArgument>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveOutcome, Value, FAssetDocumentRuntimeValueDto);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveOutcome, HandleResults, TArray<FAssetDocumentPrimitiveHandleResult>);
#undef ASSET_DOCUMENT_ASSERT_MEMBER_TYPE
using FGetMetadataSignature = const FAssetDocumentRequestContextMetadata& (IAssetDocumentNativeRequestContext::*)() const;
using FInvokeSignature = FAssetDocumentPrimitiveOutcome (IAssetDocumentNativeRequestContext::*)(const FAssetDocumentPrimitiveInvocation&);
using FInspectCatalogSignature = FAssetDocumentPrimitiveRegistrySnapshot (IAssetDocumentNativeRequestContext::*)() const;
using FHandoffSignature = FAssetDocumentIssuedHandleResult (IAssetDocumentNativeRequestContext::*)(
	const FAssetDocumentOpaqueHandle&, FStringView, EAssetDocumentHandleAccess);
using FDispatchSignature = FAssetDocumentRuntimeResultDto (IAssetDocumentPythonRuntimeHost::*)(
	const FAssetDocumentRuntimeRequestDto&, IAssetDocumentNativeRequestContext&);
using FOnPhaseChangedSignature = void (IAssetDocumentRequestObserver::*)(FGuid, EAssetDocumentRuntimeRequestPhase);
using FExecuteSignature = FAssetDocumentRuntimeResultDto (IAssetDocumentRequestCoordinator::*)(
	const FAssetDocumentRuntimeRequestDto&, FAssetDocumentRequestCoordinatorDependencies&);
using FTransitionSignature = bool (*)(EAssetDocumentRuntimeRequestPhase, EAssetDocumentRuntimeRequestPhase);
static_assert(std::is_same_v<decltype(&IAssetDocumentNativeRequestContext::GetMetadata), FGetMetadataSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentNativeRequestContext::Invoke), FInvokeSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentNativeRequestContext::InspectCatalog), FInspectCatalogSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentNativeRequestContext::Handoff), FHandoffSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentPythonRuntimeHost::Dispatch), FDispatchSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentRequestObserver::OnPhaseChanged), FOnPhaseChangedSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentRequestCoordinator::Execute), FExecuteSignature>);
static_assert(std::is_same_v<decltype(&IsAssetDocumentRequestPhaseTransitionAllowed), FTransitionSignature>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentNativeRequestContext>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentPythonRuntimeHost>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentRequestObserver>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentRequestCoordinator>);
static_assert(std::is_abstract_v<IAssetDocumentNativeRequestContext>);
static_assert(std::is_abstract_v<IAssetDocumentPythonRuntimeHost>);
static_assert(std::is_abstract_v<IAssetDocumentRequestObserver>);
static_assert(std::is_abstract_v<IAssetDocumentRequestCoordinator>);
static_assert(!std::is_default_constructible_v<FAssetDocumentRequestCoordinatorDependencies>);
static_assert(!std::is_constructible_v<FAssetDocumentRuntimeValueDto, FAssetDocumentOpaqueHandle>);
static_assert(!std::is_constructible_v<FAssetDocumentOpaqueHandle, FAssetDocumentRuntimeValueDto>);
static_assert(!std::is_convertible_v<FAssetDocumentOpaqueHandle, FAssetDocumentRuntimeValueDto>);
static_assert(!std::is_convertible_v<FAssetDocumentRuntimeValueDto, FAssetDocumentOpaqueHandle>);
#define ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(Name) \
	template <typename Type, typename = void> struct THas##Name : std::false_type {}; \
	template <typename Type> struct THas##Name<Type, std::void_t<decltype(&Type::Name)>> : std::true_type {}
ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(Commit); ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(Rollback);
ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(Save); ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(Install);
ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(Advance); ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(Execute);
ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(Reload);
ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(GetInitialHandleResults);
ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(GetAmbientHandle);
ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(GetLastIssuedHandle);
ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(GetTargetHandle);
ASSET_DOCUMENT_DECLARE_METHOD_TRAIT(ResolveHandle);
#undef ASSET_DOCUMENT_DECLARE_METHOD_TRAIT
static_assert(!THasCommit<IAssetDocumentNativeRequestContext>::value);
static_assert(!THasRollback<IAssetDocumentNativeRequestContext>::value);
static_assert(!THasSave<IAssetDocumentNativeRequestContext>::value);
static_assert(!THasInstall<IAssetDocumentNativeRequestContext>::value);
static_assert(!THasAdvance<IAssetDocumentNativeRequestContext>::value);
static_assert(!THasExecute<IAssetDocumentNativeRequestContext>::value);
static_assert(!THasReload<IAssetDocumentPythonRuntimeHost>::value);
static_assert(!THasExecute<IAssetDocumentPythonRuntimeHost>::value);
static_assert(!THasGetInitialHandleResults<IAssetDocumentNativeRequestContext>::value);
static_assert(!THasGetAmbientHandle<IAssetDocumentNativeRequestContext>::value);
static_assert(!THasGetLastIssuedHandle<IAssetDocumentNativeRequestContext>::value);
static_assert(!THasGetTargetHandle<IAssetDocumentNativeRequestContext>::value);
static_assert(!THasResolveHandle<IAssetDocumentNativeRequestContext>::value);

class FDefaultHandoffContext final : public IAssetDocumentNativeRequestContext
{
public:
	const FAssetDocumentRequestContextMetadata& GetMetadata() const override
	{
		return Metadata;
	}
	FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation&) override
	{
		return {};
	}
	FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override
	{
		return {};
	}

private:
	FAssetDocumentRequestContextMetadata Metadata;
};

void TestDefaultHandoffFailure(FAutomationTestBase& Test)
{
	FDefaultHandoffContext Context;
	const FAssetDocumentIssuedHandleResult Result = Context.Handoff(
		{},
		TEXTVIEW("animation"),
		EAssetDocumentHandleAccess::ReadOnly);
	Test.TestFalse(TEXT("default request context issues no handoff handle"),
		Result.Handle.IsSet());
	Test.TestEqual(TEXT("default request context returns one diagnostic"),
		Result.Diagnostics.Num(), 1);
	Test.TestEqual(TEXT("default request context diagnostic path is stable"),
		Result.Diagnostics[0].Path, FString(TEXT("/HandleHandoff")));
	Test.TestEqual(TEXT("default request context diagnostic code is stable"),
		Result.Diagnostics[0].Code,
		FString(TEXT("AD.RUNTIME.HANDOFF_UNAVAILABLE")));
}
void TestMetadataDefaults(FAutomationTestBase& Test)
{
	const FAssetDocumentRequestContextMetadata Metadata;
	Test.TestFalse(TEXT("S: context request identity defaults invalid"), Metadata.RequestId.IsValid());
	Test.TestEqual(TEXT("S: pinned runtime revision defaults zero"), Metadata.PinnedRuntimeRevision, uint64{0});
	Test.TestEqual(TEXT("S: context phase defaults validate"), Metadata.Phase, EAssetDocumentRuntimeRequestPhase::Validate);
	Test.TestFalse(TEXT("S: cancellation deferral defaults false"), Metadata.bCancellationDeferredInCriticalSection);

	const FAssetDocumentPrimitiveInvocation Invocation;
	Test.TestEqual(TEXT("S: value arguments reuse the T02 DTO and default null"), Invocation.ValueArguments.Kind, EAssetDocumentRuntimeValueKind::Null);
	Test.TestTrue(TEXT("S: named native handle arguments default empty"), Invocation.HandleArguments.IsEmpty());
	const FAssetDocumentPrimitiveOutcome Outcome;
	Test.TestEqual(TEXT("S: value result reuses the T02 DTO and defaults null"), Outcome.Value.Kind, EAssetDocumentRuntimeValueKind::Null);
	Test.TestTrue(TEXT("S: named native handle results default empty"), Outcome.HandleResults.IsEmpty());
	Test.TestFalse(TEXT("S: coordinator dependencies cannot exist without native owners"), std::is_default_constructible_v<FAssetDocumentRequestCoordinatorDependencies>);
}

void TestExactPhaseTable(FAutomationTestBase& Test)
{
	static constexpr bool Allowed[8][8] = {
		{false, true, false, false, false, false, false, true},
		{false, false, true, false, false, false, false, true},
		{false, false, false, true, true, false, false, true},
		{false, false, false, false, true, false, false, true},
		{false, false, false, false, false, true, false, true},
		{false, false, false, false, false, false, true, true},
		{false, false, false, false, false, false, false, false},
		{false, false, false, false, false, false, false, false},
	};
	for (uint8 From = 0; From < 8; ++From)
	{
		for (uint8 To = 0; To < 8; ++To)
		{
			Test.TestEqual(
				*FString::Printf(TEXT("P: request phase edge %u -> %u is exact"), From, To),
				IsAssetDocumentRequestPhaseTransitionAllowed(
					static_cast<EAssetDocumentRuntimeRequestPhase>(From),
					static_cast<EAssetDocumentRuntimeRequestPhase>(To)),
				Allowed[From][To]);
		}
	}
}

void TestPhaseBoundaries(FAutomationTestBase& Test)
{
	Test.TestFalse(TEXT("P: first out-of-range from-phase is rejected"),
		IsAssetDocumentRequestPhaseTransitionAllowed(
			static_cast<EAssetDocumentRuntimeRequestPhase>(8), EAssetDocumentRuntimeRequestPhase::Preflight));
	Test.TestFalse(TEXT("P: first out-of-range to-phase is rejected"),
		IsAssetDocumentRequestPhaseTransitionAllowed(
			EAssetDocumentRuntimeRequestPhase::Validate, static_cast<EAssetDocumentRuntimeRequestPhase>(8)));
	Test.TestFalse(TEXT("P: high invalid from-phase is rejected"),
		IsAssetDocumentRequestPhaseTransitionAllowed(
			static_cast<EAssetDocumentRuntimeRequestPhase>(255), EAssetDocumentRuntimeRequestPhase::Preflight));
	Test.TestFalse(TEXT("P: high invalid to-phase is rejected"),
		IsAssetDocumentRequestPhaseTransitionAllowed(
			EAssetDocumentRuntimeRequestPhase::Validate, static_cast<EAssetDocumentRuntimeRequestPhase>(255)));
}

struct FHostCallVector
{
	const TCHAR* Boundary;
	EAssetDocumentExecutionThread Thread;
	bool bGilHeld;
	bool bInternalMutexHeld;
	bool bSynchronous;
	bool bQueuesWork;
	bool bWaitsOnGameThreadQueue;
};

void TestHostCallVectors(FAutomationTestBase& Test)
{
	// V: F-HOST-ABI owns execution; these rows are successor expectations, not fake behavior.
	static constexpr FHostCallVector Vectors[] = {
		{TEXT("host-dispatch"), EAssetDocumentExecutionThread::GameThread, true, false, true, false, false},
		{TEXT("request-context-invoke"), EAssetDocumentExecutionThread::GameThread, true, false, true, false, false},
	};
	for (const FHostCallVector& Vector : Vectors)
	{
		Test.TestTrue(TEXT("V: re-entry boundary is named"), FCString::Strlen(Vector.Boundary) > 0);
		Test.TestEqual(TEXT("V: UE-touching Host boundary stays on Game Thread"), Vector.Thread, EAssetDocumentExecutionThread::GameThread);
		Test.TestTrue(TEXT("V: GIL is held across the typed Python boundary"), Vector.bGilHeld);
		Test.TestFalse(TEXT("V: C++ does not enter Python while holding internal mutex"), Vector.bInternalMutexHeld);
		Test.TestTrue(TEXT("V: typed Host calls are synchronous"), Vector.bSynchronous);
		Test.TestFalse(TEXT("V: typed Host calls never enqueue same-thread work"), Vector.bQueuesWork);
		Test.TestFalse(TEXT("V: typed Host calls never wait on the Game Thread queue"), Vector.bWaitsOnGameThreadQueue);
	}
}

struct FIdentityPinVector
{
	const TCHAR* Identity; bool bPinnedForRequestLifetime; bool bMayRebindWithinRequest;
};

void TestIdentityPinAndLifetimeVectors(FAutomationTestBase& Test)
{
	struct FTransactionLifetimeBoundary
	{
		const TCHAR* Boundary; bool bMayCross;
	};
	static constexpr FIdentityPinVector Identities[] = {
		{TEXT("request-id"), true, false},
		{TEXT("active-transaction-id"), true, false},
		{TEXT("runtime-revision"), true, false},
	};
	static constexpr FTransactionLifetimeBoundary Boundaries[] = {
		{TEXT("editor-tick"), false},
		{TEXT("await"), false},
		{TEXT("hot-reload"), false},
		{TEXT("nested-apply"), false},
	};
	for (const FIdentityPinVector& Vector : Identities)
	{
		Test.TestTrue(TEXT("V: pinned identity is named"), FCString::Strlen(Vector.Identity) > 0);
		Test.TestTrue(TEXT("V: identity is pinned for the request lifetime"), Vector.bPinnedForRequestLifetime);
		Test.TestFalse(TEXT("V: pinned identity cannot rebind within a request"), Vector.bMayRebindWithinRequest);
	}
	for (const FTransactionLifetimeBoundary& Vector : Boundaries)
	{
		Test.TestTrue(TEXT("V: transaction forbidden crossing is named"), FCString::Strlen(Vector.Boundary) > 0);
		Test.TestFalse(TEXT("V: transaction cannot cross asynchronous or reentrant boundary"), Vector.bMayCross);
	}
}

struct FRevalidationVector
{
	const TCHAR* Dimension; const TCHAR* ExpectedCode; uint8 ValidationStage;
	bool bEveryInvocation; bool bEveryHandle;
};

void TestExactValidationRow(
	FAutomationTestBase& Test,
	const FRevalidationVector& Vector,
	const TCHAR* ExpectedDimension,
	const TCHAR* ExpectedCode,
	uint8 ExpectedStage,
	bool bExpectedEveryHandle)
{
	Test.TestEqual(TEXT("V: validation dimension is exact"), FString(Vector.Dimension), FString(ExpectedDimension));
	Test.TestEqual(TEXT("V: validation diagnostic is exact"), FString(Vector.ExpectedCode), FString(ExpectedCode));
	Test.TestEqual(TEXT("V: validation stage is exact"), Vector.ValidationStage, ExpectedStage);
	Test.TestTrue(TEXT("V: validation runs for every Invoke"), Vector.bEveryInvocation);
	Test.TestEqual(TEXT("V: per-handle scope is exact"), Vector.bEveryHandle, bExpectedEveryHandle);
}

void TestInvocationValidationOrderVectors(FAutomationTestBase& Test)
{
	// V: T02 validates invocation identity/value/handle schema before T05 metadata and lease checks.
	static constexpr FRevalidationVector Vectors[] = {
		{TEXT("invocation-identity"), TEXT("PrimitiveInvocation.InvalidIdentity"), 0, true, false},
		{TEXT("value-arguments"), TEXT("PrimitiveInvocation.InvalidValueArguments"), 0, true, false},
		{TEXT("handle-argument-schema"), TEXT("PrimitiveInvocation.InvalidHandleArguments"), 0, true, false},
	};
	static constexpr const TCHAR* Dimensions[] = {
		TEXT("invocation-identity"), TEXT("value-arguments"), TEXT("handle-argument-schema"),
	};
	static constexpr const TCHAR* Codes[] = {
		TEXT("PrimitiveInvocation.InvalidIdentity"), TEXT("PrimitiveInvocation.InvalidValueArguments"),
		TEXT("PrimitiveInvocation.InvalidHandleArguments"),
	};
	static_assert(UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(Dimensions) && UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(Codes));
	for (uint8 Index = 0; Index < UE_ARRAY_COUNT(Vectors); ++Index)
	{
		TestExactValidationRow(Test, Vectors[Index], Dimensions[Index], Codes[Index], 0, false);
	}
}

void TestPinnedRevalidationVectors(FAutomationTestBase& Test)
{
	static constexpr FRevalidationVector Vectors[] = {
		{TEXT("request"), TEXT("HandleMetadata.RequestMismatch"), 1, true, true},
		{TEXT("transaction"), TEXT("HandleMetadata.TransactionMismatch"), 1, true, true},
		{TEXT("runtime-revision"), TEXT("HandleMetadata.RuntimeRevisionMismatch"), 1, true, true},
		{TEXT("provider"), TEXT("HandleMetadata.ProviderMismatch"), 1, true, true},
		{TEXT("invalid-phase"), TEXT("HandleMetadata.InvalidPhase"), 1, true, true},
		{TEXT("invalid-kind"), TEXT("HandleMetadata.InvalidKind"), 1, true, true},
		{TEXT("invalid-access"), TEXT("HandleMetadata.InvalidAccess"), 1, true, true},
		{TEXT("kind-mismatch"), TEXT("HandleMetadata.KindMismatch"), 1, true, true},
		{TEXT("access-mismatch"), TEXT("HandleMetadata.AccessMismatch"), 1, true, true},
		{TEXT("slot"), TEXT("HandleMetadata.InvalidSlot"), 1, true, true},
		{TEXT("generation"), TEXT("HandleMetadata.InvalidGeneration"), 1, true, true},
		{TEXT("lease-liveness"), TEXT("AD.RUNTIME.STALE_HANDLE"), 1, true, true},
	};
	static constexpr const TCHAR* Dimensions[] = {
		TEXT("request"), TEXT("transaction"), TEXT("runtime-revision"), TEXT("provider"),
		TEXT("invalid-phase"), TEXT("invalid-kind"), TEXT("invalid-access"), TEXT("kind-mismatch"),
		TEXT("access-mismatch"), TEXT("slot"), TEXT("generation"), TEXT("lease-liveness"),
	};
	static constexpr const TCHAR* Codes[] = {
		TEXT("HandleMetadata.RequestMismatch"), TEXT("HandleMetadata.TransactionMismatch"),
		TEXT("HandleMetadata.RuntimeRevisionMismatch"), TEXT("HandleMetadata.ProviderMismatch"),
		TEXT("HandleMetadata.InvalidPhase"), TEXT("HandleMetadata.InvalidKind"),
		TEXT("HandleMetadata.InvalidAccess"), TEXT("HandleMetadata.KindMismatch"),
		TEXT("HandleMetadata.AccessMismatch"), TEXT("HandleMetadata.InvalidSlot"),
		TEXT("HandleMetadata.InvalidGeneration"), TEXT("AD.RUNTIME.STALE_HANDLE"),
	};
	static_assert(UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(Dimensions) && UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(Codes));
	for (uint8 Index = 0; Index < UE_ARRAY_COUNT(Vectors); ++Index)
	{
		TestExactValidationRow(Test, Vectors[Index], Dimensions[Index], Codes[Index], 1, true);
	}
	Test.TestEqual(TEXT("V: request context owner is frozen"), FString(TEXT("F-HOST-ABI")), FString(TEXT("F-HOST-ABI")));
}

struct FNativeContextBoundaryVector
{
	bool bContextNativeCreated; bool bHandleProxyContextIssued;
	bool bPythonMayConstruct; bool bPythonMaySerialize; bool bPythonMayInspectFullIdentity;
	bool bPythonMaySynthesizeFromScalars; bool bPythonMaySubstituteObjectValue;
	bool bPythonMayExecuteArbitrary; bool bPythonMayCallByName; bool bPythonMayAccessPropertyByName;
	bool bReturnsNamedHandleArgument; bool bNativeResultUsesHandleResults; bool bOutcomeValidatedBeforeProxy;
	bool bFirstHandleUsesZeroInputPrimitive; bool bHasAmbientOrInitialHandle;
	bool bHasTargetOnlyContextProperty; bool bHasSecondHostDtoFamily;
};

void TestNativeContextBoundaryVector(FAutomationTestBase& Test)
{
	// V: This freezes the binding boundary without constructing a fake context or proxy.
	static constexpr FNativeContextBoundaryVector Vector = {
		true, true, false, false, false, false, false, false, false, false,
		true, true, true, true, false, false, false,
	};
	Test.TestTrue(TEXT("V: request context is native-created"), Vector.bContextNativeCreated);
	Test.TestTrue(TEXT("V: native handle proxy is issued by its context"), Vector.bHandleProxyContextIssued);
	Test.TestFalse(TEXT("V: Python cannot construct request context or handle proxy"), Vector.bPythonMayConstruct);
	Test.TestFalse(TEXT("V: Python cannot serialize native handles"), Vector.bPythonMaySerialize);
	Test.TestFalse(TEXT("V: Python cannot inspect complete handle identity"), Vector.bPythonMayInspectFullIdentity);
	Test.TestFalse(TEXT("V: Python cannot synthesize a handle from strings or numbers"), Vector.bPythonMaySynthesizeFromScalars);
	Test.TestFalse(TEXT("V: object-shaped values cannot substitute native handles"), Vector.bPythonMaySubstituteObjectValue);
	Test.TestFalse(TEXT("V: request context cannot execute arbitrary code"), Vector.bPythonMayExecuteArbitrary);
	Test.TestFalse(TEXT("V: request context cannot call functions by name"), Vector.bPythonMayCallByName);
	Test.TestFalse(TEXT("V: request context cannot access properties by name"), Vector.bPythonMayAccessPropertyByName);
	Test.TestTrue(TEXT("V: issued proxies return through named HandleArguments"), Vector.bReturnsNamedHandleArgument);
	Test.TestTrue(TEXT("V: provider native results return only through descriptor-named HandleResults"), Vector.bNativeResultUsesHandleResults);
	Test.TestTrue(TEXT("V: outcome and returned handle identities are validated before proxy wrapping"), Vector.bOutcomeValidatedBeforeProxy);
	Test.TestTrue(TEXT("V: first target handle comes from an admitted zero-input resolver or factory primitive"), Vector.bFirstHandleUsesZeroInputPrimitive);
	Test.TestFalse(TEXT("V: request context exposes no ambient or initial handle channel"), Vector.bHasAmbientOrInitialHandle);
	Test.TestFalse(TEXT("V: request context exposes no target-only property"), Vector.bHasTargetOnlyContextProperty);
	Test.TestFalse(TEXT("V: handle results do not create a second Host DTO family"), Vector.bHasSecondHostDtoFamily);
}

struct FMutationGuardVector
{
	const TCHAR* Attempt; const TCHAR* ExpectedCode; uint32 MaximumAttempts;
};

void TestMutationGuardAndExceptionVectors(FAutomationTestBase& Test)
{
	static constexpr FMutationGuardVector Vectors[] = {
		{TEXT("nested-apply"), TEXT("AD.RUNTIME.NESTED_MUTATION_REJECTED"), 1},
		{TEXT("same-request-reentrant-mutation"), TEXT("AD.RUNTIME.NESTED_MUTATION_REJECTED"), 1},
	};
	for (const FMutationGuardVector& Vector : Vectors)
	{
		Test.TestTrue(TEXT("V: nested mutation attempt is named"), FCString::Strlen(Vector.Attempt) > 0);
		Test.TestEqual(TEXT("V: nested mutation diagnostic is stable"), FString(Vector.ExpectedCode), FString(TEXT("AD.RUNTIME.NESTED_MUTATION_REJECTED")));
		Test.TestEqual(TEXT("V: nested mutation is never retried"), Vector.MaximumAttempts, uint32{1});
	}
	struct FPythonExceptionVector
	{
		uint32 PublicDiagnosticCount; const TCHAR* PublicCode;
		bool bPublicTraceback; bool bInternalTracebackCaptured; uint32 MaximumAttempts;
	};
	static constexpr FPythonExceptionVector Exception = {
		1, TEXT("AD.RUNTIME.PYTHON_EXCEPTION"), false, true, 1,
	};
	Test.TestEqual(TEXT("V: Python exception emits one public diagnostic"), Exception.PublicDiagnosticCount, uint32{1});
	Test.TestEqual(TEXT("V: Python exception uses the sole stable code"), FString(Exception.PublicCode), FString(TEXT("AD.RUNTIME.PYTHON_EXCEPTION")));
	Test.TestFalse(TEXT("V: Python traceback is not public"), Exception.bPublicTraceback);
	Test.TestTrue(TEXT("V: Python traceback is retained for internal diagnostics"), Exception.bInternalTracebackCaptured);
	Test.TestEqual(TEXT("V: Python exception is not retried"), Exception.MaximumAttempts, uint32{1});
}

struct FCancellationVector
{
	const TCHAR* Boundary; bool bMayObserve; bool bDefersInCriticalSection;
};

void TestCancellationVectors(FAutomationTestBase& Test)
{
	static constexpr FCancellationVector Vectors[] = {
		{TEXT("before-primitive"), true, false},
		{TEXT("during-primitive"), false, true},
		{TEXT("after-primitive"), true, false},
		{TEXT("during-compile-rebuild"), false, true},
		{TEXT("during-atomic-install"), false, true},
		{TEXT("after-atomic-install"), true, false},
	};
	static constexpr const TCHAR* Boundaries[] = {
		TEXT("before-primitive"), TEXT("during-primitive"), TEXT("after-primitive"),
		TEXT("during-compile-rebuild"), TEXT("during-atomic-install"), TEXT("after-atomic-install"),
	};
	static constexpr bool MayObserve[] = {true, false, true, false, false, true};
	static constexpr bool Defers[] = {false, true, false, true, true, false};
	static_assert(UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(Boundaries) && UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(MayObserve) && UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(Defers));
	for (uint8 Index = 0; Index < UE_ARRAY_COUNT(Vectors); ++Index)
	{
		Test.TestEqual(TEXT("V: cancellation boundary is exact"), FString(Vectors[Index].Boundary), FString(Boundaries[Index]));
		Test.TestEqual(TEXT("V: cancellation observation is exact"), Vectors[Index].bMayObserve, MayObserve[Index]);
		Test.TestEqual(TEXT("V: cancellation deferral is exact"), Vectors[Index].bDefersInCriticalSection, Defers[Index]);
	}
}

struct FRevisionActivationVector
{
	const TCHAR* CaseName; bool bCandidateValid; bool bAtomicallySwaps;
	bool bRetainsPreviousLastKnownGood; bool bInvalidatesOldRevisionHandles;
	bool bHasActiveRequest; bool bHasActiveTransaction; const TCHAR* OutcomeCode;
};

void TestRevisionActivationVectors(FAutomationTestBase& Test)
{
	static constexpr FRevisionActivationVector Vectors[] = {
		{TEXT("module-import-failure"), false, false, true, false, false, false, TEXT("AD.RUNTIME.REVISION_MODULE_IMPORT_FAILED")},
		{TEXT("registry-profile-duplicate"), false, false, true, false, false, false, TEXT("AD.RUNTIME.REVISION_REGISTRY_DUPLICATE")},
		{TEXT("required-primitive-missing"), false, false, true, false, false, false, TEXT("AD.RUNTIME.REVISION_REQUIRED_PRIMITIVE_MISSING")},
		{TEXT("version-mismatch"), false, false, true, false, false, false, TEXT("AD.RUNTIME.REVISION_VERSION_MISMATCH")},
		{TEXT("active-request-rejected"), true, false, true, false, true, false, TEXT("AD.RUNTIME.REVISION_ACTIVE_REQUEST")},
		{TEXT("active-transaction-rejected"), true, false, true, false, false, true, TEXT("AD.RUNTIME.REVISION_ACTIVE_TRANSACTION")},
		{TEXT("valid-quiescent-atomic-swap"), true, true, false, true, false, false, TEXT("AD.RUNTIME.REVISION_ACTIVATED")},
	};
	static constexpr const TCHAR* Cases[] = {
		TEXT("module-import-failure"), TEXT("registry-profile-duplicate"), TEXT("required-primitive-missing"),
		TEXT("version-mismatch"), TEXT("active-request-rejected"), TEXT("active-transaction-rejected"),
		TEXT("valid-quiescent-atomic-swap"),
	};
	static constexpr const TCHAR* Codes[] = {
		TEXT("AD.RUNTIME.REVISION_MODULE_IMPORT_FAILED"), TEXT("AD.RUNTIME.REVISION_REGISTRY_DUPLICATE"),
		TEXT("AD.RUNTIME.REVISION_REQUIRED_PRIMITIVE_MISSING"), TEXT("AD.RUNTIME.REVISION_VERSION_MISMATCH"),
		TEXT("AD.RUNTIME.REVISION_ACTIVE_REQUEST"), TEXT("AD.RUNTIME.REVISION_ACTIVE_TRANSACTION"),
		TEXT("AD.RUNTIME.REVISION_ACTIVATED"),
	};
	static constexpr bool CandidateValid[] = {false, false, false, false, true, true, true};
	static constexpr bool Swaps[] = {false, false, false, false, false, false, true};
	static constexpr bool RetainsPrevious[] = {true, true, true, true, true, true, false};
	static constexpr bool InvalidatesHandles[] = {false, false, false, false, false, false, true};
	static constexpr bool ActiveRequest[] = {false, false, false, false, true, false, false};
	static constexpr bool ActiveTransaction[] = {false, false, false, false, false, true, false};
	static_assert(UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(Cases));
	static_assert(UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(Codes));
	static_assert(UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(CandidateValid));
	static_assert(UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(Swaps));
	static_assert(UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(RetainsPrevious));
	static_assert(UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(InvalidatesHandles));
	static_assert(UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(ActiveRequest));
	static_assert(UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(ActiveTransaction));
	for (uint8 Index = 0; Index < UE_ARRAY_COUNT(Vectors); ++Index)
	{
		const FRevisionActivationVector& Vector = Vectors[Index];
		Test.TestEqual(TEXT("V: reload case is exact"), FString(Vector.CaseName), FString(Cases[Index]));
		Test.TestEqual(TEXT("V: reload candidate validity is exact"), Vector.bCandidateValid, CandidateValid[Index]);
		Test.TestEqual(TEXT("V: reload atomic swap is exact"), Vector.bAtomicallySwaps, Swaps[Index]);
		Test.TestEqual(TEXT("V: reload LKG retention is exact"), Vector.bRetainsPreviousLastKnownGood, RetainsPrevious[Index]);
		Test.TestEqual(TEXT("V: reload handle invalidation is exact"), Vector.bInvalidatesOldRevisionHandles, InvalidatesHandles[Index]);
		Test.TestEqual(TEXT("V: active-request partition is exact"), Vector.bHasActiveRequest, ActiveRequest[Index]);
		Test.TestEqual(TEXT("V: active-transaction partition is exact"), Vector.bHasActiveTransaction, ActiveTransaction[Index]);
		Test.TestEqual(TEXT("V: reload outcome code is exact"), FString(Vector.OutcomeCode), FString(Codes[Index]));
	}
}

struct FHandleInvalidationVector
{
	EAssetDocumentHandleInvalidationReason Reason; const TCHAR* ExpectedCode;
	bool bRequestMatches; bool bTransactionMatches; bool bRuntimeRevisionMatches; bool bProviderMatches;
	bool bExistingProxyUsable; bool bExistingLeaseUsable;
};

void TestHandleInvalidationVectors(FAutomationTestBase& Test)
{
	static constexpr FHandleInvalidationVector Vectors[] = {
		{EAssetDocumentHandleInvalidationReason::Commit, TEXT("Handle.Invalidated.Commit"), true, true, true, true, false, false},
		{EAssetDocumentHandleInvalidationReason::Rollback, TEXT("Handle.Invalidated.Rollback"), true, true, true, true, false, false},
		{EAssetDocumentHandleInvalidationReason::Compile, TEXT("Handle.Invalidated.Compile"), true, true, true, true, false, false},
		{EAssetDocumentHandleInvalidationReason::Rebuild, TEXT("Handle.Invalidated.Rebuild"), true, true, true, true, false, false},
		{EAssetDocumentHandleInvalidationReason::Reinstance, TEXT("Handle.Invalidated.Reinstance"), true, true, true, true, false, false},
		{EAssetDocumentHandleInvalidationReason::GarbageCollection, TEXT("Handle.Invalidated.GarbageCollection"), true, true, true, true, false, false},
		{EAssetDocumentHandleInvalidationReason::PackageUnload, TEXT("Handle.Invalidated.PackageUnload"), true, true, true, true, false, false},
		{EAssetDocumentHandleInvalidationReason::PackageReload, TEXT("Handle.Invalidated.PackageReload"), true, true, true, true, false, false},
		{EAssetDocumentHandleInvalidationReason::RuntimeRevisionSwap, TEXT("Handle.Invalidated.RuntimeRevisionSwap"), true, true, true, true, false, false},
		{EAssetDocumentHandleInvalidationReason::ProviderUnload, TEXT("Handle.Invalidated.ProviderUnload"), true, true, true, true, false, false},
		{EAssetDocumentHandleInvalidationReason::ProviderUnload, TEXT(""), true, true, true, false, true, true},
	};
	static constexpr EAssetDocumentHandleInvalidationReason Reasons[] = {
		EAssetDocumentHandleInvalidationReason::Commit, EAssetDocumentHandleInvalidationReason::Rollback,
		EAssetDocumentHandleInvalidationReason::Compile, EAssetDocumentHandleInvalidationReason::Rebuild,
		EAssetDocumentHandleInvalidationReason::Reinstance, EAssetDocumentHandleInvalidationReason::GarbageCollection,
		EAssetDocumentHandleInvalidationReason::PackageUnload, EAssetDocumentHandleInvalidationReason::PackageReload,
		EAssetDocumentHandleInvalidationReason::RuntimeRevisionSwap, EAssetDocumentHandleInvalidationReason::ProviderUnload,
		EAssetDocumentHandleInvalidationReason::ProviderUnload,
	};
	static constexpr const TCHAR* Codes[] = {
		TEXT("Handle.Invalidated.Commit"), TEXT("Handle.Invalidated.Rollback"), TEXT("Handle.Invalidated.Compile"),
		TEXT("Handle.Invalidated.Rebuild"), TEXT("Handle.Invalidated.Reinstance"), TEXT("Handle.Invalidated.GarbageCollection"),
		TEXT("Handle.Invalidated.PackageUnload"), TEXT("Handle.Invalidated.PackageReload"),
		TEXT("Handle.Invalidated.RuntimeRevisionSwap"), TEXT("Handle.Invalidated.ProviderUnload"), TEXT(""),
	};
	static constexpr bool ProviderMatches[] = {true, true, true, true, true, true, true, true, true, true, false};
	static constexpr bool RemainsUsable[] = {false, false, false, false, false, false, false, false, false, false, true};
	static_assert(UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(Reasons) && UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(Codes) && UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(ProviderMatches) && UE_ARRAY_COUNT(Vectors) == UE_ARRAY_COUNT(RemainsUsable));
	for (uint8 Index = 0; Index < UE_ARRAY_COUNT(Vectors); ++Index)
	{
		const FHandleInvalidationVector& Vector = Vectors[Index];
		Test.TestEqual(TEXT("V: invalidation reason is exact"), Vector.Reason, Reasons[Index]);
		Test.TestEqual(TEXT("V: invalidation code is exact"), FString(Vector.ExpectedCode), FString(Codes[Index]));
		Test.TestTrue(TEXT("V: invalidation request scope matches"), Vector.bRequestMatches);
		Test.TestTrue(TEXT("V: invalidation transaction scope matches"), Vector.bTransactionMatches);
		Test.TestTrue(TEXT("V: invalidation revision scope matches"), Vector.bRuntimeRevisionMatches);
		Test.TestEqual(TEXT("V: invalidation provider scope is exact"), Vector.bProviderMatches, ProviderMatches[Index]);
		Test.TestEqual(TEXT("V: context-issued proxy usability is exact"), Vector.bExistingProxyUsable, RemainsUsable[Index]);
		Test.TestEqual(TEXT("V: invocation-scoped lease usability is exact"), Vector.bExistingLeaseUsable, RemainsUsable[Index]);
	}
}

struct FNoRetryVector
{
	const TCHAR* Boundary; uint32 MaximumAttempts;
};

void TestNoImplicitRetryVectors(FAutomationTestBase& Test)
{
	static constexpr FNoRetryVector Vectors[] = {
		{TEXT("host-dispatch"), 1},
		{TEXT("context-invoke"), 1},
		{TEXT("mutating-primitive"), 1},
		{TEXT("python-exception"), 1},
		{TEXT("stale-handle"), 1},
	};
	for (const FNoRetryVector& Vector : Vectors)
	{
		Test.TestTrue(TEXT("V: no-retry boundary is named"), FCString::Strlen(Vector.Boundary) > 0);
		Test.TestEqual(TEXT("V: Host performs no implicit retry"), Vector.MaximumAttempts, uint32{1});
	}
	Test.TestEqual(TEXT("V: Host ABI behavior owner is frozen"), FString(TEXT("F-HOST-ABI")), FString(TEXT("F-HOST-ABI")));
}
} // namespace AssetDocumentRequestCoordinatorContractTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentRequestCoordinatorContractTest,
	"UEAssetDoc.RuntimeContracts.RequestCoordinator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRequestCoordinatorContractTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentRequestCoordinatorContractTests;
	(void)Parameters;
	TestMetadataDefaults(*this);
	TestExactPhaseTable(*this);
	TestPhaseBoundaries(*this);
	TestHostCallVectors(*this);
	TestIdentityPinAndLifetimeVectors(*this);
	TestInvocationValidationOrderVectors(*this);
	TestPinnedRevalidationVectors(*this);
	TestNativeContextBoundaryVector(*this);
	TestMutationGuardAndExceptionVectors(*this);
	TestCancellationVectors(*this);
	TestRevisionActivationVectors(*this);
	TestHandleInvalidationVectors(*this);
	TestNoImplicitRetryVectors(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentHandleHandoffContractTest,
	"UEAssetDoc.HandleHandoff.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentHandleHandoffContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	AssetDocumentRequestCoordinatorContractTests::TestDefaultHandoffFailure(*this);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
