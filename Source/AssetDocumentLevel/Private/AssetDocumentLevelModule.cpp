// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Async/Async.h"
#include "AssetDocumentHttpServer.h"
#include "AssetOps/EditorAssetOps.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Import/AssetImportTool.h"
#include "LevelEditTransaction.h"
#include "LevelEditorUndo.h"
#include "LevelMapTools.h"
#include "LevelQuery.h"
#include "Modules/ModuleManager.h"
#include "Play/PlayQuery.h"
#include "Play/PlaySession.h"
#include "Settings/EditorSettingsTool.h"

namespace UE::AssetDocumentLevel::Private
{
namespace
{
template<typename CallableType>
FLevelToolResult RunOnGameThread(CallableType&& Callable)
{
	if (IsInGameThread())
	{
		return Callable();
	}
	FLevelToolResult Result;
	FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
	AsyncTask(ENamedThreads::GameThread, [&]()
	{
		Result = Callable();
		CompletionEvent->Trigger();
	});
	CompletionEvent->Wait();
	FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
	return Result;
}

TSharedPtr<FJsonObject> HandleQueryRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FLevelToolResult Result = RunOnGameThread([&RequestBody]() { return RunLevelQueryFromBody(RequestBody); });
	OutStatusCode = Result.HttpStatus;
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleEditRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FLevelToolResult Result = RunOnGameThread([&RequestBody]() { return RunLevelEditFromBody(RequestBody); });
	OutStatusCode = Result.HttpStatus;
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleUndoRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FLevelToolResult Result = RunOnGameThread([&RequestBody]() { return RunEditorUndoFromBody(RequestBody); });
	OutStatusCode = Result.HttpStatus;
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleOpenLevelRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FLevelToolResult Result = RunOnGameThread([&RequestBody]() { return RunOpenLevelFromBody(RequestBody); });
	OutStatusCode = Result.HttpStatus;
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandlePlayRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FLevelToolResult Result = RunOnGameThread([&RequestBody]() { return Play::RunPlayFromBody(RequestBody); });
	OutStatusCode = Result.HttpStatus;
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandlePlayQueryRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FLevelToolResult Result = RunOnGameThread([&RequestBody]() { return Play::RunPlayQueryFromBody(RequestBody); });
	OutStatusCode = Result.HttpStatus;
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandlePlayScreenshotRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FLevelToolResult Result = RunOnGameThread([&RequestBody]() { return Play::RunPlayScreenshotFromBody(RequestBody); });
	OutStatusCode = Result.HttpStatus;
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleNewLevelRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FLevelToolResult Result = RunOnGameThread([&RequestBody]() { return RunNewLevelFromBody(RequestBody); });
	OutStatusCode = Result.HttpStatus;
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleImportRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FLevelToolResult Result = RunOnGameThread([&RequestBody]() { return RunAssetImportFromBody(RequestBody); });
	OutStatusCode = Result.HttpStatus;
	return Result.ToJson();
}

template<FLevelToolResult (*Run)(const FString&)>
TSharedPtr<FJsonObject> HandleBodyRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FLevelToolResult Result = RunOnGameThread([&RequestBody]() { return Run(RequestBody); });
	OutStatusCode = Result.HttpStatus;
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleSettingsRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FLevelToolResult Result = RunOnGameThread([&RequestBody]() { return RunEditorSettingsFromBody(RequestBody); });
	OutStatusCode = Result.HttpStatus;
	return Result.ToJson();
}
}
}

class FAssetDocumentLevelModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		using namespace UE::AssetDocumentLevel::Private;
		AddRoute(TEXT("/assetdoc/level/query"), &HandleQueryRoute,
			TEXT("Query the editor world: levels, actors, components and their properties (query_level)"));
		AddRoute(TEXT("/assetdoc/level/edit"), &HandleEditRoute,
			TEXT("Edit the editor world in one undoable batch: spawn, delete, duplicate, set (edit_level)"));
		AddRoute(TEXT("/assetdoc/editor/undo"), &HandleUndoRoute,
			TEXT("Undo or redo the newest editor transaction, named by the caller (undo_editor)"));
		AddRoute(TEXT("/assetdoc/level/open"), &HandleOpenLevelRoute,
			TEXT("Open a map in the editor; refuses to lose unsaved changes (open_level)"));
		AddRoute(TEXT("/assetdoc/level/new"), &HandleNewLevelRoute,
			TEXT("Create a map from a New Level template, save it and open it (new_level)"));
		AddRoute(TEXT("/assetdoc/play"), &HandlePlayRoute,
			TEXT("Play In Editor control: start, stop, status, pause, resume, wait, input, console, call, set, poll (play)"));
		AddRoute(TEXT("/assetdoc/play/query"), &HandlePlayQueryRoute,
			TEXT("Read the running play world: actors, values, AI state, montages, effects; the play log (query_play)"));
		AddRoute(TEXT("/assetdoc/play/screenshot"), &HandlePlayScreenshotRoute,
			TEXT("Screenshot of the play session's game view or the Level Editor viewport (get_viewport_screenshot)"));
		Play::StartupPlayTools();
		AddRoute(TEXT("/assetdoc/import"), &HandleImportRoute,
			TEXT("Import or reimport source files (FBX, glTF, OBJ, images, WAV) as the Content Browser does (import_assets)"));
		AddRoute(TEXT("/assetdoc/settings"), &HandleSettingsRoute,
			TEXT("Read or change World Settings of the open level and Project Settings sections (settings)"));
		AddRoute(TEXT("/assetdoc/assets/save"), &HandleBodyRoute<&RunSaveAssetsFromBody>,
			TEXT("Save assets (force: re-save clean ones) or every package with unsaved changes, without dialogs (save_assets)"));
		AddRoute(TEXT("/assetdoc/assets/reload"), &HandleBodyRoute<&RunReloadAssetsFromBody>,
			TEXT("Reload assets from disk without dialogs; unsaved changes refused unless force (reload_assets)"));
		AddRoute(TEXT("/assetdoc/assets/editor"), &HandleBodyRoute<&RunAssetEditorFromBody>,
			TEXT("Open, close or list asset editors (asset_editor)"));
		AddRoute(TEXT("/assetdoc/assets/export"), &HandleBodyRoute<&RunExportAssetsFromBody>,
			TEXT("Export assets to files inside the project with the editor's exporters: wav, png, ... (export_assets)"));
	}

	virtual void ShutdownModule() override
	{
		UE::AssetDocumentLevel::Private::Play::ShutdownPlayTools();
		for (const FDelegateHandle& Handle : RouteHandles)
		{
			FAssetDocumentHttpServer::UnregisterExternalRoute(Handle);
		}
		RouteHandles.Reset();
	}

private:
	void AddRoute(const TCHAR* Path, TSharedPtr<FJsonObject> (*Handler)(const FString&, const TMap<FString, FString>&, int32&),
		const TCHAR* Description)
	{
		FAssetDocumentExternalRoute Route;
		Route.Path = Path;
		Route.Handler = FAssetDocumentExternalRouteHandler::CreateStatic(Handler);
		Route.MethodText = TEXT("POST");
		Route.Description = Description;
		Route.bRequiresBody = true;
		const FDelegateHandle Handle = FAssetDocumentHttpServer::RegisterExternalRoute(Route);
		if (Handle.IsValid())
		{
			RouteHandles.Add(Handle);
		}
	}

	TArray<FDelegateHandle> RouteHandles;
};

IMPLEMENT_MODULE(FAssetDocumentLevelModule, AssetDocumentLevel)
