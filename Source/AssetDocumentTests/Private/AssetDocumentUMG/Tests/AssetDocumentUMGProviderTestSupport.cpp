// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentUMGProviderTestSupport.h"

#include "AssetDocumentRollbackAction.h"
#include "Provider/AssetDocumentUMGNativePayloads.h"

namespace AssetDocumentUMGProviderTestSupport
{
FAssetDocumentRuntimeDiagnostic MakeDiagnostic(const TCHAR* Code)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/UMGProviderTest");
	Diagnostic.Code = Code;
	Diagnostic.Message = TEXT("The test double rejected an unexpected provider interaction.");
	return Diagnostic;
}

FAssetDocumentContractValidationResult MakeValidResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentContractValidationResult MakeInvalidResult(
	const TCHAR* Code)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = false;
	Result.Diagnostics.Add(MakeDiagnostic(Code));
	return Result;
}

class FLease final : public IAssetDocumentVerifiedHandleLease
{
public:
	FLease(
		const FAssetDocumentOpaqueHandle& InHandle,
		IAssetDocumentNativeHandlePayload& InPayload)
		: Handle(InHandle)
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
}

FAssetDocumentPrimitiveInvocation MakeAssetDocumentUMGProviderTestInvocation(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentUMGProviderTestContext& Context)
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

FAssetDocumentUMGProviderTestHandleResolver::FAssetDocumentUMGProviderTestHandleResolver(
	FAssetDocumentUMGProviderTestContext InContext)
	: Context(MoveTemp(InContext))
{
}

FAssetDocumentIssuedHandleResult FAssetDocumentUMGProviderTestHandleResolver::Issue(
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload)
{
	++IssueCount;
	FAssetDocumentIssuedHandleResult Result;
	if (!Payload || !Payload->IsAlive()
		|| (IssueFailureAfter != INDEX_NONE
			&& Entries.Num() >= IssueFailureAfter))
	{
		Result.Diagnostics.Add(
			AssetDocumentUMGProviderTestSupport::MakeDiagnostic(
				TEXT("TestHandle.IssueRejected")));
		return Result;
	}
	FAssetDocumentOpaqueHandle Handle;
	Handle.RequestId = Context.RequestId;
	Handle.TransactionId = Context.TransactionId;
	Handle.RuntimeRevision = Context.RuntimeRevision;
	Handle.ProviderId = Context.ProviderId;
	Handle.Slot = NextSlot++;
	Handle.Generation = ProviderGeneration;
	Handle.Kind = Kind;
	Handle.Access = Access;
	FEntry Entry;
	Entry.Handle = Handle;
	Entry.Payload = MoveTemp(Payload);
	Entries.Add(Handle.Slot, MoveTemp(Entry));
	Result.Handle = Handle;
	return Result;
}

FAssetDocumentVerifiedLeaseResult
FAssetDocumentUMGProviderTestHandleResolver::AcquireVerifiedLease(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& ValidationContext)
{
	++AcquireCount;
	FAssetDocumentVerifiedLeaseResult Result;
	const FAssetDocumentContractValidationResult Metadata =
		ValidateAssetDocumentHandleMetadata(
			Handle, ValidationContext);
	if (!Metadata.bValid)
	{
		Result.Diagnostics = Metadata.Diagnostics;
		return Result;
	}
	FEntry* Entry = Entries.Find(Handle.Slot);
	if (Entry == nullptr
		|| Entry->Handle.Generation != Handle.Generation
		|| Handle.Generation != ProviderGeneration
		|| !Entry->Payload
		|| !Entry->Payload->IsAlive())
	{
		Result.Diagnostics.Add(
			AssetDocumentUMGProviderTestSupport::MakeDiagnostic(
				TEXT("TestHandle.StaleOrDead")));
		return Result;
	}
	Result.Lease =
		MakeUnique<AssetDocumentUMGProviderTestSupport::FLease>(
			Handle, *Entry->Payload);
	return Result;
}

FAssetDocumentHandleInvalidationResult
FAssetDocumentUMGProviderTestHandleResolver::Invalidate(
	const FAssetDocumentHandleInvalidation& Event)
{
	FAssetDocumentHandleInvalidationResult Result;
	++InvalidationCount;
	for (auto It = Entries.CreateIterator(); It; ++It)
	{
		const FAssetDocumentOpaqueHandle& Handle =
			It.Value().Handle;
		const bool bRequestMatches =
			!Event.RequestId.IsValid()
			|| Event.RequestId == Handle.RequestId;
		const bool bTransactionMatches =
			!Event.TransactionId.IsValid()
			|| Event.TransactionId == Handle.TransactionId;
		const bool bRevisionMatches =
			Event.RuntimeRevision == 0
			|| Event.RuntimeRevision == Handle.RuntimeRevision;
		const bool bProviderMatches =
			Event.ProviderId.IsEmpty()
			|| Event.ProviderId == Handle.ProviderId;
		if (bRequestMatches && bTransactionMatches
			&& bRevisionMatches && bProviderMatches)
		{
			It.RemoveCurrent();
			++Result.InvalidatedHandleCount;
		}
	}
	return Result;
}

int32 FAssetDocumentUMGProviderTestHandleResolver::GetTotalCallCount() const
{
	return IssueCount + AcquireCount + InvalidationCount;
}

FAssetDocumentOpaqueHandle
FAssetDocumentUMGProviderTestHandleResolver::AddObjectHandle(
	UObject* Object,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access)
{
	FAssetDocumentIssuedHandleResult Result = Issue(
		Kind, Access,
		MakeAssetDocumentUMGUObjectPayload(Object));
	check(Result.Handle.IsSet());
	return Result.Handle.GetValue();
}

FAssetDocumentOpaqueHandle
FAssetDocumentUMGProviderTestHandleResolver::AddChannelHandle(
	UMovieSceneSection* OwningSection,
	FMovieSceneFloatChannel* Channel,
	FName ChannelName,
	EAssetDocumentHandleAccess Access)
{
	FAssetDocumentIssuedHandleResult Result = Issue(
		EAssetDocumentHandleKind::Channel,
		Access,
		MakeAssetDocumentUMGChannelPayload(
			OwningSection, Channel, ChannelName));
	check(Result.Handle.IsSet());
	return Result.Handle.GetValue();
}

void FAssetDocumentUMGProviderTestHandleResolver::SetIssueFailureAfter(
	int32 SuccessfulIssueCount)
{
	IssueFailureAfter = SuccessfulIssueCount;
}

void FAssetDocumentUMGProviderTestHandleResolver::AdvanceProviderGeneration()
{
	++ProviderGeneration;
}

FAssetDocumentUMGProviderTestTransaction::FAssetDocumentUMGProviderTestTransaction(
	const FAssetDocumentUMGProviderTestContext& Context)
{
	Token.RequestId = Context.RequestId;
	Token.TransactionId = Context.TransactionId;
	Token.RuntimeRevision = Context.RuntimeRevision;
	RollbackActionEnlistmentResult.bValid = true;
}

FGuid FAssetDocumentUMGProviderTestTransaction::GetTransactionId() const
{
	++ReadCount;
	return Token.TransactionId;
}

EAssetDocumentAtomicState FAssetDocumentUMGProviderTestTransaction::GetState() const
{
	++ReadCount;
	++StateReadCount;
	if (PoisonAfterStateRead != INDEX_NONE
		&& StateReadCount >= PoisonAfterStateRead)
	{
		return EAssetDocumentAtomicState::Poisoned;
	}
	return State;
}

const FAssetDocumentMutationToken&
FAssetDocumentUMGProviderTestTransaction::GetMutationToken() const
{
	++ReadCount;
	return Token;
}

FAssetDocumentContractValidationResult
FAssetDocumentUMGProviderTestTransaction::EnlistFootprint(
	TConstArrayView<FAssetDocumentMutationFootprint> Footprint)
{
	++EnlistCount;
	if (bRejectEnlist || Footprint.IsEmpty())
	{
		return AssetDocumentUMGProviderTestSupport::MakeInvalidResult(
			TEXT("TestTransaction.EnlistRejected"));
	}
	EnlistedFootprint.Append(
		Footprint.GetData(), Footprint.Num());
	return AssetDocumentUMGProviderTestSupport::MakeValidResult();
}

FAssetDocumentContractValidationResult
FAssetDocumentUMGProviderTestTransaction::MarkReadyToCommit()
{
	++MarkReadyCount;
	State = EAssetDocumentAtomicState::ReadyToCommit;
	return AssetDocumentUMGProviderTestSupport::MakeValidResult();
}

FAssetDocumentContractValidationResult FAssetDocumentUMGProviderTestTransaction::Commit(
	const FAssetDocumentDurableCompletionReceipt&)
{
	++CommitCount;
	State = EAssetDocumentAtomicState::Committed;
	return AssetDocumentUMGProviderTestSupport::MakeValidResult();
}

FAssetDocumentContractValidationResult FAssetDocumentUMGProviderTestTransaction::Rollback(
	FStringView StableFailureCode)
{
	++RollbackCount;
	if (bRejectRollback || StableFailureCode.IsEmpty())
	{
		return AssetDocumentUMGProviderTestSupport::MakeInvalidResult(
			TEXT("TestTransaction.RollbackRejected"));
	}
	if (RestoreCallback)
	{
		RestoreCallback();
	}
	State = EAssetDocumentAtomicState::RolledBack;
	return AssetDocumentUMGProviderTestSupport::MakeValidResult();
}

FAssetDocumentContractValidationResult
FAssetDocumentUMGProviderTestTransaction::EnlistRollbackAction(
	TSharedRef<IAssetDocumentRollbackAction> Action)
{
	EnlistedRollbackActionComponentIds.Add(
		FString(Action->GetRollbackComponentId()));
	return RollbackActionEnlistmentResult;
}

int32 FAssetDocumentUMGProviderTestTransaction::GetTotalCallCount() const
{
	return ReadCount + EnlistCount + MarkReadyCount + CommitCount + RollbackCount
		+ EnlistedRollbackActionComponentIds.Num();
}

void FAssetDocumentUMGProviderTestTransaction::SetRestoreCallback(
	TFunction<void()> InRestoreCallback)
{
	RestoreCallback = MoveTemp(InRestoreCallback);
}

void FAssetDocumentUMGProviderTestTransaction::SetRejectEnlist(bool bReject)
{
	bRejectEnlist = bReject;
}

void FAssetDocumentUMGProviderTestTransaction::SetRejectRollback(bool bReject)
{
	bRejectRollback = bReject;
}

void FAssetDocumentUMGProviderTestTransaction::SetRollbackActionEnlistmentResult(
	FAssetDocumentContractValidationResult InResult)
{
	RollbackActionEnlistmentResult = MoveTemp(InResult);
}

void FAssetDocumentUMGProviderTestTransaction::SetState(
	EAssetDocumentAtomicState InState)
{
	State = InState;
}

void FAssetDocumentUMGProviderTestTransaction::SetPoisonAfterStateRead(
	int32 StateReadOrdinal)
{
	PoisonAfterStateRead = StateReadOrdinal;
}
