// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h"


namespace AIBlackboardEntryRemoveLocal
{
using namespace AssetDocumentAIBlackboardMutation;
constexpr const TCHAR* ComponentId = TEXT("assetdocument.ai.blackboard.entry_remove.v2");

struct FEntryRemovePreflight
{
	UBlackboardData* Blackboard = nullptr;
	int32 TargetIndex = INDEX_NONE;
	TArray<FBlackboardEntry> Desired;
	TArray<FAssetDocumentRuntimeValueDto> ExpectedEntries;
	TArray<FString> ExpectedNames;
	TSharedPtr<IPropertyRowGenerator> Rows;
	TSharedPtr<IPropertyHandle> KeysHandle;
};

template <typename TApply>
FAssetDocumentPrimitiveOutcome ApplyWithCurrentAssetIdentity(
	const IAssetDocumentVerifiedHandleLease& Lease,
	TApply&& Apply)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	const FAssetDocumentContractValidationResult IdentityResult =
		WithAssetDocumentEditorAssetIdentity(
			Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
			{
				Outcome = Apply(Identity);
				return FAssetDocumentContractValidationResult{
					Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded, {}};
			});
	return !IdentityResult.bValid && Outcome.Diagnostics.IsEmpty()
		? Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The mutable Asset lease has no current Editor identity."))
		: Outcome;
}

bool IsExactOwnedEntry(
	const UBlackboardData& Blackboard,
	const FBlackboardEntry& Entry,
	const TSet<FName>& Names)
{
	return !Entry.EntryName.IsNone() && Entry.KeyType != nullptr
		&& Entry.KeyType->GetOuter() == &Blackboard && !Names.Contains(Entry.EntryName);
}

bool HasForbiddenParentShadow(
	const UBlackboardData& Blackboard,
	const FBlackboardEntry& Entry)
{
	return !IsEngineDerivedSelfKey(Entry)
		&& HasInheritedKey(Blackboard, Entry.EntryName);
}

bool IsRequestedAuthoredEntry(
	const FBlackboardEntry& Entry,
	const FName RequestedName)
{
	return !IsEngineDerivedSelfKey(Entry) && Entry.EntryName == RequestedName;
}

bool FindExactOwnedLocalTarget(
	const UBlackboardData& Blackboard,
	const FName RequestedName,
	int32& OutTargetIndex)
{
	TSet<FName> Names;
	for (int32 Index = 0; Index < Blackboard.Keys.Num(); ++Index)
	{
		const FBlackboardEntry& Entry = Blackboard.Keys[Index];
		if (!IsExactOwnedEntry(Blackboard, Entry, Names)) return false;
		Names.Add(Entry.EntryName);
		if (HasForbiddenParentShadow(Blackboard, Entry)) return false;
		if (IsRequestedAuthoredEntry(Entry, RequestedName)) OutTargetIndex = Index;
	}
	return true;
}

FAssetDocumentRuntimeValueDto EntryRemoveBooleanValue(const bool Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = Value;
	return Result;
}

bool ReadAuthoredEntry(
	UBlackboardData& Blackboard,
	const FBlackboardEntry& Entry,
	FAssetDocumentRuntimeValueDto& OutEntry)
{
	FAssetDocumentRuntimeValueDto Properties;
	if (Entry.KeyType == nullptr || Entry.KeyType->GetOuter() != &Blackboard
		|| !BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(
			*Entry.KeyType, Properties)) return false;
	OutEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(Entry.EntryCategory.IsNone()
				? FString() : Entry.EntryCategory.ToString()),
			StringValue(Entry.EntryDescription),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("ClassRef")),
					StringValue(Entry.KeyType->GetClass()->GetPathName())}),
			MoveTemp(Properties), StringValue(Entry.EntryName.ToString()),
			EntryRemoveBooleanValue(Entry.bInstanceSynced != 0)});
	return true;
}

bool ReadCompleteAuthoredEntries(
	UBlackboardData& Blackboard,
	const FName ExcludedName,
	TArray<FAssetDocumentRuntimeValueDto>& OutEntries,
	TArray<FString>& OutNames)
{
	OutEntries.Reset();
	OutNames.Reset();
	for (const FBlackboardEntry& Entry : Blackboard.Keys)
	{
		if (IsEngineDerivedSelfKey(Entry) || Entry.EntryName == ExcludedName) continue;
		FAssetDocumentRuntimeValueDto EntryValue;
		if (!ReadAuthoredEntry(Blackboard, Entry, EntryValue)) return false;
		OutEntries.Add(MoveTemp(EntryValue));
		OutNames.Add(Entry.EntryName.ToString());
	}
	return true;
}

TOptional<FAssetDocumentPrimitiveOutcome> PreflightEntryRemoveIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardEntryRemoveRequest& Request,
	UBlackboardData*& OutBlackboard)
{
	if (Identity.GetCanonicalObjectPath() != Request.BlackboardPath)
		return Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The mutable Asset lease does not identify the requested Blackboard."));
	UObject& Target = Identity.GetObject();
	if (Target.GetClass() != UBlackboardData::StaticClass())
		return Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The requested target is not an exact BlackboardData asset."));
	OutBlackboard = static_cast<UBlackboardData*>(&Target);
	if (!IsEditorPropertyContext(OutBlackboard))
		return Failure(TEXT("AIProvider.InvalidContext"),
			TEXT("Blackboard entry removal requires the Editor game thread."));
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> PreflightEntryRemoveTarget(
	FEntryRemovePreflight& Out,
	const FName RequestedName)
{
	if (!FindExactOwnedLocalTarget(*Out.Blackboard, RequestedName, Out.TargetIndex))
		return Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The complete current local-key snapshot is not authoritative."));
	if (Out.TargetIndex != INDEX_NONE) return {};
	const bool bInherited = IsVisibleInheritedBlackboardKey(*Out.Blackboard, RequestedName);
	return Failure(bInherited ? TEXT("AIProvider.Blackboard.KeyInherited")
		: TEXT("AIProvider.Blackboard.KeyMissing"), bInherited
			? TEXT("The requested Blackboard key is inherited, not local.")
			: TEXT("The requested existing local Blackboard key is unavailable."));
}

TOptional<FAssetDocumentPrimitiveOutcome> PreflightEntryRemoveLifecycle(
	FEntryRemovePreflight& Out)
{
	const TSet<FString> KeysAllowList = {
		GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys).ToString()};
	if (!TryMakeRows(*Out.Blackboard, Out.Rows, &KeysAllowList))
		return Failure(TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
			TEXT("The exact ordered Keys PropertyEditor lifecycle is unavailable."));
	Out.KeysHandle = FindUniqueOwnedPropertyHandle(
		*Out.Rows, *Out.Blackboard, GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
	if (!Out.KeysHandle.IsValid())
		return Failure(TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
			TEXT("The exact ordered Keys property handle is unavailable."));
	if (Out.KeysHandle->GetProperty() == nullptr)
		return Failure(TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
			TEXT("The exact ordered Keys property identity is unavailable."));
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> PreflightEntryRemove(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardEntryRemoveRequest& Request,
	FEntryRemovePreflight& Out)
{
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed =
		PreflightEntryRemoveIdentity(Identity, Request, Out.Blackboard))
		return MoveTemp(Failed.GetValue());
	const FName RequestedName(Request.KeyName);
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed =
		PreflightEntryRemoveTarget(Out, RequestedName))
		return MoveTemp(Failed.GetValue());
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed =
		PreflightEntryRemoveLifecycle(Out)) return MoveTemp(Failed.GetValue());
	if (!ReadCompleteAuthoredEntries(*Out.Blackboard, RequestedName,
		Out.ExpectedEntries, Out.ExpectedNames))
		return Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("A retained Blackboard entry has no complete canonical readback."));
	Out.Desired = Out.Blackboard->Keys;
	Out.Desired.RemoveAt(Out.TargetIndex);
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> CaptureEntryRemoveState(
	const IAssetDocumentVerifiedHandleLease& Lease,
	UBlackboardData& Blackboard,
	const FString& BlackboardPath,
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding& Binding)
{
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			Lease, Blackboard, {ComponentId}, Binding);
	if (!Capture.bSucceeded)
	{
		if (Binding.GetLoadedConsumerReceiptCaptureFailure()
			== AssetDocumentAIBlackboardLoadedConsumerCompletion::
				ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable)
			return Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The loaded BehaviorTree consumer coverage receipt is unavailable."));
		return FailureFromPreimage(Capture);
	}
	const FAssetDocumentContractValidationResult Footprint = Binding.EnlistFootprint({
		MakeFootprint(FString::Printf(TEXT("asset:%s/Keys[*]"), *BlackboardPath), ComponentId)});
	if (!Footprint.bValid) return FailureFromValidation(Footprint);
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> PublishEntryRemove(
	FEntryRemovePreflight& Preflight,
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding& Binding)
{
	UBlackboardData& Blackboard = *Preflight.Blackboard;
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::EntryRemovePublicationRejected))
		return Failure(TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
			TEXT("The test seam rejected the ordered Keys publisher."));
#endif
	if (!AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot(
		Blackboard, Preflight.Desired, Binding, Preflight.KeysHandle))
		return Failure(TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
			TEXT("The exact ordered Keys publisher rejected the removal snapshot."));
	Blackboard.UpdateIfHasSynchronizedKeys();
	Blackboard.PropagateKeyChangesToDerivedBlackboardAssets();
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> CompleteEntryRemoveConsumers(
	UBlackboardData& Blackboard,
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding& Binding)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (AssetDocumentAIBlackboardLoadedConsumerCompletion::
		ConsumeLoadedConsumerCompletionTestFailurePoint(
			AssetDocumentAIBlackboardLoadedConsumerCompletion::
				ELoadedConsumerCompletionTestFailurePoint::BeforeLoadedConsumerForwardCompletion))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The test seam rejected loaded-consumer completion before execution."));
#endif
	if (Binding.CompletePublishedLoadedConsumers(Blackboard)
		!= AssetDocumentAIBlackboardLoadedConsumerCompletion::
			ELoadedConsumerCompletionResult::Succeeded)
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("Loaded BehaviorTree consumer cache completion failed."));
#if WITH_DEV_AUTOMATION_TESTS
	if (AssetDocumentAIBlackboardLoadedConsumerCompletion::
		ConsumeLoadedConsumerCompletionTestFailurePoint(
			AssetDocumentAIBlackboardLoadedConsumerCompletion::
				ELoadedConsumerCompletionTestFailurePoint::AfterLoadedConsumerForwardCompletion))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The test seam rejected loaded-consumer completion after execution."));
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::AfterLocalKeyIdRefresh))
		return Failure(TEXT("AIProvider.OperationRejected"),
			TEXT("The test-only failure seam rejected removal after derived ID refresh."));
#endif
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> PublishAndCompleteEntryRemove(
	FEntryRemovePreflight& Preflight,
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding& Binding)
{
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed =
		PublishEntryRemove(Preflight, Binding)) return MoveTemp(Failed.GetValue());
	return CompleteEntryRemoveConsumers(*Preflight.Blackboard, Binding);
}

bool HasPublishedDerivedIdentity(
	const UBlackboardData& Blackboard,
	const FBlackboardEntry& Actual)
{
	const FBlackboard::FKey KeyId = Blackboard.GetKeyID(Actual.EntryName);
	const FBlackboardEntry* DerivedEntry = Blackboard.GetKey(KeyId);
	return KeyId != FBlackboard::InvalidKey && DerivedEntry != nullptr
		&& DerivedEntry->KeyType == Actual.KeyType;
}

bool IsPublishedEntryValid(
	const UBlackboardData& Blackboard,
	const FBlackboardEntry& Actual,
	const FBlackboardEntry& Desired)
{
	if (Actual.EntryName != Desired.EntryName || Actual.KeyType != Desired.KeyType) return false;
	if (IsEngineDerivedSelfKey(Actual)) return true;
	return HasPublishedDerivedIdentity(Blackboard, Actual);
}

bool HasExactRemovalCardinality(
	const UBlackboardData& Blackboard,
	TConstArrayView<FBlackboardEntry> Desired,
	const FName RemovedName)
{
	return Blackboard.Keys.Num() == Desired.Num()
		&& Blackboard.GetKeyID(RemovedName) == FBlackboard::InvalidKey;
}

bool ValidatePublishedEntryRemove(
	const UBlackboardData& Blackboard,
	TConstArrayView<FBlackboardEntry> Desired,
	const FName RemovedName)
{
	if (!HasExactRemovalCardinality(Blackboard, Desired, RemovedName)) return false;
	TSet<FName> Names;
	for (int32 Index = 0; Index < Desired.Num(); ++Index)
	{
		const FBlackboardEntry& Actual = Blackboard.Keys[Index];
		if (Names.Contains(Actual.EntryName)) return false;
		Names.Add(Actual.EntryName);
		if (!IsPublishedEntryValid(Blackboard, Actual, Desired[Index])) return false;
	}
	return true;
}

bool HasExpectedEntryRemoveReadbackShape(
	const FEntryRemovePreflight& Preflight,
	const TArray<FAssetDocumentRuntimeValueDto>& Entries,
	const TArray<FString>& Names)
{
	return Names == Preflight.ExpectedNames
		&& Entries.Num() == Preflight.ExpectedEntries.Num();
}

TOptional<FAssetDocumentPrimitiveOutcome> ReadFinalEntryRemoveState(
	FEntryRemovePreflight& Preflight,
	TArray<FAssetDocumentRuntimeValueDto>& OutEntries,
	TArray<FString>& OutNames)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::EntryRemoveBeforeReadback))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The test seam rejected final retained entry readback."));
#endif
	if (!ReadCompleteAuthoredEntries(
		*Preflight.Blackboard, NAME_None, OutEntries, OutNames))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("Final retained entry/order readback is unavailable."));
	if (!HasExpectedEntryRemoveReadbackShape(Preflight, OutEntries, OutNames))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("Final retained entry/order readback is unavailable."));
	for (int32 Index = 0; Index < OutEntries.Num(); ++Index)
	{
		if (!RuntimeValueEquals(OutEntries[Index], Preflight.ExpectedEntries[Index]))
			return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
				TEXT("A retained entry changed during removal."));
	}
	return {};
}

FAssetDocumentRuntimeValueDto EntryRemoveNamesValue(TConstArrayView<FString> Names)
{
	TArray<FAssetDocumentRuntimeValueDto> Values;
	Values.Reserve(Names.Num());
	for (const FString& Name : Names) Values.Add(StringValue(Name));
	return ArrayValue(MoveTemp(Values));
}

FAssetDocumentPrimitiveOutcome MakeEntryRemoveSuccess(
	const FAssetDocumentAIBlackboardEntryRemoveRequest& Request,
	TArray<FAssetDocumentRuntimeValueDto> Entries,
	TArray<FString> Names)
{
	const FString EntryPath = FString::Printf(
		TEXT("asset:%s/Keys[name=%s]"), *Request.BlackboardPath, *Request.KeyName);
	FAssetDocumentPrimitiveOutcome Outcome = MakeSuccess(
		ObjectValue({TEXT("entries"), TEXT("ordered_local_key_names"), TEXT("removed_name")},
			{ArrayValue(MoveTemp(Entries)), EntryRemoveNamesValue(Names),
				StringValue(Request.KeyName)}),
		TEXT("ai.blackboard.entry.remove.v2"), Request.BlackboardPath, EntryPath);
	FAssetDocumentChangedRecord OrderChange = Outcome.ChangedSet[0];
	OrderChange.Path = FString::Printf(
		TEXT("asset:%s/Keys.@Order"), *Request.BlackboardPath);
	Outcome.ChangedSet.Add(MoveTemp(OrderChange));
#if WITH_DEV_AUTOMATION_TESTS
	const bool bInjectedMismatch = ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::EntryRemoveChangedSetMismatch);
#else
	constexpr bool bInjectedMismatch = false;
#endif
	if (bInjectedMismatch || !Outcome.HandleResults.IsEmpty() || Outcome.ChangedSet.Num() != 2)
		return Failure(TEXT("AIProvider.Blackboard.EntryChangedSetMismatch"),
			TEXT("The exact entry-remove ChangedSet could not be proven."));
	return Outcome;
}

FAssetDocumentPrimitiveOutcome CompleteEntryRemoveOutcome(
	const FAssetDocumentAIBlackboardEntryRemoveRequest& Request,
	FEntryRemovePreflight& Preflight)
{
	UBlackboardData& Blackboard = *Preflight.Blackboard;
	if (!ValidatePublishedEntryRemove(
		Blackboard, Preflight.Desired, FName(Request.KeyName)))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The published removal identity and derived Key IDs are invalid."));
	TArray<FAssetDocumentRuntimeValueDto> Entries;
	TArray<FString> Names;
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed =
		ReadFinalEntryRemoveState(Preflight, Entries, Names))
		return MoveTemp(Failed.GetValue());
	return MakeEntryRemoveSuccess(Request, MoveTemp(Entries), MoveTemp(Names));
}

FAssetDocumentPrimitiveOutcome ApplyEntryRemoveWithIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardEntryRemoveRequest& Request,
	IAssetDocumentAtomicTransaction& Transaction)
{
	FEntryRemovePreflight Preflight;
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed =
		PreflightEntryRemove(Identity, Request, Preflight))
		return MoveTemp(Failed.GetValue());
	UBlackboardData& Blackboard = *Preflight.Blackboard;
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding
		Binding(Blackboard, ComponentId, Transaction);
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed =
		CaptureEntryRemoveState(Lease, Blackboard, Request.BlackboardPath, Binding))
		return MoveTemp(Failed.GetValue());
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed =
		PublishAndCompleteEntryRemove(Preflight, Binding))
		return MoveTemp(Failed.GetValue());
	return CompleteEntryRemoveOutcome(Request, Preflight);
}
} // namespace

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardEntryRemove(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBlackboardEntryRemoveLocal;
	FAssetDocumentAIBlackboardEntryRemoveRequest Request;
	if (!ParseAssetDocumentAIBlackboardEntryRemoveRequest(Invocation, Request))
		return Failure(TEXT("AIProvider.InvalidValueShape"),
			TEXT("The Blackboard entry-remove envelope is not canonical."));
	if (!IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
		return Failure(TEXT("AIProvider.InvalidTransaction"),
			TEXT("Blackboard entry removal requires the exact active transaction."));
	if (!IsEditorPropertyContext())
		return Failure(TEXT("AIProvider.InvalidContext"),
			TEXT("Blackboard entry removal requires the Editor game thread."));
	const FAssetDocumentVerifiedLeaseResult LeaseResult =
		AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
	if (LeaseResult.Lease == nullptr)
		return Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("A matching AI mutable Asset lease is required."), LeaseResult.Diagnostics);
	return ApplyWithCurrentAssetIdentity(
		*LeaseResult.Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
		{
			return ApplyEntryRemoveWithIdentity(
				Identity, *LeaseResult.Lease, Request, *ActiveTransaction);
		});
}
