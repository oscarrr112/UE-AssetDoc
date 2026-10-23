// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
// Persistence plans name their target by long package name, so the atomic persistence tests
// mount their scratch root as content. A content mount hands the directory to the Asset
// Registry, whose asynchronous gatherer enumerates it at some later point and reads every
// .uasset it finds; the tests' placeholder packages are a few junk bytes, so a late read logs
// "Package is unloadable" (into whichever test is running then) and, on Windows, holds a read
// handle while the persistence replaces the file (an intermittent install failure).
//
// This mount is registered once per test while the root is still empty and its scan is
// completed synchronously, before any file is written. Afterwards the registry only learns of
// new files through the directory watcher, which is ticked between frames, never during the
// synchronous test; the mount is gone by then. Package names under the root come from
// PackageNameFor(<directory under the root>).
class FAssetDocumentAtomicTestMount
{
public:
	explicit FAssetDocumentAtomicTestMount(const FString& InRoot)
		: Root(InRoot)
		, MountPoint(FString::Printf(TEXT("/AssetDocumentAtomic%s/"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		check(Active == nullptr);
		IFileManager::Get().MakeDirectory(*Root, true);
		FPackageName::RegisterMountPoint(MountPoint, Root + TEXT("/"));
		IAssetRegistry::GetChecked().ScanPathsSynchronous({MountPoint.LeftChop(1)}, true);
		Active = this;
	}

	~FAssetDocumentAtomicTestMount()
	{
		Active = nullptr;
		FPackageName::UnRegisterMountPoint(MountPoint, Root + TEXT("/"));
	}

	FAssetDocumentAtomicTestMount(const FAssetDocumentAtomicTestMount&) = delete;
	FAssetDocumentAtomicTestMount& operator=(const FAssetDocumentAtomicTestMount&) = delete;

	// "<mount>/<Directory relative to the root>/Installed/Asset" for a directory under the
	// active mount's root.
	static FString PackageNameFor(const FString& Directory)
	{
		check(Active != nullptr && Directory.StartsWith(Active->Root));
		FString Relative = Directory.RightChop(Active->Root.Len());
		Relative.RemoveFromStart(TEXT("/"));
		return Active->MountPoint + (Relative.IsEmpty() ? FString() : Relative + TEXT("/")) + TEXT("Installed/Asset");
	}

private:
	FString Root;
	FString MountPoint;
	static inline FAssetDocumentAtomicTestMount* Active = nullptr;
};
#endif
