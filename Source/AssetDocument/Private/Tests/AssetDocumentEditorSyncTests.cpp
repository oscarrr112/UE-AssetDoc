// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorSync.h"

#include "AssetDocumentIngressQueue.h"
#include "AssetDocumentService.h"
#include "AssetDocumentSidecar.h"

#include "Async/TaskGraphInterfaces.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
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

#endif
