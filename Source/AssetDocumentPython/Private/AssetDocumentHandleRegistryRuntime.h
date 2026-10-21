// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

namespace UE::AssetDocument::PythonHost
{
inline constexpr uint32 ScratchContextLifetimeContractVersion = 1;
}

struct FAssetDocumentHandleResolverBinding
{
	FGuid RequestId;
	FGuid TransactionId;
	uint64 RuntimeRevision = 0;
	FString ProviderId;
	EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	TArray<FAssetDocumentPrimitiveHandleResultSchema> AllowedResults;
};

enum class EAssetDocumentInvocationHandleCloseDisposition : uint8
{
	Succeeded = 0,
	Failed = 1,
};

struct FAssetDocumentHandleLeaseRevocationState;
class FAssetDocumentHandleRegistryRuntime;

class FAssetDocumentInvocationHandleSession final
	: public IAssetDocumentHandleResolver
{
public:
	~FAssetDocumentInvocationHandleSession() override;

	FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override;
	FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) override;
	FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event) override;

	FAssetDocumentContractValidationResult AcquireAndHoldInput(
		const FAssetDocumentOpaqueHandle& Handle,
		EAssetDocumentHandleKind RequiredKind,
		EAssetDocumentHandleAccess RequiredAccess);
	FAssetDocumentContractValidationResult MarkValidatedReturnedHandles(
		TConstArrayView<FAssetDocumentPrimitiveHandleResult> Results);
	void Close(EAssetDocumentInvocationHandleCloseDisposition Disposition);

private:
	friend class FAssetDocumentHandleRegistryRuntime;

	FAssetDocumentInvocationHandleSession(
		FAssetDocumentHandleRegistryRuntime& InRegistry,
		const FAssetDocumentHandleResolverBinding& InBinding);
	bool HasIssued(const FAssetDocumentOpaqueHandle& Handle) const;
	FAssetDocumentContractValidationResult ValidateReturnedProvenance(
		TConstArrayView<FAssetDocumentPrimitiveHandleResult> Results) const;
	FAssetDocumentContractValidationResult ValidateResultProvenance(
		const FAssetDocumentPrimitiveHandleResult& Result,
		TSet<uint64>& SeenHandles) const;
	FAssetDocumentContractValidationResult ValidateReturnedSchemas(
		TConstArrayView<FAssetDocumentPrimitiveHandleResult> Results,
		TArray<FAssetDocumentOpaqueHandle>& ValidatedHandles) const;
	FAssetDocumentContractValidationResult ValidateReturnedGroup(
		const FAssetDocumentPrimitiveHandleResult& Result,
		TSet<FString>& SeenNames,
		TArray<FAssetDocumentOpaqueHandle>& ValidatedHandles) const;
	bool HasAllRequiredReturnedGroups(const TSet<FString>& SeenNames) const;
	bool IsReturnedHandleValid(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentPrimitiveHandleResultSchema& Schema) const;
	bool ShouldRetainIssuedHandle(
		EAssetDocumentInvocationHandleCloseDisposition Disposition,
		const FAssetDocumentOpaqueHandle& Handle) const;
	void TrackLease(const TSharedRef<FAssetDocumentHandleLeaseRevocationState>& State);

	FAssetDocumentHandleRegistryRuntime& Registry;
	FAssetDocumentHandleResolverBinding Binding;
	TArray<FAssetDocumentOpaqueHandle> IssuedHandles;
	TArray<FAssetDocumentOpaqueHandle> ReturnedHandles;
	TArray<TUniquePtr<IAssetDocumentVerifiedHandleLease>> HeldInputLeases;
	TArray<TSharedRef<FAssetDocumentHandleLeaseRevocationState>> SessionLeaseStates;
	bool bMarkedReturnedHandles = false;
	bool bClosed = false;
};

class FAssetDocumentHandleRegistryRuntime final
{
public:
	FAssetDocumentHandleRegistryRuntime();
	~FAssetDocumentHandleRegistryRuntime();

	TUniquePtr<FAssetDocumentInvocationHandleSession> CreateInvocationSession(
		const FAssetDocumentHandleResolverBinding& Binding);
	FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event);
	void RetireScratchHandlesForContext(
		const FGuid& RequestId,
		const FGuid& TransactionId,
		uint64 RuntimeRevision);
	FAssetDocumentIssuedHandleResult Handoff(
		const FAssetDocumentOpaqueHandle& SourceHandle,
		const FAssetDocumentHandleValidationContext& SourceContext,
		FStringView TargetProviderId,
		EAssetDocumentHandleAccess RequestedAccess);
	bool IsIssuedAndLive(const FAssetDocumentOpaqueHandle& Handle) const;

#if WITH_DEV_AUTOMATION_TESTS
	bool HasLiveLeaseForTesting(const FAssetDocumentOpaqueHandle& Handle) const;
	void ExhaustSlotSpaceForTesting();
#endif

private:
	friend class FAssetDocumentInvocationHandleSession;

	struct FSlot;
	FAssetDocumentIssuedHandleResult Issue(
		const FAssetDocumentHandleResolverBinding& Binding,
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload);
	FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context,
		TSharedPtr<FAssetDocumentHandleLeaseRevocationState>& OutState);
	IAssetDocumentNativeHandlePayload* FindPayload(
		const FAssetDocumentOpaqueHandle& Handle) const;
	FAssetDocumentVerifiedLeaseResult CreateLease(
		const FAssetDocumentOpaqueHandle& Handle,
		IAssetDocumentNativeHandlePayload* Payload,
		TSharedPtr<FAssetDocumentHandleLeaseRevocationState>& OutState);
	static FAssetDocumentContractValidationResult ValidateHandoffRequest(
		const FAssetDocumentOpaqueHandle& SourceHandle,
		FStringView TargetProviderId,
		EAssetDocumentHandleAccess RequestedAccess);
	FAssetDocumentIssuedHandleResult IssueHandoffFromLiveSource(
		const FAssetDocumentOpaqueHandle& SourceHandle,
		FStringView TargetProviderId,
		EAssetDocumentHandleAccess RequestedAccess);
	static bool DoesEventMatch(
		const FAssetDocumentHandleInvalidation& Event,
		const FAssetDocumentOpaqueHandle& Handle);
	static void RevokeSlotLeases(FSlot& Slot);
	TSet<uint32> SelectInvalidationClosure(
		TFunctionRef<bool(const FSlot&)> Matches) const;
	bool IsDescendantOfClosedSlot(
		const FSlot& Slot,
		const TSet<uint32>& ClosedSlots) const;
	FAssetDocumentHandleInvalidationResult RetireInvalidationClosure(
		const TSet<uint32>& ClosedSlots);
	FAssetDocumentHandleInvalidationResult InvalidateMatching(
		TFunctionRef<bool(const FSlot&)> Matches);
	void InvalidateHandles(TConstArrayView<FAssetDocumentOpaqueHandle> Handles);

	mutable FCriticalSection Mutex;
	TMap<uint32, TUniquePtr<FSlot>> Slots;
	uint32 NextSlot = 1;
};
