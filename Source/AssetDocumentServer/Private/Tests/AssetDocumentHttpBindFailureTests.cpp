// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// UEAssetDoc.HttpServer.BindFailure: after an editor crash and restart the port was
//   still held, the HTTP server did not bind, and the editor ran on with only a log line. Now Start reports false and
//   retries for a few seconds; when the port stays taken it logs an Error (and shows an editor notification) that
//   says the port is in use and what to do. The server does not claim to run, writes no service discovery file, and
//   Stop cancels a pending retry.

#include "AssetDocumentHttpServer.h"
#include "Misc/AutomationTest.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentHttpBindFailureTestsLocal
{
// A loopback socket listening on a free port, as the crashed editor that still holds it.
struct FPortBlocker
{
	FUniqueSocket Socket;
	uint32 Port = 0;

	FPortBlocker()
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (Sockets == nullptr)
		{
			return;
		}
		Socket = Sockets->CreateUniqueSocket(NAME_Stream, TEXT("HTTP port blocker"));
		TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
		Address->SetLoopbackAddress();
		Address->SetPort(0);
		if (Socket && Socket->Bind(*Address) && Socket->Listen(1))
		{
			Port = static_cast<uint32>(Socket->GetPortNo());
		}
	}
};
} // namespace AssetDocumentHttpBindFailureTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentHttpBindFailureTest,
	"UEAssetDoc.HttpServer.BindFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentHttpBindFailureTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentHttpBindFailureTestsLocal;
	const TSharedRef<FPortBlocker> Blocker = MakeShared<FPortBlocker>();
	if (!TestTrue(TEXT("a free loopback port is held"), Blocker->Port != 0))
	{
		return false;
	}

	// The engine's listener logs its bind failure; the server's own Error comes after the retries.
	AddExpectedError(TEXT("unable to bind"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("is not listening/bound"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("The UEAssetDoc HTTP server cannot listen on port"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedMessage(TEXT("failed to start listening"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessage(TEXT("could not bind port"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	const FString Text = FAssetDocumentHttpServer::DescribeBindFailure(Blocker->Port);
	TestTrue(TEXT("the failure names the port and says it is in use"),
		Text.Contains(FString::Printf(TEXT("port %u"), Blocker->Port)) && Text.Contains(TEXT("in use")));
	TestTrue(TEXT("the failure says what to do"), Text.Contains(TEXT("restart the editor")) && Text.Contains(TEXT("HTTP Server Port")));
	float Total = 0.0f;
	for (const float Delay : FAssetDocumentHttpServer::BindRetryDelays())
	{
		Total += Delay;
	}
	TestTrue(TEXT("the retries last a few seconds"), Total >= 2.0f && Total <= 8.0f);

	const int32 WritesBefore = FAssetDocumentHttpServer::GetServiceDiscoveryWriteCount();
	const TSharedRef<FAssetDocumentHttpServer> Server = MakeShared<FAssetDocumentHttpServer>();
	TestFalse(TEXT("Start reports the bind failure"), Server->Start(Blocker->Port));
	TestFalse(TEXT("the server does not claim to run"), Server->IsRunning());
	TestTrue(TEXT("no port is reported"), Server->GetPort() == 0);
	TestTrue(TEXT("a retry is scheduled"), Server->IsWaitingForPort());

	// Stop cancels a pending retry.
	{
		const TSharedRef<FAssetDocumentHttpServer> Stopped = MakeShared<FAssetDocumentHttpServer>();
		TestFalse(TEXT("a second server on the port fails too"), Stopped->Start(Blocker->Port));
		Stopped->Stop();
		TestFalse(TEXT("Stop cancels the retry"), Stopped->IsWaitingForPort());
	}

	// The retries run out while the port stays taken: one Error (expected above), nothing written.
	const double Deadline = FPlatformTime::Seconds() + 15.0;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Server, Blocker, WritesBefore, Deadline]()
	{
		if (Server->IsWaitingForPort() && FPlatformTime::Seconds() < Deadline)
		{
			return false;
		}
		TestFalse(TEXT("the retries end"), Server->IsWaitingForPort());
		TestFalse(TEXT("the server still does not run"), Server->IsRunning());
		TestEqual(TEXT("no service discovery file is written for a server that does not listen"),
			FAssetDocumentHttpServer::GetServiceDiscoveryWriteCount(), WritesBefore);
		Server->Stop();
		return true;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
