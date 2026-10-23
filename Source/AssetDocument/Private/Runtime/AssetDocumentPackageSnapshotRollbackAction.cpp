// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentPackageSnapshotRollbackAction.h"

FAssetDocumentPackageSnapshotRollbackAction::FAssetDocumentPackageSnapshotRollbackAction(
	TArray<TUniquePtr<FAssetDocumentPackageSnapshot>> InSnapshots)
	: Snapshots(MoveTemp(InSnapshots))
{
}

FString FAssetDocumentPackageSnapshotRollbackAction::MakeStableId(FStringView PackageName)
{
	return FString(ComponentId) + TEXT("|") + FString(PackageName);
}

FAssetDocumentContractValidationResult FAssetDocumentPackageSnapshotRollbackAction::Rollback(
	FStringView StableFailureCode)
{
	FAssetDocumentContractValidationResult Result;
	if (bConsumed)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/PackageSnapshot");
		Diagnostic.Code = TEXT("PackageSnapshot.AlreadyRestored");
		Diagnostic.Message = TEXT("The package snapshot rollback action is single-use.");
		return Result;
	}
	bConsumed = true;
	Result.bValid = true;
	for (int32 Index = Snapshots.Num() - 1; Index >= 0; --Index)
	{
		FAssetDocumentContractValidationResult Restored = Snapshots[Index]->Restore();
		Result.bValid &= Restored.bValid;
		Result.Diagnostics.Append(MoveTemp(Restored.Diagnostics));
	}
	// Release the captured objects; nothing may restore them twice.
	Snapshots.Reset();
	return Result;
}
