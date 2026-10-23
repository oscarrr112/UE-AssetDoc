// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentFileWatcher.h"

#include "AssetDocumentIngressQueue.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentPaths.h"
#include "AssetDocumentSidecarPathUtils.h"

#include "DirectoryWatcherModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

FAssetDocumentFileWatcher::FAssetDocumentFileWatcher(
	const TSharedRef<FAssetDocumentIngressQueue>& InIngress)
	: Ingress(InIngress)
{
}

FAssetDocumentFileWatcher::~FAssetDocumentFileWatcher()
{
	Unregister();
}

void FAssetDocumentFileWatcher::Register()
{
	if (bRegistered)
	{
		return;
	}

	WatchDirectory = FAssetDocumentSidecarPathUtils::NormalizeContentDirectory();
	if (WatchDirectory.IsEmpty())
	{
		UE_LOG(LogAssetDocument, Warning, TEXT("AssetDocument file watcher could not resolve project Content directory"));
		return;
	}

	FDirectoryWatcherModule& DirectoryWatcherModule = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	IDirectoryWatcher* DirectoryWatcher = DirectoryWatcherModule.Get();
	if (!DirectoryWatcher)
	{
		UE_LOG(LogAssetDocument, Warning, TEXT("DirectoryWatcher module returned no watcher"));
		return;
	}

	if (!DirectoryWatcher->RegisterDirectoryChangedCallback_Handle(
		WatchDirectory,
		IDirectoryWatcher::FDirectoryChanged::CreateRaw(this, &FAssetDocumentFileWatcher::HandleDirectoryChanged),
		WatchHandle,
		0))
	{
		UE_LOG(LogAssetDocument, Warning, TEXT("Failed to register AssetDocument file watcher for '%s'"), *WatchDirectory);
		return;
	}

	bRegistered = true;
	UE_LOG(LogAssetDocument, Log, TEXT("AssetDocument file watcher registered for '%s'"), *WatchDirectory);
}

void FAssetDocumentFileWatcher::Unregister()
{
	if (!bRegistered)
	{
		return;
	}

	if (WatchHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("DirectoryWatcher")))
	{
		FDirectoryWatcherModule& DirectoryWatcherModule = FModuleManager::GetModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
		if (IDirectoryWatcher* DirectoryWatcher = DirectoryWatcherModule.Get())
		{
			DirectoryWatcher->UnregisterDirectoryChangedCallback_Handle(WatchDirectory, WatchHandle);
		}
		WatchHandle.Reset();
	}

	bRegistered = false;
}

bool FAssetDocumentFileWatcher::IsAssetDocumentSidecarPath(const FString& FilePath)
{
	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	if (!NormalizedPath.EndsWith(TEXT(".assetdoc.json"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	FString RelativePath;
	return AssetDocumentPaths::MakeRelativeWithin(FAssetDocumentSidecarPathUtils::NormalizeContentDirectory(), NormalizedPath, RelativePath)
		&& !RelativePath.IsEmpty();
}

void FAssetDocumentFileWatcher::CollectAssetDocumentSidecarsUnderDirectory(const FString& Directory, TArray<FString>& OutFilePaths)
{
	OutFilePaths.Reset();

	FString NormalizedDirectory = FAssetDocumentSidecarPathUtils::NormalizeFilePath(Directory);
	FPaths::NormalizeDirectoryName(NormalizedDirectory);
	if (NormalizedDirectory.IsEmpty() || !IFileManager::Get().DirectoryExists(*NormalizedDirectory))
	{
		return;
	}

	TArray<FString> FoundFiles;
	IFileManager::Get().FindFilesRecursive(FoundFiles, *NormalizedDirectory, TEXT("*.assetdoc.json"), true, false);

	for (FString& FoundFile : FoundFiles)
	{
		FoundFile = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FoundFile);
		if (IsAssetDocumentSidecarPath(FoundFile))
		{
			OutFilePaths.Add(FoundFile);
		}
	}

	OutFilePaths.Sort();
}

bool FAssetDocumentFileWatcher::TryReadFileState(const FString& FilePath, FAssetDocumentWatchedFileState& OutState)
{
	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.FileExists(*NormalizedPath))
	{
		return false;
	}

	OutState.FileSize = FileManager.FileSize(*NormalizedPath);
	OutState.Timestamp = FileManager.GetTimeStamp(*NormalizedPath);
	return OutState.FileSize >= 0 && OutState.Timestamp != FDateTime::MinValue();
}

bool FAssetDocumentFileWatcher::IsStableFileState(const FString& FilePath, const FAssetDocumentWatchedFileState& PreviousState)
{
	FAssetDocumentWatchedFileState CurrentState;
	return TryReadFileState(FilePath, CurrentState) && CurrentState == PreviousState;
}

bool FAssetDocumentFileWatcher::ShouldSkipAutoApplyForDirtyPackage(const FString& Target, FString& OutWarning)
{
	const FString NormalizedTarget = FAssetDocumentSidecarPathUtils::NormalizeObjectPath(Target);
	if (NormalizedTarget.IsEmpty())
	{
		return false;
	}

	UPackage* Package = FindPackage(nullptr, *NormalizedTarget);
	if (!Package || !Package->IsDirty())
	{
		return false;
	}

	OutWarning = FString::Printf(
		TEXT("Skipped AssetDocument auto apply for '%s' because the target package is dirty; save or revert the asset before editing the sidecar."),
		*NormalizedTarget);
	return true;
}

FGuid FAssetDocumentFileWatcher::GetIngressCoordinatorId() const
{
	return Ingress.IsValid() ? Ingress->GetCoordinatorId() : FGuid();
}

void FAssetDocumentFileWatcher::HandleDirectoryChanged(const TArray<FFileChangeData>& FileChanges)
{
	for (const FFileChangeData& FileChange : FileChanges)
	{
		FAssetDocumentIngressEvent Event;
		if (FileChange.Action == FFileChangeData::FCA_RescanRequired)
		{
			Event.Type =
				EAssetDocumentIngressEventType::
					SidecarRescanRequested;
			Event.PrimaryPath = WatchDirectory;
		}
		else if (
			FileChange.Action == FFileChangeData::FCA_Added
			|| FileChange.Action
				== FFileChangeData::FCA_Modified)
		{
			Event.Type =
				EAssetDocumentIngressEventType::SidecarChanged;
			Event.PrimaryPath =
				FAssetDocumentSidecarPathUtils::
					NormalizeWatchedFilePath(
						FileChange.Filename,
						WatchDirectory);
			if (!IsAssetDocumentSidecarPath(Event.PrimaryPath))
			{
				continue;
			}
		}
		else
		{
			continue;
		}

		if (Ingress.IsValid())
		{
			Ingress->Enqueue(MoveTemp(Event));
		}
	}
}
