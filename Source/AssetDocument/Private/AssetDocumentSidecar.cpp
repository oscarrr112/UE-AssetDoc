// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentSidecar.h"

#include "AssetDocumentJson.h"
#include "AssetDocumentPaths.h"
#include "AssetDocumentSidecarPathUtils.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FString FAssetDocumentSidecar::ResolveObjectPathFromSidecar(const FString& FilePath)
{
	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	if (NormalizedPath.IsEmpty())
	{
		return FString();
	}

	// The file must really be in the project's Content directory: links are followed and names compared
	// with the volume's case rules, and the target comes from the path as the filesystem stores it.
	FString RelativePath;
	if (!AssetDocumentPaths::MakeRelativeWithin(FAssetDocumentSidecarPathUtils::NormalizeContentDirectory(), NormalizedPath, RelativePath)
		|| RelativePath.IsEmpty())
	{
		return FString();
	}

	FString AssetName = FPaths::GetCleanFilename(RelativePath);
	if (!AssetName.RemoveFromEnd(TEXT(".assetdoc.json"), ESearchCase::IgnoreCase))
	{
		return FString();
	}

	const FString RelativeDirectory = FPaths::GetPath(RelativePath);
	FString ObjectPath = TEXT("/Game");
	if (!RelativeDirectory.IsEmpty())
	{
		ObjectPath /= RelativeDirectory;
	}
	ObjectPath /= AssetName;
	FPaths::NormalizeFilename(ObjectPath);
	return ObjectPath;
}

FString FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(const FString& ObjectPath)
{
	const FString NormalizedObjectPath = FAssetDocumentSidecarPathUtils::NormalizeObjectPath(ObjectPath);
	if (!NormalizedObjectPath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
	{
		return FString();
	}

	FString RelativePath = NormalizedObjectPath.RightChop(UE_ARRAY_COUNT(TEXT("/Game/")) - 1);
	RelativePath += TEXT(".assetdoc.json");

	FString FilePath = FPaths::Combine(FPaths::ProjectContentDir(), RelativePath);
	FilePath = FPaths::ConvertRelativePathToFull(FilePath);
	FPaths::NormalizeFilename(FilePath);
	return FilePath;
}

bool FAssetDocumentSidecar::ResolveSidecarFile(const FString& FilePath, FString& OutRealPath, FString& OutError)
{
	OutRealPath.Reset();
	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	const FString ContentDirectory = FAssetDocumentSidecarPathUtils::NormalizeContentDirectory();
	FString RealPath;
	if (NormalizedPath.IsEmpty() || !AssetDocumentPaths::ResolveForAccess(ContentDirectory, NormalizedPath, RealPath)
		|| RealPath.Equals(AssetDocumentPaths::ResolveRealPath(ContentDirectory), ESearchCase::CaseSensitive)
		|| !NormalizedPath.EndsWith(TEXT(".assetdoc.json"), ESearchCase::IgnoreCase)
		|| !RealPath.EndsWith(TEXT(".assetdoc.json"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("Unable to resolve expected Target from sidecar file '%s': a sidecar is a "
			"<AssetName>.assetdoc.json file inside the project's Content directory (%s), not reached through a link "
			"that leads out of it; the file was not read"), *NormalizedPath, *ContentDirectory);
		return false;
	}
	OutRealPath = RealPath;
	return true;
}

bool FAssetDocumentSidecar::LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
	FString RealPath;
	return ResolveSidecarFile(FilePath, RealPath, OutError) && FAssetDocumentJson::LoadJsonFile(RealPath, OutJson, OutError);
}

bool FAssetDocumentSidecar::LoadText(const FString& FilePath, FString& OutText, FString& OutError)
{
	FString RealPath;
	if (!ResolveSidecarFile(FilePath, RealPath, OutError))
	{
		return false;
	}
	if (!FFileHelper::LoadFileToString(OutText, *RealPath))
	{
		OutError = FString::Printf(TEXT("Failed to read JSON file '%s'"), *RealPath);
		return false;
	}
	return true;
}

bool FAssetDocumentSidecar::LoadBytes(const FString& FilePath, TArray<uint8>& OutBytes, FString& OutError)
{
	FString RealPath;
	if (!ResolveSidecarFile(FilePath, RealPath, OutError))
	{
		return false;
	}
	if (!FFileHelper::LoadFileToArray(OutBytes, *RealPath, FILEREAD_Silent))
	{
		OutError = FString::Printf(TEXT("Failed to read file '%s'"), *RealPath);
		return false;
	}
	return true;
}

bool FAssetDocumentSidecar::WriteJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Json, FString& OutError)
{
	FString RealPath;
	return ResolveSidecarFile(FilePath, RealPath, OutError) && FAssetDocumentJson::WriteJsonFile(RealPath, Json, OutError);
}

bool FAssetDocumentSidecar::ValidateTargetMatchesSidecar(const FString& FilePath, const TSharedPtr<FJsonObject>& Json, FString& OutError)
{
	if (FilePath.IsEmpty())
	{
		return true;
	}

	if (!Json.IsValid())
	{
		OutError = TEXT("Cannot validate sidecar Target for an invalid JSON document");
		return false;
	}

	FString Target;
	if (!Json->TryGetStringField(TEXT("Target"), Target) || Target.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("Target is required for sidecar file '%s'"),
			*FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath));
		return false;
	}

	const FString ExpectedTarget = ResolveObjectPathFromSidecar(FilePath);
	if (ExpectedTarget.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("Unable to resolve expected Target from sidecar file '%s'"),
			*FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath));
		return false;
	}

	const FString NormalizedTarget = FAssetDocumentSidecarPathUtils::NormalizeObjectPath(Target);
	if (!NormalizedTarget.Equals(ExpectedTarget, ESearchCase::CaseSensitive))
	{
		OutError = FString::Printf(
			TEXT("Target '%s' does not match sidecar file '%s'; expected '%s'"),
			*NormalizedTarget,
			*FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath),
			*ExpectedTarget);
		return false;
	}

	return true;
}
