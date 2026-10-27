// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentPathsTestSupport.h"
#include "AssetDocumentSidecar.h"
#include "AssetDocumentSidecarSyncState.h"
#include "AssetDocumentSettings.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SidecarSyncStateTestsLocal
{
const TCHAR* const GoodHash = TEXT("adsh:v1:region:aaaa");

TSharedPtr<FJsonObject> Parse(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
	return Object;
}

TOptional<FAssetDocumentSidecarSyncMeta> ReadMeta(const FString& Sync)
{
	const TSharedPtr<FJsonObject> Document = Parse(FString::Printf(TEXT("{\"Class\":\"C\",\"_meta\":{\"sync\":%s}}"), *Sync));
	return Document.IsValid() ? FAssetDocumentSidecarSyncMeta::Read(*Document) : TOptional<FAssetDocumentSidecarSyncMeta>();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentSidecarSyncMetaReadTest,
	"UEAssetDoc.SidecarSync.State.MetaTrust",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSidecarSyncMetaReadTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncStateTestsLocal;
	const TOptional<FAssetDocumentSidecarSyncMeta> Good = ReadMeta(FString::Printf(
		TEXT("{\"Version\":1,\"Package\":\"p\",\"Regions\":{\"/Properties\":{\"Sidecar\":\"%s\",\"Asset\":\"%s\"}}}"), GoodHash, GoodHash));
	if (!TestTrue(TEXT("well-formed state is read"), Good.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("package hash"), Good->Package, FString(TEXT("p")));
	TestNotNull(TEXT("a region of the current codec is trusted"), Good->FindTrusted(TEXT("/Properties"), TEXT("adsh:v1:region:")));
	TestNull(TEXT("a region hashed by another codec is not"), Good->FindTrusted(TEXT("/Properties"), TEXT("adsh:v2:region:")));
	TestNull(TEXT("an unknown region is not"), Good->FindTrusted(TEXT("/Body/Keys"), TEXT("adsh:v1:region:")));
	TestEqual(TEXT("prefix of a hash"), FAssetDocumentSidecarSyncMeta::HashPrefixOf(GoodHash), FString(TEXT("adsh:v1:region:")));

	TestFalse(TEXT("no _meta: no state"), FAssetDocumentSidecarSyncMeta::Read(*Parse(TEXT("{\"Class\":\"C\"}"))).IsSet());
	TestFalse(TEXT("_meta without sync: no state"), FAssetDocumentSidecarSyncMeta::Read(*Parse(TEXT("{\"_meta\":{\"Skipped\":{}}}"))).IsSet());
	TestFalse(TEXT("another Version: no state"), ReadMeta(TEXT("{\"Version\":2,\"Package\":\"p\",\"Regions\":{}}")).IsSet());
	TestFalse(TEXT("no Version: no state"), ReadMeta(TEXT("{\"Package\":\"p\",\"Regions\":{}}")).IsSet());
	TestFalse(TEXT("Regions not an object: no state"), ReadMeta(TEXT("{\"Version\":1,\"Package\":\"p\",\"Regions\":[]}")).IsSet());
	TestFalse(TEXT("Package not a string: no state"), ReadMeta(TEXT("{\"Version\":1,\"Package\":3,\"Regions\":{}}")).IsSet());
	TestFalse(TEXT("a malformed region: no state"),
		ReadMeta(TEXT("{\"Version\":1,\"Package\":\"p\",\"Regions\":{\"/Properties\":{\"Sidecar\":\"x\"}}}")).IsSet());
	TestFalse(TEXT("sync not an object: no state"), ReadMeta(TEXT("\"stamp\"")).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentSidecarSyncIndexTest,
	"UEAssetDoc.SidecarSync.State.IndexKeysAndRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSidecarSyncIndexTest::RunTest(const FString& Parameters)
{
	const FString Key = FAssetDocumentSidecarSyncIndex::MakeKey(TEXT("/Game/Data/DA_Item"));
	TestEqual(TEXT("an object path is keyed by its package"), FAssetDocumentSidecarSyncIndex::MakeKey(TEXT("/Game/Data/DA_Item.DA_Item")), Key);
	TestEqual(TEXT("keys ignore letter case"), FAssetDocumentSidecarSyncIndex::MakeKey(TEXT("/game/data/da_item")), Key);
	TestEqual(TEXT("backslashes are separators"), FAssetDocumentSidecarSyncIndex::MakeKey(TEXT("\\Game\\Data\\DA_Item")), Key);
	TestEqual(TEXT("surrounding space is ignored"), FAssetDocumentSidecarSyncIndex::MakeKey(TEXT(" /Game/Data/DA_Item ")), Key);
	TestNotEqual(TEXT("other assets differ"), FAssetDocumentSidecarSyncIndex::MakeKey(TEXT("/Game/Data/DA_Item2")), Key);

	const FString File = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("SidecarSyncIndex"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits), TEXT("index.json"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(File), false, true);
	};
	FAssetDocumentSidecarSyncIndex Index(File);
	FAssetDocumentSidecarSyncIndexEntry Entry;
	Entry.FileSize = 1234;
	Entry.FileTimestamp = FDateTime(2026, 1, 2, 3, 4, 5);
	Entry.FileDigest = TEXT("digest");
	Entry.PackageHash = TEXT("package");
	Entry.State = TEXT("skipped");
	Entry.Reason = TEXT("empty");
	Index.Set(TEXT("/Game/Data/DA_Item.DA_Item"), Entry);
	Index.Set(TEXT("/Game/Data/Other"), FAssetDocumentSidecarSyncIndexEntry());
	TestTrue(TEXT("a set marks the index dirty"), Index.IsDirty());
	TestTrue(TEXT("the index is saved"), Index.Save());
	TestFalse(TEXT("saving clears dirty"), Index.IsDirty());

	FAssetDocumentSidecarSyncIndex Loaded(File);
	Loaded.Load();
	const FAssetDocumentSidecarSyncIndexEntry* Read = Loaded.Find(TEXT("/GAME/data/da_item"));
	if (!TestNotNull(TEXT("the entry reads back under any spelling of the package"), Read))
	{
		return false;
	}
	TestEqual(TEXT("file size"), Read->FileSize, int64(1234));
	TestEqual(TEXT("timestamp"), Read->FileTimestamp, Entry.FileTimestamp);
	TestEqual(TEXT("digest"), Read->FileDigest, Entry.FileDigest);
	TestEqual(TEXT("package hash"), Read->PackageHash, Entry.PackageHash);
	TestEqual(TEXT("state"), Read->State, Entry.State);
	TestEqual(TEXT("reason"), Read->Reason, Entry.Reason);
	TestEqual(TEXT("both entries"), Loaded.GetEntries().Num(), 2);
	Loaded.Remove(TEXT("/Game/Data/Other.Other"));
	TestEqual(TEXT("remove by object path"), Loaded.GetEntries().Num(), 1);

	FAssetDocumentSidecarSyncIndex Missing(File + TEXT(".absent"));
	Missing.Load();
	TestEqual(TEXT("a missing index is empty"), Missing.GetEntries().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentSidecarSyncScopeTest,
	"UEAssetDoc.SidecarSync.State.MirrorScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSidecarSyncScopeTest::RunTest(const FString& Parameters)
{
	using namespace UE::AssetDocument::SidecarSync;
	UAssetDocumentSettings* Settings = GetMutableDefault<UAssetDocumentSettings>();
	const TArray<FString> SavedPaths = Settings->MirrorExcludedPaths;
	ON_SCOPE_EXIT
	{
		Settings->MirrorExcludedPaths = SavedPaths;
	};
	Settings->MirrorExcludedPaths = {TEXT("/Game/__ExternalActors__"), TEXT("/Game/Developers/")};
	FString Reason;
	TestTrue(TEXT("a /Game asset is in scope"), IsInMirrorScope(TEXT("/Game/Data/DA_Item"), &Reason));
	TestFalse(TEXT("engine content is not"), IsInMirrorScope(TEXT("/Engine/BasicShapes/Cube"), &Reason));
	TestEqual(TEXT("reason not_game"), Reason, FString(TEXT("not_game")));
	TestFalse(TEXT("an excluded folder is not"), IsInMirrorScope(TEXT("/Game/__ExternalActors__/Map/A/B"), &Reason));
	TestEqual(TEXT("reason excluded_path"), Reason, FString(TEXT("excluded_path")));
	TestFalse(TEXT("a trailing slash in the setting is accepted"), IsInMirrorScope(TEXT("/Game/Developers/Me/DA_X.DA_X")));
	TestTrue(TEXT("a prefix matches whole folders only"), IsInMirrorScope(TEXT("/Game/__ExternalActors__Extra/DA_X")));
	TestEqual(TEXT("package of an object path"), ToPackageName(TEXT("/Game/A/B.B")), FString(TEXT("/Game/A/B")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentSidecarSyncConflictPathTest,
	"UEAssetDoc.SidecarSync.State.ConflictFilePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSidecarSyncConflictPathTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentSidecarSyncTests")));
	const FString Sidecar = FPaths::Combine(Folder, TEXT("DA_Conflict.assetdoc.json"));
	const FString Conflict = FAssetDocumentSidecar::ConflictFilePath(Sidecar);
	TestEqual(TEXT("the conflict file is next to the sidecar"), FPaths::GetPath(Conflict), FPaths::GetPath(Sidecar));
	TestEqual(TEXT("named <AssetName>.assetdoc.asset.json"), FPaths::GetCleanFilename(Conflict), FString(TEXT("DA_Conflict.assetdoc.asset.json")));
	TestFalse(TEXT("which the watcher does not take for a sidecar"), Conflict.EndsWith(TEXT(".assetdoc.json")));
	TestTrue(TEXT("a path that is not a sidecar has none"), FAssetDocumentSidecar::ConflictFilePath(Folder / TEXT("DA_Conflict.json")).IsEmpty());

	const TArray<uint8> Bytes = {'{', '}', '\n'};
	FString Error;
	const FString Outside = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DA_Out.assetdoc.json")));
	TestFalse(TEXT("no conflict file for a sidecar outside Content"), FAssetDocumentSidecar::WriteConflictFile(Outside, Bytes, Error));
	TestFalse(TEXT("no write of a sidecar outside Content"), FAssetDocumentSidecar::WriteBytes(Outside, Bytes, Error));
	TestFalse(TEXT("nothing was written there"), IFileManager::Get().FileExists(*FAssetDocumentSidecar::ConflictFilePath(Outside)));

	const FString Deep = FPaths::Combine(Folder, FGuid::NewGuid().ToString(EGuidFormats::Digits), TEXT("Sub"), TEXT("DA_Conflict.assetdoc.json"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(FPaths::GetPath(Deep)), false, true);
	};
	TestTrue(FString::Printf(TEXT("the conflict file of a sidecar in a new folder is written: %s"), *Error),
		FAssetDocumentSidecar::WriteConflictFile(Deep, Bytes, Error));
	TestTrue(TEXT("in that folder"), IFileManager::Get().FileExists(*FAssetDocumentSidecar::ConflictFilePath(Deep)));
	TestFalse(TEXT("the sidecar itself is not created"), IFileManager::Get().FileExists(*Deep));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentSidecarDigestStaysInContentTest,
	"UEAssetDoc.SidecarSync.State.DigestReadsOnlySidecarsInContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSidecarDigestStaysInContentTest::RunTest(const FString& Parameters)
{
	using namespace UE::AssetDocument::SidecarSync;
	const FString Unique = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentSidecarDigestTests_") + Unique));
	const FString OutsideFolder = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentSidecarDigestTests_") + Unique));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
		IFileManager::Get().DeleteDirectory(*OutsideFolder, false, true);
	};
	const FString Sidecar = FPaths::Combine(Folder, TEXT("DA_Digest.assetdoc.json"));
	const FString Content = TEXT("{\"Properties\":{}}\n");
	if (!TestTrue(TEXT("the sidecar is written"), FFileHelper::SaveStringToFile(Content, *Sidecar, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))
	{
		return false;
	}
	TArray<uint8> Bytes;
	FFileHelper::LoadFileToArray(Bytes, *Sidecar);
	TestEqual(TEXT("a sidecar in Content: the digest of its bytes"), DigestSidecarFile(Sidecar), DigestBytes(Bytes));
	TestTrue(TEXT("no file: empty"), DigestSidecarFile(FPaths::Combine(Folder, TEXT("DA_None.assetdoc.json"))).IsEmpty());

	const FString Outside = FPaths::Combine(OutsideFolder, TEXT("Secret.json"));
	FFileHelper::SaveStringToFile(TEXT("{\"outside\":true}"), *Outside);
	TArray<uint8> OutsideBytes;
	FFileHelper::LoadFileToArray(OutsideBytes, *Outside);
	const FString Linked = FPaths::Combine(Folder, TEXT("DA_Linked.assetdoc.json"));
	if (!AssetDocumentPathsTestSupport::MakeFileLink(Linked, Outside))
	{
		AddInfo(TEXT("file links cannot be made here; the linked sidecar check is skipped"));
		return true;
	}
	const FString LinkedDigest = DigestSidecarFile(Linked);
	TestEqual(TEXT("a sidecar linked out of Content is not read"), LinkedDigest, FString(TEXT("unreadable")));
	TestNotEqual(TEXT("its digest is not that of the outside file"), LinkedDigest, DigestBytes(OutsideBytes));
	return true;
}

#endif
