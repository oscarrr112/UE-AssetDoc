// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

class UClass;
class UObject;
class UPackage;

namespace UE::AssetDocument::EditorPrimitive
{
inline const FName ClassPayloadTypeId(TEXT("assetdocument.editor.class.v1"));
inline const FName AssetPayloadTypeId(TEXT("assetdocument.editor.asset.v1"));
inline const FName PropertyTokenPayloadTypeId(TEXT("assetdocument.editor.property_token.v1"));
inline const FName StructSchemaPayloadTypeId(TEXT("assetdocument.editor.struct_schema.v1"));
inline const FName SubobjectPayloadTypeId(TEXT("assetdocument.editor.subobject.v1"));
inline const FName ScratchPayloadTypeId(TEXT("assetdocument.editor.scratch.v1"));
inline const FName CreatedAssetPayloadTypeId(TEXT("assetdocument.editor.created_asset.v1"));
inline const FName CreatedPackagePayloadTypeId(TEXT("assetdocument.editor.created_package.v1"));

struct FClassIdentityView
{
	UClass* Class = nullptr;
	FStringView CanonicalClassPath;
	uint64 TypeRevision = 0;
};

struct FObjectIdentityView
{
	UObject* Object = nullptr;
	FStringView CanonicalObjectPath;
	uint64 TypeRevision = 0;
};

struct FPackageIdentityView
{
	UPackage* Package = nullptr;
	FStringView PackageName;
	FStringView Filename;
	uint64 TypeRevision = 0;
};

class INativeIdentityPayload : public IAssetDocumentNativeHandlePayload
{
public:
	virtual bool TryGetClassIdentity(FClassIdentityView& OutView) const
	{
		(void)OutView;
		return false;
	}

	virtual bool TryGetObjectIdentity(FObjectIdentityView& OutView) const
	{
		(void)OutView;
		return false;
	}

	virtual bool TryGetPackageIdentity(FPackageIdentityView& OutView) const
	{
		(void)OutView;
		return false;
	}
};

inline INativeIdentityPayload* TryGetNativeIdentityPayload(
	IAssetDocumentNativeHandlePayload& Payload,
	FName ExpectedTypeId)
{
	if (Payload.GetPayloadTypeId() != ExpectedTypeId)
	{
		return nullptr;
	}
	return static_cast<INativeIdentityPayload*>(&Payload);
}
} // namespace UE::AssetDocument::EditorPrimitive
