// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentEditorCutoverTransactionPreparation.h"
#include "Provider/Cutover/AssetDocumentEditorMutationFootprintBuilders.h"

class UObject;
class UPackage;

struct FAssetDocumentEditorCreatedTarget
{
	UObject* Asset = nullptr;
	UPackage* Package = nullptr;
	FAssetDocumentEditorMutationIdentity Identity;
};

struct FAssetDocumentEditorCreatedTargetAbsence
{
	bool bObjectAbsent = false;
	bool bPackageAbsent = false;
	bool bRegistryAbsent = false;
	bool bFileAbsent = false;
	bool bDirtyResidue = false;
};

FAssetDocumentContractValidationResult
DeleteAssetDocumentEditorCreatedTarget(
	const FAssetDocumentEditorCreatedTarget& Target,
	FAssetDocumentEditorCreatedTargetAbsence& OutAbsence);

ASSETDOCUMENTEDITOR_API FAssetDocumentEditorCreatedTargetAbsence
CaptureAssetDocumentEditorCreatedTargetAbsence(
	const FAssetDocumentEditorMutationIdentity& Identity);

ASSETDOCUMENTEDITOR_API bool IsAssetDocumentEditorCreatedTargetFullyAbsent(
	const FAssetDocumentEditorCreatedTargetAbsence& Absence);

TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>
MakeAssetDocumentEditorCutoverRollbackSeed(
	const FAssetDocumentEditorMutationIdentity& Identity,
	FString ExactAssetClassPath);
