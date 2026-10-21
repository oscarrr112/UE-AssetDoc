// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Containers/ArrayView.h"
#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
enum class EAssetDocumentAtomicFileFailurePoint : uint8
{
	None,
	Open,
	PartialWrite,
	Flush,
	Rename,
	DirectoryFlush,
};
#endif

class FAssetDocumentAtomicFile
{
public:
	static bool WriteBytesAtomically(
		const FString& DestinationPath,
		TArrayView64<const uint8> Bytes,
		FString& OutError);
	static bool FlushParentDirectory(const FString& Path, FString& OutError);
	// Windows cannot rename over or delete a file that still has an open handle, and the
	// editor keeps the linker (and its file handle) of a loaded package open for lazy bulk
	// data. Detach the loaders of the package backed by Path (game thread only) before the
	// on-disk file is replaced or deleted. No-op elsewhere: POSIX replaces open files freely.
	static void ReleaseLoadedPackageFileHandles(const FString& Path);

#if WITH_DEV_AUTOMATION_TESTS
	static void FailNextWriteAtForTest(EAssetDocumentAtomicFileFailurePoint FailurePoint);
	static void ForceNextHardLinkBackupFallbackForTest();
	static void ForceNextCommittedRenameRollbackFailureForTest();
	static void ResetFailureForTest();
#endif
};
