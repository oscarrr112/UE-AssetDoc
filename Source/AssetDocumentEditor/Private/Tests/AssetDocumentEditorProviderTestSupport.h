// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentRollbackAction.h"
#include "CoreMinimal.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

struct FAssetDocumentEditorProviderTestContext
{
	FGuid RequestId = FGuid::NewGuid();
	FGuid TransactionId = FGuid::NewGuid();
	uint64 RuntimeRevision = 1;
	FString ProviderId = TEXT("editor");
};

inline FAssetDocumentPrimitiveInvocation
MakeAssetDocumentEditorProviderTestInvocation(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = Descriptor.PrimitiveId;
	Invocation.Version = Descriptor.Version;
	Invocation.RequestId = Context.RequestId;
	Invocation.TransactionId = Context.TransactionId;
	Invocation.RuntimeRevision = Context.RuntimeRevision;
	Invocation.Phase = Descriptor.AllowedPhases[0];
	Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	return Invocation;
}

class FAssetDocumentEditorProviderTestHandleResolver final
	: public IAssetDocumentHandleResolver
{
	struct FEntry
	{
		EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Object;
		EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::ReadOnly;
		uint32 Generation = 1;
		TUniquePtr<IAssetDocumentNativeHandlePayload> Payload;
	};

	class FLease final : public IAssetDocumentVerifiedHandleLease
	{
	public:
		FLease(
			FAssetDocumentOpaqueHandle InHandle,
			IAssetDocumentNativeHandlePayload& InPayload)
			: Handle(MoveTemp(InHandle))
			, Payload(&InPayload)
		{
		}

		virtual const FAssetDocumentOpaqueHandle& GetHandle() const override
		{
			return Handle;
		}

		virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override
		{
			check(Payload != nullptr);
			return *Payload;
		}

	private:
		FAssetDocumentOpaqueHandle Handle;
		IAssetDocumentNativeHandlePayload* Payload = nullptr;
	};

public:
	explicit FAssetDocumentEditorProviderTestHandleResolver(
		FAssetDocumentEditorProviderTestContext InContext)
		: Context(MoveTemp(InContext))
	{
	}

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		FAssetDocumentIssuedHandleResult Result;
		if (!Payload || !Payload->IsAlive())
		{
			Result.Diagnostics.Add(MakeDiagnostic(TEXT("TestHandle.InvalidPayload")));
			return Result;
		}
		FEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Kind = Kind;
		Entry.Access = Access;
		Entry.Payload = MoveTemp(Payload);
		FAssetDocumentOpaqueHandle Handle;
		Handle.RequestId = Context.RequestId;
		Handle.TransactionId = Context.TransactionId;
		Handle.RuntimeRevision = Context.RuntimeRevision;
		Handle.ProviderId = Context.ProviderId;
		Handle.Slot = Entries.Num();
		Handle.Generation = Entry.Generation;
		Handle.Kind = Kind;
		Handle.Access = Access;
		Result.Handle = MoveTemp(Handle);
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& ValidationContext) override
	{
		FAssetDocumentVerifiedLeaseResult Result;
		const FAssetDocumentContractValidationResult Metadata =
			ValidateAssetDocumentHandleMetadata(Handle, ValidationContext);
		if (!Metadata.bValid)
		{
			Result.Diagnostics = Metadata.Diagnostics;
			return Result;
		}
		const int32 Index = static_cast<int32>(Handle.Slot) - 1;
		if (!Entries.IsValidIndex(Index))
		{
			Result.Diagnostics.Add(MakeDiagnostic(TEXT("HandleMetadata.StaleHandle")));
			return Result;
		}
		FEntry& Entry = Entries[Index];
		if (Entry.Generation != Handle.Generation
			|| Entry.Kind != Handle.Kind
			|| Entry.Access != Handle.Access
			|| !Entry.Payload
			|| !Entry.Payload->IsAlive())
		{
			Result.Diagnostics.Add(MakeDiagnostic(TEXT("HandleMetadata.StaleHandle")));
			return Result;
		}
		Result.Lease = MakeUnique<FLease>(Handle, *Entry.Payload);
		return Result;
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		if (Event.RequestId == Context.RequestId
			&& Event.TransactionId == Context.TransactionId
			&& Event.RuntimeRevision == Context.RuntimeRevision
			&& Event.ProviderId == Context.ProviderId)
		{
			const uint32 InvalidatedHandleCount = static_cast<uint32>(Entries.Num());
			Entries.Reset();
			return {InvalidatedHandleCount};
		}
		return {};
	}

private:
	static FAssetDocumentRuntimeDiagnostic MakeDiagnostic(const TCHAR* Code)
	{
		FAssetDocumentRuntimeDiagnostic Diagnostic;
		Diagnostic.Path = TEXT("/Handle");
		Diagnostic.Code = Code;
		Diagnostic.Message = TEXT("The test handle resolver rejected the operation.");
		return Diagnostic;
	}

	FAssetDocumentEditorProviderTestContext Context;
	TArray<FEntry> Entries;
};

class FAssetDocumentEditorProviderTestTransaction final
	: public IAssetDocumentAtomicTransaction
{
public:
	explicit FAssetDocumentEditorProviderTestTransaction(
		const FAssetDocumentEditorProviderTestContext& Context)
	{
		Token.RequestId = Context.RequestId;
		Token.TransactionId = Context.TransactionId;
		Token.RuntimeRevision = Context.RuntimeRevision;
		RollbackActionEnlistmentResult.bValid = true;
		PostRollbackFinalizerEnlistmentResult.bValid = true;
	}

	virtual FGuid GetTransactionId() const override
	{
		return Token.TransactionId;
	}

	virtual EAssetDocumentAtomicState GetState() const override
	{
		return State;
	}

	virtual const FAssetDocumentMutationToken& GetMutationToken() const override
	{
		return Token;
	}

	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override
	{
		EnlistedFootprint.Append(Footprint.GetData(), Footprint.Num());
		State = EAssetDocumentAtomicState::Mutating;
		return MakeValidResult();
	}

	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		State = EAssetDocumentAtomicState::ReadyToCommit;
		return MakeValidResult();
	}

	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt&) override
	{
		State = EAssetDocumentAtomicState::Committed;
		return MakeValidResult();
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView) override
	{
		State = EAssetDocumentAtomicState::RolledBack;
		return MakeValidResult();
	}

	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		EnlistedRollbackActionComponentIds.Add(
			FString(Action->GetRollbackComponentId()));
		return RollbackActionEnlistmentResult;
	}

	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		return PostRollbackFinalizerEnlistmentResult;
	}

	const TArray<FAssetDocumentMutationFootprint>& GetEnlistedFootprint() const
	{
		return EnlistedFootprint;
	}

	TArray<FString> EnlistedRollbackActionComponentIds;
	FAssetDocumentContractValidationResult RollbackActionEnlistmentResult;
	FAssetDocumentContractValidationResult PostRollbackFinalizerEnlistmentResult;

private:
	static FAssetDocumentContractValidationResult MakeValidResult()
	{
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}

	FAssetDocumentMutationToken Token;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Live;
	TArray<FAssetDocumentMutationFootprint> EnlistedFootprint;
};
