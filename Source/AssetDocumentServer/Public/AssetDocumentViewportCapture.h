// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class FLevelEditorViewportClient;

// The pixels of a Level Editor viewport for the MCP's viewport screenshot (GET /screenshot). The viewport is
// redrawn first: a viewport that is not realtime draws only when invalidated (by mouse input or a Slate tick
// over it), so after edits made through the tools its last frame can be stale.
struct ASSETDOCUMENTSERVER_API FAssetDocumentViewportCapture
{
	// The active Level Editor viewport (the one last clicked), else the first perspective one, else the first.
	static FLevelEditorViewportClient* FindViewportClient();

	// Redraws Client's viewport and reads its pixels (BGRA, row-major, OutSize.X * OutSize.Y). False with an HTTP
	// status (503: no usable viewport, 500: the read failed) and an error otherwise.
	static bool Capture(FLevelEditorViewportClient* Client, TArray<FColor>& OutPixels, FIntPoint& OutSize, int32& OutHttpStatus,
		FString& OutError);
};
