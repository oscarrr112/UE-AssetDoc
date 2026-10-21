// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentAIBlackboardMutation
{
namespace
{
TOptional<FAssetDocumentPrimitiveOutcome> PreflightEntryAdd(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardEntryAddRequest& Request,
	UBlackboardData*& OutBlackboard,
	UClass*& OutKeyTypeClass,
	int32& OutPhysicalInsertionIndex)
{
	if (Identity.GetCanonicalObjectPath() != Request.BlackboardPath)
	{
		return Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The mutable Asset lease does not identify the requested Blackboard."));
	}
	UObject& Target = Identity.GetObject();
	if (Target.GetClass() != UBlackboardData::StaticClass())
	{
		return Failure(TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
			TEXT("The requested target is not an exact BlackboardData asset."));
	}
	OutBlackboard = static_cast<UBlackboardData*>(&Target);
	if (!IsEditorPropertyContext(OutBlackboard))
	{
		return Failure(TEXT("AIProvider.InvalidContext"),
			TEXT("Blackboard entry add requires the Editor game thread."));
	}
	int32 AuthoredLocalCount = 0;
	for (const FBlackboardEntry& Entry : OutBlackboard->Keys)
	{
		AuthoredLocalCount += !IsEngineDerivedSelfKey(Entry);
	}
	const FName RequestedName(Request.KeyName);
	if (Request.InsertionIndex < 0 || Request.InsertionIndex > AuthoredLocalCount
		|| Request.KeyName.IsEmpty() || RequestedName.IsNone()
		|| RequestedName == FBlackboard::KeySelf)
	{
		return Failure(TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
			TEXT("The requested entry name or insertion index is invalid."));
	}
	TSet<FName> LocalNames;
	for (const FBlackboardEntry& Entry : OutBlackboard->Keys)
	{
		if (Entry.EntryName.IsNone() || Entry.KeyType == nullptr
			|| Entry.KeyType->GetOuter() != OutBlackboard || LocalNames.Contains(Entry.EntryName))
		{
			return Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The complete current local-key snapshot is not authoritative."));
		}
		LocalNames.Add(Entry.EntryName);
	}
	if (LocalNames.Contains(RequestedName))
	{
		return Failure(TEXT("AIProvider.Blackboard.KeyAlreadyExists"),
			TEXT("The requested local Blackboard key already exists."));
	}
	if (HasInheritedKey(*OutBlackboard, RequestedName))
	{
		return Failure(TEXT("AIProvider.Blackboard.KeyInherited"),
			TEXT("The requested Blackboard key is inherited."));
	}
	for (const FBlackboardEntry& Entry : OutBlackboard->Keys)
	{
		if (!IsEngineDerivedSelfKey(Entry)
			&& HasInheritedKey(*OutBlackboard, Entry.EntryName))
		{
			return Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The current local-key snapshot contains a parent-shadowed name."));
		}
	}
	OutPhysicalInsertionIndex = OutBlackboard->Keys.Num();
	int32 AuthoredIndex = 0;
	for (int32 PhysicalIndex = 0; PhysicalIndex < OutBlackboard->Keys.Num(); ++PhysicalIndex)
	{
		if (IsEngineDerivedSelfKey(OutBlackboard->Keys[PhysicalIndex])) continue;
		if (AuthoredIndex++ == Request.InsertionIndex)
		{
			OutPhysicalInsertionIndex = PhysicalIndex;
			break;
		}
	}
	UObject* ClassObject = FSoftObjectPath(Request.ExpectedClassPath).ResolveObject();
	OutKeyTypeClass = Cast<UClass>(ClassObject);
	if (OutKeyTypeClass == nullptr
		|| OutKeyTypeClass->GetPathName() != Request.ExpectedClassPath
		|| !OutKeyTypeClass->IsChildOf(UBlackboardKeyType::StaticClass())
		|| OutKeyTypeClass->HasAnyClassFlags(
			CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		return Failure(TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
			TEXT("The requested KeyType class is not an admitted exact class."));
	}
	return {};
}

FAssetDocumentRuntimeValueDto MakeEntryAddExpectedEntry(
	const FAssetDocumentAIBlackboardEntryAddRequest& Request)
{
	FAssetDocumentRuntimeValueDto InstanceSynced;
	InstanceSynced.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	InstanceSynced.BooleanValue = Request.bInstanceSynced;
	return ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(Request.Category), StringValue(Request.Description),
			ObjectValue({TEXT("Kind"), TEXT("Path")},
				{StringValue(TEXT("ClassRef")), StringValue(Request.ExpectedClassPath)}),
			Request.CanonicalProperties, StringValue(Request.KeyName),
			MoveTemp(InstanceSynced)});
}

bool ValidateEntryAddDerivedState(
	const UBlackboardData& Blackboard,
	TConstArrayView<FBlackboardEntry> Desired,
	const FName RequestedName,
	const UBlackboardKeyType& KeyType,
	const bool bRequestedInstanceSynced)
{
	if (Blackboard.Keys.Num() != Desired.Num()) return false;
	TSet<FName> Names;
	for (int32 Index = 0; Index < Desired.Num(); ++Index)
	{
		const FBlackboardEntry& Actual = Blackboard.Keys[Index];
		if (Actual.EntryName != Desired[Index].EntryName
			|| Actual.KeyType != Desired[Index].KeyType
			|| Names.Contains(Actual.EntryName)) return false;
		Names.Add(Actual.EntryName);
	}
	const FBlackboard::FKey KeyId = Blackboard.GetKeyID(RequestedName);
	const FBlackboardEntry* DerivedEntry = Blackboard.GetKey(KeyId);
	return KeyId != FBlackboard::InvalidKey && DerivedEntry != nullptr
		&& DerivedEntry->KeyType == &KeyType
		&& (!bRequestedInstanceSynced || Blackboard.HasSynchronizedKeys());
}

FAssetDocumentPrimitiveOutcome MakeEntryAddSuccess(
	const FAssetDocumentAIBlackboardEntryAddRequest& Request,
	FAssetDocumentRuntimeValueDto Entry,
	TArray<FString> OrderedNames)
{
	TArray<FAssetDocumentRuntimeValueDto> OrderedNameValues;
	OrderedNameValues.Reserve(OrderedNames.Num());
	for (FString& Name : OrderedNames)
	{
		OrderedNameValues.Add(StringValue(MoveTemp(Name)));
	}
	const TArray<FString> ExactPaths = {
		FString::Printf(TEXT("asset:%s/Keys[name=%s]"), *Request.BlackboardPath, *Request.KeyName),
		FString::Printf(TEXT("asset:%s/Keys.@Order"), *Request.BlackboardPath)};
	FAssetDocumentPrimitiveOutcome Outcome = MakeSuccess(
		ObjectValue({TEXT("entry"), TEXT("ordered_local_key_names")},
			{MoveTemp(Entry), ArrayValue(MoveTemp(OrderedNameValues))}),
		TEXT("ai.blackboard.entry.add.v2"), Request.BlackboardPath, ExactPaths[0]);
	FAssetDocumentChangedRecord OrderChange = Outcome.ChangedSet[0];
	OrderChange.Path = ExactPaths[1];
	Outcome.ChangedSet.Add(MoveTemp(OrderChange));
#if WITH_DEV_AUTOMATION_TESTS
	const bool bInjectedMismatch = ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::EntryAddChangedSetMismatch);
#else
	const bool bInjectedMismatch = false;
#endif
	bool bMismatch = bInjectedMismatch || Outcome.HandleResults.Num() != 0
		|| Outcome.ChangedSet.Num() != ExactPaths.Num();
	if (bMismatch)
	{
		return Failure(TEXT("AIProvider.Blackboard.EntryChangedSetMismatch"),
			TEXT("The exact entry-add ChangedSet could not be proven."));
	}
	return Outcome;
}
} // namespace

FAssetDocumentPrimitiveOutcome ApplyAssetDocumentAIBlackboardEntryAddWithIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardEntryAddRequest& Request,
	IAssetDocumentAtomicTransaction& Transaction)
{
	UBlackboardData* Blackboard = nullptr;
	UClass* KeyTypeClass = nullptr;
	int32 PhysicalInsertionIndex = INDEX_NONE;
	if (TOptional<FAssetDocumentPrimitiveOutcome> FailureOutcome =
		PreflightEntryAdd(
			Identity, Request, Blackboard, KeyTypeClass, PhysicalInsertionIndex))
	{
		return MoveTemp(FailureOutcome.GetValue());
	}
	const bool bStructKeyType = KeyTypeClass == UBlackboardKeyType_Struct::StaticClass();
	TOptional<AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::FPreparedWrite> StructWrite;
	if (bStructKeyType)
	{
		auto Prepared = AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::PrepareCanonicalWrite(
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ECaller::AddV2,
			*Blackboard, *KeyTypeClass, nullptr, Transaction, Request.CanonicalProperties);
		if (!Prepared.bSucceeded)
		{
			return Prepared.Failure == AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EFailure::InvalidDto
				? Failure(TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
					TEXT("The requested Struct canonical properties are invalid."))
				: Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
					TEXT("The requested Struct canonical properties are unavailable."));
		}
		StructWrite.Emplace(MoveTemp(Prepared.Prepared));
	}
	const TCHAR* ComponentId = TEXT("assetdocument.ai.blackboard.entry.add.v2");
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding
		TransactionBinding(*Blackboard, ComponentId, Transaction);
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			Lease, *Blackboard, {ComponentId}, TransactionBinding);
	if (!Capture.bSucceeded)
	{
		if (TransactionBinding.GetLoadedConsumerReceiptCaptureFailure()
			== AssetDocumentAIBlackboardLoadedConsumerCompletion::
				ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable)
		{
			return Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The loaded BehaviorTree consumer coverage receipt is unavailable."));
		}
		return FailureFromPreimage(Capture);
	}
	const FAssetDocumentContractValidationResult Footprint = TransactionBinding.EnlistFootprint({
		MakeFootprint(FString::Printf(TEXT("asset:%s/Keys[*]"), *Request.BlackboardPath), ComponentId)});
	if (!Footprint.bValid) return FailureFromValidation(Footprint);
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(EAssetDocumentAIBlackboardTestFailurePoint::EntryAddBeforeStaging))
		return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"), TEXT("The test seam rejected staging."));
#endif
	AssetDocumentAIBlackboardStructuralPublication::FStagedKeyTypeResult Staged =
		AssetDocumentAIBlackboardStructuralPublication::StageKeyType(
			*Blackboard, KeyTypeClass, TransactionBinding);
	UBlackboardKeyType* KeyType = Staged.Get();
	if (KeyType == nullptr || !Staged.HasStrongOwnership())
		return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"), TEXT("KeyType staging failed."));
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(EAssetDocumentAIBlackboardTestFailurePoint::EntryAddAfterStaging))
		return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"), TEXT("The test seam rejected staged state."));
#endif
	if (bStructKeyType)
	{
		UBlackboardKeyType_Struct* StructKeyType = Cast<UBlackboardKeyType_Struct>(KeyType);
		if (StructKeyType == nullptr || StructKeyType->GetClass() != UBlackboardKeyType_Struct::StaticClass()
			|| StructKeyType->GetOuter() != Blackboard
			|| !AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ApplyPreparedCanonicalWrite(
				StructWrite.GetValue(),
				AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ECaller::AddV2,
				*StructKeyType, Transaction).bSucceeded)
		{
			return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
				TEXT("The exact staged Struct canonical lifecycle was rejected."));
		}
	}
	else if (IsAssetDocumentAIBlackboardDirectValueKeyType(KeyTypeClass))
	{
		if (!WriteAssetDocumentAIBlackboardDirectValueKeyType(*KeyType, Request.CanonicalProperties))
			return Failure(TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
				TEXT("Vector/Rotator KeyTypeProperties must be {\"DefaultValue\": null} or {\"DefaultValue\": {\"X\",\"Y\",\"Z\"} / {\"Pitch\",\"Roll\",\"Yaw\"} numbers}."));
	}
	else
	{
		TSharedPtr<IPropertyRowGenerator> KeyTypeRows;
		TArray<TSharedRef<IPropertyHandle>> PropertyHandles;
		TArray<const FAssetDocumentRuntimeValueDto*> PropertyValues;
		UEnum* CanonicalEnumType = nullptr;
		if (!BuildCompletePropertyWrites(*KeyType, Request.CanonicalProperties,
			KeyTypeRows, PropertyHandles, PropertyValues, CanonicalEnumType))
			return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"), TEXT("Complete KeyType configuration is unavailable."));
		for (int32 Index = 0; Index < PropertyHandles.Num(); ++Index)
		{
			if (!WriteLeaf(*PropertyHandles[Index], *PropertyValues[Index], KeyType, CanonicalEnumType))
				return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"), TEXT("Complete KeyType configuration was rejected."));
		}
	}
	FAssetDocumentRuntimeValueDto CanonicalProperties;
	if (!BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(*KeyType, CanonicalProperties)
		|| !RuntimeValueEquals(CanonicalProperties, Request.CanonicalProperties))
		return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"), TEXT("Configured KeyType canonical validation failed."));
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(EAssetDocumentAIBlackboardTestFailurePoint::EntryAddAfterConfiguration))
		return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"), TEXT("The test seam rejected configured state."));
#endif
	TArray<FBlackboardEntry> Desired = Blackboard->Keys;
	FBlackboardEntry NewEntry;
	NewEntry.EntryName = FName(Request.KeyName);
	NewEntry.KeyType = KeyType;
	NewEntry.EntryDescription = Request.Description;
	NewEntry.EntryCategory = Request.Category.IsEmpty() ? NAME_None : FName(Request.Category);
	NewEntry.bInstanceSynced = Request.bInstanceSynced;
	Desired.Insert(MoveTemp(NewEntry), PhysicalInsertionIndex);
	TSharedPtr<IPropertyRowGenerator> BlackboardRows;
	const TSet<FString> KeysAllowList = {GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys).ToString()};
	if (!TryMakeRows(*Blackboard, BlackboardRows, &KeysAllowList))
		return Failure(TEXT("AIProvider.AuthorityUnavailable"), TEXT("The exact Keys PropertyEditor rows are unavailable."));
	TSharedPtr<IPropertyHandle> KeysHandle = FindUniqueOwnedPropertyHandle(
		*BlackboardRows, *Blackboard, GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(EAssetDocumentAIBlackboardTestFailurePoint::EntryAddBeforePublication))
		return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"), TEXT("The test seam rejected the prepared snapshot."));
	if (ConsumeBlackboardTestFailurePoint(EAssetDocumentAIBlackboardTestFailurePoint::EntryAddPublicationRejected))
		return Failure(TEXT("AIProvider.AuthorityUnavailable"), TEXT("The test seam rejected publication authority."));
#endif
	if (!AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot(
		*Blackboard, Desired, TransactionBinding, KeysHandle))
		return Failure(TEXT("AIProvider.AuthorityUnavailable"), TEXT("The exact LocalKeys publisher rejected the snapshot."));
	Blackboard->UpdateIfHasSynchronizedKeys();
	Blackboard->PropagateKeyChangesToDerivedBlackboardAssets();
#if WITH_DEV_AUTOMATION_TESTS
	if (AssetDocumentAIBlackboardLoadedConsumerCompletion::
		ConsumeLoadedConsumerCompletionTestFailurePoint(
			AssetDocumentAIBlackboardLoadedConsumerCompletion::
				ELoadedConsumerCompletionTestFailurePoint::BeforeLoadedConsumerForwardCompletion))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The test seam rejected loaded-consumer completion before execution."));
#endif
	if (TransactionBinding.CompletePublishedLoadedConsumers(*Blackboard)
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
	if (ConsumeBlackboardTestFailurePoint(EAssetDocumentAIBlackboardTestFailurePoint::EntryAddAfterPublication))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"), TEXT("The test seam rejected post-publication state."));
#endif
	if (!ValidateEntryAddDerivedState(
		*Blackboard, Desired, FName(Request.KeyName), *KeyType,
		Request.bInstanceSynced))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"), TEXT("The published local-key derived state is invalid."));
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(EAssetDocumentAIBlackboardTestFailurePoint::EntryAddDerivedRefresh)
		|| ConsumeBlackboardTestFailurePoint(EAssetDocumentAIBlackboardTestFailurePoint::EntryAddBeforeReadback))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"), TEXT("The test seam rejected derived/readback state."));
#endif
	TSharedPtr<IPropertyRowGenerator> ReadbackRows;
	if (!TryMakeRows(*Blackboard, ReadbackRows, &KeysAllowList))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"), TEXT("Final entry readback rows are unavailable."));
	FAssetDocumentRuntimeValueDto EntryReadback;
	TArray<FString> OrderedNames;
	if (!BuildAssetDocumentAIBlackboardEntryMetadataReadback(
		*ReadbackRows, *Blackboard, Request.KeyName, *KeyType,
		CanonicalProperties, EntryReadback, OrderedNames)
		|| !RuntimeValueEquals(EntryReadback, MakeEntryAddExpectedEntry(Request)))
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"), TEXT("Final canonical entry/order readback failed."));
	OrderedNames.Remove(FBlackboard::KeySelf.ToString());
	return MakeEntryAddSuccess(Request, MoveTemp(EntryReadback), MoveTemp(OrderedNames));
}
} // namespace AssetDocumentAIBlackboardMutation
