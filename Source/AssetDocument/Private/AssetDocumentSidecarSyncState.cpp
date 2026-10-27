// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentSidecarSyncState.h"

#include "AssetDocumentAtomicFile.h"
#include "AssetDocumentSidecar.h"
#include "AssetDocumentSettings.h"
#include "AssetDocumentTypes.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/UObjectGlobals.h"

namespace SidecarSyncStateLocal
{
// A string field that is a JSON string (a number is not converted).
bool TryGetStrictString(const FJsonObject& Object, const TCHAR* Field, FString& OutValue)
{
	const TSharedPtr<FJsonValue> Value = Object.TryGetField(Field);
	if (!Value.IsValid() || Value->Type != EJson::String)
	{
		return false;
	}
	OutValue = Value->AsString();
	return true;
}

bool ReadRegionState(const TSharedPtr<FJsonValue>& Value, FAssetDocumentSidecarRegionState& OutState)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	return Value.IsValid() && Value->TryGetObject(Object) && Object && Object->IsValid()
		&& TryGetStrictString(**Object, TEXT("Sidecar"), OutState.Sidecar)
		&& TryGetStrictString(**Object, TEXT("Asset"), OutState.Asset)
		&& !OutState.Sidecar.IsEmpty() && !OutState.Asset.IsEmpty();
}

TOptional<FAssetDocumentSidecarSyncMeta> Untrusted(FString* OutWhy, const TCHAR* Why)
{
	if (OutWhy)
	{
		*OutWhy = Why;
	}
	return {};
}
}

TOptional<FAssetDocumentSidecarSyncMeta> FAssetDocumentSidecarSyncMeta::Read(const FJsonObject& Document, FString* OutWhyUntrusted)
{
	using namespace SidecarSyncStateLocal;
	const TSharedPtr<FJsonObject>* Meta = nullptr;
	if (!Document.TryGetObjectField(AssetDocumentSyncMetaField, Meta) || !Meta || !Meta->IsValid())
	{
		return Untrusted(OutWhyUntrusted, TEXT("the sidecar has no _meta"));
	}
	const TSharedPtr<FJsonObject>* Sync = nullptr;
	if (!(*Meta)->TryGetObjectField(TEXT("sync"), Sync) || !Sync || !Sync->IsValid())
	{
		return Untrusted(OutWhyUntrusted, TEXT("the sidecar has no _meta.sync"));
	}
	int32 Version = 0;
	if (!(*Sync)->TryGetNumberField(TEXT("Version"), Version) || Version != CurrentVersion)
	{
		return Untrusted(OutWhyUntrusted, TEXT("_meta.sync has another Version"));
	}
	FAssetDocumentSidecarSyncMeta Result;
	if (!TryGetStrictString(**Sync, TEXT("Package"), Result.Package))
	{
		return Untrusted(OutWhyUntrusted, TEXT("_meta.sync.Package is not a string"));
	}
	const TSharedPtr<FJsonObject>* Regions = nullptr;
	if (!(*Sync)->TryGetObjectField(TEXT("Regions"), Regions) || !Regions || !Regions->IsValid())
	{
		return Untrusted(OutWhyUntrusted, TEXT("_meta.sync.Regions is not an object"));
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Region : (*Regions)->Values)
	{
		FAssetDocumentSidecarRegionState State;
		if (!ReadRegionState(Region.Value, State))
		{
			return Untrusted(OutWhyUntrusted, TEXT("a _meta.sync region is malformed"));
		}
		Result.Regions.Add(Region.Key, MoveTemp(State));
	}
	return Result;
}

const FAssetDocumentSidecarRegionState* FAssetDocumentSidecarSyncMeta::FindTrusted(const FString& Region, const FString& HashPrefix) const
{
	const FAssetDocumentSidecarRegionState* State = Regions.Find(Region);
	if (State == nullptr || HashPrefix.IsEmpty())
	{
		return nullptr;
	}
	return State->Sidecar.StartsWith(HashPrefix, ESearchCase::CaseSensitive)
		&& State->Asset.StartsWith(HashPrefix, ESearchCase::CaseSensitive) ? State : nullptr;
}

FString FAssetDocumentSidecarSyncMeta::HashPrefixOf(const FString& Hash)
{
	int32 Index = INDEX_NONE;
	return Hash.FindLastChar(TEXT(':'), Index) ? Hash.Left(Index + 1) : FString();
}

FAssetDocumentSidecarSyncIndex::FAssetDocumentSidecarSyncIndex(FString InFilePath)
	: FilePath(MoveTemp(InFilePath))
{
}

FString FAssetDocumentSidecarSyncIndex::GetDefaultFilePath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocument/SidecarSync/index.json")));
}

FString FAssetDocumentSidecarSyncIndex::MakeKey(const FString& PackageOrObjectPath)
{
	// Package names are case-insensitive in the engine, also on case-sensitive file systems.
	return UE::AssetDocument::SidecarSync::ToPackageName(PackageOrObjectPath).ToLower();
}

void FAssetDocumentSidecarSyncIndex::Load()
{
	Entries.Reset();
	bDirty = false;
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		return;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		return;
	}
	int32 Version = 0;
	const TSharedPtr<FJsonObject>* Assets = nullptr;
	if (!Root->TryGetNumberField(TEXT("Version"), Version) || Version != 1
		|| !Root->TryGetObjectField(TEXT("Assets"), Assets) || !Assets || !Assets->IsValid())
	{
		return;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Asset : (*Assets)->Values)
	{
		const TSharedPtr<FJsonObject>* Row = nullptr;
		if (!Asset.Value.IsValid() || !Asset.Value->TryGetObject(Row) || !Row || !Row->IsValid())
		{
			continue;
		}
		FAssetDocumentSidecarSyncIndexEntry Entry;
		double Size = -1.0;
		FString Timestamp;
		(*Row)->TryGetNumberField(TEXT("FileSize"), Size);
		Entry.FileSize = static_cast<int64>(Size);
		(*Row)->TryGetStringField(TEXT("FileTimestamp"), Timestamp);
		FDateTime::ParseIso8601(*Timestamp, Entry.FileTimestamp);
		(*Row)->TryGetStringField(TEXT("FileDigest"), Entry.FileDigest);
		(*Row)->TryGetStringField(TEXT("PackageHash"), Entry.PackageHash);
		(*Row)->TryGetStringField(TEXT("State"), Entry.State);
		(*Row)->TryGetStringField(TEXT("Reason"), Entry.Reason);
		Entries.Add(MakeKey(Asset.Key), MoveTemp(Entry));
	}
}

bool FAssetDocumentSidecarSyncIndex::Save()
{
	const TSharedRef<FJsonObject> Assets = MakeShared<FJsonObject>();
	TArray<FString> Keys;
	Entries.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		const FAssetDocumentSidecarSyncIndexEntry& Entry = Entries[Key];
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("FileSize"), static_cast<double>(Entry.FileSize));
		Row->SetStringField(TEXT("FileTimestamp"), Entry.FileTimestamp.ToIso8601());
		Row->SetStringField(TEXT("FileDigest"), Entry.FileDigest);
		Row->SetStringField(TEXT("PackageHash"), Entry.PackageHash);
		Row->SetStringField(TEXT("State"), Entry.State);
		Row->SetStringField(TEXT("Reason"), Entry.Reason);
		Assets->SetObjectField(Key, Row);
	}
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("Version"), 1);
	Root->SetObjectField(TEXT("Assets"), Assets);
	FString Text;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
	FTCHARToUTF8 Utf8(*Text);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
	FString Error;
	if (!FAssetDocumentAtomicFile::WriteBytesAtomically(FilePath,
			TArrayView64<const uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Error))
	{
		return false;
	}
	bDirty = false;
	return true;
}

const FAssetDocumentSidecarSyncIndexEntry* FAssetDocumentSidecarSyncIndex::Find(const FString& PackageOrObjectPath) const
{
	return Entries.Find(MakeKey(PackageOrObjectPath));
}

void FAssetDocumentSidecarSyncIndex::Set(const FString& PackageOrObjectPath, const FAssetDocumentSidecarSyncIndexEntry& Entry)
{
	Entries.Add(MakeKey(PackageOrObjectPath), Entry);
	bDirty = true;
}

void FAssetDocumentSidecarSyncIndex::Remove(const FString& PackageOrObjectPath)
{
	bDirty |= Entries.Remove(MakeKey(PackageOrObjectPath)) > 0;
}

namespace UE::AssetDocument::SidecarSync
{
FString ToPackageName(const FString& PackageOrObjectPath)
{
	FString Path = PackageOrObjectPath.TrimStartAndEnd();
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));
	int32 Dot = INDEX_NONE;
	if (Path.FindChar(TEXT('.'), Dot))
	{
		Path.LeftInline(Dot);
	}
	return Path;
}

FString GetPackageSavedHash(const FString& PackageName)
{
	// The package file's own summary first: it is what is on disk, whoever wrote it (an editor save, an installed
	// apply, a version control update).
	FString Filename;
	if (FPackageName::IsValidLongPackageName(PackageName) && FPackageName::DoesPackageExist(PackageName, &Filename))
	{
		if (const TUniquePtr<FArchive> Reader = TUniquePtr<FArchive>(IFileManager::Get().CreateFileReader(*Filename, FILEREAD_Silent)))
		{
			FPackageFileSummary Summary;
			*Reader << Summary;
			if (!Reader->IsError() && Summary.Tag == PACKAGE_FILE_TAG && !Summary.GetSavedHash().IsZero())
			{
				return LexToString(Summary.GetSavedHash());
			}
		}
	}
#if WITH_EDITORONLY_DATA
	if (const UPackage* Package = FindPackage(nullptr, *PackageName))
	{
		const FIoHash Hash = Package->GetSavedHash();
		if (!Hash.IsZero())
		{
			return LexToString(Hash);
		}
	}
#endif
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const TOptional<FAssetPackageData> Data = Registry.GetAssetPackageDataCopy(FName(*PackageName));
	if (Data.IsSet() && !Data->GetPackageSavedHash().IsZero())
	{
		return LexToString(Data->GetPackageSavedHash());
	}
	return FString();
}

FString DigestBytes(TConstArrayView<uint8> Bytes)
{
	FSHAHash Hash;
	FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
	return Hash.ToString();
}

int64 GetMaxSidecarDigestBytes()
{
	return FMath::Max<int64>(GetMaxMirrorBytes() * 4, 64 * 1024 * 1024);
}

FString DigestSidecarFile(const FString& FilePath)
{
	if (FilePath.IsEmpty() || !IFileManager::Get().FileExists(*FilePath))
	{
		return FString();
	}
	FString RealPath;
	FString Error;
	if (!FAssetDocumentSidecar::ResolveSidecarFile(FilePath, RealPath, Error))
	{
		return TEXT("unreadable");
	}
	const FFileStatData Stat = IFileManager::Get().GetStatData(*RealPath);
	if (!Stat.bIsValid || Stat.bIsDirectory)
	{
		return TEXT("unreadable");
	}
	if (Stat.FileSize > GetMaxSidecarDigestBytes())
	{
		return FString::Printf(TEXT("size:%lld:%lld"), Stat.FileSize, Stat.ModificationTime.GetTicks());
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *RealPath, FILEREAD_Silent))
	{
		return TEXT("unreadable");
	}
	return DigestBytes(Bytes);
}

bool IsMirroringEnabled()
{
	const UAssetDocumentSettings* Settings = GetDefault<UAssetDocumentSettings>();
	return Settings != nullptr && Settings->bMirrorSidecars && GIsEditor && !IsRunningCommandlet();
}

int64 GetMaxMirrorBytes()
{
	const UAssetDocumentSettings* Settings = GetDefault<UAssetDocumentSettings>();
	return static_cast<int64>(FMath::Max(1, Settings ? Settings->MaxMirrorFileSizeKB : 8192)) * 1024;
}

bool IsInMirrorScope(const FString& PackageOrObjectPath, FString* OutReason)
{
	const FString PackageName = ToPackageName(PackageOrObjectPath);
	auto Exclude = [OutReason](const TCHAR* Reason)
	{
		if (OutReason)
		{
			*OutReason = Reason;
		}
		return false;
	};
	if (!PackageName.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(PackageName))
	{
		return Exclude(TEXT("not_game"));
	}
	// Documents target only ASCII letters, digits, '_', '-' and '/' paths: an asset named otherwise has no sidecar to
	// sync with.
	for (const TCHAR Char : PackageName)
	{
		const bool bAllowed = (Char < 128 && FChar::IsAlnum(Char)) || Char == TEXT('_') || Char == TEXT('-') || Char == TEXT('/');
		if (!bAllowed)
		{
			return Exclude(TEXT("unsupported_name"));
		}
	}
	// The sidecars of deleted assets, archived by the editor; never synced.
	if (PackageName.StartsWith(TEXT("/Game/_DeletedAssetDocs/"), ESearchCase::IgnoreCase))
	{
		return Exclude(TEXT("archive"));
	}
	const UAssetDocumentSettings* Settings = GetDefault<UAssetDocumentSettings>();
	if (Settings == nullptr)
	{
		return true;
	}
	for (FString Prefix : Settings->MirrorExcludedPaths)
	{
		Prefix = Prefix.TrimStartAndEnd();
		while (Prefix.EndsWith(TEXT("/")))
		{
			Prefix.LeftChopInline(1);
		}
		if (!Prefix.IsEmpty() && (PackageName.Equals(Prefix, ESearchCase::IgnoreCase)
			|| PackageName.StartsWith(Prefix + TEXT("/"), ESearchCase::IgnoreCase)))
		{
			return Exclude(TEXT("excluded_path"));
		}
	}
	if (Settings->MirrorExcludedClasses.IsEmpty())
	{
		return true;
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	// In-memory assets too: a new or just-saved asset may not be in the on-disk scan yet.
	Registry.GetAssetsByPackageName(FName(*PackageName), Assets, false);
	for (const FAssetData& Asset : Assets)
	{
		TArray<FTopLevelAssetPath> Classes;
		Registry.GetAncestorClassNames(Asset.AssetClassPath, Classes);
		Classes.Add(Asset.AssetClassPath);
		for (const FSoftClassPath& Excluded : Settings->MirrorExcludedClasses)
		{
			const FString ExcludedPath = Excluded.ToString();
			if (!ExcludedPath.IsEmpty() && Classes.ContainsByPredicate([&ExcludedPath](const FTopLevelAssetPath& Class)
				{
					return Class.ToString().Equals(ExcludedPath, ESearchCase::IgnoreCase);
				}))
			{
				return Exclude(TEXT("excluded_class"));
			}
		}
	}
	return true;
}
}
