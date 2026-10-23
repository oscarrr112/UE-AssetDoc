// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGenericRequestAdapter.h"
#include "AssetDocumentGenericRuntimeRoute.h"

#include "AssetDocumentPythonRuntime.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace GenericInspectRouteTestsLocal
{
const FAssetDocumentRuntimeValueDto* FindObjectField(
	const FAssetDocumentRuntimeValueDto& Object,
	const TCHAR* Key)
{
	const int32 Index = Object.ObjectKeys.IndexOfByKey(Key);
	return Object.Kind == EAssetDocumentRuntimeValueKind::Object
		&& Object.Elements.IsValidIndex(Index)
		? &Object.Elements[Index]
		: nullptr;
}

class FInspectContext final : public IAssetDocumentNativeRequestContext
{
public:
	explicit FInspectContext(const FAssetDocumentRequestContextMetadata& InMetadata)
		: Metadata(InMetadata)
	{
	}

	const FAssetDocumentRequestContextMetadata& GetMetadata() const override
	{
		return Metadata;
	}

	FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation) override
	{
		(void)Invocation;
		return {};
	}

	FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override
	{
		return {};
	}

private:
	FAssetDocumentRequestContextMetadata Metadata;
};

class FInspectHost final : public IAssetDocumentPythonRuntimeHost
{
public:
	FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) override
	{
		++DispatchCount;
		ObservedRequest = Request;
		ObservedPhase = Context.GetMetadata().Phase;
		FAssetDocumentRuntimeResultDto Result;
		Result.RequestId = Request.RequestId;
		Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded;
		Result.FinalPhase = Context.GetMetadata().Phase;
		Result.RuntimeRevision = Request.ExpectedRuntimeRevision;
		Result.CanonicalValue = Request.CanonicalDocument;
		return Result;
	}

	int32 DispatchCount = 0;
	FAssetDocumentRuntimeRequestDto ObservedRequest;
	EAssetDocumentRuntimeRequestPhase ObservedPhase =
		EAssetDocumentRuntimeRequestPhase::Failed;
};

class FInspectRuntime final : public IAssetDocumentPythonRuntime
{
public:
	FAssetDocumentContractValidationResult InitializePythonBinding() override
	{
		return {};
	}

	FAssetDocumentPythonRevisionActivationResult TryActivateRevision(
		const FAssetDocumentPythonRevisionCandidate& Candidate) override
	{
		(void)Candidate;
		return {};
	}

	uint64 GetActiveRuntimeRevision() const override { return 23; }
	bool HasActiveRequests() const override { return false; }
	IAssetDocumentPythonRuntimeHost& GetHost() override { return Host; }

	const IAssetDocumentPrimitiveRegistry* GetActivePrimitiveRegistry()
		const override
	{
		return nullptr;
	}

	FAssetDocumentPythonRequestContextCreateResult CreateRequestContext(
		const FAssetDocumentRequestContextMetadata& Metadata,
		const FGuid& TransactionId,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		++ContextCreateCount;
		ObservedMetadata = Metadata;
		bObservedTransaction = TransactionId.IsValid();
		bObservedActiveTransaction = ActiveTransaction != nullptr;
		FAssetDocumentPythonRequestContextCreateResult Result;
		Result.Context = MakeUnique<FInspectContext>(Metadata);
		return Result;
	}

	FAssetDocumentHandleInvalidationResult InvalidateHandles(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		(void)Event;
		return {};
	}

	void ShutdownPythonBinding() override {}

	FInspectHost Host;
	FAssetDocumentRequestContextMetadata ObservedMetadata;
	int32 ContextCreateCount = 0;
	bool bObservedTransaction = false;
	bool bObservedActiveTransaction = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericInspectAdapterTest,
	"UEAssetDoc.GenericRuntime.Adapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericInspectAdapterTest::RunTest(const FString& Parameters)
{
	using namespace GenericInspectRouteTestsLocal;
	(void)Parameters;
	const FString ClassPath = TEXT("/Script/AssetDocumentTests.TestDataAsset");
	const FString ObjectPath = TEXT("/Game/Generic/DA_Inspect.DA_Inspect");
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = ClassPath;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	const TOptional<FAssetDocumentRuntimeRequestDto> ClassOnly =
		FAssetDocumentGenericRequestAdapter::AdaptInspect(
			Request, ClassPath, TEXT(""), 2, Diagnostics);
	TestTrue(TEXT("class-only Inspect adapts"), ClassOnly.IsSet());
	TestEqual(TEXT("class-only Inspect has no diagnostics"), Diagnostics.Num(), 0);
	if (!ClassOnly.IsSet())
	{
		return false;
	}
	TestEqual(
		TEXT("class-only Inspect operation is Inspect"),
		ClassOnly->Operation,
		EAssetDocumentRuntimeOperation::Inspect);
	TestEqual(
		TEXT("class-only target is the exact class path"),
		ClassOnly->TargetAssetPath,
		ClassPath);
	TestEqual(
		TEXT("class-only requested class is exact"),
		ClassOnly->RequestedAssetClassPath,
		ClassPath);
	TestTrue(
		TEXT("class-only requested profile remains empty"),
		ClassOnly->RequestedProfileClassPath.IsEmpty());
	const FAssetDocumentRuntimeValueDto* ClassMode =
		FindObjectField(ClassOnly->CanonicalDocument, TEXT("mode"));
	TestTrue(
		TEXT("class-only canonical document is exactly class_defaults"),
		ClassOnly->CanonicalDocument.ObjectKeys.Num() == 1
			&& ClassMode != nullptr
			&& ClassMode->Kind == EAssetDocumentRuntimeValueKind::String
			&& ClassMode->StringValue == TEXT("class_defaults"));

	Diagnostics.Reset();
	Request.ClassOrAsset = ObjectPath;
	const TOptional<FAssetDocumentRuntimeRequestDto> Live =
		FAssetDocumentGenericRequestAdapter::AdaptInspect(
			Request, ClassPath, ObjectPath, 2, Diagnostics);
	TestTrue(TEXT("live Inspect adapts"), Live.IsSet());
	TestEqual(TEXT("live Inspect has no diagnostics"), Diagnostics.Num(), 0);
	if (!Live.IsSet())
	{
		return false;
	}
	TestEqual(TEXT("live target is the exact object path"), Live->TargetAssetPath, ObjectPath);
	const FAssetDocumentRuntimeValueDto* LiveMode =
		FindObjectField(Live->CanonicalDocument, TEXT("mode"));
	TestTrue(
		TEXT("live canonical document is exactly live_asset"),
		Live->CanonicalDocument.ObjectKeys.Num() == 1
			&& LiveMode != nullptr
			&& LiveMode->Kind == EAssetDocumentRuntimeValueKind::String
			&& LiveMode->StringValue == TEXT("live_asset"));

	Diagnostics.Reset();
	const FString ReadOnlyPath = TEXT("/Niagara/Enums/SpriteRenderer/ENiagara_Example.ENiagara_Example");
	Request.ClassOrAsset = ReadOnlyPath;
	const TOptional<FAssetDocumentRuntimeRequestDto> ReadOnly =
		FAssetDocumentGenericRequestAdapter::AdaptInspect(
			Request, ClassPath, ReadOnlyPath, 2, Diagnostics);
	TestTrue(TEXT("read-only engine/plugin Inspect adapts as a live target"),
		ReadOnly.IsSet() && ReadOnly->TargetAssetPath == ReadOnlyPath);
	Request.ClassOrAsset = ObjectPath;

	Diagnostics.Reset();
	TestFalse(
		TEXT("empty class rejects Inspect adaptation"),
		FAssetDocumentGenericRequestAdapter::AdaptInspect(
			Request, TEXT(""), ObjectPath, 2, Diagnostics).IsSet());
	Diagnostics.Reset();
	TestFalse(
		TEXT("noncanonical live object path rejects Inspect adaptation"),
		FAssetDocumentGenericRequestAdapter::AdaptInspect(
			Request, ClassPath, TEXT("/Game/Generic/DA_Inspect"), 2, Diagnostics).IsSet());
	Diagnostics.Reset();
	TestFalse(
		TEXT("class path cannot occupy the live object path slot"),
		FAssetDocumentGenericRequestAdapter::AdaptInspect(
			Request, ClassPath, ClassPath, 2, Diagnostics).IsSet());
	Diagnostics.Reset();
	TestFalse(
		TEXT("zero revision rejects Inspect adaptation"),
		FAssetDocumentGenericRequestAdapter::AdaptInspect(
			Request, ClassPath, ObjectPath, 0, Diagnostics).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericInspectRouteDispatchTest,
	"UEAssetDoc.GenericRuntime.Route",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericInspectRouteDispatchTest::RunTest(
	const FString& Parameters)
{
	using namespace GenericInspectRouteTestsLocal;
	(void)Parameters;
	FInspectRuntime Runtime;
	FAssetDocumentGenericRuntimeRoute Route(Runtime);
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = TEXT("/Script/AssetDocumentTests.TestDataAsset");
	const FAssetDocumentRuntimeResultDto Result = Route.DispatchInspect(
		Request,
		TEXT("/Script/AssetDocumentTests.TestDataAsset"),
		TEXT(""));
	TestEqual(TEXT("Inspect route dispatches Python exactly once"), Runtime.Host.DispatchCount, 1);
	TestEqual(
		TEXT("Inspect route dispatches only Verify"),
		Runtime.Host.ObservedPhase,
		EAssetDocumentRuntimeRequestPhase::Verify);
	TestEqual(
		TEXT("Inspect route request operation is Inspect"),
		Runtime.Host.ObservedRequest.Operation,
		EAssetDocumentRuntimeOperation::Inspect);
	TestEqual(
		TEXT("Inspect route creates one request context"),
		Runtime.ContextCreateCount,
		1);
	TestFalse(TEXT("Inspect route has no active transaction"), Runtime.bObservedActiveTransaction);
	TestTrue(TEXT("Inspect route returns no changed-set"), Result.ChangedSet.IsEmpty());
	TestEqual(
		TEXT("Inspect route returns the Python terminal unchanged"),
		Result.TerminalStatus,
		EAssetDocumentRuntimeTerminalStatus::Succeeded);
	return true;
}
#endif
