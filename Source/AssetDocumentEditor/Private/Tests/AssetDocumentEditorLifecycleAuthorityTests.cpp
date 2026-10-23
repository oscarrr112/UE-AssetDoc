// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorLifecycleAuthorityTests
{
struct FLifecycleSource
{
	const TCHAR* RelativePath;
	FString Text;
};

bool LoadLifecycleSources(
	FAutomationTestBase& Test,
	TArray<FLifecycleSource>& OutSources)
{
	const TCHAR* RelativePaths[] = {
		TEXT("Provider/Lifecycle/AssetDocumentEditorLifecycleAdmission.cpp"),
  TEXT("Provider/Lifecycle/AssetDocumentEditorGenericFactoryContext.cpp"),
		TEXT("Provider/Lifecycle/AssetDocumentEditorLifecyclePayloads.cpp"),
		TEXT("Provider/Lifecycle/AssetDocumentEditorScratchCloneHandler.cpp"),
		TEXT("Provider/Lifecycle/AssetDocumentEditorScratchCreateFromClassHandler.cpp"),
		TEXT("Provider/Lifecycle/AssetDocumentEditorResolveExistingHandler.cpp"),
		TEXT("Provider/Lifecycle/AssetDocumentEditorFactoryCreateHandler.cpp"),
		TEXT("Provider/Lifecycle/AssetDocumentEditorRollbackCreatedHandler.cpp"),
		TEXT("Provider/Cutover/AssetDocumentEditorRollbackSeed.cpp"),
	};
	const FString SourceRoot = FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocumentEditor/Private"));
	for (const TCHAR* RelativePath : RelativePaths)
	{
		FLifecycleSource& Source = OutSources.AddDefaulted_GetRef();
		Source.RelativePath = RelativePath;
		const FString AbsolutePath =
			FPaths::Combine(SourceRoot, RelativePath);
		if (!Test.TestTrue(
			*FString::Printf(
				TEXT("lifecycle source is readable: %s"), RelativePath),
			FFileHelper::LoadFileToString(Source.Text, *AbsolutePath)))
		{
			return false;
		}
	}
	return true;
}

const FString* FindSource(
	TConstArrayView<FLifecycleSource> Sources,
	const TCHAR* Filename)
{
	for (const FLifecycleSource& Source : Sources)
	{
		if (FPaths::GetCleanFilename(Source.RelativePath) == Filename)
		{
			return &Source.Text;
		}
	}
	return nullptr;
}

int32 CountOccurrences(FStringView Text, FStringView Needle)
{
	int32 Count = 0;
	int32 Offset = 0;
	while (Offset < Text.Len())
	{
		const int32 Index = Text.Find(
			Needle,
			Offset,
			ESearchCase::CaseSensitive);
		if (Index == INDEX_NONE)
		{
			break;
		}
		++Count;
		Offset = Index + Needle.Len();
	}
	return Count;
}

int32 CountAcrossSources(
	TConstArrayView<FLifecycleSource> Sources,
	FStringView Needle)
{
	int32 Count = 0;
	for (const FLifecycleSource& Source : Sources)
	{
		Count += CountOccurrences(Source.Text, Needle);
	}
	return Count;
}

void TestExactAuthorityCallsites(
	FAutomationTestBase& Test,
	TConstArrayView<FLifecycleSource> Sources)
{
	const FString* Scratch = FindSource(
		Sources, TEXT("AssetDocumentEditorScratchCloneHandler.cpp"));
	const FString* CreateScratch = FindSource(
		Sources,
		TEXT("AssetDocumentEditorScratchCreateFromClassHandler.cpp"));
	const FString* Factory = FindSource(
		Sources, TEXT("AssetDocumentEditorFactoryCreateHandler.cpp"));
	const FString* Rollback = FindSource(
		Sources, TEXT("AssetDocumentEditorRollbackCreatedHandler.cpp"));
	const FString* RollbackHelper = FindSource(
		Sources, TEXT("AssetDocumentEditorRollbackSeed.cpp"));
	if (!Test.TestNotNull(TEXT("scratch source is present"), Scratch)
		|| !Test.TestNotNull(
			TEXT("create-scratch source is present"), CreateScratch)
		|| !Test.TestNotNull(TEXT("factory source is present"), Factory)
		|| !Test.TestNotNull(TEXT("rollback source is present"), Rollback)
		|| !Test.TestNotNull(
			TEXT("rollback helper source is present"), RollbackHelper))
	{
		return;
	}
	Test.TestEqual(TEXT("scratch has one admitted raw duplication call"),
		CountOccurrences(*Scratch, TEXT("StaticDuplicateObjectEx(")), 1);
	// The Montage-only clone handler is gone; generic scratch clones live outside this source set.
	Test.TestEqual(TEXT("raw duplication is isolated to scratch"),
		CountAcrossSources(Sources, TEXT("StaticDuplicateObjectEx(")), 1);
	Test.TestEqual(TEXT("scratch has one raw lifecycle allowlist marker"),
		CountOccurrences(
			*Scratch,
			TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_LIFECYCLE_ALLOWLIST(")),
		1);
	Test.TestEqual(TEXT("scratch has one raw callsite marker"),
		CountOccurrences(
			*Scratch, TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_CALLSITE(")),
		1);
	Test.TestEqual(TEXT("create-scratch calls the exported factory helper once"),
		CountOccurrences(*CreateScratch, TEXT("FactoryCreateNew(")), 1);
	const TCHAR* CreateScratchForbidden[] = {
		TEXT("NewObject"),
		TEXT("StaticConstructObject"),
		TEXT("DuplicateObject"),
		TEXT("StaticDuplicateObject"),
		TEXT("ImportObject"),
		TEXT("IAssetTools::Get().CreateAsset"),
		TEXT("CreatePackage"),
		TEXT("AssetCreated"),
		TEXT("SavePackage"),
		TEXT("FindFunction"),
		TEXT("ProcessEvent"),
		TEXT("FindFProperty"),
		TEXT("FindPropertyByName"),
		TEXT("ContainerPtrToValuePtr"),
		TEXT("ExecPython"),
		TEXT("PyRun_"),
		TEXT("PyCapsule"),
		TEXT("RetireScratchHandlesForContext"),
		TEXT("Invalidate("),
	};
	for (const TCHAR* Needle : CreateScratchForbidden)
	{
		Test.TestEqual(
			*FString::Printf(
				TEXT("create-scratch has no forbidden surface: %s"), Needle),
			CountOccurrences(*CreateScratch, Needle),
			0);
	}
	Test.TestEqual(TEXT("create-scratch has no raw lifecycle allowlist marker"),
		CountOccurrences(
			*CreateScratch,
			TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_LIFECYCLE_ALLOWLIST(")),
		0);
	Test.TestEqual(TEXT("create-scratch has no raw callsite marker"),
		CountOccurrences(
			*CreateScratch, TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_CALLSITE(")),
		0);
	Test.TestEqual(TEXT("factory never enumerates exported factories"),
		CountOccurrences(*Factory, TEXT("IAssetTools::Get().GetNewAssetFactories()")),
		0);
	Test.TestEqual(TEXT("factory calls authoritative create once"),
		CountOccurrences(*Factory, TEXT("IAssetTools::Get().CreateAsset(")),
		1);
	Test.TestEqual(TEXT("factory admits the exact factory class"),
		CountOccurrences(
			*Factory,
			TEXT("Candidate->GetClass() == UDataAssetFactory::StaticClass()")),
		1);
	Test.TestEqual(TEXT("factory uses the exact loaded BehaviorTree factory path"),
		CountOccurrences(*Factory, TEXT("BehaviorTreeFactoryClassPath")), 2);
	Test.TestEqual(TEXT("factory performs one non-loading exact class lookup"),
		CountOccurrences(*Factory, TEXT("StaticFindObjectSafe(")), 1);
	Test.TestEqual(TEXT("factory does not link the BehaviorTree factory type"),
		CountOccurrences(*Factory, TEXT("UBehaviorTreeFactory")), 0);
	Test.TestEqual(TEXT("rollback delegates created-target deletion once"),
		CountOccurrences(
			*Rollback, TEXT("DeleteAssetDocumentEditorCreatedTarget(")), 1);
	Test.TestEqual(TEXT("rollback has no direct authoritative delete call"),
		CountOccurrences(
			*Rollback, TEXT("ObjectTools::DeleteObjects(")), 0);
	Test.TestEqual(TEXT("shared helper calls authoritative delete once"),
		CountOccurrences(
			*RollbackHelper, TEXT("ObjectTools::DeleteObjects(")), 1);
}

void TestForbiddenFallbacks(
	FAutomationTestBase& Test,
	TConstArrayView<FLifecycleSource> Sources)
{
	const TCHAR* Forbidden[] = {
				TEXT("NewObject("),
		TEXT("StaticConstructObject"),
		TEXT("ConstructObject<"),
		TEXT("ConstructObject("),
		TEXT("DuplicateObject<"),
		TEXT("DuplicateObject("),
		TEXT("CreatePackage("),
		TEXT("UPackage::SavePackage("),
		TEXT("FAssetRegistryModule::AssetCreated("),
		TEXT("CreateAssetWithDialog("),
	};
 // Factories are instantiated only by AssetDocumentEditorGenericFactoryInstance.cpp;
 // the factory-context configuration (AnimMontage Skeleton) never opens a dialog.
 const FString* Context=FindSource(Sources,TEXT("AssetDocumentEditorGenericFactoryContext.cpp"));
 Test.TestEqual(TEXT("lifecycle sources construct no objects"),CountAcrossSources(Sources,TEXT("NewObject<")),0);
 if (Test.TestNotNull(TEXT("factory context source"),Context))
  Test.TestEqual(TEXT("no UI configuration"),CountOccurrences(*Context,TEXT("ConfigureProperties(")),0);
	for (const TCHAR* Needle : Forbidden)
	{
		Test.TestEqual(
			*FString::Printf(
				TEXT("lifecycle has no forbidden fallback: %s"), Needle),
			CountAcrossSources(Sources, Needle),
			0);
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderLifecycleAuthoritySourceTest,
	"UEAssetDoc.GenericProvider.LifecycleSourceGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderLifecycleAuthoritySourceTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorLifecycleAuthorityTests;
	TArray<FLifecycleSource> Sources;
	if (!LoadLifecycleSources(*this, Sources))
	{
		return false;
	}
	TestExactAuthorityCallsites(*this, Sources);
	TestForbiddenFallbacks(*this, Sources);
	return true;
}
#endif
