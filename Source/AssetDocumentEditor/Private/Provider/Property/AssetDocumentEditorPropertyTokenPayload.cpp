// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Property/AssetDocumentEditorPropertyTokenPayload.h"

#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

uint64 GetAssetDocumentEditorPropertyTypeRevision(
	const UClass& DeclaringClass)
{
	return DeclaringClass.GetUniqueID();
}

FAssetDocumentEditorPropertyTokenPayload::
FAssetDocumentEditorPropertyTokenPayload(
	UClass& InDeclaringClass,
	FProperty& InProperty,
	FString InCodecId,
	bool bInReadable,
	bool bInWritable,
	uint64 InTypeRevision,
	bool bInDeclared)
	: DeclaringClass(&InDeclaringClass)
	, Property(&InProperty)
	, CodecId(MoveTemp(InCodecId))
	, TypeRevision(InTypeRevision)
	, bReadable(bInReadable)
	, bWritable(bInWritable)
	, bDeclared(bInDeclared)
{
}

FName FAssetDocumentEditorPropertyTokenPayload::GetPayloadTypeId() const
{
	return UE::AssetDocument::EditorPrimitive::PropertyTokenPayloadTypeId;
}

bool FAssetDocumentEditorPropertyTokenPayload::IsAlive() const
{
	const UClass* Class = DeclaringClass.Get();
	const FProperty* ResolvedProperty = Property.Get();
	return Class != nullptr
		&& ResolvedProperty != nullptr
		&& ResolvedProperty->GetOwnerClass() == Class
		&& TypeRevision == GetAssetDocumentEditorPropertyTypeRevision(*Class);
}

bool FAssetDocumentEditorPropertyTokenPayload::TryGetView(
	FAssetDocumentEditorPropertyTokenView& OutView) const
{
	OutView = {};
	if (!IsAlive() || CodecId.IsEmpty() || !bReadable)
	{
		return false;
	}
	OutView.DeclaringClass = DeclaringClass.Get();
	OutView.Property = Property.Get();
	OutView.CodecId = CodecId;
	OutView.TypeRevision = TypeRevision;
	OutView.bReadable = bReadable;
	OutView.bWritable = bWritable;
	OutView.bDeclared = bDeclared;
	return true;
}
