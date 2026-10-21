// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

struct FMovieSceneFloatChannel;
class UMovieSceneSection;
class UObject;

struct FAssetDocumentUMGProviderTestContext
{
	FGuid RequestId = FGuid::NewGuid();
	FGuid TransactionId = FGuid::NewGuid();
	uint64 RuntimeRevision = 1;
	FString ProviderId = TEXT("umg");
};

FAssetDocumentPrimitiveInvocation MakeAssetDocumentUMGProviderTestInvocation(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentUMGProviderTestContext& Context);

class FAssetDocumentUMGProviderTestHandleResolver final
	: public IAssetDocumentHandleResolver
{
public:
	explicit FAssetDocumentUMGProviderTestHandleResolver(
		FAssetDocumentUMGProviderTestContext InContext);

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override;
	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& ValidationContext) override;
	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event) override;

	int32 GetIssueCount() const { return IssueCount; }
	int32 GetAcquireCount() const { return AcquireCount; }
	int32 GetInvalidationCount() const { return InvalidationCount; }
	int32 GetTotalCallCount() const;
	const FAssetDocumentUMGProviderTestContext& GetContext() const
	{
		return Context;
	}
	FAssetDocumentOpaqueHandle AddObjectHandle(
		UObject* Object,
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access);
	FAssetDocumentOpaqueHandle AddChannelHandle(
		UMovieSceneSection* OwningSection,
		FMovieSceneFloatChannel* Channel,
		FName ChannelName,
		EAssetDocumentHandleAccess Access);
	void SetIssueFailureAfter(int32 SuccessfulIssueCount);
	void AdvanceProviderGeneration();

private:
	struct FEntry
	{
		FAssetDocumentOpaqueHandle Handle;
		TUniquePtr<IAssetDocumentNativeHandlePayload> Payload;
	};

	FAssetDocumentUMGProviderTestContext Context;
	TMap<uint32, FEntry> Entries;
	uint32 NextSlot = 1;
	uint32 ProviderGeneration = 1;
	int32 IssueFailureAfter = INDEX_NONE;
	int32 IssueCount = 0;
	int32 AcquireCount = 0;
	int32 InvalidationCount = 0;
};

class FAssetDocumentUMGProviderTestTransaction final
	: public IAssetDocumentAtomicTransaction
{
public:
	explicit FAssetDocumentUMGProviderTestTransaction(
		const FAssetDocumentUMGProviderTestContext& Context);

	virtual FGuid GetTransactionId() const override;
	virtual EAssetDocumentAtomicState GetState() const override;
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override;
	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override;
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override;
	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt& Receipt) override;
	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) override;
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override;
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		return PostRollbackFinalizerEnlistmentResult;
	}

	int32 GetEnlistCount() const { return EnlistCount; }
	int32 GetMarkReadyCount() const { return MarkReadyCount; }
	int32 GetCommitCount() const { return CommitCount; }
	int32 GetRollbackCount() const { return RollbackCount; }
	int32 GetTotalCallCount() const;
	const TArray<FAssetDocumentMutationFootprint>& GetEnlistedFootprint() const
	{
		return EnlistedFootprint;
	}
	const TArray<FString>& GetEnlistedRollbackActionComponentIds() const
	{
		return EnlistedRollbackActionComponentIds;
	}
	void SetRestoreCallback(TFunction<void()> InRestoreCallback);
	void SetRejectEnlist(bool bReject);
	void SetRejectRollback(bool bReject);
	void SetRollbackActionEnlistmentResult(
		FAssetDocumentContractValidationResult InResult);
	void SetState(EAssetDocumentAtomicState InState);
	void SetPoisonAfterStateRead(int32 StateReadOrdinal);

private:
	FAssetDocumentMutationToken Token;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Mutating;
	TArray<FAssetDocumentMutationFootprint> EnlistedFootprint;
	TArray<FString> EnlistedRollbackActionComponentIds;
	FAssetDocumentContractValidationResult RollbackActionEnlistmentResult;
	FAssetDocumentContractValidationResult PostRollbackFinalizerEnlistmentResult;
	TFunction<void()> RestoreCallback;
	bool bRejectEnlist = false;
	bool bRejectRollback = false;
	int32 PoisonAfterStateRead = INDEX_NONE;
	mutable int32 StateReadCount = 0;
	int32 EnlistCount = 0;
	int32 MarkReadyCount = 0;
	int32 CommitCount = 0;
	int32 RollbackCount = 0;
	mutable int32 ReadCount = 0;
};
