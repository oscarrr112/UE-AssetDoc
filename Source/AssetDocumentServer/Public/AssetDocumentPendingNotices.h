// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Problems from work the editor did on its own, outside any tool call: for example a sidecar file that the editor's
// file watcher applied and whose apply failed. Nobody waits for such a result, so it is kept here until it is handled:
//   - get_editor_context lists every open notice ("pendingNotices");
//   - the MCP endpoint appends the notices it has not reported yet to the next tool result, once;
//   - a notice stays open until its source resolves it (Resolve, or Post again for the same Kind and File).
// Thread safe.
struct ASSETDOCUMENTSERVER_API FAssetDocumentPendingNotice
{
	// What happened, e.g. "sidecar_watcher_apply_failed".
	FString Kind;
	// The file the notice is about (absolute path).
	FString File;
	// The error code (the first diagnostic's) and the message.
	FString Code;
	FString Message;
	FDateTime TimeUtc;
	bool bReported = false;

	// {"kind", "file", "code", "message", "time_utc"}
	TSharedRef<FJsonObject> ToJson() const;
};

class ASSETDOCUMENTSERVER_API FAssetDocumentPendingNotices
{
public:
	// Opens (or replaces) the notice of this Kind for this File; it is reported again. A non-empty NotificationText is
	// also shown as a failure notification in the editor (on the game thread, when Slate runs).
	static void Post(const FString& Kind, const FString& File, const FString& Code, const FString& Message,
		const FString& NotificationText = FString());
	// Closes the notice of this Kind for this File, if any.
	static void Resolve(const FString& Kind, const FString& File);

	// The open notices, oldest first.
	static TArray<FAssetDocumentPendingNotice> GetOpen();
	// The open notices not reported yet, oldest first; they count as reported afterwards.
	static TArray<FAssetDocumentPendingNotice> TakeUnreported();

	// The open notices as a JSON array (get_editor_context).
	static TArray<TSharedPtr<FJsonValue>> OpenToJson();
	// The text a tool result carries for these notices (empty for none).
	static FString DescribeForToolResult(TConstArrayView<FAssetDocumentPendingNotice> Notices);
	// Appends the notices not reported yet to an MCP tool result ({"content": [...]}) as one more text block.
	static void AppendUnreportedToToolResult(FJsonObject& ToolResult);
};
