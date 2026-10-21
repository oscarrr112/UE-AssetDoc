#include "AssetDocumentRuntimeModule.h"

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentInactiveLoadingTests
{
FString GetPluginRoot()
{
	return FPaths::ConvertRelativePathToFull(GetAssetDocumentPluginBaseDir());
}

void TestModuleOwnership(
	FAutomationTestBase& Test,
	const TCHAR* ModuleName,
	bool bExpectedActive)
{
	IAssetDocumentRuntimeModule* Module =
		FModuleManager::LoadModulePtr<IAssetDocumentRuntimeModule>(ModuleName);
	Test.TestNotNull(*FString::Printf(TEXT("%s module loads"), ModuleName), Module);
	if (Module == nullptr)
	{
		return;
	}
	Test.TestEqual(TEXT("module reports its exact name"), Module->GetRuntimeModuleName(), FName(ModuleName));
	Test.TestEqual(
		TEXT("runtime registration matches sole-owner policy"),
		Module->HasActiveRuntimeRegistration(),
		bExpectedActive);
}

void TestStartupSourceHasNoTraffic(FAutomationTestBase& Test, const TCHAR* ModuleName)
{
	const FString Path = FPaths::Combine(
		GetPluginRoot(), TEXT("Source"), ModuleName, TEXT("Private"),
		FString::Printf(TEXT("%sModule.cpp"), ModuleName));
	FString Text;
	Test.TestTrue(*FString::Printf(TEXT("%s startup source is readable"), ModuleName),
		FFileHelper::LoadFileToString(Text, *Path));
	static const TCHAR* Forbidden[] = {
		TEXT(".Register("), TEXT("Freeze("), TEXT("Dispatch("), TEXT("Invoke("),
		TEXT("FAssetDocumentService"), TEXT("HttpRoutes"), TEXT("FileWatcher"),
		TEXT("EditorSync"), TEXT("SyncCoordinator"), TEXT("PrimitiveRegistry")
		, TEXT("BindGenericRuntimeRoute")
	};
	for (const TCHAR* Token : Forbidden)
	{
		Test.TestFalse(*FString::Printf(TEXT("%s startup excludes %s"), ModuleName, Token), Text.Contains(Token));
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentNoTrafficBeforeActiveRevisionTest,
	"UEAssetDoc.Architecture.Loading.NoTrafficBeforeActiveRevision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNoTrafficBeforeActiveRevisionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	static const TCHAR* Modules[] = {
		TEXT("AssetDocumentEditor"), TEXT("AssetDocumentAI"), TEXT("AssetDocumentAnimation"),
		TEXT("AssetDocumentUMG"), TEXT("AssetDocumentPython"),
		TEXT("AssetDocumentIntegration")
	};
	for (const TCHAR* ModuleName : Modules)
	{
		AssetDocumentInactiveLoadingTests::TestModuleOwnership(
			*this,
			ModuleName,
			FCString::Strcmp(
				ModuleName,
				TEXT("AssetDocumentIntegration")) == 0);
		AssetDocumentInactiveLoadingTests::TestStartupSourceHasNoTraffic(*this, ModuleName);
	}
	return true;
}
#endif
