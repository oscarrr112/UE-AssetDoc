// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Cutover/AssetDocumentEditorMutationFootprintBuilders.h"

#include "AssetDocumentEditorTrialScope.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace
{
constexpr const TCHAR* CreatedRollbackComponentId =
	TEXT("editor.asset.rollback_created.v1");

FString MakeFactoryStableId(
	const TCHAR* Kind,
	const FString& CanonicalResourceIdentity)
{
	return FString::Printf(
		TEXT("%s|%s|%s"),
		CreatedRollbackComponentId,
		Kind,
		*CanonicalResourceIdentity);
}
}

bool TryBuildAssetDocumentEditorMutationIdentity(
	FStringView CanonicalObjectPath,
	FAssetDocumentEditorMutationIdentity& OutIdentity)
{
	OutIdentity = {};
	const FString ObjectPath(CanonicalObjectPath);
	const FSoftObjectPath SoftPath(ObjectPath);
	if (ObjectPath.IsEmpty()
		|| !FPackageName::IsValidObjectPath(ObjectPath)
		|| SoftPath.IsSubobject()
		|| SoftPath.ToString() != ObjectPath)
	{
		return false;
	}
	const FString PackageName = SoftPath.GetLongPackageName();
	const FString AssetName = SoftPath.GetAssetName();
	// Validate's staged trial targets live under the read-only /Temp root (never saved).
	const UPackage* const Loaded = FindPackage(nullptr, *PackageName);
	const bool bStagedTrial = Loaded != nullptr && FAssetDocumentEditorTrialScope::IsStagedTrialPackage(*Loaded);
	if (!FPackageName::IsValidLongPackageName(PackageName, /*bIncludeReadOnlyRoots*/ bStagedTrial))
	{
		return false;
	}
	if (AssetName.IsEmpty()
		|| ObjectPath
			!= FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName))
	{
		return false;
	}
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	if (Filename.IsEmpty())
	{
		return false;
	}
	OutIdentity.CanonicalObjectPath = ObjectPath;
	OutIdentity.PackageName = PackageName;
	OutIdentity.PackageFilename = Filename;
	return true;
}

TArray<FAssetDocumentMutationFootprint>
BuildAssetDocumentEditorPropertyMutationFootprint(
	const FAssetDocumentEditorMutationIdentity& Identity)
{
	return {
		{EAssetDocumentMutationFootprintKind::UObjectState,
			FString::Printf(
				TEXT("Object:%s"), *Identity.CanonicalObjectPath),
			TEXT("rollback.uobject")},
		{EAssetDocumentMutationFootprintKind::Package,
			FString::Printf(
				TEXT("Package:%s"), *Identity.PackageName),
			TEXT("rollback.package")},
	};
}

TArray<FAssetDocumentMutationFootprint>
BuildAssetDocumentEditorFactoryMutationFootprint(
	const FAssetDocumentEditorMutationIdentity& Identity)
{
	return {
		{EAssetDocumentMutationFootprintKind::UObjectState,
			MakeFactoryStableId(
				TEXT("UObjectState"), Identity.CanonicalObjectPath),
			CreatedRollbackComponentId},
		{EAssetDocumentMutationFootprintKind::Package,
			MakeFactoryStableId(TEXT("Package"), Identity.PackageName),
			CreatedRollbackComponentId},
		{EAssetDocumentMutationFootprintKind::AssetRegistry,
			MakeFactoryStableId(
				TEXT("AssetRegistry"), Identity.CanonicalObjectPath),
			CreatedRollbackComponentId},
		{EAssetDocumentMutationFootprintKind::File,
			MakeFactoryStableId(TEXT("File"), Identity.PackageFilename),
			CreatedRollbackComponentId},
		{EAssetDocumentMutationFootprintKind::Handle,
			MakeFactoryStableId(
				TEXT("Handle"), Identity.CanonicalObjectPath),
			CreatedRollbackComponentId},
	};
}

TArray<FAssetDocumentMutationFootprint>
BuildAssetDocumentEditorEmbeddedMutationFootprint(
	FStringView CanonicalOuterPath)
{
	const FString OuterPath(CanonicalOuterPath);
	const FSoftObjectPath SoftPath(OuterPath);
	if (OuterPath.IsEmpty()
		|| !FPackageName::IsValidObjectPath(OuterPath)
		|| SoftPath.ToString() != OuterPath)
	{
		return {};
	}
	return {
		{EAssetDocumentMutationFootprintKind::UObjectState,
			OuterPath,
			TEXT("assetdocument.editor.embedded.asset_user_data.v1")},
	};
}
