// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorSync.h"

#include "AssetDocumentIngressQueue.h"
#include "AssetDocumentService.h"
#include "AssetDocumentSidecar.h"
#include "AssetDocumentSidecarSyncState.h"

#include "Async/TaskGraphInterfaces.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
FString MakeUniqueSyncTestRoot()
{
	FString TestRoot = FPaths::Combine(
		FPaths::ProjectContentDir(),
		FString::Printf(TEXT("AssetDocumentEditorSyncTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FPaths::NormalizeFilename(TestRoot);
	return TestRoot;
}

FString MakeTargetFromContentPath(const FString& ContentRelativePath)
{
	return TEXT("/Game/") + ContentRelativePath;
}

TSharedPtr<FJsonObject> MakeSidecarDocument(const FString& Target)
{
	TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("SchemaVersion"), 1);
	Document->SetStringField(TEXT("AssetType"), TEXT("GenericAsset"));
	Document->SetStringField(TEXT("Target"), Target);
	return Document;
}

bool WriteSidecarForTarget(const FString& Target)
{
	const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true);

	FString Error;
	return FAssetDocumentSidecar::WriteJsonFile(SidecarPath, MakeSidecarDocument(Target), Error);
}

FString ReadSidecarTarget(const FString& SidecarPath)
{
	TSharedPtr<FJsonObject> Document;
	FString Error;
	if (!FAssetDocumentSidecar::LoadJsonFile(SidecarPath, Document, Error) || !Document.IsValid())
	{
		return FString();
	}

	FString Target;
	Document->TryGetStringField(TEXT("Target"), Target);
	return Target;
}

void DeleteDirectoryTree(const FString& Directory)
{
	if (!Directory.IsEmpty())
	{
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorSyncRenameMoveTest,
	"UEAssetDoc.EditorSync.RenameMove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorSyncRenameMoveTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueSyncTestRoot();
	const FString TestFolderName = FPaths::GetCleanFilename(TestRoot);
	const FString OldTarget = MakeTargetFromContentPath(TestFolderName / TEXT("OldFolder/DA_Source"));
	const FString NewTarget = MakeTargetFromContentPath(TestFolderName / TEXT("NewFolder/DA_Renamed"));
	const FString OldSidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(OldTarget);
	const FString NewSidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(NewTarget);

	ON_SCOPE_EXIT
	{
		DeleteDirectoryTree(TestRoot);
	};

	TestTrue(TEXT("Writes source sidecar"), WriteSidecarForTarget(OldTarget));

	FAssetDocumentEditorSync Sync;
	TestTrue(TEXT("Syncs rename/move sidecar"), Sync.SyncSidecarForRenameOrMove(OldTarget, NewTarget));
	TestFalse(TEXT("Old sidecar is removed"), IFileManager::Get().FileExists(*OldSidecarPath));
	TestTrue(TEXT("New sidecar exists"), IFileManager::Get().FileExists(*NewSidecarPath));
	TestEqual(TEXT("New sidecar Target is retargeted"), ReadSidecarTarget(NewSidecarPath), NewTarget);
	FString ValidationError;
	TestTrue(TEXT("New sidecar validates"), FAssetDocumentSidecar::ValidateTargetMatchesSidecar(NewSidecarPath, MakeSidecarDocument(NewTarget), ValidationError));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorSyncDuplicateTest,
	"UEAssetDoc.EditorSync.Duplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorSyncDuplicateTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueSyncTestRoot();
	const FString TestFolderName = FPaths::GetCleanFilename(TestRoot);
	const FString SourceTarget = MakeTargetFromContentPath(TestFolderName / TEXT("DA_Source"));
	const FString NewTarget = MakeTargetFromContentPath(TestFolderName / TEXT("DA_Source_Copy2"));
	const FString SourceSidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(SourceTarget);
	const FString NewSidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(NewTarget);

	ON_SCOPE_EXIT
	{
		DeleteDirectoryTree(TestRoot);
	};

	TestEqual(TEXT("Infers duplicate source from _CopyN name"), FAssetDocumentEditorSync::InferDuplicateSourceObjectPath(NewTarget), SourceTarget);
	TestTrue(TEXT("Writes source sidecar"), WriteSidecarForTarget(SourceTarget));

	FAssetDocumentEditorSync Sync;
	TestTrue(TEXT("Copies duplicate sidecar"), Sync.SyncSidecarForDuplicate(SourceTarget, NewTarget));
	TestTrue(TEXT("Source sidecar remains"), IFileManager::Get().FileExists(*SourceSidecarPath));
	TestTrue(TEXT("Duplicate sidecar exists"), IFileManager::Get().FileExists(*NewSidecarPath));
	TestEqual(TEXT("Duplicate sidecar Target is retargeted"), ReadSidecarTarget(NewSidecarPath), NewTarget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorSyncDeleteArchiveTest,
	"UEAssetDoc.EditorSync.DeleteArchive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorSyncDeleteArchiveTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueSyncTestRoot();
	const FString TestFolderName = FPaths::GetCleanFilename(TestRoot);
	const FString Target = MakeTargetFromContentPath(TestFolderName / TEXT("DA_DeleteMe"));
	const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
	FString ArchivePath;

	ON_SCOPE_EXIT
	{
		DeleteDirectoryTree(TestRoot);
		DeleteDirectoryTree(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("_DeletedAssetDocs"), TestFolderName));
	};

	TestTrue(TEXT("Writes source sidecar"), WriteSidecarForTarget(Target));

	FAssetDocumentEditorSync Sync;
	TestTrue(TEXT("Archives deleted sidecar"), Sync.ArchiveSidecarForDeletedAsset(Target, &ArchivePath));
	TestFalse(TEXT("Original sidecar is removed"), IFileManager::Get().FileExists(*SidecarPath));
	TestTrue(TEXT("Archive sidecar exists"), IFileManager::Get().FileExists(*ArchivePath));
	TestTrue(TEXT("Archive is under _DeletedAssetDocs"), ArchivePath.Contains(TEXT("/_DeletedAssetDocs/")));
	TestEqual(TEXT("Archived Target remains original target"), ReadSidecarTarget(ArchivePath), Target);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorSyncAssetRemovedAdmissionSuppressionTest,
	"UEAssetDoc.EditorSync.AssetRemovedAdmissionSuppression",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorSyncAssetRemovedAdmissionSuppressionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString TestRoot = MakeUniqueSyncTestRoot();
	const FString TestFolderName = FPaths::GetCleanFilename(TestRoot);
	const FString SuppressedTarget =
		MakeTargetFromContentPath(TestFolderName / TEXT("DA_SuppressedRemove"));
	const FString OrdinaryTarget =
		MakeTargetFromContentPath(TestFolderName / TEXT("DA_OrdinaryRemove"));
	const FString SuppressedSidecarPath =
		FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(SuppressedTarget);
	const FString OrdinarySidecarPath =
		FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(OrdinaryTarget);
	const FString ArchiveDirectory = FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("_DeletedAssetDocs"),
		TestFolderName);

	ON_SCOPE_EXIT
	{
		DeleteDirectoryTree(TestRoot);
		DeleteDirectoryTree(ArchiveDirectory);
	};

	TestTrue(
		TEXT("Writes sidecar protected by the active apply window"),
		WriteSidecarForTarget(SuppressedTarget));
	TestTrue(
		TEXT("Writes ordinary user-delete sidecar"),
		WriteSidecarForTarget(OrdinaryTarget));

	FAssetDocumentIngressEvent SuppressedRemove;
	SuppressedRemove.Type = EAssetDocumentIngressEventType::AssetRemoved;
	SuppressedRemove.PrimaryPath = SuppressedTarget;
	FAssetDocumentIngressEvent OrdinaryRemove;
	OrdinaryRemove.Type = EAssetDocumentIngressEventType::AssetRemoved;
	OrdinaryRemove.PrimaryPath = OrdinaryTarget;
	TSharedRef<FAssetDocumentService> Service =
		MakeShared<FAssetDocumentService>();
	TSharedRef<FAssetDocumentIngressQueue> Ingress =
		MakeShared<FAssetDocumentIngressQueue>(Service);

	{
		FAssetDocumentEditorSync::FScopedSidecarWrite Guard(
			SuppressedSidecarPath);
		Ingress->Enqueue(SuppressedRemove);
		Ingress->Enqueue(OrdinaryRemove);
	}
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(
		ENamedThreads::GameThread);

	TestTrue(
		TEXT("Queued remove keeps the sidecar after the active window closes"),
		IFileManager::Get().FileExists(*SuppressedSidecarPath));
	TestFalse(
		TEXT("An unrelated queued remove is not globally suppressed"),
		IFileManager::Get().FileExists(*OrdinarySidecarPath));
	TArray<FString> ArchivedSidecars;
	IFileManager::Get().FindFiles(
		ArchivedSidecars,
		*FPaths::Combine(ArchiveDirectory, TEXT("*")),
		true,
		false);
	TestEqual(
		TEXT("Only the ordinary queued deletion creates an archive"),
		ArchivedSidecars.Num(),
		1);
	TestTrue(
		TEXT("The queued archive belongs to the ordinary target"),
		ArchivedSidecars.Num() == 1
			&& ArchivedSidecars[0].StartsWith(TEXT("DA_OrdinaryRemove")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorSyncSuppressionFollowsContentTest,
	"UEAssetDoc.EditorSync.SuppressionFollowsContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorSyncSuppressionFollowsContentTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueSyncTestRoot();
	const FString SidecarPath = FPaths::Combine(TestRoot, TEXT("DA_Suppressed.assetdoc.json"));
	ON_SCOPE_EXIT
	{
		DeleteDirectoryTree(TestRoot);
	};
	IFileManager::Get().MakeDirectory(*TestRoot, true);
	{
		FAssetDocumentEditorSync::FScopedSidecarWrite Guard(SidecarPath);
		FFileHelper::SaveStringToFile(TEXT("{\"written\":\"by the editor\"}"), *SidecarPath);
	}
	TestTrue(TEXT("the editor's own write is not seen as a change"), FAssetDocumentEditorSync::IsSidecarWriteSuppressed(SidecarPath));
	// Someone edits the file right after: that edit is a change, however soon it comes.
	FFileHelper::SaveStringToFile(TEXT("{\"written\":\"by someone else\"}"), *SidecarPath);
	TestFalse(TEXT("a different content right after is a change"), FAssetDocumentEditorSync::IsSidecarWriteSuppressed(SidecarPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorSyncOwnWriteNotOtherTest,
	"UEAssetDoc.EditorSync.WriteScopeHidesOnlyItsOwnWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorSyncOwnWriteNotOtherTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueSyncTestRoot();
	const FString SidecarPath = FPaths::Combine(TestRoot, TEXT("DA_OwnWrite.assetdoc.json"));
	ON_SCOPE_EXIT
	{
		DeleteDirectoryTree(TestRoot);
	};
	IFileManager::Get().MakeDirectory(*TestRoot, true);
	const FString Ours = TEXT("{\"written\":\"by the editor\"}\n");
	{
		FAssetDocumentEditorSync::FScopedSidecarWrite Guard(SidecarPath);
		FString Error;
		const FTCHARToUTF8 Bytes(*Ours);
		TestTrue(TEXT("the editor writes the sidecar"), FAssetDocumentSidecar::WriteBytes(SidecarPath,
			TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length()), Error));
		// Someone writes the file while the scope still runs (after the editor's write).
		FFileHelper::SaveStringToFile(TEXT("{\"written\":\"by someone else\"}"), *SidecarPath);
	}
	TestFalse(TEXT("the other write is not taken for the editor's"), FAssetDocumentEditorSync::IsSidecarWriteSuppressed(SidecarPath));
	FFileHelper::SaveStringToFile(Ours, *SidecarPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	TestTrue(TEXT("the editor's own content still is"), FAssetDocumentEditorSync::IsSidecarWriteSuppressed(SidecarPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorSyncExpectedContentTest,
	"UEAssetDoc.EditorSync.WriteScopeReplacesOnlyExpectedContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorSyncExpectedContentTest::RunTest(const FString& Parameters)
{
	using FScope = FAssetDocumentEditorSync::FScopedSidecarWrite;
	const FString TestRoot = MakeUniqueSyncTestRoot();
	const FString SidecarPath = FPaths::Combine(TestRoot, TEXT("DA_Expected.assetdoc.json"));
	ON_SCOPE_EXIT
	{
		DeleteDirectoryTree(TestRoot);
	};
	IFileManager::Get().MakeDirectory(*TestRoot, true);
	FFileHelper::SaveStringToFile(TEXT("{\"read\":1}"), *SidecarPath);
	const FString ReadDigest = UE::AssetDocument::SidecarSync::DigestSidecarFile(SidecarPath);
	FFileHelper::SaveStringToFile(TEXT("{\"edited\":2}"), *SidecarPath);
	const FString Edited = UE::AssetDocument::SidecarSync::DigestSidecarFile(SidecarPath);
	const TArray<uint8> Bytes = {'{', '}', '\n'};
	FString Error;
	{
		const FScope Guard(SidecarPath, FScope::ERecord::OwnWrites, ReadDigest);
		TestFalse(TEXT("a write based on older content does not replace the file"), FAssetDocumentSidecar::WriteBytes(SidecarPath, Bytes, Error));
	}
	TestEqual(TEXT("the newer content is kept"), UE::AssetDocument::SidecarSync::DigestSidecarFile(SidecarPath), Edited);
	TestFalse(TEXT("and is a change for the file watcher"), FAssetDocumentEditorSync::IsSidecarWriteSuppressed(SidecarPath));
	{
		const FScope Guard(SidecarPath, FScope::ERecord::OwnWrites, Edited);
		TestTrue(FString::Printf(TEXT("a write based on the current content replaces it: %s"), *Error), FAssetDocumentSidecar::WriteBytes(SidecarPath, Bytes, Error));
		TestTrue(TEXT("and the next write in the scope is based on that write"), FAssetDocumentSidecar::WriteBytes(SidecarPath, Bytes, Error));
	}
	TestTrue(TEXT("the write is hidden from the file watcher"), FAssetDocumentEditorSync::IsSidecarWriteSuppressed(SidecarPath));
	{
		const FScope Guard(SidecarPath, FScope::ERecord::OwnWrites);
		FFileHelper::SaveStringToFile(TEXT("{\"during\":3}"), *SidecarPath);
	}
	TestFalse(TEXT("a scope that wrote nothing hides nothing afterwards"), FAssetDocumentEditorSync::IsSidecarWriteSuppressed(SidecarPath));
	return true;
}

#endif
