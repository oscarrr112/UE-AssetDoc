// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Embedded/AssetDocumentEditorSubobjectPayload.h"

#include "Engine/AssetUserData.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "UObject/Object.h"

namespace
{
uint64 GetEmbeddedObjectRevision(const UObject& Object)
{
	return static_cast<uint64>(Object.GetUniqueID());
}

bool HasExactEmbeddedOwnership(
	const UObject& Subobject,
	const UObject& Outer,
	FStringView SubobjectPath,
	FStringView OuterPath)
{
	return Subobject.GetOuter() == &Outer
		&& Subobject.GetPathName() == SubobjectPath
		&& Outer.GetPathName() == OuterPath;
}

bool HasExactEmbeddedRevisions(
	const UObject& Subobject,
	const UObject& Outer,
	uint64 SubobjectRevision,
	uint64 OuterRevision)
{
	return GetEmbeddedObjectRevision(Subobject) == SubobjectRevision
		&& GetEmbeddedObjectRevision(Outer) == OuterRevision;
}

bool AreEmbeddedObjectsResolved(
	const UObject* Subobject,
	const UObject* Outer)
{
	return Subobject != nullptr && Outer != nullptr;
}

bool HasExactAssetUserDataAttachment(
	UObject& Subobject,
	UObject& Outer)
{
	IInterface_AssetUserData* Owner =
		Cast<IInterface_AssetUserData>(&Outer);
	return Owner != nullptr
		&& Owner->GetAssetUserDataOfClass(
			Subobject.GetClass()) == &Subobject;
}
}

FAssetDocumentEditorSubobjectPayload::
	FAssetDocumentEditorSubobjectPayload(
		UObject& InSubobject,
		UObject& InOuter,
		FString InCategoryId)
	: Subobject(&InSubobject)
	, Outer(&InOuter)
	, SubobjectPath(InSubobject.GetPathName())
	, OuterPath(InOuter.GetPathName())
	, CategoryId(MoveTemp(InCategoryId))
	, SubobjectRevision(GetEmbeddedObjectRevision(InSubobject))
	, OuterRevision(GetEmbeddedObjectRevision(InOuter))
{
}

FName FAssetDocumentEditorSubobjectPayload::GetPayloadTypeId() const
{
	return UE::AssetDocument::EditorPrimitive::SubobjectPayloadTypeId;
}

bool FAssetDocumentEditorSubobjectPayload::IsAlive() const
{
	UObject* ResolvedSubobject = Subobject.Get();
	UObject* ResolvedOuter = Outer.Get();
	if (!AreEmbeddedObjectsResolved(
		ResolvedSubobject, ResolvedOuter))
	{
		return false;
	}
	if (!HasExactEmbeddedOwnership(
		*ResolvedSubobject, *ResolvedOuter,
		SubobjectPath, OuterPath))
	{
		return false;
	}
	return HasExactEmbeddedRevisions(
			*ResolvedSubobject, *ResolvedOuter,
			SubobjectRevision, OuterRevision)
		&& HasExactAssetUserDataAttachment(
			*ResolvedSubobject, *ResolvedOuter)
		&& CategoryId == TEXT("asset_user_data.v1");
}

bool FAssetDocumentEditorSubobjectPayload::TryGetObjectIdentity(
	UE::AssetDocument::EditorPrimitive::FObjectIdentityView&
		OutView) const
{
	if (!IsAlive())
	{
		return false;
	}
	OutView.Object = Subobject.Get();
	OutView.CanonicalObjectPath = SubobjectPath;
	OutView.TypeRevision = SubobjectRevision;
	return true;
}
