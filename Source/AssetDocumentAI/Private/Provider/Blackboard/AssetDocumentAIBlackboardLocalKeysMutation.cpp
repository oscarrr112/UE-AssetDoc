// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h"


namespace AIBlackboardLocalKeysMutationLocal
{
using namespace AssetDocumentAIBlackboardMutation;
struct FLocalKeyWritePreparation
{
	TArray<FBlackboardEntry> Desired;
	TArray<TArray<TSharedRef<IPropertyHandle>>> PropertyHandles;
	TArray<TArray<const FAssetDocumentRuntimeValueDto*>> PropertyValues;
	TArray<TSharedPtr<IPropertyRowGenerator>> PropertyRows;
};

bool FindDerivedSelfKey(
	const UBlackboardData& Blackboard,
	TOptional<FBlackboardEntry>& OutDerivedSelf,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	for (const FBlackboardEntry& Existing : Blackboard.Keys)
	{
		if (!IsEngineDerivedSelfKey(Existing)) continue;
		if (OutDerivedSelf.IsSet())
		{
			OutFailure = Failure(TEXT("AIProvider.Blackboard.DuplicateKey"),
				TEXT("The engine-derived SelfActor key is not unique."));
			return false;
		}
		OutDerivedSelf = Existing;
	}
	return true;
}

const FBlackboardEntry* FindExistingKey(const UBlackboardData& Blackboard, const FName Name)
{
	for (const FBlackboardEntry& Existing : Blackboard.Keys)
	{
		if (Existing.EntryName == Name) return &Existing;
	}
	return nullptr;
}

bool PrepareOneLocalKey(
	const FAssetDocumentAIBlackboardLocalKeyRequest& Row,
	UBlackboardData& Blackboard,
	TSet<FName>& Names,
	FLocalKeyWritePreparation& OutPreparation,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const FName Name(*Row.KeyName);
	if (Names.Contains(Name))
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.DuplicateKey"),
			TEXT("Blackboard local key names must be unique."));
		return false;
	}
	if (Name == FBlackboard::KeySelf)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.ShadowConflict"),
			TEXT("The engine-derived SelfActor key is not an authored local row."));
		return false;
	}
	Names.Add(Name);
	if (HasInheritedKey(Blackboard, Name))
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.ShadowConflict"),
			TEXT("A local Blackboard key cannot shadow an inherited key."));
		return false;
	}
	const FBlackboardEntry* Existing = FindExistingKey(Blackboard, Name);
	UBlackboardKeyType* KeyType = Existing != nullptr ? Existing->KeyType.Get() : nullptr;
	if (KeyType == nullptr || KeyType->GetOuter() != &Blackboard
		|| KeyType->GetClass()->GetPathName() != Row.KeyTypeClassPath)
	{
		OutFailure = NewKeyAuthorityFailure(
			TEXT("A new Blackboard KeyType instance or replacement class requires the frozen G1 authority."));
		return false;
	}
	TSharedPtr<IPropertyRowGenerator> Rows;
	TArray<TSharedRef<IPropertyHandle>> Handles;
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	if (!BuildPropertyWrites(*KeyType, *Row.CanonicalProperties, Rows, Handles, Values))
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The PropertyEditor module or an admitted local key-type property is unavailable."));
		return false;
	}
	OutPreparation.Desired.Add(*Existing);
	OutPreparation.PropertyRows.Add(Rows);
	OutPreparation.PropertyHandles.Add(MoveTemp(Handles));
	OutPreparation.PropertyValues.Add(MoveTemp(Values));
	return true;
}

bool PrepareLocalKeyWrite(
	const FAssetDocumentAIBlackboardLocalKeysRequest& Request,
	UBlackboardData& Blackboard,
	FLocalKeyWritePreparation& OutPreparation,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	TOptional<FBlackboardEntry> DerivedSelf;
	if (!FindDerivedSelfKey(Blackboard, DerivedSelf, OutFailure)) return false;
	TSet<FName> Names;
	for (const FAssetDocumentAIBlackboardLocalKeyRequest& Row : Request.OrderedKeys)
	{
		if (!PrepareOneLocalKey(Row, Blackboard, Names, OutPreparation, OutFailure)) return false;
	}
	if (DerivedSelf.IsSet()) OutPreparation.Desired.Add(DerivedSelf.GetValue());
	return true;
}

bool EnlistLocalKeyWrite(
	const IAssetDocumentVerifiedHandleLease& Lease,
	UBlackboardData& Blackboard,
	const FString& StablePath,
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding& TransactionBinding,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const TCHAR* ComponentId = TEXT("assetdocument.ai.blackboard.local_keys.v1");
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			Lease, Blackboard, {ComponentId}, TransactionBinding);
	if (!Capture.bSucceeded)
	{
		if (TransactionBinding.GetLoadedConsumerReceiptCaptureFailure()
			== AssetDocumentAIBlackboardLoadedConsumerCompletion::
				ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable)
		{
			OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The loaded BehaviorTree consumer coverage receipt is unavailable."));
		}
		else
		{
			OutFailure = FailureFromPreimage(Capture);
		}
		return false;
	}
	const FAssetDocumentContractValidationResult Enlisted =
		TransactionBinding.EnlistFootprint({MakeFootprint(StablePath, ComponentId)});
	if (!Enlisted.bValid)
	{
		OutFailure = FailureFromValidation(Enlisted);
		return false;
	}
	return true;
}

bool WriteExistingKeyProperties(
	const FLocalKeyWritePreparation& Preparation,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	for (int32 RowIndex = 0; RowIndex < Preparation.PropertyHandles.Num(); ++RowIndex)
	{
		for (int32 PropertyIndex = 0; PropertyIndex < Preparation.PropertyHandles[RowIndex].Num(); ++PropertyIndex)
		{
			if (WriteLeaf(Preparation.PropertyHandles[RowIndex][PropertyIndex].Get(),
				*Preparation.PropertyValues[RowIndex][PropertyIndex])) continue;
			OutFailure = Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
				TEXT("An existing local Blackboard KeyType property write was rejected."));
			return false;
		}
	}
	return true;
}

bool ReplaceLocalKeysThroughPropertyLifecycle(
	UBlackboardData& Blackboard,
	const TArray<FBlackboardEntry>& Desired,
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding& TransactionBinding,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	TSharedPtr<IPropertyRowGenerator> Rows;
	const TSet<FString> KeysAllowList = {GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys).ToString()};
	if (!TryMakeRows(Blackboard, Rows, &KeysAllowList))
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The PropertyEditor module is unavailable for the Blackboard Keys lifecycle."));
		return false;
	}
	const TSharedPtr<IPropertyHandle> KeysHandle = FindUniqueOwnedPropertyHandle(
		*Rows, Blackboard, GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
	if (!KeysHandle.IsValid() || KeysHandle->GetProperty() == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The exact Blackboard Keys property handle is unavailable."));
		return false;
	}
	if (!AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot(
		Blackboard, Desired, TransactionBinding, KeysHandle))
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The exact Blackboard Keys property handle is unavailable."));
		return false;
	}
	return true;
}

bool CollectInheritedKeyNames(
	const UBlackboardData& Blackboard,
	TArray<FAssetDocumentRuntimeValueDto>& OutInherited)
{
	for (int32 Index = 0; Index < Blackboard.GetNumKeys() - Blackboard.Keys.Num(); ++Index)
	{
		const FBlackboardEntry* Entry = Blackboard.GetKey(FBlackboard::FKey(Index));
		if (Entry == nullptr) return false;
		if (!IsEngineDerivedSelfKey(*Entry)) OutInherited.Add(StringValue(Entry->EntryName.ToString()));
	}
	return true;
}

bool HasContiguousKeyIds(const UBlackboardData& Blackboard)
{
	for (int32 Index = 0; Index < Blackboard.GetNumKeys(); ++Index)
	{
		if (Blackboard.GetKey(FBlackboard::FKey(Index)) == nullptr) return false;
	}
	return true;
}

FAssetDocumentPrimitiveOutcome BuildLocalKeysSuccess(
	const FAssetDocumentAIBlackboardLocalKeysRequest& Request,
	const UBlackboardData& Blackboard,
	const FLocalKeyWritePreparation& Preparation,
	const FString& StablePath)
{
	if (Blackboard.Keys.Num() != Preparation.Desired.Num())
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The Blackboard local-key order did not persist."));
	}
	TArray<FAssetDocumentRuntimeValueDto> Rows;
	for (int32 Index = 0; Index < Request.OrderedKeys.Num(); ++Index)
	{
		const FBlackboardEntry& Entry = Blackboard.Keys[Index];
		if (!(Entry == Preparation.Desired[Index]) || Entry.KeyType.Get() != Preparation.Desired[Index].KeyType.Get())
		{
			return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
				TEXT("The Blackboard local-key metadata or identity changed during refresh."));
		}
		FAssetDocumentRuntimeValueDto ActualProperties;
		if (!BuildCanonicalProperties(Preparation.PropertyHandles[Index], ActualProperties))
		{
			return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
				TEXT("A local Blackboard key-type has no canonical public readback."));
		}
		Rows.Add(ObjectValue({TEXT("canonical_properties"), TEXT("key_type_class_path"), TEXT("name")},
			{MoveTemp(ActualProperties), StringValue(Entry.KeyType->GetClass()->GetPathName()),
				StringValue(Entry.EntryName.ToString())}));
	}
	TArray<FAssetDocumentRuntimeValueDto> Inherited;
	if (!CollectInheritedKeyNames(Blackboard, Inherited))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The inherited Blackboard projection is incomplete."));
	}
	if (!HasContiguousKeyIds(Blackboard))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The Blackboard key IDs are not contiguous."));
	}
	return MakeSuccess(
		ObjectValue({TEXT("inherited_key_names"), TEXT("ordered_keys")},
			{ArrayValue(MoveTemp(Inherited)), ArrayValue(MoveTemp(Rows))}),
		TEXT("ai.blackboard.local_keys.replace.v1"), Request.BlackboardPath, StablePath);
}

bool MatchesLocalKeyIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardLocalKeysRequest& Request,
	const UBlackboardData& Blackboard)
{
	return &Identity.GetObject() == &Blackboard
		&& Identity.GetCanonicalObjectPath() == Request.BlackboardPath
		&& IsEditorPropertyContext(&Blackboard);
}

bool ApplyLocalKeyMutation(
	const IAssetDocumentVerifiedHandleLease& Lease,
	UBlackboardData& Blackboard,
	const FLocalKeyWritePreparation& Preparation,
	const FString& StablePath,
	IAssetDocumentAtomicTransaction& Transaction,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const FString ComponentId = TEXT("assetdocument.ai.blackboard.local_keys.v1");
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding
		TransactionBinding(Blackboard, ComponentId, Transaction);
	if (!EnlistLocalKeyWrite(
		Lease, Blackboard, StablePath, TransactionBinding, OutFailure))
	{
		return false;
	}
	if (!WriteExistingKeyProperties(Preparation, OutFailure))
	{
		return false;
	}
	if (!ReplaceLocalKeysThroughPropertyLifecycle(
		Blackboard, Preparation.Desired, TransactionBinding, OutFailure))
	{
		return false;
	}
	Blackboard.UpdateIfHasSynchronizedKeys();
	Blackboard.PropagateKeyChangesToDerivedBlackboardAssets();
#if WITH_DEV_AUTOMATION_TESTS
	if (AssetDocumentAIBlackboardLoadedConsumerCompletion::
		ConsumeLoadedConsumerCompletionTestFailurePoint(
			AssetDocumentAIBlackboardLoadedConsumerCompletion::
				ELoadedConsumerCompletionTestFailurePoint::BeforeLoadedConsumerForwardCompletion))
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The test seam rejected loaded-consumer completion before execution."));
		return false;
	}
#endif
	if (TransactionBinding.CompletePublishedLoadedConsumers(Blackboard)
		!= AssetDocumentAIBlackboardLoadedConsumerCompletion::
			ELoadedConsumerCompletionResult::Succeeded)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("Loaded BehaviorTree consumer cache completion failed."));
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (AssetDocumentAIBlackboardLoadedConsumerCompletion::
		ConsumeLoadedConsumerCompletionTestFailurePoint(
			AssetDocumentAIBlackboardLoadedConsumerCompletion::
				ELoadedConsumerCompletionTestFailurePoint::AfterLoadedConsumerForwardCompletion))
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The test seam rejected loaded-consumer completion after execution."));
		return false;
	}
#endif
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::AfterLocalKeyIdRefresh))
	{
		OutFailure = Failure(TEXT("AIProvider.OperationRejected"),
			TEXT("The test-only failure seam rejected the local-key mutation after derived ID refresh."));
		return false;
	}
#endif
	return true;
}

FAssetDocumentPrimitiveOutcome ExecuteLocalKeyReplace(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardLocalKeysRequest& Request,
	UBlackboardData& Blackboard,
	const FLocalKeyWritePreparation& Preparation,
	IAssetDocumentAtomicTransaction& Transaction)
{
	if (!MatchesLocalKeyIdentity(Identity, Request, Blackboard))
	{
		return Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The Asset lease does not identify the requested Blackboard."));
	}
	const FString StablePath = FString::Printf(TEXT("asset:%s/Keys[*]"), *Request.BlackboardPath);
	FAssetDocumentPrimitiveOutcome FailureOutcome;
	if (!ApplyLocalKeyMutation(Lease, Blackboard, Preparation,
		StablePath, Transaction, FailureOutcome)) return FailureOutcome;
	return BuildLocalKeysSuccess(Request, Blackboard, Preparation, StablePath);
}

bool PrepareLocalKeysMutation(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FAssetDocumentAIBlackboardLocalKeysRequest& OutRequest,
	UBlackboardData*& OutBlackboard,
	FLocalKeyWritePreparation& OutPreparation,
	FAssetDocumentVerifiedLeaseResult& OutLease,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	if (!ParseAssetDocumentAIBlackboardLocalKeysRequest(Invocation, OutRequest))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The Blackboard local-key request is not canonical."));
		return false;
	}
	OutBlackboard = FindBlackboard(OutRequest.BlackboardPath);
	if (OutBlackboard == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The requested Blackboard is unavailable."));
		return false;
	}
	if (!IsEditorPropertyContext())
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidContext"), TEXT("Blackboard local-key mutation requires the Editor game thread."));
		return false;
	}
	if (!PrepareLocalKeyWrite(OutRequest, *OutBlackboard, OutPreparation, OutFailure)) return false;
	OutLease = AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
	if (OutLease.Lease != nullptr) return true;
	OutFailure = Failure(TEXT("AIProvider.InvalidHandle"),
		TEXT("A matching AI mutable Asset lease is required."), OutLease.Diagnostics);
	return false;
}

FAssetDocumentPrimitiveOutcome RunLocalKeysMutation(
	const FAssetDocumentAIBlackboardLocalKeysRequest& Request,
	UBlackboardData& Blackboard,
	const FLocalKeyWritePreparation& Preparation,
	const FAssetDocumentVerifiedLeaseResult& LeaseResult,
	IAssetDocumentAtomicTransaction& Transaction)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	const FAssetDocumentContractValidationResult IdentityResult = WithAssetDocumentEditorAssetIdentity(
		*LeaseResult.Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
		{
			Outcome = ExecuteLocalKeyReplace(
				Identity, *LeaseResult.Lease, Request, Blackboard, Preparation, Transaction);
			FAssetDocumentContractValidationResult Result;
			Result.bValid = Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
			return Result;
		});
	if (!IdentityResult.bValid && Outcome.Diagnostics.IsEmpty())
	{
		return Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The mutable Asset lease has no current Editor identity."), IdentityResult.Diagnostics);
	}
	return Outcome;
}
} // namespace

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardLocalKeysReplace(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBlackboardLocalKeysMutationLocal;
	FAssetDocumentAIBlackboardLocalKeysRequest Request;
	UBlackboardData* Blackboard = nullptr;
	FLocalKeyWritePreparation Preparation;
	FAssetDocumentPrimitiveOutcome FailureOutcome;
	FAssetDocumentVerifiedLeaseResult LeaseResult;
	if (!PrepareLocalKeysMutation(Invocation, Handles, Request, Blackboard, Preparation, LeaseResult, FailureOutcome)) return FailureOutcome;
	return RunLocalKeysMutation(Request, *Blackboard, Preparation, LeaseResult, *ActiveTransaction);
}
