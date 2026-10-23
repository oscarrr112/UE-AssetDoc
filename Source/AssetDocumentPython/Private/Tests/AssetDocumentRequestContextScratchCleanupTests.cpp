// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentHandleRegistryRuntime.h"
#include "AssetDocumentPrimitiveRegistryRuntime.h"
#include "AssetDocumentPythonHostTestDoubles.h"
#include "AssetDocumentRequestContextRuntime.h"
#include "Misc/AutomationTest.h"

namespace
{
class FAssetDocumentRequestContextScratchPayload final
	: public IAssetDocumentNativeHandlePayload
{
public:
	explicit FAssetDocumentRequestContextScratchPayload(int32* InDestructionCount)
		: DestructionCount(InDestructionCount)
	{
	}

	~FAssetDocumentRequestContextScratchPayload() override
	{
		++*DestructionCount;
	}

	FName GetPayloadTypeId() const override
	{
		return TEXT("host.request_context_scratch_cleanup.payload");
	}

	bool IsAlive() const override
	{
		return true;
	}

private:
	int32* DestructionCount;
};

FAssetDocumentHandleResolverBinding MakeScratchCleanupBinding(
	const FAssetDocumentRequestContextMetadata& Metadata,
	const FGuid& TransactionId)
{
	FAssetDocumentHandleResolverBinding Binding;
	Binding.RequestId = Metadata.RequestId;
	Binding.TransactionId = TransactionId;
	Binding.RuntimeRevision = Metadata.PinnedRuntimeRevision;
	Binding.ProviderId = TEXT("editor");
	Binding.Phase = Metadata.Phase;
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

TSharedPtr<FAssetDocumentPrimitiveRegistryRuntime> MakeFrozenTestRegistry(uint64 Revision)
{
	FAssetDocumentPrimitiveRegistryBuildInput Input;
	Input.RuntimeRevision = Revision;
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeAssetDocumentHostTestDescriptor();
	Input.Required.Add(MakeAssetDocumentHostTestRequirement(Descriptor));
	Input.Providers.Add(MakeShared<FAssetDocumentHostTestProvider>(
		TEXT("editor"), TArray<FAssetDocumentPrimitiveDescriptor>{MoveTemp(Descriptor)}));
	return FAssetDocumentPrimitiveRegistryRuntime::CreateFrozen(Input).Registry;
}

FAssetDocumentOpaqueHandle IssueAndMarkReturned(
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
		MakeUnique<FAssetDocumentRequestContextScratchPayload>(DestructionCount));
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
	FAssetDocumentRequestContextScratchCleanupTest,
	"UEAssetDoc.PythonHost.RequestContextScratchCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRequestContextScratchCleanupTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentRequestContextMetadata Metadata;
	Metadata.RequestId = FGuid::NewGuid();
	Metadata.PinnedRuntimeRevision = 1;
	Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	const FGuid TransactionId = FGuid::NewGuid();
	const TSharedPtr<FAssetDocumentPrimitiveRegistryRuntime> Registry =
		MakeFrozenTestRegistry(Metadata.PinnedRuntimeRevision);
	TestTrue(TEXT("frozen test registry is created"), Registry.IsValid());
	if (!Registry.IsValid())
	{
		return false;
	}
	int32 ScratchDestructionCount = 0;
	int32 ObjectDestructionCount = 0;
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Handles =
		MakeShared<FAssetDocumentHandleRegistryRuntime>();
	const FAssetDocumentHandleResolverBinding Binding =
		MakeScratchCleanupBinding(Metadata, TransactionId);
	const FAssetDocumentOpaqueHandle Scratch = IssueAndMarkReturned(
		*Handles, Binding, EAssetDocumentHandleKind::Scratch, &ScratchDestructionCount);
	const FAssetDocumentOpaqueHandle Object = IssueAndMarkReturned(
		*Handles, Binding, EAssetDocumentHandleKind::Object, &ObjectDestructionCount);

	TestTrue(TEXT("returned Scratch is live before context destruction"),
		Handles->IsIssuedAndLive(Scratch));
	TestTrue(TEXT("returned Object is live before context destruction"),
		Handles->IsIssuedAndLive(Object));
	TUniquePtr<FAssetDocumentNativeRequestContextRuntime> Context =
		MakeUnique<FAssetDocumentNativeRequestContextRuntime>(
			Metadata, TransactionId, Registry.ToSharedRef(), Handles, nullptr);
	Context.Reset();

	TestEqual(TEXT("context destruction destroys Scratch exactly once"),
		ScratchDestructionCount, 1);
	TestFalse(TEXT("context destruction makes Scratch stale"), Handles->IsIssuedAndLive(Scratch));
	TestTrue(TEXT("same-scope Object remains live"), Handles->IsIssuedAndLive(Object));
	TestEqual(TEXT("returned-but-never-consumed Scratch is still closed"),
		ScratchDestructionCount, 1);
	TestEqual(TEXT("context cleanup does not destroy the Object"), ObjectDestructionCount, 0);
	return true;
}
