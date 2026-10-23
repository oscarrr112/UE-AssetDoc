// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

class UObject;
class UPackage;
struct FAssetDocumentEditorCreationRecord;

ASSETDOCUMENTEDITOR_API TUniquePtr<IAssetDocumentNativeHandlePayload>
MakeAssetDocumentEditorResolvedAssetPayload(UObject& Asset);

TUniquePtr<IAssetDocumentNativeHandlePayload>
MakeAssetDocumentEditorResolvedPackagePayload(UObject& Asset);

// Detaches the editor-wide delegate bindings that native struct state inside a transient scratch (and its inner
// objects) still holds, so dropping the scratch cannot leave a binding to freed memory (see the .cpp). Call it only
// when the scratch is no longer used: the detached values are reset.
void ReleaseAssetDocumentEditorScratchNativeBindings(UObject& Scratch);

const FAssetDocumentEditorCreationRecord*
TryGetAssetDocumentEditorCreatedAssetRecord(
	IAssetDocumentNativeHandlePayload& Payload);

const FAssetDocumentEditorCreationRecord*
TryGetAssetDocumentEditorCreatedPackageRecord(
	IAssetDocumentNativeHandlePayload& Payload);

struct FAssetDocumentEditorCreationRecord
{
	FAssetDocumentEditorCreationRecord(
		FGuid InRequestId,
		FGuid InTransactionId,
		uint64 InRuntimeRevision,
		FString InFactoryId,
		FString InCanonicalObjectPath,
		FString InPackageName,
		FString InPackageFilename,
		bool bInObjectExistedBefore,
		bool bInPackageExistedBefore,
		bool bInFileExistedBefore,
		bool bInPackageWasDirty,
		UObject& InAsset,
		UPackage& InPackage);

	const FGuid RequestId;
	const FGuid TransactionId;
	const uint64 RuntimeRevision;
	const FString FactoryId;
	const FString CanonicalObjectPath;
	const FString PackageName;
	const FString PackageFilename;
	const bool bObjectExistedBefore;
	const bool bPackageExistedBefore;
	const bool bFileExistedBefore;
	const bool bPackageWasDirty;
	const TWeakObjectPtr<UObject> Asset;
	const TWeakObjectPtr<UPackage> Package;
};

class FAssetDocumentEditorCreatedAssetPayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	FAssetDocumentEditorCreatedAssetPayload(
		UObject& InAsset,
		TSharedRef<const FAssetDocumentEditorCreationRecord> InRecord);

	virtual FName GetPayloadTypeId() const override;
	virtual bool IsAlive() const override;
	virtual bool TryGetObjectIdentity(
		UE::AssetDocument::EditorPrimitive::FObjectIdentityView& OutView)
		const override;
	const FAssetDocumentEditorCreationRecord& GetCreationRecord() const;

private:
	TWeakObjectPtr<UObject> Asset;
	TSharedRef<const FAssetDocumentEditorCreationRecord> Record;
};

class FAssetDocumentEditorCreatedPackagePayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	FAssetDocumentEditorCreatedPackagePayload(
		UPackage& InPackage,
		TSharedRef<const FAssetDocumentEditorCreationRecord> InRecord);

	virtual FName GetPayloadTypeId() const override;
	virtual bool IsAlive() const override;
	virtual bool TryGetPackageIdentity(
		UE::AssetDocument::EditorPrimitive::FPackageIdentityView& OutView)
		const override;
	const FAssetDocumentEditorCreationRecord& GetCreationRecord() const;

private:
	TWeakObjectPtr<UPackage> Package;
	TSharedRef<const FAssetDocumentEditorCreationRecord> Record;
};

class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorScratchPayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	explicit FAssetDocumentEditorScratchPayload(UObject& InScratchAsset);
	virtual ~FAssetDocumentEditorScratchPayload() override;

	virtual FName GetPayloadTypeId() const override;
	virtual bool IsAlive() const override;
	virtual bool TryGetObjectIdentity(
		UE::AssetDocument::EditorPrimitive::FObjectIdentityView& OutView)
		const override;

private:
	TStrongObjectPtr<UObject> ScratchAsset;
	FString CanonicalObjectPath;
};
