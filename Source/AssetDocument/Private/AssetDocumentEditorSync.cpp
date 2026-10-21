// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorSync.h"

#include "AssetDocumentIngressQueue.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentSidecar.h"
#include "AssetDocumentSidecarPathUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace
{
FCriticalSection GSidecarSyncGuardLock;
TMap<FString, int32> GActiveSidecarWrites;
TMap<FString, FDateTime> GRecentSidecarWrites;
int32 GActiveSidecarSyncDepth = 0;

FString ObjectPathFromAssetData(const FAssetData& AssetData)
{
	if (!AssetData.PackageName.IsNone())
	{
		return AssetData.PackageName.ToString();
	}

	return FAssetDocumentSidecarPathUtils::NormalizeObjectPath(AssetData.GetObjectPathString());
}

bool IsGameObjectPath(const FString& ObjectPath)
{
	return ObjectPath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive);
}

bool FileExists(const FString& FilePath)
{
	return !FilePath.IsEmpty() && IFileManager::Get().FileExists(*FilePath);
}

void RememberSidecarWriteLocked(const FString& FilePath)
{
	if (FilePath.IsEmpty())
	{
		return;
	}

	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	GActiveSidecarWrites.FindOrAdd(NormalizedPath)++;
	GRecentSidecarWrites.Add(NormalizedPath, FDateTime::UtcNow() + FTimespan::FromSeconds(5));
}

void ForgetActiveSidecarWriteLocked(const FString& FilePath)
{
	if (FilePath.IsEmpty())
	{
		return;
	}

	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	if (int32* Count = GActiveSidecarWrites.Find(NormalizedPath))
	{
		(*Count)--;
		if (*Count <= 0)
		{
			GActiveSidecarWrites.Remove(NormalizedPath);
		}
	}
}

void PruneRecentSidecarWritesLocked()
{
	const FDateTime Now = FDateTime::UtcNow();
	for (TMap<FString, FDateTime>::TIterator It(GRecentSidecarWrites); It; ++It)
	{
		if (It.Value() <= Now)
		{
			It.RemoveCurrent();
		}
	}
}

bool LoadAndRetargetSidecar(const FString& SourceSidecarPath, const FString& DestinationSidecarPath, const FString& NewTarget)
{
	if (!FileExists(SourceSidecarPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Document;
	FString Error;
	if (!FAssetDocumentSidecar::LoadJsonFile(SourceSidecarPath, Document, Error))
	{
		UE_LOG(LogAssetDocument, Warning, TEXT("Failed to load AssetDocument sidecar '%s': %s"), *SourceSidecarPath, *Error);
		return false;
	}

	Document->SetStringField(TEXT("Target"), NewTarget);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestinationSidecarPath), true);

	FAssetDocumentEditorSync::FScopedSidecarWrite Guard(DestinationSidecarPath);
	if (!FAssetDocumentSidecar::WriteJsonFile(DestinationSidecarPath, Document, Error))
	{
		UE_LOG(LogAssetDocument, Warning, TEXT("Failed to write AssetDocument sidecar '%s': %s"), *DestinationSidecarPath, *Error);
		return false;
	}

	return true;
}

FString MakeUniqueArchivePath(const FString& SourceSidecarPath)
{
	FString RelativePath = SourceSidecarPath;
	FPaths::MakePathRelativeTo(RelativePath, *FPaths::ProjectContentDir());
	FPaths::NormalizeFilename(RelativePath);

	const FString RelativeDirectory = FPaths::GetPath(RelativePath);
	const FString BaseName = FPaths::GetBaseFilename(RelativePath);
	const FString Extension = TEXT(".assetdoc.json");

	const FString ArchiveDirectory = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("_DeletedAssetDocs"), RelativeDirectory);
	IFileManager::Get().MakeDirectory(*ArchiveDirectory, true);

	FString ArchivePath = FPaths::Combine(ArchiveDirectory, BaseName + Extension);
	if (!FileExists(ArchivePath))
	{
		FPaths::NormalizeFilename(ArchivePath);
		return ArchivePath;
	}

	const FString Stamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
	ArchivePath = FPaths::Combine(
		ArchiveDirectory,
		FString::Printf(TEXT("%s__%s_%s%s"), *BaseName, *Stamp, *FGuid::NewGuid().ToString(EGuidFormats::Digits), *Extension));
	FPaths::NormalizeFilename(ArchivePath);
	return ArchivePath;
}
}

FAssetDocumentEditorSync::FAssetDocumentEditorSync(
	const TSharedRef<FAssetDocumentIngressQueue>& InIngress)
	: Ingress(InIngress)
{
}

FAssetDocumentEditorSync::FScopedSidecarWrite::FScopedSidecarWrite(const TArray<FString>& InFilePaths)
	: FilePaths(InFilePaths)
{
	FScopeLock Lock(&GSidecarSyncGuardLock);
	GActiveSidecarSyncDepth++;
	for (const FString& FilePath : FilePaths)
	{
		RememberSidecarWriteLocked(FilePath);
	}
}

FAssetDocumentEditorSync::FScopedSidecarWrite::FScopedSidecarWrite(const FString& InFilePath)
	: FScopedSidecarWrite(TArray<FString>{ InFilePath })
{
}

FAssetDocumentEditorSync::FScopedSidecarWrite::~FScopedSidecarWrite()
{
	FScopeLock Lock(&GSidecarSyncGuardLock);
	for (const FString& FilePath : FilePaths)
	{
		ForgetActiveSidecarWriteLocked(FilePath);
	}
	GActiveSidecarSyncDepth = FMath::Max(0, GActiveSidecarSyncDepth - 1);
}

FAssetDocumentEditorSync::~FAssetDocumentEditorSync()
{
	Unregister();
}

void FAssetDocumentEditorSync::Register()
{
	if (bRegistered)
	{
		return;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	AssetRenamedHandle = AssetRegistry.OnAssetRenamed().AddRaw(this, &FAssetDocumentEditorSync::HandleAssetRenamed);
	AssetAddedHandle = AssetRegistry.OnAssetAdded().AddRaw(this, &FAssetDocumentEditorSync::HandleAssetAdded);
	AssetRemovedHandle = AssetRegistry.OnAssetRemoved().AddRaw(this, &FAssetDocumentEditorSync::HandleAssetRemoved);
	bRegistered = true;
}

void FAssetDocumentEditorSync::Unregister()
{
	if (!bRegistered)
	{
		return;
	}

	if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		AssetRegistry.OnAssetRenamed().Remove(AssetRenamedHandle);
		AssetRegistry.OnAssetAdded().Remove(AssetAddedHandle);
		AssetRegistry.OnAssetRemoved().Remove(AssetRemovedHandle);
	}

	AssetRenamedHandle.Reset();
	AssetAddedHandle.Reset();
	AssetRemovedHandle.Reset();
	bRegistered = false;
}

bool FAssetDocumentEditorSync::IsSidecarWriteSuppressed(const FString& FilePath)
{
	FScopeLock Lock(&GSidecarSyncGuardLock);
	PruneRecentSidecarWritesLocked();

	if (GActiveSidecarSyncDepth > 0 && FilePath.IsEmpty())
	{
		return true;
	}

	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	return GActiveSidecarWrites.Contains(NormalizedPath) || GRecentSidecarWrites.Contains(NormalizedPath);
}

FString FAssetDocumentEditorSync::InferDuplicateSourceObjectPath(const FString& NewObjectPath)
{
	const FString NormalizedNewObjectPath = FAssetDocumentSidecarPathUtils::NormalizeObjectPath(NewObjectPath);
	if (!IsGameObjectPath(NormalizedNewObjectPath))
	{
		return FString();
	}

	const FString NewName = FPackageName::GetLongPackageAssetName(NormalizedNewObjectPath);
	const FString CopyMarker = TEXT("_Copy");
	const int32 CopyMarkerIndex = NewName.Find(CopyMarker, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (CopyMarkerIndex <= 0)
	{
		return FString();
	}

	const FString Suffix = NewName.RightChop(CopyMarkerIndex + CopyMarker.Len());
	for (const TCHAR Character : Suffix)
	{
		if (!FChar::IsDigit(Character))
		{
			return FString();
		}
	}

	const FString SourceName = NewName.Left(CopyMarkerIndex);
	const FString PackagePath = FPackageName::GetLongPackagePath(NormalizedNewObjectPath);
	return PackagePath / SourceName;
}

FGuid FAssetDocumentEditorSync::GetIngressCoordinatorId() const
{
	return Ingress.IsValid() ? Ingress->GetCoordinatorId() : FGuid();
}

bool FAssetDocumentEditorSync::SyncSidecarForRenameOrMove(const FString& OldObjectPath, const FString& NewObjectPath)
{
	const FString OldTarget = FAssetDocumentSidecarPathUtils::NormalizeObjectPath(OldObjectPath);
	const FString NewTarget = FAssetDocumentSidecarPathUtils::NormalizeObjectPath(NewObjectPath);
	if (!IsGameObjectPath(OldTarget) || !IsGameObjectPath(NewTarget))
	{
		return false;
	}

	const FString OldSidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(OldTarget);
	const FString NewSidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(NewTarget);
	if (!LoadAndRetargetSidecar(OldSidecarPath, NewSidecarPath, NewTarget))
	{
		return false;
	}

	if (!OldSidecarPath.Equals(NewSidecarPath, ESearchCase::IgnoreCase) && FileExists(OldSidecarPath))
	{
		FScopedSidecarWrite Guard(OldSidecarPath);
		if (!IFileManager::Get().Delete(*OldSidecarPath, false, true))
		{
			UE_LOG(LogAssetDocument, Warning, TEXT("Failed to remove old AssetDocument sidecar '%s' after moving to '%s'"), *OldSidecarPath, *NewSidecarPath);
			return false;
		}
	}

	UE_LOG(LogAssetDocument, Log, TEXT("Synced AssetDocument sidecar rename/move '%s' -> '%s'"), *OldTarget, *NewTarget);
	return true;
}

bool FAssetDocumentEditorSync::SyncSidecarForDuplicate(const FString& SourceObjectPath, const FString& NewObjectPath)
{
	const FString SourceTarget = FAssetDocumentSidecarPathUtils::NormalizeObjectPath(SourceObjectPath);
	const FString NewTarget = FAssetDocumentSidecarPathUtils::NormalizeObjectPath(NewObjectPath);
	if (!IsGameObjectPath(SourceTarget) || !IsGameObjectPath(NewTarget))
	{
		return false;
	}

	const FString SourceSidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(SourceTarget);
	const FString NewSidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(NewTarget);
	if (FileExists(NewSidecarPath))
	{
		return false;
	}

	if (!LoadAndRetargetSidecar(SourceSidecarPath, NewSidecarPath, NewTarget))
	{
		return false;
	}

	UE_LOG(LogAssetDocument, Log, TEXT("Synced AssetDocument sidecar duplicate '%s' -> '%s'"), *SourceTarget, *NewTarget);
	return true;
}

bool FAssetDocumentEditorSync::ArchiveSidecarForDeletedAsset(const FString& ObjectPath, FString* OutArchivePath)
{
	const FString Target = FAssetDocumentSidecarPathUtils::NormalizeObjectPath(ObjectPath);
	if (!IsGameObjectPath(Target))
	{
		return false;
	}

	const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
	if (!FileExists(SidecarPath))
	{
		return false;
	}

	const FString ArchivePath = MakeUniqueArchivePath(SidecarPath);
	FScopedSidecarWrite Guard(TArray<FString>{ SidecarPath, ArchivePath });
	if (!IFileManager::Get().Move(*ArchivePath, *SidecarPath, false, true))
	{
		UE_LOG(LogAssetDocument, Warning, TEXT("Failed to archive deleted AssetDocument sidecar '%s' to '%s'"), *SidecarPath, *ArchivePath);
		return false;
	}

	if (OutArchivePath)
	{
		*OutArchivePath = ArchivePath;
	}

	UE_LOG(LogAssetDocument, Log, TEXT("Archived deleted AssetDocument sidecar '%s' to '%s'"), *SidecarPath, *ArchivePath);
	return true;
}

void FAssetDocumentEditorSync::HandleAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath)
{
	FAssetDocumentIngressEvent Event;
	Event.Type = EAssetDocumentIngressEventType::AssetRenamedOrMoved;
	Event.PrimaryPath = OldObjectPath;
	Event.SecondaryPath = ObjectPathFromAssetData(AssetData);
	if (Ingress.IsValid())
	{
		Ingress->Enqueue(MoveTemp(Event));
	}
}

void FAssetDocumentEditorSync::HandleAssetAdded(const FAssetData& AssetData)
{
	FAssetDocumentIngressEvent Event;
	Event.Type = EAssetDocumentIngressEventType::AssetAdded;
	Event.PrimaryPath = ObjectPathFromAssetData(AssetData);
	if (Ingress.IsValid())
	{
		Ingress->Enqueue(MoveTemp(Event));
	}
}

void FAssetDocumentEditorSync::HandleAssetRemoved(const FAssetData& AssetData)
{
	FAssetDocumentIngressEvent Event;
	Event.Type = EAssetDocumentIngressEventType::AssetRemoved;
	Event.PrimaryPath = ObjectPathFromAssetData(AssetData);
	if (Ingress.IsValid())
	{
		Ingress->Enqueue(MoveTemp(Event));
	}
}
