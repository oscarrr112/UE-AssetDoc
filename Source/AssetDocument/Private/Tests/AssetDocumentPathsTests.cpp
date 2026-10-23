// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// AssetDocumentPaths: whether a path lies inside a directory, judged by the filesystem's real paths.
//
//   Paths.Within: ordinary paths inside (existing or not yet created) are inside; a sibling whose name
//     starts with the root's name is not; '..' that leaves the root, a link that leads out of the root, and a
//     dangling link are outside; a link to another place inside stays inside; the root spelled in another
//     case is inside exactly when the volume treats both spellings as the same directory.
//   Paths.VolumeCase: with a resolver standing for a case-sensitive volume, "content" is not "Content";
//     with one standing for a case-insensitive volume, it is (the canonical case is used).
//   Paths.SidecarScope: a sidecar is resolved to its /Game target only from inside the project's Content
//     directory: not through a Content folder that links out of the project, and not from "content/" on a
//     case-sensitive volume.
//   Paths.ResolveForAccess: the file an access reaches is checked, the leaf included: a file link to a file outside
//     the root is refused, a dangling one too; a link to a file inside gives that file; an ordinary path (existing or
//     not yet created) gives itself.
//   Paths.SidecarReadScope: apply_asset_document_file, validate and diff refuse a sidecar outside Content before
//     reading it: an absolute path outside the project, and a Content sidecar that is a link to a file outside. The
//     outside files hold a duplicate key, whose name any read would report (Document.DuplicateKey); it is never
//     reported. A Content sidecar with a duplicate key still reports it.

#include "AssetDocumentPaths.h"
#include "AssetDocumentPathsTestSupport.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentSidecar.h"
#include "AssetDocumentTypes.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentPathsTests
{
FString MakeScratchDirectory()
{
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentPathsTests"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Directory, true);
	return Directory;
}

// A resolver that knows only the given paths (each resolves to its key's value) and nothing else.
AssetDocumentPaths::FResolveExisting MapResolver(TMap<FString, FString> Known, ESearchCase::Type Case)
{
	return [Known = MoveTemp(Known), Case](const FString& Path, FString& OutRealPath)
	{
		for (const TPair<FString, FString>& Entry : Known)
		{
			if (Entry.Key.Equals(Path, Case))
			{
				OutRealPath = Entry.Value;
				return AssetDocumentPaths::EPathLookup::Resolved;
			}
		}
		return AssetDocumentPaths::EPathLookup::Missing;
	};
}

// Every ancestor of Path (and Path itself) mapped to itself.
void AddWithAncestors(TMap<FString, FString>& Known, FString Path)
{
	FPaths::NormalizeFilename(Path);
	while (!Path.IsEmpty())
	{
		Known.Add(Path, Path);
		int32 Slash = INDEX_NONE;
		if (!Path.FindLastChar(TEXT('/'), Slash) || Slash == 0 || (Slash == 2 && Path[1] == TEXT(':')))
		{
			Known.Add(Path.Left(Slash + 1), Path.Left(Slash + 1));
			break;
		}
		Path = Path.Left(Slash);
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPathsWithinTest, "UEAssetDoc.Paths.Within",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPathsWithinTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPathsTests;
	using namespace AssetDocumentPathsTestSupport;
	using AssetDocumentPaths::IsWithin;
	using AssetDocumentPaths::MakeRelativeWithin;

	const FString Scratch = MakeScratchDirectory();
	const FString Outside = MakeOutsideDirectory(TEXT("Within"));
	const FString Root = Scratch / TEXT("Root");
	const FString OutLink = Root / TEXT("Out");
	const FString AliasLink = Root / TEXT("Alias");
	const FString DanglingLink = Root / TEXT("Dangling");
	ON_SCOPE_EXIT
	{
		RemoveDirectoryLink(OutLink);
		RemoveDirectoryLink(AliasLink);
		RemoveDirectoryLink(DanglingLink);
		IFileManager::Get().DeleteDirectory(*Scratch, false, true);
		IFileManager::Get().DeleteDirectory(*Outside, false, true);
	};
	IFileManager::Get().MakeDirectory(*(Root / TEXT("Inside")), true);
	IFileManager::Get().MakeDirectory(*(Scratch / TEXT("RootSibling")), true);
	IFileManager::Get().MakeDirectory(*(Outside / TEXT("Gone")), true);
	TestTrue(TEXT("a file inside"), FFileHelper::SaveStringToFile(TEXT("x"), *(Root / TEXT("Inside/File.txt"))));

	TestTrue(TEXT("the root itself"), IsWithin(Root, Root));
	TestTrue(TEXT("an existing file inside"), IsWithin(Root, Root / TEXT("Inside/File.txt")));
	TestTrue(TEXT("a folder not created yet"), IsWithin(Root, Root / TEXT("Inside/New/Deeper")));
	FString Relative;
	TestTrue(TEXT("relative path inside"), MakeRelativeWithin(Root, Root / TEXT("Inside/File.txt"), Relative));
	TestEqual(TEXT("relative path"), Relative, FString(TEXT("Inside/File.txt")));
	TestFalse(TEXT("a sibling whose name starts with the root's"), IsWithin(Root, Scratch / TEXT("RootSibling")));
	TestFalse(TEXT("'..' that leaves the root"), IsWithin(Root, Root / TEXT("Inside/../../RootSibling")));
	TestFalse(TEXT("an unrelated directory"), IsWithin(Root, Outside));

	if (TestTrue(TEXT("a link out of the root"), MakeDirectoryLink(OutLink, Outside)))
	{
		TestFalse(TEXT("the link that leads out"), IsWithin(Root, OutLink));
		TestFalse(TEXT("a folder through the link"), IsWithin(Root, OutLink / TEXT("Exports")));
		TestFalse(TEXT("a file through the link"), IsWithin(Root, OutLink / TEXT("File.txt")));
	}
	if (TestTrue(TEXT("a link to a folder inside"), MakeDirectoryLink(AliasLink, Root / TEXT("Inside"))))
	{
		TestTrue(TEXT("a link that stays inside"), IsWithin(Root, AliasLink / TEXT("File.txt")));
	}
	if (TestTrue(TEXT("a link to a folder that goes away"), MakeDirectoryLink(DanglingLink, Outside / TEXT("Gone"))))
	{
		IFileManager::Get().DeleteDirectory(*(Outside / TEXT("Gone")), false, true);
		TestFalse(TEXT("a dangling link"), IsWithin(Root, DanglingLink / TEXT("New.txt")));
	}

	// The root spelled in another case: the same directory on a case-insensitive volume, none on a
	// case-sensitive one.
	const FString OtherCaseRoot = Scratch / TEXT("rOOT");
	const bool bSameDirectory = IFileManager::Get().DirectoryExists(*OtherCaseRoot);
	AddInfo(FString::Printf(TEXT("the test volume is case-%s"), bSameDirectory ? TEXT("insensitive") : TEXT("sensitive")));
	TestEqual(TEXT("another case is inside exactly when it names the same directory"),
		IsWithin(Root, OtherCaseRoot / TEXT("Inside/File.txt")), bSameDirectory);
	if (bSameDirectory)
	{
		TestTrue(TEXT("canonical case"), MakeRelativeWithin(Root, OtherCaseRoot / TEXT("inside/file.txt"), Relative));
		TestEqual(TEXT("the relative path has the stored case"), Relative, FString(TEXT("Inside/File.txt")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPathsVolumeCaseTest, "UEAssetDoc.Paths.VolumeCase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPathsVolumeCaseTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPathsTests;
	const FString Project = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SimulatedVolume/Project")));
	const FString Content = Project / TEXT("Content");
	const FString Sidecar = Content / TEXT("AI/BB_Enemy.assetdoc.json");
	TMap<FString, FString> Known;
	AddWithAncestors(Known, Sidecar);
	{
		AssetDocumentPaths::FScopedResolverOverride CaseSensitive(MapResolver(Known, ESearchCase::CaseSensitive));
		TestTrue(TEXT("case-sensitive: the stored name"), AssetDocumentPaths::IsWithin(Content, Sidecar));
		TestFalse(TEXT("case-sensitive: \"content\" is another directory"),
			AssetDocumentPaths::IsWithin(Content, Project / TEXT("content/AI/BB_Enemy.assetdoc.json")));
	}
	{
		AssetDocumentPaths::FScopedResolverOverride CaseInsensitive(MapResolver(Known, ESearchCase::IgnoreCase));
		FString Relative;
		TestTrue(TEXT("case-insensitive: \"content\" is the same directory"),
			AssetDocumentPaths::MakeRelativeWithin(Content, Project / TEXT("content/ai/bb_enemy.assetdoc.json"), Relative));
		TestEqual(TEXT("case-insensitive: the stored case"), Relative, FString(TEXT("AI/BB_Enemy.assetdoc.json")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPathsSidecarScopeTest, "UEAssetDoc.Paths.SidecarScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPathsSidecarScopeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPathsTests;
	using namespace AssetDocumentPathsTestSupport;
	const FString Folder = FString::Printf(TEXT("AssetDocumentPathsTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FString ContentDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
	FPaths::NormalizeDirectoryName(ContentDir);
	const FString Inside = ContentDir / Folder;
	const FString Outside = MakeOutsideDirectory(TEXT("Sidecar"));
	const FString Link = ContentDir / (Folder + TEXT("_Linked"));
	ON_SCOPE_EXIT
	{
		RemoveDirectoryLink(Link);
		IFileManager::Get().DeleteDirectory(*Inside, false, true);
		IFileManager::Get().DeleteDirectory(*Outside, false, true);
	};
	const FString Document = TEXT("{\"SchemaVersion\":1}");
	TestTrue(TEXT("a sidecar inside Content"), FFileHelper::SaveStringToFile(Document, *(Inside / TEXT("DA_In.assetdoc.json"))));
	TestTrue(TEXT("a sidecar outside the project"), FFileHelper::SaveStringToFile(Document, *(Outside / TEXT("DA_Out.assetdoc.json"))));

	TestEqual(TEXT("an ordinary sidecar"), FAssetDocumentSidecar::ResolveObjectPathFromSidecar(Inside / TEXT("DA_In.assetdoc.json")),
		FString::Printf(TEXT("/Game/%s/DA_In"), *Folder));
	if (TestTrue(TEXT("a Content folder that links out of the project"), MakeDirectoryLink(Link, Outside)))
	{
		TestEqual(TEXT("a sidecar through the link is outside Content"),
			FAssetDocumentSidecar::ResolveObjectPathFromSidecar(Link / TEXT("DA_Out.assetdoc.json")), FString());
	}

	// "content/" on a case-sensitive volume: another directory than Content.
	TMap<FString, FString> Known;
	AddWithAncestors(Known, Inside / TEXT("DA_In.assetdoc.json"));
	AssetDocumentPaths::FScopedResolverOverride CaseSensitive(MapResolver(Known, ESearchCase::CaseSensitive));
	TestEqual(TEXT("case-sensitive volume: the Content sidecar"),
		FAssetDocumentSidecar::ResolveObjectPathFromSidecar(Inside / TEXT("DA_In.assetdoc.json")), FString::Printf(TEXT("/Game/%s/DA_In"), *Folder));
	const FString LowerContent = FPaths::GetPath(ContentDir) / TEXT("content") / Folder / TEXT("DA_In.assetdoc.json");
	TestEqual(TEXT("case-sensitive volume: a sidecar under content/ is outside Content"),
		FAssetDocumentSidecar::ResolveObjectPathFromSidecar(LowerContent), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPathsResolveForAccessTest, "UEAssetDoc.Paths.ResolveForAccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPathsResolveForAccessTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPathsTests;
	using namespace AssetDocumentPathsTestSupport;
	const FString Root = MakeScratchDirectory();
	const FString Outside = MakeOutsideDirectory(TEXT("Access"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().DeleteDirectory(*Outside, false, true);
	};
	const FString RealRoot = AssetDocumentPaths::ResolveRealPath(Root);
	TestTrue(TEXT("files"), FFileHelper::SaveStringToFile(TEXT("in"), *(Root / TEXT("In.txt")))
		&& FFileHelper::SaveStringToFile(TEXT("out"), *(Outside / TEXT("Out.txt"))));
	FString Real;
	TestTrue(TEXT("an ordinary file"), AssetDocumentPaths::ResolveForAccess(Root, Root / TEXT("In.txt"), Real));
	TestEqual(TEXT("an ordinary file is itself"), Real, RealRoot / TEXT("In.txt"));
	TestTrue(TEXT("a file not yet written"), AssetDocumentPaths::ResolveForAccess(Root, Root / TEXT("New/Later.txt"), Real));
	TestEqual(TEXT("a file not yet written is itself"), Real, RealRoot / TEXT("New/Later.txt"));
	if (!MakeFileLink(Root / TEXT("LinkOut.txt"), Outside / TEXT("Out.txt")))
	{
		AddInfo(TEXT("this system does not let the test make a file link; the link checks are skipped"));
		return true;
	}
	TestFalse(TEXT("a file link to a file outside the root"), AssetDocumentPaths::ResolveForAccess(Root, Root / TEXT("LinkOut.txt"), Real));
	TestTrue(TEXT("refused: no path given"), Real.IsEmpty());
	TestFalse(TEXT("IsWithin agrees"), AssetDocumentPaths::IsWithin(Root, Root / TEXT("LinkOut.txt")));
	TestTrue(TEXT("a file link to a file inside"), MakeFileLink(Root / TEXT("LinkIn.txt"), Root / TEXT("In.txt"))
		&& AssetDocumentPaths::ResolveForAccess(Root, Root / TEXT("LinkIn.txt"), Real));
	TestEqual(TEXT("a link inside gives the file it reaches"), Real, RealRoot / TEXT("In.txt"));
	TestTrue(TEXT("a dangling file link"), MakeFileLink(Root / TEXT("Dangling.txt"), Outside / TEXT("Missing.txt")));
	TestFalse(TEXT("a dangling file link is refused"), AssetDocumentPaths::ResolveForAccess(Root, Root / TEXT("Dangling.txt"), Real));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPathsSidecarReadScopeTest, "UEAssetDoc.Paths.SidecarReadScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentPathsSidecarReadScopeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPathsTestSupport;
	const FString Folder = FString::Printf(TEXT("AssetDocumentPathsRead_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FString ContentDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
	FPaths::NormalizeDirectoryName(ContentDir);
	const FString Inside = ContentDir / Folder;
	const FString Outside = MakeOutsideDirectory(TEXT("SidecarRead"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Inside, false, true);
		IFileManager::Get().DeleteDirectory(*Outside, false, true);
	};
	// Two keys of one object equal ignoring case: a read reports both names.
	const auto Document = [](const FString& Target, const TCHAR* Key)
	{
		return FString::Printf(TEXT("{\"SchemaVersion\": 1, \"Target\": \"%s\", \"Class\": \"/Script/Engine.DataAsset\", "
			"\"Action\": \"Update\", \"Properties\": {\"%s\": 1, \"%s\": 2}}"), *Target, Key, *FString(Key).ToLower());
	};
	const FString OutsideFile = Outside / TEXT("DA_Outside.assetdoc.json");
	TestTrue(TEXT("an outside sidecar"), FFileHelper::SaveStringToFile(Document(TEXT("/Game/DA_Outside"), TEXT("OutsideSecretKey")), *OutsideFile));
	const FString InsideFile = Inside / TEXT("DA_Inside.assetdoc.json");
	TestTrue(TEXT("a Content sidecar"), FFileHelper::SaveStringToFile(Document(FString::Printf(TEXT("/Game/%s/DA_Inside"), *Folder),
		TEXT("InsideDuplicateKey")), *InsideFile));

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const auto Text = [](const FAssetDocumentResult& Result)
	{
		FString All = Result.Message;
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			All += TEXT("\n") + Diagnostic.Code + TEXT(" ") + Diagnostic.Path + TEXT(" ") + Diagnostic.Message;
		}
		return All;
	};
	const auto ExpectNotRead = [this, &Service, &Text](const FString& File, const TCHAR* Case)
	{
		FAssetDocumentApplyFileRequest Apply;
		Apply.FilePath = File;
		Apply.bSaveAsset = true;
		FAssetDocumentValidateRequest Validate;
		Validate.FilePath = File;
		FAssetDocumentDiffRequest Diff;
		Diff.FilePath = File;
		const TPair<const TCHAR*, FAssetDocumentResult> Results[] = {
			{TEXT("apply"), Service.ApplyFile(Apply)}, {TEXT("validate"), Service.Validate(Validate)}, {TEXT("diff"), Service.Diff(Diff)}};
		for (const TPair<const TCHAR*, FAssetDocumentResult>& Entry : Results)
		{
			const FString All = Text(Entry.Value);
			AddInfo(FString::Printf(TEXT("%s, %s: %s"), Case, Entry.Key, *All));
			TestFalse(FString::Printf(TEXT("%s, %s: refused"), Case, Entry.Key), Entry.Value.IsSuccess());
			TestTrue(FString::Printf(TEXT("%s, %s: refused as outside Content"), Case, Entry.Key),
				All.Contains(TEXT("Unable to resolve expected Target from sidecar file")));
			TestFalse(FString::Printf(TEXT("%s, %s: the file was not read (its key is not reported)"), Case, Entry.Key),
				All.Contains(TEXT("SecretKey"), ESearchCase::IgnoreCase) || All.Contains(TEXT("DuplicateKey")));
		}
	};
	ExpectNotRead(OutsideFile, TEXT("an absolute path outside the project"));
	const FString Linked = Inside / TEXT("DA_Linked.assetdoc.json");
	if (MakeFileLink(Linked, OutsideFile))
	{
		ExpectNotRead(Linked, TEXT("a Content sidecar that links to a file outside"));
	}
	else
	{
		AddInfo(TEXT("this system does not let the test make a file link; the linked sidecar is skipped"));
	}

	// A sidecar inside Content is read: its duplicate key is reported.
	FAssetDocumentValidateRequest Validate;
	Validate.FilePath = InsideFile;
	const FString InsideText = Text(Service.Validate(Validate));
	AddInfo(FString::Printf(TEXT("Content sidecar: %s"), *InsideText));
	TestTrue(TEXT("a Content sidecar is read: Document.DuplicateKey names its key"),
		InsideText.Contains(TEXT("Document.DuplicateKey")) && InsideText.Contains(TEXT("InsideDuplicateKey")));
	return true;
}

#endif
