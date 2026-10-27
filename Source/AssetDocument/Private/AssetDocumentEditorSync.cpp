// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorSync.h"

#include "AssetDocumentIngressQueue.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentSidecar.h"
#include "AssetDocumentSidecarPathUtils.h"
#include "AssetDocumentSidecarSyncState.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"

namespace EditorSyncWriteGuardLocal
{
// An editor write of a sidecar: when it stops hiding changes of the file, and the file's content as the write left it
// (unset while the write is running and has recorded no write).
struct FRecentSidecarWrite
{
	FDateTime Expiry;
	TOptional<FString> Digest;
	// Digest is what a recorded write left (not the file at the end of the scope).
	bool bNoted = false;
	bool bOwnWritesOnly = false;
	// The content the scope's writes may replace (unset: any).
	TOptional<FString> Expected;
	uint64 Serial = 0;
};

uint64 GSidecarWriteSerial = 0;

// The file's content digest; empty when there is no file.
FString DigestOfFile(const FString& FilePath)
{
	return UE::AssetDocument::SidecarSync::DigestSidecarFile(FilePath);
}
}

namespace
{
FCriticalSection GSidecarSyncGuardLock;
TMap<FString, int32> GActiveSidecarWrites;
TMap<FString, EditorSyncWriteGuardLocal::FRecentSidecarWrite> GRecentSidecarWrites;
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

void RememberSidecarWriteLocked(const FString& FilePath, bool bOwnWritesOnly, const TOptional<FString>& Expected)
{
	if (FilePath.IsEmpty())
	{
		return;
	}

	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	GActiveSidecarWrites.FindOrAdd(NormalizedPath)++;
	EditorSyncWriteGuardLocal::FRecentSidecarWrite Recent;
	Recent.Expiry = FDateTime::UtcNow() + FTimespan::FromSeconds(5);
	Recent.bOwnWritesOnly = bOwnWritesOnly;
	Recent.Expected = Expected;
	GRecentSidecarWrites.Add(NormalizedPath, MoveTemp(Recent));
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
	for (auto It = GRecentSidecarWrites.CreateIterator(); It; ++It)
	{
		// A write still running keeps its record, however long it takes.
		if (It.Value().Expiry <= Now && !GActiveSidecarWrites.Contains(It.Key()))
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
	// Checked before its folder is made: no folder is created through a link that leads out of Content.
	FString DestinationRealPath;
	if (!FAssetDocumentSidecar::ResolveSidecarFile(DestinationSidecarPath, DestinationRealPath, Error))
	{
		UE_LOG(LogAssetDocument, Warning, TEXT("AssetDocument sidecar '%s' not written: %s"), *DestinationSidecarPath, *Error);
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestinationRealPath), true);

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
		RememberSidecarWriteLocked(FilePath, false, TOptional<FString>());
	}
}

FAssetDocumentEditorSync::FScopedSidecarWrite::FScopedSidecarWrite(const FString& InFilePath)
	: FScopedSidecarWrite(TArray<FString>{ InFilePath })
{
}

FAssetDocumentEditorSync::FScopedSidecarWrite::FScopedSidecarWrite(const FString& InFilePath, ERecord InRecord,
	const TOptional<FString>& InExpectedDigest)
	: FilePaths({InFilePath})
	, Record(InRecord)
{
	FScopeLock Lock(&GSidecarSyncGuardLock);
	GActiveSidecarSyncDepth++;
	RememberSidecarWriteLocked(InFilePath, InRecord == ERecord::OwnWrites, InExpectedDigest);
}

FAssetDocumentEditorSync::FScopedSidecarWrite::~FScopedSidecarWrite()
{
	// What the scope left in each file: a later change of the file to anything else is not the editor's.
	TArray<FString> Digests;
	for (const FString& FilePath : FilePaths)
	{
		Digests.Add(Record == ERecord::FileAtEnd
			? EditorSyncWriteGuardLocal::DigestOfFile(FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath))
			: FString());
	}
	FScopeLock Lock(&GSidecarSyncGuardLock);
	for (int32 Index = 0; Index < FilePaths.Num(); ++Index)
	{
		ForgetActiveSidecarWriteLocked(FilePaths[Index]);
		const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePaths[Index]);
		EditorSyncWriteGuardLocal::FRecentSidecarWrite* Recent = GRecentSidecarWrites.Find(NormalizedPath);
		if (Recent == nullptr)
		{
			continue;
		}
		const bool bStillActive = GActiveSidecarWrites.Contains(NormalizedPath);
		if (!bStillActive)
		{
			Recent->Expected.Reset();
		}
		if (Recent->bNoted)
		{
			continue;
		}
		if (Record == ERecord::FileAtEnd)
		{
			Recent->Digest = Digests[Index];
			Recent->Expiry = FDateTime::UtcNow() + FTimespan::FromSeconds(5);
		}
		else if (!bStillActive)
		{
			// Nothing written: nothing to hide.
			GRecentSidecarWrites.Remove(NormalizedPath);
		}
	}
	GActiveSidecarSyncDepth = FMath::Max(0, GActiveSidecarSyncDepth - 1);
}

bool FAssetDocumentEditorSync::CheckSidecarUnchanged(const FString& FilePath, FString& OutError)
{
	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	FString Expected;
	{
		FScopeLock Lock(&GSidecarSyncGuardLock);
		const EditorSyncWriteGuardLocal::FRecentSidecarWrite* Recent = GRecentSidecarWrites.Find(NormalizedPath);
		if (Recent == nullptr || !Recent->Expected.IsSet() || !GActiveSidecarWrites.Contains(NormalizedPath))
		{
			return true;
		}
		Expected = Recent->Expected.GetValue();
	}
	if (EditorSyncWriteGuardLocal::DigestOfFile(NormalizedPath) == Expected)
	{
		return true;
	}
	OutError = FString::Printf(TEXT("'%s' changed after the content this write is based on was read; it was not overwritten"),
		*NormalizedPath);
	return false;
}

void FAssetDocumentEditorSync::NoteSidecarWritten(const FString& FilePath, const uint8* Data, int64 Num)
{
	FString Digest;
	if (Data != nullptr)
	{
		FSHAHash Hash;
		FSHA1::HashBuffer(Data, static_cast<uint64>(FMath::Max<int64>(Num, 0)), Hash.Hash);
		Digest = Hash.ToString();
	}
	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	FScopeLock Lock(&GSidecarSyncGuardLock);
	EditorSyncWriteGuardLocal::FRecentSidecarWrite* Recent = GRecentSidecarWrites.Find(NormalizedPath);
	if (Recent == nullptr || !GActiveSidecarWrites.Contains(NormalizedPath))
	{
		return;
	}
	Recent->Digest = Digest;
	Recent->bNoted = true;
	Recent->Serial = ++EditorSyncWriteGuardLocal::GSidecarWriteSerial;
	Recent->Expiry = FDateTime::UtcNow() + FTimespan::FromSeconds(5);
	if (Recent->Expected.IsSet())
	{
		Recent->Expected = Digest;
	}
}

uint64 FAssetDocumentEditorSync::GetSidecarWriteSerial()
{
	FScopeLock Lock(&GSidecarSyncGuardLock);
	return EditorSyncWriteGuardLocal::GSidecarWriteSerial;
}

TOptional<FString> FAssetDocumentEditorSync::FindSidecarWriteSince(const FString& FilePath, uint64 Serial)
{
	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	FScopeLock Lock(&GSidecarSyncGuardLock);
	const EditorSyncWriteGuardLocal::FRecentSidecarWrite* Recent = GRecentSidecarWrites.Find(NormalizedPath);
	if (Recent == nullptr || !Recent->bNoted || Recent->Serial <= Serial || !Recent->Digest.IsSet())
	{
		return TOptional<FString>();
	}
	return Recent->Digest;
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
	const FString NormalizedPath = FAssetDocumentSidecarPathUtils::NormalizeFilePath(FilePath);
	FString Written;
	{
		FScopeLock Lock(&GSidecarSyncGuardLock);
		PruneRecentSidecarWritesLocked();

		if (GActiveSidecarSyncDepth > 0 && FilePath.IsEmpty())
		{
			return true;
		}
		if (GActiveSidecarWrites.Contains(NormalizedPath))
		{
			return true;
		}
		const EditorSyncWriteGuardLocal::FRecentSidecarWrite* Recent = GRecentSidecarWrites.Find(NormalizedPath);
		if (Recent == nullptr)
		{
			return false;
		}
		if (!Recent->Digest.IsSet())
		{
			return true;
		}
		Written = Recent->Digest.GetValue();
	}
	// Shortly after an editor write, only that write's content is hidden: an edit made right after it is a change.
	return EditorSyncWriteGuardLocal::DigestOfFile(NormalizedPath) == Written;
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

	// The sidecar and its archive place are both checked before the archive folder is made and the file moved.
	const FString ArchivePath = MakeUniqueArchivePath(SidecarPath);
	FString SidecarRealPath;
	FString ArchiveRealPath;
	FString Error;
	if (!FAssetDocumentSidecar::ResolveSidecarFile(SidecarPath, SidecarRealPath, Error)
		|| !FAssetDocumentSidecar::ResolveSidecarFile(ArchivePath, ArchiveRealPath, Error))
	{
		UE_LOG(LogAssetDocument, Warning, TEXT("Deleted AssetDocument sidecar '%s' not archived: %s"), *SidecarPath, *Error);
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ArchiveRealPath), true);
	FScopedSidecarWrite Guard(TArray<FString>{ SidecarPath, ArchivePath });
	if (!IFileManager::Get().Move(*ArchiveRealPath, *SidecarPath, false, true))
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
