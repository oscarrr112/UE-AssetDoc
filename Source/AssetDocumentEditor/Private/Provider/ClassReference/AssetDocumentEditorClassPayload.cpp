// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/ClassReference/AssetDocumentEditorClassPayload.h"

#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "UObject/Class.h"

namespace
{
uint64 GetClassTypeRevision(const UClass& Class)
{
	return static_cast<uint64>(Class.GetUniqueID());
}

class FAssetDocumentEditorClassPayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	explicit FAssetDocumentEditorClassPayload(UClass& InClass)
		: Class(&InClass)
		, CanonicalClassPath(InClass.GetPathName())
		, TypeRevision(GetClassTypeRevision(InClass))
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return UE::AssetDocument::EditorPrimitive::ClassPayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		const UClass* ResolvedClass = Class.Get();
		return ResolvedClass != nullptr
			&& ResolvedClass->GetPathName() == CanonicalClassPath
			&& GetClassTypeRevision(*ResolvedClass) == TypeRevision;
	}

	virtual bool TryGetClassIdentity(
		UE::AssetDocument::EditorPrimitive::FClassIdentityView& OutView) const override
	{
		if (!IsAlive())
		{
			return false;
		}
		OutView.Class = Class.Get();
		OutView.CanonicalClassPath = CanonicalClassPath;
		OutView.TypeRevision = TypeRevision;
		return true;
	}

private:
	TWeakObjectPtr<UClass> Class;
	FString CanonicalClassPath;
	uint64 TypeRevision = 0;
};
}

TUniquePtr<IAssetDocumentNativeHandlePayload>
MakeAssetDocumentEditorClassPayload(UClass& Class)
{
	return MakeUnique<FAssetDocumentEditorClassPayload>(Class);
}
