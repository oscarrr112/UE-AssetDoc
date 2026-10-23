// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentHandleRegistryRuntime.h"
#include "Misc/AutomationTest.h"

namespace
{
class FAssetDocumentScratchScopeTestPayload final : public IAssetDocumentNativeHandlePayload
{
public:
	explicit FAssetDocumentScratchScopeTestPayload(int32* InDestructionCount)
		: DestructionCount(InDestructionCount)
	{
	}

	~FAssetDocumentScratchScopeTestPayload() override
	{
		++*DestructionCount;
	}

	FName GetPayloadTypeId() const override
	{
		return TEXT("host.scratch_scope_test.payload");
	}

	bool IsAlive() const override
	{
		return true;
	}

private:
	int32* DestructionCount;
};

FAssetDocumentHandleResolverBinding MakeScratchScopeBinding()
{
	FAssetDocumentHandleResolverBinding Binding;
	Binding.RequestId = FGuid::NewGuid();
	Binding.TransactionId = FGuid::NewGuid();
	Binding.RuntimeRevision = 7;
	Binding.ProviderId = TEXT("editor");
	Binding.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	for (const EAssetDocumentHandleKind Kind : {
		EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleKind::Object})
	{
		FAssetDocumentPrimitiveHandleResultSchema& Schema =
			Binding.AllowedResults.AddDefaulted_GetRef();
		Schema.Name = Kind == EAssetDocumentHandleKind::Scratch
			? TEXT("scratch")
			: TEXT("object");
		Schema.Kind = Kind;
		Schema.GrantedAccess = EAssetDocumentHandleAccess::Mutable;
		Schema.Cardinality = EAssetDocumentHandleResultCardinality::ZeroOrMore;
	}
	return Binding;
}

FAssetDocumentHandleValidationContext MakeScratchScopeContext(
	const FAssetDocumentHandleResolverBinding& Binding,
	EAssetDocumentHandleKind RequiredKind)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Binding.RequestId;
	Context.TransactionId = Binding.TransactionId;
	Context.RuntimeRevision = Binding.RuntimeRevision;
	Context.ProviderId = Binding.ProviderId;
	Context.Phase = Binding.Phase;
	Context.RequiredKind = RequiredKind;
	Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Context;
}

FAssetDocumentOpaqueHandle IssueAndRetain(
	FAssetDocumentHandleRegistryRuntime& Registry,
	const FAssetDocumentHandleResolverBinding& Binding,
	EAssetDocumentHandleKind Kind,
	int32* DestructionCount)
{
	TUniquePtr<FAssetDocumentInvocationHandleSession> Session =
		Registry.CreateInvocationSession(Binding);
	FAssetDocumentIssuedHandleResult Issued = Session->Issue(
		Kind,
		EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentScratchScopeTestPayload>(DestructionCount));
	check(Issued.Handle.IsSet());
	FAssetDocumentPrimitiveHandleResult Returned;
	Returned.Name = Kind == EAssetDocumentHandleKind::Scratch
		? TEXT("scratch")
		: TEXT("object");
	Returned.Handles.Add(Issued.Handle.GetValue());
	check(Session->MarkValidatedReturnedHandles(MakeArrayView(&Returned, 1)).bValid);
	Session->Close(EAssetDocumentInvocationHandleCloseDisposition::Succeeded);
	return Issued.Handle.GetValue();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentScratchContextLifetimeTest,
	"UEAssetDoc.HandleRegistry.ScratchContextLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentScratchContextLifetimeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(TEXT("scratch lifetime private contract version"),
		UE::AssetDocument::PythonHost::ScratchContextLifetimeContractVersion, 1u);

	TSharedRef<FAssetDocumentHandleRegistryRuntime> Registry =
		MakeShared<FAssetDocumentHandleRegistryRuntime>();
	const FAssetDocumentHandleResolverBinding Binding = MakeScratchScopeBinding();
	FAssetDocumentHandleResolverBinding OtherTransaction = Binding;
	OtherTransaction.TransactionId = FGuid::NewGuid();
	FAssetDocumentHandleResolverBinding OtherRequest = Binding;
	OtherRequest.RequestId = FGuid::NewGuid();
	FAssetDocumentHandleResolverBinding OtherRevision = Binding;
	++OtherRevision.RuntimeRevision;

	int32 DestructionCount = 0;
	const FAssetDocumentOpaqueHandle ExactScratch = IssueAndRetain(
		*Registry, Binding, EAssetDocumentHandleKind::Scratch, &DestructionCount);
	const FAssetDocumentOpaqueHandle SameScopeObject = IssueAndRetain(
		*Registry, Binding, EAssetDocumentHandleKind::Object, &DestructionCount);
	const FAssetDocumentOpaqueHandle OtherTransactionScratch = IssueAndRetain(
		*Registry, OtherTransaction, EAssetDocumentHandleKind::Scratch, &DestructionCount);
	const FAssetDocumentOpaqueHandle OtherRequestScratch = IssueAndRetain(
		*Registry, OtherRequest, EAssetDocumentHandleKind::Scratch, &DestructionCount);
	const FAssetDocumentOpaqueHandle OtherRevisionScratch = IssueAndRetain(
		*Registry, OtherRevision, EAssetDocumentHandleKind::Scratch, &DestructionCount);

	TestTrue(TEXT("returned same-scope Scratch survives successful invocation close"),
		Registry->IsIssuedAndLive(ExactScratch));
	TUniquePtr<FAssetDocumentInvocationHandleSession> LeaseSession =
		Registry->CreateInvocationSession(Binding);
	FAssetDocumentVerifiedLeaseResult Lease = LeaseSession->AcquireVerifiedLease(
		ExactScratch, MakeScratchScopeContext(Binding, EAssetDocumentHandleKind::Scratch));
	TestTrue(TEXT("retained Scratch lease is acquired"), Lease.Lease.IsValid());
	TestTrue(TEXT("retained Scratch lease state is live before retirement"),
		Registry->HasLiveLeaseForTesting(ExactScratch));

	Registry->RetireScratchHandlesForContext(
		Binding.RequestId,
		Binding.TransactionId,
		Binding.RuntimeRevision);

	TestTrue(TEXT("same-scope Object survives context-scope retirement"),
		Registry->IsIssuedAndLive(SameScopeObject));
	TestTrue(TEXT("same-request other-transaction Scratch survives"),
		Registry->IsIssuedAndLive(OtherTransactionScratch));
	TestTrue(TEXT("same-transaction other-request Scratch survives"),
		Registry->IsIssuedAndLive(OtherRequestScratch));
	TestTrue(TEXT("same-request transaction other-revision Scratch survives"),
		Registry->IsIssuedAndLive(OtherRevisionScratch));
	TestFalse(TEXT("exact-scope Scratch becomes stale"), Registry->IsIssuedAndLive(ExactScratch));
	TestEqual(TEXT("exact-scope Scratch is destroyed once"), DestructionCount, 1);
	TestFalse(TEXT("retirement revokes retained lease state"),
		Registry->HasLiveLeaseForTesting(ExactScratch));
	FAssetDocumentVerifiedLeaseResult StaleLease = LeaseSession->AcquireVerifiedLease(
		ExactScratch, MakeScratchScopeContext(Binding, EAssetDocumentHandleKind::Scratch));
	TestFalse(TEXT("stale Scratch cannot acquire a future lease"), StaleLease.Lease.IsValid());

	Registry->RetireScratchHandlesForContext(
		Binding.RequestId,
		Binding.TransactionId,
		Binding.RuntimeRevision);
	TestEqual(TEXT("second retirement is a no-op"), DestructionCount, 1);
	return true;
}
