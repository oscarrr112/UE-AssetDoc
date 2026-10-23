// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentModuleGraphTests
{
// Layering of the AssetDocument modules, lowest first:
//   AssetDocument (core) <- AssetDocumentEditor / AssetDocumentPython
//   <- AssetDocumentGraph (graph provider; its public adapter registry is where other
//      providers plug in schema / host / settings adapters, contract graph-primitives §5)
//   <- provider modules (AI, Animation, UMG, Widget, Data, Niagara) <- AssetDocumentIntegration.
// A module never depends on a module at its own level or above. Only the direction is
// checked; the exact dependency lists are free to change. AssetDocumentGraph is also
// asset-class agnostic: it never links the engine modules of a provider's asset class
// (Widget Blueprint: UMG / UMGEditor / MovieScene / MovieSceneTracks); providers plug their
// class-specific behaviour into its public adapter registry instead.
const TCHAR* const GraphModule = TEXT("AssetDocumentGraph");
const TCHAR* const ProviderModules[] = {
	TEXT("AssetDocumentAI"), TEXT("AssetDocumentAnimation"),
	TEXT("AssetDocumentUMG"), TEXT("AssetDocumentWidget"), TEXT("AssetDocumentData"), TEXT("AssetDocumentNiagara")};

// Engine modules that only a provider module may link.
const TCHAR* const ProviderEngineModules[] = {
	TEXT("UMG"), TEXT("UMGEditor"), TEXT("MovieScene"), TEXT("MovieSceneTracks"), TEXT("Niagara"), TEXT("NiagaraEditor")};

struct FLayerRule
{
	FString Module;
	TArray<FString> Forbidden;
};

TArray<FLayerRule> MakeLayerRules()
{
	TArray<FString> Providers{GraphModule};
	for (const TCHAR* Provider : ProviderModules)
	{
		Providers.Add(Provider);
	}
	auto Rule = [&Providers](const TCHAR* Module, TArray<FString> Extra)
	{
		FLayerRule Result{Module, Providers};
		Result.Forbidden.Append(MoveTemp(Extra));
		Result.Forbidden.Remove(Module);
		return Result;
	};
	TArray<FLayerRule> Rules;
	Rules.Add(Rule(TEXT("AssetDocument"),
		{TEXT("AssetDocumentEditor"), TEXT("AssetDocumentPython"), TEXT("AssetDocumentIntegration")}));
	Rules.Add(Rule(TEXT("AssetDocumentEditor"), {TEXT("AssetDocumentPython"), TEXT("AssetDocumentIntegration")}));
	Rules.Add(Rule(TEXT("AssetDocumentPython"), {TEXT("AssetDocumentEditor"), TEXT("AssetDocumentIntegration")}));
	FLayerRule GraphRule = Rule(GraphModule, {TEXT("AssetDocumentPython"), TEXT("AssetDocumentIntegration")});
	for (const TCHAR* EngineModule : ProviderEngineModules)
	{
		GraphRule.Forbidden.Add(EngineModule);
	}
	Rules.Add(MoveTemp(GraphRule));
	for (const TCHAR* Provider : ProviderModules)
	{
		FLayerRule ProviderRule = Rule(Provider, {TEXT("AssetDocumentPython"), TEXT("AssetDocumentIntegration")});
		ProviderRule.Forbidden.Remove(GraphModule);
		Rules.Add(MoveTemp(ProviderRule));
	}
	return Rules;
}

// Loads <Module>.Build.cs without // comments so commented-out names do not count.
bool LoadBuildRules(const FString& Module, FString& OutText)
{
	const FString Path = FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source"),
		Module, Module + TEXT(".Build.cs"));
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *Path))
	{
		return false;
	}
	TArray<FString> Lines;
	Raw.ParseIntoArrayLines(Lines, false);
	for (const FString& Line : Lines)
	{
		const int32 Comment = Line.Find(TEXT("//"));
		OutText += (Comment == INDEX_NONE ? Line : Line.Left(Comment)) + TEXT("\n");
	}
	return true;
}
} // namespace AssetDocumentModuleGraphTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentModuleGraphNoReverseDependenciesTest,
	"UEAssetDoc.Architecture.ModuleGraph.NoReverseDependencies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentModuleGraphNoReverseDependenciesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentModuleGraphTests;
	for (const FLayerRule& Rule : MakeLayerRules())
	{
		FString Text;
		if (!TestTrue(FString::Printf(TEXT("%s.Build.cs is readable"), *Rule.Module), LoadBuildRules(Rule.Module, Text)))
		{
			continue;
		}
		for (const FString& Forbidden : Rule.Forbidden)
		{
			TestFalse(FString::Printf(TEXT("%s does not depend on %s"), *Rule.Module, *Forbidden),
				Text.Contains(TEXT("\"") + Forbidden + TEXT("\""), ESearchCase::CaseSensitive));
		}
	}
	return true;
}
#endif
