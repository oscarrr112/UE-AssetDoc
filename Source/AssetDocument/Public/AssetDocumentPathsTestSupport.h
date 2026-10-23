// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Directory links for the path-scope tests: a symbolic link on Mac and Linux, a junction on Windows
// (no privilege needed). Header only, test builds only.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if !PLATFORM_WINDOWS
#include <unistd.h>
#endif

namespace AssetDocumentPathsTestSupport
{
// Makes Link a link to the existing directory Target (both absolute or project relative).
inline bool MakeDirectoryLink(const FString& Link, const FString& Target)
{
	const FString FullLink = FPaths::ConvertRelativePathToFull(Link);
	const FString FullTarget = FPaths::ConvertRelativePathToFull(Target);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FullLink), true);
#if PLATFORM_WINDOWS
	const FString Arguments = FString::Printf(TEXT("/c mklink /J \"%s\" \"%s\""),
		*FullLink.Replace(TEXT("/"), TEXT("\\")), *FullTarget.Replace(TEXT("/"), TEXT("\\")));
	int32 ReturnCode = -1;
	FString Output;
	FString Errors;
	return FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *Arguments, &ReturnCode, &Output, &Errors) && ReturnCode == 0;
#else
	return symlink(TCHAR_TO_UTF8(*FullTarget), TCHAR_TO_UTF8(*FullLink)) == 0;
#endif
}

// Makes Link a symbolic link to the file Target. On Windows this needs the right to create symbolic links
// (Developer Mode or an elevated process); false when the system refuses, and the caller skips that check.
inline bool MakeFileLink(const FString& Link, const FString& Target)
{
	const FString FullLink = FPaths::ConvertRelativePathToFull(Link);
	const FString FullTarget = FPaths::ConvertRelativePathToFull(Target);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FullLink), true);
#if PLATFORM_WINDOWS
	const FString Arguments = FString::Printf(TEXT("/c mklink \"%s\" \"%s\""),
		*FullLink.Replace(TEXT("/"), TEXT("\\")), *FullTarget.Replace(TEXT("/"), TEXT("\\")));
	int32 ReturnCode = -1;
	FString Output;
	FString Errors;
	return FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *Arguments, &ReturnCode, &Output, &Errors) && ReturnCode == 0;
#else
	return symlink(TCHAR_TO_UTF8(*FullTarget), TCHAR_TO_UTF8(*FullLink)) == 0;
#endif
}

// Removes a file link itself, never what it points to.
inline void RemoveFileLink(const FString& Link)
{
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*FPaths::ConvertRelativePathToFull(Link));
}

// Removes the link itself, never what it points to.
inline void RemoveDirectoryLink(const FString& Link)
{
	const FString FullLink = FPaths::ConvertRelativePathToFull(Link);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
#if PLATFORM_WINDOWS
	PlatformFile.DeleteDirectory(*FullLink);
#else
	PlatformFile.DeleteFile(*FullLink);
#endif
}

// A new directory outside the project (in the user's temporary directory).
inline FString MakeOutsideDirectory(const TCHAR* Label)
{
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(),
		FString::Printf(TEXT("AssetDocumentPaths_%s_%s"), Label, *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	IFileManager::Get().MakeDirectory(*Directory, true);
	return Directory;
}
}

#endif
