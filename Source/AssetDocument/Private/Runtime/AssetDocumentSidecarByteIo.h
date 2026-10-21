// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentPersistenceCoordinator.h"
#include "Runtime/AssetDocumentPersistenceJournal.h"

struct FAssetDocumentByteArtifact
{
	FString StagedPath;
	FString TargetPath;
	FString ContentDigest;
	bool bTargetExisted = false;
};

ASSETDOCUMENT_API FString ComputeAssetDocumentContentDigest(TArrayView64<const uint8> Bytes);

class FAssetDocumentSidecarByteIo
{
public:
	FAssetDocumentContractValidationResult Stage(
		const FAssetDocumentPersistencePlan& Plan,
		FAssetDocumentByteArtifact& OutArtifact);
	void CleanupStaged(const FAssetDocumentByteArtifact& Artifact, FStringView StagingRoot);
	FAssetDocumentContractValidationResult CaptureInstalledBaseline(
		const FAssetDocumentByteArtifact& Artifact,
		const FString& BackupPath,
		FAssetDocumentJournalArtifact& OutJournalArtifact);
	FAssetDocumentContractValidationResult Install(const FAssetDocumentByteArtifact& Artifact);
	FAssetDocumentContractValidationResult Restore(const FAssetDocumentJournalArtifact& Artifact);
};
