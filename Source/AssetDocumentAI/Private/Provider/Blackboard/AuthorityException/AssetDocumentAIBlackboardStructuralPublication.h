// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "BehaviorTree/BlackboardData.h"
#include "CoreMinimal.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardLoadedConsumerCompletion.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

class IPropertyHandle;

namespace AssetDocumentAIBlackboardStructuralPublication
{
inline constexpr FStringView PublicHeaderPrefix = TEXTVIEW(
	"AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h+"
	"BehaviorTree/BlackboardData.h+PropertyHandle.h");
inline constexpr FStringView StagingExportedAuthoritativeApi = TEXTVIEW(
	"AssetDocumentAIBlackboardStructuralPublication::StageKeyType");
inline constexpr FStringView StagingAuthorityFingerprintSuffix = TEXTVIEW(
	"|AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1|"
	"AssetDocumentAIBlackboardStructuralPublication::StageKeyType");
inline constexpr FStringView PublicationLifecyclePacket = TEXTVIEW(
	"PublishLocalKeysSnapshot>IPropertyHandle::NotifyPreChange>"
	"UBlackboardData::Keys assignment>"
	"IPropertyHandle::NotifyPostChange(EPropertyChangeType::ValueSet)>"
	"IPropertyHandle::NotifyFinishedChangingProperties>"
	"UBlackboardData::UpdateParentKeys()>UBlackboardData::UpdateKeyIDs()");
inline constexpr FStringView PublicationExportedAuthoritativeApi = TEXTVIEW(
	"AssetDocumentAIBlackboardStructuralPublication::"
	"PublishLocalKeysSnapshot>IPropertyHandle::NotifyPreChange>"
	"UBlackboardData::Keys assignment>"
	"IPropertyHandle::NotifyPostChange(EPropertyChangeType::ValueSet)>"
	"IPropertyHandle::NotifyFinishedChangingProperties>"
	"UBlackboardData::UpdateParentKeys()>UBlackboardData::UpdateKeyIDs()");
inline constexpr FStringView PublicationAuthorityFingerprintSuffix = TEXTVIEW(
	"|AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1|"
	"AssetDocumentAIBlackboardStructuralPublication::"
	"PublishLocalKeysSnapshot>IPropertyHandle::NotifyPreChange>"
	"UBlackboardData::Keys assignment>"
	"IPropertyHandle::NotifyPostChange(EPropertyChangeType::ValueSet)>"
	"IPropertyHandle::NotifyFinishedChangingProperties>"
	"UBlackboardData::UpdateParentKeys()>UBlackboardData::UpdateKeyIDs()");

class FStructuralPublicationStagingState;
class FStructuralPublicationTerminalObservation;
class FStructuralPublicationLoadedConsumerState;
struct FStagedKeyTypeResult;

enum class EStageKeyTypeFailure : uint8
{
	None,
	InvalidTarget,
	InvalidKeyTypeClass,
	MissingTransactionPreconditions,
	ConstructionFailed,
};

class ASSETDOCUMENTAI_API FStructuralPublicationTransactionBinding final
	: public IAssetDocumentAtomicTransaction
{
public:
	FStructuralPublicationTransactionBinding(
		UBlackboardData& InExactTarget,
		FString InRollbackComponentId,
		IAssetDocumentAtomicTransaction& InTransaction);
	virtual ~FStructuralPublicationTransactionBinding() override;

	FStructuralPublicationTransactionBinding(
		const FStructuralPublicationTransactionBinding&) = delete;
	FStructuralPublicationTransactionBinding& operator=(
		const FStructuralPublicationTransactionBinding&) = delete;

	virtual FGuid GetTransactionId() const override;
	virtual EAssetDocumentAtomicState GetState() const override;
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override;
	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override;
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override;
	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt& Receipt) override;
	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override;
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override;
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer) override;

	bool IsReadyFor(const UBlackboardData& Blackboard) const;
	bool IsBoundTo(const UBlackboardData& Blackboard) const;
	bool HasRollbackVerifiedTerminal() const;
	bool CanRetainStagedKeyType() const;
	bool TryRetainStagedKeyType(UBlackboardKeyType& KeyType);
	bool PreparePublishedSnapshot(TConstArrayView<FBlackboardEntry> Snapshot);
	AssetDocumentAIBlackboardLoadedConsumerCompletion::ELoadedConsumerReceiptCaptureFailure
	GetLoadedConsumerReceiptCaptureFailure() const;
	AssetDocumentAIBlackboardLoadedConsumerCompletion::ELoadedConsumerCompletionResult
	CompletePublishedLoadedConsumers(UBlackboardData& Blackboard);
	TWeakPtr<FStructuralPublicationStagingState> GetStagingState() const;
	TSharedPtr<FStructuralPublicationTerminalObservation> GetTerminalObservation() const;

private:
	bool HasCurrentIdentity() const;
	TWeakObjectPtr<UBlackboardData> ExactTarget;
	FString RollbackComponentId;
	IAssetDocumentAtomicTransaction* Transaction = nullptr;
	FAssetDocumentMutationToken InitialToken;
	TSharedPtr<FStructuralPublicationStagingState> StagingState;
	TSharedPtr<FStructuralPublicationLoadedConsumerState> LoadedConsumerState;
	bool bTypedPreimageActionEnlisted = false;
	bool bExactFootprintEnlisted = false;
};

struct ASSETDOCUMENTAI_API FStagedKeyTypeResult
{
	EStageKeyTypeFailure Failure = EStageKeyTypeFailure::ConstructionFailed;

	FStagedKeyTypeResult() = default;
	FStagedKeyTypeResult(
		EStageKeyTypeFailure InFailure,
		UBlackboardKeyType* InKeyType,
		TWeakPtr<FStructuralPublicationStagingState> InStagingState,
		TSharedPtr<FStructuralPublicationTerminalObservation> InTerminalObservation);
	UBlackboardKeyType* Get() const;
	bool HasStrongOwnership() const;
	bool HasRollbackVerifiedTerminal() const;
	bool WasStrongOwnershipRetainedDuringRollback() const;
	bool HasTransactionTerminalReleasedOwnership() const;

private:
	TWeakObjectPtr<UBlackboardKeyType> KeyType;
	TWeakPtr<FStructuralPublicationStagingState> StagingState;
	TSharedPtr<FStructuralPublicationTerminalObservation> TerminalObservation;
};

ASSETDOCUMENTAI_API FStagedKeyTypeResult StageKeyType(
	UBlackboardData& Blackboard,
	const UClass* KeyTypeClass,
	FStructuralPublicationTransactionBinding& TransactionBinding);

ASSETDOCUMENTAI_API bool PublishLocalKeysSnapshot(
	UBlackboardData& Blackboard,
	const TArray<FBlackboardEntry>& Snapshot,
	FStructuralPublicationTransactionBinding& TransactionBinding,
	const TSharedPtr<IPropertyHandle>& KeysHandle);
} // namespace AssetDocumentAIBlackboardStructuralPublication
