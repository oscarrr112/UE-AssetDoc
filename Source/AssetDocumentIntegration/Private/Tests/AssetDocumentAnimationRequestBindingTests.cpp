// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentAnimationRequestBinding.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"
#include "Animation/AnimMontage.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
class FRequestProvider final : public IAssetDocumentAnimationRequestProvider
{
public:
	FAssetDocumentContractValidationResult BindRequest(const FAssetDocumentRuntimeRequestDto&) override
	{ FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
	void CloseRequest() override { ++Closed; }
	TUniquePtr<FAssetDocumentAnimationStagedReadLease> BindStagedRead(UAnimMontage&, const FString&, uint64,
		TArray<FAssetDocumentRuntimeDiagnostic>&) override
	{ if (!AcceptStagedRead) return {}; return MakeUnique<FAssetDocumentAnimationStagedReadLease>([] {}); }
	FString GetProviderId() const override { return TEXT("animation"); }
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override { return {}; }
	FAssetDocumentProviderAvailability InspectAvailability() const override
	{ return {TEXT("animation"), EAssetDocumentProviderAvailabilityState::Available}; }
	FAssetDocumentContractValidationResult Prepare(const FAssetDocumentRuntimeRequestDto&,
		TConstArrayView<FAssetDocumentMutationIntent>, FAssetDocumentPreparedMutationBatch&) override
	{ ++Prepared; FAssetDocumentContractValidationResult R; R.bValid = true; return R; }
	FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation&,
		IAssetDocumentHandleResolver&, IAssetDocumentAtomicTransaction*) override
	{ ++Invoked; FAssetDocumentPrimitiveOutcome R; R.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded; return R; }
	int32 Prepared = 0;
	int32 Invoked = 0;
	int32 Closed = 0;
	bool AcceptStagedRead = false;
};
class FNoHandles final : public IAssetDocumentHandleResolver
{
public:
	FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind, EAssetDocumentHandleAccess,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&&) override { return {}; }
	FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle&,
		const FAssetDocumentHandleValidationContext&) override { return {}; }
	FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override { return {}; }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationRequestBindingIsolationTest,
	"UEAssetDoc.Animation.Production.RequestBindingIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAnimationRequestBindingIsolationTest::RunTest(const FString&)
{
	TArray<TSharedRef<FRequestProvider>> Created;
	FAssetDocumentAnimationRequestBinding Binding([&]() -> TSharedRef<IAssetDocumentAnimationRequestProvider>
	{
		auto Provider = MakeShared<FRequestProvider>(); Created.Add(Provider); return Provider;
	});
	FAssetDocumentRuntimeRequestDto Request;
	Request.RequestId = FGuid::NewGuid(); Request.ExpectedRuntimeRevision = 2;
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.RequestId = Request.RequestId; Invocation.RuntimeRevision = 2;
	FNoHandles Handles;
	FAssetDocumentPreparedMutationBatch Batch;
	TestFalse(TEXT("unbound preparation rejected"), Binding.Prepare(Request, {}, Batch).bValid);
	TestEqual(TEXT("unbound invocation rejected"), Binding.Invoke(Invocation, Handles, nullptr).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("first request binds"), Binding.BindRequest(Request).bValid);
	TestEqual(TEXT("catalog owner is not a request owner"), Created.Num(), 2);
	TestFalse(TEXT("nested binding cannot replace an active request"), Binding.BindRequest(Request).bValid);
	TestTrue(TEXT("first preparation succeeds"), Binding.Prepare(Request, {}, Batch).bValid);
	TestEqual(TEXT("first invoke succeeds"), Binding.Invoke(Invocation, Handles, nullptr).Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestEqual(TEXT("same instance prepared"), Created[1]->Prepared, 1);
	TestEqual(TEXT("same instance invoked"), Created[1]->Invoked, 1);
	auto Foreign = Invocation; Foreign.RequestId = FGuid::NewGuid();
	TestEqual(TEXT("foreign request rejected"), Binding.Invoke(Foreign, Handles, nullptr).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Foreign = Invocation; ++Foreign.RuntimeRevision;
	TestEqual(TEXT("foreign revision rejected"), Binding.Invoke(Foreign, Handles, nullptr).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Binding.CloseRequest();
	TestEqual(TEXT("native request closes at scope end"), Created[1]->Closed, 1);
	TestEqual(TEXT("closed request cannot invoke"), Binding.Invoke(Invocation, Handles, nullptr).Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Request.RequestId = FGuid::NewGuid();
	TestTrue(TEXT("second request in same revision binds"), Binding.BindRequest(Request).bValid);
	TestEqual(TEXT("second request gets a fresh owner"), Created.Num(), 3);
	TestTrue(TEXT("second request prepares independently"), Binding.Prepare(Request, {}, Batch).bValid);
	Invocation.RequestId = Request.RequestId;
	Binding.Invoke(Invocation, Handles, nullptr);
	TestEqual(TEXT("old request owner remains unchanged"), Created[1]->Invoked, 1);
	TestEqual(TEXT("new request owner invoked"), Created[2]->Invoked, 1);
	TestEqual(TEXT("catalog owner never prepared"), Created[0]->Prepared, 0);
	TestEqual(TEXT("catalog owner never invoked"), Created[0]->Invoked, 0);
	Binding.CloseRequest();
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationReferencesDocumentBindingIsolationTest,
	"UEAssetDoc.Animation.Production.ReferencesDocumentBindingIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAnimationReferencesDocumentBindingIsolationTest::RunTest(const FString&)
{
	class FReadonlyContext final : public IAssetDocumentNativeRequestContext
	{
	public:
		FAssetDocumentRequestContextMetadata Metadata;
		int32 Invoked = 0;
		const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
		FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return {}; }
		FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation&) override { ++Invoked; return {}; }
	} Context;
	TArray<TSharedRef<FRequestProvider>> Created;
	FAssetDocumentAnimationRequestBinding Binding([&]() -> TSharedRef<IAssetDocumentAnimationRequestProvider>
	{ auto Provider = MakeShared<FRequestProvider>(); Created.Add(Provider); return Provider; });
	FAssetDocumentRuntimeRequestDto Request; Request.RequestId = FGuid::NewGuid(); Request.ExpectedRuntimeRevision = 94;
	Context.Metadata.RequestId = Request.RequestId; Context.Metadata.PinnedRuntimeRevision = Request.ExpectedRuntimeRevision;
	Context.Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
	const FGuid ReadTransaction = FGuid::NewGuid(); TArray<FAssetDocumentMutationIntent> Actual; FAssetDocumentPreparedMutationBatch Batch;
	auto Attempt = [&](const FAssetDocumentRuntimeRequestDto& R)
	{
		Actual.AddDefaulted();
		return Binding.PrepareSequenceReferencesDocument(R, Context, ReadTransaction, {}, 0, 1,
			EAssetDocumentRuntimeRequestPhase::Apply, {}, Actual, Batch);
	};
	TestFalse(TEXT("unbound References preparation refused"), Attempt(Request).bValid);
	TestTrue(TEXT("unbound refusal clears output"), Actual.IsEmpty() && Batch.Contributions.IsEmpty());
	TestTrue(TEXT("same private facade binds original request"), Binding.BindRequest(Request).bValid);
	TestEqual(TEXT("no new document-only owner"), Created.Num(), 2);
	auto Foreign = Request; Foreign.RequestId = FGuid::NewGuid();
	TestFalse(TEXT("foreign request References refused"), Attempt(Foreign).bValid);
	Foreign = Request; ++Foreign.ExpectedRuntimeRevision;
	TestFalse(TEXT("foreign revision References refused"), Attempt(Foreign).bValid);
	Foreign = Request; Foreign.TargetAssetPath = TEXT("/Game/Foreign.Foreign");
	TestFalse(TEXT("same request cannot substitute a target before readonly resolution"), Attempt(Foreign).bValid);
	Foreign = Request; Foreign.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
	TestFalse(TEXT("same request cannot substitute requested class"), Attempt(Foreign).bValid);
	Created[1]->AcceptStagedRead = true;
	TStrongObjectPtr<UAnimMontage> Staged(NewObject<UAnimMontage>(GetTransientPackage()));
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	auto Lease = Binding.BindStagedRead(*Staged, TEXT("fixture-staged-path"), 95, Diagnostics);
	if (!TestTrue(TEXT("original fake staged lease establishes isolation boundary"), bool(Lease))) return false;
	TestFalse(TEXT("References preparation cannot use staged authority"), Attempt(Request).bValid);
	Binding.CloseRequest();
	TestFalse(TEXT("closed request References refused"), Attempt(Request).bValid);
	TestEqual(TEXT("all isolation refusals make zero loader calls"), Context.Invoked, 0);
	TestEqual(TEXT("all isolation refusals make zero native preparations"), Created[1]->Prepared, 0);
	TestEqual(TEXT("catalog still never prepares"), Created[0]->Prepared, 0);
	return !HasAnyErrors();
}
#endif
