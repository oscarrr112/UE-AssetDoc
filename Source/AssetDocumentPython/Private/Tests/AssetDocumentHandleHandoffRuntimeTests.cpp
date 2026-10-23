// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentHandleRegistryRuntime.h"
#include "AssetDocumentPrimitiveRegistryRuntime.h"
#include "AssetDocumentPythonHostTestDoubles.h"
#include "AssetDocumentRequestContextRuntime.h"
#include "AssetDocumentRollbackAction.h"

#include "Misc/AutomationTest.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

#include <type_traits>

namespace
{
class FHandleHandoffTestPayload final : public IAssetDocumentNativeHandlePayload
{
public:
	explicit FHandleHandoffTestPayload(TSharedPtr<bool> InLiveness = nullptr)
		: Liveness(MoveTemp(InLiveness))
	{
	}

	FName GetPayloadTypeId() const override
	{
		return TEXT("host.test.handoff-payload");
	}

	bool IsAlive() const override
	{
		return !Liveness.IsValid() || *Liveness;
	}

private:
	TSharedPtr<bool> Liveness;
};

FAssetDocumentContractValidationResult MakeValidHandoffContractResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

class FHandleHandoffTestTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	FHandleHandoffTestTransaction(
		const FGuid& InRequestId,
		const FGuid& InTransactionId,
		uint64 InRuntimeRevision)
	{
		Token.RequestId = InRequestId;
		Token.TransactionId = InTransactionId;
		Token.RuntimeRevision = InRuntimeRevision;
		RollbackActionEnlistmentResult.bValid = true;
		PostRollbackFinalizerEnlistmentResult.bValid = true;
	}

	FGuid GetTransactionId() const override { return Token.TransactionId; }
	EAssetDocumentAtomicState GetState() const override
	{
		return EAssetDocumentAtomicState::Live;
	}
	const FAssetDocumentMutationToken& GetMutationToken() const override
	{
		return Token;
	}
	FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint>) override
	{
		return MakeValidHandoffContractResult();
	}
	FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		return MakeValidHandoffContractResult();
	}
	FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt&) override
	{
		return MakeValidHandoffContractResult();
	}
	FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		return MakeValidHandoffContractResult();
	}
	FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		EnlistedRollbackActionComponentIds.Add(
			FString(Action->GetRollbackComponentId()));
		return RollbackActionEnlistmentResult;
	}
	FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		return PostRollbackFinalizerEnlistmentResult;
	}

	TArray<FString> EnlistedRollbackActionComponentIds;
	FAssetDocumentContractValidationResult RollbackActionEnlistmentResult;
	FAssetDocumentContractValidationResult PostRollbackFinalizerEnlistmentResult;

private:
	FAssetDocumentMutationToken Token;
};

class FHandleHandoffTestRollbackAction final
	: public IAssetDocumentRollbackAction
{
public:
	virtual FStringView GetRollbackComponentId() const override
	{
		return TEXTVIEW("rollback.test-handle-handoff");
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView) override
	{
		return MakeValidHandoffContractResult();
	}
};

void TestTransactionRollbackActionEnlistment(FAutomationTestBase& Test)
{
	FHandleHandoffTestTransaction Transaction(
		FGuid::NewGuid(), FGuid::NewGuid(), 41);
	Transaction.RollbackActionEnlistmentResult.bValid = false;
	FAssetDocumentRuntimeDiagnostic& ExpectedDiagnostic =
		Transaction.RollbackActionEnlistmentResult.Diagnostics.AddDefaulted_GetRef();
	ExpectedDiagnostic.Code = TEXT("Test.HandleHandoff.RollbackActionRejected");

	const FAssetDocumentContractValidationResult Actual =
		Transaction.EnlistRollbackAction(
			MakeShared<FHandleHandoffTestRollbackAction>());
	Test.TestFalse(
		TEXT("handle-handoff fake returns configured action-enlistment result"),
		Actual.bValid);
	Test.TestEqual(
		TEXT("handle-handoff fake preserves configured diagnostic"),
		Actual.Diagnostics.Num() == 1 ? Actual.Diagnostics[0].Code : FString(),
		FString(TEXT("Test.HandleHandoff.RollbackActionRejected")));
	Test.TestEqual(
		TEXT("handle-handoff fake records rollback component ID"),
		Transaction.EnlistedRollbackActionComponentIds,
		TArray<FString>({TEXT("rollback.test-handle-handoff")}));
}

FAssetDocumentHandleResolverBinding MakeHandoffBinding(FStringView ProviderId)
{
	FAssetDocumentHandleResolverBinding Binding;
	Binding.RequestId = FGuid(1, 2, 3, 4);
	Binding.TransactionId = FGuid(5, 6, 7, 8);
	Binding.RuntimeRevision = 41;
	Binding.ProviderId = ProviderId;
	Binding.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	FAssetDocumentPrimitiveHandleResultSchema& Schema =
		Binding.AllowedResults.AddDefaulted_GetRef();
	Schema.Name = TEXT("asset");
	Schema.Kind = EAssetDocumentHandleKind::Asset;
	Schema.GrantedAccess = EAssetDocumentHandleAccess::Mutable;
	Schema.Cardinality = EAssetDocumentHandleResultCardinality::ExactlyOne;
	return Binding;
}

FAssetDocumentHandleValidationContext MakeHandoffContext(
	const FAssetDocumentHandleResolverBinding& Binding,
	EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::Mutable)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Binding.RequestId;
	Context.TransactionId = Binding.TransactionId;
	Context.RuntimeRevision = Binding.RuntimeRevision;
	Context.ProviderId = Binding.ProviderId;
	Context.Phase = Binding.Phase;
	Context.RequiredKind = EAssetDocumentHandleKind::Asset;
	Context.RequiredAccess = Access;
	return Context;
}

FAssetDocumentOpaqueHandle IssueHandoffRetainedAsset(
	FAssetDocumentHandleRegistryRuntime& Registry,
	const FAssetDocumentHandleResolverBinding& Binding,
	TSharedPtr<bool> Liveness = nullptr,
	EAssetDocumentHandleAccess Access =
		EAssetDocumentHandleAccess::Mutable)
{
	TUniquePtr<FAssetDocumentInvocationHandleSession> Session =
		Registry.CreateInvocationSession(Binding);
	FAssetDocumentIssuedHandleResult Issued = Session->Issue(
		EAssetDocumentHandleKind::Asset,
		Access,
		MakeUnique<FHandleHandoffTestPayload>(MoveTemp(Liveness)));
	check(Issued.Handle.IsSet());
	FAssetDocumentPrimitiveHandleResult Returned;
	Returned.Name = TEXT("asset");
	Returned.Handles.Add(Issued.Handle.GetValue());
	check(Session->MarkValidatedReturnedHandles(
		MakeArrayView(&Returned, 1)).bValid);
	Session->Close(EAssetDocumentInvocationHandleCloseDisposition::Succeeded);
	return Issued.Handle.GetValue();
}

FAssetDocumentOpaqueHandle HandoffRetainedAsset(
	FAssetDocumentHandleRegistryRuntime& Registry,
	const FAssetDocumentOpaqueHandle& Source,
	const FAssetDocumentHandleResolverBinding& SourceBinding,
	FStringView TargetProviderId)
{
	const FAssetDocumentIssuedHandleResult Handoff = Registry.Handoff(
		Source, MakeHandoffContext(SourceBinding), TargetProviderId,
		EAssetDocumentHandleAccess::Mutable);
	check(Handoff.Handle.IsSet());
	return Handoff.Handle.GetValue();
}

TSharedPtr<FAssetDocumentPrimitiveRegistryRuntime> MakeHandoffPrimitiveRegistry()
{
	FAssetDocumentPrimitiveDescriptor Editor =
		MakeAssetDocumentHostTestDescriptor();
	FAssetDocumentPrimitiveDescriptor Animation =
		MakeAssetDocumentHostTestDescriptor(
			TEXT("animation.host_test.echo"),
			1,
			TEXT("host-test-animation-echo-v1"));
	Animation.ProviderId = TEXT("animation");
	Animation.Owner = EAssetDocumentPrimitiveOwner::Animation;
	Animation.Admission.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;

	FAssetDocumentPrimitiveRegistryBuildInput Input;
	Input.RuntimeRevision = 41;
	Input.Required = {
		MakeAssetDocumentHostTestRequirement(Editor),
		MakeAssetDocumentHostTestRequirement(Animation),
	};
	Input.Providers = {
		MakeShared<FAssetDocumentHostTestProvider>(
			TEXT("editor"),
			TArray<FAssetDocumentPrimitiveDescriptor>{MoveTemp(Editor)}),
		MakeShared<FAssetDocumentHostTestProvider>(
			TEXT("animation"),
			TArray<FAssetDocumentPrimitiveDescriptor>{MoveTemp(Animation)}),
	};
	return FAssetDocumentPrimitiveRegistryRuntime::CreateFrozen(Input).Registry;
}

FString FirstDiagnosticCode(const FAssetDocumentIssuedHandleResult& Result)
{
	return Result.Diagnostics.IsEmpty()
		? FString()
		: Result.Diagnostics[0].Code;
}

bool IsHandoffTransactionInvalidation(
	EAssetDocumentHandleInvalidationReason Reason)
{
	return Reason == EAssetDocumentHandleInvalidationReason::Commit
		|| Reason == EAssetDocumentHandleInvalidationReason::Rollback;
}

FAssetDocumentHandleInvalidation MakeHandoffInvalidation(
	EAssetDocumentHandleInvalidationReason Reason,
	const FAssetDocumentHandleResolverBinding& Binding)
{
	FAssetDocumentHandleInvalidation Event;
	Event.Reason = Reason;
	if (IsHandoffTransactionInvalidation(Reason))
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

template <typename ResolverType>
TOptional<uint32> InvalidateAndReadCount(
	ResolverType& Resolver,
	const FAssetDocumentHandleInvalidation& Event)
{
	using TResult = decltype(Resolver.Invalidate(Event));
	if constexpr (std::is_void_v<TResult>)
	{
		Resolver.Invalidate(Event);
		return NullOpt;
	}
	else
	{
		return Resolver.Invalidate(Event).InvalidatedHandleCount;
	}
}

void TestContextAdmissionRejections(FAutomationTestBase& Test)
{
	const FAssetDocumentHandleResolverBinding Editor =
		MakeHandoffBinding(TEXTVIEW("editor"));
	const TSharedPtr<FAssetDocumentPrimitiveRegistryRuntime> PrimitiveRegistry =
		MakeHandoffPrimitiveRegistry();
	Test.TestTrue(TEXT("handoff provider registry freezes"),
		PrimitiveRegistry.IsValid());
	if (!PrimitiveRegistry.IsValid())
	{
		return;
	}
	TSharedRef<FAssetDocumentHandleRegistryRuntime> Handles =
		MakeShared<FAssetDocumentHandleRegistryRuntime>();
	const FAssetDocumentOpaqueHandle MutableSource =
		IssueHandoffRetainedAsset(*Handles, Editor);
	FAssetDocumentRequestContextMetadata Metadata;
	Metadata.RequestId = Editor.RequestId;
	Metadata.PinnedRuntimeRevision = Editor.RuntimeRevision;
	Metadata.Phase = Editor.Phase;
	FHandleHandoffTestTransaction Transaction(
		Editor.RequestId, Editor.TransactionId, Editor.RuntimeRevision);
	FAssetDocumentNativeRequestContextRuntime Context(
		Metadata,
		Editor.TransactionId,
		PrimitiveRegistry.ToSharedRef(),
		Handles,
		&Transaction);

	const FAssetDocumentIssuedHandleResult Accepted = Context.Handoff(
		MutableSource,
		TEXTVIEW("animation"),
		EAssetDocumentHandleAccess::Mutable);
	Test.TestTrue(TEXT("valid mutable handoff is admitted"),
		Accepted.Handle.IsSet());
	const FAssetDocumentIssuedHandleResult Downgraded = Context.Handoff(
		MutableSource,
		TEXTVIEW("animation"),
		EAssetDocumentHandleAccess::ReadOnly);
	Test.TestTrue(TEXT("mutable source may hand off as read-only"),
		Downgraded.Handle.IsSet());
	if (Downgraded.Handle.IsSet())
	{
		Test.TestTrue(TEXT("downgraded target is read-only"),
			Downgraded.Handle.GetValue().Access
				== EAssetDocumentHandleAccess::ReadOnly);
	}

	FAssetDocumentHandleResolverBinding ReadOnlyEditor = Editor;
	ReadOnlyEditor.AllowedResults[0].GrantedAccess =
		EAssetDocumentHandleAccess::ReadOnly;
	const FAssetDocumentOpaqueHandle IssuedReadOnlySource =
		IssueHandoffRetainedAsset(
			*Handles,
			ReadOnlyEditor,
			nullptr,
			EAssetDocumentHandleAccess::ReadOnly);
	struct FAllowedReadOnlyPhase
	{
		EAssetDocumentRuntimeRequestPhase Phase;
		const TCHAR* Name;
	};
	static constexpr FAllowedReadOnlyPhase AllowedReadOnlyPhases[] = {
		{EAssetDocumentRuntimeRequestPhase::Preflight, TEXT("Preflight")},
		{EAssetDocumentRuntimeRequestPhase::Apply, TEXT("Apply")},
		{EAssetDocumentRuntimeRequestPhase::Repair, TEXT("Repair")},
		{EAssetDocumentRuntimeRequestPhase::Verify, TEXT("Verify")},
	};
	for (const FAllowedReadOnlyPhase& Allowed : AllowedReadOnlyPhases)
	{
		FAssetDocumentRequestContextMetadata ReadOnlyMetadata = Metadata;
		ReadOnlyMetadata.Phase = Allowed.Phase;
		FAssetDocumentNativeRequestContextRuntime ReadOnlyContext(
			ReadOnlyMetadata,
			Editor.TransactionId,
			PrimitiveRegistry.ToSharedRef(),
			Handles,
			nullptr);
		const FAssetDocumentIssuedHandleResult ReadOnlyHandoff =
			ReadOnlyContext.Handoff(
				IssuedReadOnlySource,
				TEXTVIEW("animation"),
				EAssetDocumentHandleAccess::ReadOnly);
		Test.TestTrue(
			FString::Printf(
				TEXT("issued read-only source is admitted in %s"),
				Allowed.Name),
			ReadOnlyHandoff.Handle.IsSet());
		if (ReadOnlyHandoff.Handle.IsSet())
		{
			Test.TestTrue(
				FString::Printf(
					TEXT("%s handoff target remains read-only"),
					Allowed.Name),
				ReadOnlyHandoff.Handle.GetValue().Access
					== EAssetDocumentHandleAccess::ReadOnly);
		}
	}


	FAssetDocumentRequestContextMetadata VerifyMetadata = Metadata;
	VerifyMetadata.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
	FAssetDocumentNativeRequestContextRuntime VerifyContext(
		VerifyMetadata, Editor.TransactionId, PrimitiveRegistry.ToSharedRef(),
		Handles, &Transaction);
	Test.TestEqual(TEXT("Verify rejects mutable even with matching live transaction"),
		FirstDiagnosticCode(VerifyContext.Handoff(
			MutableSource, TEXTVIEW("animation"), EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("AD.RUNTIME.HANDOFF_TRANSACTION_REQUIRED")));
	Test.TestTrue(TEXT("Verify admits mutable-to-read-only downgrade"),
		VerifyContext.Handoff(MutableSource, TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::ReadOnly).Handle.IsSet());

	FAssetDocumentOpaqueHandle ReadOnlySource = MutableSource;
	ReadOnlySource.Access = EAssetDocumentHandleAccess::ReadOnly;
	Test.TestEqual(TEXT("read-only escalation code"),
		FirstDiagnosticCode(Context.Handoff(
			ReadOnlySource,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("AD.RUNTIME.HANDOFF_ACCESS_ESCALATION")));

	Test.TestEqual(TEXT("empty target code"),
		FirstDiagnosticCode(Context.Handoff(
			MutableSource, TEXTVIEW(""), EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));
	Test.TestEqual(TEXT("same target code"),
		FirstDiagnosticCode(Context.Handoff(
			MutableSource,
			TEXTVIEW("editor"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));
	Test.TestEqual(TEXT("missing target code"),
		FirstDiagnosticCode(Context.Handoff(
			MutableSource,
			TEXTVIEW("missing"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("AD.RUNTIME.HANDOFF_TARGET_UNAVAILABLE")));

	FAssetDocumentOpaqueHandle WrongKind = MutableSource;
	WrongKind.Kind = EAssetDocumentHandleKind::Object;
	Test.TestEqual(TEXT("non-Asset kind code"),
		FirstDiagnosticCode(Context.Handoff(
			WrongKind,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::ReadOnly)),
		FString(TEXT("AD.RUNTIME.HANDOFF_KIND_UNSUPPORTED")));

	FAssetDocumentOpaqueHandle WrongRequest = MutableSource;
	WrongRequest.RequestId = FGuid::NewGuid();
	Test.TestEqual(TEXT("request mismatch is preserved"),
		FirstDiagnosticCode(Context.Handoff(
			WrongRequest,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("HandleMetadata.RequestMismatch")));
	FAssetDocumentOpaqueHandle WrongTransactionId = MutableSource;
	WrongTransactionId.TransactionId = FGuid::NewGuid();
	Test.TestEqual(TEXT("source transaction mismatch is preserved"),
		FirstDiagnosticCode(Context.Handoff(
			WrongTransactionId,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("HandleMetadata.TransactionMismatch")));
	FAssetDocumentOpaqueHandle WrongRevision = MutableSource;
	++WrongRevision.RuntimeRevision;
	Test.TestEqual(TEXT("revision mismatch is preserved"),
		FirstDiagnosticCode(Context.Handoff(
			WrongRevision,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("HandleMetadata.RuntimeRevisionMismatch")));
	FAssetDocumentOpaqueHandle StaleGeneration = MutableSource;
	++StaleGeneration.Generation;
	Test.TestEqual(TEXT("stale generation remains stale"),
		FirstDiagnosticCode(Context.Handoff(
			StaleGeneration,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("AD.RUNTIME.STALE_HANDLE")));
	FAssetDocumentOpaqueHandle StaleSlot = MutableSource;
	++StaleSlot.Slot;
	Test.TestEqual(TEXT("stale slot remains stale"),
		FirstDiagnosticCode(Context.Handoff(
			StaleSlot,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("AD.RUNTIME.STALE_HANDLE")));
	Test.TestEqual(TEXT("invalid requested access is rejected"),
		FirstDiagnosticCode(Context.Handoff(
			MutableSource,
			TEXTVIEW("animation"),
			static_cast<EAssetDocumentHandleAccess>(2))),
		FString(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));

	FAssetDocumentHandleValidationContext WrongProviderContext =
		MakeHandoffContext(Editor);
	WrongProviderContext.ProviderId = TEXT("animation");
	Test.TestEqual(TEXT("provider mismatch is preserved"),
		FirstDiagnosticCode(Handles->Handoff(
			MutableSource,
			WrongProviderContext,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("HandleMetadata.ProviderMismatch")));

	FAssetDocumentNativeRequestContextRuntime MissingTransactionContext(
		Metadata,
		Editor.TransactionId,
		PrimitiveRegistry.ToSharedRef(),
		Handles,
		nullptr);
	Test.TestEqual(TEXT("mutable handoff requires transaction"),
		FirstDiagnosticCode(MissingTransactionContext.Handoff(
			MutableSource,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("AD.RUNTIME.HANDOFF_TRANSACTION_REQUIRED")));
	FHandleHandoffTestTransaction WrongTransaction(
		Editor.RequestId, FGuid::NewGuid(), Editor.RuntimeRevision);
	FAssetDocumentNativeRequestContextRuntime WrongTransactionContext(
		Metadata,
		Editor.TransactionId,
		PrimitiveRegistry.ToSharedRef(),
		Handles,
		&WrongTransaction);
	Test.TestEqual(TEXT("mutable handoff rejects wrong transaction"),
		FirstDiagnosticCode(WrongTransactionContext.Handoff(
			MutableSource,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("AD.RUNTIME.HANDOFF_TRANSACTION_REQUIRED")));

	FAssetDocumentRequestContextMetadata ValidateMetadata = Metadata;
	ValidateMetadata.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	FAssetDocumentNativeRequestContextRuntime ValidateContext(
		ValidateMetadata,
		Editor.TransactionId,
		PrimitiveRegistry.ToSharedRef(),
		Handles,
		&Transaction);
	Test.TestEqual(TEXT("invalid handoff phase code"),
		FirstDiagnosticCode(ValidateContext.Handoff(
			MutableSource,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::ReadOnly)),
		FString(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));
	Test.TestEqual(TEXT("Validate rejects read-only source with active transaction"),
		FirstDiagnosticCode(ValidateContext.Handoff(
			IssuedReadOnlySource, TEXTVIEW("animation"), EAssetDocumentHandleAccess::ReadOnly)),
		FString(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));
	FAssetDocumentNativeRequestContextRuntime ValidateReadOnlyContext(
		ValidateMetadata, Editor.TransactionId, PrimitiveRegistry.ToSharedRef(), Handles, nullptr);
	const auto ValidatedReadOnly = ValidateReadOnlyContext.Handoff(
		IssuedReadOnlySource, TEXTVIEW("animation"), EAssetDocumentHandleAccess::ReadOnly);
	Test.TestTrue(TEXT("Validate admits an actual read-only Asset source without transaction"), ValidatedReadOnly.Handle.IsSet());
	if (ValidatedReadOnly.Handle.IsSet())
	{
		const auto& Alias = ValidatedReadOnly.Handle.GetValue();
		Test.TestEqual(TEXT("Validate alias remains read-only"), Alias.Access, EAssetDocumentHandleAccess::ReadOnly);
		Test.TestEqual(TEXT("Validate alias preserves request"), Alias.RequestId, IssuedReadOnlySource.RequestId);
		Test.TestEqual(TEXT("Validate alias preserves revision"), Alias.RuntimeRevision, IssuedReadOnlySource.RuntimeRevision);
		Test.TestNotEqual(TEXT("Validate alias owns a fresh slot"), Alias.Slot, IssuedReadOnlySource.Slot);
	}
	Test.TestEqual(TEXT("Validate does not admit mutable-source downgrade"),
		FirstDiagnosticCode(ValidateReadOnlyContext.Handoff(
			MutableSource, TEXTVIEW("animation"), EAssetDocumentHandleAccess::ReadOnly)),
		FString(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));
	Test.TestEqual(TEXT("Validate does not admit read-only-source upgrade"),
		FirstDiagnosticCode(ValidateReadOnlyContext.Handoff(
			IssuedReadOnlySource, TEXTVIEW("animation"), EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT")));
	FAssetDocumentOpaqueHandle WrongValidateRequest = IssuedReadOnlySource;
	WrongValidateRequest.RequestId = FGuid::NewGuid();
	Test.TestEqual(TEXT("Validate still enforces source request identity"),
		FirstDiagnosticCode(ValidateReadOnlyContext.Handoff(
			WrongValidateRequest, TEXTVIEW("animation"), EAssetDocumentHandleAccess::ReadOnly)),
		FString(TEXT("HandleMetadata.RequestMismatch")));

	TSharedRef<bool> SourceLiveness = MakeShared<bool>(true);
	const FAssetDocumentOpaqueHandle DeadSource = IssueHandoffRetainedAsset(
		*Handles, Editor, SourceLiveness);
	*SourceLiveness = false;
	Test.TestEqual(TEXT("dead source payload remains stale"),
		FirstDiagnosticCode(Context.Handoff(
			DeadSource,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("AD.RUNTIME.STALE_HANDLE")));

	Handles->ExhaustSlotSpaceForTesting();
	Test.TestEqual(TEXT("slot exhaustion uses stable handoff diagnostic"),
		FirstDiagnosticCode(Context.Handoff(
			MutableSource,
			TEXTVIEW("animation"),
			EAssetDocumentHandleAccess::Mutable)),
		FString(TEXT("AD.RUNTIME.HANDOFF_STORAGE_REJECTED")));
}

void TestSharedStorageIdentity(FAutomationTestBase& Test)
{
	FAssetDocumentHandleRegistryRuntime Registry;
	const FAssetDocumentHandleResolverBinding Editor =
		MakeHandoffBinding(TEXTVIEW("editor"));
	const FAssetDocumentHandleResolverBinding Animation =
		MakeHandoffBinding(TEXTVIEW("animation"));
	const FAssetDocumentOpaqueHandle Source =
		IssueHandoffRetainedAsset(Registry, Editor);
	const FAssetDocumentIssuedHandleResult Handoff = Registry.Handoff(
		Source,
		MakeHandoffContext(Editor),
		TEXTVIEW("animation"),
		EAssetDocumentHandleAccess::Mutable);
	Test.TestTrue(TEXT("handoff issues a target handle"), Handoff.Handle.IsSet());
	if (!Handoff.Handle.IsSet())
	{
		return;
	}
	const FAssetDocumentOpaqueHandle Target = Handoff.Handle.GetValue();
	Test.TestEqual(TEXT("target provider is fresh"), Target.ProviderId,
		FString(TEXT("animation")));
	Test.TestNotEqual(TEXT("target slot is fresh"), Target.Slot, Source.Slot);
	Test.TestEqual(TEXT("target generation starts at one"), Target.Generation, uint32{1});

	TUniquePtr<FAssetDocumentInvocationHandleSession> SourceSession =
		Registry.CreateInvocationSession(Editor);
	TUniquePtr<FAssetDocumentInvocationHandleSession> TargetSession =
		Registry.CreateInvocationSession(Animation);
	FAssetDocumentVerifiedLeaseResult SourceLease =
		SourceSession->AcquireVerifiedLease(Source, MakeHandoffContext(Editor));
	FAssetDocumentVerifiedLeaseResult TargetLease =
		TargetSession->AcquireVerifiedLease(
			Target, MakeHandoffContext(Animation));
	Test.TestTrue(TEXT("source lease remains live"), SourceLease.Lease.IsValid());
	Test.TestTrue(TEXT("target lease is live"), TargetLease.Lease.IsValid());
	if (SourceLease.Lease.IsValid() && TargetLease.Lease.IsValid())
	{
		Test.TestTrue(TEXT("source and target share exact native payload"),
			&SourceLease.Lease->GetPayloadChecked()
				== &TargetLease.Lease->GetPayloadChecked());
	}
}

void TestTerminalInvalidationReason(
	FAutomationTestBase& Test,
	EAssetDocumentHandleInvalidationReason Reason)
{
	FAssetDocumentHandleRegistryRuntime Registry;
	const FAssetDocumentHandleResolverBinding Editor =
		MakeHandoffBinding(TEXTVIEW("editor"));
	const FAssetDocumentHandleResolverBinding Animation =
		MakeHandoffBinding(TEXTVIEW("animation"));
	const FAssetDocumentHandleResolverBinding Render =
		MakeHandoffBinding(TEXTVIEW("render"));
	const FAssetDocumentOpaqueHandle Source =
		IssueHandoffRetainedAsset(Registry, Editor);
	const FAssetDocumentIssuedHandleResult FirstHandoff = Registry.Handoff(
		Source, MakeHandoffContext(Editor), TEXTVIEW("animation"),
		EAssetDocumentHandleAccess::Mutable);
	check(FirstHandoff.Handle.IsSet());
	const FAssetDocumentOpaqueHandle Target = FirstHandoff.Handle.GetValue();
	const FAssetDocumentIssuedHandleResult SecondHandoff = Registry.Handoff(
		Target, MakeHandoffContext(Animation), TEXTVIEW("render"),
		EAssetDocumentHandleAccess::Mutable);
	check(SecondHandoff.Handle.IsSet());
	const FAssetDocumentOpaqueHandle Descendant = SecondHandoff.Handle.GetValue();

	TUniquePtr<FAssetDocumentInvocationHandleSession> SourceSession =
		Registry.CreateInvocationSession(Editor);
	TUniquePtr<FAssetDocumentInvocationHandleSession> TargetSession =
		Registry.CreateInvocationSession(Animation);
	TUniquePtr<FAssetDocumentInvocationHandleSession> DescendantSession =
		Registry.CreateInvocationSession(Render);
	FAssetDocumentVerifiedLeaseResult SourceLease =
		SourceSession->AcquireVerifiedLease(Source, MakeHandoffContext(Editor));
	FAssetDocumentVerifiedLeaseResult TargetLease =
		TargetSession->AcquireVerifiedLease(Target, MakeHandoffContext(Animation));
	FAssetDocumentVerifiedLeaseResult DescendantLease =
		DescendantSession->AcquireVerifiedLease(
			Descendant, MakeHandoffContext(Render));
	Test.TestTrue(TEXT("terminal source lease starts live"),
		SourceLease.Lease.IsValid()
			&& Registry.HasLiveLeaseForTesting(Source));
	Test.TestTrue(TEXT("terminal target lease starts live"),
		TargetLease.Lease.IsValid()
			&& Registry.HasLiveLeaseForTesting(Target));
	Test.TestTrue(TEXT("terminal descendant lease starts live"),
		DescendantLease.Lease.IsValid()
			&& Registry.HasLiveLeaseForTesting(Descendant));

	Registry.Invalidate(MakeHandoffInvalidation(Reason, Editor));
	Test.TestFalse(TEXT("terminal invalidation retires source"),
		Registry.IsIssuedAndLive(Source));
	Test.TestFalse(TEXT("terminal invalidation retires target"),
		Registry.IsIssuedAndLive(Target));
	Test.TestFalse(TEXT("terminal invalidation retires transitive descendant"),
		Registry.IsIssuedAndLive(Descendant));
	Test.TestFalse(TEXT("terminal invalidation revokes source lease"),
		Registry.HasLiveLeaseForTesting(Source));
	Test.TestFalse(TEXT("terminal invalidation revokes target lease"),
		Registry.HasLiveLeaseForTesting(Target));
	Test.TestFalse(TEXT("terminal invalidation revokes descendant lease"),
		Registry.HasLiveLeaseForTesting(Descendant));
}

void TestAllTerminalInvalidations(FAutomationTestBase& Test)
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
		TestTerminalInvalidationReason(Test, Reason);
	}
}

void TestTargetOnlyProviderUnload(FAutomationTestBase& Test)
{
	FAssetDocumentHandleRegistryRuntime Registry;
	const FAssetDocumentHandleResolverBinding Editor =
		MakeHandoffBinding(TEXTVIEW("editor"));
	const FAssetDocumentHandleResolverBinding Animation =
		MakeHandoffBinding(TEXTVIEW("animation"));
	const FAssetDocumentOpaqueHandle Source =
		IssueHandoffRetainedAsset(Registry, Editor);
	const FAssetDocumentIssuedHandleResult FirstHandoff = Registry.Handoff(
		Source, MakeHandoffContext(Editor), TEXTVIEW("animation"),
		EAssetDocumentHandleAccess::Mutable);
	check(FirstHandoff.Handle.IsSet());
	const FAssetDocumentOpaqueHandle Target = FirstHandoff.Handle.GetValue();
	const FAssetDocumentIssuedHandleResult SecondHandoff = Registry.Handoff(
		Target, MakeHandoffContext(Animation), TEXTVIEW("render"),
		EAssetDocumentHandleAccess::Mutable);
	check(SecondHandoff.Handle.IsSet());
	const FAssetDocumentOpaqueHandle Descendant = SecondHandoff.Handle.GetValue();
	TUniquePtr<FAssetDocumentInvocationHandleSession> SourceSession =
		Registry.CreateInvocationSession(Editor);
	TUniquePtr<FAssetDocumentInvocationHandleSession> TargetSession =
		Registry.CreateInvocationSession(Animation);
	FAssetDocumentVerifiedLeaseResult SourceLease =
		SourceSession->AcquireVerifiedLease(Source, MakeHandoffContext(Editor));
	FAssetDocumentVerifiedLeaseResult TargetLease =
		TargetSession->AcquireVerifiedLease(Target, MakeHandoffContext(Animation));

	FAssetDocumentHandleInvalidation TargetUnload;
	TargetUnload.Reason = EAssetDocumentHandleInvalidationReason::ProviderUnload;
	TargetUnload.ProviderId = TEXT("animation");
	Registry.Invalidate(TargetUnload);
	Test.TestTrue(TEXT("target-only unload preserves source"),
		Registry.IsIssuedAndLive(Source));
	Test.TestFalse(TEXT("target-only unload retires target"),
		Registry.IsIssuedAndLive(Target));
	Test.TestFalse(TEXT("target-only unload retires target descendant"),
		Registry.IsIssuedAndLive(Descendant));
	Test.TestTrue(TEXT("target-only unload preserves source lease"),
		SourceLease.Lease.IsValid()
			&& Registry.HasLiveLeaseForTesting(Source));
	Test.TestFalse(TEXT("target-only unload revokes target lease"),
		TargetLease.Lease.IsValid()
			&& Registry.HasLiveLeaseForTesting(Target));
}

void TestExplicitSourceRetirement(FAutomationTestBase& Test)
{
	FAssetDocumentHandleRegistryRuntime Registry;
	const FAssetDocumentHandleResolverBinding Editor =
		MakeHandoffBinding(TEXTVIEW("editor"));
	const FAssetDocumentHandleResolverBinding Animation =
		MakeHandoffBinding(TEXTVIEW("animation"));
	TUniquePtr<FAssetDocumentInvocationHandleSession> IssuingSession =
		Registry.CreateInvocationSession(Editor);
	FAssetDocumentIssuedHandleResult Issued = IssuingSession->Issue(
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FHandleHandoffTestPayload>());
	check(Issued.Handle.IsSet());
	const FAssetDocumentOpaqueHandle Source = Issued.Handle.GetValue();
	const FAssetDocumentIssuedHandleResult Handoff = Registry.Handoff(
		Source, MakeHandoffContext(Editor), TEXTVIEW("animation"),
		EAssetDocumentHandleAccess::Mutable);
	check(Handoff.Handle.IsSet());
	const FAssetDocumentOpaqueHandle Target = Handoff.Handle.GetValue();
	TUniquePtr<FAssetDocumentInvocationHandleSession> SourceSession =
		Registry.CreateInvocationSession(Editor);
	TUniquePtr<FAssetDocumentInvocationHandleSession> TargetSession =
		Registry.CreateInvocationSession(Animation);
	FAssetDocumentVerifiedLeaseResult SourceLease =
		SourceSession->AcquireVerifiedLease(Source, MakeHandoffContext(Editor));
	FAssetDocumentVerifiedLeaseResult TargetLease =
		TargetSession->AcquireVerifiedLease(Target, MakeHandoffContext(Animation));
	Test.TestTrue(TEXT("explicit retirement source lease starts live"),
		SourceLease.Lease.IsValid());
	Test.TestTrue(TEXT("explicit retirement target lease starts live"),
		TargetLease.Lease.IsValid());

	IssuingSession->Close(EAssetDocumentInvocationHandleCloseDisposition::Failed);
	Test.TestFalse(TEXT("explicit source retirement retires source"),
		Registry.IsIssuedAndLive(Source));
	Test.TestFalse(TEXT("explicit source retirement cascades to target"),
		Registry.IsIssuedAndLive(Target));
	Test.TestFalse(TEXT("explicit source retirement revokes source lease"),
		Registry.HasLiveLeaseForTesting(Source));
	Test.TestFalse(TEXT("explicit source retirement revokes target lease"),
		Registry.HasLiveLeaseForTesting(Target));
}

void TestInvalidationResult(FAutomationTestBase& Test)
{
	const FAssetDocumentHandleResolverBinding Editor =
		MakeHandoffBinding(TEXTVIEW("editor"));
	const FAssetDocumentHandleResolverBinding Animation =
		MakeHandoffBinding(TEXTVIEW("animation"));
	const FAssetDocumentHandleResolverBinding Render =
		MakeHandoffBinding(TEXTVIEW("render"));

	FAssetDocumentHandleRegistryRuntime DirectRegistry;
	const FAssetDocumentOpaqueHandle Direct =
		IssueHandoffRetainedAsset(DirectRegistry, Editor);
	TUniquePtr<FAssetDocumentInvocationHandleSession> DirectLeaseSession =
		DirectRegistry.CreateInvocationSession(Editor);
	FAssetDocumentVerifiedLeaseResult DirectLease =
		DirectLeaseSession->AcquireVerifiedLease(Direct, MakeHandoffContext(Editor));
	Test.TestTrue(TEXT("direct match lease starts live"),
		DirectLease.Lease.IsValid()
			&& DirectRegistry.HasLiveLeaseForTesting(Direct));
	TUniquePtr<FAssetDocumentInvocationHandleSession> DirectInvalidatingSession =
		DirectRegistry.CreateInvocationSession(Editor);
	const TOptional<uint32> DirectCount = InvalidateAndReadCount(
		*DirectInvalidatingSession,
		MakeHandoffInvalidation(
			EAssetDocumentHandleInvalidationReason::Compile, Editor));
	Test.TestFalse(TEXT("direct match retires the live slot"),
		DirectRegistry.IsIssuedAndLive(Direct));
	Test.TestFalse(TEXT("direct match revokes its lease"),
		DirectRegistry.HasLiveLeaseForTesting(Direct));
	const FAssetDocumentOpaqueHandle Fresh =
		IssueHandoffRetainedAsset(DirectRegistry, Editor);
	Test.TestTrue(TEXT("fresh issuance receives a nonzero new slot"),
		Fresh.Slot != 0 && Fresh.Slot != Direct.Slot);
	Test.TestTrue(TEXT("fresh issuance remains live after completed invalidation"),
		DirectRegistry.IsIssuedAndLive(Fresh));

	FAssetDocumentHandleRegistryRuntime NoMatchRegistry;
	const FAssetDocumentOpaqueHandle Unrelated =
		IssueHandoffRetainedAsset(NoMatchRegistry, Editor);
	TUniquePtr<FAssetDocumentInvocationHandleSession> NoMatchLeaseSession =
		NoMatchRegistry.CreateInvocationSession(Editor);
	FAssetDocumentVerifiedLeaseResult UnrelatedLease =
		NoMatchLeaseSession->AcquireVerifiedLease(
			Unrelated, MakeHandoffContext(Editor));
	Test.TestTrue(TEXT("no-match unrelated lease starts live"),
		UnrelatedLease.Lease.IsValid()
			&& NoMatchRegistry.HasLiveLeaseForTesting(Unrelated));
	FAssetDocumentHandleInvalidation NoMatchEvent = MakeHandoffInvalidation(
		EAssetDocumentHandleInvalidationReason::Compile, Editor);
	NoMatchEvent.RequestId = FGuid(901, 902, 903, 904);
	TUniquePtr<FAssetDocumentInvocationHandleSession> NoMatchInvalidatingSession =
		NoMatchRegistry.CreateInvocationSession(Editor);
	const TOptional<uint32> NoMatchCount = InvalidateAndReadCount(
		*NoMatchInvalidatingSession, NoMatchEvent);
	Test.TestTrue(TEXT("no-match keeps its unrelated slot live"),
		NoMatchRegistry.IsIssuedAndLive(Unrelated));
	Test.TestTrue(TEXT("no-match keeps its unrelated lease live"),
		NoMatchRegistry.HasLiveLeaseForTesting(Unrelated));

	FAssetDocumentHandleRegistryRuntime RepeatedRegistry;
	const FAssetDocumentOpaqueHandle Repeated =
		IssueHandoffRetainedAsset(RepeatedRegistry, Editor);
	TUniquePtr<FAssetDocumentInvocationHandleSession> RepeatedLeaseSession =
		RepeatedRegistry.CreateInvocationSession(Editor);
	FAssetDocumentVerifiedLeaseResult RepeatedLease =
		RepeatedLeaseSession->AcquireVerifiedLease(
			Repeated, MakeHandoffContext(Editor));
	Test.TestTrue(TEXT("repeated invalidation lease starts live"),
		RepeatedLease.Lease.IsValid()
			&& RepeatedRegistry.HasLiveLeaseForTesting(Repeated));
	const FAssetDocumentHandleInvalidation RepeatedEvent =
		MakeHandoffInvalidation(
			EAssetDocumentHandleInvalidationReason::Compile, Editor);
	TUniquePtr<FAssetDocumentInvocationHandleSession> RepeatedInvalidatingSession =
		RepeatedRegistry.CreateInvocationSession(Editor);
	const TOptional<uint32> RepeatedFirstCount = InvalidateAndReadCount(
		*RepeatedInvalidatingSession, RepeatedEvent);
	Test.TestFalse(TEXT("first repeated invalidation retires its slot"),
		RepeatedRegistry.IsIssuedAndLive(Repeated));
	Test.TestFalse(TEXT("first repeated invalidation revokes its lease"),
		RepeatedRegistry.HasLiveLeaseForTesting(Repeated));
	TUniquePtr<FAssetDocumentInvocationHandleSession> StaleLeaseSession =
		RepeatedRegistry.CreateInvocationSession(Editor);
	FAssetDocumentVerifiedLeaseResult StaleLease =
		StaleLeaseSession->AcquireVerifiedLease(
			Repeated, MakeHandoffContext(Editor));
	Test.TestFalse(TEXT("retired generation cannot acquire a lease"),
		StaleLease.Lease.IsValid());
	const TOptional<uint32> RepeatedSecondCount = InvalidateAndReadCount(
		*RepeatedInvalidatingSession, RepeatedEvent);
	Test.TestFalse(TEXT("repeated invalidation does not restore the slot"),
		RepeatedRegistry.IsIssuedAndLive(Repeated));
	Test.TestFalse(TEXT("repeated invalidation does not restore the lease"),
		RepeatedRegistry.HasLiveLeaseForTesting(Repeated));

	FAssetDocumentHandleRegistryRuntime DescendantRegistry;
	const FAssetDocumentOpaqueHandle DescendantSource =
		IssueHandoffRetainedAsset(DescendantRegistry, Editor);
	const FAssetDocumentOpaqueHandle DescendantChild =
		HandoffRetainedAsset(
			DescendantRegistry, DescendantSource, Editor, TEXTVIEW("animation"));
	const FAssetDocumentOpaqueHandle DescendantGrandchild =
		HandoffRetainedAsset(
			DescendantRegistry, DescendantChild, Animation, TEXTVIEW("render"));
	TUniquePtr<FAssetDocumentInvocationHandleSession> DescendantSourceLeaseSession =
		DescendantRegistry.CreateInvocationSession(Editor);
	TUniquePtr<FAssetDocumentInvocationHandleSession> DescendantChildLeaseSession =
		DescendantRegistry.CreateInvocationSession(Animation);
	TUniquePtr<FAssetDocumentInvocationHandleSession> DescendantGrandchildLeaseSession =
		DescendantRegistry.CreateInvocationSession(Render);
	FAssetDocumentVerifiedLeaseResult DescendantSourceLease =
		DescendantSourceLeaseSession->AcquireVerifiedLease(
			DescendantSource, MakeHandoffContext(Editor));
	FAssetDocumentVerifiedLeaseResult DescendantChildLease =
		DescendantChildLeaseSession->AcquireVerifiedLease(
			DescendantChild, MakeHandoffContext(Animation));
	FAssetDocumentVerifiedLeaseResult DescendantGrandchildLease =
		DescendantGrandchildLeaseSession->AcquireVerifiedLease(
			DescendantGrandchild, MakeHandoffContext(Render));
	Test.TestTrue(TEXT("descendant closure leases start live"),
		DescendantSourceLease.Lease.IsValid()
			&& DescendantChildLease.Lease.IsValid()
			&& DescendantGrandchildLease.Lease.IsValid());
	TUniquePtr<FAssetDocumentInvocationHandleSession> DescendantInvalidatingSession =
		DescendantRegistry.CreateInvocationSession(Editor);
	const TOptional<uint32> DescendantCount = InvalidateAndReadCount(
		*DescendantInvalidatingSession,
		MakeHandoffInvalidation(
			EAssetDocumentHandleInvalidationReason::Compile, Editor));
	Test.TestFalse(TEXT("descendant closure retires source"),
		DescendantRegistry.IsIssuedAndLive(DescendantSource));
	Test.TestFalse(TEXT("descendant closure retires child"),
		DescendantRegistry.IsIssuedAndLive(DescendantChild));
	Test.TestFalse(TEXT("descendant closure retires grandchild"),
		DescendantRegistry.IsIssuedAndLive(DescendantGrandchild));
	Test.TestFalse(TEXT("descendant closure revokes all leases"),
		DescendantRegistry.HasLiveLeaseForTesting(DescendantSource)
			|| DescendantRegistry.HasLiveLeaseForTesting(DescendantChild)
			|| DescendantRegistry.HasLiveLeaseForTesting(DescendantGrandchild));

	FAssetDocumentHandleRegistryRuntime TargetUnloadRegistry;
	const FAssetDocumentOpaqueHandle TargetUnloadSource =
		IssueHandoffRetainedAsset(TargetUnloadRegistry, Editor);
	const FAssetDocumentOpaqueHandle TargetUnloadTarget =
		HandoffRetainedAsset(
			TargetUnloadRegistry, TargetUnloadSource, Editor, TEXTVIEW("animation"));
	const FAssetDocumentOpaqueHandle TargetUnloadDescendant =
		HandoffRetainedAsset(
			TargetUnloadRegistry, TargetUnloadTarget, Animation, TEXTVIEW("render"));
	TUniquePtr<FAssetDocumentInvocationHandleSession> TargetUnloadSourceLeaseSession =
		TargetUnloadRegistry.CreateInvocationSession(Editor);
	TUniquePtr<FAssetDocumentInvocationHandleSession> TargetUnloadTargetLeaseSession =
		TargetUnloadRegistry.CreateInvocationSession(Animation);
	TUniquePtr<FAssetDocumentInvocationHandleSession> TargetUnloadDescendantLeaseSession =
		TargetUnloadRegistry.CreateInvocationSession(Render);
	FAssetDocumentVerifiedLeaseResult TargetUnloadSourceLease =
		TargetUnloadSourceLeaseSession->AcquireVerifiedLease(
			TargetUnloadSource, MakeHandoffContext(Editor));
	FAssetDocumentVerifiedLeaseResult TargetUnloadTargetLease =
		TargetUnloadTargetLeaseSession->AcquireVerifiedLease(
			TargetUnloadTarget, MakeHandoffContext(Animation));
	FAssetDocumentVerifiedLeaseResult TargetUnloadDescendantLease =
		TargetUnloadDescendantLeaseSession->AcquireVerifiedLease(
			TargetUnloadDescendant, MakeHandoffContext(Render));
	Test.TestTrue(TEXT("target-only unload leases start live"),
		TargetUnloadSourceLease.Lease.IsValid()
			&& TargetUnloadTargetLease.Lease.IsValid()
			&& TargetUnloadDescendantLease.Lease.IsValid());
	FAssetDocumentHandleInvalidation TargetUnloadEvent;
	TargetUnloadEvent.Reason =
		EAssetDocumentHandleInvalidationReason::ProviderUnload;
	TargetUnloadEvent.ProviderId = TEXT("animation");
	TUniquePtr<FAssetDocumentInvocationHandleSession> TargetUnloadInvalidatingSession =
		TargetUnloadRegistry.CreateInvocationSession(Animation);
	const TOptional<uint32> TargetUnloadCount = InvalidateAndReadCount(
		*TargetUnloadInvalidatingSession, TargetUnloadEvent);
	Test.TestTrue(TEXT("target-only unload keeps source live"),
		TargetUnloadRegistry.IsIssuedAndLive(TargetUnloadSource));
	Test.TestTrue(TEXT("target-only unload keeps source lease live"),
		TargetUnloadRegistry.HasLiveLeaseForTesting(TargetUnloadSource));
	Test.TestFalse(TEXT("target-only unload retires target"),
		TargetUnloadRegistry.IsIssuedAndLive(TargetUnloadTarget));
	Test.TestFalse(TEXT("target-only unload retires descendant"),
		TargetUnloadRegistry.IsIssuedAndLive(TargetUnloadDescendant));
	Test.TestFalse(TEXT("target-only unload revokes target leases"),
		TargetUnloadRegistry.HasLiveLeaseForTesting(TargetUnloadTarget)
			|| TargetUnloadRegistry.HasLiveLeaseForTesting(TargetUnloadDescendant));

	FAssetDocumentHandleRegistryRuntime MixedScopeRegistry;
	FAssetDocumentHandleResolverBinding MixedSourceBinding =
		MakeHandoffBinding(TEXTVIEW("scope-source"));
	MixedSourceBinding.RequestId = FGuid(101, 102, 103, 104);
	MixedSourceBinding.TransactionId = FGuid(201, 202, 203, 204);
	MixedSourceBinding.RuntimeRevision = 73;
	FAssetDocumentHandleResolverBinding MixedChildBinding = MixedSourceBinding;
	MixedChildBinding.ProviderId = TEXT("scope-child");
	FAssetDocumentHandleResolverBinding RequestControlBinding = MixedSourceBinding;
	RequestControlBinding.RequestId = FGuid(301, 302, 303, 304);
	FAssetDocumentHandleResolverBinding TransactionControlBinding = MixedSourceBinding;
	TransactionControlBinding.TransactionId = FGuid(401, 402, 403, 404);
	FAssetDocumentHandleResolverBinding RevisionControlBinding = MixedSourceBinding;
	RevisionControlBinding.RuntimeRevision = 74;
	FAssetDocumentHandleResolverBinding ProviderControlBinding = MixedSourceBinding;
	ProviderControlBinding.ProviderId = TEXT("scope-other-provider");
	const FAssetDocumentOpaqueHandle MixedSource =
		IssueHandoffRetainedAsset(MixedScopeRegistry, MixedSourceBinding);
	const FAssetDocumentOpaqueHandle MixedChild =
		HandoffRetainedAsset(
			MixedScopeRegistry, MixedSource, MixedSourceBinding,
			TEXTVIEW("scope-child"));
	const FAssetDocumentOpaqueHandle RequestControl =
		IssueHandoffRetainedAsset(MixedScopeRegistry, RequestControlBinding);
	const FAssetDocumentOpaqueHandle TransactionControl =
		IssueHandoffRetainedAsset(MixedScopeRegistry, TransactionControlBinding);
	const FAssetDocumentOpaqueHandle RevisionControl =
		IssueHandoffRetainedAsset(MixedScopeRegistry, RevisionControlBinding);
	const FAssetDocumentOpaqueHandle ProviderControl =
		IssueHandoffRetainedAsset(MixedScopeRegistry, ProviderControlBinding);
	TUniquePtr<FAssetDocumentInvocationHandleSession> MixedSourceLeaseSession =
		MixedScopeRegistry.CreateInvocationSession(MixedSourceBinding);
	TUniquePtr<FAssetDocumentInvocationHandleSession> MixedChildLeaseSession =
		MixedScopeRegistry.CreateInvocationSession(MixedChildBinding);
	FAssetDocumentVerifiedLeaseResult MixedSourceLease =
		MixedSourceLeaseSession->AcquireVerifiedLease(
			MixedSource, MakeHandoffContext(MixedSourceBinding));
	FAssetDocumentVerifiedLeaseResult MixedChildLease =
		MixedChildLeaseSession->AcquireVerifiedLease(
			MixedChild, MakeHandoffContext(MixedChildBinding));
	Test.TestTrue(TEXT("mixed scope source and child leases start live"),
		MixedSourceLease.Lease.IsValid() && MixedChildLease.Lease.IsValid());
	FAssetDocumentHandleInvalidation MixedScopeEvent;
	MixedScopeEvent.Reason = EAssetDocumentHandleInvalidationReason::Commit;
	MixedScopeEvent.RequestId = MixedSourceBinding.RequestId;
	MixedScopeEvent.TransactionId = MixedSourceBinding.TransactionId;
	MixedScopeEvent.RuntimeRevision = MixedSourceBinding.RuntimeRevision;
	MixedScopeEvent.ProviderId = MixedSourceBinding.ProviderId;
	TUniquePtr<FAssetDocumentInvocationHandleSession> MixedInvalidatingSession =
		MixedScopeRegistry.CreateInvocationSession(MixedSourceBinding);
	const TOptional<uint32> MixedScopeCount = InvalidateAndReadCount(
		*MixedInvalidatingSession, MixedScopeEvent);
	Test.TestFalse(TEXT("mixed scope retires the exact source"),
		MixedScopeRegistry.IsIssuedAndLive(MixedSource));
	Test.TestFalse(TEXT("mixed scope closes differently-provided child"),
		MixedScopeRegistry.IsIssuedAndLive(MixedChild));
	Test.TestFalse(TEXT("mixed scope revokes source and child leases"),
		MixedScopeRegistry.HasLiveLeaseForTesting(MixedSource)
			|| MixedScopeRegistry.HasLiveLeaseForTesting(MixedChild));
	Test.TestTrue(TEXT("mixed scope preserves request-only control"),
		MixedScopeRegistry.IsIssuedAndLive(RequestControl));
	Test.TestTrue(TEXT("mixed scope preserves transaction-only control"),
		MixedScopeRegistry.IsIssuedAndLive(TransactionControl));
	Test.TestTrue(TEXT("mixed scope preserves revision-only control"),
		MixedScopeRegistry.IsIssuedAndLive(RevisionControl));
	Test.TestTrue(TEXT("mixed scope preserves provider-only control"),
		MixedScopeRegistry.IsIssuedAndLive(ProviderControl));

	const TOptional<uint32>& Count = DirectCount;
	Test.TestTrue(TEXT("invalidation returns its exact slot count"), Count.IsSet());
	if (!Count.IsSet())
	{
		return;
	}
	Test.TestEqual(TEXT("direct live match count"),
		Count.GetValue(), uint32{1});
	Test.TestEqual(TEXT("no matching scope count"),
		NoMatchCount.GetValue(), uint32{0});
	Test.TestEqual(TEXT("repeated first invalidation count"),
		RepeatedFirstCount.GetValue(), uint32{1});
	Test.TestEqual(TEXT("repeated second invalidation count"),
		RepeatedSecondCount.GetValue(), uint32{0});
	Test.TestEqual(TEXT("source and descendant closure count"),
		DescendantCount.GetValue(), uint32{3});
	Test.TestEqual(TEXT("target-provider-only unload count"),
		TargetUnloadCount.GetValue(), uint32{2});
	Test.TestEqual(TEXT("mixed scope direct and descendant count"),
		MixedScopeCount.GetValue(), uint32{2});
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentHandleHandoffRuntimeTest,
	"UEAssetDoc.HandleHandoff.TerminalInvalidationCascade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentHandleHandoffRuntimeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestTransactionRollbackActionEnlistment(*this);
	TestSharedStorageIdentity(*this);
	TestAllTerminalInvalidations(*this);
	TestTargetOnlyProviderUnload(*this);
	TestExplicitSourceRetirement(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentHandleHandoffRejectionsTest,
	"UEAssetDoc.HandleHandoff.RejectsAccessTransactionRevisionAndProviderMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentHandleHandoffRejectionsTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TestContextAdmissionRejections(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentHandleInvalidationResultTest,
	"UEAssetDoc.HandleRegistry.InvalidationResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentHandleInvalidationResultTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TestInvalidationResult(*this);
	return true;
}
