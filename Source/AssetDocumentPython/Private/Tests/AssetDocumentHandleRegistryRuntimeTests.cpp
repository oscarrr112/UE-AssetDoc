// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentHandleRegistryRuntime.h"
#include "Misc/AutomationTest.h"

namespace
{
class FAssetDocumentHostTestPayload final : public IAssetDocumentNativeHandlePayload
{
public:
	explicit FAssetDocumentHostTestPayload(
		bool bInAlive = true,
		int32* InDestructionCount = nullptr)
		: bAlive(bInAlive)
		, DestructionCount(InDestructionCount)
	{
	}
	~FAssetDocumentHostTestPayload() override
	{
		if (DestructionCount != nullptr)
		{
			++*DestructionCount;
		}
	}

	FName GetPayloadTypeId() const override
	{
		return TEXT("host.test.payload");
	}

	bool IsAlive() const override
	{
		return bAlive;
	}

private:
	bool bAlive;
	int32* DestructionCount;
};

FAssetDocumentHandleResolverBinding MakeBinding()
{
	FAssetDocumentHandleResolverBinding Binding;
	Binding.RequestId = FGuid::NewGuid();
	Binding.TransactionId = FGuid::NewGuid();
	Binding.RuntimeRevision = 7;
	Binding.ProviderId = TEXT("editor");
	Binding.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	FAssetDocumentPrimitiveHandleResultSchema& Schema = Binding.AllowedResults.AddDefaulted_GetRef();
	Schema.Name = TEXT("target");
	Schema.Kind = EAssetDocumentHandleKind::Object;
	Schema.GrantedAccess = EAssetDocumentHandleAccess::Mutable;
	Schema.Cardinality = EAssetDocumentHandleResultCardinality::ExactlyOne;
	return Binding;
}

FAssetDocumentHandleValidationContext MakeContext(
	const FAssetDocumentHandleResolverBinding& Binding)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Binding.RequestId;
	Context.TransactionId = Binding.TransactionId;
	Context.RuntimeRevision = Binding.RuntimeRevision;
	Context.ProviderId = Binding.ProviderId;
	Context.Phase = Binding.Phase;
	Context.RequiredKind = EAssetDocumentHandleKind::Object;
	Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Context;
}

FAssetDocumentOpaqueHandle IssueObject(FAssetDocumentInvocationHandleSession& Session)
{
	FAssetDocumentIssuedHandleResult Result = Session.Issue(
		EAssetDocumentHandleKind::Object,
		EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentHostTestPayload>());
	check(Result.Handle.IsSet());
	return Result.Handle.GetValue();
}

FAssetDocumentPrimitiveHandleResult MakeReturned(
	const FAssetDocumentOpaqueHandle& Handle)
{
	FAssetDocumentPrimitiveHandleResult Result;
	Result.Name = TEXT("target");
	Result.Handles.Add(Handle);
	return Result;
}

bool IsTransactionInvalidation(EAssetDocumentHandleInvalidationReason Reason)
{
	return Reason == EAssetDocumentHandleInvalidationReason::Commit
		|| Reason == EAssetDocumentHandleInvalidationReason::Rollback;
}

FAssetDocumentHandleInvalidation MakeInvalidation(
	EAssetDocumentHandleInvalidationReason Reason,
	const FAssetDocumentHandleResolverBinding& Binding)
{
	FAssetDocumentHandleInvalidation Event;
	Event.Reason = Reason;
	if (IsTransactionInvalidation(Reason))
	{
		Event.TransactionId = Binding.TransactionId;
	}
	else if (Reason == EAssetDocumentHandleInvalidationReason::RuntimeRevisionSwap)
	{
		Event.RuntimeRevision = Binding.RuntimeRevision;
	}
	else if (Reason == EAssetDocumentHandleInvalidationReason::ProviderUnload)
	{
		Event.ProviderId = Binding.ProviderId;
	}
	else if (Reason != EAssetDocumentHandleInvalidationReason::GarbageCollection)
	{
		Event.RequestId = Binding.RequestId;
	}
	return Event;
}

void TestInvalidationReason(
	FAutomationTestBase& Test,
	EAssetDocumentHandleInvalidationReason Reason)
{
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Registry = MakeShared<FAssetDocumentHandleRegistryRuntime>();
	const FAssetDocumentHandleResolverBinding Binding = MakeBinding();
	TUniquePtr<FAssetDocumentInvocationHandleSession> Session = Registry->CreateInvocationSession(Binding);
	const FAssetDocumentOpaqueHandle Handle = IssueObject(*Session);
	Test.TestTrue(TEXT("registry assigns nonzero slot"), Handle.Slot != 0);
	Test.TestTrue(TEXT("registry assigns nonzero generation"), Handle.Generation != 0);
	FAssetDocumentVerifiedLeaseResult Before = Session->AcquireVerifiedLease(Handle, MakeContext(Binding));
	Test.TestTrue(TEXT("live handle acquires one lease"), Before.Lease.IsValid());
	Session->Invalidate(MakeInvalidation(Reason, Binding));
	FAssetDocumentVerifiedLeaseResult After = Session->AcquireVerifiedLease(Handle, MakeContext(Binding));
	Test.TestFalse(TEXT("invalidated handle has no lease"), After.Lease.IsValid());
	Test.TestEqual(TEXT("invalidated handle returns one diagnostic"), After.Diagnostics.Num(), 1);
	Test.TestEqual(TEXT("invalidated handle is stale"), After.Diagnostics[0].Code,
		FString(TEXT("AD.RUNTIME.STALE_HANDLE")));
}

void TestAllInvalidationReasons(FAutomationTestBase& Test)
{
	static constexpr EAssetDocumentHandleInvalidationReason Reasons[] = {
		EAssetDocumentHandleInvalidationReason::Commit,
		EAssetDocumentHandleInvalidationReason::Rollback,
		EAssetDocumentHandleInvalidationReason::Compile,
		EAssetDocumentHandleInvalidationReason::Rebuild,
		EAssetDocumentHandleInvalidationReason::Reinstance,
		EAssetDocumentHandleInvalidationReason::GarbageCollection,
		EAssetDocumentHandleInvalidationReason::PackageUnload,
		EAssetDocumentHandleInvalidationReason::PackageReload,
		EAssetDocumentHandleInvalidationReason::RuntimeRevisionSwap,
		EAssetDocumentHandleInvalidationReason::ProviderUnload,
	};
	for (EAssetDocumentHandleInvalidationReason Reason : Reasons)
	{
		TestInvalidationReason(Test, Reason);
	}
}

void TestIssueFailures(FAutomationTestBase& Test)
{
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Registry = MakeShared<FAssetDocumentHandleRegistryRuntime>();
	TUniquePtr<FAssetDocumentInvocationHandleSession> Session = Registry->CreateInvocationSession(MakeBinding());
	TUniquePtr<IAssetDocumentNativeHandlePayload> Empty;
	FAssetDocumentIssuedHandleResult EmptyResult = Session->Issue(
		EAssetDocumentHandleKind::Object, EAssetDocumentHandleAccess::Mutable, MoveTemp(Empty));
	Test.TestFalse(TEXT("empty payload is rejected"), EmptyResult.Handle.IsSet());
	FAssetDocumentIssuedHandleResult DeadResult = Session->Issue(
		EAssetDocumentHandleKind::Object, EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentHostTestPayload>(false));
	Test.TestFalse(TEXT("dead payload is rejected"), DeadResult.Handle.IsSet());
}

void TestMetadataFailure(
	FAutomationTestBase& Test,
	FAssetDocumentInvocationHandleSession& Session,
	const FAssetDocumentOpaqueHandle& Handle,
	FAssetDocumentHandleValidationContext Context,
	const TCHAR* ExpectedCode)
{
	FAssetDocumentVerifiedLeaseResult Result = Session.AcquireVerifiedLease(Handle, Context);
	Test.TestFalse(TEXT("invalid metadata returns no lease"), Result.Lease.IsValid());
	Test.TestEqual(TEXT("invalid metadata returns one diagnostic"), Result.Diagnostics.Num(), 1);
	Test.TestEqual(TEXT("metadata code is preserved"), Result.Diagnostics[0].Code, FString(ExpectedCode));
}

void TestMetadataFailures(FAutomationTestBase& Test)
{
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Registry = MakeShared<FAssetDocumentHandleRegistryRuntime>();
	const FAssetDocumentHandleResolverBinding Binding = MakeBinding();
	TUniquePtr<FAssetDocumentInvocationHandleSession> Session = Registry->CreateInvocationSession(Binding);
	const FAssetDocumentOpaqueHandle Handle = IssueObject(*Session);
	FAssetDocumentHandleValidationContext Context = MakeContext(Binding);
	Context.RequestId = FGuid::NewGuid();
	TestMetadataFailure(Test, *Session, Handle, Context, TEXT("HandleMetadata.RequestMismatch"));
	Context = MakeContext(Binding); Context.TransactionId = FGuid::NewGuid();
	TestMetadataFailure(Test, *Session, Handle, Context, TEXT("HandleMetadata.TransactionMismatch"));
	Context = MakeContext(Binding); ++Context.RuntimeRevision;
	TestMetadataFailure(Test, *Session, Handle, Context, TEXT("HandleMetadata.RuntimeRevisionMismatch"));
	Context = MakeContext(Binding); Context.ProviderId = TEXT("blueprint");
	TestMetadataFailure(Test, *Session, Handle, Context, TEXT("HandleMetadata.ProviderMismatch"));
	Context = MakeContext(Binding); Context.RequiredKind = EAssetDocumentHandleKind::Node;
	TestMetadataFailure(Test, *Session, Handle, Context, TEXT("HandleMetadata.KindMismatch"));
	Context = MakeContext(Binding); Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	TestMetadataFailure(Test, *Session, Handle, Context, TEXT("HandleMetadata.AccessMismatch"));
}

void TestCrossSessionProvenance(FAutomationTestBase& Test)
{
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Registry = MakeShared<FAssetDocumentHandleRegistryRuntime>();
	const FAssetDocumentHandleResolverBinding Binding = MakeBinding();
	TUniquePtr<FAssetDocumentInvocationHandleSession> SessionA = Registry->CreateInvocationSession(Binding);
	TUniquePtr<FAssetDocumentInvocationHandleSession> SessionB = Registry->CreateInvocationSession(Binding);
	const FAssetDocumentOpaqueHandle HandleA = IssueObject(*SessionA);
	Test.TestTrue(TEXT("cross-session source remains globally live"), Registry->IsIssuedAndLive(HandleA));
	const FAssetDocumentPrimitiveHandleResult Returned = MakeReturned(HandleA);
	const FAssetDocumentContractValidationResult Mark =
		SessionB->MarkValidatedReturnedHandles(MakeArrayView(&Returned, 1));
	Test.TestFalse(TEXT("cross-session return is rejected"), Mark.bValid);
	Test.TestEqual(TEXT("cross-session return has stable code"), Mark.Diagnostics[0].Code,
		FString(TEXT("AD.RUNTIME.PRIMITIVE_OUTCOME_REJECTED")));
}

void TestSuccessfulCloseSets(FAutomationTestBase& Test)
{
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Registry = MakeShared<FAssetDocumentHandleRegistryRuntime>();
	TUniquePtr<FAssetDocumentInvocationHandleSession> Session = Registry->CreateInvocationSession(MakeBinding());
	const FAssetDocumentOpaqueHandle ReturnedHandle = IssueObject(*Session);
	const FAssetDocumentOpaqueHandle OmittedHandle = IssueObject(*Session);
	const FAssetDocumentPrimitiveHandleResult Returned = MakeReturned(ReturnedHandle);
	Test.TestTrue(TEXT("returned set validates"),
		Session->MarkValidatedReturnedHandles(MakeArrayView(&Returned, 1)).bValid);
	Session->Close(EAssetDocumentInvocationHandleCloseDisposition::Succeeded);
	Test.TestTrue(TEXT("marked return remains live"), Registry->IsIssuedAndLive(ReturnedHandle));
	Test.TestFalse(TEXT("omitted issue becomes stale"), Registry->IsIssuedAndLive(OmittedHandle));
}

void TestFailedAndDestructorClose(FAutomationTestBase& Test)
{
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Registry = MakeShared<FAssetDocumentHandleRegistryRuntime>();
	TUniquePtr<FAssetDocumentInvocationHandleSession> Failed = Registry->CreateInvocationSession(MakeBinding());
	const FAssetDocumentOpaqueHandle FailedHandle = IssueObject(*Failed);
	Failed->Close(EAssetDocumentInvocationHandleCloseDisposition::Failed);
	Test.TestFalse(TEXT("failed close invalidates every issue"), Registry->IsIssuedAndLive(FailedHandle));
	FAssetDocumentOpaqueHandle DestructorHandle;
	{
		TUniquePtr<FAssetDocumentInvocationHandleSession> Unclosed = Registry->CreateInvocationSession(MakeBinding());
		DestructorHandle = IssueObject(*Unclosed);
	}
	Test.TestFalse(TEXT("destructor defaults to failed close"), Registry->IsIssuedAndLive(DestructorHandle));
}

void TestHeldInputLeaseClose(FAutomationTestBase& Test)
{
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Registry = MakeShared<FAssetDocumentHandleRegistryRuntime>();
	const FAssetDocumentHandleResolverBinding Binding = MakeBinding();
	TUniquePtr<FAssetDocumentInvocationHandleSession> Issuer = Registry->CreateInvocationSession(Binding);
	const FAssetDocumentOpaqueHandle Input = IssueObject(*Issuer);
	const FAssetDocumentPrimitiveHandleResult Returned = MakeReturned(Input);
	Test.TestTrue(TEXT("issuer marks returned input"),
		Issuer->MarkValidatedReturnedHandles(MakeArrayView(&Returned, 1)).bValid);
	Issuer->Close(EAssetDocumentInvocationHandleCloseDisposition::Succeeded);
	TUniquePtr<FAssetDocumentInvocationHandleSession> Consumer = Registry->CreateInvocationSession(Binding);
	Test.TestTrue(TEXT("consumer holds live input lease"), Consumer->AcquireAndHoldInput(
		Input, EAssetDocumentHandleKind::Object, EAssetDocumentHandleAccess::Mutable).bValid);
	Consumer->Close(EAssetDocumentInvocationHandleCloseDisposition::Failed);
	Test.TestTrue(TEXT("input identity remains live after lease release"), Registry->IsIssuedAndLive(Input));
}

void TestProviderLeaseCannotRetainPayload(FAutomationTestBase& Test)
{
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Registry = MakeShared<FAssetDocumentHandleRegistryRuntime>();
	const FAssetDocumentHandleResolverBinding Binding = MakeBinding();
	TUniquePtr<FAssetDocumentInvocationHandleSession> Session = Registry->CreateInvocationSession(Binding);
	int32 DestructionCount = 0;
	FAssetDocumentIssuedHandleResult Issued = Session->Issue(
		EAssetDocumentHandleKind::Object,
		EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentHostTestPayload>(true, &DestructionCount));
	check(Issued.Handle.IsSet());
	FAssetDocumentVerifiedLeaseResult Lease =
		Session->AcquireVerifiedLease(Issued.Handle.GetValue(), MakeContext(Binding));
	Test.TestTrue(TEXT("provider lease is acquired before close"), Lease.Lease.IsValid());
	Session->Close(EAssetDocumentInvocationHandleCloseDisposition::Failed);
	Test.TestEqual(TEXT("revoked lease does not retain native payload"), DestructionCount, 1);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentHandleRegistryAllInvalidationEventsTest,
	"UEAssetDoc.HandleRegistry.AllInvalidationEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentHandleRegistryAllInvalidationEventsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestAllInvalidationReasons(*this);
	TestIssueFailures(*this);
	TestMetadataFailures(*this);
	TestCrossSessionProvenance(*this);
	TestSuccessfulCloseSets(*this);
	TestFailedAndDestructorClose(*this);
	TestHeldInputLeaseClose(*this);
	TestProviderLeaseCannotRetainPayload(*this);
	return true;
}
