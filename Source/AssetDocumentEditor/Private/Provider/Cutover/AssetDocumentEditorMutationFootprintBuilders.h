// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

struct FAssetDocumentEditorMutationIdentity
{
	FString CanonicalObjectPath;
	FString PackageName;
	FString PackageFilename;
};

ASSETDOCUMENTEDITOR_API bool TryBuildAssetDocumentEditorMutationIdentity(
	FStringView CanonicalObjectPath,
	FAssetDocumentEditorMutationIdentity& OutIdentity);

TArray<FAssetDocumentMutationFootprint>
BuildAssetDocumentEditorPropertyMutationFootprint(
	const FAssetDocumentEditorMutationIdentity& Identity);

TArray<FAssetDocumentMutationFootprint>
BuildAssetDocumentEditorFactoryMutationFootprint(
	const FAssetDocumentEditorMutationIdentity& Identity);

TArray<FAssetDocumentMutationFootprint>
BuildAssetDocumentEditorEmbeddedMutationFootprint(
	FStringView CanonicalOuterPath);

