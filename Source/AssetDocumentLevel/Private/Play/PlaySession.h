// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "LevelToolResult.h"

// play (POST /assetdoc/play): Play In Editor control for an MCP client, what a human tester does
// with the editor's Play toolbar, keyboard and mouse. Request and response: MCP/schemas/Playtest.md.
//
// Actions: start, stop, status, pause, resume, wait, input, console, poll. Nothing blocks the
// editor's game thread: start, stop, wait and input return {"pending": {"id", "poll_ms"}} at once,
// and the caller polls {"action": "poll", "id"} until the operation completes (the MCP tool does
// this itself, so one tool call is one finished operation). Completion is evaluated every frame
// from world-tick hooks; every operation has a real-time bound.
//
// Safety: input and console only reach the running play world (Play.NoSession otherwise); the
// editor world is never changed; start refuses while an editor transaction or modal dialog is open
// and never shows the editor's Blueprint error prompt (Play.BlueprintCompileErrors lists them).
namespace UE::AssetDocumentLevel::Private::Play
{
FLevelToolResult RunPlay(const TSharedPtr<FJsonObject>& Request);
FLevelToolResult RunPlayFromBody(const FString& RequestBody);

// get_viewport_screenshot (POST /assetdoc/play/screenshot): {"view": "auto" | "game" | "editor",
// "hud": bool}. "auto" (default) captures the play session's game viewport while one runs, else the
// Level Editor viewport. Payload: {view, width, height, mime_type, data (base64 JPEG)}.
FLevelToolResult RunPlayScreenshotFromBody(const FString& RequestBody);

// The status payload of play {"action": "status"}.
TSharedRef<FJsonObject> MakeSessionStatusJson();

// The log position and real time when the current (or last) play session started; 0 before any.
void GetSessionLogBase(uint64& OutSeq, double& OutRealTime);

// Hands the log lines queued on the logging thread to the play log (game thread).
void FlushPendingLog();

void StartupPlayTools();
void ShutdownPlayTools();
}
