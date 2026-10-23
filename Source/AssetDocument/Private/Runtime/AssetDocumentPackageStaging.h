// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPersistenceCoordinator.h"
#include "Runtime/AssetDocumentPersistenceJournal.h"

class UObject;
class UPackage;

struct FAssetDocumentPackageArtifact
{
	FString StagedPath;
	FString TargetPath;
	FString ContentDigest;
};

struct FAssetDocumentPackageArtifactSet
{
	TArray<FAssetDocumentPackageArtifact> Artifacts;
	FString AggregateContentDigest;
};

class IAssetDocumentPackageSaveBackend
{
public:
	virtual ~IAssetDocumentPackageSaveBackend() = default;
	virtual FAssetDocumentContractValidationResult SaveToStaging(
		const FString& StagedHeaderPath,
		const FString& TargetHeaderPath,
		FAssetDocumentPackageArtifactSet& OutArtifacts) = 0;
};

class FAssetDocumentPackageStager
{
public:
	explicit FAssetDocumentPackageStager(IAssetDocumentPackageSaveBackend& InBackend);
	FAssetDocumentContractValidationResult Stage(
		const FAssetDocumentPersistencePlan& Plan,
		FAssetDocumentPackageArtifactSet& OutArtifacts);
	void CleanupStaged(
		const FAssetDocumentPackageArtifactSet& Artifacts,
		FStringView StagingRoot);
	FAssetDocumentContractValidationResult CaptureInstalledBaseline(
		const FAssetDocumentPackageArtifactSet& Artifacts,
		const FString& BackupRoot,
		TArray<FAssetDocumentJournalArtifact>& OutJournalArtifacts);

private:
	IAssetDocumentPackageSaveBackend& Backend;
};

class FAssetDocumentPublicPackageSaveBackend final : public IAssetDocumentPackageSaveBackend
{
public:
	FAssetDocumentPublicPackageSaveBackend(UPackage& InPackage, UObject* InAsset);
	virtual FAssetDocumentContractValidationResult SaveToStaging(
		const FString& StagedHeaderPath,
		const FString& TargetHeaderPath,
		FAssetDocumentPackageArtifactSet& OutArtifacts) override;

private:
	UPackage& Package;
	UObject* Asset = nullptr;
};
