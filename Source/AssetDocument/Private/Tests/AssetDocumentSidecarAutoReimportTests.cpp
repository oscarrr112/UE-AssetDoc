// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.SidecarSync.AutoReimportExcludesSidecars: with Exclude Sidecars From Auto Reimport on (the default), the
// editor's Auto Reimport directory that covers Content has an exclude wildcard for sidecars and conflict files, once,
// beside the user's own entries; the rules the Auto Reimport manager builds from that entry (the import factories'
// extensions, JSON among them, plus its wildcards) do not offer a sidecar or a conflict file for import, and still offer
// another JSON file. Turning the setting off removes the two wildcards and nothing else.

#include "AssetDocumentSettings.h"
#include "AssetDocumentSidecarAutoReimport.h"
#include "FileCacheUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/WildcardString.h"
#include "Settings/EditorLoadingSavingSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SidecarAutoReimportTestsLocal
{
using namespace UE::AssetDocument::SidecarAutoReimport;

int32 CountExcludes(const FAutoReimportDirectoryConfig& Directory, const TCHAR* Wildcard)
{
	return Directory.Wildcards.FilterByPredicate([Wildcard](const FAutoReimportWildcard& Entry)
	{
		return !Entry.bInclude && Entry.Wildcard.Equals(Wildcard, ESearchCase::IgnoreCase);
	}).Num();
}

bool HasEntry(const FAutoReimportDirectoryConfig& Directory, const TCHAR* Wildcard, bool bInclude)
{
	return Directory.Wildcards.ContainsByPredicate([Wildcard, bInclude](const FAutoReimportWildcard& Entry)
	{
		return Entry.bInclude == bInclude && Entry.Wildcard == Wildcard;
	});
}

// The rules FAutoReimportManager::SetUpDirectoryMonitors builds for the directory: the extensions an import factory
// reads (here JSON, which the Data Table / Curve Table importer reads), then the entry's wildcards.
DirectoryWatcher::FMatchRules MonitorRules(const FAutoReimportDirectoryConfig& Directory)
{
	DirectoryWatcher::FMatchRules Rules;
	Rules.SetApplicableExtensions(TEXT(";json;png;"));
	for (const FAutoReimportWildcard& Entry : Directory.Wildcards)
	{
		Rules.AddWildcardRule(Entry.Wildcard, Entry.bInclude);
	}
	return Rules;
}

FAutoReimportDirectoryConfig Directory(const FString& Source, TArray<FAutoReimportWildcard> Wildcards)
{
	FAutoReimportDirectoryConfig Config;
	Config.SourceDirectory = Source;
	Config.Wildcards = MoveTemp(Wildcards);
	return Config;
}

FAutoReimportWildcard Entry(const TCHAR* Wildcard, bool bInclude)
{
	FAutoReimportWildcard Result;
	Result.Wildcard = Wildcard;
	Result.bInclude = bInclude;
	return Result;
}
} // namespace SidecarAutoReimportTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSidecarAutoReimportTest, "UEAssetDoc.SidecarSync.AutoReimportExcludesSidecars",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSidecarAutoReimportTest::RunTest(const FString& Parameters)
{
	using namespace SidecarAutoReimportTestsLocal;
	UEditorLoadingSavingSettings* Editor = GetMutableDefault<UEditorLoadingSavingSettings>();
	UAssetDocumentSettings* Plugin = GetMutableDefault<UAssetDocumentSettings>();
	const TArray<FAutoReimportDirectoryConfig> Original = Editor->AutoReimportDirectorySettings;
	const bool bOriginal = Plugin->bExcludeSidecarsFromAutoReimport;
	ON_SCOPE_EXIT
	{
		Plugin->bExcludeSidecarsFromAutoReimport = bOriginal;
		Editor->AutoReimportDirectorySettings = Original;
		Sync();
	};

	// The editor's own settings: the plugin added the wildcards at startup to the entry that covers Content.
	Plugin->bExcludeSidecarsFromAutoReimport = true;
	Sync();
	const FAutoReimportDirectoryConfig* Content = Editor->AutoReimportDirectorySettings.FindByPredicate(
		[](const FAutoReimportDirectoryConfig& Config) { return CoversContent(Config); });
	if (TestNotNull(TEXT("an Auto Reimport directory covers Content"), Content))
	{
		AddInfo(FString::Printf(TEXT("Auto Reimport directory %s, %d wildcard(s)"), *Content->SourceDirectory, Content->Wildcards.Num()));
		for (const TCHAR* Wildcard : GetWildcards())
		{
			TestEqual(FString::Printf(TEXT("%s is excluded once"), Wildcard), CountExcludes(*Content, Wildcard), 1);
		}
		const DirectoryWatcher::FMatchRules Rules = MonitorRules(*Content);
		TestFalse(TEXT("a sidecar is not offered for import"), Rules.IsFileApplicable(TEXT("AI/BB_Enemy.assetdoc.json")));
		TestFalse(TEXT("a sidecar at the Content root is not offered for import"), Rules.IsFileApplicable(TEXT("BP_Hero.assetdoc.json")));
		TestFalse(TEXT("a conflict file is not offered for import"), Rules.IsFileApplicable(TEXT("AI/BB_Enemy.assetdoc.asset.json")));
		TestTrue(TEXT("another JSON file still is"), Rules.IsFileApplicable(TEXT("Data/DT_Items.json")));
	}
	TestFalse(TEXT("syncing again changes nothing"), Sync());

	// Only a directory that covers Content gets them, next to the user's own entries; off removes exactly them.
	const FString Outside = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
	TArray<FAutoReimportDirectoryConfig> Directories = {
		Directory(TEXT("/Game/"), {Entry(TEXT("Localization/*"), false), Entry(TEXT("Raw/*.json"), true)}),
		Directory(Outside, {Entry(TEXT("*.tmp"), false)})};
	TestTrue(TEXT("on: the wildcards are added"), UpdateDirectories(Directories, true));
	TestFalse(TEXT("on again: nothing changes"), UpdateDirectories(Directories, true));
	TestEqual(TEXT("on: the Content directory has the user's two entries and the two wildcards"), Directories[0].Wildcards.Num(), 4);
	TestTrue(TEXT("on: the user's entries are kept"),
		HasEntry(Directories[0], TEXT("Localization/*"), false) && HasEntry(Directories[0], TEXT("Raw/*.json"), true));
	TestEqual(TEXT("on: a directory outside Content is not changed"), Directories[1].Wildcards.Num(), 1);
	const DirectoryWatcher::FMatchRules WithInclude = MonitorRules(Directories[0]);
	TestFalse(TEXT("a user include of *.json does not bring sidecars back"), WithInclude.IsFileApplicable(TEXT("Raw/BP_Hero.assetdoc.json")));
	TestTrue(TEXT("the user's include still applies"), WithInclude.IsFileApplicable(TEXT("Raw/DT_Items.json")));
	TestTrue(TEXT("off: the wildcards are removed"), UpdateDirectories(Directories, false));
	TestFalse(TEXT("off again: nothing changes"), UpdateDirectories(Directories, false));
	TestTrue(TEXT("off: only the user's entries are left"), Directories[0].Wildcards.Num() == 2
		&& HasEntry(Directories[0], TEXT("Localization/*"), false) && HasEntry(Directories[0], TEXT("Raw/*.json"), true));

	// The plugin setting turned off takes them out of the editor's settings.
	Plugin->bExcludeSidecarsFromAutoReimport = false;
	TestTrue(TEXT("turning the setting off changes the editor's settings"), Sync());
	for (const FAutoReimportDirectoryConfig& Config : Editor->AutoReimportDirectorySettings)
	{
		for (const TCHAR* Wildcard : GetWildcards())
		{
			TestEqual(FString::Printf(TEXT("off: %s no longer excluded in %s"), Wildcard, *Config.SourceDirectory), CountExcludes(Config, Wildcard), 0);
		}
	}
	return true;
}

#endif
