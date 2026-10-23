// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/ClassReference/AssetDocumentEditorAssetPayload.h"

#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "UObject/Object.h"

namespace
{
uint64 GetAssetPayloadTypeRevision(const UObject& Asset)
{
	return static_cast<uint64>(Asset.GetUniqueID());
}

class FAssetDocumentEditorAssetPayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	explicit FAssetDocumentEditorAssetPayload(UObject& InAsset)
		: Asset(&InAsset)
		, CanonicalObjectPath(InAsset.GetPathName())
		, TypeRevision(GetAssetPayloadTypeRevision(InAsset))
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		const UObject* ResolvedAsset = Asset.Get();
		return ResolvedAsset != nullptr
			&& ResolvedAsset->GetPathName() == CanonicalObjectPath
			&& GetAssetPayloadTypeRevision(*ResolvedAsset) == TypeRevision;
	}

	virtual bool TryGetObjectIdentity(
		UE::AssetDocument::EditorPrimitive::FObjectIdentityView& OutView) const override
	{
		if (!IsAlive())
		{
			return false;
		}
		OutView.Object = Asset.Get();
		OutView.CanonicalObjectPath = CanonicalObjectPath;
		OutView.TypeRevision = TypeRevision;
		return true;
	}

private:
	TWeakObjectPtr<UObject> Asset;
	FString CanonicalObjectPath;
	uint64 TypeRevision = 0;
};
}

TUniquePtr<IAssetDocumentNativeHandlePayload>
MakeAssetDocumentEditorAssetPayload(UObject& Asset)
{
	return MakeUnique<FAssetDocumentEditorAssetPayload>(Asset);
}
