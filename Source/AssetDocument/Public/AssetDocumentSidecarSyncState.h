// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// The last-sync state a mirrored sidecar carries in _meta.sync:
//   {"Version": 1, "Package": <saved hash of the package at the last sync>,
//    "Regions": {<region pointer>: {"Sidecar": <hash>, "Asset": <hash>}}}
// Region hashes are the Python semantic hashes of asset_document/sync_evidence.py. A missing or malformed
// _meta.sync, or another Version, means there is no state that can be trusted (the initialization rules apply).
struct ASSETDOCUMENT_API FAssetDocumentSidecarRegionState
{
	FString Sidecar;
	FString Asset;
};

struct ASSETDOCUMENT_API FAssetDocumentSidecarSyncMeta
{
	static constexpr int32 CurrentVersion = 1;

	FString Package;
	TMap<FString, FAssetDocumentSidecarRegionState> Regions;

	// The state in Document's _meta.sync; unset (with the reason) when there is none that can be trusted.
	static TOptional<FAssetDocumentSidecarSyncMeta> Read(const FJsonObject& Document, FString* OutWhyUntrusted = nullptr);
	// A recorded region state is usable only when both hashes come from the current hash codec (HashPrefix, e.g.
	// "adsh:v1:region:"): hashes of another codec cannot be compared.
	const FAssetDocumentSidecarRegionState* FindTrusted(const FString& Region, const FString& HashPrefix) const;
	// "adsh:v1:region:" for "adsh:v1:region:<digest>".
	static FString HashPrefixOf(const FString& Hash);
};

// What this machine knows about each mirrored asset, so that an unchanged sidecar and package need neither Python nor
// an extract at editor start. Kept in Saved/AssetDocument/SidecarSync/index.json; an optimization only: losing it
// costs time, never a wrong sync decision. Keys are package names compared case-insensitively.
struct ASSETDOCUMENT_API FAssetDocumentSidecarSyncIndexEntry
{
	// The sidecar file as the last sync left it (unset for an asset without a mirror).
	int64 FileSize = INDEX_NONE;
	FDateTime FileTimestamp;
	FString FileDigest;
	// The package's saved hash at the last sync.
	FString PackageHash;
	// The last outcome (in_sync, skipped, conflict, ...) and, for a skip or a failure, its code.
	FString State;
	FString Reason;
};

class ASSETDOCUMENT_API FAssetDocumentSidecarSyncIndex
{
public:
	explicit FAssetDocumentSidecarSyncIndex(FString InFilePath);

	static FString GetDefaultFilePath();
	// Package name (an object path's package, case folded).
	static FString MakeKey(const FString& PackageOrObjectPath);

	void Load();
	bool Save();
	bool IsDirty() const { return bDirty; }

	const FAssetDocumentSidecarSyncIndexEntry* Find(const FString& PackageOrObjectPath) const;
	void Set(const FString& PackageOrObjectPath, const FAssetDocumentSidecarSyncIndexEntry& Entry);
	void Remove(const FString& PackageOrObjectPath);
	const TMap<FString, FAssetDocumentSidecarSyncIndexEntry>& GetEntries() const { return Entries; }
	const FString& GetFilePath() const { return FilePath; }

private:
	FString FilePath;
	TMap<FString, FAssetDocumentSidecarSyncIndexEntry> Entries;
	bool bDirty = false;
};

namespace UE::AssetDocument::SidecarSync
{
// The package of a package or object path ("/Game/A/B.B" -> "/Game/A/B").
ASSETDOCUMENT_API FString ToPackageName(const FString& PackageOrObjectPath);
// The package's saved hash (the one the engine records in the package summary): read from the package file, else from
// the loaded package or the Asset Registry; empty when none has one.
ASSETDOCUMENT_API FString GetPackageSavedHash(const FString& PackageName);
// SHA-1 of bytes (hex).
ASSETDOCUMENT_API FString DigestBytes(TConstArrayView<uint8> Bytes);
// The digest of a sidecar file's content, read only through FAssetDocumentSidecar::ResolveSidecarFile and only up to
// GetMaxSidecarDigestBytes: empty when there is no file; "unreadable" when the path does not reach a sidecar inside
// Content or the file cannot be read; "size:<bytes>:<modified ticks>" for a larger file (not read). Two equal digests
// mean the same content, except that a larger file is compared by size and time only.
ASSETDOCUMENT_API FString DigestSidecarFile(const FString& FilePath);
ASSETDOCUMENT_API int64 GetMaxSidecarDigestBytes();
// Whether mirroring is on (UAssetDocumentSettings::bMirrorSidecars) and the editor is interactive.
ASSETDOCUMENT_API bool IsMirroringEnabled();
// Whether the asset is in the mirroring scope: a /Game package that a document can target (ASCII letters, digits, '_',
// '-'), outside MirrorExcludedPaths and the archive of deleted assets' sidecars (/Game/_DeletedAssetDocs), whose asset
// class is not in MirrorExcludedClasses. OutReason names the exclusion ("not_game", "unsupported_name", "archive",
// "excluded_path", "excluded_class").
ASSETDOCUMENT_API bool IsInMirrorScope(const FString& PackageName, FString* OutReason = nullptr);
ASSETDOCUMENT_API int64 GetMaxMirrorBytes();
}
