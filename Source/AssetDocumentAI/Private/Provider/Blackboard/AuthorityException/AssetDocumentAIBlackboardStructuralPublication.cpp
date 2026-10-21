// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h"

#include "AssetDocumentRollbackAction.h"
#include "PropertyHandle.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

using namespace AssetDocumentAIBlackboardStructuralPublication;
using namespace AssetDocumentAIBlackboardLoadedConsumerCompletion;

class AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTerminalObservation
{
public:
	bool bRollbackVerified = false;
	bool bStrongOwnershipRetainedDuringRollback = false;
	bool bTransactionTerminalReleasedOwnership = false;
};

class AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationStagingState
{
public:
	FStructuralPublicationStagingState()
		: TerminalObservation(MakeShared<FStructuralPublicationTerminalObservation>())
	{
	}

	bool Retain(UBlackboardKeyType& KeyType)
	{
		if (OwnedKeyType.IsValid())
		{
			return false;
		}
		OwnedKeyType.Reset(&KeyType);
		return true;
	}

	bool CanRetain() const
	{
		return !OwnedKeyType.IsValid();
	}

	bool Owns(const UBlackboardKeyType* KeyType) const
	{
		return OwnedKeyType.IsValid() && OwnedKeyType.Get() == KeyType;
	}

	bool PreparePublishedSnapshot(TConstArrayView<FBlackboardEntry> Snapshot)
	{
		if (!OwnedKeyType.IsValid())
		{
			return true;
		}
		const UBlackboardKeyType* Staged = OwnedKeyType.Get();
		return Snapshot.ContainsByPredicate(
			[Staged](const FBlackboardEntry& Entry)
			{
				return Entry.KeyType.Get() == Staged;
			});
	}

	void CompleteRollback(const bool bVerified)
	{
		TerminalObservation->bRollbackVerified = bVerified;
		TerminalObservation->bStrongOwnershipRetainedDuringRollback =
			OwnedKeyType.IsValid();
	}

	void CompleteTerminal()
	{
		OwnedKeyType.Reset();
		TerminalObservation->bTransactionTerminalReleasedOwnership = true;
	}

	bool HasRollbackVerifiedTerminal() const
	{
		return TerminalObservation->bRollbackVerified;
	}

	TSharedPtr<FStructuralPublicationTerminalObservation> GetTerminalObservation() const
	{
		return TerminalObservation;
	}

private:
	TStrongObjectPtr<UBlackboardKeyType> OwnedKeyType;
	TSharedPtr<FStructuralPublicationTerminalObservation> TerminalObservation;
};

class AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationLoadedConsumerState
{
public:
	bool Capture(UBlackboardData& Target)
	{
		if (bCaptureAttempted)
		{
			return false;
		}
		bCaptureAttempted = true;
		auto Captured = CaptureLoadedBehaviorTreeConsumerReceipt(Target);
		if (!Captured.HasValue())
		{
			CaptureFailure = ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable;
			return false;
		}
		Receipt = MakeUnique<FLoadedBehaviorTreeConsumerReceipt>(Captured.StealValue());
		return true;
	}

	ELoadedConsumerCompletionResult Complete(
		UBlackboardData& Target,
		const ELoadedConsumerCompletionPhase Phase)
	{
		bool& PhaseCompleted = Phase == ELoadedConsumerCompletionPhase::ForwardPublished
			? bForwardCompleted : bRollbackCompleted;
		if (!Receipt.IsValid() || PhaseCompleted)
		{
			return ELoadedConsumerCompletionResult::DerivedRefreshFailed;
		}
		PhaseCompleted = true;
		const ELoadedConsumerCompletionResult Result =
			CompleteLoadedBehaviorTreeConsumerReceipt(*Receipt, Target, Phase);
		return Result;
	}

	ELoadedConsumerReceiptCaptureFailure GetCaptureFailure() const
	{
		return CaptureFailure;
	}

private:
	TUniquePtr<FLoadedBehaviorTreeConsumerReceipt> Receipt;
	ELoadedConsumerReceiptCaptureFailure CaptureFailure =
		ELoadedConsumerReceiptCaptureFailure::None;
	bool bCaptureAttempted = false;
	bool bForwardCompleted = false;
	bool bRollbackCompleted = false;
};

namespace
{
class FTransactionOwnedStagingRollbackAction final
	: public IAssetDocumentRollbackAction
{
public:
	FTransactionOwnedStagingRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> InInner,
		TSharedRef<FStructuralPublicationStagingState> InStagingState,
		TSharedRef<FStructuralPublicationLoadedConsumerState> InLoadedConsumerState,
		UBlackboardData& InExactTarget)
		: Inner(MoveTemp(InInner))
		, StagingState(MoveTemp(InStagingState))
		, LoadedConsumerState(MoveTemp(InLoadedConsumerState))
		, ExactTarget(&InExactTarget)
	{
	}

	virtual ~FTransactionOwnedStagingRollbackAction() override
	{
		StagingState->CompleteTerminal();
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return Inner->GetRollbackComponentId();
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		FAssetDocumentContractValidationResult Result =
			Inner->Rollback(StableFailureCode);
		if (Result.bValid && (!ExactTarget.IsValid()
			|| LoadedConsumerState->Complete(
				*ExactTarget.Get(), ELoadedConsumerCompletionPhase::RollbackRestored)
				!= ELoadedConsumerCompletionResult::Succeeded))
		{
			Result.bValid = false;
		}
		StagingState->CompleteRollback(Result.bValid);
		return Result;
	}

private:
	TSharedRef<IAssetDocumentRollbackAction> Inner;
	TSharedRef<FStructuralPublicationStagingState> StagingState;
	TSharedRef<FStructuralPublicationLoadedConsumerState> LoadedConsumerState;
	TWeakObjectPtr<UBlackboardData> ExactTarget;
};

bool IsPrevalidatedKeyTypeClass(const UClass* KeyTypeClass)
{
	return KeyTypeClass != nullptr
		&& KeyTypeClass->IsChildOf(UBlackboardKeyType::StaticClass())
		&& !KeyTypeClass->HasAnyClassFlags(
			CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
}

bool IsExactKeysHandle(
	const UBlackboardData& Blackboard,
	const TSharedPtr<IPropertyHandle>& KeysHandle)
{
	const FProperty* Expected = FindFProperty<FProperty>(
		UBlackboardData::StaticClass(), GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
	const FProperty* Actual = KeysHandle.IsValid() ? KeysHandle->GetProperty() : nullptr;
	const bool bExactIdentity = Expected != nullptr && Actual != nullptr && Actual == Expected
		&& Actual->GetOwnerClass() == UBlackboardData::StaticClass()
		&& Actual->GetPathName() == Expected->GetPathName();
	if (!bExactIdentity
		|| KeysHandle->GeneratePathToProperty()
			!= GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys).ToString())
	{
		return false;
	}
	TArray<UObject*> OuterObjects;
	KeysHandle->GetOuterObjects(OuterObjects);
	return OuterObjects.Num() == 1 && OuterObjects[0] == &Blackboard;
}

bool HasMatchingTransactionIdentity(
	const FGuid& TransactionId,
	const FAssetDocumentMutationToken& Current,
	const FAssetDocumentMutationToken& Initial)
{
	return TransactionId == Initial.TransactionId
		&& Current.RequestId == Initial.RequestId
		&& Current.TransactionId == Initial.TransactionId
		&& Current.RuntimeRevision == Initial.RuntimeRevision;
}

UBlackboardKeyType* ConstructAndRetainStagedKeyType(
	UBlackboardData& Blackboard,
	const UClass* KeyTypeClass,
	FStructuralPublicationTransactionBinding& TransactionBinding)
{
	if (!TransactionBinding.CanRetainStagedKeyType())
	{
		return nullptr;
	}
	UBlackboardKeyType* KeyType = NewObject<UBlackboardKeyType>(
		&Blackboard,
		KeyTypeClass,
		MakeUniqueObjectName(&Blackboard, KeyTypeClass,
			TEXT("AssetDocumentAIBlackboardStagedKeyType")),
		RF_Transactional);
	if (KeyType == nullptr)
	{
		return nullptr;
	}
	if (!TransactionBinding.TryRetainStagedKeyType(*KeyType))
	{
		return nullptr;
	}
	return KeyType;
}
} // namespace

FStructuralPublicationTransactionBinding::FStructuralPublicationTransactionBinding(
	UBlackboardData& InExactTarget,
	FString InRollbackComponentId,
	IAssetDocumentAtomicTransaction& InTransaction)
	: ExactTarget(&InExactTarget)
	, RollbackComponentId(MoveTemp(InRollbackComponentId))
	, Transaction(&InTransaction)
	, InitialToken(InTransaction.GetMutationToken())
	, StagingState(MakeShared<FStructuralPublicationStagingState>())
	, LoadedConsumerState(MakeShared<FStructuralPublicationLoadedConsumerState>())
{
}

FStructuralPublicationTransactionBinding::~FStructuralPublicationTransactionBinding() = default;

FGuid FStructuralPublicationTransactionBinding::GetTransactionId() const
{
	return Transaction->GetTransactionId();
}

EAssetDocumentAtomicState FStructuralPublicationTransactionBinding::GetState() const
{
	return Transaction->GetState();
}

const FAssetDocumentMutationToken&
FStructuralPublicationTransactionBinding::GetMutationToken() const
{
	return Transaction->GetMutationToken();
}

FAssetDocumentContractValidationResult
FStructuralPublicationTransactionBinding::EnlistFootprint(
	TConstArrayView<FAssetDocumentMutationFootprint> Footprint)
{
	if (Footprint.Num() == 1
		&& Footprint[0].RollbackComponentId == RollbackComponentId
		&& bExactFootprintEnlisted)
	{
		FAssetDocumentContractValidationResult Rejected;
		Rejected.bValid = false;
		return Rejected;
	}
	const FAssetDocumentContractValidationResult Result =
		Transaction->EnlistFootprint(Footprint);
	if (Result.bValid && bTypedPreimageActionEnlisted && Footprint.Num() == 1
		&& Footprint[0].RollbackComponentId == RollbackComponentId)
	{
		bExactFootprintEnlisted = true;
	}
	return Result;
}

FAssetDocumentContractValidationResult
FStructuralPublicationTransactionBinding::MarkReadyToCommit()
{
	return Transaction->MarkReadyToCommit();
}

FAssetDocumentContractValidationResult FStructuralPublicationTransactionBinding::Commit(
	const FAssetDocumentDurableCompletionReceipt& Receipt)
{
	return Transaction->Commit(Receipt);
}

FAssetDocumentContractValidationResult FStructuralPublicationTransactionBinding::Rollback(
	FStringView StableFailureCode)
{
	return Transaction->Rollback(StableFailureCode);
}

FAssetDocumentContractValidationResult
FStructuralPublicationTransactionBinding::EnlistRollbackAction(
	TSharedRef<IAssetDocumentRollbackAction> Action)
{
	if (Action->GetRollbackComponentId() != RollbackComponentId)
	{
		return Transaction->EnlistRollbackAction(MoveTemp(Action));
	}
	if (bTypedPreimageActionEnlisted)
	{
		FAssetDocumentContractValidationResult Rejected;
		Rejected.bValid = false;
		return Rejected;
	}
	if (!ExactTarget.IsValid() || !LoadedConsumerState->Capture(*ExactTarget.Get()))
	{
		FAssetDocumentContractValidationResult Rejected;
		Rejected.bValid = false;
		return Rejected;
	}
	const TSharedRef<IAssetDocumentRollbackAction> Wrapped =
		MakeShared<FTransactionOwnedStagingRollbackAction>(
			MoveTemp(Action), StagingState.ToSharedRef(),
			LoadedConsumerState.ToSharedRef(), *ExactTarget.Get());
	const FAssetDocumentContractValidationResult Result =
		Transaction->EnlistRollbackAction(Wrapped);
	bTypedPreimageActionEnlisted = Result.bValid;
	return Result;
}

FAssetDocumentContractValidationResult
FStructuralPublicationTransactionBinding::EnlistPostRollbackFinalizer(
	TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer)
{
	return Transaction->EnlistPostRollbackFinalizer(MoveTemp(Finalizer));
}

bool FStructuralPublicationTransactionBinding::HasCurrentIdentity() const
{
	if (Transaction == nullptr || !ExactTarget.IsValid()
		|| RollbackComponentId.IsEmpty())
	{
		return false;
	}
	const FAssetDocumentMutationToken& Current = Transaction->GetMutationToken();
	return HasMatchingTransactionIdentity(
		Transaction->GetTransactionId(), Current, InitialToken);
}

bool FStructuralPublicationTransactionBinding::IsReadyFor(
	const UBlackboardData& Blackboard) const
{
	return ExactTarget.Get() == &Blackboard
		&& HasCurrentIdentity()
		&& bTypedPreimageActionEnlisted
		&& bExactFootprintEnlisted
		&& Transaction->GetState() == EAssetDocumentAtomicState::Mutating;
}

bool FStructuralPublicationTransactionBinding::IsBoundTo(
	const UBlackboardData& Blackboard) const
{
	return ExactTarget.Get() == &Blackboard;
}

bool FStructuralPublicationTransactionBinding::TryRetainStagedKeyType(
	UBlackboardKeyType& KeyType)
{
	return ExactTarget.Get() == KeyType.GetOuter()
		&& StagingState->Retain(KeyType);
}

bool FStructuralPublicationTransactionBinding::CanRetainStagedKeyType() const
{
	return StagingState->CanRetain();
}

bool FStructuralPublicationTransactionBinding::PreparePublishedSnapshot(
	TConstArrayView<FBlackboardEntry> Snapshot)
{
	return StagingState->PreparePublishedSnapshot(Snapshot);
}

ELoadedConsumerReceiptCaptureFailure
FStructuralPublicationTransactionBinding::GetLoadedConsumerReceiptCaptureFailure() const
{
	return LoadedConsumerState->GetCaptureFailure();
}

ELoadedConsumerCompletionResult
FStructuralPublicationTransactionBinding::CompletePublishedLoadedConsumers(
	UBlackboardData& Blackboard)
{
	if (!IsReadyFor(Blackboard))
	{
		return ELoadedConsumerCompletionResult::DerivedRefreshFailed;
	}
	return LoadedConsumerState->Complete(
		Blackboard, ELoadedConsumerCompletionPhase::ForwardPublished);
}

bool FStructuralPublicationTransactionBinding::HasRollbackVerifiedTerminal() const
{
	return StagingState->HasRollbackVerifiedTerminal();
}

TWeakPtr<FStructuralPublicationStagingState>
FStructuralPublicationTransactionBinding::GetStagingState() const
{
	return StagingState;
}

TSharedPtr<FStructuralPublicationTerminalObservation>
FStructuralPublicationTransactionBinding::GetTerminalObservation() const
{
	return StagingState->GetTerminalObservation();
}

FStagedKeyTypeResult::FStagedKeyTypeResult(
	const EStageKeyTypeFailure InFailure,
	UBlackboardKeyType* InKeyType,
	TWeakPtr<FStructuralPublicationStagingState> InStagingState,
	TSharedPtr<FStructuralPublicationTerminalObservation> InTerminalObservation)
	: Failure(InFailure)
	, KeyType(InKeyType)
	, StagingState(MoveTemp(InStagingState))
	, TerminalObservation(MoveTemp(InTerminalObservation))
{
}

UBlackboardKeyType* FStagedKeyTypeResult::Get() const
{
	return KeyType.Get();
}

bool FStagedKeyTypeResult::HasStrongOwnership() const
{
	const TSharedPtr<FStructuralPublicationStagingState> State = StagingState.Pin();
	return State.IsValid() && State->Owns(KeyType.Get());
}

bool FStagedKeyTypeResult::HasRollbackVerifiedTerminal() const
{
	return TerminalObservation.IsValid()
		&& TerminalObservation->bRollbackVerified;
}

bool FStagedKeyTypeResult::WasStrongOwnershipRetainedDuringRollback() const
{
	return TerminalObservation.IsValid()
		&& TerminalObservation->bStrongOwnershipRetainedDuringRollback;
}

bool FStagedKeyTypeResult::HasTransactionTerminalReleasedOwnership() const
{
	return TerminalObservation.IsValid()
		&& TerminalObservation->bTransactionTerminalReleasedOwnership;
}

FStagedKeyTypeResult AssetDocumentAIBlackboardStructuralPublication::StageKeyType(
	UBlackboardData& Blackboard,
	const UClass* KeyTypeClass,
	FStructuralPublicationTransactionBinding& TransactionBinding)
{
	FStagedKeyTypeResult Result;
	if (!TransactionBinding.IsBoundTo(Blackboard))
	{
		Result.Failure = EStageKeyTypeFailure::InvalidTarget;
		return Result;
	}
	if (!IsPrevalidatedKeyTypeClass(KeyTypeClass))
	{
		Result.Failure = EStageKeyTypeFailure::InvalidKeyTypeClass;
		return Result;
	}
	if (!TransactionBinding.IsReadyFor(Blackboard))
	{
		Result.Failure = EStageKeyTypeFailure::MissingTransactionPreconditions;
		return Result;
	}
	UBlackboardKeyType* KeyType = ConstructAndRetainStagedKeyType(
		Blackboard, KeyTypeClass, TransactionBinding);
	if (KeyType == nullptr)
	{
		return Result;
	}
	return FStagedKeyTypeResult(
		EStageKeyTypeFailure::None,
		KeyType,
		TransactionBinding.GetStagingState(),
		TransactionBinding.GetTerminalObservation());
}

bool AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot(
	UBlackboardData& Blackboard,
	const TArray<FBlackboardEntry>& Snapshot,
	FStructuralPublicationTransactionBinding& TransactionBinding,
	const TSharedPtr<IPropertyHandle>& KeysHandle)
{
	if (!TransactionBinding.IsReadyFor(Blackboard)
		|| !IsExactKeysHandle(Blackboard, KeysHandle)
		|| !TransactionBinding.PreparePublishedSnapshot(Snapshot))
	{
		return false;
	}
	KeysHandle->NotifyPreChange();
	Blackboard.Keys = Snapshot;
	KeysHandle->NotifyPostChange(EPropertyChangeType::ValueSet);
	KeysHandle->NotifyFinishedChangingProperties();
	Blackboard.UpdateParentKeys();
	Blackboard.UpdateKeyIDs();
	return true;
}
