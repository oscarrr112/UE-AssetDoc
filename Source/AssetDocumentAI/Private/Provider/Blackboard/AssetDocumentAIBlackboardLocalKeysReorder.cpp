// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h"


namespace AIBlackboardLocalKeysReorderLocal
{
using namespace AssetDocumentAIBlackboardMutation;
constexpr const TCHAR* ComponentId = TEXT("assetdocument.ai.blackboard.local_keys_reorder.v2");
constexpr const TCHAR* PrimitiveId = TEXT("ai.blackboard.local_keys.reorder.v2");

struct FLocalKeysReorderPreflight
{
	UBlackboardData* Blackboard = nullptr;
	TArray<FBlackboardEntry> Desired;
	TArray<FAssetDocumentRuntimeValueDto> ExpectedEntries;
	TArray<FString> ExpectedNames;
	TMap<FString, int32> BeforePhysicalIndices;
	TMap<FString, FBlackboard::FKey> BeforeKeyIds;
	FBlackboard::FKey BeforeSelfKeyId = FBlackboard::InvalidKey;
	TSharedPtr<IPropertyRowGenerator> Rows;
	TSharedPtr<IPropertyHandle> KeysHandle;
	bool bIdentity = false;
};

template <typename TApply>
FAssetDocumentPrimitiveOutcome ApplyWithReorderIdentity(
	const IAssetDocumentVerifiedHandleLease& Lease, TApply&& Apply)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	const FAssetDocumentContractValidationResult Result = WithAssetDocumentEditorAssetIdentity(
		Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
		{
			Outcome = Apply(Identity);
			FAssetDocumentContractValidationResult CallbackResult;
			CallbackResult.bValid = Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
			return CallbackResult;
		});
	return !Result.bValid && Outcome.Diagnostics.IsEmpty()
		? Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The mutable Asset lease has no current Editor identity.")) : Outcome;
}

FAssetDocumentRuntimeValueDto BooleanValue(const bool Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = Value;
	return Result;
}

bool ReadAuthoredEntry(UBlackboardData& Blackboard, const FBlackboardEntry& Entry,
	FAssetDocumentRuntimeValueDto& OutEntry)
{
	FAssetDocumentRuntimeValueDto Properties;
	if (Entry.KeyType == nullptr || Entry.KeyType->GetOuter() != &Blackboard
		|| !BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(*Entry.KeyType, Properties)) return false;
	OutEntry = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(Entry.EntryCategory.IsNone() ? FString() : Entry.EntryCategory.ToString()),
			StringValue(Entry.EntryDescription), ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("ClassRef")), StringValue(Entry.KeyType->GetClass()->GetPathName())}),
			MoveTemp(Properties), StringValue(Entry.EntryName.ToString()),
			BooleanValue(Entry.bInstanceSynced != 0)});
	return true;
}

bool IsExactAuthoredReorderEntry(const UBlackboardData& Blackboard,
	const FBlackboardEntry& Entry)
{
	return Entry.KeyType != nullptr && Entry.KeyType->GetOuter() == &Blackboard
		&& !HasInheritedKey(Blackboard, Entry.EntryName);
}

TOptional<FAssetDocumentPrimitiveOutcome> PreflightReorderIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardLocalKeysReorderRequest& Request,
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
			TEXT("Blackboard local-key reorder requires the Editor game thread."));
	return {};
}

bool RegisterReorderPhysicalName(const FBlackboardEntry& Entry,
	TSet<FName>& InOutNames, FString& OutName)
{
	OutName = Entry.EntryName.ToString();
	if (Entry.EntryName.IsNone() || InOutNames.Contains(Entry.EntryName)) return false;
	InOutNames.Add(Entry.EntryName);
	return true;
}

bool HasExactReorderDerivedIdentity(const UBlackboardData& Blackboard,
	const FBlackboardEntry& Entry, FBlackboard::FKey& OutKeyId)
{
	OutKeyId = Blackboard.GetKeyID(Entry.EntryName);
	const FBlackboardEntry* DerivedEntry = Blackboard.GetKey(OutKeyId);
	return OutKeyId != FBlackboard::InvalidKey && DerivedEntry != nullptr
		&& DerivedEntry->EntryName == Entry.EntryName && DerivedEntry->KeyType == Entry.KeyType;
}

bool CaptureAuthoredReorderEntry(FLocalKeysReorderPreflight& Out,
	const FBlackboardEntry& Entry, const FString& Name, const int32 Index,
	TMap<FString, FAssetDocumentRuntimeValueDto>& OutCanonicalEntries,
	TArray<FString>& OutCurrentNames)
{
	if (!IsExactAuthoredReorderEntry(*Out.Blackboard, Entry)) return false;
	FBlackboard::FKey KeyId = FBlackboard::InvalidKey;
	if (!HasExactReorderDerivedIdentity(*Out.Blackboard, Entry, KeyId)) return false;
	FAssetDocumentRuntimeValueDto Canonical;
	if (!ReadAuthoredEntry(*Out.Blackboard, Entry, Canonical)) return false;
	Out.BeforePhysicalIndices.Add(Name, Index);
	Out.BeforeKeyIds.Add(Name, KeyId);
	OutCanonicalEntries.Add(Name, MoveTemp(Canonical));
	OutCurrentNames.Add(Name);
	return true;
}

bool CaptureOnePhysicalReorderEntry(FLocalKeysReorderPreflight& Out,
	const FBlackboardEntry& Entry, const FString& Name, const int32 Index,
	TMap<FString, FAssetDocumentRuntimeValueDto>& OutCanonicalEntries,
	TArray<FString>& OutCurrentNames)
{
	if (IsEngineDerivedSelfKey(Entry))
		return HasExactReorderDerivedIdentity(*Out.Blackboard, Entry, Out.BeforeSelfKeyId);
	return CaptureAuthoredReorderEntry(
		Out, Entry, Name, Index, OutCanonicalEntries, OutCurrentNames);
}

TOptional<FAssetDocumentPrimitiveOutcome> CapturePhysicalReorderSnapshot(
	FLocalKeysReorderPreflight& Out,
	TMap<FString, FAssetDocumentRuntimeValueDto>& OutCanonicalEntries,
	TArray<FString>& OutCurrentNames)
{
	TSet<FName> PhysicalNames;
	for (int32 Index = 0; Index < Out.Blackboard->Keys.Num(); ++Index)
	{
		const FBlackboardEntry& Entry = Out.Blackboard->Keys[Index];
		FString Name;
		if (!RegisterReorderPhysicalName(Entry, PhysicalNames, Name))
			return Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The complete current local-key snapshot is not authoritative."));
		if (!CaptureOnePhysicalReorderEntry(
			Out, Entry, Name, Index, OutCanonicalEntries, OutCurrentNames))
			return Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("A local Blackboard identity or canonical readback is unavailable."));
	}
	return {};
}

bool IsExactReorderPermutation(const FLocalKeysReorderPreflight& Preflight,
	const FAssetDocumentAIBlackboardLocalKeysReorderRequest& Request,
	const TArray<FString>& CurrentNames)
{
	if (Request.OrderedLocalKeyNames.Num() != CurrentNames.Num()) return false;
	TArray<FString> RemainingNames = CurrentNames;
	for (const FString& Name : Request.OrderedLocalKeyNames)
	{
		const int32 Match = RemainingNames.IndexOfByPredicate([&Name](const FString& Current)
		{
			return Current.Equals(Name, ESearchCase::CaseSensitive);
		});
		if (Match == INDEX_NONE || !Preflight.BeforePhysicalIndices.Contains(Name)) return false;
		RemainingNames.RemoveAtSwap(Match, EAllowShrinking::No);
	}
	return RemainingNames.IsEmpty();
}

void PopulateReorderExpectedReadback(FLocalKeysReorderPreflight& Out,
	const FAssetDocumentAIBlackboardLocalKeysReorderRequest& Request,
	const TMap<FString, FAssetDocumentRuntimeValueDto>& CanonicalEntries)
{
	Out.ExpectedNames = Request.OrderedLocalKeyNames;
	Out.ExpectedEntries.Reserve(Out.ExpectedNames.Num());
	for (const FString& Name : Out.ExpectedNames) Out.ExpectedEntries.Add(CanonicalEntries[Name]);
}

TOptional<FAssetDocumentPrimitiveOutcome> BuildDesiredReorderSnapshot(
	FLocalKeysReorderPreflight& Out,
	const FAssetDocumentAIBlackboardLocalKeysReorderRequest& Request,
	const TMap<FString, FAssetDocumentRuntimeValueDto>& CanonicalEntries,
	const TArray<FString>& CurrentNames)
{
	if (!IsExactReorderPermutation(Out, Request, CurrentNames))
		return Failure(TEXT("AIProvider.Blackboard.OrderedKeysPermutationInvalid"),
			TEXT("The requested local-key order is not an exact permutation."));
	Out.bIdentity = CurrentNames == Request.OrderedLocalKeyNames;
	PopulateReorderExpectedReadback(Out, Request, CanonicalEntries);
	Out.Desired = Out.Blackboard->Keys;
	int32 RequestIndex = 0;
	for (int32 Index = 0; Index < Out.Desired.Num(); ++Index)
	{
		if (IsEngineDerivedSelfKey(Out.Desired[Index])) continue;
		Out.Desired[Index] = Out.Blackboard->Keys[
			Out.BeforePhysicalIndices[Out.ExpectedNames[RequestIndex++]]];
	}
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> CaptureReorderSnapshot(
	FLocalKeysReorderPreflight& Out,
	const FAssetDocumentAIBlackboardLocalKeysReorderRequest& Request)
{
	TMap<FString, FAssetDocumentRuntimeValueDto> CanonicalEntries;
	TArray<FString> CurrentNames;
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed =
		CapturePhysicalReorderSnapshot(Out, CanonicalEntries, CurrentNames))
		return MoveTemp(Failed.GetValue());
	return BuildDesiredReorderSnapshot(Out, Request, CanonicalEntries, CurrentNames);
}

TOptional<FAssetDocumentPrimitiveOutcome> PrepareReorderLifecycle(FLocalKeysReorderPreflight& Out)
{
	const TSet<FString> AllowList = {GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys).ToString()};
	if (!TryMakeRows(*Out.Blackboard, Out.Rows, &AllowList))
		return Failure(TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
			TEXT("The exact ordered Keys PropertyEditor lifecycle is unavailable."));
	Out.KeysHandle = FindUniqueOwnedPropertyHandle(
		*Out.Rows, *Out.Blackboard, GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
	if (!Out.KeysHandle.IsValid() || Out.KeysHandle->GetProperty() == nullptr)
		return Failure(TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
			TEXT("The exact ordered Keys property handle is unavailable."));
	return {};
}

FAssetDocumentRuntimeValueDto MakeReorderSuccessValue(
	const FLocalKeysReorderPreflight& Preflight)
{
	TArray<FAssetDocumentRuntimeValueDto> Names;
	for (const FString& Name : Preflight.ExpectedNames) Names.Add(StringValue(Name));
	return ObjectValue({TEXT("entries"), TEXT("ordered_local_key_names")},
		{ArrayValue(Preflight.ExpectedEntries), ArrayValue(MoveTemp(Names))});
}

bool HasNoReorderOutcomeResidue(const FAssetDocumentPrimitiveOutcome& Outcome)
{
	return Outcome.HandleResults.IsEmpty() && Outcome.ChangedSet.IsEmpty();
}

FAssetDocumentPrimitiveOutcome MakeIdentityReorderSuccess(
	const FLocalKeysReorderPreflight& Preflight)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MakeReorderSuccessValue(Preflight);
	if (!HasNoReorderOutcomeResidue(Outcome))
		return Failure(TEXT("AIProvider.Blackboard.EntryChangedSetMismatch"),
			TEXT("The identity local-key reorder emitted mutable residue."));
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeChangedReorderSuccess(
	const FLocalKeysReorderPreflight& Preflight,
	const FAssetDocumentAIBlackboardLocalKeysReorderRequest& Request)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MakeReorderSuccessValue(Preflight);
	FAssetDocumentChangedRecord& Change = Outcome.ChangedSet.AddDefaulted_GetRef();
	Change.AuthorityId = PrimitiveId;
	Change.StableObjectId = Request.BlackboardPath;
	Change.Path = FString::Printf(TEXT("asset:%s/Keys.@Order"), *Request.BlackboardPath);
	Change.ChangeKind = TEXT("Update");
#if WITH_DEV_AUTOMATION_TESTS
	const bool bInjectedMismatch = ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::LocalKeysReorderChangedSetMismatch);
#else
	constexpr bool bInjectedMismatch = false;
#endif
	if (bInjectedMismatch || !Outcome.HandleResults.IsEmpty() || Outcome.ChangedSet.Num() != 1)
		return Failure(TEXT("AIProvider.Blackboard.EntryChangedSetMismatch"),
			TEXT("The exact local-key reorder ChangedSet could not be proven."));
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeReorderSuccess(const FLocalKeysReorderPreflight& Preflight,
	const FAssetDocumentAIBlackboardLocalKeysReorderRequest& Request, const bool bChanged)
{
	return bChanged ? MakeChangedReorderSuccess(Preflight, Request)
		: MakeIdentityReorderSuccess(Preflight);
}

bool MatchesReorderEntryIdentity(const FBlackboardEntry& Actual,
	const FBlackboardEntry& Expected)
{
	return Actual.EntryName == Expected.EntryName && Actual.KeyType == Expected.KeyType;
}

bool HasExpectedReorderKeyIdChange(const FLocalKeysReorderPreflight& Preflight,
	const FString& Name, const int32 Index, const FBlackboard::FKey KeyId)
{
	const int32* BeforeIndex = Preflight.BeforePhysicalIndices.Find(Name);
	const FBlackboard::FKey* BeforeKeyId = Preflight.BeforeKeyIds.Find(Name);
	if (BeforeIndex == nullptr || BeforeKeyId == nullptr) return false;
	return Index == *BeforeIndex ? KeyId == *BeforeKeyId : KeyId != *BeforeKeyId;
}

bool HasExactPublishedReorderEntry(const FLocalKeysReorderPreflight& Preflight,
	const int32 Index)
{
	const UBlackboardData& Blackboard = *Preflight.Blackboard;
	const FBlackboardEntry& Actual = Blackboard.Keys[Index];
	const FBlackboardEntry& Expected = Preflight.Desired[Index];
	if (!MatchesReorderEntryIdentity(Actual, Expected)) return false;
	if (IsEngineDerivedSelfKey(Actual)) return Blackboard.GetKeyID(Actual.EntryName) == Preflight.BeforeSelfKeyId;
	FBlackboard::FKey KeyId = FBlackboard::InvalidKey;
	if (!HasExactReorderDerivedIdentity(Blackboard, Actual, KeyId)) return false;
	return HasExpectedReorderKeyIdChange(Preflight, Actual.EntryName.ToString(), Index, KeyId);
}

bool HasExactPublishedReorder(const FLocalKeysReorderPreflight& Preflight)
{
	const UBlackboardData& Blackboard = *Preflight.Blackboard;
	if (Blackboard.Keys.Num() != Preflight.Desired.Num()) return false;
	for (int32 Index = 0; Index < Blackboard.Keys.Num(); ++Index)
		if (!HasExactPublishedReorderEntry(Preflight, Index)) return false;
	return true;
}

bool ReadCurrentAuthoredReorderEntries(FLocalKeysReorderPreflight& Preflight,
	TArray<FString>& OutNames, TArray<FAssetDocumentRuntimeValueDto>& OutEntries)
{
	for (const FBlackboardEntry& Entry : Preflight.Blackboard->Keys)
	{
		if (IsEngineDerivedSelfKey(Entry)) continue;
		FAssetDocumentRuntimeValueDto Canonical;
		if (!ReadAuthoredEntry(*Preflight.Blackboard, Entry, Canonical)) return false;
		OutNames.Add(Entry.EntryName.ToString());
		OutEntries.Add(MoveTemp(Canonical));
	}
	return true;
}

bool HasExactFinalReorderReadback(const FLocalKeysReorderPreflight& Preflight,
	const TArray<FString>& Names, const TArray<FAssetDocumentRuntimeValueDto>& Entries)
{
	if (Names != Preflight.ExpectedNames || Entries.Num() != Preflight.ExpectedEntries.Num()) return false;
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
		if (!RuntimeValueEquals(Entries[Index], Preflight.ExpectedEntries[Index])) return false;
	return true;
}

bool ReadFinalReorderState(FLocalKeysReorderPreflight& Preflight)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::LocalKeysReorderBeforeReadback)) return false;
#endif
	TArray<FString> Names;
	TArray<FAssetDocumentRuntimeValueDto> Entries;
	return ReadCurrentAuthoredReorderEntries(Preflight, Names, Entries)
		&& HasExactFinalReorderReadback(Preflight, Names, Entries);
}

TOptional<FAssetDocumentPrimitiveOutcome> PrepareReorderPreflight(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardLocalKeysReorderRequest& Request,
	FLocalKeysReorderPreflight& Out)
{
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed =
		PreflightReorderIdentity(Identity, Request, Out.Blackboard)) return MoveTemp(Failed.GetValue());
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed = CaptureReorderSnapshot(Out, Request))
		return MoveTemp(Failed.GetValue());
	if (!Out.bIdentity)
		return PrepareReorderLifecycle(Out);
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> CaptureReorderTransaction(
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardLocalKeysReorderRequest& Request,
	FLocalKeysReorderPreflight& Preflight,
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding& Binding)
{
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			Lease, *Preflight.Blackboard, {ComponentId}, Binding);
	if (!Capture.bSucceeded)
	{
		if (Binding.GetLoadedConsumerReceiptCaptureFailure()
			== AssetDocumentAIBlackboardLoadedConsumerCompletion::
				ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable)
			return Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The loaded BehaviorTree consumer coverage receipt is unavailable."));
		return FailureFromPreimage(Capture);
	}
	const FAssetDocumentContractValidationResult Footprint = Binding.EnlistFootprint({MakeFootprint(
		FString::Printf(TEXT("asset:%s/Keys[*]"), *Request.BlackboardPath), ComponentId)});
	if (!Footprint.bValid) return FailureFromValidation(Footprint);
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> PublishReorder(
	FLocalKeysReorderPreflight& Preflight,
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding& Binding)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::LocalKeysReorderPublicationRejected))
		return Failure(TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
			TEXT("The test seam rejected the ordered Keys publisher."));
#endif
	if (!AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot(
		*Preflight.Blackboard, Preflight.Desired, Binding, Preflight.KeysHandle))
		return Failure(TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
			TEXT("The exact ordered Keys publisher rejected the reorder snapshot."));
	Preflight.Blackboard->UpdateIfHasSynchronizedKeys();
	Preflight.Blackboard->PropagateKeyChangesToDerivedBlackboardAssets();
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> CompleteReorderConsumers(
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
		!= AssetDocumentAIBlackboardLoadedConsumerCompletion::ELoadedConsumerCompletionResult::Succeeded)
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
			TEXT("The test-only failure seam rejected reorder after derived ID refresh."));
#endif
	return {};
}

TOptional<FAssetDocumentPrimitiveOutcome> PublishAndCompleteReorder(
	FLocalKeysReorderPreflight& Preflight,
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding& Binding)
{
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed = PublishReorder(Preflight, Binding))
		return MoveTemp(Failed.GetValue());
	return CompleteReorderConsumers(*Preflight.Blackboard, Binding);
}

FAssetDocumentPrimitiveOutcome CompleteReorderOutcome(
	const FAssetDocumentAIBlackboardLocalKeysReorderRequest& Request,
	FLocalKeysReorderPreflight& Preflight)
{
	if (!HasExactPublishedReorder(Preflight) || !ReadFinalReorderState(Preflight))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The published local-key order has no exact canonical readback."));
	return MakeReorderSuccess(Preflight, Request, true);
}

FAssetDocumentPrimitiveOutcome ApplyReorderWithIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardLocalKeysReorderRequest& Request,
	IAssetDocumentAtomicTransaction& Transaction)
{
	FLocalKeysReorderPreflight Preflight;
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed =
		PrepareReorderPreflight(Identity, Request, Preflight))
		return MoveTemp(Failed.GetValue());
	if (Preflight.bIdentity) return MakeReorderSuccess(Preflight, Request, false);
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding Binding(
		*Preflight.Blackboard, ComponentId, Transaction);
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed =
		CaptureReorderTransaction(Lease, Request, Preflight, Binding)) return MoveTemp(Failed.GetValue());
	if (TOptional<FAssetDocumentPrimitiveOutcome> Failed = PublishAndCompleteReorder(Preflight, Binding))
		return MoveTemp(Failed.GetValue());
	return CompleteReorderOutcome(Request, Preflight);
}
} // namespace

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardLocalKeysReorder(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBlackboardLocalKeysReorderLocal;
	FAssetDocumentAIBlackboardLocalKeysReorderRequest Request;
	if (!ParseAssetDocumentAIBlackboardLocalKeysReorderRequest(Invocation, Request))
		return Failure(TEXT("AIProvider.InvalidValueShape"),
			TEXT("The Blackboard local-key reorder envelope is not canonical."));
	if (!IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
		return Failure(TEXT("AIProvider.InvalidTransaction"),
			TEXT("Blackboard local-key reorder requires the exact active transaction."));
	if (!IsEditorPropertyContext()) return Failure(TEXT("AIProvider.InvalidContext"),
		TEXT("Blackboard local-key reorder requires the Editor game thread."));
	const FAssetDocumentVerifiedLeaseResult LeaseResult =
		AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
	if (LeaseResult.Lease == nullptr) return Failure(TEXT("AIProvider.InvalidHandle"),
		TEXT("A matching AI mutable Asset lease is required."), LeaseResult.Diagnostics);
	return ApplyWithReorderIdentity(*LeaseResult.Lease,
		[&](const FAssetDocumentEditorAssetIdentityView& Identity)
		{
			return ApplyReorderWithIdentity(Identity, *LeaseResult.Lease, Request, *ActiveTransaction);
		});
}
