// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Provider/AssetDocumentUMGPrimitiveCatalog.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentUMGProviderArchitectureTestsPrivate
{
FString GetAssetDocumentUMGProductionRoot()
{
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocumentUMG")));
}

bool LoadProductionSources(
	FAutomationTestBase& Test,
	TArray<FString>& OutPaths,
	TArray<FString>& OutTexts)
{
	const FString Root = GetAssetDocumentUMGProductionRoot();
	TArray<FString> Paths;
	IFileManager::Get().FindFilesRecursive(
		Paths, *Root, TEXT("*.h"), true, false);
	IFileManager::Get().FindFilesRecursive(
		Paths, *Root, TEXT("*.cpp"), true, false);
	Paths.Sort();
	for (const FString& Path : Paths)
	{
		if (Path.Contains(TEXT("/Private/Tests/")))
		{
			continue;
		}
		FString Text;
		if (!Test.TestTrue(
			*FString::Printf(
				TEXT("production source is readable: %s"), *Path),
			FFileHelper::LoadFileToString(Text, *Path)))
		{
			return false;
		}
		OutPaths.Add(Path);
		OutTexts.Add(MoveTemp(Text));
	}
	return Test.TestTrue(
		TEXT("UMG production sources are discovered"),
		!OutPaths.IsEmpty());
}
} // namespace AssetDocumentUMGProviderArchitectureTestsPrivate


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUMGProviderRejectsReflectionInvocationTest,
	"UEAssetDoc.UMGProvider.RejectsReflectionInvocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGProviderRejectsReflectionInvocationTest::RunTest(
	const FString& Parameters)
{
	using namespace AssetDocumentUMGProviderArchitectureTestsPrivate;
	(void)Parameters;
	TArray<FString> Paths;
	TArray<FString> Sources;
	if (!LoadProductionSources(*this, Paths, Sources))
	{
		return false;
	}
	const TCHAR* Forbidden[] = {
		TEXT("ProcessEvent("),
		TEXT("FindFunction("),
		TEXT("CallFunctionByNameWithArguments"),
		TEXT("NewObject<"),
		TEXT("StaticConstructObject"),
		TEXT("Animations.Add"),
		TEXT("Bindings.Add"),
		TEXT("WidgetVariableGuids.Add"),
		TEXT("WidgetVariableGuids["),
		TEXT("AssetDocumentBlueprint"),
		TEXT("BlueprintTimeline"),
		TEXT("TimelineTrack"),
		TEXT("/Private/"),
		TEXT("UMG-WIDGET-ANIMATION-CREATE-v1"),
		TEXT("UMG-ANIMATION-BINDING-APPLY-DELTA-v1"),
		TEXT("AssetDocumentHandleRegistryRuntime"),
		TEXT("PrimitiveRegistryRuntime")
	};
	for (int32 Index = 0; Index < Sources.Num(); ++Index)
	{
		for (const TCHAR* Needle : Forbidden)
		{
			TestFalse(
				*FString::Printf(
					TEXT("%s excludes %s"),
					*FPaths::GetCleanFilename(Paths[Index]),
					Needle),
				Sources[Index].Contains(
					Needle, ESearchCase::CaseSensitive));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUMGProviderConsumerParityTest,
	"UEAssetDoc.UMGProvider.ConsumerParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGProviderConsumerParityTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TArray<FAssetDocumentUMGDecisionRow> Projection =
		BuildAssetDocumentUMGDecisionProjection();
	const TArray<FAssetDocumentPrimitiveDescriptor> Catalog =
		BuildAssetDocumentUMGPrimitiveCatalog();
	TestEqual(TEXT("projection cardinality"), Projection.Num(), 20);
	TestEqual(TEXT("catalog cardinality"), Catalog.Num(), 16);
	int32 OrdinaryCount = 0;
	int32 BlockerCount = 0;
	for (const FAssetDocumentUMGDecisionRow& Row : Projection)
	{
		if (Row.Disposition
			== EAssetDocumentUMGDecisionDisposition::Ordinary)
		{
			++OrdinaryCount;
			TestEqual(
				TEXT("ordinary consumer is exact"),
				Row.ConsumerIds,
				TArray<FString>({TEXT("umg_profile")}));
			TestTrue(
				TEXT("ordinary diagnostic is empty"),
				Row.StableBlockerDiagnostic.IsEmpty());
		}
		else
		{
			++BlockerCount;
			TestEqual(
				TEXT("blocker consumer is exact"),
				Row.ConsumerIds,
				TArray<FString>({TEXT("umg_provider_design")}));
			TestFalse(
				TEXT("blocker diagnostic is frozen"),
				Row.StableBlockerDiagnostic.IsEmpty());
			TestFalse(
				TEXT("blocker has no runtime descriptor"),
				Catalog.ContainsByPredicate(
					[&Row](
						const FAssetDocumentPrimitiveDescriptor& Descriptor)
					{
						return Descriptor.PrimitiveId == Row.DecisionId
							&& Descriptor.Version == Row.Version;
					}));
		}
	}
	TestEqual(TEXT("ordinary cardinality"), OrdinaryCount, 16);
	TestEqual(TEXT("blocker cardinality"), BlockerCount, 4);
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Catalog)
	{
		TestEqual(
			TEXT("descriptor consumer is exact"),
			Descriptor.RequiredConsumerIds,
			TArray<FString>({TEXT("umg_profile")}));
		TestFalse(
			TEXT("no design consumer is callable"),
			Descriptor.RequiredConsumerIds.Contains(
				TEXT("umg_provider_design")));
	}
	return true;
}
#endif
