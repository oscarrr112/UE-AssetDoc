// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/AssetDocumentUMGMutationContract.h"

#include "WidgetBlueprint.h"
#include "UObject/Package.h"

namespace AssetDocumentUMGMutationContractPrivate
{
void AddUniqueFootprint(
	TArray<FAssetDocumentMutationFootprint>& Out,
	TSet<FString>& StableIds,
	EAssetDocumentMutationFootprintKind Kind,
	FString StableId,
	const TCHAR* RollbackComponentId)
{
	if (StableId.IsEmpty() || StableIds.Contains(StableId))
	{
		return;
	}
	StableIds.Add(StableId);
	Out.Add({Kind, MoveTemp(StableId), RollbackComponentId});
}

FAssetDocumentContractValidationResult MakeInvalid(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = false;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Mutation");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}
} // namespace AssetDocumentUMGMutationContractPrivate


TArray<FAssetDocumentMutationFootprint>
BuildWidgetBlueprintMutationFootprint(
	UWidgetBlueprint& Blueprint,
	TConstArrayView<UObject*> TouchedObjects)
{
	using namespace AssetDocumentUMGMutationContractPrivate;
	TArray<FAssetDocumentMutationFootprint> Footprint;
	TSet<FString> StableIds;
	AddUniqueFootprint(
		Footprint, StableIds,
		EAssetDocumentMutationFootprintKind::UObjectState,
		Blueprint.GetPathName(),
		TEXT("rollback.uobject"));
	for (UObject* Object : TouchedObjects)
	{
		if (!IsValid(Object))
		{
			continue;
		}
		AddUniqueFootprint(
			Footprint, StableIds,
			EAssetDocumentMutationFootprintKind::UObjectState,
			Object->GetPathName(),
			TEXT("rollback.uobject"));
	}
	if (UPackage* Package = Blueprint.GetOutermost())
	{
		AddUniqueFootprint(
			Footprint, StableIds,
			EAssetDocumentMutationFootprintKind::Package,
			Package->GetName(),
			TEXT("rollback.package"));
	}
	return Footprint;
}

FAssetDocumentContractValidationResult
EnlistWidgetBlueprintMutationFootprint(
	IAssetDocumentAtomicTransaction& Transaction,
	UWidgetBlueprint& Blueprint,
	TConstArrayView<UObject*> TouchedObjects)
{
	using namespace AssetDocumentUMGMutationContractPrivate;
	if (Transaction.GetState() != EAssetDocumentAtomicState::Mutating)
	{
		return MakeInvalid(
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The UMG footprint requires an active Mutating transaction."));
	}
	const TArray<FAssetDocumentMutationFootprint> Footprint =
		BuildWidgetBlueprintMutationFootprint(Blueprint, TouchedObjects);
	if (Footprint.Num() < 2)
	{
		return MakeInvalid(
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The UMG footprint must include the blueprint and package."));
	}
	return Transaction.EnlistFootprint(Footprint);
}

bool ValidateWidgetBlueprintMutationCompletion(
	bool bPostcondition,
	const TCHAR* FailureMessage,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	if (bPostcondition)
	{
		return true;
	}
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Mutation/Postcondition");
	Diagnostic.Code = TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED");
	Diagnostic.Message = FailureMessage;
	OutDiagnostics.Add(MoveTemp(Diagnostic));
	return false;
}

bool RollbackAssetDocumentUMGMutation(
	IAssetDocumentAtomicTransaction& Transaction,
	FStringView StableFailureCode,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	const FAssetDocumentContractValidationResult Result =
		Transaction.Rollback(StableFailureCode);
	if (Result.bValid)
	{
		return true;
	}
	OutDiagnostics.Append(
		Result.Diagnostics.GetData(), Result.Diagnostics.Num());
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Mutation/Rollback");
	Diagnostic.Code = TEXT("AD.UMG.PROVIDER.ROLLBACK_FAILED");
	Diagnostic.Message = TEXT("The provider-local rollback callback failed.");
	OutDiagnostics.Add(MoveTemp(Diagnostic));
	return false;
}

FAssetDocumentChangedRecord MakeAssetDocumentUMGChangedRecord(
	UObject& Object,
	const TCHAR* Path,
	const TCHAR* ChangeKind)
{
	FAssetDocumentChangedRecord Record;
	Record.AuthorityId = TEXT("umg");
	Record.StableObjectId = Object.GetPathName();
	Record.Path = Path;
	Record.ChangeKind = ChangeKind;
	return Record;
}
