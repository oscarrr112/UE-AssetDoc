// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentRollbackAction.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

// Core rollback action restoring N whole-package snapshots. Enlisted before any
// provider action, so the atomic runtime runs it after every typed inverse.
class FAssetDocumentPackageSnapshotRollbackAction final : public IAssetDocumentRollbackAction
{
public:
	static constexpr const TCHAR* ComponentId = TEXT("core.package_snapshot.v1");

	explicit FAssetDocumentPackageSnapshotRollbackAction(
		TArray<TUniquePtr<FAssetDocumentPackageSnapshot>> InSnapshots);

	static FString MakeStableId(FStringView PackageName);

	virtual FStringView GetRollbackComponentId() const override { return ComponentId; }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) override;

private:
	TArray<TUniquePtr<FAssetDocumentPackageSnapshot>> Snapshots;
	bool bConsumed = false;
};
