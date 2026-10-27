// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorSync.h"
#include "AssetDocumentIngressQueue.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentPaths.h"
#include "AssetDocumentPendingNotices.h"
#include "AssetDocumentService.h"
#include "AssetDocumentSettings.h"
#include "AssetDocumentSidecar.h"
#include "AssetDocumentSidecarSync.h"
#include "AssetDocumentSidecarSyncRunner.h"
#include "AssetDocumentSidecarSyncState.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SidecarSyncInterplayTestsLocal
{
using namespace UE::AssetDocument::SidecarSync;

FAssetDocumentService& Service()
{
	return FAssetDocumentModule::Get().GetService();
}

FAssetDocumentSidecarSyncRunner& Runner()
{
	return Service().GetSidecarSyncRunner();
}

// Mirroring on for the test (without the background run turning it on in the editor starts); queued passes run and
// the setting is restored at the end.
struct FScopedMirroringOn
{
	FScopedMirroringOn()
		: bWas(GetDefault<UAssetDocumentSettings>()->bMirrorSidecars)
	{
		GetMutableDefault<UAssetDocumentSettings>()->bMirrorSidecars = true;
	}
	~FScopedMirroringOn()
	{
		Runner().Cancel();
		Runner().DrainForTest();
		GetMutableDefault<UAssetDocumentSettings>()->bMirrorSidecars = bWas;
	}
	bool bWas = false;
};

// A folder of test assets under /Game/AssetDocumentSidecarSyncTests (Sub, when set, is a nested path below the run
// folder), deleted with its files and the run folder's archives at the end.
struct FInterplayFolder
{
	FString RunName;
	FString Root;

	explicit FInterplayFolder(const FString& Sub = FString())
		: RunName(FString::Printf(TEXT("Interplay_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		Root = FString::Printf(TEXT("/Game/AssetDocumentSidecarSyncTests/%s"), *RunName);
		if (!Sub.IsEmpty())
		{
			Root /= Sub;
		}
	}
	~FInterplayFolder()
	{
		Runner().Cancel();
		Runner().DrainForTest();
		const FString RunDirectory = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentSidecarSyncTests"), RunName));
		const FString ArchiveDirectory = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectContentDir(), TEXT("_DeletedAssetDocs"), TEXT("AssetDocumentSidecarSyncTests"), RunName));
		for (const FString& Cleaned : {RunDirectory, ArchiveDirectory})
		{
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *Cleaned, TEXT("*.assetdoc.json"), true, false);
			for (const FString& File : Files)
			{
				FAssetDocumentPendingNotices::Resolve(FAssetDocumentSidecarSync::ConflictNotice, File);
				FAssetDocumentPendingNotices::Resolve(FAssetDocumentSidecarSync::FailedNotice, File);
			}
			IFileManager::Get().DeleteDirectory(*Cleaned, false, true);
		}
	}

	FString Target(const FString& Name) const { return Root / Name; }
	FString ObjectPath(const FString& Name) const { return Target(Name) + TEXT(".") + Name; }
	FString Sidecar(const FString& Name) const { return FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(ObjectPath(Name)); }
	FString PackageFile(const FString& Name) const
	{
		return FPackageName::LongPackageNameToFilename(Target(Name), FPackageName::GetAssetPackageExtension());
	}
};

FString ReadText(const FString& Path)
{
	FString Text;
	FFileHelper::LoadFileToString(Text, *Path);
	return Text;
}

bool ReadBytes(const FString& Path, TArray<uint8>& OutBytes)
{
	return FFileHelper::LoadFileToArray(OutBytes, *Path);
}

FString ReadMetaPackage(const FString& SidecarPath)
{
	TSharedPtr<FJsonObject> Document;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ReadText(SidecarPath)), Document);
	const TSharedPtr<FJsonObject>* Meta = nullptr;
	const TSharedPtr<FJsonObject>* Sync = nullptr;
	FString Package;
	if (Document.IsValid() && Document->TryGetObjectField(TEXT("_meta"), Meta) && (*Meta)->TryGetObjectField(TEXT("sync"), Sync))
	{
		(*Sync)->TryGetStringField(TEXT("Package"), Package);
	}
	return Package;
}

FString IndexState(const FString& PackageName)
{
	const FAssetDocumentSidecarSyncIndexEntry* Entry = Service().GetSidecarSync().GetIndex().Find(PackageName);
	return Entry ? Entry->State : FString();
}

FAssetDocumentSidecarSyncOutcome RunSync(const FString& PackageName, EAssetDocumentSidecarSyncTrigger Trigger)
{
	FAssetDocumentSidecarSyncRequest Request;
	Request.PackageName = PackageName;
	Request.Trigger = Trigger;
	return Service().GetSidecarSync().Sync(Request);
}

FString Describe(const FAssetDocumentSidecarSyncOutcome& Outcome)
{
	return FString::Printf(TEXT("state=%s code=%s reason=%s message=%s"), *Outcome.State, *Outcome.Code, *Outcome.Reason, *Outcome.Message);
}

UObject* LoadAsset(const FInterplayFolder& Folder, const FString& Name)
{
	return StaticLoadObject(UObject::StaticClass(), nullptr, *Folder.ObjectPath(Name));
}

FString Value(UObject* Object)
{
	const FProperty* Found = Object ? Object->GetClass()->FindPropertyByName(TEXT("TestString")) : nullptr;
	FString Text;
	if (Found)
	{
		Found->ExportTextItem_Direct(Text, Found->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
	}
	return Text;
}

bool SavePackageOf(UObject* Object, const FString& PackageFile)
{
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Object->GetOutermost(), Object, *PackageFile, Args);
}

bool SetAndSave(const FInterplayFolder& Folder, const FString& Name, const TCHAR* NewValue)
{
	UObject* Object = LoadAsset(Folder, Name);
	FProperty* Found = Object ? Object->GetClass()->FindPropertyByName(TEXT("TestString")) : nullptr;
	if (!Found)
	{
		return false;
	}
	Found->ImportText_Direct(NewValue, Found->ContainerPtrToValuePtr<void>(Object), Object, PPF_None);
	Object->MarkPackageDirty();
	return SavePackageOf(Object, Folder.PackageFile(Name));
}

// A TestDataAsset with TestString = Value, created without a pass, then mirrored ("wrote").
bool CreateMirrored(FAutomationTestBase& Test, const FInterplayFolder& Folder, const FString& Name, const FString& Value)
{
	FString Escaped = Value.ReplaceCharWithEscapedChar();
	const FString Document = FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",")
		TEXT("\"Class\":\"/Script/AssetDocumentTests.TestDataAsset\",\"Action\":\"CreateOrUpdate\",\"Properties\":{\"TestString\":\"%s\"}}"),
		*Folder.Target(Name), *Escaped);
	const FString SidecarPath = Folder.Sidecar(Name);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true);
	if (!Test.TestTrue(TEXT("setup sidecar written"), FFileHelper::SaveStringToFile(Document, *SidecarPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))
	{
		return false;
	}
	UAssetDocumentSettings* Settings = GetMutableDefault<UAssetDocumentSettings>();
	const bool bMirroring = Settings->bMirrorSidecars;
	Settings->bMirrorSidecars = false;
	FAssetDocumentApplyFileRequest Apply;
	Apply.FilePath = SidecarPath;
	Apply.bBypassSidecarSync = true;
	const FAssetDocumentResult Result = Service().ApplyFile(Apply);
	Settings->bMirrorSidecars = bMirroring;
	IFileManager::Get().Delete(*SidecarPath, false, true);
	if (!Test.TestTrue(FString::Printf(TEXT("%s created: %s"), *Name, *Result.Message), Result.IsSuccess()))
	{
		return false;
	}
	const FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Folder.Target(Name), EAssetDocumentSidecarSyncTrigger::Materialize);
	return Test.TestEqual(FString::Printf(TEXT("%s mirrored (%s)"), *Name, *Describe(Outcome)), Outcome.State, FString(TEXT("wrote")));
}

// The mirror's text with TestString's value replaced, written as an outside edit would.
bool EditMirror(const FString& SidecarPath, const FString& From, const FString& To)
{
	const FString Text = ReadText(SidecarPath);
	if (!Text.Contains(From))
	{
		return false;
	}
	return FFileHelper::SaveStringToFile(Text.Replace(*From, *To), *SidecarPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool HasCarriageReturn(const FString& Path)
{
	TArray<uint8> Bytes;
	return ReadBytes(Path, Bytes) && Bytes.Contains(static_cast<uint8>('\r'));
}

bool HasOpenNotice(const FString& File)
{
	for (const FAssetDocumentPendingNotice& Notice : FAssetDocumentPendingNotices::GetOpen())
	{
		if (Notice.File.Equals(File, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncInterplayRenameDuplicateTest,
	"UEAssetDoc.SidecarSync.Interplay.RenameAndDuplicateRestamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncInterplayRenameDuplicateTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncInterplayTestsLocal;
	const FScopedMirroringOn Mirroring;
	const FInterplayFolder Folder;
	if (!CreateMirrored(*this, Folder, TEXT("DA_Source"), TEXT("kept")) || !CreateMirrored(*this, Folder, TEXT("DA_Old"), TEXT("moved")))
	{
		return false;
	}

	// Duplicate: EditorSync copies the sidecar (Target retargeted, _meta kept), then the new package is saved.
	UObject* Source = LoadAsset(Folder, TEXT("DA_Source"));
	UPackage* CopyPackage = CreatePackage(*Folder.Target(TEXT("DA_Copied")));
	UObject* Copy = DuplicateObject<UObject>(Source, CopyPackage, TEXT("DA_Copied"));
	Copy->SetFlags(RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(Copy);
	TestTrue(TEXT("the sidecar is copied"), FAssetDocumentEditorSync::SyncSidecarForDuplicate(Folder.ObjectPath(TEXT("DA_Source")), Folder.ObjectPath(TEXT("DA_Copied"))));
	TestTrue(TEXT("the copy is saved"), SavePackageOf(Copy, Folder.PackageFile(TEXT("DA_Copied"))));
	TestTrue(TEXT("its save queues a pass"), Runner().IsQueued(Folder.Target(TEXT("DA_Copied"))));
	Runner().DrainForTest();
	TestEqual(TEXT("the copy's pass finds nothing to sync"), IndexState(Folder.Target(TEXT("DA_Copied"))), FString(TEXT("in_sync")));
	TestEqual(TEXT("and restamps its package"), ReadMetaPackage(Folder.Sidecar(TEXT("DA_Copied"))), GetPackageSavedHash(Folder.Target(TEXT("DA_Copied"))));
	TestFalse(TEXT("no conflict file"), IFileManager::Get().FileExists(*FAssetDocumentSidecar::ConflictFilePath(Folder.Sidecar(TEXT("DA_Copied")))));
	TestFalse(TEXT("the restamped mirror is LF only"), HasCarriageReturn(Folder.Sidecar(TEXT("DA_Copied"))));
	TestEqual(TEXT("the source stays in sync"), RunSync(Folder.Target(TEXT("DA_Source")), EAssetDocumentSidecarSyncTrigger::Save).State, FString(TEXT("in_sync")));

	// Rename: EditorSync moves the sidecar, then the renamed package is saved.
	UObject* Old = LoadAsset(Folder, TEXT("DA_Old"));
	UPackage* NewPackage = CreatePackage(*Folder.Target(TEXT("DA_New")));
	TestTrue(TEXT("renamed"), Old->Rename(TEXT("DA_New"), NewPackage, REN_DontCreateRedirectors | REN_NonTransactional));
	TestTrue(TEXT("the sidecar is moved"), FAssetDocumentEditorSync::SyncSidecarForRenameOrMove(Folder.ObjectPath(TEXT("DA_Old")), Folder.ObjectPath(TEXT("DA_New"))));
	IFileManager::Get().Delete(*Folder.PackageFile(TEXT("DA_Old")), false, true);
	TestTrue(TEXT("the renamed asset is saved"), SavePackageOf(Old, Folder.PackageFile(TEXT("DA_New"))));
	Runner().DrainForTest();
	TestFalse(TEXT("the old sidecar is gone"), IFileManager::Get().FileExists(*Folder.Sidecar(TEXT("DA_Old"))));
	TestEqual(TEXT("the renamed asset's pass finds nothing to sync"), IndexState(Folder.Target(TEXT("DA_New"))), FString(TEXT("in_sync")));
	TestEqual(TEXT("and restamps its package"), ReadMetaPackage(Folder.Sidecar(TEXT("DA_New"))), GetPackageSavedHash(Folder.Target(TEXT("DA_New"))));
	TestTrue(TEXT("the mirror names the new target"), ReadText(Folder.Sidecar(TEXT("DA_New"))).Contains(TEXT("\"") + Folder.Target(TEXT("DA_New")) + TEXT("\"")));
	TestFalse(TEXT("no notice for the moved sidecar"), HasOpenNotice(Folder.Sidecar(TEXT("DA_New"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncInterplayDeleteTest,
	"UEAssetDoc.SidecarSync.Interplay.DeleteArchivesAndSidecarReturns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncInterplayDeleteTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncInterplayTestsLocal;
	const FScopedMirroringOn Mirroring;
	const FInterplayFolder Folder;
	if (!CreateMirrored(*this, Folder, TEXT("DA_Deleted"), TEXT("gone")) || !CreateMirrored(*this, Folder, TEXT("DA_Kept"), TEXT("first")))
	{
		return false;
	}

	// A deleted asset's sidecar is archived under _DeletedAssetDocs, which the sync leaves alone.
	FString ArchivePath;
	TestTrue(TEXT("archived"), FAssetDocumentEditorSync::ArchiveSidecarForDeletedAsset(Folder.ObjectPath(TEXT("DA_Deleted")), &ArchivePath));
	IFileManager::Get().Delete(*Folder.PackageFile(TEXT("DA_Deleted")), false, true);
	const FString ArchivedPackage = ToPackageName(FAssetDocumentSidecar::ResolveObjectPathFromSidecar(ArchivePath));
	FString Reason;
	TestFalse(TEXT("an archived sidecar is out of scope"), IsInMirrorScope(ArchivedPackage, &Reason));
	TestEqual(TEXT("as an archive"), Reason, FString(TEXT("archive")));
	const FString ArchiveRoot = FString::Printf(TEXT("/Game/_DeletedAssetDocs/AssetDocumentSidecarSyncTests/%s"), *Folder.RunName);
	TestEqual(TEXT("a reconcile of the archive has nothing to do"), Runner().ReconcileNowForTest(ArchiveRoot), 0);
	Runner().DrainForTest();
	TestFalse(TEXT("no notice for the archive"), HasOpenNotice(ArchivePath));
	TestFalse(TEXT("the archive is unchanged"), ReadText(ArchivePath).IsEmpty());

	// Deleting a sidecar leaves the asset; the next save writes the mirror again.
	IFileManager::Get().Delete(*Folder.Sidecar(TEXT("DA_Kept")), false, true);
	TestTrue(TEXT("the asset is kept"), LoadAsset(Folder, TEXT("DA_Kept")) != nullptr);
	TestTrue(TEXT("saved"), SetAndSave(Folder, TEXT("DA_Kept"), TEXT("second")));
	Runner().DrainForTest();
	TestTrue(TEXT("the mirror is back with the saved value"), ReadText(Folder.Sidecar(TEXT("DA_Kept"))).Contains(TEXT("\"second\"")));
	TestEqual(TEXT("and in sync"), RunSync(Folder.Target(TEXT("DA_Kept")), EAssetDocumentSidecarSyncTrigger::Save).State, FString(TEXT("in_sync")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPathsDeepFolderTest,
	"UEAssetDoc.SidecarSync.Paths.DeepFolders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPathsDeepFolderTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncInterplayTestsLocal;
	const FScopedMirroringOn Mirroring;
	// Nested folders bringing the conflict file's full path (the longest file the sync writes) to 255 characters,
	// just under the Windows path limit.
	const FInterplayFolder Probe;
	const int32 BaseLength = FPaths::ConvertRelativePathToFull(
		FAssetDocumentSidecar::ConflictFilePath(Probe.Sidecar(TEXT("DA_Deep")))).Len();
	FString Sub;
	int32 Missing = 255 - BaseLength;
	for (int32 Index = 0; Missing > 1; ++Index)
	{
		const int32 SegmentLength = FMath::Min(Missing - 1, 24);
		FString Segment = FString::Printf(TEXT("Deep%d_"), Index);
		while (Segment.Len() < SegmentLength)
		{
			Segment.AppendChar(TEXT('d'));
		}
		Segment.LeftInline(SegmentLength);
		Sub = Sub.IsEmpty() ? Segment : Sub / Segment;
		Missing -= SegmentLength + 1;
	}
	const FInterplayFolder Folder(Sub);
	const FString ConflictPath = FPaths::ConvertRelativePathToFull(FAssetDocumentSidecar::ConflictFilePath(Folder.Sidecar(TEXT("DA_Deep"))));
	AddInfo(FString::Printf(TEXT("conflict file path: %d characters"), ConflictPath.Len()));
	TestTrue(TEXT("the path is near the limit"), ConflictPath.Len() >= 245 && ConflictPath.Len() <= 259);
	if (!CreateMirrored(*this, Folder, TEXT("DA_Deep"), TEXT("deep")))
	{
		return false;
	}
	TestTrue(TEXT("the sidecar is edited"), EditMirror(Folder.Sidecar(TEXT("DA_Deep")), TEXT("\"deep\""), TEXT("\"deeper\"")));
	FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Folder.Target(TEXT("DA_Deep")), EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	TestEqual(FString::Printf(TEXT("the edit is applied (%s)"), *Describe(Outcome)), Outcome.State, FString(TEXT("applied")));
	TestEqual(TEXT("to the asset"), Value(LoadAsset(Folder, TEXT("DA_Deep"))), FString(TEXT("deeper")));

	// Both sides changed: the conflict file is written next to the sidecar.
	TestTrue(TEXT("the asset is changed"), SetAndSave(Folder, TEXT("DA_Deep"), TEXT("from_asset")));
	Runner().Cancel();
	TestTrue(TEXT("the sidecar is changed"), EditMirror(Folder.Sidecar(TEXT("DA_Deep")), TEXT("\"deeper\""), TEXT("\"from_file\"")));
	Outcome = RunSync(Folder.Target(TEXT("DA_Deep")), EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	TestEqual(FString::Printf(TEXT("a conflict (%s)"), *Describe(Outcome)), Outcome.State, FString(TEXT("conflict")));
	TestTrue(TEXT("its file is written at the long path"), IFileManager::Get().FileExists(*ConflictPath));
	TestFalse(TEXT("LF only"), HasCarriageReturn(ConflictPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPathsNamesTest,
	"UEAssetDoc.SidecarSync.Paths.CaseNonAsciiAndLineEndings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPathsNamesTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncInterplayTestsLocal;
	const FScopedMirroringOn Mirroring;
	const FInterplayFolder Folder(TEXT("Mixed_Case-Folder"));
	const FString Name = TEXT("DA_Values");
	const FString Initial = TEXT("a b \u540D\u524D \u00E9t\u00E9 \u00DCbung");
	if (!CreateMirrored(*this, Folder, Name, Initial))
	{
		return false;
	}
	const FString SidecarPath = Folder.Sidecar(Name);
	TestTrue(TEXT("the mirror holds the value as UTF-8 text"), ReadText(SidecarPath).Contains(Initial));
	TestFalse(TEXT("the mirror is LF only"), HasCarriageReturn(SidecarPath));
	TArray<uint8> Bytes;
	TestTrue(TEXT("no byte order mark"), ReadBytes(SidecarPath, Bytes) && !(Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF));

	const FString Edited = TEXT("x y \u5909\u66F4");
	TestTrue(TEXT("the sidecar is edited"), EditMirror(SidecarPath, Initial, Edited));
	FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Folder.Target(Name), EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	TestEqual(FString::Printf(TEXT("the edit is applied (%s)"), *Describe(Outcome)), Outcome.State, FString(TEXT("applied")));
	TestEqual(TEXT("to the asset"), Value(LoadAsset(Folder, Name)), Edited);

	// A path differing only in case names the same asset, sidecar and index entry.
	const FString OtherCase = TEXT("/Game") + Folder.Target(Name).Mid(5).ToLower();
	Outcome = RunSync(OtherCase, EAssetDocumentSidecarSyncTrigger::Save);
	TestEqual(FString::Printf(TEXT("the other case is the same, synced asset (%s)"), *Describe(Outcome)), Outcome.State, FString(TEXT("in_sync")));
	TestEqual(TEXT("one index entry for both"), Service().GetSidecarSync().GetIndex().Find(OtherCase), Service().GetSidecarSync().GetIndex().Find(Folder.Target(Name)));
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(FPaths::GetPath(SidecarPath) / TEXT("*.assetdoc.json")), true, false);
	TestEqual(TEXT("one sidecar file"), Files.Num(), 1);

	// Names a document cannot target are outside the mirroring scope: non-ASCII names, and spaces (not valid in a
	// package name at all).
	FString Reason;
	TestFalse(TEXT("a non-ASCII name is out of scope"), IsInMirrorScope(Folder.Root / TEXT("DA_\u540D\u524D_\u00E9t\u00E9"), &Reason));
	TestEqual(TEXT("as an unsupported name"), Reason, FString(TEXT("unsupported_name")));
	TestFalse(TEXT("a name with a space is out of scope"), IsInMirrorScope(Folder.Root / TEXT("DA With Space"), &Reason));
	TestEqual(TEXT("as not a /Game package"), Reason, FString(TEXT("not_game")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPathsPrecomposedTest,
	"UEAssetDoc.SidecarSync.Paths.AccentedDirectoriesResolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPathsPrecomposedTest::RunTest(const FString& Parameters)
{
	// A directory with accented characters (a project under such a folder, say): its resolved path compares equal to
	// the path as the engine and documents write it, so files under it are found inside it.
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
		FString::Printf(TEXT("AssetDocumentPathsAccents_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	const FString Accented = Root / TEXT("\u00DCbung_\u00E9t\u00E9_\u540D\u524D");
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	};
	TestTrue(TEXT("directory made"), IFileManager::Get().MakeDirectory(*Accented, true));
	const FString File = Accented / TEXT("DA_X.assetdoc.json");
	TestTrue(TEXT("file written"), FFileHelper::SaveStringToFile(TEXT("{}"), *File));
	FString Normalized = Accented;
	FPaths::NormalizeDirectoryName(Normalized);
	TestEqual(TEXT("the resolved directory is the written path"), AssetDocumentPaths::ResolveRealPath(Accented), Normalized);
	FString Relative;
	TestTrue(TEXT("a file under it is within it"), AssetDocumentPaths::MakeRelativeWithin(Accented, File, Relative));
	TestEqual(TEXT("relative name"), Relative, FString(TEXT("DA_X.assetdoc.json")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncInterplayWatcherQueuesTest,
	"UEAssetDoc.SidecarSync.Interplay.WatcherEventsAndRescanQueuePasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncInterplayWatcherQueuesTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncInterplayTestsLocal;
	const FScopedMirroringOn Mirroring;
	const FInterplayFolder Folder;
	if (!CreateMirrored(*this, Folder, TEXT("DA_Changed"), TEXT("before")) || !CreateMirrored(*this, Folder, TEXT("DA_Same"), TEXT("same"))
		|| !CreateMirrored(*this, Folder, TEXT("DA_Event"), TEXT("before")))
	{
		return false;
	}
	Runner().DrainForTest();

	// A rescan of the folder (the watcher lost track of single changes, e.g. after a checkout).
	TestTrue(TEXT("one sidecar is edited"), EditMirror(Folder.Sidecar(TEXT("DA_Changed")), TEXT("before"), TEXT("after")));
	const int32 Extracts = FAssetDocumentSidecarSync::GetExtractCountForTest();
	FAssetDocumentIngressEvent Rescan;
	Rescan.Type = EAssetDocumentIngressEventType::SidecarRescanRequested;
	Rescan.PrimaryPath = FPaths::GetPath(Folder.Sidecar(TEXT("DA_Changed")));
	TestTrue(TEXT("the rescan is handled"), Service().HandleIngressEventForTest(Rescan).IsSuccess());
	TestEqual(TEXT("it runs no pass itself"), FAssetDocumentSidecarSync::GetExtractCountForTest(), Extracts);
	TestEqual(TEXT("nothing is applied yet"), Value(LoadAsset(Folder, TEXT("DA_Changed"))), FString(TEXT("before")));
	TestTrue(TEXT("the changed sidecar is queued"), Runner().IsQueued(Folder.Target(TEXT("DA_Changed"))));
	TestTrue(TEXT("so is every other sidecar of the folder"), Runner().IsQueued(Folder.Target(TEXT("DA_Same"))));
	const int32 FastPaths = FAssetDocumentSidecarSyncRunner::GetFastPathCountForTest();
	Runner().DrainForTest();
	TestEqual(TEXT("the runner applies the change"), Value(LoadAsset(Folder, TEXT("DA_Changed"))), FString(TEXT("after")));
	TestTrue(TEXT("and answers the unchanged ones from the index"), FAssetDocumentSidecarSyncRunner::GetFastPathCountForTest() - FastPaths >= 2);

	// A single changed sidecar.
	TestTrue(TEXT("another sidecar is edited"), EditMirror(Folder.Sidecar(TEXT("DA_Event")), TEXT("before"), TEXT("after")));
	FAssetDocumentIngressEvent Changed;
	Changed.Type = EAssetDocumentIngressEventType::SidecarChanged;
	Changed.PrimaryPath = Folder.Sidecar(TEXT("DA_Event"));
	TestTrue(TEXT("the change is handled"), Service().HandleIngressEventForTest(Changed).IsSuccess());
	TestEqual(TEXT("it is not applied in the event"), Value(LoadAsset(Folder, TEXT("DA_Event"))), FString(TEXT("before")));
	TestTrue(TEXT("but queued"), Runner().IsQueued(Folder.Target(TEXT("DA_Event"))));
	Runner().DrainForTest();
	TestEqual(TEXT("the runner applies it"), Value(LoadAsset(Folder, TEXT("DA_Event"))), FString(TEXT("after")));
	TestEqual(TEXT("and the pair is in sync"), IndexState(Folder.Target(TEXT("DA_Event"))), FString(TEXT("applied")));
	return true;
}

#endif
