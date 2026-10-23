// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Algo/Sort.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorGenericProviderGovernanceTests
{
FString GetPluginRoot()
{
	return FPaths::ConvertRelativePathToFull(GetAssetDocumentPluginBaseDir());
}

bool LoadPluginText(
	FAutomationTestBase& Test,
	const TCHAR* RelativePath,
	FString& OutText)
{
	return Test.TestTrue(
		*FString::Printf(TEXT("plugin file is readable: %s"), RelativePath),
		FFileHelper::LoadFileToString(
			OutText, *FPaths::Combine(GetPluginRoot(), RelativePath)));
}

TArray<FString> ExtractDependencyBlock(
	FAutomationTestBase& Test,
	const FString& BuildRules,
	const TCHAR* Marker)
{
	TArray<FString> Result;
	const int32 MarkerIndex = BuildRules.Find(Marker);
	const int32 OpenBrace = BuildRules.Find(
		TEXT("{"), ESearchCase::CaseSensitive,
		ESearchDir::FromStart, MarkerIndex);
	const int32 CloseBrace = BuildRules.Find(
		TEXT("}"), ESearchCase::CaseSensitive,
		ESearchDir::FromStart, OpenBrace);
	if (!Test.TestTrue(
		*FString::Printf(TEXT("%s dependency block exists"), Marker),
		MarkerIndex != INDEX_NONE
			&& OpenBrace != INDEX_NONE
			&& CloseBrace != INDEX_NONE))
	{
		return Result;
	}
	TArray<FString> Lines;
	BuildRules.Mid(OpenBrace + 1, CloseBrace - OpenBrace - 1)
		.ParseIntoArrayLines(Lines);
	for (FString Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (!Line.StartsWith(TEXT("\"")))
		{
			continue;
		}
		const int32 CloseQuote = Line.Find(
			TEXT("\""), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, 1);
		if (CloseQuote != INDEX_NONE)
		{
			Result.Add(Line.Mid(1, CloseQuote - 1));
		}
	}
	Algo::Sort(Result);
	return Result;
}

}

// The editor module that hosts the generic provider keeps its exact module dependencies: a new dependency is a
// deliberate change to this list.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderEditorModuleDependenciesTest,
	"UEAssetDoc.GenericProvider.Governance.EditorModuleDependencies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderEditorModuleDependenciesTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderGovernanceTests;
	FString BuildRules;
	if (!LoadPluginText(
		*this,
		TEXT("Source/AssetDocumentEditor/AssetDocumentEditor.Build.cs"),
		BuildRules))
	{
		return false;
	}
	const TArray<FString> ExpectedPublic = {
		TEXT("AssetDocument"), TEXT("Core")};
	const TArray<FString> ExpectedPrivate = {
		TEXT("AssetDocumentServer"), TEXT("AssetRegistry"), TEXT("AssetTools"), TEXT("CoreUObject"),
		TEXT("Engine"), TEXT("EnhancedInput"), TEXT("GraphEditor"), TEXT("InputCore"), TEXT("Json"), TEXT("JsonUtilities"),
		TEXT("PropertyEditor"), TEXT("Slate"), TEXT("SlateCore"), TEXT("UnrealEd")};
	TestTrue(TEXT("public dependencies are exact"),
		ExtractDependencyBlock(
			*this, BuildRules, TEXT("PublicDependencyModuleNames"))
		== ExpectedPublic);
	TestTrue(TEXT("private dependencies are exact"),
		ExtractDependencyBlock(
			*this, BuildRules, TEXT("PrivateDependencyModuleNames"))
		== ExpectedPrivate);
	return true;
}
#endif
