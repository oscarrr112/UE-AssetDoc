// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"

#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Struct.h"
#include "StructUtils/InstancedStruct.h"


#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite
{
namespace
{
EOuterPreflightAuditTerminal GOuterPreflightAuditTerminal = EOuterPreflightAuditTerminal::NotEntered;
}

void ResetOuterPreflightAudit()
{
	GOuterPreflightAuditTerminal = EOuterPreflightAuditTerminal::NotEntered;
}

EOuterPreflightAuditTerminal GetOuterPreflightAuditTerminal()
{
	return GOuterPreflightAuditTerminal;
}

void SetOuterPreflightAuditTerminal(const EOuterPreflightAuditTerminal Terminal)
{
	GOuterPreflightAuditTerminal = Terminal;
}

const TCHAR* GetOuterPreflightAuditTerminalName(const EOuterPreflightAuditTerminal Terminal)
{
	switch (Terminal)
	{
	case EOuterPreflightAuditTerminal::NotEntered: return TEXT("NotEntered");
	case EOuterPreflightAuditTerminal::CanonicalPrepareFailed: return TEXT("CanonicalPrepareFailed");
	case EOuterPreflightAuditTerminal::TypedPreimageHandleUnavailable: return TEXT("TypedPreimageHandleUnavailable");
	case EOuterPreflightAuditTerminal::CurrentReadUnavailable: return TEXT("CurrentReadUnavailable");
	case EOuterPreflightAuditTerminal::CurrentKeysMismatch: return TEXT("CurrentKeysMismatch");
	case EOuterPreflightAuditTerminal::MetadataReadUnavailable: return TEXT("MetadataReadUnavailable");
	case EOuterPreflightAuditTerminal::Prepared: return TEXT("Prepared");
	default: return TEXT("Unknown");
	}
}
} // namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite
#endif

namespace AIBlackboardKeyTypePropertyMutationLocal
{
using namespace AssetDocumentAIBlackboardMutation;
struct FKeyTypePropertyWrite
{
	TSharedPtr<IPropertyRowGenerator> Rows;
	TArray<TSharedRef<IPropertyHandle>> Handles;
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	UBlackboardKeyType* KeyType = nullptr;
	UEnum* CanonicalEnumType = nullptr;
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
			TEXT("The mutable Asset lease has no current Editor identity."), IdentityResult.Diagnostics)
		: Outcome;
}

bool MatchesBlackboardRequest(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FString& BlackboardPath,
	UBlackboardData*& OutBlackboard)
{
	OutBlackboard = Cast<UBlackboardData>(&Identity.GetObject());
	return OutBlackboard != nullptr
		&& Identity.GetCanonicalObjectPath() == BlackboardPath
		&& IsEditorPropertyContext(OutBlackboard);
}

bool BuildCanonicalKeyTypePropertyReadback(
	const FKeyTypePropertyWrite& Write,
	FAssetDocumentRuntimeValueDto& OutProperties)
{
	const bool bStockExactCodec = Write.KeyType != nullptr
		&& (Write.KeyType->GetClass() == UBlackboardKeyType_Object::StaticClass()
			|| Write.KeyType->GetClass() == UBlackboardKeyType_Class::StaticClass()
			|| Write.KeyType->GetClass() == UBlackboardKeyType_Enum::StaticClass());
	return bStockExactCodec
		? BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(*Write.KeyType, OutProperties)
		: BuildCanonicalProperties(Write.Handles, OutProperties);
}

FString EntryPath(
	const FString& BlackboardPath,
	const FString& KeyName)
{
	return FString::Printf(TEXT("asset:%s/Keys[name=%s]"),
		*BlackboardPath, *KeyName);
}

TSharedPtr<IPropertyHandle> FindExactStructDefaultValueHandle(
	IPropertyRowGenerator& Rows,
	UBlackboardKeyType_Struct& ExactTarget)
{
	if (ExactTarget.GetClass() != UBlackboardKeyType_Struct::StaticClass()) return nullptr;
	TArray<TSharedPtr<IPropertyHandle>> Matches;
	TFunction<void(const TArray<TSharedRef<IDetailTreeNode>>&)> Gather =
		[&](const TArray<TSharedRef<IDetailTreeNode>>& Nodes)
	{
		for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
		{
			const TSharedPtr<IPropertyHandle> Candidate = Node->CreatePropertyHandle();
			if (Candidate.IsValid())
			{
				const FStructProperty* Property = CastField<FStructProperty>(Candidate->GetProperty());
				TArray<UObject*> Outers;
				Candidate->GetOuterObjects(Outers);
				if (Property != nullptr
					&& Property->GetFName() == GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Struct, DefaultValue)
					&& Property->GetOwnerClass() == UBlackboardKeyType_Struct::StaticClass()
					&& Property->GetOwnerProperty() == Property
					&& Property->Struct == FInstancedStruct::StaticStruct()
					&& Candidate->GeneratePathToProperty() == TEXT("DefaultValue")
					&& Outers.Num() == 1 && Outers[0] == &ExactTarget)
				{
					Matches.Add(Candidate);
				}
				continue;
			}
			TArray<TSharedRef<IDetailTreeNode>> Children;
			Node->GetChildren(Children, true);
			Gather(Children);
		}
	};
	Gather(Rows.GetRootTreeNodes());
	return Matches.Num() == 1 ? Matches[0] : nullptr;
}

bool PrepareKeyTypePropertyWrite(
	UBlackboardData& Blackboard,
	const FAssetDocumentAIBlackboardKeyTypeRequest& Request,
	FKeyTypePropertyWrite& OutWrite,
	UBlackboardKeyType*& OutKeyType,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	OutKeyType = FindOwnedKeyType(Blackboard, Request.KeyName);
	if (OutKeyType == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.KeyMissing"),
			TEXT("The requested existing Blackboard key is unavailable."));
		return false;
	}
	if (OutKeyType->GetClass()->GetPathName() != Request.ExpectedClassPath)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.KeyTypeClassMismatch"),
			TEXT("The existing Blackboard key type does not match the requested exact class."));
		return false;
	}
	if (!BuildPropertyWrites(*OutKeyType, *Request.CanonicalProperties,
		OutWrite.Rows, OutWrite.Handles, OutWrite.Values))
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The PropertyEditor module or an admitted key-type property is unavailable."));
		return false;
	}
	return true;
}

bool EnlistKeyTypePropertyWrite(
	const IAssetDocumentVerifiedHandleLease& Lease,
	UBlackboardKeyType& KeyType,
	const FKeyTypePropertyWrite& Write,
	const FString& StablePath,
	IAssetDocumentAtomicTransaction& Transaction,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const TCHAR* ComponentId = TEXT("assetdocument.ai.blackboard.key_type_properties.v1");
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			Lease, KeyType, Write.Rows.ToSharedRef(), Write.Handles, {ComponentId}, Transaction);
	if (!Capture.bSucceeded)
	{
		OutFailure = FailureFromPreimage(Capture);
		return false;
	}
	const FAssetDocumentContractValidationResult Enlisted =
		Transaction.EnlistFootprint({MakeFootprint(StablePath, ComponentId)});
	if (!Enlisted.bValid)
	{
		OutFailure = FailureFromValidation(Enlisted);
		return false;
	}
	return true;
}

bool WriteAndReadKeyTypeProperties(
	const FKeyTypePropertyWrite& Write,
	FAssetDocumentRuntimeValueDto& OutActualProperties,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	for (int32 Index = 0; Index < Write.Handles.Num(); ++Index)
	{
		if (!WriteLeaf(
			Write.Handles[Index].Get(), *Write.Values[Index], Write.KeyType, Write.CanonicalEnumType))
		{
			OutFailure = Failure(Write.KeyType != nullptr
				? TEXT("AIProvider.Blackboard.EntryMetadataForwardWriteUnavailable")
				: TEXT("AIProvider.Blackboard.PropertyWriteRejected"),
				TEXT("An admitted Blackboard key-type property write was rejected."));
			return false;
		}
		#if WITH_DEV_AUTOMATION_TESTS
		if (Write.KeyType != nullptr && ConsumeBlackboardMetadataWriteTestFailurePoint(
			EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::AfterKeyTypeLeaf, Index))
		{
			OutFailure = Failure(TEXT("AIProvider.OperationRejected"),
				TEXT("The test-only failure seam rejected the metadata mutation after a KeyType leaf."));
			return false;
		}
		#endif
	}

#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(
		EAssetDocumentAIBlackboardTestFailurePoint::AfterKeyTypeAuthoredAssignment))
	{
		OutFailure = Failure(TEXT("AIProvider.OperationRejected"),
			TEXT("The test-only failure seam rejected the key-type mutation after authored assignment."));
		return false;
	}
#endif
	if (!BuildCanonicalKeyTypePropertyReadback(Write, OutActualProperties))
	{
		OutFailure = Failure(Write.KeyType != nullptr
			? TEXT("AIProvider.Blackboard.DerivedRefreshFailed")
			: TEXT("AIProvider.Blackboard.PropertyWriteRejected"),
			TEXT("The written key-type properties have no canonical public readback."));
		return false;
	}
	return true;
}

FAssetDocumentPrimitiveOutcome ExecuteKeyTypePropertyWrite(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardKeyTypeRequest& Request,
	IAssetDocumentAtomicTransaction& Transaction)
{
	FAssetDocumentPrimitiveOutcome FailureOutcome;
	UBlackboardData* Blackboard = nullptr;
	if (!MatchesBlackboardRequest(Identity, Request.BlackboardPath, Blackboard))
	{
		return Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The Asset identity does not match the requested Blackboard."));
	}
	FKeyTypePropertyWrite Write;
	UBlackboardKeyType* KeyType = nullptr;
	if (!PrepareKeyTypePropertyWrite(*Blackboard, Request, Write, KeyType, FailureOutcome)) return FailureOutcome;
	const FString StablePath = EntryPath(Request.BlackboardPath, Request.KeyName)
		+ TEXT(".KeyType.Properties");
	if (!EnlistKeyTypePropertyWrite(Lease, *KeyType, Write, StablePath, Transaction, FailureOutcome)) return FailureOutcome;
	FAssetDocumentRuntimeValueDto ActualProperties;
	if (!WriteAndReadKeyTypeProperties(
		Write, ActualProperties, FailureOutcome)) return FailureOutcome;
	return MakeSuccess(
		ObjectValue({TEXT("canonical_properties"), TEXT("class_path")},
			{MoveTemp(ActualProperties), StringValue(KeyType->GetClass()->GetPathName())}),
		TEXT("ai.blackboard.key_type.apply_properties.v1"), Request.BlackboardPath, StablePath);
}

bool PrepareKeyTypePropertyMutation(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FAssetDocumentAIBlackboardKeyTypeRequest& OutRequest,
	FAssetDocumentVerifiedLeaseResult& OutLease,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	if (!ParseAssetDocumentAIBlackboardKeyTypeRequest(Invocation, true, OutRequest))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"),
			TEXT("The Blackboard key-type property request is not canonical."));
		return false;
	}
	if (!IsEditorPropertyContext())
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidContext"),
			TEXT("Blackboard property mutation requires the Editor game thread."));
		return false;
	}
	OutLease = AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
	if (OutLease.Lease != nullptr) return true;
	OutFailure = Failure(TEXT("AIProvider.InvalidHandle"),
		TEXT("A matching AI mutable Asset lease is required."), OutLease.Diagnostics);
	return false;
}

FAssetDocumentPrimitiveOutcome RunKeyTypePropertyMutation(
	const FAssetDocumentAIBlackboardKeyTypeRequest& Request,
	const FAssetDocumentVerifiedLeaseResult& LeaseResult,
	IAssetDocumentAtomicTransaction& Transaction)
{
	return ApplyWithCurrentAssetIdentity(
		*LeaseResult.Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
		{
			return ExecuteKeyTypePropertyWrite(
				Identity, *LeaseResult.Lease, Request, Transaction);
		});
}

FAssetDocumentRuntimeValueDto LocalKeyNamesValue(TConstArrayView<FString> Names)
{
	TArray<FAssetDocumentRuntimeValueDto> Values;
	Values.Reserve(Names.Num());
	for (const FString& Name : Names) Values.Add(StringValue(Name));
	return ArrayValue(MoveTemp(Values));
}

using FChangedEntryField = TPair<FString, bool>;

struct FEntryMetadataForwardWrite
{
	FKeyTypePropertyWrite KeyType;
	TSharedPtr<IPropertyRowGenerator> MetadataRows;
	TSharedPtr<IPropertyHandle> EntryHandle;
	TSharedPtr<IPropertyHandle> Description;
	TSharedPtr<IPropertyHandle> Category;
	TSharedPtr<IPropertyHandle> InstanceSynced;
	UBlackboardKeyType* ExactKeyType = nullptr;
	TOptional<AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::FPreparedWrite> StructWrite;
	TArray<FString> OrderedLocalKeyNames;
	TArray<FChangedEntryField> ChangedFields;
};

bool ResolveEntryMetadataHandles(
	UBlackboardData& Blackboard,
	const FString& KeyName,
	FEntryMetadataForwardWrite& OutWrite,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const TSet<FString> KeysAllowList = {GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys).ToString()};
	if (!TryMakeRows(Blackboard, OutWrite.MetadataRows, &KeysAllowList))
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The Blackboard metadata PropertyEditor lifecycle is unavailable."));
		return false;
	}
	const TSharedPtr<IPropertyHandle> Keys = FindUniqueOwnedPropertyHandle(
		*OutWrite.MetadataRows, Blackboard, GET_MEMBER_NAME_CHECKED(UBlackboardData, Keys));
	if (!Keys.IsValid())
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The Blackboard Keys PropertyEditor locator is unavailable."));
		return false;
	}
	uint32 ChildCount = 0;
	if (Keys->GetNumChildren(ChildCount) != FPropertyAccess::Success)
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The Blackboard Keys PropertyEditor children are unavailable."));
		return false;
	}
	for (uint32 ChildIndex = 0; ChildIndex < ChildCount; ++ChildIndex)
	{
		const TSharedPtr<IPropertyHandle> Candidate = Keys->GetChildHandle(ChildIndex);
		const TSharedPtr<IPropertyHandle> Name = Candidate.IsValid()
			? Candidate->GetChildHandle(TEXT("EntryName")) : nullptr;
		const TSharedPtr<IPropertyHandle> KeyType = Candidate.IsValid()
			? Candidate->GetChildHandle(TEXT("KeyType")) : nullptr;
		if (!Name.IsValid() || !KeyType.IsValid())
		{
			OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("A Blackboard entry PropertyEditor child is unavailable."));
			return false;
		}
		FName CandidateName;
		UObject* CandidateKeyTypeObject = nullptr;
		if (Name->GetValue(CandidateName) != FPropertyAccess::Success
			|| KeyType->GetValue(CandidateKeyTypeObject) != FPropertyAccess::Success)
		{
			OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("A Blackboard entry PropertyEditor value is unavailable."));
			return false;
		}
		const UBlackboardKeyType* LocalKeyType = Cast<UBlackboardKeyType>(CandidateKeyTypeObject);
		if (LocalKeyType != nullptr && LocalKeyType->GetOuter() == &Blackboard)
		{
			OutWrite.OrderedLocalKeyNames.Add(CandidateName.ToString());
		}
		if (CandidateName.ToString() != KeyName) continue;
		if (LocalKeyType == nullptr || LocalKeyType->GetOuter() != &Blackboard)
		{
			OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The requested local Blackboard key has malformed PropertyEditor evidence."));
			return false;
		}
		if (OutWrite.EntryHandle.IsValid())
		{
			OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The requested local Blackboard key is ambiguous."));
			return false;
		}
		OutWrite.EntryHandle = Candidate;
		OutWrite.ExactKeyType = Cast<UBlackboardKeyType>(CandidateKeyTypeObject);
	}
	if (!OutWrite.EntryHandle.IsValid() || OutWrite.ExactKeyType == nullptr
		|| OutWrite.ExactKeyType->GetOuter() != &Blackboard)
	{
		const bool bInherited = IsVisibleInheritedBlackboardKey(Blackboard, FName(KeyName));
		OutFailure = Failure(bInherited
			? TEXT("AIProvider.Blackboard.KeyInherited") : TEXT("AIProvider.Blackboard.KeyMissing"),
			bInherited ? TEXT("The requested Blackboard key is inherited, not local.")
				: TEXT("The requested existing local Blackboard key is unavailable."));
		return false;
	}
	OutWrite.Description = OutWrite.EntryHandle->GetChildHandle(TEXT("EntryDescription"));
	OutWrite.Category = OutWrite.EntryHandle->GetChildHandle(TEXT("EntryCategory"));
	OutWrite.InstanceSynced = OutWrite.EntryHandle->GetChildHandle(TEXT("bInstanceSynced"));
	auto IsOwnedMetadataLeaf = [&](const TSharedPtr<IPropertyHandle>& Handle, const TCHAR* Name)
	{
		TArray<UObject*> Outers;
		if (Handle.IsValid()) Handle->GetOuterObjects(Outers);
		return Handle.IsValid() && Handle->GetProperty() != nullptr
			&& Handle->GetProperty()->GetFName() == FName(Name)
			&& Outers.Num() == 1 && Outers[0] == &Blackboard;
	};
	if (!OutWrite.Description.IsValid() || !OutWrite.Category.IsValid() || !OutWrite.InstanceSynced.IsValid()
		|| !OutWrite.Description->IsEditable() || !OutWrite.Category->IsEditable() || !OutWrite.InstanceSynced->IsEditable())
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The exact Blackboard metadata property handles are unavailable."));
		return false;
	}
	if (!IsOwnedMetadataLeaf(OutWrite.Description, TEXT("EntryDescription"))
		|| !IsOwnedMetadataLeaf(OutWrite.Category, TEXT("EntryCategory"))
		|| !IsOwnedMetadataLeaf(OutWrite.InstanceSynced, TEXT("bInstanceSynced")))
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The exact Blackboard metadata owned property handles are unavailable."));
		return false;
	}
	return true;
}

bool PrepareEntryMetadataForwardWrite(
	UBlackboardData& Blackboard,
	const FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest& Request,
	IAssetDocumentAtomicTransaction& Transaction,
	FEntryMetadataForwardWrite& OutWrite,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	if (Request.KeyName.IsEmpty() || Request.ExpectedClassPath.IsEmpty()
		|| !IsAssetDocumentAICanonicalRuntimeValue(Request.CanonicalProperties))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"),
			TEXT("The private Blackboard entry metadata request is not canonical."));
		return false;
	}
	if (!ResolveEntryMetadataHandles(Blackboard, Request.KeyName, OutWrite, OutFailure)) return false;
	if (OutWrite.ExactKeyType->GetClass()->GetPathName() != Request.ExpectedClassPath)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.KeyClassMismatch"),
			TEXT("The existing Blackboard key type does not match the requested exact class."));
		return false;
	}
	if (OutWrite.ExactKeyType->GetClass() == UBlackboardKeyType_Struct::StaticClass())
	{
		using namespace AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite;
		UBlackboardKeyType_Struct& ExactStructTarget =
			*static_cast<UBlackboardKeyType_Struct*>(OutWrite.ExactKeyType);
		FPrepareResult Prepared = PrepareCanonicalWrite(ECaller::UpdateExistingV2, Blackboard,
			*UBlackboardKeyType_Struct::StaticClass(), &ExactStructTarget,
			Transaction, Request.CanonicalProperties);
		if (!Prepared.bSucceeded)
		{
			#if WITH_DEV_AUTOMATION_TESTS
			SetOuterPreflightAuditTerminal(EOuterPreflightAuditTerminal::CanonicalPrepareFailed);
			#endif
			const bool bInvalidDto = Prepared.Failure == EFailure::InvalidDto;
			OutFailure = Failure(bInvalidDto ? TEXT("AIProvider.Blackboard.EntrySchemaInvalid")
				: TEXT("AIProvider.AuthorityUnavailable"), bInvalidDto
				? TEXT("The embedded Blackboard entry does not satisfy BlackboardEntryV2.")
				: TEXT("The exact Struct canonical write preflight is unavailable."));
			return false;
		}
		const TSet<FString> StructAllowList = {
			GET_MEMBER_NAME_CHECKED(UBlackboardKeyType_Struct, DefaultValue).ToString()};
		TSharedPtr<IPropertyHandle> DefaultValueHandle;
		if (!TryMakeRows(ExactStructTarget, OutWrite.KeyType.Rows, &StructAllowList)
			|| !(DefaultValueHandle = FindExactStructDefaultValueHandle(
				*OutWrite.KeyType.Rows, ExactStructTarget)).IsValid())
		{
			#if WITH_DEV_AUTOMATION_TESTS
			SetOuterPreflightAuditTerminal(EOuterPreflightAuditTerminal::TypedPreimageHandleUnavailable);
			#endif
			OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The exact Struct DefaultValue typed preimage handle is unavailable."));
			return false;
		}
		OutWrite.KeyType.Handles.Add(DefaultValueHandle.ToSharedRef());
		OutWrite.StructWrite.Emplace(MoveTemp(Prepared.Prepared));
	}
	else if (IsAssetDocumentAIBlackboardDirectValueKeyType(OutWrite.ExactKeyType->GetClass()))
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.EntryMetadataForwardWriteUnavailable"),
			TEXT("Vector/Rotator Blackboard entries are updated by replace_class.v2 (a fresh key type), not update_existing.v2."));
		return false;
	}
	else if (!BuildCompletePropertyWrites(*OutWrite.ExactKeyType, Request.CanonicalProperties,
		OutWrite.KeyType.Rows, OutWrite.KeyType.Handles, OutWrite.KeyType.Values,
		OutWrite.KeyType.CanonicalEnumType))
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The canonical Blackboard key-type leaves have no PropertyEditor write handles."));
		return false;
	}
	OutWrite.KeyType.KeyType = OutWrite.ExactKeyType;
	FAssetDocumentRuntimeValueDto CurrentProperties;
	const bool bCurrentPropertiesRead = OutWrite.StructWrite.IsSet()
			? BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(*OutWrite.ExactKeyType, CurrentProperties)
			: BuildCanonicalKeyTypePropertyReadback(OutWrite.KeyType, CurrentProperties);
	if (!bCurrentPropertiesRead)
	{
		#if WITH_DEV_AUTOMATION_TESTS
		if (OutWrite.StructWrite.IsSet())
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::SetOuterPreflightAuditTerminal(
				AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EOuterPreflightAuditTerminal::CurrentReadUnavailable);
		#endif
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The complete Blackboard key-type pre-state is unavailable."));
		return false;
	}
	if (CurrentProperties.ObjectKeys != Request.CanonicalProperties.ObjectKeys)
	{
		#if WITH_DEV_AUTOMATION_TESTS
		if (OutWrite.StructWrite.IsSet())
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::SetOuterPreflightAuditTerminal(
				AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EOuterPreflightAuditTerminal::CurrentKeysMismatch);
		#endif
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The complete Blackboard key-type pre-state is unavailable."));
		return false;
	}
	const bool bKeyTypePropertiesChanged =
		!RuntimeValueEquals(CurrentProperties, Request.CanonicalProperties);
	FString Description;
	FName Category;
	bool InstanceSynced = false;
	if (OutWrite.Description->GetValue(Description) != FPropertyAccess::Success
		|| OutWrite.Category->GetValue(Category) != FPropertyAccess::Success
		|| OutWrite.InstanceSynced->GetValue(InstanceSynced) != FPropertyAccess::Success)
	{
		#if WITH_DEV_AUTOMATION_TESTS
		if (OutWrite.StructWrite.IsSet())
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::SetOuterPreflightAuditTerminal(
				AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EOuterPreflightAuditTerminal::MetadataReadUnavailable);
		#endif
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("The complete Blackboard metadata pre-state is unavailable."));
		return false;
	}
	const FString StableEntryPath = EntryPath(Request.BlackboardPath, Request.KeyName);
	OutWrite.ChangedFields = {
		{StableEntryPath + TEXT(".KeyTypeProperties"), bKeyTypePropertiesChanged},
		{StableEntryPath + TEXT(".Description"), Description != Request.Description},
		{StableEntryPath + TEXT(".Category"), Category.ToString() != Request.Category},
		{StableEntryPath + TEXT(".bInstanceSynced"), InstanceSynced != Request.bInstanceSynced}};
	#if WITH_DEV_AUTOMATION_TESTS
	if (OutWrite.StructWrite.IsSet())
		AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::SetOuterPreflightAuditTerminal(
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::EOuterPreflightAuditTerminal::Prepared);
	#endif
	return true;
}

bool IsEntryMetadataNoOp(const FEntryMetadataForwardWrite& Write)
{
	return !Write.ChangedFields.ContainsByPredicate(
		[](const FChangedEntryField& Field) { return Field.Value; });
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

bool MatchesCompleteEntryReadback(
	const FAssetDocumentRuntimeValueDto& Entry,
	TConstArrayView<FString> OrderedLocalKeyNames,
	const FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest& Request,
	const FEntryMetadataForwardWrite& Write)
{
	if (Write.ExactKeyType == nullptr || OrderedLocalKeyNames != Write.OrderedLocalKeyNames) return false;
	const FAssetDocumentRuntimeValueDto Expected = ObjectValue(
		{TEXT("Category"), TEXT("Description"), TEXT("KeyTypeClass"),
			TEXT("KeyTypeProperties"), TEXT("Name"), TEXT("bInstanceSynced")},
		{StringValue(Request.Category), StringValue(Request.Description),
			ClassReferenceValue(*Write.ExactKeyType->GetClass()), Request.CanonicalProperties,
			StringValue(Request.KeyName), BooleanValue(Request.bInstanceSynced)});
	return RuntimeValueEquals(Entry, Expected);
}

FAssetDocumentPrimitiveOutcome MakeEntryMetadataForwardWriteSuccess(
	FAssetDocumentRuntimeValueDto Value,
	const FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest& Request,
	const FEntryMetadataForwardWrite& Write)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MoveTemp(Value);
	for (const FChangedEntryField& Field : Write.ChangedFields)
	{
		if (!Field.Value) continue;
		FAssetDocumentChangedRecord& Change = Outcome.ChangedSet.AddDefaulted_GetRef();
		Change.AuthorityId = TEXT("ai.blackboard.entry.update_existing.v2");
		Change.StableObjectId = Request.BlackboardPath;
		Change.Path = Field.Key;
		Change.ChangeKind = TEXT("Update");
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome ReadEntryMetadataForwardWriteOutcome(
	UBlackboardData& Blackboard,
	const FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest& Request,
	const FEntryMetadataForwardWrite& Write,
	const FAssetDocumentRuntimeValueDto& CanonicalProperties,
	const bool bForcedMismatch,
	const TCHAR* ReadbackUnavailableMessage,
	const TCHAR* MismatchMessage)
{
	FAssetDocumentRuntimeValueDto Entry;
	TArray<FString> Names;
	if (!BuildAssetDocumentAIBlackboardEntryMetadataReadback(
		*Write.MetadataRows, Blackboard, Request.KeyName, *Write.ExactKeyType,
		CanonicalProperties, Entry, Names))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			ReadbackUnavailableMessage);
	}
	if (bForcedMismatch || !MatchesCompleteEntryReadback(Entry, Names, Request, Write))
	{
		return Failure(TEXT("AIProvider.Blackboard.EntryChangedSetMismatch"),
			MismatchMessage);
	}
	Names.Remove(FBlackboard::KeySelf.ToString());
	return MakeEntryMetadataForwardWriteSuccess(
		ObjectValue({TEXT("entry"), TEXT("ordered_local_key_names")},
			{MoveTemp(Entry), LocalKeyNamesValue(Names)}), Request, Write);
}

bool WriteEntryMetadataLeaf(
	IPropertyHandle& Handle,
	const FAssetDocumentRuntimeValueDto& Value,
	const EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint FailurePoint,
	const TCHAR* RejectionMessage,
	const TCHAR* FailureMessage,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	if (!WriteLeaf(Handle, Value))
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.EntryMetadataForwardWriteUnavailable"),
			RejectionMessage);
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardMetadataWriteTestFailurePoint(FailurePoint))
	{
		OutFailure = Failure(TEXT("AIProvider.OperationRejected"), FailureMessage);
		return false;
	}
#endif
	return true;
}

FAssetDocumentPrimitiveOutcome ApplyEntryMetadataForwardWriteChanges(
	UBlackboardData& Blackboard,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest& Request,
	FEntryMetadataForwardWrite& Write,
	IAssetDocumentAtomicTransaction& Transaction)
{
	const TCHAR* MetadataComponent = TEXT("assetdocument.ai.blackboard.entry_metadata.v2");
	const TCHAR* KeyTypeComponent = TEXT("assetdocument.ai.blackboard.key_type_properties.v2");
	const FAssetDocumentEditorTypedPreimageResult MetadataCapture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			Lease, Blackboard, {MetadataComponent}, Transaction);
	if (!MetadataCapture.bSucceeded) return FailureFromPreimage(MetadataCapture);
	const FAssetDocumentEditorTypedPreimageResult KeyTypeCapture =
		CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage(
			Lease, *Write.ExactKeyType, Write.KeyType.Rows.ToSharedRef(), Write.KeyType.Handles,
			{KeyTypeComponent}, Transaction);
	if (!KeyTypeCapture.bSucceeded) return FailureFromPreimage(KeyTypeCapture);
	const FAssetDocumentContractValidationResult Enlisted = Transaction.EnlistFootprint({
		MakeFootprint(EntryPath(Request.BlackboardPath, Request.KeyName)
			+ TEXT(".LocalKeysMetadataAggregate"), MetadataComponent),
		MakeFootprint(Write.ChangedFields[0].Key, KeyTypeComponent)});
	if (!Enlisted.bValid) return FailureFromValidation(Enlisted);
	FAssetDocumentPrimitiveOutcome FailureOutcome;
	FAssetDocumentRuntimeValueDto ActualProperties;
	if (Write.StructWrite.IsSet())
	{
		const AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::FApplyResult Applied =
			AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ApplyPreparedCanonicalWrite(
				Write.StructWrite.GetValue(),
				AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ECaller::UpdateExistingV2,
				*static_cast<UBlackboardKeyType_Struct*>(Write.ExactKeyType), Transaction);
		if (!Applied.bSucceeded)
		{
			return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
				TEXT("The exact Struct canonical lifecycle/readback failed."));
		}
		if (!BuildAssetDocumentAIBlackboardCanonicalKeyTypeProperties(*Write.ExactKeyType, ActualProperties))
		{
			return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
				TEXT("The exact Struct canonical readback failed."));
		}
	}
	else if (!WriteAndReadKeyTypeProperties(Write.KeyType, ActualProperties, FailureOutcome)) return FailureOutcome;
	if (!RuntimeValueEquals(ActualProperties, Request.CanonicalProperties))
	{
		return Failure(TEXT("AIProvider.Blackboard.EntryChangedSetMismatch"),
			TEXT("The written Blackboard key-type properties do not match the desired canonical state."));
	}
	if (!WriteEntryMetadataLeaf(*Write.Description, StringValue(Request.Description),
		EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::AfterDescription,
		TEXT("An admitted Blackboard metadata write was rejected."),
		TEXT("The test-only failure seam rejected the metadata mutation after Description."), FailureOutcome)
		|| !WriteEntryMetadataLeaf(*Write.Category, StringValue(Request.Category),
			EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::AfterCategory,
			TEXT("An admitted Blackboard metadata write was rejected."),
			TEXT("The test-only failure seam rejected the metadata mutation after Category."), FailureOutcome)) return FailureOutcome;
	const FAssetDocumentRuntimeValueDto Sync = BooleanValue(Request.bInstanceSynced);
	if (!WriteEntryMetadataLeaf(*Write.InstanceSynced, Sync,
		EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::AfterInstanceSynced,
		TEXT("The admitted Blackboard instance-sync write was rejected."),
		TEXT("The test-only failure seam rejected the metadata mutation after bInstanceSynced."), FailureOutcome)) return FailureOutcome;
	bool bForcedPostReadbackMismatch = false;
#if WITH_DEV_AUTOMATION_TESTS
	bForcedPostReadbackMismatch = ConsumeBlackboardMetadataWriteTestFailurePoint(
		EAssetDocumentAIBlackboardMetadataWriteTestFailurePoint::AfterPostReadback);
#endif
	return ReadEntryMetadataForwardWriteOutcome(
		Blackboard, Request, Write, ActualProperties, bForcedPostReadbackMismatch,
		TEXT("The written Blackboard entry has no complete canonical readback."),
		TEXT("The written Blackboard entry does not match the complete desired state."));
}

FAssetDocumentPrimitiveOutcome ExecuteEntryMetadataForwardWrite(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest& Request,
	IAssetDocumentAtomicTransaction& Transaction)
{
	UBlackboardData* Blackboard = nullptr;
	if (!MatchesBlackboardRequest(Identity, Request.BlackboardPath, Blackboard))
	{
		return Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The Asset identity does not match the requested Blackboard."));
	}
	FEntryMetadataForwardWrite Write;
	FAssetDocumentPrimitiveOutcome FailureOutcome;
	if (!PrepareEntryMetadataForwardWrite(*Blackboard, Request, Transaction, Write, FailureOutcome)) return FailureOutcome;
	if (IsEntryMetadataNoOp(Write))
	{
		return ReadEntryMetadataForwardWriteOutcome(
			*Blackboard, Request, Write, Request.CanonicalProperties, false,
			TEXT("The complete Blackboard entry readback authority is unavailable."),
			TEXT("The no-op Blackboard entry readback does not match the complete desired state."));
	}
	return ApplyEntryMetadataForwardWriteChanges(
		*Blackboard, Lease, Request, Write, Transaction);
}

} // namespace

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardEntryReplaceClass(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBlackboardKeyTypePropertyMutationLocal;
	FAssetDocumentAIBlackboardEntryReplaceClassRequest Request;
	const EAssetDocumentAIBlackboardEntryReplaceClassParseResult ParseResult =
		ParseAssetDocumentAIBlackboardEntryReplaceClassRequest(Invocation, Request);
	if (ParseResult != EAssetDocumentAIBlackboardEntryReplaceClassParseResult::Succeeded)
	{
		const bool bInvalidEnvelope =
			ParseResult == EAssetDocumentAIBlackboardEntryReplaceClassParseResult::InvalidEnvelope;
		return Failure(bInvalidEnvelope ? TEXT("AIProvider.InvalidValueShape")
			: TEXT("AIProvider.Blackboard.EntrySchemaInvalid"), bInvalidEnvelope
				? TEXT("The Blackboard replace-class envelope is not canonical.")
				: TEXT("The embedded replacement entry does not satisfy BlackboardEntryV2."));
	}
	if (ActiveTransaction == nullptr)
	{
		return Failure(TEXT("AIProvider.InvalidTransaction"),
			TEXT("Blackboard entry class replacement requires an active transaction."));
	}
	if (!IsEditorPropertyContext())
	{
		return Failure(TEXT("AIProvider.InvalidContext"),
			TEXT("Blackboard entry class replacement requires the Editor game thread."));
	}
	const FAssetDocumentVerifiedLeaseResult LeaseResult =
		AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
	if (LeaseResult.Lease == nullptr)
	{
		return Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("A matching AI mutable Asset lease is required."), LeaseResult.Diagnostics);
	}
	return ApplyWithCurrentAssetIdentity(
		*LeaseResult.Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
		{
			return ApplyAssetDocumentAIBlackboardEntryReplaceClassWithIdentity(
				Identity, *LeaseResult.Lease, Request, *ActiveTransaction);
		});
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardKeyTypeApplyProperties(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBlackboardKeyTypePropertyMutationLocal;
	FAssetDocumentAIBlackboardKeyTypeRequest Request;
	FAssetDocumentVerifiedLeaseResult LeaseResult;
	FAssetDocumentPrimitiveOutcome FailureOutcome;
	if (!PrepareKeyTypePropertyMutation(Invocation, Handles, Request, LeaseResult, FailureOutcome)) return FailureOutcome;
	return RunKeyTypePropertyMutation(Request, LeaseResult, *ActiveTransaction);
}

FAssetDocumentPrimitiveOutcome
AssetDocumentAIBlackboardMutation::ExecuteAssetDocumentAIBlackboardEntryMetadataForwardWritePrerequisite(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentAIBlackboardEntryMetadataForwardWriteRequest& Request,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBlackboardKeyTypePropertyMutationLocal;
	if (ActiveTransaction == nullptr)
	{
		return Failure(TEXT("AIProvider.InvalidTransaction"),
			TEXT("The private Blackboard entry metadata lifecycle requires an active transaction."));
	}
	if (!IsEditorPropertyContext())
	{
		return Failure(TEXT("AIProvider.InvalidContext"),
			TEXT("Blackboard entry metadata mutation requires the Editor game thread."));
	}
	const FAssetDocumentVerifiedLeaseResult LeaseResult = AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
	if (LeaseResult.Lease == nullptr)
	{
		return Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("A matching AI mutable Asset lease is required."), LeaseResult.Diagnostics);
	}
	return ApplyWithCurrentAssetIdentity(
		*LeaseResult.Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
		{
			return ExecuteEntryMetadataForwardWrite(
				Identity, *LeaseResult.Lease, Request, *ActiveTransaction);
		});
}
