// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// UEAssetDoc.Shutdown.ServiceDiscoveryCoalesced: external route changes (the AssetDocument and level
//   routes register about twenty at startup, and unregister them at shutdown) bind or unbind only that route and
//   mark the service discovery file dirty; the file is written once on the next tick (not rewritten, and every
//   route rebound, on each call). Several registrations then write the file once, with every route listed; a flush with
//   nothing pending writes nothing; unregistering writes it once more without the routes.

#include "AssetDocumentHttpServer.h"
#include "AssetDocumentServerModule.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentServiceDiscoveryTests
{
FString ReadDiscoveryFile()
{
	FString Text;
	FFileHelper::LoadFileToString(Text, *FAssetDocumentHttpServer::GetServiceDiscoveryFilePath());
	return Text;
}
} // namespace AssetDocumentServiceDiscoveryTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentServiceDiscoveryCoalescedTest,
	"UEAssetDoc.Shutdown.ServiceDiscoveryCoalesced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentServiceDiscoveryCoalescedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentServiceDiscoveryTests;
	FAssetDocumentServerModule* Module = FModuleManager::GetModulePtr<FAssetDocumentServerModule>(TEXT("AssetDocumentServer"));
	const FAssetDocumentHttpServer* Server = Module != nullptr ? Module->GetHttpServer() : nullptr;
	const bool bRunning = Server != nullptr && Server->IsRunning();
	if (!bRunning)
	{
		AddInfo(TEXT("The UEAssetDoc HTTP server is not running: only the deferral is checked."));
	}
	const int32 ExpectedWrites = bRunning ? 1 : 0;

	FAssetDocumentHttpServer::FlushServiceDiscovery();
	const int32 Before = FAssetDocumentHttpServer::GetServiceDiscoveryWriteCount();

	TArray<FDelegateHandle> Handles;
	const TArray<FString> Paths = {
		TEXT("/assetdoc/test/p62/one"), TEXT("/assetdoc/test/p62/two"), TEXT("/assetdoc/test/p62/three")};
	for (const FString& Path : Paths)
	{
		FAssetDocumentExternalRoute Route;
		Route.Path = Path;
		Route.MethodText = TEXT("GET");
		Route.Description = TEXT("Discovery test route");
		Route.Handler = FAssetDocumentExternalRouteHandler::CreateLambda(
			[](const FString&, const TMap<FString, FString>&, int32& OutStatusCode) -> TSharedPtr<FJsonObject>
			{
				OutStatusCode = 200;
				return MakeShared<FJsonObject>();
			});
		Handles.Add(FAssetDocumentHttpServer::RegisterExternalRoute(Route));
		TestTrue(*FString::Printf(TEXT("%s registered"), *Path), Handles.Last().IsValid());
	}
	TestEqual(TEXT("registering writes nothing before the flush"),
		FAssetDocumentHttpServer::GetServiceDiscoveryWriteCount(), Before);
	TestEqual(TEXT("one flush writes each running server's file once"), FAssetDocumentHttpServer::FlushServiceDiscovery(), ExpectedWrites);
	TestEqual(TEXT("one write for three registrations"), FAssetDocumentHttpServer::GetServiceDiscoveryWriteCount(), Before + ExpectedWrites);
	if (bRunning)
	{
		const FString Text = ReadDiscoveryFile();
		for (const FString& Path : Paths)
		{
			TestTrue(*FString::Printf(TEXT("discovery file lists %s"), *Path), Text.Contains(Path));
		}
	}
	TestEqual(TEXT("a flush with nothing pending writes nothing"), FAssetDocumentHttpServer::FlushServiceDiscovery(), 0);

	for (const FDelegateHandle& Handle : Handles)
	{
		FAssetDocumentHttpServer::UnregisterExternalRoute(Handle);
	}
	TestEqual(TEXT("unregistering writes nothing before the flush"),
		FAssetDocumentHttpServer::GetServiceDiscoveryWriteCount(), Before + ExpectedWrites);
	TestEqual(TEXT("one flush after unregistering"), FAssetDocumentHttpServer::FlushServiceDiscovery(), ExpectedWrites);
	if (bRunning)
	{
		const FString Text = ReadDiscoveryFile();
		TestFalse(TEXT("discovery file no longer lists the test routes"), Text.Contains(TEXT("/assetdoc/test/p62/")));
		TestTrue(TEXT("discovery file still lists the AssetDocument routes"), Text.Contains(TEXT("/assetdoc/assetdocument/validate")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
