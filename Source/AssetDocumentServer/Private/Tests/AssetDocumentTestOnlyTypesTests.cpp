// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.TestOnlyTypes: the plugin's test-only reflected types (fixture classes, structs, enums, row structs)
//   never reach a user's editor session. They live in the test-only modules (LoadingPhase "None"), which the editor
//   does not load at startup and the automation framework loads before it lists the tests; no other module of the
//   plugin declares a test type. So class / struct pickers, Blueprint actions and list_types never show them.

#include "AssetDocumentTestOnlyModules.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTestOnlyTypesTestsLocal
{
FString JoinNames(const TArray<FName>& Names)
{
	TArray<FString> Strings;
	for (const FName Name : Names)
	{
		Strings.Add(Name.ToString());
	}
	return FString::Join(Strings, TEXT(", "));
}

// The plugin's module script packages (/Script/<Module>), split into test-only and the others.
void GetPluginScriptPackages(TSet<FName>& OutTestOnly, TSet<FName>& OutOther)
{
	const TArray<FName> TestOnly = UE::AssetDocumentServer::GetTestOnlyModuleNames();
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEAssetDoc")))
	{
		for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
		{
			const FName Package(*(TEXT("/Script/") + Module.Name.ToString()));
			(TestOnly.Contains(Module.Name) ? OutTestOnly : OutOther).Add(Package);
		}
	}
}

// A type is a test type when its header is in a Tests folder or its name says Test (as a word: "...Test", "TestX").
bool IsTestType(const UField& Type, FString& OutWhy)
{
	const FString Header = Type.GetMetaData(TEXT("ModuleRelativePath"));
	if (Header.StartsWith(TEXT("Tests/")) || Header.Contains(TEXT("/Tests/")))
	{
		OutWhy = Header;
		return true;
	}
	const FString Name = Type.GetName();
	for (int32 Index = Name.Find(TEXT("Test"), ESearchCase::CaseSensitive); Index != INDEX_NONE;
		Index = Name.Find(TEXT("Test"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 1))
	{
		const int32 After = Index + 4;
		if (After == Name.Len() || FChar::IsUpper(Name[After]))
		{
			OutWhy = TEXT("name");
			return true;
		}
	}
	return false;
}

template <typename TType>
void CollectPluginTypes(const TSet<FName>& TestOnlyPackages, const TSet<FName>& OtherPackages,
	TArray<FString>& OutMisplaced, int32& OutTestOnlyCount)
{
	for (TObjectIterator<TType> It; It; ++It)
	{
		const FName Package = It->GetOutermost()->GetFName();
		if (TestOnlyPackages.Contains(Package))
		{
			++OutTestOnlyCount;
			continue;
		}
		FString Why;
		if (OtherPackages.Contains(Package) && IsTestType(**It, Why))
		{
			OutMisplaced.Add(FString::Printf(TEXT("%s (%s)"), *It->GetPathName(), *Why));
		}
	}
}
} // namespace AssetDocumentTestOnlyTypesTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTestOnlyModulesNotLoadedAtStartupTest,
	"UEAssetDoc.TestOnlyTypes.ModulesNotLoadedAtStartup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTestOnlyModulesNotLoadedAtStartupTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTestOnlyTypesTestsLocal;

	const TArray<FName> TestOnly = UE::AssetDocumentServer::GetTestOnlyModuleNames();
	TestTrue(TEXT("the plugin has test-only modules (LoadingPhase None)"), TestOnly.Num() > 0);

	// What a user's session has: none of them is loaded once the editor has started, so none of their types exists.
	TArray<FName> LoadedAtStartup;
	TestTrue(TEXT("the editor recorded the test-only modules at startup"),
		UE::AssetDocumentServer::GetTestOnlyModulesLoadedAtStartup(LoadedAtStartup));
	TestEqual(FString::Printf(TEXT("test-only modules loaded at startup: [%s]"), *JoinNames(LoadedAtStartup)),
		LoadedAtStartup.Num(), 0);

	// The automation framework loads them before it lists the tests (AutomationTestSettings EditorTestModules).
	TArray<FString> EditorTestModules;
	GConfig->GetArray(TEXT("/Script/Engine.AutomationTestSettings"), TEXT("EditorTestModules"), EditorTestModules, GEngineIni);
	for (const FName Name : TestOnly)
	{
		TestTrue(FString::Printf(TEXT("%s is an EditorTestModules entry"), *Name.ToString()),
			EditorTestModules.Contains(Name.ToString()));
		TestTrue(FString::Printf(TEXT("%s is loaded while tests run"), *Name.ToString()),
			FModuleManager::Get().IsModuleLoaded(Name));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTestOnlyTypesOnlyInTestModulesTest,
	"UEAssetDoc.TestOnlyTypes.OnlyInTestOnlyModules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTestOnlyTypesOnlyInTestModulesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTestOnlyTypesTestsLocal;

	TSet<FName> TestOnlyPackages;
	TSet<FName> OtherPackages;
	GetPluginScriptPackages(TestOnlyPackages, OtherPackages);
	TestTrue(TEXT("the plugin's modules are known"), OtherPackages.Num() > 0);

	// Every class, struct and enum of the modules a user's session loads: none is a test type.
	TArray<FString> Misplaced;
	int32 TestOnlyCount = 0;
	CollectPluginTypes<UClass>(TestOnlyPackages, OtherPackages, Misplaced, TestOnlyCount);
	CollectPluginTypes<UScriptStruct>(TestOnlyPackages, OtherPackages, Misplaced, TestOnlyCount);
	CollectPluginTypes<UEnum>(TestOnlyPackages, OtherPackages, Misplaced, TestOnlyCount);
	Misplaced.Sort();
	for (const FString& Type : Misplaced)
	{
		AddError(FString::Printf(TEXT("test type outside the test-only modules: %s"), *Type));
	}
	TestEqual(TEXT("test types outside the test-only modules"), Misplaced.Num(), 0);

	// The test-only modules are loaded now (the tests run), so their types exist and this check saw them.
	TestTrue(TEXT("the test-only modules' types are loaded while tests run"), TestOnlyCount > 0);
	return true;
}

#endif
