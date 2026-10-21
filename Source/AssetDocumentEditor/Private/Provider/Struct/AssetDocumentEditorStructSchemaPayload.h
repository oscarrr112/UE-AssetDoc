// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

class UScriptStruct;

class FAssetDocumentEditorStructSchemaPayload final
	: public IAssetDocumentNativeHandlePayload
{
public:
	FAssetDocumentEditorStructSchemaPayload(
		UScriptStruct& InSchema,
		FString InCanonicalPath);

	virtual FName GetPayloadTypeId() const override;
	virtual bool IsAlive() const override;

	UScriptStruct* GetSchema() const;
	FStringView GetCanonicalPath() const;
	uint64 GetTypeRevision() const;

private:
	TWeakObjectPtr<UScriptStruct> Schema;
	FString CanonicalPath;
	uint64 TypeRevision = 0;
};

FAssetDocumentEditorStructSchemaPayload*
TryGetAssetDocumentEditorStructSchemaPayload(
	IAssetDocumentNativeHandlePayload& Payload);
