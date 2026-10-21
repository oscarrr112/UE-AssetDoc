// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"

class UObject;

class FAssetDocumentEditorSubobjectPayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	FAssetDocumentEditorSubobjectPayload(
		UObject& InSubobject,
		UObject& InOuter,
		FString InCategoryId);

	virtual FName GetPayloadTypeId() const override;
	virtual bool IsAlive() const override;
	virtual bool TryGetObjectIdentity(
		UE::AssetDocument::EditorPrimitive::FObjectIdentityView&
			OutView) const override;

private:
	TWeakObjectPtr<UObject> Subobject;
	TWeakObjectPtr<UObject> Outer;
	FString SubobjectPath;
	FString OuterPath;
	FString CategoryId;
	uint64 SubobjectRevision = 0;
	uint64 OuterRevision = 0;
};
