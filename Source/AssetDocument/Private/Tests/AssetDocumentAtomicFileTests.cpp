// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentAtomicFile.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
FString MakeUniqueAtomicFileTestRoot()
{
	FString TestRoot = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("Automation"),
		FString::Printf(TEXT("AssetDocumentAtomicFile_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FPaths::NormalizeFilename(TestRoot);
	return TestRoot;
}

bool HasAtomicTemporaryFile(const FString& Directory)
{
	bool bFoundTemporaryFile = false;
	IFileManager::Get().IterateDirectory(
		*Directory,
		[&bFoundTemporaryFile](const TCHAR* Path, bool bIsDirectory)
		{
			const FString CleanFilename = FPaths::GetCleanFilename(Path);
			bFoundTemporaryFile = !bIsDirectory
				&& CleanFilename.StartsWith(TEXT("."))
				&& (CleanFilename.EndsWith(TEXT(".tmp"))
					|| CleanFilename.EndsWith(TEXT(".bak")));
			return !bFoundTemporaryFile;
		});
	return bFoundTemporaryFile;
}

bool ReadBytes(const FString& Path, TArray64<uint8>& OutBytes)
{
	OutBytes.Reset();
	return FFileHelper::LoadFileToArray(OutBytes, *Path);
}

bool WriteFixtureBytes(const FString& Path, TArrayView64<const uint8> Bytes)
{
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

#if PLATFORM_MAC || PLATFORM_LINUX
uint32 AssetDocumentAtomicFileTestGetMode(const FString& Path)
{
	FTCHARToUTF8 NativePath(*Path);
	struct stat FileStat;
	return ::stat(NativePath.Get(), &FileStat) == 0
		? static_cast<uint32>(FileStat.st_mode & 07777)
		: 0;
}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicFileCreateExactBytesTest,
	"UEAssetDoc.AtomicFile.CreateExactBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicFileCreateExactBytesTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueAtomicFileTestRoot();
	const FString DestinationPath = FPaths::Combine(TestRoot, TEXT("payload.bin"));
	IFileManager& FileManager = IFileManager::Get();
	ON_SCOPE_EXIT
	{
		FileManager.DeleteDirectory(*TestRoot, false, true);
	};

	TestTrue(TEXT("Creates the test parent directory"), FileManager.MakeDirectory(*TestRoot, true));
	TestFalse(TEXT("Destination starts missing"), FileManager.FileExists(*DestinationPath));

	const TArray64<uint8> ExpectedBytes = {0x00, 0x41, 0xff, 0x10, 0x00, 0x7f};
	FString Error;
	TestTrue(
		TEXT("Creates a missing destination atomically"),
		FAssetDocumentAtomicFile::WriteBytesAtomically(DestinationPath, ExpectedBytes, Error));
	TestTrue(TEXT("Successful write leaves no error"), Error.IsEmpty());

	TArray64<uint8> ActualBytes;
	TestTrue(TEXT("Reads the committed destination"), FFileHelper::LoadFileToArray(ActualBytes, *DestinationPath));
	TestEqual(TEXT("Committed byte count is exact"), ActualBytes.Num(), ExpectedBytes.Num());
	TestTrue(TEXT("Committed bytes are exact"), ActualBytes == ExpectedBytes);
	TestFalse(TEXT("Successful commit leaks no temporary file"), HasAtomicTemporaryFile(TestRoot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicFileReplaceExactBytesTest,
	"UEAssetDoc.AtomicFile.ReplaceExactBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicFileReplaceExactBytesTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueAtomicFileTestRoot();
	const FString DestinationPath = FPaths::Combine(TestRoot, TEXT("replace.bin"));
	IFileManager& FileManager = IFileManager::Get();
	ON_SCOPE_EXIT
	{
		FileManager.DeleteDirectory(*TestRoot, false, true);
	};

	TestTrue(TEXT("Creates the test parent directory"), FileManager.MakeDirectory(*TestRoot, true));
	const TArray64<uint8> OriginalBytes = {0x10, 0x20, 0x30, 0x40};
	const TArray64<uint8> ReplacementBytes = {0xff, 0x00, 0x7f, 0x80, 0x01};
	TestTrue(TEXT("Writes the original fixture"), WriteFixtureBytes(DestinationPath, OriginalBytes));

	FString Error;
	TestTrue(
		TEXT("Atomically replaces an existing destination"),
		FAssetDocumentAtomicFile::WriteBytesAtomically(DestinationPath, ReplacementBytes, Error));
	TestTrue(TEXT("Successful replacement leaves no error"), Error.IsEmpty());

	TArray64<uint8> ActualBytes;
	TestTrue(TEXT("Reads the replaced destination"), ReadBytes(DestinationPath, ActualBytes));
	TestEqual(TEXT("Replacement byte count is exact"), ActualBytes.Num(), ReplacementBytes.Num());
	TestTrue(TEXT("Replacement bytes are exact"), ActualBytes == ReplacementBytes);
	TestFalse(TEXT("Successful replacement leaks no temporary file"), HasAtomicTemporaryFile(TestRoot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicFileReplaceWithEmptyBytesTest,
	"UEAssetDoc.AtomicFile.ReplaceNonEmptyWithEmptyBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicFileReplaceWithEmptyBytesTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueAtomicFileTestRoot();
	const FString DestinationPath = FPaths::Combine(TestRoot, TEXT("replace-with-empty.bin"));
	IFileManager& FileManager = IFileManager::Get();
	ON_SCOPE_EXIT
	{
		FileManager.DeleteDirectory(*TestRoot, false, true);
	};

	TestTrue(TEXT("Creates the test parent directory"), FileManager.MakeDirectory(*TestRoot, true));
	const TArray64<uint8> OriginalBytes = {0x10, 0x20, 0x30, 0x40};
	const TArray64<uint8> EmptyBytes;
	TestTrue(TEXT("Writes the non-empty original fixture"), WriteFixtureBytes(DestinationPath, OriginalBytes));

	FString Error;
	TestTrue(
		TEXT("Atomically replaces an existing non-empty destination with empty bytes"),
		FAssetDocumentAtomicFile::WriteBytesAtomically(DestinationPath, EmptyBytes, Error));
	TestTrue(TEXT("Successful empty replacement leaves no error"), Error.IsEmpty());
	TestTrue(TEXT("Empty replacement keeps the destination present"), FileManager.FileExists(*DestinationPath));
	TestEqual(TEXT("Empty replacement commits a zero-byte file"), FileManager.FileSize(*DestinationPath), static_cast<int64>(0));
	TestFalse(TEXT("Empty replacement leaks no temporary file"), HasAtomicTemporaryFile(TestRoot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicFileFailurePreservesExistingTest,
	"UEAssetDoc.AtomicFile.FailurePreservesExisting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicFileFailurePreservesExistingTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueAtomicFileTestRoot();
	const FString DestinationPath = FPaths::Combine(TestRoot, TEXT("existing.bin"));
	IFileManager& FileManager = IFileManager::Get();
	ON_SCOPE_EXIT
	{
		FAssetDocumentAtomicFile::ResetFailureForTest();
		FileManager.DeleteDirectory(*TestRoot, false, true);
	};

	TestTrue(TEXT("Creates the test parent directory"), FileManager.MakeDirectory(*TestRoot, true));
	const TArray64<uint8> OriginalBytes = {0x01, 0x02, 0x03, 0x04};
	const TArray64<uint8> ReplacementBytes = {0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0};

	struct FFailureCase
	{
		EAssetDocumentAtomicFileFailurePoint FailurePoint;
		const TCHAR* ExpectedErrorStage;
	};
	const FFailureCase FailureCases[] = {
		{EAssetDocumentAtomicFileFailurePoint::Open, TEXT("AtomicFile.Open")},
		{EAssetDocumentAtomicFileFailurePoint::PartialWrite, TEXT("AtomicFile.Write")},
		{EAssetDocumentAtomicFileFailurePoint::Flush, TEXT("AtomicFile.Flush")},
		{EAssetDocumentAtomicFileFailurePoint::Rename, TEXT("AtomicFile.Rename")},
		{EAssetDocumentAtomicFileFailurePoint::DirectoryFlush, TEXT("AtomicFile.DirectoryFlush")},
	};

	for (const FFailureCase& FailureCase : FailureCases)
	{
		TestTrue(TEXT("Restores the original fixture before failure injection"), WriteFixtureBytes(DestinationPath, OriginalBytes));
		FAssetDocumentAtomicFile::FailNextWriteAtForTest(FailureCase.FailurePoint);

		FString Error;
		TestFalse(
			FString::Printf(TEXT("Injected %s stage reports failure"), FailureCase.ExpectedErrorStage),
			FAssetDocumentAtomicFile::WriteBytesAtomically(DestinationPath, ReplacementBytes, Error));
		TestTrue(
			FString::Printf(TEXT("Injected failure reports stable %s stage"), FailureCase.ExpectedErrorStage),
			Error.Contains(FailureCase.ExpectedErrorStage));

		TArray64<uint8> ActualBytes;
		TestTrue(TEXT("Reads the destination after injected failure"), ReadBytes(DestinationPath, ActualBytes));
		TestTrue(TEXT("Injected failure preserves exact original bytes"), ActualBytes == OriginalBytes);
		TestFalse(TEXT("Injected failure leaks no temporary file"), HasAtomicTemporaryFile(TestRoot));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicFilePartialWriteShortPayloadTest,
	"UEAssetDoc.AtomicFile.PartialWriteShortPayloadsFailClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicFilePartialWriteShortPayloadTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueAtomicFileTestRoot();
	IFileManager& FileManager = IFileManager::Get();
	ON_SCOPE_EXIT
	{
		FAssetDocumentAtomicFile::ResetFailureForTest();
		FileManager.DeleteDirectory(*TestRoot, false, true);
	};

	TestTrue(TEXT("Creates the test parent directory"), FileManager.MakeDirectory(*TestRoot, true));
	const TArray64<uint8> OriginalBytes = {0x10, 0x20, 0x30, 0x40};
	const TArray64<uint8> EmptyBytes;
	const TArray64<uint8> SingleByte = {0x7f};
	struct FShortPayloadCase
	{
		const TCHAR* Label;
		const TCHAR* Filename;
		const TArray64<uint8>* Bytes;
	};
	const FShortPayloadCase PayloadCases[] = {
		{TEXT("empty"), TEXT("partial-empty.bin"), &EmptyBytes},
		{TEXT("single-byte"), TEXT("partial-single.bin"), &SingleByte},
	};

	for (const FShortPayloadCase& PayloadCase : PayloadCases)
	{
		const FString DestinationPath = FPaths::Combine(TestRoot, PayloadCase.Filename);
		TestTrue(
			FString::Printf(TEXT("Writes the original fixture for the %s payload"), PayloadCase.Label),
			WriteFixtureBytes(DestinationPath, OriginalBytes));
		FAssetDocumentAtomicFile::FailNextWriteAtForTest(EAssetDocumentAtomicFileFailurePoint::PartialWrite);

		FString Error;
		TestFalse(
			FString::Printf(TEXT("Injected partial-write failure rejects the %s payload"), PayloadCase.Label),
			FAssetDocumentAtomicFile::WriteBytesAtomically(DestinationPath, *PayloadCase.Bytes, Error));
		TestTrue(
			FString::Printf(TEXT("The %s payload failure reports the write stage"), PayloadCase.Label),
			Error.Contains(TEXT("AtomicFile.Write")));

		TArray64<uint8> ActualBytes;
		TestTrue(
			FString::Printf(TEXT("Reads the destination after the %s payload failure"), PayloadCase.Label),
			ReadBytes(DestinationPath, ActualBytes));
		TestTrue(
			FString::Printf(TEXT("The %s payload failure preserves exact original bytes"), PayloadCase.Label),
			ActualBytes == OriginalBytes);
		TestFalse(
			FString::Printf(TEXT("The %s payload failure leaks no temporary file"), PayloadCase.Label),
			HasAtomicTemporaryFile(TestRoot));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicFileFailureKeepsMissingDestinationAbsentTest,
	"UEAssetDoc.AtomicFile.FailureKeepsMissingDestinationAbsent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicFileFailureKeepsMissingDestinationAbsentTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueAtomicFileTestRoot();
	const FString DestinationPath = FPaths::Combine(TestRoot, TEXT("missing.bin"));
	IFileManager& FileManager = IFileManager::Get();
	ON_SCOPE_EXIT
	{
		FAssetDocumentAtomicFile::ResetFailureForTest();
		FileManager.DeleteDirectory(*TestRoot, false, true);
	};

	TestTrue(TEXT("Creates the test parent directory"), FileManager.MakeDirectory(*TestRoot, true));
	const TArray64<uint8> Bytes = {0x31, 0x32, 0x33, 0x34};
	struct FFailureCase
	{
		EAssetDocumentAtomicFileFailurePoint FailurePoint;
		const TCHAR* ExpectedErrorStage;
	};
	const FFailureCase FailureCases[] = {
		{EAssetDocumentAtomicFileFailurePoint::Open, TEXT("AtomicFile.Open")},
		{EAssetDocumentAtomicFileFailurePoint::PartialWrite, TEXT("AtomicFile.Write")},
		{EAssetDocumentAtomicFileFailurePoint::Flush, TEXT("AtomicFile.Flush")},
		{EAssetDocumentAtomicFileFailurePoint::Rename, TEXT("AtomicFile.Rename")},
		{EAssetDocumentAtomicFileFailurePoint::DirectoryFlush, TEXT("AtomicFile.DirectoryFlush")},
	};

	for (const FFailureCase& FailureCase : FailureCases)
	{
		FileManager.Delete(*DestinationPath, false, true);
		FAssetDocumentAtomicFile::FailNextWriteAtForTest(FailureCase.FailurePoint);
		FString Error;
		TestFalse(
			FString::Printf(TEXT("Injected %s stage is reported for a missing destination"), FailureCase.ExpectedErrorStage),
			FAssetDocumentAtomicFile::WriteBytesAtomically(DestinationPath, Bytes, Error));
		TestTrue(
			FString::Printf(TEXT("Missing-destination failure reports stable %s stage"), FailureCase.ExpectedErrorStage),
			Error.Contains(FailureCase.ExpectedErrorStage));
		TestFalse(TEXT("Injected failure keeps the destination absent"), FileManager.FileExists(*DestinationPath));
		TestFalse(TEXT("Injected failure leaks no temporary file"), HasAtomicTemporaryFile(TestRoot));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicFileFailureHookMatchesStageTest,
	"UEAssetDoc.AtomicFile.FailureHookConsumesOnlyAtMatchingStage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicFileFailureHookMatchesStageTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueAtomicFileTestRoot();
	const FString MissingParent = FPaths::Combine(TestRoot, TEXT("not-created"));
	const FString InvalidDestinationPath = FPaths::Combine(MissingParent, TEXT("invalid.bin"));
	const FString ValidDestinationPath = FPaths::Combine(TestRoot, TEXT("valid.bin"));
	IFileManager& FileManager = IFileManager::Get();
	ON_SCOPE_EXIT
	{
		FAssetDocumentAtomicFile::ResetFailureForTest();
		FileManager.DeleteDirectory(*TestRoot, false, true);
	};

	TestTrue(TEXT("Creates the valid test parent directory"), FileManager.MakeDirectory(*TestRoot, true));
	TestFalse(TEXT("Invalid destination parent starts missing"), FileManager.DirectoryExists(*MissingParent));
	const TArray64<uint8> Bytes = {0x41, 0x42, 0x43, 0x44};
	FAssetDocumentAtomicFile::FailNextWriteAtForTest(EAssetDocumentAtomicFileFailurePoint::Rename);

	FString OpenError;
	TestFalse(
		TEXT("A real open failure occurs before the armed rename hook"),
		FAssetDocumentAtomicFile::WriteBytesAtomically(InvalidDestinationPath, Bytes, OpenError));
	TestTrue(TEXT("Earlier real failure reports the open stage"), OpenError.Contains(TEXT("AtomicFile.Open")));
	TestFalse(TEXT("Earlier real failure does not create the missing parent"), FileManager.DirectoryExists(*MissingParent));

	FString RenameError;
	TestFalse(
		TEXT("The still-armed hook fails the next write at rename"),
		FAssetDocumentAtomicFile::WriteBytesAtomically(ValidDestinationPath, Bytes, RenameError));
	TestTrue(TEXT("Deferred hook reports the rename stage"), RenameError.Contains(TEXT("AtomicFile.Rename")));
	TestFalse(TEXT("Deferred rename failure keeps destination absent"), FileManager.FileExists(*ValidDestinationPath));
	TestFalse(TEXT("Stage-matched hook leaves no temporary file"), HasAtomicTemporaryFile(TestRoot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicFileUnicodeEmptyBytesTest,
	"UEAssetDoc.AtomicFile.UnicodePathAndEmptyBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicFileUnicodeEmptyBytesTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueAtomicFileTestRoot();
	const FString DestinationPath = FPaths::Combine(TestRoot, TEXT("原子文件-测试.bin"));
	IFileManager& FileManager = IFileManager::Get();
	ON_SCOPE_EXIT
	{
		FileManager.DeleteDirectory(*TestRoot, false, true);
	};

	TestTrue(TEXT("Creates the test parent directory"), FileManager.MakeDirectory(*TestRoot, true));
	const TArray64<uint8> EmptyBytes;
	FString Error;
	TestTrue(
		TEXT("Writes empty bytes to a Unicode destination"),
		FAssetDocumentAtomicFile::WriteBytesAtomically(DestinationPath, EmptyBytes, Error));
	TestTrue(TEXT("Empty write creates the destination"), FileManager.FileExists(*DestinationPath));
	TestEqual(TEXT("Empty write commits a zero-byte file"), FileManager.FileSize(*DestinationPath), static_cast<int64>(0));
	TestFalse(TEXT("Empty write leaks no temporary file"), HasAtomicTemporaryFile(TestRoot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicFileMissingParentTest,
	"UEAssetDoc.AtomicFile.MissingParentFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicFileMissingParentTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = MakeUniqueAtomicFileTestRoot();
	const FString MissingParent = FPaths::Combine(TestRoot, TEXT("not-created"));
	const FString DestinationPath = FPaths::Combine(MissingParent, TEXT("payload.bin"));
	IFileManager& FileManager = IFileManager::Get();
	ON_SCOPE_EXIT
	{
		FileManager.DeleteDirectory(*TestRoot, false, true);
	};

	TestFalse(TEXT("Parent starts missing"), FileManager.DirectoryExists(*MissingParent));
	const TArray64<uint8> Bytes = {0x01};
	FString Error;
	TestFalse(
		TEXT("Missing parent fails closed"),
		FAssetDocumentAtomicFile::WriteBytesAtomically(DestinationPath, Bytes, Error));
	TestTrue(TEXT("Missing parent reports the open stage"), Error.Contains(TEXT("AtomicFile.Open")));
	TestFalse(TEXT("Helper does not create the missing parent"), FileManager.DirectoryExists(*MissingParent));
	TestFalse(TEXT("Missing-parent failure keeps destination absent"), FileManager.FileExists(*DestinationPath));
	return true;
}

#if PLATFORM_MAC || PLATFORM_LINUX
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicFilePreservesModeTest,
	"UEAssetDoc.AtomicFile.ReplacePreservesExistingMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicFilePreservesModeTest::RunTest(const FString&)
{
	const FString TestRoot = MakeUniqueAtomicFileTestRoot();
	const FString DestinationPath = FPaths::Combine(TestRoot, TEXT("mode.bin"));
	IFileManager& FileManager = IFileManager::Get();
	ON_SCOPE_EXIT
	{
		FAssetDocumentAtomicFile::ResetFailureForTest();
		FileManager.DeleteDirectory(*TestRoot, false, true);
	};

	TestTrue(TEXT("Creates the mode test directory"), FileManager.MakeDirectory(*TestRoot, true));
	const TArray64<uint8> OriginalBytes = {0x01, 0x02};
	const TArray64<uint8> ReplacementBytes = {0x03, 0x04, 0x05};
	TestTrue(TEXT("Writes the mode fixture"), WriteFixtureBytes(DestinationPath, OriginalBytes));
	FTCHARToUTF8 NativeDestinationPath(*DestinationPath);
	TestEqual(TEXT("Sets the fixture mode"), ::chmod(NativeDestinationPath.Get(), 0640), 0);
	const uint32 ModeBefore = AssetDocumentAtomicFileTestGetMode(DestinationPath);

	FString Error;
	TestTrue(
		TEXT("Atomic replacement succeeds for a custom-mode destination"),
		FAssetDocumentAtomicFile::WriteBytesAtomically(DestinationPath, ReplacementBytes, Error));
	TestEqual(TEXT("Atomic replacement preserves the exact existing mode"), AssetDocumentAtomicFileTestGetMode(DestinationPath), ModeBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAtomicFileHardLinkFallbackTest,
	"UEAssetDoc.AtomicFile.HardLinkFailureUsesDurableCopyBackup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAtomicFileHardLinkFallbackTest::RunTest(const FString&)
{
	const FString TestRoot = MakeUniqueAtomicFileTestRoot();
	const FString DestinationPath = FPaths::Combine(TestRoot, TEXT("fallback.bin"));
	IFileManager& FileManager = IFileManager::Get();
	ON_SCOPE_EXIT
	{
		FAssetDocumentAtomicFile::ResetFailureForTest();
		FileManager.DeleteDirectory(*TestRoot, false, true);
	};

	TestTrue(TEXT("Creates the fallback test directory"), FileManager.MakeDirectory(*TestRoot, true));
	const TArray64<uint8> OriginalBytes = {0x10, 0x20, 0x30};
	const TArray64<uint8> ReplacementBytes = {0x40, 0x50, 0x60, 0x70};
	TestTrue(TEXT("Writes the fallback fixture"), WriteFixtureBytes(DestinationPath, OriginalBytes));
	FTCHARToUTF8 NativeDestinationPath(*DestinationPath);
	TestEqual(TEXT("Sets the fallback fixture mode"), ::chmod(NativeDestinationPath.Get(), 0440), 0);
	const uint32 ModeBefore = AssetDocumentAtomicFileTestGetMode(DestinationPath);
	FAssetDocumentAtomicFile::ForceNextHardLinkBackupFallbackForTest();

	FString Error;
	TestTrue(
		TEXT("A forced hard-link failure succeeds through the full-sync copy fallback"),
		FAssetDocumentAtomicFile::WriteBytesAtomically(DestinationPath, ReplacementBytes, Error));
	TArray64<uint8> ActualBytes;
	TestTrue(TEXT("Reads the hard-link fallback result"), ReadBytes(DestinationPath, ActualBytes));
	TestTrue(TEXT("Hard-link fallback commits exact replacement bytes"), ActualBytes == ReplacementBytes);
	TestEqual(TEXT("Hard-link fallback preserves exact existing mode"), AssetDocumentAtomicFileTestGetMode(DestinationPath), ModeBefore);
	TestFalse(TEXT("Hard-link fallback leaves no recovery artifact"), HasAtomicTemporaryFile(TestRoot));
	return true;
}
#endif

#endif // WITH_DEV_AUTOMATION_TESTS
