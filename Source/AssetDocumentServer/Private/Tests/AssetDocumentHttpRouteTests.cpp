// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// UEAssetDoc.HttpServer.Routes: the routes the HTTP server binds itself. health, context, screenshot,
//   execute and delete answer (requests to the running editor's server); the routes of the removed legacy generator
//   and BSL stack (generate, extract, query, generators, datatable/rows, apply_bsl, extract_bsl, extract_graph) answer
//   route-not-found, and the service discovery file no longer lists them.

#include "AssetDocumentHttpServer.h"
#include "AssetDocumentServerModule.h"
#include "AssetDocumentViewportCapture.h"
#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentHttpRouteTestsLocal
{
struct FRouteProbe
{
	const TCHAR* Verb = TEXT("GET");
	const TCHAR* Path = TEXT("");
	const TCHAR* Body = nullptr;
	bool bKept = true;
};

// The kept routes with a request each answers without side effects (execute and delete reject an empty request), then
// the removed ones with the verb they were bound to.
const FRouteProbe RouteProbes[] = {
	{TEXT("GET"), TEXT("/assetdoc/health"), nullptr, true},
	{TEXT("GET"), TEXT("/assetdoc/context"), nullptr, true},
	{TEXT("GET"), TEXT("/assetdoc/screenshot"), nullptr, true},
	{TEXT("POST"), TEXT("/assetdoc/execute"), TEXT("{}"), true},
	{TEXT("POST"), TEXT("/assetdoc/delete"), TEXT("{}"), true},
	{TEXT("POST"), TEXT("/assetdoc/generate"), TEXT("{}"), false},
	{TEXT("POST"), TEXT("/assetdoc/extract"), TEXT("{}"), false},
	{TEXT("POST"), TEXT("/assetdoc/query"), TEXT("{}"), false},
	{TEXT("GET"), TEXT("/assetdoc/generators"), nullptr, false},
	{TEXT("POST"), TEXT("/assetdoc/datatable/rows"), TEXT("{}"), false},
	{TEXT("POST"), TEXT("/assetdoc/apply_bsl"), TEXT("{}"), false},
	{TEXT("POST"), TEXT("/assetdoc/extract_bsl"), TEXT("{}"), false},
	{TEXT("POST"), TEXT("/assetdoc/extract_graph"), TEXT("{}"), false},
};

// What the engine's HTTP server answers for a path no route handles (HttpServerConstantsPrivate.h).
const TCHAR* const RouteNotFound = TEXT("route_handler_not_found");

TSharedPtr<FJsonObject> ParseRouteAnswer(const FString& Body)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	return FJsonSerializer::Deserialize(Reader, Json) ? Json : nullptr;
}

FString RouteAnswerString(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field)
{
	FString Value;
	return Json.IsValid() && Json->TryGetStringField(Field, Value) ? Value : FString();
}

bool RouteAnswerSucceeded(const TSharedPtr<FJsonObject>& Json)
{
	bool bSuccess = false;
	return Json.IsValid() && Json->TryGetBoolField(TEXT("success"), bSuccess) && bSuccess;
}

struct FRouteAnswers
{
	TArray<int32> Codes;
	TArray<FString> Bodies;
	int32 Pending = 0;
};
} // namespace AssetDocumentHttpRouteTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentHttpRoutesTest,
	"UEAssetDoc.HttpServer.Routes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentHttpRoutesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentHttpRouteTestsLocal;
	FAssetDocumentServerModule* Module = FModuleManager::GetModulePtr<FAssetDocumentServerModule>(TEXT("AssetDocumentServer"));
	const FAssetDocumentHttpServer* Server = Module != nullptr ? Module->GetHttpServer() : nullptr;
	if (Server == nullptr || !Server->IsRunning())
	{
		AddInfo(TEXT("The UEAssetDoc HTTP server is not running: nothing to request."));
		return true;
	}
	const uint32 Port = Server->GetPort();

	// The service discovery file lists the kept routes and none of the removed ones.
	FAssetDocumentHttpServer::FlushServiceDiscovery();
	FString Discovery;
	if (TestTrue(TEXT("the service discovery file is readable"),
		FFileHelper::LoadFileToString(Discovery, *FAssetDocumentHttpServer::GetServiceDiscoveryFilePath())))
	{
		for (const FRouteProbe& Probe : RouteProbes)
		{
			const FString Quoted = FString::Printf(TEXT("\"%s\""), Probe.Path);
			if (!Probe.bKept)
			{
				TestFalse(*FString::Printf(TEXT("discovery does not list %s"), Probe.Path), Discovery.Contains(Quoted));
			}
		}
		TestTrue(TEXT("discovery lists health"), Discovery.Contains(TEXT("\"/assetdoc/health\"")));
		TestTrue(TEXT("discovery lists execute"), Discovery.Contains(TEXT("\"/assetdoc/execute\"")));
	}

	// execute and delete reject the empty request with a 400 (logged as a warning); the screenshot logs one too when
	// this session cannot read its viewport.
	TArray<FColor> Pixels;
	FIntPoint Size;
	int32 CaptureStatus = 0;
	FString CaptureError;
	const bool bCaptureWorks = FAssetDocumentViewportCapture::Capture(
		FAssetDocumentViewportCapture::FindViewportClient(), Pixels, Size, CaptureStatus, CaptureError);
	AddExpectedMessage(TEXT("HTTP Error"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, bCaptureWorks ? 2 : 3);

	const TSharedRef<FRouteAnswers> Answers = MakeShared<FRouteAnswers>();
	const int32 Count = UE_ARRAY_COUNT(RouteProbes);
	Answers->Codes.Init(0, Count);
	Answers->Bodies.SetNum(Count);
	Answers->Pending = Count;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FRouteProbe& Probe = RouteProbes[Index];
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(FString::Printf(TEXT("http://127.0.0.1:%u%s"), Port, Probe.Path));
		Request->SetVerb(Probe.Verb);
		if (Probe.Body != nullptr)
		{
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Request->SetContentAsString(Probe.Body);
		}
		Request->OnProcessRequestComplete().BindLambda(
			[Answers, Index](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				Answers->Codes[Index] = bConnected && Response.IsValid() ? Response->GetResponseCode() : -1;
				Answers->Bodies[Index] = Response.IsValid() ? Response->GetContentAsString() : FString();
				--Answers->Pending;
			});
		if (!Request->ProcessRequest())
		{
			Answers->Codes[Index] = -1;
			--Answers->Pending;
		}
	}

	const double Deadline = FPlatformTime::Seconds() + 30.0;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Answers, Count, Deadline, bCaptureWorks, Port]()
	{
		if (Answers->Pending > 0 && FPlatformTime::Seconds() < Deadline)
		{
			return false;
		}
		TestEqual(TEXT("every request is answered"), Answers->Pending, 0);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FRouteProbe& Probe = RouteProbes[Index];
			const int32 Code = Answers->Codes[Index];
			const FString& Body = Answers->Bodies[Index];
			const FString What = FString::Printf(TEXT("%s %s (%d %s)"), Probe.Verb, Probe.Path, Code, *Body.Left(160));
			if (!Probe.bKept)
			{
				TestTrue(*FString::Printf(TEXT("removed route answers route-not-found: %s"), *What),
					Code == 404 && Body.Contains(RouteNotFound));
				continue;
			}
			TestFalse(*FString::Printf(TEXT("kept route is bound: %s"), *What), Body.Contains(RouteNotFound));
			const FString Path(Probe.Path);
			const TSharedPtr<FJsonObject> Json = ParseRouteAnswer(Body);
			if (Path.EndsWith(TEXT("/health")))
			{
				double AnsweredPort = 0.0;
				TestTrue(*FString::Printf(TEXT("health is ok: %s"), *What), Code == 200 && RouteAnswerString(Json, TEXT("status")) == TEXT("ok")
					&& Json->TryGetNumberField(TEXT("port"), AnsweredPort) && static_cast<uint32>(AnsweredPort) == Port);
			}
			else if (Path.EndsWith(TEXT("/context")))
			{
				TestTrue(*FString::Printf(TEXT("context answers the editor state: %s"), *What), Code == 200 && RouteAnswerSucceeded(Json)
					&& Json->HasField(TEXT("selectedActors")) && Json->HasField(TEXT("currentLevel")));
			}
			else if (Path.EndsWith(TEXT("/screenshot")))
			{
				TestTrue(*FString::Printf(TEXT("screenshot answers as the viewport allows: %s"), *What), bCaptureWorks
					? Code == 200 && RouteAnswerSucceeded(Json) && RouteAnswerString(Json, TEXT("mimeType")) == TEXT("image/jpeg")
					: Code >= 500 && !RouteAnswerSucceeded(Json));
			}
			else if (Path.EndsWith(TEXT("/execute")))
			{
				TestTrue(*FString::Printf(TEXT("execute asks for the code: %s"), *What), Code == 400
					&& RouteAnswerString(Json, TEXT("error")).Contains(TEXT("'Code'")));
			}
			else if (Path.EndsWith(TEXT("/delete")))
			{
				TestTrue(*FString::Printf(TEXT("delete asks for the assets: %s"), *What), Code == 400
					&& RouteAnswerString(Json, TEXT("error")).Contains(TEXT("No assets specified")));
			}
		}
		return true;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
