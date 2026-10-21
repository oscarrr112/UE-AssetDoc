// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentSidecarPathUtils.h"

#include "Misc/Paths.h"

FString FAssetDocumentSidecarPathUtils::NormalizeFilePath(const FString& FilePath)
{
	if (FilePath.IsEmpty())
	{
		return FString();
	}

	FString NormalizedPath = FilePath;
	FPaths::NormalizeFilename(NormalizedPath);
	if (FPaths::IsRelative(NormalizedPath) && NormalizedPath.StartsWith(TEXT("Content/"), ESearchCase::IgnoreCase))
	{
		NormalizedPath = FPaths::Combine(FPaths::ProjectDir(), NormalizedPath);
	}
	NormalizedPath = FPaths::ConvertRelativePathToFull(NormalizedPath);
	FPaths::NormalizeFilename(NormalizedPath);
	return NormalizedPath;
}

FString FAssetDocumentSidecarPathUtils::NormalizeWatchedFilePath(
	const FString& FilePath,
	const FString& WatchDirectory)
{
	FString ResolvedPath = FilePath;
	FPaths::NormalizeFilename(ResolvedPath);
	if (FPaths::IsRelative(ResolvedPath) && !WatchDirectory.IsEmpty())
	{
		ResolvedPath = FPaths::Combine(WatchDirectory, ResolvedPath);
	}
	return NormalizeFilePath(ResolvedPath);
}

FString FAssetDocumentSidecarPathUtils::NormalizeContentDirectory()
{
	FString ContentDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
	FPaths::NormalizeDirectoryName(ContentDirectory);
	return ContentDirectory;
}

FString FAssetDocumentSidecarPathUtils::NormalizeObjectPath(const FString& ObjectPath)
{
	FString NormalizedPath = ObjectPath;
	FPaths::NormalizeFilename(NormalizedPath);
	NormalizedPath.TrimStartAndEndInline();

	FString PackagePath;
	FString ObjectName;
	if (NormalizedPath.Split(TEXT("."), &PackagePath, &ObjectName))
	{
		NormalizedPath = PackagePath;
	}
	return NormalizedPath;
}
