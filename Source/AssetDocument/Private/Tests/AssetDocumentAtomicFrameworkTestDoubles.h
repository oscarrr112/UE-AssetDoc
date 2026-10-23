// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Runtime/AssetDocumentAtomicTransactionRuntime.h"

namespace AssetDocumentAtomicFrameworkTestDoubles
{
struct FCallRecord
{
	TArray<FString> Calls;
};

class FCancellationView final : public IAssetDocumentCancellationView
{
public:
	virtual bool IsCancellationRequested() const override
	{
		return bCancelled;
	}

	bool bCancelled = false;
};

class FHandleResolver final
	: public IAssetDocumentHandleResolver
	, public IAssetDocumentVerifiedHandleLease
	, public IAssetDocumentNativeHandlePayload
{
public:
	explicit FHandleResolver(FCallRecord* InRecord = nullptr)
		: Record(InRecord)
	{
	}

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		FAssetDocumentIssuedHandleResult Result;
		Result.Handle = NextIssuedHandle;
		Operations.Add(TEXT("Issue"));
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) override
	{
		LastAcquiredHandle = Handle;
		AcquiredHandles.Add(Handle);
		Operations.Add(TEXT("Acquire"));
		FAssetDocumentVerifiedLeaseResult Result;
		if (bSucceedNextAcquire)
		{
			bSucceedNextAcquire = false;
			TUniquePtr<FHandleResolver> Lease = MakeUnique<FHandleResolver>();
			Lease->LeaseHandle = Handle;
			Result.Lease = MoveTemp(Lease);
		}
		return Result;
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation& Event) override
	{
		Invalidations.Add(Event);
		Operations.Add(Event.Reason == EAssetDocumentHandleInvalidationReason::Compile
			? TEXT("Invalidate.Compile")
			: TEXT("Invalidate.TransactionBoundary"));
		if (Record != nullptr)
		{
			Record->Calls.Add(Event.Reason == EAssetDocumentHandleInvalidationReason::Commit
				? TEXT("Invalidate.Commit")
				: TEXT("Invalidate.Rollback"));
		}
		return {};
	}

	virtual const FAssetDocumentOpaqueHandle& GetHandle() const override
	{
		return LeaseHandle.GetValue();
	}

	virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override
	{
		return const_cast<FHandleResolver&>(*this);
	}

	virtual FName GetPayloadTypeId() const override
	{
		return TEXT("AssetDocument.AtomicFramework.TestPayload");
	}

	virtual bool IsAlive() const override
	{
		return true;
	}

	TArray<FAssetDocumentHandleInvalidation> Invalidations;
	TArray<FAssetDocumentOpaqueHandle> AcquiredHandles;
	TArray<FString> Operations;
	FCallRecord* Record = nullptr;
	TOptional<FAssetDocumentOpaqueHandle> NextIssuedHandle;
	TOptional<FAssetDocumentOpaqueHandle> LastAcquiredHandle;
	TOptional<FAssetDocumentOpaqueHandle> LeaseHandle;
	bool bSucceedNextAcquire = false;
};

class FRollbackParticipant final : public IAssetDocumentRollbackParticipant
{
public:
	FRollbackParticipant(FString InId, FCallRecord* InRecord = nullptr)
		: Id(MoveTemp(InId))
		, Record(InRecord)
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return Id;
	}

	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) override
	{
		++RollbackCount;
		if (Record != nullptr)
		{
			Record->Calls.Add(FString::Printf(TEXT("Rollback.%s"), *Id));
		}
		FAssetDocumentContractValidationResult Result;
		Result.bValid = !bFailRollback;
		return Result;
	}

	FString Id;
	FCallRecord* Record = nullptr;
	int32 RollbackCount = 0;
	bool bFailRollback = false;
};

class FParticipantRegistry final : public IAssetDocumentRollbackParticipantRegistry
{
public:
	virtual IAssetDocumentRollbackParticipant* Resolve(FStringView RollbackComponentId) override
	{
		IAssetDocumentRollbackParticipant** Found = Participants.Find(FString(RollbackComponentId));
		return Found == nullptr ? nullptr : *Found;
	}

	void Add(IAssetDocumentRollbackParticipant& Participant)
	{
		Participants.Add(FString(Participant.GetRollbackComponentId()), &Participant);
	}

	TMap<FString, IAssetDocumentRollbackParticipant*> Participants;
};
} // namespace AssetDocumentAtomicFrameworkTestDoubles
