// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AssetDocumentAIBlackboardMutationInternal.h"

#include "UObject/SoftObjectPath.h"


namespace AIBlackboardParentMutationLocal
{
using namespace AssetDocumentAIBlackboardMutation;
bool ResolveParent(
	const FAssetDocumentAIBlackboardParentRequest& Request,
	UBlackboardData& Blackboard,
	UBlackboardData*& OutParent,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	OutParent = nullptr;
	if (Request.bClearParent) return true;
	UObject* Candidate = FSoftObjectPath(Request.ParentPath).ResolveObject();
	if (Candidate == nullptr || Candidate->GetPathName() != Request.ParentPath)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.ParentUnavailable"),
			TEXT("The requested parent Blackboard is unavailable."));
		return false;
	}
	OutParent = Cast<UBlackboardData>(Candidate);
	if (OutParent == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.ParentClassMismatch"),
			TEXT("The requested parent is not an exact BlackboardData asset."));
		return false;
	}
	if (IsParentCycle(Blackboard, *OutParent))
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.ParentCycle"),
			TEXT("The requested Blackboard parent would introduce a cycle."));
		return false;
	}
	return true;
}

bool EnlistParentWrite(
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentEditorAssetIdentityView& Identity,
	UBlackboardData& Blackboard,
	IAssetDocumentAtomicTransaction& Transaction,
	TSharedPtr<IPropertyRowGenerator>& OutRows,
	TSharedPtr<IPropertyHandle>& OutParentHandle,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const TCHAR* KeysComponentId = TEXT("assetdocument.ai.blackboard.parent_keys.v1");
	const FAssetDocumentEditorTypedPreimageResult KeysCapture =
		CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage(
			Lease, Blackboard, {KeysComponentId}, Transaction);
	if (!KeysCapture.bSucceeded)
	{
		OutFailure = FailureFromPreimage(KeysCapture);
		return false;
	}
	if (!TryMakeRows(Blackboard, OutRows))
	{
		OutFailure = Failure(TEXT("AIProvider.AuthorityUnavailable"), TEXT("The PropertyEditor module is unavailable."));
		return false;
	}
	OutParentHandle = FindUniqueOwnedPropertyHandle(
		*OutRows, Blackboard, GET_MEMBER_NAME_CHECKED(UBlackboardData, Parent));
	if (!OutParentHandle.IsValid())
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.ParentUnavailable"),
			TEXT("The exact Blackboard Parent property handle is unavailable."));
		return false;
	}
	const TCHAR* ComponentId = TEXT("assetdocument.ai.blackboard.parent.v1");
	const FAssetDocumentEditorTypedPreimageResult Capture =
		CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
			Identity, Blackboard, OutRows.ToSharedRef(), OutParentHandle.ToSharedRef(), {ComponentId}, Transaction);
	if (!Capture.bSucceeded)
	{
		OutFailure = FailureFromPreimage(Capture);
		return false;
	}
	const FString CanonicalPath(Identity.GetCanonicalObjectPath());
	const FString KeysStablePath = FString::Printf(TEXT("asset:%s/Keys[*]"), *CanonicalPath);
	const FString StablePath = FString::Printf(TEXT("asset:%s/Parent"), *CanonicalPath);
	const FAssetDocumentContractValidationResult Enlisted = Transaction.EnlistFootprint(
		{MakeFootprint(KeysStablePath, KeysComponentId), MakeFootprint(StablePath, ComponentId)});
	if (!Enlisted.bValid)
	{
		OutFailure = FailureFromValidation(Enlisted);
		return false;
	}
	return true;
}

bool ApplyAndReadParent(
	IPropertyHandle& ParentHandle,
	UBlackboardData& Blackboard,
	UBlackboardData* Parent,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	if (ParentHandle.SetValue(Parent, EPropertyValueSetFlags::NotTransactable) != FPropertyAccess::Success)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The authoritative Blackboard Parent write was rejected."));
		return false;
	}
	UObject* ParentReadback = nullptr;
	if (ParentHandle.GetValue(ParentReadback) != FPropertyAccess::Success
		|| ParentReadback != Parent || Blackboard.Parent != Parent)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The Blackboard Parent readback or native parent projection does not match."));
		return false;
	}
	return true;
}

bool CollectInheritedKeys(
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

FAssetDocumentPrimitiveOutcome BuildParentSuccess(
	const FAssetDocumentAIBlackboardParentRequest& Request,
	const UBlackboardData& Blackboard,
	const FString& StablePath)
{
	TArray<FAssetDocumentRuntimeValueDto> Inherited;
	if (!CollectInheritedKeys(Blackboard, Inherited))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The Blackboard inherited key projection is incomplete."));
	}
	TArray<FAssetDocumentRuntimeValueDto> Local;
	for (const FBlackboardEntry& Entry : Blackboard.Keys)
	{
		if (!IsEngineDerivedSelfKey(Entry)) Local.Add(StringValue(Entry.EntryName.ToString()));
	}
	if (!HasContiguousKeyIds(Blackboard))
	{
		return Failure(TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("The Blackboard key IDs are not contiguous."));
	}
	return MakeSuccess(
		ObjectValue({TEXT("inherited_key_names"), TEXT("local_key_names"), TEXT("parent_path_or_null")},
			{ArrayValue(MoveTemp(Inherited)), ArrayValue(MoveTemp(Local)),
				Request.bClearParent ? FAssetDocumentRuntimeValueDto{} : StringValue(Request.ParentPath)}),
		TEXT("ai.blackboard.parent.set.v1"), Request.BlackboardPath, StablePath);
}

bool MatchesParentIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FAssetDocumentAIBlackboardParentRequest& Request,
	const UBlackboardData& Blackboard)
{
	return &Identity.GetObject() == &Blackboard
		&& Identity.GetCanonicalObjectPath() == Request.BlackboardPath
		&& IsEditorPropertyContext(&Blackboard);
}

FAssetDocumentPrimitiveOutcome ExecuteParentSet(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBlackboardParentRequest& Request,
	UBlackboardData& Blackboard,
	UBlackboardData* Parent,
	IAssetDocumentAtomicTransaction& Transaction)
{
	if (!MatchesParentIdentity(Identity, Request, Blackboard))
	{
		return Failure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The Asset lease does not identify the requested Blackboard."));
	}
	FAssetDocumentPrimitiveOutcome FailureOutcome;
	TSharedPtr<IPropertyRowGenerator> Rows;
	TSharedPtr<IPropertyHandle> ParentHandle;
	if (!EnlistParentWrite(Lease, Identity, Blackboard, Transaction, Rows, ParentHandle, FailureOutcome)) return FailureOutcome;
	if (!ApplyAndReadParent(*ParentHandle, Blackboard, Parent, FailureOutcome)) return FailureOutcome;
#if WITH_DEV_AUTOMATION_TESTS
	if (ConsumeBlackboardTestFailurePoint(EAssetDocumentAIBlackboardTestFailurePoint::AfterParentNativeRefresh))
	{
		return Failure(TEXT("AIProvider.OperationRejected"),
			TEXT("The test-only failure seam rejected the Parent mutation after native refresh."));
	}
#endif
	const FString StablePath = FString::Printf(TEXT("asset:%s/Parent"), *Request.BlackboardPath);
	return BuildParentSuccess(Request, Blackboard, StablePath);
}

bool PrepareParentMutation(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FAssetDocumentAIBlackboardParentRequest& OutRequest,
	UBlackboardData*& OutBlackboard,
	UBlackboardData*& OutParent,
	FAssetDocumentVerifiedLeaseResult& OutLease,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	if (!ParseAssetDocumentAIBlackboardParentRequest(Invocation, OutRequest))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The Blackboard parent request is not canonical."));
		return false;
	}
	OutBlackboard = FindBlackboard(OutRequest.BlackboardPath);
	if (OutBlackboard == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.Blackboard.ParentUnavailable"), TEXT("The target Blackboard is unavailable."));
		return false;
	}
	if (!IsEditorPropertyContext())
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidContext"), TEXT("Blackboard parent mutation requires the Editor game thread."));
		return false;
	}
	if (!ResolveParent(OutRequest, *OutBlackboard, OutParent, OutFailure)) return false;
	OutLease = AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
	if (OutLease.Lease != nullptr) return true;
	OutFailure = Failure(TEXT("AIProvider.InvalidHandle"),
		TEXT("A matching AI mutable Asset lease is required."), OutLease.Diagnostics);
	return false;
}

FAssetDocumentPrimitiveOutcome RunParentMutation(
	const FAssetDocumentAIBlackboardParentRequest& Request,
	UBlackboardData& Blackboard,
	UBlackboardData* Parent,
	const FAssetDocumentVerifiedLeaseResult& LeaseResult,
	IAssetDocumentAtomicTransaction& Transaction)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	const FAssetDocumentContractValidationResult IdentityResult = WithAssetDocumentEditorAssetIdentity(
		*LeaseResult.Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
		{
			Outcome = ExecuteParentSet(Identity, *LeaseResult.Lease, Request, Blackboard, Parent, Transaction);
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

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBlackboardParentSet(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBlackboardParentMutationLocal;
	FAssetDocumentAIBlackboardParentRequest Request;
	UBlackboardData* Blackboard = nullptr;
	FAssetDocumentPrimitiveOutcome FailureOutcome;
	UBlackboardData* Parent = nullptr;
	FAssetDocumentVerifiedLeaseResult LeaseResult;
	if (!PrepareParentMutation(Invocation, Handles, Request, Blackboard, Parent, LeaseResult, FailureOutcome)) return FailureOutcome;
	return RunParentMutation(Request, *Blackboard, Parent, LeaseResult, *ActiveTransaction);
}
