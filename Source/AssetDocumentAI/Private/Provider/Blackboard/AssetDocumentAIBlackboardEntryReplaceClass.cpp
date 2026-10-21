// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "UObject/SoftObjectPath.h"


namespace AIBlackboardEntryReplaceClassLocal
{
using namespace AssetDocumentAIBlackboardMutation;
FAssetDocumentRuntimeValueDto LocalKeyNamesValue(TConstArrayView<FString> Names)
{
	TArray<FAssetDocumentRuntimeValueDto> Values;
	Values.Reserve(Names.Num());
	for (const FString& Name : Names) Values.Add(StringValue(Name));
	return ArrayValue(MoveTemp(Values));
}

FAssetDocumentRuntimeValueDto ClassReferenceValue(const UClass& Class)
{
	return ObjectValue({TEXT("Kind"), TEXT("Path")},
		{StringValue(TEXT("ClassRef")), StringValue(Class.GetPathName())});
}

FAssetDocumentRuntimeValueDto BooleanValue(const bool Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = Value;
	return Result;
}

FString EntryPath(const FString& BlackboardPath, const FString& KeyName)
{
	return FString::Printf(TEXT("asset:%s/Keys[name=%s]"), *BlackboardPath, *KeyName);
}

struct FChangedEntryField
{
	FString Key;
	bool Value = false;
};

struct FEntryReplaceClassPreflight
{
	UBlackboardData* Blackboard = nullptr;
	UBlackboardKeyType* OldKeyType = nullptr;
	UClass* NewKeyTypeClass = nullptr;
	int32 TargetIndex = INDEX_NONE;
	FAssetDocumentRuntimeValueDto OldCanonicalProperties;
	FString OldDescription;
	FString OldCategory;
	bool bOldInstanceSynced = false;
	bool bClassChanged = true;
	TOptional<AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::FPreparedWrite>
		StructWrite;
};

TOptional<FAssetDocumentPrimitiveOutcome> PreflightEntryReplaceClass(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardEntryReplaceClassRequest& Request,
	IAssetDocumentAtomicTransaction& Transaction,
	FEntryReplaceClassPreflight& Out)
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
	Out.Blackboard = static_cast<UBlackboardData*>(&Target);
	if (!IsEditorPropertyContext(Out.Blackboard))
	{
		return Failure(TEXT("AIProvider.InvalidContext"),
			TEXT("Blackboard entry class replacement requires the Editor game thread."));
	}
	const FName RequestedName(Request.KeyName);
	if (Request.KeyName.IsEmpty() || RequestedName.IsNone()
		|| RequestedName == FBlackboard::KeySelf)
	{
		return Failure(TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
			TEXT("The requested replacement entry name is invalid."));
	}
	TSet<FName> LocalNames;
	for (int32 Index = 0; Index < Out.Blackboard->Keys.Num(); ++Index)
	{
		const FBlackboardEntry& Entry = Out.Blackboard->Keys[Index];
		if (Entry.EntryName.IsNone() || Entry.KeyType == nullptr
			|| Entry.KeyType->GetOuter() != Out.Blackboard || LocalNames.Contains(Entry.EntryName))
		{
			return Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The complete current local-key snapshot is not authoritative."));
		}
		LocalNames.Add(Entry.EntryName);
		if (Entry.EntryName == RequestedName)
		{
			Out.TargetIndex = Index;
			Out.OldKeyType = Entry.KeyType;
			Out.OldDescription = Entry.EntryDescription;
			Out.OldCategory = Entry.EntryCategory.IsNone()
				? FString() : Entry.EntryCategory.ToString();
			Out.bOldInstanceSynced = Entry.bInstanceSynced != 0;
		}
	}
	if (Out.TargetIndex == INDEX_NONE || Out.OldKeyType == nullptr)
	{
		const bool bInherited = IsVisibleInheritedBlackboardKey(
			*Out.Blackboard, RequestedName);
		return Failure(bInherited
			? TEXT("AIProvider.Blackboard.KeyInherited")
			: TEXT("AIProvider.Blackboard.KeyMissing"),
			bInherited
				? TEXT("The requested Blackboard key is inherited, not local.")
				: TEXT("The requested existing local Blackboard key is unavailable."));
	}
	UObject* ClassObject = FSoftObjectPath(Request.ExpectedClassPath).ResolveObject();
	Out.NewKeyTypeClass = Cast<UClass>(ClassObject);
	if (Out.NewKeyTypeClass == nullptr
		|| Out.NewKeyTypeClass->GetPathName() != Request.ExpectedClassPath
		|| !Out.NewKeyTypeClass->IsChildOf(UBlackboardKeyType::StaticClass())
		|| Out.NewKeyTypeClass->HasAnyClassFlags(
			CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		return Failure(TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
			TEXT("The requested replacement KeyType class is not an admitted exact class."));
	}
	Out.bClassChanged = Out.NewKeyTypeClass != Out.OldKeyType->GetClass();
	if (!Out.bClassChanged && !IsAssetDocumentAIBlackboardDirectValueKeyType(Out.NewKeyTypeClass))
	{
		return Failure(TEXT("AIProvider.Blackboard.KeyClassReplacementUnavailable"),
			TEXT("Same-class requests belong to update_existing.v2, not replace_class.v2."));
	}
	if (!BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(
		*Out.OldKeyType, Out.OldCanonicalProperties))
	{
		return Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The old exact KeyType has no complete canonical pre-state."));
	}
	if (Out.OldKeyType->GetClass() != UBlackboardKeyType_Struct::StaticClass()
		&& Out.NewKeyTypeClass == UBlackboardKeyType_Struct::StaticClass())
	{
		auto Prepared =
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::PrepareCanonicalWrite(
				AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ECaller::ReplaceClassV2,
				*Out.Blackboard, *Out.NewKeyTypeClass, nullptr, Transaction,
				Request.CanonicalProperties);
		if (!Prepared.bSucceeded)
		{
			return Prepared.Failure == AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EFailure::InvalidDto
				? Failure(TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
					TEXT("The requested replacement Struct canonical properties are invalid."))
				: Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
					TEXT("The requested replacement Struct canonical properties are unavailable."));
		}
		Out.StructWrite.Emplace(MoveTemp(Prepared.Prepared));
	}
	return {};
}

FAssetDocumentRuntimeValueDto MakeEntryReplaceClassExpectedEntry(
	const FAssetDocumentAIBlackboardEntryReplaceClassRequest& Request,
	const UClass& KeyTypeClass)
{
	return ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(Request.Category), StringValue(Request.Description),
			ClassReferenceValue(KeyTypeClass), Request.CanonicalProperties,
			StringValue(Request.KeyName), BooleanValue(Request.bInstanceSynced)});
}

bool ValidateEntryReplaceClassDerivedState(
	const UBlackboardData& Blackboard,
	TConstArrayView<FBlackboardEntry> Desired,
	const FAssetDocumentAIBlackboardEntryReplaceClassRequest& Request,
	const UBlackboardKeyType& NewKeyType)
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
	const FBlackboard::FKey KeyId = Blackboard.GetKeyID(FName(Request.KeyName));
	const FBlackboardEntry* DerivedEntry = Blackboard.GetKey(KeyId);
	return KeyId != FBlackboard::InvalidKey && DerivedEntry != nullptr
		&& DerivedEntry->KeyType == &NewKeyType
		&& Blackboard.GetKeyType(KeyId).Get() == NewKeyType.GetClass()
		&& Blackboard.IsKeyInstanceSynced(KeyId) == Request.bInstanceSynced
		&& (!Request.bInstanceSynced || Blackboard.HasSynchronizedKeys());
}

FAssetDocumentPrimitiveOutcome MakeEntryReplaceClassSuccess(
	const FAssetDocumentAIBlackboardEntryReplaceClassRequest& Request,
	const FEntryReplaceClassPreflight& Preflight,
	FAssetDocumentRuntimeValueDto Entry,
	TArray<FString> OrderedNames)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = ObjectValue(
		{TEXT("entry"), TEXT("ordered_local_key_names")},
		{MoveTemp(Entry), LocalKeyNamesValue(OrderedNames)});
	const bool bPropertiesChanged = !RuntimeValueEquals(
		Preflight.OldCanonicalProperties, Request.CanonicalProperties);
	const FString StableEntryPath = EntryPath(Request.BlackboardPath, Request.KeyName);
	const TArray<FChangedEntryField> ChangedFields = {
		{StableEntryPath + TEXT(".KeyTypeClass"), Preflight.bClassChanged},
		{StableEntryPath + TEXT(".KeyTypeProperties"), bPropertiesChanged},
		{StableEntryPath + TEXT(".Description"), Preflight.OldDescription != Request.Description},
		{StableEntryPath + TEXT(".Category"), Preflight.OldCategory != Request.Category},
		{StableEntryPath + TEXT(".bInstanceSynced"),
			Preflight.bOldInstanceSynced != Request.bInstanceSynced}};
	int32 ExpectedChangeCount = 0;
	for (const FChangedEntryField& Field : ChangedFields)
	{
		if (!Field.Value) continue;
		++ExpectedChangeCount;
		FAssetDocumentChangedRecord& Change = Outcome.ChangedSet.AddDefaulted_GetRef();
		Change.AuthorityId = TEXT("ai.blackboard.entry.replace_class.v2");
		Change.StableObjectId = Request.BlackboardPath;
		Change.Path = Field.Key;
		Change.ChangeKind = TEXT("Update");
	}
#if WITH_DEV_AUTOMATION_TESTS
	const bool bInjectedMismatch = ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassChangedSetMismatch);
#else
	const bool bInjectedMismatch = false;
#endif
	bool bMismatch = bInjectedMismatch || !Outcome.HandleResults.IsEmpty()
		|| Outcome.ChangedSet.Num() != ExpectedChangeCount;
	if (bMismatch)
	{
		return Failure(TEXT("AIProvider.Blackboard.EntryChangedSetMismatch"),
			TEXT("The exact replace-class ChangedSet could not be proven."));
	}
	return Outcome;
}

} // namespace

namespace AssetDocumentAIBlackboardMutation
{
FAssetDocumentPrimitiveOutcome ApplyAssetDocumentAIBlackboardEntryReplaceClassWithIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardEntryReplaceClassRequest& Request,
	IAssetDocumentAtomicTransaction& Transaction)
{
	using namespace AIBlackboardEntryReplaceClassLocal;
	FEntryReplaceClassPreflight Preflight;
	if (TOptional<FAssetDocumentPrimitiveOutcome> FailureOutcome =
		PreflightEntryReplaceClass(Identity, Request, Transaction, Preflight))
	{
		return MoveTemp(FailureOutcome.GetValue());
	}
	UBlackboardData& Blackboard = *Preflight.Blackboard;
	const TCHAR* ComponentId = TEXT("assetdocument.ai.blackboard.entry.replace_class.v2");
	AssetDocumentAIBlackboardStructuralPublication::FStructuralPublicationTransactionBinding
		TransactionBinding(Blackboard, ComponentId, Transaction);
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			Lease, Blackboard, {ComponentId}, TransactionBinding);
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
	const FAssetDocumentContractValidationResult Footprint =
		TransactionBinding.EnlistFootprint({MakeFootprint(
			FString::Printf(TEXT("asset:%s/Keys[*]"), *Request.BlackboardPath), ComponentId)});
	if (!Footprint.bValid) return FailureFromValidation(Footprint);
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassBeforeStaging))
	{
		return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
			TEXT("The test seam rejected replacement staging."));
	}
#endif
	AssetDocumentAIBlackboardStructuralPublication::FStagedKeyTypeResult Staged =
		AssetDocumentAIBlackboardStructuralPublication::StageKeyType(
			Blackboard, Preflight.NewKeyTypeClass, TransactionBinding);
	UBlackboardKeyType* NewKeyType = Staged.Get();
	if (NewKeyType == nullptr || !Staged.HasStrongOwnership())
	{
		return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
			TEXT("Replacement KeyType staging failed."));
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassAfterStaging))
	{
		return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
			TEXT("The test seam rejected the staged replacement."));
	}
#endif
	if (Preflight.StructWrite.IsSet())
	{
		UBlackboardKeyType_Struct* StructKeyType = Cast<UBlackboardKeyType_Struct>(NewKeyType);
		if (StructKeyType == nullptr
			|| StructKeyType->GetClass() != UBlackboardKeyType_Struct::StaticClass()
			|| StructKeyType->GetOuter() != &Blackboard
			|| !Staged.HasStrongOwnership()
			|| !AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ApplyPreparedCanonicalWrite(
				Preflight.StructWrite.GetValue(),
				AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ECaller::ReplaceClassV2,
				*StructKeyType, Transaction).bSucceeded)
		{
			return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
				TEXT("The exact staged Struct canonical lifecycle was rejected."));
		}
	}
	else if (IsAssetDocumentAIBlackboardDirectValueKeyType(NewKeyType->GetClass()))
	{
		if (!WriteAssetDocumentAIBlackboardDirectValueKeyType(*NewKeyType, Request.CanonicalProperties))
		{
			return Failure(TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
				TEXT("Vector/Rotator KeyTypeProperties must be {\"DefaultValue\": null} or {\"DefaultValue\": {\"X\",\"Y\",\"Z\"} / {\"Pitch\",\"Roll\",\"Yaw\"} numbers}."));
		}
	}
	else
	{
		TSharedPtr<IPropertyRowGenerator> KeyTypeRows;
		TArray<TSharedRef<IPropertyHandle>> PropertyHandles;
		TArray<const FAssetDocumentRuntimeValueDto*> PropertyValues;
		UEnum* CanonicalEnumType = nullptr;
		if (!BuildCompletePropertyWrites(*NewKeyType, Request.CanonicalProperties,
			KeyTypeRows, PropertyHandles, PropertyValues, CanonicalEnumType))
		{
			return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
				TEXT("Complete replacement KeyType configuration is unavailable."));
		}
		for (int32 Index = 0; Index < PropertyHandles.Num(); ++Index)
		{
			if (!WriteLeaf(*PropertyHandles[Index], *PropertyValues[Index],
				NewKeyType, CanonicalEnumType))
			{
				return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
					TEXT("Complete replacement KeyType configuration was rejected."));
			}
		}
	}
	FAssetDocumentRuntimeValueDto CanonicalProperties;
	if (!BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(
		*NewKeyType, CanonicalProperties)
		|| !RuntimeValueEquals(CanonicalProperties, Request.CanonicalProperties))
	{
		return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
			TEXT("Configured replacement KeyType canonical validation failed."));
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassAfterConfiguration))
	{
		return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
			TEXT("The test seam rejected configured replacement state."));
	}
#endif
	TArray<FBlackboardEntry> Desired = Blackboard.Keys;
	const int32 TargetIndex = Preflight.TargetIndex;
	FBlackboardEntry Replacement;
	Replacement.EntryName = FName(Request.KeyName);
	Replacement.KeyType = NewKeyType;
	Replacement.EntryDescription = Request.Description;
	Replacement.EntryCategory = Request.Category.IsEmpty() ? NAME_None : FName(Request.Category);
	Replacement.bInstanceSynced = Request.bInstanceSynced;
	Desired[TargetIndex] = MoveTemp(Replacement);
	TSharedPtr<IPropertyRowGenerator> BlackboardRows;
	const TSet<FString> KeysAllowList = {
		GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys).ToString()};
	if (!TryMakeRows(Blackboard, BlackboardRows, &KeysAllowList))
	{
		return Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The exact Keys PropertyEditor rows are unavailable."));
	}
	TSharedPtr<IPropertyHandle> KeysHandle = FindUniqueOwnedPropertyHandle(
		*BlackboardRows, Blackboard, GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassBeforePublication))
	{
		return Failure(TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
			TEXT("The test seam rejected the prepared replacement snapshot."));
	}
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassPublicationRejected))
	{
		return Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The test seam rejected replacement publication authority."));
	}
#endif
	if (!AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot(
		Blackboard, Desired, TransactionBinding, KeysHandle))
	{
		return Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The exact LocalKeys publisher rejected the replacement snapshot."));
	}
	Blackboard.UpdateIfHasSynchronizedKeys();
	Blackboard.PropagateKeyChangesToDerivedBlackboardAssets();
#if WITH_DEV_AUTOMATION_TESTS
	if (AssetDocumentAIBlackboardLoadedConsumerCompletion::
		ConsumeLoadedConsumerCompletionTestFailurePoint(
			AssetDocumentAIBlackboardLoadedConsumerCompletion::
				ELoadedConsumerCompletionTestFailurePoint::BeforeLoadedConsumerForwardCompletion))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The test seam rejected loaded-consumer completion before execution."));
	}
#endif
	if (TransactionBinding.CompletePublishedLoadedConsumers(Blackboard)
		!= AssetDocumentAIBlackboardLoadedConsumerCompletion::
			ELoadedConsumerCompletionResult::Succeeded)
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("Loaded BehaviorTree consumer cache completion failed."));
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (AssetDocumentAIBlackboardLoadedConsumerCompletion::
		ConsumeLoadedConsumerCompletionTestFailurePoint(
			AssetDocumentAIBlackboardLoadedConsumerCompletion::
				ELoadedConsumerCompletionTestFailurePoint::AfterLoadedConsumerForwardCompletion))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The test seam rejected loaded-consumer completion after execution."));
	}
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassAfterPublication))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The test seam rejected post-publication replacement state."));
	}
#endif
	if (!ValidateEntryReplaceClassDerivedState(
		Blackboard, Desired, Request, *NewKeyType))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The replacement local-key derived state is invalid."));
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
			EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassDerivedRefresh)
		|| ConsumeBlackboardTestFailurePoint(
			EAssetDocumentAIBlackboardTestFailurePoint::EntryReplaceClassBeforeReadback))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The test seam rejected replacement derived/readback state."));
	}
#endif
	TSharedPtr<IPropertyRowGenerator> ReadbackRows;
	if (!TryMakeRows(Blackboard, ReadbackRows, &KeysAllowList))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("Final replacement entry readback rows are unavailable."));
	}
	FAssetDocumentRuntimeValueDto EntryReadback;
	TArray<FString> OrderedNames;
	if (!BuildAssetDocumentAIBlackboardEntryMetadataReadback(
		*ReadbackRows, Blackboard, Request.KeyName, *NewKeyType,
		CanonicalProperties, EntryReadback, OrderedNames)
		|| !RuntimeValueEquals(EntryReadback,
			MakeEntryReplaceClassExpectedEntry(Request, *Preflight.NewKeyTypeClass)))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("Final replacement canonical entry/order readback failed."));
	}
	OrderedNames.Remove(FBlackboard::KeySelf.ToString());
	return MakeEntryReplaceClassSuccess(
		Request, Preflight, MoveTemp(EntryReadback), MoveTemp(OrderedNames));
}
} // namespace
