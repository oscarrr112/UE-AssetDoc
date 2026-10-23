// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentRollbackAction.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Misc/AutomationTest.h"
namespace AssetDocumentAIBehaviorTreeProviderTestSupport
{
class FResolveExistingHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	struct FEntry
	{
		EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Object;
		EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::ReadOnly;
		FString ProviderId;
		uint32 Generation = 1;
		TSharedPtr<IAssetDocumentNativeHandlePayload> Payload;
	};

	class FLease final : public IAssetDocumentVerifiedHandleLease
	{
	public:
		FLease(FAssetDocumentOpaqueHandle InHandle,
			IAssetDocumentNativeHandlePayload& InPayload)
			: Handle(MoveTemp(InHandle))
			, Payload(&InPayload)
		{
		}
		virtual const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
		virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override
		{
			check(Payload != nullptr);
			return *Payload;
		}
	private:
		FAssetDocumentOpaqueHandle Handle;
		IAssetDocumentNativeHandlePayload* Payload = nullptr;
	};

	void SetMutationToken(const FAssetDocumentMutationToken& InToken) { Token = InToken; }
	void SetIssuingProviderId(FString InProviderId) { IssuingProviderId = MoveTemp(InProviderId); }
	bool bRejectNextIssue = false;

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		FAssetDocumentIssuedHandleResult Result;
		if (bRejectNextIssue)
		{
			bRejectNextIssue = false;
			FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Code = TEXT("Test.RejectHandleIssue");
			return Result;
		}
		if (!Payload || !Payload->IsAlive())
		{
			return Result;
		}
		FEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Kind = Kind;
		Entry.Access = Access;
		Entry.ProviderId = IssuingProviderId;
		Entry.Payload = MakeShareable(Payload.Release());
		FAssetDocumentOpaqueHandle Handle;
		Handle.RequestId = Token.RequestId;
		Handle.TransactionId = Token.TransactionId;
		Handle.RuntimeRevision = Token.RuntimeRevision;
		Handle.ProviderId = Entry.ProviderId;
		Handle.Slot = Entries.Num();
		Handle.Generation = Entry.Generation;
		Handle.Kind = Kind;
		Handle.Access = Access;
		Result.Handle = Handle;
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) override
	{
		FAssetDocumentVerifiedLeaseResult Result;
		const FAssetDocumentContractValidationResult Metadata =
			ValidateAssetDocumentHandleMetadata(Handle, Context);
		if (!Metadata.bValid)
		{
			Result.Diagnostics = Metadata.Diagnostics;
			return Result;
		}
		const int32 Index = static_cast<int32>(Handle.Slot) - 1;
		if (!Entries.IsValidIndex(Index))
		{
			return Result;
		}
		FEntry& Entry = Entries[Index];
		IAssetDocumentNativeHandlePayload* Payload = Entry.Payload.Get();
		if (Entry.Generation == Handle.Generation && Entry.Kind == Handle.Kind
			&& Entry.Access == Handle.Access && Entry.ProviderId == Handle.ProviderId
			&& Payload != nullptr && Payload->IsAlive())
		{
			Result.Lease = MakeUnique<FLease>(Handle, *Payload);
		}
		return Result;
	}

	TOptional<FAssetDocumentOpaqueHandle> RegisterProviderScopedAlias(
		const FAssetDocumentOpaqueHandle& SourceHandle,
		FStringView TargetProviderId,
		EAssetDocumentHandleAccess RequestedAccess)
	{
		const int32 SourceIndex = static_cast<int32>(SourceHandle.Slot) - 1;
		if (!Entries.IsValidIndex(SourceIndex) || TargetProviderId.IsEmpty())
		{
			return {};
		}
		FEntry& SourceEntry = Entries[SourceIndex];
		const TSharedPtr<IAssetDocumentNativeHandlePayload> SharedPayload = SourceEntry.Payload;
		if (SourceEntry.Generation != SourceHandle.Generation
			|| SourceEntry.ProviderId != SourceHandle.ProviderId
			|| !SharedPayload.IsValid() || !SharedPayload->IsAlive())
		{
			return {};
		}

		FEntry& TargetEntry = Entries.AddDefaulted_GetRef();
		TargetEntry.Kind = SourceHandle.Kind;
		TargetEntry.Access = RequestedAccess;
		TargetEntry.ProviderId = FString(TargetProviderId);
		TargetEntry.Payload = SharedPayload;
		FAssetDocumentOpaqueHandle TargetHandle = SourceHandle;
		TargetHandle.ProviderId = TargetEntry.ProviderId;
		TargetHandle.Slot = Entries.Num();
		TargetHandle.Generation = TargetEntry.Generation;
		TargetHandle.Access = RequestedAccess;
		return TargetHandle;
	}
	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		if (Event.RequestId == Token.RequestId && Event.TransactionId == Token.TransactionId
			&& Event.RuntimeRevision == Token.RuntimeRevision
			&& (Event.ProviderId.IsEmpty()
				|| Entries.ContainsByPredicate([&Event](const FEntry& Entry)
				{
					return Entry.ProviderId == Event.ProviderId;
				})))
		{
			const uint32 Count = static_cast<uint32>(Entries.Num());
			Entries.Reset();
			++InvalidationCount;
			return {Count};
		}
		return {};
	}
	int32 GetInvalidationCount() const { return InvalidationCount; }

private:
	FAssetDocumentMutationToken Token;
	FString IssuingProviderId = TEXT("editor");
	TArray<FEntry> Entries;
	int32 InvalidationCount = 0;
};

inline TUniquePtr<IAssetDocumentVerifiedHandleLease> ResolveExistingMutableAssetLease(
	FAutomationTestBase& Test,
	UObject& Object,
	IAssetDocumentAtomicTransaction& Transaction,
	FResolveExistingHandleResolver& Resolver)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = TEXT("editor.asset.resolve_existing.v1");
	Invocation.Version = 1;
	Invocation.RequestId = Transaction.GetMutationToken().RequestId;
	Invocation.TransactionId = Transaction.GetMutationToken().TransactionId;
	Invocation.RuntimeRevision = Transaction.GetMutationToken().RuntimeRevision;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	Invocation.ValueArguments.ObjectKeys = {TEXT("path"), TEXT("expected_class")};
	FAssetDocumentRuntimeValueDto Path;
	Path.Kind = EAssetDocumentRuntimeValueKind::String;
	Path.StringValue = Object.GetPathName();
	FAssetDocumentRuntimeValueDto Class;
	Class.Kind = EAssetDocumentRuntimeValueKind::String;
	Class.StringValue = Object.GetClass()->GetPathName();
	Invocation.ValueArguments.Elements = {MoveTemp(Path), MoveTemp(Class)};
	const FAssetDocumentPrimitiveOutcome Outcome =
		CreateAssetDocumentEditorPrimitiveProvider()->Invoke(Invocation, Resolver, nullptr);
	if (!Test.TestEqual(TEXT("public resolve-existing provider succeeds for exact test tree"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return nullptr;
	}
	for (const FAssetDocumentPrimitiveHandleResult& Result : Outcome.HandleResults)
	{
		if (Result.Name == TEXT("asset_mutable") && Result.Handles.Num() == 1)
		{
			FAssetDocumentHandleValidationContext Context;
			Context.RequestId = Transaction.GetMutationToken().RequestId;
			Context.TransactionId = Transaction.GetMutationToken().TransactionId;
			Context.RuntimeRevision = Transaction.GetMutationToken().RuntimeRevision;
			Context.ProviderId = TEXT("editor");
			Context.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
			Context.RequiredKind = EAssetDocumentHandleKind::Asset;
			Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
			return Resolver.AcquireVerifiedLease(Result.Handles[0], Context).Lease;
		}
	}
	Test.AddError(TEXT("resolve-existing returned no exact mutable Asset lease"));
	return nullptr;
}

class FInstrumentedTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	explicit FInstrumentedTransaction(IAssetDocumentAtomicTransaction& InInner) : Inner(InInner) {}
	bool bFailFinalizer = false;
	bool bFailFootprint = false;
	int32 FinalizerCalls = 0;
	int32 FootprintCalls = 0;
	int32 ActionCalls = 0;
	bool bFailAction = false;
	TArray<FString> Calls;
	virtual FGuid GetTransactionId() const override { return Inner.GetTransactionId(); }
	virtual EAssetDocumentAtomicState GetState() const override { return Inner.GetState(); }
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override { return Inner.GetMutationToken(); }
	virtual FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Value) override
	{
		++FootprintCalls; Calls.Add(TEXT("footprint"));
		return bFailFootprint ? Failure(TEXT("Test.InjectedFootprintFailure")) : Inner.EnlistFootprint(Value);
	}
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override { return Inner.MarkReadyToCommit(); }
	virtual FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt& Value) override { return Inner.Commit(Value); }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView Value) override { return Inner.Rollback(Value); }
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction> Value) override
	{
		++ActionCalls;
		return bFailAction ? Failure(TEXT("Test.InjectedActionFailure")) : Inner.EnlistRollbackAction(Value);
	}
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer> Value) override
	{
		++FinalizerCalls; Calls.Add(TEXT("finalizer"));
		return bFailFinalizer ? Failure(TEXT("Test.InjectedFinalizerFailure")) : Inner.EnlistPostRollbackFinalizer(Value);
	}
private:
	static FAssetDocumentContractValidationResult Failure(const TCHAR* Code)
	{
		FAssetDocumentContractValidationResult Result; Result.bValid = false;
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/NodeBTest"); Diagnostic.Code = Code; Diagnostic.Message = TEXT("Injected enlistment failure.");
		return Result;
	}
	IAssetDocumentAtomicTransaction& Inner;
};

class FNoCancellation final : public IAssetDocumentCancellationView
{
public:
	virtual bool IsCancellationRequested() const override { return false; }
};

class FOrderedAuthoredAction final : public IAssetDocumentRollbackAction
{
public:
	FOrderedAuthoredAction(
		FString InComponentId,
		TArray<FString>& InOrder,
		FString InName,
		TFunction<void()> InRestore = {})
		: ComponentId(MoveTemp(InComponentId)), Order(InOrder), Name(MoveTemp(InName)), Restore(MoveTemp(InRestore)) {}
	virtual FStringView GetRollbackComponentId() const override { return ComponentId; }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		Order.Add(Name);
		if (Restore)
		{
			Restore();
		}
		FAssetDocumentContractValidationResult Result; Result.bValid = true; return Result;
	}
private:
	FString ComponentId;
	TArray<FString>& Order;
	FString Name;
	TFunction<void()> Restore;
};

}
