// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/FieldPath.h"

class FProperty;
class UClass;

struct FAssetDocumentEditorPropertyTokenView
{
	UClass* DeclaringClass = nullptr;
	FProperty* Property = nullptr;
	FStringView CodecId;
	uint64 TypeRevision = 0;
	bool bReadable = false;
	bool bWritable = false;
	// Admitted by Profile declaration (non-edit serialized field); access bypasses
	// PropertyAccessUtil's edit-visibility gate through the declared access helpers.
	bool bDeclared = false;
};

ASSETDOCUMENTEDITOR_API uint64 GetAssetDocumentEditorPropertyTypeRevision(const UClass& DeclaringClass);

class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyTokenPayload final
	: public IAssetDocumentNativeHandlePayload
{
public:
	FAssetDocumentEditorPropertyTokenPayload(
		UClass& InDeclaringClass,
		FProperty& InProperty,
		FString InCodecId,
		bool bInReadable,
		bool bInWritable,
		uint64 InTypeRevision,
		bool bInDeclared = false);

	virtual FName GetPayloadTypeId() const override;
	virtual bool IsAlive() const override;
	bool TryGetView(FAssetDocumentEditorPropertyTokenView& OutView) const;

private:
	TWeakObjectPtr<UClass> DeclaringClass;
	TFieldPath<FProperty> Property;
	FString CodecId;
	uint64 TypeRevision = 0;
	bool bReadable = false;
	bool bWritable = false;
	bool bDeclared = false;
};
