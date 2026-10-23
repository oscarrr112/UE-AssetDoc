// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Struct/AssetDocumentEditorStructSchemaPayload.h"

#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "UObject/Class.h"

namespace
{
uint64 GetStructTypeRevision(const UScriptStruct& Schema)
{
	return static_cast<uint64>(Schema.GetUniqueID());
}
}

FAssetDocumentEditorStructSchemaPayload::
	FAssetDocumentEditorStructSchemaPayload(
		UScriptStruct& InSchema,
		FString InCanonicalPath)
	: Schema(&InSchema)
	, CanonicalPath(MoveTemp(InCanonicalPath))
	, TypeRevision(GetStructTypeRevision(InSchema))
{
}

FName FAssetDocumentEditorStructSchemaPayload::GetPayloadTypeId() const
{
	return UE::AssetDocument::EditorPrimitive::StructSchemaPayloadTypeId;
}

bool FAssetDocumentEditorStructSchemaPayload::IsAlive() const
{
	const UScriptStruct* Resolved = Schema.Get();
	return Resolved != nullptr
		&& TypeRevision != 0
		&& GetStructTypeRevision(*Resolved) == TypeRevision
		&& Resolved->GetPathName() == CanonicalPath;
}

UScriptStruct* FAssetDocumentEditorStructSchemaPayload::GetSchema() const
{
	return IsAlive() ? Schema.Get() : nullptr;
}

FStringView
FAssetDocumentEditorStructSchemaPayload::GetCanonicalPath() const
{
	return CanonicalPath;
}

uint64 FAssetDocumentEditorStructSchemaPayload::GetTypeRevision() const
{
	return TypeRevision;
}

FAssetDocumentEditorStructSchemaPayload*
TryGetAssetDocumentEditorStructSchemaPayload(
	IAssetDocumentNativeHandlePayload& Payload)
{
	using namespace UE::AssetDocument::EditorPrimitive;
	if (Payload.GetPayloadTypeId() != StructSchemaPayloadTypeId)
	{
		return nullptr;
	}
	return static_cast<FAssetDocumentEditorStructSchemaPayload*>(&Payload);
}
