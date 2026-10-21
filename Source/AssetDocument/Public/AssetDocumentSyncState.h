// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

struct FAssetDocumentRegionSyncState
{
	int32 PolicyVersion = 1;
	FString SidecarHash;
	FString AssetEvidenceHash;
	FString LastSyncedAtUtc;
};

struct FAssetDocumentSyncState
{
	int32 SchemaVersion = 1;
	FString AssetObjectPath;
	FString AssetPackageGuid;
	FString UpdatedAtUtc;
	TMap<FString, FAssetDocumentRegionSyncState> Regions;
};
