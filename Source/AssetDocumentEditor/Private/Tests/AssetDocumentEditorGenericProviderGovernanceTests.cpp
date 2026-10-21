// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Algo/Sort.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorGenericProviderGovernanceTests
{
struct FWriteSetRow
{
	int32 Ordinal = 0;
	FString Action;
	FString Path;
	FString Invariant;
};

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
		*FString::Printf(TEXT("governance input is readable: %s"), RelativePath),
		FFileHelper::LoadFileToString(
			OutText, *FPaths::Combine(GetPluginRoot(), RelativePath)));
}

FString UnwrapCodeCell(FString Value)
{
	Value.TrimStartAndEndInline();
	if (Value.Len() >= 2
		&& Value[0] == TEXT('`')
		&& Value[Value.Len() - 1] == TEXT('`'))
	{
		return Value.Mid(1, Value.Len() - 2);
	}
	return Value;
}

bool ParseWriteSet(
	FAutomationTestBase& Test,
	const FString& Plan,
	TArray<FWriteSetRow>& OutRows)
{
	const int32 SectionStart = Plan.Find(TEXT("## 4. Exact Implementation"));
	const int32 SectionEnd = Plan.Find(
		TEXT("\n---"), ESearchCase::CaseSensitive,
		ESearchDir::FromStart, SectionStart);
	if (!Test.TestTrue(TEXT("plan has bounded write-set section"),
		SectionStart != INDEX_NONE && SectionEnd != INDEX_NONE))
	{
		return false;
	}
	TArray<FString> Lines;
	Plan.Mid(SectionStart, SectionEnd - SectionStart)
		.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		TArray<FString> Cells;
		Line.ParseIntoArray(Cells, TEXT("|"), false);
		if (Cells.Num() < 5)
		{
			continue;
		}
		const FString OrdinalText = Cells[1].TrimStartAndEnd();
		if (OrdinalText.IsEmpty()
			|| !FChar::IsDigit(OrdinalText[0]))
		{
			continue;
		}
		FWriteSetRow& Row = OutRows.AddDefaulted_GetRef();
		Row.Ordinal = FCString::Atoi(*OrdinalText);
		Row.Action = Cells[2].TrimStartAndEnd();
		Row.Path = UnwrapCodeCell(Cells[3]);
		Row.Invariant = Cells[4].TrimStartAndEnd();
	}
	if (!Test.TestEqual(TEXT("plan declares exactly 63 write-set rows"),
		OutRows.Num(), 63))
	{
		return false;
	}
	for (int32 Index = 0; Index < OutRows.Num(); ++Index)
	{
		Test.TestEqual(TEXT("write-set ordinals are contiguous"),
			OutRows[Index].Ordinal, Index + 1);
		Test.TestTrue(TEXT("write-set action is exact"),
			OutRows[Index].Action == TEXT("Create")
				|| OutRows[Index].Action == TEXT("Modify"));
		Test.TestFalse(TEXT("write-set path is non-empty"),
			OutRows[Index].Path.IsEmpty());
		Test.TestFalse(TEXT("write-set invariant is non-empty"),
			OutRows[Index].Invariant.IsEmpty());
	}
	return true;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderGovernanceWriteSetTest,
	"UEAssetDoc.GenericProvider.Governance.WriteSetAndDependencies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderGovernanceWriteSetTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderGovernanceTests;
	FString Plan;
	if (!LoadPluginText(
		*this,
		TEXT("docs/superpowers/plans/"
			"2026-07-20-assetdocument-python-runtime-generic-ue-provider.md"),
		Plan))
	{
		return false;
	}
	TArray<FWriteSetRow> Rows;
	if (!ParseWriteSet(*this, Plan, Rows))
	{
		return false;
	}
	TSet<FString> UniquePaths;
	for (const FWriteSetRow& Row : Rows)
	{
		TestTrue(TEXT("write-set path is unique"),
			!UniquePaths.Contains(Row.Path));
		UniquePaths.Add(Row.Path);
		TestTrue(
			*FString::Printf(TEXT("write-set path exists: %s"), *Row.Path),
			IFileManager::Get().FileExists(
				*FPaths::Combine(GetPluginRoot(), Row.Path)));
	}
	TestEqual(TEXT("all 63 paths are unique"), UniquePaths.Num(), 63);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderGovernanceDurableAuthorityTest,
	"UEAssetDoc.GenericProvider.Governance.DurableAuthorityFinal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderGovernanceDurableAuthorityTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderGovernanceTests;
	FString Dossiers;
	FString Ledger;
	if (!LoadPluginText(
			*this,
			TEXT("docs/superpowers/research/assetdocument-python-runtime/"
				"f-generic-ue-engine-authority-dossiers.md"),
			Dossiers)
		|| !LoadPluginText(
			*this,
			TEXT("docs/superpowers/research/assetdocument-python-runtime/"
				"f-generic-ue-category-admission-ledger.md"),
			Ledger))
	{
		return false;
	}
	TestFalse(TEXT("dossiers contain no provisional admission state"),
		Dossiers.Contains(TEXT("still admitted only after")));
	TestFalse(TEXT("ledger contains no T0-only terminal"),
		Ledger.Contains(TEXT("## T0 state")));
	TestFalse(TEXT("ledger contains no pending provider admission"),
		Ledger.Contains(TEXT("provider admission still requires")));
	TestTrue(TEXT("dossiers record T6 evidence closure"),
		Dossiers.Contains(TEXT("## T6 evidence closure")));
	TestTrue(TEXT("ledger records T6 evidence closure"),
		Ledger.Contains(TEXT("## T6 evidence closure")));
	return true;
}
#endif
