// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentSidecar.h"

#include "AssetDocumentJson.h"
#include "AssetDocumentSidecarPathUtils.h"

#include "Misc/Paths.h"

FString FAssetDocumentSidecar::ResolveObjectPathFromSidecar(const FString& FilePath)
{
	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	if (NormalizedPath.IsEmpty())
	{
		return FString();
	}

	const FString ContentDir = FAssetDocumentSidecarPathUtils::NormalizeContentDirectory();
	FString ContentRoot = ContentDir;
	if (!ContentRoot.EndsWith(TEXT("/")))
	{
		ContentRoot += TEXT("/");
	}

	if (!NormalizedPath.StartsWith(ContentRoot, ESearchCase::IgnoreCase))
	{
		return FString();
	}

	FString RelativePath = NormalizedPath.RightChop(ContentRoot.Len());
	FPaths::NormalizeFilename(RelativePath);

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

bool FAssetDocumentSidecar::LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
	return FAssetDocumentJson::LoadJsonFile(
		FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath),
		OutJson,
		OutError);
}

bool FAssetDocumentSidecar::WriteJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Json, FString& OutError)
{
	return FAssetDocumentJson::WriteJsonFile(
		FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath),
		Json,
		OutError);
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
